#!/usr/bin/env python3
"""Replay the divider's finite hardware contract from independently read inputs.

The cluster trace's complete_cycle remains its no-stall prediction. The divider
trace carries actual production and response times, including blocked cycles.
"""
import argparse
from collections import deque
import csv
import json
import math
from pathlib import Path
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError('divider: ' + message)


def numeric_bits(a, b):
    x, y = (struct.unpack('>d', bytes.fromhex(v))[0] for v in (a, b))
    if math.isnan(x) or math.isnan(y) or (math.isinf(x) and math.isinf(y)):
        return 0x7ff8000000000000
    if y == 0:
        if x == 0:
            return 0x7ff8000000000000
        sign = (int(a, 16) ^ int(b, 16)) & (1 << 63)
        return sign | 0x7ff0000000000000
    z = x / y
    return int.from_bytes(struct.pack('>d', z), 'big')


def validate_divider_trace(rows, summary):
    """Replay global clock enable, FIFO fall-through and writeback arbitration."""
    s = {k: (v if k == 'model' else int(v)) for k, v in summary.items()}
    latency, ii, capacity, depth, wb = (s[k] for k in
        ('lat', 'ii', 'capacity', 'result_depth', 'writeback_ii'))
    require(min(latency, ii, capacity, depth, wb) > 0, 'nonpositive configuration')
    require(s['model'] in ('pipelined', 'iterative'), 'unknown model')
    if s['model'] == 'iterative':
        require(capacity == 1 and ii >= latency, 'overlapping iterative core')
    admits, responses = {}, {}
    tags = set()
    last_cycle = -1
    for r in rows:
        t = int(r['cycle'])
        require(t >= last_cycle, 'trace is not ordered')
        last_cycle = t
        if r['event'] == 'accept':
            tag = r['tag']
            require(tag not in tags and t not in admits, 'duplicate admission')
            tags.add(tag)
            admits[t] = r
        else:
            require(r['event'] == 'result' and t not in responses, 'invalid/duplicate response')
            responses[t] = r
    require(bool(admits), 'missing admissions')
    core, results = deque(), deque()
    cooldown = next_return = 0
    core_sum = result_sum = compute = wait = residence = peak_latency = 0
    core_peak = result_peak = stalls = output_stalls = returned = flags = dropped = 0
    remaining_admits = iter(sorted(admits))
    next_admit = next(remaining_admits, None)
    now = min(admits)
    end = max([*admits, *responses])
    while now <= end:
        core_sum += len(core)
        result_sum += len(results)
        ready = now >= next_return
        response = None
        if results:
            if ready:
                response = results.popleft()
            else:
                output_stalls += 1
        stalled = bool(core and core[0]['remaining'] == 1 and len(results) == depth)
        if stalled:
            stalls += 1
        else:
            cooldown = max(0, cooldown - 1)
            for token in core:
                token['remaining'] -= 1
            if core and core[0]['remaining'] == 0:
                token = core.popleft()
                token['produced'] = now
                compute += now - token['accepted']
                if ready and response is None and not results:
                    response = token
                else:
                    results.append(token)
        result_peak = max(result_peak, len(results))
        actual = responses.get(now)
        require((actual is None) == (response is None), f'wrong response cycle {now}')
        if response is not None:
            for key in ('tag', 'owner', 'a', 'b'):
                require(actual[key] == response[key], f'changed {key} or reordered result')
            require(int(actual['accepted']) == response['accepted'] and
                    int(actual['produced']) == response['produced'], 'incorrect token timestamps')
            require(int(actual['result'], 16) == numeric_bits(actual['a'], actual['b']),
                    'incorrect FP64 result or stale operands')
            require(int(actual['core']) == len(core) and int(actual['outputs']) == len(results),
                    'incorrect response occupancy')
            require(0 <= int(actual['flags']) <= 31 and int(actual['dropped']) in (0, 1),
                    'invalid flags/discard marker')
            flags |= int(actual['flags'])
            dropped += int(actual['dropped'])
            returned += 1
            wait += now - response['produced']
            residence += now - response['accepted']
            peak_latency = max(peak_latency, now - response['accepted'])
            next_return = now + wb
        if now in admits:
            r = admits[now]
            require(not stalled and cooldown == 0 and len(core) < capacity,
                    f'admission without ready/credit at {now}')
            require(int(r['accepted']) == now, 'incorrect acceptance timestamp')
            core.append(dict(r, accepted=now, remaining=latency))
            cooldown = ii
            core_peak = max(core_peak, len(core))
            require(int(r['core']) == len(core) and int(r['outputs']) == len(results),
                    'incorrect admission occupancy')
            next_admit = next(remaining_admits, None)
        require(len(core) <= capacity and len(results) <= depth, 'unbounded occupancy')
        if not core and not results and next_admit is not None and next_admit > now + 1:
            cooldown = max(0, cooldown - (next_admit - now - 1))
            now = next_admit
        else:
            now += 1
    require(not core and not results and returned == len(admits) == len(responses),
            'lost or live transactions at quiescence')
    expected = dict(accepted=len(admits), produced=returned, returned=returned,
        core_live=0, result_live=0, core_peak=core_peak, result_peak=result_peak,
        pipeline_stalls=stalls, output_stalls=output_stalls,
        core_occupancy=core_sum, result_occupancy=result_sum, compute_cycles=compute,
        result_wait_cycles=wait, residence_cycles=residence, max_residence=peak_latency,
        dropped=dropped, flags=flags)
    require(core_sum == compute and result_sum == wait and core_sum+result_sum == residence,
            'invalid residence accounting')
    for key, value in expected.items():
        require(s[key] == value, f'{key}: summary={s[key]}, replay={value}')
    return dict(s, admissions=[(t, r['tag']) for t, r in admits.items()])


def check_case(path, config=None):
    path = Path(path)
    summaries = [dict(re.findall(r'(\w+)=([^ ]+)', line))
        for line in (path/'simout.txt').read_text().splitlines()
        if line.startswith('CFD_DIVIDER_SUMMARY ')]
    if not summaries:
        require(not config or config.get('--coeff-divider-model', 'timing') == 'timing',
                'detailed mode has no summary')
        return None
    with (path/'coeff_preprocess_trace.csv.divider.csv').open(newline='') as src:
        rows = list(csv.DictReader(src))
    result = validate_divider_trace(rows, summaries[-1])
    if config:
        for option, key in (('--coeff-cluster-div-lat', 'lat'),
                ('--coeff-cluster-div-ii', 'ii'), ('--coeff-divider-result-depth', 'result_depth'),
                ('--coeff-divider-writeback-ii', 'writeback_ii')):
            if option in config:
                require(int(config[option]) == result[key], f'{option} not applied')
        if '--coeff-divider-model' in config:
            require(config['--coeff-divider-model'] == result['model'], 'model not applied')
        capacity = int(config.get('--coeff-divider-inflight', '0'))
        if not capacity:
            capacity = 1 if result['model'] == 'iterative' else (result['lat']+result['ii']-1)//result['ii']
        require(capacity == result['capacity'], 'capacity/auto capacity not applied')
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open(newline='') as src:
        cluster = [int(r['cycle']) for r in csv.DictReader(src) if r['pool'] == 'div']
    require(cluster == [t for t, _ in result['admissions']], 'division bypassed tagged interface')
    return result


def actual_divider_completions(path):
    trace = Path(path)/'coeff_preprocess_trace.csv.divider.csv'
    if not trace.exists():
        return {}
    with trace.open(newline='') as src:
        return {int(r['accepted']): int(r['cycle']) for r in csv.DictReader(src)
                if r['event'] == 'result'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cases', type=Path, nargs='+')
    args = parser.parse_args()
    for case in args.cases:
        result = check_case(case)
        print(json.dumps(dict(case=str(case), divider=result), sort_keys=True))
