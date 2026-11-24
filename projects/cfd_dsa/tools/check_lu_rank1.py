"""Independently bind rank1 LU barriers and finite products to real RF returns."""
from collections import Counter
import re


def rows(text):
    return [dict(re.findall(r'(\w+)=([^\s]+)', m[0]))
            for m in re.finditer(r'^CFD_LU_RANK1 [^\n]*', text, re.M)]


def replay_lu_rank1(text, config, events):
    trace=rows(text)
    if config.get('--coeff-lu-rank1','0')!='1':
        assert not trace,'disabled rank1 LU emitted transfers'
        return dict(summary={},product_ids=set())
    assert config.get('--coeff-lu-product-ahead','0')=='1'
    assert config.get('--coeff-lu-upper-overlap','0')=='0'
    folded=config.get('--coeff-lu-control-fold','0')=='1'
    control_latency=0 if folded else 1
    broadcast=config.get('--coeff-preprocess-broadcast','0')=='1'
    fused=int(config.get('--coeff-lu-fma-count','0'))>0
    prefetch=config.get('--coeff-lu-pivot-prefetch','0')=='1'
    contexts=int(config.get('--lu5-count','4'))
    depth=int(config.get('--coeff-rhs-product-depth','16'))
    operations={r['id']:r for prefix,r in events if prefix=='CFD_RF_ISSUE' and r['caller'] in ('0','1','2','15')}
    returned={r['id']:int(r['cycle']) for prefix,r in events if prefix=='CFD_RF_COMPLETE'}
    owners={r['id']:r['owner'] for r in
            (dict(re.findall(r'(\w+)=([^\s]+)',m[0])) for m in
             re.finditer(r'^CFD_RF_OWNER [^\n]*',text,re.M))}
    states={};physical_contexts={};bound=set();products=set();counts=Counter();grants=Counter()
    previous=-1
    for r in trace:
        now=int(r['cycle']);assert now>=previous,'rank1 events out of cycle order';previous=now
        owner=(r['token'],r['generation']);event=r['event'];c=int(r['column'])
        assert 0<=c<5
        if event=='bind':
            index=int(r['slot']);assert 0<=index<contexts and c==0 and owner not in states
            assert int(r['bank'])==27+index,'rank1 context bank mismatch'
            if index in physical_contexts:
                old=states[physical_contexts[index]]
                assert old['cancelled'] or old['done'],'rank1 context reused while live'
                assert all(returned[i]<=now for i in old['operations']),'rank1 context reused before actual RF drain'
            physical_contexts[index]=owner
            states[owner]=dict(index=index,column=0,mode='pivot',available=now+control_latency,
                operations=set(),columns={},done=False,cancelled=False,prefetch=None)
            counts['contexts']+=1;continue
        assert owner in states,'rank1 operation without owned context'
        s=states[owner]
        if event=='cancel':
            assert not s['cancelled'];s['cancelled']=True;counts['cancelled_contexts']+=1;continue
        assert not s['cancelled'],'rank1 activity after cancellation'
        if event=='pivot':
            assert s['mode']=='pivot' and c==s['column'] and now>=s['available'],'rank1 pivot before registered trailing update'
            assert c not in s['columns'],'prefetched pivot was divided twice'
            s['columns'][c]=dict(pivot=r['id'],pivot_issue=now,reciprocal=None,scale=None,
                                 scale_return=None,slots={},pending={},completed=set(),next_mul=c+1,next_sub=c+1)
            s['mode']='finishing' if c==4 else 'reciprocal'
        elif event=='pivot-prefetch':
            assert prefetch and s['mode']=='rows' and c==s['column']+1
            prior_col=s['columns'][c-1]
            assert c in prior_col['completed'],'prefetch bypassed actual diagonal row return'
            assert s['prefetch'] is None and c not in s['columns'],'more than one early pivot per context'
            s['prefetch']=c
            s['columns'][c]=dict(pivot=r['id'],pivot_issue=now,reciprocal=None,scale=None,
                scale_return=None,slots={},pending={},completed=set(),next_mul=c+1,next_sub=c+1,
                prefetched=True,adopted=None)
        elif event=='pivot-adopt':
            assert prefetch and s['mode']=='pivot' and c==s['column'] and now>=s['available']
            assert s['prefetch']==c and s['columns'][c]['pivot']==r['id']
            s['columns'][c]['adopted']=now;s['prefetch']=None
            s['mode']='finishing' if c==4 else 'reciprocal'
        else:
            assert c in s['columns'],'rank1 event before pivot'
        col=s['columns'][c]
        if event in ('pivot','pivot-prefetch','scale','mul-issue','sub-issue','fma-issue'):
            pivot=event in ('pivot','pivot-prefetch')
            i=r['id'];op=operations[i];width=1 if pivot else 4-c
            assert i not in bound and i in returned and returned[i]>=now
            assert op['cycle']==r['cycle'] and int(op['lanes'])==width
            assert owners.get(i)==owner[0],'rank1 operation has no physical owner'
            assert op['caller']==('15' if event=='fma-issue' else '0' if pivot else '2' if event=='sub-issue' else '1')
            assert op['pool']==('lu_fma' if event=='fma-issue' else 'div' if pivot else 'add' if event=='sub-issue' else 'mul')
            if event=='pivot-prefetch':
                assert int(op['read_words'])==2 and op.get('bank_access','0')=='0'
            if not pivot:
                grants[owner,now,event!='sub-issue']+=1
                assert grants[owner,now,event!='sub-issue']<=1,'rank1 per-context issue exceeded one MUL/SUB'
            if event=='scale':
                assert s['mode']=='reciprocal' and col['reciprocal'] is not None and col['reciprocal']<=now
                assert op.get('bank_access')=='0','rank1 normalization must use its paid RF tag'
                assert c<4 and all(int(op['bank_'+k])==-1 for k in ('a','b','dst'))
                assert int(op['read_words'])==2*width,'rank1 normalization skipped RF inputs'
                col['scale']=i;s['mode']='scale'
            elif event=='fma-issue':
                assert fused and s['mode']=='rows' and c==s['column']
                row=int(r['row']);assert c<row<5 and int(r['slot'])==-1 and int(r['bank'])==-1
                assert row==col['next_mul']==col['next_sub'] and row not in col['pending']
                assert op.get('bank_access')=='0' and all(int(op['bank_'+v])==-1 for v in ('a','b','dst'))
                assert int(op['read_words'])==(2*width+1 if broadcast and width>1 else 3*width), 'LU FMA skipped paid inputs'
                col['pending'][row]=i;col['next_mul']+=1;col['next_sub']+=1
            elif event in ('mul-issue','sub-issue'):
                assert not fused,'fused LU emitted fictitious split MUL/SUB work'
                assert op.get('bank_access')=='1' and (op['bank_owner'],op['bank_epoch'])==owner
                assert s['mode']=='rows' and c==s['column']
                row,slot=int(r['row']),int(r['slot'])
                assert 0<=slot<2 and c<row<5
                bank=27+s['index']+(contexts+depth if slot else 0)
                assert int(r['bank'])==bank and int(op['bank_b'])==-1
                if event=='mul-issue':
                    assert row==col['next_mul'] and slot not in col['slots']
                    assert int(op['bank_dst'])==bank and int(op['bank_a'])==-1
                    assert int(op['read_words'])==(width+1 if broadcast and width>1 else 2*width),'rank1 MUL skipped RF inputs'
                    col['slots'][slot]=dict(row=row,id=i,ready=False);col['next_mul']+=1
                else:
                    assert row==col['next_sub'] and row not in col['pending']
                    product=col['slots'].pop(slot)
                    assert product['ready'] and product['row']==row,'rank1 subtraction before actual product return'
                    assert int(op['bank_a'])==bank and int(op['bank_words_a'])==width and int(op['bank_dst'])==-1
                    assert int(op['read_words'])==width,'rank1 trailing accumulator bypassed RF'
                    col['pending'][row]=i;col['next_sub']+=1
                products.add(i)
            bound.add(i);s['operations'].add(i)
        elif event in ('reciprocal-ready','prefetch-ready'):
            assert r['id']==col['pivot'] and col['reciprocal'] is None and returned[r['id']]==now,'rank1 reciprocal published before actual RF return'
            if event=='prefetch-ready':assert prefetch and col.get('prefetched') and col['adopted'] is None
            elif col.get('prefetched'):assert col['adopted'] is not None
            col['reciprocal']=now
        elif event=='pivot-adopt':
            pass # Adoption consumes the existing tag; it cannot issue a new RF operation.
        elif event=='scale-ready':
            assert s['mode']=='scale' and r['id']==col['scale'] and returned[r['id']]==now,'rank1 normalization published before actual RF return'
            col['scale_return']=now;s['mode']='rows-ready'
        elif event=='rows-begin':
            assert s['mode']=='rows-ready' and col['scale_return'] is not None and now>=col['scale_return']+control_latency,'rank1 upper row bypassed registered publication'
            s['mode']='rows'
        elif event=='mul-ready':
            p=col['slots'][int(r['slot'])]
            assert p['row']==int(r['row']) and p['id']==r['id'] and not p['ready'] and returned[r['id']]==now,'rank1 product published before actual RF return'
            p['ready']=True
        elif event in ('sub-ready','fma-ready'):
            assert (event=='fma-ready')==fused
            row=int(r['row']);assert col['pending'].pop(row)==r['id'] and returned[r['id']]==now,'rank1 trailing update published before actual RF return'
            assert row not in col['completed'];col['completed'].add(row)
            if col['completed']==set(range(c+1,5)):
                assert not col['slots'] and not col['pending'] and col['next_mul']==col['next_sub']==5
                s.update(column=c+1,mode='pivot',available=now+control_latency)
        elif event=='done':
            assert c==4 and s['mode']=='finishing' and now>=(col.get('adopted') or col['pivot_issue'])+1 and not s['done']
            s['done']=True;s['mode']='done'
        else:
            raise AssertionError('unknown rank1 event '+event)
        counts[event]+=1
    assert bound==set(operations),'rank1 LU arithmetic missing independent ownership proof'
    for s in states.values():
        if s['cancelled']:continue
        assert s['done'] and set(s['columns'])==set(range(5)),'successful rank1 LU context did not drain'
        for c,v in s['columns'].items():
            assert v['reciprocal'] is not None
            if c<4:assert v['completed']==set(range(c+1,5))
    return dict(product_ids=products,summary=dict(counts,extra_reservation_bytes=512*contexts+128,
                control_latency_after_actual_rf_return=control_latency,
                additional_product_banks=0,matrix_accumulators='existing owned RF; original per-element k order'),
                control_summary=dict(mode='rank1 bounded ready-return control',
                    extra_reservation_bytes=32*contexts+128,physical_returns_bound=True,
                    max_arithmetic_grants_per_context_tick='one LU FMA, normalization or pivot' if fused else 'one MUL plus one SUB, or one pivot') if folded else {})


def check_rank1_schedule(text,config,trace):
    messages=rows(text)
    if config.get('--coeff-lu-rank1','0')!='1':
        assert not messages
        return {}
    allocated={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    issued={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='LU_ISSUE'}
    inputs={}
    logical=Counter((r['requestId'],r['stage'],r['k'],r['cycle']) for r in trace)
    for r in trace:
        if r['stage'] in ('INPUT_D_READY','INPUT_TOKEN'):
            inputs[r['requestId']]=min(inputs.get(r['requestId'],int(r['cycle'])),int(r['cycle']))
    binds=set()
    mapping={'pivot':'LU_STEP_READY','pivot-adopt':'LU_STEP_READY','pivot-prefetch':'LU_RECIP_DIV_ISSUE',
             'reciprocal-ready':'LU_RECIP_READY','prefetch-ready':'LU_RECIP_READY',
             'rows-begin':'LU_COLUMN_DONE','done':'LU_COLUMN_DONE'}
    for r in messages:
        assert allocated[r['request']]==(r['generation'],r['cell'])
        if r['event']=='bind':
            assert r['request'] not in binds and issued[r['request']]==int(r['cycle'])
            assert inputs[r['request']]<=int(r['cycle']),'rank1 input alias precedes paid D input'
            binds.add(r['request'])
        elif r['event'] in mapping:
            key=(r['request'],mapping[r['event']],r['column'],r['cycle'])
            assert logical[key]==1,'rank1 factor publication differs from logical LU readiness'
            if r['event']=='done':assert logical[r['request'],'LU_COMPLETE','-1',r['cycle']]==1
    assert binds==set(issued),'rank1 LU issue lacks paid input/context proof'
    return dict(contexts=len(binds),paid_input_bindings=len(binds))
