#!/usr/bin/env python3
"""Validate bank/port limits, readiness latency and exact SPM word traffic."""
from collections import Counter
from check_single_line_c import fields
from onchip_replay import replay_onchip
from check_input_split import check_input_split
from check_cold_rhs import check_cold_rhs


def check_onchip_arbitration(starts, activations, accesses, latency, tasks):
    """Count tasks that transfer words, rather than scheduler queue visits."""
    assert latency >= 1 and tasks >= 0
    jobs={r['id']:r for r in starts}
    activated={r['id']:int(r['cycle']) for r in activations}
    progress={}
    for row in accesses:
        job=jobs[row['id']];cycle=int(row['cycle'])
        assert cycle>=int(job['cycle'])+latency, 'SPM access bypassed arbitration latency'
        if job['active']=='0':
            assert row['id'] in activated and cycle>=activated[row['id']]+latency, 'inactive SPM task bypassed activation arbitration latency'
        progress.setdefault(cycle,set()).add(row['id'])
        assert not tasks or len(progress[cycle])<=tasks, 'SPM arbiter serviced too many distinct tasks'
    return dict(arb_latency=latency, arb_tasks=tasks,
                peak_progress_tasks=max(map(len,progress.values()),default=0))


def check_onchip(text,config,storage,trace):
    level=int(config.get('--coeff-line-opt-level','0'))
    if config.get('--coeff-onchip-timing','1' if level>=8 else '0')!='1':
        assert not fields(text,'CFD_ONCHIP_SUMMARY')
        return
    summary,ids,ends,word_counts,replay = replay_onchip(text,config)
    from check_spm_slot_rotation import check_spm_slot_rotation
    rotation=check_spm_slot_rotation(text,config,trace,ids,ends)
    if rotation:replay['spm_slot_rotation']=rotation
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
            write+=110*n-75+30*prefix+int(s['cache_bytes'])//8
            if s.get('tile_y_only','0')=='1':
                write-=25*(n-1+prefix)
        else:
            read+=(85*n-50 if mode=='direct' else 90*n-50)
            write+=10*n+5*prefix
            if int(s['cache_bytes'])==400*n:
                read+=25*(n-1)
                write+=50*(n-1)+25*prefix
            if mode=='inverse' and config.get('--coeff-warm-y-source')=='fifo':
                read-=5*n
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
        backward=check_backward_operands(text,config,ids,ends)
        if config.get('--coeff-back-independent-ready','0')=='1':
            replay['backward_independent_ready']=backward
    if level>=10:
        check_local_ready(text,config,ids,ends,storage)
    check_cached_ubar(text,storage,ids,ends)
    replay['input_split'] = check_input_split(text,config,trace,ids,ends)
    replay['cold_rhs'] = check_cold_rhs(text,config,trace)
    return replay


def check_backward_operands(text,config,jobs,completed):
    active={};cells={};used=Counter();reads={};allocated=set();released=set()
    independent=config.get('--coeff-back-independent-ready','0')=='1'
    y_uses={};column_uses={};early_operands=0
    schedules=fields(text,'CFD_LINE_SCHEDULE');fixture=fields(text,'CFD_CACHE_CONTRACT')
    expected=set()
    for i,s in enumerate(schedules):
        n=int(fixture[i]['cells']) if fixture else int(config['--lusgs-cells'])
        expected.update((s['token'],str(c)) for c in range(n-1))
    for r in fields(text,'CFD_BACK_OPERAND'):
        key=r['token'],r['cell'];cycle=int(r['cycle']);event=r['event']
        slot=r['token'],int(r['cell'])%int(config.get('--coeff-back-operand-depth','2'))
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
            assert column in range(5) and r['job']==reads[key,column]
            assert cycle>=int(completed[r['job']]['cycle'])
            if not independent or r['y_job']!='0':
                assert r['y_job']==reads[key,-1]
                assert cycle>=int(completed[r['y_job']]['cycle']), 'backward operands consumed before SPM response'
            used[key,column]+=1
            column_uses[key,column]=cycle
            early_operands+=cycle<int(completed[reads[key,-1]]['cycle']) if (key,-1) in reads else 1
        elif event=='y_use':
            assert independent and active[slot]==key and key not in y_uses
            assert r['job']==reads[key,-1]
            assert cycle>=int(completed[r['job']]['cycle']), 'backward y consumed before SPM response'
            y_uses[key]=cycle
        elif event=='release':
            assert active.pop(slot)==key and key not in released
            assert all(used[key,col]==1 for col in range(5))
            assert all(cycle>=int(completed[reads[key,col]]['cycle']) for col in range(-1,5))
            released.add(key)
        else:raise ValueError('unknown backward operand event')
    assert not active and allocated==released==expected
    assert len(reads)==6*len(expected) and sum(used.values())==5*len(expected)
    early_products=set()
    if independent:
        assert set(y_uses)==expected, 'backward y consumption missing or repeated'
        consumers=Counter()
        for prefix,scalar in (('CFD_MVM_FEEDBACK',False),('CFD_LANE_MVM',True)):
            for r in fields(text,prefix):
                if r['context']!='1' or r['event']!='issue' or int(r['node'])!=(5 if scalar else 0):continue
                key=r['token'],r['cell'];cycle=int(r['cycle'])
                assert key in y_uses and cycle>=y_uses[key], 'signed reduction bypassed y publication'
                assert cycle>=int(completed[reads[key,-1]]['cycle']), 'signed reduction used y before its SPM response'
                consumers[key]+=1
        per_cell=5 if config.get('--coeff-lane-mvm','0')=='1' else 1
        assert set(consumers)==expected and all(n==per_cell for n in consumers.values()), 'backward y consumer missing or duplicated'
        products=Counter()
        for prefix,scalar in (('CFD_MVM_PRODUCT',False),('CFD_LANE_MVM',True)):
            for r in fields(text,prefix):
                if r['context']!='1' or r['event']!='issue':continue
                col=int(r['node'] if scalar else r['column'])
                if col>=5:continue
                key=r['token'],r['cell'];cycle=int(r['cycle'])
                assert cycle>=column_uses[key,col], 'product bypassed its SPM operand publication'
                assert cycle>=int(completed[reads[key,col]]['cycle'])
                if cycle<int(completed[reads[key,-1]]['cycle']):early_products.add((key,col))
                products[key,col]+=1
        assert set(products)=={(key,col) for key in expected for col in range(5)}
        assert all(n==per_cell for n in products.values()), 'backward products missing or duplicated'
    return dict(cells=len(expected),early_operand_columns=early_operands,
                early_product_columns=len(early_products),y_consumptions=len(y_uses))


def check_local_ready(text,config,jobs,completed,storage):
    ready={};schedules=fields(text,'CFD_LINE_SCHEDULE');fixture=fields(text,'CFD_CACHE_CONTRACT')
    expected=set();sizes={};cache={}
    for i,s in enumerate(schedules):
        n=int(fixture[i]['cells']) if fixture else int(config['--lusgs-cells'])
        expected.update((s['token'],str(c)) for c in range(n));sizes[s['token']]=n
        allocation=next(r for r in storage if r['token']==s['token'])
        cache[s['token']]=(allocation.get('tile_y_only','0')=='1' or
            (int(s['cache_hit']) and int(allocation['cache_bytes'])==600*n))
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


def check_cached_ubar(text,storage,jobs,completed):
    """Prove cold reads use the one cache copy after its physical write."""
    lines={r['token']:r for r in fields(text,'CFD_LINE_SCHEDULE')}
    layouts={r['token']:r for r in storage if r.get('tile_y_only','0')=='1'}
    ready={}
    # Older logs also publish cache entries, but retain their original layout.
    events=[r for r in fields(text,'CFD_UBAR_CACHE_READY') if r['token'] in layouts]
    for r in events:
        token=r['token'];cell=int(r['cell']);s=layouts[token];key=(token,cell)
        assert key not in ready, 'duplicate cached Ubar publication'
        assert not int(lines[token]['cache_hit'])
        assert 0<=cell<int(s['cells']) and int(r['version'])>0
        job=jobs[r['job']]
        stride=int(s['cache_bytes'])//int(s['cells'])
        assert stride in (600, 608), 'unexpected full cache entry stride'
        base=int(s['reserved'])+int(s['resident'])+stride*cell+(stride-200)
        assert job['owner']==token and job['op']=='W'
        assert int(job['words'])==25 and int(job['stride'])==8
        assert int(job['base'],16)==base, 'Ubar was stored outside its cache cell'
        assert int(completed[r['job']]['cycle'])<=int(r['cycle'])
        ready[key]=r
    expected={(token,cell) for token,s in layouts.items()
              if not int(lines[token]['cache_hit']) for cell in range(int(s['cells']))}
    assert set(ready)==expected, 'missing cached Ubar publication'
    for r in fields(text,'CFD_LOCAL_READY'):
        token=r['token'];cell=int(r['cell'])
        if token not in layouts or cell+1==int(layouts[token]['cells']):continue
        if not int(lines[token]['cache_hit']):
            store=ready[token,cell]
            assert r.get('cache_job')==store['job'], 'local-ready lost its cache write dependency'
            assert int(r['cycle'])>=int(store['cycle']), 'local-ready preceded cached Ubar'
    for r in fields(text,'CFD_BACK_OPERAND'):
        token=r['token']
        if token not in layouts or r['event']!='read' or int(r['column'])<0:continue
        cell=int(r['cell']);col=int(r['column']);s=layouts[token];job=jobs[r['job']]
        stride=int(s['cache_bytes'])//int(s['cells'])
        base=int(s['reserved'])+int(s['resident'])+stride*cell+(stride-200)+8*col
        assert int(job['base'],16)==base, 'backward Ubar did not come from the cache'
        if not int(lines[token]['cache_hit']):
            assert int(r['cycle'])>=int(ready[token,cell]['cycle']), 'backward read before cached Ubar was ready'


def check_onchip_credits(text,depth,reserve):
    """Replay credit ownership, including inactive DMA reservations."""
    live={}; inactive=0
    for line in text.splitlines():
        kind=line.split(' ',1)[0]
        if kind not in ('CFD_ONCHIP_START','CFD_ONCHIP_ACTIVATE',
                        'CFD_ONCHIP_CANCEL','CFD_ONCHIP_COMPLETE'):continue
        r=dict(w.split('=',1) for w in line.split()[1:] if '=' in w)
        ident=r['id']
        if kind=='CFD_ONCHIP_START':
            assert ident not in live
            if r['active']=='0':
                assert len(live)<depth-reserve, 'DMA occupied reserved compute credit'
            live[ident]=r['active']=='0';inactive+=live[ident]
        elif kind=='CFD_ONCHIP_ACTIVATE':
            assert live[ident]
            live[ident]=False;inactive-=1
        else:
            if kind=='CFD_ONCHIP_CANCEL':assert live[ident]
            else:assert not live[ident]
            inactive-=live.pop(ident)
        assert len(live)<=depth and 0<=inactive<=depth-reserve
    assert not live and inactive==0
