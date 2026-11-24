"""Independently replay the bounded, registered line-record hash engine."""
from trace_text import fields

SEED=14695981039346656037
PRIME=1099511628211
MASK=(1<<64)-1


def check_summary_hash(text,config,trace):
    rows=fields(text,'CFD_SUMMARY_HASH')
    if config.get('--coeff-record-policy') not in ('line-summary','cold-summary'):
        assert not rows, 'unrequested timed summary engine'
        return {}
    if int(config.get('--coeff-summary-depth','0')):
        return check_snapshot_hash(text,config,trace)
    retired={r['requestId']:r for r in trace if r['stage']=='REQUEST_SUMMARY'}
    completed={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='REQUEST_COMPLETE'}
    sources={r['request']:r for r in fields(text,'CFD_RECORD_DIGEST_SOURCE') if 'summary_owner' in r}
    aborts={r['token']:int(r['cycle']) for r in fields(text,'CFD_LINE_ABORT') if r['event']=='begin'}
    hits={r['token']:int(r['cache_hit']) for r in fields(text,'CFD_LINE_SCHEDULE')}
    active=None;seen=set();done=set();words=dropped=busy=0
    for r in rows:
        now=int(r['cycle']);event=r['event']
        if event=='accept':
            if config.get('--coeff-record-policy')=='cold-summary':
                assert not hits.get(r['owner'],0), 'cold-only policy hashed a warm record'
            assert active is None, 'overlapping metadata engines'
            request=r['request']
            assert request not in seen, 'duplicate record hash'
            assert r['words']=='40' and r['word_cycles']=='2' and r['bytes']=='128', 'unbudgeted hash configuration'
            assert now>=completed.get(request,1<<64), 'hash reads a mutable incomplete record'
            seen.add(request)
            active=dict(token=r['token'],owner=r['owner'],request=request,start=now,
                        next=now+2,index=0,digest=SEED)
            continue
        assert active is not None, 'orphan hash result'
        assert (r['token'],r['owner'])==(active['token'],active['owner']), 'hash ownership changed'
        if event=='drop':
            assert now==active['next'] and aborts.get(r['owner'],1<<64)<=now, 'undrained or unrequested cancellation'
            active=None;dropped+=1
        elif event=='word':
            assert r['request']==active['request'] and int(r['index'])==active['index']<40, 'missing or repeated hash word'
            assert now==active['next'], 'hash bypassed registered word latency'
            assert int(r['before'],16)==active['digest'], 'hash dependency bypass'
            word=int(r['word'],16)
            if active['index']==0:assert word==int(active['request']), 'wrong request hashed'
            if active['request'] in retired and active['index']==1:
                assert word==int(retired[active['request']]['generation']), 'wrong generation hashed'
            if active['index']==20:assert word==1, 'auto-retire flag omitted'
            digest=((active['digest']^word)*PRIME)&MASK
            assert int(r['after'],16)==digest, 'incorrect hash arithmetic'
            active.update(digest=digest,index=active['index']+1,next=now+2)
            words+=1
        elif event=='complete':
            request=active['request']
            assert r['request']==request and active['index']==40, 'incomplete record aggregated'
            assert now==active['start']+80==int(retired[request]['cycle']), 'record released before timed hash completed'
            assert int(r['hash'],16)==active['digest']==int(sources[request]['hash'],16), 'aggregate digest bypass'
            assert sources[request]['summary_owner']==active['owner'], 'aggregate owner mismatch'
            done.add(request);busy+=80;active=None
        else:raise AssertionError('unknown summary engine event')
    assert active is None, 'metadata engine left in flight'
    assert done==set(retired)==set(sources), 'record summary lacks timed hash proof'
    return dict(records=len(done),words=words,dropped=dropped,engine_busy_cycles=busy,
                engines=1,state_bytes=128,word_latency=2,word_ii=2,
                record_words=40,record_service_cycles=80)


def check_snapshot_hash(text,config,trace):
    """Replay finite entries, the shared ingress and every integer engine."""
    import re
    depth=int(config['--coeff-summary-depth']);engines=int(config.get('--coeff-summary-engines','1'))
    sme = config.get('--coeff-compute-backend') == 'sme-za'
    max_depth, max_engines = (24, 16) if sme else (8, 4)
    assert 1 <= engines <= max_engines and engines <= depth <= max_depth
    completed={r['requestId']:r for r in trace if r['stage']=='REQUEST_COMPLETE'}
    detached={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='REQUEST_SNAPSHOT_DETACH'}
    retired={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='REQUEST_AUTO_RETIRE'}
    summaries={r['requestId']:int(r['cycle']) for r in trace if r['stage']=='REQUEST_SUMMARY'}
    sources={r['request']:r for r in fields(text,'CFD_RECORD_DIGEST_SOURCE') if 'summary_owner' in r}
    aborts={r['token']:int(r['cycle']) for r in fields(text,'CFD_LINE_ABORT') if r['event']=='begin'}
    hits={r['token']:int(r['cache_hit']) for r in fields(text,'CFD_LINE_SCHEDULE')}
    entries={};active={};seen=set();done=set();copy_cycles=set();peak=words=dropped=allocated=0
    for line in text.splitlines():
        prefix=line.split(' ',1)[0]
        if prefix not in ('CFD_SUMMARY_HASH','CFD_SUMMARY_SNAPSHOT'):continue
        r=dict(re.findall(r'(\w+)=([^\s]+)',line));now=int(r['cycle']);event=r['event'];token=r['token']
        if prefix=='CFD_SUMMARY_SNAPSHOT':
            if event=='allocate':
                assert token not in entries and token not in seen
                request=r['request'];slot=int(r['slot'])
                assert request in completed and now>=int(completed[request]['cycle'])
                assert int(r['depth'])==depth and 0<=slot<depth
                assert not any(e['slot']==slot for e in entries.values()),'snapshot storage reused while live'
                entries[token]=dict(request=request,owner=r['owner'],slot=slot,allocated=now,data=[],detached=False)
                allocated+=1;peak=max(peak,len(entries));assert peak<=depth
            else:
                assert token in entries,'orphan snapshot event'
                e=entries[token];assert r['owner']==e['owner']
                if event=='copy':
                    assert now>e['allocated'] and now not in copy_cycles,'snapshot ingress oversubscribed'
                    assert not e['detached'] and int(r['first'])==len(e['data'])<40 and r['words']=='8'
                    copy_cycles.add(now);e['data'].extend(int(r['w'+str(i)],16) for i in range(8));e['last_copy']=now
                elif event=='detach':
                    assert len(e['data'])==40 and now==e['last_copy']
                    assert r['request']==e['request'] and detached[e['request']]==retired[e['request']]==now
                    assert e['data'][0]==int(e['request']) and e['data'][1]==int(completed[e['request']]['generation'])
                    assert e['data'][20]==1,'snapshot omitted auto-retirement'
                    e['detached']=True
                elif event=='drop':
                    assert aborts.get(e['owner'],1<<64)<=now and not any(a['token']==token for a in active.values())
                    assert int(r['detached'])==int(e['detached']);del entries[token];dropped+=1
                else:raise AssertionError('unknown snapshot event')
            continue
        engine=int(r['engine']);assert 0<=engine<engines and r['snapshot']=='1'
        if event=='accept':
            assert engine not in active and token in entries and token not in seen
            e=entries[token];assert e['detached'] and len(e['data'])==40 and now>e['last_copy']
            assert r['request']==e['request'] and r['owner']==e['owner']
            assert (r['words'],r['word_cycles'],r['bytes'])==('40','2','128')
            if config.get('--coeff-record-policy')=='cold-summary':assert not hits.get(r['owner'],0)
            seen.add(token);active[engine]=dict(token=token,owner=e['owner'],start=now,next=now+2,index=0,digest=SEED)
            continue
        assert engine in active and token==active[engine]['token'],'hash engine ownership changed'
        a=active[engine];e=entries[token]
        assert r['owner']==a['owner'] and now==(a['next']-2 if event=='complete' else a['next'])
        if event=='drop':
            assert aborts.get(e['owner'],1<<64)<=now,'undrained or unrequested hash drop'
            del active[engine];del entries[token];dropped+=1
        elif event=='word':
            assert int(r['index'])==a['index']<40 and r['request']==e['request']
            word=int(r['word'],16);assert word==e['data'][a['index']], 'hash read outside owned snapshot'
            assert int(r['before'],16)==a['digest']
            digest=((a['digest']^word)*PRIME)&MASK
            assert int(r['after'],16)==digest
            a.update(index=a['index']+1,digest=digest,next=now+2);words+=1
        elif event=='complete':
            request=e['request']
            assert a['index']==40 and now==a['start']+80==summaries[request]
            assert int(r['hash'],16)==a['digest']==int(sources[request]['hash'],16)
            assert sources[request]['summary_owner']==e['owner']
            done.add(request);del active[engine];del entries[token]
        else:raise AssertionError('unknown snapshot hash event')
    assert not active and not entries,'snapshots/hash engines did not drain'
    assert done==set(summaries)==set(sources)
    assert allocated==len(done)+dropped
    stats=fields(text,'CFD_SUMMARY_SNAPSHOTS')
    assert stats,'snapshot storage statistics missing'
    last=stats[-1]
    assert int(last['depth'])==depth and int(last['engines'])==engines
    assert last['copy_words']=='8' and last['entry_bytes']=='512' and last['live']=='0'
    assert (int(last['issued']),int(last['completed']),int(last['dropped']),int(last['peak']))==(allocated,len(done),dropped,peak)
    return dict(records=len(done),words=words,dropped=dropped,engine_busy_cycles=80*len(done),
                engines=engines,state_bytes=128*engines+512*depth,word_latency=2,word_ii=2,
                record_words=40,record_service_cycles=80,snapshot_depth=depth,snapshot_peak=peak,
                snapshot_copy_width=8,snapshot_copy_cycles=len(copy_cycles))
