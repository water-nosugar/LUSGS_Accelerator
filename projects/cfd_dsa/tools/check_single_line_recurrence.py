"""Independent replay of streaming reductions and bounded compute/commit credits."""
from collections import Counter, defaultdict
from check_single_line_c import fields
from check_register_ports import rf_enabled, replay_register_ports


def check_wait_accounting(text):
    """Cycle unions overlap across reasons; frontier states partition time."""
    waits = {r['reason']: r for r in fields(text,'CFD_WAIT_CYCLES')}
    rejects = {(r['caller'],r['resource']): r for r in fields(text,'CFD_FU_WAIT')}
    states = {r['state']: r for r in fields(text,'CFD_FRONTIER_STATE')}
    if not (waits or rejects or states): return {}
    pools = {r['pool']:r for r in fields(text,'CFD_RESOURCE_STATS')}
    samples = int(pools['shared_mul']['samples'])
    for r in [*waits.values(),*rejects.values(),*states.values()]:
        assert int(r['samples']) == samples
        assert 0 <= int(r['cycles']) <= samples, 'visit count presented as cycles'
    assert sum(int(r['cycles']) for r in states.values()) == samples, 'frontier state partition lost cycles'
    assert len(rejects) == 30
    for r in rejects.values():
        assert 0 <= int(r['frontier_cycles']) <= int(r['cycles'])
    return dict(samples=samples, reasons=waits, fu_rejections=list(rejects.values()),
                frontier_states=states)


def check_cached_diagonal(text, config, trace, storage):
    encoded = fields(text,'CFD_LU_CACHE_FORMAT')
    enabled = config.get('--coeff-warm-diagonal','divide') == 'reciprocal'
    fixture = fields(text,'CFD_CACHE_CONTRACT')
    schedules = fields(text,'CFD_LINE_SCHEDULE')
    interface = fields(text,'CFD_INTERFACE_DMA')
    reused = 0
    for i,line in enumerate(schedules):
        token = line['token'];hit = int(line['cache_hit'])
        mode = fixture[i]['mode'] if fixture else config['--coeff-base-solve']
        size = next(r for r in storage if r['token']==token)
        n = int(size['cells'])
        active = enabled and mode=='direct' and (n>1 or config.get('--coeff-lu-reciprocal-share','0')=='1') and int(size['cache_bytes'])>0
        entries = [r for r in encoded if r['token']==token]
        assert len(entries)==(n if active and not hit else 0)
        assert all(r['format']=='reciprocal-diagonal' for r in entries)
        assert len({r['cell'] for r in entries})==len(entries)
        start = min(int(r['cycle']) for r in interface if r['token']==token and r['kind']=='descriptor')
        end = int(line['visible_done'])
        rows = [r for r in trace if start<=int(r['cycle'])<=end]
        flags = [r for r in rows if r['stage']=='LU_RECIPROCAL_REUSE']
        assert len(flags)==(n if active and hit else 0)
        if flags:
            requests = {r['requestId'] for r in flags}
            assert not any(r['stage'] in ('DIV_ISSUE','RECIP_DIV_ISSUE') for r in rows), 'warm cache recomputed a diagonal divide'
            multiplies = Counter(r['requestId'] for r in rows if r['stage']=='RECIP_MUL_ISSUE')
            assert set(multiplies)==requests and all(v==5 for v in multiplies.values())
            reused += len(flags)
    if not enabled:assert not encoded
    return dict(reused_rhs=reused)


def check_tree(text, config, cluster, rf=None):
    products = fields(text, 'CFD_MVM_PRODUCT')
    nodes = fields(text, 'CFD_MVM_TREE')
    feedback = fields(text, 'CFD_MVM_FEEDBACK')
    signed = config.get('--coeff-backward-feedback-tree','0') == '1'
    if config['--coeff-line-mvm-model'] != 'tree':
        assert not products and not nodes and not feedback
        return {}
    assert signed or not feedback
    mul_lat = int(config['--coeff-cluster-mul-lat'])
    add_lat = int(config['--coeff-cluster-add-lat'])
    if rf_enabled(config) and rf is None:
        rf = replay_register_ports(text, config, cluster)
    contexts = defaultdict(lambda: {'p': {}, 'n': {}, 'f': {}})
    def key(r):
        return tuple(r[k] for k in ('token', 'context', 'epoch', 'cell'))
    expected = Counter()
    for kind, rows, latency, pool, caller in (
            ('p', products, mul_lat, 'mul', '8'),
            ('n', nodes, add_lat, 'add', '9'),
            ('f', feedback, add_lat, 'add', '9')):
        for r in rows:
            group = contexts[key(r)][kind]
            index = int(r['column' if kind == 'p' else 'node'])
            assert index in range(4 if kind == 'n' else 5)
            if kind == 'f': assert signed and r['context'] == '1'
            one = group.setdefault(index, {})
            assert r['event'] in ('issue', 'complete') and r['event'] not in one
            one[r['event']] = int(r['cycle'])
            if r['event'] == 'issue':
                expected[pool, '7' if kind == 'f' and index == 4 else caller, r['cycle']] += 1
    legacy_cluster=[r for r in cluster if not (
        config.get('--coeff-lane-mvm','0')=='1' and r['caller'] in ('7','8','9') and int(r['lanes'])==1)]
    actual = Counter((r['pool'], r['caller'], r['cycle']) for r in legacy_cluster
                     if r['caller'] in ('8', '9') or
                     (r['caller'] == '7' and ('add','7',r['cycle']) in expected))
    assert expected == actual, 'reduction bypassed or duplicated physical issue'
    early = signed_count = 0
    writes = Counter()
    for context, state in contexts.items():
        p = state['p']
        is_feedback = signed and context[1] == '1'
        n = state['f' if is_feedback else 'n']
        assert not state['n' if is_feedback else 'f']
        assert set(p) == set(range(5)) and set(n) == set(range(5 if is_feedback else 4))
        for group, latency, pool in ((p, mul_lat, 'mul'), (n, add_lat, 'add')):
            for index, r in group.items():
                assert set(r) == {'issue', 'complete'}
                if rf is None:
                    assert r['complete'] - r['issue'] == latency
                else:
                    caller = '8' if pool == 'mul' else '7' if is_feedback and index == 4 else '9'
                    writes[pool, caller, r['issue'], r['complete'], 5] += 1
        dependencies = ({0: [p[0]], 1: [p[1], p[2]], 2: [p[3], p[4]],
                         3: [n[0], n[1]], 4: [n[3], n[2]]} if is_feedback else
                        {0: [p[0], p[1]], 1: [p[2], p[3]],
                         2: [n[0], n[1]], 3: [n[2], p[4]]})
        for node, deps in dependencies.items():
            assert n[node]['issue'] >= max(d['complete'] for d in deps), 'tree input not ready'
        early += n[0]['issue'] < max(r['complete'] for r in p.values())
        signed_count += is_feedback
    if rf is not None:
        assert all(rf.completions[key] >= count for key, count in writes.items()), 'tree completion did not match an actual RF write'
    result = dict(contexts=len(contexts), early_first_pairs=early)
    if signed: result['feedback_contexts'] = signed_count
    return result


def check_data_commit(text, config, trace, allow_abort=False):
    depth = int(config.get('--coeff-data-commit-depth', '0'))
    summaries = fields(text, 'CFD_DATA_COMMIT')
    if not depth:
        assert not summaries
        assert not any(r['stage'].startswith('DATA_COMMIT_') for r in trace)
        return {}
    assert summaries and depth <= int(config['--coeff-output-buffer-depth'])
    capacity = sum(int(config[k]) for k in ('--coeff-input-slots', '--lu5-pending-depth',
                    '--coeff3-pending-depth', '--coeff-output-buffer-depth'))
    by_request = defaultdict(list)
    for r in trace:
        by_request[r['requestId']].append(r)
    compute_changes = Counter(); total_changes = Counter(); commit_changes = Counter()
    issued = 0
    for rows in by_request.values():
        stages = defaultdict(list)
        for r in rows:
            stages[r['stage']].append(int(r['cycle']))
        assert len(stages['REQUEST_ALLOC']) == 1
        alloc = stages['REQUEST_ALLOC'][0]
        retire = min(stages['REQUEST_DETACH'] or stages['REQUEST_AUTO_RETIRE'])
        total_changes[alloc] += 1; total_changes[retire] -= 1
        compute_changes[alloc] += 1
        detaches = stages['DATA_COMMIT_DETACH']; releases = stages['DATA_COMMIT_RELEASE']
        if not detaches:
            assert not releases
            compute_changes[retire] -= 1
            continue
        issued += 1
        assert len(detaches) == len(releases) == 1
        detach, release = detaches[0], releases[0]
        assert alloc <= detach <= release == retire
        assert stages['COEFF3_COMPLETE'] and stages['HANDOFF_PRODUCE']
        assert max(stages['COEFF3_COMPLETE'] + stages['HANDOFF_PRODUCE']) <= detach
        failed = any(stages[s] for s in ('REQUEST_FAILED', 'CANCEL_PENDING', 'CANCEL_COMPLETE'))
        assert not failed or allow_abort
        if not failed:
            assert len(stages['REQUEST_COMPLETE']) == 1
            assert detach <= stages['REQUEST_COMPLETE'][0] <= release
            # All data packet payloads must have left compute state before
            # detach; completion must still wait for every actual response.
            drain_issues = [int(r['cycle']) for r in rows if r['stage'] == 'SPM_PACKET_ISSUE'
                            and r['requester'] == 'CoeffDrain']
            responses = [int(r['cycle']) for r in rows if r['stage'] == 'SPM_PACKET_RESPONSE']
            assert drain_issues and max(drain_issues) <= detach
            assert responses and max(responses) <= stages['REQUEST_COMPLETE'][0]
        compute_changes[detach] -= 1
        commit_changes[detach] += 1; commit_changes[release] -= 1
    def replay(changes, bound):
        live = peak = 0
        for cycle, delta in sorted(changes.items()):
            live += delta; peak = max(peak, live)
            assert 0 <= live <= bound, 'request/commit credit capacity violated'
        assert live == 0, 'leaked request/commit credits'
        return peak
    total_peak = replay(total_changes, capacity)
    compute_peak = replay(compute_changes, int(config['--coeff-stream-window']))
    commit_peak = replay(commit_changes, depth)
    last = summaries[-1]
    assert int(last['depth']) == depth and int(last['capacity']) == capacity
    assert int(last['issued']) == int(last['released']) == issued and last['live'] == '0'
    # Same-cycle acquire/release can have a greater transient peak than the
    # end-of-cycle replay; both remain bounded by the physical queue depth.
    assert commit_peak <= int(last['peak']) <= depth
    return dict(detached=issued, total_peak=total_peak, compute_peak=compute_peak,
                commit_peak=int(last['peak']), capacity=capacity)
