"""Bind early next-pivot inputs to the actual diagonal-row RF return."""
from collections import Counter
import math,re
from check_rhs_fma import bits,rounded_fma

def rows(text,prefix):
    return [dict(re.findall(r'(\w+)=([^\s]+)',m[0])) for m in re.finditer(r'^'+prefix+r' [^\n]*',text,re.M)]

def replay_lu_pivot_prefetch(text,config,events):
    early=rows(text,'CFD_LU_PIVOT_PREFETCH');diagonals=rows(text,'CFD_LU_PIVOT_ROW');caps=rows(text,'CFD_LU_PIVOT_PREFETCH_CAPACITY')
    enabled=config.get('--coeff-lu-pivot-prefetch','0')=='1'
    if not enabled:
        assert not early and not diagonals and not caps,'disabled LU pivot prefetch emitted work'
        return {}
    assert config.get('--coeff-lu-rank1','0')==config.get('--coeff-lu-reciprocal-share','0')=='1'
    physical={r['id']:r for p,r in events if p=='CFD_RF_ISSUE'}
    returned={r['id']:r for p,r in events if p=='CFD_RF_COMPLETE'}
    rank=rows(text,'CFD_LU_RANK1');rank_by_id={(r['event'],r['id']):r for r in rank}
    owner=lambda r:tuple(r[k] for k in ['token','generation','request','cell'])
    by_source={};counts=Counter();lead=[]
    for r in diagonals:
        rf=r['rf'];c=int(r['column']);assert rf not in by_source and 1<=c<=4
        op=physical[rf];ret=returned[rf];now=int(r['cycle'])
        assert int(ret['cycle'])==now and int(op['lanes'])==5-c,'pivot row bypassed its actual RF return'
        fused=r['fused']=='1';assert (op['pool'],op['caller'])==(('lu_fma','15') if fused else ('add','2'))
        a,b,acc,result=[float.fromhex(r[k]) for k in ['a','b','c','result']]
        expected=rounded_fma(a,b,acc) if fused else a-b
        assert bits(result)==bits(expected),'pivot diagonal is not its actual rounded row update'
        row=rank_by_id['fma-ready' if fused else 'sub-ready',rf]
        assert owner(row)==owner(r) and int(row['row'])==c and int(row['column'])==c-1 and row['cycle']==r['cycle']
        by_source[rf]=r;counts['diagonal_rows']+=1
    assert set(by_source)=={r['id'] for r in rank if r['event'] in ('sub-ready','fma-ready') and int(r['row'])==int(r['column'])+1},'missing actual diagonal row witness'
    seen=set()
    for r in early:
        rf=r['rf'];src=by_source[r['source']];c=int(r['column']);now=int(r['cycle'])
        assert rf not in seen;seen.add(rf)
        assert owner(src)==owner(r) and src['column']==r['column']
        assert int(src['cycle'])<=now,'prefetch read an unreturned diagonal'
        assert bits(float.fromhex(src['result']))==bits(float.fromhex(r['diagonal'])),'prefetch read another diagonal'
        assert math.isfinite(float.fromhex(r['diagonal'])) and float.fromhex(r['diagonal'])!=0
        op=physical[rf];assert (op['pool'],op['caller'],op['lanes'],op['read_words'])==('div','0','1','2')
        assert int(op['cycle'])==now and int(returned[rf]['cycle'])>=now+int(config.get('--coeff-cluster-div-lat','12'))
        msg=rank_by_id['pivot-prefetch',rf];assert owner(msg)==owner(r) and msg['column']==r['column'] and msg['cycle']==r['cycle']
        if ('pivot-adopt',rf) in rank_by_id:
            adopt=rank_by_id['pivot-adopt',rf];assert owner(adopt)==owner(r) and int(adopt['cycle'])>=now
            lead.append(int(adopt['cycle'])-now)
        counts['issues']+=1
    assert seen=={r['id'] for r in rank if r['event']=='pivot-prefetch'}
    assert caps
    for cap in caps:
        assert int(cap['contexts'])==int(config.get('--lu5-count','4'))
        assert tuple(int(cap[k]) for k in ['per_context','context_reserved','control_bytes','selector_width','extra_units'])==(1,512,128,1,0)
        assert int(cap['state_bytes'])<=512
    return dict(counts,control_bytes=128,per_context=1,extra_arithmetic_units=0,adoptions=len(lead),admission_to_adoption_cycles=sum(lead),max_admission_to_adoption=max(lead,default=0))

def check_pivot_prefetch_schedule(text,config,trace,divider):
    early=rows(text,'CFD_LU_PIVOT_PREFETCH')
    if config.get('--coeff-lu-pivot-prefetch','0')!='1':assert not early;return {}
    from check_lu_reciprocal import value
    allocated={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    issued={(r['requestId'],r['k']):r['cycle'] for r in trace if r['stage']=='LU_RECIP_DIV_ISSUE'}
    physical={r['accepted']:r for r in divider if r['event']=='result'}
    for r in early:
        assert allocated[r['request']]==(r['generation'],r['cell'])
        assert issued[r['request'],r['column']]==r['cycle']
        d=physical[r['cycle']];diagonal=float.fromhex(r['diagonal'])
        assert bits(value(d['a']))==bits(1.0) and bits(value(d['b']))==bits(diagonal),'prefetch divider consumed another diagonal'
        assert bits(value(d['result']))==bits(1.0/diagonal),'prefetch reciprocal numeric mismatch'
    return dict(paid_divider_sources=len(early))
