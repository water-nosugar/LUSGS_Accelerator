static int
patha_default_config_match(const Config *cfg)
{
    return !strcmp(cfg->patha_store_mode, "pred40") &&
           cfg->patha_store_stride == 40 &&
           cfg->patha_unroll == 8 &&
           !strcmp(cfg->patha_acc_mode, "internal-buffer") &&
           cfg->patha_buffer_depth == 2 &&
           cfg->patha_streaming == 0 &&
           !strcmp(cfg->patha_kernel, "baseline");
}

static double
ratio_or_zero(uint64_t num, uint64_t den)
{
    if (den == 0)
        return 0.0;
    return (double)num / (double)den;
}

static void
print_trace(const Problem *p, const Config *cfg)
{
    if (!cfg->trace_enable)
        return;
    size_t lines = cfg->trace_lines < p->lines ? cfg->trace_lines : p->lines;
    size_t cells = cfg->trace_cells < p->cells ? cfg->trace_cells : p->cells;
    printf("lusgs.trace.lines = %zu\n", lines);
    printf("lusgs.trace.cells = %zu\n", cells);
    for (size_t line = 0; line < lines; line++) {
        for (size_t cell = 0; cell < cells; cell++) {
            const double *ref = vec_const_at(p->dq_ref, p, line, cell);
            const double *step1 = vec_const_at(p->dq_step1, p, line, cell);
            const double *step2 = vec_const_at(p->dq_step2, p, line, cell);
            const double *step2_core =
                vec_const_at(p->dq_step2_core, p, line, cell);
            const double *step2_core_forwarded =
                vec_const_at(p->dq_step2_core_forwarded, p, line, cell);
            const double *step2_core_forwarded_context =
                vec_const_at(p->dq_step2_core_forwarded_context, p, line,
                             cell);
            const double *step2_core_forwarded_linebuf =
                vec_const_at(p->dq_step2_core_forwarded_linebuf, p, line,
                             cell);
            printf("lusgs.trace[%zu][%zu].ref =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", ref[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step1 =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step1[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step2 =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step2[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step2_core =", line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step2_core[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step2_core_forwarded =",
                   line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step2_core_forwarded[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step2_core_forwarded_context =",
                   line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step2_core_forwarded_context[lane]);
            printf("\n");
            printf("lusgs.trace[%zu][%zu].step2_core_forwarded_linebuf =",
                   line, cell);
            for (int lane = 0; lane < N; lane++)
                printf(" %.17g", step2_core_forwarded_linebuf[lane]);
            printf("\n");
        }
    }
}

static void
print_mode_stats(const char *prefix, const Stats *stats)
{
    uint64_t actual_total_mvm =
        stats->patha_mvm_forward + stats->patha_mvm_backward;
    uint64_t tmp_bytes =
        stats->tmp_result_store_bytes + stats->tmp_result_load_bytes;

    printf("%s.forwardCells = %lu\n", prefix,
           (unsigned long)stats->forward_cells);
    printf("%s.backwardCells = %lu\n", prefix,
           (unsigned long)stats->backward_cells);
    printf("%s.pathaMvmForward = %lu\n", prefix,
           (unsigned long)stats->patha_mvm_forward);
    printf("%s.pathaMvmBackward = %lu\n", prefix,
           (unsigned long)stats->patha_mvm_backward);
    printf("%s.forwardContextHits = %lu\n", prefix,
           (unsigned long)stats->forward_context_hits);
    printf("%s.backwardContextHits = %lu\n", prefix,
           (unsigned long)stats->backward_context_hits);
    printf("%s.pathaMvmTotal = %lu\n", prefix,
           (unsigned long)actual_total_mvm);
    printf("%s.softwareTrsv = %lu\n", prefix,
           (unsigned long)stats->software_trsv);
    printf("%s.hardwareTrsv = %lu\n", prefix,
           (unsigned long)stats->hardware_trsv);
    printf("%s.softwareTrsvHotPath = %lu\n", prefix,
           (unsigned long)stats->software_trsv_hot_path);
    printf("%s.vectorSubOperations = %lu\n", prefix,
           (unsigned long)stats->vector_sub_ops);
    printf("%s.vectorSubLanes = %lu\n", prefix,
           (unsigned long)(stats->vector_sub_ops * N));
    printf("%s.vectorSubLaneOps = %lu\n", prefix,
           (unsigned long)(stats->vector_sub_ops * N));
    printf("%s.fusedForwardUpdate = %lu\n", prefix,
           (unsigned long)stats->fused_forward_updates);
    printf("%s.fusedBackwardUpdate = %lu\n", prefix,
           (unsigned long)stats->fused_backward_updates);
    printf("%s.fusedSubOps = %lu\n", prefix,
           (unsigned long)stats->fused_sub_ops);
    printf("%s.hardwareVectorSub = %lu\n", prefix,
           (unsigned long)stats->hardware_vector_sub);
    printf("%s.trsv5RhsForwarded = %lu\n", prefix,
           (unsigned long)stats->trsv5_rhs_forwarded);
    printf("%s.trsv5RhsSpmStageElided = %lu\n", prefix,
           (unsigned long)stats->trsv5_rhs_spm_stage_elided);
    printf("%s.step2CoreForwardRhsStackStoreElided = %lu\n", prefix,
           (unsigned long)stats->step2_core_forward_rhs_stack_store_elided);
    printf("%s.trsv5RhsForwardStallCycles = %lu\n", prefix,
           (unsigned long)stats->trsv5_rhs_forward_stall_cycles);
    printf("%s.trsv5RhsForwardInvalid = %lu\n", prefix,
           (unsigned long)stats->trsv5_rhs_forward_invalid);
    printf("%s.trsv5RhsForwardConsumed = %lu\n", prefix,
           (unsigned long)stats->trsv5_rhs_forward_consumed);
    printf("%s.forwardContextHits = %lu\n", prefix,
           (unsigned long)stats->forward_context_hits);
    printf("%s.forwardContextMisses = %lu\n", prefix,
           (unsigned long)stats->forward_context_misses);
    printf("%s.forwardDqstarHeapVectorLoadElided = %lu\n", prefix,
           (unsigned long)stats->forward_dqstar_heap_vector_load_elided);
    printf("%s.forwardContextVectorStages = %lu\n", prefix,
           (unsigned long)stats->forward_context_vector_stages);
    printf("%s.forwardContextUpdates = %lu\n", prefix,
           (unsigned long)stats->forward_context_updates);
    printf("%s.forwardContextInvalid = %lu\n", prefix,
           (unsigned long)stats->forward_context_invalid);
    printf("%s.forwardContextStallCycles = %lu\n", prefix,
           (unsigned long)stats->forward_context_stall_cycles);
    printf("%s.backwardContextHits = %lu\n", prefix,
           (unsigned long)stats->backward_context_hits);
    printf("%s.backwardContextMisses = %lu\n", prefix,
           (unsigned long)stats->backward_context_misses);
    printf("%s.backwardDqHeapVectorLoadElided = %lu\n", prefix,
           (unsigned long)stats->backward_dq_heap_vector_load_elided);
    printf("%s.backwardContextVectorStages = %lu\n", prefix,
           (unsigned long)stats->backward_context_vector_stages);
    printf("%s.backwardContextUpdates = %lu\n", prefix,
           (unsigned long)stats->backward_context_updates);
    printf("%s.backwardContextInvalid = %lu\n", prefix,
           (unsigned long)stats->backward_context_invalid);
    printf("%s.backwardContextStallCycles = %lu\n", prefix,
           (unsigned long)stats->backward_context_stall_cycles);
    printf("%s.lineContextForwardingEnabled = %lu\n", prefix,
           (unsigned long)stats->line_context_forwarding_enabled);
    printf("%s.lineContextTotalHits = %lu\n", prefix,
           (unsigned long)stats->line_context_total_hits);
    printf("%s.lineContextTotalMisses = %lu\n", prefix,
           (unsigned long)stats->line_context_total_misses);
    printf("%s.linebufForwardReads = %lu\n", prefix,
           (unsigned long)stats->linebuf_forward_reads);
    printf("%s.linebufForwardWrites = %lu\n", prefix,
           (unsigned long)stats->linebuf_forward_writes);
    printf("%s.linebufBackwardReads = %lu\n", prefix,
           (unsigned long)stats->linebuf_backward_reads);
    printf("%s.linebufBackwardWrites = %lu\n", prefix,
           (unsigned long)stats->linebuf_backward_writes);
    printf("%s.linebufForwardHits = %lu\n", prefix,
           (unsigned long)stats->linebuf_forward_hits);
    printf("%s.linebufForwardMisses = %lu\n", prefix,
           (unsigned long)stats->linebuf_forward_misses);
    printf("%s.linebufBackwardHits = %lu\n", prefix,
           (unsigned long)stats->linebuf_backward_hits);
    printf("%s.linebufBackwardMisses = %lu\n", prefix,
           (unsigned long)stats->linebuf_backward_misses);
    printf("%s.linebufSpmVectorStagesElided = %lu\n", prefix,
           (unsigned long)stats->linebuf_spm_vector_stages_elided);
    printf("%s.linebufLmatVecLoadsElided = %lu\n", prefix,
           (unsigned long)stats->linebuf_lmat_vec_loads_elided);
    printf("%s.linebufContextMemoryStoresElided = %lu\n", prefix,
           (unsigned long)stats->linebuf_context_memory_stores_elided);
    printf("%s.linebufContextMemoryLoadsElided = %lu\n", prefix,
           (unsigned long)stats->linebuf_context_memory_loads_elided);
    printf("%s.linebufTagConflicts = %lu\n", prefix,
           (unsigned long)stats->linebuf_tag_conflicts);
    printf("%s.linebufInvalidReads = %lu\n", prefix,
           (unsigned long)stats->linebuf_invalid_reads);
    printf("%s.linebufStallCycles = %lu\n", prefix,
           (unsigned long)stats->linebuf_stall_cycles);
    printf("%s.linebufForwardingEnabled = %lu\n", prefix,
           (unsigned long)stats->linebuf_forwarding_enabled);
    printf("%s.tmpResultStores = %lu\n", prefix,
           (unsigned long)stats->tmp_result_stores);
    printf("%s.tmpResultLoads = %lu\n", prefix,
           (unsigned long)stats->tmp_result_loads);
    printf("%s.tmpResultStoreBytes = %lu\n", prefix,
           (unsigned long)stats->tmp_result_store_bytes);
    printf("%s.tmpResultLoadBytes = %lu\n", prefix,
           (unsigned long)stats->tmp_result_load_bytes);
    printf("%s.tmpResultBytes = %lu\n", prefix, (unsigned long)tmp_bytes);
    printf("%s.forwardCycles = %lu\n", prefix,
           (unsigned long)stats->forward_cycles);
    printf("%s.backwardCycles = %lu\n", prefix,
           (unsigned long)stats->backward_cycles);
    printf("%s.totalCycles = %lu\n", prefix,
           (unsigned long)stats->total_cycles);
    printf("%s.cyclesPerMvm = %.6f\n", prefix,
           ratio_or_zero(stats->total_cycles, actual_total_mvm));
    printf("%s.cyclesPerTrsv = %.6f\n", prefix,
           ratio_or_zero(stats->total_cycles,
                         stats->hardware_trsv + stats->software_trsv));
    printf("%s.softwareTrsvCycles = %lu\n", prefix,
           (unsigned long)stats->software_trsv_cycles);
    printf("%s.trsv5LatencyConfigured = %lu\n", prefix,
           (unsigned long)stats->trsv5_latency_configured);
}

static void
print_compare(const char *prefix, const CompareResult *cmp)
{
    printf("%s.max_abs_error = %.17g\n", prefix, cmp->max_abs_error);
    printf("%s.max_rel_error = %.17g\n", prefix, cmp->max_rel_error);
    printf("%s.mismatch_count = %lu\n", prefix,
           (unsigned long)cmp->mismatch_count);
    printf("%s.first_mismatch_line = %ld\n", prefix, cmp->first_line);
    printf("%s.first_mismatch_cell = %ld\n", prefix, cmp->first_cell);
    printf("%s.first_mismatch_lane = %ld\n", prefix, cmp->first_lane);
    printf("%s.first_mismatch_ref = %.17g\n", prefix, cmp->first_ref);
    printf("%s.first_mismatch_got = %.17g\n", prefix, cmp->first_got);
}

static void
print_report(const Config *cfg, const Stats *ref_stats,
             const Stats *step1_stats, const Stats *step2_stats,
             const Stats *step2_core_stats,
             const Stats *step2_core_forwarded_stats,
             const Stats *step2_core_forwarded_context_stats,
             const Stats *step2_core_forwarded_linebuf_stats,
             const CompareResult *step1_cmp,
             const CompareResult *step2_cmp,
             const CompareResult *step2_vs_step1_cmp,
             const CompareResult *step2_core_cmp,
             const CompareResult *step2_core_vs_step2_cmp,
             const CompareResult *step2_core_forwarded_cmp,
             const CompareResult *step2_core_forwarded_vs_step2_core_cmp,
             const CompareResult *step2_core_forwarded_context_cmp,
             const CompareResult *step2_core_forwarded_context_vs_forwarded_cmp,
             const CompareResult *step2_core_forwarded_context_vs_core_cmp,
             const CompareResult *step2_core_forwarded_linebuf_cmp,
             const CompareResult *step2_core_forwarded_linebuf_vs_context_cmp,
             const CompareResult *step2_core_forwarded_linebuf_vs_forwarded_cmp,
             const CompareResult *step2_core_forwarded_linebuf_vs_core_cmp,
             int counts_ok, int config_match)
{
    uint64_t mvm_per_dir_per_sweep =
        (uint64_t)cfg->lines * (uint64_t)(cfg->cells > 0 ? cfg->cells - 1 : 0);
    uint64_t expected_forward_mvm = mvm_per_dir_per_sweep * cfg->sweeps;
    uint64_t expected_backward_mvm = mvm_per_dir_per_sweep * cfg->sweeps;
    uint64_t expected_total_mvm = expected_forward_mvm + expected_backward_mvm;
    uint64_t expected_trsv =
        (uint64_t)cfg->lines * (uint64_t)cfg->cells * cfg->sweeps;
    uint64_t expected_vector_sub = expected_total_mvm;
    uint64_t total_cells =
        (uint64_t)cfg->lines * (uint64_t)cfg->cells * cfg->sweeps;
    uint64_t expected_all_cells = total_cells;
    uint64_t expected_linebuf_ordinary_cells =
        expected_forward_mvm;
    uint64_t step2_total_mvm =
        step2_stats->patha_mvm_forward + step2_stats->patha_mvm_backward;
    uint64_t step2_core_total_mvm =
        step2_core_stats->patha_mvm_forward +
        step2_core_stats->patha_mvm_backward;
    uint64_t step2_core_forwarded_total_mvm =
        step2_core_forwarded_stats->patha_mvm_forward +
        step2_core_forwarded_stats->patha_mvm_backward;
    uint64_t step2_core_forwarded_context_total_mvm =
        step2_core_forwarded_context_stats->patha_mvm_forward +
        step2_core_forwarded_context_stats->patha_mvm_backward;
    uint64_t step2_core_forwarded_linebuf_total_mvm =
        step2_core_forwarded_linebuf_stats->patha_mvm_forward +
        step2_core_forwarded_linebuf_stats->patha_mvm_backward;
    const Stats *rhs_forwarding_alias_stats =
        cfg->run_step2_core_forwarded_linebuf ?
        step2_core_forwarded_linebuf_stats :
        (cfg->run_step2_core_forwarded_context ?
         step2_core_forwarded_context_stats : step2_core_forwarded_stats);

    printf("\n=== LU-SGS Path A Step2 Report ===\n");
    printf("lusgs.lines = %zu\n", cfg->lines);
    printf("lusgs.cells = %zu\n", cfg->cells);
    printf("lusgs.interleave = %zu\n", cfg->interleave);
    printf("lusgs.sweeps = %lu\n", (unsigned long)cfg->sweeps);
    printf("mode.reference = %d\n", cfg->run_reference);
    printf("mode.step1 = %d\n", cfg->run_step1);
    printf("mode.step2 = %d\n", cfg->run_step2);
    printf("mode.step2_core = %d\n", cfg->run_step2_core);
    printf("mode.step2_core_forwarded = %d\n",
           cfg->run_step2_core_forwarded);
    printf("mode.step2_core_forwarded_context = %d\n",
           cfg->run_step2_core_forwarded_context);
    printf("mode.step2_core_forwarded_linebuf = %d\n",
           cfg->run_step2_core_forwarded_linebuf);
    printf("mode.step2_pretransform = %d\n", cfg->run_step2_pretransform);
    printf("mode.step2_pretransform_context = %d\n",
           cfg->run_step2_pretransform_context);
    printf("lusgs.linebufEnable = %d\n", cfg->linebuf_enable);
    printf("lusgs.linebufEntries = %lu\n",
           (unsigned long)cfg->linebuf_entries);
    printf("lusgs.referenceMvmForward = %lu\n",
           (unsigned long)ref_stats->reference_mvm_forward);
    printf("lusgs.referenceMvmBackward = %lu\n",
           (unsigned long)ref_stats->reference_mvm_backward);
    printf("lusgs.referenceMvmTotal = %lu\n",
           (unsigned long)(ref_stats->reference_mvm_forward +
                           ref_stats->reference_mvm_backward));
    printf("lusgs.referenceSoftwareTrsv = %lu\n",
           (unsigned long)ref_stats->reference_software_trsv);
    printf("lusgs.expected.pathaMvmForward = %lu\n",
           (unsigned long)expected_forward_mvm);
    printf("lusgs.expected.pathaMvmBackward = %lu\n",
           (unsigned long)expected_backward_mvm);
    printf("lusgs.expected.pathaMvmTotal = %lu\n",
           (unsigned long)expected_total_mvm);
    printf("lusgs.expected.softwareTrsv = %lu\n",
           (unsigned long)expected_trsv);
    printf("lusgs.expected.hardwareTrsv = %lu\n",
           (unsigned long)expected_trsv);
    printf("lusgs.expected.vectorSubOperations = %lu\n",
           (unsigned long)expected_vector_sub);
    printf("lusgs.expected.fusedForwardUpdate = %lu\n",
           (unsigned long)expected_forward_mvm);
    printf("lusgs.expected.fusedBackwardUpdate = %lu\n",
           (unsigned long)expected_backward_mvm);
    printf("lusgs.expected.trsv5RhsForwarded = %lu\n",
           (unsigned long)expected_forward_mvm);
    printf("lusgs.expected.forwardContextHits = %lu\n",
           (unsigned long)expected_forward_mvm);
    printf("lusgs.expected.backwardContextHits = %lu\n",
           (unsigned long)expected_backward_mvm);
    printf("lusgs.expected.lineContextTotalHits = %lu\n",
           (unsigned long)(expected_forward_mvm + expected_backward_mvm));
    printf("lusgs.expected.linebufForwardReads = %lu\n",
           (unsigned long)expected_linebuf_ordinary_cells);
    printf("lusgs.expected.linebufForwardWrites = %lu\n",
           (unsigned long)expected_all_cells);
    printf("lusgs.expected.linebufBackwardReads = %lu\n",
           (unsigned long)expected_linebuf_ordinary_cells);
    printf("lusgs.expected.linebufBackwardWrites = %lu\n",
           (unsigned long)expected_all_cells);
    printf("lusgs.expected.linebufForwardHits = %lu\n",
           (unsigned long)expected_linebuf_ordinary_cells);
    printf("lusgs.expected.linebufBackwardHits = %lu\n",
           (unsigned long)expected_linebuf_ordinary_cells);
    printf("lusgs.expected.linebufSpmVectorStagesElided = %lu\n",
           (unsigned long)(2U * expected_linebuf_ordinary_cells));
    printf("lusgs.expected.linebufLmatVecLoadsElided = %lu\n",
           (unsigned long)(2U * expected_linebuf_ordinary_cells));

    print_mode_stats("lusgs.step1", step1_stats);
    print_mode_stats("lusgs.step2", step2_stats);
    print_mode_stats("lusgs.step2_core", step2_core_stats);
    print_mode_stats("lusgs.step2_core_forwarded",
                     step2_core_forwarded_stats);
    print_mode_stats("lusgs.step2_core_forwarded_context",
                     step2_core_forwarded_context_stats);
    print_mode_stats("lusgs.step2_core_forwarded_linebuf",
                     step2_core_forwarded_linebuf_stats);

    printf("lusgs.pathaMvmTotal = %lu\n", (unsigned long)step2_total_mvm);
    printf("lusgs.hardwareTrsv = %lu\n",
           (unsigned long)step2_stats->hardware_trsv);
    printf("lusgs.softwareTrsvHotPath = %lu\n",
           (unsigned long)step2_stats->software_trsv_hot_path);
    printf("lusgs.softwareTrsv = %lu\n",
           (unsigned long)step2_stats->software_trsv);
    printf("lusgs.vectorSubOperations = %lu\n",
           (unsigned long)step2_stats->vector_sub_ops);
    printf("lusgs.totalCycles = %lu\n",
           (unsigned long)step2_stats->total_cycles);
    printf("lusgs.step2BaselineCycles = %lu\n",
           (unsigned long)step2_stats->total_cycles);
    printf("lusgs.step2CoreCycles = %lu\n",
           (unsigned long)step2_core_stats->total_cycles);
    printf("lusgs.step2CoreForwardedCycles = %lu\n",
           (unsigned long)step2_core_forwarded_stats->total_cycles);
    printf("lusgs.step2CoreForwardedContextCycles = %lu\n",
           (unsigned long)step2_core_forwarded_context_stats->total_cycles);
    printf("lusgs.step2CoreForwardedLinebufCycles = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->total_cycles);
    printf("lusgs.step2CoreSpeedup = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles,
                         step2_core_stats->total_cycles));
    printf("lusgs.step2CoreForwardedSpeedupVsStep2 = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles,
                         step2_core_forwarded_stats->total_cycles));
    printf("lusgs.step2CoreForwardedSpeedupVsCore = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles,
                         step2_core_forwarded_stats->total_cycles));
    printf("lusgs.step2CoreForwardedContextSpeedupVsStep2 = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles,
                         step2_core_forwarded_context_stats->total_cycles));
    printf("lusgs.step2CoreForwardedContextSpeedupVsCore = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles,
                         step2_core_forwarded_context_stats->total_cycles));
    printf("lusgs.step2CoreForwardedContextSpeedupVsForwarded = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         step2_core_forwarded_context_stats->total_cycles));
    printf("lusgs.step2CoreForwardedLinebufSpeedupVsStep2 = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles,
                         step2_core_forwarded_linebuf_stats->total_cycles));
    printf("lusgs.step2CoreForwardedLinebufSpeedupVsContext = %.6f\n",
           ratio_or_zero(step2_core_forwarded_context_stats->total_cycles,
                         step2_core_forwarded_linebuf_stats->total_cycles));
    printf("lusgs.step2CoreForwardedLinebufSpeedupVsForwarded = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         step2_core_forwarded_linebuf_stats->total_cycles));
    printf("lusgs.fusedForwardUpdate = %lu\n",
           (unsigned long)step2_core_stats->fused_forward_updates);
    printf("lusgs.fusedBackwardUpdate = %lu\n",
           (unsigned long)step2_core_stats->fused_backward_updates);
    printf("lusgs.fusedSubOps = %lu\n",
           (unsigned long)step2_core_stats->fused_sub_ops);
    printf("lusgs.hardwareVectorSub = %lu\n",
           (unsigned long)step2_core_stats->hardware_vector_sub);
    printf("lusgs.trsv5RhsForwarded = %lu\n",
           (unsigned long)rhs_forwarding_alias_stats->trsv5_rhs_forwarded);
    printf("lusgs.trsv5RhsSpmStageElided = %lu\n",
           (unsigned long)
           rhs_forwarding_alias_stats->trsv5_rhs_spm_stage_elided);
    printf("lusgs.step2CoreForwardRhsStackStoreElided = %lu\n",
           (unsigned long)rhs_forwarding_alias_stats->
               step2_core_forward_rhs_stack_store_elided);
    printf("lusgs.trsv5RhsForwardStallCycles = %lu\n",
           (unsigned long)
           rhs_forwarding_alias_stats->trsv5_rhs_forward_stall_cycles);
    printf("lusgs.trsv5RhsForwardInvalid = %lu\n",
           (unsigned long)
           rhs_forwarding_alias_stats->trsv5_rhs_forward_invalid);
    printf("lusgs.trsv5RhsForwardConsumed = %lu\n",
           (unsigned long)
           rhs_forwarding_alias_stats->trsv5_rhs_forward_consumed);
    const Stats *line_context_alias_stats =
        cfg->run_step2_core_forwarded_linebuf ?
        step2_core_forwarded_linebuf_stats :
        step2_core_forwarded_context_stats;
    printf("lusgs.forwardContextHits = %lu\n",
           (unsigned long)line_context_alias_stats->forward_context_hits);
    printf("lusgs.forwardContextMisses = %lu\n",
           (unsigned long)line_context_alias_stats->forward_context_misses);
    printf("lusgs.forwardDqstarHeapVectorLoadElided = %lu\n",
           (unsigned long)line_context_alias_stats->
               forward_dqstar_heap_vector_load_elided);
    printf("lusgs.backwardContextHits = %lu\n",
           (unsigned long)line_context_alias_stats->backward_context_hits);
    printf("lusgs.backwardContextMisses = %lu\n",
           (unsigned long)line_context_alias_stats->backward_context_misses);
    printf("lusgs.backwardDqHeapVectorLoadElided = %lu\n",
           (unsigned long)line_context_alias_stats->
               backward_dq_heap_vector_load_elided);
    printf("lusgs.lineContextForwardingEnabled = %lu\n",
           (unsigned long)line_context_alias_stats->
               line_context_forwarding_enabled);
    printf("lusgs.lineContextTotalHits = %lu\n",
           (unsigned long)line_context_alias_stats->line_context_total_hits);
    printf("lusgs.lineContextTotalMisses = %lu\n",
           (unsigned long)line_context_alias_stats->line_context_total_misses);
    printf("lusgs.linebufForwardReads = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_forward_reads);
    printf("lusgs.linebufForwardWrites = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_forward_writes);
    printf("lusgs.linebufBackwardReads = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_backward_reads);
    printf("lusgs.linebufBackwardWrites = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_backward_writes);
    printf("lusgs.linebufForwardHits = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_forward_hits);
    printf("lusgs.linebufForwardMisses = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_forward_misses);
    printf("lusgs.linebufBackwardHits = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_backward_hits);
    printf("lusgs.linebufBackwardMisses = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_backward_misses);
    printf("lusgs.linebufSpmVectorStagesElided = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_spm_vector_stages_elided);
    printf("lusgs.linebufLmatVecLoadsElided = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_lmat_vec_loads_elided);
    printf("lusgs.linebufContextMemoryStoresElided = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_context_memory_stores_elided);
    printf("lusgs.linebufContextMemoryLoadsElided = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_context_memory_loads_elided);
    printf("lusgs.linebufTagConflicts = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_tag_conflicts);
    printf("lusgs.linebufInvalidReads = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_invalid_reads);
    printf("lusgs.linebufStallCycles = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_stall_cycles);
    printf("lusgs.linebufForwardingEnabled = %lu\n",
           (unsigned long)step2_core_forwarded_linebuf_stats->
               linebuf_forwarding_enabled);
    printf("lusgs.cycleSource = m5_rpns_x2_for_2GHz\n");
    printf("lusgs.softwareTrsvCycles = %lu\n",
           (unsigned long)step1_stats->software_trsv_cycles);
    printf("lusgs.softwareTrsvCycleSource = calibration_before_stats_reset\n");
    printf("lusgs.step2.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles, total_cells));
    printf("lusgs.step2.cyclesPerMvm = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles, step2_total_mvm));
    printf("lusgs.step2.cyclesPerSweep = %.6f\n",
           ratio_or_zero(step2_stats->total_cycles, cfg->sweeps));
    printf("lusgs.step2_core.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles, total_cells));
    printf("lusgs.step2_core.cyclesPerMvm = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles,
                         step2_core_total_mvm));
    printf("lusgs.step2_core.cyclesPerTrsv = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles,
                         step2_core_stats->hardware_trsv));
    printf("lusgs.step2_core.cyclesPerSweep = %.6f\n",
           ratio_or_zero(step2_core_stats->total_cycles, cfg->sweeps));
    printf("lusgs.step2_core_forwarded.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         total_cells));
    printf("lusgs.step2_core_forwarded.cyclesPerMvm = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         step2_core_forwarded_total_mvm));
    printf("lusgs.step2_core_forwarded.cyclesPerTrsv = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         step2_core_forwarded_stats->hardware_trsv));
    printf("lusgs.step2_core_forwarded.cyclesPerSweep = %.6f\n",
           ratio_or_zero(step2_core_forwarded_stats->total_cycles,
                         cfg->sweeps));
    printf("lusgs.step2_core_forwarded_context.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(step2_core_forwarded_context_stats->total_cycles,
                         total_cells));
    printf("lusgs.step2_core_forwarded_context.cyclesPerMvm = %.6f\n",
           ratio_or_zero(step2_core_forwarded_context_stats->total_cycles,
                         step2_core_forwarded_context_total_mvm));
    printf("lusgs.step2_core_forwarded_context.cyclesPerTrsv = %.6f\n",
           ratio_or_zero(step2_core_forwarded_context_stats->total_cycles,
                         step2_core_forwarded_context_stats->hardware_trsv));
    printf("lusgs.step2_core_forwarded_context.cyclesPerSweep = %.6f\n",
           ratio_or_zero(step2_core_forwarded_context_stats->total_cycles,
                         cfg->sweeps));
    printf("lusgs.step2_core_forwarded_linebuf.cyclesPerFullCell = %.6f\n",
           ratio_or_zero(step2_core_forwarded_linebuf_stats->total_cycles,
                         total_cells));
    printf("lusgs.step2_core_forwarded_linebuf.cyclesPerMvm = %.6f\n",
           ratio_or_zero(step2_core_forwarded_linebuf_stats->total_cycles,
                         step2_core_forwarded_linebuf_total_mvm));
    printf("lusgs.step2_core_forwarded_linebuf.cyclesPerTrsv = %.6f\n",
           ratio_or_zero(step2_core_forwarded_linebuf_stats->total_cycles,
                         step2_core_forwarded_linebuf_stats->hardware_trsv));
    printf("lusgs.step2_core_forwarded_linebuf.cyclesPerSweep = %.6f\n",
           ratio_or_zero(step2_core_forwarded_linebuf_stats->total_cycles,
                         cfg->sweeps));
    printf("lusgs.step1ToStep2Speedup = %.6f\n",
           ratio_or_zero(step1_stats->total_cycles, step2_stats->total_cycles));
    printf("lusgs.step2.trsvShareEstimate = %.6f\n",
           ratio_or_zero(step2_stats->hardware_trsv * cfg->trsv5_lat,
                         step2_stats->total_cycles));
    printf("lusgs.step2.pathaShareEstimate = %.6f\n",
           ratio_or_zero(step2_total_mvm * 5U * 7U,
                         step2_stats->total_cycles));
    printf("lusgs.pathaStoreMode = %s\n", cfg->patha_store_mode);
    printf("lusgs.pathaStoreStride = %lu\n",
           (unsigned long)cfg->patha_store_stride);
    printf("lusgs.pathaUnroll = %lu\n", (unsigned long)cfg->patha_unroll);
    printf("lusgs.pathaAccMode = %s\n", cfg->patha_acc_mode);
    printf("lusgs.pathaResultBufferDepth = %lu\n",
           (unsigned long)cfg->patha_buffer_depth);
    printf("lusgs.pathaStreaming = %d\n", cfg->patha_streaming);
    printf("lusgs.pathaKernel = %s\n", cfg->patha_kernel);
    printf("lusgs.trsv5Count = %lu\n", (unsigned long)cfg->trsv5_count);
    printf("lusgs.trsv5Lat = %lu\n", (unsigned long)cfg->trsv5_lat);
    printf("lusgs.trsv5Mode = %s\n", cfg->trsv5_mode);
    printf("lusgs.pathaDefaultConfigMatch = %d\n", config_match);
    printf("lusgs.countsMatch = %d\n", counts_ok);

    print_compare("lusgs.step1_vs_reference", step1_cmp);
    print_compare("lusgs.step2_vs_reference", step2_cmp);
    print_compare("lusgs.step2_vs_step1", step2_vs_step1_cmp);
    print_compare("lusgs.step2_core_vs_reference", step2_core_cmp);
    print_compare("lusgs.step2_core_vs_step2", step2_core_vs_step2_cmp);
    print_compare("lusgs.step2_core_forwarded_vs_reference",
                  step2_core_forwarded_cmp);
    print_compare("lusgs.step2_core_forwarded_vs_step2_core",
                  step2_core_forwarded_vs_step2_core_cmp);
    print_compare("lusgs.step2_core_forwarded_context_vs_reference",
                  step2_core_forwarded_context_cmp);
    print_compare("lusgs.step2_core_forwarded_context_vs_step2_core_forwarded",
                  step2_core_forwarded_context_vs_forwarded_cmp);
    print_compare("lusgs.step2_core_forwarded_context_vs_step2_core",
                  step2_core_forwarded_context_vs_core_cmp);
    print_compare("lusgs.step2_core_forwarded_linebuf_vs_reference",
                  step2_core_forwarded_linebuf_cmp);
    print_compare("lusgs.step2_core_forwarded_linebuf_vs_step2_core_forwarded_context",
                  step2_core_forwarded_linebuf_vs_context_cmp);
    print_compare("lusgs.step2_core_forwarded_linebuf_vs_step2_core_forwarded",
                  step2_core_forwarded_linebuf_vs_forwarded_cmp);
    print_compare("lusgs.step2_core_forwarded_linebuf_vs_step2_core",
                  step2_core_forwarded_linebuf_vs_core_cmp);

    const CompareResult *final_cmp =
        cfg->run_step2_core_forwarded_linebuf ?
            step2_core_forwarded_linebuf_cmp :
        (cfg->run_step2_core_forwarded_context ?
            step2_core_forwarded_context_cmp :
        (cfg->run_step2_core_forwarded ? step2_core_forwarded_cmp :
        (cfg->run_step2_core ? step2_core_cmp : step2_cmp)));
    printf("max_abs_error = %.17g\n", final_cmp->max_abs_error);
    printf("max_rel_error = %.17g\n", final_cmp->max_rel_error);
    printf("mismatch_count = %lu\n",
           (unsigned long)final_cmp->mismatch_count);
}

static void
print_matrix_compare(const char *prefix, const MatrixCompareResult *cmp)
{
    printf("%s.max_abs_error = %.17g\n", prefix, cmp->max_abs_error);
    printf("%s.max_rel_error = %.17g\n", prefix, cmp->max_rel_error);
    printf("%s.max_residual = %.17g\n", prefix, cmp->max_residual);
    printf("%s.mismatch_count = %lu\n", prefix,
           (unsigned long)cmp->mismatch_count);
}

static double
break_even_sweeps(const Config *cfg, const Stats *baseline,
                  const Stats *pretransform)
{
    if (!cfg->sweeps || !baseline->total_cycles)
        return 0.0;
    double baseline_per_sweep =
        (double)baseline->total_cycles / (double)cfg->sweeps;
    double hot_per_sweep =
        (double)pretransform->runtime_sweep_cycles / (double)cfg->sweeps;
    double saving = baseline_per_sweep - hot_per_sweep;
    if (saving <= 0.0)
        return 0.0;
    return ceil((double)pretransform->preprocess_cycles / saving);
}

static void
print_pretransform_mode(const char *prefix, const Config *cfg,
                        const Stats *stats, const PretransformCheck *check,
                        const Stats *forwarded, const Stats *context)
{
    char matrix_prefix[160];
    uint64_t mvm_count = stats->pretransform_dinv_mvm +
        stats->pretransform_lbar_mvm + stats->pretransform_ubar_mvm;
    uint64_t full_cells =
        (uint64_t)cfg->lines * (uint64_t)cfg->cells * cfg->sweeps;
    printf("%s.preprocessCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_cycles);
    printf("%s.preprocessTotalCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_cycles);
    printf("%s.runtimeCycles = %lu\n", prefix,
           (unsigned long)stats->runtime_sweep_cycles);
    printf("%s.totalCyclesIncludingPreprocess = %lu\n", prefix,
           (unsigned long)stats->total_cycles_including_preprocess);
    printf("%s.amortizedCyclesPerSweep = %.6f\n", prefix,
           ratio_or_zero(stats->total_cycles_including_preprocess,
                         cfg->sweeps));
    printf("%s.cyclesPerFullCell = %.6f\n", prefix,
           ratio_or_zero(stats->runtime_sweep_cycles, full_cells));
    printf("%s.cyclesPerMvm = %.6f\n", prefix,
           ratio_or_zero(stats->runtime_sweep_cycles, mvm_count));
    printf("%s.trsm5MrhsIssued = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_issued);
    printf("%s.trsm5MrhsCompleted = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_completed);
    printf("%s.trsm5MrhsInvalid = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_invalid);
    printf("%s.trsm5MrhsBusyCycles = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_busy_cycles);
    printf("%s.trsm5MrhsStallCycles = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_stall_cycles);
    printf("%s.trsm5MrhsInvBatches = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_inv_batches);
    printf("%s.trsm5MrhsLbarBatches = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_lbar_batches);
    printf("%s.trsm5MrhsUbarBatches = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_ubar_batches);
    printf("%s.trsm5MrhsColumnsSolved = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_columns_solved);
    printf("%s.trsm5MrhsInputLuBytes = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_input_lu_bytes);
    printf("%s.trsm5MrhsInputRhsBytes = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_input_rhs_bytes);
    printf("%s.trsm5MrhsOutputBytes = %lu\n", prefix,
           (unsigned long)stats->trsm5_mrhs_output_bytes);
    printf("%s.preprocessCells = %lu\n", prefix,
           (unsigned long)stats->preprocess_cells);
    printf("%s.preprocessMatricesGenerated = %lu\n", prefix,
           (unsigned long)stats->preprocess_matrices_generated);
    printf("%s.uBarReusedExisting = %lu\n", prefix,
           (unsigned long)stats->ubar_reused_existing);
    printf("%s.pretransformValidCells = %lu\n", prefix,
           (unsigned long)stats->pretransform_valid_cells);
    printf("%s.pretransformInvalidCells = %lu\n", prefix,
           (unsigned long)stats->pretransform_invalid_cells);
    printf("%s.pretransformFallbackRuns = %lu\n", prefix,
           (unsigned long)stats->pretransform_fallback_runs);
    printf("%s.pretransformNaNInf = %lu\n", prefix,
           (unsigned long)stats->pretransform_nan_inf);
    printf("%s.pretransformDinvMvm = %lu\n", prefix,
           (unsigned long)stats->pretransform_dinv_mvm);
    printf("%s.pretransformLbarMvm = %lu\n", prefix,
           (unsigned long)stats->pretransform_lbar_mvm);
    printf("%s.pretransformUbarMvm = %lu\n", prefix,
           (unsigned long)stats->pretransform_ubar_mvm);
    printf("%s.pretransformVectorSub = %lu\n", prefix,
           (unsigned long)stats->pretransform_vector_sub);
    printf("%s.pretransformHotLoopTrsv5Elided = %lu\n", prefix,
           (unsigned long)stats->pretransform_hot_loop_trsv5_elided);
    printf("%s.pretransformCoefficientBytesRead = %lu\n", prefix,
           (unsigned long)stats->pretransform_coefficient_bytes_read);
    printf("%s.pretransformTmpStores = %lu\n", prefix,
           (unsigned long)stats->pretransform_tmp_stores);
    printf("%s.pretransformTmpLoads = %lu\n", prefix,
           (unsigned long)stats->pretransform_tmp_loads);
    printf("%s.pretransformPathAPingIssued = %lu\n", prefix,
           (unsigned long)stats->pretransform_patha_ping_issued);
    printf("%s.pretransformPathAPongIssued = %lu\n", prefix,
           (unsigned long)stats->pretransform_patha_pong_issued);
    printf("%s.pretransformPathASlotStalls = %lu\n", prefix,
           (unsigned long)stats->pretransform_patha_slot_stalls);
    printf("%s.pretransformPathAInvalidSlots = %lu\n", prefix,
           (unsigned long)stats->pretransform_patha_invalid_slots);
    printf("%s.hotSpeedupVsStep2CoreForwarded = %.6f\n", prefix,
           ratio_or_zero(forwarded->total_cycles,
                         stats->runtime_sweep_cycles));
    printf("%s.totalSpeedupVsStep2CoreForwarded = %.6f\n", prefix,
           ratio_or_zero(forwarded->total_cycles,
                         stats->total_cycles_including_preprocess));
    printf("%s.hotSpeedupVsStep2CoreForwardedContext = %.6f\n", prefix,
           ratio_or_zero(context->total_cycles,
                         stats->runtime_sweep_cycles));
    printf("%s.totalSpeedupVsStep2CoreForwardedContext = %.6f\n", prefix,
           ratio_or_zero(context->total_cycles,
                         stats->total_cycles_including_preprocess));
    printf("%s.breakEvenSweepsVsStep2CoreForwarded = %.0f\n", prefix,
           break_even_sweeps(cfg, forwarded, stats));
    printf("%s.breakEvenSweepsVsStep2CoreForwardedContext = %.0f\n", prefix,
           break_even_sweeps(cfg, context, stats));
    printf("%s.preprocessStageCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_stage_cycles);
    printf("%s.preprocessComputeCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_compute_cycles);
    printf("%s.preprocessDrainCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_drain_cycles);
    printf("%s.preprocessValidationCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_validation_cycles);
    printf("%s.preprocessResidualCheckCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_residual_check_cycles);
    printf("%s.preprocessFastCheckCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_fast_check_cycles);
    printf("%s.preprocessBarrierCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_barrier_cycles);
    printf("%s.preprocessIdleCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_idle_cycles);
    printf("%s.preprocessOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_overlap_cycles);
    printf("%s.preprocessStageComputeOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_stage_compute_overlap_cycles);
    printf("%s.preprocessComputeDrainOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->preprocess_compute_drain_overlap_cycles);
    printf("%s.preprocessOverlapEfficiency = %.9f\n", prefix,
           stats->preprocess_cycles ?
           (double)stats->preprocess_overlap_cycles /
               (double)stats->preprocess_cycles : 0.0);
    printf("%s.preprocessLuStageBytes = %lu\n", prefix,
           (unsigned long)stats->preprocess_lu_stage_bytes);
    printf("%s.preprocessCStageBytes = %lu\n", prefix,
           (unsigned long)stats->preprocess_c_stage_bytes);
    printf("%s.preprocessIdentityStageBytes = %lu\n", prefix,
           (unsigned long)stats->preprocess_identity_stage_bytes);
    printf("%s.preprocessOutputSpmBytes = %lu\n", prefix,
           (unsigned long)stats->preprocess_output_spm_bytes);
    printf("%s.preprocessOutputHeapBytes = %lu\n", prefix,
           (unsigned long)stats->preprocess_output_heap_bytes);
    printf("%s.preprocessLuStages = %lu\n", prefix,
           (unsigned long)stats->preprocess_lu_stages);
    printf("%s.preprocessIdentityStages = %lu\n", prefix,
           (unsigned long)stats->preprocess_identity_stages);
    printf("%s.preprocessCStages = %lu\n", prefix,
           (unsigned long)stats->preprocess_c_stages);
    printf("%s.trsm5InvLbarIssued = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_issued);
    printf("%s.trsm5InvLbarCompleted = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_completed);
    printf("%s.trsm5InvLbarBusyCycles = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_busy_cycles);
    printf("%s.trsm5InvLbarStallCycles = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_stall_cycles);
    printf("%s.trsm5InvLbarLuReads = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_lu_reads);
    printf("%s.trsm5InvLbarColumnsSolved = %lu\n", prefix,
           (unsigned long)stats->trsm5_inv_lbar_columns_solved);
    printf("%s.pretransformOutputBufferAlloc = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_buffer_alloc);
    printf("%s.pretransformOutputBufferFree = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_buffer_free);
    printf("%s.pretransformOutputBufferFullStalls = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_buffer_full_stalls);
    printf("%s.pretransformOutputBufferMaxOccupancy = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_buffer_max_occupancy);
    printf("%s.pretransformOutputDrainCycles = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_drain_cycles);
    printf("%s.pretransformBarriersIssued = %lu\n", prefix,
           (unsigned long)stats->pretransform_barriers_issued);
    printf("%s.pretransformBarriersElided = %lu\n", prefix,
           (unsigned long)stats->pretransform_barriers_elided);
    printf("%s.pretransformInputTokenStalls = %lu\n", prefix,
           (unsigned long)stats->pretransform_input_token_stalls);
    printf("%s.pretransformOutputTokenStalls = %lu\n", prefix,
           (unsigned long)stats->pretransform_output_token_stalls);
    printf("%s.pretransformAutoSelected = %lu\n", prefix,
           (unsigned long)stats->pretransform_auto_selected);
    printf("%s.trsv5AutoSelected = %lu\n", prefix,
           (unsigned long)stats->trsv5_auto_selected);
    printf("%s.estimatedBreakEvenSweeps = %lu\n", prefix,
           (unsigned long)stats->estimated_break_even_sweeps);
    printf("%s.expectedSweeps = %lu\n", prefix,
           (unsigned long)stats->expected_sweeps);
    printf("%s.preprocessValidationMode = %s\n", prefix,
           cfg->pretransform_validate);
    snprintf(matrix_prefix, sizeof(matrix_prefix), "%s.d_inv", prefix);
    print_matrix_compare(matrix_prefix, &check->d_inv);
    snprintf(matrix_prefix, sizeof(matrix_prefix), "%s.l_bar", prefix);
    print_matrix_compare(matrix_prefix, &check->l_bar);
    snprintf(matrix_prefix, sizeof(matrix_prefix), "%s.u_bar", prefix);
    print_matrix_compare(matrix_prefix, &check->u_bar);
}

static void
print_raw_mode(const char *prefix, const Config *cfg, const Stats *stats,
               const CompareResult *cmp, const PretransformCheck *check)
{
    printf("%s.inputSemantics = raw-D-L-U-R\n", prefix);
    printf("%s.rawInputDBytes = %lu\n", prefix,
           (unsigned long)stats->raw_input_d_bytes);
    printf("%s.rawInputLBytes = %lu\n", prefix,
           (unsigned long)stats->raw_input_l_bytes);
    printf("%s.rawInputUBytes = %lu\n", prefix,
           (unsigned long)stats->raw_input_u_bytes);
    printf("%s.rawInputRBytes = %lu\n", prefix,
           (unsigned long)stats->raw_input_r_bytes);
    printf("%s.lu5FactorCells = %lu\n", prefix,
           (unsigned long)stats->lu5_factor_cells);
    printf("%s.lu5FactorCycles = %lu\n", prefix,
           (unsigned long)stats->lu5_factor_cycles);
    printf("%s.lu5FactorFailures = %lu\n", prefix,
           (unsigned long)stats->lu5_factor_failures);
    printf("%s.lu5FactorInputBytes = %lu\n", prefix,
           (unsigned long)stats->lu5_factor_input_bytes);
    printf("%s.lu5FactorOutputBytes = %lu\n", prefix,
           (unsigned long)stats->lu5_factor_output_bytes);
    printf("%s.maxAbsLuResidual = %.17g\n", prefix,
           stats->max_abs_lu_residual);
    printf("%s.uBarGenerated = %lu\n", prefix,
           (unsigned long)stats->ubar_generated);
    printf("%s.uBarTrsmIssued = %lu\n", prefix,
           (unsigned long)stats->ubar_trsm_issued);
    printf("%s.uBarColumnsSolved = %lu\n", prefix,
           (unsigned long)stats->ubar_columns_solved);
    printf("%s.uBarCycles = %lu\n", prefix,
           (unsigned long)stats->ubar_cycles);
    printf("%s.uBarStageBytes = %lu\n", prefix,
           (unsigned long)stats->ubar_stage_bytes);
    printf("%s.uBarOutputBytes = %lu\n", prefix,
           (unsigned long)stats->ubar_output_bytes);
    printf("%s.uBarResidualMax = %.17g\n", prefix,
           stats->max_abs_ubar_residual);
    printf("%s.uBarRelativeResidualMax = %.17g\n", prefix,
           stats->max_rel_ubar_residual);
    printf("%s.uBarReusedExisting = %lu\n", prefix,
           (unsigned long)stats->ubar_reused_existing);
    printf("%s.dInvGenerated = %lu\n", prefix,
           (unsigned long)stats->dinv_generated);
    printf("%s.baseDirectRhsSolved = %lu\n", prefix,
           (unsigned long)stats->base_direct_rhs_solved);
    printf("%s.baseLuCacheHits = %lu\n", prefix,
           (unsigned long)stats->base_lu_cache_hits);
    printf("%s.lBarGenerated = %lu\n", prefix,
           (unsigned long)stats->lbar_generated);
    printf("%s.coeff3Issued = %lu\n", prefix,
           (unsigned long)stats->coeff3_issued);
    printf("%s.coeff3ColumnsSolved = %lu\n", prefix,
           (unsigned long)stats->coeff3_columns_solved);
    printf("%s.coeff3Cycles = %lu\n", prefix,
           (unsigned long)stats->coeff3_cycles);
    printf("%s.commonLuCycles = %lu\n", prefix,
           (unsigned long)stats->common_lu_cycles);
    printf("%s.commonUbarCycles = %lu\n", prefix,
           (unsigned long)stats->common_ubar_cycles);
    printf("%s.commonPreprocessCycles = %lu\n", prefix,
           (unsigned long)stats->common_preprocess_cycles);
    printf("%s.extraPretransformCycles = %lu\n", prefix,
           (unsigned long)stats->extra_pretransform_cycles);
    printf("%s.fairExtraPretransformCycles = %lu\n", prefix,
           (unsigned long)stats->fair_extra_pretransform_cycles);
    printf("%s.measuredPreprocessCycles = %lu\n", prefix,
           (unsigned long)(stats->common_preprocess_cycles +
                           stats->extra_pretransform_cycles));
    printf("%s.forwardCycles = %lu\n", prefix,
           (unsigned long)stats->forward_cycles);
    printf("%s.backwardCycles = %lu\n", prefix,
           (unsigned long)stats->backward_cycles);
    printf("%s.pathaMvmForward = %lu\n", prefix,
           (unsigned long)stats->patha_mvm_forward);
    printf("%s.pathaMvmBackward = %lu\n", prefix,
           (unsigned long)stats->patha_mvm_backward);
    printf("%s.pathaMvmTotal = %lu\n", prefix,
           (unsigned long)(stats->patha_mvm_forward +
                           stats->patha_mvm_backward));
    printf("%s.vectorSubOperations = %lu\n", prefix,
           (unsigned long)stats->vector_sub_ops);
    printf("%s.hardwareVectorSub = %lu\n", prefix,
           (unsigned long)stats->hardware_vector_sub);
    printf("%s.hardwareTrsv = %lu\n", prefix,
           (unsigned long)stats->hardware_trsv);
    printf("%s.trsv5RawRuntimeCycles = %lu\n", prefix,
           (unsigned long)stats->trsv5_raw_runtime_cycles);
    printf("%s.pretransformRawRuntimeCycles = %lu\n", prefix,
           (unsigned long)stats->pretransform_raw_runtime_cycles);
    printf("%s.trsv5RawTotalCycles = %lu\n", prefix,
           (unsigned long)stats->trsv5_raw_total_cycles);
    printf("%s.pretransformRawTotalCycles = %lu\n", prefix,
           (unsigned long)stats->pretransform_raw_total_cycles);
    printf("%s.fairBreakEvenSweeps = %lu\n", prefix,
           (unsigned long)stats->fair_break_even_sweeps);
    printf("%s.coefficientUpdateInterval = %lu\n", prefix,
           (unsigned long)stats->coefficient_update_interval);
    printf("%s.coefficientRebuilds = %lu\n", prefix,
           (unsigned long)stats->coefficient_rebuilds);
    printf("%s.coefficientReuses = %lu\n", prefix,
           (unsigned long)stats->coefficient_reuses);
    printf("%s.commonPreprocessFallbackRuns = %lu\n", prefix,
           (unsigned long)stats->common_preprocess_fallback_runs);
    printf("%s.pretransformFallbackToTrsv5Raw = %lu\n", prefix,
           (unsigned long)stats->pretransform_fallback_to_trsv5_raw);
    printf("%s.uBarInvalid = %lu\n", prefix,
           (unsigned long)stats->ubar_invalid);
    printf("%s.coeffOutputAlloc = %lu\n", prefix,
           (unsigned long)stats->coeff_output_alloc);
    printf("%s.coeffOutputFree = %lu\n", prefix,
           (unsigned long)stats->coeff_output_free);
    printf("%s.coeffOutputFullStalls = %lu\n", prefix,
           (unsigned long)stats->coeff_output_full_stalls);
    printf("%s.coeffOutputMaxOccupancy = %lu\n", prefix,
           (unsigned long)stats->coeff_output_max_occupancy);
    printf("%s.coeffOutputDrainCycles = %lu\n", prefix,
           (unsigned long)stats->coeff_output_drain_cycles);
    printf("%s.persistentCoefficientBytes = %lu\n", prefix,
           (unsigned long)stats->raw_persistent_coefficient_bytes);
    printf("%s.extraCoefficientBytes = %lu\n", prefix,
           (unsigned long)stats->raw_extra_coefficient_bytes);
    printf("%s.coeffPreprocessModel = %s\n", prefix,
           cfg->coeff_preprocess_model);
    printf("%s.lu5Model = %s\n", prefix, cfg->lu5_model);
    printf("%s.coeffValidationMode = %s\n", prefix,
           cfg->coeff_validation);
    if (stats->streaming_enabled) {
        printf("%s.streamingStep2.enabled = 1\n", prefix);
        printf("%s.streamingStep2.stageAImplemented = 1\n", prefix);
        printf("%s.streamingStep2.stageB1IndependentReaperImplemented = 1\n",
               prefix);
        printf("%s.streamingStep2.stageB15AutoRetireImplemented = 1\n",
               prefix);
        printf("%s.streamingStep2.stageB25FixedMaskImplemented = 1\n",
               prefix);
        printf("%s.streamingStep2.stageB3DinvConsumerImplemented = 1\n",
               prefix);
        printf("%s.streamingStep2.stageB4LbarConsumerImplemented = 1\n",
               prefix);
        printf("%s.streamingStep2.stageCFrontierSchedulerImplemented = 0\n",
               prefix);
        printf("%s.streamingStep2.windowDepth = %lu\n", prefix,
               (unsigned long)stats->streaming_window_depth);
        printf("%s.streamingStep2.firstInputIssueCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_input_issue_cycle);
        printf("%s.streamingStep2.firstLuIssueCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_lu_issue_cycle);
        printf("%s.streamingStep2.firstDinvColumnReadyCycle = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_first_dinv_column_ready_cycle);
        printf("%s.streamingStep2.firstLbarColumnReadyCycle = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_first_lbar_column_ready_cycle);
        printf("%s.streamingStep2.firstUbarColumnReadyCycle = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_first_ubar_column_ready_cycle);
        printf("%s.streamingStep2.firstDinvReadyCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_dinv_ready_cycle);
        printf("%s.streamingStep2.firstLbarReadyCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_lbar_ready_cycle);
        printf("%s.streamingStep2.firstUbarReadyCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_ubar_ready_cycle);
        printf("%s.streamingStep2.firstForwardIssueCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_first_forward_issue_cycle);
        printf("%s.streamingStep2.preprocessFinishCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_preprocess_finish_cycle);
        printf("%s.streamingStep2.forwardFinishCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_finish_cycle);
        printf("%s.streamingStep2.backwardStartCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_backward_start_cycle);
        printf("%s.streamingStep2.backwardFinishCycle = %lu\n", prefix,
               (unsigned long)stats->streaming_backward_finish_cycle);
        printf("%s.streamingStep2.forwardStartedBeforePreprocessDone = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_forward_started_before_preprocess_done);
        printf("%s.streamingStep2.preprocessForwardOverlapCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_preprocess_forward_overlap_cycles);
        printf("%s.streamingStep2.preprocessBackwardPrepareOverlapCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_preprocess_backward_prepare_overlap_cycles);
        printf("%s.streamingStep2.forwardBackwardTurnaroundStallCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_forward_backward_turnaround_stall_cycles);
        printf("%s.streamingStep2.timelineInconsistencies = %lu\n", prefix,
               (unsigned long)stats->streaming_timeline_inconsistencies);
        printf("%s.streamingStep2.forwardWaitDinvCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_wait_dinv_cycles);
        printf("%s.streamingStep2.forwardWaitLbarCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_wait_lbar_cycles);
        printf("%s.streamingStep2.forwardWaitPrevVectorCycles = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_forward_wait_prev_vector_cycles);
        printf("%s.streamingStep2.backwardWaitUbarCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_backward_wait_ubar_cycles);
        printf("%s.streamingStep2.backwardWaitNextVectorCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_backward_wait_next_vector_cycles);
        printf("%s.streamingStep2.requestWindowAverageOccupancy = %.6f\n",
               prefix, ratio_or_zero(
                   stats->streaming_request_window_occupancy_sum,
                   stats->streaming_request_window_samples));
        printf("%s.streamingStep2.requestWindowMaxOccupancy = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_request_window_max_occupancy);
        printf("%s.streamingStep2.requestWindowFullStallCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_request_window_full_stall_cycles);
        printf("%s.streamingStep2.outputRingAverageOccupancy = %.6f\n",
               prefix, ratio_or_zero(
                   stats->streaming_output_ring_occupancy_sum,
                   stats->streaming_output_ring_samples));
        printf("%s.streamingStep2.forwardCellsConsumed = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_cells_consumed);
        printf("%s.streamingStep2.requestSlotsReused = %lu\n", prefix,
               (unsigned long)stats->streaming_request_slots_reused);
        printf("%s.streamingStep2.staleGenerationRejected = %lu\n", prefix,
               (unsigned long)stats->streaming_stale_generation_rejected);
        printf("%s.streamingStep2.dinvColumnsBypassed = %lu\n", prefix,
               (unsigned long)stats->streaming_dinv_columns_bypassed);
        printf("%s.streamingStep2.lbarColumnsBypassed = %lu\n", prefix,
               (unsigned long)stats->streaming_lbar_columns_bypassed);
        printf("%s.streamingStep2.ubarColumnsBypassed = %lu\n", prefix,
               (unsigned long)stats->streaming_ubar_columns_bypassed);
        printf("%s.streamingStep2.coefficientDrainBytesAvoided = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_coefficient_drain_bytes_avoided);
        printf("%s.streamingStep2.basePrefetchIssued = %lu\n", prefix,
               (unsigned long)stats->streaming_base_prefetch_issued);
        printf("%s.streamingStep2.basePrefetchHits = %lu\n", prefix,
               (unsigned long)stats->streaming_base_prefetch_hits);
        printf("%s.streamingStep2.basePrefetchMisses = %lu\n", prefix,
               (unsigned long)stats->streaming_base_prefetch_misses);
        printf("%s.streamingStep2.pathaIdleWithReadyWorkCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_patha_idle_with_ready_work_cycles);
        printf("%s.streamingStep2.trsmIdleWithReadyRhsCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_trsm_idle_with_ready_rhs_cycles);
        printf("%s.streamingStep2.dividerIdleWithReadyRhsCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_divider_idle_with_ready_rhs_cycles);
        printf("%s.streamingStep2.mulSubIdleWithReadyWorkCycles = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_mulsub_idle_with_ready_work_cycles);
        printf("%s.streamingStep2.rhsQueueAverageOccupancy = %.6f\n",
               prefix, ratio_or_zero(
                   stats->streaming_rhs_queue_occupancy_sum,
                   stats->streaming_rhs_queue_samples));
        printf("%s.streamingStep2.drainQueueAverageOccupancy = %.6f\n",
               prefix, ratio_or_zero(
                   stats->streaming_drain_queue_occupancy_sum,
                   stats->streaming_drain_queue_samples));
        printf("%s.streamingStep2.boundarySkippedDinvRhs = %lu\n", prefix,
               (unsigned long)stats->streaming_boundary_skipped_dinv_rhs);
        printf("%s.streamingStep2.boundarySkippedLbarRhs = %lu\n", prefix,
               (unsigned long)stats->streaming_boundary_skipped_lbar_rhs);
        printf("%s.streamingStep2.boundarySkippedUbarRhs = %lu\n", prefix,
               (unsigned long)stats->streaming_boundary_skipped_ubar_rhs);
        printf("%s.streamingStep2.boundarySkippedRhsTotal = %lu\n", prefix,
               (unsigned long)(stats->streaming_boundary_skipped_dinv_rhs +
                   stats->streaming_boundary_skipped_lbar_rhs +
                   stats->streaming_boundary_skipped_ubar_rhs));
        printf("%s.streamingStep2.requestedDinvBatches = %lu\n", prefix,
               (unsigned long)stats->streaming_requested_dinv_batches);
        printf("%s.streamingStep2.requestedLbarBatches = %lu\n", prefix,
               (unsigned long)stats->streaming_requested_lbar_batches);
        printf("%s.streamingStep2.requestedUbarBatches = %lu\n", prefix,
               (unsigned long)stats->streaming_requested_ubar_batches);
        printf("%s.streamingStep2.avoidedLbarDrains = %lu\n", prefix,
               (unsigned long)stats->streaming_avoided_lbar_drains);
        printf("%s.streamingStep2.avoidedUbarDrains = %lu\n", prefix,
               (unsigned long)stats->streaming_avoided_ubar_drains);
        printf("%s.streamingStep2.effectiveRhsCount = %lu\n", prefix,
               (unsigned long)stats->streaming_effective_rhs_count);
        printf("%s.streamingStep2.completionReaperPolls = %lu\n", prefix,
               (unsigned long)stats->streaming_completion_reaper_polls);
        printf("%s.streamingStep2.completionReaperTerminalHits = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_completion_reaper_terminal_hits);
        printf("%s.streamingStep2.completionReaperBusyPolls = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_completion_reaper_busy_polls);
        printf("%s.streamingStep2.completionReaperQueueMaxOccupancy = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_completion_reaper_queue_max_occupancy);
        printf("%s.streamingStep2.completionReaperQueueFullStalls = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_completion_reaper_queue_full_stalls);
        printf("%s.streamingStep2.completionReaperOutOfOrderReaps = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_completion_reaper_out_of_order_reaps);
        printf("%s.streamingStep2.completionReaperCancels = %lu\n", prefix,
               (unsigned long)stats->streaming_completion_reaper_cancels);
        printf("%s.streamingStep2.forwardWaitTerminalCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_wait_terminal_cycles);
        printf("%s.streamingStep2.forwardWaitUbarCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_wait_ubar_cycles);
        printf("%s.streamingStep2.forwardWaitDrainCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_forward_wait_drain_cycles);
        printf("%s.streamingStep2.slotReuseAfterForwardBeforeTerminal = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_slot_reuse_after_forward_before_terminal);
        printf("%s.streamingStep2.detachedCompletionEntries = %lu\n", prefix,
               (unsigned long)
                   stats->streaming_detached_completion_entries);
        printf("%s.streamingStep2.unsafeSlotReuseAttempts = %lu\n", prefix,
               (unsigned long)stats->streaming_unsafe_slot_reuse_attempts);
        printf("%s.streamingStep2.staleGenerationReads = %lu\n", prefix,
               (unsigned long)stats->streaming_stale_generation_reads);
        printf("%s.streamingStep2.doubleColumnConsumes = %lu\n", prefix,
               (unsigned long)stats->streaming_double_column_consumes);
        printf("%s.streamingStep2.autoRetiredRequests = %lu\n", prefix,
               (unsigned long)stats->auto_retired_requests);
        printf("%s.streamingStep2.autoRetiredTokens = %lu\n", prefix,
               (unsigned long)stats->auto_retired_tokens);
        printf("%s.streamingStep2.autoRetireErrors = %lu\n", prefix,
               (unsigned long)stats->auto_retire_errors);
        printf("%s.streamingStep2.autoRetireBeforeForwardFinish = %lu\n",
               prefix, (unsigned long)
                   stats->auto_retire_before_forward_finish);
        printf("%s.streamingStep2.autoRetireBeforeBackwardStart = %lu\n",
               prefix, (unsigned long)
                   stats->auto_retire_before_backward_start);
        printf("%s.streamingStep2.terminalWaitCallsAvoided = %lu\n", prefix,
               (unsigned long)stats->terminal_wait_calls_avoided);
        printf("%s.streamingStep2.completionQueueScansAvoided = %lu\n",
               prefix, (unsigned long)
                   stats->completion_queue_scans_avoided);
        printf("%s.streamingStep2.completionQueueEntriesAvoided = %lu\n",
               prefix, (unsigned long)
                   stats->completion_queue_entries_avoided);
        printf("%s.streamingStep2.progressPollCalls = %lu\n", prefix,
               (unsigned long)stats->progress_poll_calls);
        printf("%s.streamingStep2.progressPollBusy = %lu\n", prefix,
               (unsigned long)stats->progress_poll_busy);
        printf("%s.streamingStep2.progressPollUseful = %lu\n", prefix,
               (unsigned long)stats->progress_poll_useful);
        printf("%s.streamingStep2.progressPollCycles = %lu\n", prefix,
               (unsigned long)stats->progress_poll_cycles);
        printf("%s.streamingStep2.frontierWaitDinvCycles = %lu\n", prefix,
               (unsigned long)stats->frontier_wait_dinv_cycles);
        printf("%s.streamingStep2.frontierWaitLbarCycles = %lu\n", prefix,
               (unsigned long)stats->frontier_wait_lbar_cycles);
        printf("%s.streamingStep2.frontierWaitUbarCycles = %lu\n", prefix,
               (unsigned long)stats->frontier_wait_ubar_cycles);
        printf("%s.streamingStep2.frontierNoWorkCycles = %lu\n", prefix,
               (unsigned long)stats->frontier_no_work_cycles);
        printf("%s.streamingStep2.maskTableLookups = %lu\n", prefix,
               (unsigned long)stats->mask_table_lookups);
        printf("%s.streamingStep2.rhsMaskSchedulerScans = %lu\n", prefix,
               (unsigned long)stats->rhs_mask_scheduler_scans);
        printf("%s.streamingStep2.rhsMaskSchedulerUsefulIssues = %lu\n",
               prefix, (unsigned long)
                   stats->rhs_mask_scheduler_useful_issues);
        printf("%s.streamingStep2.rhsMaskSchedulerEmptyScans = %lu\n",
               prefix, (unsigned long)
                   stats->rhs_mask_scheduler_empty_scans);
        printf("%s.streamingStep2.guestCompletionRecordChecks = %lu\n",
               prefix, (unsigned long)
                   stats->guest_completion_record_checks);
        printf("%s.streamingStep2.guestTerminalErrorChecks = %lu\n", prefix,
               (unsigned long)stats->guest_terminal_error_checks);
        printf("%s.streamingStep2.progressWaitCalls = %lu\n", prefix,
               (unsigned long)stats->progress_wait_calls);
        printf("%s.streamingStep2.terminalWaitCalls = %lu\n", prefix,
               (unsigned long)stats->terminal_wait_calls);
        printf("%s.streamingStep2.terminalWaitBusyReturns = %lu\n", prefix,
               (unsigned long)stats->terminal_wait_busy_returns);
        printf("%s.streamingStep2.staleTokenWaits = %lu\n", prefix,
               (unsigned long)stats->stale_token_waits);
        printf("%s.streamingStep2.cyclesInProgressWait = %lu\n", prefix,
               (unsigned long)stats->progress_poll_cycles);
        printf("%s.streamingStep2.cyclesInTerminalWait = %lu\n", prefix,
               (unsigned long)stats->frontier_wait_ubar_cycles);
        printf("%s.streamingStep2.cyclesInGuestBookkeeping = %lu\n", prefix,
               (unsigned long)stats->guest_bookkeeping_cycles);
        printf("%s.streamingStep2.cyclesBlockedByCompletionQueue = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_completion_reaper_queue_full_stalls);
        printf("%s.streamingStep2.cyclesBlockedByDescriptorRecord = %lu\n",
               prefix, (unsigned long)
                   stats->streaming_request_window_full_stall_cycles);
        printf("%s.streamingStep2.cyclesBlockedByUbar = %lu\n", prefix,
               (unsigned long)stats->frontier_wait_ubar_cycles);
        printf("%s.streamingStep2.cyclesBlockedByBackwardCoeff = %lu\n",
               prefix, (unsigned long)stats->frontier_wait_ubar_cycles);
        printf("%s.streamingStep2.dInvColumnConsumerEnabled = %lu\n", prefix,
               (unsigned long)stats->dinv_column_consumer_enabled);
        printf("%s.streamingStep2.dInvColumnConsumerShadowRuns = %lu\n",
               prefix, (unsigned long)
                   stats->dinv_column_consumer_shadow_runs);
        printf("%s.streamingStep2.dInvColumnConsumerDirectRuns = %lu\n",
               prefix, (unsigned long)
                   stats->dinv_column_consumer_direct_runs);
#define PRINT_B3_U64(label, field) \
        printf("%s.streamingStep2." label " = %lu\n", prefix, \
               (unsigned long)stats->field)
        PRINT_B3_U64("dInvColumnsReady", dinv_columns_ready);
        PRINT_B3_U64("dInvColumnsQueued", dinv_columns_queued);
        PRINT_B3_U64("dInvColumnsIssued", dinv_columns_issued);
        PRINT_B3_U64("dInvColumnsCompleted", dinv_columns_completed);
        PRINT_B3_U64("dInvColumnsConsumed", dinv_columns_consumed);
        PRINT_B3_U64("dInvColumnsDuplicateRejected",
                     dinv_columns_duplicate_rejected);
        PRINT_B3_U64("dInvColumnsGenerationRejected",
                     dinv_columns_generation_rejected);
        PRINT_B3_U64("dInvColumnsOutOfOrderHeld",
                     dinv_columns_out_of_order_held);
        PRINT_B3_U64("columnFmaQueueMaxOccupancy",
                     column_fma_queue_max_occupancy);
        PRINT_B3_U64("columnFmaQueueFullStalls",
                     column_fma_queue_full_stalls);
        PRINT_B3_U64("columnFmaIssueCycles", column_fma_issue_cycles);
        PRINT_B3_U64("columnFmaBusyCycles", column_fma_busy_cycles);
        PRINT_B3_U64("columnFmaCompletions", column_fma_completions);
        PRINT_B3_U64("columnFmaRetries", column_fma_retries);
        PRINT_B3_U64("columnFmaReadyButBlockedCycles",
                     column_fma_ready_but_blocked_cycles);
        printf("%s.streamingStep2.columnFmaQueueOccupancy = %.6f\n",
               prefix, stats->column_fma_queue_sample_cycles ?
               (double)stats->column_fma_queue_occupancy_sum /
                   (double)stats->column_fma_queue_sample_cycles : 0.0);
        const uint64_t column_fma_span =
            stats->last_base_ready_cycle >
                    stats->streaming_first_input_issue_cycle ?
            stats->last_base_ready_cycle -
                stats->streaming_first_input_issue_cycle : 0;
        printf("%s.streamingStep2.columnFmaUtilization = %.6f\n",
               prefix, column_fma_span && cfg->coeff_column_fma_count ?
               (double)stats->column_fma_busy_cycles /
                   ((double)column_fma_span *
                    (double)cfg->coeff_column_fma_count) : 0.0);
        PRINT_B3_U64("baseAccumulatorsAllocated",
                     base_accumulators_allocated);
        PRINT_B3_U64("baseAccumulatorsCompleted",
                     base_accumulators_completed);
        PRINT_B3_U64("baseAccumulatorsCancelled",
                     base_accumulators_cancelled);
        PRINT_B3_U64("baseAccumulatorMissingColumns",
                     base_accumulator_missing_columns);
        PRINT_B3_U64("baseAccumulatorDoubleConsumes",
                     base_accumulator_double_consumes);
        PRINT_B3_U64("baseAccumulatorStaleWrites",
                     base_accumulator_stale_writes);
        PRINT_B3_U64("firstBaseReadyCycle", first_base_ready_cycle);
        PRINT_B3_U64("lastBaseReadyCycle", last_base_ready_cycle);
        PRINT_B3_U64("baseVectorPublishCount", base_vector_publish_count);
        PRINT_B3_U64("baseVectorPublishBytes", base_vector_publish_bytes);
        PRINT_B3_U64("baseVectorPublishStalls", base_vector_publish_stalls);
        PRINT_B3_U64("dInvMatrixDrainsAvoided",
                     dinv_matrix_drains_avoided);
        PRINT_B3_U64("dInvMatrixDrainBytesAvoided",
                     dinv_matrix_drain_bytes_avoided);
        PRINT_B3_U64("dInvGuestReloadBytesAvoided",
                     dinv_guest_reload_bytes_avoided);
        PRINT_B3_U64("pathaDinvMvmEliminated",
                     patha_dinv_mvm_eliminated);
        PRINT_B3_U64("pathaDinvCustomInstructionsEliminated",
                     patha_dinv_custom_instructions_eliminated);
        PRINT_B3_U64("frontierWaitBaseCycles", frontier_wait_base_cycles);
        PRINT_B3_U64("baseReadyBeforeFrontier", base_ready_before_frontier);
        PRINT_B3_U64("baseReadyAfterFrontier", base_ready_after_frontier);
        PRINT_B3_U64("shadowBaseBitwiseMismatch",
                     shadow_base_bitwise_mismatch);
        PRINT_B3_U64("shadowBaseMismatchCell", shadow_base_mismatch_cell);
        PRINT_B3_U64("shadowBaseMismatchLane", shadow_base_mismatch_lane);
        PRINT_B3_U64("lbarColumnConsumerEnabled",
                     lbar_column_consumer_enabled);
        PRINT_B3_U64("lbarColumnConsumerShadowRuns",
                     lbar_column_consumer_shadow_runs);
        PRINT_B3_U64("lbarColumnConsumerDirectRuns",
                     lbar_column_consumer_direct_runs);
        PRINT_B3_U64("lbarColumnsReady", lbar_columns_ready);
        PRINT_B3_U64("lbarColumnsQueued", lbar_columns_queued);
        PRINT_B3_U64("lbarColumnsIssued", lbar_columns_issued);
        PRINT_B3_U64("lbarColumnsCompleted", lbar_columns_completed);
        PRINT_B3_U64("lbarColumnsConsumed", lbar_columns_consumed);
        PRINT_B3_U64("lbarColumnsDuplicateRejected",
                     lbar_columns_duplicate_rejected);
        PRINT_B3_U64("lbarColumnsGenerationRejected",
                     lbar_columns_generation_rejected);
        PRINT_B3_U64("lbarColumnsOutOfOrderHeld",
                     lbar_columns_out_of_order_held);
        PRINT_B3_U64("correctionAccumulatorsAllocated",
                     correction_accumulators_allocated);
        PRINT_B3_U64("correctionAccumulatorsCompleted",
                     correction_accumulators_completed);
        PRINT_B3_U64("correctionAccumulatorsCancelled",
                     correction_accumulators_cancelled);
        PRINT_B3_U64("correctionAccumulatorMissingColumns",
                     correction_accumulator_missing_columns);
        PRINT_B3_U64("correctionAccumulatorDoubleConsumes",
                     correction_accumulator_double_consumes);
        PRINT_B3_U64("correctionAccumulatorStaleWrites",
                     correction_accumulator_stale_writes);
        PRINT_B3_U64("forwardHandoffsProduced", forward_handoffs_produced);
        PRINT_B3_U64("forwardHandoffsConsumed", forward_handoffs_consumed);
        PRINT_B3_U64("forwardHandoffWaitCycles",
                     forward_handoff_wait_cycles);
        PRINT_B3_U64("forwardHandoffMaxLive", forward_handoff_max_live);
        PRINT_B3_U64("forwardHandoffOverwriteErrors",
                     forward_handoff_overwrite_errors);
        PRINT_B3_U64("forwardHandoffWrongCellErrors",
                     forward_handoff_wrong_cell_errors);
        PRINT_B3_U64("forwardHandoffWrongGenerationErrors",
                     forward_handoff_wrong_generation_errors);
        PRINT_B3_U64("forwardHandoffDoubleConsumes",
                     forward_handoff_double_consumes);
        PRINT_B3_U64("forwardHandoffMissingConsumes",
                     forward_handoff_missing_consumes);
        PRINT_B3_U64("forwardCombineQueued", forward_combine_queued);
        PRINT_B3_U64("forwardCombineIssued", forward_combine_issued);
        PRINT_B3_U64("forwardCombineCompleted", forward_combine_completed);
        PRINT_B3_U64("forwardCombineQueueMaxOccupancy",
                     forward_combine_queue_max_occupancy);
        PRINT_B3_U64("forwardCombineQueueFullStalls",
                     forward_combine_queue_full_stalls);
        PRINT_B3_U64("forwardCombineBusyCycles",
                     forward_combine_busy_cycles);
        PRINT_B3_U64("forwardCombineReadyButBlockedCycles",
                     forward_combine_ready_but_blocked_cycles);
        printf("%s.streamingStep2.forwardCombineUtilization = %.6f\n",
               prefix, stats->streaming_total_cycles &&
               cfg->forward_combine_count ?
               (double)stats->forward_combine_busy_cycles /
                   ((double)stats->streaming_total_cycles *
                    (double)cfg->forward_combine_count) : 0.0);
        PRINT_B3_U64("dqStarPublishCount", dqstar_publish_count);
        PRINT_B3_U64("dqStarPublishBytes", dqstar_publish_bytes);
        PRINT_B3_U64("dqStarPublishStalls", dqstar_publish_stalls);
        PRINT_B3_U64("baseStandalonePublishesAvoided",
                     base_standalone_publishes_avoided);
        PRINT_B3_U64("baseStandalonePublishBytesAvoided",
                     base_standalone_publish_bytes_avoided);
        PRINT_B3_U64("lbarMatrixDrainsAvoided",
                     lbar_matrix_drains_avoided);
        PRINT_B3_U64("lbarMatrixDrainBytesAvoided",
                     lbar_matrix_drain_bytes_avoided);
        PRINT_B3_U64("lbarGuestReloadBytesAvoided",
                     lbar_guest_reload_bytes_avoided);
        PRINT_B3_U64("pathaLbarMvmEliminated",
                     patha_lbar_mvm_eliminated);
        PRINT_B3_U64("pathaLbarCustomInstructionsEliminated",
                     patha_lbar_custom_instructions_eliminated);
        PRINT_B3_U64("lbarConsumerReadyButBlockedCycles",
                     lbar_consumer_ready_but_blocked_cycles);
        PRINT_B3_U64("lbarConsumerWaitPreviousDqCycles",
                     lbar_consumer_wait_previous_dq_cycles);
        PRINT_B3_U64("lbarConsumerWaitColumnCycles",
                     lbar_consumer_wait_column_cycles);
        PRINT_B3_U64("lbarConsumerWaitEngineCycles",
                     lbar_consumer_wait_engine_cycles);
        PRINT_B3_U64("consumerSchedulerAgingPromotions",
                     consumer_scheduler_aging_promotions);
        PRINT_B3_U64("consumerSchedulerStarvationViolations",
                     consumer_scheduler_starvation_violations);
        PRINT_B3_U64("frontierWaitDqStarCycles",
                     frontier_wait_dqstar_cycles);
        PRINT_B3_U64("frontierWaitPreviousDqCycles",
                     frontier_wait_previous_dq_cycles);
        PRINT_B3_U64("frontierWaitLbarColumnsCycles",
                     frontier_wait_lbar_columns_cycles);
        PRINT_B3_U64("frontierWaitConsumerEngineCycles",
                     frontier_wait_consumer_engine_cycles);
        PRINT_B3_U64("frontierWaitDqStarPublishCycles",
                     frontier_wait_dqstar_publish_cycles);
        PRINT_B3_U64("shadowCorrectionBitwiseMismatch",
                     shadow_correction_bitwise_mismatch);
        PRINT_B3_U64("shadowDqStarBitwiseMismatch",
                     shadow_dqstar_bitwise_mismatch);
#undef PRINT_B3_U64
        printf("%s.streamingStep2.shadowBaseMaxAbsError = %.17g\n", prefix,
               stats->shadow_base_max_abs_error);
        printf("%s.streamingStep2.shadowBaseMaxRelError = %.17g\n", prefix,
               stats->shadow_base_max_rel_error);
        printf("%s.streamingStep2.shadowCorrectionMaxAbsError = %.17g\n",
               prefix, stats->shadow_correction_max_abs_error);
        printf("%s.streamingStep2.shadowCorrectionMaxRelError = %.17g\n",
               prefix, stats->shadow_correction_max_rel_error);
        printf("%s.streamingStep2.shadowDqStarMaxAbsError = %.17g\n",
               prefix, stats->shadow_dqstar_max_abs_error);
        printf("%s.streamingStep2.shadowDqStarMaxRelError = %.17g\n",
               prefix, stats->shadow_dqstar_max_rel_error);
        printf("%s.streamingStep2.totalCycles = %lu\n", prefix,
               (unsigned long)stats->streaming_total_cycles);
    }
    printf("%s.eventRequestsAllocated = %lu\n", prefix,
           (unsigned long)stats->event_requests_allocated);
    printf("%s.eventRequestsCompleted = %lu\n", prefix,
           (unsigned long)stats->event_requests_completed);
    printf("%s.eventRequestsFailed = %lu\n", prefix,
           (unsigned long)stats->event_requests_failed);
    printf("%s.eventRequestsFreed = %lu\n", prefix,
           (unsigned long)stats->event_requests_freed);
    printf("%s.eventRequestsMaxLive = %lu\n", prefix,
           (unsigned long)stats->event_requests_max_live);
    printf("%s.eventFirstCellLatency = %lu\n", prefix,
           (unsigned long)stats->event_first_cell_latency);
    printf("%s.eventAverageCellLatency = %.6f\n", prefix,
           ratio_or_zero(stats->event_cell_latency_sum,
                         stats->event_requests_completed));
    printf("%s.eventLastCellLatency = %lu\n", prefix,
           (unsigned long)stats->event_last_cell_latency);
    printf("%s.eventSteadyStateII = %.6f\n", prefix,
           stats->event_requests_completed > 1 ?
           (double)(stats->event_last_completion_cycle -
                    stats->event_first_completion_cycle) /
           (double)(stats->event_requests_completed - 1) : 0.0);
    printf("%s.eventAverageDInvReadyLatency = %.6f\n", prefix,
           ratio_or_zero(stats->event_dinv_ready_latency_sum,
                         stats->event_requests_completed));
    printf("%s.eventAverageLBarReadyLatency = %.6f\n", prefix,
           ratio_or_zero(stats->event_lbar_ready_latency_sum,
                         stats->event_requests_completed));
    printf("%s.eventAverageUBarReadyLatency = %.6f\n", prefix,
           ratio_or_zero(stats->event_ubar_ready_latency_sum,
                         stats->event_requests_completed));
    printf("%s.eventInputActiveCycles = %lu\n", prefix,
           (unsigned long)stats->event_input_active_cycles);
    printf("%s.eventLuActiveCycles = %lu\n", prefix,
           (unsigned long)stats->event_lu_active_cycles);
    printf("%s.eventSolveActiveCycles = %lu\n", prefix,
           (unsigned long)stats->event_solve_active_cycles);
    printf("%s.eventDrainActiveCycles = %lu\n", prefix,
           (unsigned long)stats->event_drain_active_cycles);
    printf("%s.eventInputLuOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_input_lu_overlap_cycles);
    printf("%s.eventInputSolveOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_input_solve_overlap_cycles);
    printf("%s.eventLuSolveOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_lu_solve_overlap_cycles);
    printf("%s.eventComputeDrainOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_compute_drain_overlap_cycles);
    printf("%s.eventThreeWayOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_three_way_overlap_cycles);
    printf("%s.eventLuDivIssued = %lu\n", prefix,
           (unsigned long)stats->event_lu_div_issued);
    printf("%s.eventLuMulIssued = %lu\n", prefix,
           (unsigned long)stats->event_lu_mul_issued);
    printf("%s.eventLuSubIssued = %lu\n", prefix,
           (unsigned long)stats->event_lu_sub_issued);
    printf("%s.eventCoeffDivIssued = %lu\n", prefix,
           (unsigned long)stats->event_coeff_div_issued);
    printf("%s.eventCoeffMulIssued = %lu\n", prefix,
           (unsigned long)stats->event_coeff_mul_issued);
    printf("%s.eventCoeffSubIssued = %lu\n", prefix,
           (unsigned long)stats->event_coeff_sub_issued);
    printf("%s.eventDividerBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_divider_busy_lane_cycles);
    printf("%s.eventMulBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_mul_busy_lane_cycles);
    printf("%s.eventSubBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_sub_busy_lane_cycles);
    printf("%s.eventStallInputSlot = %lu\n", prefix,
           (unsigned long)stats->event_stall_input_slot);
    printf("%s.eventStallLuPending = %lu\n", prefix,
           (unsigned long)stats->event_stall_lu_pending);
    printf("%s.eventStallSolvePending = %lu\n", prefix,
           (unsigned long)stats->event_stall_solve_pending);
    printf("%s.eventStallOutputRing = %lu\n", prefix,
           (unsigned long)stats->event_stall_output_ring);
    printf("%s.eventStallLuDivider = %lu\n", prefix,
           (unsigned long)stats->event_stall_lu_divider);
    printf("%s.eventStallLuMulSub = %lu\n", prefix,
           (unsigned long)stats->event_stall_lu_mulsub);
    printf("%s.eventStallCoeffDivider = %lu\n", prefix,
           (unsigned long)stats->event_stall_coeff_divider);
    printf("%s.eventStallCoeffMulSub = %lu\n", prefix,
           (unsigned long)stats->event_stall_coeff_mulsub);
    printf("%s.eventStallDependency = %lu\n", prefix,
           (unsigned long)stats->event_stall_dependency);
    printf("%s.eventStallSpmReadPort = %lu\n", prefix,
           (unsigned long)stats->event_stall_spm_read_port);
    printf("%s.eventStallSpmWritePort = %lu\n", prefix,
           (unsigned long)stats->event_stall_spm_write_port);
    printf("%s.eventStallSpmBank = %lu\n", prefix,
           (unsigned long)stats->event_stall_spm_bank);
    printf("%s.eventStallDrainQueue = %lu\n", prefix,
           (unsigned long)stats->event_stall_drain_queue);
    printf("%s.eventInputBytes = %lu\n", prefix,
           (unsigned long)stats->event_input_bytes);
    printf("%s.eventOutputBytes = %lu\n", prefix,
           (unsigned long)stats->event_output_bytes);
    printf("%s.eventInputWriteRequests = %lu\n", prefix,
           (unsigned long)stats->event_input_write_requests);
    printf("%s.eventDrainRequests = %lu\n", prefix,
           (unsigned long)stats->event_drain_requests);
    printf("%s.eventMaxOutputOccupancy = %lu\n", prefix,
           (unsigned long)stats->event_max_output_occupancy);
    printf("%s.eventSchedulerScans = %lu\n", prefix,
           (unsigned long)stats->event_scheduler_scans);
    printf("%s.eventSchedulerSelections = %lu\n", prefix,
           (unsigned long)stats->event_scheduler_selections);
    printf("%s.eventSchedulerNoSelection = %lu\n", prefix,
           (unsigned long)stats->event_scheduler_no_selection);
    printf("%s.eventReadyRhsAverage = %.6f\n", prefix,
           ratio_or_zero(stats->event_ready_rhs_count_sum,
                         stats->event_scheduler_cycles));
    printf("%s.eventReadyRhsMax = %lu\n", prefix,
           (unsigned long)stats->event_ready_rhs_count_max);
    printf("%s.eventCyclesWithNoReadyRhs = %lu\n", prefix,
           (unsigned long)stats->event_cycles_with_no_ready_rhs);
    printf("%s.eventDividerReadyCandidates = %lu\n", prefix,
           (unsigned long)stats->event_divider_ready_candidates);
    printf("%s.eventMulSubReadyCandidates = %lu\n", prefix,
           (unsigned long)stats->event_mulsub_ready_candidates);
    printf("%s.eventRhsAverageWaitCycles = %.6f\n", prefix,
           ratio_or_zero(stats->event_rhs_wait_cycles,
                         stats->event_scheduler_selections));
    printf("%s.eventRhsMaxWaitCycles = %lu\n", prefix,
           (unsigned long)stats->event_rhs_max_wait_cycles);
    printf("%s.eventRhsStarvationCycles = %lu\n", prefix,
           (unsigned long)stats->event_rhs_starvation_cycles);
    printf("%s.eventIdentityBatchWaitCycles = %lu\n", prefix,
           (unsigned long)stats->event_identity_batch_wait_cycles);
    printf("%s.eventLBarBatchWaitCycles = %lu\n", prefix,
           (unsigned long)stats->event_lbar_batch_wait_cycles);
    printf("%s.eventUBarBatchWaitCycles = %lu\n", prefix,
           (unsigned long)stats->event_ubar_batch_wait_cycles);
    printf("%s.eventCoeffDividerBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_coeff_divider_busy_lane_cycles);
    printf("%s.eventCoeffDividerIdleLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_coeff_divider_idle_lane_cycles);
    printf("%s.eventCoeffDividerUtilization = %.6f\n", prefix,
           ratio_or_zero(stats->event_coeff_divider_busy_lane_cycles,
                         stats->event_coeff_divider_busy_lane_cycles +
                         stats->event_coeff_divider_idle_lane_cycles));
    printf("%s.eventCoeffMulBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_coeff_mul_busy_lane_cycles);
    printf("%s.eventCoeffSubBusyLaneCycles = %lu\n", prefix,
           (unsigned long)stats->event_coeff_sub_busy_lane_cycles);
    for (int k = 0; k < N; k++) {
        printf("%s.eventLuStep%dAverageReadyLatency = %.6f\n", prefix, k,
               ratio_or_zero(stats->event_lu_step_ready_cycle[k],
                             stats->event_requests_completed));
        printf("%s.eventLuStep%dAverageFirstUseLatency = %.6f\n", prefix, k,
               ratio_or_zero(stats->event_lu_step_first_use_latency[k],
                             stats->event_requests_completed));
    }
    printf("%s.eventEarlySolveIssued = %lu\n", prefix,
           (unsigned long)stats->event_early_solve_issued);
    printf("%s.eventEarlySolveMissed = %lu\n", prefix,
           (unsigned long)stats->event_early_solve_missed);
    printf("%s.eventTrsmBlockedByLuStepCycles = %lu\n", prefix,
           (unsigned long)stats->event_trsm_blocked_by_lu_step_cycles);
    printf("%s.eventSameCellLuSolveOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_same_cell_lu_solve_overlap_cycles);
    printf("%s.eventCrossCellLuSolveOverlapCycles = %lu\n", prefix,
           (unsigned long)stats->event_cross_cell_lu_solve_overlap_cycles);
    printf("%s.eventCoeffSpmPacketIssued = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_packet_issued);
    printf("%s.eventCoeffSpmPacketCompleted = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_packet_completed);
    printf("%s.eventCoeffInputPacketBytes = %lu\n", prefix,
           (unsigned long)stats->event_coeff_input_packet_bytes);
    printf("%s.eventCoeffDrainPacketBytes = %lu\n", prefix,
           (unsigned long)stats->event_coeff_drain_packet_bytes);
    printf("%s.eventCoeffSpmReadPortStalls = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_read_port_stalls);
    printf("%s.eventCoeffSpmWritePortStalls = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_write_port_stalls);
    printf("%s.eventCoeffSpmBankStalls = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_bank_stalls);
    printf("%s.eventCoeffSpmOutstandingStalls = %lu\n", prefix,
           (unsigned long)stats->event_coeff_spm_outstanding_stalls);
    printf("%s.runtimeCyclesPerSweep = %.6f\n", prefix,
           ratio_or_zero(stats->trsv5_raw_runtime_cycles ?
                         stats->trsv5_raw_runtime_cycles :
                         stats->pretransform_raw_runtime_cycles,
                         cfg->sweeps));
    printf("%s.result = %s\n", prefix,
           cmp->mismatch_count == 0 ? "PASS" : "FAIL");
    print_compare(prefix, cmp);
    if (check) {
        char name[160];
        snprintf(name, sizeof(name), "%s.d_inv", prefix);
        print_matrix_compare(name, &check->d_inv);
        snprintf(name, sizeof(name), "%s.l_bar", prefix);
        print_matrix_compare(name, &check->l_bar);
        snprintf(name, sizeof(name), "%s.u_bar", prefix);
        print_matrix_compare(name, &check->u_bar);
    }
}
