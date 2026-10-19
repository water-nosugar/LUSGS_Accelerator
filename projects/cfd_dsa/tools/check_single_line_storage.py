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
from single_line_contract import coefficient_cache_bytes

from check_context_ablation import read_case
from check_single_line_c import fields, check_numeric, check_shared, work, contract
from check_single_line_output_dma import options, validate_row


def lifecycle_reserve(config):
    level=int(config.get('--coeff-line-opt-level','0'))
    reserve=0
    if level>=5:
        reserve=(512 if level>=11 else 1024)*int(config.get('--coeff-completion-depth','8'))+2048+256*int(config.get('--coeff-warm-write-depth','8'))+64*int(config['--coeff-spm-outstanding'])
    if config.get('--coeff-onchip-timing','1' if level>=8 else '0')=='1':
        capacity=sum(int(config[k]) for k in ('--coeff-input-slots','--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))
        reserve+=256*int(config.get('--coeff-onchip-queue-depth','64'))+128*capacity+1024
    if level>=9:reserve+=1024
    if level>=10:reserve+=512
    return reserve


def check_case(path):
    config = options(json.loads((path/'manifest.json').read_text())['command'])
    if config.get('--coeff-cache-contract-test')=='1':
        return check_contract(path,config)
    cycles, _, pools, _, binary = read_case(path, require_digest=False)
    n, sweeps = int(config['--lusgs-cells']), int(config['--bench-iters'])
    text = (path/'simout.txt').read_text()
    error = check_numeric(text, [n]*sweeps)
    numeric = fields(text, 'CFD_SINGLE_LINE_NUMERIC')
    hashes = [r['result_hash'] for r in numeric]
    lines = sorted(fields(text, 'CFD_LINE_SCHEDULE'), key=lambda r:int(r['token']))
    if len(lines) != sweeps:
        raise ValueError('missing sweeps')
    for row in lines:
        validate_row(row,n,int(config['--coeff-drain-queue-depth']),True)
    opt=int(config.get('--coeff-line-opt-level','0'))
    tiled=config['--coeff-line-storage']=='tiled'
    reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
        '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+16384+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
    t=int(config['--coeff-line-tile-cells'])
    buffers=int(config.get('--coeff-line-tile-buffers','1'))
    resident=min(n,t)*240*buffers
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
                  config['--coeff-line-mvm-model'],config['--coeff3-div-mode']=='reciprocal',True)
    check_shared(path,config,pools,expected['issues'],expected['col'])
    records=fields(text,'CFD_LINE_STORAGE')
    timed=config.get('--coeff-interface-timing','0')=='1'
    reload_bytes=0
    if tiled:
        t=int(config['--coeff-line-tile-cells'])
        prefix=(n-1)//t*t
        if opt>=6 and buffers==2: prefix=max(0,prefix-t)
        capacity=int(config['--coeff-line-spm-bytes'])
        reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
            '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+16384+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
        if len(records)!=sweeps:
            raise ValueError('missing finite storage accounting')
        bytoken={int(r['token']):r for r in records}
        events=fields(text,'CFD_TILE_EVENT')
        packets=fields(text,'CFD_TILE_DMA')
        for line in lines:
            token=int(line['token']); r=bytoken[token]
            hot=opt>=3 and int(line['cache_hit'])
            read_width=40 if hot and not solve_only else 240
            write_bytes=40*n if hot else 240*n-200
            expected_values=dict(cells=n,tile_cells=t,forward_tiles=(n+t-1)//t,
                reload_tiles=prefix//t,reserved=reserve,resident=resident,
                capacity=capacity,write_issued=write_bytes,write_completed=write_bytes,
                read_issued=prefix*read_width,read_completed=prefix*read_width,
                host_line_arrays=0,cache_bytes=allocation if cache_fit and (sweeps>1 or int(config['--coefficient-update-interval'])>1) else 0)
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
                [int(e['first']) for e in ready]!=list(range(prefix-t,-1,-t)) or \
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
            aggregate=int(line['cache_hit']) and not inv_warm and config.get('--coeff-record-policy')=='warm-summary'
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
        wanted={'rhs':40*n if inv_warm else 0, 'y':40*n if inv_warm else 0,
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
    if dma('coeffDmaReadBytes')!=(640*n-400)*rebuilds+(40*n+(200*(n-1) if solve_only else 0))*reuse+reload_bytes+interface_reads or \
            dma('coeffDmaWriteBytes')!=(480*n-200)*rebuilds+(80*n if opt>=3 else 280*n-200)*reuse+interface_writes or \
            dma('coeffDmaIssued')!=dma('coeffDmaCompleted'):
        raise ValueError(f'{path.name}: timing DMA traffic not closed')
    return dict(cycles=cycles,hashes=hashes,max_abs=error,binary=binary,
                reload_bytes=reload_bytes,storage=records,config=config,rebuilds=rebuilds,reuses=reuse)


def check_record_release(path,text):
    """Prove each child stays owned until its terminal record is acknowledged."""
    with (path/'coeff_preprocess_trace.csv').open(newline='') as src:
        trace=list(csv.DictReader(src))
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
    lines=sorted(fields(text,'CFD_LINE_SCHEDULE'),key=lambda r:int(r['token']))
    cases=fields(text,'CFD_CACHE_CONTRACT')
    records={r['token']:r for r in fields(text,'CFD_LINE_STORAGE')}
    summaries={r['token']:r for r in fields(text,'CFD_INTERFACE_SUMMARY')}
    t=int(config['--coeff-line-tile-cells'])
    buffers=int(config.get('--coeff-line-tile-buffers','1'))
    read_bytes=write_bytes=reload=0
    hashes=[r['result_hash'] for r in fields(text,'CFD_SINGLE_LINE_NUMERIC')]
    for case,line in zip(cases,lines):
        n=int(case['cells']);hit=int(line['cache_hit']);token=line['token']
        inv_warm=hit and case['mode']=='inverse'
        r=records[token];prefix=(n-1)//t*t
        if opt>=6 and buffers==2: prefix=max(0,prefix-t)
        hot=opt>=3 and hit
        width=40 if hot else 240
        backing=40*n if hot else 240*n-200
        reserve=8192*sum(int(config[k]) for k in ('--coeff-input-slots',
            '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))+16384+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+lifecycle_reserve(config)
        wanted=dict(cells=n,reserved=reserve,resident=240*min(n,t)*buffers,
            cache_bytes=600*n,capacity=int(config['--coeff-line-spm-bytes']),
            forward_tiles=(n+t-1)//t,reload_tiles=prefix//t,
            write_issued=backing,write_completed=backing,
            read_issued=width*prefix,read_completed=width*prefix,host_line_arrays=0,
            cache_read=40*n if inv_warm else 0,cache_write=backing if inv_warm else 0)
        if any(int(r[k])!=v for k,v in wanted.items()) or \
                reserve+int(r['resident'])+int(r['cache_bytes'])>int(r['capacity']):
            raise ValueError('fixture capacity/traffic ledger failed')
        validate_row(line,n,int(config['--coeff-drain-queue-depth']),True)
        summary=summaries[token]
        desc=int(summary['descriptor_bytes'])
        aggregate=hit and not inv_warm and config.get('--coeff-record-policy')=='warm-summary'
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
    suite=json.loads((suite_path or Path(__file__).resolve().parents[1]/'suites/single-line-d-storage.json').read_text())
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
