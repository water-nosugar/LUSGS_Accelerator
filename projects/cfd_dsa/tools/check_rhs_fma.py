"""Independent three-input FMA, dependency and real RF-return replay."""
from collections import Counter
from decimal import Decimal, localcontext
import math
import re
import struct


def bits(value):
    return struct.pack('>d',value)


def rounded_fma(a,b,c):
    # All binary64 products and exact sums fit within this precision, including
    # the largest exponent spread and subnormal products. Convert only once.
    with localcontext() as ctx:
        ctx.prec=4096
        # Negate the binary64 operand before conversion: Decimal unary minus
        # canonicalizes a zero sign, whereas FP64 FMA must retain -0 + -0.
        return float(Decimal.from_float(-a)*Decimal.from_float(b)+Decimal.from_float(c))


def replay_rhs_fma(text,config,operations,upper=None,stream=None):
    stream=stream or dict(physical_ids=set(),fma_totals={},summary={})
    upper=upper or dict(owners=set(),physical_ids=set())
    count=int(config.get('--coeff-rhs-fma-count','0'))
    if not count:
        assert not re.search(r'^CFD_RHS_FMA(?: |_)',text,re.M), 'disabled FMA emitted work'
        assert not any(r.get('pool')=='fma' for _,r in operations)
        return {}
    assert 4<=count<=16
    all_fma={r['id']:r for p,r in operations if p=='CFD_RF_ISSUE' and r['pool']=='fma'}
    issues={identity:r for identity,r in all_fma.items() if r['caller']=='4' and identity not in stream['physical_ids']}
    assert set(all_fma)==set(issues)|upper['physical_ids']|stream['physical_ids'],'unowned FMA issue'
    returns={r['id']:r for p,r in operations if p=='CFD_RF_COMPLETE' and r['id'] in issues}
    factors={}
    for m in re.finditer(r'^CFD_RHS_FACTOR [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
        if r['event']!='clear' and r['id'] in issues:
            factors[r['id']]=r
    for m in re.finditer(r'^CFD_RHS_FACTOR_FANOUT [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
        if r['event']=='use':
            assert config.get('--coeff-rhs-fma-factor-fanout','0')=='1'
            assert r['id'] not in factors
            factors[r['id']]=r
    live={};seen=set();states={};totals=Counter();peak=0;previous=-1
    order=[(0,k,k+1,4-k) for k in range(4)]+[(1,k,j,1) for k in range(3,-1,-1) for j in range(k+1,5)]
    upper_order=order[:4]+[(1,k,0,4-k) for k in range(3,-1,-1)]
    for m in re.finditer(r'^CFD_RHS_FMA [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
        if r.get('streaming','0')=='1':continue
        cycle=int(r['cycle']);rf=r['rf'];width=int(r['width'])
        assert cycle>=previous,'FMA events reordered';previous=cycle
        key=tuple(r[k] for k in ('token','generation','request','cell','rhs'))
        transformed=key[:2] in upper['owners']
        sequence=upper_order if transformed else order
        state=states.setdefault(key,dict(step=0,values={},pending=None,dropped=False,expected_steps=len(sequence)))
        topology=tuple(int(r[k]) for k in ('backward','k','first','width'))
        inputs=tuple(tuple(float.fromhex(r[v+str(i)]) for i in range(width)) for v in ('a','b','c'))
        a,b,c=inputs
        assert all(math.isfinite(x) for values in inputs for x in values),'nonfinite FMA input'
        if r['event']=='issue':
            assert rf not in seen and state['pending'] is None and not state['dropped']
            assert state['step']<len(sequence) and topology==sequence[state['step']],'FMA solve order changed'
            assert int(r.get('upper_inverse','0'))==int(transformed and topology[0]==1)
            op=issues[rf]
            assert op['cycle']==r['cycle'] and op['caller']=='4' and int(op['lanes'])==width
            assert int(op['ready'])==cycle+5 and int(op['capacity'])==5*count
            assert int(op.get('bank_dst','-1'))==-1 and op.get('local_result','0')=='0'
            if op.get('bank_access','0')=='1':
                assert rf in factors and factors[rf]['event']=='use'
            if rf in factors:
                f=factors[rf]
                assert (topology[0]==0 or transformed) and f['token']==r['token'] and f['generation']==r['generation']
                assert all(bits(a[i])==bits(float.fromhex(f['v'+str((int(r['k'])+1 if transformed and topology[0] else topology[2])+i)])) for i in range(width))
            backward,k,first,_=topology
            values=state['values']
            if not backward:
                assert all(bits(x)==bits(b[0]) for x in b),'FMA scalar broadcast changed input'
                values[k]=b[0] # Paid normalization is checked by the solve/output reference.
                for i in range(width):
                    row=first+i
                    if row in values:assert bits(c[i])==bits(values[row]),'forward FMA accumulator lost its producer'
            elif transformed:
                j=4-k
                assert all(bits(x)==bits(b[0]) for x in b),'upper column scalar changed'
                if j==4:values[4]=b[0]
                assert bits(b[0])==bits(values[j]),'upper column overwrote its broadcast source'
                for i in range(width):assert bits(c[i])==bits(values[i]),'upper apply accumulator lost its producer'
            else:
                assert bits(c[0])==bits(values[k]),'backward FMA accumulator lost its producer'
                # The last diagonal normalization occurs after forward k=3.
                # Its result is first captured here, before any backward update.
                if state['step']==4:values[4]=b[0]
                assert bits(b[0])==bits(values[first]),'backward solved operand lost its producer'
            state['pending']=rf;live[rf]=(key,inputs,topology,cycle);seen.add(rf)
            totals['issue_groups']+=1;totals['scalar_fma']+=width
            peak=max(peak,sum(v[2][3] for v in live.values()))
            assert peak<=5*count,'FMA pending-result capacity exceeded'
        else:
            assert r['event'] in ('complete','drop') and rf in live
            oldkey,oldinputs,oldtopology,issued=live.pop(rf)
            assert oldkey==key and oldinputs==inputs and oldtopology==topology
            assert returns[rf]['cycle']==r['cycle'] and cycle>=issued+5,'FMA published before actual RF return'
            assert state['pending']==rf;state['pending']=None
            result=tuple(float.fromhex(r['r'+str(i)]) for i in range(width))
            for i in range(width):
                assert bits(result[i])==bits(rounded_fma(a[i],b[i],c[i])),'FMA did not round once'
            if r['event']=='drop':state['dropped']=True
            else:
                backward,k,first,_=topology
                for i in range(width):state['values'][k if backward and not transformed else first+i]=result[i]
                state['step']+=1
            totals[r['event']+'_groups']+=1
    assert seen==set(issues)==set(returns),'physical FMA operations lack a complete arithmetic witness'
    assert not live and all(s['pending'] is None for s in states.values()),'FMA did not drain'
    # The hardware pool also serves caller14 upper precompute operations.
    # Conserve its full RF lifetime independently of the RHS dataflow subset.
    pool_live=pool_peak=rhs_live=rhs_peak=0;pool_active=set()
    for prefix,row in operations:
        if row.get('pool')!='fma':continue
        rf=row['id']
        if prefix=='CFD_RF_ISSUE':
            assert rf not in pool_active;pool_active.add(rf)
            pool_live+=int(row['lanes']);pool_peak=max(pool_peak,pool_live)
            assert pool_live<=5*count,'shared FMA pool capacity exceeded'
            if row['caller']=='4':rhs_live+=int(row['lanes']);rhs_peak=max(rhs_peak,rhs_live)
        elif prefix=='CFD_RF_COMPLETE':
            assert rf in pool_active;pool_active.remove(rf)
            pool_live-=int(all_fma[rf]['lanes']);assert pool_live>=0
            if all_fma[rf]['caller']=='4':rhs_live-=int(all_fma[rf]['lanes']);assert rhs_live>=0
    assert not pool_active and pool_live==rhs_live==0
    caps=[]
    for m in re.finditer(r'^CFD_RHS_FMA_CAPACITY [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]));caps.append(r)
        assert tuple(int(r[k]) for k in ('count','lat','ii','slots','bytes','live'))==(count,5,1,5*count,192*5*count+256,0)
    assert caps and int(caps[-1]['peak'])==pool_peak
    totals.update({k:v for k,v in stream['fma_totals'].items() if k!='complete_rhs'})
    return dict(totals,peak_scalar_slots=pool_peak,rhs_peak_scalar_slots=rhs_peak,capacity=5*count,bytes=192*5*count+256,
                complete_rhs=sum(s['step']==s['expected_steps'] for s in states.values())+stream['fma_totals'].get('complete_rhs',0),
                latency_cycles=5,initiation_interval_cycles=1)
