"""Accept actual simulated failures/cancellations only after a clean retry."""
import json
import csv
import re
from pathlib import Path
from check_single_line_c import fields, check_numeric
from check_single_line_output_dma import options, resolved_options
from check_single_line_dma_packets import check_dma_packets
from check_divider_model import check_case as check_divider


def check_recovery(path, *, sme_abort=None):
    text=(path/'simout.txt').read_text()
    config=resolved_options(path)
    divider=check_divider(path,config)
    from check_register_ports import rf_enabled,replay_register_ports
    register_ports={}
    if rf_enabled(config):
        with (path/'coeff_preprocess_trace.csv.cluster.csv').open() as source:
            cluster=list(csv.DictReader(source))
        replay=replay_register_ports(text,config,cluster)
        from check_arithmetic_groups import replay_arithmetic_groups
        arithmetic_groups,_,_=replay_arithmetic_groups(text,config,cluster,replay)
        register_ports=replay.summary
        register_ports['arithmetic_groups']=arithmetic_groups
    from onchip_replay import replay_onchip
    from check_input_split import check_input_split
    from check_cold_rhs import check_cold_rhs
    onchip={}
    jobs=ends={}
    if config.get('--coeff-onchip-timing','1' if int(config.get('--coeff-line-opt-level','0'))>=8 else '0')=='1':
        _,jobs,ends,_,onchip=replay_onchip(text,config)
    with (path/'coeff_preprocess_trace.csv').open() as src:
        trace=list(csv.DictReader(src))
    from check_spm_slot_rotation import check_spm_slot_rotation
    rotation=check_spm_slot_rotation(text,config,trace,jobs,ends)
    if rotation:onchip['spm_slot_rotation']=rotation
    from check_rhs_admission import check_rhs_admission
    register_ports['rhs_admission']=check_rhs_admission(text,config,trace)
    from check_rhs_upper import check_rhs_upper_schedule
    onchip['rhs_upper']=check_rhs_upper_schedule(config,trace,register_ports.get('rhs_upper',{}))
    from check_rhs_upper_stream import check_rhs_stream_schedule
    divider_rows=[]
    if config.get('--coeff-rhs-upper-stream','0')=='1' or config.get('--coeff-lu-pivot-prefetch','0')=='1':
        with (path/'coeff_preprocess_trace.csv.divider.csv').open() as source:divider_rows=list(csv.DictReader(source))
    onchip['rhs_upper_stream']=check_rhs_stream_schedule(config,trace,register_ports.get('rhs_upper_stream',{}),divider_rows)
    from check_lu_pivot_prefetch import check_pivot_prefetch_schedule
    onchip['lu_pivot_prefetch']=check_pivot_prefetch_schedule(text,config,trace,divider_rows)
    onchip['input_split']=check_input_split(text,config,trace,jobs,ends,allow_abort=True)
    if sme_abort is None:
        onchip['cold_rhs']=check_cold_rhs(text,config,trace,allow_abort=True)
    from check_single_line_frontier import check_drain_arb
    drain_arb=check_drain_arb(text,config,trace,allow_abort=True)
    from check_single_line_supply import check_rhs_bursts, check_direct_rhs
    clock=int(json.loads((path/'config.json').read_text())['system']['clk_domain']['clock'][0])
    bursts=check_rhs_bursts(text,config,trace,clock,allow_abort=True)
    direct_rhs=check_direct_rhs(text,config,trace,clock,allow_abort=True)
    warm_cancel={}
    if config.get('--coeff-recovery-warm','0')=='1':
        reused={r['requestId'] for r in trace if r['stage']=='LU_CACHE_REUSE'}
        cancelled={r['requestId'] for r in trace if r['stage'] in ('CANCEL_PENDING','CANCEL_COMPLETE')}
        affected=reused & cancelled
        if sme_abort is None:
            rhs=[r for r in fields(text,'CFD_RHS_TREE') if r['request'] in affected and r['event']=='issue']
            forward=[r for r in fields(text,'CFD_FORWARD_TREE') if r['request'] in affected and r['event']=='begin']
            assert affected and rhs and forward, 'warm tree cancellation not actually exercised'
            warm_cancel=dict(requests=len(affected),rhs_issues=len(rhs),forward_contexts=len(forward))
        else:
            assert affected and sme_abort['cache_hit'], 'warm SME cancellation was not exercised'
            warm_cancel=dict(requests=len(affected), sme=sme_abort)
    from check_single_line_recurrence import check_data_commit
    commit={}
    if int(config.get('--coeff-data-commit-depth','0')):
        with (path/'coeff_preprocess_trace.csv').open() as src:
            commit=check_data_commit(text,config,list(csv.DictReader(src)),allow_abort=True)
    rows=fields(text,'CFD_RECOVERY_CONTRACT')
    count=6 if int(config['--lusgs-cells'])>=17 else 4
    if 'LUSGS_RECOVERY_CONTRACT_PASS' not in text or len(rows)!=count:
        raise ValueError('recovery contract did not finish')
    check_numeric(text,[int(config['--lusgs-cells'])]*(count//2))
    for step,row in enumerate(rows):
        wanted=int(step not in (0,2,4))
        if int(row['step'])!=step or int(row['actual'])!=wanted or int(row['expected'])!=wanted:
            raise ValueError('failed/cancelled invocation did not recover')
    aborts=fields(text,'CFD_LINE_ABORT')
    begins={r['token']:r for r in aborts if r['event']=='begin'}
    visible={r['token']:r for r in aborts if r['event']=='visible'}
    if len(begins)!=count//2 or begins.keys()!=visible.keys():
        raise ValueError('missing error/cancellation completion fence')
    for token,row in visible.items():
        if any(int(row[k]) for k in ('dma_live','records','children')):
            raise ValueError('error became visible before pending operations drained')
        if int(row['cycle'])<int(begins[token]['cycle']):
            raise ValueError('reversed abort lifetime')
        if row['status']!=begins[token]['status']:
            raise ValueError('a later cancellation overwrote the first error')
    schedules=fields(text,'CFD_LINE_SCHEDULE')
    if len(schedules)!=count//2 or any(int(r['cache_hit']) for r in schedules):
        raise ValueError('retry reused coefficients from a failed invocation')
    if fields(text,'CFD_DMA_PACKET'):
        check_dma_packets(text,path,config)
    else:
        if int(config.get('--coeff-line-opt-level','0'))>=11:
            raise ValueError('physical DMA trace missing')
        stats=(path/'stats.txt').read_text()
        issued=int(re.search(r'\.coeffDmaIssued\s+(\d+)',stats)[1])
        completed=int(re.search(r'\.coeffDmaCompleted\s+(\d+)',stats)[1])
        if not issued or issued!=completed:
            raise ValueError('full-storage recovery left DMA outstanding')
    for prefix,keys in [('CFD_RHS_ROWS',('live',)),('CFD_COMPLETION_QUEUE',('live',)),
                        ('CFD_ONCHIP_SUMMARY',('live',)),('CFD_CLUSTER_BUFFER',('live',))]:
        rows=fields(text,prefix)
        required={'CFD_RHS_ROWS':config.get('--coeff-rhs-row-parallel','0')=='1',
            'CFD_COMPLETION_QUEUE':int(config.get('--coeff-line-opt-level','0'))>=5,
            'CFD_ONCHIP_SUMMARY':int(config.get('--coeff-line-opt-level','0'))>=8,
            'CFD_CLUSTER_BUFFER':config.get('--coeff-compute-model')=='shared'}
        if not rows:
            if required[prefix]:raise ValueError('missing resource closure: '+prefix)
            continue
        row=rows[-1]
        if any(int(row[k]) for k in keys):
            raise ValueError('recovery leaked resources: '+prefix)
    return dict(cases=rows,aborts=list(visible.values()),result='passed',divider=divider,
                register_ports=register_ports,onchip_replay=onchip,
                data_commit=commit,drain_arb=drain_arb,warm_cancel=warm_cancel,rhs_bursts=bursts,direct_rhs=direct_rhs)
