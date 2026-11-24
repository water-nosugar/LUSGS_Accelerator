"""Validate cold R publication, batch initialization and parked RHS credits."""
from collections import defaultdict

from check_single_line_c import fields


def check_cold_rhs(text, config, trace, allow_abort=False):
    if config.get('--coeff-input-batch-ready','0')=='1':
        from check_input_batch import check_batch_rhs
        return check_batch_rhs(text,config,trace,allow_abort)
    consumed = [r for r in fields(text, 'CFD_DIRECT_RHS')
                if r['event'] == 'consume' and r.get('mode') == 'cold']
    special = {'INPUT_R_READY', 'RHS_INPUT_INIT', 'TRSM_INPUT_PARK', 'TRSM_INPUT_RESUME'}
    if config.get('--coeff-cold-rhs-prefetch', '0') != '1':
        assert not consumed and not any(r['stage'] in special for r in trace)
        return dict(cold_requests=0, early_b_requests=0, parked_requests=0)
    cold = {r['request']: r for r in consumed}
    assert len(cold) == len(consumed), 'R consumed twice'
    groups = defaultdict(list)
    for r in trace:
        groups[r['requestId']].append(r)
        if r['stage'] in special:
            assert r['requestId'] in cold, 'cold RHS event has no R owner'
    aborted = {r['requestId'] for r in trace if r['stage'] in
               ('CANCEL_PENDING', 'CANCEL_COMPLETE', 'REQUEST_FAILED')}
    early = early_done = parks = 0
    for request, source in cold.items():
        rows = groups[request]
        cancelled = allow_abort and request in aborted

        def once(stage, optional=False):
            matches = [r for r in rows if r['stage'] == stage]
            assert len(matches) <= 1 if optional else len(matches) == 1, stage
            return int(matches[0]['cycle']) if matches else None

        ready = once('INPUT_R_READY')
        assert ready == int(source['cycle'])
        assert all(r['generation'] == source['generation'] and r['cell'] == source['cell']
                   for r in rows), 'cold R generation changed'
        full = once('INPUT_TOKEN', optional=cancelled)
        issue = once('COEFF3_ISSUE', optional=cancelled)
        b_done = once('DIRECT_BASE_READY', optional=cancelled)
        if issue is not None:
            assert issue >= ready
            early += full is not None and issue < full
        if b_done is not None:
            early_done += full is not None and b_done < full
        init = {}
        complete = {}
        for r in rows:
            key = r['rhsBatch'], r['rhsColumn']
            if r['stage'] == 'RHS_INPUT_INIT':
                assert key not in init, 'RHS initialized twice'
                assert key[0] in ('0', '1', '2')
                assert key[0] != '0' or key[1] == '0'
                dependency = ready if key[0] == '0' else full
                assert dependency is not None and int(r['cycle']) >= dependency, 'RHS initialized before input'
                init[key] = int(r['cycle'])
            elif r['stage'] == 'RHS_COMPLETE':
                assert key not in complete
                complete[key] = int(r['cycle'])
        if not cancelled:
            assert init.keys() == complete.keys() and ('0', '0') in init
            once('COEFF3_COMPLETE')
        else:
            assert complete.keys() <= init.keys()
        assert all(complete[k] >= init[k] for k in complete)
        for r in rows:
            if (r['stage'].startswith('TRSM_') and r['stage'].endswith('_ISSUE')
                    and int(r['rhsBatch']) >= 0):
                key = r['rhsBatch'], r['rhsColumn']
                assert key in init and int(r['cycle']) >= init[key], 'arithmetic before RHS initialization'
        park = once('TRSM_INPUT_PARK', optional=True)
        resume = once('TRSM_INPUT_RESUME', optional=True)
        if park is not None:
            assert b_done is not None and park >= b_done
            assert full is None or park <= full
            assert cancelled or resume is not None
            parks += 1
        if resume is not None:
            assert park is not None and full is not None
            assert resume >= max(full, park+1), 'resume bypassed input or registered selection'

    # One shared pool for all cold and warm calls, with no extra credits for
    # parked requests. A parked b has finished; no partial RHS is spilled.
    active = set()
    parked = set()
    peak = 0
    for r in sorted(enumerate(trace), key=lambda pair: (int(pair[1]['cycle']), pair[0])):
        row = r[1]
        request, stage = row['requestId'], row['stage']
        if stage == 'COEFF3_ISSUE':
            assert request not in active and request not in parked
            active.add(request)
        elif stage == 'TRSM_INPUT_PARK':
            assert request in active and request not in parked
            active.remove(request)
            parked.add(request)
        elif stage == 'TRSM_INPUT_RESUME':
            assert request in parked and request not in active
            parked.remove(request)
            active.add(request)
        elif stage == 'COEFF3_COMPLETE':
            assert request in active and request not in parked
            active.remove(request)
        elif stage in ('CANCEL_PENDING', 'CANCEL_COMPLETE', 'REQUEST_FAILED'):
            assert allow_abort
            active.discard(request)
            parked.discard(request)
        elif stage.startswith('TRSM_') and stage.endswith('_ISSUE') and int(row['rhsBatch']) >= 0:
            assert request in active and request not in parked, 'parked RHS issued arithmetic'
        peak = max(peak, len(active))
        assert len(active) <= int(config['--coeff3-count']), 'RHS contexts overbooked'
    assert not active and not parked, 'RHS execution credit leak'
    return dict(cold_requests=len(cold), early_b_requests=early,
                early_b_completed=early_done, parked_requests=parks,
                peak_contexts=peak, resume_selection_cycles=1)
