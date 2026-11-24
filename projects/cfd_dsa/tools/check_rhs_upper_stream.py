"""Replay the two RHS chains from actual normalization and FMA RF returns."""
from collections import Counter
import math
import re
from check_rhs_fma import bits,rounded_fma


def replay_rhs_upper_stream(text,config,operations,upper):
    enabled=config.get('--coeff-rhs-upper-stream','0')=='1'
    if not enabled:
        assert not re.search(r'^CFD_RHS_NORM ',text,re.M)
        assert not re.search(r'^CFD_RHS_FMA [^\n]*streaming=1',text,re.M)
        assert not re.search(r'^CFD_RHS_UPPER_STREAM_CAPACITY ',text,re.M)
        return dict(physical_ids=set(),summary={},fma_totals={})
    assert config.get('--coeff-rhs-upper-inverse','0')=='1'
    count=int(config.get('--coeff-rhs-fma-count','0'));assert 4<=count<=16
    mul_latency=int(config.get('--coeff-cluster-mul-lat','3'))
    physical={r['id']:r for p,r in operations if p=='CFD_RF_ISSUE'}
    returned={r['id']:r for p,r in operations if p=='CFD_RF_COMPLETE'}
    factors={}
    for m in re.finditer(r'^CFD_RHS_FACTOR [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
        if r['event']!='clear':factors[r['id']]=r
    for m in re.finditer(r'^CFD_RHS_FACTOR_FANOUT [^\n]*',text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',m[0]))
        if r['event']=='use':
            assert config.get('--coeff-rhs-fma-factor-fanout','0')=='1'
            assert r['id'] not in factors
            factors[r['id']]=r
    allocations={};cancelled=set();released=set();states={};finished=set()
    seen=set();fma_ids=set();norm_ids=set();fma_totals=Counter();totals=Counter()
    reciprocal_uses={};forward_uses={};first_issues=[];lower_values={}
    fma_live=fma_peak=0;previous=-1
    def key(r):return tuple(r[k] for k in ('token','generation','request','cell','rhs'))
    def number(encoded):
        v=float.fromhex(encoded);assert math.isfinite(v),'nonfinite streamed RHS operand'
        return v
    def retire(k):
        s=states[k]
        if any(s[x] is not None for x in ['fw','back','norm']):return
        complete=s['forward']==4 and s['upper']==5 and s['mask']==31
        if complete or k[:2] in cancelled:
            if complete:fma_totals['complete_rhs']+=1
            finished.add(k);del states[k]
    pattern=r'^CFD_(?:RHS_UPPER |RHS_FMA |RHS_NORM )[^\n]*'
    for m in re.finditer(pattern,text,re.M):
        line=m[0];prefix=line.split(' ',1)[0];r=dict(re.findall(r'(\w+)=([^\s]+)',line))
        now=int(r['cycle']);assert now>=previous,'stream events reordered';previous=now
        owner=r['token'],r['generation']
        if prefix=='CFD_RHS_UPPER':
            if r['event']=='allocate':
                assert owner not in allocations
                allocations[owner]=(r['request'],r['cell'],now)
            elif r['event']=='cancel':
                cancelled.add(owner)
                for k in list(states):
                    if k[:2]==owner:retire(k)
            elif r['event']=='release':released.add(owner)
            continue
        if prefix=='CFD_RHS_FMA':
            stream=r.get('streaming','0')=='1'
            assert stream==(owner in upper['owners']), 'cold shared-upper RHS missed its stream ownership'
            if not stream:continue
        k=key(r);assert 0<=int(k[4])<15
        assert owner in allocations and allocations[owner][:2]==k[2:4]
        rf=r['rf'];event=r['event']
        if event=='issue':
            assert owner not in cancelled and owner not in released and k not in finished
            assert rf not in seen and rf in physical and rf in returned
            if k not in states:
                states[k]=dict(values={},mask=0,forward=0,upper=1,fw=None,back=None,norm=None)
                first_issues.append(dict(token=k[0],generation=k[1],request=k[2],cell=k[3],rhs=k[4],cycle=now))
            seen.add(rf)
        assert k in states,'stream return has no live RHS owner'
        s=states[k];op=physical[rf]
        if prefix=='CFD_RHS_NORM':
            row=int(r['k']);assert 0<=row<5
            a,b=number(r['a']),number(r['b'])
            if event=='issue':
                assert row==s['forward'] and s['norm'] is None and s['fw'] is None
                assert not s['mask']&(1<<row)
                assert (op['pool'],op['caller'],int(op['lanes']),int(op['read_words']))==('mul','4',1,2)
                assert int(op['ready'])==now+mul_latency and op['cycle']==r['cycle']
                assert op.get('bank_access','0')==op.get('local_result','0')=='0'
                if row in s['values']:assert bits(a)==bits(s['values'][row]),'norm lost its actual forward producer'
                else:assert row==0
                use=k[:4]+(row,)
                if use in reciprocal_uses:assert bits(b)==bits(number(reciprocal_uses[use]['value']))
                else:reciprocal_uses[use]=dict(generation=k[1],request=k[2],cell=k[3],k=row,cycle=now,value=b.hex())
                s['norm']=(rf,row,a,b);norm_ids.add(rf);totals['norm_issue']+=1
            else:
                assert event in ('complete','drop') and s['norm']==(rf,row,a,b)
                assert returned[rf]['cycle']==r['cycle'],'normalization bypassed actual RF return'
                result=number(r['result']);assert bits(result)==bits(a*b),'normalization did not round once'
                s['norm']=None;totals['norm_'+event]+=1
                if event=='drop':assert owner in cancelled
                else:
                    assert owner not in cancelled
                    s['values'][row]=result;s['mask']|=1<<row
                retire(k)
            continue
        backward,column,first,width=(int(r[x]) for x in ['backward','k','first','width'])
        assert 0<=column<4 and width==4-column
        assert first==(0 if backward else column+1)
        assert int(r.get('upper_inverse','0'))==backward
        operands=tuple(tuple(number(r[v+str(i)]) for i in range(width)) for v in ['a','b','c'])
        a,b,c=operands;chain='back' if backward else 'fw'
        if event=='issue':
            assert s[chain] is None
            assert (op['pool'],op['caller'],int(op['lanes']))==('fma','4',width)
            assert op['cycle']==r['cycle'] and int(op['ready'])==now+5 and int(op['capacity'])==5*count
            assert int(op.get('bank_dst','-1'))==-1 and op.get('local_result','0')=='0'
            if op.get('bank_access','0')=='1':
                assert rf in factors and factors[rf]['event']=='use'
            if rf in factors:
                factor=factors[rf];start=column+1
                assert (factor['token'],factor['generation'])==owner
                assert all(bits(a[i])==bits(number(factor['v'+str(start+i)])) for i in range(width))
            assert all(bits(x)==bits(b[0]) for x in b),'stream changed its scalar broadcast source'
            if backward:
                j=4-column;assert j==s['upper'] and s['forward']>=j
                assert s['mask']&((1<<(j+1))-1)==(1<<(j+1))-1,'upper read before actual normalization return'
                assert bits(b[0])==bits(s['values'][j])
                for i in range(width):assert bits(c[i])==bits(s['values'][i]),'upper accumulator lost its preceding actual return'
            else:
                assert column==s['forward'] and s['mask']&(1<<column) and s['norm'] is None
                assert bits(b[0])==bits(s['values'][column])
                for i in range(width):
                    row=first+i
                    assert not s['mask']&(1<<row),'forward overwrote a normalized upper operand'
                    if row in s['values']:assert bits(c[i])==bits(s['values'][row]),'forward accumulator lost its actual return'
                    use=k[:2]+(row,column)
                    if use in lower_values:assert bits(a[i])==bits(lower_values[use]),'LU lower factor changed across RHS'
                    else:lower_values[use]=a[i]
                use=k[:4]+(column,)
                forward_uses.setdefault(use,dict(generation=k[1],request=k[2],cell=k[3],k=column,cycle=now))
            s[chain]=(rf,operands,column,first,width);fma_ids.add(rf)
            fma_live+=width;fma_peak=max(fma_peak,fma_live);assert fma_live<=5*count
            fma_totals['issue_groups']+=1;fma_totals['scalar_fma']+=width
        else:
            assert event in ('complete','drop') and s[chain]==(rf,operands,column,first,width)
            assert returned[rf]['cycle']==r['cycle'],'stream FMA bypassed actual RF return'
            result=tuple(number(r['r'+str(i)]) for i in range(width))
            assert all(bits(result[i])==bits(rounded_fma(a[i],b[i],c[i])) for i in range(width)), 'stream FMA did not round once'
            s[chain]=None;fma_live-=width;assert fma_live>=0
            fma_totals[event+'_groups']+=1
            if event=='drop':assert owner in cancelled
            else:
                assert owner not in cancelled
                for i in range(width):s['values'][first+i]=result[i]
                s['upper' if backward else 'forward']+=1
            retire(k)
    assert not states and not fma_live,'streamed RHS or actual RF return did not drain'
    assert seen==fma_ids|norm_ids and fma_ids.isdisjoint(norm_ids)
    caps=[dict(re.findall(r'(\w+)=([^\s]+)',m[0])) for m in re.finditer(r'^CFD_RHS_UPPER_STREAM_CAPACITY [^\n]*',text,re.M)]
    assert caps
    for cap in caps:assert tuple(int(cap[x]) for x in ['bytes','rhs_state_bytes','forward_groups','upper_groups','independent_chains'])==(512,80,4,4,2)
    summary=dict(totals,control_bytes=512,rhs_states=len(finished),fma_peak=fma_peak,
                 reciprocal_uses=list(reciprocal_uses.values()),forward_uses=list(forward_uses.values()),first_issues=first_issues)
    return dict(physical_ids=fma_ids,fma_totals=dict(fma_totals),summary=summary)


def check_rhs_stream_schedule(config,trace,summary,divider):
    if config.get('--coeff-rhs-upper-stream','0')!='1':
        assert not summary
        return {}
    from check_lu_reciprocal import value
    allocated={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    ready={};issued={};lower={};init={};solve={}
    for r in trace:
        key=r['requestId'],int(r['k'])
        if r['stage']=='LU_RECIP_READY':ready[key]=int(r['cycle'])
        elif r['stage']=='LU_RECIP_DIV_ISSUE':issued[key]=int(r['cycle'])
        elif r['stage']=='LU_STEP_READY':lower[key]=int(r['cycle'])
        elif r['stage']=='COEFF3_ISSUE':solve[r['requestId']]=int(r['cycle'])
        elif r['stage']=='RHS_INPUT_INIT':
            init[r['requestId'],5*int(r['rhsBatch'])+int(r['rhsColumn'])]=int(r['cycle'])
    produced={int(r['accepted']):r for r in divider if r['event']=='result'}
    for use in summary['reciprocal_uses']:
        assert allocated[use['request']]==(use['generation'],use['cell'])
        key=use['request'],use['k'];result=produced[issued[key]]
        assert ready[key]==int(result['cycle'])<=use['cycle'],'normalization used an unreturned LU reciprocal'
        assert bits(float.fromhex(use['value']))==bits(value(result['result'])), 'stream normalization used another LU reciprocal'
    for use in summary['forward_uses']:
        assert allocated[use['request']]==(use['generation'],use['cell'])
        assert lower[use['request'],use['k']]<=use['cycle'],'stream FMA read an unpublished LU lower column'
    for use in summary['first_issues']:
        assert allocated[use['request']]==(use['generation'],use['cell'])
        assert init.get((use['request'],int(use['rhs'])),solve[use['request']])<=use['cycle'], 'stream RHS ran before actual input initialization'
    return dict(reciprocal_owners=len(summary['reciprocal_uses']),lower_source_rows=len(summary['forward_uses']),
                initialized_rhs=len(summary['first_issues']))
