"""Bind reserved-context holds to actual pending base inputs and owner lifetimes."""
from collections import defaultdict
from trace_text import fields


def check_rhs_admission(text,config,trace):
    holds=fields(text,'CFD_RHS_ADMISSION')
    enabled=config.get('--coeff-rhs-frontier-reserve','0')=='1'
    if not enabled:
        assert not holds,'disabled RHS admission reservation emitted holds'
        return {}
    capacity=int(config.get('--coeff3-count','4'))
    allocated={};base={};started={};failed={};by_generation=defaultdict(list)
    for r in trace:
        request=r['requestId'];event=r['stage']
        if event=='REQUEST_ALLOC':
            assert request not in allocated
            allocated[request]=r
            by_generation[r['generation']].append(request)
        elif event=='DIRECT_BASE_READY':base[request]=int(r['cycle'])
        elif event=='COEFF3_ISSUE':started[request]=int(r['cycle'])
        elif event=='REQUEST_FAILED':failed[request]=int(r['cycle'])
    owners=set();previous=-1
    for r in holds:
        now=int(r['cycle']);assert now>=previous;previous=now
        assert r['event']=='hold' and int(r['capacity'])==capacity
        assert int(r['active'])==capacity-1 and int(r['bytes'])==256,'reservation changed physical context credits'
        candidate,protected=r['request'],r['protected_request']
        assert candidate!=protected
        for request,prefix in [(candidate,''),(protected,'protected_')]:
            a=allocated[request]
            assert (a['generation'],a['cell'])==(r[prefix+'generation'],r[prefix+'cell'])
            assert int(a['cycle'])<=now and failed.get(request,now+1)>now
        assert int(r['cell'])>=int(r['protected_cell']) or base.get(candidate,now+1)<=now, 'older deferred candidate still owns unfinished b'
        assert base.get(protected,now+1)>=now and started.get(protected,now+1)>=now, 'reserved credit protects a base that already owns or finished its context'
        possible=[]
        for request in by_generation[r['protected_generation']]:
            a=allocated[request]
            if int(a['cycle'])<=now and failed.get(request,now+1)>now and base.get(request,now+1)>=now:
                possible.append((int(a['cell']),int(request)))
        assert min(possible)==(int(r['protected_cell']),int(protected)), 'reservation skipped an older unfinished b'
        owners.add((r['protected_token'],r['protected_generation']))
    return dict(holds=len(holds),protected_owners=len(owners),extra_control_bytes=256,
                physical_contexts=capacity,added_contexts=0)
