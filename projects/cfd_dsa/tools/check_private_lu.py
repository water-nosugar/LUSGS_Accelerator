"""Independently verify the opt-in private LU publication contract."""
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re


def fields(text, prefix):
    return [dict(re.findall(r'(\w+)=([^\s]+)', line))
            for line in text.splitlines() if line.startswith(prefix+' ')]


def validate_private_lu(text, config, trace):
    transition = config.get('--coeff-lu-output-transition-test', '0') == '1'
    private = config.get('--coeff-publish-lu', '1') == '0'
    ranges = fields(text, 'CFD_LU_OUTPUT_RANGE')
    if not ranges:
        if transition or private:
            raise ValueError('private LU requires explicit guest output address ranges')
        return {}
    recovery = config.get('--coeff-recovery-test','0') == '1'
    schedules = fields(text, 'CFD_LINE_SCHEDULE')
    numeric = fields(text, 'CFD_SINGLE_LINE_NUMERIC')
    outcomes = [True]*len(ranges)
    abort_visible = {}
    abort_tokens = []
    if recovery:
        steps = fields(text,'CFD_RECOVERY_CONTRACT')
        expected_count = 6 if int(config['--lusgs-cells']) >= 17 else 4
        if transition or not private or len(steps) != expected_count or len(ranges) != expected_count:
            raise ValueError('private LU recovery must execute the entire direct recovery fixture')
        if 'LUSGS_RECOVERY_CONTRACT_PASS' not in text.splitlines():
            raise ValueError('private LU recovery completion missing')
        for i,step in enumerate(steps):
            if int(step['step']) != i or int(step['actual']) != int(step['expected']) or int(step['actual']) != i%2:
                raise ValueError('private LU recovery result sequence differs')
        outcomes = [bool(int(step['actual'])) for step in steps]
        aborts = fields(text,'CFD_LINE_ABORT')
        abort_tokens = [r['token'] for r in aborts if r['event']=='begin']
        abort_visible = {r['token']:r for r in aborts if r['event']=='visible'}
        if (len(abort_tokens) != outcomes.count(False) or len(set(abort_tokens)) != len(abort_tokens) or
                set(abort_visible) != set(abort_tokens) or
                any(int(r[k]) for r in abort_visible.values() for k in ('dma_live','records','children'))):
            raise ValueError('private LU recovery abort ownership/fence incomplete')
    if len(schedules) != sum(outcomes) or len(numeric) != sum(outcomes):
        raise ValueError('LU publication/line/numeric round count differs')
    good_schedules,good_numeric,bad_tokens = iter(schedules),iter(numeric),iter(abort_tokens)
    n = int(config['--lusgs-cells'])
    if transition:
        steps = fields(text, 'CFD_LU_OUTPUT_TRANSITION')
        modes, hits = [0, 0, 1, 1, 0], [0, 1, 0, 1, 0]
        if len(steps) != 5 or len(ranges) != 5:
            raise ValueError('LU publication transition must execute all five steps')
        if 'LUSGS_LU_OUTPUT_TRANSITION_PASS' not in text.splitlines():
            raise ValueError('LU publication transition completion missing')
        for i, step in enumerate(steps):
            if (int(step['step']) != i or int(step['cells']) != n or
                    int(step['version']) != 1 or int(step['publish']) != modes[i] or
                    int(step['expected_cache_hit']) != hits[i] or
                    int(step['numeric_pass']) != 1 or int(step['output_contract_pass']) != 1 or
                    int(step['cycles']) <= 0):
                raise ValueError('LU publication transition step invalid')
        if [int(row['cache_hit']) for row in schedules] != hits:
            raise ValueError('LU publication policy change reused an incompatible cache entry')
    else:
        modes = [int(not private)]*len(ranges)
    by_generation = {}
    for i,row in enumerate(ranges):
        success = outcomes[i]
        schedule = next(good_schedules) if success else None
        check = next(good_numeric) if success else None
        base, size, generation = int(row['base'], 0), int(row['bytes']), int(row['generation'])
        if (int(row['round']) != i+1 or int(row['cells']) != n or
                int(row['publish']) != modes[i] or not base or base%8 or size != 200*n or
                generation in by_generation or (success and (int(check['mismatches']) or
                int(check['values']) != 10*n))):
            raise ValueError('LU output range or numeric round invalid')
        by_generation[generation] = dict(index=i, base=base, size=size, mode=modes[i],
            cache_hit=int(schedule['cache_hit']) if success else 0,
            token=schedule['token'] if success else next(bad_tokens), aborted=not success)
    events = defaultdict(dict)
    issues, responses = {}, {}
    all_rows = list(trace)
    for row in all_rows:
        request = row['requestId']
        if row['stage'] == 'REQUEST_ALLOC':
            if request in events:
                raise ValueError('duplicate private LU request allocation')
            generation, cell = int(row['generation']), int(row['cell'])
            parent = generation-cell
            if parent not in by_generation or not 0 <= cell < n:
                raise ValueError('request not owned by an LU publication generation')
            events[request] = dict(parent=parent, cell=cell, stages=Counter(), cycles={})
        if request in events:
            events[request]['stages'][row['stage']] += 1
            events[request]['cycles'][row['stage']] = int(row['cycle'])
            parent = by_generation[events[request]['parent']]
            if not parent['mode'] and (row['stage'] == 'DRAIN_LU_DONE' or
                    (row['stage'] in ('DRAIN_READY','DRAIN_GRANT') and row['k'] == '0')):
                raise ValueError('private LU faked or scheduled an external LU drain')
        if row['stage'] in ('SPM_PACKET_ISSUE','SPM_PACKET_RESPONSE') and row['requester'] == 'CoeffDrain':
            target = issues if row['stage'] == 'SPM_PACKET_ISSUE' else responses
            packet = row['packetId']
            if packet in target:
                raise ValueError('duplicate physical drain packet event')
            target[packet] = (request, int(row['address'], 0), int(row['size']))
    if issues != responses:
        # A cancelled request can disappear before its logical drain callback.
        # The independent physical DMA journal must still acknowledge its write.
        physical = defaultdict(dict)
        for row in fields(text,'CFD_DMA_PACKET'):
            if row['event'] in ('queue','send','response'):
                if row['event'] in physical[row['packet']]:
                    raise ValueError('duplicate physical DMA journal event')
                physical[row['packet']][row['event']]=row
        if any(issues.get(packet) != value for packet,value in responses.items()):
            raise ValueError('physical drain packets have missing/different responses')
        for packet,(request,address,size) in issues.items():
            if packet in responses:continue
            if request not in events:
                raise ValueError('physical drain lacks an owned request')
            parent=by_generation[events[request]['parent']]
            journal=physical.get(packet,{})
            if (not parent['aborted'] or set(journal) != {'queue','send','response'} or
                    journal['queue'].get('op') != 'W' or int(journal['queue']['bytes']) != size or
                    int(journal['send']['tick']) < int(journal['queue']['tick']) or
                    int(journal['response']['tick']) <= int(journal['send']['tick'])):
                raise ValueError('cancelled private LU drain lacks a physical acknowledgement')
    for generation,parent in by_generation.items():
        if parent['aborted']:
            children=[r for r in events.values() if r['parent']==generation]
            reused=any(r['stages']['LU_CACHE_REUSE'] for r in children)
            factored=any(r['stages']['LU_COMPLETE'] for r in children)
            if reused and factored:
                raise ValueError('aborted private LU call mixed cache reuse and rebuilding')
            parent['cache_hit']=int(reused) if reused or factored else None
    expected_requests = Counter((generation, cell) for generation,parent in by_generation.items()
                                if not parent['aborted'] for cell in range(n))
    actual_requests = Counter((row['parent'], row['cell']) for row in events.values()
                              if not by_generation[row['parent']]['aborted'])
    all_identities = Counter((row['parent'],row['cell']) for row in events.values())
    if any(count != 1 for count in all_identities.values()):
        raise ValueError('duplicate private LU request identity')
    if actual_requests != expected_requests:
        raise ValueError('private LU skipped or duplicated numerical requests')
    for request, row in events.items():
        stages = row['stages'];parent = by_generation[row['parent']]
        if parent['aborted']:
            if any(stages[k]>1 for k in ('REQUEST_COMPLETE','REQUEST_FAILED','CANCEL_COMPLETE',
                                         'CANCEL_PENDING','REQUEST_DETACH','LU_COMPLETE','LU_CACHE_REUSE')):
                raise ValueError('duplicate private LU aborted lifecycle event')
            terminal = sum(stages[k] for k in ('REQUEST_COMPLETE','REQUEST_FAILED','CANCEL_COMPLETE'))
            pending_detach = stages['CANCEL_PENDING'] == stages['REQUEST_DETACH'] == 1
            if (stages['REQUEST_AUTO_RETIRE'] != 1 or terminal > 1 or
                    (terminal != 1 and not pending_detach) or
                    row['cycles']['REQUEST_AUTO_RETIRE'] > int(abort_visible[parent['token']]['cycle'])):
                raise ValueError('private LU aborted request did not retire before visibility')
            continue
        for required in ('REQUEST_COMPLETE','REQUEST_AUTO_RETIRE','HANDOFF_PRODUCE','DIRECT_BASE_READY'):
            if stages[required] != 1:
                raise ValueError('private LU bypassed request/forward lifecycle')
        required_factor = 'LU_CACHE_REUSE' if parent['cache_hit'] else 'LU_COMPLETE'
        if stages[required_factor] != 1:
            raise ValueError('private LU has no actual factor calculation/cache reuse')
        if not parent['mode'] and stages['DRAIN_LU_DONE']:
            raise ValueError('private cached LU fabricated public drain completion')
    words = defaultdict(Counter)
    for packet, (request, address, size) in issues.items():
        if request not in events:
            raise ValueError('physical drain lacks an owned request')
        parent = by_generation[events[request]['parent']]
        lo, hi = parent['base'], parent['base']+parent['size']
        if address < hi and address+size > lo:
            if not parent['mode']:
                raise ValueError('physical DMA wrote the private LU output range')
            if not lo <= address < address+size <= hi or address%8 or size%8:
                raise ValueError('LU drain escaped or partially covered its range')
            for offset in range(address-lo, address-lo+size, 8):
                words[parent['index']][offset//8] += 1
    reports=[]
    for generation, row in by_generation.items():
        expected = Counter({i: 1 for i in range(25*n)}) if row['mode'] and not row['cache_hit'] else Counter()
        if words[row['index']] != expected:
            raise ValueError('public LU output missing/duplicated, or warm/private LU unexpectedly written')
        reports.append(dict(round=row['index']+1, generation=generation,
            publish=row['mode'], cache_hit=row['cache_hit'], aborted=row['aborted'],
            physical_lu_bytes=8*sum(words[row['index']].values())))
    # This explicit trace is an audit aid; physical resource and factor events
    # above remain authoritative, rather than treating a ready label as work.
    private_ready = fields(text, 'CFD_PRIVATE_LU')
    if private or transition:
        if not private_ready:
            raise ValueError('private LU contract trace missing')
        ready_count = Counter()
        for row in private_ready:
            if row.get('event') not in ('local-ready','contract'):
                raise ValueError('unknown private LU contract event')
            if row.get('request') not in events:
                raise ValueError('private LU ready event has no owned request')
            request = events[row['request']]
            if (int(row['generation'])-int(row['cell']) != request['parent'] or
                    int(row['cell']) != request['cell'] or
                    by_generation[request['parent']]['mode']):
                raise ValueError('private LU contract identity/publication mismatch')
            if row['event'] == 'local-ready':
                ready_count[row['request']] += 1
                parent=by_generation[request['parent']]
                factor='REQUEST_ALLOC' if parent['cache_hit'] else 'LU_COMPLETE'
                if factor not in request['cycles']:
                    raise ValueError('private LU ready without completed factor')
                end=request['cycles'].get('REQUEST_COMPLETE',request['cycles']['REQUEST_AUTO_RETIRE'])
                factor_cycle=request['cycles'][factor]
                if not factor_cycle <= int(row['cycle']) <= end:
                    raise ValueError('private LU local-ready outside factor/retirement lifetime')
        if ready_count != Counter({request:1 for request,row in events.items()
                if not by_generation[row['parent']]['mode'] and
                (not by_generation[row['parent']]['aborted'] or
                 row['stages']['LU_COMPLETE'] or row['stages']['LU_CACHE_REUSE'])}):
            raise ValueError('private LU local-ready missing or duplicated')
    return dict(transition=transition, recovery=recovery, rounds=reports,
                private_lu_external_dma_bytes=0,
                factor_work_and_request_lifecycle='verified against independent physical/FU/storage checkers')


def check_private_lu(directory, config=None):
    from check_single_line_output_dma import resolved_options
    directory = Path(directory)
    config = resolved_options(directory) if config is None else config
    with (directory/'coeff_preprocess_trace.csv').open() as f:
        trace = [row for row in csv.DictReader(f) if row['stage'] in (
            'REQUEST_ALLOC','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE','HANDOFF_PRODUCE',
            'DIRECT_BASE_READY','LU_COMPLETE','LU_CACHE_REUSE','DRAIN_LU_DONE',
            'REQUEST_FAILED','CANCEL_PENDING','CANCEL_COMPLETE','REQUEST_DETACH',
            'DRAIN_READY','DRAIN_GRANT','SPM_PACKET_ISSUE','SPM_PACKET_RESPONSE')]
    return validate_private_lu((directory/'simout.txt').read_text(), config, trace)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(json.dumps(check_private_lu(parser.parse_args().directory), indent=2))
