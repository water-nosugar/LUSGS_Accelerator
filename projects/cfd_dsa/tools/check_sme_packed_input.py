"""Replay the packed record geometry and the paid ingress stores."""

import csv
import json
from collections import defaultdict

from check_input_split import check_input_split
from check_spm_slot_rotation import input_base
from onchip_replay import replay_onchip
from trace_text import iter_fields


def packet_plan(base, cell, cells):
    spans = [(0, 400 if cell else 200)]
    if cell + 1 < cells:
        if cell:
            spans[0] = (0, 640)
        else:
            spans.append((400, 640))
    else:
        spans.append((600, 640))
    packets = []
    offset = 0
    for lo, hi in spans:
        while lo < hi:
            addr = base + lo
            size = min(hi - lo, 64 - addr % 64)
            packets.append((addr, size, offset))
            lo += size
            offset += size
    return packets


def check_sme_packed_input(path, text, config, cells, schedules, aborts):
    rows = list(iter_fields(text, 'CFD_SME_PACKED_INPUT'))
    layouts = {r['token']: r for r in rows}
    cancelled = {r['token']: r for r in iter_fields(text, 'CFD_SME_ABORT')}
    assert len(rows) == len(layouts) and layouts.keys() == schedules.keys() | aborts.keys()
    owners = {}
    for token, row in layouts.items():
        assert int(row['base'], 16) % 64 == 0 and int(row['stride']) == 640
        assert int(row['cells']) == cells
        assert int(row['cold']) == 1 - int((schedules.get(token) or cancelled[token])['cache_hit'])
        first = int(row['generation'])
        for cell in range(cells):
            assert first + cell not in owners
            owners[first + cell] = token, cell

    input_events = ('SPM_PACKET_ISSUE', 'SPM_PACKET_RESPONSE')
    ready_events = ('INPUT_START', 'INPUT_TOKEN', 'INPUT_D_READY', 'INPUT_L_READY',
                    'INPUT_U_READY', 'INPUT_R_READY', 'INPUT_SLOT_RELEASE',
                    'LU_ISSUE', 'COEFF3_ISSUE', 'REQUEST_FAILED',
                    'CANCEL_PENDING', 'CANCEL_COMPLETE')
    trace = []
    logical = {stage: {} for stage in input_events}
    warm = defaultdict(list)
    warm_upper = defaultdict(list)
    upper_bytes = defaultdict(int)
    upper_bases = {}
    storage = {r['token']: r for r in iter_fields(text, 'CFD_LINE_STORAGE')}
    with (path / 'coeff_preprocess_trace.csv').open() as src:
        for row in csv.DictReader(src):
            stage = row['stage']
            if stage in ready_events:
                trace.append(row)
            elif stage in input_events and row['requester'] == 'CoeffInput':
                trace.append(row)
                token, cell = owners[int(row['generation'])]
                assert int(row['cell']) == cell
                packet = row['packetId']
                assert packet not in logical[stage]
                logical[stage][packet] = row
                if stage == 'SPM_PACKET_ISSUE' and layouts[token]['cold'] == '0':
                    base = int(layouts[token]['base'], 16)
                    addr, size = int(row['address'], 0), int(row['size'])
                    if addr == base + cell * 640 + 600:
                        assert size == 40
                        warm[token, cell].append(packet)
                    else:
                        # The external adaptive cache can retain LU/Lbar and
                        # fetch Ubar through its separate legacy backing.
                        assert config.get('--coeff-sme-ubar-mode', 'external') == 'external'
                        assert token in aborts or storage[token]['tile_y_only'] == '0'
                        assert not base <= addr < base + cells * 640 and cell + 1 < cells
                        key = token, cell
                        ubar_base = addr - cell * 200 - upper_bytes[key]
                        assert upper_bases.setdefault(token, ubar_base) == ubar_base
                        assert 0 < size <= 64 - addr % 64 and upper_bytes[key] + size <= 200
                        upper_bytes[key] += size
                        warm_upper[key].append(packet)

    _, jobs, ends, _, spm = replay_onchip(text, config)
    active = {r['id']: int(r['cycle']) for r in iter_fields(text, 'CFD_ONCHIP_ACTIVATE')}
    period = int(json.loads((path / 'config.json').read_text())['system']['clk_domain']['clock'][0])
    physical = {r['packet']: r for r in iter_fields(text, 'CFD_DMA_PACKET') if r['event'] == 'queue'}
    returned = {r['packet']: int(r['tick']) // period for r in iter_fields(text, 'CFD_DMA_PACKET')
                if r['event'] == 'response'}
    transfers = defaultdict(list)
    cross = defaultdict(int)
    seen_jobs, seen_packets = set(), set()
    issues, responses = (logical[stage] for stage in input_events)
    for row in iter_fields(text, 'CFD_SME_PACKED_PACKET'):
        token, cell = row['owner'], int(row['cell'])
        layout = layouts[token]
        packet, job = row['packet'], row['job']
        assert layout['cold'] == '1' and 0 <= cell < cells
        assert packet not in seen_packets and job not in seen_jobs
        seen_packets.add(packet)
        seen_jobs.add(job)
        issue = issues[packet]
        assert (row['request'], row['cell'], row['slot'], row['cycle']) == (
            issue['requestId'], issue['cell'], issue['slot'], issue['cycle'])
        assert owners[int(issue['generation'])] == (token, cell)
        cycle, offset, slot = (int(row[k]) for k in ('cycle', 'offset', 'slot'))
        addr, size = int(issue['address'], 0), int(issue['size'])
        assert physical[packet]['op'] == 'R' and int(physical[packet]['bytes']) == size
        assert int(physical[packet]['tick']) // period == cycle
        access = jobs[job]
        assert (access['owner'], access['op'], access['active'], access['stride']) == (token, 'W', '0', '8')
        assert (int(access['base'], 16), int(access['words']), int(access['cycle'])) == (
            input_base(config, slot) + offset, size // 8, cycle)
        finish = int(ends[job]['cycle'])
        assert cycle <= returned[packet] <= active[job] <= finish
        if packet in responses:
            response = responses[packet]
            assert all(response[k] == issue[k] for k in ('requestId', 'generation', 'cell', 'address', 'size'))
            assert int(response['cycle']) == finish
        if token in aborts:
            assert finish <= int(aborts[token]['cycle'])
        else:
            assert packet in responses
        transfers[token, cell].append((addr, size, offset))
        base = int(layout['base'], 16) + cell * 640
        cross[token] += any(addr < base + boundary < addr + size for boundary in (200, 400, 600))

    # Warm lookahead bypasses the request input slot. It uses one aligned
    # 40-byte read per vector through the existing RHS ring.
    prefetched = set()
    for row in iter_fields(text, 'CFD_CACHE_DMA'):
        if row['event'] != 'issue' or row['kind'] != 'rhs':
            continue
        token, cell, packet = row['token'], int(row['cell']), row['packet']
        assert layouts[token]['cold'] == '0' and 0 <= cell < cells
        assert int(row['bytes']) == 40 and physical[packet]['bytes'] == '40'
        assert physical[packet]['op'] == 'R' and packet not in prefetched
        assert int(physical[packet]['tick']) // period == int(row['cycle'])
        prefetched.add(packet)
        warm[token, cell].append(packet)
    warm_issues = {p for packets in warm.values() for p in packets} - prefetched
    upper_issues = {p for packets in warm_upper.values() for p in packets}
    for packet in warm_issues | upper_issues:
        assert physical[packet]['op'] == 'R' and physical[packet]['bytes'] == issues[packet]['size']
        assert returned[packet] >= int(issues[packet]['cycle'])
    assert set(issues) == seen_packets | warm_issues | upper_issues

    sweeps = {}
    for token, layout in layouts.items():
        cold = layout['cold'] == '1'
        byte_count = packet_count = warm_count = upper_count = 0
        for cell in range(cells):
            got = transfers[token, cell]
            warm_count += len(warm[token, cell])
            if not cold:
                assert not got and len(warm[token, cell]) <= 1
                assert token in aborts or len(warm[token, cell]) == 1
                if token in upper_bases:
                    assert token in aborts or upper_bytes[token, cell] == (200 if cell + 1 < cells else 0)
                    upper_count += len(warm_upper[token, cell])
                continue
            assert not warm[token, cell]
            plan = packet_plan(int(layout['base'], 16) + cell * 640, cell, cells)
            assert got == (plan[:len(got)] if token in aborts else plan)
            packet_count += len(got)
            byte_count += sum(packet[1] for packet in got)
        sweeps[token] = dict(cold=cold, bytes=byte_count, packets=packet_count,
                             cross_field_packets=cross[token], warm_rhs_packets=warm_count,
                             warm_ubar_packets=upper_count)

    source = list(iter_fields(text, 'CFD_INPUT_LAYOUT_SOURCE'))
    assert source and all(r['layout'] == 'packed' and int(r['cells']) == cells and
                          int(r['bytes']) == 640 * cells and int(r['cycles']) > 0 for r in source)
    if source[0]['source'] == 'dataset':
        assert len(source) == 1
    else:
        assert all(r['source'] == 'fixture' for r in source)
        assert {r['generation'] for r in source} == {r['generation'] for r in rows}
    split = check_input_split(text, config, trace, jobs, ends, allow_abort=bool(aborts))
    return dict(sweeps=sweeps, ingress=split, onchip=spm, source=source)
