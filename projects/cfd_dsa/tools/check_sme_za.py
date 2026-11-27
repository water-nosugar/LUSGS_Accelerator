"""Validate work, capacity, numerical output and retirement of the SME array."""

import csv
import json
from pathlib import Path
import sys

from check_single_line_c import check_numeric, fields
from check_single_line_output_dma import output_depth, resolved_options, validate_row

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'configs'))
from single_line_contract import (
    sme_deferred_panel_work, sme_upper_resident_bytes, sme_za_reserved_bytes,
    sme_za_return_slot_bytes,
)

PHASES = ('lu', 'rhs', 'forward', 'backward')
ISSUE_STATES = ('full', 'partial', 'ready_blocked', 'dependency_only', 'no_attempt')


def check_windows(rows, array, phases, cells):
    names = {'sweep', 'prep_steady', 'back_steady'}
    if len(rows) != len(names) or {r['window'] for r in rows} != names:
        raise ValueError('missing or duplicated SME issue window')
    lanes, depth = array['lanes'], array['depth']
    windows = {}
    for row in rows:
        name = row['window']
        w = {k: int(v) for k, v in row.items() if k not in ('token', 'window')}
        cycles = w['cycles']
        work = sum(w[k] for k in phases)
        issued_cycles = w['full'] + w['partial']
        if (min(w.values()) < 0 or sum(w[k] for k in ISSUE_STATES) != cycles or
                not w['full'] * lanes <= work <= issued_cycles * lanes or
                not work <= w['capped_offer'] <= min(w['offered'], cycles * lanes) or
                w['pending_sum'] > cycles * depth or
                max(w['queue_full'], w['return_blocked']) > cycles):
            raise ValueError(f'invalid SME issue accounting in {name}')
        capacity = cycles * lanes
        w['effective_utilization'] = work / capacity if capacity else 0.0
        w['ready_dispatch'] = work / w['capped_offer'] if w['capped_offer'] else 0.0
        w['phase_utilization'] = {k: w[k] / capacity if capacity else 0.0
                                  for k in phases}
        windows[name] = w
    sweep = windows['sweep']
    if (sweep['cycles'] != array['done'] - array['start'] + 1 or
            any(sweep[k] != sum(phases[k][op] for op in ('fma', 'mul', 'add'))
                for k in phases)):
        raise ValueError('SME issue windows and sweep arithmetic totals differ')
    for k in ('cycles', *ISSUE_STATES, *phases, 'capped_offer'):
        if windows['prep_steady'][k] + windows['back_steady'][k] > sweep[k]:
            raise ValueError('overlapping or out-of-sweep SME steady windows')
    if cells >= 17 and not array['aborted']:
        if not windows['prep_steady']['cycles'] or not windows['back_steady']['cycles']:
            raise ValueError('SME sweep did not observe steady activity in both directions')
    return windows


def check_panel_ingress(directory, overlap, panel_pack=None):
    readiness = {}
    early = [0, 0]
    packed = [0, 0]
    packed_b_before_lu = 0
    events = 0
    with (directory / 'coeff_preprocess_trace.csv').open(newline='') as src:
        for row in csv.DictReader(src):
            stage = row['stage']
            if stage in ('INPUT_TOKEN', 'INPUT_L_READY', 'INPUT_U_READY',
                         'INPUT_R_READY', 'LU_COMPLETE', 'REQUEST_AUTO_RETIRE',
                         'SME_INV_GUARD_FALLBACK', 'SME_INV_COMPLETE',
                         'SME_RHS_PANEL_LOAD'):
                request = row['requestId']
                if stage == 'REQUEST_AUTO_RETIRE':
                    readiness.pop(request, None)
                    continue
                inputs = readiness.setdefault(request, set())
                if stage == 'SME_INV_COMPLETE':
                    if 'LU_COMPLETE' not in inputs or 'SME_INV_GUARD_FALLBACK' in inputs:
                        raise ValueError('SME inverse completed before a successful LU guard')
                    continue
                if stage != 'SME_RHS_PANEL_LOAD':
                    inputs.add(stage)
                    continue
                panel = int(row['rhsBatch'])
                if panel not in (0, 1):
                    raise ValueError('unknown SME RHS panel')
                mask = int(row['rhsColumn'])
                if mask < 0:
                    mask = 1 if int(row['cell']) == 0 else 63
                note = row.get('note')
                packed_a = note == 'packed-a' or (panel == 0 and mask == 0xff)
                packed_b_identity = note == 'packed-b-identity'
                packed_b = note == 'packed-b' or packed_b_identity
                if packed_a or packed_b:
                    if ((panel_pack is not None and not panel_pack) or
                            (packed_a and (panel != 0 or mask != 0xff)) or
                            (packed_b and (panel != 1 or mask not in (0x07, 0xff))) or
                            (packed_b_identity and mask != 0xff)):
                        raise ValueError('invalid fused SME RHS panel')
                    if packed_b and 'LU_COMPLETE' not in inputs:
                        if not packed_b_identity:
                            raise ValueError('fused SME Panel B loaded before LU completion')
                        packed_b_before_lu += 1
                    packed[panel] += 1
                events += 1
                if 'INPUT_TOKEN' in inputs:
                    continue
                required = {'INPUT_U_READY'} if panel or packed_a else set()
                if panel == 0:
                    if mask & 1:
                        required.add('INPUT_R_READY')
                    if mask & 0x3e:
                        required.add('INPUT_L_READY')
                if not overlap or not required <= inputs:
                    raise ValueError('SME panel loaded before its timed input')
                early[panel] += 1
    if not events:
        raise ValueError('no SME panel ingress was traced')
    return dict(traced_panels=events, early_rl=early[0], early_u=early[1],
                packed_a=packed[0], packed_b=packed[1],
                packed_b_before_lu=packed_b_before_lu)


def check_sme_za(directory):
    directory = Path(directory)
    resolved = json.loads((directory / 'resolved_experiment.json').read_text())
    actual = resolved['options']
    if actual['coeff_compute_backend'] != 'sme-za':
        raise ValueError('expected an active SME ZA backend')
    config = resolved_options(directory)
    n = int(actual['lusgs_cells'])
    text = (directory / 'simout.txt').read_text()
    array = fields(text, 'CFD_SME_CONFIG')
    phases = fields(text, 'CFD_SME_PHASE')
    stats2d_rows = {}
    for row in fields(text, 'CFD_SME_2D_STATS'):
        if row['token'] in stats2d_rows:
            raise ValueError('duplicate CFD_SME_2D_STATS token')
        stats2d_rows[row['token']] = row
    instrumented = 'coeff_sme_early_panels' in actual
    window_rows = fields(text, 'CFD_SME_WINDOW') if instrumented else []
    schedules = {r['token']: r for r in fields(text, 'CFD_LINE_SCHEDULE')}
    storage = {r['token']: r for r in fields(text, 'CFD_LINE_STORAGE')}
    aborts = {r['token']: r for r in fields(text, 'CFD_LINE_ABORT')
              if r['event'] == 'visible'}
    abort_start = {r['token']: r for r in fields(text, 'CFD_LINE_ABORT')
                   if r['event'] == 'begin'}
    extended = bool(array and 'reverse_contexts' in array[0])
    phase_names = (*PHASES, 'reverse_rhs') if extended else PHASES
    sme_aborts = fields(text, 'CFD_SME_ABORT') if aborts and instrumented else []
    if instrumented and (len(sme_aborts) != len(aborts) or
                         {r['token'] for r in sme_aborts} != aborts.keys()):
        raise ValueError('missing SME abort snapshots')
    starts = {}
    for r in fields(text, 'CFD_INTERFACE_DMA'):
        if r['kind'] == 'descriptor' and r['event'] == 'issue':
            starts.setdefault(r['token'], int(r['cycle']))
    if not schedules or schedules.keys() != storage.keys():
        raise ValueError('missing finite storage or completion report')
    if (len(array) != len(schedules) + len(aborts) or
            len(phases) != len(phase_names) * len(array)):
        raise ValueError('missing SME statistics')
    if {r['token'] for r in array} != schedules.keys() | aborts.keys():
        raise ValueError('SME sweep identities differ from controller reports')
    max_error = check_numeric(text, [n] * len(schedules))
    numerics = fields(text, 'CFD_SINGLE_LINE_NUMERIC')
    if not actual.get('coeff_recovery_test') and not actual.get('coeff_lu_output_transition_test'):
        expected_sweeps = int(resolved['process_cmd'][1])
        if len(schedules) != expected_sweeps:
            raise ValueError('incomplete sweep count')

    by_token = {}
    for r in phases:
        name = r['phase']
        rows = by_token.setdefault(r['token'], {})
        if name in rows:
            raise ValueError('duplicate SME phase')
        rows[name] = {k: int(v) for k, v in r.items() if k not in ('phase', 'token')}
    contexts = actual['coeff_sme_contexts']
    depth = actual['coeff_sme_depth']
    lanes = actual['coeff_sme_lanes']
    ingress = actual.get('coeff_sme_ingress_ports', 0)
    reverse_contexts = actual.get('coeff_sme_reverse_contexts', 0)
    warm_rhs_width = actual.get('coeff_sme_warm_rhs_width', 0)
    vertical_rhs = int(actual.get('coeff_sme_warm_vertical_rhs', False))
    warm_single_tile = int(actual.get('coeff_sme_warm_single_tile', False))
    warm_bypass = int(actual.get('coeff_sme_warm_bypass', False))
    critical_arb = int(actual.get('coeff_sme_critical_arb', False))
    warm_back_split = int(actual.get('coeff_sme_warm_back_split', False))
    warm_base_mode_name = actual.get('coeff_sme_warm_base_mode', 'direct')
    warm_base_mode = {'direct': 0, 'inv-fma1': 1, 'inv-fma2': 2, 'adaptive': 3}[warm_base_mode_name]
    array_mode_name = actual.get('coeff_sme_array_mode', '1d')
    array_mode = {'1d': 0, '2d-base': 1, '2d-bd': 2, '2d-rs': 3}[array_mode_name]
    segments_max = int(actual.get('coeff_sme_2d_segments', 1))
    panel_pack = int(actual.get('coeff_sme_2d_panel_pack', False))
    mixed_kind = int(actual.get('coeff_sme_2d_mixed_kind', False))
    panel_b_overlap = int(actual.get('coeff_sme_2d_panel_b_overlap', False))
    forward_tile_rotate = int(actual.get('coeff_sme_forward_tile_rotate', False))
    candidate_window = int(actual.get('coeff_sme_2d_candidate_window', 0))
    feedback_reg = int(actual.get('coeff_sme_feedback_reg', False))
    early_backward = bool(actual.get('coeff_line_early_backward', True))
    inv_sparse_i5 = int(actual.get('coeff_sme_inv_sparse_i5', True))
    inv_exp_thresh = int(actual.get('coeff_sme_inv_exp_thresh', 20))
    inv_reuse_hint = int(actual.get('coeff_sme_reuse_hint', 0))
    inv_adaptive_k = int(actual.get('coeff_sme_adaptive_k', 0))
    tagged_cache = 'coeff_sme_cache_tag_bytes' in actual
    tag_bytes = int(actual.get('coeff_sme_cache_tag_bytes', 0))
    fma_latency = int(actual.get('coeff_sme_fma_latency', 5))
    mode = actual.get('coeff_sme_ubar_mode', 'external')
    split = actual.get('coeff_sme_upper_split', 3)
    wavefront = int(actual.get('coeff_sme_reverse_wavefront', False))
    upper_bytes = sme_upper_resident_bytes(n, mode, split)
    _, shifted_fma, shifted_mul = sme_deferred_panel_work(split)
    slot_bytes = sme_za_return_slot_bytes(array_mode)
    if actual.get('coeff_sme_return_slot_bytes', slot_bytes) != slot_bytes:
        raise ValueError('invalid resolved SME return slot capacity')
    if mode != 'external' and not extended:
        raise ValueError('private upper mode requires the extended SME model')
    if array_mode == 0:
        if stats2d_rows:
            raise ValueError('unexpected CFD_SME_2D_STATS in 1d mode')
    elif stats2d_rows.keys() != {r['token'] for r in array}:
        raise ValueError('missing or extra CFD_SME_2D_STATS rows for 2d mode')
    results = []
    abort_evidence = []
    for r in array:
        token = r['token']
        a = {k: int(v) for k, v in r.items() if k != 'token'}
        a.setdefault('ingress', 0)
        a.setdefault('warm_rhs_width', 0)
        a.setdefault('warm_rhs_groups', 0)
        a.setdefault('warm_vertical_rhs', 0)
        a.setdefault('warm_single_tile', 0)
        a.setdefault('warm_bypass', 0)
        a.setdefault('warm_critical_arb', 0)
        a.setdefault('warm_back_split', 0)
        a.setdefault('warm_base_mode', 0)
        a.setdefault('inv_sparse_i5', 1)
        a.setdefault('inv_exp_thresh', 20)
        a.setdefault('inv_reuse_hint', 0)
        a.setdefault('inv_cells', 0)
        a.setdefault('inv_fallback_cells', 0)
        sized_returns = ('return_slot_bytes' in a or
                         'coeff_sme_return_slot_bytes' in actual)
        reserved = sme_za_reserved_bytes(
            contexts, depth, ingress, reverse_contexts, bool(warm_rhs_width),
            warm_back_split, bool(warm_single_tile), bool(warm_bypass),
            bool(critical_arb), warm_base_mode, array_mode, segments_max,
            bool(mixed_kind), bool(panel_pack),
            return_slot_bytes=slot_bytes if sized_returns else 128,
        )
        required = dict(contexts=contexts, depth=depth, lanes=lanes,
                        reads=actual['coeff_sme_read_ports'],
                        writes=actual['coeff_sme_write_ports'], ingress=ingress,
                        za_bytes=(3 * contexts + 2 + reverse_contexts) * 512,
                        reserved=reserved,
                        warm_rhs_width=warm_rhs_width,
                        warm_vertical_rhs=vertical_rhs,
                        warm_single_tile=warm_single_tile,
                        warm_bypass=warm_bypass,
                        warm_critical_arb=critical_arb,
                        warm_rhs_groups=3 * contexts // 4 if warm_rhs_width else 0,
                        warm_back_split=warm_back_split,
                        warm_base_mode=warm_base_mode,
                        fma_latency=fma_latency, mul_latency=actual['coeff_cluster_mul_lat'],
                        add_latency=actual['coeff_cluster_add_lat'],
                        div_latency=actual['coeff_cluster_div_lat'],
                        div_ii=actual['coeff_cluster_div_ii'])
        if sized_returns:
            required['return_slot_bytes'] = slot_bytes
        if warm_base_mode:
            required.update(inv_sparse_i5=inv_sparse_i5,
                            inv_exp_thresh=inv_exp_thresh,
                            inv_reuse_hint=inv_reuse_hint)
        if tagged_cache:
            required['inv_adaptive_k'] = inv_adaptive_k
        if instrumented:
            required['early_panels'] = int(actual['coeff_sme_early_panels'])
        if 'coeff_sme_chain_steps' in actual:
            if array_mode != 0 and actual['coeff_sme_chain_steps']:
                raise ValueError('coeff_sme_chain_steps is only valid in 1d SME array mode')
            required['chain_steps'] = int(actual['coeff_sme_chain_steps'])
        if extended:
            required.update(reverse_contexts=reverse_contexts,
                            local_ubar=int(mode != 'external'), upper_bytes=upper_bytes)
        if 'upper_split' in a or split != 3 or wavefront:
            required.update(upper_split=split, reverse_wavefront=wavefront)
        if 'split_rhs' in a or actual.get('coeff_sme_split_rhs'):
            required['split_rhs'] = int(actual.get('coeff_sme_split_rhs', False))
        if 'array_mode' in a or array_mode:
            required.update(array_mode=array_mode, segments_max=segments_max,
                            panel_pack=panel_pack)
            if 'mixed_kind' in a or mixed_kind:
                required['mixed_kind'] = mixed_kind
        if 'panel_b_overlap' in a or panel_b_overlap:
            required['panel_b_overlap'] = panel_b_overlap
        if 'forward_tile_rotate' in a or forward_tile_rotate:
            required['forward_tile_rotate'] = forward_tile_rotate
        if 'candidate_window' in a or candidate_window:
            required['candidate_window'] = candidate_window
        if 'feedback_reg' in a or feedback_reg:
            required['feedback_reg'] = feedback_reg
        if any(a.get(k) != v for k, v in required.items()) or not 0 <= a['peak'] <= depth:
            raise ValueError('SME configuration or return capacity mismatch')
        p = by_token[token]
        if set(p) != set(phase_names):
            raise ValueError('invalid SME phase set')
        for stats in p.values():
            if any(stats[k] != stats[k + '_returned'] for k in ('mul', 'add', 'fma', 'div')):
                raise ValueError('SME retirement left in-flight arithmetic')
            useful = sum(stats[k] for k in ('mul', 'add', 'fma'))
            span = stats['last'] - stats['first'] + 1 if useful else 0
            if useful > lanes * stats['issue_cycles'] or stats['issue_cycles'] > span:
                raise ValueError('SME issue exceeded physical capacity')
            if useful and not a['start'] <= stats['first'] <= stats['last'] <= a['done']:
                raise ValueError('SME arithmetic outside the sweep')
            stats['issue_span_utilization'] = useful / (lanes * span) if span else 0.0
            beat_denom = 64 if array_mode else 8
            stats['mask_utilization'] = useful / (beat_denom * stats['beats']) if stats['beats'] else 0.0
        stats_2d = None
        if array_mode:
            raw2d = stats2d_rows[token]
            hist = [int(x) for x in raw2d['row_fill_hist'].split(',')]
            if len(hist) != 8 or min(hist) < 0:
                raise ValueError('invalid SME 2D row_fill_hist')
            s2d = {k: int(v) for k, v in raw2d.items()
                   if k not in ('token', 'row_fill_hist')}
            s2d['row_fill_hist'] = hist
            seg_counts = [s2d[f'bundles_seg{k}'] for k in range(1, 5)]
            total_arith = sum(s[op] for s in p.values() for op in ('fma', 'mul', 'add'))
            expected_cap = (a['done'] - a['start'] + 1) * 64
            expected_issue_cap = s2d['bundles_total'] * 64
            if (s2d['array_mode'] != array_mode or s2d['segments_max'] != segments_max or
                    min(seg_counts) < 0 or sum(seg_counts) != s2d['bundles_total'] or
                    sum(hist) != s2d['bundles_total'] or
                    any(seg_counts[k - 1] != 0 for k in range(segments_max + 1, 5)) or
                    (array_mode == 1 and any(seg_counts[1:])) or
                    s2d['pe_active_sum'] != total_arith or
                    s2d['pe_capacity'] != expected_cap or
                    s2d['pe_issue_capacity'] != expected_issue_cap or
                    not 0 <= s2d['pe_active_sum'] <= s2d['pe_issue_capacity'] <= s2d['pe_capacity'] or
                    min(s2d['pack_stall_row'], s2d['pack_stall_col'], s2d['pack_stall_readport'],
                        s2d.get('pack_stall_ingress', 0)) < 0):
                raise ValueError('SME 2D bundle accounting or invariant violation')
            s2d['pe_sweep_utilization'] = (s2d['pe_active_sum'] / s2d['pe_capacity']
                                           if s2d['pe_capacity'] else 0.0)
            s2d['pe_issue_fill_rate'] = (s2d['pe_active_sum'] / s2d['pe_issue_capacity']
                                         if s2d['pe_issue_capacity'] else 0.0)
            s2d['avg_segments_per_bundle'] = (
                sum((idx + 1) * c for idx, c in enumerate(seg_counts)) / s2d['bundles_total']
                if s2d['bundles_total'] else 0.0)
            if 'fmopa_bundles' in s2d:
                fb = s2d['fmopa_bundles']
                fp = s2d['fmopa_pe_active']
                if not (0 <= fb <= s2d['bundles_total'] and 0 <= fp <= min(fb * 64, s2d['pe_active_sum'])):
                    raise ValueError('SME 2D fmopa accounting violation')
                s2d['fmopa_fill_rate'] = fp / (fb * 64) if fb else 0.0
            stats_2d = s2d
        windows = check_windows([w for w in window_rows if w['token'] == token],
                                a, p, n) if instrumented else {}
        if sized_returns:
            sweep_cycles = a['done'] - a['start'] + 1
            blocked = [s.get('return_blocked_cycles', -1) for s in p.values()]
            if (not 0 <= min(blocked) <= max(blocked) <= sweep_cycles or
                    (windows and not max(blocked) <= windows['sweep']['return_blocked']
                     <= sum(blocked))):
                raise ValueError('invalid SME return blocking cycle counts')

        if a['aborted']:
            if token not in aborts or a['done'] != int(aborts[token]['cycle']):
                raise ValueError('SME abort published before retirement')
            if instrumented:
                event = next(r for r in sme_aborts if r['token'] == token)
                snap = {k: int(v) for k, v in event.items()}
                begin = abort_start[token]
                if (snap['cycle'] != int(begin['cycle']) or snap['status'] != int(begin['status']) or
                        not 0 <= snap['live'] <= a['peak'] or
                        any(s['last'] > snap['cycle'] for s in p.values())):
                    raise ValueError('SME abort did not stop new arithmetic or drain the owner')
                if 'live_rhs' in snap:
                    per_phase_live = sum(snap[f'live_{ph}'] for ph in
                                         ('lu', 'rhs', 'forward', 'backward', 'reverse_rhs'))
                    if not snap['live'] <= per_phase_live <= snap['live'] * 4:
                        raise ValueError('SME abort per-phase live segment count inconsistent with return slots')
                abort_evidence.append(dict(snap, visible=a['done'],
                                           rhs_lanes=p['rhs']['fma'] + p['rhs']['mul'],
                                           reverse_rhs_lanes=sum(p.get('reverse_rhs', {}).get(k, 0)
                                                                 for k in ('fma', 'mul')),
                                           forward_lanes=p['forward']['add'] + p['forward']['mul']))
            continue
        line = schedules[token]
        tile = {k: int(v) for k, v in storage[token].items()}
        if (tile['host_line_arrays'] or tile['reserved'] < reserved or
                tile['reserved'] + tile['resident'] + tile['cache_bytes'] > tile['capacity'] or
                tile['capacity'] != actual['coeff_line_spm_bytes'] or
                tile['read_issued'] != tile['read_completed'] or
                tile['write_issued'] != tile['write_completed']):
            raise ValueError('SME finite storage or DMA lifetime mismatch')
        validate_row(line, n, output_depth(config), True, actual['coeff_output_coalesce'])
        if a['done'] != int(line['visible_done']):
            raise ValueError('SME results and controller completion differ')
        warm = int(line['cache_hit'])
        inv_cells = a['inv_cells']
        built_cells = a['inv_built_cells'] if tagged_cache else inv_cells
        if tagged_cache:
            cache_bytes = tile['cache_bytes']
            policy = bool(cache_bytes and (
                warm_base_mode in (1, 2) or
                (warm_base_mode == 3 and 0 < inv_adaptive_k <= inv_reuse_hint)))
            fallbacks = a['inv_fallback_cells']
            expected_entries = n if policy else 0
            if (a['cache_tag_bytes'] != (tag_bytes if cache_bytes else 0) or
                    cache_bytes not in (0, n * (400 + tag_bytes), n * (600 + tag_bytes)) or
                    a['inv_policy_enabled'] != policy or
                    not 0 <= inv_cells <= expected_entries or
                    inv_cells + fallbacks != expected_entries or
                    (built_cells != 0 if warm else
                     not inv_cells <= built_cells <= expected_entries)):
                raise ValueError('invalid SME coefficient cache or inverse selection')
        deferred = n - 1 if mode == 'deferred' and not warm else 0
        if extended:
            stored = n - 1 if mode != 'external' and not warm else 0
            if (a['defer_upper'], a['upper_stored'], a['reverse_done']) != (
                    bool(deferred), stored, deferred):
                raise ValueError('SME upper panels were not fully stored and consumed')
            if mode != 'external' and (not tile['tile_y_only'] or
                    tile['reserved'] != a['upper_base'] + upper_bytes or
                    tile['write_completed'] != n * 40 or
                    tile['cache_bytes'] not in (0, n * (400 + tag_bytes))):
                raise ValueError('private upper bank overlaps storage or has a matrix spill')
        rhs = n if warm else 11 * n - 10
        scans = 25 * (n - 1)
        lu_cells = n - inv_cells
        if not warm:
            packed_inverses = a.get('inv_packed_cells', 0)
            discarded_mul = a.get('inv_discarded_mul', 0)
            discarded_fma = a.get('inv_discarded_fma', 0)
            guard_rejected = n - built_cells if a.get('inv_policy_enabled') else 0
            if (not 0 <= packed_inverses <= min(built_cells, max(0, n - 2)) or
                    not 0 <= discarded_mul <= (15 if inv_sparse_i5 else 25) * guard_rejected or
                    not 0 <= discarded_fma <= (20 if inv_sparse_i5 else 50) * guard_rejected or
                    (not panel_pack and any((packed_inverses, discarded_mul, discarded_fma)))):
                raise ValueError('invalid SME packed inverse accounting')
            inv_fma = (70 if inv_sparse_i5 else 100) * built_cells + discarded_fma
            inv_mul = (10 if inv_sparse_i5 else 25) * built_cells + discarded_mul
            if inv_sparse_i5:
                inv_mul += 5 * packed_inverses
            rhs_expected = (20 * rhs - shifted_fma * deferred + inv_fma,
                            5 * rhs - shifted_mul * deferred + inv_mul, 0, 0)
        elif warm_base_mode == 2:
            rhs_expected = (20 * lu_cells + 15 * inv_cells,
                            5 * lu_cells + 10 * inv_cells,
                            5 * inv_cells, 0)
        else:
            rhs_expected = (20 * rhs, 5 * rhs, 0, 0)
        expected = {
            'lu': (30 * n * (1 - warm), 10 * n * (1 - warm), 0, 5 * n * (1 - warm)),
            'rhs': rhs_expected,
            'forward': (0, scans, scans, 0),
            'backward': (0, scans, scans, 0),
        }
        if extended:
            expected['reverse_rhs'] = (shifted_fma * deferred, shifted_mul * deferred, 0, 0)
        for name, work in expected.items():
            if tuple(p[name][k] for k in ('fma', 'mul', 'add', 'div')) != work:
                raise ValueError(f'incomplete SME {name} operation coverage')
            ingress_words = scans if ingress and name in ('forward', 'backward') else 0
            if p[name].get('ingress_words', 0) != ingress_words:
                raise ValueError(f'incomplete SME {name} ingress accounting')
            vertical = 5 * lu_cells if vertical_rhs and warm and name == 'lu' else 0
            if p[name].get('vertical_loads', 0) != vertical:
                raise ValueError(f'incorrect SME {name} vertical slice count')
        if vertical_rhs and warm:
            inv_beats = 6 if warm_base_mode == 2 else 5
            expected_beats = 13 * lu_cells + inv_beats * inv_cells
            if p['rhs']['beats'] != expected_beats:
                raise ValueError('unexpected SME warm RHS beat count')
        if warm_bypass and warm:
            rhs_p = p['rhs']
            inv_bp = 6 if warm_base_mode == 2 else 5
            if (rhs_p['za_writes'] != 0 or
                    rhs_p['za_reads'] != 13 * lu_cells + 5 * inv_cells or
                    rhs_p.get('bypass_reads', 0) != 14 * lu_cells + inv_bp * inv_cells or
                    rhs_p.get('bypass_writes', 0) != 14 * lu_cells + inv_bp * inv_cells or
                    rhs_p['return_wait'] != 0):
                raise ValueError('incorrect SME post-rounding bypass accounting')
        cycles = a['done'] - starts[token] + 1
        back, back_done = int(line['backward_start']), int(line['backward_done'])
        if deferred and not back <= p['reverse_rhs']['first'] <= p['reverse_rhs']['last'] <= back_done:
            raise ValueError('deferred upper arithmetic outside the backward phase')
        phase_cycles = dict(descriptor=a['start'] - starts[token],
                            preprocess_and_forward=back - a['start'],
                            backward=back_done - back,
                            output_retirement=a['done'] - back_done + 1)
        if min(phase_cycles.values()) < 0:
            raise ValueError('invalid exclusive SME stage boundaries')
        useful = sum(s[k] for s in p.values() for k in ('mul', 'add', 'fma'))
        sweep_entry = dict(token=int(token), cycles=cycles, cache_hit=warm, phases=p,
                           phase_cycles=phase_cycles, issue_windows=windows,
                           full_sweep_utilization=useful / (cycles * lanes),
                           resources=a, storage=tile)
        if stats_2d is not None:
            sweep_entry['stats_2d'] = stats_2d
        results.append(sweep_entry)
    if actual.get('coeff_recovery_test') and len(aborts) != (3 if n >= 17 else 2):
        raise ValueError('SME recovery fixture did not exercise all aborts')
    frontier = {}
    if len(schedules) == 1 and not aborts:
        from check_single_line_recurrence import check_wait_accounting
        frontier = check_wait_accounting(text).get('frontier_states', {})
    upper = {}
    if mode != 'external':
        from check_sme_upper import check_sme_upper
        upper = check_sme_upper(directory, text, n, array, results, abort_start,
                                actual.get('coeff_input_file'))
    packed = {}
    if actual.get('coeff_sme_packed_input'):
        from check_sme_packed_input import check_sme_packed_input
        packed = check_sme_packed_input(directory, text, config, n, schedules, aborts)
    elif 'CFD_SME_PACKED_INPUT ' in text:
        raise ValueError('packed SME input was active in an unpacked recipe')
    from check_sme_warm_rhs import check_sme_warm_rhs
    warm_rhs = check_sme_warm_rhs(directory, text, actual, array, schedules,
                                  abort_start, by_token)
    from check_sme_split_back import check_sme_split_back
    split_back = check_sme_split_back(text, actual, array, schedules, abort_start, by_token)
    if not early_backward:
        if 'CFD_EARLY_BACKWARD ' in text:
            raise ValueError('early backward triggered when --coeff-line-early-backward=0')
        commit_cycles = {}
        for ev in fields(text, 'CFD_TILE_EVENT'):
            if ev.get('event') == 'forward_commit':
                tok = ev['token']
                commit_cycles[tok] = max(commit_cycles.get(tok, -1), int(ev['cycle']))
        for tok, line in schedules.items():
            if tok in commit_cycles and int(line['backward_start']) < commit_cycles[tok]:
                raise ValueError('non-early backward started before last forward tile commit')
    if actual.get('coeff_tile_prefetch_buffers', 0) > 0 and actual.get('coeff_line_storage') == 'tiled':
        from check_single_line_storage import check_prefetch_lifetimes
        has_rot_field = 'coeff_sme_forward_tile_rotate' in actual
        rotate_expected = bool(forward_tile_rotate) if has_rot_field else ('event=forward_allocate ' in text)
        check_prefetch_lifetimes(
            text, config, aborted=aborts, abort_start=abort_start,
            schedules=schedules, forward_rotate=rotate_expected, directory=directory,
        )
    del text
    result = dict(cells=n, max_abs=max_error, sweeps=results, aborted=len(aborts),
                  result_hashes=[r['result_hash'] for r in numerics], frontier=frontier)
    if upper:
        result['private_upper'] = upper
    if packed:
        result['packed_input'] = packed
    if warm_rhs:
        result['warm_rhs'] = warm_rhs
    if split_back:
        result['split_back'] = split_back
    if instrumented:
        ingress_report = check_panel_ingress(
            directory, actual['coeff_sme_early_panels'], bool(panel_pack))
        if 'coeff_sme_2d_panel_b_overlap' in actual and not panel_b_overlap:
            if ingress_report['packed_b_before_lu']:
                raise ValueError('Panel B overlapped LU when --coeff-sme-2d-panel-b-overlap=0')
        result['panel_ingress'] = ingress_report
    if actual.get('coeff_input_file'):
        from check_single_line_dataset import check_dataset
        result['dataset'] = check_dataset(directory)
    if not actual['coeff_publish_lu'] or actual.get('coeff_lu_output_transition_test'):
        from check_private_lu import check_private_lu
        result['private_lu'] = check_private_lu(directory, config)
    if actual.get('coeff_recovery_test'):
        from check_single_line_recovery import check_recovery
        late = max(abort_evidence, key=lambda r: r['cycle']) if abort_evidence else None
        if n >= 17 and instrumented:
            if (not late['live'] or not late['rhs_lanes'] or not late['forward_lanes'] or
                    late['cache_hit'] != int(actual['coeff_recovery_warm'])):
                raise ValueError(f'SME late cancellation missed in-flight arithmetic: {late}')
            if 'live_rhs' in late and (not late['live_rhs'] or not late['live_forward']):
                raise ValueError(
                    f'SME late cancellation did not catch simultaneous in-flight RHS and Forward returns: {late}'
                )
        result['recovery'] = check_recovery(directory, sme_abort=late)
        result['abort_snapshots'] = abort_evidence
    return result


if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    print(json.dumps(check_sme_za(parser.parse_args().directory), indent=2))
