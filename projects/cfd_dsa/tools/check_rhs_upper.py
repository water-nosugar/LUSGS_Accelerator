"""Replay compact shared-upper producers, RF costs, source use and ownership."""
from collections import Counter
import math
import re
from check_rhs_fma import bits,rounded_fma

COORDS=((0,2),(0,3),(0,4),(1,3),(1,4),(2,4))
INDEX={p:i for i,p in enumerate(COORDS)}
def fields(line):return dict(re.findall(r'(\w+)=([^\s]+)',line))

def replay_rhs_upper(text,config,operations):
    enabled=config.get('--coeff-rhs-upper-inverse','0')=='1'
    physical={r['id']:r for p,r in operations if p=='CFD_RF_ISSUE' and r.get('caller')=='14'}
    if not enabled:
        assert not physical and not re.search(r'^CFD_RHS_UPPER(?: |_)',text,re.M),'disabled shared upper emitted state/work'
        return dict(owners=set(),physical_ids=set(),summary={})
    depth=int(config.get('--coeff-rhs-upper-slots','16'));fmas=int(config.get('--coeff-rhs-fma-count','0'))
    assert 4<=depth<=32 and fmas>=4
    returns={r['id']:r for p,r in operations if p=='CFD_RF_COMPLETE' and r['id'] in physical}
    slots={};owners=set();history={};allocations=set();active={};seen=set();apps={}
    totals=Counter();sources={};peak=0;previous=-1;grants=Counter()
    def raw(state,i,j,v,cycle):
        old=state['u'].setdefault((i,j),v)
        assert bits(old)==bits(v),'LU operand changed within a shared upper owner'
        key=state['identity'][1:]+(i,)
        sources[key]=min(sources.get(key,cycle),cycle)
    for m in re.finditer(r'^(?:CFD_RHS_UPPER |CFD_RHS_FMA )[^\n]*',text,re.M):
        line=m[0];r=fields(line);cycle=int(r['cycle'])
        assert cycle>=previous,'upper/RHS events reordered';previous=cycle
        identity=tuple(r[k] for k in ('token','generation','request','cell'))
        owner=identity[:2]
        if line.startswith('CFD_RHS_FMA '):
            if owner not in owners:continue
            rf=r['rf']
            if r['event']=='issue':
                slot=history[owner];assert slot in slots and not slots[slot]['cancelled']
                state=slots[slot];assert state['identity']==identity and rf not in apps
                apps[rf]=owner
                if r['backward']=='1':
                    assert r.get('upper_inverse')=='1'
                    j=4-int(r['k']);assert int(r['first'])==0 and int(r['width'])==j
                    for i in range(j):
                        value=float.fromhex(r['a'+str(i)])
                        if j==i+1:raw(state,i,j,value,cycle)
                        else:
                            entry=INDEX[i,j]
                            assert state['done']&(1<<entry) and state['ready'][entry]<=cycle,'upper transform consumed before actual RF return'
                            assert bits(value)==bits(state['value'][entry]),'RHS used another upper transform'
                else:assert r.get('upper_inverse','0')=='0'
            else:
                assert r['event'] in ('complete','drop') and apps.pop(rf)==owner
            continue
        event=r['event'];slot=int(r['slot']);allocation=r['id']
        assert 0<=slot<depth
        if event=='allocate':
            assert slot not in slots and owner not in owners and allocation not in allocations
            assert int(allocation)==len(allocations)+1
            owners.add(owner);allocations.add(allocation);history[owner]=slot
            slots[slot]=dict(identity=identity,id=allocation,u={},value={},ready={},
                next=[j-1 for i,j in COORDS],done=0,pending=0,cancelled=False,counts=Counter())
            peak=max(peak,len(slots));totals['owners']+=1
        else:
            state=slots[slot]
            assert state['identity']==identity and state['id']==allocation,'shared upper slot reused before its old RF return'
            if event=='cancel':
                assert not state['cancelled'];state['cancelled']=True;totals['cancels']+=1
            elif event=='release':
                assert not state['pending'] and not any(v[0]==allocation for v in active.values())
                if not state['cancelled']:
                    assert state['done']==63 and state['counts']['issue']==state['counts']['complete']==10
                    assert owner not in apps.values(),'shared upper owner released before its RHS returned'
                totals['releases']+=1
            else:
                assert event in ('issue','complete','drop')
                e=int(r['entry']);i,j=COORDS[e];k=int(r['k']);rf=r['rf']
                assert (int(r['i']),int(r['j']))==(i,j) and i<k<j
                a,b,c=(float.fromhex(r[n]) for n in ('a','b','c'))
                assert all(math.isfinite(v) for v in (a,b,c))
                if event=='issue':
                    assert not state['cancelled'] and not ((state['done']|state['pending'])&(1<<e))
                    assert k==state['next'][e] and rf not in seen
                    op=physical[rf]
                    assert op['pool']=='fma' and op['caller']=='14' and op['cycle']==r['cycle'] and int(op['lanes'])==1
                    assert int(op['read_words'])==3 and int(op['ready'])==cycle+5 and int(op['capacity'])==5*fmas
                    assert op.get('broadcast','0')==op.get('bank_access','0')==op.get('local_result','0')=='0'
                    assert int(op.get('bank_dst','-1'))==-1 and int(op.get('local_reads','0'))==0
                    raw(state,i,k,a,cycle)
                    if k+1==j:raw(state,k,j,b,cycle)
                    else:
                        dep=INDEX[k,j]
                        assert state['done']&(1<<dep) and state['ready'][dep]<=cycle
                        assert bits(b)==bits(state['value'][dep]),'upper dependency not the returned producer'
                    if k==j-1:raw(state,i,j,c,cycle)
                    else:assert bits(c)==bits(state['value'][e]),'upper accumulator lost its preceding FMA'
                    state['pending']|=1<<e;seen.add(rf)
                    active[rf]=(allocation,slot,e,k,a,b,c,cycle)
                    grants[cycle]+=1;assert grants[cycle]<=6,'upper selector width exceeded'
                else:
                    old=active.pop(rf)
                    assert old[:7]==(allocation,slot,e,k,a,b,c) and state['pending']&(1<<e)
                    assert returns[rf]['cycle']==r['cycle'] and cycle>=old[7]+5
                    result=float.fromhex(r['result'])
                    assert bits(result)==bits(rounded_fma(a,b,c)),'upper update did not round once'
                    state['pending']&=~(1<<e)
                    if event=='drop':assert state['cancelled']
                    else:
                        assert not state['cancelled'];state['value'][e]=result
                        if k==i+1:state['done']|=1<<e;state['ready'][e]=cycle
                        else:state['next'][e]-=1
                state['counts'][event]+=1;totals[event]+=1
        state=slots[slot]
        assert int(r['done'])==state['done'] and int(r['pending'])==state['pending'] and int(r['cancelled'])==int(state['cancelled'])
        if event=='release':del slots[slot]
    assert not slots and not active and not apps,'shared upper state/consumers did not drain'
    assert seen==set(physical)==set(returns),'upper precompute has no complete RF/arithmetic witness'
    caps=[fields(m[0]) for m in re.finditer(r'^CFD_RHS_UPPER_CAPACITY [^\n]*',text,re.M)]
    assert caps
    for r in caps:
        assert (int(r['slots']),int(r['bytes']),int(r['live']),int(r['issue_width']))==(depth,256*depth+256,0,6)
    last=caps[-1]
    assert (int(last['peak']),int(last['issued']),int(last['completed']),int(last['dropped']))==(peak,totals['issue'],totals['complete'],totals['drop'])
    source_uses=[dict(generation=key[0],request=key[1],cell=key[2],row=key[3],cycle=t) for key,t in sources.items()]
    return dict(owners=owners,physical_ids=seen,summary=dict(totals,peak=peak,slots=depth,bytes=256*depth+256,source_uses=source_uses))

def check_rhs_upper_schedule(config,trace,summary):
    if config.get('--coeff-rhs-upper-inverse','0')!='1':
        assert not summary;return {}
    ready={(r['generation'],r['requestId'],r['cell'],int(r['k'])):int(r['cycle']) for r in trace if r['stage']=='LU_COLUMN_DONE'}
    for r in summary['source_uses']:
        key=r['generation'],r['request'],r['cell'],r['row']
        assert key in ready and ready[key]<=r['cycle'],'shared upper consumed a LU row before its actual publication'
    return dict(paid_source_rows=len(summary['source_uses']))
