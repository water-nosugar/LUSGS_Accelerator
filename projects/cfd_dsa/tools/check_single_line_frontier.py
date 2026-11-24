"""Replay finite forward/RHS reductions and age-ordered drain admission."""
from collections import Counter, defaultdict
from check_single_line_c import fields
from check_register_ports import rf_enabled, replay_register_ports
from check_single_line_output_dma import drain_task_depth


def check_forward_tree(text, config, trace):
    rows = fields(text, 'CFD_FORWARD_TREE')
    warm = config.get('--coeff-warm-forward-tree', '0') == '1'
    cold = config.get('--coeff-cold-forward-tree', '0') == '1'
    if not (warm or cold):
        assert not rows
        return {}
    if cold:
        assert warm and config['--coeff-line-mvm-model'] == 'tree'
        assert config.get('--coeff-lane-mvm', '0') != '1'
    requests = defaultdict(dict)
    identities, columns = {}, defaultdict(dict)
    for r in trace:
        requests[r['requestId']].setdefault(r['stage'], int(r['cycle']))
        if cold:
            identity = (r['generation'], r['cell'])
            assert identities.setdefault(r['requestId'], identity) == identity
            if r['stage'] == 'RHS_COMPLETE' and r['rhsBatch'] == '1':
                col = int(r['rhsColumn'])
                assert col not in columns[r['requestId']], 'duplicate Lbar producer'
                columns[r['requestId']][col] = int(r['cycle'])
    expected = {key for key, r in requests.items() if 'HANDOFF_CONSUME' in r
                and ((warm and 'LU_CACHE_REUSE' in r) or (cold and 'LU_CACHE_REUSE' not in r))}
    groups = defaultdict(dict)
    for r in rows:
        event = r['event']
        assert event in ('begin', 'complete') and event not in groups[r['request']]
        groups[r['request']][event] = r
    assert set(groups) == expected, 'missing/duplicate selected forward tree'
    products = fields(text, 'CFD_MVM_PRODUCT')
    nodes = fields(text, 'CFD_MVM_TREE')
    lane_rows = fields(text,'CFD_LANE_MVM') if config.get('--coeff-lane-mvm','0')=='1' else None
    for key, events in groups.items():
        assert set(events) == {'begin', 'complete'}
        begin, end = events['begin'], events['complete']
        identity = tuple(begin[k] for k in ('token', 'epoch', 'cell'))
        assert identity == tuple(end[k] for k in ('token', 'epoch', 'cell'))
        def select(rows):
            return [r for r in rows if r['context'] == '0' and
                    tuple(r[k] for k in ('token', 'epoch', 'cell')) == identity]
        p, n = select(products), select(nodes)
        if lane_rows is not None:
            selected=select(lane_rows)
            p=[r for r in selected if int(r['node'])<5]
            n=[r for r in selected if int(r['node'])>=5]
            assert len(p)==50 and len(n)==40
        else:
            assert len(p) == 10 and len(n) == 8
        is_cold = 'LU_CACHE_REUSE' not in requests[key]
        if is_cold:
            assert cold and begin.get('mode') == end.get('mode') == 'cold'
            assert (begin['generation'], begin['cell']) == identities[key]
            assert (end['generation'], end['cell']) == identities[key]
            assert set(columns[key]) == set(range(5)), 'missing cold Lbar producer'
            assert Counter((int(r['column']), r['event']) for r in p) == Counter(
                (col, event) for col in range(5) for event in ('issue', 'complete'))
            ready = requests[key]['HANDOFF_CONSUME']
            for product in p:
                if product['event'] == 'issue':
                    assert int(product['cycle']) >= columns[key][int(product['column'])], 'cold product before actual Lbar return'
        else:
            ready = max(requests[key]['HANDOFF_CONSUME'], requests[key]['CACHE_REGISTER_READY'])
        assert int(begin['cycle']) >= ready
        assert min(int(r['cycle']) for r in p if r['event'] == 'issue') >= ready
        final_cycle=(max(int(r['cycle']) for r in n if r['node']=='8' and r['event']=='complete')
                     if lane_rows is not None else
                     int(next(r for r in n if r['node']=='3' and r['event']=='complete')['cycle']))
        assert int(end['cycle']) == final_cycle == requests[key]['CORRECTION_ACCUMULATOR_COMPLETE']
    result = dict(requests=len(groups))
    if cold:
        result['cold_requests'] = sum('LU_CACHE_REUSE' not in requests[key] for key in groups)
    return result


def check_rhs_tree(text, config, trace, cluster, rf=None):
    rows = fields(text, 'CFD_RHS_TREE')
    warm = config.get('--coeff-warm-rhs-tree', '0') == '1'
    cold = config.get('--coeff-cold-b-tree', '0') == '1'
    scope = config.get('--coeff-cold-rhs-tree', 'none')
    assert scope in ('none', 'lbar', 'forward', 'all')
    if not (warm or cold or scope != 'none'):
        assert not rows
        return {}
    if rf_enabled(config) and rf is None:
        rf = replay_register_ports(text, config, cluster)
    reused = {r['requestId'] for r in trace if r['stage'] == 'LU_CACHE_REUSE'}
    ends = {r['requestId']: int(r['cycle']) for r in trace if r['stage'] == 'DIRECT_BASE_READY'}
    enabled = (reused if warm else set()) | (set(ends)-reused if cold or scope != 'none' else set())
    completed = {(r['requestId'], int(r['rhsBatch'])*5+int(r['rhsColumn'])): int(r['cycle'])
                 for r in trace if r['stage'] == 'RHS_COMPLETE'}
    selected = {(request, 0) for request in enabled
                if request in reused or cold or scope in ('forward', 'all')}
    if scope != 'none':
        selected |= {(request, index) for request, index in completed
                     if request in enabled-reused and
                     (5 <= index < 10 or (scope == 'all' and 10 <= index < 15))}
    ends = {**{(request, 0): cycle for request, cycle in ends.items()}, **completed}
    groups = defaultdict(lambda: defaultdict(dict))
    physical = Counter((r['pool'], r['caller'], r['cycle'], int(r['lanes'])) for r in cluster)
    needed = Counter()
    for r in rows:
        rhs = int(r.get('rhs', '0'))  # Old b-only traces remain readable.
        assert (r['request'], rhs) in selected, 'unselected RHS used tree'
        group = groups[r['request'], rhs, int(r['k'])]
        index, event = int(r['node']), r['event']
        assert event not in group[index]
        group[index][event] = int(r['cycle'])
        if event in ('issue', 'products_issue'):
            needed['mul' if index == -1 else 'add', '4' if index == -1 else '5',
                   r['cycle'], 4-int(r['k']) if index == -1 else 1] += 1
    assert set(groups) == {(req, rhs, k) for req, rhs in selected for k in range(4)}, 'missing RHS tree row'
    assert all(physical[key] >= value for key, value in needed.items()), 'RHS tree bypassed physical issue'
    writes = Counter()
    for (request, rhs, k), group in groups.items():
        terms = 4-k
        assert set(group) == {-1, *range(terms)}
        product = group[-1]
        assert set(product) == {'products_issue', 'products_ready'}
        if rf is None:
            assert product['products_ready']-product['products_issue'] == int(config['--coeff-cluster-mul-lat'])
        else:
            writes['mul', '4', product['products_issue'], product['products_ready'], terms] += 1
        for node in range(terms):
            event = group[node]
            assert set(event) == {'issue', 'complete'}
            if rf is None:
                assert event['complete']-event['issue'] == int(config['--coeff-cluster-add-lat'])
            else:
                writes['add', '5', event['issue'], event['complete'], 1] += 1
            deps = [] if node == 0 or (node == 1 and terms >= 3) else [0] if node == 1 else [0, 1] if node == 2 else [2]
            assert event['issue'] >= max([product['products_ready']]+[group[n]['complete'] for n in deps]), 'RHS tree input not ready'
        if k == 0:
            assert group[terms-1]['complete'] <= ends[request, rhs]
        else:
            assert group[terms-1]['complete'] <= groups[request, rhs, k-1][-1]['products_issue']
    if rf is not None:
        assert all(rf.completions[key] >= count for key, count in writes.items()), 'RHS completion did not match an actual RF write'
    result = dict(requests=len(enabled), rows=len(groups))
    if scope != 'none': result['rhs'] = len(selected)
    if cold or scope != 'none': result['cold_requests'] = len(enabled-reused)
    return result


def check_drain_arb(text, config, trace, allow_abort=False):
    summaries = fields(text, 'CFD_DRAIN_ARB')
    if config.get('--coeff-drain-policy', 'legacy') != 'age':
        assert not summaries
        assert not any(r['stage'] in ('DRAIN_READY', 'DRAIN_GRANT') for r in trace)
        return {}
    latency = int(config.get('--coeff-drain-arb-latency', '1'))
    ii = int(config.get('--coeff-drain-arb-ii', '1'))
    pending, live = {}, set()
    next_grant = granted = wait_sum = wait_max = peak = 0
    finished = {'DRAIN_LU_DONE': 0, 'DRAIN_DINV_DONE': 1, 'DRAIN_LBAR_DONE': 2,
                'DRAIN_UBAR_DONE': 3, 'DRAIN_BASE_VECTOR_DONE': 4,
                'DRAIN_CORRECTION_VECTOR_DONE': 5, 'DRAIN_DQSTAR_VECTOR_DONE': 6}
    for r in trace:
        stage, req, cycle = r['stage'], r['requestId'], int(r['cycle'])
        if stage in ('REQUEST_FAILED', 'CANCEL_PENDING', 'CANCEL_COMPLETE'):
            assert allow_abort
            pending = {key: value for key, value in pending.items() if key[0] != req}
            live = {key for key in live if key[0] != req}
        elif stage == 'DRAIN_READY':
            key = req, int(r['k'])
            assert key not in pending and key not in live
            pending[key] = cycle
        elif stage == 'DRAIN_GRANT':
            key = req, int(r['k'])
            eligible = [key for key, t in pending.items() if cycle-t >= latency]
            assert key in eligible, 'drain granted before arbitration latency'
            assert key == min(eligible, key=lambda key: (pending[key], int(key[0]), key[1])), 'drain admission starved an older eligible descriptor'
            assert cycle >= next_grant, 'drain arbiter II violated'
            next_grant = cycle+ii
            wait = cycle-pending.pop(key)
            wait_sum += wait
            wait_max = max(wait_max, wait)
            live.add(key)
            peak = max(peak, len(live))
            assert peak <= drain_task_depth(config)
            granted += 1
        elif stage in finished:
            key = req, finished[stage]
            assert key in live, 'ungranted drain completed'
            live.remove(key)
    assert not pending and not live, 'drain readiness/output credits leaked'
    summary = summaries[-1]
    assert int(summary['latency']) == latency and int(summary['ii']) == ii
    assert int(summary['granted']) == granted and int(summary['wait_sum']) == wait_sum
    assert int(summary['wait_max']) == wait_max
    for capacity in fields(text,'CFD_DRAIN_CAPACITY'):
        depth=drain_task_depth(config)
        assert int(capacity['depth'])==depth
        assert int(capacity['active_limit'])==int(config.get('--coeff-drain-outstanding','4'))
        assert int(capacity['extra_bytes'])==64*max(0,depth-int(config['--coeff-output-buffer-depth']))
        assert 0 <= int(capacity['active_peak']) <= min(depth,int(capacity['active_limit']))
        assert 0 <= int(capacity['peak']) <= depth and int(capacity['live'])==0
    return dict(granted=granted, wait_max=wait_max, peak=peak)


def check_tail_overlap(text, config, trace, storage):
    """Check bounded LU retention, y stores and delayed Ubar activation."""
    depth = int(config.get('--coeff-tail-ubar-overlap', '0'))
    yrows = fields(text, 'CFD_LOCAL_Y_READY')
    if not depth:
        assert not yrows and not any(r['stage']=='TAIL_UBAR_HOLD' for r in trace)
        return {}
    stages = defaultdict(lambda: defaultdict(list))
    for row in trace:
        stages[row['requestId']][row['stage']].append(row)
    held = {request for request, events in stages.items() if events['TAIL_UBAR_HOLD']}
    n = int(config['--lusgs-cells'])
    schedules = {r['token']: r for r in fields(text, 'CFD_LINE_SCHEDULE')}
    starts = {r['token']: int(r['cycle']) for r in fields(text, 'CFD_EARLY_BACKWARD')}
    layouts = {r['token']: r for r in storage}
    jobs = {r['id']: r for r in fields(text, 'CFD_ONCHIP_START')}
    done = {r['id']: int(r['cycle']) for r in fields(text, 'CFD_ONCHIP_COMPLETE')}
    expected = {(token, str(cell)) for token, s in schedules.items()
                if s['cache_hit'] == '0' for cell in range(max(0,n-1-depth),n-1)}
    observed = set()
    assert len(yrows) == len(held) == len(expected), 'missing deferred y/held request'
    for y in yrows:
        token, cell, request = y['token'], y['cell'], y['request']
        key = token, cell
        assert key not in observed and key in expected
        observed.add(key)
        events = stages[request]
        assert len(events['TAIL_UBAR_HOLD']) == len(events['TAIL_UBAR_RELEASE']) == 1
        hold, release = events['TAIL_UBAR_HOLD'][0], events['TAIL_UBAR_RELEASE'][0]
        for r in (hold, release):
            assert r['cell'] == cell and r['generation'] == y['generation']
        begin, end = int(hold['cycle']), int(release['cycle'])
        assert begin <= int(y['cycle']) <= starts[token] <= end
        assert starts[token] == int(schedules[token]['backward_start'])
        assert int(events['DIRECT_BASE_READY'][0]['cycle']) <= int(y['cycle'])
        if int(cell):
            lower = [r for r in events['RHS_COMPLETE'] if r['rhsBatch'] == '1']
            assert {r['rhsColumn'] for r in lower} == set(map(str,range(5)))
            assert max(int(r['cycle']) for r in lower) <= int(y['cycle'])
        activated = events['TAIL_UBAR_ACTIVATE']
        assert len(activated) == 5 and {r['rhsColumn'] for r in activated} == set(map(str,range(5)))
        assert all(r['rhsBatch'] == '2' and int(r['cycle']) >= end for r in activated)
        assert int(events['UBAR_READY'][0]['cycle']) >= max(int(r['cycle']) for r in activated)
        job = jobs[y['y_job']]
        assert job['op'] == 'W' and job['owner'] == token
        assert int(job['words']) == 5 and int(job['stride']) == 8
        assert done[y['y_job']] <= int(y['cycle']), 'y ready before SPM write response'
        layout = layouts[token]
        tile_bytes = int(layout['resident'])//(int(config['--coeff-line-tile-buffers'])+
            int(config.get('--coeff-tile-prefetch-buffers','0')))
        address = int(layout['reserved'])+int(y['storage'])*tile_bytes+(int(cell)-int(y['first']))*40
        assert int(job['base'],16) == address, 'deferred y stored in wrong tile cell'
    assert observed == expected
    return dict(deferred_cells=len(held), depth=depth,
                early_sweeps=len({y['token'] for y in yrows}))


def check_lu_lookahead(text, config, cluster):
    radius = int(config.get('--coeff-lu-lookahead', '0'))
    rows = fields(text, 'CFD_LU_LOOKAHEAD')
    if not radius:
        assert not rows
        return {}
    assert radius in (1,2)
    available = Counter()
    for r in cluster:
        available[r['cycle'],r['pool'],r['caller']] += int(r['lanes'])
    used = Counter()
    totals = Counter()
    identities = set()
    for r in rows:
        assert 1 <= int(r['cell'])-int(r['frontier']) <= radius
        key = r['request'],r['cycle']
        assert key not in identities
        identities.add(key)
        count = 0
        for pool,caller in [('mul','1'),('add','2'),('div','0'),('lu_fma','15')]:
            n = int(r.get(pool,'0'))
            assert not n or pool!='lu_fma' or int(config.get('--coeff-lu-fma-count','0'))>0
            assert n >= 0
            used[r['cycle'],pool,caller] += n
            totals[pool] += n;count += n
        assert count, 'lookahead counted a control visit as useful work'
    assert all(n <= available[key] for key,n in used.items()), 'lookahead bypassed real LU lanes'
    return dict(events=len(rows),radius=radius,**totals)
