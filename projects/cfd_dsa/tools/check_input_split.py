"""Prove independent D readiness without weakening full-input ownership."""
from collections import defaultdict
from statistics import median

from check_single_line_c import fields


def check_input_split(text, config, trace, jobs, ends, allow_abort=False):
    if config.get('--coeff-input-batch-ready','0')=='1':
        from check_input_batch import check_input_batches
        return check_input_batches(text,config,trace,jobs,ends,allow_abort)
    assert not fields(text,'CFD_INPUT_BATCH'),'disabled batch input emitted publication'
    packed = config.get('--coeff-sme-packed-input', '0') == '1'
    input_base=lambda slot:slot*8192+(8*(slot%4) if int(config.get('--coeff-spm-slot-rotation','0'))&2 else 0)
    rows = fields(text, 'CFD_INPUT_SPLIT')
    cold = {r['request']:r for r in fields(text,'CFD_DIRECT_RHS')
            if r['event']=='consume' and r.get('mode')=='cold'}
    ready = [r for r in trace if r['stage'] == 'INPUT_D_READY']
    if config.get('--coeff-input-d-early', '0') != '1':
        assert not rows and not ready, 'unexpected split-input readiness'
        return dict(requests=0, early_lu_requests=0)
    by_request = defaultdict(list)
    for r in trace:
        by_request[r['requestId']].append(r)
    groups = defaultdict(dict)
    for r in rows:
        events = groups[r['request']]
        assert r['event'] not in events, 'duplicate split-input event'
        events[r['event']] = r
    aborted = {r['requestId'] for r in trace if r['stage'] in
               ('REQUEST_FAILED', 'CANCEL_PENDING', 'CANCEL_COMPLETE')}
    discarded = 0
    if allow_abort:
        for request in groups.keys() & aborted:
            events = groups[request]
            assert events.keys() <= {'d_read', 'd_ready', 'rest_read', 'input_ready'}
            d = events.get('d_read')
            assert d is not None
            access = jobs[d['job']]
            assert (access['owner'], access['op'], access['active']) == (d['owner'], 'R', '1')
            assert (int(access['base'], 16), int(access['words']), int(access['stride'])) == (input_base(int(d['slot'])), 25, 8)
            issues = [r for r in by_request[request] if r['stage'] == 'SPM_PACKET_ISSUE'
                      and r.get('requester') == 'CoeffInput']
            returned = {r['packetId']: int(r['cycle']) for r in by_request[request]
                        if r['stage'] == 'SPM_PACKET_RESPONSE' and
                        r.get('requester') == 'CoeffInput'}
            n = 0
            for r in issues:
                if n >= 200:
                    break
                n += int(r['size'])
                assert (packed or n <= 200) and int(access['cycle']) >= returned[r['packetId']]
            assert n >= 200
            dr = events.get('d_ready')
            if dr:
                assert d['job'] == dr['job'] and int(dr['cycle']) >= int(ends[d['job']]['cycle'])
            for r in by_request[request]:
                if r['stage'] == 'LU_ISSUE':
                    assert dr is not None and int(r['cycle']) >= int(dr['cycle'])
            discarded += 1
        groups = {key: value for key, value in groups.items() if key not in aborted}
        ready = [r for r in ready if r['requestId'] not in aborted]
    expected = set()
    for request, events in by_request.items():
        if allow_abort and request in aborted:
            continue
        if (any(r['stage'] == 'LU_ISSUE' for r in events) and
                sum(int(r['size']) for r in events
                    if r['stage'] == 'SPM_PACKET_ISSUE' and
                    r.get('requester') == 'CoeffInput') +
                (40 if request in cold else 0) > 200):
            expected.add(request)
    assert groups.keys() == expected, 'missing/extra raw D ingress contract'
    assert {r['requestId'] for r in ready} == expected
    early = 0
    leads = []
    used_jobs = set()
    intervals = defaultdict(list)
    added_jobs = 0
    for request, events in groups.items():
        assert events.keys() <= {'d_read', 'd_ready', 'rest_read', 'input_ready'}
        d, dr, full = (events[k] for k in ('d_read', 'd_ready', 'input_ready'))
        rest = events.get('rest_read')
        assert all(all(r[k] == d[k] for k in
                       ('generation', 'cell', 'owner', 'slot', 'bytes'))
                   for r in events.values()), 'split-input identity changed'
        assert d['job'] == dr['job']
        total, slot = int(d['bytes']), int(d['slot'])
        assert total > 200 and total % 8 == 0
        prefetch = request in cold
        ingress = total-(40 if prefetch else 0)
        assert ingress >= 200
        if ingress > 200:
            assert rest is not None and rest['job']==full['job']
            assert d['job'] != rest['job']
        else:
            assert rest is None and full['job']==d['job']
        added_jobs += int(rest is not None)+int(prefetch)
        trace_rows = by_request[request]
        assert all(r['generation'] == d['generation'] and r['cell'] == d['cell']
                   for r in trace_rows), 'split-input generation mismatch'
        issues = [r for r in trace_rows if r['stage'] == 'SPM_PACKET_ISSUE'
                  and r.get('requester') == 'CoeffInput']
        responses = [r for r in trace_rows if r['stage'] == 'SPM_PACKET_RESPONSE'
                     and r.get('requester') == 'CoeffInput']
        returned = {r['packetId']: r for r in responses}
        assert len(returned) == len(responses) == len(issues)
        assert set(returned) == {r['packetId'] for r in issues}
        offset = 0
        d_done = all_done = -1
        for r in issues:
            n = int(r['size'])
            response = returned[r['packetId']]
            assert n > 0 and int(response['size']) == n
            assert int(response['cycle']) >= int(r['cycle'])
            all_done = max(all_done, int(response['cycle']))
            if offset < 200:
                assert packed or offset + n <= 200, 'D packet crossed its input region'
                d_done = max(d_done, int(response['cycle']))
            offset += n
        assert offset == ingress and d_done >= 0
        loads=[(d,input_base(slot),25,d_done)]
        if rest is not None:
            loads.append((rest,input_base(slot)+200,(ingress-200)//8,all_done))
        for row, base, words, store_done in loads:
            job = row['job']
            assert job not in used_jobs, 'input load job reused'
            used_jobs.add(job)
            access = jobs[job]
            assert (access['owner'], access['op'], access['active']) == (d['owner'], 'R', '1')
            assert (int(access['base'], 16), int(access['words']), int(access['stride'])) == (base, words, 8)
            assert int(access['cycle']) == int(row['cycle']) >= store_done, 'input read preceded its stores'
            assert int(ends[job]['cycle']) >= int(access['cycle'])

        def once(stage):
            matches = [r for r in trace_rows if r['stage'] == stage]
            assert len(matches) == 1, 'missing/duplicate '+stage
            return int(matches[0]['cycle'])

        d_ready, input_ready = once('INPUT_D_READY'), once('INPUT_TOKEN')
        ready_row = next(r for r in trace_rows if r['stage'] == 'INPUT_D_READY')
        assert ready_row['packetId'] == d['job'], 'D token does not name its completed load'
        assert d_ready == int(dr['cycle']) >= int(ends[d['job']]['cycle'])
        assert input_ready == int(full['cycle']) >= int(ends[full['job']]['cycle'])
        assert input_ready >= d_ready
        r_ready = None
        if prefetch:
            r_ready = once('INPUT_R_READY')
            assert r_ready==int(cold[request]['cycle']) and input_ready>=r_ready
        lu = once('LU_ISSUE')
        assert lu >= d_ready, 'LU bypassed D register readiness'
        release, start = once('INPUT_SLOT_RELEASE'), once('INPUT_START')
        assert all(int(r['slot']) == slot for r in trace_rows if r['stage'] in
                   ('INPUT_START', 'INPUT_D_READY', 'INPUT_TOKEN', 'INPUT_SLOT_RELEASE'))
        assert release >= input_ready, 'input slot released while ingress was live'
        assert start <= int(d['cycle'])
        intervals[slot].append((start, release))
        for r in trace_rows:
            if r['stage'] == 'COEFF3_ISSUE':
                dependency=max(d_ready,r_ready) if prefetch else input_ready
                assert int(r['cycle']) >= dependency, 'RHS bypassed input readiness'
        early += lu < input_ready
        leads.append(max(0, input_ready-lu))
    for lifetimes in intervals.values():
        ordered = sorted(lifetimes)
        assert all(a[1] <= b[0] for a, b in zip(ordered, ordered[1:])), 'input landing slot overwritten'
    return dict(requests=len(groups), early_lu_requests=early,
                median_lu_lead_cycles=median(leads) if leads else 0,
                max_lu_lead_cycles=max(leads, default=0),
                extra_operand_words=0, added_load_jobs=added_jobs,
                aborted_d_reads=discarded)
