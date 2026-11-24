"""Independent row DAG, FP64 order, physical issue and forwarding proof."""
from collections import Counter
import math
from trace_text import fields, iter_fields


def advance_lane_epoch(previous, cell, epoch, begin, end, context, inverse):
    """One cell per epoch, or base+correction phases of inverse context zero."""
    assert 0 <= context < 3 and epoch >= 0 and end >= begin
    phases = 1
    if previous is not None:
        previous_cell, previous_end, previous_epoch, previous_phases = previous
        assert epoch == previous_epoch+1, 'lane epoch skipped or repeated'
        assert begin >= previous_end, 'adjacent-cell feedback was speculated'
        if cell == previous_cell:
            assert inverse and context == 0 and previous_phases == 1, 'invalid repeated cell phase'
            phases = 2
        else:
            assert cell == previous_cell+(-1 if context == 1 else 1), 'nonadjacent lane cell'
    return cell, end, epoch, phases


def check_lane_mvm(text, config, cluster, rf=None):
    events=fields(text,'CFD_LANE_MVM')
    operands=fields(text,'CFD_LANE_OPERAND')
    enabled=config.get('--coeff-lane-mvm','0')=='1'
    if not enabled:
        assert not events and not operands, 'disabled lane scheduler produced events'
        return {}
    schedules=fields(text,'CFD_LINE_SCHEDULE')
    fixtures=fields(text,'CFD_CACHE_CONTRACT')
    assert not fixtures or len(fixtures)==len(schedules)
    contracts={row['token']:(fixtures[index]['mode'],int(fixtures[index]['cells'])) if fixtures else
               (config.get('--coeff-base-solve','direct'),int(config['--lusgs-cells']))
               for index,row in enumerate(schedules)}
    local=config.get('--coeff-lane-local','0')=='1'
    select=int(config.get('--coeff-lane-select-latency','1'))
    route=int(config.get('--coeff-lane-route-latency','1'))
    width=int(config.get('--coeff-lane-issue-width','10'))
    assert 1<=select<=16 and 1<=route<=16 and 1<=width<=10
    assert int(config.get('--coeff-line-product-depth','5'))==5
    signed=config.get('--coeff-backward-feedback-tree','0')=='1'
    identity=lambda r:tuple(r[k] for k in ('token','context','epoch','cell'))
    loaded={}
    for row in operands:
        key=identity(row),int(row['column'])
        assert key not in loaded
        loaded[key]=int(row['cycle'])
    physical={r['id']:r for r in fields(text,'CFD_RF_ISSUE')}
    returned={r['id']:r for r in fields(text,'CFD_RF_COMPLETE')}
    back_used={(r['token'],r['cell'],int(r['column'])):int(r['cycle'])
               for r in fields(text,'CFD_BACK_OPERAND') if r['event']=='use'}
    y_jobs={(r['token'],r['cell']):r['job'] for r in fields(text,'CFD_BACK_OPERAND')
            if r['event']=='y_use'} if config.get('--coeff-back-independent-ready','0')=='1' else {}
    wanted_y=set(y_jobs.values())
    y_ends={r['id']:int(r['cycle']) for r in iter_fields(text,'CFD_ONCHIP_COMPLETE')
            if r['id'] in wanted_y} if wanted_y else {}
    groups={};usage=Counter();expected=Counter();used=set()
    writes=Counter();early_rows=0
    previous_cycle=-1
    for event in events:
        cycle=int(event['cycle']);assert cycle>=previous_cycle
        previous_cycle=cycle
        key=identity(event);node=int(event['node']);row=int(event['row'])
        feedback=signed and key[1]=='1';last=9 if feedback else 8
        assert 0<=node<=last and 0<=row<5 and 0<=int(key[1])<3
        group=groups.setdefault(key,{})
        one=group.setdefault((node,row),{})
        kind=event['event'];assert kind in ('issue','complete') and kind not in one
        if kind=='issue':
            assert 'id' in event and event['id'] not in used
            used.add(event['id']);op=physical[event['id']]
            pool='mul' if node<5 else 'add'
            caller='8' if node<5 else '7' if feedback and node==last else '9'
            assert op['cycle']==event['cycle'] and op['lanes']=='1'
            assert op['pool']==pool and op['caller']==caller
            a=float.fromhex(event['a']);b=float.fromhex(event['b'])
            if node<5:
                ready=loaded[key,node]
                if key[1]=='1':
                    assert ready>=back_used[key[0],key[3],node], 'lane consumed a column before SPM-to-RF'
                peers=[r['issue'] for (n,_),r in group.items() if n==node and 'issue' in r]
                assert all(p['b']==event['b'] for p in peers), 'column scalar changed between rows'
                local_reads=0
            else:
                lhs,rhs=([(-1,0),(1,2),(3,4),(5,6),(8,7)] if feedback else
                         [(0,1),(2,3),(5,6),(7,4)])[node-5]
                deps=[group[index,row]['complete'] for index in (lhs,rhs) if index>=0]
                ready=max(int(dep['cycle'])+route for dep in deps)
                if feedback and lhs<0 and y_jobs:
                    ready=max(ready,y_ends[y_jobs[key[0],key[3]]])
                if lhs>=0:
                    assert a.hex()==float.fromhex(group[lhs,row]['complete']['value']).hex(), 'left forwarding value changed'
                assert b.hex()==float.fromhex(group[rhs,row]['complete']['value']).hex(), 'right forwarding value changed'
                local_reads=(1 if lhs<0 else 2) if local else 0
            assert int(event['ready'])==ready
            assert cycle>=ready+select, 'registered selection or forwarding latency bypassed'
            assert int(op.get('local_reads','0'))==local_reads
            assert int(op['read_words'])==2-local_reads
            assert int(op.get('local_result','0'))==int(local and node!=last)
            if local_reads or local and node!=last:assert int(op['row'])==row
            usage[cycle]+=1;assert usage[cycle]<=width, 'lane selection width oversubscribed'
            expected[pool,caller,cycle]+=1
        else:
            assert 'issue' in one
            issue=one['issue'];op=returned[issue['id']]
            assert op['cycle']==event['cycle'], 'lane completion preceded physical storage'
            a=float.fromhex(issue['a']);b=float.fromhex(issue['b'])
            subtract=feedback and node in (5,8,9)
            value=a*b if node<5 else a-b if subtract else a+b
            actual=float.fromhex(event['value'])
            assert math.isfinite(actual) and actual.hex()==value.hex(), 'FP64 operation or reduction order changed'
            writes['mul' if node<5 else 'add',
                   '8' if node<5 else '7' if feedback and node==last else '9',
                   int(issue['cycle']),cycle,1]+=1
        one[kind]=event
    actual=Counter((r['pool'],r['caller'],int(r['cycle'])) for r in cluster
                   if r['caller'] in ('7','8','9') and int(r['lanes'])==1)
    assert actual==expected, 'row graph bypassed or duplicated physical arithmetic'
    if rf is not None:
        assert all(rf.completions[key]>=count for key,count in writes.items())
    back_counts=Counter();sequence={}
    for key,group in groups.items():
        last=9 if signed and key[1]=='1' else 8
        assert set(group)=={(node,row) for node in range(last+1) for row in range(5)}, 'incomplete row graph'
        assert all(set(one)=={'issue','complete'} for one in group.values())
        assert all((key,col) in loaded for col in range(5))
        for row in range(5):
            early_rows += int(group[5,row]['issue']['cycle']) < max(
                int(group[0,r]['complete']['cycle']) for r in range(5))
        stream=key[:2]
        begin=min(int(group[col,row]['issue']['cycle']) for col in range(5) for row in range(5))
        end=max(int(group[last,row]['complete']['cycle']) for row in range(5))
        sequence[stream]=advance_lane_epoch(sequence.get(stream),int(key[3]),
            int(key[2]),begin,end,int(key[1]),contracts[key[0]][0]=='inverse')
        if key[1]=='1':back_counts[key[0]]+=1
    assert events or all(cells==1 for mode,cells in contracts.values())
    assert all(back_counts[token]==cells-1 for token,(mode,cells) in contracts.items())
    assert len(loaded)==len(groups)*5
    return dict(contexts=len(groups),scalar_operations=len(used),
        peak_issue_width=max(usage.values(),default=0),
        rows_released_before_product_group=early_rows,
        local_banks=5 if local else 0,selection_latency=select,forwarding_latency=route)
