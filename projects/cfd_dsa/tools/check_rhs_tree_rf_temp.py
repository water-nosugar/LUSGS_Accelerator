"""Replay bounded RHS tree partials written to and read from the paid RF."""
from collections import Counter
import re

def fields(line):return dict(re.findall(r'(\w+)=([^\s]+)',line))

def replay_rhs_tree_rf_temp(text,config,operations):
    messages=list(re.finditer(r'^CFD_RHS_TREE_RF_TEMP [^\n]*',text,re.M))
    enabled=config.get('--coeff-rhs-tree-rf-temp','0')=='1'
    first=config.get('--coeff-rhs-tree-rf-first-column','0')=='1'
    assert not first or enabled,'first-column policy requires enabled RF temporaries'
    if not enabled:
        assert not messages,'disabled RHS RF temporary emitted state'
        return {}
    assert config.get('--coeff-local-products','0')=='1'
    assert int(config.get('--coeff-rf-read-words','0'))>0
    assert int(config.get('--coeff-rf-write-words','0'))>0
    depth=int(config.get('--coeff-rhs-product-depth','60'))
    assert 16<=depth<=60
    base=27+int(config.get('--lu5-count','4'))
    issued={r['id']:r for p,r in operations if p=='CFD_RF_ISSUE'}
    completed={r['id']:r for p,r in operations if p=='CFD_RF_COMPLETE'}
    grants={}
    for p,r in operations:
        if p=='CFD_RF_WRITE':grants.setdefault(r['id'],[]).append(r)
    positions={}
    for m in re.finditer(r'^CFD_RF_(ISSUE|COMPLETE) [^\n]*',text,re.M):
        r=fields(m[0]);key=m[1],r['id']
        assert key not in positions;positions[key]=m.start()
    tree={}
    for m in re.finditer(r'^CFD_RHS_TREE [^\n]*',text,re.M):
        r=fields(m[0])
        if r['node'] not in ('1','2') or int(r['k'])>=2:continue
        key=tuple(r[k] for k in ('request','generation','rhs','k','node','event'))
        assert key not in tree;tree[key]=(r,m.start())
    live={};writes=set();reads=set();counts=Counter();peak=0;last=-1
    identity=('token','generation','request','cell','rhs','k','slot','value')
    for m in messages:
        r=fields(m[0]);t=int(r['cycle']);slot=int(r['slot']);event=r['event']
        assert t>=last;last=t
        assert 0<=slot<depth and int(r['k']) in (0,1)
        assert 0<=int(r['rhs'])<15 and event in ('write','read','clear')
        assert not first or r['rhs']=='5','unselected RHS used a first-column RF temporary'
        if event=='clear':
            assert r['rf']=='0' and slot in live
            old,_=live.pop(slot)
            assert all(old[k]==r[k] for k in identity)
            counts['clears']+=1
            continue
        assert r['rf'] in issued
        op=issued[r['rf']]
        assert op['pool']=='add' and op['caller']=='5' and op['lanes']=='1'
        assert op.get('bank_access')=='1'
        assert op.get('local_result','0')=='0' and op.get('local_reads','0')=='0'
        assert op['bank_owner']==r['token'] and op['bank_epoch']==r['generation']
        assert op['bank_b']=='-1' and op['bank_words_b']=='0' and op['bank_dst']=='-1'
        key=tuple(r[k] for k in ('request','generation','rhs','k'))
        if event=='write':
            assert slot not in live and key not in writes,'RHS RF temporary overwritten'
            assert r['rf'] in completed
            done=completed[r['rf']]
            assert done['cycle']==r['cycle'] and int(op['ready'])<=t
            if config.get('--coeff-rf-split-writeback','0')=='1':
                paid=grants.get(r['rf'],[])
                assert len(paid)==1 and paid[0]['cycle']==r['cycle']
                assert paid[0]['lanes']=='1' and paid[0]['offset']=='0'
            assert op['bank_a']==str(base+slot) and op['bank_words_a']=='2'
            assert op['read_words']=='0','tree partial removed or added a source read'
            assert positions['COMPLETE',r['rf']]<m.start()
            source=key+('1','issue');finish=key+('1','complete')
            assert source in tree and finish in tree
            assert tree[source][0]['cycle']==op['cycle']
            assert tree[finish][0]['cycle']==r['cycle']
            assert positions['ISSUE',r['rf']]<tree[source][1]
            assert m.start()<tree[finish][1]
            live[slot]=(r,m.start());writes.add(key);counts['writes']+=1
            peak=max(peak,len(live));assert peak<=depth
        else:
            assert slot in live,'unpublished or cleared RHS RF temporary'
            source,write_position=live.pop(slot)
            assert all(source[k]==r[k] for k in identity)
            assert r['rf']!=source['rf'] and key not in reads
            assert op['cycle']==r['cycle']
            assert op['bank_a']=='-1' and op['bank_words_a']=='0'
            assert op['read_words']=='2','tree consumer did not pay both RF reads'
            assert write_position<positions['ISSUE',r['rf']]<m.start()
            consumer=key+('2','issue')
            assert consumer in tree and tree[consumer][0]['cycle']==r['cycle']
            assert m.start()<tree[consumer][1]
            reads.add(key);counts['reads']+=1
    assert not live,'live RHS RF temporary at completion'
    selected=lambda key:not first or key[2]=='5'
    assert writes=={k[:4] for k in tree if k[4:]==('1','complete') and selected(k)},'unbound tree RF write'
    assert reads=={k[:4] for k in tree if k[4:]==('2','issue') and selected(k)},'unbound tree RF read'
    local=list(re.finditer(r'^CFD_LOCAL_VECTOR [^\n]*',text,re.M));assert local
    last=fields(local[-1][0])
    assert last.get('tree_rf_temp')=='1' and last.get('tree_rf_temp_live')=='0'
    assert int(last['tree_rf_temp_peak'])==peak
    assert int(last['tree_rf_temp_bytes'])==16*depth+128
    assert int(last.get('tree_rf_first_column','0'))==int(first)
    return dict(writes=counts['writes'],reads=counts['reads'],clears=counts['clears'],
                peak=peak,extra_reservation_bytes=16*depth+128)
