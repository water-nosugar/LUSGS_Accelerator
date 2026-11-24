"""Independent replay of the registered two-destination common-LU input path."""
from collections import Counter
import math
import re
from trace_text import fields


def replay_rhs_pairs(text,config,rf_events):
    messages=fields(text,'CFD_RHS_PAIR')
    enabled=config.get('--coeff-rhs-pair','0')=='1'
    children={r['id']:r for prefix,r in rf_events
              if prefix=='CFD_RF_ISSUE' and int(r.get('pair_id','0'))}
    if not enabled:
        assert not messages and not children,'disabled RHS pairing emitted operands'
        return dict(loads=Counter(),children={},summary={})
    assert config.get('--coeff-local-products','0')=='1'
    assert config.get('--coeff-rf-broadcast','0')=='1'
    assert int(config.get('--coeff-cluster-mul-count','5'))>=8
    records=[]
    # Preserve same-tick capture -> release -> next selection order exactly.
    # Arithmetic returns are separately conserved by the full RF replay; a
    # freed ingress latch must never release those FU or product-bank credits.
    pattern=r'^(?:CFD_RHS_PAIR |CFD_RF_ISSUE [^\n]*\bpair_id=[1-9])[^\n]*'
    for match in re.finditer(pattern,text,re.M):
        r=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        if match[0].startswith('CFD_RF_ISSUE '):r['event']='child'
        assert r['event'] in ('select','load','issue','release','cancel','child')
        records.append((int(r['cycle']),match.start(),r))
    live={};seen=set();slots={};loads=Counter();ports=Counter();bindings={}
    issued=cancelled=saved=peak=0
    previous=-1
    for cycle,_,r in records:
        assert cycle>=previous,'paired events are not in cycle order'
        previous=cycle
        event=r['event']
        if event=='child':
            identity=r['pair_id'];p=live[identity];base=p['base']
            side=int(r['pair_side'])
            assert p['stage']=='issue' and cycle==p['issue']
            assert side in (0,1) and side not in p['children'],'duplicate paired destination'
            assert r['pool']=='mul' and r['caller']=='4' and int(r['lanes'])==int(base['width'])
            assert r['read_words']=='0' and r.get('broadcast','0')=='0'
            assert r.get('bank_access')=='1' and r['bank_owner']==base['token'] and r['bank_epoch']==base['generation']
            assert r['bank_a']==r['bank_b']=='-1' and int(r['bank_dst'])>=27+int(config.get('--lu5-count','4'))
            assert all(r['bank_dst']!=op['bank_dst'] for op in p['children'].values()),'pair aliases its result destinations'
            p['children'][side]=r
            bindings[r['id']]=dict(pair=identity,side=side,parent=base,load=p['load'],issue_cycle=r['cycle'])
            continue
        identity=r['id'];slot=int(r['slot']);width=int(r['width']);k=int(r['k'])
        assert 0<=slot<2 and 0<=k<=2 and width==4-k
        assert 10<=int(r['rhs0'])<int(r['rhs1'])<15,'critical b/Lbar entered pairing'
        if event=='select':
            assert identity not in seen and slot not in slots,'paired slot reused before both returns'
            seen.add(identity);slots[slot]=identity
            live[identity]=dict(base=r,stage='select',children={})
            peak=max(peak,len(live));assert peak<=2
        else:
            assert identity in live and slots.get(slot)==identity,'stale paired operand owner'
            p=live[identity];base=p['base']
            assert all(r[key]==base[key] for key in ('slot','token','request','generation','cell','k','width','rhs0','rhs1','selected'))
            if event=='load':
                assert p['stage']=='select' and cycle>int(base['cycle']),'pair bypassed registered selection'
                assert int(r['loaded'])==cycle and int(r['read_words'])==width+2
                assert all(math.isfinite(float.fromhex(r[key])) for key in
                    ('lu0','lu1','lu2','lu3','lu4','factor0','factor1'))
                loads[cycle]+=width+2;p['load']=r;p['stage']='load'
            elif event=='issue':
                assert p['stage']=='load' and cycle>int(p['load']['cycle']),'pair bypassed operand route'
                assert all(r[key]==p['load'][key] for key in ('loaded','lu0','lu1','lu2','lu3','lu4','factor0','factor1'))
                p['stage']='issue';p['issue']=cycle;issued+=1
                saved+=width if config.get('--coeff-preprocess-broadcast','0')=='1' else 3*width-2
            elif event=='release':
                assert p['stage']=='issue' and set(p['children'])=={0,1}
                assert cycle==p['issue'],'ingress latch did not release after both MUL input captures'
                del live[identity];del slots[slot]
            elif event=='cancel':
                assert p['stage'] in ('select','load') and not p['children'],'in-flight pair cancelled before drain'
                cancelled+=1;del live[identity];del slots[slot]
        if event in ('select','load','issue'):
            ports[cycle,event]+=1
            assert ports[cycle,event]<=1,'paired selector/ingress/fanout oversubscribed'
    assert not live and not slots,'paired operands did not drain'
    assert set(bindings)==set(children),'paired arithmetic has no common operand producer'
    return dict(loads=loads,children=bindings,summary=dict(selected_pairs=len(seen),
        issued_pairs=issued,cancelled_pairs=cancelled,peak=peak,depth=2,
        operand_read_words=sum(loads.values()),saved_global_read_words=saved,
        reservation_bytes=768,select_latency=1,operand_route_latency=1))


def check_pair_schedule(pairing,trace):
    """Tie both physical consumers to ordinary per-RHS selection and owners."""
    if not pairing['children']:return
    owners={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    selected=Counter((r['requestId'],r['cycle'],r['rhsBatch'],r['rhsColumn'],r['k'])
        for r in trace if r['stage']=='SCHED_SELECT')
    for r in pairing['children'].values():
        p=r['parent'];side=r['side'];rhs=int(p['rhs'+str(side)])
        assert owners[p['request']]==(p['generation'],p['cell'])
        key=p['request'],r['issue_cycle'],str(rhs//5),str(rhs%5),p['k']
        assert selected[key]==1,'paired MUL bypassed ordinary RHS selection'
