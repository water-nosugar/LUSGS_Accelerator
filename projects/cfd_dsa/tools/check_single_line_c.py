#!/usr/bin/env python3
"""Validate the shared five-lane cluster, LU reuse and feedback on one line."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import re
import statistics

from check_context_ablation import read_case, require_completed_simulation
from check_single_line_output_dma import options, resolved_options, validate_row, output_depth
from check_divider_model import check_case as check_divider
from trace_text import fields


def validate_cluster_trace(rows, config):
    dedicated_sub = config.get('--coeff-line-final-subtract', 'shared') == 'dedicated'
    lat = {'mul': int(config['--coeff-cluster-mul-lat']),
           'add': int(config['--coeff-cluster-add-lat']),
           'div': int(config['--coeff-cluster-div-lat']),
           'sub': int(config.get('--forward-combine-latency', '1'))}
    fma_count=int(config.get('--coeff-rhs-fma-count','0'))
    if fma_count: lat['fma']=5
    lu_fma_count=int(config.get('--coeff-lu-fma-count','0'))
    if lu_fma_count:lat['lu_fma']=5
    upper=config.get('--coeff-rhs-upper-inverse','0')=='1'
    counts = Counter()
    per_cycle = Counter()
    non_lu_mul = Counter()
    mul_count = int(config.get('--coeff-cluster-mul-count', '5'))
    div_count = int(config.get('--coeff-cluster-div-count', '1'))
    div_ready = [0] * div_count
    for row in rows:
        pool, cycle = row['pool'], int(row['cycle'])
        lanes, caller = int(row['lanes']), int(row['caller'])
        expected_pool = {0: 'div', 1: 'mul', 2: 'add', 3: 'div', 4: 'mul',
                         5: 'add', 7: ('sub' if dedicated_sub else 'add'),
                         8: 'mul', 9: 'add'}
        if pool not in lat or not (caller in range(10) or (caller==14 and upper and pool=='fma') or (caller==15 and lu_fma_count and pool=='lu_fma')) or \
                (caller == 6 and pool not in ('mul', 'add')) or \
                (caller not in (6,14,15) and expected_pool[caller] != pool and not (pool=='fma' and caller==4 and fma_count)):
            raise ValueError('operation bypassed or used the wrong shared bank')
        row_parallel = config.get('--coeff-rhs-row-parallel','0') == '1'
        valid_width = (1 <= lanes <= 4) if (row_parallel and caller in (4,5)) or (int(config.get('--coeff-line-opt-level','0'))>=7 and caller in (1,2)) else \
                      lanes == (5 if caller >= 6 else 1)
        if config.get('--coeff-lane-mvm','0')=='1' and caller in (7,8,9) and lanes==1:
            valid_width=True
        if config.get('--coeff-arithmetic-mode','legacy')!='legacy' and pool!='div':
            valid_width=1<=lanes<=(5 if caller>=6 else 4)
        if caller==14:valid_width=upper and pool=='fma' and lanes==1
        if caller==15:valid_width=bool(lu_fma_count) and pool=='lu_fma' and 1<=lanes<=4
        if not valid_width or \
                int(row['complete_cycle']) != cycle + lat[pool]:
            raise ValueError('incorrect lane width or completion latency')
        per_cycle[pool, cycle] += lanes
        if per_cycle[pool, cycle] > (lu_fma_count if pool=='lu_fma' else fma_count if pool=='fma' else div_count if pool == 'div' else mul_count if pool == 'mul' else int(config.get('--coeff-cluster-add-count','5')) if pool=='add' else 5):
            raise ValueError('shared bank oversubscribed')
        if pool == 'mul' and caller != 1:
            non_lu_mul[cycle] += lanes
            if config.get('--coeff-extra-mul-policy','shared') == 'lu-only' and non_lu_mul[cycle] > 5:
                raise ValueError('non-LU operation used the LU-only multiplier')
        if pool == 'div':
            core = next((i for i, ready in enumerate(div_ready) if ready <= cycle), None)
            if core is None:
                raise ValueError('shared divider II violated')
            div_ready[core] = cycle + int(config['--coeff-cluster-div-ii'])
        counts['shared_' + pool] += lanes
    return counts


def work(n, sweeps, interval, algo, mvm, reciprocal, shared, dedicated_sub=False,
         cached_reciprocal=False, warm_forward_tree=False, lu_reciprocal_share=False, rhs_fma=False, rhs_upper=False, lu_fma=False,
         cold_forward_tree=False):
    rebuilds = (sweeps + interval - 1) // interval
    reuse = sweeps - rebuilds
    direct = algo == 'direct'
    cold_rhs = rebuilds * ((11 if direct else 15)*n - 10)
    warm_rhs = reuse*n if direct else 0
    rhs = cold_rhs + warm_rhs
    requests = n * (sweeps if direct else rebuilds)
    col = rebuilds * ((0 if direct else 5*n) + 5*(n-1))
    col += 5*(n-1)*reuse if direct else 0
    line_products = 0
    if mvm == 'serial':
        col += 5*(n-1)*sweeps + (0 if direct else (10*n-5)*reuse)
    else:
        line_products = 5*(n-1)*sweeps + (0 if direct else (10*n-5)*reuse)
    if warm_forward_tree and direct:
        converted = 5*(n-1)*reuse
        col -= converted
        line_products += converted
    if cold_forward_tree:
        assert direct and warm_forward_tree and mvm == 'tree'
        converted = 5*(n-1)*rebuilds
        col -= converted
        line_products += converted
    line_adds = line_products if mvm != 'tree' else line_products//5*4
    # Cold multi-RHS solves share five reciprocals; cached direct sweeps have
    # only one RHS per request and still use five direct divisions. The n=1
    # direct boundary is also a one-RHS request.
    reciprocal_rhs = cold_rhs if reciprocal and (not direct or n > 1 or lu_reciprocal_share) else 0
    reciprocal_requests = n*rebuilds if reciprocal_rhs else 0
    if cached_reciprocal and reciprocal and direct and (n > 1 or lu_reciprocal_share):
        # Cached reciprocals were produced by the cold multi-RHS solve. Warm
        # uses a multiply, with no new reciprocal divide and no extra SRAM.
        reciprocal_rhs += warm_rhs
    div = 5*(reciprocal_requests + rhs-reciprocal_rhs)
    mul = 20*rhs + 5*reciprocal_rhs
    sub = 20*rhs
    combine = 10*(n-1)*sweeps
    if lu_reciprocal_share:
        assert shared and reciprocal
    if shared:
        issues = dict(shared_div=(0 if lu_reciprocal_share else 10)*n*rebuilds+div,
                      shared_mul=(40 if lu_reciprocal_share else 30)*n*rebuilds+mul+5*(col+line_products),
                      shared_add=30*n*rebuilds+sub+5*(col+line_adds)+
                                 (0 if dedicated_sub else combine))
        if rhs_fma:
            issues['shared_mul']-=20*rhs
            issues['shared_add']-=20*rhs
            issues['shared_fma']=20*rhs
            if rhs_upper:
                assert direct
                if n>1:issues['shared_fma']+=10*n*rebuilds
        if lu_fma:
            assert lu_reciprocal_share
            issues['shared_mul']-=30*n*rebuilds
            issues['shared_add']-=30*n*rebuilds
            issues['shared_lu_fma']=30*n*rebuilds
        if dedicated_sub:
            issues['shared_sub'] = combine
    else:
        issues = dict(lu_div=10*n*rebuilds, lu_mul=30*n*rebuilds,
                      lu_sub=30*n*rebuilds, coeff_div=div, coeff_mul=mul,
                      coeff_sub=sub, column_fma=col,
                      forward_combine=(n-1)*(sweeps if direct else rebuilds))
        if mvm != 'serial':
            issues.update(line_product=line_products, line_accumulate=line_adds)
    return dict(rhs=rhs, requests=requests, issues=issues, col=col,
                line_products=line_products, line_adds=line_adds, rebuilds=rebuilds)


def check_numeric(text, cells):
    rows = fields(text, 'CFD_SINGLE_LINE_NUMERIC')
    if len(rows) != len(cells):
        raise ValueError('missing full-sweep numeric validation')
    for row, n in zip(rows, cells):
        if 'residual_max' in row and (not math.isfinite(float(row['residual_max'])) or
                                     not 0 <= float(row['residual_max']) <= 1e-10):
            raise ValueError('raw recurrence residual failed')
        if int(row['values']) != 10*n or int(row['mismatches']) or \
                not all(math.isfinite(float(row[k])) and float(row[k]) >= 0
                        for k in ('max_abs', 'max_rel')):
            raise ValueError('incorrect y/dq output or missing values')
    return max(float(r['max_abs']) for r in rows)


def check_shared(path, config, pools, wanted, col):
    from check_register_ports import case_residence_cycles
    rf_residence = case_residence_cycles(path, config)
    divider = check_divider(path, config)
    if pools.keys() != wanted.keys():
        raise ValueError(f'{path.name}: additional or missing physical banks')
    div_count = int(config.get('--coeff-cluster-div-count', '1'))
    for name, expected in wanted.items():
        p = pools[name]
        short = name.removeprefix('shared_')
        count = int(config['--coeff-lu-fma-count']) if short=='lu_fma' else int(config['--coeff-rhs-fma-count']) if short=='fma' else int(config.get('--coeff-cluster-mul-count','5')) if short == 'mul' else int(config.get('--coeff-cluster-add-count','5')) if short=='add' else 5 if short=='sub' else div_count
        latency = 5 if short in ('fma','lu_fma') else int(config['--forward-combine-latency']) if short == 'sub' else \
                  int(config[f'--coeff-cluster-{short}-lat'])
        ii = int(config['--forward-combine-ii']) if short == 'sub' else \
             (int(config['--coeff-cluster-div-ii']) if short == 'div' else 1)
        occupancy = rf_residence[name] if rf_residence is not None else (divider['residence_cycles'] if divider and short == 'div' else expected*p['lat'])
        if p['count'] != count or p['lat'] != latency or p['ii'] != ii or \
                p['issued'] != expected or p['occupancy_sum'] != occupancy:
            raise ValueError(f'{path.name}/{name}: wrong physical budget or work')
        if p['samples'] != p['issue_cycles']+p['blocked_cycles']+p['no_attempt_cycles'] or \
                not p['issued'] <= p['eligible_slots'] <= p['samples']*p['count']:
            raise ValueError(f'{path.name}/{name}: invalid cycle/slot accounting')
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open(newline='') as src:
        actual = validate_cluster_trace(csv.DictReader(src), config)
    if actual != Counter(wanted):
        raise ValueError(f'{path.name}: physical issue trace lost/duplicated work')
    buf = fields((path/'simout.txt').read_text(), 'CFD_CLUSTER_BUFFER')[-1]
    if not 0 <= int(buf['max']) <= int(config['--coeff-cluster-product-depth']) or \
            int(buf['depth']) != int(config['--coeff-cluster-product-depth']) or \
            int(buf['issued']) != col or int(buf['completed']) != col or int(buf['live']):
        raise ValueError(f'{path.name}: leaked or unbounded column products')
    return int(buf['stalls'])


def regular(path):
    cycles, hashes, pools, _, binary = read_case(path)
    config = resolved_options(path)
    n, sweeps = int(config['--lusgs-cells']), int(config['--bench-iters'])
    interval = int(config['--coefficient-update-interval']) or 1
    algo, mvm = config['--coeff-base-solve'], config['--coeff-line-mvm-model']
    shared = config['--coeff-compute-model'] == 'shared'
    expected = work(n, sweeps, interval, algo, mvm,
                    config['--coeff3-div-mode'] == 'reciprocal', shared,
                    config.get('--coeff-line-final-subtract', 'shared') == 'dedicated',
                    config.get('--coeff-warm-diagonal','divide') == 'reciprocal',
                    config.get('--coeff-warm-forward-tree','0') == '1',
                    config.get('--coeff-lu-reciprocal-share','0') == '1',
                    int(config.get('--coeff-rhs-fma-count','0')) > 0,
                    config.get('--coeff-rhs-upper-inverse','0')=='1',
                    int(config.get('--coeff-lu-fma-count','0'))>0,
                    config.get('--coeff-cold-forward-tree','0')=='1')
    text = (path/'simout.txt').read_text()
    error = check_numeric(text, [n]*sweeps)
    lines = sorted(fields(text, 'CFD_LINE_SCHEDULE'), key=lambda r: int(r['token']))
    if len(lines) != sweeps or len(hashes) != sweeps:
        raise ValueError(f'{path.name}: missing forward/backward sweep')
    for s, row in enumerate(lines):
        if int(row['cache_hit']) != int(s % interval != 0):
            raise ValueError(f'{path.name}: version/dirty contract changed')
        validate_row(row, n, output_depth(config),
                     config['--coeff-spm-model'] == 'dma')
    if sum(int(r['line_products']) for r in lines) != expected['line_products'] or \
            sum(int(r['line_adds']) for r in lines) != expected['line_adds'] or \
            any(int(r['buffered_max']) > int(config.get('--coeff-line-product-depth', '5'))
                for r in lines):
        raise ValueError(f'{path.name}: MVM work/buffering incorrect')
    with (path/'coeff_preprocess_trace.csv').open(newline='') as src:
        trace = list(csv.DictReader(src))
    for stage in ('REQUEST_ALLOC', 'REQUEST_COMPLETE', 'REQUEST_AUTO_RETIRE', 'HANDOFF_PRODUCE'):
        if sum(r['stage'] == stage for r in trace) != expected['requests']:
            raise ValueError(f'{path.name}: wrong {stage} count')
    wanted_rhs = Counter()
    wanted_reuse = Counter()
    produced = {}
    for s in range(sweeps):
        cold = s % interval == 0
        generation = 0x200000000 + s*(n+1)
        if algo == 'inverse' and not cold:
            continue
        for cell in range(n):
            for col in range(1 if algo == 'direct' else 5):
                wanted_rhs[generation+cell, cell, 0, col] = 1
            if cold:
                for batch, needed in ((1, cell > 0), (2, cell+1 < n)):
                    if needed:
                        for col in range(5):
                            wanted_rhs[generation+cell, cell, batch, col] = 1
            else:
                wanted_reuse[generation+cell, cell] = 1
    actual = Counter((int(r['generation']), int(r['cell']), int(r['rhsBatch']),
                      int(r['rhsColumn'])) for r in trace if r['stage'] == 'RHS_COMPLETE')
    reuse = Counter((int(r['generation']), int(r['cell'])) for r in trace
                    if r['stage'] == 'LU_CACHE_REUSE')
    if actual != wanted_rhs or reuse != wanted_reuse:
        raise ValueError(f'{path.name}: wrong RHS mask or LU reuse')
    direct_ready = Counter((int(r['generation']), int(r['cell'])) for r in trace
                           if r['stage'] == 'DIRECT_BASE_READY')
    direct_wanted = Counter({(0x200000000+s*(n+1)+cell, cell): 1
                             for s in range(sweeps) for cell in range(n)})
    if direct_ready != (direct_wanted if algo == 'direct' else Counter()):
        raise ValueError(f'{path.name}: missing/duplicate direct base result')
    if algo == 'direct' and any(r['stage'] == 'DINV_READY' or
                               (r['stage'] == 'COLUMN_FMA_ISSUE' and r['rhsBatch'] == '0')
                               for r in trace):
        raise ValueError(f'{path.name}: explicit inverse work in direct mode')
    for r in trace:
        cell = int(r['cell']); gen = int(r['generation'])-cell
        if r['stage'] == 'HANDOFF_PRODUCE':
            produced[gen, cell] = int(r['cycle'])
        if r['stage'] == 'HANDOFF_CONSUME' and \
                (cell == 0 or (gen, cell-1) not in produced or
                 int(r['cycle']) < produced[gen, cell-1]):
            raise ValueError(f'{path.name}: broken forward dependency')
    for s, row in enumerate(lines):
        gen = 0x200000000+s*(n+1)
        if algo == 'direct' or s % interval == 0:
            if int(row['backward_start']) < produced[gen, n-1]:
                raise ValueError(f'{path.name}: premature backward start')
    def stat(key):
        m = re.search(r'\.' + re.escape(key) + r' = (\d+)', text)
        if not m:
            raise ValueError(f'{path.name}: missing {key}')
        return int(m[1])
    if stat('coeff3ColumnsSolved') != expected['rhs'] or \
            stat('streamingStep2.effectiveRhsCount') != expected['rhs'] or \
            stat('dInvGenerated') != (n*expected['rebuilds'] if algo == 'inverse' else 0) or \
            stat('baseDirectRhsSolved') != (n*sweeps if algo == 'direct' else 0) or \
            stat('baseLuCacheHits') != len(wanted_reuse) or stat('commonPreprocessFallbackRuns'):
        raise ValueError(f'{path.name}: inaccurate work stats or fallback')
    stalls = 0
    if shared:
        stalls = check_shared(path, config, pools, expected['issues'], expected['col'])
    elif pools.keys() != expected['issues'].keys() or \
            any(pools[k]['issued'] != v for k, v in expected['issues'].items()):
        raise ValueError(f'{path.name}: changed legacy work counts')
    if config['--coeff-spm-model'] == 'dma':
        stats = (path/'stats.txt').read_text()
        def dma(key):
            return int(re.search(r'\.'+key+r'\s+(\d+)', stats)[1])
        rebuilds = expected['rebuilds']
        reads = (640*n-400)*rebuilds + (40*n*(sweeps-rebuilds) if algo == 'direct' else 0)
        if dma('coeffDmaReadBytes') != reads or dma('coeffDmaIssued') != dma('coeffDmaCompleted'):
            raise ValueError(f'{path.name}: missing timed R/cache traffic or response')
    feedback = defaultdict(list)
    for r in fields(text, 'CFD_LINE_FEEDBACK'):
        feedback[int(r['token'])].append((int(r['cell']), int(r['finish'])))
    intervals = []
    for row in lines:
        values = feedback[int(row['token'])]
        if [cell for cell, _ in values] != list(range(n-2, -1, -1)):
            raise ValueError(f'{path.name}: backward cell duplicated or skipped')
        previous = int(row['backward_start'])
        for _, finish in values:
            if not previous < finish <= int(row['visible_done']):
                raise ValueError(f'{path.name}: invalid feedback completion')
            intervals.append(finish-previous)
            previous = finish
    return dict(cycles=cycles, rhs=expected['rhs'], hashes=hashes, binary=binary,
                config=config, resources=pools, max_abs=error,
                feedback_median=statistics.median(intervals) if intervals else 0,
                product_stalls=stalls)


def contract(path):
    manifest = json.loads((path/'manifest.json').read_text())
    text = (path/'simout.txt').read_text()
    config = resolved_options(path)
    require_completed_simulation(manifest,path)
    if 'LUSGS_CACHE_CONTRACT_PASS' not in text:
        raise ValueError(f'{path.name}: cache contract simulation failed')
    hits = [0,1,0,1,0,0,1,0,0,1]
    ns = [3]*5+[2]*5
    rows = fields(text, 'CFD_CACHE_CONTRACT')
    lines = sorted(fields(text, 'CFD_LINE_SCHEDULE'), key=lambda r: int(r['token']))
    if len(rows) != 10 or [int(r['cache_hit']) for r in lines] != hits or \
            any(int(r['numeric_pass']) != 1 for r in rows):
        raise ValueError(f'{path.name}: mode/size/version/dirty key failed')
    check_numeric(text, ns)
    with (path/'coeff_preprocess_trace.csv').open(newline='') as src:
        trace = list(csv.DictReader(src))
    if sum(r['stage']=='LU_CACHE_REUSE' for r in trace) != 7 or \
            sum(r['stage']=='RHS_COMPLETE' for r in trace) != 124:
        raise ValueError(f'{path.name}: stale base or coefficient rebuild on hit')
    for stage in ('REQUEST_ALLOC', 'REQUEST_COMPLETE', 'REQUEST_AUTO_RETIRE', 'HANDOFF_PRODUCE'):
        if sum(r['stage'] == stage for r in trace) != 22:
            raise ValueError(f'{path.name}: missing or duplicate cache-hit child')
    pools = {}
    for r in fields(text, 'CFD_RESOURCE_STATS'):
        name = r.pop('pool'); pools[name] = {k:int(v) for k,v in r.items()}
    # The fixture changes mode and line length between sweeps. A split MVM
    # uses the line-product buffer rather than the serial column buffer;
    # compute both work budgets from each actual fixture step.
    shared = config['--coeff-compute-model'] == 'shared'
    wanted = Counter()
    columns = 0
    for row, hit in zip(rows, hits):
        n = int(row['cells'])
        cold = work(n, 1, 1, row['mode'], config['--coeff-line-mvm-model'],
                    config['--coeff3-div-mode'] == 'reciprocal', shared,
                    config.get('--coeff-line-final-subtract', 'shared') == 'dedicated',
                    config.get('--coeff-warm-diagonal','divide') == 'reciprocal',
                    config.get('--coeff-warm-forward-tree','0') == '1',
                    config.get('--coeff-lu-reciprocal-share','0') == '1',
                    int(config.get('--coeff-rhs-fma-count','0')) > 0,
                    config.get('--coeff-rhs-upper-inverse','0')=='1',
                    int(config.get('--coeff-lu-fma-count','0'))>0,
                    config.get('--coeff-cold-forward-tree','0')=='1')
        step = cold
        if hit:
            combined = work(n, 2, 2, row['mode'], config['--coeff-line-mvm-model'],
                            config['--coeff3-div-mode'] == 'reciprocal', shared,
                            config.get('--coeff-line-final-subtract', 'shared') == 'dedicated',
                            config.get('--coeff-warm-diagonal','divide') == 'reciprocal',
                    config.get('--coeff-warm-forward-tree','0') == '1',
                    config.get('--coeff-lu-reciprocal-share','0') == '1',
                    int(config.get('--coeff-rhs-fma-count','0')) > 0,
                    config.get('--coeff-rhs-upper-inverse','0')=='1',
                    int(config.get('--coeff-lu-fma-count','0'))>0,
                    config.get('--coeff-cold-forward-tree','0')=='1')
            step = dict(issues={k: v-cold['issues'][k]
                                for k, v in combined['issues'].items()},
                        col=combined['col']-cold['col'])
        wanted.update(step['issues'])
        columns += step['col']
    if config['--coeff-compute-model'] == 'shared':
        check_shared(path, config, pools, dict(wanted), columns)
    else:
        if pools.keys() != wanted.keys() or \
                any(pools[k]['issued'] != v for k,v in wanted.items()):
            raise ValueError(f'{path.name}: incorrect legacy cache-reuse work')
    return dict(binary=manifest['gem5_sha256'], contract_hits=hits)


def check(root, report=None):
    suite = json.loads((Path(__file__).resolve().parents[1]/'suites/single-line-c-complete.json').read_text())
    results = {}
    for case in suite['cases']:
        name = case['name']; path = root/name
        result = contract(path) if name.startswith('cache-contract-') else regular(path)
        results[name] = result
        if 'config' in result:
            if result['config']['--lusgs-lines'] != '1' or result['config']['--lusgs-interleave'] != '1':
                raise ValueError(f'{name}: outside one-line scope')
            print(f'{name}: cycles={result["cycles"]} rhs={result["rhs"]} '
                  f'feedback_median={result["feedback_median"]} '
                  f'error={result["max_abs"]:.3g} product_stalls={result["product_stalls"]}')
        if name.endswith('-direct') and name.removesuffix('-direct')+'-inverse' in results:
            base = results[name.removesuffix('-direct')+'-inverse']
            left, right = dict(base['config']), dict(result['config'])
            left.pop('--coeff-base-solve'); right.pop('--coeff-base-solve')
            if left != right or base['resources'].keys() != result['resources'].keys() or \
                    any(base['resources'][p][k] != result['resources'][p][k]
                        for p in base['resources'] for k in ('count','lat','ii')):
                raise ValueError(f'{name}: non-algorithm resource/config changed')
    for algo in ('inverse','direct'):
        for phase in ('cold','mixed'):
            serial, split = results[f'{phase}-17-serial-{algo}'], results[f'{phase}-17-split-{algo}']
            if serial['hashes'] != split['hashes']:
                raise ValueError('ordered products changed FP64 bit patterns')
        for mvm in ('serial','split'):
            if results[f'mixed-17-{mvm}-{algo}']['hashes'] != results[f'legacy-mixed-{mvm}-{algo}']['hashes']:
                raise ValueError('sharing resources changed FP64 bit patterns')
    if not any(r.get('product_stalls',0) for n,r in results.items() if n.startswith('pressure-depth1')):
        raise ValueError('bounded product pressure was not exercised')
    if len({r['binary'] for r in results.values()}) != 1:
        raise ValueError('different simulator binaries')
    if report:
        report.write_text(json.dumps(dict(physical_shared_banks=dict(mul=5,add=5,div=1),
                                         synthesis_area_available=False,cases=results),indent=2)+'\n')
    print('SINGLE_LINE_C_COMPLETE_CHECK_PASS')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('campaign',type=Path); parser.add_argument('--report',type=Path)
    args=parser.parse_args(); check(args.campaign,args.report)
