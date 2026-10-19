#!/usr/bin/env python3
"""Validate bank/port limits, readiness latency and exact SPM word traffic."""
from collections import Counter
from check_single_line_c import fields


def check_onchip(text,config,storage,trace):
    level=int(config.get('--coeff-line-opt-level','0'))
    if config.get('--coeff-onchip-timing','1' if level>=8 else '0')!='1':
        assert not fields(text,'CFD_ONCHIP_SUMMARY')
        return
    summary=fields(text,'CFD_ONCHIP_SUMMARY')[-1]
    depth=int(config.get('--coeff-onchip-queue-depth','64'))
    banks=int(config['--coeff-spm-banks']);grain=int(config['--coeff-spm-bank-granularity'])
    ports={'R':int(config['--coeff-spm-read-ports']),'W':int(config['--coeff-spm-write-ports'])}
    latency=int(config.get('--coeff-onchip-latency','1'))
    assert int(summary['depth'])==depth and int(summary['peak'])<=depth and int(summary['live'])==0
    assert int(summary['banks'])==banks and int(summary['grain'])==grain
    assert int(summary['read_ports'])==ports['R'] and int(summary['write_ports'])==ports['W']
    assert int(summary['latency'])==latency
    starts=fields(text,'CFD_ONCHIP_START');done=fields(text,'CFD_ONCHIP_COMPLETE')
    cancellations=fields(text,'CFD_ONCHIP_CANCEL');activations=fields(text,'CFD_ONCHIP_ACTIVATE')
    ids={r['id']:r for r in starts};ends={r['id']:r for r in done};cancelled={r['id'] for r in cancellations}
    activated={r['id']:int(r['cycle']) for r in activations}
    assert len(ids)==len(starts)==int(summary['created'])
    assert len(ends)==len(done)==int(summary['completed'])
    assert len(cancelled)==len(cancellations)==int(summary['cancelled'])
    assert set(ids)==set(ends)|cancelled and not set(ends)&cancelled
    per_port=Counter();words=Counter();word_counts=Counter();last={}
    for r in fields(text,'CFD_ONCHIP_ACCESS'):
        job=ids[r['id']];cycle=int(r['cycle']);bank=int(r['bank']);op=r['op']
        address=int(r['addr'],16);index=words[r['id']]
        assert r['id'] not in cancelled and op==job['op'] and r['owner']==job['owner']
        assert address==int(job['base'],16)+index*int(job['stride'])
        assert address+8<=int(config['--coeff-line-spm-bytes'])
        assert bank==(address//grain)%banks
        assert cycle>=int(job['cycle'])+1
        if job['active']=='0':assert cycle>=activated[r['id']]+1
        per_port[cycle,bank,op]+=1
        assert per_port[cycle,bank,op]<=ports[op], 'SPM bank port oversubscribed'
        words[r['id']]+=1;word_counts[op,bank]+=1;last[r['id']]=cycle
    for id,end in ends.items():
        assert words[id]==int(ids[id]['words'])
        assert int(end['cycle'])==last[id]+latency, 'SPM access completed before its latency'
    latest={int(r['bank']):r for r in fields(text,'CFD_ONCHIP_BANK')}
    assert len(latest)==banks
    for bank,r in latest.items():
        assert int(r['read_words'])==word_counts['R',bank]
        assert int(r['write_words'])==word_counts['W',bank]
    # Derive traffic from the actual sweep contract, independent of scheduler.
    schedules=fields(text,'CFD_LINE_SCHEDULE');fixture=fields(text,'CFD_CACHE_CONTRACT')
    read=write=0;t=int(config['--coeff-line-tile-cells'])
    buffers=int(config['--coeff-line-tile-buffers'])
    for i,line in enumerate(schedules):
        n=int(fixture[i]['cells']) if fixture else int(config['--lusgs-cells'])
        mode=fixture[i]['mode'] if fixture else config['--coeff-base-solve']
        hit=int(line['cache_hit']);s=next(s for s in storage if s['token']==line['token'])
        prefix=(n-1)//t*t
        if level>=6 and buffers==2:prefix=max(0,prefix-t)
        if not hit:
            read+=110*n-75
            write+=110*n-75+30*prefix+(75*n if int(s['cache_bytes']) else 0)
        else:
            read+=(85*n-50 if mode=='direct' else 90*n-50)
            write+=10*n+5*prefix
    assert sum(v for (op,_),v in word_counts.items() if op=='R')==read, 'missing/duplicate SPM operand reads'
    assert sum(v for (op,_),v in word_counts.items() if op=='W')==write, 'missing/duplicate SPM stores'
    reused={r['requestId'] for r in trace if r['stage']=='LU_CACHE_REUSE'}
    ready={r['requestId']:r for r in trace if r['stage']=='CACHE_REGISTER_READY'}
    assert set(ready)==reused, 'cached operands bypassed SPM-to-register loading'
    tokens={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='INPUT_TOKEN'}
    for id,r in ready.items():
        assert r['packetId'] in ends and int(r['cycle'])>=int(ends[r['packetId']]['cycle'])
        assert int(r['cycle'])<=tokens[id]
    for r in trace:
        if r['requestId'] in reused and r['stage']=='COLUMN_FMA_ISSUE':
            assert int(r['cycle'])>=int(ready[r['requestId']]['cycle']), 'cached Lbar consumed before bank response'
    if level>=9:
        check_backward_operands(text,config,ids,ends)
    if level>=10:
        check_local_ready(text,config,ids,ends)


def check_backward_operands(text,config,jobs,completed):
    active={};cells={};used=Counter();reads={};allocated=set();released=set()
    schedules=fields(text,'CFD_LINE_SCHEDULE');fixture=fields(text,'CFD_CACHE_CONTRACT')
    expected=set()
    for i,s in enumerate(schedules):
        n=int(fixture[i]['cells']) if fixture else int(config['--lusgs-cells'])
        expected.update((s['token'],str(c)) for c in range(n-1))
    for r in fields(text,'CFD_BACK_OPERAND'):
        key=r['token'],r['cell'];cycle=int(r['cycle']);event=r['event']
        slot=r['token'],int(r['cell'])%2
        if event=='allocate':
            assert key not in allocated and slot not in active
            assert int(r['slot'])==slot[1]
            allocated.add(key);active[slot]=key;cells[key]=cycle
        elif event=='read':
            assert active[slot]==key
            column=int(r['column']);job=jobs[r['job']]
            assert (key,column) not in reads and column in range(-1,5)
            assert job['owner']==r['token'] and job['op']=='R' and job['words']=='5'
            assert int(job['stride'])==(8 if column==-1 else 40)
            assert int(job['cycle'])==cycle
            reads[key,column]=r['job']
        elif event=='use':
            assert active[slot]==key and cycle>=cells[key]
            column=int(r['column'])
            assert r['job']==reads[key,column] and r['y_job']==reads[key,-1]
            assert cycle>=int(completed[r['job']]['cycle'])
            assert cycle>=int(completed[r['y_job']]['cycle']), 'backward operands consumed before SPM response'
            used[key,column]+=1
        elif event=='release':
            assert active.pop(slot)==key and key not in released
            assert all(used[key,col]==1 for col in range(5))
            assert all(cycle>=int(completed[reads[key,col]]['cycle']) for col in range(-1,5))
            released.add(key)
        else:raise ValueError('unknown backward operand event')
    assert not active and allocated==released==expected
    assert len(reads)==6*len(expected) and sum(used.values())==5*len(expected)


def check_local_ready(text,config,jobs,completed):
    ready={};schedules=fields(text,'CFD_LINE_SCHEDULE');fixture=fields(text,'CFD_CACHE_CONTRACT')
    expected=set();sizes={};cache={}
    for i,s in enumerate(schedules):
        n=int(fixture[i]['cells']) if fixture else int(config['--lusgs-cells'])
        expected.update((s['token'],str(c)) for c in range(n));sizes[s['token']]=n;cache[s['token']]=int(s['cache_hit'])
    for r in fields(text,'CFD_LOCAL_READY'):
        key=r['token'],r['cell'];assert key not in ready
        ready[key]=r;cycle=int(r['cycle'])
        for name,words in [('y_job',5),('u_job',25)]:
            if r[name]=='0':
                assert name=='u_job' and (cache[r['token']] or int(r['cell'])+1==sizes[r['token']])
                continue
            job=jobs[r[name]]
            assert job['op']=='W' and int(job['words'])==words and job['owner']==r['token']
            assert int(completed[r[name]]['cycle'])<=cycle, 'resident ready before SPM stores completed'
    assert set(ready)==expected
    for r in fields(text,'CFD_BACK_OPERAND'):
        if r['event']=='allocate':
            assert int(r['cycle'])>=int(ready[r['token'],r['cell']]['cycle'])
    for r in fields(text,'CFD_EARLY_BACKWARD'):
        tail=ready[r['token'],str(sizes[r['token']]-1)]
        job=jobs[r['tail_job']]
        assert job['op']=='R' and job['words']=='5'
        assert int(tail['cycle'])<=int(job['cycle'])<=int(completed[r['tail_job']]['cycle'])<=int(r['cycle'])
