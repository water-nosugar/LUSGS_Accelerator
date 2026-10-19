#!/usr/bin/env python3
"""Read-only bottleneck audit of accepted single-line traces (no simulation edits)."""
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys

REPO = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
ROOT = REPO / 'results/cfd_dsa/campaigns'
sys.path.insert(0, str(REPO / 'projects/cfd_dsa/tools'))
from check_single_line_c import fields, validate_cluster_trace, work
from check_single_line_output_dma import options


def union_length(intervals):
    end = -1
    total = 0
    for lo, hi in sorted(intervals):
        total += max(0, hi - max(lo, end))
        end = max(end, hi)
    return total


def window(rows, start, end):
    """Half-open interval; activity is [issue, completion), not issue rate."""
    issued = Counter()
    by_cycle = {p: Counter() for p in ('mul', 'add', 'div')}
    intervals = {p: [] for p in by_cycle}
    callers = Counter()
    for row in rows:
        c, done, lanes = map(int, (row['cycle'], row['complete_cycle'], row['lanes']))
        pool = row['pool']
        if start <= c < end:
            issued[pool] += lanes
            by_cycle[pool][c] += lanes
            callers[f"{pool}:{row['caller']}"] += lanes
        lo, hi = max(start, c), min(end, done)
        if lo < hi:
            intervals[pool].append((lo, hi))
    duration = end - start
    issue_cycles = set().union(*(set(x) for x in by_cycle.values()))
    live = union_length([r for v in intervals.values() for r in v])
    return dict(start=start, end_exclusive=end, duration=duration,
        issued={p: issued[p] for p in by_cycle}, caller_lanes=dict(callers),
        throughput_pct={p: 100 * issued[p] * (4 if p == 'div' else 1) /
                        ((1 if p == 'div' else 5) * duration) for p in by_cycle},
        any_pipeline_active_cycles=live,
        all_pipelines_empty_cycles=duration-live,
        all_pipelines_empty_pct=100*(duration-live)/duration,
        no_new_issue_cycles=duration-len(issue_cycles),
        pool_active_cycles={p: union_length(v) for p, v in intervals.items()},
        issue_width_histogram={p: dict(Counter(v.values())) for p, v in by_cycle.items()})


def stats_file(path):
    result = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2 and (parts[0].startswith('system.coeff_dma_cache.') or
                               '.coeffDma' in parts[0]):
            try:
                value = float(parts[1])
                if value == value:
                    result[parts[0]] = value
            except ValueError:
                pass
    return result


def audit(path):
    text = (path/'simout.txt').read_text()
    manifest = json.loads((path/'manifest.json').read_text())
    assert manifest['status'] == 'passed'
    assert 'LUSGS_STEP2_STREAMING_PASS' in text.splitlines()
    assert manifest['gem5_sha256'] == DELIVERY['gem5_sha256']
    config = options(manifest['command'])
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as src:
        cluster = list(csv.DictReader(src))
    with (path/'coeff_preprocess_trace.csv').open() as src:
        trace = list(csv.DictReader(src))
    pools = {x['pool']: {k:int(v) for k,v in x.items() if k != 'pool'}
             for x in fields(text, 'CFD_RESOURCE_STATS')}
    actual = validate_cluster_trace(cluster, config)
    expected = work(int(config['--lusgs-cells']), int(config['--bench-iters']),
                    int(config['--coefficient-update-interval']), config['--coeff-base-solve'],
                    config['--coeff-line-mvm-model'], False, True)['issues']
    assert actual == Counter(expected), path
    samples = pools['shared_mul']['samples']
    for pool, value in actual.items():
        stats = pools[pool]
        assert stats['issued'] == value
        assert stats['occupancy_sum'] == value * stats['lat']
        assert stats['samples'] == samples
        assert samples == stats['issue_cycles']+stats['blocked_cycles']+stats['no_attempt_cycles']
    cycles = int(re.search(r'streamingStep2.totalCycles = (\d+)', text)[1])
    packets = fields(text, 'CFD_INTERFACE_DMA')
    schedules = fields(text, 'CFD_LINE_SCHEDULE')
    storage = {r['token']:r for r in fields(text, 'CFD_LINE_STORAGE')}
    interfaces = {r['token']:r for r in fields(text, 'CFD_INTERFACE_SUMMARY')}
    feedback = fields(text, 'CFD_LINE_FEEDBACK')
    sweeps = []
    for s in schedules:
        token = s['token']
        start = min(int(r['cycle']) for r in packets if r['token'] == token and
                    r['kind'] == 'descriptor' and r['event'] == 'issue')
        end = int(s['visible_done'])+1
        backward, done = int(s['backward_start']), int(s['backward_done'])
        w = window(cluster, start, end)
        w.update(token=token, cache_hit=int(s['cache_hit']), schedule=s, storage=storage[token],
                 interface=interfaces[token],
                 forward=window(cluster,start,backward),
                 backward=window(cluster,backward,done),
                 completion_tail=window(cluster,done,end))
        times = sorted(int(r['finish']) for r in feedback if r['token']==token)
        w['feedback_gap_histogram'] = dict(Counter(b-a for a,b in zip(times,times[1:])))
        w['feedback_gap_median'] = statistics.median([b-a for a,b in zip(times,times[1:])])
        starts = [r for r in trace if r['stage']=='INPUT_START' and start<=int(r['cycle'])<end]
        w['input_admission'] = [{'cell':int(r['cell']),'relative_cycle':int(r['cycle'])-start} for r in starts]
        record = [r for r in packets if r['token']==token and r['kind']=='record' and r['event']=='issue']
        w['record_bytes'] = sum(int(r['bytes']) for r in record)
        sweeps.append(w)
    assert sum(w['duration'] for w in sweeps) == samples
    for short in ('mul','add','div'):
        assert sum(w['issued'][short] for w in sweeps)==pools['shared_'+short]['issued']
        assert sum(w['pool_active_cycles'][short] for w in sweeps)==pools['shared_'+short]['active_cycles']
    no_issue=sum(w['no_new_issue_cycles'] for w in sweeps)
    empty=sum(w['all_pipelines_empty_cycles'] for w in sweeps)
    io_stats=stats_file(path/'stats.txt')
    def io(suffix):
        return next((v for k,v in io_stats.items() if k.endswith(suffix)),None)
    return dict(source=str(path.relative_to(REPO)), config=config,
        input_sha256={name: hashlib.sha256((path/name).read_bytes()).hexdigest() for name in
                      ('manifest.json','simout.txt','stats.txt','coeff_preprocess_trace.csv','coeff_preprocess_trace.csv.cluster.csv')},
        guest_total_cycles=cycles, controller_cycles=samples,
        guest_outside_controller_cycles=cycles-samples, pools=pools,
        controller_issue_pct={p:100*actual['shared_'+p]*(4 if p=='div' else 1)/(samples*(1 if p=='div' else 5)) for p in ('mul','add','div')},
        whole_round_issue_pct={p:100*actual['shared_'+p]*(4 if p=='div' else 1)/(cycles*(1 if p=='div' else 5)) for p in ('mul','add','div')},
        no_new_issue_cycles=no_issue, all_pipelines_empty_cycles=empty,
        all_pipelines_empty_pct=100*empty/samples, sweeps=sweeps,
        dma=dict(issued=io('.coeffDmaIssued'),completed=io('.coeffDmaCompleted'),
                 read_bytes=io('.coeffDmaReadBytes'),write_bytes=io('.coeffDmaWriteBytes'),
                 live_max=io('.coeffDmaLiveMax'),outstanding_rejections=io('.coeffDmaOutstandingStalls'),
                 port_retries=io('.coeffDmaPortRetries')),
        io_cache=dict(write_buffer_blocked_cycles=io('.blockedCycles::no_wbuffers'),
                      mshr_blocked_cycles=io('.blockedCycles::no_mshrs'),
                      miss_rate=io('.demandMissRate::total')),
        cluster_buffers=fields(text,'CFD_CLUSTER_BUFFER')[-1],
        diagnostics={r['reason']:int(r['count']) for r in fields(text,'CFD_SCHED_DIAG')})


DELIVERY=json.loads((ROOT/'single-line-d-delivery-20261009/acceptance.json').read_text())
for filename, sha in DELIVERY['source_sha256'].items():
    assert hashlib.sha256((REPO/filename).read_bytes()).hexdigest()==sha, filename
cases={}
for name in ('overlap-'+m+'-b'+str(b)+'-w'+str(width)
             for m in ('direct','inverse') for b,width in ((1,40),(1,64),(2,40),(2,64))):
    cases[name]=ROOT/'single-line-d-optimization-accepted-20261009'/name
for name in json.loads((ROOT/'single-line-d-tuning-accepted-20261009/acceptance.json').read_text()):
    cases[name]=ROOT/'single-line-d-tuning-accepted-20261009'/name
for name in json.loads((ROOT/'single-line-d-mode-tuned-accepted-20261009/acceptance.json').read_text()):
    cases[name]=ROOT/'single-line-d-mode-tuned-accepted-20261009'/name
for k in (1,7):
    for mode in ('direct','inverse'):
        name=f'n65-t4-k{k}-{mode}'
        cases[name]=ROOT/'single-line-d-mode-accepted-20261009'/name
result={name:audit(path) for name,path in cases.items()}
(OUT/'metrics.json').write_text(json.dumps(dict(
    scope='Accepted existing single-line shared/tiled/dma runs; no new performance simulation',
    source_hashes_verified=DELIVERY['source_sha256'],
    window='[first descriptor issue, visible_done + 1), cumulative controller-active samples',
    formulas=dict(mul_add='issued/(5*cycles)',div='issued*4/cycles'),
    cases=result),indent=2)+'\n')
for name,row in result.items():
    print(name, 'total',row['guest_total_cycles'],'active',row['controller_cycles'],
          'issue%', {k:round(v,2) for k,v in row['controller_issue_pct'].items()},
          'all-empty%',round(row['all_pipelines_empty_pct'],2))
print('BOTTLENECK_AUDIT_PASS',len(result),'cases;',sum(len(r['sweeps']) for r in result.values()),'sweeps')
