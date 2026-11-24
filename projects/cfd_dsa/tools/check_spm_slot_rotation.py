"""Bind each rotated ingress store and RHS-ring read to its exact slot."""
from collections import defaultdict
from check_single_line_c import fields

def input_base(config,slot):
    mode=int(config.get('--coeff-spm-slot-rotation','0'))
    return slot*8192+(8*(slot%4) if mode&2 else 0)

def ring_base(config):
    total=sum(int(config[k]) for k in ['--coeff-input-slots','--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'])
    level=int(config.get('--coeff-line-opt-level','0'))
    return total*8192+16384+(int(config.get('--coeff-rhs-product-depth','60'))*64+256 if config.get('--coeff-rhs-row-parallel','0')=='1' else 0)+int(config.get('--coeff-completion-depth','0'))*(512 if level>=11 else 1024)

def check_spm_slot_rotation(text,config,trace,jobs,ends):
    mode=int(config.get('--coeff-spm-slot-rotation','0'));stores=fields(text,'CFD_SPM_SLOT_STORE');caps=fields(text,'CFD_SPM_SLOT_ROTATION_CAPACITY')
    if not mode:
        assert not stores and not caps,'disabled slot rotation emitted address witnesses'
        return {}
    assert 1<=mode<=3 and config['--coeff-spm-banks']=='4' and config['--coeff-spm-bank-granularity']=='8'
    assert caps
    for r in caps:
        assert int(r['mode'])==mode
        assert tuple(int(r[k]) for k in ['bytes','banks','grain','input_stride','rhs_stride','max_shift','extra_ports','extra_units'])==(128,4,8,8192,128,24,0,0)
    logical={};offsets=defaultdict(int);allocated={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    for r in trace:
        if r['stage']=='SPM_PACKET_ISSUE' and r.get('requester')=='CoeffInput':
            key=r['requestId'],r['packetId'];assert key not in logical
            size=int(r['size']);logical[key]=(r,offsets[r['requestId']]);offsets[r['requestId']]+=size
    seen=set();seen_jobs=set();rotated_input=0
    for r in stores:
        key=r['request'],r['packet'];assert key in logical and key not in seen;seen.add(key)
        row,offset=logical[key];job=jobs[r['job']]
        assert r['job'] not in seen_jobs;seen_jobs.add(r['job'])
        assert allocated[r['request']]==(r['generation'],r['cell'])
        assert (row['generation'],row['cell'])==(r['generation'],r['cell']) and row['cycle']==r['cycle']
        slot=int(r['slot']);size=int(row['size']);assert int(r['offset'])==offset
        assert slot==int(row['slot']) and 0<=offset and offset+size<=8192
        assert job['op']=='W' and job['active']=='0' and job['priority']=='2' and job['owner']==r['owner'] and job['cycle']==r['cycle']
        assert int(job['base'],16)==input_base(config,slot)+offset,'ingress writer used a different slot/bank mapping'
        assert int(job['words'])==(size+7)//8 and job['stride']=='8' and r['job'] in ends
        rotated_input+=input_base(config,slot)!=slot*8192
    assert seen==set(logical),'missing or extra ingress slot store witness'
    sizes={r['token']:int(r['cells']) for r in fields(text,'CFD_LINE_STORAGE')}
    window=int(config.get('--coeff-stream-window','1'));allocations={};used=set();readers={};producers=defaultdict(list);warm_reads=warm_writes=rotated_rhs=0
    for r in fields(text,'CFD_DIRECT_RHS'):
        if r['event']=='allocate':
            key=r['token'],r['cell'];stride=min(8,window,sizes.get(r['token'],window));slot=int(r['slot'])
            assert slot==int(r['cell'])%stride
            allocations[key]=ring_base(config)+slot*128+(8*(slot%4) if mode&1 else 0)
        elif r['event']=='consume':
            key=r['token'],r['cell'];assert key in allocations
            job=jobs[r['job']];assert job['op']=='R' and job['words']=='5' and job['stride']=='8'
            assert job['owner']==r['token'] and job['active']=='1' and job['priority']=='0'
            assert int(job['base'],16)==allocations[key],'RHS reader used a different slot/bank mapping'
            assert int(ends[r['job']]['cycle'])<=int(r['cycle']);readers[key]=r['job']
            warm_reads+=1;rotated_rhs+=(int(job['base'],16)//8)%4!=0
    offsets=defaultdict(int)
    for r in fields(text,'CFD_CACHE_DMA'):
        if r.get('kind')!='rhs' or r['event']!='issue':continue
        key=r['token'],r['cell'];assert key in allocations and r['job'] not in used;used.add(r['job'])
        job=jobs[r['job']];size=int(r['bytes']);assert job['op']=='W' and job['active']=='0' and job['priority']=='0' and job['words']==str((size+7)//8)
        assert int(job['base'],16)==allocations[key]+offsets[key],'RHS writer used a different slot/bank mapping'
        assert job['owner']==r['token'] and job['cycle']==r['cycle'] and r['job'] in ends
        offsets[key]+=size;assert offsets[key]<=40;warm_writes+=1;producers[key].append(r['job'])
    for key,reader in readers.items():
        assert offsets[key]==40,'RHS read without its entire paid input vector'
        assert all(int(ends[j]['cycle'])<=int(jobs[reader]['cycle']) for j in producers[key]),'RHS read bypassed a producer store'
    return dict(mode=mode,control_bytes=128,ingress_stores=len(stores),rhs_loads=warm_reads,rhs_stores=warm_writes,rotated_ingress_stores=rotated_input,rotated_rhs_loads=rotated_rhs,extra_ports=0,extra_units=0)
