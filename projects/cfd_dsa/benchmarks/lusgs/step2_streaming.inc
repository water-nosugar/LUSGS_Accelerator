static int
launch_streaming_coeff_cell(Problem *p, const Config *cfg,
                            StreamingCellState *state,
                            StreamingCompletionEntry *completion,
                            double *base_out, double *correction_out,
                            double *dq_star_out,
                            size_t cell, uint64_t generation,
                            uint64_t issue_sequence)
{
    memset(state, 0, sizeof(*state));
    memset(completion, 0, sizeof(*completion));
    state->line_id = 0;
    state->cell_id = cell;
    state->generation = generation;
    state->completion = completion;
    completion->active = 1;
    completion->attached = 1;
    completion->line_id = 0;
    completion->cell_id = cell;
    completion->generation = generation;
    completion->issue_sequence = issue_sequence;
    completion->record.status = COEFF_PRE_STATUS_BUSY;
    uint64_t coefficient_mask = 0;
    if (cfg->coeff_stream_boundary_mask) {
        coefficient_mask = COEFF_PRE_NEED_DINV;
        if (p->cells > 1 && cell > 0)
            coefficient_mask |= COEFF_PRE_NEED_LBAR;
        if (p->cells > 1 && cell + 1 < p->cells)
            coefficient_mask |= COEFF_PRE_NEED_UBAR;
    }
    completion->required_coefficient_mask = coefficient_mask ?
        coefficient_mask : (COEFF_PRE_NEED_DINV | COEFF_PRE_NEED_LBAR |
                            COEFF_PRE_NEED_UBAR);
    CoeffPreprocessDescriptor *d = &state->descriptor;
    d->version = 1;
    d->size = sizeof(*d);
    d->flags = COEFF_PRE_COEFF3;
    d->flags |= COEFF_PRE_STREAMING_PROGRESS;
    if (!strcmp(cfg->coeff_stream_retire_mode, "auto"))
        d->flags |= COEFF_PRE_STREAMING_AUTO_RETIRE;
    if (strcmp(cfg->coeff_stream_dinv_consumer, "off")) {
        d->flags |= COEFF_PRE_DINV_BASE_CONSUMER;
        if (!strcmp(cfg->coeff_stream_dinv_consumer, "direct"))
            d->flags |= COEFF_PRE_DINV_DIRECT_BYPASS;
        d->reserved[0] = (uint64_t)(uintptr_t)
            vec_const_at(p->rhs, p, 0, cell);
        d->reserved[1] = (uint64_t)(uintptr_t)base_out;
    }
    if (strcmp(cfg->coeff_stream_lbar_consumer, "off")) {
        d->flags |= COEFF_PRE_LBAR_FORWARD_CONSUMER;
        if (!strcmp(cfg->coeff_stream_lbar_consumer, "direct"))
            d->flags |= COEFF_PRE_LBAR_DIRECT_BYPASS;
        d->reserved[2] = (uint64_t)(uintptr_t)correction_out;
        d->reserved[3] = (uint64_t)(uintptr_t)dq_star_out;
    }
    d->flags |= coefficient_mask;
    if (cfg->coeff3_partial_output)
        d->flags |= COEFF_PRE_PARTIAL_OUTPUT;
    if (cfg->lu_forwarding)
        d->flags |= COEFF_PRE_LU_FORWARD;
    d->generation = generation;
    d->line_id = 0;
    d->cell_id = cell;
    d->d_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->d_mat, p, 0, cell);
    d->l_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->l_mat, p, 0, cell);
    d->u_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->u_mat, p, 0, cell);
    d->lu_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->lu_d, p, 0, cell);
    d->d_inv_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->d_inv, p, 0, cell);
    d->l_bar_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->l_bar_precomputed, p, 0, cell);
    d->u_bar_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->u_bar, p, 0, cell);
    d->record_addr = (uint64_t)(uintptr_t)&completion->record;
    d->pivot_epsilon = cfg->pretransform_diag_epsilon;
    completion->token = coeff_preprocess_launch(d);
    if (!completion->token) {
        completion->active = 0;
        completion->attached = 0;
        state->completion = NULL;
        return 0;
    }
    return 1;
}

static void
sample_streaming_window(Stats *stats, uint64_t live)
{
    stats->streaming_request_window_occupancy_sum += live;
    stats->streaming_request_window_samples++;
    stats->streaming_output_ring_occupancy_sum += live;
    stats->streaming_output_ring_samples++;
    if (live > stats->streaming_request_window_max_occupancy)
        stats->streaming_request_window_max_occupancy = live;
}

static uint64_t
first_nonzero(uint64_t current, uint64_t candidate)
{
    if (!candidate)
        return current;
    return !current || candidate < current ? candidate : current;
}

static void
record_streaming_timestamps(Stats *stats, StreamingCompletionEntry *entry)
{
    const CoeffPreprocessRecord *r = &entry->record;

    stats->streaming_first_input_issue_cycle = first_nonzero(
        stats->streaming_first_input_issue_cycle, r->input_start_cycle);
    stats->streaming_first_lu_issue_cycle = first_nonzero(
        stats->streaming_first_lu_issue_cycle, r->lu_issue_cycle);
    stats->streaming_first_dinv_ready_cycle = first_nonzero(
        stats->streaming_first_dinv_ready_cycle, r->d_inv_ready_cycle);
    stats->streaming_first_lbar_ready_cycle = first_nonzero(
        stats->streaming_first_lbar_ready_cycle, r->l_bar_ready_cycle);
    stats->streaming_first_ubar_ready_cycle = first_nonzero(
        stats->streaming_first_ubar_ready_cycle, r->u_bar_ready_cycle);
    for (int col = 0; col < N; col++) {
        stats->streaming_first_dinv_column_ready_cycle = first_nonzero(
            stats->streaming_first_dinv_column_ready_cycle,
            r->d_inv_column_ready_cycle[col]);
        stats->streaming_first_lbar_column_ready_cycle = first_nonzero(
            stats->streaming_first_lbar_column_ready_cycle,
            r->l_bar_column_ready_cycle[col]);
        stats->streaming_first_ubar_column_ready_cycle = first_nonzero(
            stats->streaming_first_ubar_column_ready_cycle,
            r->u_bar_column_ready_cycle[col]);
    }
    if (r->complete_cycle > stats->streaming_preprocess_finish_cycle)
        stats->streaming_preprocess_finish_cycle = r->complete_cycle;
    if (r->generation != entry->generation)
        stats->streaming_stale_generation_rejected++;

}

static void
finalize_streaming_timeline(Stats *stats)
{
    stats->streaming_forward_started_before_preprocess_done = 0;
    stats->streaming_preprocess_forward_overlap_cycles = 0;
    stats->streaming_preprocess_backward_prepare_overlap_cycles = 0;

    if (!stats->streaming_first_input_issue_cycle ||
        !stats->streaming_first_forward_issue_cycle ||
        !stats->streaming_preprocess_finish_cycle ||
        !stats->streaming_forward_finish_cycle ||
        !stats->streaming_backward_start_cycle ||
        !stats->streaming_backward_finish_cycle ||
        stats->streaming_preprocess_finish_cycle <
            stats->streaming_first_input_issue_cycle ||
        stats->streaming_forward_finish_cycle <
            stats->streaming_first_forward_issue_cycle ||
        stats->streaming_backward_start_cycle <
            stats->streaming_forward_finish_cycle ||
        stats->streaming_backward_finish_cycle <
            stats->streaming_backward_start_cycle)
        stats->streaming_timeline_inconsistencies++;

    if (stats->streaming_first_forward_issue_cycle &&
        stats->streaming_first_forward_issue_cycle <
            stats->streaming_preprocess_finish_cycle) {
        stats->streaming_forward_started_before_preprocess_done = 1;
        uint64_t overlap_end = stats->streaming_forward_finish_cycle <
            stats->streaming_preprocess_finish_cycle ?
            stats->streaming_forward_finish_cycle :
            stats->streaming_preprocess_finish_cycle;
        if (overlap_end > stats->streaming_first_forward_issue_cycle)
            stats->streaming_preprocess_forward_overlap_cycles =
                overlap_end - stats->streaming_first_forward_issue_cycle;
    }

    uint64_t backward_prepare_start =
        stats->streaming_first_ubar_column_ready_cycle >
            stats->streaming_first_forward_issue_cycle ?
        stats->streaming_first_ubar_column_ready_cycle :
        stats->streaming_first_forward_issue_cycle;
    uint64_t backward_prepare_end = stats->streaming_forward_finish_cycle <
            stats->streaming_preprocess_finish_cycle ?
        stats->streaming_forward_finish_cycle :
        stats->streaming_preprocess_finish_cycle;
    if (backward_prepare_end > backward_prepare_start)
        stats->streaming_preprocess_backward_prepare_overlap_cycles =
            backward_prepare_end - backward_prepare_start;
}

static int
streaming_progress_status(uint64_t status)
{
    return status == COEFF_PRE_STATUS_DINV_READY ||
        status == COEFF_PRE_STATUS_DINV_LBAR_READY ||
        status == COEFF_PRE_STATUS_DINV_LBAR_UBAR_READY;
}

static uint64_t
streaming_reaper_occupancy(StreamingCompletionEntry *entries,
                           uint64_t capacity)
{
    uint64_t active = 0;
    for (uint64_t i = 0; i < capacity; i++)
        active += entries[i].active != 0;
    return active;
}

static void
sample_streaming_reaper(Stats *stats, StreamingCompletionEntry *entries,
                        uint64_t capacity)
{
    uint64_t occupancy = streaming_reaper_occupancy(entries, capacity);
    if (occupancy > stats->streaming_completion_reaper_queue_max_occupancy)
        stats->streaming_completion_reaper_queue_max_occupancy = occupancy;
}

static StreamingCompletionEntry *
allocate_streaming_completion(StreamingCompletionEntry *entries,
                              uint64_t capacity, uint64_t *cursor,
                              Stats *stats)
{
    for (uint64_t offset = 0; offset < capacity; offset++) {
        uint64_t index = (*cursor + offset) % capacity;
        if (!entries[index].active) {
            *cursor = (index + 1) % capacity;
            return &entries[index];
        }
    }
    sample_streaming_reaper(stats, entries, capacity);
    return NULL;
}

static void
update_streaming_progress(StreamingCompletionEntry *entry, uint64_t status)
{
    if (status == COEFF_PRE_STATUS_DINV_READY) {
        entry->dinv_progress = 1;
    } else if (status == COEFF_PRE_STATUS_DINV_LBAR_READY) {
        entry->dinv_progress = 1;
        entry->lbar_progress = 1;
    } else if (status == COEFF_PRE_STATUS_DINV_LBAR_UBAR_READY) {
        entry->dinv_progress = 1;
        entry->lbar_progress = 1;
        entry->ubar_progress = 1;
    }
}

static int
streaming_completion_forward_ready(const StreamingCompletionEntry *entry)
{
    return entry->dinv_progress &&
        (entry->cell_id == 0 || entry->lbar_progress);
}

static int
poll_streaming_entry(StreamingCompletionEntry *entries, uint64_t capacity,
                     StreamingCompletionEntry *entry, Stats *stats)
{
    if (!entry || !entry->active || entry->reaped)
        return 0;

    stats->streaming_completion_reaper_polls++;
    stats->progress_wait_calls++;
    uint64_t status = coeff_preprocess_wait(entry->token);
    if (status == COEFF_PRE_STATUS_BUSY) {
        stats->streaming_completion_reaper_busy_polls++;
        stats->terminal_wait_busy_returns++;
        return 1;
    }
    if (streaming_progress_status(status)) {
        update_streaming_progress(entry, status);
        return 1;
    }

    __asm__ __volatile__("" ::: "memory");
    entry->terminal = 1;
    entry->reaped = 1;
    entry->terminal_status = status;
    stats->streaming_completion_reaper_terminal_hits++;
    stats->terminal_wait_calls++;
    stats->event_requests_freed++;
    for (uint64_t i = 0; i < capacity; i++) {
        if (entries[i].active && !entries[i].reaped &&
            entries[i].issue_sequence < entry->issue_sequence) {
            stats->streaming_completion_reaper_out_of_order_reaps++;
            break;
        }
    }
    if (status != COEFF_PRE_STATUS_COMPLETE ||
        entry->record.status != COEFF_PRE_STATUS_COMPLETE) {
        entry->error_status = status ? (int)status :
            (int)entry->record.status;
        stats->event_requests_failed++;
        return -1;
    }
    if (entry->record.generation != entry->generation ||
        entry->record.line_id != entry->line_id ||
        entry->record.cell_id != entry->cell_id) {
        entry->error_status = COEFF_PRE_STATUS_GENERATION_MISMATCH;
        stats->streaming_stale_generation_rejected++;
        stats->streaming_stale_generation_reads++;
        stats->event_requests_failed++;
        return -1;
    }

    entry->dinv_progress = entry->record.d_inv_drain_done != 0;
    entry->lbar_progress = entry->record.l_bar_drain_done != 0;
    entry->ubar_progress = entry->record.u_bar_drain_done != 0;
    aggregate_coeff_event_record(stats, &entry->record);
    record_streaming_timestamps(stats, entry);
    if (!entry->attached)
        entry->active = 0;
    sample_streaming_reaper(stats, entries, capacity);
    return 1;
}

static int
poll_streaming_reaper(StreamingCompletionEntry *entries, uint64_t capacity,
                      uint64_t *poll_cursor, Stats *stats)
{
    StreamingCompletionEntry *entry = NULL;
    for (uint64_t offset = 0; offset < capacity; offset++) {
        uint64_t index = (*poll_cursor + offset) % capacity;
        if (entries[index].active && !entries[index].reaped) {
            entry = &entries[index];
            *poll_cursor = (index + 1) % capacity;
            break;
        }
    }
    return poll_streaming_entry(entries, capacity, entry, stats);
}

// Report before cache conditioning and the measured interval. The monotonic
// call index also identifies invocations in functional policy-transition tests.
static void
report_lu_output_range(const Problem *p, const Config *cfg, uint64_t generation)
{
    static uint64_t round = 0;
    printf("CFD_LU_OUTPUT_RANGE round=%llu generation=%llu publish=%d "
           "base=0x%llx bytes=%llu cells=%llu\n",
           (unsigned long long)++round, (unsigned long long)generation,
           cfg->coeff_publish_lu, (unsigned long long)(uintptr_t)p->lu_d,
           (unsigned long long)(p->lines*p->cells*N*N*sizeof(double)),
           (unsigned long long)(p->lines*p->cells));
}

// The private ABI promises that the external LU destination is untouched.
// Debug reconstruction below must never turn this buffer into a CPU output.
static int
validate_local_matrix_poison(const Problem *p, const double *matrix,
                             const char *name)
{
    const double poison = NAN;
    uint64_t expected;
    memcpy(&expected, &poison, sizeof(expected));
    for (size_t i = 0; i < p->lines*p->cells*N*N; ++i) {
        uint64_t actual;
        memcpy(&actual, &matrix[i], sizeof(actual));
        if (actual != expected) {
            fprintf(stderr, "CFD_LOCAL_%s_UNEXPECTED_WRITE index=%lu\n",
                    name, (unsigned long)i);
            return 0;
        }
    }
    return 1;
}

static int
validate_local_lu_poison(const Problem *p)
{
    return validate_local_matrix_poison(p, p->lu_d, "LU");
}

static int
validate_streaming_coefficients(Problem *p, const Config *cfg, Stats *stats,
                                PretransformCheck *check,
                                const double *base_vectors)
{
    int all_valid = 1;
    const int direct = !strcmp(cfg->coeff_stream_dinv_consumer, "direct");
    const int direct_solve = !strcmp(cfg->coeff_base_solve, "direct");
    const int lbar_direct =
        !strcmp(cfg->coeff_stream_lbar_consumer, "direct");
    for (size_t line = 0; line < p->lines; line++) {
      for (size_t cell = 0; cell < p->cells; cell++) {
        const int use_lbar = p->cells > 1 && cell > 0;
        const int use_ubar = p->cells > 1 && cell + 1 < p->cells;
        const int need_lbar = !cfg->coeff_stream_boundary_mask || use_lbar;
        const int need_ubar = !cfg->coeff_stream_boundary_mask || use_ubar;
        const double *raw_d = mat_const_at(p->d_mat, p, line, cell);
        const double *lower = mat_const_at(p->l_mat, p, line, cell);
        const double *upper = mat_const_at(p->u_mat, p, line, cell);
        double debug_lu[N*N];
        const double *lu;
        if (cfg->coeff_publish_lu) {
            lu = mat_const_at(p->lu_d, p, line, cell);
        } else {
            // Reference-only sanity check outside all performance ROIs. This
            // is not a validation or reconstruction of private hardware LU;
            // the original y/dq recurrence and independent oracle test that.
            if (!cfd_lu5_factor(raw_d, debug_lu,
                               cfg->pretransform_diag_epsilon)) {
                all_valid = 0;
                continue;
            }
            lu = debug_lu;
        }
        const double *d_inv = mat_const_at(p->d_inv, p, line, cell);
        const double *l_bar =
            mat_const_at(p->l_bar_precomputed, p, line, cell);
        const double *u_bar = mat_const_at(p->u_bar, p, line, cell);
        stats->lu5_factor_cells++;
        stats->lu5_factor_input_bytes += N * N * sizeof(double);
        if (cfg->coeff_publish_lu)
            stats->lu5_factor_output_bytes += N * N * sizeof(double);
        stats->ubar_generated += need_ubar;
        stats->coeff3_issued++;
        stats->coeff3_completed++;
        stats->coeff3_columns_solved += (direct_solve ? 1 : 5) +
            5 * need_lbar + 5 * need_ubar;
        stats->dinv_generated += !direct_solve;
        stats->base_direct_rhs_solved += direct_solve;
        stats->lbar_generated += need_lbar;
        for (int i = 0; i < N * N; i++) {
            if (!isfinite(lu[i]) || (!direct && !isfinite(d_inv[i])) ||
                (need_lbar && !isfinite(l_bar[i])) ||
                (need_ubar && !cfg->coeff_local_ubar && !isfinite(u_bar[i])))
                all_valid = 0;
        }
        int validate = !strcmp(cfg->coeff_validation, "full") ||
            (!strcmp(cfg->coeff_validation, "sampled") &&
            (line * p->cells + cell) %
                cfg->coeff_validation_sample_rate == 0);
        if (!validate)
            continue;
        uint64_t start = read_cycle_counter();
        double identity[N * N] = {};
        double ref[N * N];
        for (int i = 0; i < N; i++)
            identity[i * N + i] = 1.0;
        double lu_residual = lu_factor_residual_max(raw_d, lu);
        double d_residual = direct ? 0.0 :
            matrix_residual_max(lu, d_inv, identity);
        double lbar_debug[N * N];
        if (lbar_direct && need_lbar) {
            // Direct B4 deliberately has no Lbar guest drain.  Reconstruct
            // the debug matrix outside the measured streaming solve.
            trsm5_mrhs_software(lu, lower, lbar_debug);
            l_bar = lbar_debug;
        }
        double ubar_debug[N * N];
        if (cfg->coeff_local_ubar && need_ubar) {
            trsm5_mrhs_software(lu, upper, ubar_debug);
            u_bar = ubar_debug;
        }
        double l_residual = need_lbar ?
            matrix_residual_max(lu, l_bar, lower) : 0.0;
        double u_residual = need_ubar ?
            matrix_residual_max(lu, u_bar, upper) : 0.0;
        double u_relative = need_ubar ?
            matrix_residual_max_relative(lu, u_bar, upper) : 0.0;
        if (lu_residual > stats->max_abs_lu_residual)
            stats->max_abs_lu_residual = lu_residual;
        if (u_residual > stats->max_abs_ubar_residual)
            stats->max_abs_ubar_residual = u_residual;
        if (u_relative > stats->max_rel_ubar_residual)
            stats->max_rel_ubar_residual = u_relative;
        if (lu_residual > 1.0e-9 || d_residual > 1.0e-9 ||
            l_residual > 1.0e-9 || u_residual > 1.0e-9)
            all_valid = 0;
        if (check) {
            trsm5_mrhs_software(lu, identity, ref);
            if (!direct)
                update_matrix_compare(&check->d_inv, ref, d_inv,
                                      d_residual, 1.0e-12, 1.0e-12);
            if (direct && base_vectors && !lbar_direct) {
                double reference_base[N];
                mvm5_software(ref, vec_const_at(p->rhs, p, line, cell),
                              reference_base);
                for (int lane = 0; lane < N; lane++) {
                    double got = base_vectors[cell * N + lane];
                    double abs_error = fabs(got - reference_base[lane]);
                    double rel_error = abs_error /
                        fmax(fabs(reference_base[lane]), 1.0e-14);
                    if (!isfinite(got) ||
                        (abs_error > 1.0e-12 && rel_error > 1.0e-12))
                        all_valid = 0;
                }
            }
            if (need_lbar) {
                trsm5_mrhs_software(lu, lower, ref);
                update_matrix_compare(&check->l_bar, ref, l_bar,
                                      l_residual, 1.0e-12, 1.0e-12);
            }
            if (need_ubar) {
                trsm5_mrhs_software(lu, upper, ref);
                update_matrix_compare(&check->u_bar, ref, u_bar,
                                      u_residual, 1.0e-12, 1.0e-12);
            }
        }
        uint64_t end = read_cycle_counter();
        if (end >= start) {
            stats->preprocess_validation_cycles += end - start;
            stats->preprocess_residual_check_cycles += end - start;
        }
    }
    }
    return all_valid;
}

static void
cancel_streaming_reaper(StreamingCompletionEntry *entries,
                        uint64_t capacity, Stats *stats)
{
    for (uint64_t slot = 0; slot < capacity; slot++) {
        StreamingCompletionEntry *entry = &entries[slot];
        if (!entry->active)
            continue;
        if (entry->reaped) {
            entry->active = 0;
            continue;
        }
        entry->cancel_pending = 1;
        (void)coeff_preprocess_cancel(entry->token);
        stats->streaming_completion_reaper_cancels++;
        uint64_t status;
        do {
            status = coeff_preprocess_wait(entry->token);
        } while (status == COEFF_PRE_STATUS_BUSY ||
                 status == COEFF_PRE_STATUS_DINV_READY ||
                 status == COEFF_PRE_STATUS_DINV_LBAR_READY ||
                 status == COEFF_PRE_STATUS_DINV_LBAR_UBAR_READY);
        stats->event_requests_freed++;
        entry->terminal_status = status;
        entry->terminal = 1;
        entry->reaped = 1;
        entry->active = 0;
    }
}

static int
auto_record_identity_valid(const StreamingCompletionEntry *entry)
{
    const CoeffPreprocessRecord *r = &entry->record;
    return r->request_id && r->generation == entry->generation &&
        r->line_id == entry->line_id && r->cell_id == entry->cell_id;
}

static int
auto_record_forward_ready(const StreamingCompletionEntry *entry,
                          int consumer, int direct,
                          const char *lbar_consumer)
{
    const CoeffPreprocessRecord *r = &entry->record;
    if (!strcmp(lbar_consumer, "direct"))
        return r->dq_star_ready != 0;
    const int base_ready = !consumer || r->base_ready;
    const int dinv_ready = direct || r->d_inv_drain_done;
    const int lbar_ready = entry->cell_id == 0 || r->l_bar_drain_done;
    const int shadow_dq_ready = strcmp(lbar_consumer, "shadow") ||
                                r->dq_star_ready;
    return base_ready && dinv_ready && lbar_ready && shadow_dq_ready;
}

static int
wait_auto_forward_record(StreamingCompletionEntry *entry, const Config *cfg,
                         Stats *stats)
{
    uint64_t start = read_cycle_counter();
    const int consumer = strcmp(cfg->coeff_stream_dinv_consumer, "off") != 0;
    const int direct = !strcmp(cfg->coeff_stream_dinv_consumer, "direct");
    const int lbar_direct =
        !strcmp(cfg->coeff_stream_lbar_consumer, "direct");
    if (lbar_direct) {
        // Direct B4 has exactly one release condition.  Keep the polling
        // loop to an ordinary acquire-like record load; the generic B3 loop
        // performs several strcmp/identity/stat updates per probe and would
        // turn guest bookkeeping into the dominant forward cost.
        uint64_t polls = 0;
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
            if (entry->record.dq_star_ready)
                break;
            if (entry->record.complete_cycle &&
                entry->record.status != COEFF_PRE_STATUS_COMPLETE) {
                stats->guest_terminal_error_checks++;
                stats->auto_retire_errors++;
                return 0;
            }
            polls++;
        }
        stats->progress_poll_calls += polls + 1;
        stats->guest_completion_record_checks += polls + 1;
        stats->progress_poll_busy += polls;
        stats->progress_poll_useful++;
        if (!auto_record_identity_valid(entry)) {
            stats->streaming_stale_generation_reads++;
            return 0;
        }
        uint64_t end = read_cycle_counter();
        if (end >= start) {
            const uint64_t cycles = end - start;
            stats->progress_poll_cycles += cycles;
            stats->frontier_no_work_cycles += cycles;
            stats->frontier_wait_dqstar_cycles += cycles;
        }
        entry->dinv_progress = 1;
        entry->lbar_progress = 1;
        return 1;
    }
    const int base_ready_at_entry = entry->record.base_ready != 0;
    int saw_dinv = direct, saw_base = !consumer;
    int saw_lbar = entry->cell_id == 0;
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
        stats->progress_poll_calls++;
        stats->guest_completion_record_checks++;
        const CoeffPreprocessRecord *r = &entry->record;
        if (r->request_id && !auto_record_identity_valid(entry)) {
            stats->streaming_stale_generation_reads++;
            return 0;
        }
        if (r->complete_cycle && r->status != COEFF_PRE_STATUS_COMPLETE) {
            stats->guest_terminal_error_checks++;
            stats->auto_retire_errors++;
            return 0;
        }
        if (!saw_dinv && r->d_inv_drain_done) {
            saw_dinv = 1;
            stats->progress_poll_useful++;
        }
        if (!saw_lbar && r->l_bar_drain_done) {
            saw_lbar = 1;
            stats->progress_poll_useful++;
        }
        if (!saw_base && r->base_ready) {
            saw_base = 1;
            stats->progress_poll_useful++;
        }
        if (auto_record_forward_ready(entry, consumer, direct,
                                      cfg->coeff_stream_lbar_consumer))
            break;
        stats->progress_poll_busy++;
    }
    uint64_t end = read_cycle_counter();
    if (end >= start) {
        const uint64_t cycles = end - start;
        stats->progress_poll_cycles += cycles;
        stats->frontier_no_work_cycles += cycles;
        if (lbar_direct)
            stats->frontier_wait_dqstar_cycles += cycles;
        else if (consumer)
            stats->frontier_wait_base_cycles += cycles;
        if (!direct)
            stats->frontier_wait_dinv_cycles += cycles;
        if (entry->cell_id && !lbar_direct)
            stats->frontier_wait_lbar_cycles += cycles;
    }
    entry->dinv_progress = 1;
    entry->lbar_progress = lbar_direct || entry->cell_id == 0 ||
        entry->record.l_bar_drain_done;
    if (consumer) {
        if (base_ready_at_entry)
            stats->base_ready_before_frontier++;
        else
            stats->base_ready_after_frontier++;
    }
    return 1;
}

static int
wait_auto_terminal_record(StreamingCompletionEntry *entry, Stats *stats)
{
    uint64_t start = read_cycle_counter();
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
        stats->guest_completion_record_checks++;
        if (entry->record.complete_cycle)
            break;
    }
    uint64_t end = read_cycle_counter();
    if (end >= start) {
        stats->frontier_wait_ubar_cycles += end - start;
        stats->terminal_record_check_cycles += end - start;
    }
    stats->guest_terminal_error_checks++;
    if (!auto_record_identity_valid(entry) ||
        entry->record.status != COEFF_PRE_STATUS_COMPLETE ||
        !entry->record.auto_retired) {
        stats->auto_retire_errors++;
        stats->event_requests_failed++;
        return 0;
    }
    entry->terminal = 1;
    entry->reaped = 1;
    entry->terminal_status = entry->record.status;
    entry->dinv_progress = entry->record.d_inv_drain_done != 0;
    entry->lbar_progress = entry->record.l_bar_drain_done != 0;
    entry->ubar_progress = entry->record.u_bar_drain_done != 0;
    stats->auto_retired_requests += entry->record.auto_retired;
    stats->auto_retired_tokens += entry->record.auto_retired;
    stats->auto_retire_errors += entry->record.auto_retire_error;
    stats->event_requests_freed++;
    return 1;
}

static int
compare_shadow_base(Stats *stats, size_t cell, const double expected[N],
                    const double got[N])
{
    int valid = 1;
    for (int lane = 0; lane < N; lane++) {
        const double abs_error = fabs(expected[lane] - got[lane]);
        const double rel_error = abs_error /
            fmax(fabs(expected[lane]), 1.0e-14);
        if (abs_error > stats->shadow_base_max_abs_error)
            stats->shadow_base_max_abs_error = abs_error;
        if (rel_error > stats->shadow_base_max_rel_error)
            stats->shadow_base_max_rel_error = rel_error;
        if (memcmp(&expected[lane], &got[lane], sizeof(double))) {
            if (!stats->shadow_base_bitwise_mismatch) {
                stats->shadow_base_mismatch_cell = cell;
                stats->shadow_base_mismatch_lane = lane;
            }
            stats->shadow_base_bitwise_mismatch++;
            valid = 0;
        }
    }
    return valid;
}

static void
pack_sme_fixture_input(Problem *p, uint64_t generation)
{
    const uint64_t start = read_cycle_counter();
    if (!p->sme_cold_input)
        allocate_sme_cold_input(p);
    const size_t matrix_bytes = N * N * sizeof(double);
    for (size_t cell = 0; cell < p->cells; ++cell) {
        double *row = p->sme_cold_input + cell * SmeColdInputWords;
        memcpy(row, mat_const_at(p->d_mat, p, 0, cell), matrix_bytes);
        memcpy(row + N*N, mat_const_at(p->l_mat, p, 0, cell), matrix_bytes);
        memcpy(row + 2*N*N, mat_const_at(p->u_mat, p, 0, cell), matrix_bytes);
        memcpy(row + 3*N*N, vec_const_at(p->rhs, p, 0, cell), N * sizeof(double));
    }
    const uint64_t cycles = read_cycle_counter() - start;
    printf("CFD_INPUT_LAYOUT_SOURCE layout=packed source=fixture generation=%llu "
           "cells=%lu bytes=%lu cycles=%llu\n",
           (unsigned long long)generation, (unsigned long)p->cells,
           (unsigned long)(p->cells * SmeColdInputWords * sizeof(double)),
           (unsigned long long)cycles);
}

static int
compare_shadow_forward_vector(uint64_t *mismatches, uint64_t *first_cell,
                              uint64_t *first_lane, double *max_abs,
                              double *max_rel, size_t cell,
                              const double expected[N], const double got[N])
{
    int valid = 1;
    for (int lane = 0; lane < N; lane++) {
        const double abs_error = fabs(expected[lane] - got[lane]);
        const double rel_error = abs_error /
            fmax(fabs(expected[lane]), 1.0e-14);
        if (abs_error > *max_abs)
            *max_abs = abs_error;
        if (rel_error > *max_rel)
            *max_rel = rel_error;
        if (memcmp(&expected[lane], &got[lane], sizeof(double))) {
            if (!*mismatches) {
                *first_cell = cell;
                *first_lane = lane;
            }
            (*mismatches)++;
            valid = 0;
        }
    }
    return valid;
}

static int
run_pretransform_raw_stream_line_once(
    Problem *p, const Config *cfg, Stats *stats, double *dq_star, double *dq,
    PretransformCheck *check, uint64_t coefficient_version,
    int coefficient_dirty)
{
    static uint64_t next_line_generation = 0x200000000ULL;
    coefficient_dirty |= !cfg->coeff_line_cache_allowed;
    const uint64_t total_cells = p->lines * p->cells;
    const uint64_t matrix_bytes = total_cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = total_cells * N * sizeof(double);
    CoeffPreprocessRecord *records =
        (CoeffPreprocessRecord *)xcalloc_raw(
            total_cells, sizeof(*records), "coeff_stream_line_records");
    const int compact_records = cfg->coeff_line_opt_level >= 2;
    const int packed_records = cfg->coeff_line_opt_level >= 11;
    const size_t record_stride = packed_records ? sizeof(CoeffPackedRecord) :
                                compact_records ? sizeof(CoeffCompactRecord) :
                                                  sizeof(*records);
    CoeffPreprocessDescriptor *descs =
        (CoeffPreprocessDescriptor *)xcalloc_raw(
            p->lines, sizeof(*descs), "coeff_stream_line_descriptors");
    uint64_t *tokens = (uint64_t *)xcalloc_raw(
        p->lines, sizeof(*tokens), "coeff_stream_line_tokens");
    uint64_t *line_status = (uint64_t *)xcalloc_raw(
        p->lines, sizeof(*line_status), "coeff_stream_line_status");
    BackwardLineContext *backward_ctx =
        (BackwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*backward_ctx), "coeff_stream_backward_ctx");
    int all_valid = 1;
    // In SE mode controller-side proxy writes do not trigger guest page
    // faults.  Touch every stable record before timing/launch so multi-line
    // arrays spanning fresh heap pages have valid translations.
    for (size_t index = 0; index < total_cells; ++index)
        records[index].status = COEFF_PRE_STATUS_BUSY;
    volatile unsigned char *record_pages =
        (volatile unsigned char *)records;
    const size_t record_bytes = total_cells * sizeof(*records);
    for (size_t offset = 0; offset < record_bytes; offset += 4096)
        record_pages[offset] = 0;
    if (record_bytes)
        record_pages[record_bytes - 1] = 0;
    for (size_t line = 0; line < p->lines; ++line)
        line_status[line] = COEFF_PRE_STATUS_BUSY;
    // Independent reference y/dq arrays survive this poisoning. Missing or
    // stale writes must fail even when successive sweeps use the same R.
    if (cfg->coeff_poison_outputs) {
        for (size_t i = 0; i < total_cells*N; ++i)
            dq_star[i] = dq[i] = NAN;
        if (coefficient_dirty)
            for (size_t i = 0; i < total_cells*N*N; ++i)
                p->lu_d[i] = p->u_bar[i] = NAN;
    }
    if (!cfg->coeff_publish_lu && coefficient_dirty)
        for (size_t i = 0; i < total_cells*N*N; ++i)
            p->lu_d[i] = NAN;
    report_lu_output_range(p, cfg, next_line_generation);
    if (cfg->coeff_local_ubar) {
        for (size_t i = 0; i < total_cells*N*N; ++i)
            p->u_bar[i] = NAN;
        printf("CFD_UBAR_OUTPUT_RANGE generation=%llu base=0x%llx bytes=%llu\n",
               (unsigned long long)next_line_generation,
               (unsigned long long)(uintptr_t)p->u_bar,
               (unsigned long long)matrix_bytes);
    }
    // File-backed runs decode the source directly to this layout. Fixtures
    // may change D or R between calls, including failed attempts and retries.
    if (cfg->coeff_sme_packed_input && !p->dataset_oracle_y)
        pack_sme_fixture_input(p, next_line_generation);
    // Reporting from the preceding sweep must reach the console before new
    // asynchronous controller traces; flushing is outside the measured solve.
    fflush(stdout);
    const uint64_t study_round = study_condition_cache(p, cfg);
    const uint64_t total_start = read_cycle_counter();

    for (size_t line = 0; line < p->lines; ++line) {
    CoeffPreprocessDescriptor *desc = &descs[line];
    const int direct_solve = !strcmp(cfg->coeff_base_solve, "direct");
    const int cache_enabled =
        cfg->coeff_line_cache_allowed &&
        (cfg->sweeps > 1 || cfg->coefficient_update_interval > 1);
    desc->version = 1;
    desc->size = sizeof(*desc);
    desc->flags = COEFF_PRE_COEFF3 | COEFF_PRE_STREAMING_PROGRESS |
        COEFF_PRE_STREAMING_AUTO_RETIRE | COEFF_PRE_DINV_BASE_CONSUMER |
        COEFF_PRE_DINV_DIRECT_BYPASS |
        COEFF_PRE_LBAR_FORWARD_CONSUMER |
        COEFF_PRE_LBAR_DIRECT_BYPASS |
        COEFF_PRE_LINE_AUTONOMOUS;
    if (compact_records)
        desc->flags |= COEFF_PRE_COMPACT_RECORD;
    if (packed_records)
        desc->flags |= COEFF_PRE_PACKED_RECORD;
    if (cfg->coeff_warm_summary)
        desc->flags |= COEFF_PRE_WARM_SUMMARY;
    if (direct_solve)
        desc->flags |= COEFF_PRE_BASE_DIRECT_SOLVE;
    if (!cfg->coeff_publish_lu)
        desc->flags |= COEFF_PRE_LOCAL_LU;
    if (cfg->coeff_local_ubar)
        desc->flags |= COEFF_PRE_LOCAL_UBAR;
    if (cache_enabled)
        desc->flags |= COEFF_PRE_LINE_COEFF_CACHE;
    if (cache_enabled && coefficient_dirty)
        desc->flags |= COEFF_PRE_LINE_COEFF_DIRTY;
    if (!strcmp(cfg->coeff_stream_ubar_consumer, "direct"))
        desc->flags |= COEFF_PRE_LINE_BACKWARD_DIRECT;
    if (cfg->coeff3_partial_output)
        desc->flags |= COEFF_PRE_PARTIAL_OUTPUT;
    if (cfg->lu_forwarding)
        desc->flags |= COEFF_PRE_LU_FORWARD;
    desc->generation = next_line_generation;
    next_line_generation += p->cells + 1;
    desc->line_id = line;
    // Stage 7 parent-line ABI: cell_id is the guest-published coefficient
    // version. Child descriptors still carry their ordinary cell index.
    desc->cell_id = cache_enabled ? coefficient_version : 0;
    desc->d_addr = (uint64_t)(uintptr_t)
        mat_at(p->d_mat, p, line, 0);
    desc->l_addr = (uint64_t)(uintptr_t)
        mat_at(p->l_mat, p, line, 0);
    desc->u_addr = (uint64_t)(uintptr_t)
        mat_at(p->u_mat, p, line, 0);
    desc->lu_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->lu_d, p, line, 0);
    // DInv direct mode suppresses the matrix drain, so this otherwise-unused
    // descriptor field carries the parent line completion word.
    desc->d_inv_out_addr = (uint64_t)(uintptr_t)&line_status[line];
    desc->l_bar_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->l_bar_precomputed, p, line, 0);
    desc->u_bar_out_addr = (uint64_t)(uintptr_t)
        mat_at(p->u_bar, p, line, 0);
    desc->record_addr = (uint64_t)(uintptr_t)&records[line * p->cells];
    if (compact_records)
        desc->record_addr = (uint64_t)(uintptr_t)records + line*p->cells*record_stride;
    desc->pivot_epsilon = cfg->pretransform_diag_epsilon;
    desc->reserved[0] = (uint64_t)(uintptr_t)
        vec_at(p->rhs, p, line, 0);
    if (cfg->coeff_sme_packed_input) {
        desc->flags |= COEFF_PRE_PACKED_COLD_INPUT;
        desc->d_addr = (uint64_t)(uintptr_t)p->sme_cold_input;
        desc->l_addr = desc->d_addr + N*N*sizeof(double);
        desc->u_addr = desc->d_addr + 2*N*N*sizeof(double);
        desc->reserved[0] = desc->d_addr + 3*N*N*sizeof(double);
    }
    desc->reserved[1] = cfg->coeff_stream_window;
    desc->reserved[2] = (uint64_t)(uintptr_t)
        vec_at(dq, p, line, 0);
    desc->reserved[3] = (uint64_t)(uintptr_t)
        vec_at(dq_star, p, line, 0);
    desc->reserved[4] = p->cells;
    }

    stats->streaming_enabled = 1;
    stats->streaming_window_depth = cfg->coeff_stream_window;
    stats->line_context_forwarding_enabled = 1;
    stats->dinv_column_consumer_enabled = 1;
    stats->dinv_column_consumer_direct_runs = 1;
    stats->lbar_column_consumer_enabled = 1;
    stats->lbar_column_consumer_direct_runs = 1;
    if (strcmp(cfg->coeff_base_solve, "direct")) {
        stats->raw_input_d_bytes += matrix_bytes;
        stats->raw_input_l_bytes += matrix_bytes;
        stats->raw_input_u_bytes += matrix_bytes;
    } else if (coefficient_dirty) {
        stats->raw_input_d_bytes += matrix_bytes;
        stats->raw_input_l_bytes += (p->cells - 1) * p->lines * 200;
        stats->raw_input_u_bytes += (p->cells - 1) * p->lines * 200;
    }
    stats->raw_input_r_bytes += rhs_bytes;
    if (coefficient_dirty) {
        stats->coefficient_rebuilds++;
        if (strcmp(cfg->coeff_base_solve, "direct"))
            stats->streaming_requested_dinv_batches += total_cells;
        if (p->cells > 1) {
            stats->streaming_requested_lbar_batches +=
                p->lines * (p->cells - 1);
            stats->streaming_requested_ubar_batches +=
                p->lines * (p->cells - 1);
        }
        stats->streaming_boundary_skipped_lbar_rhs += 5 * p->lines;
        stats->streaming_boundary_skipped_ubar_rhs += 5 * p->lines;
        stats->streaming_avoided_lbar_drains += p->lines;
        stats->streaming_avoided_ubar_drains += p->lines;
        stats->streaming_effective_rhs_count += p->lines *
            ((!strcmp(cfg->coeff_base_solve, "direct") ? 1 : 5) * p->cells +
             10 * (p->cells > 1 ? p->cells - 1 : 0));
    } else {
        stats->coefficient_reuses++;
        if (!strcmp(cfg->coeff_base_solve, "direct")) {
            stats->streaming_effective_rhs_count += total_cells;
            stats->coeff3_columns_solved += total_cells;
            stats->coeff3_issued += total_cells;
            stats->coeff3_completed += total_cells;
            stats->base_direct_rhs_solved += total_cells;
            stats->base_lu_cache_hits += total_cells;
        }
    }

    stats->streaming_first_input_issue_cycle = read_cycle_counter();
    for (size_t line = 0; line < p->lines; ++line) {
        tokens[line] = coeff_preprocess_launch(&descs[line]);
        if (!tokens[line])
            all_valid = 0;
    }
    if (all_valid) {
        uint64_t polls = 0;
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
            int all_terminal = 1;
            for (size_t line = 0; line < p->lines; ++line) {
                if (line_status[line] == COEFF_PRE_STATUS_BUSY) {
                    all_terminal = 0;
                } else if (line_status[line] !=
                           COEFF_PRE_STATUS_COMPLETE) {
                    all_valid = 0;
                }
            }
            if (all_terminal)
                break;
            polls++;
            if (cfg->coeff_cancel_after_polls && polls == cfg->coeff_cancel_after_polls) {
                uint64_t result = coeff_preprocess_cancel(tokens[0]);
                printf("CFD_RECOVERY_CANCEL polls=%lu result=%lu\n",(unsigned long)polls,(unsigned long)result);
            }
        }
        stats->progress_poll_calls += polls + 1;
        stats->progress_poll_busy += polls;
        stats->progress_poll_useful++;
        stats->guest_completion_record_checks += polls + 1;
        for (size_t line = 0; line < p->lines; ++line) {
            uint64_t terminal;
            do {
                stats->progress_wait_calls++;
                terminal = coeff_preprocess_wait(tokens[line]);
            } while (terminal == COEFF_PRE_STATUS_BUSY);
            if (terminal != COEFF_PRE_STATUS_COMPLETE)
                all_valid = 0;
        }
    }

    if (strcmp(cfg->coeff_stream_ubar_consumer, "direct"))
        stats->streaming_forward_finish_cycle = read_cycle_counter();
    if (coefficient_dirty || !strcmp(cfg->coeff_base_solve, "direct")) {
        stats->event_requests_allocated += total_cells;
        stats->event_requests_freed += total_cells;
    }
    stats->terminal_wait_calls_avoided += total_cells;
    stats->completion_queue_scans_avoided += total_cells;
    stats->completion_queue_entries_avoided +=
        p->lines * (cfg->coeff_stream_window +
                    cfg->coeff_output_buffer_depth);
    stats->streaming_forward_cells_consumed += total_cells;
    stats->forward_cells += total_cells;
    stats->pretransform_forward_cells += total_cells;
    stats->pretransform_hot_loop_trsv5_elided += total_cells;
    stats->patha_dinv_mvm_eliminated += total_cells;
    stats->patha_dinv_custom_instructions_eliminated += 12 * total_cells;
    stats->dinv_guest_reload_bytes_avoided += matrix_bytes;

    if (all_valid && strcmp(cfg->coeff_stream_ubar_consumer, "direct")) {
        stats->streaming_backward_start_cycle = read_cycle_counter();
        pretransform_backward_sweep(p, cfg, stats, dq_star, dq, 1,
                                    backward_ctx, p->u_bar);
        stats->streaming_backward_finish_cycle = read_cycle_counter();
    } else if (all_valid) {
        stats->backward_cells += total_cells;
        stats->pretransform_backward_cells += total_cells;
        stats->streaming_backward_finish_cycle = read_cycle_counter();
    } else {
        stats->common_preprocess_fallback_runs++;
        memcpy(dq_star, study_cache_enabled(cfg) ? p->dataset_oracle_y : p->dq_star_raw_ref, rhs_bytes);
        memcpy(dq, study_cache_enabled(cfg) ? p->dataset_oracle_dq : p->dq_raw_ref, rhs_bytes);
    }

    const uint64_t compute_finish = read_cycle_counter();
    stats->streaming_total_cycles += compute_finish - total_start;
    if (study_round)
        printf("CFD_STUDY_ACCEL_ROI round=%llu start=%llu finish=%llu cycles=%llu "
            "coefficients=%s\n", (unsigned long long)study_round,
            (unsigned long long)total_start, (unsigned long long)compute_finish,
            (unsigned long long)(compute_finish-total_start),
            coefficient_dirty ? "rebuild" : "reuse");
    if (compact_records)
        printf("CFD_RECORD_FORMAT bytes=%lu full_diagnostics=0 diagnostics_source=trace\n",
               (unsigned long)record_stride);
    // Validate both wavefronts on every sweep, outside its timing window.
    // Direct solve and inverse-MVM have different rounding, so compare the
    // entire output to the software raw solve using the existing tolerances.
    if (cfg->check && all_valid) {
        double max_abs = 0.0, max_rel = 0.0;
        uint64_t mismatches = 0;
        uint64_t result_hash = 14695981039346656037ULL;
        const double *got[] = {dq_star, dq};
        const double *reference[] = {
            study_cache_enabled(cfg) ? p->dataset_oracle_y : p->dq_star_raw_ref,
            study_cache_enabled(cfg) ? p->dataset_oracle_dq : p->dq_raw_ref};
        for (int phase = 0; phase < 2; ++phase) {
            for (size_t index = 0; index < total_cells * N; ++index) {
                uint64_t bits;
                memcpy(&bits, &got[phase][index], sizeof(bits));
                result_hash = (result_hash ^ bits) * 1099511628211ULL;
                double abs_error = fabs(got[phase][index] -
                                        reference[phase][index]);
                double rel_error = abs_error /
                    fmax(fabs(reference[phase][index]), 1.0e-14);
                if (abs_error > max_abs) max_abs = abs_error;
                if (rel_error > max_rel) max_rel = rel_error;
                if (!isfinite(got[phase][index]) || !isfinite(reference[phase][index]) ||
                    (abs_error > 1.0e-10 && rel_error > 1.0e-10))
                    mismatches++;
            }
        }
        const double residual = raw_recurrence_residual(p,dq_star,dq);
        if (!isfinite(residual) || residual > 1e-10) mismatches++;
        printf("CFD_SINGLE_LINE_NUMERIC version=%lu base_solve=%s "
               "values=%lu max_abs=%.17g max_rel=%.17g mismatches=%lu "
               "result_hash=%016lx residual_max=%.17g\n",
               (unsigned long)coefficient_version, cfg->coeff_base_solve,
               (unsigned long)(2 * total_cells * N), max_abs, max_rel,
               (unsigned long)mismatches, (unsigned long)result_hash, residual);
        all_valid &= mismatches == 0;
        all_valid &= validate_dataset_oracle(p,dq_star,dq,"accelerator");
    }
    // Stable-record verification and aggregation are reporting work.  Keep
    // them outside the measured solve so the controller-to-backward handoff
    // is not separated by per-cell guest bookkeeping.
    // A mode/size/version miss can rebuild despite dirty=0. The terminal
    // record timestamp also identifies that rebuild; warm inverse hits leave
    // the zero-initialized record buffer untouched. This is outside timing.
    const CoeffLineSummary *summary = (const CoeffLineSummary *)records;
    const int summary_record = cfg->coeff_warm_summary && summary->magic == COEFF_LINE_SUMMARY_MAGIC;
    if (summary_record) {
        const int summary_valid = summary->cells == total_cells && summary->completed == total_cells &&
            summary->errors == 0 && summary->generation == descs[0].generation &&
            summary->first_issue <= summary->last_complete;
        all_valid &= summary_valid;
        if (summary_valid) {
            stats->summary_completed_cells += summary->completed;
            stats->summary_completed_lines += p->lines;
            stats->event_requests_completed += summary->completed;
            stats->auto_retired_requests += summary->completed;
            stats->auto_retired_tokens += summary->completed;
        }
        printf("CFD_RECORD_SUMMARY_GUEST generation=%lu cells=%lu completed=%lu errors=%lu hash=%016lx\n",
            (unsigned long)summary->generation,(unsigned long)summary->cells,
            (unsigned long)summary->completed,(unsigned long)summary->errors,(unsigned long)summary->digest);
    }
    const int generated_records = !summary_record && (coefficient_dirty ||
        !strcmp(cfg->coeff_base_solve, "direct") ||
        (packed_records && ((const CoeffPackedRecord *)records)->complete_cycle));
    for (size_t line = 0; generated_records && line < p->lines; ++line) {
      for (size_t cell = 0; cell < p->cells; ++cell) {
        CoeffPreprocessRecord expanded = {};
        const CoeffPreprocessRecord *record =
            &records[line * p->cells + cell];
        if (packed_records) {
            const CoeffPackedRecord *packed = (const CoeffPackedRecord *)
                ((const unsigned char *)records + (line*p->cells+cell)*record_stride);
#define CFD_PACKED_FIELD(type, cpp, guest) expanded.guest = packed->guest;
#include "../../../../src/arch/arm/cfd_packed_record_fields.inc"
#undef CFD_PACKED_FIELD
            record = &expanded;
            uint64_t digest=14695981039346656037ULL;
#define CFD_RECORD_FIELD(cpp, guest) digest=(digest^expanded.guest)*1099511628211ULL;
#include "../../../../src/arch/arm/cfd_compact_record_fields.inc"
#undef CFD_RECORD_FIELD
            printf("CFD_RECORD_DIGEST_GUEST request=%lu generation=%lu hash=%016lx\n",
                (unsigned long)expanded.request_id,(unsigned long)expanded.generation,
                (unsigned long)digest);
        } else if (compact_records) {
            const CoeffCompactRecord *compact = (const CoeffCompactRecord *)
                ((const unsigned char *)records + (line*p->cells+cell)*record_stride);
#define CFD_RECORD_FIELD(cpp, guest) expanded.guest = compact->guest;
#include "../../../../src/arch/arm/cfd_compact_record_fields.inc"
#undef CFD_RECORD_FIELD
            record = &expanded;
        }
        if (record->status != COEFF_PRE_STATUS_COMPLETE ||
            !record->dq_star_ready || record->forward_consumer_error ||
            record->generation != descs[line].generation + cell ||
            record->line_id != line || record->cell_id != cell) {
            fprintf(stderr, "CFD_RECORD_INVALID cell=%lu status=%lu dq_ready=%lu "
                    "generation=%lu expected=%lu line=%lu actual_cell=%lu error=%lu\n",
                    (unsigned long)cell, (unsigned long)record->status,
                    (unsigned long)record->dq_star_ready, (unsigned long)record->generation,
                    (unsigned long)(descs[line].generation+cell), (unsigned long)record->line_id,
                    (unsigned long)record->cell_id, (unsigned long)record->forward_consumer_error);
            all_valid = 0;
        }
        if (!strcmp(cfg->coeff_base_solve, "direct") &&
            (!record->base_accumulators_completed ||
             !record->base_compute_ready_cycle ||
             record->d_inv_columns_ready || record->d_inv_columns_consumed ||
             record->d_inv_ready_cycle || record->base_consumed_column_mask)) {
            fprintf(stderr, "CFD_DIRECT_RECORD_INVALID cell=%lu base_done=%lu "
                    "base_cycle=%lu inv_ready=%lu inv_consumed=%lu inv_cycle=%lu mask=%lu\n",
                    (unsigned long)cell, (unsigned long)record->base_accumulators_completed,
                    (unsigned long)record->base_compute_ready_cycle,
                    (unsigned long)record->d_inv_columns_ready, (unsigned long)record->d_inv_columns_consumed,
                    (unsigned long)record->d_inv_ready_cycle, (unsigned long)record->base_consumed_column_mask);
            all_valid = 0;
        }
        StreamingCompletionEntry entry = {};
        entry.line_id = line;
        entry.cell_id = cell;
        entry.generation = descs[line].generation + cell;
        entry.record = *record;
        aggregate_coeff_event_record(stats, record);
        record_streaming_timestamps(stats, &entry);
        stats->auto_retired_requests += record->auto_retired;
        stats->auto_retired_tokens += record->auto_retired;
        stats->auto_retire_errors += record->auto_retire_error;
        stats->streaming_first_forward_issue_cycle = first_nonzero(
            stats->streaming_first_forward_issue_cycle,
            record->rhs_vector_issue_cycle);
        if (!strcmp(cfg->coeff_stream_ubar_consumer, "direct")) {
            if (record->dq_star_publish_cycle >
                stats->streaming_forward_finish_cycle)
                stats->streaming_forward_finish_cycle =
                    record->dq_star_publish_cycle;
            if (record->complete_cycle >
                stats->streaming_backward_start_cycle)
                stats->streaming_backward_start_cycle =
                    record->complete_cycle;
        }
      }
    }
    if (!strcmp(cfg->coeff_stream_ubar_consumer, "direct") &&
        stats->streaming_backward_finish_cycle >=
            stats->streaming_backward_start_cycle)
        stats->backward_cycles =
            stats->streaming_backward_finish_cycle -
            stats->streaming_backward_start_cycle;
    finalize_streaming_timeline(stats);
    if (!cfg->coeff_publish_lu)
        all_valid &= validate_local_lu_poison(p);
    if (cfg->coeff_local_ubar)
        all_valid &= validate_local_matrix_poison(p, p->u_bar, "UBAR");
    if (all_valid && coefficient_dirty)
        all_valid &= validate_streaming_coefficients(
            p, cfg, stats, check, NULL);
    stats->lu5_factor_cycles += stats->event_lu_active_cycles;
    stats->common_lu_cycles += stats->event_lu_active_cycles;
    stats->ubar_cycles += stats->event_solve_active_cycles;
    stats->common_ubar_cycles += stats->event_solve_active_cycles;
    stats->coeff3_cycles += stats->event_solve_active_cycles;
    stats->raw_persistent_coefficient_bytes = 4U * matrix_bytes;
    stats->raw_extra_coefficient_bytes = 2U * matrix_bytes;
    if (stats->streaming_preprocess_finish_cycle >=
        stats->streaming_first_input_issue_cycle)
        stats->extra_pretransform_cycles =
            stats->streaming_preprocess_finish_cycle -
            stats->streaming_first_input_issue_cycle;
    stats->preprocess_cycles = stats->extra_pretransform_cycles;
    stats->pretransform_raw_runtime_cycles =
        stats->forward_cycles + stats->backward_cycles;
    stats->pretransform_raw_total_cycles = stats->streaming_total_cycles;
    stats->total_cycles = stats->streaming_total_cycles;
    free(backward_ctx);
    free(line_status);
    free(tokens);
    free(descs);
    free(records);
    fflush(stdout);
    return all_valid;
}

// Functional ABI transition test. A publication-policy change with the same
// coefficient version must not reuse an entry promising different outputs.
// This is not a performance experiment; expected cache decisions are checked
// independently against the controller trace by the host acceptance tool.
static int
run_lu_output_transition_test(const Config *original)
{
    const int published[] = {0, 0, 1, 1, 0};
    const int expected_hit[] = {0, 1, 0, 1, 0};
    Config cfg = *original;
    cfg.lines = cfg.interleave = 1;
    cfg.sweeps = cfg.coefficient_update_interval = 2;
    cfg.coeff_warm_summary = 0;
    cfg.coeff_poison_outputs = 1;
    cfg.coeff_cancel_after_polls = 0;
    Problem p;
    init_problem(&p, 1, cfg.cells);
    apply_raw_input_pattern(&p, &cfg);
    load_step2_dataset(&p, &cfg);
    Stats reference = {};
    int valid = raw_reference_solve(&p, &reference);
    if (valid)
        valid &= validate_dataset_oracle(
            &p, p.dq_star_raw_ref, p.dq_raw_ref, "cpu");
    for (unsigned step = 0; valid && step < 5; ++step) {
        cfg.coeff_publish_lu = published[step];
        if (!cfg.coeff_publish_lu)
            for (size_t i = 0; i < p.cells*N*N; ++i)
                p.lu_d[i] = NAN;
        Stats stats = {};
        PretransformCheck check = {};
        int ok = run_pretransform_raw_stream_line_once(&p, &cfg, &stats,
            p.dq_star_pretransform_raw_stream, p.dq_pretransform_raw_stream,
            &check, 1, step == 0);
        int output_ok = cfg.coeff_publish_lu ? 1 : validate_local_lu_poison(&p);
        if (cfg.coeff_publish_lu)
            for (size_t cell = 0; cell < p.cells; ++cell) {
                const double *lu = mat_const_at(p.lu_d, &p, 0, cell);
                for (unsigned i = 0; i < N*N; ++i)
                    output_ok &= isfinite(lu[i]);
                output_ok &= lu_factor_residual_max(
                    mat_const_at(p.d_mat, &p, 0, cell), lu) <= 1e-9;
            }
        printf("CFD_LU_OUTPUT_TRANSITION step=%u cells=%lu version=1 publish=%d "
               "expected_cache_hit=%d numeric_pass=%d output_contract_pass=%d cycles=%lu\n",
               step, (unsigned long)cfg.cells, published[step],
               expected_hit[step], ok, output_ok,
               (unsigned long)stats.streaming_total_cycles);
        valid &= ok && output_ok;
        fflush(stdout);
    }
    free_problem(&p);
    puts(valid ? "LUSGS_LU_OUTPUT_TRANSITION_PASS" :
                 "LUSGS_LU_OUTPUT_TRANSITION_FAIL");
    return valid;
}

static int
run_pretransform_raw_stream_line(Problem *p, const Config *cfg, Stats *stats,
                                 double *dq_star, double *dq,
                                 PretransformCheck *check);

static int
run_pretransform_raw_stream_auto(Problem *p, const Config *cfg, Stats *stats,
                                 double *dq_star, double *dq,
                                 PretransformCheck *check)
{
    if (cfg->coeff_stream_line_autonomous)
        return run_pretransform_raw_stream_line(
            p, cfg, stats, dq_star, dq, check);
    static uint64_t next_stream_generation = 0x100000000ULL;
    const uint64_t depth = cfg->coeff_stream_window;
    StreamingCellState *window = (StreamingCellState *)xcalloc_raw(
        depth, sizeof(*window), "coeff_stream_auto_window");
    // Stable guest-owned records are indexed by cell.  This is benchmark
    // memory, not an unbounded controller/request queue.
    StreamingCompletionEntry *records =
        (StreamingCompletionEntry *)xcalloc_raw(
            p->cells, sizeof(*records), "coeff_stream_auto_records");
    double *base_vectors = xcalloc(
        p->cells * N, sizeof(double), "coeff_stream_base_vectors");
    double *shadow_corrections = xcalloc(
        p->cells * N, sizeof(double), "coeff_stream_shadow_corrections");
    double *controller_dq_star = xcalloc(
        p->cells * N, sizeof(double), "coeff_stream_controller_dq_star");
    ForwardLineContext forward_ctx = {};
    BackwardLineContext backward_ctx = {};
    size_t next_cell = 0;
    uint64_t live = 0, issue_sequence = 1;
    int all_valid = 1;
    const uint64_t matrix_bytes = p->cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = p->cells * N * sizeof(double);
    const uint64_t total_start = read_cycle_counter();

    stats->streaming_enabled = 1;
    stats->streaming_window_depth = depth;
    stats->line_context_forwarding_enabled = 1;
    stats->raw_input_d_bytes += matrix_bytes;
    stats->raw_input_l_bytes += matrix_bytes;
    stats->raw_input_u_bytes += matrix_bytes;
    stats->raw_input_r_bytes += rhs_bytes;
    stats->coefficient_rebuilds++;
    if (strcmp(cfg->coeff_stream_dinv_consumer, "off")) {
        stats->dinv_column_consumer_enabled = 1;
        if (!strcmp(cfg->coeff_stream_dinv_consumer, "shadow"))
            stats->dinv_column_consumer_shadow_runs++;
        else
            stats->dinv_column_consumer_direct_runs++;
    }
    if (strcmp(cfg->coeff_stream_lbar_consumer, "off")) {
        stats->lbar_column_consumer_enabled = 1;
        if (!strcmp(cfg->coeff_stream_lbar_consumer, "shadow"))
            stats->lbar_column_consumer_shadow_runs++;
        else
            stats->lbar_column_consumer_direct_runs++;
    }
    stats->streaming_requested_dinv_batches += p->cells;
    if (cfg->coeff_stream_boundary_mask) {
        if (p->cells > 1) {
            stats->streaming_requested_lbar_batches += p->cells - 1;
            stats->streaming_requested_ubar_batches += p->cells - 1;
        }
        stats->streaming_boundary_skipped_lbar_rhs += 5;
        stats->streaming_boundary_skipped_ubar_rhs += 5;
        stats->streaming_avoided_lbar_drains++;
        stats->streaming_avoided_ubar_drains++;
        stats->streaming_effective_rhs_count +=
            5 * (p->cells + (p->cells > 1 ? 2 * (p->cells - 1) : 0));
    } else {
        stats->streaming_requested_lbar_batches += p->cells;
        stats->streaming_requested_ubar_batches += p->cells;
        stats->streaming_effective_rhs_count += 15 * p->cells;
    }
    stats->terminal_wait_calls_avoided += p->cells;
    stats->completion_queue_scans_avoided += p->cells;
    stats->completion_queue_entries_avoided +=
        depth + cfg->coeff_output_buffer_depth;

    while (next_cell < p->cells && live < depth) {
        StreamingCellState *state = &window[live];
        if (!launch_streaming_coeff_cell(
                p, cfg, state, &records[next_cell],
                &base_vectors[next_cell * N],
                &shadow_corrections[next_cell * N],
                !strcmp(cfg->coeff_stream_lbar_consumer, "direct") ?
                    vec_at(dq_star, p, 0, next_cell) :
                    &controller_dq_star[next_cell * N], next_cell,
                next_stream_generation++, issue_sequence++)) {
            all_valid = 0;
            break;
        }
        stats->event_requests_allocated++;
        live++;
        next_cell++;
        sample_streaming_window(stats, live);
        if (live > stats->event_requests_max_live)
            stats->event_requests_max_live = live;
    }

    for (size_t frontier = 0; all_valid && frontier < p->cells; ++frontier) {
        StreamingCellState *state = &window[frontier % depth];
        StreamingCompletionEntry *entry = &records[frontier];
        if (state->completion != entry || state->cell_id != frontier ||
            state->generation != entry->generation) {
            stats->streaming_unsafe_slot_reuse_attempts++;
            all_valid = 0;
            break;
        }
        if (!wait_auto_forward_record(entry, cfg, stats)) {
            all_valid = 0;
            break;
        }
        if (!strcmp(cfg->coeff_stream_dinv_consumer, "shadow")) {
            double patha_base[N];
            patha_mvm5_existing(
                mat_const_at(p->d_inv, p, 0, frontier),
                vec_const_at(p->rhs, p, 0, frontier), patha_base);
            if (!compare_shadow_base(stats, frontier, patha_base,
                                     &base_vectors[frontier * N])) {
                all_valid = 0;
                break;
            }
        }
        uint64_t forward_start = read_cycle_counter();
        double guest_correction[N] = {};
        if (!stats->streaming_first_forward_issue_cycle)
            stats->streaming_first_forward_issue_cycle = forward_start;
        if (!strcmp(cfg->coeff_stream_lbar_consumer, "direct")) {
            // The timed DqStarVector drain is the sole guest-visible forward
            // result.  No DInv/Lbar Path A instruction is retained here.
            stats->forward_cells++;
            stats->pretransform_forward_cells++;
            stats->pretransform_hot_loop_trsv5_elided++;
            stats->patha_dinv_mvm_eliminated++;
            stats->patha_dinv_custom_instructions_eliminated += 12;
            stats->dinv_guest_reload_bytes_avoided += N * N * sizeof(double);
        } else if (!strcmp(cfg->coeff_stream_dinv_consumer, "direct"))
            pretransform_forward_cell_from_base(
                p, stats, dq_star, 0, frontier, &forward_ctx,
                &base_vectors[frontier * N], guest_correction);
        else
            pretransform_forward_cell(p, stats, dq_star, 0, frontier,
                                      &forward_ctx);
        if (!strcmp(cfg->coeff_stream_lbar_consumer, "shadow")) {
            if (!compare_shadow_forward_vector(
                    &stats->shadow_correction_bitwise_mismatch,
                    &stats->shadow_correction_mismatch_cell,
                    &stats->shadow_correction_mismatch_lane,
                    &stats->shadow_correction_max_abs_error,
                    &stats->shadow_correction_max_rel_error, frontier,
                    guest_correction,
                    &shadow_corrections[frontier * N]) ||
                !compare_shadow_forward_vector(
                    &stats->shadow_dqstar_bitwise_mismatch,
                    &stats->shadow_dqstar_mismatch_cell,
                    &stats->shadow_dqstar_mismatch_lane,
                    &stats->shadow_dqstar_max_abs_error,
                    &stats->shadow_dqstar_max_rel_error, frontier,
                    vec_const_at(dq_star, p, 0, frontier),
                    &controller_dq_star[frontier * N])) {
                all_valid = 0;
                break;
            }
        }
        uint64_t forward_end = read_cycle_counter();
        if (forward_end >= forward_start)
            stats->forward_cycles += forward_end - forward_start;
        entry->forward_consumed = 1;
        state->forward_consumed = 1;
        stats->streaming_forward_cells_consumed++;
        live--;

        if (next_cell < p->cells) {
            uint64_t generation = next_stream_generation++;
            while (!launch_streaming_coeff_cell(
                       p, cfg, state, &records[next_cell],
                       &base_vectors[next_cell * N],
                       &shadow_corrections[next_cell * N],
                       !strcmp(cfg->coeff_stream_lbar_consumer, "direct") ?
                           vec_at(dq_star, p, 0, next_cell) :
                           &controller_dq_star[next_cell * N], next_cell,
                       generation, issue_sequence)) {
                // Launch itself is the bounded-controller backpressure event;
                // no completion queue scan or WAIT instruction is issued.
                stats->streaming_request_window_full_stall_cycles++;
            }
            issue_sequence++;
            stats->event_requests_allocated++;
            stats->streaming_request_slots_reused++;
            live++;
            next_cell++;
            sample_streaming_window(stats, live);
            if (live > stats->event_requests_max_live)
                stats->event_requests_max_live = live;
        }
    }

    stats->streaming_forward_finish_cycle = read_cycle_counter();
    for (size_t cell = 0; all_valid && cell < p->cells; ++cell) {
        if (!wait_auto_terminal_record(&records[cell], stats)) {
            all_valid = 0;
            break;
        }
        if (strcmp(cfg->coeff_stream_dinv_consumer, "off") &&
            (records[cell].record.base_consumed_column_mask != 0x1f ||
             records[cell].record.d_inv_columns_consumed != N ||
             (strcmp(cfg->coeff_stream_lbar_consumer, "direct") &&
              !records[cell].record.base_ready) ||
             records[cell].record.base_consumer_error)) {
            stats->base_accumulator_missing_columns++;
            all_valid = 0;
            break;
        }
        if (!strcmp(cfg->coeff_stream_dinv_consumer, "direct") &&
            records[cell].record.d_inv_drain_done) {
            stats->auto_retire_errors++;
            all_valid = 0;
            break;
        }
        if (strcmp(cfg->coeff_stream_lbar_consumer, "off")) {
            const int head = cell == 0;
            const CoeffPreprocessRecord *r = &records[cell].record;
            if (!r->dq_star_ready || r->forward_consumer_error ||
                (!head && (r->lbar_consumed_column_mask != 0x1f ||
                 r->lbar_columns_consumed != N ||
                 !r->correction_accumulators_completed ||
                 !r->forward_combine_completed))) {
                stats->correction_accumulator_missing_columns++;
                all_valid = 0;
                break;
            }
            if (!strcmp(cfg->coeff_stream_lbar_consumer, "direct") &&
                r->l_bar_drain_done) {
                stats->auto_retire_errors++;
                all_valid = 0;
                break;
            }
        }
        if (records[cell].record.complete_cycle <=
            stats->streaming_forward_finish_cycle)
            stats->auto_retire_before_forward_finish++;
    }

    if (!all_valid) {
        for (size_t cell = 0; cell < p->cells; ++cell) {
            if (!records[cell].token || records[cell].record.complete_cycle)
                continue;
            uint64_t status = coeff_preprocess_cancel(records[cell].token);
            if (status == COEFF_PRE_STATUS_BAD_TOKEN)
                stats->stale_token_waits++;
        }
        stats->common_preprocess_fallback_runs++;
        memcpy(dq_star, p->dq_star_raw_ref, p->cells * N * sizeof(double));
        memcpy(dq, p->dq_raw_ref, p->cells * N * sizeof(double));
        free(base_vectors);
        free(shadow_corrections);
        free(controller_dq_star);
        free(records);
        free(window);
        return 0;
    }

    stats->streaming_backward_start_cycle = read_cycle_counter();
    stats->auto_retire_before_backward_start = stats->auto_retired_requests;
    pretransform_backward_sweep(p, cfg, stats, dq_star, dq, 1,
                                &backward_ctx, p->u_bar);
    stats->streaming_backward_finish_cycle = read_cycle_counter();
    if (stats->streaming_backward_start_cycle >=
        stats->streaming_forward_finish_cycle)
        stats->streaming_forward_backward_turnaround_stall_cycles =
            stats->streaming_backward_start_cycle -
            stats->streaming_forward_finish_cycle;

    uint64_t compute_finish = read_cycle_counter();
    stats->streaming_total_cycles = compute_finish - total_start;
    // Full record aggregation is reporting-only.  Keep it after the measured
    // solve so backward is gated only by the terminal/error release check.
    uint64_t bookkeeping_start = read_cycle_counter();
    for (size_t cell = 0; cell < p->cells; ++cell) {
        aggregate_coeff_event_record(stats, &records[cell].record);
        record_streaming_timestamps(stats, &records[cell]);
    }
    finalize_streaming_timeline(stats);
    uint64_t bookkeeping_end = read_cycle_counter();
    if (bookkeeping_end >= bookkeeping_start)
        stats->guest_bookkeeping_cycles +=
            bookkeeping_end - bookkeeping_start;
    all_valid &= validate_streaming_coefficients(
        p, cfg, stats, check, base_vectors);
    stats->lu5_factor_cycles += stats->event_lu_active_cycles;
    stats->common_lu_cycles += stats->event_lu_active_cycles;
    stats->ubar_cycles += stats->event_solve_active_cycles;
    stats->common_ubar_cycles += stats->event_solve_active_cycles;
    stats->coeff3_cycles += stats->event_solve_active_cycles;
    stats->raw_persistent_coefficient_bytes = 4U * matrix_bytes;
    stats->raw_extra_coefficient_bytes = 2U * matrix_bytes;
    if (stats->streaming_preprocess_finish_cycle >=
        stats->streaming_first_input_issue_cycle)
        stats->extra_pretransform_cycles =
            stats->streaming_preprocess_finish_cycle -
            stats->streaming_first_input_issue_cycle;
    stats->preprocess_cycles = stats->extra_pretransform_cycles;
    stats->pretransform_raw_runtime_cycles =
        stats->forward_cycles + stats->backward_cycles;
    stats->pretransform_raw_total_cycles = stats->streaming_total_cycles;
    stats->total_cycles = stats->streaming_total_cycles;
    free(base_vectors);
    free(shadow_corrections);
    free(controller_dq_star);
    free(records);
    free(window);
    return all_valid;
}

static int
run_pretransform_raw_stream_line(Problem *p, const Config *cfg, Stats *stats,
                                 double *dq_star, double *dq,
                                 PretransformCheck *check)
{
    // The benchmark/algorithm is the sole publisher of stability. interval K
    // means D/L/U are immutable for K consecutive sweeps. Zero preserves the
    // safe default: publish DIRTY and rebuild on every sweep.
    const uint64_t interval = cfg->coefficient_update_interval ?
        cfg->coefficient_update_interval : 1;
    int all_valid = 1;
    for (uint64_t sweep = 0; sweep < cfg->sweeps; ++sweep) {
        const uint64_t version = 1 + sweep / interval;
        const int dirty = (sweep % interval) == 0;
        all_valid &= run_pretransform_raw_stream_line_once(
            p, cfg, stats, dq_star, dq, check, version, dirty);
        if (!all_valid)
            break;
    }
    stats->coefficient_update_interval = interval;
    return all_valid;
}

static int
run_pretransform_raw_stream(Problem *p, const Config *cfg, Stats *stats,
                            double *dq_star, double *dq,
                            PretransformCheck *check)
{
    if (!strcmp(cfg->coeff_stream_retire_mode, "auto"))
        return run_pretransform_raw_stream_auto(
            p, cfg, stats, dq_star, dq, check);
    static uint64_t next_stream_generation = 1;
    const uint64_t depth = cfg->coeff_stream_window;
    const uint64_t completion_capacity =
        depth + cfg->coeff_output_buffer_depth;
    StreamingCellState *window = (StreamingCellState *)xcalloc_raw(
        depth, sizeof(*window), "coeff_stream_window");
    StreamingCompletionEntry *completions =
        (StreamingCompletionEntry *)xcalloc_raw(
            completion_capacity, sizeof(*completions),
            "coeff_stream_completion_queue");
    ForwardLineContext forward_ctx = {};
    BackwardLineContext backward_ctx = {};
    uint64_t head = 0;
    uint64_t live = 0;
    uint64_t completion_alloc_cursor = 0;
    uint64_t completion_poll_cursor = 0;
    uint64_t issue_sequence = 1;
    size_t next_cell = 0;
    int all_valid = 1;
    const uint64_t matrix_bytes = p->cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = p->cells * N * sizeof(double);
    const uint64_t total_start = read_cycle_counter();

    stats->streaming_enabled = 1;
    stats->streaming_window_depth = depth;
    stats->line_context_forwarding_enabled = 1;
    stats->raw_input_d_bytes += matrix_bytes;
    stats->raw_input_l_bytes += matrix_bytes;
    stats->raw_input_u_bytes += matrix_bytes;
    stats->raw_input_r_bytes += rhs_bytes;
    stats->coefficient_rebuilds++;
    stats->streaming_requested_dinv_batches += p->cells;
    if (p->cells > 1) {
        stats->streaming_requested_lbar_batches += p->cells - 1;
        stats->streaming_requested_ubar_batches += p->cells - 1;
    }
    stats->streaming_boundary_skipped_lbar_rhs += 5;
    stats->streaming_boundary_skipped_ubar_rhs += 5;
    stats->streaming_avoided_lbar_drains++;
    stats->streaming_avoided_ubar_drains++;
    stats->streaming_effective_rhs_count +=
        5 * (p->cells + (p->cells > 1 ? 2 * (p->cells - 1) : 0));

    while (next_cell < p->cells && live < depth) {
        StreamingCellState *state = &window[(head + live) % depth];
        StreamingCompletionEntry *completion =
            allocate_streaming_completion(
                completions, completion_capacity, &completion_alloc_cursor,
                stats);
        if (!completion) {
            all_valid = 0;
            break;
        }
        if (!launch_streaming_coeff_cell(
                p, cfg, state, completion, NULL, NULL, NULL, next_cell,
                next_stream_generation++, issue_sequence++)) {
            all_valid = 0;
            break;
        }
        stats->event_requests_allocated++;
        sample_streaming_reaper(stats, completions, completion_capacity);
        live++;
        uint64_t outstanding = streaming_reaper_occupancy(
            completions, completion_capacity);
        if (outstanding > stats->event_requests_max_live)
            stats->event_requests_max_live = outstanding;
        next_cell++;
        sample_streaming_window(stats, live);
        // One non-blocking frontier probe per launch keeps progress and reap
        // ownership separate without waiting on the oldest token.  A Busy
        // result simply leaves the bounded completion entry for later polls.
        StreamingCompletionEntry *front_completion =
            window[head].completion;
        int initial_poll = poll_streaming_entry(
            completions, completion_capacity, front_completion, stats);
        if (initial_poll < 0) {
            all_valid = 0;
            break;
        }
    }

    for (size_t frontier = 0; all_valid && frontier < p->cells; frontier++) {
        StreamingCellState *state = &window[head];
        StreamingCompletionEntry *completion = state->completion;
        if (!completion || !completion->active || !completion->attached ||
            state->cell_id != frontier ||
            completion->cell_id != frontier ||
            completion->generation != state->generation) {
            stats->streaming_unsafe_slot_reuse_attempts++;
            all_valid = 0;
            break;
        }
        uint64_t wait_start = read_cycle_counter();
        completion->forward_wait_start_cycle = wait_start;
        int window_full = live == depth && next_cell < p->cells;
        while (!streaming_completion_forward_ready(completion)) {
            int poll = poll_streaming_entry(
                completions, completion_capacity, completion, stats);
            if (poll < 0) {
                all_valid = 0;
                break;
            }
            if (!poll) {
                stats->streaming_unsafe_slot_reuse_attempts++;
                all_valid = 0;
                break;
            }
        }
        if (!all_valid)
            break;
        uint64_t inputs_ready_cycle = read_cycle_counter();
        completion->inputs_ready_cycle = inputs_ready_cycle;
        if (inputs_ready_cycle >= wait_start) {
            stats->streaming_forward_wait_dinv_cycles +=
                inputs_ready_cycle - wait_start;
            if (frontier)
                stats->streaming_forward_wait_lbar_cycles +=
                    inputs_ready_cycle - wait_start;
        }
        if (window_full && inputs_ready_cycle >= wait_start)
            stats->streaming_request_window_full_stall_cycles +=
                inputs_ready_cycle - wait_start;

        uint64_t forward_start = read_cycle_counter();
        if (!stats->streaming_first_forward_issue_cycle)
            stats->streaming_first_forward_issue_cycle = forward_start;
        pretransform_forward_cell(p, stats, dq_star, 0, frontier,
                                  &forward_ctx);
        uint64_t forward_end = read_cycle_counter();
        if (forward_end >= forward_start)
            stats->forward_cycles += forward_end - forward_start;
        state->forward_consumed = 1;
        stats->streaming_forward_cells_consumed++;
        completion->forward_consumed = 1;
        if (!strcmp(cfg->coeff_stream_retire_mode, "sync")) {
            uint64_t terminal_start = read_cycle_counter();
            while (!completion->reaped) {
                int poll = poll_streaming_entry(
                    completions, completion_capacity, completion, stats);
                if (poll < 0 || !poll) {
                    all_valid = 0;
                    break;
                }
            }
            uint64_t terminal_end = read_cycle_counter();
            if (terminal_end >= terminal_start)
                stats->streaming_forward_wait_terminal_cycles +=
                    terminal_end - terminal_start;
            if (!all_valid)
                break;
        }
        const uint64_t detached_issue_sequence = completion->issue_sequence;
        if (!completion->attached || completion->generation !=
                state->generation) {
            stats->streaming_unsafe_slot_reuse_attempts++;
            all_valid = 0;
            break;
        }
        completion->attached = 0;
        state->completion = NULL;
        __asm__ __volatile__("" ::: "memory");
        if (!completion->reaped && !completion->record.complete_cycle) {
            stats->streaming_detached_completion_entries++;
        } else {
            if (completion->reaped)
                completion->active = 0;
        }
        live--;
        sample_streaming_window(stats, live);

        if (next_cell < p->cells) {
            StreamingCompletionEntry *next_completion = NULL;
            uint64_t stall_start = read_cycle_counter();
            int queue_full = 0;
            while (!(next_completion = allocate_streaming_completion(
                         completions, completion_capacity,
                         &completion_alloc_cursor, stats))) {
                queue_full = 1;
                int poll = poll_streaming_reaper(
                    completions, completion_capacity,
                    &completion_poll_cursor, stats);
                if (poll < 0 || !poll) {
                    all_valid = 0;
                    break;
                }
            }
            uint64_t stall_end = read_cycle_counter();
            if (queue_full && stall_end >= stall_start)
                stats->streaming_completion_reaper_queue_full_stalls +=
                    stall_end - stall_start;
            if (!all_valid)
                break;

            uint64_t generation = next_stream_generation++;
            int launched = 0;
            while (!launched) {
                if (next_completion && launch_streaming_coeff_cell(
                        p, cfg, state, next_completion, NULL, NULL, NULL,
                        next_cell,
                        generation, issue_sequence)) {
                    launched = 1;
                    break;
                }
                next_completion = NULL;
                int poll = poll_streaming_reaper(
                    completions, completion_capacity,
                    &completion_poll_cursor, stats);
                if (poll < 0 || !poll) {
                    all_valid = 0;
                    break;
                }
                next_completion = allocate_streaming_completion(
                    completions, completion_capacity,
                    &completion_alloc_cursor, stats);
            }
            if (!all_valid)
                break;
            issue_sequence++;
            stats->event_requests_allocated++;
            stats->streaming_request_slots_reused++;
            if (completion->issue_sequence == detached_issue_sequence &&
                completion->active && !completion->reaped &&
                !completion->record.complete_cycle)
                stats->streaming_slot_reuse_after_forward_before_terminal++;
            sample_streaming_reaper(stats, completions,
                                    completion_capacity);
            live++;
            uint64_t outstanding = streaming_reaper_occupancy(
                completions, completion_capacity);
            if (outstanding > stats->event_requests_max_live)
                stats->event_requests_max_live = outstanding;
            next_cell++;
            sample_streaming_window(stats, live);
        }
        int background_poll = poll_streaming_reaper(
            completions, completion_capacity, &completion_poll_cursor,
            stats);
        if (background_poll < 0) {
            all_valid = 0;
            break;
        }
        head = (head + 1) % depth;
    }

    stats->streaming_forward_finish_cycle = read_cycle_counter();
    while (all_valid && streaming_reaper_occupancy(
               completions, completion_capacity)) {
        int poll = poll_streaming_reaper(
            completions, completion_capacity, &completion_poll_cursor,
            stats);
        if (poll < 0 || !poll)
            all_valid = 0;
    }

    if (!all_valid) {
        cancel_streaming_reaper(completions, completion_capacity, stats);
        stats->common_preprocess_fallback_runs++;
        memcpy(dq_star, p->dq_star_raw_ref, p->cells * N * sizeof(double));
        memcpy(dq, p->dq_raw_ref, p->cells * N * sizeof(double));
        free(completions);
        free(window);
        return 0;
    }

    stats->streaming_backward_start_cycle = read_cycle_counter();
    pretransform_backward_sweep(p, cfg, stats, dq_star, dq, 1,
                                &backward_ctx, p->u_bar);
    stats->streaming_backward_finish_cycle = read_cycle_counter();
    if (stats->streaming_backward_start_cycle >=
        stats->streaming_forward_finish_cycle)
        stats->streaming_forward_backward_turnaround_stall_cycles =
            stats->streaming_backward_start_cycle -
            stats->streaming_forward_finish_cycle;

    uint64_t compute_finish = read_cycle_counter();
    stats->streaming_total_cycles = compute_finish - total_start;
    finalize_streaming_timeline(stats);
    all_valid &= validate_streaming_coefficients(p, cfg, stats, check, NULL);
    stats->lu5_factor_cycles += stats->event_lu_active_cycles;
    stats->common_lu_cycles += stats->event_lu_active_cycles;
    stats->ubar_cycles += stats->event_solve_active_cycles;
    stats->common_ubar_cycles += stats->event_solve_active_cycles;
    stats->coeff3_cycles += stats->event_solve_active_cycles;
    stats->raw_persistent_coefficient_bytes = 4U * matrix_bytes;
    stats->raw_extra_coefficient_bytes = 2U * matrix_bytes;
    if (stats->streaming_preprocess_finish_cycle >=
        stats->streaming_first_input_issue_cycle)
        stats->extra_pretransform_cycles =
            stats->streaming_preprocess_finish_cycle -
            stats->streaming_first_input_issue_cycle;
    stats->preprocess_cycles = stats->extra_pretransform_cycles;
    stats->pretransform_raw_runtime_cycles =
        stats->forward_cycles + stats->backward_cycles;
    stats->pretransform_raw_total_cycles = stats->streaming_total_cycles;
    stats->total_cycles = stats->streaming_total_cycles;
    stats->coefficient_update_interval = 1;
    free(completions);
    free(window);
    return all_valid;
}
