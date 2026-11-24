"""Replay cross-point ZA groups, including timed ingress and cancellation."""

from bisect import bisect_right
from collections import Counter, defaultdict
import csv

from trace_text import fields


N = 5
PROGRAM = tuple(
    [(row, k) for k in range(N) for row in range(k, N)] +
    [(row, k) for k in reversed(range(1, N)) for row in range(k)]
)
ROWS = N + len(PROGRAM)
POINT_STAGES = ('INPUT_TOKEN', 'CACHE_REGISTER_READY', 'LU_CACHE_REUSE',
                'SME_RHS_PANEL_LOAD', 'COEFF3_ISSUE', 'DIRECT_BASE_READY',
                'COEFF3_COMPLETE')


def point_timing(directory, arrays, warm):
    ordered = sorted((int(a['start']), a['token'], int(a['done'])) for a in arrays)
    starts = [row[0] for row in ordered]
    points = defaultdict(dict)
    with (directory / 'coeff_preprocess_trace.csv').open(newline='') as source:
        for row in csv.DictReader(source):
            stage = row['stage']
            if stage not in POINT_STAGES:
                continue
            cycle = int(row['cycle'])
            index = bisect_right(starts, cycle) - 1
            if index < 0:
                continue
            _, owner, done = ordered[index]
            if owner not in warm or cycle > done:
                continue
            events = points[owner, int(row['cell'])]
            if stage in events:
                raise ValueError('duplicate packed warm RHS request milestone')
            events[stage] = cycle
    return points


def check_sme_warm_rhs(directory, text, actual, arrays, schedules, aborts, phases):
    """Check independent logical replays against the physical ZA counters."""
    width = actual.get('coeff_sme_warm_rhs_width', 0)
    events = fields(text, 'CFD_SME_WARM_RHS')
    if not width:
        if events:
            raise ValueError('unconfigured packed warm RHS executed')
        return {}
    warm = {t for t, row in schedules.items() if int(row['cache_hit'])}
    warm |= {r['token'] for r in fields(text, 'CFD_SME_ABORT') if int(r['cache_hit'])}
    config = {a['token']: {k: int(v) for k, v in a.items() if k != 'token'} for a in arrays}
    points = point_timing(directory, arrays, warm) if events else {}
    cells = actual['lusgs_cells']
    window = min(actual['coeff_stream_window'], cells)
    tile_cells = actual['coeff_line_tile_cells']
    traced_cells = min(actual['coeff_preprocess_trace_cells'], cells)
    if not 2 <= width <= N or not all(config[t]['warm_rhs_groups'] for t in warm):
        raise ValueError('invalid packed warm RHS configuration')

    live = {}
    next_cell = Counter()
    peaks = Counter()
    totals = defaultdict(Counter)
    gather_cycles = set()
    reads = defaultdict(set)
    last = -1
    for event in events:
        token, kind = event['token'], event['event']
        cycle, group, first, count, index = (int(event[k]) for k in
                                            ('cycle', 'group', 'first', 'count', 'index'))
        a = config[token]
        if (token not in warm or not a['start'] <= cycle <= a['done'] or cycle < last or
                not 0 <= group < a['warm_rhs_groups']):
            raise ValueError('packed warm RHS event outside the cache-hit owner')
        last = cycle
        stop = int(aborts[token]['cycle']) if token in aborts else a['done']
        if kind != 'drop' and cycle > stop:
            raise ValueError('packed warm RHS issued work after cancellation')
        key = token, group
        total = totals[token]
        if kind == 'bind':
            limit = min(width, window, cells - first, tile_cells - first % tile_cells)
            if key in live or first != next_cell[token] or count != limit or limit <= 0:
                raise ValueError('invalid packed warm RHS group admission')
            for cell in range(first, min(first + count, traced_cells)):
                p = points[token, cell]
                if not p['CACHE_REGISTER_READY'] <= p['INPUT_TOKEN'] <= p['LU_CACHE_REUSE'] == cycle:
                    raise ValueError('packed warm RHS admitted a point before timed operands')
            live[key] = dict(first=first, count=count, loads=[], issued=0, exported=0,
                             ready=[0] * N)
            next_cell[token] += count
            total['groups'] += 1
            total['points'] += count
            peaks[token] = max(peaks[token], len(live))
            continue

        group_state = live[key]
        if (first, count) != (group_state['first'], group_state['count']):
            raise ValueError('packed warm RHS group changed owner or width')
        loaded = group_state['loads']
        ready = group_state['ready']
        if kind == 'load':
            if index != len(loaded) or index >= ROWS or cycle in gather_cycles:
                raise ValueError('packed warm RHS exceeded the registered gather port')
            loaded.append(cycle)
            gather_cycles.add(cycle)
            total['load_rows'] += 1
            total['load_words'] += count
            if index < N:
                ready[index] = cycle + 1
            if index in (0, N - 1):
                stage = 'SME_RHS_PANEL_LOAD' if index == 0 else 'COEFF3_ISSUE'
                for cell in range(first, min(first + count, traced_cells)):
                    if points[token, cell][stage] != cycle:
                        raise ValueError('packed warm RHS stage differs from ZA ingress')
        elif kind == 'issue':
            if index != group_state['issued'] or not index < len(PROGRAM):
                raise ValueError('packed warm RHS changed the scalar operation order')
            row, pivot = PROGRAM[index]
            factor = N + index
            if len(loaded) <= factor or max(ready[row], ready[pivot], loaded[factor] + 1) > cycle:
                raise ValueError('packed warm RHS used a pending factor or RHS row')
            op = 'mul' if row == pivot else 'fma'
            ready[row] = cycle + a[op + '_latency']
            reads[token, cycle].update((group, r) for r in (row, pivot, factor))
            group_state['issued'] += 1
            total[op] += count
            total['beats'] += 1
        elif kind == 'export':
            if (group_state['issued'] != len(PROGRAM) or index != group_state['exported'] or
                    index >= N or ready[index] > cycle):
                raise ValueError('packed warm RHS exported before arithmetic returned')
            reads[token, cycle].add((group, index))
            group_state['exported'] += 1
        elif kind in ('release', 'drop'):
            if max(ready) > cycle:
                raise ValueError('packed warm RHS recycled a group with pending ZA data')
            if kind == 'release':
                if group_state['exported'] != N:
                    raise ValueError('packed warm RHS released an incomplete solve')
                for cell in range(first, min(first + count, traced_cells)):
                    p = points[token, cell]
                    if p['DIRECT_BASE_READY'] != cycle or p['COEFF3_COMPLETE'] != cycle:
                        raise ValueError('packed warm RHS published outside group retirement')
                total['completed'] += count
            elif token not in aborts or cycle < stop:
                raise ValueError('packed warm RHS discarded a group without cancellation')
            else:
                total['dropped'] += 1
            del live[key]
        else:
            raise ValueError('unknown packed warm RHS event')

    if live:
        raise ValueError('packed warm RHS leaked a group after retirement')
    for (token, _), rows in reads.items():
        if len(rows) > config[token]['reads']:
            raise ValueError('packed warm RHS exceeded ZA row read ports')
        totals[token]['read_rows'] += len(rows)
    for token in warm:
        t = totals[token]
        p = phases[token]['rhs']
        expected = dict(mul=t['mul'], fma=t['fma'], beats=t['beats'],
                        loaded_words=t['load_words'], za_reads=t['read_rows'],
                        za_writes=t['load_rows'] + t['beats'])
        if any(p[k] != value for k, value in expected.items()):
            raise ValueError('packed warm RHS replay disagrees with physical ZA accounting')
        if token in schedules and (t['points'] != cells or t['completed'] != cells):
            raise ValueError('packed warm RHS did not retire every point')
        t['peak_groups'] = peaks[token]
    return {token: dict(totals[token]) for token in sorted(warm, key=int)}
