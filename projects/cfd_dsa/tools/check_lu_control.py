"""Replay bounded LU control folding against actual, owned RF returns."""
from collections import Counter
import re


def replay_lu_control(text, config, events):
    matches=list(re.finditer(r'^CFD_LU_CONTROL [^\n]*',text,re.M))
    enabled=config.get('--coeff-lu-control-fold','0')=='1'
    if not enabled:
        assert not matches,'disabled LU control folding emitted transitions'
        return {}
    operations={r['id']:r for prefix,r in events if prefix=='CFD_RF_ISSUE'}
    returns={r['id']:int(r['cycle']) for prefix,r in events if prefix=='CFD_RF_COMPLETE'}
    owners={}
    for match in re.finditer(r'^CFD_RF_OWNER [^\n]*',text,re.M):
        row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        assert row['id'] not in owners,'duplicate RF owner binding'
        assert operations[row['id']]['cycle']==row['cycle']
        owners[row['id']]=row['owner']
    last_sub={}
    if config.get('--coeff-lu-product-ahead','0')=='1':
        for match in re.finditer(r'^CFD_LU_PRODUCTS [^\n]*',text,re.M):
            row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
            if row['event']=='sub-issue' and int(row['k'])+1==int(row['column']):
                last_sub[row['id']]=row
    active=set();cancelled=set();finished=set();counts=Counter();per_request=Counter();indices=Counter()
    previous=-1;peak=0
    contexts=int(config.get('--lu5-count','4'))
    assert contexts>0
    for match in matches:
        row=dict(re.findall(r'(\w+)=([^\s]+)',match[0]))
        now=int(row['cycle']);assert now>=previous,'LU control events out of cycle order'
        previous=now;key=(row['token'],row['generation'])
        if row['event']=='cancel':
            active.discard(key);cancelled.add(key);counts['cancel_events']+=1
            continue
        assert row['event']=='fold' and key not in cancelled,'LU fold after cancellation'
        c,nxt,phase,target,index=(int(row[k]) for k in ('column','next_column','from','to','index'))
        assert 0<=c<5
        wanted={0:(3 if c==0 else 1),3:4,5:(8 if c==0 else 6),9:10,10:(0 if c<4 else 11)}
        assert phase in wanted and target==wanted[phase],'arithmetic/wait/pivot state was folded'
        assert nxt==(c+1 if phase==10 else c),'invalid LU column advance'
        indices[key,now]+=1
        assert index==indices[key,now]<=3,'unbounded LU control chain or repeated visit'
        if phase==0 and c==0:
            assert key not in active|finished,'reused live/completed LU control context'
            active.add(key);peak=max(peak,len(active))
            assert peak<=contexts,'LU control context capacity exceeded'
        assert key in active,'LU control transition without active owner'
        parent=row['parent'];returned=int(row['returned'])
        if phase==3 and c==0:
            assert parent=='0' and returned==0,'first LU column has an unexpected producer'
        elif phase in (3,9):
            op=operations[parent]
            assert parent in returns and owners.get(parent)==row['token'],'LU commit lacks owned RF producer'
            assert returns[parent]==returned<=now,'LU commit preceded the actual RF return'
            upper=phase==9
            assert op['pool']==('mul' if upper else 'add') and op['caller']==('1' if upper else '2')
            assert int(op['lanes'])==5-c-int(upper),'LU producer has wrong row width'
            if upper:
                assert int(op.get('bank_dst','-1'))==-1,'upper normalization bypassed RF'
            else:
                assert op.get('bank_owner')==row['token'] and op.get('bank_epoch')==row['generation']
                if last_sub:
                    source=last_sub[parent]
                    assert source['token']==row['token'] and source['generation']==row['generation']
                    assert source['column']==str(c) and source['upper']=='0','LU commit used a different column'
            counts['actual_return_commits']+=1
        per_request[key]+=1;counts['folds']+=1
        if phase==10 and c==4:
            assert per_request[key]==23,'successful LU control path skipped/duplicated bookkeeping'
            active.remove(key);finished.add(key);counts['completed_contexts']+=1
    assert not active,'LU control contexts did not drain or cancel'
    assert counts['folds'] or not any(r['caller'] in ('1','2') for r in operations.values()),'LU arithmetic lacks control-fold evidence'
    return dict(counts,context_peak=peak,extra_reservation_bytes=32*contexts+128)
