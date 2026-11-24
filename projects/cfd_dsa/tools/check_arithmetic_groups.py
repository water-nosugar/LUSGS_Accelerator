"""Join finite logical groups to physical RF fragments cycle by cycle.

Physical RF replay runs first. Only dependency validators consume this logical
view; metrics keep physical issues. Intermediate replay state is bounded by
live groups/FU credits, rather than line size.
"""
from collections import Counter
from dataclasses import replace
from itertools import groupby
import re
import sys


def replay_arithmetic_groups(text, config, physical, rf):
    mode=config.get('--coeff-arithmetic-mode','legacy')
    if mode=='legacy':
        assert not re.search(r'^CFD_ARITH_GROUP ',text,re.M), 'group events while disabled'
        return {},physical,rf
    assert mode in ('shared','fixed','borrow') and rf is not None
    depth=int(config.get('--coeff-arithmetic-depth','12'))
    select=int(config.get('--coeff-arithmetic-select','1'))
    route=int(config.get('--coeff-arithmetic-route','1'))
    borrow_route=int(config.get('--coeff-arithmetic-borrow-route','1'))
    issue_width=int(config.get('--coeff-arithmetic-width','4'))
    age=int(config.get('--coeff-arithmetic-age','32'))
    counts={'mul':int(config.get('--coeff-cluster-mul-count','5')),
            'add':int(config.get('--coeff-cluster-add-count','5'))}
    assert 4<=depth<=32 and select>=1
    pending={};slots={};active_rf={};lane_next=Counter();live=Counter()
    accepted=completed=fragments=crossings=peak=last_group=last_rf=0
    logical=[];completions=Counter();last_cycle=-1
    def events():
        for m in re.finditer(r'^(CFD_ARITH_GROUP|CFD_RF_ISSUE|CFD_RF_COMPLETE) (.*)$',text,re.M):
            r=dict(re.findall(r'(\w+)=([^\s]+)',m[2]))
            yield int(r['cycle']),m[1],r
    for cycle,stream in groupby(events(),key=lambda r:r[0]):
        assert cycle>=last_cycle, 'group/RF events are not in cycle order'
        last_cycle=cycle;actual=[];unbound={};div_reads=admissions=0
        for _,prefix,r in stream:
            identity=int(r['id'])
            if prefix=='CFD_RF_ISSUE':
                assert identity>last_rf, 'duplicate/nonmonotonic RF identity'
                last_rf=identity
                if r['pool']=='div':div_reads+=int(r['read_words'])
                else:unbound[identity]=r
                continue
            if prefix=='CFD_RF_COMPLETE':
                if r.get('pool')=='div':continue
                assert identity in active_rf, 'orphan RF fragment return'
                group,offset,n=active_rf.pop(identity)
                assert group in pending, 'returned fragment lost group ownership'
                g=pending[group];mask=((1<<n)-1)<<offset
                assert not g['returned']&mask, 'duplicate fragment return'
                g['returned']|=mask;g['last_return']=cycle;live[g['pool']]-=n
                assert live[g['pool']]>=0
                continue
            event=r['event']
            if event=='accept':
                assert identity==last_group+1, 'duplicate/nonmonotonic group identity'
                last_group=identity
                assert len(pending)<depth, 'group capacity overflow'
                scan=int(r['scan']);caller=int(r['caller']);width=int(r['width'])
                assert scan==int(caller>=6) and r['pool'] in counts
                assert caller in (1,2,4,5,6,7,8,9)
                assert 1<=width<=(5 if scan else 4)
                assert caller==6 or r['pool']==('mul' if caller in (1,4,8) else 'add')
                assert int(r['ready'])==cycle+select, 'unregistered group selection'
                assert r['slot'] not in slots, 'live scoreboard slot overwritten'
                if not scan:assert sum(not g['scan'] for g in pending.values())<depth-2, 'producer used reserved scan entries'
                admissions+=1;assert admissions<=issue_width, 'group admission bandwidth exceeded'
                slots[r['slot']]=identity
                pending[identity]=dict(id=identity,slot=r['slot'],pool=sys.intern(r['pool']),
                    caller=sys.intern(r['caller']),width=width,scan=bool(scan),broadcast=bool(int(r['broadcast'])),
                    accepted=cycle,ready=cycle+select,issued=0,returned=0,last_return=0,crossed=False)
                accepted+=1;peak=max(peak,len(pending))
            elif event=='issue':
                assert identity in pending, 'orphan physical fragment'
                rf_id=int(r['rf'])
                assert rf_id in unbound, 'orphan/duplicate fragment'
                op=unbound.pop(rf_id);g=pending[identity]
                assert op['pool']==g['pool'] and op['caller']==g['caller']
                assert int(op['lanes'])==int(r['width'])
                actual.append((r,rf_id))
            elif event=='complete':
                assert identity in pending, 'group completion without ownership'
                g=pending[identity]
                assert g['issued']==g['width'] and g['returned']==(1<<g['width'])-1, 'partial group publication'
                assert cycle==g['last_return']+route+(borrow_route if g['crossed'] else 0), 'premature/late group publication'
                assert bool(int(r['crossed']))==g['crossed']
                assert g['slot']==r['slot'] and slots.pop(r['slot'])==identity
                logical.append(dict(cycle=str(g['accepted']),pool=g['pool'],lanes=str(g['width']),
                                    caller=g['caller'],complete_cycle=str(cycle)))
                completions[g['pool'],g['caller'],g['accepted'],cycle,g['width']]+=1
                del pending[identity];completed+=1
            else:raise AssertionError('unknown arithmetic group event')
        assert not unbound, 'physical arithmetic bypassed group ownership'
        reads=int(config['--coeff-rf-read-words'])-div_reads
        candidates=[g for g in pending.values() if g['ready']<=cycle and g['issued']<g['width']]
        def priority(g):
            if mode=='borrow':
                old=cycle-g['accepted']>=age
                return (0 if old else 1,0 if old or g['scan'] else 1,g['id'])
            return (g['id'],)
        expected=[]
        for g in sorted(candidates,key=priority):
            if len(expected)>=issue_width:break
            pool=g['pool'];count=counts[pool]
            if mode=='fixed':order=list(range(5,count)) if g['scan'] else list(range(5))
            elif mode=='borrow' and g['scan']:order=list(range(5,count))+list(range(5))
            else:order=list(range(count))
            lanes=[i for i in order if lane_next[pool,i]<=cycle][:g['width']-g['issued']]
            lanes=lanes[:max(0,rf.summary['capacities'][pool]-live[pool])]
            n=len(lanes)
            def read_cost(n):
                return n+1 if config.get('--coeff-rf-broadcast','0')=='1' and g['broadcast'] and n>1 else 2*n
            while n and read_cost(n)>reads:n-=1
            if not n:continue
            lanes=lanes[:n];mask=sum(1<<i for i in lanes)
            g['crossed'] |= mode=='borrow' and any((i>=5)!=g['scan'] for i in lanes)
            expected.append((g['id'],g['issued'],n,mask,int(g['crossed'])))
            g['issued']+=n;reads-=read_cost(n);live[pool]+=n
            for lane in lanes:lane_next[pool,lane]=cycle+1
        observed=[(int(r['id']),int(r['offset']),int(r['width']),int(r['mask']),int(r['crossed'])) for r,_ in actual]
        assert observed==expected, ('physical group selection/ownership mismatch',cycle,observed,expected)
        for r,rf_id in actual:
            assert rf_id not in active_rf
            active_rf[rf_id]=(int(r['id']),int(r['offset']),int(r['width']))
            fragments+=1;crossings+=bool(int(r['crossed']))
    assert not pending and not slots and not active_rf and not any(live.values()), 'arithmetic groups leaked'
    for r in rf.actual_cluster:
        if r['pool']=='div':
            logical.append(r)
            completions[r['pool'],r['caller'],int(r['cycle']),int(r['complete_cycle']),int(r['lanes'])]+=1
    logical.sort(key=lambda r:int(r['cycle']))
    summary=dict(mode=mode,groups=completed,fragments=fragments,depth=depth,peak=peak,
                 crossed_fragments=crossings,logical_view='accepted operations joined to all physical RF returns and registered route')
    assert completed==accepted
    return summary,logical,replace(rf,completions=completions,actual_cluster=logical)
