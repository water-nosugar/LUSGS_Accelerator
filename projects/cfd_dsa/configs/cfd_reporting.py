"""CFD statistics/reporting, separated from gem5 system construction."""
import re
from types import SimpleNamespace


def dram_statistics(stats_text):
    """Sum physical channels, selecting each counter's final snapshot once."""
    values={}
    pattern=re.compile(r'^\s*(system\.(?:dram(?:\.dram)?|dram_ctrls\d+\.dram))\.(bytesRead|bytesWritten)::total\s+(\d+)',re.M)
    for match in pattern.finditer(stats_text):
        path,kind,value=match.groups()
        child=path.endswith('.dram') and path!='system.dram'
        controller=path.removesuffix('.dram') if child else path
        key=controller,kind
        if key not in values or child>=values[key][0]:values[key]=(child,int(value))
    result={}
    for kind,key in [('bytesRead','dram_bytes_read'),('bytesWritten','dram_bytes_written')]:
        found=[v for (path,k),(priority,v) in values.items() if k==kind]
        if found:result[key]=sum(found)
    if values:result['dram_reported_channels']=len({path for path,kind in values})
    return result

CONFIG_KEYS = ('local_spm_bank_granularity', 'local_spm_banks', 'local_spm_read_ports', 'patha_acc_mode', 'patha_async_store', 'patha_dotp_count', 'patha_dotp_lat', 'patha_input_buffer_depth', 'patha_kernel', 'patha_matld_extra_per_run', 'patha_pack_count', 'patha_result_buffer_depth', 'patha_spm_mode', 'patha_spm_read_width', 'patha_store_mode', 'patha_store_queue_depth', 'patha_store_stride', 'patha_stream_read_ports', 'patha_streaming', 'patha_unroll', 'readport_count', 'report_focus', 'sme_fmopa_array_lat', 'sme_fmopa_count', 'sme_fmopa_lat', 'sme_outer_count', 'sme_outer_full_array', 'sme_outer_lat', 'sme_pipe_count', 'sme_pipe_lat')
THEORY = {
    'N': 5,
    'lmat_bytes': 40,
    'FLOPs_per_matvec': 50,
    'spm_bytes_per_matvec': 240,       # 6 * 40B: 5 matrix rows + 1 vector row
    'result_store_bytes': 40,          # predicate st1d stores only 5 FP64 lanes
    'bytes_per_iter': 280,             # 240B SPM logical read + 40B result store
    'cfd_matld_per_matvec': 6,         # Scheme-B: vector also loaded with lmat5_spm row=0
    'cfd_dotp_per_matvec': 5,
    'cfd_pack_per_matvec': 1,
    'sme_fmopa_per_matvec': 5,
    'sme_zero_per_matvec': 1,
    'sme_mova_per_matvec': 1,
    'sme_outer_product_k_steps': 5,
    # Each batch invocation has one prologue (6 lmat5_spm) before the steady loop.
    'matld_prologue_per_batch': 6,
    'benchmark_batch_invocations': 1,  # stats reset leaves benchmark call only
    'dotp_matld_extra_per_run': 12,     # legacy binary includes correctness + benchmark prologues
    'dotp_result_store_bytes': 64,      # first method uses full 512-bit str z
    'CFD_instructions': 13,            # 6 lmat + 5 dotp + 1 pack + 1 store
    'cpu_freq_hz': 2e9,
    'cpu_period_ns': 0.5,
    'l1_latency_cycles': 2,
    'l2_latency_cycles': 6,
    'memory_latency_cycles': 100,
    'ideal_inline': 75,
    'ideal_pure_c': 100,
    'ideal_cfd_doc': 8,                # Documented v4.0 target
    'ideal_cfd_steady': 6,             # Single pipelined dotp engine lower-bound path
    # Benchmark currently runs a 2-matvec correctness pair before timed loop.
    'precheck_matvecs': 2,
    'sme_precheck_matvecs': 0,         # m5_reset_stats excludes correctness
    # Column-stream benchmark starts with x/col0/col1; every loop body carries
    # the remaining current-tile loads plus the next Ping head.
    'sme_matld_extra_per_run': 3,
}


class ReportContext:
    """One immutable-by-convention configuration snapshot per report run."""
    def __init__(self, config):
        self.config = SimpleNamespace(**{key: config[key] for key in CONFIG_KEYS})

    def calc_theoretical_cycles(self, approach='CFD', cache_level='L1'):
        if approach == 'CFD':
            if cache_level == 'L1':
                return THEORY['ideal_cfd_doc']
            elif cache_level == 'L2':
                return 8 + 10  # DMA overlap
            else:
                return 8 + 50  # DMA overlap
        elif approach == 'inline':
            if cache_level == 'L1':
                return 75
            return 200
        elif approach == 'pure_c':
            if cache_level == 'L1':
                return 100
            return 300
        return 100

    def _stat_value(self, stats_text, names, dtype=float):
        for name in names:
            pattern = (
                r'^\s*' + re.escape(name) +
                r'\s+([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)'
            )
            matches = re.findall(pattern, stats_text, re.MULTILINE)
            if not matches:
                continue
            value = float(matches[-1])
            return int(value) if dtype is int else value
        return None

    def _add_stat(self, stats, stats_text, key, names, dtype=float):
        value = self._stat_value(stats_text, names, dtype)
        if value is not None:
            stats[key] = value

    def analyze_stats(self, stats_text):
        stats = {}
        specs = {
            'sim_seconds': (['simSeconds'], float),
            'sim_ticks': (['simTicks', 'finalTick'], int),
            'total_cycles': (['system.cpu.numCycles'], int),
            'committed_insts': ([
                'system.cpu.commitStats0.numInsts',
                'system.cpu.thread_0.numInsts',
                'system.cpu.commit.committedInstType_0::total',
            ], int),
            'committed_ops': ([
                'system.cpu.commitStats0.numOps',
                'system.cpu.thread_0.numOps',
                'system.cpu.commit.committedInstType_0::total',
            ], int),
            'CPI': (['system.cpu.cpi'], float),
            'IPC': (['system.cpu.ipc'], float),
            'l1d_hits': (['system.l1d.overallHits::total'], int),
            'l1d_misses': (['system.l1d.overallMisses::total'], int),
            'l2_hits': (['system.l2.overallHits::total'], int),
            'l2_misses': (['system.l2.overallMisses::total'], int),
            'cfd_dotp_committed': ([
                'system.cpu.commit.committedInstType_0::CFDDSADotp',
                'system.cpu.commit.committedInstType::CFDDSADotp',
                'system.cpu.commitStats0.committedInstType::CFDDSADotp',
            ], int),
            'cfd_matld_committed': ([
                'system.cpu.commit.committedInstType_0::CFDDSAMatLd',
                'system.cpu.commit.committedInstType::CFDDSAMatLd',
                'system.cpu.commitStats0.committedInstType::CFDDSAMatLd',
            ], int),
            'cfd_pack_committed': ([
                'system.cpu.commit.committedInstType_0::CFDDSAPack',
                'system.cpu.commit.committedInstType::CFDDSAPack',
                'system.cpu.commitStats0.committedInstType::CFDDSAPack',
            ], int),
            'patha_stream_ld_committed': ([
                'system.cpu.commit.committedInstType_0::CFDDSAStreamLd',
                'system.cpu.commit.committedInstType::CFDDSAStreamLd',
                'system.cpu.commitStats0.committedInstType::CFDDSAStreamLd',
            ], int),
            'patha_store5_committed': ([
                'system.cpu.commit.committedInstType_0::CFDDSAStore5',
                'system.cpu.commit.committedInstType::CFDDSAStore5',
                'system.cpu.commitStats0.committedInstType::CFDDSAStore5',
            ], int),
            'sme_fmopa_committed': ([
                'system.cpu.commit.committedInstType_0::CFDSMEFmopaStep',
                'system.cpu.commit.committedInstType::CFDSMEFmopaStep',
                'system.cpu.commitStats0.committedInstType::CFDSMEFmopaStep',
            ], int),
            'sme_pipe_committed': ([
                'system.cpu.commit.committedInstType_0::CFDSMEPipeStep',
                'system.cpu.commit.committedInstType::CFDSMEPipeStep',
                'system.cpu.commitStats0.committedInstType::CFDSMEPipeStep',
            ], int),
            'sme_outer_committed': ([
                'system.cpu.commit.committedInstType_0::CFDSMEZaOuterStep',
                'system.cpu.commit.committedInstType::CFDSMEZaOuterStep',
                'system.cpu.commitStats0.committedInstType::CFDSMEZaOuterStep',
            ], int),
            'sme_zero_committed': ([
                'system.cpu.commit.committedInstType_0::Matrix',
                'system.cpu.commit.committedInstType::Matrix',
                'system.cpu.commitStats0.committedInstType::Matrix',
            ], int),
            'sme_mova_committed': ([
                'system.cpu.commit.committedInstType_0::MatrixMov',
                'system.cpu.commit.committedInstType::MatrixMov',
                'system.cpu.commitStats0.committedInstType::MatrixMov',
            ], int),
            'memwrite_committed': ([
                'system.cpu.commit.committedInstType_0::MemWrite',
                'system.cpu.commit.committedInstType::MemWrite',
                'system.cpu.commitStats0.committedInstType::MemWrite',
            ], int),
            'cfd_dotp_issued': (['system.cpu.issuedInstType_0::CFDDSADotp'], int),
            'cfd_matld_issued': (['system.cpu.issuedInstType_0::CFDDSAMatLd'], int),
            'cfd_pack_issued': (['system.cpu.issuedInstType_0::CFDDSAPack'], int),
            'patha_stream_ld_issued': (['system.cpu.issuedInstType_0::CFDDSAStreamLd'], int),
            'patha_store5_issued': (['system.cpu.issuedInstType_0::CFDDSAStore5'], int),
            'linebuf_issued': (['system.cpu.issuedInstType_0::CFDDSALineBuf'], int),
            'trsm5_mrhs_issued': (['system.cpu.issuedInstType_0::CFDDSATrsm5Mrhs'], int),
            'trsm5_inv_lbar_issued': (['system.cpu.issuedInstType_0::CFDDSATrsm5InvLbar'], int),
            'vec5_issued': (['system.cpu.issuedInstType_0::CFDDSAVec5'], int),
            'sme_fmopa_issued': (['system.cpu.issuedInstType_0::CFDSMEFmopaStep'], int),
            'sme_pipe_issued': (['system.cpu.issuedInstType_0::CFDSMEPipeStep'], int),
            'sme_outer_issued': (['system.cpu.issuedInstType_0::CFDSMEZaOuterStep'], int),
            'sme_zero_issued': (['system.cpu.issuedInstType_0::Matrix'], int),
            'sme_mova_issued': (['system.cpu.issuedInstType_0::MatrixMov'], int),
            'memwrite_issued': (['system.cpu.issuedInstType_0::MemWrite'], int),
            'intalu_issued': (['system.cpu.issuedInstType_0::IntAlu'], int),
            'fu_busy_dotp': (['system.cpu.statFuBusy::CFDDSADotp'], int),
            'fu_busy_matld': (['system.cpu.statFuBusy::CFDDSAMatLd'], int),
            'fu_busy_pack': (['system.cpu.statFuBusy::CFDDSAPack'], int),
            'fu_busy_patha_stream_ld': (['system.cpu.statFuBusy::CFDDSAStreamLd'], int),
            'fu_busy_patha_store5': (['system.cpu.statFuBusy::CFDDSAStore5'], int),
            'fu_busy_linebuf': (['system.cpu.statFuBusy::CFDDSALineBuf'], int),
            'fu_busy_trsm5_mrhs': (['system.cpu.statFuBusy::CFDDSATrsm5Mrhs'], int),
            'fu_busy_trsm5_inv_lbar': (['system.cpu.statFuBusy::CFDDSATrsm5InvLbar'], int),
            'fu_busy_vec5': (['system.cpu.statFuBusy::CFDDSAVec5'], int),
            'fu_busy_sme_fmopa': (['system.cpu.statFuBusy::CFDSMEFmopaStep'], int),
            'fu_busy_sme_pipe': (['system.cpu.statFuBusy::CFDSMEPipeStep'], int),
            'fu_busy_sme_outer': (['system.cpu.statFuBusy::CFDSMEZaOuterStep'], int),
            'fu_busy_sme_zero': (['system.cpu.statFuBusy::Matrix'], int),
            'fu_busy_sme_mova': (['system.cpu.statFuBusy::MatrixMov'], int),
            'fu_busy_memwrite': (['system.cpu.statFuBusy::MemWrite'], int),
            'fu_busy_intalu': (['system.cpu.statFuBusy::IntAlu'], int),
            'fu_busy_total': (['system.cpu.fuBusy'], int),
            'nonspec_added': (['system.cpu.nonSpecInstsAdded'], int),
            'disp_nonspec': (['system.cpu.iew.dispNonSpecInsts'], int),
            'rename_serializing': (['system.cpu.rename.serializing'], int),
            'rename_serialize_stall_cycles': (
                ['system.cpu.rename.status::SerializeStall'], int),
            'issue_dist_samples': (['system.cpu.numIssuedDist::samples'], int),
            'issue_dist_0': (['system.cpu.numIssuedDist::0'], int),
            'issue_dist_1': (['system.cpu.numIssuedDist::1'], int),
            'issue_dist_2': (['system.cpu.numIssuedDist::2'], int),
            'issue_dist_3': (['system.cpu.numIssuedDist::3'], int),
            'rename_blocked_cycles': (['system.cpu.rename.status::Blocked'], int),
            'dispatch_blocked_cycles': (['system.cpu.iew.dispatchStatus::blocked'],
                                        int),
            'rename_iq_full': (['system.cpu.rename.IQFullEvents'], int),
            'rename_rob_full': (['system.cpu.rename.ROBFullEvents'], int),
            'iew_iq_full': (['system.cpu.iew.iqFullEvents'], int),
            'iew_lsq_full': (['system.cpu.iew.lsqFullEvents'], int),
            'lsq_blocked_by_cache': (['system.cpu.lsq0.blockedByCache'], int),
            'commit_bw_full': (['system.cpu.commit.commitEligibleSamples'], int),
            'branches_executed': (['system.cpu.executeStats0.numBranches'], int),
            'branches_fetched': (['system.cpu.fetchStats0.numBranches'], int),
            'spm_bytes_read': (['system.spm.bytesRead::total'], int),
            'spm_bytes_written': (['system.spm.bytesWritten::total'], int),
            'spm_reads': (['system.spm.numReads::total'], int),
            'spm_writes': (['system.spm.numWrites::total'], int),
            'spm_bw_read': (['system.spm.bwRead::total'], float),
            'spm_bw_write': (['system.spm.bwWrite::total'], float),
            'spm_bw_total': (['system.spm.bwTotal::total'], float),
            'local_spm_bytes_read': (['local_spm.bytesRead', 'system.cpu.local_spm.bytesRead'], int),
            'local_spm_logical_bytes_read': (['local_spm.logicalBytesRead', 'system.cpu.local_spm.logicalBytesRead'], int),
            'local_spm_physical_bytes_read': (['local_spm.physicalBytesRead', 'system.cpu.local_spm.physicalBytesRead'], int),
            'local_spm_reads': (['local_spm.numReads', 'system.cpu.local_spm.numReads'], int),
            'local_spm_read_latency_total': (['local_spm.readLatencyTotal', 'system.cpu.local_spm.readLatencyTotal'], int),
            'local_spm_avg_read_latency': (['local_spm.avgReadLatency', 'system.cpu.local_spm.avgReadLatency'], float),
            'local_spm_max_read_latency': (['local_spm.maxReadLatency', 'system.cpu.local_spm.maxReadLatency'], int),
            'local_spm_port_busy_cycles': (['local_spm.portBusyCycles', 'system.cpu.local_spm.portBusyCycles'], int),
            'local_spm_port_conflict_cycles': (['local_spm.portConflictCycles', 'system.cpu.local_spm.portConflictCycles'], int),
            'local_spm_bank_conflict_cycles': (['local_spm.bankConflictCycles', 'system.cpu.local_spm.bankConflictCycles'], int),
            'local_spm_queue_full_cycles': (['local_spm.queueFullCycles', 'system.cpu.local_spm.queueFullCycles'], int),
            'local_spm_z_wb_conflict_cycles': (['local_spm.zWritebackConflictCycles', 'system.cpu.local_spm.zWritebackConflictCycles'], int),
            'local_spm_request_to_response_total': (['local_spm.requestToResponseTotal', 'system.cpu.local_spm.requestToResponseTotal'], int),
            'local_spm_avg_request_to_response': (['local_spm.avgRequestToResponse', 'system.cpu.local_spm.avgRequestToResponse'], float),
            'local_spm_response_to_zwb_total': (['local_spm.responseToZWritebackTotal', 'system.cpu.local_spm.responseToZWritebackTotal'], int),
            'local_spm_avg_zwb_wait': (['local_spm.avgZWritebackWait', 'system.cpu.local_spm.avgZWritebackWait'], float),
            'local_spm_max_zwb_wait': (['local_spm.maxZWritebackWait', 'system.cpu.local_spm.maxZWritebackWait'], int),
            'linebuf_forward_reads_model': (['system.cpu.local_spm.linebufForwardReads'], int),
            'linebuf_forward_writes_model': (['system.cpu.local_spm.linebufForwardWrites'], int),
            'linebuf_backward_reads_model': (['system.cpu.local_spm.linebufBackwardReads'], int),
            'linebuf_backward_writes_model': (['system.cpu.local_spm.linebufBackwardWrites'], int),
            'linebuf_forward_hits_model': (['system.cpu.local_spm.linebufForwardHits'], int),
            'linebuf_forward_misses_model': (['system.cpu.local_spm.linebufForwardMisses'], int),
            'linebuf_backward_hits_model': (['system.cpu.local_spm.linebufBackwardHits'], int),
            'linebuf_backward_misses_model': (['system.cpu.local_spm.linebufBackwardMisses'], int),
            'linebuf_tag_conflicts_model': (['system.cpu.local_spm.linebufTagConflicts'], int),
            'linebuf_invalid_reads_model': (['system.cpu.local_spm.linebufInvalidReads'], int),
            'linebuf_stall_cycles_model': (['system.cpu.local_spm.linebufStallCycles'], int),
            'trsm5_mrhs_completed_model': (['system.cpu.local_spm.trsm5MrhsCompleted'], int),
            'trsm5_mrhs_busy_cycles_model': (['system.cpu.local_spm.trsm5MrhsBusyCycles'], int),
            'trsm5_inv_lbar_issued_model': (['system.cpu.local_spm.trsm5InvLbarIssued'], int),
            'trsm5_inv_lbar_completed_model': (['system.cpu.local_spm.trsm5InvLbarCompleted'], int),
            'trsm5_inv_lbar_busy_cycles_model': (['system.cpu.local_spm.trsm5InvLbarBusyCycles'], int),
            'trsm5_inv_lbar_stall_cycles_model': (['system.cpu.local_spm.trsm5InvLbarStallCycles'], int),
            'trsm5_inv_lbar_lu_reads_model': (['system.cpu.local_spm.trsm5InvLbarLuReads'], int),
            'trsm5_inv_lbar_columns_model': (['system.cpu.local_spm.trsm5InvLbarColumnsSolved'], int),
            'local_spm_outstanding_avg': (['local_spm.outstandingReadsAvg', 'system.cpu.local_spm.outstandingReadsAvg'], float),
            'local_spm_outstanding_max': (['local_spm.outstandingReadsMax', 'system.cpu.local_spm.outstandingReadsMax'], int),
            'local_spm_reads_per_cycle_model': (['local_spm.readsPerCycle', 'system.cpu.local_spm.readsPerCycle'], float),
            'local_spm_bytes_per_cycle_model': (['local_spm.bytesPerCycle', 'system.cpu.local_spm.bytesPerCycle'], float),
            'local_spm_logical_bytes_per_cycle_model': (['local_spm.logicalBytesPerCycle', 'system.cpu.local_spm.logicalBytesPerCycle'], float),
            'local_spm_physical_bytes_per_cycle_model': (['local_spm.physicalBytesPerCycle', 'system.cpu.local_spm.physicalBytesPerCycle'], float),
            'local_event_created': (['local_spm.localEventRequestsCreated', 'system.cpu.local_spm.localEventRequestsCreated'], int),
            'local_event_accepted': (['local_spm.localEventRequestsAccepted', 'system.cpu.local_spm.localEventRequestsAccepted'], int),
            'local_event_completed': (['local_spm.localEventRequestsCompleted', 'system.cpu.local_spm.localEventRequestsCompleted'], int),
            'local_event_accept_events': (['local_spm.localEventAcceptEvents', 'system.cpu.local_spm.localEventAcceptEvents'], int),
            'local_event_read_start_events': (['local_spm.localEventReadStartEvents', 'system.cpu.local_spm.localEventReadStartEvents'], int),
            'local_event_response_events': (['local_spm.localEventResponseEvents', 'system.cpu.local_spm.localEventResponseEvents'], int),
            'local_event_zwb_events': (['local_spm.localEventZwbEvents', 'system.cpu.local_spm.localEventZwbEvents'], int),
            'local_event_complete_events': (['local_spm.localEventCompleteEvents', 'system.cpu.local_spm.localEventCompleteEvents'], int),
            'local_event_retry_events': (['local_spm.localEventRetryEvents', 'system.cpu.local_spm.localEventRetryEvents'], int),
            'local_event_queue_full_events': (['local_spm.localEventQueueFullEvents', 'system.cpu.local_spm.localEventQueueFullEvents'], int),
            'local_event_outstanding_full_events': (['local_spm.localEventOutstandingFullEvents', 'system.cpu.local_spm.localEventOutstandingFullEvents'], int),
            'local_event_live_max': (['local_spm.localEventLiveRequestsMax', 'system.cpu.local_spm.localEventLiveRequestsMax'], int),
            'local_event_live_end': (['local_spm.localEventLiveRequestsEnd', 'system.cpu.local_spm.localEventLiveRequestsEnd'], int),
            'local_event_avg_issue_to_accept': (['local_spm.localEventAvgIssueToAccept', 'system.cpu.local_spm.localEventAvgIssueToAccept'], float),
            'local_event_avg_accept_to_read': (['local_spm.localEventAvgAcceptToReadStart', 'system.cpu.local_spm.localEventAvgAcceptToReadStart'], float),
            'local_event_avg_read_to_response': (['local_spm.localEventAvgReadStartToResponse', 'system.cpu.local_spm.localEventAvgReadStartToResponse'], float),
            'local_event_avg_response_to_zwb': (['local_spm.localEventAvgResponseToZwb', 'system.cpu.local_spm.localEventAvgResponseToZwb'], float),
            'local_event_avg_issue_to_complete': (['local_spm.localEventAvgIssueToComplete', 'system.cpu.local_spm.localEventAvgIssueToComplete'], float),
            'vector_zwb_requests': (['local_spm.vectorZwbRequests', 'system.cpu.local_spm.vectorZwbRequests'], int),
            'vector_zwb_lmat_requests': (['local_spm.vectorZwbLmatRequests', 'system.cpu.local_spm.vectorZwbLmatRequests'], int),
            'vector_zwb_mova_requests': (['local_spm.vectorZwbMovaRequests', 'system.cpu.local_spm.vectorZwbMovaRequests'], int),
            'vector_zwb_conflicts': (['local_spm.vectorZwbConflicts', 'system.cpu.local_spm.vectorZwbConflicts'], int),
            'vector_zwb_conflict_cycles': (['local_spm.vectorZwbConflictCycles', 'system.cpu.local_spm.vectorZwbConflictCycles'], int),
            'vector_zwb_avg_wait': (['local_spm.vectorZwbAvgWait', 'system.cpu.local_spm.vectorZwbAvgWait'], float),
            'vector_zwb_max_wait': (['local_spm.vectorZwbMaxWait', 'system.cpu.local_spm.vectorZwbMaxWait'], int),
            'patha_buffer_allocs': ([
                'patha.bufferAllocs',
                'system.cpu.patha.bufferAllocs',
                'system.cpu.local_spm.pathAResultBufferAllocs',
            ], int),
            'patha_buffer_frees': ([
                'patha.bufferFrees',
                'system.cpu.patha.bufferFrees',
                'system.cpu.local_spm.pathAResultBufferFrees',
            ], int),
            'patha_buffer_writes': ([
                'patha.bufferWrites',
                'system.cpu.patha.bufferWrites',
                'system.cpu.local_spm.pathAResultBufferWrites',
            ], int),
            'patha_buffer_packs': ([
                'patha.bufferPacks',
                'system.cpu.patha.bufferPacks',
                'system.cpu.local_spm.pathAResultBufferPacks',
            ], int),
            'patha_buffer_full_stalls': ([
                'patha.bufferFullStalls',
                'system.cpu.patha.bufferFullStalls',
                'system.cpu.local_spm.pathAResultBufferFullStalls',
            ], int),
            'patha_buffer_live_max': ([
                'patha.bufferLiveMax',
                'system.cpu.patha.bufferLiveMax',
                'system.cpu.local_spm.pathAResultBufferLiveMax',
            ], int),
            'patha_buffer_live_end': ([
                'patha.bufferLiveEnd',
                'system.cpu.patha.bufferLiveEnd',
                'system.cpu.local_spm.pathAResultBufferLiveEnd',
            ], int),
            'patha_buffer_overlap_cycles': ([
                'patha.bufferOverlapCycles',
                'system.cpu.patha.bufferOverlapCycles',
                'system.cpu.local_spm.pathAResultBufferOverlapCycles',
            ], int),
            'patha_buffer_pack_while_dotp_cycles': ([
                'patha.bufferPackWhileDotpCycles',
                'system.cpu.patha.bufferPackWhileDotpCycles',
                'system.cpu.local_spm.pathAResultBufferPackWhileDotpCycles',
            ], int),
            'patha_dotp_issued_model': ([
                'pathA.dotpIssued',
                'system.cpu.pathA.dotpIssued',
                'system.cpu.local_spm.pathADotpIssued',
            ], int),
            'patha_dotp_issue_cycles': ([
                'pathA.dotpIssueCycles',
                'system.cpu.pathA.dotpIssueCycles',
                'system.cpu.local_spm.pathADotpIssueCycles',
            ], int),
            'patha_dotp_issue_gaps': ([
                'pathA.dotpIssueGaps',
                'system.cpu.pathA.dotpIssueGaps',
                'system.cpu.local_spm.pathADotpIssueGaps',
            ], int),
            'patha_dotp_max_issue_gap': ([
                'pathA.maxDotpIssueGap',
                'system.cpu.pathA.maxDotpIssueGap',
                'system.cpu.local_spm.pathAMaxDotpIssueGap',
            ], int),
            'patha_dotp_burst_total': ([
                'pathA.consecutiveDotpBurstTotal',
                'system.cpu.pathA.consecutiveDotpBurstTotal',
                'system.cpu.local_spm.pathAConsecutiveDotpBurstTotal',
            ], int),
            'patha_dotp_burst_max': ([
                'pathA.consecutiveDotpBurstMax',
                'system.cpu.pathA.consecutiveDotpBurstMax',
                'system.cpu.local_spm.pathAConsecutiveDotpBurstMax',
            ], int),
            'patha_dotp_burst_avg': ([
                'pathA.consecutiveDotpBurstAvg',
                'system.cpu.pathA.consecutiveDotpBurstAvg',
                'system.cpu.local_spm.pathAConsecutiveDotpBurstAvg',
            ], float),
            'patha_dotp_blocked_fu_busy': ([
                'pathA.dotpBlockedByFuBusy',
                'system.cpu.pathA.dotpBlockedByFuBusy',
                'system.cpu.local_spm.pathADotpBlockedByFuBusy',
            ], int),
            'patha_dotp_blocked_input_not_ready': ([
                'pathA.dotpBlockedByInputNotReady',
                'system.cpu.pathA.dotpBlockedByInputNotReady',
                'system.cpu.local_spm.pathADotpBlockedByInputNotReady',
            ], int),
            'patha_dotp_blocked_result_buffer_full': ([
                'pathA.dotpBlockedByResultBufferFull',
                'system.cpu.pathA.dotpBlockedByResultBufferFull',
                'system.cpu.local_spm.pathADotpBlockedByResultBufferFull',
            ], int),
            'patha_dotp_blocked_pack_store': ([
                'pathA.dotpBlockedByPackStoreBackpressure',
                'system.cpu.pathA.dotpBlockedByPackStoreBackpressure',
                'system.cpu.local_spm.pathADotpBlockedByPackStoreBackpressure',
            ], int),
            'patha_dotp_ready_but_not_issued': ([
                'pathA.dotpReadyButNotIssued',
                'system.cpu.pathA.dotpReadyButNotIssued',
                'system.cpu.local_spm.pathADotpReadyButNotIssued',
            ], int),
            'patha_early_dotp_start_events': ([
                'pathA.earlyDotpStartEvents',
                'system.cpu.local_spm.pathAEarlyDotpStartEvents',
            ], int),
            'patha_dotp_before_all_fields_ready': ([
                'pathA.dotpIssuedBeforeAllFieldsReady',
                'system.cpu.local_spm.pathADotpIssuedBeforeAllFieldsReady',
            ], int),
            'patha_early_ready_opportunities': ([
                'pathA.earlyReadyOpportunities',
                'system.cpu.local_spm.pathAEarlyReadyOpportunities',
            ], int),
            'patha_early_ready_issued': ([
                'pathA.earlyReadyIssued',
                'system.cpu.local_spm.pathAEarlyReadyIssued',
            ], int),
            'patha_early_ready_execute_started': ([
                'pathA.earlyReadyExecuteStarted',
                'system.cpu.local_spm.pathAEarlyReadyExecuteStarted',
            ], int),
            'patha_early_ready_missed': ([
                'pathA.earlyReadyMissed',
                'system.cpu.local_spm.pathAEarlyReadyMissed',
            ], int),
            'patha_early_ready_efficiency': ([
                'pathA.earlyReadyIssueEfficiency',
                'system.cpu.local_spm.pathAEarlyReadyIssueEfficiency',
            ], float),
            'patha_early_missed_not_in_iq': ([
                'pathA.earlyMissedDueToNotInIQ',
                'system.cpu.local_spm.pathAEarlyMissedDueToNotInIQ',
            ], int),
            'patha_early_missed_fu_busy': ([
                'pathA.earlyMissedDueToDotpFuBusy',
                'system.cpu.local_spm.pathAEarlyMissedDueToDotpFuBusy',
            ], int),
            'patha_early_missed_issue_width': ([
                'pathA.earlyMissedDueToIssueWidth',
                'system.cpu.local_spm.pathAEarlyMissedDueToIssueWidth',
            ], int),
            'patha_early_missed_result_busy': ([
                'pathA.earlyMissedDueToResultSlotBusy',
                'system.cpu.local_spm.pathAEarlyMissedDueToResultSlotBusy',
            ], int),
            'patha_early_missed_token_not_ready': ([
                'pathA.earlyMissedDueToTokenNotReady',
                'system.cpu.local_spm.pathAEarlyMissedDueToTokenNotReady',
            ], int),
            'patha_early_missed_other': ([
                'pathA.earlyMissedDueToOther',
                'system.cpu.local_spm.pathAEarlyMissedDueToOther',
            ], int),
            'patha_dotp_dep_ready_before_all': ([
                'pathA.dotpDependencyReadyBeforeAllFields',
                'system.cpu.local_spm.pathADotpDependencyReadyBeforeAllFields',
            ], int),
            'patha_dotp_execute_before_all': ([
                'pathA.dotpExecuteStartedBeforeAllFields',
                'system.cpu.local_spm.pathADotpExecuteStartedBeforeAllFields',
            ], int),
            'patha_dotp_complete_before_all': ([
                'pathA.dotpCompletedBeforeAllFields',
                'system.cpu.local_spm.pathADotpCompletedBeforeAllFields',
            ], int),
            'patha_load_compute_overlap_cycles': ([
                'pathA.loadComputeOverlapCycles',
                'system.cpu.local_spm.pathALoadComputeOverlapCycles',
            ], int),
            'patha_field_ready_wait_cycles': ([
                'pathA.fieldReadyWaitCycles',
                'system.cpu.local_spm.pathAFieldReadyWaitCycles',
            ], int),
            'patha_whole_slot_barrier_stall_cycles': ([
                'pathA.wholeSlotBarrierStallCycles',
                'system.cpu.local_spm.pathAWholeSlotBarrierStallCycles',
            ], int),
            'patha_stream_port0_busy_cycles': ([
                'pathA.streamReadPort0BusyCycles',
                'system.cpu.local_spm.pathAStreamReadPort0BusyCycles',
            ], int),
            'patha_stream_port1_busy_cycles': ([
                'pathA.streamReadPort1BusyCycles',
                'system.cpu.local_spm.pathAStreamReadPort1BusyCycles',
            ], int),
            'patha_dual_load_issue_cycles': ([
                'pathA.dualLoadIssueCycles',
                'system.cpu.local_spm.pathADualLoadIssueCycles',
            ], int),
            'patha_dotp_avg_after_field_ready': ([
                'pathA.dotpAvgAfterFieldReady',
                'system.cpu.local_spm.pathADotpAvgAfterFieldReady',
            ], float),
            'patha_dotp_avg_after_vector_ready': ([
                'pathA.dotpAvgAfterVectorReady',
                'system.cpu.local_spm.pathADotpAvgAfterVectorReady',
            ], float),
            'patha_input_buffer_allocs': ([
                'pathA.inputBufferAllocs',
                'system.cpu.pathA.inputBufferAllocs',
                'system.cpu.local_spm.pathAInputBufferAllocs',
            ], int),
            'patha_input_buffer_frees': ([
                'pathA.inputBufferFrees',
                'system.cpu.pathA.inputBufferFrees',
                'system.cpu.local_spm.pathAInputBufferFrees',
            ], int),
            'patha_input_buffer_full_stalls': ([
                'pathA.inputBufferFullStalls',
                'system.cpu.pathA.inputBufferFullStalls',
                'system.cpu.local_spm.pathAInputBufferFullStalls',
            ], int),
            'patha_input_buffer_live_max': ([
                'pathA.inputBufferLiveMax',
                'system.cpu.pathA.inputBufferLiveMax',
                'system.cpu.local_spm.pathAInputBufferLiveMax',
            ], int),
            'patha_async_store_enqueues': ([
                'pathA.asyncStoreEnqueues',
                'system.cpu.pathA.asyncStoreEnqueues',
                'system.cpu.local_spm.pathAAsyncStoreEnqueues',
            ], int),
            'patha_async_store_commits': ([
                'pathA.asyncStoreCommits',
                'system.cpu.pathA.asyncStoreCommits',
                'system.cpu.local_spm.pathAAsyncStoreCommits',
            ], int),
            'patha_async_store_queue_full_stalls': ([
                'pathA.asyncStoreQueueFullStalls',
                'system.cpu.pathA.asyncStoreQueueFullStalls',
                'system.cpu.local_spm.pathAAsyncStoreQueueFullStalls',
            ], int),
            'patha_async_store_live_max': ([
                'pathA.asyncStoreLiveMax',
                'system.cpu.pathA.asyncStoreLiveMax',
                'system.cpu.local_spm.pathAAsyncStoreLiveMax',
            ], int),
            'za_tokens_produced': ([
                'za.tokensProduced',
                'system.cpu.za.tokensProduced',
                'system.cpu.local_spm.zaTokensProduced',
            ], int),
            'za_tokens_consumed': ([
                'za.tokensConsumed',
                'system.cpu.za.tokensConsumed',
                'system.cpu.local_spm.zaTokensConsumed',
            ], int),
            'za_pending_updates': ([
                'za.pendingUpdates',
                'system.cpu.za.pendingUpdates',
                'system.cpu.local_spm.zaPendingUpdates',
            ], int),
            'za_pending_queue_full': ([
                'za.pendingQueueFull',
                'system.cpu.za.pendingQueueFull',
                'system.cpu.local_spm.zaPendingQueueFull',
            ], int),
            'za_squash_discards': ([
                'za.squashDiscards',
                'system.cpu.za.squashDiscards',
                'system.cpu.local_spm.zaSquashDiscards',
            ], int),
            'za_commit_updates': ([
                'za.commitUpdates',
                'system.cpu.za.commitUpdates',
                'system.cpu.local_spm.zaCommitUpdates',
            ], int),
            'za_mova_wait_cycles': ([
                'za.movaWaitCycles',
                'system.cpu.za.movaWaitCycles',
                'system.cpu.local_spm.zaMovaWaitCycles',
            ], int),
            'za_next_zero_wait_cycles': ([
                'za.nextZeroWaitCycles',
                'system.cpu.za.nextZeroWaitCycles',
                'system.cpu.local_spm.zaNextZeroWaitCycles',
            ], int),
            'za_forwarding_stalls': ([
                'za.forwardingStalls',
                'system.cpu.za.forwardingStalls',
                'system.cpu.local_spm.zaForwardingStalls',
            ], int),
            'za_rename_allocs': ([
                'za.renameAllocs',
                'system.cpu.za.renameAllocs',
                'system.cpu.local_spm.zaRenameAllocs',
            ], int),
            'za_rename_frees': ([
                'za.renameFrees',
                'system.cpu.za.renameFrees',
                'system.cpu.local_spm.zaRenameFrees',
            ], int),
            'za_rename_rollbacks': ([
                'za.renameRollbacks',
                'system.cpu.za.renameRollbacks',
                'system.cpu.local_spm.zaRenameRollbacks',
            ], int),
            'za_physical_live_max': ([
                'za.physicalLiveMax',
                'system.cpu.za.physicalLiveMax',
                'system.cpu.local_spm.zaPhysicalLiveMax',
            ], int),
            'za_physical_live_end': ([
                'za.physicalLiveEnd',
                'system.cpu.za.physicalLiveEnd',
                'system.cpu.local_spm.zaPhysicalLiveEnd',
            ], int),
            'za_mova_rename_hits': ([
                'za.movaRenameHits',
                'system.cpu.za.movaRenameHits',
                'system.cpu.local_spm.zaMovaRenameHits',
            ], int),
            'za_mova_arch_fallbacks': ([
                'za.movaArchFallbacks',
                'system.cpu.za.movaArchFallbacks',
                'system.cpu.local_spm.zaMovaArchFallbacks',
            ], int),
            'za_mova_blocked_not_ready': ([
                'za.movaBlockedNotReady',
                'system.cpu.za.movaBlockedNotReady',
                'system.cpu.local_spm.zaMovaBlockedNotReady',
            ], int),
            'za_mova_reads': ([
                'za.movaReads',
                'system.cpu.za.movaReads',
                'system.cpu.local_spm.zaMovaReads',
            ], int),
            'pathb_pipe_steps': ([
                'pathB.pipeSteps',
                'system.cpu.local_spm.pathBPipeSteps',
            ], int),
            'pathb_pipe_pending_updates': ([
                'pathB.pipePendingUpdates',
                'system.cpu.local_spm.pathBPipePendingUpdates',
            ], int),
            'pathb_pipe_commit_updates': ([
                'pathB.pipeCommitUpdates',
                'system.cpu.local_spm.pathBPipeCommitUpdates',
            ], int),
            'pathb_pipe_squash_discards': ([
                'pathB.pipeSquashDiscards',
                'system.cpu.local_spm.pathBPipeSquashDiscards',
            ], int),
            'pathb_pipe_col0_writes': ([
                'pathB.pipeCol0Writes',
                'system.cpu.local_spm.pathBPipeCol0Writes',
            ], int),
            'pathb_pipe_col1_writes': ([
                'pathB.pipeCol1Writes',
                'system.cpu.local_spm.pathBPipeCol1Writes',
            ], int),
            'pathb_pipe_col2_writes': ([
                'pathB.pipeCol2Writes',
                'system.cpu.local_spm.pathBPipeCol2Writes',
            ], int),
            'pathb_pipe_col3_writes': ([
                'pathB.pipeCol3Writes',
                'system.cpu.local_spm.pathBPipeCol3Writes',
            ], int),
            'pathb_pipe_col4_writes': ([
                'pathB.pipeCol4Writes',
                'system.cpu.local_spm.pathBPipeCol4Writes',
            ], int),
            'pathb_pipe_final_reads': ([
                'pathB.pipeFinalReads',
                'system.cpu.local_spm.pathBPipeFinalReads',
            ], int),
            'pathb_pipe_forward_hits': ([
                'pathB.pipeForwardHits',
                'system.cpu.local_spm.pathBPipeForwardHits',
            ], int),
            'pathb_pipe_forward_stalls': ([
                'pathB.pipeForwardStalls',
                'system.cpu.local_spm.pathBPipeForwardStalls',
            ], int),
            'pathc_outer_steps': ([
                'pathC.outerSteps',
                'system.cpu.local_spm.pathCOuterSteps',
            ], int),
            'pathc_outer_pending_updates': ([
                'pathC.outerPendingUpdates',
                'system.cpu.local_spm.pathCOuterPendingUpdates',
            ], int),
            'pathc_outer_commit_updates': ([
                'pathC.outerCommitUpdates',
                'system.cpu.local_spm.pathCOuterCommitUpdates',
            ], int),
            'pathc_outer_squash_discards': ([
                'pathC.outerSquashDiscards',
                'system.cpu.local_spm.pathCOuterSquashDiscards',
            ], int),
            'pathc_outer_col0_writes': ([
                'pathC.outerCol0Writes',
                'system.cpu.local_spm.pathCOuterCol0Writes',
            ], int),
            'pathc_outer_col1_writes': ([
                'pathC.outerCol1Writes',
                'system.cpu.local_spm.pathCOuterCol1Writes',
            ], int),
            'pathc_outer_col2_writes': ([
                'pathC.outerCol2Writes',
                'system.cpu.local_spm.pathCOuterCol2Writes',
            ], int),
            'pathc_outer_col3_writes': ([
                'pathC.outerCol3Writes',
                'system.cpu.local_spm.pathCOuterCol3Writes',
            ], int),
            'pathc_outer_col4_writes': ([
                'pathC.outerCol4Writes',
                'system.cpu.local_spm.pathCOuterCol4Writes',
            ], int),
            'pathc_outer_final_reads': ([
                'pathC.outerFinalReads',
                'system.cpu.local_spm.pathCOuterFinalReads',
            ], int),
            'pathc_outer_forward_hits': ([
                'pathC.outerForwardHits',
                'system.cpu.local_spm.pathCOuterForwardHits',
            ], int),
            'pathc_outer_forward_stalls': ([
                'pathC.outerForwardStalls',
                'system.cpu.local_spm.pathCOuterForwardStalls',
            ], int),
            'pathc_outer_mova_wait_cycles': ([
                'pathC.outerMovaWaitCycles',
                'system.cpu.local_spm.pathCOuterMovaWaitCycles',
            ], int),
            'pathc_outer_next_zero_wait_cycles': ([
                'pathC.outerNextZeroWaitCycles',
                'system.cpu.local_spm.pathCOuterNextZeroWaitCycles',
            ], int),
            'pathc_early_outer_start_events': ([
                'pathC.earlyOuterStartEvents',
                'system.cpu.local_spm.pathCEarlyOuterStartEvents',
            ], int),
            'pathc_step_before_all_columns_ready': ([
                'pathC.stepIssuedBeforeAllColumnsReady',
                'system.cpu.local_spm.pathCStepIssuedBeforeAllColumnsReady',
            ], int),
            'pathc_outer_fu_issue_count': ([
                'pathC.outerFuIssueCount',
                'system.cpu.local_spm.pathCOuterFuIssueCount',
            ], int),
            'pathc_outer_fu_busy_cycles': ([
                'pathC.outerFuBusyCycles',
                'system.cpu.local_spm.pathCOuterFuBusyCycles',
            ], int),
            'pathc_outer_fu_utilization': ([
                'pathC.outerFuUtilization',
                'system.cpu.local_spm.pathCOuterFuUtilization',
            ], float),
            'pathc_load_outer_overlap_cycles': ([
                'pathC.loadOuterOverlapCycles',
                'system.cpu.local_spm.pathCLoadOuterOverlapCycles',
            ], int),
            'pathc_column_ready_wait_cycles': ([
                'pathC.columnReadyWaitCycles',
                'system.cpu.local_spm.pathCColumnReadyWaitCycles',
            ], int),
            'pathc_za_token_wait_cycles': ([
                'pathC.zaTokenWaitCycles',
                'system.cpu.local_spm.pathCZaTokenWaitCycles',
            ], int),
            'pathc_mova_boundary_wait_cycles': ([
                'pathC.movaBoundaryWaitCycles',
                'system.cpu.local_spm.pathCMovaBoundaryWaitCycles',
            ], int),
            'pathc_dual_load_issue_cycles': ([
                'pathC.dualLoadIssueCycles',
                'system.cpu.local_spm.pathCDualLoadIssueCycles',
            ], int),
            'pathc_lmat_port0_busy_cycles': ([
                'pathC.lmatReadPort0BusyCycles',
                'system.cpu.local_spm.pathCLmatReadPort0BusyCycles',
            ], int),
            'pathc_lmat_port1_busy_cycles': ([
                'pathC.lmatReadPort1BusyCycles',
                'system.cpu.local_spm.pathCLmatReadPort1BusyCycles',
            ], int),
            'pathc_step_avg_after_column_ready': ([
                'pathC.stepAvgAfterColumnReady',
                'system.cpu.local_spm.pathCStepAvgAfterColumnReady',
            ], float),
            'pathc_step_avg_after_vector_ready': ([
                'pathC.stepAvgAfterVectorReady',
                'system.cpu.local_spm.pathCStepAvgAfterVectorReady',
            ], float),
            'trsv5_issued': ([
                'trsv5.issued',
                'system.cpu.local_spm.trsv5Issued',
            ], int),
            'trsv5_completed': ([
                'trsv5.completed',
                'system.cpu.local_spm.trsv5Completed',
            ], int),
            'trsv5_squashed': ([
                'trsv5.squashed',
                'system.cpu.local_spm.trsv5Squashed',
            ], int),
            'trsv5_busy_cycles': ([
                'trsv5.busyCycles',
                'system.cpu.local_spm.trsv5BusyCycles',
            ], int),
            'trsv5_idle_cycles': ([
                'trsv5.idleCycles',
                'system.cpu.local_spm.trsv5IdleCycles',
            ], int),
            'trsv5_full_stall_cycles': ([
                'trsv5.fullStallCycles',
                'system.cpu.local_spm.trsv5FullStallCycles',
            ], int),
            'trsv5_div_ops': ([
                'trsv5.divOps',
                'system.cpu.local_spm.trsv5DivOps',
            ], int),
            'trsv5_forward_mulsub_ops': ([
                'trsv5.forwardMulSubOps',
                'system.cpu.local_spm.trsv5ForwardMulSubOps',
            ], int),
            'trsv5_backward_mulsub_ops': ([
                'trsv5.backwardMulSubOps',
                'system.cpu.local_spm.trsv5BackwardMulSubOps',
            ], int),
            'trsv5_total_mulsub_ops': ([
                'trsv5.totalMulSubOps',
                'system.cpu.local_spm.trsv5TotalMulSubOps',
            ], int),
            'trsv5_input_matrix_bytes': ([
                'trsv5.inputMatrixBytes',
                'system.cpu.local_spm.trsv5InputMatrixBytes',
            ], int),
            'trsv5_input_vector_bytes': ([
                'trsv5.inputVectorBytes',
                'system.cpu.local_spm.trsv5InputVectorBytes',
            ], int),
            'trsv5_output_bytes': ([
                'trsv5.outputBytes',
                'system.cpu.local_spm.trsv5OutputBytes',
            ], int),
            'trsv5_latency_configured': ([
                'trsv5.latencyConfigured',
                'system.cpu.local_spm.trsv5LatencyConfigured',
            ], int),
            'trsv5_average_observed_latency': ([
                'trsv5.averageObservedLatency',
                'system.cpu.local_spm.trsv5AverageObservedLatency',
            ], float),
            'trsv5_max_in_flight': ([
                'trsv5.maxInFlight',
                'system.cpu.local_spm.trsv5MaxInFlight',
            ], int),
            'dram_bytes_read': (['system.dram.bytesRead::total'], int),
            'dram_bytes_written': (['system.dram.bytesWritten::total'], int),
        }

        for key, (names, dtype) in specs.items():
            self._add_stat(stats, stats_text, key, names, dtype)
        stats.update(dram_statistics(stats_text))

        if 'l1d_hits' in stats and 'l1d_misses' in stats:
            total = stats['l1d_hits'] + stats['l1d_misses']
            stats['l1d_hit_rate'] = stats['l1d_hits'] / total if total > 0 else 0
            stats['l1d_miss_rate'] = stats['l1d_misses'] / total if total > 0 else 0

        if 'l2_hits' in stats and 'l2_misses' in stats:
            total = stats['l2_hits'] + stats['l2_misses']
            stats['l2_hit_rate'] = stats['l2_hits'] / total if total > 0 else 0

        if 'total_cycles' in stats and 'committed_insts' in stats:
            if stats['committed_insts'] > 0:
                stats['calc_cpi'] = stats['total_cycles'] / stats['committed_insts']
                stats['calc_ipc'] = stats['committed_insts'] / stats['total_cycles']

        dotp = stats.get('cfd_dotp_committed')
        matld = stats.get('cfd_matld_committed')
        stream_ld = stats.get('patha_stream_ld_committed')
        sme_fmopa = stats.get('sme_fmopa_committed')
        sme_pipe = stats.get('sme_pipe_committed')
        sme_outer = stats.get('sme_outer_committed')
        matvecs_from_dotp = None
        matvecs_from_matld = None
        matvecs_from_stream_ld = None
        matvecs_from_sme_fmopa = None
        matvecs_from_sme_pipe = None
        matvecs_from_sme_outer = None

        if sme_pipe is not None and sme_pipe > 0:
            matvecs_from_sme_pipe = sme_pipe // THEORY['sme_fmopa_per_matvec']
            stats['matvecs_from_sme_pipe'] = matvecs_from_sme_pipe
        if sme_outer is not None and sme_outer > 0:
            matvecs_from_sme_outer = sme_outer // THEORY['sme_fmopa_per_matvec']
            stats['matvecs_from_sme_outer'] = matvecs_from_sme_outer
        if sme_fmopa is not None and sme_fmopa > 0:
            matvecs_from_sme_fmopa = sme_fmopa // THEORY['sme_fmopa_per_matvec']
            stats['matvecs_from_sme_fmopa'] = matvecs_from_sme_fmopa
        if dotp is not None and dotp > 0:
            matvecs_from_dotp = dotp // THEORY['cfd_dotp_per_matvec']
            stats['matvecs_from_dotp'] = matvecs_from_dotp
        if matld is not None and matld > 0:
            matld_extra = self.config.patha_matld_extra_per_run() if dotp else (
                THEORY['matld_prologue_per_batch'] *
                THEORY['benchmark_batch_invocations']
            )
            matld_steady = max(
                0,
                matld - matld_extra
            )
            matvecs_from_matld = matld_steady // THEORY['cfd_matld_per_matvec']
            stats['matvecs_from_matld'] = matvecs_from_matld
        if stream_ld is not None and stream_ld > 0:
            matvecs_from_stream_ld = stream_ld // THEORY['cfd_matld_per_matvec']
            stats['matvecs_from_stream_ld'] = matvecs_from_stream_ld

        if matvecs_from_sme_pipe is not None:
            stats['method'] = 'SME_ZA_SPATIAL_PIPE'
            stats['inferred_matvecs'] = matvecs_from_sme_pipe
            stats['inferred_matvecs_source'] = 'CFDSMEPipeStep committed / 5'
        elif matvecs_from_sme_outer is not None:
            stats['method'] = 'SME_ZA_OUTER_PIPE'
            stats['inferred_matvecs'] = matvecs_from_sme_outer
            stats['inferred_matvecs_source'] = 'CFDSMEZaOuterStep committed / 5'
        elif matvecs_from_sme_fmopa is not None:
            stats['method'] = 'SME_ZA_COLUMN_STREAM_DEPRECATED'
            stats['inferred_matvecs'] = matvecs_from_sme_fmopa
            stats['inferred_matvecs_source'] = 'CFDSMEFmopaStep committed / 5'
        elif matvecs_from_dotp is not None:
            stats['method'] = 'DOTP_ROW'
            stats['inferred_matvecs'] = matvecs_from_dotp
            stats['inferred_matvecs_source'] = 'CFDDSADotp committed'
        elif matvecs_from_matld is not None:
            stats['method'] = 'DOTP_ROW'
            stats['inferred_matvecs'] = matvecs_from_matld
            stats['inferred_matvecs_source'] = 'CFDDSAMatLd committed'

        if (self.config.patha_kernel != 'stream-dsa' and
                matvecs_from_dotp is not None and matvecs_from_matld is not None and
                matvecs_from_dotp != matvecs_from_matld):
            stats['matvec_inference_mismatch'] = (
                matvecs_from_dotp, matvecs_from_matld
            )

        if 'spm_bytes_read' in stats or 'spm_bytes_written' in stats:
            stats['spm_bytes_total'] = stats.get('spm_bytes_read', 0) + stats.get('spm_bytes_written', 0)

        inferred_matvecs = stats.get('inferred_matvecs', 0)
        if inferred_matvecs > 0:
            if stats.get('method') in (
                    'SME_ZA_COLUMN_STREAM_DEPRECATED',
                    'SME_ZA_SPATIAL_PIPE',
                    'SME_ZA_OUTER_PIPE'):
                stats['sme_fmopa_token_lat'] = self.config.sme_fmopa_lat
                stats['sme_fmopa_array_lat'] = self.config.sme_fmopa_array_lat
                stats['sme_fmopa_count'] = self.config.sme_fmopa_count
                stats['sme_pipe_lat'] = self.config.sme_pipe_lat
                stats['sme_pipe_count'] = self.config.sme_pipe_count
                stats['sme_outer_lat'] = self.config.sme_outer_lat
                stats['sme_outer_count'] = self.config.sme_outer_count
                stats['sme_outer_full_array'] = 1 if self.config.sme_outer_full_array else 0
                stats['expected_sme_fmopa'] = inferred_matvecs * THEORY['sme_fmopa_per_matvec']
                stats['expected_sme_pipe'] = inferred_matvecs * THEORY['sme_fmopa_per_matvec']
                stats['expected_sme_outer'] = inferred_matvecs * THEORY['sme_fmopa_per_matvec']
                stats['expected_sme_zero'] = inferred_matvecs * THEORY['sme_zero_per_matvec']
                stats['expected_sme_mova'] = inferred_matvecs * THEORY['sme_mova_per_matvec']
                stats['expected_cfd_matld'] = (
                    inferred_matvecs * THEORY['cfd_matld_per_matvec'] +
                    THEORY['sme_matld_extra_per_run']
                )
                stats['implied_spm_read_bytes'] = stats.get('cfd_matld_committed', 0) * 40
                if stats.get('implied_spm_read_bytes', 0):
                    stats['spm_timing_read_coverage'] = (
                        stats.get('spm_bytes_read', 0) /
                        stats['implied_spm_read_bytes'])
                    stats['local_spm_coverage'] = (
                        stats.get('local_spm_bytes_read', 0) /
                        stats['implied_spm_read_bytes'])
                local_logical = stats.get('local_spm_logical_bytes_read',
                                          stats.get('local_spm_bytes_read', 0))
                local_physical = stats.get('local_spm_physical_bytes_read', 0)
                local_reads = stats.get('local_spm_reads', 0)
                if local_physical:
                    stats['local_spm_payload_utilization'] = (
                        local_logical / local_physical)
                if local_reads and local_physical:
                    stats['local_spm_physical_read_width'] = (
                        local_physical / local_reads)
                stats['strict_outer_product_k_steps'] = stats.get('sme_fmopa_committed', 0)
                stats['pathb_spatial_pipe_steps'] = stats.get('sme_pipe_committed', 0)
                stats['pathc_outer_steps'] = stats.get('sme_outer_committed', 0)
                step_count = (
                    stats.get('sme_pipe_committed', 0)
                    if stats.get('method') == 'SME_ZA_SPATIAL_PIPE'
                    else (
                        stats.get('sme_outer_committed', 0)
                        if stats.get('method') == 'SME_ZA_OUTER_PIPE'
                        else stats.get('sme_fmopa_committed', 0)
                    )
                )
                stats['effective_macs'] = step_count * 5
                stats['effective_flops'] = stats['effective_macs'] * 2
                stats['implied_result_write_bytes'] = inferred_matvecs * THEORY['result_store_bytes']
                stats['logical_bytes_per_matvec'] = (
                    THEORY['spm_bytes_per_matvec'] + THEORY['result_store_bytes']
                )
                if 'cfd_matld_committed' in stats:
                    stats['delta_cfd_matld'] = stats['cfd_matld_committed'] - stats['expected_cfd_matld']
                if 'sme_fmopa_committed' in stats:
                    stats['delta_sme_fmopa'] = stats['sme_fmopa_committed'] - stats['expected_sme_fmopa']
                if 'sme_pipe_committed' in stats:
                    stats['delta_sme_pipe'] = stats['sme_pipe_committed'] - stats['expected_sme_pipe']
                if 'sme_outer_committed' in stats:
                    stats['delta_sme_outer'] = stats['sme_outer_committed'] - stats['expected_sme_outer']
                if 'sme_zero_committed' in stats:
                    stats['delta_sme_zero'] = stats['sme_zero_committed'] - stats['expected_sme_zero']
                if 'sme_mova_committed' in stats:
                    stats['delta_sme_mova'] = stats['sme_mova_committed'] - stats['expected_sme_mova']
                if 'total_cycles' in stats and stats.get('effective_flops', 0):
                    stats['effective_flops_per_cycle'] = (
                        stats['effective_flops'] / stats['total_cycles'])
                if stats.get('total_cycles', 0) > 0:
                    stats['local_spm_reads_per_cycle_measured'] = (
                        stats.get('local_spm_reads', 0) / stats['total_cycles'])
                    stats['local_spm_bytes_per_cycle_measured'] = (
                        stats.get('local_spm_bytes_read', 0) / stats['total_cycles'])
                token_lat = (
                    self.config.sme_pipe_lat if stats.get('method') == 'SME_ZA_SPATIAL_PIPE'
                    else self.config.sme_outer_lat if stats.get('method') == 'SME_ZA_OUTER_PIPE'
                    else self.config.sme_fmopa_lat
                )
                token_compute_lb = THEORY['sme_fmopa_per_matvec'] * token_lat
                load_issue_lb = THEORY['cfd_matld_per_matvec']
                load_bandwidth_lb = (
                    (THEORY['cfd_matld_per_matvec'] + max(1, self.config.local_spm_read_ports) - 1) //
                    max(1, self.config.local_spm_read_ports)
                )
                stats['sme_forwarding_compute_lb'] = token_compute_lb
                stats['sme_load_bandwidth_lb'] = load_bandwidth_lb
                stats['sme_explicit_lmat_slots'] = load_issue_lb
                stats['sme_forwarding_combined_lb'] = max(
                    token_compute_lb, load_issue_lb
                )
            else:
                stream_dsa_kernel = self.config.patha_kernel == 'stream-dsa'
                stats['expected_cfd_matld'] = (
                    0 if stream_dsa_kernel else
                    inferred_matvecs * THEORY['cfd_matld_per_matvec'] +
                    self.config.patha_matld_extra_per_run()
                )
                stats['expected_patha_stream_ld'] = (
                    inferred_matvecs * THEORY['cfd_matld_per_matvec'] +
                    2
                    if stream_dsa_kernel else 0
                )
                stats['expected_cfd_dotp'] = inferred_matvecs * THEORY['cfd_dotp_per_matvec']
                stats['expected_pack_acc'] = (
                    0 if stream_dsa_kernel else
                    inferred_matvecs * THEORY['cfd_pack_per_matvec']
                )
                patha_read_count = (
                    stats.get('patha_stream_ld_committed', 0)
                    if stream_dsa_kernel else stats.get('cfd_matld_committed', 0)
                )
                patha_logical = patha_read_count * THEORY['lmat_bytes']
                patha_physical = patha_read_count * self.config.patha_spm_read_width
                patha_result_store = (
                    40 if self.config.patha_store_mode in ('pred40', 'store5') else 64
                )
                stats['implied_spm_read_bytes'] = patha_logical
                stats['patha_spm_mode'] = self.config.patha_spm_mode
                stats['patha_spm_read_width'] = self.config.patha_spm_read_width
                stats['patha_logical_bytes_read'] = patha_logical
                stats['patha_physical_bytes_read'] = patha_physical
                stats['patha_num_spm_reads'] = patha_read_count
                if patha_physical:
                    stats['patha_payload_utilization'] = patha_logical / patha_physical
                if stats.get('implied_spm_read_bytes', 0):
                    stats['patha_local_spm_coverage'] = (
                        patha_logical / stats['implied_spm_read_bytes'])
                stats['patha_store_mode'] = self.config.patha_store_mode
                stats['patha_store_stride'] = self.config.patha_store_stride
                stats['patha_acc_mode'] = self.config.patha_acc_mode
                stats['patha_result_buffer_depth'] = self.config.patha_result_buffer_depth
                stats['patha_dotp_lat'] = self.config.patha_dotp_lat
                stats['patha_dotp_count'] = self.config.patha_dotp_count
                stats['patha_pack_count'] = self.config.patha_pack_count
                stats['patha_unroll'] = self.config.patha_unroll
                stats['patha_streaming'] = int(self.config.patha_streaming)
                stats['patha_kernel'] = self.config.patha_kernel
                stats['patha_input_buffer_depth'] = self.config.patha_input_buffer_depth
                stats['patha_stream_read_ports'] = self.config.patha_stream_read_ports
                stats['patha_store_queue_depth'] = self.config.patha_store_queue_depth
                stats['patha_async_store'] = int(self.config.patha_async_store)
                stats['patha_result_store_bytes_per_matvec'] = patha_result_store
                stats['implied_result_write_bytes'] = inferred_matvecs * patha_result_store
                stats['patha_result_layout_bytes'] = inferred_matvecs * self.config.patha_store_stride
                stats['patha_store_width'] = patha_result_store
                stats['patha_store_instructions'] = (
                    stats.get('patha_store5_committed', 0)
                    if self.config.patha_store_mode == 'store5' else inferred_matvecs
                )
                stats['patha_pack_acc_count'] = stats.get(
                    'cfd_pack_committed',
                    0 if stream_dsa_kernel else
                    inferred_matvecs * THEORY['cfd_pack_per_matvec'])
                stats['patha_pack_acc_per_matvec'] = (
                    0 if stream_dsa_kernel else THEORY['cfd_pack_per_matvec']
                )
                stats['patha_matld_readports'] = (
                    self.config.patha_stream_read_ports if stream_dsa_kernel else self.config.readport_count
                )
                stats['patha_spm_banks'] = self.config.local_spm_banks
                stats['patha_spm_bank_granularity'] = self.config.local_spm_bank_granularity
                stats['logical_bytes_per_matvec'] = THEORY['spm_bytes_per_matvec'] + patha_result_store
                stats['effective_macs'] = stats.get('cfd_dotp_committed', 0) * 5
                stats['effective_flops'] = stats['effective_macs'] * 2
                if 'total_cycles' in stats and stats.get('effective_flops', 0):
                    stats['effective_flops_per_cycle'] = (
                        stats['effective_flops'] / stats['total_cycles'])
                if 'cfd_matld_committed' in stats:
                    stats['delta_cfd_matld'] = stats['cfd_matld_committed'] - stats['expected_cfd_matld']
                if 'patha_stream_ld_committed' in stats:
                    stats['delta_patha_stream_ld'] = (
                        stats['patha_stream_ld_committed'] -
                        stats['expected_patha_stream_ld'])
                if 'cfd_dotp_committed' in stats:
                    stats['delta_cfd_dotp'] = stats['cfd_dotp_committed'] - stats['expected_cfd_dotp']

        if 'total_cycles' in stats and inferred_matvecs > 0:
            stats['cycles_per_iter'] = stats['total_cycles'] / inferred_matvecs

        if all(k in stats for k in ('issue_dist_samples', 'issue_dist_0',
                                     'issue_dist_1', 'issue_dist_2',
                                     'issue_dist_3')):
            samples = max(1, stats['issue_dist_samples'])
            ge2 = samples - stats['issue_dist_0'] - stats['issue_dist_1']
            stats['issue_0_ratio'] = stats['issue_dist_0'] / samples
            stats['issue_1_ratio'] = stats['issue_dist_1'] / samples
            stats['issue_2plus_ratio'] = max(0, ge2) / samples

        return stats

    def print_performance_report(self, stats, requested_bench_iters=10000000):
        method = stats.get('method', 'DOTP_ROW')
        focus = self.config.report_focus
        if focus == 'auto':
            if method == 'SME_ZA_SPATIAL_PIPE':
                focus = 'pathb'
            elif method in ('SME_ZA_OUTER_PIPE', 'SME_ZA_COLUMN_STREAM_DEPRECATED'):
                focus = 'pathc'
            else:
                focus = 'patha'

        print("\n" + "=" * 70)
        if focus == 'patha':
            print("  PATH A REPORT -- dotp-row CFD-DSA Simulation")
        elif focus == 'pathb':
            print("  PATH B REPORT -- SME-ZA spatial pipeline CFD-DSA Simulation")
        elif focus == 'pathc':
            print("  PATH C REPORT -- SME-ZA CFD-DSA Simulation")
        else:
            print("  PERFORMANCE ANALYSIS REPORT -- CFD-DSA gem5 Simulation")
        print("=" * 70)

        if 'sim_seconds' in stats:
            print(f"\n  Simulation Summary:")
            print(f"    Simulated seconds:   {stats['sim_seconds']:.6f}s")
            if 'sim_ticks' in stats:
                print(f"    Total ticks:         {stats['sim_ticks']:,}")
            if 'stats_segments' in stats:
                print(f"    Stats segment:       {stats['selected_stats_segment']} / {stats['stats_segments']}")

        if 'total_cycles' in stats:
            print(f"\n  CPU Cycles:")
            print(f"    Total cycles:       {stats['total_cycles']:,}")

        if 'committed_insts' in stats:
            print(f"\n  Instruction Count:")
            print(f"    Committed insts:    {stats['committed_insts']:,}")
            if 'committed_ops' in stats:
                print(f"    Committed ops:      {stats['committed_ops']:,}")

        if 'CPI' in stats:
            print(f"\n  CPI / IPC Analysis:")
            print(f"    CPI:                {stats['CPI']:.4f}")
            print(f"    IPC:                {stats.get('IPC', 1.0/stats['CPI']):.4f}")
        elif 'calc_cpi' in stats:
            print(f"\n  CPI / IPC Analysis:")
            print(f"    CPI:                {stats['calc_cpi']:.4f}")
            print(f"    IPC:                {stats['calc_ipc']:.4f}")

        aligned_requested = requested_bench_iters + (requested_bench_iters & 1)
        inferred_matvecs = stats.get('inferred_matvecs', aligned_requested)
        precheck_matvecs = (THEORY['sme_precheck_matvecs']
                            if method in (
                                'SME_ZA_COLUMN_STREAM_DEPRECATED',
                                'SME_ZA_SPATIAL_PIPE',
                                'SME_ZA_OUTER_PIPE')
                            else THEORY['precheck_matvecs'])
        print(f"\n  Benchmark Matvecs:")
        print(f"    Method:             {method}")
        print(f"    Requested (CLI):    {requested_bench_iters:,}")
        print(f"    Requested aligned:  {aligned_requested:,}")
        print(f"    Precheck matvecs:   {precheck_matvecs:,}")
        print(f"    Inferred total:     {inferred_matvecs:,}")
        print(f"    Inferred bench-only:{max(0, inferred_matvecs - precheck_matvecs):,}")
        if 'inferred_matvecs_source' in stats:
            print(f"    Inferred from:      {stats['inferred_matvecs_source']}")
        if 'matvec_inference_mismatch' in stats:
            by_dotp, by_matld = stats['matvec_inference_mismatch']
            print(f"    Warning:            dotp/matld inferred mismatch: {by_dotp:,} vs {by_matld:,}")

        if 'l1d_hit_rate' in stats:
            print(f"\n  L1 Data Cache:")
            print(f"    Hit rate:           {stats['l1d_hit_rate']*100:.2f}%")
            print(f"    Hits / misses:      {stats['l1d_hits']:,} / {stats['l1d_misses']:,}")

        if 'l2_hit_rate' in stats:
            print(f"\n  L2 Cache:")
            print(f"    Hit rate:           {stats['l2_hit_rate']*100:.2f}%")

        print(f"\n  Memory Routing:")
        implied_spm_read_bytes = stats.get('implied_spm_read_bytes', stats.get('cfd_matld_committed', 0) * 40)
        if 'spm_bytes_total' in stats:
            print(f"    SPM timing bytes:   {stats['spm_bytes_total']:,}")
            print(f"    SPM reads/writes:   {stats.get('spm_reads', 0):,} / {stats.get('spm_writes', 0):,}")
            if 'spm_bw_read' in stats:
                print(f"    SPM read BW:        {stats['spm_bw_read']/1e9:.3f} GB/s")
            if 'spm_bw_total' in stats:
                print(f"    SPM total BW:       {stats['spm_bw_total']/1e9:.3f} GB/s")
        else:
            print(f"    SPM timing bytes:   0 (no SPM traffic stat found)")
        if implied_spm_read_bytes:
            if method in (
                    'SME_ZA_COLUMN_STREAM_DEPRECATED',
                    'SME_ZA_SPATIAL_PIPE',
                    'SME_ZA_OUTER_PIPE'):
                print(f"    SPM SME reads:      {implied_spm_read_bytes:,} bytes implied by CFDDSAMatLd")
            else:
                print(f"    SPM lmat reads:     {implied_spm_read_bytes:,} bytes implied by CFDDSAMatLd")
                if stats.get('patha_num_spm_reads', 0):
                    print(f"    Path A SPM mode:    {stats.get('patha_spm_mode', self.config.patha_spm_mode)}")
                    print(f"    Path A read width:  {stats.get('patha_spm_read_width', self.config.patha_spm_read_width)} B physical")
                    print(f"    Path A read ports:  {stats.get('patha_matld_readports', self.config.readport_count)}")
                    print(f"    Path A SPM banks:   {stats.get('patha_spm_banks', self.config.local_spm_banks)}")
                    print(f"    Path A bank gran:   {stats.get('patha_spm_bank_granularity', self.config.local_spm_bank_granularity)} B")
                    print(f"    Path A logical:     {stats.get('patha_logical_bytes_read', 0):,} bytes")
                    print(f"    Path A physical:    {stats.get('patha_physical_bytes_read', 0):,} bytes")
                    if 'patha_payload_utilization' in stats:
                        print(f"    Path A payload util:{stats['patha_payload_utilization']*100:.2f}%")
                    if 'patha_local_spm_coverage' in stats:
                        print(f"    Path A local cover: {stats['patha_local_spm_coverage']*100:.2f}%")
            if stats.get('spm_bytes_read', 0):
                print("    SPM read model:     timing path observed by system.spm")
                if 'spm_timing_read_coverage' in stats:
                    print(f"    SPM read coverage:  {stats['spm_timing_read_coverage']*100:.2f}% of implied lmat bytes")
            else:
                print("    SPM read model:     not observed by system.spm; using committed lmat inference")
        if stats.get('local_spm_bytes_read', 0):
            if 'local_spm_physical_read_width' in stats:
                print(f"    Local SPM read width: {stats['local_spm_physical_read_width']:.0f} B physical")
            print(f"    Local SPM bytes:    {stats['local_spm_bytes_read']:,}")
            if stats.get('local_spm_physical_bytes_read', 0):
                print(f"    Local SPM physical: {stats['local_spm_physical_bytes_read']:,} bytes")
            if 'local_spm_payload_utilization' in stats:
                print(f"    Local SPM payload util: {stats['local_spm_payload_utilization']*100:.2f}%")
            print(f"    Local SPM reads:    {stats.get('local_spm_reads', 0):,}")
            if 'local_spm_coverage' in stats:
                print(f"    Local SPM coverage: {stats['local_spm_coverage']*100:.2f}% of implied lmat bytes")
            print(f"    Local SPM avg lat:  {stats.get('local_spm_avg_read_latency', 0):.2f} cycles")
            print(f"    Local SPM max lat:  {stats.get('local_spm_max_read_latency', 0):,} cycles")
            print(f"    Local SPM port busy:{stats.get('local_spm_port_busy_cycles', 0):,} cycles")
            print(f"    Local SPM port conf:{stats.get('local_spm_port_conflict_cycles', 0):,} cycles")
            print(f"    Local SPM bank conf:{stats.get('local_spm_bank_conflict_cycles', 0):,} cycles")
            print(f"    Local SPM queuefull:{stats.get('local_spm_queue_full_cycles', 0):,} cycles")
            print(f"    Local SPM ZWB conf: {stats.get('local_spm_z_wb_conflict_cycles', 0):,} cycles")
            if 'local_spm_avg_request_to_response' in stats:
                print(f"    Local SPM req->rsp: {stats.get('local_spm_avg_request_to_response', 0):.2f} cycles avg")
            if 'local_spm_avg_zwb_wait' in stats:
                print(f"    Local SPM ZWB wait: {stats.get('local_spm_avg_zwb_wait', 0):.2f} avg / {stats.get('local_spm_max_zwb_wait', 0):,} max")
            print(f"    Local SPM out avg/max: {stats.get('local_spm_outstanding_avg', 0):.1f} / {stats.get('local_spm_outstanding_max', 0):,}")
            if 'local_spm_bytes_per_cycle_measured' in stats:
                print(f"    Local SPM measured BW: {stats['local_spm_bytes_per_cycle_measured']:.2f} B/cycle")
            if 'local_spm_physical_bytes_per_cycle_model' in stats:
                print(f"    Local SPM model BW: {stats.get('local_spm_logical_bytes_per_cycle_model', 0):.2f} logical / {stats.get('local_spm_physical_bytes_per_cycle_model', 0):.2f} physical B/cycle")
            if focus != 'patha' and stats.get('local_event_created', 0):
                print("    -- local-event lifecycle --")
                print(f"    requests c/a/d:    {stats.get('local_event_created', 0):,} / {stats.get('local_event_accepted', 0):,} / {stats.get('local_event_completed', 0):,}")
                print(f"    events a/r/rsp/zwb/c: {stats.get('local_event_accept_events', 0):,} / {stats.get('local_event_read_start_events', 0):,} / {stats.get('local_event_response_events', 0):,} / {stats.get('local_event_zwb_events', 0):,} / {stats.get('local_event_complete_events', 0):,}")
                print(f"    retry qfull/ofull: {stats.get('local_event_retry_events', 0):,} / {stats.get('local_event_queue_full_events', 0):,} / {stats.get('local_event_outstanding_full_events', 0):,}")
                print(f"    live max/end:      {stats.get('local_event_live_max', 0):,} / {stats.get('local_event_live_end', 0):,}")
                print(f"    avg issue->accept/read/rsp/zwb/complete: {stats.get('local_event_avg_issue_to_accept', 0):.2f} / {stats.get('local_event_avg_accept_to_read', 0):.2f} / {stats.get('local_event_avg_read_to_response', 0):.2f} / {stats.get('local_event_avg_response_to_zwb', 0):.2f} / {stats.get('local_event_avg_issue_to_complete', 0):.2f}")
            if focus != 'patha' and stats.get('vector_zwb_requests', 0):
                print("    -- vector writeback arbiter --")
                print(f"    requests lmat/mova: {stats.get('vector_zwb_lmat_requests', 0):,} / {stats.get('vector_zwb_mova_requests', 0):,}")
                print(f"    conflicts/cycles:  {stats.get('vector_zwb_conflicts', 0):,} / {stats.get('vector_zwb_conflict_cycles', 0):,}")
                print(f"    avg/max wait:      {stats.get('vector_zwb_avg_wait', 0):.2f} / {stats.get('vector_zwb_max_wait', 0):,}")
        result_store_bytes = (
            THEORY['result_store_bytes'] if method in (
                'SME_ZA_COLUMN_STREAM_DEPRECATED',
                'SME_ZA_SPATIAL_PIPE',
                'SME_ZA_OUTER_PIPE')
            else stats.get('patha_result_store_bytes_per_matvec',
                           THEORY['dotp_result_store_bytes'])
        )
        logical_bytes_per_matvec = stats.get(
            'logical_bytes_per_matvec',
            THEORY['spm_bytes_per_matvec'] + result_store_bytes
        )
        print(f"    Logical SPM/matvec: {THEORY['spm_bytes_per_matvec']} bytes")
        print(f"    Result store width: {result_store_bytes} bytes")
        if method == 'DOTP_ROW':
            print(f"    Path A store mode:  {stats.get('patha_store_mode', self.config.patha_store_mode)}")
            print(f"    Path A store width: {stats.get('patha_store_width', result_store_bytes)} bytes")
            print(f"    Path A store stride:{stats.get('patha_store_stride', self.config.patha_store_stride)} bytes")
            print(f"    Path A acc mode:    {stats.get('patha_acc_mode', self.config.patha_acc_mode)}")
            print(f"    Path A buf depth:   {stats.get('patha_result_buffer_depth', self.config.patha_result_buffer_depth)}")
            print(f"    Path A dotp lat:    {stats.get('patha_dotp_lat', self.config.patha_dotp_lat)}")
            print(f"    Path A dotp count:  {stats.get('patha_dotp_count', self.config.patha_dotp_count)}")
            print(f"    Path A pack count:  {stats.get('patha_pack_count', self.config.patha_pack_count)}")
            print(f"    Path A unroll:      {stats.get('patha_unroll', self.config.patha_unroll)}")
            print(f"    Path A streaming:   {stats.get('patha_streaming', int(self.config.patha_streaming))} ({stats.get('patha_kernel', self.config.patha_kernel)})")
            print(f"    Path A input depth: {stats.get('patha_input_buffer_depth', self.config.patha_input_buffer_depth)}")
            print(f"    Path A stream ports:{stats.get('patha_stream_read_ports', self.config.patha_stream_read_ports)}")
            print(f"    Path A store queue: {stats.get('patha_store_queue_depth', self.config.patha_store_queue_depth)} async={stats.get('patha_async_store', int(self.config.patha_async_store))}")
            print(f"    Path A store insts: {stats.get('patha_store_instructions', inferred_matvecs):,}")
            print(f"    Path A result bytes:{stats.get('implied_result_write_bytes', 0):,}")
            print(f"    Path A layout bytes:{stats.get('patha_result_layout_bytes', 0):,}")
        print(f"    Logical bytes/matvec:{logical_bytes_per_matvec} bytes")
        if 'dram_bytes_read' in stats or 'dram_bytes_written' in stats:
            dram_total = stats.get('dram_bytes_read', 0) + stats.get('dram_bytes_written', 0)
            print(f"    DRAM bytes:         {dram_total:,}")

        if focus != 'patha' and stats.get('za_tokens_produced', 0):
            print("\n  ZA Token/Pending Model:")
            print(f"    tokens produced:    {stats.get('za_tokens_produced', 0):,}")
            print(f"    tokens consumed:    {stats.get('za_tokens_consumed', 0):,}")
            print(f"    pending updates:    {stats.get('za_pending_updates', 0):,}")
            print(f"    commit updates:     {stats.get('za_commit_updates', 0):,}")
            print(f"    squash discards:    {stats.get('za_squash_discards', 0):,}")
            print(f"    pending queue full: {stats.get('za_pending_queue_full', 0):,}")
            print(f"    MOVA wait cycles:   {stats.get('za_mova_wait_cycles', 0):,}")
            print(f"    next ZERO wait:     {stats.get('za_next_zero_wait_cycles', 0):,}")
            print(f"    forwarding stalls:  {stats.get('za_forwarding_stalls', 0):,}")
            if stats.get('za_rename_allocs', 0) or stats.get('za_mova_reads', 0):
                print("    -- rename model --")
                print(f"    rename allocs:      {stats.get('za_rename_allocs', 0):,}")
                print(f"    rename frees:       {stats.get('za_rename_frees', 0):,}")
                print(f"    rename rollbacks:   {stats.get('za_rename_rollbacks', 0):,}")
                print(f"    phys live max/end:  {stats.get('za_physical_live_max', 0):,} / {stats.get('za_physical_live_end', 0):,}")
                print(f"    MOVA reads:         {stats.get('za_mova_reads', 0):,}")
                print(f"    MOVA rename hits:   {stats.get('za_mova_rename_hits', 0):,}")
                print(f"    MOVA arch fallback: {stats.get('za_mova_arch_fallbacks', 0):,}")
                print(f"    MOVA not-ready:     {stats.get('za_mova_blocked_not_ready', 0):,}")

        print(f"\n  CFD Instruction Mix:")
        if focus != 'patha' and 'sme_zero_committed' in stats:
            print(f"    official ZERO ZA:   {stats['sme_zero_committed']:,} (Matrix OpClass)")
        if focus != 'patha' and 'sme_fmopa_committed' in stats:
            print(f"    cfdsme fmopa step:  {stats['sme_fmopa_committed']:,}")
        if focus != 'patha' and 'sme_pipe_committed' in stats:
            print(f"    cfdsme pipe step:   {stats['sme_pipe_committed']:,} (Path B)")
        if focus != 'patha' and 'sme_outer_committed' in stats:
            print(f"    cfdsme outer step:  {stats['sme_outer_committed']:,} (Path C)")
        if focus != 'patha' and 'sme_mova_committed' in stats:
            print(f"    official MOVA ZA->Z:{stats['sme_mova_committed']:,} (MatrixMov OpClass)")
        if 'memwrite_committed' in stats:
            print(f"    stores committed:   {stats['memwrite_committed']:,} (MemWrite OpClass)")
        issued_keys = [('cfd_matld_issued', 'matld issued')]
        if focus != 'pathc':
            issued_keys.append(('patha_stream_ld_issued', 'stream_ld issued'))
        if focus != 'patha':
            issued_keys.extend([
                ('sme_fmopa_issued', 'fmopa issued'),
                ('sme_pipe_issued', 'pipe issued'),
                ('sme_outer_issued', 'outer issued'),
                ('sme_zero_issued', 'ZERO issued'),
                ('sme_mova_issued', 'MOVA issued'),
            ])
        issued_keys.append(('memwrite_issued', 'store issued'))
        if focus != 'pathc':
            issued_keys.append(('patha_store5_issued', 'store5 issued'))
        if focus != 'pathc':
            issued_keys.append(('intalu_issued', 'IntAlu issued'))
            issued_keys.append(('cfd_pack_issued', 'pack issued'))
        if focus != 'pathc':
            issued_keys.append(('cfd_dotp_issued', 'dotp issued'))
        issued_lines = [f"{label}={stats[key]:,}" for key, label in issued_keys if key in stats]
        if issued_lines:
            print(f"    issued breakdown:   {', '.join(issued_lines)}")
        if focus != 'pathc' and 'cfd_dotp_committed' in stats:
            print(f"    dotp committed:     {stats['cfd_dotp_committed']:,}")
        if 'cfd_matld_committed' in stats:
            print(f"    matld committed:    {stats['cfd_matld_committed']:,}")
        if focus != 'pathc' and 'patha_stream_ld_committed' in stats:
            print(f"    stream_ld committed:{stats['patha_stream_ld_committed']:,} (CFDDSAStreamLd OpClass)")
        if focus != 'pathc' and 'patha_store5_committed' in stats:
            print(f"    store5 committed:   {stats['patha_store5_committed']:,} (CFDDSAStore5 OpClass)")
        if focus != 'pathc' and 'expected_cfd_dotp' in stats:
            print(f"    dotp expected:      {stats['expected_cfd_dotp']:,}")
        if 'expected_cfd_matld' in stats:
            print(f"    matld expected:     {stats['expected_cfd_matld']:,}")
            if method in (
                    'SME_ZA_COLUMN_STREAM_DEPRECATED',
                    'SME_ZA_SPATIAL_PIPE',
                    'SME_ZA_OUTER_PIPE'):
                matld_extra = THEORY['sme_matld_extra_per_run']
                matld_note = "benchmark prologue / next-ping fill"
            else:
                matld_extra = self.config.patha_matld_extra_per_run()
                matld_note = "legacy correctness pair + benchmark prologues"
            print(f"    matld prologue ovh: +{matld_extra} ({matld_note})")
        if focus != 'pathc' and 'expected_patha_stream_ld' in stats:
            print(f"    stream_ld expected: {stats['expected_patha_stream_ld']:,}")
        if focus != 'pathc' and 'delta_cfd_dotp' in stats:
            print(f"    dotp delta:         {stats['delta_cfd_dotp']:+,}")
        if 'delta_cfd_matld' in stats:
            print(f"    matld delta:        {stats['delta_cfd_matld']:+,}")
        if focus != 'pathc' and 'delta_patha_stream_ld' in stats:
            print(f"    stream_ld delta:    {stats['delta_patha_stream_ld']:+,}")
        if focus != 'pathc' and 'expected_pack_acc' in stats:
            print(f"    pack_acc expected:  {stats['expected_pack_acc']:,}")
        if focus != 'pathc' and 'cfd_pack_committed' in stats:
            print(f"    pack_acc committed: {stats['cfd_pack_committed']:,} (CFDDSAPack OpClass)")
        if focus != 'pathc' and 'patha_pack_acc_count' in stats:
            print(f"    pack_acc modeled:   {stats['patha_pack_acc_count']:,} "
                  f"({stats.get('patha_pack_acc_per_matvec', 1)}/matvec, cross-domain harvest)")
        if method == 'DOTP_ROW' and any(k in stats for k in (
                'patha_buffer_allocs', 'patha_buffer_writes',
                'patha_buffer_packs')):
            print("    -- Path A internal result buffer --")
            print(f"    allocs/frees:       {stats.get('patha_buffer_allocs', 0):,} / {stats.get('patha_buffer_frees', 0):,}")
            print(f"    writes/packs:       {stats.get('patha_buffer_writes', 0):,} / {stats.get('patha_buffer_packs', 0):,}")
            print(f"    full stalls:        {stats.get('patha_buffer_full_stalls', 0):,}")
            print(f"    live max/end:       {stats.get('patha_buffer_live_max', 0):,} / {stats.get('patha_buffer_live_end', 0):,}")
            print(f"    overlap events:     {stats.get('patha_buffer_overlap_cycles', 0):,}")
            print(f"    pack-while-live:    {stats.get('patha_buffer_pack_while_dotp_cycles', 0):,}")
        if focus != 'pathc' and stats.get('patha_dotp_issued_model', 0):
            print("  Path A dotp issue/burst model:")
            print(f"    dotp issued:        {stats.get('patha_dotp_issued_model', 0):,}")
            print(f"    issue cycles:       {stats.get('patha_dotp_issue_cycles', 0):,}")
            print(f"    issue gaps:         {stats.get('patha_dotp_issue_gaps', 0):,}")
            print(f"    max issue gap:      {stats.get('patha_dotp_max_issue_gap', 0):,}")
            print(f"    burst max/avg:      {stats.get('patha_dotp_burst_max', 0):,} / {stats.get('patha_dotp_burst_avg', 0):.2f}")
            print(f"    blocked input:      {stats.get('patha_dotp_blocked_input_not_ready', 0):,}")
            print(f"    blocked result buf: {stats.get('patha_dotp_blocked_result_buffer_full', 0):,}")
            print(f"    blocked pack/store: {stats.get('patha_dotp_blocked_pack_store', 0):,}")
        if focus != 'pathc' and (
                stats.get('patha_input_buffer_allocs', 0) or
                stats.get('patha_async_store_enqueues', 0) or
                stats.get('patha_streaming', 0)):
            print("  Path A streaming DSA model:")
            print(f"    input alloc/free:   {stats.get('patha_input_buffer_allocs', 0):,} / {stats.get('patha_input_buffer_frees', 0):,}")
            print(f"    input full stalls:  {stats.get('patha_input_buffer_full_stalls', 0):,}")
            print(f"    input live max:     {stats.get('patha_input_buffer_live_max', 0):,}")
            print(f"    async enq/commit:   {stats.get('patha_async_store_enqueues', 0):,} / {stats.get('patha_async_store_commits', 0):,}")
            print(f"    async full stalls:  {stats.get('patha_async_store_queue_full_stalls', 0):,}")
            print(f"    async live max:     {stats.get('patha_async_store_live_max', 0):,}")
            print("    -- field-ready early-start --")
            print(f"    early-ready opps:   {stats.get('patha_early_dotp_start_events', 0):,}")
            print(f"    before all fields:  {stats.get('patha_dotp_before_all_fields_ready', 0):,}")
            print(f"    dep-ready before all:{stats.get('patha_dotp_dep_ready_before_all', 0):,}")
            print(f"    execute before all: {stats.get('patha_dotp_execute_before_all', 0):,}")
            print(f"    complete before all:{stats.get('patha_dotp_complete_before_all', 0):,}")
            print(f"    early issued/missed:{stats.get('patha_early_ready_issued', 0):,} / "
                  f"{stats.get('patha_early_ready_missed', 0):,} "
                  f"(eff={stats.get('patha_early_ready_efficiency', 0.0) * 100.0:.2f}%)")
            print(f"    missed breakdown:   notIQ={stats.get('patha_early_missed_not_in_iq', 0):,}, "
                  f"fu={stats.get('patha_early_missed_fu_busy', 0):,}, "
                  f"issue={stats.get('patha_early_missed_issue_width', 0):,}, "
                  f"result={stats.get('patha_early_missed_result_busy', 0):,}, "
                  f"token={stats.get('patha_early_missed_token_not_ready', 0):,}, "
                  f"other={stats.get('patha_early_missed_other', 0):,}")
            print(f"    load/compute ovlp:  {stats.get('patha_load_compute_overlap_cycles', 0):,}")
            print(f"    field wait cycles:  {stats.get('patha_field_ready_wait_cycles', 0):,}")
            print(f"    whole-slot stalls:  {stats.get('patha_whole_slot_barrier_stall_cycles', 0):,}")
            print(f"    stream port busy:   p0={stats.get('patha_stream_port0_busy_cycles', 0):,}, p1={stats.get('patha_stream_port1_busy_cycles', 0):,}")
            print(f"    dual-load cycles:   {stats.get('patha_dual_load_issue_cycles', 0):,}")
            print(f"    avg dotp after row/vector ready: "
                  f"{stats.get('patha_dotp_avg_after_field_ready', 0):.2f} / "
                  f"{stats.get('patha_dotp_avg_after_vector_ready', 0):.2f} cycles")
        if focus != 'patha' and method == 'SME_ZA_COLUMN_STREAM_DEPRECATED' and 'expected_sme_fmopa' in stats:
            print(f"    fmopa step expected:{stats['expected_sme_fmopa']:,}")
        if focus != 'patha' and method == 'SME_ZA_COLUMN_STREAM_DEPRECATED' and 'delta_sme_fmopa' in stats:
            print(f"    fmopa step delta:   {stats['delta_sme_fmopa']:+,}")
        if focus != 'patha' and method == 'SME_ZA_SPATIAL_PIPE' and 'expected_sme_pipe' in stats:
            print(f"    pipe step expected: {stats['expected_sme_pipe']:,}")
        if focus != 'patha' and method == 'SME_ZA_SPATIAL_PIPE' and 'delta_sme_pipe' in stats:
            print(f"    pipe step delta:    {stats['delta_sme_pipe']:+,}")
        if focus != 'patha' and method == 'SME_ZA_OUTER_PIPE' and 'expected_sme_outer' in stats:
            print(f"    outer step expected:{stats['expected_sme_outer']:,}")
        if focus != 'patha' and method == 'SME_ZA_OUTER_PIPE' and 'delta_sme_outer' in stats:
            print(f"    outer step delta:   {stats['delta_sme_outer']:+,}")
        if focus != 'patha' and 'delta_sme_zero' in stats:
            print(f"    zero ZA delta:      {stats['delta_sme_zero']:+,}")
        if focus != 'patha' and 'delta_sme_mova' in stats:
            print(f"    mova ZA delta:      {stats['delta_sme_mova']:+,}")
        if focus != 'patha' and 'strict_outer_product_k_steps' in stats:
            print(f"    SME k-steps:        {stats['strict_outer_product_k_steps']:,}")
            print(f"    Effective MACs:     {stats['effective_macs']:,}")
        if focus != 'patha' and method == 'SME_ZA_SPATIAL_PIPE':
            print(f"    Path B pipe steps:  {stats.get('pathb_spatial_pipe_steps', 0):,}")
            print(f"    Path B pending/commit/squash: "
                  f"{stats.get('pathb_pipe_pending_updates', 0):,} / "
                  f"{stats.get('pathb_pipe_commit_updates', 0):,} / "
                  f"{stats.get('pathb_pipe_squash_discards', 0):,}")
            print(f"    Path B col writes:  "
                  f"{stats.get('pathb_pipe_col0_writes', 0):,}/"
                  f"{stats.get('pathb_pipe_col1_writes', 0):,}/"
                  f"{stats.get('pathb_pipe_col2_writes', 0):,}/"
                  f"{stats.get('pathb_pipe_col3_writes', 0):,}/"
                  f"{stats.get('pathb_pipe_col4_writes', 0):,}")
            print(f"    Path B final reads: {stats.get('pathb_pipe_final_reads', 0):,}")
            print(f"    Path B fw hits/stall: {stats.get('pathb_pipe_forward_hits', 0):,} / "
                  f"{stats.get('pathb_pipe_forward_stalls', 0):,}")
        if focus != 'patha' and method == 'SME_ZA_OUTER_PIPE':
            print(f"    Path C outer steps: {stats.get('pathc_outer_steps', 0):,}")
            print(f"    Path C pending/commit/squash: "
                  f"{stats.get('pathc_outer_pending_updates', 0):,} / "
                  f"{stats.get('pathc_outer_commit_updates', 0):,} / "
                  f"{stats.get('pathc_outer_squash_discards', 0):,}")
            print(f"    Path C col writes:  "
                  f"{stats.get('pathc_outer_col0_writes', 0):,}/"
                  f"{stats.get('pathc_outer_col1_writes', 0):,}/"
                  f"{stats.get('pathc_outer_col2_writes', 0):,}/"
                  f"{stats.get('pathc_outer_col3_writes', 0):,}/"
                  f"{stats.get('pathc_outer_col4_writes', 0):,}")
            print(f"    Path C final reads: {stats.get('pathc_outer_final_reads', 0):,}")
            print(f"    Path C fw hits/stall: {stats.get('pathc_outer_forward_hits', 0):,} / "
                  f"{stats.get('pathc_outer_forward_stalls', 0):,}")
            print(f"    Path C local outer FU issue/busy/util: "
                  f"{stats.get('pathc_outer_fu_issue_count', 0):,} / "
                  f"{stats.get('pathc_outer_fu_busy_cycles', 0):,} / "
                  f"{stats.get('pathc_outer_fu_utilization', 0.0) * 100.0:.2f}%")
            print("    -- Path C column-ready early-start --")
            print(f"    early outer starts: {stats.get('pathc_early_outer_start_events', 0):,}")
            print(f"    before all columns: {stats.get('pathc_step_before_all_columns_ready', 0):,}")
            print(f"    load/outer overlap: {stats.get('pathc_load_outer_overlap_cycles', 0):,}")
            print(f"    column wait cycles: {stats.get('pathc_column_ready_wait_cycles', 0):,}")
            print(f"    ZA token wait:      {stats.get('pathc_za_token_wait_cycles', 0):,}")
            print(f"    MOVA boundary wait: {stats.get('pathc_mova_boundary_wait_cycles', 0):,}")
            print(f"    lmat port busy:     p0={stats.get('pathc_lmat_port0_busy_cycles', 0):,}, p1={stats.get('pathc_lmat_port1_busy_cycles', 0):,}")
            print(f"    dual-load cycles:   {stats.get('pathc_dual_load_issue_cycles', 0):,}")
            print(f"    avg step after col/vector ready: "
                  f"{stats.get('pathc_step_avg_after_column_ready', 0):.2f} / "
                  f"{stats.get('pathc_step_avg_after_vector_ready', 0):.2f} cycles")
        if focus != 'pathc' and 'fu_busy_dotp' in stats:
            print(f"    dotp FU busy:       {stats['fu_busy_dotp']:,}")
        if focus != 'pathc' and 'fu_busy_pack' in stats:
            print(f"    pack FU busy:       {stats['fu_busy_pack']:,}")
        if 'fu_busy_matld' in stats:
            print(f"    matld FU busy:      {stats['fu_busy_matld']:,}")
        if focus != 'patha' and 'fu_busy_sme_pipe' in stats:
            print(f"    pipe FU busy:       {stats['fu_busy_sme_pipe']:,}")
        if focus != 'patha' and 'fu_busy_sme_outer' in stats:
            print(f"    outer FU busy:      {stats['fu_busy_sme_outer']:,}")
        if focus != 'pathc' and 'fu_busy_patha_stream_ld' in stats:
            print(f"    stream_ld FU busy:  {stats['fu_busy_patha_stream_ld']:,}")
        if focus != 'pathc' and 'fu_busy_patha_store5' in stats:
            print(f"    store5 FU busy:     {stats['fu_busy_patha_store5']:,}")
        if focus != 'patha' and 'fu_busy_sme_fmopa' in stats:
            print(f"    fmopa step FU busy: {stats['fu_busy_sme_fmopa']:,}")
        if focus != 'patha' and 'fu_busy_sme_zero' in stats:
            print(f"    zero ZA FU busy:    {stats['fu_busy_sme_zero']:,}")
        if focus != 'patha' and 'fu_busy_sme_mova' in stats:
            print(f"    mova ZA FU busy:    {stats['fu_busy_sme_mova']:,}")
        if 'fu_busy_memwrite' in stats:
            print(f"    store FU busy:      {stats['fu_busy_memwrite']:,}")
        if focus != 'pathc' and 'fu_busy_intalu' in stats:
            print(f"    IntAlu FU busy:     {stats['fu_busy_intalu']:,}")
        if 'nonspec_added' in stats:
            print(f"    nonSpecInstsAdded:  {stats['nonspec_added']:,}")
        if 'disp_nonspec' in stats:
            print(f"    dispNonSpecInsts:   {stats['disp_nonspec']:,}")
        if 'rename_serializing' in stats:
            print(f"    rename serializing: {stats['rename_serializing']:,}")
        if 'rename_serialize_stall_cycles' in stats:
            print(f"    serialize stalls:   {stats['rename_serialize_stall_cycles']:,} cycles")
        if all(k in stats for k in ('issue_dist_samples', 'issue_dist_0',
                                     'issue_dist_1', 'issue_dist_2',
                                     'issue_dist_3')):
            samples = max(1, stats['issue_dist_samples'])
            ge4 = max(
                0,
                samples - stats['issue_dist_0'] - stats['issue_dist_1'] -
                stats['issue_dist_2'] - stats['issue_dist_3']
            )
            zratio = stats['issue_dist_0'] / samples
            print(
                "    issue dist:         "
                f"0:{stats['issue_dist_0']:,} ({zratio*100:.2f}%), "
                f"1:{stats['issue_dist_1']:,}, "
                f"2:{stats['issue_dist_2']:,}, "
                f"3:{stats['issue_dist_3']:,}, >=4:{ge4:,}"
            )
            print(
                "    issue ratios:       "
                f"0:{stats.get('issue_0_ratio', 0)*100:.2f}%, "
                f"1:{stats.get('issue_1_ratio', 0)*100:.2f}%, "
                f"2+:{stats.get('issue_2plus_ratio', 0)*100:.2f}%"
            )

        print(f"\n  Theoretical Baseline (5x5 matvec):")
        print(f"    FLOPs per matvec:    {THEORY['FLOPs_per_matvec']}")
        print(f"    Memory per matvec:   {logical_bytes_per_matvec} bytes")
        print(f"    CFD instructions:    {THEORY['CFD_instructions']} steady-state")
        print(f"    Document target:     {THEORY['ideal_cfd_doc']} cycles/matvec")
        print(f"    Steady lower bound:  {THEORY['ideal_cfd_steady']} cycles/matvec")
        if method == 'SME_ZA_COLUMN_STREAM_DEPRECATED':
            print("    SME semantics:       deprecated fixed-column ZA[:,0] accumulation")
            print("    SME active tile:     ZA[0:4,0], 5 k-steps, 25 MACs")
            print("    SME note:            legacy Path C; default decoder now uses ZA outer step")
            print(f"    Internal array lat.: {stats.get('sme_fmopa_array_lat', self.config.sme_fmopa_array_lat)} cycles")
            print(f"    ZA token latency:    {stats.get('sme_fmopa_token_lat', self.config.sme_fmopa_lat)} cycles")
            print(f"    Compute lower bound: {stats.get('sme_forwarding_compute_lb', THEORY['sme_fmopa_per_matvec'])} cycles/matvec")
            print(f"    Load BW lower bound: >= {stats.get('sme_load_bandwidth_lb', 6)} cycles/matvec")
            print(f"    Explicit lmat slots: {stats.get('sme_explicit_lmat_slots', 6)} per matvec")
            if 'sme_forwarding_combined_lb' in stats:
                print(f"    Forwarding bound:    >= {stats['sme_forwarding_combined_lb']} cycles/matvec")
            print("    Store lower bound:   >= 1 predicated st1d/matvec")
        elif method == 'SME_ZA_SPATIAL_PIPE':
            print("    Path B semantics:    ZERO + 5 ZA spatial pipe steps + MOVA final ZA[:,4]")
            print("    Path B active tile:  ZA[0:4,0] -> ... -> ZA[0:4,4], 25 MACs")
            print("    Path B note:         no Z partial sums and no REDUCE5/fused MVM")
            print(f"    Pipe step latency:   {stats.get('sme_pipe_lat', self.config.sme_pipe_lat)} cycles")
            print(f"    Pipe engines:        {stats.get('sme_pipe_count', self.config.sme_pipe_count)}")
            print(f"    Compute lower bound: {stats.get('sme_forwarding_compute_lb', THEORY['sme_fmopa_per_matvec'])} cycles/matvec")
            print(f"    Load BW lower bound: >= {stats.get('sme_load_bandwidth_lb', 6)} cycles/matvec")
            print(f"    Explicit lmat slots: {stats.get('sme_explicit_lmat_slots', 6)} per matvec")
            if 'sme_forwarding_combined_lb' in stats:
                print(f"    Forwarding bound:    >= {stats['sme_forwarding_combined_lb']} cycles/matvec")
            print("    Store lower bound:   >= 1 predicated st1d/matvec")
        elif method == 'SME_ZA_OUTER_PIPE':
            print("    Path C semantics:    ZERO + 5 selected-column outer steps + MOVA final ZA[:,4]")
            print("    Path C active tile:  conceptual outer[i,j]=A[i,k]*x[j], selected update ZA[:,k]")
            print("    Path C recurrence:   ZA[:,0]=A[:,0]*x0; ZA[:,k]=ZA[:,k-1]+A[:,k]*xk")
            print("    Path C note:         no fixed ZA[:,0] accumulation, no REDUCE5/fused MVM")
            print(f"    Outer step latency:  {stats.get('sme_outer_lat', self.config.sme_outer_lat)} cycles")
            print(f"    Outer engines:       {stats.get('sme_outer_count', self.config.sme_outer_count)}")
            print(f"    Full-array label:    {bool(stats.get('sme_outer_full_array', 0))}")
            print(f"    Compute lower bound: {stats.get('sme_forwarding_compute_lb', THEORY['sme_fmopa_per_matvec'])} cycles/matvec")
            print(f"    Load BW lower bound: >= {stats.get('sme_load_bandwidth_lb', 6)} cycles/matvec")
            print(f"    Explicit lmat slots: {stats.get('sme_explicit_lmat_slots', 6)} per matvec")
            if 'sme_forwarding_combined_lb' in stats:
                print(f"    Forwarding bound:    >= {stats['sme_forwarding_combined_lb']} cycles/matvec")
            print("    Store lower bound:   >= 1 predicated st1d/matvec")

        if stats.get('cycles_per_iter', 0) > 0:
            cycles_per_iter = stats['cycles_per_iter']
            print(f"\n  Per-Iteration Cost:")
            print(f"    Cycles/matvec:       {cycles_per_iter:.2f} (measured)")
            doc_eff = THEORY['ideal_cfd_doc'] / cycles_per_iter
            steady_eff = THEORY['ideal_cfd_steady'] / cycles_per_iter
            print(f"    Doc target eff.:     {doc_eff*100:.2f}%")
            print(f"    Steady-bound eff.:   {steady_eff*100:.2f}%")
            if 'sme_forwarding_combined_lb' in stats:
                forwarding_eff = stats['sme_forwarding_combined_lb'] / cycles_per_iter
                print(f"    Forwarding-bound eff.: {forwarding_eff*100:.2f}%")

            insts_per_iter = stats['committed_insts'] / inferred_matvecs if 'committed_insts' in stats and inferred_matvecs else 0
            print(f"    Total insts/matvec:  {insts_per_iter:.1f}")
            if method == 'DOTP_ROW' and inferred_matvecs:
                if 'intalu_issued' in stats:
                    print(f"    IntAlu issued/matvec:{stats['intalu_issued'] / inferred_matvecs:.2f}")
                if 'memwrite_issued' in stats:
                    print(f"    MemWrite issued/matvec:{stats['memwrite_issued'] / inferred_matvecs:.2f}")
                if 'branches_executed' in stats:
                    print(f"    Branches/matvec:    {stats['branches_executed'] / inferred_matvecs:.2f}")
            print(f"    Bytes/cycle:         {logical_bytes_per_matvec/cycles_per_iter:.2f}")
            if 'effective_flops_per_cycle' in stats:
                print(f"    Effective FLOPs/cycle: {stats['effective_flops_per_cycle']:.3f}")
            if method == 'DOTP_ROW' and stats.get('patha_store_mode') == 'pred40':
                if stats.get('patha_store_stride', self.config.patha_store_stride) == 40:
                    print("    Path A pred40 note:  exact 40B store with compact40 result layout; "
                          "compare stride64 to isolate layout/cache-line effects")
                else:
                    print("    Path A pred40 note:  exact 40B store with 64B-spaced layout; "
                          "extra cycles indicate result-layout/cache-line pressure, not extra store bytes")

            flops_per_byte = THEORY['FLOPs_per_matvec'] / logical_bytes_per_matvec
            print(f"    Arithmetic intensity: {flops_per_byte:.2f} FLOPs/byte")

        print(f"\n  Bottleneck Diagnosis:")
        dotp_busy = stats.get('fu_busy_dotp', 0)
        matld_busy = stats.get('fu_busy_matld', 0)
        sme_fmopa_busy = stats.get('fu_busy_sme_fmopa', 0)
        if sme_fmopa_busy > max(dotp_busy, matld_busy):
            print("    Primary:            CFD_SME_FMOPA_Engine issue/latency")
        elif dotp_busy > matld_busy and dotp_busy > 0:
            print("    Primary:            CFD_Matrix_Engine issue bandwidth")
        elif matld_busy > 0:
            print("    Primary:            SPM/matld read-port pressure")
        else:
            print("    Primary:            no dominant CFD FU stall in stats")
        if stats.get('rename_iq_full', 0):
            print(f"    Rename IQ full:      {stats['rename_iq_full']:,}")
        if stats.get('rename_blocked_cycles', 0):
            print(f"    Rename blocked cyc:  {stats['rename_blocked_cycles']:,}")
        if stats.get('dispatch_blocked_cycles', 0):
            print(f"    Dispatch blocked cyc:{stats['dispatch_blocked_cycles']:,}")
        if stats.get('iew_lsq_full', 0):
            print(f"    LSQ full events:     {stats['iew_lsq_full']:,}")
        if stats.get('lsq_blocked_by_cache', 0):
            print(f"    Cache block events:  {stats['lsq_blocked_by_cache']:,}")
        if stats.get('spm_bytes_total', 0) == 0 and implied_spm_read_bytes == 0:
            print("    Warning:            SPM traffic was not observed")

        print("\n" + "=" * 70)
        print("  Performance Rating:")
        if stats.get('cycles_per_iter', 0) > 0:
            cycles_per_iter = stats['cycles_per_iter']
            forwarding_bound = stats.get('sme_forwarding_combined_lb', 0)
            if forwarding_bound and cycles_per_iter <= forwarding_bound * 1.15:
                print("    [MEETS] near ZA-forwarding lower-bound performance")
            elif cycles_per_iter <= THEORY['ideal_cfd_steady'] * 1.05:
                print("    [MEETS] at steady-state lower-bound performance")
            elif cycles_per_iter <= THEORY['ideal_cfd_doc']:
                print("    [MEETS] below documented 8-cycle target")
            elif cycles_per_iter <= THEORY['ideal_cfd_doc'] * 1.25:
                print("    [CLOSE] modest overhead above documented target")
            else:
                print("    [LOW] above target; continue bottleneck tuning")
        print("=" * 70)

    def parse_stats_file(self, filepath):
        with open(filepath, 'r') as f:
            content = f.read()
        segments = re.findall(
            r'-+ Begin Simulation Statistics -+\n(.*?)\n-+ End Simulation Statistics\s+-+',
            content, re.DOTALL)
        if not segments:
            return self.analyze_stats(content)

        analyzed = [self.analyze_stats(seg) for seg in segments]

        def work_key(item):
            work = max(item.get('sme_fmopa_committed', 0),
                       item.get('cfd_dotp_committed', 0),
                       item.get('cfd_matld_committed', 0))
            # If multiple dumps contain the same benchmark work, prefer the
            # earliest/smallest cycle segment.  The final implicit exit dump may
            # include post-benchmark verification after m5_dump_stats().
            cycles = item.get('total_cycles', 1 << 62)
            return (work, -cycles)

        best = max(analyzed, key=work_key)
        best['stats_segments'] = len(analyzed)
        best['selected_stats_segment'] = analyzed.index(best) + 1
        return best

def bind(config):
    return ReportContext(config)
