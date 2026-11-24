"""Independent LU FMA operands, exact rounding, RF returns and factor residual."""
from collections import Counter
from decimal import Decimal, localcontext
import math
import re
from check_rhs_fma import bits, rounded_fma


def replay_lu_fma(text, config, operations):
    count=int(config.get('--coeff-lu-fma-count','0'))
    expression=r'^CFD_(?:LU_FMA(?:_MATRIX|_CAPACITY)?|LU_SCALE|LU_RANK1) [^\n]*'
    messages=[]
    for match in re.finditer(expression,text,re.M):
        prefix=match[0].split(' ',1)[0]
        messages.append((prefix,dict(re.findall(r'(\w+)=([^\s]+)',match[0]))))
    physical={r['id']:r for p,r in operations if p=='CFD_RF_ISSUE' and r['pool']=='lu_fma'}
    if not count:
        assert not physical and not any(p.startswith('CFD_LU_FMA') for p,_ in messages), 'disabled LU FMA emitted work'
        return {}
    assert 4<=count<=16 and config.get('--coeff-lu-rank1','0')=='1'
    broadcast=config.get('--coeff-preprocess-broadcast','0')=='1' and config.get('--coeff-rf-broadcast','0')=='1'
    returns={r['id']:r for p,r in operations if p=='CFD_RF_COMPLETE' and r['id'] in physical}
    scale_returns={}
    for prefix,r in messages:
        if prefix=='CFD_LU_RANK1' and r['event']=='scale-ready':
            scale_returns[r['generation'],r['request'],r['cell'],int(r['column'])]=int(r['cycle'])
    states={};by_request={};bindings={};pending={};seen=set();capacities=[]
    grants=Counter();counts=Counter();max_residual=0.0;previous=-1
    def key(r):return tuple(r[k] for k in ('token','generation','request','cell'))
    def finite(encoded):
        value=float.fromhex(encoded);assert math.isfinite(value), 'nonfinite LU FMA operand'
        return value
    for prefix,r in messages:
        if prefix=='CFD_LU_FMA_CAPACITY':
            capacities.append(r);continue
        now=int(r['cycle']);assert now>=previous,'LU FMA witness reordered';previous=now
        if prefix=='CFD_LU_RANK1':
            if r['event']=='bind':
                owner=key(r);assert owner not in bindings;bindings[owner]=now
            elif r['event']=='cancel':
                states[key(r)]['cancelled']=True
            continue
        if prefix=='CFD_LU_FMA_MATRIX':
            owner=key(r);values=[finite(r['v'+str(i)]) for i in range(25)]
            if r['event']=='input':
                assert owner not in states and bindings[owner]==now, 'LU input matrix lacks a paid rank1 bind'
                states[owner]=dict(matrix=values[:],original=values[:],cancelled=False,
                                   factor=False,issued=set(),completed=set(),scaled=set())
                assert owner[1:] not in by_request
                by_request[owner[1:]]=owner
            else:
                assert r['event']=='factor';s=states[owner]
                assert not s['factor'] and not s['cancelled']
                assert not any(v['owner']==owner for v in pending.values()), 'LU factor published before actual FMA return'
                assert s['completed']=={(c,row) for c in range(4) for row in range(c+1,5)}
                assert s['scaled']=={(c,j) for c in range(4) for j in range(c+1,5)}
                assert all(bits(x)==bits(y) for x,y in zip(values,s['matrix'])), 'LU factor differs from independently replayed arithmetic'
                with localcontext() as ctx:
                    ctx.prec=160
                    lu=list(map(Decimal.from_float,values));original=list(map(Decimal.from_float,s['original']))
                    for i in range(5):
                        for j in range(5):
                            terms=[lu[5*i+k]*(Decimal(1) if k==j else lu[5*k+j])
                                   for k in range(min(i,j)+1)]
                            residual=abs(sum(terms)-original[5*i+j])/(1+abs(original[5*i+j])+sum(map(abs,terms)))
                            max_residual=max(max_residual,float(residual))
                            assert residual<Decimal('1e-12'), 'LU factor fails independent L*U reconstruction'
                s['factor']=True;counts['factor_matrices']+=1
            continue
        if prefix=='CFD_LU_SCALE':
            owner=by_request[r['generation'],r['request'],r['cell']];s=states[owner]
            c,j=int(r['k']),int(r['column']);m=s['matrix']
            assert not s['cancelled'] and not s['factor'] and 0<=c<j<5
            assert scale_returns[owner[1],owner[2],owner[3],c]==now
            assert (c,j) not in s['scaled'], 'duplicated LU normalization'
            numerator,reciprocal,result=(finite(r[v]) for v in ('numerator','reciprocal','result'))
            assert bits(numerator)==bits(m[5*c+j]), 'LU normalization lost its preceding column'
            assert bits(reciprocal)==bits(1.0/m[5*c+c]), 'LU normalization used the wrong diagonal reciprocal'
            assert bits(result)==bits(numerator*reciprocal), 'LU normalization must round separately'
            m[5*c+j]=result;s['scaled'].add((c,j));counts['normalization_muls']+=1
            continue
        assert prefix=='CFD_LU_FMA'
        owner=key(r);s=states[owner];rf=r['rf']
        c,row,width=(int(r[v]) for v in ('column','row','width'))
        assert 0<=c<row<5 and width==4-c
        inputs=tuple(tuple(finite(r[v+str(i)]) for i in range(width)) for v in ('a','b','c'))
        a,b,acc=inputs
        if r['event']=='issue':
            assert not s['cancelled'] and not s['factor'] and rf not in seen
            assert (c,row) not in s['issued']
            assert all((c,j) in s['scaled'] for j in range(c+1,5)), 'LU FMA used an unnormalized U row'
            if c:assert all((c-1,i) in s['completed'] for i in range(c,5)), 'LU FMA crossed an unreturned trailing column'
            op=physical[rf]
            assert op['caller']=='15' and op['cycle']==r['cycle'] and int(op['lanes'])==width
            assert int(op['ready'])==now+5 and int(op['capacity'])==5*count
            assert op.get('bank_access','0')=='0' and op.get('local_result','0')=='0'
            assert all(int(op.get('bank_'+v,'-1'))==-1 for v in ('a','b','dst'))
            shared=broadcast and width>1
            assert int(op.get('broadcast','0'))==int(shared)
            assert int(op['read_words'])==(2*width+1 if shared else 3*width), 'LU FMA did not pay its three input RF reads'
            grants[owner,now]+=1;assert grants[owner,now]<=1, 'LU FMA exceeded one row admission per context/tick'
            m=s['matrix']
            for lane in range(width):
                j=c+1+lane
                assert all(bits(x)==bits(y) for x,y in [(a[lane],m[5*row+c]),(b[lane],m[5*c+j]),(acc[lane],m[5*row+j])]), 'LU FMA operand bypassed its preceding actual return'
            pending[rf]=dict(owner=owner,inputs=inputs,column=c,row=row,width=width)
            seen.add(rf);s['issued'].add((c,row));counts['issue_groups']+=1;counts['scalar_fma']+=width
        else:
            assert r['event'] in ('complete','drop') and rf in pending
            issued=pending.pop(rf)
            assert (issued['owner'],issued['column'],issued['row'],issued['width'])==(owner,c,row,width)
            assert all(bits(x)==bits(y) for old,new in zip(issued['inputs'],inputs) for x,y in zip(old,new)), 'LU FMA operands changed while pending'
            assert int(returns[rf]['cycle'])==now and int(returns[rf]['lanes'])==width, 'LU FMA published before actual RF return'
            result=[finite(r['r'+str(i)]) for i in range(width)]
            assert all(bits(result[i])==bits(rounded_fma(a[i],b[i],acc[i])) for i in range(width)), 'LU FMA must round once'
            if r['event']=='drop':
                assert s['cancelled'], 'LU FMA dropped a live owner'
                counts['drop_groups']+=1
            else:
                assert not s['cancelled'] and (c,row) not in s['completed']
                for lane,value in enumerate(result):s['matrix'][5*row+c+1+lane]=value
                s['completed'].add((c,row));counts['complete_groups']+=1
    assert seen==set(physical)==set(returns) and not pending, 'LU FMA operations lack complete issue/return witnesses'
    assert set(states)==set(bindings) and all(s['factor'] or s['cancelled'] for s in states.values())
    live=peak=0
    for prefix,r in operations:
        if r.get('pool')!='lu_fma':continue
        if prefix=='CFD_RF_ISSUE':live+=int(r['lanes']);peak=max(peak,live)
        elif prefix=='CFD_RF_COMPLETE':live-=int(r['lanes'])
        else:assert prefix=='CFD_RF_WRITE','LU FMA used an unexpected release path'
        assert 0<=live<=5*count, 'LU FMA finite return capacity exceeded'
    assert not live and capacities,'missing LU FMA drain/capacity proof'
    for r in capacities:
        assert tuple(int(r[v]) for v in ('count','lat','ii','slots','bytes','live','issue_width_per_context'))==(count,5,1,5*count,192*5*count+256,0,1)
        assert 0<=int(r['peak'])<=peak
    assert int(capacities[-1]['peak'])==peak,'LU FMA reported peak differs from physical RF events'
    return dict(counts,owner_matrices=len(states),capacity=5*count,peak_scalar_slots=peak,
                bytes=192*5*count+256,max_scaled_factor_residual=max_residual,
                coefficient_storage='ordinary paid global RF; no U row cache')
