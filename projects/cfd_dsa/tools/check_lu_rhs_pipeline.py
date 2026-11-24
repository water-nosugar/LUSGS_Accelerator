"""Independent replay of bounded LU products and temporal RHS factor captures."""
from collections import Counter
import re


def replay_rhs_factors(text, config, operations):
    enabled=config.get('--coeff-rhs-factor-reuse','0')=='1'
    if not enabled:
        assert not re.search(r'^CFD_RHS_FACTOR ',text,re.M),'disabled factor latches emitted transfers'
        assert not any(int(r.get('factor_id','0')) for prefix,r in operations if prefix=='CFD_RF_ISSUE')
        return dict(uses={},fills=[],base=0,summary={})
    slots={}; issued={r['id']:r for prefix,r in operations if prefix=='CFD_RF_ISSUE' and r['caller']=='4'}
    depth=int(config.get('--coeff-rhs-factor-slots','2'))
    assert depth in (2,4,8,16,32,64), 'invalid factor latch capacity'
    uses={}; fills=[]; identities=set(); parents=set();previous=-1
    counts=Counter(); last_fill=-1
    base=27+int(config.get('--lu5-count','4'))+int(config.get('--coeff-rhs-product-depth','16'))
    if config.get('--coeff-lu-product-ahead','0')=='1':base+=int(config.get('--lu5-count','4'))
    for match in re.finditer(r'^CFD_RHS_FACTOR [^\n]*',text,re.M):
        row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        cycle=int(row['cycle']);slot=int(row['slot']);bank=int(row['bank'])
        assert cycle>=previous,'factor events out of cycle order'
        previous=cycle
        k=int(row['k']);width=int(row['width']);event=row['event']
        assert 0<=slot<depth and bank==base+slot and 0<=k<4 and width==4-k
        if depth>8:
            token=int(row['token']);first=((token^(token>>3))%(depth//8))*8
            assert first<=slot<first+8,'factor hash set exceeded eight ways'
        assert row['backward'] in ('0','1')
        key=tuple(row[n] for n in ('token','generation','backward','k'))
        payload=tuple(row['v'+str(i)] for i in range(5))
        if event=='clear':
            old=slots.pop(slot)
            assert old['cache']==row['cache'] and old['key']==key
            counts['clears']+=1;continue
        op=issued[row['id']]
        assert op['cycle']==row['cycle'] and op['pool'] in ('mul','fma') and op['caller']=='4'
        assert int(op['lanes'])==width
        fma=op['pool']=='fma'
        assert not fma or (int(config.get('--coeff-rhs-fma-count','0'))>0 and (row['backward']=='0' or config.get('--coeff-rhs-upper-inverse','0')=='1'))
        assert op.get('bank_access')=='1' or (fma and event=='fill')
        assert (fma and event=='fill') or (op['bank_owner']==row['token'] and op['bank_epoch']==row['generation'])
        assert 0<=int(row['rhs'])<15
        if event=='fill':
            assert row['cache'] not in identities and row['id'] not in parents
            assert int(op.get('factor_id','0'))==0
            assert int(op['bank_a'])==int(op['bank_b'])==-1
            assert int(op['read_words'])==((2*width+1 if op['broadcast']=='1' else 3*width) if fma else (width+1 if op['broadcast']=='1' else 2*width))
            assert last_fill!=cycle,'factor ingress exceeds one capture/cycle'
            assert not any(s['key']==key for s in slots.values()),'duplicate retained factor'
            if slot in slots:assert slots[slot]['used']!=cycle,'factor overwritten during capture'
            identities.add(row['cache']);parents.add(row['id']);last_fill=cycle
            slots[slot]=dict(row,key=key,payload=payload,filled=cycle,used=cycle)
            fills.append({n:row[n] for n in ('cycle','bank','width')});counts['fills']+=1
        else:
            assert event=='use' and row['id'] not in uses
            old=slots[slot]
            assert old['cache']==row['cache'] and old['key']==key and old['payload']==payload
            assert old['filled']<cycle,'factor register publication bypassed'
            assert old['used']!=cycle,'factor bank read more than once/cycle'
            assert op.get('factor_id')==row['cache']
            assert int(op['bank_a'])==bank and int(op['bank_words_a'])==width
            assert int(op['bank_b'])==-1 and int(op['bank_words_b'])==0
            old['used']=cycle;uses[row['id']]={n:row[n] for n in ('token','generation','bank','width')}
            counts['hits']+=1;counts['saved_read_words']+=width
            counts['b_hits' if int(row['rhs'])<5 else 'lbar_hits' if int(row['rhs'])<10 else 'ubar_hits']+=1
    physical={row['id'] for prefix,row in operations if prefix=='CFD_RF_ISSUE' and int(row.get('factor_id','0')) and not int(row.get('factor_source','0'))}
    assert physical==set(uses),'factor read has no registered producer/consumer binding'
    if not enabled:assert not uses and not fills
    return dict(uses=uses,fills=fills,base=base,summary=dict(counts))


def replay_lu_products(text, config, events=None, independent_upper=None):
    enabled=config.get('--coeff-lu-product-ahead','0')=='1'
    if not enabled:
        assert not re.search(r'^CFD_LU_PRODUCTS[ _]',text,re.M),'disabled LU window emitted transfers'
        return {}
    if events is None:
        from check_register_ports import _rows
        events=list(_rows(text))
    operations={r['id']:r for prefix,r in events if prefix=='CFD_RF_ISSUE' and r['caller'] in ('1','2')}
    returns={r['id']:int(r['cycle']) for prefix,r in events if prefix=='CFD_RF_COMPLETE' and r['id'] in operations}
    states={};counts=Counter();cycle_grants=Counter();bound=set();previous=-1
    cancelled=set()
    for match in re.finditer(r'^CFD_LU_PRODUCTS_CANCEL [^\n]*',text,re.M):
        row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        cancelled.add((row['token'],row['generation']))
    lu_count=int(config.get('--lu5-count','4'));rhs_depth=int(config.get('--coeff-rhs-product-depth','16'))
    for match in re.finditer(r'^CFD_LU_PRODUCTS [^\n]*',text,re.M):
        row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        cycle=int(row['cycle']);c=int(row['column']);k=int(row['k']);slot=int(row['slot'])
        assert cycle>=previous,'LU product events out of cycle order'
        previous=cycle
        assert 0<=k<c<5 and slot==k%2 and row['upper'] in ('0','1')
        key=tuple(row[n] for n in ('token','generation','column','upper'))
        state=states.setdefault(key,dict(slots={},next_mul=0,next_sub=0,pending=None))
        event=row['event'];mul=event.startswith('mul')
        if event.endswith('issue'):
            op=operations[row['id']]
            assert row['id'] not in bound;bound.add(row['id'])
            assert op['cycle']==row['cycle'] and op['caller']==('1' if mul else '2')
            assert int(op['lanes'])==5-c-int(row['upper'])
            assert op['bank_owner']==row['token'] and op['bank_epoch']==row['generation']
            bank=int(row['bank'])
            assert (27<=bank<27+lu_count) if slot==0 else (27+lu_count+rhs_depth<=bank<27+2*lu_count+rhs_depth)
            grant=key[:2]+(cycle,mul)
            cycle_grants[grant]+=1;assert cycle_grants[grant]<=1,'unbounded per-context LU issue'
            if mul:
                assert k==state['next_mul'] and slot not in state['slots']
                assert int(op['bank_dst'])==bank
                state['slots'][slot]=dict(k=k,id=row['id'],ready=False,bank=bank)
                state['next_mul']+=1
            else:
                assert k==state['next_sub'] and state['pending'] is None
                product=state['slots'].pop(slot)
                assert product['k']==k and product['ready']
                assert bank==product['bank'] and int(op['bank_a'])==bank
                state['pending']=dict(k=k,id=row['id'])
            counts[event]+=1
        else:
            assert event in ('mul-ready','sub-ready')
            pending=state['slots'][slot] if mul else state['pending']
            assert pending['k']==k and returns[pending['id']]==cycle
            if mul:
                assert not pending['ready'];pending['ready']=True
            else:
                state['pending']=None;state['next_sub']+=1
            counts[event]+=1
    # Failure/cancel may drop callbacks after a physically paid return. Every
    # remaining operation must nevertheless be present in the RF return log.
    for key,state in states.items():
        pending=list(state['slots'].values())+([state['pending']] if state['pending'] else [])
        if pending or state['next_sub']!=int(key[2]):
            assert key[:2] in cancelled,'successful LU window did not drain'
        assert all(p['id'] in returns for p in pending),'LU pipeline did not physically drain'
    if enabled:
        expected={i for i,r in operations.items() if r['caller'] in ('1','2') and int(r['bank_dst'])>=0 or r['caller']=='2'}
        # Counts include every dot-product MUL/SUB. Normalization MUL has no
        # local destination and is deliberately outside the product window.
        assert expected-set(independent_upper or ())==bound
    return dict(counts)
