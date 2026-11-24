"""Prove independent upper order, finite banks and owned actual RF returns."""
from collections import Counter
import re


def replay_lu_upper(text,config,events):
    rows=[dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
          for m in re.finditer(r'^CFD_LU_UPPER [^\n]*',text,re.M)]
    if config.get('--coeff-lu-upper-overlap','0')!='1':
        assert not rows,'disabled upper overlap emitted independent work'
        return dict(summary={},operation_ids=set())
    contexts=int(config.get('--lu5-count','4'))
    base=27+contexts+int(config.get('--coeff-rhs-product-depth','16'))
    base+=contexts if config.get('--coeff-lu-product-ahead','0')=='1' else 0
    base+=int(config.get('--coeff-rhs-factor-slots','2')) if config.get('--coeff-rhs-factor-reuse','0')=='1' else 0
    operations={r['id']:r for prefix,r in events if prefix=='CFD_RF_ISSUE'}
    returned={r['id']:int(r['cycle']) for prefix,r in events if prefix=='CFD_RF_COMPLETE'}
    owners={r['id']:r['owner'] for r in
        (dict(re.findall(r'(\w+)=([^\s]+)',m[0])) for m in
         re.finditer(r'^CFD_RF_OWNER [^\n]*',text,re.M))}
    expected={i for i,r in operations.items() if r['caller'] in ('1','2') and
              any(int(r.get('bank_'+name,'-1'))>=base for name in ('a','b','dst'))}
    states={};last_column={};bank_owners={};cancelled=set();bound=set();counts=Counter();grants=Counter()
    previous=-1
    for r in rows:
        now=int(r['cycle']);assert now>=previous,'upper events out of cycle order';previous=now
        owner=(r['token'],r['generation']);c=int(r['column']);event=r['event'];key=owner+(c,)
        if event=='cancel':
            cancelled.add(owner);counts['cancelled_contexts']+=1;continue
        assert owner not in cancelled,'upper activity after cancellation'
        assert 0<=c<4
        if event=='begin':
            assert key not in states and c==last_column.get(owner,-1)+1,'upper column reset/order'
            bank=int(r['bank']);assert base<=bank<base+2*contexts and (bank-base)%2==0
            index=(bank-base)//2
            if c:
                old=states[owner+(c-1,)]
                assert old['scale'] in returned and returned[old['scale']]<=now,'upper read an unpublished previous column'
                assert old['bank']==bank
            elif index in bank_owners:
                old_owner=bank_owners[index]
                old_states=[v for k,v in states.items() if k[:2]==old_owner]
                assert old_owner in cancelled or (len(old_states)==4 and
                    all(v['scale'] in returned and returned[v['scale']]<=now for v in old_states))
                for i in bound:
                    op=operations[i]
                    if ((op['bank_owner'],op['bank_epoch'])==old_owner and
                            any(base+2*index<=int(op.get('bank_'+n,'-1'))<base+2*index+2
                                for n in ('a','b','dst'))):
                        assert returned[i]<=now,'upper bank reused before cancelled RF drain'
            bank_owners[index]=owner;last_column[owner]=c
            states[key]=dict(begin=now,bank=bank,next_mul=0,next_sub=0,slots={},pending=None,
                             ready=None,join=None,scale=None)
            counts['begins']+=1;continue
        assert key in states,'upper operation without an owned context'
        s=states[key]
        if event in ('mul-issue','sub-issue'):
            mul=event=='mul-issue';k=int(r['k']);slot=int(r['slot']);i=r['id'];op=operations[i]
            assert now>=s['begin']+1 and 0<=k<c and slot==k%2
            assert i not in bound and i in returned and returned[i]>=now
            assert op['cycle']==r['cycle'] and op['caller']==('1' if mul else '2')
            assert op['pool']==('mul' if mul else 'add') and int(op['lanes'])==4-c
            assert (op['bank_owner'],op['bank_epoch'])==owner
            bank=s['bank']+slot;assert int(r['bank'])==bank
            grants[owner,now,mul]+=1;assert grants[owner,now,mul]<=1,'unbounded upper issue path'
            if mul:
                assert k==s['next_mul'] and slot not in s['slots'] and int(op['bank_dst'])==bank
                s['slots'][slot]=dict(k=k,id=i,ready=False);s['next_mul']+=1
            else:
                assert k==s['next_sub'] and s['pending'] is None and slot in s['slots']
                product=s['slots'].pop(slot)
                assert product['k']==k and product['ready'] and int(op['bank_a'])==bank
                s['pending']=dict(k=k,id=i)
            bound.add(i)
        elif event in ('mul-ready','sub-ready'):
            mul=event=='mul-ready';k=int(r['k']);slot=int(r['slot'])
            p=s['slots'][slot] if mul else s['pending']
            assert p and p['k']==k and p['id']==r['id'] and returned[p['id']]==now,'upper published before actual RF return'
            if mul:
                assert not p['ready'];p['ready']=True
            else:
                s['pending']=None;s['next_sub']+=1
        elif event=='ready':
            assert s['ready'] is None and now>=s['begin']+1
            assert s['next_sub']==c and s['pending'] is None and not s['slots'],'upper readiness bypassed ordered returns'
            s['ready']=now
        elif event=='join':
            assert s['ready'] is not None and s['ready']<=now and s['join'] is None,'upper join before readiness'
            s['join']=now
        else:
            assert event=='scale' and s['join'] is not None and s['join']<=now and s['scale'] is None
            op=operations[r['id']]
            assert op['cycle']==r['cycle'] and op['pool']=='mul' and op['caller']=='1'
            assert int(op['lanes'])==4-c and int(op.get('bank_dst','-1'))==-1
            assert owners.get(r['id'])==owner[0] and r['id'] in returned,'upper normalization has no owned RF return'
            s['scale']=r['id']
        counts[event]+=1
    assert expected==bound,'upper RF arithmetic missing independent ownership proof'
    for owner,last in last_column.items():
        if owner in cancelled:continue
        assert last==3 and all(states[owner+(c,)]['scale'] in returned for c in range(4)),'successful upper context did not drain'
    return dict(operation_ids=bound,summary=dict(counts,contexts=len(last_column),
                added_banks=2*contexts,extra_reservation_bytes=256*contexts+128))
