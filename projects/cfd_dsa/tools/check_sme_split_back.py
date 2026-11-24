"""Replay two-point partial q feedback through timed SPM and ZA resources."""

from collections import Counter, defaultdict

from trace_text import fields


GROUPS = ((0, 1, 2), (3, 4))
DEPENDENCIES = {5: (0, 1), 6: (2, 3), 7: (4,), 8: (5, 6), 9: (7, 8)}
READ_ROWS = {5: (0, 1), 6: (2, 3), 7: (5, 4), 8: (0, 2), 9: (5, 0)}


def check_sme_split_back(text, actual, arrays, schedules, aborts, phases):
    events = fields(text, 'CFD_SME_SPLIT_BACK')
    if not actual.get('coeff_sme_warm_back_split', False):
        if events:
            raise ValueError('unconfigured split backward scan executed')
        return {}

    cells = actual['lusgs_cells']
    tile_cells = actual['coeff_line_tile_cells']
    warm_contexts = (2 * actual['coeff_sme_contexts']
                     if actual.get('coeff_sme_warm_single_tile', False)
                     else actual['coeff_sme_contexts'])
    use_bypass = bool(actual.get('coeff_sme_warm_bypass', False)) and warm_contexts >= 6
    warm = {t for t, row in schedules.items() if int(row['cache_hit'])}
    warm |= {r['token'] for r in fields(text, 'CFD_SME_ABORT') if int(r['cache_hit'])}
    config = {r['token']: {k: int(v) for k, v in r.items() if k != 'token'} for r in arrays}
    early_backward = bool(actual.get('coeff_line_early_backward', True))
    feedback_reg = int(bool(actual.get('coeff_sme_feedback_reg', False)))
    seeds = {r['token']: r for r in fields(text, 'CFD_EARLY_BACKWARD') if r['token'] in warm}
    if not early_backward:
        for r in fields(text, 'CFD_BACKWARD_SEED'):
            if r['token'] in warm:
                seeds[r['token']] = r
    operands = {(r['token'], int(r['cell']), int(r['column'])): r['job']
                for r in fields(text, 'CFD_BACK_OPERAND')
                if r['token'] in warm and r['event'] == 'read'}
    jobs = set(operands.values()) | {r['tail_job'] for r in seeds.values()}
    starts = {r['id']: r for r in fields(text, 'CFD_ONCHIP_START') if r['id'] in jobs}
    returns = {r['id']: int(r['cycle']) for r in fields(text, 'CFD_ONCHIP_COMPLETE') if r['id'] in jobs}
    full_feedback = {(r['token'], int(r['cell'])): int(r['finish'])
                     for r in fields(text, 'CFD_LINE_FEEDBACK') if r['token'] in warm}

    def operand_ready(token, job, cycle):
        start = starts[job]
        if ((start['owner'], start['op'], start['words'], start['active']) != (token, 'R', '5', '1') or
                not int(start['cycle']) < returns[job] <= cycle):
            raise ValueError('split backward used an unpaid SPM operand')

    live = {}
    released = defaultdict(dict)
    feedback = {}
    heads = dict.fromkeys(warm, cells - 2)
    read_ports = defaultdict(set)
    ingress_ports = Counter()
    counts = defaultdict(Counter)
    peaks = Counter()
    last = -1
    for event in events:
        token, kind = event['token'], event['event']
        cycle, cell, part, op = (int(event[k]) for k in ('cycle','cell','part','op'))
        a = config[token]
        stop = int(aborts[token]['cycle']) if token in aborts else a['done']
        if (token not in warm or not a['start'] <= cycle <= a['done'] or cycle < last or
                (cycle > stop and kind != 'drop') or not 0 <= part < len(GROUPS) or
                not 0 <= cell < cells - 1):
            raise ValueError('split backward work escaped its owner, stage or cancellation fence')
        last = cycle
        key = token, cell
        width = len(GROUPS[part])
        total = counts[token]
        if kind == 'bind':
            if key in live or key in released[token] or cell not in (heads[token], heads[token]-1):
                raise ValueError('invalid split backward context admission')
            live[key] = [{}, {}]
            peaks[token] = max(peaks[token], len(live))
            continue
        parts = live[key]
        ready = parts[part]
        tile = 2 * (cell % 2) + part
        if kind == 'base':
            if 'base' in ready:
                raise ValueError('split backward loaded base twice')
            operand_ready(token, operands[token, cell, -1], cycle)
            ready['base'] = cycle + 1
            total['load_rows'] += 1
            total['load_words'] += width
        elif kind == 'issue':
            if not 0 <= op <= 9 or op in ready:
                raise ValueError('duplicate or invalid split backward operation')
            if op < 5:
                operand_ready(token, operands[token, cell, op], cycle)
                if cell == cells - 2:
                    seed = seeds[token]
                    tail = (cells - 1) // tile_cells * tile_cells
                    if int(seed['tail']) != tail or int(seed['cycle']) + feedback_reg > cycle:
                        raise ValueError('split backward missed the timed tail seed')
                    operand_ready(token, seed['tail_job'], cycle)
                else:
                    prior = 0 if op < 3 else 1
                    if feedback[token, cell+1, prior] + feedback_reg > cycle:
                        raise ValueError('split backward read an unreturned feedback component')
                    if (token, cell+1, 1-prior) not in feedback:
                        total['partial_feedback_uses'] += 1
                ingress_ports[token, cycle] += 1
                total['mul'] += width
                ready[op] = cycle + a['mul_latency']
            else:
                deps = DEPENDENCIES[op] + (('base',) if op == 7 else ())
                if any(ready[dep] > cycle for dep in deps):
                    raise ValueError('split backward reduction read a pending ZA row')
                rows = READ_ROWS[op]
                if use_bypass:
                    rows = tuple(row for row in rows if row not in (0, 2, 5))
                read_ports[token, cycle].update((tile, row) for row in rows)
                total['add'] += width
                ready[op] = cycle + a['add_latency']
            total['beats'] += 1
            if not use_bypass or op in (1, 3, 4):
                total['za_beats'] += 1
        elif kind == 'feedback':
            returned = token, cell, part
            if returned in feedback or op != 9 or ready[9] > cycle:
                raise ValueError('split backward published a pending or duplicate row group')
            feedback[returned] = cycle
            if not use_bypass:
                read_ports[token, cycle].add((tile, 5))
        elif kind in ('commit', 'drop'):
            if any(due > cycle for part_ready in parts for due in part_ready.values()):
                raise ValueError('split backward recycled a live ZA return')
            if kind == 'commit':
                if (cell != heads[token] or full_feedback[key] != cycle or
                        any((token,cell,p) not in feedback for p in range(len(GROUPS)))):
                    raise ValueError('split backward committed incomplete or out-of-order feedback')
                heads[token] -= 1
                total['committed'] += 1
                released[token][key] = cycle
            elif token not in aborts or cycle < stop:
                raise ValueError('split backward discarded a point without cancellation')
            else:
                total['dropped'] += 1
            del live[key]
        else:
            raise ValueError('unknown split backward event')
    if live:
        raise ValueError('split backward leaked point contexts')
    for (token, cycle), read_rows in read_ports.items():
        if len(read_rows) > config[token]['reads']:
            raise ValueError('split backward exceeded ZA reads')
        counts[token]['za_reads'] += len(read_rows)
    for (token, cycle), used in ingress_ports.items():
        if used > config[token]['ingress']:
            raise ValueError('split backward exceeded direct operand ingress')
    for token in warm:
        t = counts[token]
        p = phases[token]['backward']
        expected = dict(mul=t['mul'], add=t['add'], beats=t['beats'],
                        loaded_words=t['load_words'], ingress_words=t['mul'],
                        za_writes=(0 if use_bypass else t['load_rows']) + t['za_beats'],
                        za_reads=t['za_reads'])
        if any(p[k] != v for k, v in expected.items()) or peaks[token] > 2:
            raise ValueError('split backward replay disagrees with ZA accounting')
        if token in schedules and t['committed'] != cells-1:
            raise ValueError('split backward scan did not publish every q point')
        t['peak_points'] = peaks[token]
    return {token: dict(counts[token]) for token in sorted(warm, key=int)}
