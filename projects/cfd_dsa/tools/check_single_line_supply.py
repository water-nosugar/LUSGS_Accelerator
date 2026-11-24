"""Replay physical coalesced reads and per-cell reverse reload readiness."""
from collections import defaultdict
from check_single_line_c import fields
from trace_text import iter_fields


def check_output_bursts(text,config,clock=1000):
    events=fields(text,'CFD_OUTPUT_BURST');slices=fields(text,'CFD_OUTPUT_SLICE')
    if config.get('--coeff-output-coalesce','0')!='1':
        assert not events and not slices
        return dict(packets=0,merged=0)
    schedules={r['token']:r for r in fields(text,'CFD_LINE_SCHEDULE')}
    storage={r['token']:int(r['cells']) for r in fields(text,'CFD_LINE_STORAGE')}
    physical={r['packet']:r for r in fields(text,'CFD_DMA_PACKET') if r['event']=='queue'}
    returns={r['packet']:int(r['tick'])//clock for r in fields(text,'CFD_DMA_PACKET') if r['event']=='response'}
    issues={r['packet']:r for r in events if r['event']=='issue'}
    responses={r['packet']:r for r in events if r['event']=='response'}
    assert len(events)==2*len(issues) and issues.keys()==responses.keys()
    groups=defaultdict(list);words=defaultdict(set);bases={};merged=0
    for s in slices:groups[s['packet']].append(s)
    assert groups.keys()==issues.keys()
    for packet,r in issues.items():
        size=int(r['bytes']);addr=int(r['addr'],16);token=r['token']
        resp=responses[packet];parts=groups[packet]
        assert all(r[k]==resp[k] for k in ('token','addr','bytes','members'))
        assert 0<size<=min(64,int(config.get('--coeff-drain-width','64')))
        assert addr//64==(addr+size-1)//64
        assert physical[packet]['op']=='W' and int(physical[packet]['bytes'])==size
        assert int(physical[packet]['tick'])//clock==int(r['cycle'])
        assert int(resp['cycle'])==returns[packet]<=int(schedules[token]['visible_done'])
        assert len(parts)==int(r['members'])<=3
        merged+=len(parts)>1;covered=0
        for s in parts:
            cell=int(s['cell']);offset=int(s['offset']);n=int(s['bytes'])
            assert s['token']==token and s['cycle']==r['cycle']
            assert int(s['payload_offset'])==covered and 0<=cell<storage[token]
            assert offset%8==n%8==0 and 0<n<=40-offset
            base=addr+covered-cell*40-offset
            assert bases.setdefault(token,base)==base
            for word in range(offset//8,(offset+n)//8):
                assert (cell,word) not in words[token]
                words[token].add((cell,word))
            covered+=n
        assert covered==size
    for token,s in schedules.items():
        assert words[token]=={(cell,w) for cell in range(storage[token]) for w in range(5)}
        assert sum(r['token']==token for r in issues.values())==int(s['output_packets_issued'])
    return dict(packets=len(issues),merged=merged)


def check_direct_rhs(text,config,trace,clock=1000,allow_abort=False):
    events=fields(text,'CFD_DIRECT_RHS')
    depth=int(config.get('--coeff-rhs-lookahead','0'))
    if not depth:
        assert not events
        return dict(allocated=0,consumed=0,discarded=0,peak=0)
    window=int(config['--coeff-stream-window'])
    sizes={r['token']:int(r['cells']) for r in fields(text,'CFD_LINE_STORAGE')}
    schedules=fields(text,'CFD_LINE_SCHEDULE');storage={r['token']:r for r in fields(text,'CFD_LINE_STORAGE')}
    fixture=fields(text,'CFD_CACHE_CONTRACT');expected=set();cold_expected=set()
    for i,s in enumerate(schedules):
        mode=fixture[i]['mode'] if fixture else config['--coeff-base-solve']
        store=storage[s['token']];n=int(store['cells'])
        if int(s['cache_hit']) and mode=='direct' and store.get('tile_y_only')=='1' and n>1:
            expected.update((s['token'],str(c)) for c in range(n))
        if not int(s['cache_hit']) and mode=='direct' and config.get('--coeff-cold-rhs-prefetch','0')=='1':
            cold_expected.update((s['token'],str(c)) for c in range(n))
    expected |= cold_expected
    if not events:
        assert not expected, 'missing direct RHS prefetch lifecycle'
        return dict(allocated=0,consumed=0,discarded=0,peak=0)
    jobs={r['id']:r for r in fields(text,'CFD_ONCHIP_START')}
    ends={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_COMPLETE')}
    activated={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_ACTIVATE')}
    dma={r['packet']:int(r['tick'])//clock for r in fields(text,'CFD_DMA_PACKET') if r['event']=='response'}
    transfers=fields(text,'CFD_CACHE_DMA')
    transfers_by_cell=defaultdict(list)
    for transfer in transfers:
        if transfer['kind']=='rhs':
            transfers_by_cell[transfer['token'],transfer['cell']].append(transfer)
    tokens_by_request_stage=defaultdict(list)
    for token in trace:
        if token['stage'] in ('INPUT_TOKEN','INPUT_R_READY'):
            tokens_by_request_stage[token['requestId'],token['stage']].append(token)
    live={}; allocated=set(); consumed=set();discarded=set();peak=0
    for r in events:
        # The descriptor window is clamped to the actual line length. Cache
        # contract fixtures can change that length between invocations.
        stride=min(8,window,sizes.get(r['token'],window))
        bound=min(depth,stride)
        key=r['token'],r['cell']; slot=r['token'],int(r['cell'])%stride; cycle=int(r['cycle'])
        if r['event']=='allocate':
            assert key not in allocated and slot not in live
            assert int(r['slot'])==slot[1] and int(r['depth'])==bound
            live[slot]=key;allocated.add(key)
            occupancy=sum(k[0]==r['token'] for k in live)
            assert occupancy<=bound, 'direct R prefetch exceeded finite slots'
            peak=max(peak,occupancy)
        elif r['event']=='consume':
            assert live.pop(slot)==key and key not in consumed
            job=jobs[r['job']]
            assert job['op']=='R' and job['words']=='5' and job['stride']=='8'
            assert int(job['cycle'])>=int(r['dma_done']) and ends[r['job']]<=cycle
            ps=transfers_by_cell[key]
            issues={p['packet']:p for p in ps if p['event']=='issue'}
            responses={p['packet']:p for p in ps if p['event']=='response'}
            assert issues.keys()==responses.keys() and sum(int(p['bytes']) for p in issues.values())==40
            assert max(int(p['cycle']) for p in responses.values())==int(r['dma_done'])
            for packet,p in issues.items():
                store=jobs[p['job']];resp=responses[packet]
                assert store['op']=='W' and store['active']=='0' and int(store['words'])*8==int(p['bytes'])
                assert p['job']==resp['job'] and activated[p['job']]==dma[packet]
                assert ends[p['job']]==int(resp['cycle']), 'prefetched R bypassed its SPM store'
            cold=r.get('mode')=='cold'
            assert cold == (key in cold_expected) or allow_abort
            marker='INPUT_R_READY' if cold else 'INPUT_TOKEN'
            tokens=tokens_by_request_stage[r['request'],marker]
            assert len(tokens)==1 and int(tokens[0]['cycle'])==cycle
            assert tokens[0]['cell']==r['cell'] and tokens[0]['generation']==r['generation']
            if cold:
                assert tokens[0]['packetId']==r['job']
                assert job['owner']==r['token'] and job['active']=='1'
                offset=0;landing=None
                for p in ps:
                    if p['event']!='issue':continue
                    store=jobs[p['job']];base=int(store['base'],16)-offset
                    assert store['owner']==r['token']
                    if landing is None:landing=base
                    assert base==landing
                    offset+=int(p['bytes'])
                assert offset==40 and int(job['base'],16)==landing
            consumed.add(key)
        elif r['event']=='discard':
            assert allow_abort and live.pop(slot)==key and key not in discarded
            discarded.add(key)
        else:raise AssertionError('unknown direct R lifecycle')
    assert not live and allocated==consumed|discarded, 'direct R slot leak'
    assert expected<=consumed
    if not allow_abort:assert consumed==expected and not discarded
    return dict(allocated=len(allocated),consumed=len(consumed),discarded=len(discarded),peak=peak)


def check_rhs_bursts(text, config, trace, clock=1000, allow_abort=False):
    bursts=fields(text,'CFD_RHS_BURST'); slices=fields(text,'CFD_RHS_SLICE')
    if config.get('--coeff-rhs-input-coalesce','0')!='1':
        assert not bursts and not slices
        return dict(packets=0,logical_fragments=0)
    jobs={r['id']:r for r in fields(text,'CFD_ONCHIP_START')}
    ends={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_COMPLETE')}
    active={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_ACTIVATE')}
    packets={r['packet']:r for r in fields(text,'CFD_DMA_PACKET') if r['event']=='queue'}
    responses={r['packet']:int(r['tick'])//clock for r in fields(text,'CFD_DMA_PACKET') if r['event']=='response'}
    grouped=defaultdict(dict); parts=defaultdict(list)
    aborted={r['requestId'] for r in trace if r['stage'] in
             ('CANCEL_PENDING','CANCEL_COMPLETE','REQUEST_FAILED')} if allow_abort else set()
    for r in bursts:
        assert r['event'] not in grouped[r['packet']]
        grouped[r['packet']][r['event']]=r
    for r in slices:parts[r['packet']].append(r)
    assert grouped.keys()==parts.keys()
    for packet,events in grouped.items():
        assert set(events)=={'issue','response'}
        issue,response=events['issue'],events['response']
        cycle=int(issue['cycle']); received=int(response['cycle'])
        assert received>cycle
        assert received==responses[packet], 'burst completed before physical DMA response'
        assert all(issue[k]==response[k] for k in ('addr','bytes','members'))
        physical=packets[packet]
        assert physical['op']=='R' and physical['bytes']==issue['bytes']
        members=sorted(parts[packet],key=lambda r:int(r['payload_offset']))
        assert 2<=len(members)==int(issue['members'])<=3
        covered=0
        for r in members:
            assert int(r['cycle'])==cycle and int(r['payload_offset'])==covered
            size=int(r['bytes']); offset=int(r['offset']); job=jobs[r['job']]
            assert size>0 and size%8==0 and offset+size<=40
            covered+=size
            assert job['op']=='W' and job['active']=='0'
            assert int(job['base'],16)==int(r['slot'])*8192+(8*(int(r['slot'])%4) if int(config.get('--coeff-spm-slot-rotation','0'))&2 else 0)+offset
            assert int(job['words'])*8==size and int(job['stride'])==8
            assert active[r['job']]==received and ends[r['job']]>received
            logical=[t for t in trace if t['requestId']==r['request'] and
                t['stage']=='SPM_PACKET_RESPONSE' and t['requester']=='CoeffInput' and
                t['packetId']==packet]
            if r['request'] not in aborted:assert len(logical)==1
            assert len(logical)<=1
            if logical:
                assert int(logical[0]['cycle'])==ends[r['job']]
                assert int(logical[0]['size'])==size
                assert int(logical[0]['address'],16)==int(issue['addr'],16)+int(r['payload_offset'])
                assert all(logical[0][k]==r[k] for k in ('generation','cell'))
            tokens=[int(t['cycle']) for t in trace if t['requestId']==r['request'] and t['stage']=='INPUT_TOKEN']
            if r['request'] not in aborted:assert len(tokens)==1
            assert all(t>=ends[r['job']] for t in tokens), 'R consumed before its SPM write'
        assert covered==int(issue['bytes'])<=64, 'burst slices overlap or omit bytes'
    return dict(packets=len(grouped),logical_fragments=len(slices))


def check_tile_read_order(text, config, storage, starts, packets):
    policies=fields(text,'CFD_TILE_READ_POLICY')
    orders=fields(text,'CFD_TILE_ORDER')
    enabled=config.get('--coeff-tile-cell-order','0')=='1'
    if not enabled:
        assert not policies and not orders, 'disabled tile order produced events'
        return {}
    assert len(policies)==len(storage) and {r['token'] for r in policies}==set(storage)
    assert all(r['cell_order']=='1' and r['vector_bytes']=='40' and
               r['matrix_bytes']=='200' for r in policies)
    eligible={(s['token'],s['first']):int(s['cells']) for s in starts
              if storage[s['token']].get('tile_y_only')!='1'}
    ordered={r['packet']:r for r in orders}
    assert len(ordered)==len(orders), 'tile order packet repeated'
    seen=set();coverage={key:[0,0] for key in eligible};fallbacks=0
    for p in packets:
        key=p['token'],p['first']
        if p['event']!='issue' or key not in eligible:continue
        r=ordered[p['packet']];seen.add(p['packet'])
        assert all(r[k]==p[k] for k in ('token','first','packet','cycle','region'))
        y,u=int(r['y_issued']),int(r['u_issued']);n=eligible[key]
        assert 0<=y<=40*n and 0<=u<=200*n and y%8==u%8==0
        preferred=0 if u==200*n else 1 if y==40*n else int(5*y>u)
        assert int(r['preferred'])==preferred, 'tile ordering did not match cell coverage'
        step=int(r['pass']);region=int(r['region'])
        assert step in (0,1) and region==(preferred+step)%2
        # A second pass can follow the first accepted packet, or use the
        # remaining region when the preferred one could not obtain a credit.
        before=coverage[key]
        assert y<=before[0] and u<=before[1]
        delta=(before[0]-y,before[1]-u)
        assert sum(delta)<=64 and (not any(delta) or step==1 and delta[preferred]>0)
        assert step or delta==(0,0)
        before[region]+=int(p['bytes']);fallbacks+=step
    assert seen==set(ordered), 'tile ordering record missing or unrelated to physical DMA'
    return dict(packets=len(seen),second_pass_packets=fallbacks)


def check_streamed_tiles(text, config, clock=1000):
    events=fields(text,'CFD_TILE_EVENT'); ready=fields(text,'CFD_TILE_CELL_READY')
    starts=[r for r in events if r['event']=='reload_start' and r.get('stream')=='1']
    if config.get('--coeff-tile-reload','ordered')!='reverse-stream':
        assert not starts and not ready
        return dict(tiles=0,cells=0,early_cells=0)
    storage={r['token']:r for r in fields(text,'CFD_LINE_STORAGE')}
    for r in events:
        if r['event']=='reload_start' and r['token'] in storage:
            yonly=storage[r['token']].get('tile_y_only')=='1'
            eligible=(yonly or config.get('--coeff-tile-operand-stream','0')=='1') and int(r['cells'])*(5 if yonly else 30)<=1024
            assert (r.get('stream')=='1')==eligible, 'reload policy ignored or exceeded bitmap capacity'
    packets=fields(text,'CFD_TILE_DMA')
    ordering=check_tile_read_order(text,config,storage,starts,packets)
    needed={r['job'] for r in packets}
    jobs={r['id']:r for r in iter_fields(text,'CFD_ONCHIP_START') if r['id'] in needed}
    ends={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_COMPLETE')}
    activations={r['id']:int(r['cycle']) for r in fields(text,'CFD_ONCHIP_ACTIVATE')}
    physical={r['packet']:int(r['tick'])//clock for r in fields(text,'CFD_DMA_PACKET') if r['event']=='response'}
    expected={}; word_ready={}; columns={}; early=0
    for start in starts:
        key=start['token'],start['first']; n=int(start['cells'])
        ps=[r for r in packets if (r['token'],r['first'])==key]
        issued=[r for r in ps if r['event']=='issue']
        responses={r['packet']:r for r in ps if r['event']=='response'}
        assert len(responses)==len(issued) and len({r['packet'] for r in issued})==len(issued)
        matrix=start['token'] in storage and storage[start['token']].get('tile_y_only')!='1'
        frontier={0:n*40,1:n*200} if matrix else {0:n*40}; words={}
        for r in issued:
            offset=int(r['offset']); size=int(r['bytes'])
            region=int(r['region'])
            assert region in frontier and offset+size==frontier[region] and size%8==offset%8==0
            frontier[region]=offset
            resp=responses[r['packet']]
            assert all(r[k]==resp[k] for k in ('offset','bytes','job','region'))
            job=jobs[r['job']]
            assert job['op']=='W' and int(job['words'])*8==size
            assert int(resp['cycle'])==ends[r['job']], 'tile ready bypassed SPM response'
            assert activations[r['job']]==physical[r['packet']], 'tile store preceded DMA response'
            for w in range(offset//8,(offset+size)//8):
                assert (region,w) not in words; words[region,w]=int(resp['cycle'])
        assert all(v==0 for v in frontier.values())
        assert set(words)=={(reg,w) for reg,size in ((0,n*5),(1,n*25)) if reg in frontier for w in range(size)}
        whole=next(int(r['cycle']) for r in events if r['event']=='reload_ready' and (r['token'],r['first'])==key)
        for c in range(n):
            cellkey=start['token'],str(int(start['first'])+c)
            expected[cellkey]=max(words[0,w] for w in range(c*5,(c+1)*5))
            if matrix:
                for col in range(5):
                    columns[cellkey,col]=max(words[1,c*25+row*5+col] for row in range(5))
            word_ready[cellkey]=whole
    actual={}
    for r in ready:
        key=r['token'],r['cell']; assert key not in actual
        actual[key]=int(r['cycle'])
    assert actual==expected, 'partial tile published before all five words were stored'
    for r in fields(text,'CFD_BACK_OPERAND'):
        key=r['token'],r['cell']
        if r['event']=='allocate' and key in expected:
            cycle=int(r['cycle'])
            if config.get('--coeff-back-independent-ready','0')=='1':
                start=next(s for s in starts if s['token']==r['token'] and
                    int(s['first'])<=int(r['cell'])<int(s['first'])+int(s['cells']))
                assert cycle>=int(start['cycle']), 'operand range bound before reload started'
            else:
                assert cycle>=expected[key], 'reload operand read before cell ready'
            early+=cycle<word_ready[key]
        if r['event']=='read' and key in expected:
            cycle=int(r['cycle']);col=int(r['column'])
            if col<0 or config.get('--coeff-back-independent-ready','0')!='1':
                assert cycle>=expected[key], 'reload y read before SPM response'
            if col>=0 and (key,col) in columns:
                assert cycle>=columns[key,col], 'Ubar column read before all five words arrived'
    result=dict(tiles=len(starts),cells=len(expected),early_cells=early)
    if config.get('--coeff-tile-cell-order','0')=='1':result['read_order']=ordering
    return result
