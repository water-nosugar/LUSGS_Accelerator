#!/usr/bin/env python3
"""Validate finite tile residency, reverse timing reads and full y/dq output."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'configs'))
from single_line_contract import coefficient_cache_bytes, line_storage_bytes

from check_context_ablation import read_case
from check_single_line_c import fields, check_numeric, check_shared, work, contract
from check_single_line_output_dma import (options, resolved_options, validate_row,
    output_depth, fixed_control_reserve)


def lifecycle_reserve(config):
    level=int(config.get('--coeff-line-opt-level','0'))
    reserve=128*((24*int(config.get('--coeff-cluster-mul-lat','3'))+143)//128)*(int(config.get('--coeff-cluster-mul-count','5'))-5)
    reserve+=128*((24*int(config.get('--coeff-cluster-add-lat','4'))+143)//128)*(int(config.get('--coeff-cluster-add-count','5'))-5)
    if config.get('--coeff-arithmetic-mode','legacy')!='legacy':
        reserve+=256*int(config.get('--coeff-arithmetic-depth','12'))
    if level>=5:
        reserve+=(512 if level>=11 else 1024)*int(config.get('--coeff-completion-depth','8'))+2048+256*int(config.get('--coeff-warm-write-depth','8'))+64*int(config['--coeff-spm-outstanding'])
    if config.get('--coeff-onchip-timing','1' if level>=8 else '0')=='1':
        capacity=sum(int(config[k]) for k in ('--coeff-input-slots','--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))
        reserve+=256*int(config.get('--coeff-onchip-queue-depth','64'))+128*capacity+1024
    if level>=9:reserve+=1024
    if level>=10:reserve+=512
    if config.get('--coeff-output-coalesce','0')=='1':
        reserve+=128*int(config['--coeff-spm-outstanding'])
    reserve+=512*int(config.get('--coeff-tile-prefetch-buffers','0'))
    reserve+=4096 if config.get('--coeff-lane-mvm','0')=='1' else 0
    reserve+=4608 if config.get('--coeff-vector-local','0')=='1' or config.get('--coeff-local-products','0')=='1' else 0
    reserve+=1536 if config.get('--coeff-vector-local','0')=='1' else 0
    reserve+=128*int(config.get('--lu5-count','4'))+256 if config.get('--coeff-local-products','0')=='1' else 0
    reserve+=768 if config.get('--coeff-rhs-pair','0')=='1' else 0
    reserve+=128*int(config.get('--lu5-count','4'))+128 if config.get('--coeff-lu-product-ahead','0')=='1' else 0
    reserve+=256*int(config.get('--lu5-count','4'))+128 if config.get('--coeff-lu-upper-overlap','0')=='1' else 0
    reserve+=512*int(config.get('--lu5-count','4'))+128 if config.get('--coeff-lu-rank1','0')=='1' else 0
    reserve+=128*int(config.get('--coeff-rhs-factor-slots','2'))+256 if config.get('--coeff-rhs-factor-reuse','0')=='1' else 0
    reserve+=128 if config.get('--coeff-rhs-fma-factor-fanout','0')=='1' else 0
    reserve+=256 if config.get('--coeff-rhs-factor-fanout','0')=='1' else 0
    fma_count=int(config.get('--coeff-rhs-fma-count','0'))
    reserve+=192*5*fma_count+256 if fma_count else 0
    lu_fma_count=int(config.get('--coeff-lu-fma-count','0'))
    reserve+=192*5*lu_fma_count+256 if lu_fma_count else 0
    reserve+=256 if config.get('--coeff-rhs-frontier-reserve','0')=='1' else 0
    reserve+=256*int(config.get('--coeff-rhs-upper-slots','16'))+256 if config.get('--coeff-rhs-upper-inverse','0')=='1' else 0
    reserve+=512 if config.get('--coeff-rhs-upper-stream','0')=='1' else 0
    reserve+=128 if config.get('--coeff-lu-pivot-prefetch','0')=='1' else 0
    reserve+=128 if config.get('--coeff-input-r-before-u','0')=='1' else 0
    reserve+=128 if int(config.get('--coeff-spm-slot-rotation','0')) else 0
    reserve+=16*int(config.get('--coeff-rhs-product-depth','60'))+128 if config.get('--coeff-rhs-tree-rf-temp','0')=='1' else 0
    reserve+=32*int(config.get('--lu5-count','4'))+128 if config.get('--coeff-lu-control-fold','0')=='1' else 0
    reserve+=512*(int(config.get('--coeff-back-operand-depth','2'))-2)
    return reserve


def check_prefetch_lifetimes(text, config, aborted=(), abort_start=None,
                             schedules=None, forward_rotate=None, directory=None):
    """Prove fixed buffer IDs and DMA/store fences across range reuse."""
    extra = int(config.get('--coeff-tile-prefetch-buffers', '0'))
    if not extra:
        return
    buffers = int(config['--coeff-line-tile-buffers']) + extra
    layouts = {r['token']: r for r in fields(text, 'CFD_LINE_STORAGE')}
    aborted_map = aborted if isinstance(aborted, dict) else {t: None for t in aborted}
    aborted_tokens = set(aborted_map)
    abort_begin = abort_start if isinstance(abort_start, dict) else {}
    require_rotate = (
        bool(forward_rotate) if forward_rotate is not None
        else config.get('--coeff-sme-forward-tile-rotate') == '1'
    )
    tile_cells = int(config.get('--coeff-line-tile-cells', '0'))
    cells_cfg = int(config.get('--lusgs-cells', '0'))

    # Collect independent per-cell external y-write completion cycles so
    # physical slot commits and reuses are proven by real write responses,
    # never by forward_commit alone.
    cell_writes = {}
    write_responses = {}
    if tile_cells > 0:
        for r in fields(text, 'CFD_CACHE_DMA'):
            if r.get('event') == 'response' and r.get('kind') == 'y':
                tok = r['token']
                cell = int(r['cell'])
                first = (cell // tile_cells) * tile_cells
                cyc = int(r['cycle'])
                per_tok = cell_writes.setdefault(tok, {})
                per_tok[cell] = max(cyc, per_tok.get(cell, -1))
                write_responses[(tok, first)] = max(cyc, write_responses.get((tok, first), -1))
        if directory is not None:
            trace_path = Path(directory) / 'coeff_preprocess_trace.csv'
            assert trace_path.is_file(), 'missing coeff_preprocess_trace.csv for write-fence verification'
            windows = []
            for r in fields(text, 'CFD_SME_CONFIG'):
                windows.append((int(r['start']), int(r['done']), r['token']))
            if not windows:
                for r in fields(text, 'CFD_LINE_SCHEDULE'):
                    windows.append((0, int(r['visible_done']), r['token']))
            windows.sort()
            with trace_path.open(newline='') as src:
                for row in csv.DictReader(src):
                    if row.get('stage') != 'DRAIN_DQSTAR_VECTOR_DONE':
                        continue
                    cyc = int(row['cycle'])
                    cell = int(row['cell'])
                    first = (cell // tile_cells) * tile_cells
                    matched = [tok for lo, hi, tok in windows if lo <= cyc <= hi]
                    assert len(matched) == 1, f'ambiguous or orphan write completion at cycle {cyc}'
                    tok = matched[0]
                    per_tok = cell_writes.setdefault(tok, {})
                    per_tok[cell] = max(cyc, per_tok.get(cell, -1))
                    write_responses[(tok, first)] = max(cyc, write_responses.get((tok, first), -1))

    def verify_spans(spans, expected, partial_ok=False):
        ordered = sorted(spans)
        cursor = 0
        for offset, length in ordered:
            assert length > 0 and offset >= 0, f'invalid DMA span ({offset}, {length})'
            if partial_ok:
                assert offset >= cursor and offset + length <= expected, (
                    f'overlapping or out-of-bounds aborted DMA span ({offset}, {length})'
                )
                cursor = offset + length
            else:
                assert offset == cursor, (
                    f'non-contiguous or duplicate reload DMA offset {offset} (expected {cursor})'
                )
                cursor += length
        if not partial_ok:
            assert cursor == expected, f'incomplete tile reload coverage {cursor} != {expected}'

    local = {}
    ranges = {}
    packets = {}
    seen_packets = set()
    committed = {}  # (token, str(first)) -> max(commit_cycle, last_write_cycle)
    started = set()
    forward_ranges = {}
    forward_ready = {}
    forward_max_ready = {}
    allocated_until = {}
    records = re.finditer(r'^(CFD_LOCAL_READY|CFD_TILE_EVENT|CFD_TILE_DMA) ([^\n]*)',
                          text, re.M)
    for match in records:
        prefix, line = match.groups()
        row = dict(re.findall(r'(\w+)=([^\s]+)', line))
        token = row['token']
        cyc = int(row['cycle']) if 'cycle' in row else None
        is_aborted = token in aborted_tokens
        if is_aborted:
            if aborted_map.get(token):
                assert cyc is not None and cyc <= int(aborted_map[token]['cycle']), (
                    'tile lifecycle event escaped aborted sweep fence'
                )
            if token in abort_begin:
                begin_cyc = int(abort_begin[token]['cycle'])
                is_new_work = (
                    (prefix == 'CFD_TILE_EVENT' and row['event'] in ('forward_allocate', 'reload_start')) or
                    (prefix == 'CFD_TILE_DMA' and row['event'] == 'issue')
                )
                if is_new_work:
                    assert cyc is not None and cyc <= begin_cyc, (
                        f'new tile work {row.get("event")} initiated at {cyc} after abort begin {begin_cyc}'
                    )
        if prefix == 'CFD_LOCAL_READY':
            storage = int(row['storage'])
            assert 0 <= storage < buffers, 'invalid forward storage slot'
            local[token, storage] = row['first']
            if token in allocated_until:
                assert (token, storage) in forward_ranges, 'local ready without allocated forward slot'
                first, count, alloc_cyc = forward_ranges[token, storage]
                cell = int(row['cell'])
                assert int(row['first']) == first and first <= cell < first + count
                if alloc_cyc is not None and cyc is not None:
                    assert cyc >= alloc_cyc, 'CFD_LOCAL_READY timestamp preceded forward_allocate'
                ready = forward_ready.setdefault((token, first), set())
                assert cell not in ready, 'duplicate forward tile publication'
                ready.add(cell)
                if cyc is not None:
                    forward_max_ready[(token, first)] = max(
                        forward_max_ready.get((token, first), -1), cyc
                    )
        elif prefix == 'CFD_TILE_EVENT' and row['event'] == 'forward_allocate':
            first, count, storage = (int(row[k]) for k in ('first', 'cells', 'storage'))
            assert 0 <= storage < buffers, 'invalid forward_allocate storage slot'
            cells = int(layouts[token]['cells']) if token in layouts else cells_cfg
            assert tile_cells and first == allocated_until.get(token, 0) and first < cells
            assert count == min(tile_cells, cells - first)
            assert storage == (first // tile_cells) % buffers
            key = token, storage
            if key in forward_ranges:
                old, _, _ = forward_ranges[key]
                old_key = (token, str(old))
                assert old_key in committed, 'forward tile overwritten before external writes retired'
                if committed[old_key] is not None:
                    assert cyc is not None and cyc >= committed[old_key], (
                        'forward tile reused before external write response completed'
                    )
                last_ext = write_responses.get((token, old), -1)
                if last_ext >= 0:
                    assert cyc is not None and cyc >= last_ext, (
                        'forward tile reused before last external write DMA response'
                    )
            forward_ranges[key] = first, count, cyc
            allocated_until[token] = first + count
        elif prefix == 'CFD_TILE_EVENT' and row['event'] == 'forward_commit':
            first_i = int(row['first'])
            count_i = int(row['cells']) if 'cells' in row else None
            if 'storage' in row:
                storage = int(row['storage'])
                assert 0 <= storage < buffers, 'invalid forward_commit storage slot'
                if token in allocated_until:
                    first, count, alloc_cyc = forward_ranges[token, storage]
                    assert count_i is not None and (first, count) == (first_i, count_i)
                    assert len(forward_ready.pop((token, first), ())) == count
                    max_rdy = forward_max_ready.pop((token, first), -1)
                    if alloc_cyc is not None and cyc is not None:
                        assert cyc >= alloc_cyc, 'forward_commit timestamp preceded forward_allocate'
                    if max_rdy >= 0 and cyc is not None:
                        assert cyc >= max_rdy, 'forward_commit timestamp preceded CFD_LOCAL_READY'
            elif token in allocated_until:
                raise AssertionError('forward_commit missing storage slot in rotated mode')
            fence_cycle = cyc
            if directory is not None and (require_rotate or token in allocated_until):
                assert 'last_write' in row, 'forward_commit missing last_write fence in rotated mode'
            if 'last_write' in row:
                lw = int(row['last_write'])
                assert cyc is not None and lw <= cyc, 'forward_commit preceded its last recorded write response'
                fence_cycle = max(fence_cycle, lw)
            if directory is not None and tile_cells > 0 and not is_aborted:
                cells_total = int(layouts[token]['cells']) if token in layouts else cells_cfg
                span_cells = count_i if count_i is not None else min(tile_cells, cells_total - first_i)
                per_tok = cell_writes.get(token, {})
                missing = [c for c in range(first_i, first_i + span_cells) if c not in per_tok]
                assert not missing, (
                    f'forward_commit at first={first_i} lacks independent write completion evidence for cells {missing[:4]}'
                )
                observed_last = max(per_tok[c] for c in range(first_i, first_i + span_cells))
                assert cyc is not None and observed_last <= cyc, (
                    'forward_commit preceded external write DMA response'
                )
                if 'last_write' in row:
                    assert int(row['last_write']) >= observed_last, (
                        'forward_commit last_write disagrees with observed write completions'
                    )
                fence_cycle = max(fence_cycle, observed_last)
            else:
                last_ext = write_responses.get((token, first_i), -1)
                if last_ext >= 0:
                    assert cyc is not None and last_ext <= cyc, 'forward_commit preceded external write DMA response'
                    fence_cycle = max(fence_cycle, last_ext)
            committed[(token, row['first'])] = fence_cycle
        elif prefix == 'CFD_TILE_EVENT' and row['event'] == 'reload_start':
            storage = int(row['storage'])
            assert 0 <= storage < buffers, 'invalid reload_start storage slot'
            key = token, storage
            old = ranges.get(key)
            assert (token, row['first']) not in started, 'duplicate prefetch range'
            started.add((token, row['first']))
            if old:
                assert old['issued'] == old['completed'] == old['expected'], (
                    'tile overwritten before all DMA/SPM landings'
                )
                if old['spans'] is not None:
                    verify_spans(old['spans'], old['expected'])
            elif key in forward_ranges:
                first, _, _ = forward_ranges[key]
                old_key = (token, str(first))
                assert old_key in committed, 'forward tile overwritten before external writes retired'
                if committed[old_key] is not None:
                    assert cyc is not None and cyc >= committed[old_key], (
                        'reload reused forward slot before write response'
                    )
                last_ext = write_responses.get((token, first), -1)
                if last_ext >= 0:
                    assert cyc is not None and cyc >= last_ext, (
                        'reload reused forward slot before external write DMA response'
                    )
            elif key in local:
                old_first = local[key]
                old_key = (token, old_first)
                assert old_key in committed, 'forward tile overwritten before external writes retired'
                if committed[old_key] is not None:
                    assert cyc is not None and cyc >= committed[old_key], (
                        'reload reused forward slot before write response'
                    )
                last_ext = write_responses.get((token, int(old_first)), -1)
                if last_ext >= 0:
                    assert cyc is not None and cyc >= last_ext, (
                        'reload reused forward slot before external write DMA response'
                    )
            tile_y_only = (layouts[token]['tile_y_only'] == '1') if token in layouts else True
            expected = int(row['cells']) * (40 if tile_y_only else 240)
            ranges[key] = dict(first=row['first'], issued=0, completed=0, expected=expected,
                               spans=[] if directory is not None else None,
                               tile_y_only=tile_y_only)
        elif prefix == 'CFD_TILE_DMA':
            owners = [key for key, value in ranges.items()
                      if key[0] == token and value['first'] == row['first']]
            assert len(owners) == 1, 'DMA addresses a nonresident or overwritten tile range'
            owner = owners[0]
            amount = int(row['bytes'])
            has_offset = 'offset' in row
            if directory is not None:
                assert has_offset and 'region' in row and 'job' in row and cyc is not None, (
                    'missing offset/region/job/cycle on CFD_TILE_DMA'
                )
            offset = int(row['offset']) if has_offset else None
            region = int(row['region']) if 'region' in row else 0
            job = int(row['job']) if 'job' in row else 0
            pkt_key = (token, row['packet'])
            if row['event'] == 'issue':
                assert pkt_key not in packets and pkt_key not in seen_packets, 'duplicate tile DMA packet ID'
                seen_packets.add(pkt_key)
                if ranges[owner]['tile_y_only'] and 'region' in row:
                    assert region == 0, 'unexpected non-zero region in y-only tile reload'
                packets[pkt_key] = (owner, row['first'], offset, amount, region, job, cyc)
                ranges[owner]['issued'] += amount
                if has_offset:
                    if ranges[owner]['spans'] is None:
                        ranges[owner]['spans'] = []
                    ranges[owner]['spans'].append((offset, amount))
            else:
                issued = packets.pop(pkt_key, None)
                assert issued is not None, 'unmatched tile DMA response'
                exp_owner, exp_first, exp_off, exp_amt, exp_reg, exp_job, issue_cyc = issued
                assert (owner, row['first'], offset, amount, region, job) == (
                    exp_owner, exp_first, exp_off, exp_amt, exp_reg, exp_job
                ), 'mismatched tile DMA response fields'
                if issue_cyc is not None and cyc is not None:
                    assert cyc >= issue_cyc, 'tile DMA response preceded issue'
                ranges[owner]['completed'] += amount
    assert not packets, 'outstanding tile reload DMA packets at sweep end'
    for (tok, _), value in ranges.items():
        if tok in aborted_tokens:
            assert value['issued'] == value['completed']
            if value['spans'] is not None:
                verify_spans(value['spans'], value['expected'], partial_ok=True)
        else:
            assert value['issued'] == value['completed'] == value['expected']
            if value['spans'] is not None:
                verify_spans(value['spans'], value['expected'], partial_ok=False)

    expected_tokens = (
        set(schedules.keys()) if schedules is not None
        else set(layouts.keys()) - aborted_tokens
    )
    if directory is not None and tile_cells > 0:
        for tok in expected_tokens:
            cells_total = int(layouts[tok]['cells']) if tok in layouts else cells_cfg
            per_tok = cell_writes.get(tok, {})
            assert len(per_tok) == cells_total and set(per_tok) == set(range(cells_total)), (
                f'incomplete per-cell external y write evidence for sweep token {tok}'
            )
    if require_rotate:
        for token in expected_tokens:
            assert token in allocated_until, f'missing forward_allocate events for rotated sweep {token}'
    for token, end in allocated_until.items():
        if token in aborted_tokens:
            continue
        cells = int(layouts[token]['cells'])
        assert end == cells
        written = {int(first) for owner, first in committed if owner == token}
        assert written == set(range(0, cells, tile_cells))
        tiles = (cells + tile_cells - 1) // tile_cells
        reload_end = max(0, tiles - buffers) * tile_cells
        reloaded = {int(first) for owner, first in started if owner == token}
        assert reloaded == set(range(0, reload_end, tile_cells))


def check_case(path):
    config = resolved_options(path)
    if config.get('--coeff-cache-contract-test')=='1':
        return check_contract(path,config)
    if config.get('--coeff-lu-output-transition-test','0')=='1':
        # The guest fixture explicitly performs five calls with two policy
        # transitions: cold, hit, policy miss, hit, policy miss. Its local
        # cache-enable configuration is independent of bench-iters CLI.
        config = dict(config, **{'--bench-iters':'5',
            '--coefficient-update-interval':'2','--coeff-record-policy':'per-cell'})
    cycles, _, pools, _, binary = read_case(path, require_digest=False)
    n, sweeps = int(config['--lusgs-cells']), int(config['--bench-iters'])
    text = (path/'simout.txt').read_text()
    check_prefetch_lifetimes(text,config)
    error = check_numeric(text, [n]*sweeps)
    numeric = fields(text, 'CFD_SINGLE_LINE_NUMERIC')
    hashes = [r['result_hash'] for r in numeric]
    lines = sorted(fields(text, 'CFD_LINE_SCHEDULE'), key=lambda r:int(r['token']))
    if len(lines) != sweeps:
        raise ValueError('missing sweeps')
    for row in lines:
        validate_row(row,n,output_depth(config),True,
                     config.get('--coeff-output-coalesce','0')=='1')
    opt=int(config.get('--coeff-line-opt-level','0'))
    tiled=config['--coeff-line-storage']=='tiled'
    reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
        '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+fixed_control_reserve(config)+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
    t=int(config['--coeff-line-tile-cells'])
    buffers=int(config.get('--coeff-line-tile-buffers','1'))
    ahead=int(config.get('--coeff-tile-prefetch-buffers','0'))
    records=fields(text,'CFD_LINE_STORAGE')
    new_layout=tiled and bool(records) and 'tile_y_only' in records[0]
    resident=min(n,t)*240*(buffers+ahead)
    cache_requested=sweeps>1 or int(config['--coefficient-update-interval'])>1
    if new_layout:
        resident,allocation=line_storage_bytes(n,t,buffers+ahead,
            int(config.get('--coeff-line-cache-cells','0')),
            int(config['--coeff-line-spm-bytes'])-reserve,
            config['--coeff-base-solve']=='direct',
            config.get('--coeff-line-cache-policy','full'),cache_requested)
    else:
        allocation=coefficient_cache_bytes(n,int(config.get('--coeff-line-cache-cells','0')),
            int(config['--coeff-line-spm-bytes'])-reserve-resident,
            config['--coeff-base-solve']=='direct',config.get('--coeff-line-cache-policy','full')) if tiled else 600*n
    cache_fit=bool(allocation)
    solve_only=allocation==400*n
    interval=(int(config['--coefficient-update-interval']) or 1) if cache_fit else 1
    rebuilds=(sweeps+interval-1)//interval
    reuse=sweeps-rebuilds
    for index,line in enumerate(lines):
        if int(line['cache_hit'])!=int(index%interval!=0):
            raise ValueError('finite coefficient capacity/version contract violated')
    expected=work(n,sweeps,interval,config['--coeff-base-solve'],
                  config['--coeff-line-mvm-model'],config['--coeff3-div-mode']=='reciprocal',True,
                  config.get('--coeff-line-final-subtract','shared')=='dedicated',
                  config.get('--coeff-warm-diagonal','divide')=='reciprocal',
                  config.get('--coeff-warm-forward-tree','0')=='1',
                  config.get('--coeff-lu-reciprocal-share','0')=='1',
                  int(config.get('--coeff-rhs-fma-count','0'))>0,
                  config.get('--coeff-rhs-upper-inverse','0')=='1',
                  int(config.get('--coeff-lu-fma-count','0'))>0,
                    config.get('--coeff-cold-forward-tree','0')=='1')
    check_shared(path,config,pools,expected['issues'],expected['col'])
    timed=config.get('--coeff-interface-timing','0')=='1'
    reload_bytes=0
    if tiled:
        t=int(config['--coeff-line-tile-cells'])
        prefix=(n-1)//t*t
        if opt>=6 and buffers==2: prefix=max(0,prefix-t)
        capacity=int(config['--coeff-line-spm-bytes'])
        reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
            '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+fixed_control_reserve(config)+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
        if len(records)!=sweeps:
            raise ValueError('missing finite storage accounting')
        bytoken={int(r['token']):r for r in records}
        events=fields(text,'CFD_TILE_EVENT')
        packets=fields(text,'CFD_TILE_DMA')
        for line in lines:
            token=int(line['token']); r=bytoken[token]
            hot=opt>=3 and int(line['cache_hit'])
            y_only=new_layout and allocation==600*n
            read_width=40 if y_only or (hot and not solve_only) else 240
            write_bytes=40*n if hot else 240*n-200
            expected_values=dict(cells=n,tile_cells=t,forward_tiles=(n+t-1)//t,
                reload_tiles=prefix//t,reserved=reserve,resident=resident,
                capacity=capacity,write_issued=write_bytes,write_completed=write_bytes,
                read_issued=prefix*read_width,read_completed=prefix*read_width,
                host_line_arrays=0,cache_bytes=allocation if cache_fit and cache_requested else 0)
            if new_layout:
                expected_values['tile_y_only']=int(y_only)
            if any(int(r[k])!=v for k,v in expected_values.items()):
                raise ValueError(f'{path.name}: incorrect residency/traffic ledger')
            if reserve+int(r['resident'])+int(r['cache_bytes'])>capacity or \
                int(r['packets_issued'])!=int(r['packets_completed']) or \
                int(r['last_response'])>int(line['visible_done']):
                raise ValueError('capacity, response or completion fence violated')
            es=[e for e in events if int(e['token'])==token]
            commits=[e for e in es if e['event']=='forward_commit']
            starts=[e for e in es if e['event']=='reload_start']
            ready=[e for e in es if e['event']=='reload_ready']
            commit_order=[int(e['first']) for e in commits]
            if (sorted(commit_order) if opt>=10 else commit_order)!=list(range(0,n,t)) or \
                [int(e['first']) for e in starts]!=list(range(prefix-t,-1,-t)) or \
                (sorted((int(e['first']) for e in ready),reverse=True) if ahead else
                 [int(e['first']) for e in ready])!=list(range(prefix-t,-1,-t)) or \
                (opt<10 and int(commits[-1]['cycle'])>int(line['backward_start'])):
                raise ValueError('incorrect tile order or premature backward start')
            ps=[e for e in packets if int(e['token'])==token]
            issue=Counter((int(e['packet']),int(e['first']),int(e['region']),int(e['bytes']))
                for e in ps if e['event']=='issue')
            response=Counter((int(e['packet']),int(e['first']),int(e['region']),int(e['bytes']))
                for e in ps if e['event']=='response')
            if issue!=response or sum(k[3]*v for k,v in issue.items())!=prefix*read_width:
                raise ValueError('missing or duplicate timing read responses')
            for e in ready:
                arrivals=[int(p['cycle']) for p in ps if p['event']=='response'
                          and p['first']==e['first']]
                if not arrivals or max(arrivals)>int(e['cycle']):
                    raise ValueError('tile consumed before timing data arrived')
            reload_bytes+=prefix*read_width
    elif records:
        raise ValueError('unbounded control labeled as finite storage')
    if tiled and timed:
        check_record_release(path,text)
    stats=(path/'stats.txt').read_text()
    def dma(key):
        return int(re.search(r'\.'+key+r'\s+(\d+)',stats)[1])
    interface_reads=interface_writes=0
    if timed:
        summaries=fields(text,'CFD_INTERFACE_SUMMARY')
        transfers=fields(text,'CFD_INTERFACE_DMA')
        if len(summaries)!=sweeps:
            raise ValueError('missing timed interface accounting')
        bytoken={int(r['token']):r for r in summaries}
        for line in lines:
            token=int(line['token']);r=bytoken[token]
            inv_warm=int(line['cache_hit']) and config['--coeff-base-solve']=='inverse'
            if int(r['record_bytes'])!=(160 if opt>=11 else 320 if opt>=2 else 1784):
                raise ValueError('incorrect compact/full record ABI size')
            aggregate=not inv_warm and (config.get('--coeff-record-policy')=='line-summary' or
                not int(line['cache_hit']) and config.get('--coeff-record-policy')=='cold-summary' or
                int(line['cache_hit']) and config.get('--coeff-record-policy')=='warm-summary')
            record_bytes=0 if inv_warm else 64 if aggregate else n*int(r['record_bytes'])
            reads=int(r['descriptor_bytes']);writes=record_bytes+8
            if int(r['read_issued'])!=reads or int(r['read_completed'])!=reads or \
                    int(r['write_issued'])!=writes or int(r['write_completed'])!=writes or \
                    int(r['packets_issued'])!=int(r['packets_completed']) or \
                    int(r['last_response'])>int(line['visible_done']):
                raise ValueError('incorrect descriptor/record/status timing or fence')
            ps=[e for e in transfers if int(e['token'])==token]
            issue=Counter((e['packet'],e['kind'],int(e['bytes']))
                          for e in ps if e['event']=='issue')
            response=Counter((e['packet'],e['kind'],int(e['bytes']))
                             for e in ps if e['event']=='response')
            wanted={'descriptor':reads,'record':0 if aggregate else record_bytes,
                    'line-summary':record_bytes if aggregate else 0,'status':8}
            if issue!=response or any(sum(k[2]*v for k,v in issue.items() if k[1]==kind)!=size
                                     for kind,size in wanted.items()):
                raise ValueError('interface packets missing or duplicated')
            first_status=min(int(p['cycle']) for p in ps if p['event']=='issue' and p['kind']=='status')
            if first_status<max(int(line['children_retired']),int(line['output_last_response'])):
                raise ValueError('completion status issued before data/record responses')
            for prefix in ('CFD_CACHE_DMA','CFD_TILE_DMA'):
                responses=[int(p['cycle']) for p in fields(text,prefix)
                           if int(p['token'])==token and p['event']=='response']
                if responses and max(responses)>first_status:
                    raise ValueError('status issued before required cache/tile DMA response')
            summary_responses=[int(p['cycle']) for p in ps if p['kind']=='line-summary' and p['event']=='response']
            if summary_responses and max(summary_responses)>first_status:
                raise ValueError('status published before line summary response')
            interface_reads+=reads;interface_writes+=writes
    # Warm inverse uses bounded R DMA and explicit y/Ubar drains, no cell records.
    for line in lines if tiled else []:
        token=int(line['token']);r=next(r for r in records if int(r['token'])==token)
        inv_warm=int(line['cache_hit']) and config['--coeff-base-solve']=='inverse'
        direct_prefetch=(int(line['cache_hit']) and config['--coeff-base-solve']=='direct' and
            int(config.get('--coeff-rhs-lookahead','0'))>0 and n>1 and r.get('tile_y_only')=='1')
        direct_prefetch |= (not int(line['cache_hit']) and config['--coeff-base-solve']=='direct' and
            config.get('--coeff-cold-rhs-prefetch','0')=='1')
        wanted={'rhs':40*n if inv_warm or direct_prefetch else 0, 'y':40*n if inv_warm else 0,
                'ubar':200*(n-1) if inv_warm and opt<3 else 0}
        ps=[p for p in fields(text,'CFD_CACHE_DMA') if int(p['token'])==token]
        issue=Counter((p['packet'],p['kind'],p['cell'],int(p['bytes'])) for p in ps if p['event']=='issue')
        response=Counter((p['packet'],p['kind'],p['cell'],int(p['bytes'])) for p in ps if p['event']=='response')
        if issue!=response or any(sum(k[3]*v for k,v in issue.items() if k[1]==kind)!=size
                                 for kind,size in wanted.items()):
            raise ValueError('inverse warm R/backing DMA bypassed or lost responses')
        if int(r.get('cache_read','0'))!=wanted['rhs'] or \
                int(r.get('cache_write','0'))!=wanted['y']+wanted['ubar']:
            raise ValueError('incorrect cache data transfer accounting')
    if config.get('--coeff-lu-output-transition-test','0')=='1':
        publish_lu = [0,0,1,1,0]
    else:
        publish_lu = [int(config.get('--coeff-publish-lu','1'))]*sweeps
    private_cold = sum(not publish_lu[i] and not int(line['cache_hit'])
                       for i,line in enumerate(lines))
    if dma('coeffDmaReadBytes')!=(640*n-400)*rebuilds+(40*n+(200*(n-1) if solve_only else 0))*reuse+reload_bytes+interface_reads or \
            dma('coeffDmaWriteBytes')!=(480*n-200)*rebuilds-200*n*private_cold+(80*n if opt>=3 else 280*n-200)*reuse+interface_writes or \
            dma('coeffDmaIssued')!=dma('coeffDmaCompleted'):
        raise ValueError(f'{path.name}: timing DMA traffic not closed')
    return dict(cycles=cycles,hashes=hashes,max_abs=error,binary=binary,
                reload_bytes=reload_bytes,storage=records,config=config,rebuilds=rebuilds,reuses=reuse)


def check_record_release(path,text):
    """Prove each child stays owned until its terminal record is acknowledged."""
    with (path/'coeff_preprocess_trace.csv').open(newline='') as src:
        # Only these events enter the ownership proof. Long uncached lines
        # contain millions of scheduler/wait samples; do not materialize them.
        trace=[r for r in csv.DictReader(src) if r['stage'] in (
            'REQUEST_ALLOC','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE','REQUEST_SUMMARY')]
    retired=[r for r in trace if r['stage']=='REQUEST_AUTO_RETIRE']
    stages=[Counter(r['requestId'] for r in trace if r['stage']==stage) for stage in
            ('REQUEST_ALLOC','REQUEST_COMPLETE','REQUEST_AUTO_RETIRE')]
    if stages[0]!=stages[1] or stages[0]!=stages[2] or any(v!=1 for v in stages[0].values()):
        raise ValueError('terminal child state was duplicated or skipped')
    transfers=fields(text,'CFD_INTERFACE_DMA')
    summarized={r['requestId'] for r in trace if r['stage']=='REQUEST_SUMMARY'}
    for line in fields(text,'CFD_LINE_SCHEDULE'):
        ps=[p for p in transfers if p['token']==line['token']]
        first=min(int(p['cycle']) for p in ps if p['kind']=='descriptor')
        end=int(line['visible_done'])
        cells={int(p['cell']) for p in ps if p['kind']=='record'}
        rs=[r for r in retired if first<int(r['cycle'])<=end]
        if any(p['kind']=='line-summary' for p in ps):
            if not rs or any(r['requestId'] not in summarized for r in rs) or cells:
                raise ValueError('line summary did not replace exactly its child records')
            responses=[int(p['cycle']) for p in ps if p['kind']=='line-summary' and p['event']=='response']
            if not responses or max(responses)>end:
                raise ValueError('line summary not acknowledged before completion')
            first_issue=min(int(p['cycle']) for p in ps if p['kind']=='line-summary' and p['event']=='issue')
            if first_issue<max(int(r['cycle']) for r in rs):
                raise ValueError('line summary issued before its requests retired')
            continue
        if {int(r['cell']) for r in rs}!=cells or len(rs)!=len(cells):
            raise ValueError('timed record/cell retirement ownership mismatch')
        for r in rs:
            responses=[int(p['cycle']) for p in ps if p['kind']=='record' and
                       p['event']=='response' and int(p['cell'])==int(r['cell'])]
            if not responses or max(responses)>int(r['cycle']):
                raise ValueError('child released before terminal record responses')


def check_contract(path,config):
    opt=int(config.get('--coeff-line-opt-level','0'))
    result=contract(path) # mode/size/version/dirty + R changed on every hit
    text=(path/'simout.txt').read_text()
    check_prefetch_lifetimes(text,config)
    lines=sorted(fields(text,'CFD_LINE_SCHEDULE'),key=lambda r:int(r['token']))
    cases=fields(text,'CFD_CACHE_CONTRACT')
    records={r['token']:r for r in fields(text,'CFD_LINE_STORAGE')}
    summaries={r['token']:r for r in fields(text,'CFD_INTERFACE_SUMMARY')}
    t=int(config['--coeff-line-tile-cells'])
    buffers=int(config.get('--coeff-line-tile-buffers','1'))
    ahead=int(config.get('--coeff-tile-prefetch-buffers','0'))
    read_bytes=write_bytes=reload=0
    hashes=[r['result_hash'] for r in fields(text,'CFD_SINGLE_LINE_NUMERIC')]
    for case,line in zip(cases,lines):
        n=int(case['cells']);hit=int(line['cache_hit']);token=line['token']
        inv_warm=hit and case['mode']=='inverse'
        r=records[token];prefix=(n-1)//t*t
        if opt>=6 and buffers==2: prefix=max(0,prefix-t)
        hot=opt>=3 and hit
        y_only=r.get('tile_y_only','0')=='1'
        width=40 if y_only or hot else 240
        backing=40*n if hot else 240*n-200
        reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
            '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+fixed_control_reserve(config)+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
        resident,allocation=line_storage_bytes(n,t,buffers+ahead,
            int(config.get('--coeff-line-cache-cells','0')),
            int(config['--coeff-line-spm-bytes'])-reserve,case['mode']=='direct',
            config.get('--coeff-line-cache-policy','full')) if 'tile_y_only' in r else (240*min(n,t)*(buffers+ahead),600*n)
        wanted=dict(cells=n,reserved=reserve,resident=resident,
            cache_bytes=600*n,capacity=int(config['--coeff-line-spm-bytes']),
            forward_tiles=(n+t-1)//t,reload_tiles=prefix//t,
            write_issued=backing,write_completed=backing,
            read_issued=width*prefix,read_completed=width*prefix,host_line_arrays=0,
            cache_read=40*n if inv_warm or (case['mode']=='direct' and
                ((hit and n>1 and y_only and int(config.get('--coeff-rhs-lookahead','0'))>0) or
                 (not hit and config.get('--coeff-cold-rhs-prefetch','0')=='1'))) else 0,
            cache_write=backing if inv_warm else 0)
        if 'tile_y_only' in r:
            wanted['tile_y_only']=int(allocation==600*n)
        if any(int(r[k])!=v for k,v in wanted.items()) or \
                reserve+int(r['resident'])+int(r['cache_bytes'])>int(r['capacity']):
            raise ValueError('fixture capacity/traffic ledger failed')
        validate_row(line,n,output_depth(config),True,config.get('--coeff-output-coalesce','0')=='1')
        summary=summaries[token]
        desc=int(summary['descriptor_bytes'])
        aggregate=not inv_warm and (config.get('--coeff-record-policy')=='line-summary' or
            not hit and config.get('--coeff-record-policy')=='cold-summary' or
            hit and config.get('--coeff-record-policy')=='warm-summary')
        records_bytes=0 if inv_warm else 64 if aggregate else n*int(summary['record_bytes'])
        if int(summary['read_issued'])!=desc or int(summary['read_completed'])!=desc or \
                int(summary['write_issued'])!=records_bytes+8 or int(summary['write_completed'])!=records_bytes+8 or \
                int(summary['packets_issued'])!=int(summary['packets_completed']) or \
                int(summary['last_response'])>int(line['visible_done']):
            raise ValueError('fixture timed interface incomplete')
        for name in ('CFD_TILE_DMA','CFD_INTERFACE_DMA','CFD_CACHE_DMA'):
            ps=[p for p in fields(text,name) if p['token']==token]
            issue=Counter((p['packet'],int(p['bytes'])) for p in ps if p['event']=='issue')
            response=Counter((p['packet'],int(p['bytes'])) for p in ps if p['event']=='response')
            if issue!=response: raise ValueError('fixture lost timing response')
        read_bytes+=(40*n if hit else 640*n-400)+width*prefix+desc
        write_bytes+=((80*n if opt>=3 else 280*n-200) if hit else 480*n-200)+records_bytes+8
        reload+=width*prefix
    stats=(path/'stats.txt').read_text()
    dma=lambda k:int(re.search(r'\.'+k+r'\s+(\d+)',stats)[1])
    if dma('coeffDmaReadBytes')!=read_bytes or dma('coeffDmaWriteBytes')!=write_bytes or \
            dma('coeffDmaIssued')!=dma('coeffDmaCompleted'):
        raise ValueError('fixture DMA data/interface traffic not closed')
    check_record_release(path,text)
    return dict(result,config=config,cycles=0,hashes=hashes,reload_bytes=reload,
                max_abs=check_numeric(text,[3]*5+[2]*5),storage=list(records.values()))


def check(root,report,suite_path=None):
    suite=json.loads((suite_path or Path(__file__).resolve().parents[1]/'suites/history/single-line-d-storage.json').read_text())
    rows={}
    for case in suite['cases']:
        name=case['name']; rows[name]=check_case(root/name)
        print(name,'cycles',rows[name]['cycles'],'reload',rows[name]['reload_bytes'])
        if name.endswith('-tiled'):
            peer=name.removesuffix('-tiled')+'-full'
            if peer not in rows: continue
            full=rows[peer]
            if rows[name]['hashes']!=full['hashes']:
                raise ValueError('tile spills changed FP64 bit patterns')
    if len({r['binary'] for r in rows.values()})!=1:
        raise ValueError('different gem5 binaries')
    if report: report.write_text(json.dumps(rows,indent=2)+'\n')
    print('SINGLE_LINE_STORAGE_CHECK_PASS')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign',type=Path);parser.add_argument('--report',type=Path)
    parser.add_argument('--suite',type=Path)
    args=parser.parse_args();check(args.campaign,args.report,args.suite)
