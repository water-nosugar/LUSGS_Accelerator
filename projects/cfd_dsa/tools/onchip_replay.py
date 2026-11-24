"""Replay all SPM words while retaining only live and completed jobs."""
from collections import Counter
from trace_text import prefixed_lines


def replay_onchip(text,config):
    depth=int(config.get('--coeff-onchip-queue-depth','64'))
    reserve=int(config.get('--coeff-onchip-compute-reserve','0'))
    banks=int(config['--coeff-spm-banks']);grain=int(config['--coeff-spm-bank-granularity'])
    ports={'R':int(config['--coeff-spm-read-ports']),'W':int(config['--coeff-spm-write-ports'])}
    latency=int(config.get('--coeff-onchip-latency','1'))
    arb_latency=int(config.get('--coeff-onchip-arb-latency','1'))
    arb_tasks=int(config.get('--coeff-onchip-arb-tasks','0'))
    capacity=int(config['--coeff-line-spm-bytes'])
    assert 0<=reserve<depth and latency>=1 and arb_latency>=1 and arb_tasks>=0
    live={};jobs={};ends={};word_counts=Counter();bank_reports={}
    started=completed=cancelled=peak=peak_grants=0
    last_id=-1;access_cycle=-1;per_port=Counter();grants=set();summary=None
    for line in prefixed_lines(text,'CFD_ONCHIP_'):
        kind,_,body=line.partition(' ')
        if kind not in ('CFD_ONCHIP_START','CFD_ONCHIP_ACTIVATE','CFD_ONCHIP_ACCESS',
                        'CFD_ONCHIP_COMPLETE','CFD_ONCHIP_CANCEL','CFD_ONCHIP_SUMMARY','CFD_ONCHIP_BANK'):
            continue
        row=dict(word.split('=',1) for word in body.split() if '=' in word)
        if kind=='CFD_ONCHIP_START':
            ident=row['id'];number=int(ident);cycle=int(row['cycle'])
            # The controller assigns monotonically increasing IDs. This proves
            # uniqueness without storing every cancelled speculative probe.
            assert number>last_id,'SPM task ID reused or reordered'
            last_id=number
            active=row['active']=='1'
            assert row['active'] in ('0','1') and int(row['words'])>0
            assert len(live)<depth,'SPM queue exceeded capacity'
            if not active:assert len(live)<depth-reserve,'DMA occupied reserved compute credit'
            live[ident]=dict(start=row,next=0,last=-1,active=active,eligible=cycle+arb_latency)
            started+=1;peak=max(peak,len(live))
        elif kind=='CFD_ONCHIP_ACTIVATE':
            assert row['id'] in live,'SPM activation lacks a live reservation'
            job=live[row['id']];cycle=int(row['cycle'])
            assert not job['active'] and cycle>=int(job['start']['cycle'])
            job['active']=True;job['eligible']=cycle+arb_latency
        elif kind=='CFD_ONCHIP_CANCEL':
            assert row['id'] in live,'SPM cancellation lacks a live reservation'
            job=live.pop(row['id'])
            assert not job['active'] and not job['next'],'active SPM job cancelled'
            cancelled+=1
        elif kind=='CFD_ONCHIP_ACCESS':
            assert row['id'] in live,'SPM word access lacks a live task'
            job=live[row['id']];start=job['start'];cycle=int(row['cycle'])
            assert job['active'] and cycle>=job['eligible'],'SPM access bypassed activation/arbitration latency'
            assert cycle>=access_cycle,'SPM word events reversed time'
            if cycle!=access_cycle:
                per_port.clear();grants.clear();access_cycle=cycle
            op=row['op'];bank=int(row['bank']);address=int(row['addr'],16)
            assert op==start['op'] and row['owner']==start['owner']
            assert job['next']<int(start['words']),'duplicate SPM operand word'
            assert address==int(start['base'],16)+job['next']*int(start['stride'])
            assert 0<=address and address%8==0 and address+8<=capacity
            assert bank==(address//grain)%banks
            per_port[bank,op]+=1
            assert per_port[bank,op]<=ports[op],'SPM bank port oversubscribed'
            grants.add(row['id']);peak_grants=max(peak_grants,len(grants))
            assert not arb_tasks or len(grants)<=arb_tasks,'SPM arbiter serviced too many distinct tasks'
            job['next']+=1;job['last']=cycle;word_counts[op,bank]+=1
        elif kind=='CFD_ONCHIP_COMPLETE':
            assert row['id'] in live,'SPM completion lacks a live task'
            job=live.pop(row['id']);start=job['start']
            assert job['active'] and job['next']==int(start['words']),'missing SPM operand words'
            assert int(row['cycle'])==job['last']+latency,'SPM access completed before its latency'
            assert row['owner']==start['owner']
            jobs[row['id']]=start;ends[row['id']]=row;completed+=1
        elif kind=='CFD_ONCHIP_SUMMARY':
            summary=row
            expected=dict(depth=depth,banks=banks,grain=grain,read_ports=ports['R'],
                write_ports=ports['W'],latency=latency,created=started,completed=completed,
                cancelled=cancelled,live=len(live),peak=peak,compute_reserve=reserve,
                inactive=sum(not job['active'] for job in live.values()))
            assert all(int(row.get(key,'0'))==value for key,value in expected.items()),'SPM summary differs from word/ownership replay'
        else:
            bank_reports[int(row['bank'])]=row
    assert summary is not None and not live,'SPM access tasks did not drain'
    assert started==completed+cancelled and len(jobs)==len(ends)==completed
    assert len(bank_reports)==banks
    for bank,row in bank_reports.items():
        assert int(row['read_words'])==word_counts['R',bank]
        assert int(row['write_words'])==word_counts['W',bank]
    metrics=dict(started=started,completed=completed,cancelled=cancelled,
        retained_completed_jobs=len(jobs),peak_live_jobs=peak,peak_progress_tasks=peak_grants,
        cancelled_job_metadata_retained=0)
    return summary,jobs,ends,word_counts,metrics
