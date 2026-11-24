"""Prove batch publication from paid ingress stores and finite SPM reads."""
from collections import defaultdict
from statistics import median
from check_single_line_c import fields


def check_input_batches(text, config, trace, jobs, ends, allow_abort=False):
    assert config.get('--coeff-input-d-early','0') == '1'
    packed = config.get('--coeff-sme-packed-input', '0') == '1'
    by_request=defaultdict(list)
    for r in trace:by_request[r['requestId']].append(r)
    split=defaultdict(dict)
    for r in fields(text,'CFD_INPUT_SPLIT'):
        assert r['event'] in ('d_read','d_ready','input_ready')
        assert r['event'] not in split[r['request']]
        split[r['request']][r['event']]=r
    batches=defaultdict(lambda:defaultdict(dict))
    for r in fields(text,'CFD_INPUT_BATCH'):
        assert r['request'] in split and r['matrix'] in ('L','U','R')
        assert r['event'] in ('read','ready')
        entry=batches[r['request']][r['matrix']]
        assert r['event'] not in entry,'duplicate batch input event'
        entry[r['event']]=r
    cold={r['request']:r for r in fields(text,'CFD_DIRECT_RHS')
          if r['event']=='consume' and r.get('mode')=='cold'}
    aborted={r['requestId'] for r in trace if r['stage'] in
             ('REQUEST_FAILED','CANCEL_PENDING','CANCEL_COMPLETE')}
    used=set();intervals=defaultdict(list);leads=[];early=discarded=loads=early_batches=0
    expected={request for request,rows in by_request.items()
              if (not allow_abort or request not in aborted) and
              any(r['stage']=='LU_ISSUE' for r in rows) and
              sum(int(r['size']) for r in rows if r['stage']=='SPM_PACKET_ISSUE'
                  and r.get('requester')=='CoeffInput')+(40 if request in cold else 0)>200}
    assert set(split)-aborted==expected,'missing batch ingress ownership'
    for request,events in split.items():
        cancelled=allow_abort and request in aborted
        discarded+=cancelled
        rows=by_request[request];d=events['d_read']
        owner,slot,total=d['owner'],int(d['slot']),int(d['bytes'])
        ingress=total-(40 if request in cold else 0)
        assert ingress>=200 and ingress%8==0
        assert all(all(r[k]==d[k] for k in ('generation','cell','owner','slot','bytes'))
                   for r in events.values())
        returned={r['packetId']:r for r in rows if r['stage']=='SPM_PACKET_RESPONSE'
                  and r.get('requester')=='CoeffInput'}
        ranges=[];offset=0
        for r in rows:
            if r['stage']!='SPM_PACKET_ISSUE' or r.get('requester')!='CoeffInput':continue
            size=int(r['size']);assert size>0
            response=returned.get(r['packetId'])
            if response:
                assert response['size']==r['size'] and int(response['cycle'])>=int(r['cycle'])
            ranges.append((offset,offset+size,int(response['cycle']) if response else None))
            offset+=size
        assert offset<=ingress
        if not cancelled:assert offset==ingress and len(returned)==len(ranges)

        def once(stage,optional=False):
            matches=[r for r in rows if r['stage']==stage]
            assert len(matches)<=1 if optional else len(matches)==1,stage
            return matches[0] if matches else None

        def covered_ranges(base, bytes):
            end = base + bytes
            overlaps = [(a,b,t) for a,b,t in ranges if a < end and b > base]
            if packed:
                # One paid input beat can supply words to two batch regions.
                return [(max(a,base),min(b,end),t) for a,b,t in overlaps]
            return overlaps

        def paid_load(row,base,bytes):
            job=row['job'];assert job not in used,'SPM load job reused';used.add(job)
            access=jobs[job]
            assert (access['owner'],access['op'],access['active'])==(owner,'R','1')
            assert (int(access['base'],16),int(access['words']),int(access['stride']))==(slot*8192+(8*(slot%4) if int(config.get('--coeff-spm-slot-rotation','0'))&2 else 0)+base,bytes//8,8)
            assert int(access['cycle'])==int(row['cycle'])
            covering=covered_ranges(base,bytes)
            assert covering and covering[0][0]==base and covering[-1][1]==base+bytes
            assert all(a>=base and b<=base+bytes and t is not None and t<=int(row['cycle'])
                       for a,b,t in covering),'batch read preceded its own stores'
            assert int(ends[job]['cycle'])>=int(row['cycle'])
            return int(ends[job]['cycle'])

        d_end=paid_load(d,0,200)
        dr=events.get('d_ready');ready_d=once('INPUT_D_READY',cancelled)
        if dr:
            assert ready_d and dr['job']==d['job']==ready_d['packetId']
            assert int(dr['cycle'])==int(ready_d['cycle'])>=d_end
        else:assert cancelled and ready_d is None
        ordered=sorted(batches[request].items(),key=lambda pair:int(pair[1]['read']['offset']))
        priority=config.get('--coeff-input-r-before-u','0')=='1' and ingress==640 and request not in cold
        r_stored=None
        if priority:
            assert config.get('--coeff-input-l-first','0')=='1'
            assert config.get('--coeff-cold-rhs-prefetch','0')!='1'
            # Physical coverage remains D,L,U,R regardless of temporal selection.
            offsets={'L':200,'U':400,'R':600}
            for matrix,pair in ordered:
                assert int(pair['read']['offset'])==offsets[matrix], 'wrong physical batch region'
            if not cancelled:assert set(batches[request])==set(offsets)
            r_ranges=covered_ranges(600,40)
            if (r_ranges and r_ranges[0][0]==600 and r_ranges[-1][1]==640 and
                    all(a>=600 and b<=640 and t is not None for a,b,t in r_ranges)):
                r_stored=max(t for a,b,t in r_ranges)
            ordered=sorted(ordered,key=lambda pair:int(pair[1]['read']['cycle']))
        sequence=[];covered=200;last_end=None;last_job=d['job']
        for matrix,pair in ordered:
            read=pair['read'];bytes=40 if matrix=='R' else 200
            assert matrix!='R' or request not in cold,'R loaded twice'
            assert int(read['bytes'])==bytes
            if not priority:assert int(read['offset'])==covered
            assert int(read['region'])*200==int(read['offset'])
            assert (read['owner'],read['slot'],read['generation'],read['cell'])==(owner,d['slot'],d['generation'],d['cell'])
            end=paid_load(read,int(read['offset']),bytes);loads+=1
            if priority and matrix!='L':
                lower=batches[request].get('L',{}).get('ready')
                assert lower and int(read['cycle'])>=int(lower['cycle']), 'priority input bypassed L publication'
                if matrix=='U' and 'R' not in sequence:
                    assert r_stored is None or r_stored>int(read['cycle']), 'stored R priority bypassed by U'
            if last_end is not None:assert int(read['cycle'])>=last_end,'more than one rest read per request'
            ready=pair.get('ready');stage=once('INPUT_'+matrix+'_READY',cancelled)
            if ready:
                assert stage and ready['job']==read['job']==stage['packetId']
                assert all(ready[k]==read[k] for k in ('owner','slot','generation','cell','region','offset','bytes','matrix'))
                assert int(ready['cycle'])==int(stage['cycle'])>=end
            else:assert cancelled and stage is None
            sequence.append(matrix);covered+=bytes;last_end=end;last_job=read['job']
        order=['L','U','R'] if packed or config.get('--coeff-input-l-first','0')=='1' else ['U','L','R']
        if priority:
            assert any(sequence==candidate[:len(sequence)]
                       for candidate in (['L','U','R'],['L','R','U'])), 'invalid R priority sequence'
        else:
            assert sequence==[m for m in order if m in sequence] and len(set(sequence))==len(sequence)
        full=once('INPUT_TOKEN',cancelled)
        if not cancelled:
            assert covered==ingress and full is not None and dr is not None
            assert events['input_ready']['job']==last_job
            assert int(events['input_ready']['cycle'])==int(full['cycle'])
            assert int(full['cycle'])>=max(d_end,last_end or d_end)
            if request in cold:assert int(full['cycle'])>=int(cold[request]['cycle'])
            start,release,lu=(once(s) for s in ('INPUT_START','INPUT_SLOT_RELEASE','LU_ISSUE'))
            assert int(start['cycle'])<=int(d['cycle'])
            assert int(release['cycle'])>=int(full['cycle']),'input slot released while reads live'
            intervals[slot].append((int(start['cycle']),int(release['cycle'])))
            early+=int(lu['cycle'])<int(full['cycle'])
            leads.append(max(0,int(full['cycle'])-int(lu['cycle'])))
            early_batches+=sum(int(p['ready']['cycle'])<int(full['cycle']) for p in batches[request].values())
        for r in rows:
            if r['stage']=='LU_ISSUE':assert ready_d and int(r['cycle'])>=int(ready_d['cycle'])
            if r['stage']=='COEFF3_ISSUE':
                inputs=[int(x['cycle']) for x in rows if x['stage'] in ('INPUT_L_READY','INPUT_U_READY','INPUT_R_READY')]
                assert ready_d and inputs and int(r['cycle'])>=max(int(ready_d['cycle']),min(inputs))
    for lifetimes in intervals.values():
        ordered=sorted(lifetimes)
        assert all(a[1]<=b[0] for a,b in zip(ordered,ordered[1:])),'input landing slot overwritten'
    return dict(requests=len(split)-discarded,early_lu_requests=early,
                median_lu_lead_cycles=median(leads) if leads else 0,max_lu_lead_cycles=max(leads,default=0),
                extra_operand_words=0,added_load_jobs=loads+len(cold),aborted_d_reads=discarded,
                batch_publication=True,early_batches=early_batches,max_rest_reads_per_request=1)


def check_batch_rhs(text,config,trace,allow_abort=False):
    groups=defaultdict(list)
    for r in trace:groups[r['requestId']].append(r)
    raw={r['request'] for r in fields(text,'CFD_INPUT_SPLIT')}
    aborted={r['requestId'] for r in trace if r['stage'] in ('REQUEST_FAILED','CANCEL_PENDING','CANCEL_COMPLETE')}
    count=early=parks=0
    for request in raw:
        rows=groups[request];cancelled=allow_abort and request in aborted
        ready={r['stage']:int(r['cycle']) for r in rows if r['stage'] in ('INPUT_L_READY','INPUT_U_READY','INPUT_R_READY')}
        expected={('0','0')}
        for batch,stage in [('1','INPUT_L_READY'),('2','INPUT_U_READY')]:
            if stage in ready:expected.update((batch,str(i)) for i in range(5))
        init={};complete={};park=None
        full=next((int(r['cycle']) for r in rows if r['stage']=='INPUT_TOKEN'),None)
        for r in rows:
            key=r['rhsBatch'],r['rhsColumn']
            if r['stage']=='RHS_INPUT_INIT':
                assert key not in init and key in expected,'duplicate or unowned batch RHS'
                stage={'0':'INPUT_R_READY','1':'INPUT_L_READY','2':'INPUT_U_READY'}[key[0]]
                assert stage in ready and int(r['cycle'])>=ready[stage],'RHS initialized before its paid load'
                if key[0]=='2':
                    assert full is not None and int(r['cycle'])>=full,'Ubar occupied a context before full input'
                init[key]=int(r['cycle']);count+=1
                early+=full is not None and int(r['cycle'])<full
            elif r['stage']=='RHS_COMPLETE':
                assert key not in complete;complete[key]=int(r['cycle'])
            elif r['stage']=='TRSM_INPUT_PARK':
                assert config.get('--coeff-cold-rhs-prefetch','0')=='1'
                assert park is None and set(init)=={('0','0')} and ('0','0') in complete,'partial batch RHS released its context'
                park=int(r['cycle']);parks+=1
                assert park>=complete['0','0'] and (full is None or park<=full)
            elif r['stage']=='TRSM_INPUT_RESUME':
                candidates=[t for t in (full,ready.get('INPUT_L_READY')) if t is not None]
                assert park is not None and candidates and int(r['cycle'])>=max(park+1,min(candidates)), 'batch resume bypassed input or registered selection'
            elif r['stage'].startswith('TRSM_') and r['stage'].endswith('_ISSUE') and int(r['rhsBatch'])>=0:
                assert key in init and int(r['cycle'])>=init[key],'batch RHS arithmetic preceded initialization'
        if not cancelled:assert init.keys()==complete.keys()==expected
        else:assert complete.keys()<=init.keys()
        assert all(complete[k]>=init[k] for k in complete)
    active=set();parked=set();peak=0
    for _,r in sorted(enumerate(trace),key=lambda pair:(int(pair[1]['cycle']),pair[0])):
        request,stage=r['requestId'],r['stage']
        if stage=='COEFF3_ISSUE':
            assert request not in active and request not in parked;active.add(request)
        elif stage=='TRSM_INPUT_PARK':
            assert request in active and request not in parked
            active.remove(request);parked.add(request)
        elif stage=='TRSM_INPUT_RESUME':
            assert request in parked and request not in active
            parked.remove(request);active.add(request)
        elif stage=='COEFF3_COMPLETE':
            assert request in active;active.remove(request)
        elif stage in ('REQUEST_FAILED','CANCEL_PENDING','CANCEL_COMPLETE'):
            assert allow_abort;active.discard(request);parked.discard(request)
        elif stage.startswith('TRSM_') and stage.endswith('_ISSUE') and int(r['rhsBatch'])>=0:
            assert request in active,'batch arithmetic without execution credit'
        peak=max(peak,len(active))
        assert len(active)<=int(config['--coeff3-count']),'RHS contexts overbooked'
    assert not active and not parked,'batch RHS execution credit leak'
    return dict(batch_requests=len(raw),batch_initializations=count,early_batch_initializations=early,
                parked_requests=parks,peak_contexts=peak,resume_selection_cycles=1)
