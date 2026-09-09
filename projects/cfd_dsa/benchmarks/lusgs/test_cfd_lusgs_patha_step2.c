#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 5
#define SPM_BASE 0x70000000ULL
#define SPM_STRIDE_BYTES 40ULL
#define SPM_STRIDE_ELEMS (SPM_STRIDE_BYTES / sizeof(double))
#define SPM_MATRIX_ROWS 5
#define SPM_PING_MAT_BASE SPM_BASE
#define SPM_PING_VEC_BASE (SPM_BASE + (SPM_MATRIX_ROWS * SPM_STRIDE_BYTES))
#define SPM_TRSV_MAT_BASE (SPM_BASE + 0x1000ULL)
#define SPM_TRSV_RHS_BASE (SPM_TRSV_MAT_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_LU_BASE (SPM_BASE + 0x1800ULL)
#define SPM_TRSM5_RHS_BASE (SPM_TRSM5_LU_BASE + (N * N * sizeof(double)))
#define SPM_TRSM5_OUT_BASE (SPM_TRSM5_RHS_BASE + (N * N * sizeof(double)))
#define SPM_MVM1_MAT_BASE (SPM_BASE + 0x2000ULL)
#define SPM_MVM1_VEC_BASE (SPM_MVM1_MAT_BASE + (N * SPM_STRIDE_BYTES))
#define SPM_TRSM_OPT_SLOT0_BASE (SPM_BASE + 0x2800ULL)
#define SPM_TRSM_OPT_SLOT1_BASE (SPM_BASE + 0x3000ULL)
#define SPM_TRSM_OPT_C_OFFSET (N * N * sizeof(double))
#define SPM_TRSM_OPT_DINV_OFFSET (2 * N * N * sizeof(double))
#define SPM_TRSM_OPT_LBAR_OFFSET (3 * N * N * sizeof(double))
#define SPM_TRSM_OPT_BUFFER_BASE (SPM_BASE + 0x4000ULL)
#define SPM_TRSM_OPT_BUFFER_STRIDE 0x200ULL
#define SPM_COEFF_SLOT0_BASE (SPM_BASE + 0x5000ULL)
#define SPM_COEFF_SLOT1_BASE (SPM_BASE + 0x5800ULL)
#define SPM_COEFF_LU_OFFSET 0ULL
#define SPM_COEFF_L_OFFSET (N * N * sizeof(double))
#define SPM_COEFF_U_OFFSET (2 * N * N * sizeof(double))
#define SPM_COEFF_DINV_OFFSET (3 * N * N * sizeof(double))
#define SPM_COEFF_LBAR_OFFSET (4 * N * N * sizeof(double))
#define SPM_COEFF_UBAR_OFFSET (5 * N * N * sizeof(double))

typedef struct {
    int valid;
    uint64_t line_id;
    uint64_t cell_id;
    uint64_t generation;
    uint64_t completion_token;
    uintptr_t d_inv_addr;
    uintptr_t l_bar_addr;
} PretransformCoefficientEntry;

typedef struct {
    int valid;
    int busy;
    uint64_t generation;
    uint64_t line_id;
    uint64_t cell_id;
} PretransformSpmSlot;

typedef struct {
    int valid;
    uint64_t generation;
    uint64_t line_id;
    uint64_t cell_id;
    uint64_t completion_token;
    uintptr_t d_inv_addr;
    uintptr_t l_bar_addr;
    uintptr_t u_bar_addr;
} CoeffOutputEntry;

typedef struct {
    size_t lines;
    size_t cells;
    double *d_mat;
    double *l_mat;
    double *u_mat;
    double *lu_d;
    double *u_bar;
    double *lu_a;
    double *c_mat;
    double *b_bar;
    double *d_inv;
    double *l_bar_precomputed;
    double *u_bar_precomputed;
    uint8_t *pretransform_valid;
    double *rhs;
    double *dq_star_ref;
    double *dq_ref;
    double *dq_star_raw_ref;
    double *dq_raw_ref;
    double *dq_star_step1;
    double *dq_step1;
    double *dq_star_step2;
    double *dq_step2;
    double *dq_star_step2_core;
    double *dq_step2_core;
    double *dq_star_step2_core_forwarded;
    double *dq_step2_core_forwarded;
    double *dq_star_step2_core_forwarded_context;
    double *dq_step2_core_forwarded_context;
    double *dq_star_step2_core_forwarded_linebuf;
    double *dq_step2_core_forwarded_linebuf;
    double *dq_star_step2_pretransform;
    double *dq_step2_pretransform;
    double *dq_star_step2_pretransform_context;
    double *dq_step2_pretransform_context;
    double *dq_star_step2_pretransform_optprep;
    double *dq_step2_pretransform_optprep;
    double *dq_star_step2_pretransform_optprep_context;
    double *dq_step2_pretransform_optprep_context;
    double *dq_star_trsv5_raw;
    double *dq_trsv5_raw;
    double *dq_star_trsv5_raw_context;
    double *dq_trsv5_raw_context;
    double *dq_star_pretransform_raw;
    double *dq_pretransform_raw;
    double *dq_star_pretransform_raw_context;
    double *dq_pretransform_raw_context;
    double *dq_star_pretransform_raw_optprep;
    double *dq_pretransform_raw_optprep;
    double *dq_star_pretransform_raw_optprep_context;
    double *dq_pretransform_raw_optprep_context;
    double *dq_star_pretransform_raw_stream;
    double *dq_pretransform_raw_stream;
    double *tmp_slots;
} Problem;

typedef struct {
    uint64_t sweeps;
    size_t lines;
    size_t cells;
    size_t interleave;
    int run_reference;
    int run_step1;
    int run_step2;
    int run_step2_core;
    int run_step2_core_forwarded;
    int run_step2_core_forwarded_context;
    int run_step2_core_forwarded_linebuf;
    int run_step2_pretransform;
    int run_step2_pretransform_context;
    int run_step2_pretransform_optprep;
    int run_step2_pretransform_optprep_context;
    int run_step2_trsv5_raw;
    int run_step2_trsv5_raw_context;
    int run_step2_pretransform_raw;
    int run_step2_pretransform_raw_context;
    int run_step2_pretransform_raw_optprep;
    int run_step2_pretransform_raw_optprep_context;
    int run_step2_pretransform_raw_stream;
    int linebuf_enable;
    uint64_t linebuf_entries;
    int trsm5_mrhs_enable;
    uint64_t trsm5_mrhs_lat;
    uint64_t trsm5_mrhs_count;
    uint64_t vec5_lat;
    uint64_t vec5_count;
    int pretransform_fallback_trsv5;
    double pretransform_diag_epsilon;
    int pretransform_per_sweep;
    int trsm5_inv_lbar_enable;
    uint64_t trsm5_inv_lbar_lat;
    uint64_t trsm5_inv_lbar_count;
    int pretransform_output_buffer_enable;
    uint64_t pretransform_output_buffer_depth;
    uint64_t pretransform_spm_slots;
    int pretransform_overlap_enable;
    int pretransform_use_barrier;
    const char *pretransform_validate;
    int lusgs_pretransform_auto;
    uint64_t lusgs_expected_sweeps;
    uint64_t estimated_break_even_sweeps;
    int trsm5_coeff3_enable;
    uint64_t trsm5_coeff3_lat;
    uint64_t trsm5_coeff3_count;
    uint64_t coeff_preprocess_spm_slots;
    uint64_t coeff_output_buffer_depth;
    uint64_t coefficient_update_interval;
    const char *coeff_preprocess_model;
    const char *lu5_model;
    const char *coeff_validation;
    uint64_t coeff_validation_sample_rate;
    int coeff3_partial_output;
    int lu_forwarding;
    int ubar_inplace;
    int coeff_cancel_test;
    int coeff_streaming_enable;
    int coeff_stream_line_autonomous;
    uint64_t coeff_stream_window;
    const char *coeff_stream_retire_mode;
    int coeff_stream_boundary_mask;
    const char *coeff_stream_dinv_consumer;
    const char *coeff_stream_lbar_consumer;
    const char *coeff_stream_ubar_consumer;
    uint64_t coeff_column_fma_latency;
    uint64_t coeff_column_fma_ii;
    uint64_t coeff_column_fma_count;
    uint64_t coeff_column_fma_queue_depth;
    uint64_t forward_combine_latency;
    uint64_t forward_combine_ii;
    uint64_t forward_combine_count;
    uint64_t forward_combine_queue_depth;
    int check;
    int trace_enable;
    size_t trace_lines;
    size_t trace_cells;
    uint64_t trsv5_count;
    uint64_t trsv5_lat;
    const char *trsv5_mode;
    int trsv5_trace_enable;
    uint64_t trsv5_trace_solves;
    const char *trsv5_trace_file;
    const char *patha_store_mode;
    uint64_t patha_store_stride;
    uint64_t patha_unroll;
    const char *patha_acc_mode;
    uint64_t patha_buffer_depth;
    int patha_streaming;
    const char *patha_kernel;
} Config;

typedef struct {
    uint64_t reference_mvm_forward;
    uint64_t reference_mvm_backward;
    uint64_t reference_software_trsv;
    uint64_t reference_vector_sub_ops;
    uint64_t forward_cells;
    uint64_t backward_cells;
    uint64_t patha_mvm_forward;
    uint64_t patha_mvm_backward;
    uint64_t software_trsv;
    uint64_t hardware_trsv;
    uint64_t software_trsv_hot_path;
    uint64_t vector_sub_ops;
    uint64_t fused_forward_updates;
    uint64_t fused_backward_updates;
    uint64_t fused_sub_ops;
    uint64_t hardware_vector_sub;
    uint64_t trsv5_rhs_forwarded;
    uint64_t trsv5_rhs_spm_stage_elided;
    uint64_t step2_core_forward_rhs_stack_store_elided;
    uint64_t trsv5_rhs_forward_stall_cycles;
    uint64_t trsv5_rhs_forward_invalid;
    uint64_t trsv5_rhs_forward_consumed;
    uint64_t forward_context_hits;
    uint64_t forward_context_misses;
    uint64_t forward_dqstar_heap_vector_load_elided;
    uint64_t forward_context_vector_stages;
    uint64_t forward_context_updates;
    uint64_t forward_context_invalid;
    uint64_t forward_context_stall_cycles;
    uint64_t backward_context_hits;
    uint64_t backward_context_misses;
    uint64_t backward_dq_heap_vector_load_elided;
    uint64_t backward_context_vector_stages;
    uint64_t backward_context_updates;
    uint64_t backward_context_invalid;
    uint64_t backward_context_stall_cycles;
    uint64_t line_context_forwarding_enabled;
    uint64_t line_context_total_hits;
    uint64_t line_context_total_misses;
    uint64_t linebuf_forward_reads;
    uint64_t linebuf_forward_writes;
    uint64_t linebuf_backward_reads;
    uint64_t linebuf_backward_writes;
    uint64_t linebuf_forward_hits;
    uint64_t linebuf_forward_misses;
    uint64_t linebuf_backward_hits;
    uint64_t linebuf_backward_misses;
    uint64_t linebuf_spm_vector_stages_elided;
    uint64_t linebuf_lmat_vec_loads_elided;
    uint64_t linebuf_context_memory_stores_elided;
    uint64_t linebuf_context_memory_loads_elided;
    uint64_t linebuf_tag_conflicts;
    uint64_t linebuf_invalid_reads;
    uint64_t linebuf_stall_cycles;
    uint64_t linebuf_forwarding_enabled;
    uint64_t tmp_result_stores;
    uint64_t tmp_result_loads;
    uint64_t tmp_result_store_bytes;
    uint64_t tmp_result_load_bytes;
    uint64_t forward_cycles;
    uint64_t backward_cycles;
    uint64_t total_cycles;
    uint64_t software_trsv_cycles;
    uint64_t trsv5_latency_configured;
    uint64_t trsm5_mrhs_issued;
    uint64_t trsm5_mrhs_completed;
    uint64_t trsm5_mrhs_invalid;
    uint64_t trsm5_mrhs_busy_cycles;
    uint64_t trsm5_mrhs_stall_cycles;
    uint64_t trsm5_mrhs_inv_batches;
    uint64_t trsm5_mrhs_lbar_batches;
    uint64_t trsm5_mrhs_ubar_batches;
    uint64_t trsm5_mrhs_columns_solved;
    uint64_t trsm5_mrhs_input_lu_bytes;
    uint64_t trsm5_mrhs_input_rhs_bytes;
    uint64_t trsm5_mrhs_output_bytes;
    uint64_t preprocess_cycles;
    uint64_t preprocess_cells;
    uint64_t preprocess_matrices_generated;
    uint64_t ubar_reused_existing;
    uint64_t pretransform_valid_cells;
    uint64_t pretransform_invalid_cells;
    uint64_t pretransform_fallback_runs;
    uint64_t pretransform_nan_inf;
    uint64_t pretransform_dinv_mvm;
    uint64_t pretransform_lbar_mvm;
    uint64_t pretransform_ubar_mvm;
    uint64_t pretransform_vector_sub;
    uint64_t pretransform_forward_cells;
    uint64_t pretransform_backward_cells;
    uint64_t pretransform_hot_loop_trsv5_elided;
    uint64_t pretransform_coefficient_bytes_read;
    uint64_t pretransform_tmp_stores;
    uint64_t pretransform_tmp_loads;
    uint64_t pretransform_patha_ping_issued;
    uint64_t pretransform_patha_pong_issued;
    uint64_t pretransform_patha_slot_stalls;
    uint64_t pretransform_patha_invalid_slots;
    uint64_t runtime_sweep_cycles;
    uint64_t total_cycles_including_preprocess;
    uint64_t preprocess_stage_cycles;
    uint64_t preprocess_compute_cycles;
    uint64_t preprocess_drain_cycles;
    uint64_t preprocess_validation_cycles;
    uint64_t preprocess_residual_check_cycles;
    uint64_t preprocess_fast_check_cycles;
    uint64_t preprocess_barrier_cycles;
    uint64_t preprocess_idle_cycles;
    uint64_t preprocess_overlap_cycles;
    uint64_t preprocess_stage_compute_overlap_cycles;
    uint64_t preprocess_compute_drain_overlap_cycles;
    uint64_t preprocess_lu_stage_bytes;
    uint64_t preprocess_c_stage_bytes;
    uint64_t preprocess_identity_stage_bytes;
    uint64_t preprocess_output_spm_bytes;
    uint64_t preprocess_output_heap_bytes;
    uint64_t preprocess_lu_stages;
    uint64_t preprocess_identity_stages;
    uint64_t preprocess_c_stages;
    uint64_t trsm5_inv_lbar_issued;
    uint64_t trsm5_inv_lbar_completed;
    uint64_t trsm5_inv_lbar_busy_cycles;
    uint64_t trsm5_inv_lbar_stall_cycles;
    uint64_t trsm5_inv_lbar_lu_reads;
    uint64_t trsm5_inv_lbar_columns_solved;
    uint64_t pretransform_output_buffer_alloc;
    uint64_t pretransform_output_buffer_free;
    uint64_t pretransform_output_buffer_full_stalls;
    uint64_t pretransform_output_buffer_max_occupancy;
    uint64_t pretransform_output_drain_cycles;
    uint64_t pretransform_barriers_issued;
    uint64_t pretransform_barriers_elided;
    uint64_t pretransform_input_token_stalls;
    uint64_t pretransform_output_token_stalls;
    uint64_t pretransform_auto_selected;
    uint64_t trsv5_auto_selected;
    uint64_t expected_sweeps;
    uint64_t estimated_break_even_sweeps;
    uint64_t raw_input_d_bytes;
    uint64_t raw_input_l_bytes;
    uint64_t raw_input_u_bytes;
    uint64_t raw_input_r_bytes;
    uint64_t lu5_factor_cells;
    uint64_t lu5_factor_cycles;
    uint64_t lu5_factor_failures;
    uint64_t lu5_factor_input_bytes;
    uint64_t lu5_factor_output_bytes;
    uint64_t ubar_generated;
    uint64_t ubar_trsm_issued;
    uint64_t ubar_trsm_completed;
    uint64_t ubar_columns_solved;
    uint64_t ubar_cycles;
    uint64_t ubar_stage_bytes;
    uint64_t ubar_output_bytes;
    uint64_t ubar_invalid;
    uint64_t dinv_generated;
    uint64_t lbar_generated;
    uint64_t coeff3_issued;
    uint64_t coeff3_completed;
    uint64_t coeff3_columns_solved;
    uint64_t coeff3_cycles;
    uint64_t coeff3_lu_reads;
    uint64_t common_lu_cycles;
    uint64_t common_ubar_cycles;
    uint64_t common_preprocess_cycles;
    uint64_t extra_dinv_cycles;
    uint64_t extra_lbar_cycles;
    uint64_t extra_pretransform_cycles;
    uint64_t fair_extra_pretransform_cycles;
    uint64_t trsv5_raw_runtime_cycles;
    uint64_t pretransform_raw_runtime_cycles;
    uint64_t trsv5_raw_total_cycles;
    uint64_t pretransform_raw_total_cycles;
    uint64_t fair_break_even_sweeps;
    uint64_t coefficient_update_interval;
    uint64_t coefficient_rebuilds;
    uint64_t coefficient_reuses;
    uint64_t common_preprocess_fallback_runs;
    uint64_t pretransform_fallback_to_trsv5_raw;
    uint64_t coeff_output_alloc;
    uint64_t coeff_output_free;
    uint64_t coeff_output_full_stalls;
    uint64_t coeff_output_max_occupancy;
    uint64_t coeff_output_drain_cycles;
    uint64_t raw_persistent_coefficient_bytes;
    uint64_t raw_extra_coefficient_bytes;
    uint64_t event_requests_allocated;
    uint64_t event_requests_completed;
    uint64_t event_requests_failed;
    uint64_t event_requests_freed;
    uint64_t event_requests_max_live;
    uint64_t event_first_cell_latency;
    uint64_t event_last_cell_latency;
    uint64_t event_cell_latency_sum;
    uint64_t event_first_completion_cycle;
    uint64_t event_last_completion_cycle;
    uint64_t event_dinv_ready_latency_sum;
    uint64_t event_lbar_ready_latency_sum;
    uint64_t event_ubar_ready_latency_sum;
    uint64_t event_input_active_cycles;
    uint64_t event_lu_active_cycles;
    uint64_t event_solve_active_cycles;
    uint64_t event_drain_active_cycles;
    uint64_t event_input_lu_overlap_cycles;
    uint64_t event_input_solve_overlap_cycles;
    uint64_t event_lu_solve_overlap_cycles;
    uint64_t event_compute_drain_overlap_cycles;
    uint64_t event_three_way_overlap_cycles;
    uint64_t event_lu_div_issued;
    uint64_t event_lu_mul_issued;
    uint64_t event_lu_sub_issued;
    uint64_t event_coeff_div_issued;
    uint64_t event_coeff_mul_issued;
    uint64_t event_coeff_sub_issued;
    uint64_t event_divider_busy_lane_cycles;
    uint64_t event_mul_busy_lane_cycles;
    uint64_t event_sub_busy_lane_cycles;
    uint64_t event_stall_input_slot;
    uint64_t event_stall_lu_pending;
    uint64_t event_stall_solve_pending;
    uint64_t event_stall_output_ring;
    uint64_t event_stall_lu_divider;
    uint64_t event_stall_lu_mulsub;
    uint64_t event_stall_coeff_divider;
    uint64_t event_stall_coeff_mulsub;
    uint64_t event_stall_dependency;
    uint64_t event_stall_spm_read_port;
    uint64_t event_stall_spm_write_port;
    uint64_t event_stall_spm_bank;
    uint64_t event_stall_drain_queue;
    uint64_t event_input_bytes;
    uint64_t event_output_bytes;
    uint64_t event_input_write_requests;
    uint64_t event_drain_requests;
    uint64_t event_max_output_occupancy;
    uint64_t event_scheduler_scans;
    uint64_t event_scheduler_selections;
    uint64_t event_scheduler_no_selection;
    uint64_t event_ready_rhs_count_sum;
    uint64_t event_ready_rhs_count_max;
    uint64_t event_cycles_with_no_ready_rhs;
    uint64_t event_divider_ready_candidates;
    uint64_t event_mulsub_ready_candidates;
    uint64_t event_rhs_starvation_cycles;
    uint64_t event_rhs_max_wait_cycles;
    uint64_t event_rhs_wait_cycles;
    uint64_t event_identity_batch_wait_cycles;
    uint64_t event_lbar_batch_wait_cycles;
    uint64_t event_ubar_batch_wait_cycles;
    uint64_t event_coeff_divider_busy_lane_cycles;
    uint64_t event_coeff_divider_idle_lane_cycles;
    uint64_t event_coeff_mul_busy_lane_cycles;
    uint64_t event_coeff_sub_busy_lane_cycles;
    uint64_t event_scheduler_cycles;
    uint64_t event_lu_step_ready_cycle[5];
    uint64_t event_lu_step_first_use_latency[5];
    uint64_t event_early_solve_issued;
    uint64_t event_early_solve_missed;
    uint64_t event_trsm_blocked_by_lu_step_cycles;
    uint64_t event_same_cell_lu_solve_overlap_cycles;
    uint64_t event_cross_cell_lu_solve_overlap_cycles;
    uint64_t event_coeff_spm_packet_issued;
    uint64_t event_coeff_spm_packet_completed;
    uint64_t event_coeff_input_packet_bytes;
    uint64_t event_coeff_drain_packet_bytes;
    uint64_t event_coeff_spm_read_port_stalls;
    uint64_t event_coeff_spm_write_port_stalls;
    uint64_t event_coeff_spm_bank_stalls;
    uint64_t event_coeff_spm_outstanding_stalls;
    uint64_t streaming_enabled;
    uint64_t streaming_window_depth;
    uint64_t streaming_first_input_issue_cycle;
    uint64_t streaming_first_lu_issue_cycle;
    uint64_t streaming_first_dinv_column_ready_cycle;
    uint64_t streaming_first_lbar_column_ready_cycle;
    uint64_t streaming_first_ubar_column_ready_cycle;
    uint64_t streaming_first_dinv_ready_cycle;
    uint64_t streaming_first_lbar_ready_cycle;
    uint64_t streaming_first_ubar_ready_cycle;
    uint64_t streaming_first_forward_issue_cycle;
    uint64_t streaming_preprocess_finish_cycle;
    uint64_t streaming_forward_finish_cycle;
    uint64_t streaming_backward_start_cycle;
    uint64_t streaming_backward_finish_cycle;
    uint64_t streaming_forward_started_before_preprocess_done;
    uint64_t streaming_preprocess_forward_overlap_cycles;
    uint64_t streaming_preprocess_backward_prepare_overlap_cycles;
    uint64_t streaming_forward_backward_turnaround_stall_cycles;
    uint64_t streaming_timeline_inconsistencies;
    uint64_t streaming_forward_wait_dinv_cycles;
    uint64_t streaming_forward_wait_lbar_cycles;
    uint64_t streaming_forward_wait_prev_vector_cycles;
    uint64_t streaming_backward_wait_ubar_cycles;
    uint64_t streaming_backward_wait_next_vector_cycles;
    uint64_t streaming_request_window_occupancy_sum;
    uint64_t streaming_request_window_samples;
    uint64_t streaming_request_window_max_occupancy;
    uint64_t streaming_request_window_full_stall_cycles;
    uint64_t streaming_output_ring_occupancy_sum;
    uint64_t streaming_output_ring_samples;
    uint64_t streaming_total_cycles;
    uint64_t streaming_forward_cells_consumed;
    uint64_t streaming_request_slots_reused;
    uint64_t streaming_stale_generation_rejected;
    uint64_t streaming_dinv_columns_bypassed;
    uint64_t streaming_lbar_columns_bypassed;
    uint64_t streaming_ubar_columns_bypassed;
    uint64_t streaming_coefficient_drain_bytes_avoided;
    uint64_t streaming_base_prefetch_issued;
    uint64_t streaming_base_prefetch_hits;
    uint64_t streaming_base_prefetch_misses;
    uint64_t streaming_patha_idle_with_ready_work_cycles;
    uint64_t streaming_trsm_idle_with_ready_rhs_cycles;
    uint64_t streaming_divider_idle_with_ready_rhs_cycles;
    uint64_t streaming_mulsub_idle_with_ready_work_cycles;
    uint64_t streaming_rhs_queue_occupancy_sum;
    uint64_t streaming_rhs_queue_samples;
    uint64_t streaming_drain_queue_occupancy_sum;
    uint64_t streaming_drain_queue_samples;
    uint64_t streaming_boundary_skipped_dinv_rhs;
    uint64_t streaming_boundary_skipped_lbar_rhs;
    uint64_t streaming_boundary_skipped_ubar_rhs;
    uint64_t streaming_requested_dinv_batches;
    uint64_t streaming_requested_lbar_batches;
    uint64_t streaming_requested_ubar_batches;
    uint64_t streaming_avoided_lbar_drains;
    uint64_t streaming_avoided_ubar_drains;
    uint64_t streaming_effective_rhs_count;
    uint64_t streaming_completion_reaper_polls;
    uint64_t streaming_completion_reaper_terminal_hits;
    uint64_t streaming_completion_reaper_busy_polls;
    uint64_t streaming_completion_reaper_queue_max_occupancy;
    uint64_t streaming_completion_reaper_queue_full_stalls;
    uint64_t streaming_completion_reaper_out_of_order_reaps;
    uint64_t streaming_completion_reaper_cancels;
    uint64_t streaming_forward_wait_terminal_cycles;
    uint64_t streaming_forward_wait_ubar_cycles;
    uint64_t streaming_forward_wait_drain_cycles;
    uint64_t streaming_slot_reuse_after_forward_before_terminal;
    uint64_t streaming_detached_completion_entries;
    uint64_t streaming_unsafe_slot_reuse_attempts;
    uint64_t streaming_stale_generation_reads;
    uint64_t streaming_double_column_consumes;
    uint64_t auto_retired_requests;
    uint64_t auto_retired_tokens;
    uint64_t auto_retire_errors;
    uint64_t auto_retire_before_forward_finish;
    uint64_t auto_retire_before_backward_start;
    uint64_t terminal_wait_calls_avoided;
    uint64_t completion_queue_scans_avoided;
    uint64_t completion_queue_entries_avoided;
    uint64_t progress_poll_calls;
    uint64_t progress_poll_busy;
    uint64_t progress_poll_useful;
    uint64_t progress_poll_cycles;
    uint64_t frontier_wait_dinv_cycles;
    uint64_t frontier_wait_lbar_cycles;
    uint64_t frontier_wait_ubar_cycles;
    uint64_t frontier_no_work_cycles;
    uint64_t mask_table_lookups;
    uint64_t rhs_mask_scheduler_scans;
    uint64_t rhs_mask_scheduler_useful_issues;
    uint64_t rhs_mask_scheduler_empty_scans;
    uint64_t guest_completion_record_checks;
    uint64_t guest_terminal_error_checks;
    uint64_t progress_wait_calls;
    uint64_t terminal_wait_calls;
    uint64_t terminal_wait_busy_returns;
    uint64_t stale_token_waits;
    uint64_t terminal_record_check_cycles;
    uint64_t guest_bookkeeping_cycles;
    uint64_t dinv_column_consumer_enabled;
    uint64_t dinv_column_consumer_shadow_runs;
    uint64_t dinv_column_consumer_direct_runs;
    uint64_t dinv_columns_ready;
    uint64_t dinv_columns_queued;
    uint64_t dinv_columns_issued;
    uint64_t dinv_columns_completed;
    uint64_t dinv_columns_consumed;
    uint64_t dinv_columns_duplicate_rejected;
    uint64_t dinv_columns_generation_rejected;
    uint64_t dinv_columns_out_of_order_held;
    uint64_t column_fma_queue_max_occupancy;
    uint64_t column_fma_queue_full_stalls;
    uint64_t column_fma_issue_cycles;
    uint64_t column_fma_busy_cycles;
    uint64_t column_fma_completions;
    uint64_t column_fma_retries;
    uint64_t column_fma_ready_but_blocked_cycles;
    uint64_t column_fma_queue_occupancy_sum;
    uint64_t column_fma_queue_sample_cycles;
    uint64_t base_accumulators_allocated;
    uint64_t base_accumulators_completed;
    uint64_t base_accumulators_cancelled;
    uint64_t base_accumulator_missing_columns;
    uint64_t base_accumulator_double_consumes;
    uint64_t base_accumulator_stale_writes;
    uint64_t first_base_ready_cycle;
    uint64_t last_base_ready_cycle;
    uint64_t base_vector_publish_count;
    uint64_t base_vector_publish_bytes;
    uint64_t base_vector_publish_stalls;
    uint64_t dinv_matrix_drains_avoided;
    uint64_t dinv_matrix_drain_bytes_avoided;
    uint64_t dinv_guest_reload_bytes_avoided;
    uint64_t patha_dinv_mvm_eliminated;
    uint64_t patha_dinv_custom_instructions_eliminated;
    uint64_t frontier_wait_base_cycles;
    uint64_t base_ready_before_frontier;
    uint64_t base_ready_after_frontier;
    uint64_t shadow_base_bitwise_mismatch;
    uint64_t shadow_base_mismatch_cell;
    uint64_t shadow_base_mismatch_lane;
    double shadow_base_max_abs_error;
    double shadow_base_max_rel_error;
    uint64_t lbar_column_consumer_enabled;
    uint64_t lbar_column_consumer_shadow_runs;
    uint64_t lbar_column_consumer_direct_runs;
    uint64_t lbar_columns_ready, lbar_columns_queued;
    uint64_t lbar_columns_issued, lbar_columns_completed;
    uint64_t lbar_columns_consumed, lbar_columns_duplicate_rejected;
    uint64_t lbar_columns_generation_rejected;
    uint64_t lbar_columns_out_of_order_held;
    uint64_t correction_accumulators_allocated;
    uint64_t correction_accumulators_completed;
    uint64_t correction_accumulators_cancelled;
    uint64_t correction_accumulator_missing_columns;
    uint64_t correction_accumulator_double_consumes;
    uint64_t correction_accumulator_stale_writes;
    uint64_t forward_handoffs_produced, forward_handoffs_consumed;
    uint64_t forward_handoff_wait_cycles, forward_handoff_max_live;
    uint64_t forward_handoff_overwrite_errors;
    uint64_t forward_handoff_wrong_cell_errors;
    uint64_t forward_handoff_wrong_generation_errors;
    uint64_t forward_handoff_double_consumes;
    uint64_t forward_handoff_missing_consumes;
    uint64_t forward_combine_queued, forward_combine_issued;
    uint64_t forward_combine_completed;
    uint64_t forward_combine_queue_max_occupancy;
    uint64_t forward_combine_queue_full_stalls;
    uint64_t forward_combine_busy_cycles;
    uint64_t forward_combine_ready_but_blocked_cycles;
    uint64_t dqstar_publish_count, dqstar_publish_bytes;
    uint64_t dqstar_publish_stalls;
    uint64_t base_standalone_publishes_avoided;
    uint64_t base_standalone_publish_bytes_avoided;
    uint64_t lbar_matrix_drains_avoided;
    uint64_t lbar_matrix_drain_bytes_avoided;
    uint64_t lbar_guest_reload_bytes_avoided;
    uint64_t patha_lbar_mvm_eliminated;
    uint64_t patha_lbar_custom_instructions_eliminated;
    uint64_t lbar_consumer_ready_but_blocked_cycles;
    uint64_t lbar_consumer_wait_previous_dq_cycles;
    uint64_t lbar_consumer_wait_column_cycles;
    uint64_t lbar_consumer_wait_engine_cycles;
    uint64_t consumer_scheduler_aging_promotions;
    uint64_t consumer_scheduler_starvation_violations;
    uint64_t frontier_wait_dqstar_cycles;
    uint64_t frontier_wait_previous_dq_cycles;
    uint64_t frontier_wait_lbar_columns_cycles;
    uint64_t frontier_wait_consumer_engine_cycles;
    uint64_t frontier_wait_dqstar_publish_cycles;
    uint64_t shadow_correction_bitwise_mismatch;
    uint64_t shadow_dqstar_bitwise_mismatch;
    uint64_t shadow_correction_mismatch_cell;
    uint64_t shadow_correction_mismatch_lane;
    uint64_t shadow_dqstar_mismatch_cell;
    uint64_t shadow_dqstar_mismatch_lane;
    double shadow_correction_max_abs_error;
    double shadow_correction_max_rel_error;
    double shadow_dqstar_max_abs_error;
    double shadow_dqstar_max_rel_error;
    double max_abs_lu_residual;
    double max_abs_ubar_residual;
    double max_rel_ubar_residual;
} Stats;

enum {
    COEFF_PRE_TRSV = 1ULL << 0,
    COEFF_PRE_COEFF3 = 1ULL << 1,
    COEFF_PRE_PARTIAL_OUTPUT = 1ULL << 2,
    COEFF_PRE_LU_FORWARD = 1ULL << 3,
    COEFF_PRE_PACKED_LU_INPUT = 1ULL << 4,
    COEFF_PRE_STREAMING_PROGRESS = 1ULL << 5,
    COEFF_PRE_NEED_DINV = 1ULL << 6,
    COEFF_PRE_NEED_LBAR = 1ULL << 7,
    COEFF_PRE_NEED_UBAR = 1ULL << 8,
    COEFF_PRE_STREAMING_AUTO_RETIRE = 1ULL << 9,
    COEFF_PRE_DINV_BASE_CONSUMER = 1ULL << 10,
    COEFF_PRE_DINV_DIRECT_BYPASS = 1ULL << 11,
    COEFF_PRE_LBAR_FORWARD_CONSUMER = 1ULL << 12,
    COEFF_PRE_LBAR_DIRECT_BYPASS = 1ULL << 13,
    COEFF_PRE_TEST_HANDOFF_WRONG_CELL = 1ULL << 14,
    COEFF_PRE_TEST_HANDOFF_WRONG_GENERATION = 1ULL << 15,
    COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH = 1ULL << 16,
    COEFF_PRE_LINE_AUTONOMOUS = 1ULL << 17,
    COEFF_PRE_LINE_BACKWARD_DIRECT = 1ULL << 18,
    COEFF_PRE_LINE_COEFF_CACHE = 1ULL << 19,
    COEFF_PRE_LINE_COEFF_DIRTY = 1ULL << 20,
    COEFF_PRE_STATUS_COMPLETE = 0,
    COEFF_PRE_STATUS_BUSY = 1,
    COEFF_PRE_STATUS_BAD_TOKEN = 5,
    COEFF_PRE_STATUS_GENERATION_MISMATCH = 7,
    COEFF_PRE_STATUS_INTERNAL_ERROR = 8,
    COEFF_PRE_STATUS_CANCELLED = 9,
    COEFF_PRE_STATUS_ALREADY_COMPLETE = 10,
    COEFF_PRE_STATUS_CANCEL_PENDING = 11,
    COEFF_PRE_STATUS_DINV_READY = 12,
    COEFF_PRE_STATUS_DINV_LBAR_READY = 13,
    COEFF_PRE_STATUS_DINV_LBAR_UBAR_READY = 14
};

typedef struct {
    uint64_t version;
    uint64_t size;
    uint64_t flags;
    uint64_t generation;
    uint64_t line_id;
    uint64_t cell_id;
    uint64_t d_addr;
    uint64_t l_addr;
    uint64_t u_addr;
    uint64_t lu_out_addr;
    uint64_t d_inv_out_addr;
    uint64_t l_bar_out_addr;
    uint64_t u_bar_out_addr;
    uint64_t record_addr;
    double pivot_epsilon;
    uint64_t reserved[5];
} CoeffPreprocessDescriptor;

typedef struct {
    uint64_t request_id, generation, line_id, cell_id, status, slot;
    uint64_t issue_cycle, input_start_cycle, input_done_cycle;
    uint64_t lu_issue_cycle, lu_first_result_cycle, lu_complete_cycle;
    uint64_t solve_issue_cycle, first_column_cycle;
    uint64_t d_inv_ready_cycle, l_bar_ready_cycle, u_bar_ready_cycle;
    uint64_t d_inv_column_ready_cycle[5];
    uint64_t l_bar_column_ready_cycle[5];
    uint64_t u_bar_column_ready_cycle[5];
    uint64_t solve_complete_cycle;
    uint64_t d_inv_drain_start, d_inv_drain_done;
    uint64_t l_bar_drain_start, l_bar_drain_done;
    uint64_t u_bar_drain_start, u_bar_drain_done;
    uint64_t lu_drain_start, lu_drain_done, complete_cycle;
    uint64_t input_bytes, input_write_requests, output_bytes, drain_requests;
    uint64_t lu_div_issued, lu_mul_issued, lu_sub_issued;
    uint64_t coeff_div_issued, coeff_mul_issued, coeff_sub_issued;
    uint64_t stall_input_slot, stall_lu_pending, stall_solve_pending;
    uint64_t stall_output_ring, stall_lu_divider, stall_lu_mulsub;
    uint64_t stall_coeff_divider, stall_coeff_mulsub, stall_dependency;
    uint64_t stall_spm_read_port, stall_spm_write_port, stall_spm_bank;
    uint64_t stall_drain_queue;
    uint64_t input_active_cycles, lu_active_cycles, solve_active_cycles;
    uint64_t drain_active_cycles, input_lu_overlap_cycles;
    uint64_t input_solve_overlap_cycles, lu_solve_overlap_cycles;
    uint64_t compute_drain_overlap_cycles, three_way_overlap_cycles;
    uint64_t divider_busy_lane_cycles, mul_busy_lane_cycles;
    uint64_t sub_busy_lane_cycles, max_output_occupancy;
    uint64_t scheduler_scans, scheduler_selections;
    uint64_t scheduler_no_selection, ready_rhs_count_sum;
    uint64_t ready_rhs_count_max, cycles_with_no_ready_rhs;
    uint64_t divider_ready_candidates, mulsub_ready_candidates;
    uint64_t rhs_starvation_cycles, rhs_max_wait_cycles, rhs_wait_cycles;
    uint64_t identity_batch_wait_cycles, lbar_batch_wait_cycles;
    uint64_t ubar_batch_wait_cycles;
    uint64_t coeff_divider_busy_lane_cycles;
    uint64_t coeff_divider_idle_lane_cycles;
    uint64_t coeff_mul_busy_lane_cycles, coeff_sub_busy_lane_cycles;
    uint64_t scheduler_cycles;
    uint64_t lu_step_ready_cycle[5];
    uint64_t lu_step_first_use_latency[5];
    uint64_t early_solve_issued, early_solve_missed;
    uint64_t trsm_blocked_by_lu_step_cycles;
    uint64_t same_cell_lu_solve_overlap_cycles;
    uint64_t cross_cell_lu_solve_overlap_cycles;
    uint64_t coeff_spm_packet_issued, coeff_spm_packet_completed;
    uint64_t coeff_input_packet_bytes, coeff_drain_packet_bytes;
    uint64_t coeff_spm_read_port_stalls, coeff_spm_write_port_stalls;
    uint64_t coeff_spm_bank_stalls, coeff_spm_outstanding_stalls;
    uint64_t auto_retired, auto_retire_error, mask_table_lookups;
    uint64_t rhs_mask_scheduler_scans, rhs_mask_useful_issues;
    uint64_t rhs_mask_empty_scans;
    uint64_t rhs_vector_issue_cycle, rhs_vector_ready_cycle;
    uint64_t base_compute_ready_cycle, base_publish_issue_cycle;
    uint64_t base_ready_cycle, base_ready, base_consumer_error;
    uint64_t base_consumed_column_mask;
    uint64_t d_inv_columns_ready, d_inv_columns_queued;
    uint64_t d_inv_columns_issued, d_inv_columns_completed;
    uint64_t d_inv_columns_consumed, d_inv_columns_duplicate_rejected;
    uint64_t d_inv_columns_generation_rejected;
    uint64_t d_inv_columns_out_of_order_held;
    uint64_t column_fma_queue_max_occupancy;
    uint64_t column_fma_queue_full_stalls, column_fma_issue_cycles;
    uint64_t column_fma_busy_cycles, column_fma_completions;
    uint64_t column_fma_retries, column_fma_ready_but_blocked_cycles;
    uint64_t base_accumulators_allocated, base_accumulators_completed;
    uint64_t base_accumulators_cancelled, base_accumulator_missing_columns;
    uint64_t base_accumulator_double_consumes;
    uint64_t base_accumulator_stale_writes;
    uint64_t base_vector_publish_count, base_vector_publish_bytes;
    uint64_t base_vector_publish_stalls;
    uint64_t d_inv_matrix_drains_avoided;
    uint64_t d_inv_matrix_drain_bytes_avoided;
    uint64_t column_fma_queue_occupancy_sum;
    uint64_t column_fma_queue_sample_cycles;
    uint64_t dq_star_ready, dq_star_ready_cycle, dq_star_publish_cycle;
    uint64_t lbar_consumed_column_mask, correction_ready_cycle;
    uint64_t forward_combine_issue_cycle, forward_combine_complete_cycle;
    uint64_t forward_handoff_ready_cycle, forward_handoff_consumed_cycle;
    uint64_t forward_consumer_error;
    uint64_t lbar_columns_ready, lbar_columns_queued;
    uint64_t lbar_columns_issued, lbar_columns_completed;
    uint64_t lbar_columns_consumed, lbar_columns_duplicate_rejected;
    uint64_t lbar_columns_generation_rejected;
    uint64_t lbar_columns_out_of_order_held;
    uint64_t correction_accumulators_allocated;
    uint64_t correction_accumulators_completed;
    uint64_t correction_accumulators_cancelled;
    uint64_t correction_accumulator_missing_columns;
    uint64_t correction_accumulator_double_consumes;
    uint64_t correction_accumulator_stale_writes;
    uint64_t forward_handoffs_produced, forward_handoffs_consumed;
    uint64_t forward_handoff_wait_cycles, forward_handoff_max_live;
    uint64_t forward_handoff_overwrite_errors;
    uint64_t forward_handoff_wrong_cell_errors;
    uint64_t forward_handoff_wrong_generation_errors;
    uint64_t forward_handoff_double_consumes;
    uint64_t forward_handoff_missing_consumes;
    uint64_t forward_combine_queued, forward_combine_issued;
    uint64_t forward_combine_completed;
    uint64_t forward_combine_queue_max_occupancy;
    uint64_t forward_combine_queue_full_stalls;
    uint64_t forward_combine_busy_cycles;
    uint64_t forward_combine_ready_but_blocked_cycles;
    uint64_t dq_star_publish_count, dq_star_publish_bytes;
    uint64_t dq_star_publish_stalls;
    uint64_t base_standalone_publishes_avoided;
    uint64_t base_standalone_publish_bytes_avoided;
    uint64_t lbar_matrix_drains_avoided;
    uint64_t lbar_matrix_drain_bytes_avoided;
    uint64_t lbar_guest_reload_bytes_avoided;
    uint64_t patha_lbar_mvm_eliminated;
    uint64_t patha_lbar_custom_instructions_eliminated;
    uint64_t lbar_consumer_ready_but_blocked_cycles;
    uint64_t lbar_consumer_wait_previous_dq_cycles;
    uint64_t lbar_consumer_wait_column_cycles;
    uint64_t lbar_consumer_wait_engine_cycles;
    uint64_t consumer_scheduler_aging_promotions;
    uint64_t consumer_scheduler_starvation_violations;
    uint64_t frontier_wait_dqstar_cycles;
    uint64_t frontier_wait_previous_dq_cycles;
    uint64_t frontier_wait_lbar_columns_cycles;
    uint64_t frontier_wait_consumer_engine_cycles;
    uint64_t frontier_wait_dqstar_publish_cycles;
} CoeffPreprocessRecord;

typedef struct {
    CoeffPreprocessDescriptor descriptor;
    CoeffPreprocessRecord record;
    uint64_t token;
    int valid;
} CoeffPreprocessEntry;

typedef struct {
    CoeffPreprocessRecord record;
    uint64_t token;
    uint64_t line_id;
    uint64_t cell_id;
    uint64_t generation;
    uint64_t issue_sequence;
    uint64_t required_coefficient_mask;
    uint64_t terminal_status;
    uint64_t forward_wait_start_cycle;
    uint64_t inputs_ready_cycle;
    int active;
    int attached;
    int terminal;
    int reaped;
    int cancel_pending;
    int error_status;
    int dinv_progress;
    int lbar_progress;
    int ubar_progress;
    int forward_consumed;
} StreamingCompletionEntry;

typedef struct {
    CoeffPreprocessDescriptor descriptor;
    StreamingCompletionEntry *completion;
    uint64_t line_id;
    uint64_t cell_id;
    uint64_t generation;
    int input_started;
    int input_complete;
    int lu_started;
    int lu_complete;
    int dinv_ready;
    int lbar_ready;
    int ubar_ready;
    int base_ready;
    int forward_ready;
    int backward_coeff_ready;
    int forward_consumed;
    int backward_consumed;
    int drain_complete;
    int request_reaped;
} StreamingCellState;

_Static_assert(sizeof(CoeffPreprocessDescriptor) == 160,
               "coefficient event descriptor ABI mismatch");

typedef enum {
    VECTOR_FROM_HEAP = 0,
    VECTOR_FROM_FORWARD_CONTEXT = 1,
    VECTOR_FROM_BACKWARD_CONTEXT = 2
} VectorSource;

typedef struct {
    double prev_dqstar[N];
    int valid;
    int line_id;
    int cell_id;
} ForwardLineContext;

typedef struct {
    double next_dq[N];
    int valid;
    int line_id;
    int cell_id;
} BackwardLineContext;

static volatile double trsv_calibration_sink;

typedef struct {
    double max_abs_error;
    double max_rel_error;
    uint64_t mismatch_count;
    long first_line;
    long first_cell;
    long first_lane;
    double first_ref;
    double first_got;
} CompareResult;

typedef struct {
    double max_abs_error;
    double max_rel_error;
    double max_residual;
    uint64_t mismatch_count;
} MatrixCompareResult;

typedef struct {
    MatrixCompareResult d_inv;
    MatrixCompareResult l_bar;
    MatrixCompareResult u_bar;
} PretransformCheck;

static inline uint32_t
enc_lmat5_spm(int row, int rs1, int zd)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b0U << 24) |
           ((row & 7) << 21) | (0U << 20) |
           ((rs1 & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_dotp_row(int lane_id, int zra, int zrb)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b1U << 24) | (0b0U << 23) |
           ((zra & 0x1f) << 10) | ((zrb & 0x1f) << 5) | (lane_id & 0x1f);
}

static inline uint32_t
enc_pack_acc(int zd, int xn)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b1U << 24) | (0b1U << 23) |
           ((xn & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_pack_sub_acc(int zd, int base, int xn)
{
    return (0b00000U << 27) | (0b00U << 25) | (0b11U << 23) |
           (0b00001U << 15) | ((base & 0x1f) << 10) |
           ((xn & 0x1f) << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_trsv5_lu_spm(int zd, int lu_base, int rhs_base)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((lu_base & 0x1f) << 15) | ((rhs_base & 0x1f) << 10) |
           (0U << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_trsv5_lu_spm_zrhs(int zd, int lu_base, int zrhs)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((lu_base & 0x1f) << 15) | ((zrhs & 0x1f) << 10) |
           (0b10001U << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_trsm5_mrhs_spm(int lu_base, int rhs_base, int out_base)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((lu_base & 0x1f) << 15) | ((rhs_base & 0x1f) << 10) |
           ((out_base & 0x1f) << 5) | 0b11101U;
}

static inline uint32_t
enc_trsm5_inv_lbar_spm(void)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           (21U << 15) | (22U << 10) | (23U << 5) | 24U;
}

static inline uint32_t
enc_trsm5_coeff3_spm(void)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           (20U << 15) | (21U << 10) | (22U << 5) | 23U;
}

static inline uint32_t
enc_coeff_preprocess_launch(int token_dest, int desc_reg)
{
    return (0b11U << 23) | (0b11101U << 15) |
           ((desc_reg & 0x1f) << 10) | (0b00011U << 5) |
           (token_dest & 0x1f);
}

static inline uint32_t
enc_coeff_preprocess_wait(int status_dest, int token_reg)
{
    return (0b11U << 23) | (0b11100U << 15) |
           ((token_reg & 0x1f) << 10) | (0b00100U << 5) |
           (status_dest & 0x1f);
}

static inline uint32_t
enc_coeff_preprocess_cancel(int status_dest, int token_reg)
{
    return (0b11U << 23) | (0b11011U << 15) |
           ((token_reg & 0x1f) << 10) | (0b00101U << 5) |
           (status_dest & 0x1f);
}

__attribute__((noinline)) static uint64_t
coeff_preprocess_launch(CoeffPreprocessDescriptor *descriptor)
{
#if defined(__aarch64__)
    const uint32_t inst = enc_coeff_preprocess_launch(25, 10);
    uint64_t token;
    __asm__ __volatile__(
        "mov x10, %[descriptor]\n\t"
        ".long %[inst]\n\t"
        "mov %[token], x25\n\t"
        : [token] "=&r"(token)
        : [descriptor] "r"((uint64_t)(uintptr_t)descriptor),
          [inst] "n"(inst)
        : "memory", "cc", "x10", "x25");
    return token;
#else
    (void)descriptor;
    return 0;
#endif
}

__attribute__((noinline)) static uint64_t
coeff_preprocess_wait(uint64_t token)
{
#if defined(__aarch64__)
    const uint32_t inst = enc_coeff_preprocess_wait(25, 26);
    uint64_t status;
    __asm__ __volatile__(
        "mov x26, %[token]\n\t"
        ".long %[inst]\n\t"
        "mov %[status], x25\n\t"
        : [status] "=&r"(status)
        : [token] "r"(token), [inst] "n"(inst)
        : "memory", "cc", "x25", "x26");
    return status;
#else
    (void)token;
    return 5;
#endif
}

__attribute__((noinline)) static uint64_t
coeff_preprocess_cancel(uint64_t token)
{
#if defined(__aarch64__)
    const uint32_t inst = enc_coeff_preprocess_cancel(25, 26);
    uint64_t status;
    __asm__ __volatile__(
        "mov x26, %[token]\n\t"
        ".long %[inst]\n\t"
        "mov %[status], x25\n\t"
        : [status] "=&r"(status)
        : [token] "r"(token), [inst] "n"(inst)
        : "memory", "cc", "x25", "x26");
    return status;
#else
    (void)token;
    return COEFF_PRE_STATUS_BAD_TOKEN;
#endif
}

static inline uint32_t
enc_vec5_sub_z(int zd, int zsrc0, int zsrc1)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((zsrc0 & 0x1f) << 15) | ((zsrc1 & 0x1f) << 10) |
           (0b10110U << 5) | (zd & 0x1f);
}

#define PRETRANSFORM_MVM0_ASM \
    ".long 0x000002a0\n\t" ".long 0x002002a1\n\t" \
    ".long 0x004002a2\n\t" ".long 0x006002a3\n\t" \
    ".long 0x008002a4\n\t" ".long 0x000002e5\n\t" \
    ".long 0x010000a0\n\t" ".long 0x010004a1\n\t" \
    ".long 0x010008a2\n\t" ".long 0x01000ca3\n\t" \
    ".long 0x010010a4\n\t" ".long 0x0180020a\n\t"

#define PRETRANSFORM_MVM1_ASM \
    ".long 0x000002cd\n\t" ".long 0x002002ce\n\t" \
    ".long 0x004002cf\n\t" ".long 0x006002d0\n\t" \
    ".long 0x008002d1\n\t" ".long 0x00000312\n\t" \
    ".long 0x01003640\n\t" ".long 0x01003a41\n\t" \
    ".long 0x01003e42\n\t" ".long 0x01004243\n\t" \
    ".long 0x01004644\n\t" ".long 0x0180020b\n\t"

#define PRETRANSFORM_VEC_SUB_ASM ".long 0x01852ecc\n\t"

static inline uint32_t
enc_linebuf_rd_fwd5(int zd, int xline, int xcell)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((xline & 0x1f) << 15) | ((xcell & 0x1f) << 10) |
           (0b10010U << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_linebuf_wr_fwd5(int zsrc, int xline, int xcell)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((xline & 0x1f) << 15) | ((xcell & 0x1f) << 10) |
           (0b10011U << 5) | (zsrc & 0x1f);
}

static inline uint32_t
enc_linebuf_rd_bwd5(int zd, int xline, int xcell)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((xline & 0x1f) << 15) | ((xcell & 0x1f) << 10) |
           (0b10100U << 5) | (zd & 0x1f);
}

static inline uint32_t
enc_linebuf_wr_bwd5(int zsrc, int xline, int xcell)
{
    return (0b00000U << 27) | (0b00U << 25) |
           (0b11U << 23) | (0U << 22) | (0b00U << 20) |
           ((xline & 0x1f) << 15) | ((xcell & 0x1f) << 10) |
           (0b10101U << 5) | (zsrc & 0x1f);
}

static inline uint64_t
read_cycle_counter(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    __asm__ __volatile__(".long 0xff070110\n\t" : "+r"(x0) :: "memory");
    return x0 * 2U;
#else
    return 0;
#endif
}

static inline uint64_t
read_serialized_cycle_counter(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
    uint64_t value = read_cycle_counter();
    __asm__ __volatile__("isb" ::: "memory");
    return value;
#else
    return read_cycle_counter();
#endif
}

static inline void
full_barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\n\tisb\n\t" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void
spm_input_release(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb st\n\t" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void
gem5_m5_reset_stats(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff400110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
#endif
}

static inline void
gem5_m5_dump_stats(void)
{
#if defined(__aarch64__)
    register uint64_t x0 asm("x0") = 0;
    register uint64_t x1 asm("x1") = 0;
    __asm__ __volatile__(".long 0xff410110\n\t"
                         : "+r"(x0), "+r"(x1) :: "memory");
#endif
}

static void *
xcalloc_raw(size_t count, size_t size, const char *name)
{
    if (size != 0 && count > ((size_t)-1) / size) {
        fprintf(stderr, "allocation overflow for %s\n", name);
        exit(2);
    }
    void *ptr = calloc(count, size);
    if (!ptr) {
        fprintf(stderr, "calloc failed for %s: %s\n", name, strerror(errno));
        exit(2);
    }
    return ptr;
}

static double *
xcalloc(size_t count, size_t size, const char *name)
{
    return (double *)xcalloc_raw(count, size, name);
}

static double *
vec_at(double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N];
}

static const double *
vec_const_at(const double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N];
}

static double *
mat_at(double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N * N];
}

static const double *
mat_const_at(const double *base, const Problem *p, size_t line, size_t cell)
{
    return &base[((line * p->cells) + cell) * N * N];
}

static double *
tmp_slot_at(Problem *p, size_t line, size_t slot)
{
    return &p->tmp_slots[((line * 2U) + (slot & 1U)) * N];
}

static double
trsv5_mul_sub(double acc, double lhs, double rhs)
{
    volatile double product = lhs * rhs;
    return acc - product;
}

static void
trsv5_software(const double lu[N * N], double value[N])
{
    for (int k = 0; k < 4; k++) {
        value[k] /= lu[k * N + k];
        for (int i = k + 1; i < 5; i++)
            value[i] = trsv5_mul_sub(value[i], lu[i * N + k], value[k]);
    }
    value[4] /= lu[4 * N + 4];
    for (int k = 3; k >= 0; k--) {
        for (int i = k + 1; i < 5; i++)
            value[k] = trsv5_mul_sub(value[k], lu[k * N + i], value[i]);
    }
}

static void
trsv5_software_counted(const double lu[N * N], double value[N], Stats *stats)
{
    trsv5_software(lu, value);
    stats->software_trsv++;
    stats->software_trsv_hot_path++;
}

static void
stage_trsv5_inputs_to_spm(const double lu[N * N], const double rhs[N])
{
    volatile double *lu_dst = (volatile double *)(uintptr_t)SPM_TRSV_MAT_BASE;
    volatile double *rhs_dst = (volatile double *)(uintptr_t)SPM_TRSV_RHS_BASE;
    for (int i = 0; i < N * N; i++)
        lu_dst[i] = lu[i];
    for (int lane = 0; lane < N; lane++)
        rhs_dst[lane] = rhs[lane];
    full_barrier();
}

__attribute__((noinline)) static void
trsv5_hardware_counted(const double lu[N * N], const double rhs[N],
                       double result[N], Stats *stats)
{
    stage_trsv5_inputs_to_spm(lu, rhs);

#if defined(__aarch64__)
    const uint32_t trsv = enc_trsv5_lu_spm(11, 21, 23);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[trsv]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
          [rhs] "r"((uint64_t)SPM_TRSV_RHS_BASE),
          [out] "r"((uint64_t)(uintptr_t)result),
          [trsv] "n"(trsv)
        : "memory", "cc", "x11", "x21", "x23", "p0", "z11");
#else
    for (int lane = 0; lane < N; lane++)
        result[lane] = rhs[lane];
    trsv5_software(lu, result);
#endif

    stats->hardware_trsv++;
}

__attribute__((noinline)) static void
linebuf_write5_from_mem(int forward, uint64_t line_id, uint64_t cell_id,
                        const double value[N])
{
#if defined(__aarch64__)
    if (forward) {
        const uint32_t wr = enc_linebuf_wr_fwd5(11, 9, 10);
        __asm__ __volatile__(
            "mov x11, %[value]\n\t"
            "mov x9, %[line]\n\t"
            "mov x10, %[cell]\n\t"
            "ptrue p0.d, vl5\n\t"
            "ld1d {z11.d}, p0/z, [x11]\n\t"
            ".long %[wr]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [value] "r"((uint64_t)(uintptr_t)value),
              [line] "r"(line_id),
              [cell] "r"(cell_id),
              [wr] "n"(wr)
            : "memory", "cc", "x9", "x10", "x11", "p0", "z11");
    } else {
        const uint32_t wr = enc_linebuf_wr_bwd5(11, 9, 10);
        __asm__ __volatile__(
            "mov x11, %[value]\n\t"
            "mov x9, %[line]\n\t"
            "mov x10, %[cell]\n\t"
            "ptrue p0.d, vl5\n\t"
            "ld1d {z11.d}, p0/z, [x11]\n\t"
            ".long %[wr]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [value] "r"((uint64_t)(uintptr_t)value),
              [line] "r"(line_id),
              [cell] "r"(cell_id),
              [wr] "n"(wr)
            : "memory", "cc", "x9", "x10", "x11", "p0", "z11");
    }
#else
    (void)forward;
    (void)line_id;
    (void)cell_id;
    (void)value;
#endif
}

__attribute__((noinline)) static void
trsv5_hardware_linebuf_fwd_counted(const double lu[N * N],
                                   const double rhs[N], double result[N],
                                   Stats *stats, uint64_t line_id,
                                   uint64_t cell_id)
{
    stage_trsv5_inputs_to_spm(lu, rhs);

#if defined(__aarch64__)
    const uint32_t trsv = enc_trsv5_lu_spm(11, 21, 23);
    const uint32_t wr = enc_linebuf_wr_fwd5(11, 9, 10);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x11, %[out]\n\t"
        "mov x9, %[line]\n\t"
        "mov x10, %[cell]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[trsv]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        ".long %[wr]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
          [rhs] "r"((uint64_t)SPM_TRSV_RHS_BASE),
          [out] "r"((uint64_t)(uintptr_t)result),
          [line] "r"(line_id),
          [cell] "r"(cell_id),
          [trsv] "n"(trsv),
          [wr] "n"(wr)
        : "memory", "cc", "x9", "x10", "x11", "x21", "x23",
          "p0", "z11");
#else
    (void)line_id;
    (void)cell_id;
    for (int lane = 0; lane < N; lane++)
        result[lane] = rhs[lane];
    trsv5_software(lu, result);
#endif

    stats->hardware_trsv++;
    stats->linebuf_forwarding_enabled = 1;
    stats->linebuf_forward_writes++;
    stats->linebuf_context_memory_stores_elided++;
}

static void
trsv5_step_counted(const double lu[N * N], double value[N], Stats *stats,
                   int use_hardware_trsv)
{
    if (use_hardware_trsv) {
        double result[N];
        trsv5_hardware_counted(lu, value, result, stats);
        for (int lane = 0; lane < N; lane++)
            value[lane] = result[lane];
    } else {
        trsv5_software_counted(lu, value, stats);
    }
}

static void
mvm5_software(const double mat[N * N], const double vec[N], double out[N])
{
    for (int r = 0; r < N; r++) {
        double acc = 0.0;
        for (int c = 0; c < N; c++)
            acc += mat[r * N + c] * vec[c];
        out[r] = acc;
    }
}

static void
trsm5_mrhs_software(const double lu[N * N], const double rhs[N * N],
                    double out[N * N])
{
    for (int col = 0; col < N; col++) {
        double value[N];
        for (int row = 0; row < N; row++)
            value[row] = rhs[row * N + col];
        trsv5_software(lu, value);
        for (int row = 0; row < N; row++)
            out[row * N + col] = value[row];
    }
}

static void
reconstruct_d_from_packed_lu(const double lu[N * N], double d[N * N])
{
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            double value = 0.0;
            for (int k = 0; k < N; k++) {
                double lower = 0.0;
                double upper = 0.0;
                if (k < row)
                    lower = lu[row * N + k];
                else if (k == row)
                    lower = lu[row * N + row];
                if (col == k)
                    upper = 1.0;
                else if (col > k)
                    upper = lu[k * N + col];
                value += lower * upper;
            }
            d[row * N + col] = value;
        }
    }
}

static int
cfd_lu5_factor(const double d[N * N], double lu[N * N], double epsilon)
{
    memset(lu, 0, N * N * sizeof(double));
    for (int col = 0; col < N; col++) {
        for (int row = col; row < N; row++) {
            double value = d[row * N + col];
            for (int k = 0; k < col; k++)
                value -= lu[row * N + k] * lu[k * N + col];
            lu[row * N + col] = value;
        }
        if (!isfinite(lu[col * N + col]) ||
            fabs(lu[col * N + col]) <= epsilon)
            return 0;
        for (int upper_col = col + 1; upper_col < N; upper_col++) {
            double value = d[col * N + upper_col];
            for (int k = 0; k < col; k++)
                value -= lu[col * N + k] * lu[k * N + upper_col];
            lu[col * N + upper_col] = value / lu[col * N + col];
            if (!isfinite(lu[col * N + upper_col]))
                return 0;
        }
    }
    return 1;
}

static double
lu_factor_residual_max(const double d[N * N], const double lu[N * N])
{
    double reconstructed[N * N];
    double maximum = 0.0;
    reconstruct_d_from_packed_lu(lu, reconstructed);
    for (int i = 0; i < N * N; i++) {
        double residual = fabs(reconstructed[i] - d[i]);
        if (residual > maximum)
            maximum = residual;
    }
    return maximum;
}

static double
matrix_residual_max_relative(const double lu[N * N],
                             const double x[N * N],
                             const double rhs[N * N])
{
    double d[N * N];
    double maximum = 0.0;
    reconstruct_d_from_packed_lu(lu, d);
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            double value = 0.0;
            for (int k = 0; k < N; k++)
                value += d[row * N + k] * x[k * N + col];
            double residual = fabs(value - rhs[row * N + col]) /
                fmax(fabs(rhs[row * N + col]), 1.0e-14);
            if (residual > maximum)
                maximum = residual;
        }
    }
    return maximum;
}

static double
matrix_residual_max(const double lu[N * N], const double x[N * N],
                    const double rhs[N * N])
{
    double d[N * N];
    double max_residual = 0.0;
    reconstruct_d_from_packed_lu(lu, d);
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            double value = 0.0;
            for (int k = 0; k < N; k++)
                value += d[row * N + k] * x[k * N + col];
            double residual = fabs(value - rhs[row * N + col]);
            if (residual > max_residual)
                max_residual = residual;
        }
    }
    return max_residual;
}

static void
update_matrix_compare(MatrixCompareResult *cmp, const double ref[N * N],
                      const double got[N * N], double residual,
                      double abs_tol, double rel_tol)
{
    if (residual > cmp->max_residual)
        cmp->max_residual = residual;
    for (int i = 0; i < N * N; i++) {
        double abs_err = fabs(got[i] - ref[i]);
        double rel_err = abs_err / fmax(fabs(ref[i]), 1.0e-14);
        if (abs_err > cmp->max_abs_error)
            cmp->max_abs_error = abs_err;
        if (rel_err > cmp->max_rel_error)
            cmp->max_rel_error = rel_err;
        if (abs_err > abs_tol && rel_err > rel_tol)
            cmp->mismatch_count++;
    }
}

static void
stage_trsm5_inputs_to_spm(const double lu[N * N],
                          const double rhs[N * N])
{
    volatile double *lu_dst =
        (volatile double *)(uintptr_t)SPM_TRSM5_LU_BASE;
    volatile double *rhs_dst =
        (volatile double *)(uintptr_t)SPM_TRSM5_RHS_BASE;
    for (int i = 0; i < N * N; i++) {
        lu_dst[i] = lu[i];
        rhs_dst[i] = rhs[i];
    }
    full_barrier();
}

__attribute__((noinline)) static void
trsm5_mrhs_hardware(const double lu[N * N], const double rhs[N * N],
                    double out[N * N])
{
    stage_trsm5_inputs_to_spm(lu, rhs);
#if defined(__aarch64__)
    const uint32_t trsm = enc_trsm5_mrhs_spm(21, 23, 24);
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x23, %[rhs]\n\t"
        "mov x24, %[out]\n\t"
        ".long %[trsm]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [lu] "r"((uint64_t)SPM_TRSM5_LU_BASE),
          [rhs] "r"((uint64_t)SPM_TRSM5_RHS_BASE),
          [out] "r"((uint64_t)SPM_TRSM5_OUT_BASE),
          [trsm] "n"(trsm)
        : "memory", "cc", "x21", "x23", "x24");
    volatile const double *src =
        (volatile const double *)(uintptr_t)SPM_TRSM5_OUT_BASE;
    for (int i = 0; i < N * N; i++)
        out[i] = src[i];
#else
    trsm5_mrhs_software(lu, rhs, out);
#endif
}

static int
prepare_raw_common(Problem *p, const Config *cfg, Stats *stats,
                   int generate_ubar)
{
    int all_valid = 1;
    const uint64_t matrix_bytes =
        p->lines * p->cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = p->lines * p->cells * N * sizeof(double);
    stats->raw_input_d_bytes += matrix_bytes;
    stats->raw_input_l_bytes += matrix_bytes;
    stats->raw_input_u_bytes += matrix_bytes;
    stats->raw_input_r_bytes += rhs_bytes;
    stats->coefficient_rebuilds++;
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *d = mat_const_at(p->d_mat, p, line, cell);
            double *lu = mat_at(p->lu_d, p, line, cell);
            uint64_t start = read_cycle_counter();
            int factor_ok = cfd_lu5_factor(
                d, lu, cfg->pretransform_diag_epsilon);
            uint64_t end = read_cycle_counter();
            if (end >= start) {
                stats->lu5_factor_cycles += end - start;
                stats->common_lu_cycles += end - start;
            }
            stats->lu5_factor_cells++;
            stats->lu5_factor_input_bytes += N * N * sizeof(double);
            stats->lu5_factor_output_bytes += N * N * sizeof(double);
            if (!factor_ok) {
                stats->lu5_factor_failures++;
                all_valid = 0;
                continue;
            }
            double lu_residual = lu_factor_residual_max(d, lu);
            if (lu_residual > stats->max_abs_lu_residual)
                stats->max_abs_lu_residual = lu_residual;
            if (lu_residual > 1.0e-9)
                all_valid = 0;

            if (!generate_ubar)
                continue;
            const double *upper = mat_const_at(p->u_mat, p, line, cell);
            double *u_bar = mat_at(p->u_bar, p, line, cell);
            start = read_cycle_counter();
            trsm5_mrhs_hardware(lu, upper, u_bar);
            end = read_cycle_counter();
            if (end >= start) {
                stats->ubar_cycles += end - start;
                stats->common_ubar_cycles += end - start;
            }
            stats->ubar_generated++;
            stats->ubar_trsm_issued++;
            stats->ubar_trsm_completed++;
            stats->ubar_columns_solved += N;
            stats->ubar_stage_bytes += 2U * N * N * sizeof(double);
            stats->ubar_output_bytes += N * N * sizeof(double);
            double abs_residual = matrix_residual_max(lu, u_bar, upper);
            double rel_residual =
                matrix_residual_max_relative(lu, u_bar, upper);
            if (abs_residual > stats->max_abs_ubar_residual)
                stats->max_abs_ubar_residual = abs_residual;
            if (rel_residual > stats->max_rel_ubar_residual)
                stats->max_rel_ubar_residual = rel_residual;
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(u_bar[i])) {
                    stats->ubar_invalid++;
                    all_valid = 0;
                    break;
                }
            }
            if (abs_residual > 1.0e-9)
                all_valid = 0;
        }
    }
    stats->common_preprocess_cycles =
        stats->common_lu_cycles + stats->common_ubar_cycles;
    stats->raw_persistent_coefficient_bytes =
        2U * p->lines * p->cells * N * N * sizeof(double);
    return all_valid;
}

static int
preprocess_coefficients(Problem *p, const Config *cfg, Stats *stats,
                        PretransformCheck *check)
{
    const double identity[N * N] = {
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, 1, 0,
        0, 0, 0, 0, 1
    };
    int all_valid = 1;
    uint64_t start = read_cycle_counter();
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *lu = mat_const_at(p->lu_a, p, line, cell);
            const double *lower = mat_const_at(p->c_mat, p, line, cell);
            double *d_inv = mat_at(p->d_inv, p, line, cell);
            double *l_bar = mat_at(p->l_bar_precomputed, p, line, cell);
            double d_ref[N * N];
            double l_ref[N * N];
            int valid = 1;
            for (int lane = 0; lane < N; lane++) {
                if (!isfinite(lu[lane * N + lane]) ||
                    fabs(lu[lane * N + lane]) <=
                        cfg->pretransform_diag_epsilon)
                    valid = 0;
            }

            trsm5_mrhs_software(lu, identity, d_ref);
            trsm5_mrhs_software(lu, lower, l_ref);
            trsm5_mrhs_hardware(lu, identity, d_inv);
            stats->trsm5_mrhs_issued++;
            stats->trsm5_mrhs_completed++;
            stats->trsm5_mrhs_inv_batches++;
            trsm5_mrhs_hardware(lu, lower, l_bar);
            stats->trsm5_mrhs_issued++;
            stats->trsm5_mrhs_completed++;
            stats->trsm5_mrhs_lbar_batches++;
            stats->trsm5_mrhs_columns_solved += 10;
            stats->trsm5_mrhs_input_lu_bytes += 2U * N * N * sizeof(double);
            stats->trsm5_mrhs_input_rhs_bytes += 2U * N * N * sizeof(double);
            stats->trsm5_mrhs_output_bytes += 2U * N * N * sizeof(double);
            stats->preprocess_matrices_generated += 2;
            stats->ubar_reused_existing++;

            double d_residual = matrix_residual_max(lu, d_inv, identity);
            double l_residual = matrix_residual_max(lu, l_bar, lower);
            update_matrix_compare(&check->d_inv, d_ref, d_inv, d_residual,
                                  1.0e-12, 1.0e-12);
            update_matrix_compare(&check->l_bar, l_ref, l_bar, l_residual,
                                  1.0e-12, 1.0e-12);
            update_matrix_compare(&check->u_bar,
                                  mat_const_at(p->b_bar, p, line, cell),
                                  mat_const_at(p->u_bar_precomputed, p, line,
                                               cell),
                                  0.0, 0.0, 0.0);
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(d_inv[i]) || !isfinite(l_bar[i]) ||
                    !isfinite(p->u_bar_precomputed[
                        ((line * p->cells + cell) * N * N) + i])) {
                    valid = 0;
                    stats->pretransform_nan_inf++;
                }
            }
            if (d_residual > 1.0e-9 || l_residual > 1.0e-9)
                valid = 0;
            p->pretransform_valid[line * p->cells + cell] = valid ? 1 : 0;
            if (valid)
                stats->pretransform_valid_cells++;
            else {
                stats->pretransform_invalid_cells++;
                stats->trsm5_mrhs_invalid++;
                all_valid = 0;
            }
            stats->preprocess_cells++;
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->preprocess_cycles += end - start;
    stats->trsm5_mrhs_busy_cycles =
        stats->trsm5_mrhs_issued * cfg->trsm5_mrhs_lat;
    return all_valid;
}

static uintptr_t
pretransform_slot_base(uint64_t slot)
{
    return slot ? SPM_TRSM_OPT_SLOT1_BASE : SPM_TRSM_OPT_SLOT0_BASE;
}

static void
stage_optprep_inputs(const double lu[N * N], const double c[N * N],
                     uintptr_t base, const Config *cfg, Stats *stats)
{
    uint64_t start = read_cycle_counter();
    volatile double *lu_dst = (volatile double *)base;
    volatile double *c_dst =
        (volatile double *)(base + SPM_TRSM_OPT_C_OFFSET);
    for (int i = 0; i < N * N; i++) {
        lu_dst[i] = lu[i];
        c_dst[i] = c[i];
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->preprocess_stage_cycles += end - start;
    stats->preprocess_lu_stages++;
    stats->preprocess_c_stages++;
    stats->preprocess_lu_stage_bytes += N * N * sizeof(double);
    stats->preprocess_c_stage_bytes += N * N * sizeof(double);
    if (cfg->pretransform_use_barrier) {
        start = read_cycle_counter();
        full_barrier();
        end = read_cycle_counter();
        if (end >= start)
            stats->preprocess_barrier_cycles += end - start;
        stats->pretransform_barriers_issued++;
    } else {
        spm_input_release();
        stats->pretransform_barriers_elided++;
    }
}

__attribute__((noinline)) static uint64_t
trsm5_inv_lbar_hardware(uintptr_t lu_addr, uintptr_t c_addr,
                        uintptr_t d_inv_addr, uintptr_t l_bar_addr,
                        uint64_t input_token)
{
#if defined(__aarch64__)
    const uint32_t inst = enc_trsm5_inv_lbar_spm();
    uint64_t completion_token;
    __asm__ __volatile__(
        "mov x21, %[lu]\n\t"
        "mov x22, %[c]\n\t"
        "mov x23, %[dinv]\n\t"
        "mov x24, %[lbar]\n\t"
        "mov x26, %[token]\n\t"
        ".long %[inst]\n\t"
        "mov %[done], x25\n\t"
        : [done] "=&r"(completion_token)
        : [lu] "r"((uint64_t)lu_addr), [c] "r"((uint64_t)c_addr),
          [dinv] "r"((uint64_t)d_inv_addr),
          [lbar] "r"((uint64_t)l_bar_addr),
          [token] "r"(input_token), [inst] "n"(inst)
        : "memory", "cc", "x21", "x22", "x23", "x24", "x25", "x26");
    return completion_token;
#else
    const double *lu = (const double *)lu_addr;
    const double *c = (const double *)c_addr;
    double *d_inv = (double *)d_inv_addr;
    double *l_bar = (double *)l_bar_addr;
    double identity[N * N] = {};
    for (int i = 0; i < N; i++)
        identity[i * N + i] = 1.0;
    trsm5_mrhs_software(lu, identity, d_inv);
    trsm5_mrhs_software(lu, c, l_bar);
    return input_token + 1;
#endif
}

static void
drain_optprep_entry(Problem *p, PretransformCoefficientEntry *entry,
                    Stats *stats)
{
    if (!entry->valid)
        return;
    uint64_t start = read_cycle_counter();
    const uintptr_t token_offset = (uintptr_t)
        (entry->completion_token - (entry->generation + 1));
    volatile const double *d_src = (volatile const double *)
        (entry->d_inv_addr + token_offset);
    volatile const double *l_src = (volatile const double *)
        (entry->l_bar_addr + token_offset);
    double *d_dst = mat_at(p->d_inv, p, entry->line_id, entry->cell_id);
    double *l_dst =
        mat_at(p->l_bar_precomputed, p, entry->line_id, entry->cell_id);
    for (int i = 0; i < N * N; i++) {
        d_dst[i] = d_src[i];
        l_dst[i] = l_src[i];
    }
    uint64_t end = read_cycle_counter();
    if (end >= start) {
        stats->preprocess_drain_cycles += end - start;
        stats->pretransform_output_drain_cycles += end - start;
    }
    stats->preprocess_output_heap_bytes += 2U * N * N * sizeof(double);
    stats->pretransform_output_buffer_free++;
    entry->valid = 0;
}

static int
validate_optprep_coefficients(Problem *p, const Config *cfg, Stats *stats,
                              PretransformCheck *check)
{
    const int full = !strcmp(cfg->pretransform_validate, "full");
    int all_valid = 1;
    uint64_t validate_start = read_cycle_counter();
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *lu = mat_const_at(p->lu_a, p, line, cell);
            const double *c = mat_const_at(p->c_mat, p, line, cell);
            const double *d_inv = mat_const_at(p->d_inv, p, line, cell);
            const double *l_bar =
                mat_const_at(p->l_bar_precomputed, p, line, cell);
            int valid = 1;
            uint64_t fast_start = read_cycle_counter();
            for (int lane = 0; lane < N; lane++) {
                if (!isfinite(lu[lane * N + lane]) ||
                    fabs(lu[lane * N + lane]) <=
                        cfg->pretransform_diag_epsilon)
                    valid = 0;
            }
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(d_inv[i]) || !isfinite(l_bar[i]) ||
                    !isfinite(p->u_bar_precomputed[
                        ((line * p->cells + cell) * N * N) + i])) {
                    valid = 0;
                    stats->pretransform_nan_inf++;
                }
            }
            uint64_t fast_end = read_cycle_counter();
            if (fast_end >= fast_start)
                stats->preprocess_fast_check_cycles += fast_end - fast_start;

            if (full) {
                double identity[N * N] = {};
                double d_ref[N * N];
                double l_ref[N * N];
                for (int i = 0; i < N; i++)
                    identity[i * N + i] = 1.0;
                uint64_t residual_start = read_cycle_counter();
                trsm5_mrhs_software(lu, identity, d_ref);
                trsm5_mrhs_software(lu, c, l_ref);
                double d_residual =
                    matrix_residual_max(lu, d_inv, identity);
                double l_residual = matrix_residual_max(lu, l_bar, c);
                update_matrix_compare(&check->d_inv, d_ref, d_inv,
                                      d_residual, 1.0e-12, 1.0e-12);
                update_matrix_compare(&check->l_bar, l_ref, l_bar,
                                      l_residual, 1.0e-12, 1.0e-12);
                update_matrix_compare(
                    &check->u_bar,
                    mat_const_at(p->b_bar, p, line, cell),
                    mat_const_at(p->u_bar_precomputed, p, line, cell),
                    0.0, 0.0, 0.0);
                if (d_residual > 1.0e-9 || l_residual > 1.0e-9)
                    valid = 0;
                uint64_t residual_end = read_cycle_counter();
                if (residual_end >= residual_start)
                    stats->preprocess_residual_check_cycles +=
                        residual_end - residual_start;
            }
            p->pretransform_valid[line * p->cells + cell] = valid ? 1 : 0;
            if (valid)
                stats->pretransform_valid_cells++;
            else {
                stats->pretransform_invalid_cells++;
                all_valid = 0;
            }
            stats->preprocess_cells++;
            stats->preprocess_matrices_generated += 2;
            stats->ubar_reused_existing++;
        }
    }
    uint64_t validate_end = read_cycle_counter();
    if (validate_end >= validate_start)
        stats->preprocess_validation_cycles +=
            validate_end - validate_start;
    return all_valid;
}

static int
preprocess_coefficients_optprep(Problem *p, const Config *cfg, Stats *stats,
                                PretransformCheck *check)
{
    uint64_t total_start = read_cycle_counter();
    const uint64_t slots = cfg->pretransform_spm_slots > 1 ? 2 : 1;
    const uint64_t depth = cfg->pretransform_output_buffer_enable ?
        cfg->pretransform_output_buffer_depth : 0;
    PretransformCoefficientEntry *buffer = depth ?
        (PretransformCoefficientEntry *)xcalloc_raw(
            depth, sizeof(*buffer), "pretransform_output_buffer") : NULL;
    PretransformSpmSlot slot_state[2] = {{0}};
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t occupancy = 0;
    uint64_t generation = 1;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++, generation++) {
            const uint64_t slot_id = (generation - 1) % slots;
            PretransformSpmSlot *slot = &slot_state[slot_id];
            if (slot->busy)
                stats->pretransform_input_token_stalls++;
            const uintptr_t base = pretransform_slot_base(slot_id);
            stage_optprep_inputs(mat_const_at(p->lu_a, p, line, cell),
                                 mat_const_at(p->c_mat, p, line, cell),
                                 base, cfg, stats);
            slot->valid = 1;
            slot->busy = 1;
            slot->generation = generation;
            slot->line_id = line;
            slot->cell_id = cell;

            if (depth && occupancy == depth) {
                stats->pretransform_output_buffer_full_stalls++;
                drain_optprep_entry(p, &buffer[head], stats);
                head = (head + 1) % depth;
                occupancy--;
            }

            double *d_inv_out;
            double *l_bar_out;
            if (depth) {
                PretransformCoefficientEntry *entry = &buffer[tail];
                const uintptr_t output_base =
                    SPM_TRSM_OPT_BUFFER_BASE +
                    tail * SPM_TRSM_OPT_BUFFER_STRIDE;
                entry->d_inv_addr = output_base;
                entry->l_bar_addr =
                    output_base + N * N * sizeof(double);
                d_inv_out = (double *)entry->d_inv_addr;
                l_bar_out = (double *)entry->l_bar_addr;
                entry->line_id = line;
                entry->cell_id = cell;
                entry->generation = generation;
            } else {
                d_inv_out = (double *)(base + SPM_TRSM_OPT_DINV_OFFSET);
                l_bar_out = (double *)(base + SPM_TRSM_OPT_LBAR_OFFSET);
            }

            uint64_t compute_start = read_cycle_counter();
            uint64_t completion_token = trsm5_inv_lbar_hardware(
                base, base + SPM_TRSM_OPT_C_OFFSET,
                (uintptr_t)d_inv_out, (uintptr_t)l_bar_out, generation);
            uint64_t compute_end = read_cycle_counter();
            if (compute_end >= compute_start)
                stats->preprocess_compute_cycles +=
                    compute_end - compute_start;
            if (completion_token != generation + 1)
                stats->pretransform_output_token_stalls++;
            stats->trsm5_inv_lbar_issued++;
            stats->trsm5_inv_lbar_completed++;
            stats->trsm5_inv_lbar_lu_reads++;
            stats->trsm5_inv_lbar_columns_solved += 10;
            slot->busy = 0;

            if (depth) {
                buffer[tail].completion_token = completion_token;
                buffer[tail].valid = 1;
                tail = (tail + 1) % depth;
                occupancy++;
                stats->pretransform_output_buffer_alloc++;
                stats->preprocess_output_spm_bytes +=
                    2U * N * N * sizeof(double);
                if (occupancy > stats->pretransform_output_buffer_max_occupancy)
                    stats->pretransform_output_buffer_max_occupancy = occupancy;
                /* Keep one completed entry queued so compute and drain remain
                 * decoupled without driving the normal depth-4 case full. */
                if (depth > 1 && occupancy > 1) {
                    drain_optprep_entry(p, &buffer[head], stats);
                    head = (head + 1) % depth;
                    occupancy--;
                }
            } else {
                uint64_t drain_start = read_cycle_counter();
                const uintptr_t token_offset = (uintptr_t)
                    (completion_token - (generation + 1));
                volatile const double *d_src =
                    (volatile const double *)
                    ((uintptr_t)d_inv_out + token_offset);
                volatile const double *l_src =
                    (volatile const double *)
                    ((uintptr_t)l_bar_out + token_offset);
                double *d_dst = mat_at(p->d_inv, p, line, cell);
                double *l_dst = mat_at(p->l_bar_precomputed, p, line, cell);
                for (int i = 0; i < N * N; i++) {
                    d_dst[i] = d_src[i];
                    l_dst[i] = l_src[i];
                }
                uint64_t drain_end = read_cycle_counter();
                if (drain_end >= drain_start) {
                    stats->preprocess_drain_cycles +=
                        drain_end - drain_start;
                    stats->pretransform_output_drain_cycles +=
                        drain_end - drain_start;
                }
                stats->preprocess_output_spm_bytes +=
                    2U * N * N * sizeof(double);
                stats->preprocess_output_heap_bytes +=
                    2U * N * N * sizeof(double);
            }
        }
    }
    while (occupancy) {
        drain_optprep_entry(p, &buffer[head], stats);
        head = (head + 1) % depth;
        occupancy--;
    }
    free(buffer);

    int all_valid = validate_optprep_coefficients(p, cfg, stats, check);
    stats->trsm5_inv_lbar_busy_cycles =
        stats->trsm5_inv_lbar_issued * cfg->trsm5_inv_lbar_lat;
    uint64_t total_end = read_cycle_counter();
    if (total_end >= total_start)
        stats->preprocess_cycles += total_end - total_start;
    uint64_t accounted = stats->preprocess_stage_cycles +
        stats->preprocess_compute_cycles + stats->preprocess_drain_cycles +
        stats->preprocess_validation_cycles + stats->preprocess_barrier_cycles;
    if (stats->preprocess_cycles > accounted)
        stats->preprocess_idle_cycles = stats->preprocess_cycles - accounted;
    /* The current instruction is non-speculative and serialized. Ping/pong
     * slots and the output queue remove redundant staging/backpressure, but
     * do not prove temporal overlap; report overlap as zero until the
     * event-driven implementation supplies completion timestamps. */
    (void)slots;
    return all_valid;
}

static void
stage_patha_inputs_to_spm_with_source(const double matrix[N * N],
                                      const double vector[N],
                                      VectorSource source)
{
    (void)source;
    volatile double *mat_dst = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    volatile double *vec_dst = (volatile double *)(uintptr_t)SPM_PING_VEC_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            mat_dst[r * SPM_STRIDE_ELEMS + c] = matrix[r * N + c];
    }
    for (int c = 0; c < N; c++)
        vec_dst[c] = vector[c];
    full_barrier();
}

static void
stage_patha_inputs_to_spm(const double matrix[N * N], const double vector[N])
{
    stage_patha_inputs_to_spm_with_source(matrix, vector, VECTOR_FROM_HEAP);
}

static void
stage_patha_inputs_to_spm_from_context(const double matrix[N * N],
                                       const double vector[N],
                                       VectorSource source)
{
    stage_patha_inputs_to_spm_with_source(matrix, vector, source);
}

static void
stage_patha_and_trsv_lu_to_spm_with_source(const double matrix[N * N],
                                           const double vector[N],
                                           const double lu[N * N],
                                           VectorSource source)
{
    (void)source;
    volatile double *mat_dst = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    volatile double *vec_dst = (volatile double *)(uintptr_t)SPM_PING_VEC_BASE;
    volatile double *lu_dst = (volatile double *)(uintptr_t)SPM_TRSV_MAT_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            mat_dst[r * SPM_STRIDE_ELEMS + c] = matrix[r * N + c];
    }
    for (int c = 0; c < N; c++)
        vec_dst[c] = vector[c];
    for (int i = 0; i < N * N; i++)
        lu_dst[i] = lu[i];
    full_barrier();
}

static void
stage_patha_matrix_to_spm(const double matrix[N * N])
{
    volatile double *mat_dst = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            mat_dst[r * SPM_STRIDE_ELEMS + c] = matrix[r * N + c];
    }
    full_barrier();
}

static void
stage_patha_matrix_and_trsv_lu_to_spm(const double matrix[N * N],
                                      const double lu[N * N])
{
    volatile double *mat_dst = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    volatile double *lu_dst = (volatile double *)(uintptr_t)SPM_TRSV_MAT_BASE;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++)
            mat_dst[r * SPM_STRIDE_ELEMS + c] = matrix[r * N + c];
    }
    for (int i = 0; i < N * N; i++)
        lu_dst[i] = lu[i];
    full_barrier();
}

__attribute__((noinline)) static void
patha_mvm5_existing(const double matrix[N * N], const double vector[N],
                    double result[N])
{
    stage_patha_inputs_to_spm(matrix, vector);

#if defined(__aarch64__)
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv = enc_lmat5_spm(0, 23, 5);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack = enc_pack_acc(11, 16);

    __asm__ __volatile__(
        "mov x21, %[mat]\n\t"
        "mov x23, %[vec]\n\t"
        "mov x11, %[out]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
        ".long %[lm3]\n\t" ".long %[lm4]\n\t"
        ".long %[lv]\n\t"
        ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
        ".long %[dp3]\n\t" ".long %[dp4]\n\t"
        ".long %[pack]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
          [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
          [out] "r"((uint64_t)(uintptr_t)result),
          [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
          [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
          [lv] "n"(lv),
          [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
          [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
          [pack] "n"(pack)
        : "memory", "cc",
          "x11", "x16", "x17", "x18", "x19", "x20", "x21", "x23",
          "p0", "z0", "z1", "z2", "z3", "z4", "z5", "z11");
#else
    mvm5_software(matrix, vector, result);
#endif
}

static void
stage_pretransform_dual_mvm(const double matrix0[N * N],
                            const double vector0[N],
                            const double matrix1[N * N],
                            const double vector1[N])
{
    volatile double *mat0 = (volatile double *)(uintptr_t)SPM_PING_MAT_BASE;
    volatile double *vec0 = (volatile double *)(uintptr_t)SPM_PING_VEC_BASE;
    volatile double *mat1 = (volatile double *)(uintptr_t)SPM_MVM1_MAT_BASE;
    volatile double *vec1 = (volatile double *)(uintptr_t)SPM_MVM1_VEC_BASE;
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            mat0[row * SPM_STRIDE_ELEMS + col] = matrix0[row * N + col];
            mat1[row * SPM_STRIDE_ELEMS + col] = matrix1[row * N + col];
        }
        vec0[row] = vector0[row];
        vec1[row] = vector1[row];
    }
    full_barrier();
}

__attribute__((noinline)) static void
patha_pretransform_forward_update(const double d_inv[N * N],
                                  const double rhs[N],
                                  const double l_bar[N * N],
                                  const double prev[N], double result[N],
                                  double context_result[N])
{
    stage_pretransform_dual_mvm(d_inv, rhs, l_bar, prev);
#if defined(__aarch64__)
    if (context_result) {
        __asm__ __volatile__(
            "mov x21, %[mat0]\n\t" "mov x23, %[vec0]\n\t"
            "mov x22, %[mat1]\n\t" "mov x24, %[vec1]\n\t"
            "mov x11, %[out]\n\t" "mov x10, %[ctx]\n\t"
            "ptrue p0.d, vl5\n\t"
            PRETRANSFORM_MVM0_ASM
            PRETRANSFORM_MVM1_ASM
            PRETRANSFORM_VEC_SUB_ASM
            "st1d {z12.d}, p0, [x11]\n\t"
            "st1d {z12.d}, p0, [x10]\n\t"
            "dsb sy\n\t" "isb\n\t"
            :
            : [mat0] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec0] "r"((uint64_t)SPM_PING_VEC_BASE),
              [mat1] "r"((uint64_t)SPM_MVM1_MAT_BASE),
              [vec1] "r"((uint64_t)SPM_MVM1_VEC_BASE),
              [out] "r"((uint64_t)(uintptr_t)result),
              [ctx] "r"((uint64_t)(uintptr_t)context_result)
            : "memory", "cc", "x10", "x11", "x16", "x17", "x18",
              "x19", "x20", "x21", "x22", "x23", "x24", "p0",
              "z0", "z1", "z2", "z3", "z4", "z5", "z10", "z11",
              "z12", "z13", "z14", "z15", "z16", "z17", "z18");
    } else {
        __asm__ __volatile__(
            "mov x21, %[mat0]\n\t" "mov x23, %[vec0]\n\t"
            "mov x22, %[mat1]\n\t" "mov x24, %[vec1]\n\t"
            "mov x11, %[out]\n\t" "ptrue p0.d, vl5\n\t"
            PRETRANSFORM_MVM0_ASM
            PRETRANSFORM_MVM1_ASM
            PRETRANSFORM_VEC_SUB_ASM
            "st1d {z12.d}, p0, [x11]\n\t"
            "dsb sy\n\t" "isb\n\t"
            :
            : [mat0] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec0] "r"((uint64_t)SPM_PING_VEC_BASE),
              [mat1] "r"((uint64_t)SPM_MVM1_MAT_BASE),
              [vec1] "r"((uint64_t)SPM_MVM1_VEC_BASE),
              [out] "r"((uint64_t)(uintptr_t)result)
            : "memory", "cc", "x11", "x16", "x17", "x18", "x19",
              "x20", "x21", "x22", "x23", "x24", "p0", "z0", "z1",
              "z2", "z3", "z4", "z5", "z10", "z11", "z12", "z13",
              "z14", "z15", "z16", "z17", "z18");
    }
#else
    double t0[N];
    double t1[N];
    mvm5_software(d_inv, rhs, t0);
    mvm5_software(l_bar, prev, t1);
    for (int lane = 0; lane < N; lane++) {
        result[lane] = t0[lane] - t1[lane];
        if (context_result)
            context_result[lane] = result[lane];
    }
#endif
}

static void
stage_pretransform_lbar_mvm(const double l_bar[N * N],
                            const double prev[N])
{
    volatile double *mat =
        (volatile double *)(uintptr_t)SPM_MVM1_MAT_BASE;
    volatile double *vec =
        (volatile double *)(uintptr_t)SPM_MVM1_VEC_BASE;
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++)
            mat[row * SPM_STRIDE_ELEMS + col] = l_bar[row * N + col];
        vec[row] = prev[row];
    }
    full_barrier();
}

__attribute__((noinline)) static void
patha_pretransform_forward_from_base(const double base[N],
                                     const double l_bar[N * N],
                                     const double prev[N], double result[N],
                                     double context_result[N],
                                     double correction_result[N])
{
    stage_pretransform_lbar_mvm(l_bar, prev);
#if defined(__aarch64__)
    if (context_result) {
        __asm__ __volatile__(
            "mov x25, %[base]\n\t" "mov x22, %[mat1]\n\t"
            "mov x24, %[vec1]\n\t" "mov x11, %[out]\n\t"
            "mov x10, %[ctx]\n\t" "ptrue p0.d, vl5\n\t"
            "mov x9, %[correction]\n\t"
            "ld1d {z10.d}, p0/z, [x25]\n\t"
            PRETRANSFORM_MVM1_ASM
            "st1d {z11.d}, p0, [x9]\n\t"
            PRETRANSFORM_VEC_SUB_ASM
            "st1d {z12.d}, p0, [x11]\n\t"
            "st1d {z12.d}, p0, [x10]\n\t"
            "dsb sy\n\t" "isb\n\t"
            :
            : [base] "r"((uint64_t)(uintptr_t)base),
              [mat1] "r"((uint64_t)SPM_MVM1_MAT_BASE),
              [vec1] "r"((uint64_t)SPM_MVM1_VEC_BASE),
              [out] "r"((uint64_t)(uintptr_t)result),
              [ctx] "r"((uint64_t)(uintptr_t)context_result),
              [correction] "r"((uint64_t)(uintptr_t)correction_result)
            : "memory", "cc", "x9", "x10", "x11", "x16", "x17", "x18",
              "x19", "x20", "x22", "x24", "x25", "p0", "z10",
              "z11", "z12", "z13", "z14", "z15", "z16", "z17",
              "z18");
    } else {
        __asm__ __volatile__(
            "mov x25, %[base]\n\t" "mov x22, %[mat1]\n\t"
            "mov x24, %[vec1]\n\t" "mov x11, %[out]\n\t"
            "mov x9, %[correction]\n\t"
            "ptrue p0.d, vl5\n\t"
            "ld1d {z10.d}, p0/z, [x25]\n\t"
            PRETRANSFORM_MVM1_ASM
            "st1d {z11.d}, p0, [x9]\n\t"
            PRETRANSFORM_VEC_SUB_ASM
            "st1d {z12.d}, p0, [x11]\n\t"
            "dsb sy\n\t" "isb\n\t"
            :
            : [base] "r"((uint64_t)(uintptr_t)base),
              [mat1] "r"((uint64_t)SPM_MVM1_MAT_BASE),
              [vec1] "r"((uint64_t)SPM_MVM1_VEC_BASE),
              [out] "r"((uint64_t)(uintptr_t)result),
              [correction] "r"((uint64_t)(uintptr_t)correction_result)
            : "memory", "cc", "x9", "x11", "x16", "x17", "x18", "x19",
              "x20", "x22", "x24", "x25", "p0", "z10", "z11",
              "z12", "z13", "z14", "z15", "z16", "z17", "z18");
    }
#else
    double lbar_result[N];
    mvm5_software(l_bar, prev, lbar_result);
    for (int lane = 0; lane < N; lane++) {
        correction_result[lane] = lbar_result[lane];
        result[lane] = base[lane] - lbar_result[lane];
        if (context_result)
            context_result[lane] = result[lane];
    }
#endif
}

__attribute__((noinline)) static void
patha_pretransform_backward_update(const double u_bar[N * N],
                                   const double next[N],
                                   const double dq_star[N], double result[N],
                                   double context_result[N])
{
    stage_patha_inputs_to_spm(u_bar, next);
#if defined(__aarch64__)
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv = enc_lmat5_spm(0, 23, 5);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack = enc_pack_acc(11, 16);
    const uint32_t sub = enc_vec5_sub_z(12, 10, 11);
    if (context_result) {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t" "mov x23, %[vec]\n\t"
            "mov x10, %[base]\n\t" "mov x11, %[out]\n\t"
            "mov x12, %[ctx]\n\t" "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t" ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t" ".long %[pack]\n\t"
            "ld1d {z10.d}, p0/z, [x10]\n\t" ".long %[sub]\n\t"
            "st1d {z12.d}, p0, [x11]\n\t"
            "st1d {z12.d}, p0, [x12]\n\t" "dsb sy\n\t" "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)dq_star),
              [out] "r"((uint64_t)(uintptr_t)result),
              [ctx] "r"((uint64_t)(uintptr_t)context_result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]), [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]), [pack] "n"(pack),
              [sub] "n"(sub)
            : "memory", "cc", "x10", "x11", "x12", "x16", "x17",
              "x18", "x19", "x20", "x21", "x23", "p0", "z0", "z1",
              "z2", "z3", "z4", "z5", "z10", "z11", "z12");
    } else {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t" "mov x23, %[vec]\n\t"
            "mov x10, %[base]\n\t" "mov x11, %[out]\n\t"
            "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t" ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t" ".long %[pack]\n\t"
            "ld1d {z10.d}, p0/z, [x10]\n\t" ".long %[sub]\n\t"
            "st1d {z12.d}, p0, [x11]\n\t" "dsb sy\n\t" "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)dq_star),
              [out] "r"((uint64_t)(uintptr_t)result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]), [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]), [pack] "n"(pack),
              [sub] "n"(sub)
            : "memory", "cc", "x10", "x11", "x16", "x17", "x18",
              "x19", "x20", "x21", "x23", "p0", "z0", "z1", "z2",
              "z3", "z4", "z5", "z10", "z11", "z12");
    }
#else
    double t2[N];
    mvm5_software(u_bar, next, t2);
    for (int lane = 0; lane < N; lane++) {
        result[lane] = dq_star[lane] - t2[lane];
        if (context_result)
            context_result[lane] = result[lane];
    }
#endif
}

__attribute__((noinline)) static void
patha_mvm_sub5_fused_from_source(const double matrix[N * N],
                                 const double vector[N],
                                 const double base[N], double result[N],
                                 VectorSource vector_source,
                                 double context_result[N])
{
    stage_patha_inputs_to_spm_from_context(matrix, vector, vector_source);

#if defined(__aarch64__)
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv = enc_lmat5_spm(0, 23, 5);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack_sub = enc_pack_sub_acc(11, 12, 16);

    if (context_result) {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t"
            "mov x23, %[vec]\n\t"
            "mov x12, %[base]\n\t"
            "mov x11, %[out]\n\t"
            "mov x10, %[ctx]\n\t"
            "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t"
            ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t"
            ".long %[pack_sub]\n\t"
            "st1d {z11.d}, p0, [x11]\n\t"
            "st1d {z11.d}, p0, [x10]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)base),
              [out] "r"((uint64_t)(uintptr_t)result),
              [ctx] "r"((uint64_t)(uintptr_t)context_result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
              [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
              [pack_sub] "n"(pack_sub)
            : "memory", "cc",
              "x10", "x11", "x12", "x16", "x17", "x18", "x19", "x20",
              "x21", "x23", "p0", "z0", "z1", "z2", "z3", "z4",
              "z5", "z11");
    } else {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t"
            "mov x23, %[vec]\n\t"
            "mov x12, %[base]\n\t"
            "mov x11, %[out]\n\t"
            "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t"
            ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t"
            ".long %[pack_sub]\n\t"
            "st1d {z11.d}, p0, [x11]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)base),
              [out] "r"((uint64_t)(uintptr_t)result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
              [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
              [pack_sub] "n"(pack_sub)
            : "memory", "cc",
              "x11", "x12", "x16", "x17", "x18", "x19", "x20",
              "x21", "x23", "p0", "z0", "z1", "z2", "z3", "z4",
              "z5", "z11");
    }
#else
    double mvm[N];
    mvm5_software(matrix, vector, mvm);
    for (int lane = 0; lane < N; lane++) {
        result[lane] = base[lane] - mvm[lane];
        if (context_result)
            context_result[lane] = result[lane];
    }
#endif
}

__attribute__((noinline)) static void
patha_mvm_sub5_trsv5_forwarded_from_source(const double matrix[N * N],
                                           const double vector[N],
                                           const double base[N],
                                           const double lu[N * N],
                                           double result[N], Stats *stats,
                                           VectorSource vector_source,
                                           double context_result[N])
{
    stage_patha_and_trsv_lu_to_spm_with_source(matrix, vector, lu,
                                               vector_source);

#if defined(__aarch64__)
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t lv = enc_lmat5_spm(0, 23, 5);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack_sub = enc_pack_sub_acc(11, 12, 16);
    const uint32_t trsv_zrhs = enc_trsv5_lu_spm_zrhs(11, 22, 11);

    if (context_result) {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t"
            "mov x23, %[vec]\n\t"
            "mov x12, %[base]\n\t"
            "mov x22, %[lu]\n\t"
            "mov x11, %[out]\n\t"
            "mov x10, %[ctx]\n\t"
            "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t"
            ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t"
            ".long %[pack_sub]\n\t"
            ".long %[trsv_zrhs]\n\t"
            "st1d {z11.d}, p0, [x11]\n\t"
            "st1d {z11.d}, p0, [x10]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)base),
              [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
              [out] "r"((uint64_t)(uintptr_t)result),
              [ctx] "r"((uint64_t)(uintptr_t)context_result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
              [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
              [pack_sub] "n"(pack_sub),
              [trsv_zrhs] "n"(trsv_zrhs)
            : "memory", "cc",
              "x10", "x11", "x12", "x16", "x17", "x18", "x19", "x20",
              "x21", "x22", "x23", "p0", "z0", "z1", "z2", "z3",
              "z4", "z5", "z11");
    } else {
        __asm__ __volatile__(
            "mov x21, %[mat]\n\t"
            "mov x23, %[vec]\n\t"
            "mov x12, %[base]\n\t"
            "mov x22, %[lu]\n\t"
            "mov x11, %[out]\n\t"
            "ptrue p0.d, vl5\n\t"
            ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
            ".long %[lm3]\n\t" ".long %[lm4]\n\t"
            ".long %[lv]\n\t"
            ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
            ".long %[dp3]\n\t" ".long %[dp4]\n\t"
            ".long %[pack_sub]\n\t"
            ".long %[trsv_zrhs]\n\t"
            "st1d {z11.d}, p0, [x11]\n\t"
            "dsb sy\n\t"
            "isb\n\t"
            :
            : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
              [vec] "r"((uint64_t)SPM_PING_VEC_BASE),
              [base] "r"((uint64_t)(uintptr_t)base),
              [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
              [out] "r"((uint64_t)(uintptr_t)result),
              [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
              [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
              [lv] "n"(lv),
              [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
              [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
              [pack_sub] "n"(pack_sub),
              [trsv_zrhs] "n"(trsv_zrhs)
            : "memory", "cc",
              "x11", "x12", "x16", "x17", "x18", "x19", "x20",
              "x21", "x22", "x23", "p0", "z0", "z1", "z2", "z3",
              "z4", "z5", "z11");
    }
#else
    double mvm[N];
    mvm5_software(matrix, vector, mvm);
    for (int lane = 0; lane < N; lane++)
        result[lane] = base[lane] - mvm[lane];
    trsv5_software(lu, result);
    if (context_result) {
        for (int lane = 0; lane < N; lane++)
            context_result[lane] = result[lane];
    }
#endif

    stats->hardware_trsv++;
    stats->trsv5_rhs_forwarded++;
    stats->trsv5_rhs_spm_stage_elided++;
    stats->step2_core_forward_rhs_stack_store_elided++;
    stats->trsv5_rhs_forward_consumed++;
}

__attribute__((noinline)) static void
patha_mvm_sub5_trsv5_forwarded_linebuf_fwd(
    const double matrix[N * N], const double fallback_vector[N],
    const double base[N], const double lu[N * N], double result[N],
    Stats *stats, uint64_t line_id, uint64_t read_cell_id,
    uint64_t write_cell_id)
{
    stage_patha_matrix_and_trsv_lu_to_spm(matrix, lu);

#if defined(__aarch64__)
    (void)fallback_vector;
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t rd = enc_linebuf_rd_fwd5(5, 9, 10);
    const uint32_t wr = enc_linebuf_wr_fwd5(11, 9, 10);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack_sub = enc_pack_sub_acc(11, 12, 16);
    const uint32_t trsv_zrhs = enc_trsv5_lu_spm_zrhs(11, 22, 11);

    __asm__ __volatile__(
        "mov x21, %[mat]\n\t"
        "mov x22, %[lu]\n\t"
        "mov x12, %[base]\n\t"
        "mov x11, %[out]\n\t"
        "mov x9, %[line]\n\t"
        "mov x10, %[read_cell]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[rd]\n\t"
        ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
        ".long %[lm3]\n\t" ".long %[lm4]\n\t"
        ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
        ".long %[dp3]\n\t" ".long %[dp4]\n\t"
        ".long %[pack_sub]\n\t"
        ".long %[trsv_zrhs]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "mov x10, %[write_cell]\n\t"
        ".long %[wr]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
          [lu] "r"((uint64_t)SPM_TRSV_MAT_BASE),
          [base] "r"((uint64_t)(uintptr_t)base),
          [out] "r"((uint64_t)(uintptr_t)result),
          [line] "r"(line_id),
          [read_cell] "r"(read_cell_id),
          [write_cell] "r"(write_cell_id),
          [rd] "n"(rd),
          [wr] "n"(wr),
          [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
          [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
          [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
          [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
          [pack_sub] "n"(pack_sub),
          [trsv_zrhs] "n"(trsv_zrhs)
        : "memory", "cc",
          "x9", "x10", "x11", "x12", "x16", "x17", "x18",
          "x19", "x20", "x21", "x22", "p0", "z0", "z1", "z2",
          "z3", "z4", "z5", "z11");
#else
    (void)line_id;
    (void)read_cell_id;
    (void)write_cell_id;
    double mvm[N];
    mvm5_software(matrix, fallback_vector, mvm);
    for (int lane = 0; lane < N; lane++)
        result[lane] = base[lane] - mvm[lane];
    trsv5_software(lu, result);
#endif

    stats->hardware_trsv++;
    stats->trsv5_rhs_forwarded++;
    stats->trsv5_rhs_spm_stage_elided++;
    stats->step2_core_forward_rhs_stack_store_elided++;
    stats->trsv5_rhs_forward_consumed++;
    stats->linebuf_forwarding_enabled = 1;
    stats->linebuf_forward_reads++;
    stats->linebuf_forward_writes++;
    stats->linebuf_forward_hits++;
    stats->linebuf_spm_vector_stages_elided++;
    stats->linebuf_lmat_vec_loads_elided++;
    stats->linebuf_context_memory_loads_elided++;
    stats->linebuf_context_memory_stores_elided++;
}

__attribute__((noinline)) static void
patha_mvm_sub5_linebuf_bwd(const double matrix[N * N],
                           const double fallback_vector[N],
                           const double base[N], double result[N],
                           Stats *stats, uint64_t line_id,
                           uint64_t read_cell_id, uint64_t write_cell_id)
{
    stage_patha_matrix_to_spm(matrix);

#if defined(__aarch64__)
    (void)fallback_vector;
    const uint32_t lm[5] = {
        enc_lmat5_spm(0, 21, 0), enc_lmat5_spm(1, 21, 1),
        enc_lmat5_spm(2, 21, 2), enc_lmat5_spm(3, 21, 3),
        enc_lmat5_spm(4, 21, 4)
    };
    const uint32_t rd = enc_linebuf_rd_bwd5(5, 9, 10);
    const uint32_t wr = enc_linebuf_wr_bwd5(11, 9, 10);
    const uint32_t dp[5] = {
        enc_dotp_row(0, 0, 5), enc_dotp_row(1, 1, 5),
        enc_dotp_row(2, 2, 5), enc_dotp_row(3, 3, 5),
        enc_dotp_row(4, 4, 5)
    };
    const uint32_t pack_sub = enc_pack_sub_acc(11, 12, 16);

    __asm__ __volatile__(
        "mov x21, %[mat]\n\t"
        "mov x12, %[base]\n\t"
        "mov x11, %[out]\n\t"
        "mov x9, %[line]\n\t"
        "mov x10, %[read_cell]\n\t"
        "ptrue p0.d, vl5\n\t"
        ".long %[rd]\n\t"
        ".long %[lm0]\n\t" ".long %[lm1]\n\t" ".long %[lm2]\n\t"
        ".long %[lm3]\n\t" ".long %[lm4]\n\t"
        ".long %[dp0]\n\t" ".long %[dp1]\n\t" ".long %[dp2]\n\t"
        ".long %[dp3]\n\t" ".long %[dp4]\n\t"
        ".long %[pack_sub]\n\t"
        "st1d {z11.d}, p0, [x11]\n\t"
        "mov x10, %[write_cell]\n\t"
        ".long %[wr]\n\t"
        "dsb sy\n\t"
        "isb\n\t"
        :
        : [mat] "r"((uint64_t)SPM_PING_MAT_BASE),
          [base] "r"((uint64_t)(uintptr_t)base),
          [out] "r"((uint64_t)(uintptr_t)result),
          [line] "r"(line_id),
          [read_cell] "r"(read_cell_id),
          [write_cell] "r"(write_cell_id),
          [rd] "n"(rd),
          [wr] "n"(wr),
          [lm0] "n"(lm[0]), [lm1] "n"(lm[1]), [lm2] "n"(lm[2]),
          [lm3] "n"(lm[3]), [lm4] "n"(lm[4]),
          [dp0] "n"(dp[0]), [dp1] "n"(dp[1]), [dp2] "n"(dp[2]),
          [dp3] "n"(dp[3]), [dp4] "n"(dp[4]),
          [pack_sub] "n"(pack_sub)
        : "memory", "cc",
          "x9", "x10", "x11", "x12", "x16", "x17", "x18",
          "x19", "x20", "x21", "p0", "z0", "z1", "z2", "z3",
          "z4", "z5", "z11");
#else
    (void)line_id;
    (void)read_cell_id;
    (void)write_cell_id;
    double mvm[N];
    mvm5_software(matrix, fallback_vector, mvm);
    for (int lane = 0; lane < N; lane++)
        result[lane] = base[lane] - mvm[lane];
#endif

    stats->linebuf_forwarding_enabled = 1;
    stats->linebuf_backward_reads++;
    stats->linebuf_backward_writes++;
    stats->linebuf_backward_hits++;
    stats->linebuf_spm_vector_stages_elided++;
    stats->linebuf_lmat_vec_loads_elided++;
    stats->linebuf_context_memory_loads_elided++;
    stats->linebuf_context_memory_stores_elided++;
}

static void
read_tmp_result(const double src[N], double dst[N], Stats *stats)
{
    const volatile double *tmp = (const volatile double *)src;
    for (int lane = 0; lane < N; lane++)
        dst[lane] = tmp[lane];
    stats->tmp_result_loads++;
    stats->tmp_result_load_bytes += SPM_STRIDE_BYTES;
}

static double
signed_scale(size_t line, size_t cell, int r, int c, double base)
{
    unsigned code = (unsigned)((line + 1) * 17 + (cell + 3) * 13 +
                               (size_t)(r + 5) * 7 + (size_t)(c + 11) * 3);
    double sign = (code & 1U) ? -1.0 : 1.0;
    return sign * base * (double)(1U + (code % 5U));
}

static void
init_problem(Problem *p, size_t lines, size_t cells)
{
    memset(p, 0, sizeof(*p));
    p->lines = lines;
    p->cells = cells;

    size_t vec_count = lines * cells * N;
    size_t mat_count = lines * cells * N * N;
    p->d_mat = xcalloc(mat_count, sizeof(double), "d_mat");
    p->l_mat = xcalloc(mat_count, sizeof(double), "l_mat");
    p->u_mat = xcalloc(mat_count, sizeof(double), "u_mat");
    p->lu_d = xcalloc(mat_count, sizeof(double), "lu_d");
    p->u_bar = xcalloc(mat_count, sizeof(double), "u_bar");
    p->lu_a = xcalloc(mat_count, sizeof(double), "lu_a");
    p->c_mat = xcalloc(mat_count, sizeof(double), "c_mat");
    p->b_bar = xcalloc(mat_count, sizeof(double), "b_bar");
    p->d_inv = xcalloc(mat_count, sizeof(double), "d_inv");
    p->l_bar_precomputed =
        xcalloc(mat_count, sizeof(double), "l_bar_precomputed");
    p->u_bar_precomputed = p->b_bar;
    p->pretransform_valid = (uint8_t *)xcalloc_raw(
        lines * cells, sizeof(uint8_t), "pretransform_valid");
    p->rhs = xcalloc(vec_count, sizeof(double), "rhs");
    p->dq_star_ref = xcalloc(vec_count, sizeof(double), "dq_star_ref");
    p->dq_ref = xcalloc(vec_count, sizeof(double), "dq_ref");
    p->dq_star_raw_ref =
        xcalloc(vec_count, sizeof(double), "dq_star_raw_ref");
    p->dq_raw_ref = xcalloc(vec_count, sizeof(double), "dq_raw_ref");
    p->dq_star_step1 = xcalloc(vec_count, sizeof(double), "dq_star_step1");
    p->dq_step1 = xcalloc(vec_count, sizeof(double), "dq_step1");
    p->dq_star_step2 = xcalloc(vec_count, sizeof(double), "dq_star_step2");
    p->dq_step2 = xcalloc(vec_count, sizeof(double), "dq_step2");
    p->dq_star_step2_core =
        xcalloc(vec_count, sizeof(double), "dq_star_step2_core");
    p->dq_step2_core = xcalloc(vec_count, sizeof(double), "dq_step2_core");
    p->dq_star_step2_core_forwarded =
        xcalloc(vec_count, sizeof(double), "dq_star_step2_core_forwarded");
    p->dq_step2_core_forwarded =
        xcalloc(vec_count, sizeof(double), "dq_step2_core_forwarded");
    p->dq_star_step2_core_forwarded_context =
        xcalloc(vec_count, sizeof(double),
                "dq_star_step2_core_forwarded_context");
    p->dq_step2_core_forwarded_context =
        xcalloc(vec_count, sizeof(double),
                "dq_step2_core_forwarded_context");
    p->dq_star_step2_core_forwarded_linebuf =
        xcalloc(vec_count, sizeof(double),
                "dq_star_step2_core_forwarded_linebuf");
    p->dq_step2_core_forwarded_linebuf =
        xcalloc(vec_count, sizeof(double),
                "dq_step2_core_forwarded_linebuf");
    p->dq_star_step2_pretransform =
        xcalloc(vec_count, sizeof(double), "dq_star_step2_pretransform");
    p->dq_step2_pretransform =
        xcalloc(vec_count, sizeof(double), "dq_step2_pretransform");
    p->dq_star_step2_pretransform_context =
        xcalloc(vec_count, sizeof(double),
                "dq_star_step2_pretransform_context");
    p->dq_step2_pretransform_context =
        xcalloc(vec_count, sizeof(double),
                "dq_step2_pretransform_context");
    p->dq_star_step2_pretransform_optprep =
        xcalloc(vec_count, sizeof(double),
                "dq_star_step2_pretransform_optprep");
    p->dq_step2_pretransform_optprep =
        xcalloc(vec_count, sizeof(double),
                "dq_step2_pretransform_optprep");
    p->dq_star_step2_pretransform_optprep_context =
        xcalloc(vec_count, sizeof(double),
                "dq_star_step2_pretransform_optprep_context");
    p->dq_step2_pretransform_optprep_context =
        xcalloc(vec_count, sizeof(double),
                "dq_step2_pretransform_optprep_context");
    p->dq_star_trsv5_raw =
        xcalloc(vec_count, sizeof(double), "dq_star_trsv5_raw");
    p->dq_trsv5_raw = xcalloc(vec_count, sizeof(double), "dq_trsv5_raw");
    p->dq_star_trsv5_raw_context =
        xcalloc(vec_count, sizeof(double), "dq_star_trsv5_raw_context");
    p->dq_trsv5_raw_context =
        xcalloc(vec_count, sizeof(double), "dq_trsv5_raw_context");
    p->dq_star_pretransform_raw =
        xcalloc(vec_count, sizeof(double), "dq_star_pretransform_raw");
    p->dq_pretransform_raw =
        xcalloc(vec_count, sizeof(double), "dq_pretransform_raw");
    p->dq_star_pretransform_raw_context =
        xcalloc(vec_count, sizeof(double), "dq_star_pretransform_raw_context");
    p->dq_pretransform_raw_context =
        xcalloc(vec_count, sizeof(double), "dq_pretransform_raw_context");
    p->dq_star_pretransform_raw_optprep =
        xcalloc(vec_count, sizeof(double), "dq_star_pretransform_raw_optprep");
    p->dq_pretransform_raw_optprep =
        xcalloc(vec_count, sizeof(double), "dq_pretransform_raw_optprep");
    p->dq_star_pretransform_raw_optprep_context =
        xcalloc(vec_count, sizeof(double),
                "dq_star_pretransform_raw_optprep_context");
    p->dq_pretransform_raw_optprep_context =
        xcalloc(vec_count, sizeof(double),
                "dq_pretransform_raw_optprep_context");
    p->dq_star_pretransform_raw_stream =
        xcalloc(vec_count, sizeof(double), "dq_star_pretransform_raw_stream");
    p->dq_pretransform_raw_stream =
        xcalloc(vec_count, sizeof(double), "dq_pretransform_raw_stream");
    p->tmp_slots = xcalloc(lines * 2U * N, sizeof(double), "tmp_slots");

    for (size_t line = 0; line < lines; line++) {
        for (size_t cell = 0; cell < cells; cell++) {
            double *lu = mat_at(p->lu_a, p, line, cell);
            double *cm = mat_at(p->c_mat, p, line, cell);
            double *bb = mat_at(p->b_bar, p, line, cell);
            double *d = mat_at(p->d_mat, p, line, cell);
            double *lower = mat_at(p->l_mat, p, line, cell);
            double *upper = mat_at(p->u_mat, p, line, cell);
            double *rhs = vec_at(p->rhs, p, line, cell);
            for (int r = 0; r < N; r++) {
                for (int c = 0; c < N; c++) {
                    if (r == c) {
                        lu[r * N + c] = 2.40 + 0.09 * (double)(r + 1) +
                                        0.015 * (double)(line + 1) +
                                        0.007 * (double)(cell + 1);
                    } else if (r > c) {
                        lu[r * N + c] = signed_scale(line, cell, r, c, 0.006);
                    } else {
                        lu[r * N + c] = signed_scale(line, cell, r, c, 0.004);
                    }
                    cm[r * N + c] = signed_scale(line + 1, cell, r, c, 0.010);
                    bb[r * N + c] = signed_scale(line, cell + 1, r, c, 0.008);
                    d[r * N + c] = r == c ?
                        3.20 + 0.11 * (double)(r + 1) +
                            0.009 * (double)(line + 1) +
                            0.004 * (double)(cell + 1) :
                        signed_scale(line + 3, cell + 2, r, c, 0.003);
                    lower[r * N + c] =
                        signed_scale(line + 5, cell + 1, r, c, 0.010);
                    upper[r * N + c] =
                        signed_scale(line + 2, cell + 7, r, c, 0.008);
                }
            }
            for (int lane = 0; lane < N; lane++) {
                unsigned code = (unsigned)((line + 1) * 19 + (cell + 1) * 11 +
                                           (size_t)(lane + 1) * 5);
                double sign = (code & 1U) ? -1.0 : 1.0;
                rhs[lane] = sign * (0.40 + 0.031 * (double)(line + 1) +
                                    0.019 * (double)(cell + 1) +
                                    0.013 * (double)(lane + 1));
            }
        }
    }
}

static void
free_problem(Problem *p)
{
    free(p->d_mat);
    free(p->l_mat);
    free(p->u_mat);
    free(p->lu_d);
    free(p->u_bar);
    free(p->lu_a);
    free(p->c_mat);
    free(p->b_bar);
    free(p->d_inv);
    free(p->l_bar_precomputed);
    free(p->pretransform_valid);
    free(p->rhs);
    free(p->dq_star_ref);
    free(p->dq_ref);
    free(p->dq_star_raw_ref);
    free(p->dq_raw_ref);
    free(p->dq_star_step1);
    free(p->dq_step1);
    free(p->dq_star_step2);
    free(p->dq_step2);
    free(p->dq_star_step2_core);
    free(p->dq_step2_core);
    free(p->dq_star_step2_core_forwarded);
    free(p->dq_step2_core_forwarded);
    free(p->dq_star_step2_core_forwarded_context);
    free(p->dq_step2_core_forwarded_context);
    free(p->dq_star_step2_core_forwarded_linebuf);
    free(p->dq_step2_core_forwarded_linebuf);
    free(p->dq_star_step2_pretransform);
    free(p->dq_step2_pretransform);
    free(p->dq_star_step2_pretransform_context);
    free(p->dq_step2_pretransform_context);
    free(p->dq_star_step2_pretransform_optprep);
    free(p->dq_step2_pretransform_optprep);
    free(p->dq_star_step2_pretransform_optprep_context);
    free(p->dq_step2_pretransform_optprep_context);
    free(p->dq_star_trsv5_raw);
    free(p->dq_trsv5_raw);
    free(p->dq_star_trsv5_raw_context);
    free(p->dq_trsv5_raw_context);
    free(p->dq_star_pretransform_raw);
    free(p->dq_pretransform_raw);
    free(p->dq_star_pretransform_raw_context);
    free(p->dq_pretransform_raw_context);
    free(p->dq_star_pretransform_raw_optprep);
    free(p->dq_pretransform_raw_optprep);
    free(p->dq_star_pretransform_raw_optprep_context);
    free(p->dq_pretransform_raw_optprep_context);
    free(p->dq_star_pretransform_raw_stream);
    free(p->dq_pretransform_raw_stream);
    free(p->tmp_slots);
}

static void
copy_vec(double dst[N], const double src[N])
{
    for (int i = 0; i < N; i++)
        dst[i] = src[i];
}

static void
reference_solve(const Problem *p, Stats *stats)
{
    for (size_t line = 0; line < p->lines; line++) {
        double value[N];
        copy_vec(value, vec_const_at(p->rhs, p, line, 0));
        trsv5_software(mat_const_at(p->lu_a, p, line, 0), value);
        stats->reference_software_trsv++;
        copy_vec(vec_at(p->dq_star_ref, p, line, 0), value);

        for (size_t cell = 1; cell < p->cells; cell++) {
            double tmp[N];
            double rhs_prime[N];
            mvm5_software(mat_const_at(p->c_mat, p, line, cell),
                          vec_const_at(p->dq_star_ref, p, line, cell - 1),
                          tmp);
            stats->reference_mvm_forward++;
            for (int lane = 0; lane < N; lane++)
                rhs_prime[lane] =
                    vec_const_at(p->rhs, p, line, cell)[lane] - tmp[lane];
            stats->reference_vector_sub_ops++;
            trsv5_software(mat_const_at(p->lu_a, p, line, cell), rhs_prime);
            stats->reference_software_trsv++;
            copy_vec(vec_at(p->dq_star_ref, p, line, cell), rhs_prime);
        }

        copy_vec(vec_at(p->dq_ref, p, line, p->cells - 1),
                 vec_const_at(p->dq_star_ref, p, line, p->cells - 1));
        for (size_t cc = p->cells - 1; cc > 0; cc--) {
            size_t cell = cc - 1;
            double tmp[N];
            mvm5_software(mat_const_at(p->b_bar, p, line, cell),
                          vec_const_at(p->dq_ref, p, line, cell + 1),
                          tmp);
            stats->reference_mvm_backward++;
            for (int lane = 0; lane < N; lane++)
                vec_at(p->dq_ref, p, line, cell)[lane] =
                    vec_const_at(p->dq_star_ref, p, line, cell)[lane] -
                    tmp[lane];
            stats->reference_vector_sub_ops++;
        }
    }
}

static int
raw_reference_solve(Problem *p, Stats *stats)
{
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            double *lu = mat_at(p->lu_d, p, line, cell);
            if (!cfd_lu5_factor(mat_const_at(p->d_mat, p, line, cell), lu,
                                1.0e-12))
                return 0;
            trsm5_mrhs_software(lu,
                mat_const_at(p->u_mat, p, line, cell),
                mat_at(p->u_bar, p, line, cell));
        }
    }

    for (size_t line = 0; line < p->lines; line++) {
        double value[N];
        copy_vec(value, vec_const_at(p->rhs, p, line, 0));
        trsv5_software(mat_const_at(p->lu_d, p, line, 0), value);
        stats->reference_software_trsv++;
        copy_vec(vec_at(p->dq_star_raw_ref, p, line, 0), value);
        for (size_t cell = 1; cell < p->cells; cell++) {
            double tmp[N];
            mvm5_software(mat_const_at(p->l_mat, p, line, cell),
                          vec_const_at(p->dq_star_raw_ref, p, line, cell - 1),
                          tmp);
            stats->reference_mvm_forward++;
            for (int lane = 0; lane < N; lane++)
                value[lane] = vec_const_at(p->rhs, p, line, cell)[lane] -
                    tmp[lane];
            stats->reference_vector_sub_ops++;
            trsv5_software(mat_const_at(p->lu_d, p, line, cell), value);
            stats->reference_software_trsv++;
            copy_vec(vec_at(p->dq_star_raw_ref, p, line, cell), value);
        }
        copy_vec(vec_at(p->dq_raw_ref, p, line, p->cells - 1),
                 vec_const_at(p->dq_star_raw_ref, p, line, p->cells - 1));
        for (size_t cc = p->cells - 1; cc > 0; cc--) {
            size_t cell = cc - 1;
            double tmp[N];
            mvm5_software(mat_const_at(p->u_bar, p, line, cell),
                          vec_const_at(p->dq_raw_ref, p, line, cell + 1), tmp);
            stats->reference_mvm_backward++;
            for (int lane = 0; lane < N; lane++)
                vec_at(p->dq_raw_ref, p, line, cell)[lane] =
                    vec_const_at(p->dq_star_raw_ref, p, line, cell)[lane] -
                    tmp[lane];
            stats->reference_vector_sub_ops++;
        }
    }
    return 1;
}

static uint64_t
measure_software_trsv_cycles(const Problem *p, uint64_t sweeps)
{
    double sink = 0.0;
    uint64_t start = read_cycle_counter();
    for (uint64_t sweep = 0; sweep < sweeps; sweep++) {
        for (size_t line = 0; line < p->lines; line++) {
            for (size_t cell = 0; cell < p->cells; cell++) {
                double value[N];
                copy_vec(value, vec_const_at(p->rhs, p, line, cell));
                trsv5_software(mat_const_at(p->lu_a, p, line, cell), value);
                sink += value[0];
            }
        }
    }
    uint64_t end = read_cycle_counter();
    trsv_calibration_sink += sink * 1.0e-300;
    return (end >= start) ? (end - start) : 0;
}

static void
reset_forward_contexts(ForwardLineContext *contexts, size_t lines)
{
    for (size_t line = 0; line < lines; line++) {
        memset(contexts[line].prev_dqstar, 0, sizeof(contexts[line].prev_dqstar));
        contexts[line].valid = 0;
        contexts[line].line_id = -1;
        contexts[line].cell_id = -1;
    }
}

static void
reset_backward_contexts(BackwardLineContext *contexts, size_t lines)
{
    for (size_t line = 0; line < lines; line++) {
        memset(contexts[line].next_dq, 0, sizeof(contexts[line].next_dq));
        contexts[line].valid = 0;
        contexts[line].line_id = -1;
        contexts[line].cell_id = -1;
    }
}

static void
patha_forward_sweep(Problem *p, const Config *cfg, Stats *stats,
                    double *dq_star, int use_hardware_trsv,
                    int use_fused_sub, int use_forwarded_rhs,
                    int use_line_context, int use_linebuf,
                    ForwardLineContext *forward_ctx)
{
    uint64_t start = read_cycle_counter();
    uint64_t forward_context_hits = 0;
    uint64_t forward_context_misses = 0;
    uint64_t forward_heap_elided = 0;
    uint64_t forward_context_stages = 0;
    uint64_t forward_context_updates = 0;
    uint64_t forward_context_invalid = 0;
    uint64_t line_context_hits = 0;
    uint64_t line_context_misses = 0;
    for (size_t base = 0; base < p->lines; base += cfg->interleave) {
        size_t end = base + cfg->interleave;
        if (end > p->lines)
            end = p->lines;
        for (size_t line = base; line < end; line++) {
            double value[N];
            copy_vec(value, vec_const_at(p->rhs, p, line, 0));
            if (use_linebuf && use_hardware_trsv) {
                trsv5_hardware_linebuf_fwd_counted(
                    mat_const_at(p->lu_a, p, line, 0), value,
                    vec_at(dq_star, p, line, 0), stats,
                    (uint64_t)line, 0);
                copy_vec(value, vec_const_at(dq_star, p, line, 0));
            } else {
                trsv5_step_counted(mat_const_at(p->lu_a, p, line, 0), value,
                                   stats, use_hardware_trsv);
                copy_vec(vec_at(dq_star, p, line, 0), value);
            }
            if (use_line_context) {
                ForwardLineContext *ctx = &forward_ctx[line];
                copy_vec(ctx->prev_dqstar, value);
                ctx->valid = 1;
                ctx->line_id = (int)line;
                ctx->cell_id = 0;
                forward_context_updates++;
            }
            stats->forward_cells++;
        }
    }

    for (size_t cell = 1; cell < p->cells; cell++) {
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                const double *vector =
                    vec_const_at(dq_star, p, line, cell - 1);
                VectorSource vector_source = VECTOR_FROM_HEAP;
                ForwardLineContext *ctx =
                    use_line_context ? &forward_ctx[line] : NULL;
                if (use_linebuf) {
                    forward_context_hits++;
                    forward_heap_elided++;
                    line_context_hits++;
                    stats->patha_mvm_forward++;
                    double *out = vec_at(dq_star, p, line, cell);
                    patha_mvm_sub5_trsv5_forwarded_linebuf_fwd(
                        mat_const_at(p->c_mat, p, line, cell),
                        vec_const_at(dq_star, p, line, cell - 1),
                        vec_const_at(p->rhs, p, line, cell),
                        mat_const_at(p->lu_a, p, line, cell),
                        out, stats, (uint64_t)line,
                        (uint64_t)(cell - 1), (uint64_t)cell);
                    stats->fused_forward_updates++;
                    stats->fused_sub_ops++;
                    stats->hardware_vector_sub++;
                    stats->forward_cells++;
                    continue;
                }
                if (use_line_context) {
                    vector = ctx->prev_dqstar;
                    vector_source = VECTOR_FROM_FORWARD_CONTEXT;
                    forward_context_hits++;
                    forward_heap_elided++;
                    forward_context_stages++;
                    line_context_hits++;
                }
                stats->patha_mvm_forward++;
                if (use_fused_sub && use_forwarded_rhs && use_hardware_trsv) {
                    double *out = vec_at(dq_star, p, line, cell);
                    patha_mvm_sub5_trsv5_forwarded_from_source(
                        mat_const_at(p->c_mat, p, line, cell),
                        vector,
                        vec_const_at(p->rhs, p, line, cell),
                        mat_const_at(p->lu_a, p, line, cell),
                        out, stats, vector_source,
                        use_line_context ? ctx->prev_dqstar : NULL);
                    if (use_line_context) {
                        ctx->cell_id = (int)cell;
                        forward_context_updates++;
                    }
                    stats->fused_forward_updates++;
                    stats->fused_sub_ops++;
                    stats->hardware_vector_sub++;
                    stats->forward_cells++;
                    continue;
                }

                double rhs_prime[N];
                if (use_fused_sub) {
                    patha_mvm_sub5_fused_from_source(
                        mat_const_at(p->c_mat, p, line, cell),
                        vector, vec_const_at(p->rhs, p, line, cell),
                        rhs_prime, vector_source, NULL);
                    stats->fused_forward_updates++;
                    stats->fused_sub_ops++;
                    stats->hardware_vector_sub++;
                } else {
                    double *slot = tmp_slot_at(p, line, cell);
                    double tmp[N];
                    patha_mvm5_existing(
                        mat_const_at(p->c_mat, p, line, cell),
                        vec_const_at(dq_star, p, line, cell - 1), slot);
                    stats->tmp_result_stores++;
                    stats->tmp_result_store_bytes += SPM_STRIDE_BYTES;
                    read_tmp_result(slot, tmp, stats);
                    for (int lane = 0; lane < N; lane++)
                        rhs_prime[lane] =
                            vec_const_at(p->rhs, p, line, cell)[lane] -
                            tmp[lane];
                    stats->vector_sub_ops++;
                }
                trsv5_step_counted(mat_const_at(p->lu_a, p, line, cell),
                                   rhs_prime, stats, use_hardware_trsv);
                copy_vec(vec_at(dq_star, p, line, cell), rhs_prime);
                if (use_line_context) {
                    copy_vec(ctx->prev_dqstar, rhs_prime);
                    ctx->cell_id = (int)cell;
                    forward_context_updates++;
                }
                stats->forward_cells++;
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->forward_cycles += end - start;
    stats->forward_context_hits += forward_context_hits;
    stats->forward_context_misses += forward_context_misses;
    stats->forward_dqstar_heap_vector_load_elided += forward_heap_elided;
    stats->forward_context_vector_stages += forward_context_stages;
    stats->forward_context_updates += forward_context_updates;
    stats->forward_context_invalid += forward_context_invalid;
    stats->line_context_total_hits += line_context_hits;
    stats->line_context_total_misses += line_context_misses;
}

static void
patha_backward_sweep(Problem *p, const Config *cfg, Stats *stats,
                     const double *dq_star, double *dq, int use_fused_sub,
                     int use_line_context, int use_linebuf,
                     BackwardLineContext *backward_ctx)
{
    uint64_t start = read_cycle_counter();
    uint64_t backward_context_hits = 0;
    uint64_t backward_context_misses = 0;
    uint64_t backward_heap_elided = 0;
    uint64_t backward_context_stages = 0;
    uint64_t backward_context_updates = 0;
    uint64_t backward_context_invalid = 0;
    uint64_t line_context_hits = 0;
    uint64_t line_context_misses = 0;
    for (size_t base = 0; base < p->lines; base += cfg->interleave) {
        size_t end = base + cfg->interleave;
        if (end > p->lines)
            end = p->lines;
        for (size_t line = base; line < end; line++) {
            copy_vec(vec_at(dq, p, line, p->cells - 1),
                     vec_const_at(dq_star, p, line, p->cells - 1));
            if (use_linebuf) {
                linebuf_write5_from_mem(
                    0, (uint64_t)line, (uint64_t)(p->cells - 1),
                    vec_const_at(dq, p, line, p->cells - 1));
                stats->linebuf_forwarding_enabled = 1;
                stats->linebuf_backward_writes++;
                stats->linebuf_context_memory_stores_elided++;
            }
            if (use_line_context) {
                BackwardLineContext *ctx = &backward_ctx[line];
                copy_vec(ctx->next_dq,
                         vec_const_at(dq_star, p, line, p->cells - 1));
                ctx->valid = 1;
                ctx->line_id = (int)line;
                ctx->cell_id = (int)(p->cells - 1);
                backward_context_updates++;
            }
            stats->backward_cells++;
        }
    }

    for (size_t cc = p->cells - 1; cc > 0; cc--) {
        size_t cell = cc - 1;
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                const double *vector =
                    vec_const_at(dq, p, line, cell + 1);
                VectorSource vector_source = VECTOR_FROM_HEAP;
                BackwardLineContext *ctx =
                    use_line_context ? &backward_ctx[line] : NULL;
                if (use_linebuf) {
                    backward_context_hits++;
                    backward_heap_elided++;
                    line_context_hits++;
                    stats->patha_mvm_backward++;
                    patha_mvm_sub5_linebuf_bwd(
                        mat_const_at(p->b_bar, p, line, cell),
                        vec_const_at(dq, p, line, cell + 1),
                        vec_const_at(dq_star, p, line, cell),
                        vec_at(dq, p, line, cell), stats,
                        (uint64_t)line, (uint64_t)(cell + 1),
                        (uint64_t)cell);
                    stats->fused_backward_updates++;
                    stats->fused_sub_ops++;
                    stats->hardware_vector_sub++;
                    stats->backward_cells++;
                    continue;
                }
                if (use_line_context) {
                    vector = ctx->next_dq;
                    vector_source = VECTOR_FROM_BACKWARD_CONTEXT;
                    backward_context_hits++;
                    backward_heap_elided++;
                    backward_context_stages++;
                    line_context_hits++;
                }
                stats->patha_mvm_backward++;
                if (use_fused_sub) {
                    double *out = vec_at(dq, p, line, cell);
                    patha_mvm_sub5_fused_from_source(
                        mat_const_at(p->b_bar, p, line, cell),
                        vector,
                        vec_const_at(dq_star, p, line, cell),
                        out, vector_source,
                        use_line_context ? ctx->next_dq : NULL);
                    if (use_line_context) {
                        ctx->cell_id = (int)cell;
                        backward_context_updates++;
                    }
                    stats->fused_backward_updates++;
                    stats->fused_sub_ops++;
                    stats->hardware_vector_sub++;
                } else {
                    double *slot = tmp_slot_at(p, line, cell);
                    double tmp[N];
                    patha_mvm5_existing(
                        mat_const_at(p->b_bar, p, line, cell),
                        vec_const_at(dq, p, line, cell + 1), slot);
                    stats->tmp_result_stores++;
                    stats->tmp_result_store_bytes += SPM_STRIDE_BYTES;
                    read_tmp_result(slot, tmp, stats);
                    for (int lane = 0; lane < N; lane++)
                        vec_at(dq, p, line, cell)[lane] =
                            vec_const_at(dq_star, p, line, cell)[lane] -
                            tmp[lane];
                    stats->vector_sub_ops++;
                    if (use_line_context) {
                        copy_vec(ctx->next_dq,
                                 vec_const_at(dq, p, line, cell));
                        ctx->cell_id = (int)cell;
                        backward_context_updates++;
                    }
                }
                stats->backward_cells++;
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->backward_cycles += end - start;
    stats->backward_context_hits += backward_context_hits;
    stats->backward_context_misses += backward_context_misses;
    stats->backward_dq_heap_vector_load_elided += backward_heap_elided;
    stats->backward_context_vector_stages += backward_context_stages;
    stats->backward_context_updates += backward_context_updates;
    stats->backward_context_invalid += backward_context_invalid;
    stats->line_context_total_hits += line_context_hits;
    stats->line_context_total_misses += line_context_misses;
}

static void
patha_lusgs_solve(Problem *p, const Config *cfg, Stats *stats,
                  double *dq_star, double *dq, int use_hardware_trsv,
                  int use_fused_sub, int use_forwarded_rhs,
                  int use_line_context, int use_linebuf)
{
    ForwardLineContext *forward_ctx = NULL;
    BackwardLineContext *backward_ctx = NULL;
    if (use_line_context) {
        forward_ctx = (ForwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*forward_ctx), "forward_line_context");
        backward_ctx = (BackwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*backward_ctx), "backward_line_context");
        stats->line_context_forwarding_enabled = 1;
    }
    if (use_linebuf) {
        stats->line_context_forwarding_enabled = 1;
        stats->linebuf_forwarding_enabled = 1;
    }

    uint64_t start = read_cycle_counter();
    for (uint64_t sweep = 0; sweep < cfg->sweeps; sweep++) {
        if (use_line_context) {
            reset_forward_contexts(forward_ctx, p->lines);
            reset_backward_contexts(backward_ctx, p->lines);
        }
        patha_forward_sweep(p, cfg, stats, dq_star, use_hardware_trsv,
                            use_fused_sub, use_forwarded_rhs,
                            use_line_context, use_linebuf, forward_ctx);
        patha_backward_sweep(p, cfg, stats, dq_star, dq, use_fused_sub,
                             use_line_context, use_linebuf, backward_ctx);
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->total_cycles += end - start;

    free(forward_ctx);
    free(backward_ctx);
}

static void
pretransform_forward_cell(Problem *p, Stats *stats, double *dq_star,
                          size_t line, size_t cell,
                          ForwardLineContext *ctx)
{
    double *out = vec_at(dq_star, p, line, cell);
    if (cell == 0) {
        patha_mvm5_existing(
            mat_const_at(p->d_inv, p, line, cell),
            vec_const_at(p->rhs, p, line, cell), out);
        if (ctx) {
            copy_vec(ctx->prev_dqstar, out);
            ctx->valid = 1;
            ctx->line_id = (int)line;
            ctx->cell_id = 0;
            stats->forward_context_updates++;
        }
        stats->pretransform_dinv_mvm++;
        stats->pretransform_patha_ping_issued++;
        stats->patha_mvm_forward++;
    } else {
        const double *prev = ctx ? ctx->prev_dqstar :
            vec_const_at(dq_star, p, line, cell - 1);
        patha_pretransform_forward_update(
            mat_const_at(p->d_inv, p, line, cell),
            vec_const_at(p->rhs, p, line, cell),
            mat_const_at(p->l_bar_precomputed, p, line, cell),
            prev, out, ctx ? ctx->prev_dqstar : NULL);
        if (ctx) {
            ctx->cell_id = (int)cell;
            stats->forward_context_hits++;
            stats->line_context_total_hits++;
            stats->forward_context_updates++;
            stats->forward_dqstar_heap_vector_load_elided++;
        }
        stats->pretransform_dinv_mvm++;
        stats->pretransform_lbar_mvm++;
        stats->pretransform_vector_sub++;
        stats->hardware_vector_sub++;
        stats->pretransform_patha_ping_issued++;
        stats->pretransform_patha_pong_issued++;
        stats->patha_mvm_forward += 2;
    }
    stats->forward_cells++;
    stats->pretransform_forward_cells++;
    stats->pretransform_hot_loop_trsv5_elided++;
}

static void
pretransform_forward_cell_from_base(Problem *p, Stats *stats,
                                    double *dq_star, size_t line,
                                    size_t cell, ForwardLineContext *ctx,
                                    const double base[N],
                                    double correction_result[N])
{
    double *out = vec_at(dq_star, p, line, cell);
    if (cell == 0) {
        copy_vec(out, base);
        if (ctx) {
            copy_vec(ctx->prev_dqstar, out);
            ctx->valid = 1;
            ctx->line_id = (int)line;
            ctx->cell_id = 0;
            stats->forward_context_updates++;
        }
    } else {
        double unused_correction[N];
        const double *prev = ctx ? ctx->prev_dqstar :
            vec_const_at(dq_star, p, line, cell - 1);
        patha_pretransform_forward_from_base(
            base, mat_const_at(p->l_bar_precomputed, p, line, cell),
            prev, out, ctx ? ctx->prev_dqstar : NULL,
            correction_result ? correction_result : unused_correction);
        if (ctx) {
            ctx->cell_id = (int)cell;
            stats->forward_context_hits++;
            stats->line_context_total_hits++;
            stats->forward_context_updates++;
            stats->forward_dqstar_heap_vector_load_elided++;
        }
        stats->pretransform_lbar_mvm++;
        stats->pretransform_vector_sub++;
        stats->hardware_vector_sub++;
        stats->pretransform_patha_pong_issued++;
        stats->patha_mvm_forward++;
    }
    stats->patha_dinv_mvm_eliminated++;
    stats->patha_dinv_custom_instructions_eliminated += 12;
    stats->dinv_guest_reload_bytes_avoided += N * N * sizeof(double);
    stats->forward_cells++;
    stats->pretransform_forward_cells++;
    stats->pretransform_hot_loop_trsv5_elided++;
}

static void
pretransform_forward_sweep(Problem *p, const Config *cfg, Stats *stats,
                           double *dq_star, int use_context,
                           ForwardLineContext *forward_ctx)
{
    uint64_t start = read_cycle_counter();
    for (size_t cell = 0; cell < p->cells; cell++) {
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                ForwardLineContext *ctx =
                    use_context ? &forward_ctx[line] : NULL;
                pretransform_forward_cell(p, stats, dq_star, line, cell, ctx);
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->forward_cycles += end - start;
}

static void
pretransform_backward_sweep(Problem *p, const Config *cfg, Stats *stats,
                            const double *dq_star, double *dq,
                            int use_context,
                            BackwardLineContext *backward_ctx,
                            const double *u_bar_data)
{
    uint64_t start = read_cycle_counter();
    for (size_t base = 0; base < p->lines; base += cfg->interleave) {
        size_t end = base + cfg->interleave;
        if (end > p->lines)
            end = p->lines;
        for (size_t line = base; line < end; line++) {
            size_t cell = p->cells - 1;
            double *out = vec_at(dq, p, line, cell);
            copy_vec(out, vec_const_at(dq_star, p, line, cell));
            if (use_context) {
                BackwardLineContext *ctx = &backward_ctx[line];
                copy_vec(ctx->next_dq, out);
                ctx->valid = 1;
                ctx->line_id = (int)line;
                ctx->cell_id = (int)cell;
                stats->backward_context_updates++;
            }
            stats->backward_cells++;
            stats->pretransform_backward_cells++;
        }
    }

    for (size_t cc = p->cells - 1; cc > 0; cc--) {
        size_t cell = cc - 1;
        for (size_t base = 0; base < p->lines; base += cfg->interleave) {
            size_t end = base + cfg->interleave;
            if (end > p->lines)
                end = p->lines;
            for (size_t line = base; line < end; line++) {
                BackwardLineContext *ctx =
                    use_context ? &backward_ctx[line] : NULL;
                const double *next = ctx ? ctx->next_dq :
                    vec_const_at(dq, p, line, cell + 1);
                patha_pretransform_backward_update(
                    mat_const_at(u_bar_data, p, line, cell),
                    next, vec_const_at(dq_star, p, line, cell),
                    vec_at(dq, p, line, cell), ctx ? ctx->next_dq : NULL);
                if (ctx) {
                    ctx->cell_id = (int)cell;
                    stats->backward_context_hits++;
                    stats->line_context_total_hits++;
                    stats->backward_context_updates++;
                    stats->backward_dq_heap_vector_load_elided++;
                }
                stats->pretransform_ubar_mvm++;
                stats->pretransform_vector_sub++;
                stats->hardware_vector_sub++;
                stats->pretransform_patha_ping_issued++;
                stats->patha_mvm_backward++;
                stats->backward_cells++;
                stats->pretransform_backward_cells++;
            }
        }
    }
    uint64_t end = read_cycle_counter();
    if (end >= start)
        stats->backward_cycles += end - start;
}

static int
pretransform_lusgs_solve(Problem *p, const Config *cfg, Stats *stats,
                         double *dq_star, double *dq, int use_context,
                         int optimized_prep, PretransformCheck *check)
{
    ForwardLineContext *forward_ctx = NULL;
    BackwardLineContext *backward_ctx = NULL;
    if (use_context) {
        forward_ctx = (ForwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*forward_ctx), "pretransform_forward_context");
        backward_ctx = (BackwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*backward_ctx), "pretransform_backward_context");
        stats->line_context_forwarding_enabled = 1;
    }

    int all_valid = 1;
    if (!cfg->pretransform_per_sweep)
        all_valid = optimized_prep ?
            preprocess_coefficients_optprep(p, cfg, stats, check) :
            preprocess_coefficients(p, cfg, stats, check);
    if (!all_valid && cfg->pretransform_fallback_trsv5) {
        stats->pretransform_fallback_runs++;
        uint64_t preprocess_cycles = stats->preprocess_cycles;
        patha_lusgs_solve(p, cfg, stats, dq_star, dq, 1, 1, 1,
                          use_context, 0);
        stats->runtime_sweep_cycles = stats->total_cycles;
        stats->total_cycles_including_preprocess =
            preprocess_cycles + stats->runtime_sweep_cycles;
        free(forward_ctx);
        free(backward_ctx);
        return 0;
    }

    uint64_t runtime_start = read_cycle_counter();
    for (uint64_t sweep = 0; sweep < cfg->sweeps; sweep++) {
        if (cfg->pretransform_per_sweep) {
            int sweep_valid = optimized_prep ?
                preprocess_coefficients_optprep(p, cfg, stats, check) :
                preprocess_coefficients(p, cfg, stats, check);
            all_valid &= sweep_valid;
            if (!sweep_valid && cfg->pretransform_fallback_trsv5) {
                stats->pretransform_fallback_runs++;
                break;
            }
        }
        if (use_context) {
            reset_forward_contexts(forward_ctx, p->lines);
            reset_backward_contexts(backward_ctx, p->lines);
        }
        pretransform_forward_sweep(p, cfg, stats, dq_star, use_context,
                                   forward_ctx);
        pretransform_backward_sweep(p, cfg, stats, dq_star, dq, use_context,
                                    backward_ctx, p->u_bar_precomputed);
    }
    uint64_t runtime_end = read_cycle_counter();
    if (runtime_end >= runtime_start)
        stats->runtime_sweep_cycles += runtime_end - runtime_start;
    stats->total_cycles = stats->runtime_sweep_cycles;
    stats->total_cycles_including_preprocess =
        stats->preprocess_cycles + stats->runtime_sweep_cycles;
    stats->pretransform_coefficient_bytes_read =
        (stats->pretransform_dinv_mvm + stats->pretransform_lbar_mvm +
         stats->pretransform_ubar_mvm) * N * N * sizeof(double);
    stats->pretransform_tmp_stores = 0;
    stats->pretransform_tmp_loads = 0;

    free(forward_ctx);
    free(backward_ctx);
    return all_valid;
}

static int
prepare_raw_extra_dual(Problem *p, const Config *cfg, Stats *stats,
                       PretransformCheck *check)
{
    int all_valid = 1;
    const int full = !strcmp(cfg->pretransform_validate, "full");
    uint64_t total_start = read_cycle_counter();
    uint64_t generation = 1;
    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++, generation++) {
            const uint64_t slot = (generation - 1) %
                (cfg->coeff_preprocess_spm_slots > 1 ? 2 : 1);
            const uintptr_t base = pretransform_slot_base(slot);
            const double *lu = mat_const_at(p->lu_d, p, line, cell);
            const double *lower = mat_const_at(p->l_mat, p, line, cell);
            stage_optprep_inputs(lu, lower, base, cfg, stats);
            uint64_t compute_start = read_cycle_counter();
            uint64_t token = trsm5_inv_lbar_hardware(
                base, base + SPM_TRSM_OPT_C_OFFSET,
                base + SPM_TRSM_OPT_DINV_OFFSET,
                base + SPM_TRSM_OPT_LBAR_OFFSET, generation);
            uint64_t compute_end = read_cycle_counter();
            if (compute_end >= compute_start)
                stats->preprocess_compute_cycles +=
                    compute_end - compute_start;
            const uintptr_t token_offset =
                token - (generation + 1);
            volatile const double *d_src = (volatile const double *)
                (base + SPM_TRSM_OPT_DINV_OFFSET + token_offset);
            volatile const double *l_src = (volatile const double *)
                (base + SPM_TRSM_OPT_LBAR_OFFSET + token_offset);
            double *d_inv = mat_at(p->d_inv, p, line, cell);
            double *l_bar = mat_at(p->l_bar_precomputed, p, line, cell);
            for (int i = 0; i < N * N; i++) {
                d_inv[i] = d_src[i];
                l_bar[i] = l_src[i];
            }
            stats->trsm5_inv_lbar_issued++;
            stats->trsm5_inv_lbar_completed++;
            stats->trsm5_inv_lbar_lu_reads++;
            stats->trsm5_inv_lbar_columns_solved += 10;
            stats->dinv_generated++;
            stats->lbar_generated++;

            double identity[N * N] = {};
            for (int i = 0; i < N; i++)
                identity[i * N + i] = 1.0;
            double d_residual = 0.0;
            double l_residual = 0.0;
            if (full) {
                double d_ref[N * N];
                double l_ref[N * N];
                d_residual = matrix_residual_max(lu, d_inv, identity);
                l_residual = matrix_residual_max(lu, l_bar, lower);
                trsm5_mrhs_software(lu, identity, d_ref);
                trsm5_mrhs_software(lu, lower, l_ref);
                update_matrix_compare(&check->d_inv, d_ref, d_inv,
                                      d_residual, 1.0e-12, 1.0e-12);
                update_matrix_compare(&check->l_bar, l_ref, l_bar,
                                      l_residual, 1.0e-12, 1.0e-12);
            }
            if (full && (d_residual > 1.0e-9 || l_residual > 1.0e-9))
                all_valid = 0;
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(d_inv[i]) || !isfinite(l_bar[i]))
                    all_valid = 0;
            }
        }
    }
    uint64_t total_end = read_cycle_counter();
    if (total_end >= total_start)
        stats->extra_pretransform_cycles += total_end - total_start;
    stats->raw_extra_coefficient_bytes =
        2U * p->lines * p->cells * N * N * sizeof(double);
    stats->raw_persistent_coefficient_bytes =
        4U * p->lines * p->cells * N * N * sizeof(double);
    return all_valid;
}

static uintptr_t
coeff_slot_base(uint64_t slot)
{
    return slot ? SPM_COEFF_SLOT1_BASE : SPM_COEFF_SLOT0_BASE;
}

static void
stage_coeff3_inputs(const double lu[N * N], const double lower[N * N],
                    const double upper[N * N], uintptr_t base)
{
    volatile double *lu_dst = (volatile double *)(base + SPM_COEFF_LU_OFFSET);
    volatile double *l_dst = (volatile double *)(base + SPM_COEFF_L_OFFSET);
    volatile double *u_dst = (volatile double *)(base + SPM_COEFF_U_OFFSET);
    for (int i = 0; i < N * N; i++) {
        lu_dst[i] = lu[i];
        l_dst[i] = lower[i];
        u_dst[i] = upper[i];
    }
    spm_input_release();
}

__attribute__((noinline)) static uint64_t
trsm5_coeff3_hardware(uintptr_t lu, uintptr_t lower, uintptr_t upper,
                      uintptr_t d_inv, uintptr_t l_bar, uintptr_t u_bar,
                      uint64_t input_token)
{
#if defined(__aarch64__)
    const uint32_t inst = enc_trsm5_coeff3_spm();
    uint64_t completion_token;
    __asm__ __volatile__(
        "mov x10, %[lu]\n\t"
        "mov x11, %[lower]\n\t"
        "mov x12, %[upper]\n\t"
        "mov x13, %[dinv]\n\t"
        "mov x14, %[lbar]\n\t"
        "mov x15, %[ubar]\n\t"
        "mov x26, %[token]\n\t"
        ".long %[inst]\n\t"
        "dsb sy\n\t"
        "mov %[done], x25\n\t"
        : [done] "=&r"(completion_token)
        : [lu] "r"((uint64_t)lu), [lower] "r"((uint64_t)lower),
          [upper] "r"((uint64_t)upper), [dinv] "r"((uint64_t)d_inv),
          [lbar] "r"((uint64_t)l_bar), [ubar] "r"((uint64_t)u_bar),
          [token] "r"(input_token), [inst] "n"(inst)
        : "memory", "cc", "x10", "x11", "x12", "x13", "x14", "x15",
          "x25", "x26");
    return completion_token;
#else
    double identity[N * N] = {};
    for (int i = 0; i < N; i++)
        identity[i * N + i] = 1.0;
    trsm5_mrhs_software((const double *)lu, identity, (double *)d_inv);
    trsm5_mrhs_software((const double *)lu, (const double *)lower,
                        (double *)l_bar);
    trsm5_mrhs_software((const double *)lu, (const double *)upper,
                        (double *)u_bar);
    return input_token + 1;
#endif
}

static void
drain_coeff3_entry(Problem *p, CoeffOutputEntry *entry, Stats *stats)
{
    if (!entry->valid)
        return;
    uint64_t start = read_cycle_counter();
    uintptr_t offset = entry->completion_token - (entry->generation + 1);
    volatile const double *d_src =
        (volatile const double *)(entry->d_inv_addr + offset);
    volatile const double *l_src =
        (volatile const double *)(entry->l_bar_addr + offset);
    volatile const double *u_src =
        (volatile const double *)(entry->u_bar_addr + offset);
    double *d_dst = mat_at(p->d_inv, p, entry->line_id, entry->cell_id);
    double *l_dst =
        mat_at(p->l_bar_precomputed, p, entry->line_id, entry->cell_id);
    double *u_dst = mat_at(p->u_bar, p, entry->line_id, entry->cell_id);
    for (int i = 0; i < N * N; i++) {
        d_dst[i] = d_src[i];
        l_dst[i] = l_src[i];
        u_dst[i] = u_src[i];
    }
    uint64_t end = read_cycle_counter();
    if (end >= start) {
        stats->coeff_output_drain_cycles += end - start;
        stats->preprocess_drain_cycles += end - start;
    }
    stats->coeff_output_free++;
    entry->valid = 0;
}

static int
prepare_raw_coeff3(Problem *p, const Config *cfg, Stats *stats,
                   PretransformCheck *check)
{
    int all_valid = 1;
    const int full = !strcmp(cfg->pretransform_validate, "full");
    uint64_t total_start = read_cycle_counter();
    uint64_t depth = cfg->coeff_output_buffer_depth;
    CoeffOutputEntry *buffer = (CoeffOutputEntry *)xcalloc_raw(
        depth, sizeof(*buffer), "coeff3_output_buffer");
    uint64_t head = 0, tail = 0, occupancy = 0, generation = 1;
    const uint64_t slots = cfg->coeff_preprocess_spm_slots > 1 ? 2 : 1;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++, generation++) {
            if (occupancy == depth) {
                stats->coeff_output_full_stalls++;
                drain_coeff3_entry(p, &buffer[head], stats);
                head = (head + 1) % depth;
                occupancy--;
            }
            uintptr_t base = coeff_slot_base((generation - 1) % slots);
            stage_coeff3_inputs(mat_const_at(p->lu_d, p, line, cell),
                                mat_const_at(p->l_mat, p, line, cell),
                                mat_const_at(p->u_mat, p, line, cell), base);
            CoeffOutputEntry *entry = &buffer[tail];
            uintptr_t output = SPM_BASE + 0x6000ULL + tail * 0x400ULL;
            entry->d_inv_addr = output;
            entry->l_bar_addr = output + N * N * sizeof(double);
            entry->u_bar_addr = output + 2U * N * N * sizeof(double);
            entry->generation = generation;
            entry->line_id = line;
            entry->cell_id = cell;
            uint64_t compute_start = read_cycle_counter();
            entry->completion_token = trsm5_coeff3_hardware(
                base + SPM_COEFF_LU_OFFSET, base + SPM_COEFF_L_OFFSET,
                base + SPM_COEFF_U_OFFSET, entry->d_inv_addr,
                entry->l_bar_addr, entry->u_bar_addr, generation);
            uint64_t compute_end = read_cycle_counter();
            if (compute_end >= compute_start) {
                stats->coeff3_cycles += compute_end - compute_start;
                stats->preprocess_compute_cycles +=
                    compute_end - compute_start;
            }
            entry->valid = 1;
            tail = (tail + 1) % depth;
            occupancy++;
            stats->coeff_output_alloc++;
            if (occupancy > stats->coeff_output_max_occupancy)
                stats->coeff_output_max_occupancy = occupancy;
            stats->coeff3_issued++;
            stats->coeff3_completed++;
            stats->coeff3_lu_reads++;
            stats->coeff3_columns_solved += 15;
            stats->dinv_generated++;
            stats->lbar_generated++;
            stats->ubar_generated++;
            if (depth > 1 && occupancy > 1) {
                drain_coeff3_entry(p, &buffer[head], stats);
                head = (head + 1) % depth;
                occupancy--;
            }
        }
    }
    while (occupancy) {
        drain_coeff3_entry(p, &buffer[head], stats);
        head = (head + 1) % depth;
        occupancy--;
    }
    free(buffer);

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *lu = mat_const_at(p->lu_d, p, line, cell);
            const double *lower = mat_const_at(p->l_mat, p, line, cell);
            const double *upper = mat_const_at(p->u_mat, p, line, cell);
            const double *d_inv = mat_const_at(p->d_inv, p, line, cell);
            const double *l_bar =
                mat_const_at(p->l_bar_precomputed, p, line, cell);
            const double *u_bar = mat_const_at(p->u_bar, p, line, cell);
            double identity[N * N] = {};
            for (int i = 0; i < N; i++)
                identity[i * N + i] = 1.0;
            double d_residual = 0.0;
            double l_residual = 0.0;
            double u_residual = 0.0;
            if (full) {
                double ref[N * N];
                d_residual = matrix_residual_max(lu, d_inv, identity);
                l_residual = matrix_residual_max(lu, l_bar, lower);
                u_residual = matrix_residual_max(lu, u_bar, upper);
                if (u_residual > stats->max_abs_ubar_residual)
                    stats->max_abs_ubar_residual = u_residual;
                trsm5_mrhs_software(lu, identity, ref);
                update_matrix_compare(&check->d_inv, ref, d_inv,
                                      d_residual, 1.0e-12, 1.0e-12);
                trsm5_mrhs_software(lu, lower, ref);
                update_matrix_compare(&check->l_bar, ref, l_bar,
                                      l_residual, 1.0e-12, 1.0e-12);
                trsm5_mrhs_software(lu, upper, ref);
                update_matrix_compare(&check->u_bar, ref, u_bar,
                                      u_residual, 1.0e-12, 1.0e-12);
            }
            if (full && (d_residual > 1.0e-9 || l_residual > 1.0e-9 ||
                u_residual > 1.0e-9))
                all_valid = 0;
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(d_inv[i]) || !isfinite(l_bar[i]) ||
                    !isfinite(u_bar[i]))
                    all_valid = 0;
            }
        }
    }
    uint64_t total_end = read_cycle_counter();
    if (total_end >= total_start)
        stats->extra_pretransform_cycles += total_end - total_start;
    stats->raw_persistent_coefficient_bytes =
        4U * p->lines * p->cells * N * N * sizeof(double);
    stats->raw_extra_coefficient_bytes =
        2U * p->lines * p->cells * N * N * sizeof(double);
    return all_valid;
}

static void
aggregate_coeff_event_record(Stats *stats, const CoeffPreprocessRecord *r)
{
    uint64_t latency = r->complete_cycle >= r->issue_cycle ?
        r->complete_cycle - r->issue_cycle : 0;
    if (!stats->event_requests_completed) {
        stats->event_first_cell_latency = latency;
        stats->event_first_completion_cycle = r->complete_cycle;
    }
    stats->event_last_cell_latency = latency;
    stats->event_last_completion_cycle = r->complete_cycle;
    stats->event_cell_latency_sum += latency;
    stats->event_dinv_ready_latency_sum += r->d_inv_ready_cycle ?
        r->d_inv_ready_cycle - r->issue_cycle : 0;
    stats->event_lbar_ready_latency_sum += r->l_bar_ready_cycle ?
        r->l_bar_ready_cycle - r->issue_cycle : 0;
    stats->event_ubar_ready_latency_sum += r->u_bar_ready_cycle ?
        r->u_bar_ready_cycle - r->issue_cycle : 0;
    stats->event_input_active_cycles += r->input_active_cycles;
    stats->event_lu_active_cycles += r->lu_active_cycles;
    stats->event_solve_active_cycles += r->solve_active_cycles;
    stats->event_drain_active_cycles += r->drain_active_cycles;
    stats->event_input_lu_overlap_cycles += r->input_lu_overlap_cycles;
    stats->event_input_solve_overlap_cycles += r->input_solve_overlap_cycles;
    stats->event_lu_solve_overlap_cycles += r->lu_solve_overlap_cycles;
    stats->event_compute_drain_overlap_cycles +=
        r->compute_drain_overlap_cycles;
    stats->event_three_way_overlap_cycles += r->three_way_overlap_cycles;
    stats->event_lu_div_issued += r->lu_div_issued;
    stats->event_lu_mul_issued += r->lu_mul_issued;
    stats->event_lu_sub_issued += r->lu_sub_issued;
    stats->event_coeff_div_issued += r->coeff_div_issued;
    stats->event_coeff_mul_issued += r->coeff_mul_issued;
    stats->event_coeff_sub_issued += r->coeff_sub_issued;
    stats->event_divider_busy_lane_cycles += r->divider_busy_lane_cycles;
    stats->event_mul_busy_lane_cycles += r->mul_busy_lane_cycles;
    stats->event_sub_busy_lane_cycles += r->sub_busy_lane_cycles;
    stats->event_stall_input_slot += r->stall_input_slot;
    stats->event_stall_lu_pending += r->stall_lu_pending;
    stats->event_stall_solve_pending += r->stall_solve_pending;
    stats->event_stall_output_ring += r->stall_output_ring;
    stats->event_stall_lu_divider += r->stall_lu_divider;
    stats->event_stall_lu_mulsub += r->stall_lu_mulsub;
    stats->event_stall_coeff_divider += r->stall_coeff_divider;
    stats->event_stall_coeff_mulsub += r->stall_coeff_mulsub;
    stats->event_stall_dependency += r->stall_dependency;
    stats->event_stall_spm_read_port += r->stall_spm_read_port;
    stats->event_stall_spm_write_port += r->stall_spm_write_port;
    stats->event_stall_spm_bank += r->stall_spm_bank;
    stats->event_stall_drain_queue += r->stall_drain_queue;
    stats->event_input_bytes += r->input_bytes;
    stats->event_output_bytes += r->output_bytes;
    stats->event_input_write_requests += r->input_write_requests;
    stats->event_drain_requests += r->drain_requests;
    if (r->max_output_occupancy > stats->event_max_output_occupancy)
        stats->event_max_output_occupancy = r->max_output_occupancy;
    stats->event_scheduler_scans += r->scheduler_scans;
    stats->event_scheduler_selections += r->scheduler_selections;
    stats->event_scheduler_no_selection += r->scheduler_no_selection;
    stats->event_ready_rhs_count_sum += r->ready_rhs_count_sum;
    if (r->ready_rhs_count_max > stats->event_ready_rhs_count_max)
        stats->event_ready_rhs_count_max = r->ready_rhs_count_max;
    stats->event_cycles_with_no_ready_rhs += r->cycles_with_no_ready_rhs;
    stats->event_divider_ready_candidates += r->divider_ready_candidates;
    stats->event_mulsub_ready_candidates += r->mulsub_ready_candidates;
    stats->event_rhs_starvation_cycles += r->rhs_starvation_cycles;
    if (r->rhs_max_wait_cycles > stats->event_rhs_max_wait_cycles)
        stats->event_rhs_max_wait_cycles = r->rhs_max_wait_cycles;
    stats->event_rhs_wait_cycles += r->rhs_wait_cycles;
    stats->event_identity_batch_wait_cycles += r->identity_batch_wait_cycles;
    stats->event_lbar_batch_wait_cycles += r->lbar_batch_wait_cycles;
    stats->event_ubar_batch_wait_cycles += r->ubar_batch_wait_cycles;
    stats->event_coeff_divider_busy_lane_cycles +=
        r->coeff_divider_busy_lane_cycles;
    stats->event_coeff_divider_idle_lane_cycles +=
        r->coeff_divider_idle_lane_cycles;
    stats->event_coeff_mul_busy_lane_cycles += r->coeff_mul_busy_lane_cycles;
    stats->event_coeff_sub_busy_lane_cycles += r->coeff_sub_busy_lane_cycles;
    stats->event_scheduler_cycles += r->scheduler_cycles;
    for (int k = 0; k < N; k++) {
        if (r->lu_step_ready_cycle[k] >= r->lu_issue_cycle)
            stats->event_lu_step_ready_cycle[k] +=
                r->lu_step_ready_cycle[k] - r->lu_issue_cycle;
        stats->event_lu_step_first_use_latency[k] +=
            r->lu_step_first_use_latency[k];
    }
    stats->event_early_solve_issued += r->early_solve_issued;
    stats->event_early_solve_missed += r->early_solve_missed;
    stats->event_trsm_blocked_by_lu_step_cycles +=
        r->trsm_blocked_by_lu_step_cycles;
    stats->event_same_cell_lu_solve_overlap_cycles +=
        r->same_cell_lu_solve_overlap_cycles;
    stats->event_cross_cell_lu_solve_overlap_cycles +=
        r->cross_cell_lu_solve_overlap_cycles;
    stats->event_coeff_spm_packet_issued += r->coeff_spm_packet_issued;
    stats->event_coeff_spm_packet_completed += r->coeff_spm_packet_completed;
    stats->event_coeff_input_packet_bytes += r->coeff_input_packet_bytes;
    stats->event_coeff_drain_packet_bytes += r->coeff_drain_packet_bytes;
    stats->event_coeff_spm_read_port_stalls +=
        r->coeff_spm_read_port_stalls;
    stats->event_coeff_spm_write_port_stalls +=
        r->coeff_spm_write_port_stalls;
    stats->event_coeff_spm_bank_stalls += r->coeff_spm_bank_stalls;
    stats->event_coeff_spm_outstanding_stalls +=
        r->coeff_spm_outstanding_stalls;
    stats->mask_table_lookups += r->mask_table_lookups;
    stats->rhs_mask_scheduler_scans += r->rhs_mask_scheduler_scans;
    stats->rhs_mask_scheduler_useful_issues += r->rhs_mask_useful_issues;
    stats->rhs_mask_scheduler_empty_scans += r->rhs_mask_empty_scans;
    stats->dinv_columns_ready += r->d_inv_columns_ready;
    stats->dinv_columns_queued += r->d_inv_columns_queued;
    stats->dinv_columns_issued += r->d_inv_columns_issued;
    stats->dinv_columns_completed += r->d_inv_columns_completed;
    stats->dinv_columns_consumed += r->d_inv_columns_consumed;
    stats->dinv_columns_duplicate_rejected +=
        r->d_inv_columns_duplicate_rejected;
    stats->dinv_columns_generation_rejected +=
        r->d_inv_columns_generation_rejected;
    stats->dinv_columns_out_of_order_held +=
        r->d_inv_columns_out_of_order_held;
    if (r->column_fma_queue_max_occupancy >
        stats->column_fma_queue_max_occupancy)
        stats->column_fma_queue_max_occupancy =
            r->column_fma_queue_max_occupancy;
    stats->column_fma_queue_full_stalls += r->column_fma_queue_full_stalls;
    stats->column_fma_issue_cycles += r->column_fma_issue_cycles;
    stats->column_fma_busy_cycles += r->column_fma_busy_cycles;
    stats->column_fma_completions += r->column_fma_completions;
    stats->column_fma_retries += r->column_fma_retries;
    stats->column_fma_ready_but_blocked_cycles +=
        r->column_fma_ready_but_blocked_cycles;
    stats->column_fma_queue_occupancy_sum +=
        r->column_fma_queue_occupancy_sum;
    stats->column_fma_queue_sample_cycles +=
        r->column_fma_queue_sample_cycles;
    stats->base_accumulators_allocated += r->base_accumulators_allocated;
    stats->base_accumulators_completed += r->base_accumulators_completed;
    stats->base_accumulators_cancelled += r->base_accumulators_cancelled;
    stats->base_accumulator_missing_columns +=
        r->base_accumulator_missing_columns;
    stats->base_accumulator_double_consumes +=
        r->base_accumulator_double_consumes;
    stats->base_accumulator_stale_writes +=
        r->base_accumulator_stale_writes;
    const uint64_t base_ready_cycle = r->base_ready_cycle ?
        r->base_ready_cycle : r->base_compute_ready_cycle;
    if (base_ready_cycle && (!stats->first_base_ready_cycle ||
        base_ready_cycle < stats->first_base_ready_cycle))
        stats->first_base_ready_cycle = base_ready_cycle;
    if (base_ready_cycle > stats->last_base_ready_cycle)
        stats->last_base_ready_cycle = base_ready_cycle;
    stats->base_vector_publish_count += r->base_vector_publish_count;
    stats->base_vector_publish_bytes += r->base_vector_publish_bytes;
    stats->base_vector_publish_stalls += r->base_vector_publish_stalls;
    stats->dinv_matrix_drains_avoided += r->d_inv_matrix_drains_avoided;
    stats->dinv_matrix_drain_bytes_avoided +=
        r->d_inv_matrix_drain_bytes_avoided;
    stats->streaming_double_column_consumes +=
        r->base_accumulator_double_consumes;
    stats->lbar_columns_ready += r->lbar_columns_ready;
    stats->lbar_columns_queued += r->lbar_columns_queued;
    stats->lbar_columns_issued += r->lbar_columns_issued;
    stats->lbar_columns_completed += r->lbar_columns_completed;
    stats->lbar_columns_consumed += r->lbar_columns_consumed;
    stats->lbar_columns_duplicate_rejected +=
        r->lbar_columns_duplicate_rejected;
    stats->lbar_columns_generation_rejected +=
        r->lbar_columns_generation_rejected;
    stats->lbar_columns_out_of_order_held +=
        r->lbar_columns_out_of_order_held;
    stats->correction_accumulators_allocated +=
        r->correction_accumulators_allocated;
    stats->correction_accumulators_completed +=
        r->correction_accumulators_completed;
    stats->correction_accumulators_cancelled +=
        r->correction_accumulators_cancelled;
    stats->correction_accumulator_missing_columns +=
        r->correction_accumulator_missing_columns;
    stats->correction_accumulator_double_consumes +=
        r->correction_accumulator_double_consumes;
    stats->correction_accumulator_stale_writes +=
        r->correction_accumulator_stale_writes;
    stats->forward_handoffs_produced += r->forward_handoffs_produced;
    stats->forward_handoffs_consumed += r->forward_handoffs_consumed;
    stats->forward_handoff_wait_cycles += r->forward_handoff_wait_cycles;
    if (r->forward_handoff_max_live > stats->forward_handoff_max_live)
        stats->forward_handoff_max_live = r->forward_handoff_max_live;
    stats->forward_handoff_overwrite_errors +=
        r->forward_handoff_overwrite_errors;
    stats->forward_handoff_wrong_cell_errors +=
        r->forward_handoff_wrong_cell_errors;
    stats->forward_handoff_wrong_generation_errors +=
        r->forward_handoff_wrong_generation_errors;
    stats->forward_handoff_double_consumes +=
        r->forward_handoff_double_consumes;
    stats->forward_handoff_missing_consumes +=
        r->forward_handoff_missing_consumes;
    stats->forward_combine_queued += r->forward_combine_queued;
    stats->forward_combine_issued += r->forward_combine_issued;
    stats->forward_combine_completed += r->forward_combine_completed;
    if (r->forward_combine_queue_max_occupancy >
        stats->forward_combine_queue_max_occupancy)
        stats->forward_combine_queue_max_occupancy =
            r->forward_combine_queue_max_occupancy;
    stats->forward_combine_queue_full_stalls +=
        r->forward_combine_queue_full_stalls;
    stats->forward_combine_busy_cycles += r->forward_combine_busy_cycles;
    stats->forward_combine_ready_but_blocked_cycles +=
        r->forward_combine_ready_but_blocked_cycles;
    stats->dqstar_publish_count += r->dq_star_publish_count;
    stats->dqstar_publish_bytes += r->dq_star_publish_bytes;
    stats->dqstar_publish_stalls += r->dq_star_publish_stalls;
    stats->base_standalone_publishes_avoided +=
        r->base_standalone_publishes_avoided;
    stats->base_standalone_publish_bytes_avoided +=
        r->base_standalone_publish_bytes_avoided;
    stats->lbar_matrix_drains_avoided += r->lbar_matrix_drains_avoided;
    stats->lbar_matrix_drain_bytes_avoided +=
        r->lbar_matrix_drain_bytes_avoided;
    stats->lbar_guest_reload_bytes_avoided +=
        r->lbar_guest_reload_bytes_avoided;
    stats->patha_lbar_mvm_eliminated += r->patha_lbar_mvm_eliminated;
    stats->patha_lbar_custom_instructions_eliminated +=
        r->patha_lbar_custom_instructions_eliminated;
    stats->lbar_consumer_ready_but_blocked_cycles +=
        r->lbar_consumer_ready_but_blocked_cycles;
    stats->lbar_consumer_wait_previous_dq_cycles +=
        r->lbar_consumer_wait_previous_dq_cycles;
    stats->lbar_consumer_wait_column_cycles +=
        r->lbar_consumer_wait_column_cycles;
    stats->lbar_consumer_wait_engine_cycles +=
        r->lbar_consumer_wait_engine_cycles;
    stats->consumer_scheduler_aging_promotions +=
        r->consumer_scheduler_aging_promotions;
    stats->consumer_scheduler_starvation_violations +=
        r->consumer_scheduler_starvation_violations;
    stats->frontier_wait_previous_dq_cycles +=
        r->frontier_wait_previous_dq_cycles;
    stats->frontier_wait_lbar_columns_cycles +=
        r->frontier_wait_lbar_columns_cycles;
    stats->frontier_wait_consumer_engine_cycles +=
        r->frontier_wait_consumer_engine_cycles;
    stats->frontier_wait_dqstar_publish_cycles +=
        r->frontier_wait_dqstar_publish_cycles;
    stats->event_requests_completed++;
}

static int
reap_coeff_event(CoeffPreprocessEntry *entry, Stats *stats)
{
    uint64_t status;
    do {
        status = coeff_preprocess_wait(entry->token);
    } while (status == COEFF_PRE_STATUS_BUSY ||
             status == COEFF_PRE_STATUS_DINV_READY ||
             status == COEFF_PRE_STATUS_DINV_LBAR_READY ||
             status == COEFF_PRE_STATUS_DINV_LBAR_UBAR_READY);
    __asm__ __volatile__("" ::: "memory");
    stats->event_requests_freed++;
    entry->valid = 0;
    if (status != COEFF_PRE_STATUS_COMPLETE ||
        entry->record.status != COEFF_PRE_STATUS_COMPLETE) {
        stats->event_requests_failed++;
        return 0;
    }
    aggregate_coeff_event_record(stats, &entry->record);
    return 1;
}

static void
init_b4_test_descriptor(Problem *p, const Config *cfg,
                        CoeffPreprocessDescriptor *d,
                        CoeffPreprocessRecord *record, double *lu_out,
                        double *dq_out, uint64_t line_id, int cell,
                        uint64_t generation, uint64_t extra_flags)
{
    memset(record, 0, sizeof(*record));
    memset(d, 0, sizeof(*d));
    d->version = 1;
    d->size = sizeof(*d);
    d->flags = COEFF_PRE_COEFF3 | COEFF_PRE_PARTIAL_OUTPUT |
        COEFF_PRE_LU_FORWARD | COEFF_PRE_STREAMING_PROGRESS |
        COEFF_PRE_STREAMING_AUTO_RETIRE | COEFF_PRE_NEED_DINV |
        COEFF_PRE_DINV_BASE_CONSUMER | COEFF_PRE_DINV_DIRECT_BYPASS |
        COEFF_PRE_LBAR_FORWARD_CONSUMER | COEFF_PRE_LBAR_DIRECT_BYPASS |
        extra_flags;
    if (cell)
        d->flags |= COEFF_PRE_NEED_LBAR;
    d->generation = generation;
    d->line_id = line_id;
    d->cell_id = (uint64_t)cell;
    d->d_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->d_mat, p, 0, cell);
    d->l_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->l_mat, p, 0, cell);
    d->u_addr = (uint64_t)(uintptr_t)
        mat_const_at(p->u_mat, p, 0, cell);
    d->lu_out_addr = (uint64_t)(uintptr_t)lu_out;
    d->record_addr = (uint64_t)(uintptr_t)record;
    d->pivot_epsilon = cfg->pretransform_diag_epsilon;
    d->reserved[0] = (uint64_t)(uintptr_t)
        vec_const_at(p->rhs, p, 0, cell);
    d->reserved[3] = (uint64_t)(uintptr_t)dq_out;
}

static int
run_b4_handoff_fault_test(Problem *p, const Config *cfg,
                          uint64_t fault_flag, uint64_t line_id,
                          int consumer_cell)
{
    double lu_out[2][N * N];
    double dq_out[2][N];
    CoeffPreprocessRecord records[2];
    CoeffPreprocessDescriptor descs[2];
    memset(lu_out, 0, sizeof(lu_out));
    memset(dq_out, 0, sizeof(dq_out));
    memset(records, 0, sizeof(records));
    memset(descs, 0, sizeof(descs));

    const int cells[2] = {0, consumer_cell};
    uint64_t tokens[2] = {0, 0};
    for (int request = 0; request < 2; ++request) {
        const int cell = cells[request];
        CoeffPreprocessDescriptor *d = &descs[request];
        init_b4_test_descriptor(p, cfg, d, &records[request],
            lu_out[request], dq_out[request], line_id, cell,
            (uint64_t)request + 1, !request ? fault_flag : 0);
        tokens[request] = coeff_preprocess_launch(d);
    }

    const uint64_t start = read_cycle_counter();
    while (tokens[0] && tokens[1] &&
           (!records[0].auto_retired || !records[1].auto_retired) &&
           read_cycle_counter() - start < 2000000U)
        __asm__ __volatile__("" ::: "memory");

    const int wrong_cell =
        fault_flag == COEFF_PRE_TEST_HANDOFF_WRONG_CELL;
    const uint64_t expected = wrong_cell ? COEFF_PRE_STATUS_INTERNAL_ERROR :
                                           COEFF_PRE_STATUS_GENERATION_MISMATCH;
    const int pass = tokens[0] && tokens[1] && records[0].auto_retired &&
        records[1].auto_retired && records[0].status == COEFF_PRE_STATUS_COMPLETE &&
        records[1].status == expected && records[1].forward_consumer_error &&
        (wrong_cell ? records[1].forward_handoff_wrong_cell_errors == 1 :
                      records[1].forward_handoff_wrong_generation_errors == 1) &&
        coeff_preprocess_wait(tokens[0]) == COEFF_PRE_STATUS_BAD_TOKEN &&
        coeff_preprocess_wait(tokens[1]) == COEFF_PRE_STATUS_BAD_TOKEN;
    printf("lusgs.coeffCancel.b4HandoffFault.%s = %d\n",
           wrong_cell ? "wrongCell" : "wrongGeneration", pass);
    return pass;
}

static int
run_b4_cancel_phase_test(Problem *p, const Config *cfg, int phase,
                         uint64_t line_id)
{
    double lu_out[2][N * N] = {{0}};
    double dq_out[2][N] = {{0}};
    CoeffPreprocessRecord records[2];
    CoeffPreprocessDescriptor descs[2];
    uint64_t tokens[2] = {0, 0};
    const int consumer_only = phase == 0;
    const int producer_only = phase == 3;
    if (!consumer_only) {
        init_b4_test_descriptor(p, cfg, &descs[0], &records[0],
            lu_out[0], dq_out[0], line_id, 0, 1, 0);
        if (producer_only)
            descs[0].flags |= COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH;
        tokens[0] = coeff_preprocess_launch(&descs[0]);
    }
    if (!producer_only) {
        init_b4_test_descriptor(p, cfg, &descs[1], &records[1],
            lu_out[1], dq_out[1], line_id, 1,
            consumer_only ? 1 : 2,
            phase == 2 ? COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH : 0);
        tokens[1] = coeff_preprocess_launch(&descs[1]);
    }

    const int target = producer_only ? 0 : 1;
    uint64_t start = read_cycle_counter();
    int reached = phase == 0;
    while (tokens[target] && !records[target].auto_retired && !reached &&
           read_cycle_counter() - start < 4000000U) {
        // Test-only progress polling snapshots internal phase state into the
        // stable record; the normal auto-retire path does not issue WAIT.
        (void)coeff_preprocess_wait(tokens[target]);
        if (phase == 1)
            reached = records[target].lbar_columns_consumed > 0 &&
                records[target].lbar_columns_consumed < N;
        else if (phase == 2)
            reached = records[target].correction_accumulators_completed == 1 &&
                records[target].dq_star_publish_count == 0;
        else if (phase == 3)
            reached = records[target].forward_handoffs_produced == 1 &&
                records[target].dq_star_publish_count == 0;
        __asm__ __volatile__("" ::: "memory");
    }
    uint64_t cancel_status = tokens[target] ?
        coeff_preprocess_cancel(tokens[target]) : COEFF_PRE_STATUS_BAD_TOKEN;
    while (tokens[target] && !records[target].auto_retired &&
           read_cycle_counter() - start < 4000000U)
        __asm__ __volatile__("" ::: "memory");
    if (tokens[1 - target]) {
        while (!records[1 - target].auto_retired &&
               read_cycle_counter() - start < 4000000U)
            __asm__ __volatile__("" ::: "memory");
    }
    int pass = tokens[target] && reached &&
        (cancel_status == COEFF_PRE_STATUS_CANCELLED ||
         cancel_status == COEFF_PRE_STATUS_CANCEL_PENDING) &&
        records[target].status == COEFF_PRE_STATUS_CANCELLED &&
        records[target].auto_retired && records[target].auto_retire_error &&
        records[target].dq_star_publish_count == 0 &&
        coeff_preprocess_wait(tokens[target]) == COEFF_PRE_STATUS_BAD_TOKEN;
    if (phase == 1)
        pass &= records[target].lbar_columns_consumed > 0 &&
            records[target].lbar_columns_consumed < N;
    if (phase == 2)
        pass &= records[target].correction_accumulators_completed == 1;
    if (phase == 3)
        pass &= records[target].forward_handoffs_produced == 1;
    printf("lusgs.coeffCancel.b4Phase%d = %d\n", phase, pass);
    if (!pass)
        printf("lusgs.coeffCancel.b4Phase%dDebug = reached:%d cancel:%lu "
               "status:%lu auto:%lu corr:%lu handoff:%lu publish:%lu\n",
               phase, reached, (unsigned long)cancel_status,
               (unsigned long)records[target].status,
               (unsigned long)records[target].auto_retired,
               (unsigned long)records[target].correction_accumulators_completed,
               (unsigned long)records[target].forward_handoffs_produced,
               (unsigned long)records[target].dq_star_publish_count);
    return pass;
}

static int
run_coeff_cancel_test(Problem *p, const Config *cfg)
{
    if (strcmp(cfg->coeff_preprocess_model, "event"))
        return 0;
    double lu_out[N * N], d_inv_out[N * N];
    double l_bar_out[N * N], u_bar_out[N * N];
    for (int i = 0; i < N * N; ++i) {
        lu_out[i] = d_inv_out[i] = 1234567.0;
        l_bar_out[i] = u_bar_out[i] = 1234567.0;
    }
    CoeffPreprocessRecord record;
    CoeffPreprocessDescriptor desc;
    memset(&record, 0, sizeof(record));
    memset(&desc, 0, sizeof(desc));
    desc.version = 1;
    desc.size = sizeof(desc);
    desc.flags = COEFF_PRE_COEFF3 | COEFF_PRE_PARTIAL_OUTPUT |
                 COEFF_PRE_LU_FORWARD;
    desc.generation = 1;
    desc.line_id = UINT64_MAX - 1;
    desc.cell_id = 0;
    desc.d_addr = (uint64_t)(uintptr_t)mat_const_at(p->d_mat, p, 0, 0);
    desc.l_addr = (uint64_t)(uintptr_t)mat_const_at(p->l_mat, p, 0, 0);
    desc.u_addr = (uint64_t)(uintptr_t)mat_const_at(p->u_mat, p, 0, 0);
    desc.lu_out_addr = (uint64_t)(uintptr_t)lu_out;
    desc.d_inv_out_addr = (uint64_t)(uintptr_t)d_inv_out;
    desc.l_bar_out_addr = (uint64_t)(uintptr_t)l_bar_out;
    desc.u_bar_out_addr = (uint64_t)(uintptr_t)u_bar_out;
    desc.record_addr = (uint64_t)(uintptr_t)&record;
    desc.pivot_epsilon = cfg->pretransform_diag_epsilon;

    uint64_t token = coeff_preprocess_launch(&desc);
    uint64_t observed = COEFF_PRE_STATUS_BUSY;
    for (int poll = 0; token && poll < 8; ++poll) {
        observed = coeff_preprocess_wait(token);
        if (observed != COEFF_PRE_STATUS_BUSY)
            break;
    }
    uint64_t cancel_status = token ? coeff_preprocess_cancel(token) :
                                     COEFF_PRE_STATUS_BAD_TOKEN;
    uint64_t wait_status = token ? coeff_preprocess_wait(token) :
                                   COEFF_PRE_STATUS_BAD_TOKEN;
    uint64_t post_reap_status = token ? coeff_preprocess_cancel(token) :
                                        COEFF_PRE_STATUS_BAD_TOKEN;
    memset(&record, 0, sizeof(record));
    uint64_t stale_token = coeff_preprocess_launch(&desc);
    uint64_t stale_status = stale_token ? coeff_preprocess_wait(stale_token) :
                                          COEFF_PRE_STATUS_BAD_TOKEN;
    int outputs_unchanged = 1;
    for (int i = 0; i < N * N; ++i) {
        outputs_unchanged &= lu_out[i] == 1234567.0;
        outputs_unchanged &= d_inv_out[i] == 1234567.0;
        outputs_unchanged &= l_bar_out[i] == 1234567.0;
        outputs_unchanged &= u_bar_out[i] == 1234567.0;
    }
    int pass = token && observed == COEFF_PRE_STATUS_BUSY &&
        (cancel_status == COEFF_PRE_STATUS_CANCELLED ||
         cancel_status == COEFF_PRE_STATUS_CANCEL_PENDING) &&
        wait_status == COEFF_PRE_STATUS_CANCELLED &&
        post_reap_status == COEFF_PRE_STATUS_BAD_TOKEN &&
        stale_status == COEFF_PRE_STATUS_GENERATION_MISMATCH &&
        outputs_unchanged;

    // Stage B1.5 negative lifecycle: cancel an auto-retire request, wait for
    // the stable terminal record with ordinary loads, then prove the released
    // token reports BadToken rather than masquerading as a live generation.
    memset(&record, 0, sizeof(record));
    desc.flags = COEFF_PRE_COEFF3 | COEFF_PRE_PARTIAL_OUTPUT |
                 COEFF_PRE_LU_FORWARD | COEFF_PRE_STREAMING_PROGRESS |
                 COEFF_PRE_STREAMING_AUTO_RETIRE;
    desc.generation = 1;
    desc.line_id = UINT64_MAX - 2;
    uint64_t auto_token = coeff_preprocess_launch(&desc);
    uint64_t auto_cancel_status = auto_token ?
        coeff_preprocess_cancel(auto_token) : COEFF_PRE_STATUS_BAD_TOKEN;
    while (auto_token && !record.auto_retired)
        __asm__ __volatile__("" ::: "memory");
    uint64_t auto_post_retire_wait = auto_token ?
        coeff_preprocess_wait(auto_token) : COEFF_PRE_STATUS_BAD_TOKEN;
    int auto_cancel_pass = auto_token &&
        (auto_cancel_status == COEFF_PRE_STATUS_CANCELLED ||
         auto_cancel_status == COEFF_PRE_STATUS_CANCEL_PENDING) &&
        record.status == COEFF_PRE_STATUS_CANCELLED &&
        record.auto_retired && record.auto_retire_error &&
        auto_post_retire_wait == COEFF_PRE_STATUS_BAD_TOKEN;
    pass &= auto_cancel_pass;

    // Stage B3 negative lifecycle: observe a genuinely partial, in-order
    // DInv accumulator through the progress record, cancel it, and prove that
    // neither a queued ColumnFma callback nor auto-retirement publishes base.
    int b3_partial_cancel_pass = 1;
    uint64_t b3_columns_before_cancel = 0;
    uint64_t b3_columns_after_cancel = 0;
    uint64_t b3_cancel_status = COEFF_PRE_STATUS_BAD_TOKEN;
    uint64_t b3_post_retire_wait = COEFF_PRE_STATUS_BAD_TOKEN;
    if (strcmp(cfg->coeff_stream_dinv_consumer, "off")) {
        double base_out[N];
        for (int lane = 0; lane < N; ++lane)
            base_out[lane] = 1234567.0;
        memset(&record, 0, sizeof(record));
        desc.flags = COEFF_PRE_COEFF3 | COEFF_PRE_PARTIAL_OUTPUT |
                     COEFF_PRE_LU_FORWARD | COEFF_PRE_STREAMING_PROGRESS |
                     COEFF_PRE_STREAMING_AUTO_RETIRE |
                     COEFF_PRE_NEED_DINV |
                     COEFF_PRE_DINV_BASE_CONSUMER;
        if (!strcmp(cfg->coeff_stream_dinv_consumer, "direct"))
            desc.flags |= COEFF_PRE_DINV_DIRECT_BYPASS;
        desc.generation = 1;
        desc.line_id = UINT64_MAX - 3;
        desc.reserved[0] = (uint64_t)(uintptr_t)
            vec_const_at(p->rhs, p, 0, 0);
        desc.reserved[1] = (uint64_t)(uintptr_t)base_out;
        uint64_t b3_token = coeff_preprocess_launch(&desc);
        uint64_t b3_poll_start = read_cycle_counter();
        while (b3_token && !record.d_inv_columns_consumed &&
               !record.auto_retired &&
               read_cycle_counter() - b3_poll_start < 1000000U)
            __asm__ __volatile__("" ::: "memory");
        b3_columns_before_cancel = record.d_inv_columns_consumed;
        b3_cancel_status = b3_token ? coeff_preprocess_cancel(b3_token) :
                                      COEFF_PRE_STATUS_BAD_TOKEN;
        while (b3_token && !record.auto_retired &&
               read_cycle_counter() - b3_poll_start < 1000000U)
            __asm__ __volatile__("" ::: "memory");
        b3_columns_after_cancel = record.d_inv_columns_consumed;
        b3_post_retire_wait = b3_token ? coeff_preprocess_wait(b3_token) :
                                         COEFF_PRE_STATUS_BAD_TOKEN;
        int base_unchanged = 1;
        for (int lane = 0; lane < N; ++lane)
            base_unchanged &= base_out[lane] == 1234567.0;
        b3_partial_cancel_pass = b3_token &&
            b3_columns_before_cancel > 0 &&
            b3_columns_before_cancel < N &&
            b3_columns_after_cancel >= b3_columns_before_cancel &&
            b3_columns_after_cancel < N &&
            (b3_cancel_status == COEFF_PRE_STATUS_CANCELLED ||
             b3_cancel_status == COEFF_PRE_STATUS_CANCEL_PENDING) &&
            record.status == COEFF_PRE_STATUS_CANCELLED &&
            record.auto_retired && record.auto_retire_error &&
            record.base_accumulators_cancelled == 1 &&
            record.base_vector_publish_count == 0 &&
            record.base_ready == 0 && base_unchanged &&
            b3_post_retire_wait == COEFF_PRE_STATUS_BAD_TOKEN;
        pass &= b3_partial_cancel_pass;
    }
    int b4_handoff_fault_pass = 1;
    int b4_cancel_phase_pass = 1;
    if (strcmp(cfg->coeff_stream_lbar_consumer, "off")) {
        b4_handoff_fault_pass =
            run_b4_handoff_fault_test(
                p, cfg, COEFF_PRE_TEST_HANDOFF_WRONG_CELL,
                UINT64_MAX - 4, 2) &&
            run_b4_handoff_fault_test(
                p, cfg, COEFF_PRE_TEST_HANDOFF_WRONG_GENERATION,
                UINT64_MAX - 5, 1);
        pass &= b4_handoff_fault_pass;
        for (int phase = 0; phase < 4; ++phase)
            b4_cancel_phase_pass &= run_b4_cancel_phase_test(
                p, cfg, phase, UINT64_MAX - 6 - (uint64_t)phase);
        pass &= b4_cancel_phase_pass;
    }
    printf("lusgs.coeffCancel.token = %lu\n", (unsigned long)token);
    printf("lusgs.coeffCancel.issueStatus = %lu\n",
           (unsigned long)cancel_status);
    printf("lusgs.coeffCancel.waitStatus = %lu\n",
           (unsigned long)wait_status);
    printf("lusgs.coeffCancel.postReapStatus = %lu\n",
           (unsigned long)post_reap_status);
    printf("lusgs.coeffCancel.generationMismatchStatus = %lu\n",
           (unsigned long)stale_status);
    printf("lusgs.coeffCancel.outputsUnchanged = %d\n", outputs_unchanged);
    printf("lusgs.coeffCancel.autoRetireStatus = %lu\n",
           (unsigned long)record.status);
    printf("lusgs.coeffCancel.autoPostRetireWait = %lu\n",
           (unsigned long)auto_post_retire_wait);
    printf("lusgs.coeffCancel.autoRetireStableRecord = %d\n",
           auto_cancel_pass);
    printf("lusgs.coeffCancel.b3ColumnsBeforeCancel = %lu\n",
           (unsigned long)b3_columns_before_cancel);
    printf("lusgs.coeffCancel.b3ColumnsAfterCancel = %lu\n",
           (unsigned long)b3_columns_after_cancel);
    printf("lusgs.coeffCancel.b3CancelStatus = %lu\n",
           (unsigned long)b3_cancel_status);
    printf("lusgs.coeffCancel.b3PostRetireWait = %lu\n",
           (unsigned long)b3_post_retire_wait);
    printf("lusgs.coeffCancel.b3PartialAccumulator = %d\n",
           b3_partial_cancel_pass);
    printf("lusgs.coeffCancel.b4HandoffFaults = %d\n",
           b4_handoff_fault_pass);
    printf("lusgs.coeffCancel.b4CancelPhases = %d\n",
           b4_cancel_phase_pass);
    printf("%s\n", pass ? "LUSGS_COEFF_CANCEL_PASS" :
                           "LUSGS_COEFF_CANCEL_FAIL");
    return pass;
}

static int
prepare_raw_event(Problem *p, const Config *cfg, Stats *stats,
                  PretransformCheck *check, int coeff3)
{
    static uint64_t next_generation = 1;
    const uint64_t cells = (uint64_t)p->lines * p->cells;
    const uint64_t matrix_bytes = cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = cells * N * sizeof(double);
    uint64_t depth = cfg->coeff_output_buffer_depth;
    CoeffPreprocessEntry *ring = (CoeffPreprocessEntry *)xcalloc_raw(
        depth, sizeof(*ring), "coeff_event_ring");
    uint64_t head = 0, tail = 0, live = 0;
    uint64_t total_start = read_cycle_counter();
    int all_valid = 1;

    stats->raw_input_d_bytes += matrix_bytes;
    stats->raw_input_l_bytes += matrix_bytes;
    stats->raw_input_u_bytes += matrix_bytes;
    stats->raw_input_r_bytes += rhs_bytes;
    stats->coefficient_rebuilds++;

    if (!strcmp(cfg->lu5_model, "software")) {
        uint64_t lu_start = read_serialized_cycle_counter();
        for (size_t line = 0; line < p->lines; line++) {
            for (size_t cell = 0; cell < p->cells; cell++) {
                if (!cfd_lu5_factor(
                        mat_const_at(p->d_mat, p, line, cell),
                        mat_at(p->lu_d, p, line, cell),
                        cfg->pretransform_diag_epsilon)) {
                    stats->lu5_factor_failures++;
                    all_valid = 0;
                }
            }
        }
        uint64_t lu_end = read_serialized_cycle_counter();
        if (lu_end >= lu_start) {
            stats->lu5_factor_cycles += lu_end - lu_start;
            stats->common_lu_cycles += lu_end - lu_start;
        }
    }

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            uint64_t generation = next_generation++;
            if (live == depth) {
                all_valid &= reap_coeff_event(&ring[head], stats);
                head = (head + 1) % depth;
                live--;
            }
            CoeffPreprocessEntry *entry = &ring[tail];
            memset(entry, 0, sizeof(*entry));
            CoeffPreprocessDescriptor *d = &entry->descriptor;
            d->version = 1;
            d->size = sizeof(*d);
            d->flags = coeff3 ? COEFF_PRE_COEFF3 : COEFF_PRE_TRSV;
            if (!strcmp(cfg->lu5_model, "software"))
                d->flags |= COEFF_PRE_PACKED_LU_INPUT;
            if (cfg->coeff3_partial_output)
                d->flags |= COEFF_PRE_PARTIAL_OUTPUT;
            if (cfg->lu_forwarding)
                d->flags |= COEFF_PRE_LU_FORWARD;
            d->generation = generation;
            d->line_id = line;
            d->cell_id = cell;
            d->d_addr = (uint64_t)(uintptr_t)(
                !strcmp(cfg->lu5_model, "software") ?
                mat_const_at(p->lu_d, p, line, cell) :
                mat_const_at(p->d_mat, p, line, cell));
            d->l_addr = coeff3 ? (uint64_t)(uintptr_t)
                mat_const_at(p->l_mat, p, line, cell) : 0;
            d->u_addr = (uint64_t)(uintptr_t)
                mat_const_at(p->u_mat, p, line, cell);
            d->lu_out_addr = (uint64_t)(uintptr_t)
                mat_at(p->lu_d, p, line, cell);
            d->d_inv_out_addr = coeff3 ? (uint64_t)(uintptr_t)
                mat_at(p->d_inv, p, line, cell) : 0;
            d->l_bar_out_addr = coeff3 ? (uint64_t)(uintptr_t)
                mat_at(p->l_bar_precomputed, p, line, cell) : 0;
            d->u_bar_out_addr = (uint64_t)(uintptr_t)
                mat_at(p->u_bar, p, line, cell);
            d->record_addr = (uint64_t)(uintptr_t)&entry->record;
            d->pivot_epsilon = cfg->pretransform_diag_epsilon;
            entry->token = coeff_preprocess_launch(d);
            if (!entry->token) {
                if (live) {
                    all_valid &= reap_coeff_event(&ring[head], stats);
                    head = (head + 1) % depth;
                    live--;
                    cell--;
                    continue;
                }
                all_valid = 0;
                stats->event_requests_failed++;
                break;
            }
            entry->valid = 1;
            stats->event_requests_allocated++;
            live++;
            if (live > stats->event_requests_max_live)
                stats->event_requests_max_live = live;
            tail = (tail + 1) % depth;
        }
        if (!all_valid)
            break;
    }
    while (live) {
        all_valid &= reap_coeff_event(&ring[head], stats);
        head = (head + 1) % depth;
        live--;
    }
    free(ring);

    uint64_t total_end = read_cycle_counter();
    uint64_t elapsed = total_end >= total_start ? total_end - total_start : 0;
    stats->preprocess_cycles += elapsed;
    if (coeff3)
        stats->extra_pretransform_cycles += elapsed;
    else
        stats->common_preprocess_cycles += elapsed;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            uint64_t ordinal = (uint64_t)line * p->cells + cell;
            int validate = !strcmp(cfg->coeff_validation, "full") ||
                (!strcmp(cfg->coeff_validation, "sampled") &&
                 ordinal % cfg->coeff_validation_sample_rate == 0);
            const double *raw_d = mat_const_at(p->d_mat, p, line, cell);
            const double *lu = mat_const_at(p->lu_d, p, line, cell);
            const double *upper = mat_const_at(p->u_mat, p, line, cell);
            const double *u_bar = mat_const_at(p->u_bar, p, line, cell);
            stats->lu5_factor_cells++;
            stats->lu5_factor_input_bytes += N * N * sizeof(double);
            stats->lu5_factor_output_bytes += N * N * sizeof(double);
            stats->ubar_generated++;
            if (coeff3) {
                stats->coeff3_issued++;
                stats->coeff3_completed++;
                stats->coeff3_columns_solved += 15;
                stats->dinv_generated++;
                stats->lbar_generated++;
            } else {
                stats->ubar_trsm_issued++;
                stats->ubar_trsm_completed++;
                stats->ubar_columns_solved += N;
            }
            for (int i = 0; i < N * N; i++) {
                if (!isfinite(lu[i]) || !isfinite(u_bar[i]))
                    all_valid = 0;
            }
            if (!validate)
                continue;
            uint64_t validation_start = read_cycle_counter();
            double lu_residual = lu_factor_residual_max(raw_d, lu);
            double u_residual = matrix_residual_max(lu, u_bar, upper);
            double u_relative =
                matrix_residual_max_relative(lu, u_bar, upper);
            if (lu_residual > stats->max_abs_lu_residual)
                stats->max_abs_lu_residual = lu_residual;
            if (u_residual > stats->max_abs_ubar_residual)
                stats->max_abs_ubar_residual = u_residual;
            if (u_relative > stats->max_rel_ubar_residual)
                stats->max_rel_ubar_residual = u_relative;
            if (lu_residual > 1.0e-9 || u_residual > 1.0e-9)
                all_valid = 0;
            if (coeff3) {
                double identity[N * N] = {};
                double ref[N * N];
                for (int i = 0; i < N; i++)
                    identity[i * N + i] = 1.0;
                const double *lower =
                    mat_const_at(p->l_mat, p, line, cell);
                const double *d_inv =
                    mat_const_at(p->d_inv, p, line, cell);
                const double *l_bar =
                    mat_const_at(p->l_bar_precomputed, p, line, cell);
                double d_residual = matrix_residual_max(lu, d_inv, identity);
                double l_residual = matrix_residual_max(lu, l_bar, lower);
                if (d_residual > 1.0e-9 || l_residual > 1.0e-9)
                    all_valid = 0;
                if (check) {
                    trsm5_mrhs_software(lu, identity, ref);
                    update_matrix_compare(&check->d_inv, ref, d_inv,
                                          d_residual, 1.0e-12, 1.0e-12);
                    trsm5_mrhs_software(lu, lower, ref);
                    update_matrix_compare(&check->l_bar, ref, l_bar,
                                          l_residual, 1.0e-12, 1.0e-12);
                    trsm5_mrhs_software(lu, upper, ref);
                    update_matrix_compare(&check->u_bar, ref, u_bar,
                                          u_residual, 1.0e-12, 1.0e-12);
                }
                for (int i = 0; i < N * N; i++) {
                    if (!isfinite(d_inv[i]) || !isfinite(l_bar[i]))
                        all_valid = 0;
                }
            }
            uint64_t validation_end = read_cycle_counter();
            if (validation_end >= validation_start) {
                stats->preprocess_validation_cycles +=
                    validation_end - validation_start;
                stats->preprocess_residual_check_cycles +=
                    validation_end - validation_start;
            }
        }
    }
    if (stats->event_requests_completed) {
        stats->lu5_factor_cycles += stats->event_lu_active_cycles;
        stats->common_lu_cycles += stats->event_lu_active_cycles;
        stats->ubar_cycles += stats->event_solve_active_cycles;
        stats->common_ubar_cycles += stats->event_solve_active_cycles;
        stats->coeff3_cycles += coeff3 ? stats->event_solve_active_cycles : 0;
    }
    stats->raw_persistent_coefficient_bytes =
        (coeff3 ? 4U : 2U) * matrix_bytes;
    stats->raw_extra_coefficient_bytes = coeff3 ? 2U * matrix_bytes : 0;
    return all_valid;
}

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

static int
validate_streaming_coefficients(Problem *p, const Config *cfg, Stats *stats,
                                PretransformCheck *check,
                                const double *base_vectors)
{
    int all_valid = 1;
    const int direct = !strcmp(cfg->coeff_stream_dinv_consumer, "direct");
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
        const double *lu = mat_const_at(p->lu_d, p, line, cell);
        const double *d_inv = mat_const_at(p->d_inv, p, line, cell);
        const double *l_bar =
            mat_const_at(p->l_bar_precomputed, p, line, cell);
        const double *u_bar = mat_const_at(p->u_bar, p, line, cell);
        stats->lu5_factor_cells++;
        stats->lu5_factor_input_bytes += N * N * sizeof(double);
        stats->lu5_factor_output_bytes += N * N * sizeof(double);
        stats->ubar_generated += need_ubar;
        stats->coeff3_issued++;
        stats->coeff3_completed++;
        stats->coeff3_columns_solved += 5 + 5 * need_lbar + 5 * need_ubar;
        stats->dinv_generated++;
        stats->lbar_generated += need_lbar;
        for (int i = 0; i < N * N; i++) {
            if (!isfinite(lu[i]) || (!direct && !isfinite(d_inv[i])) ||
                (need_lbar && !isfinite(l_bar[i])) ||
                (need_ubar && !isfinite(u_bar[i])))
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
    const uint64_t total_cells = p->lines * p->cells;
    const uint64_t matrix_bytes = total_cells * N * N * sizeof(double);
    const uint64_t rhs_bytes = total_cells * N * sizeof(double);
    CoeffPreprocessRecord *records =
        (CoeffPreprocessRecord *)xcalloc_raw(
            total_cells, sizeof(*records), "coeff_stream_line_records");
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
    const uint64_t total_start = read_cycle_counter();

    for (size_t line = 0; line < p->lines; ++line) {
    CoeffPreprocessDescriptor *desc = &descs[line];
    const int cache_enabled =
        cfg->sweeps > 1 || cfg->coefficient_update_interval > 1;
    desc->version = 1;
    desc->size = sizeof(*desc);
    desc->flags = COEFF_PRE_COEFF3 | COEFF_PRE_STREAMING_PROGRESS |
        COEFF_PRE_STREAMING_AUTO_RETIRE | COEFF_PRE_DINV_BASE_CONSUMER |
        COEFF_PRE_DINV_DIRECT_BYPASS |
        COEFF_PRE_LBAR_FORWARD_CONSUMER |
        COEFF_PRE_LBAR_DIRECT_BYPASS |
        COEFF_PRE_LINE_AUTONOMOUS;
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
    desc->pivot_epsilon = cfg->pretransform_diag_epsilon;
    desc->reserved[0] = (uint64_t)(uintptr_t)
        vec_at(p->rhs, p, line, 0);
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
    stats->raw_input_d_bytes += matrix_bytes;
    stats->raw_input_l_bytes += matrix_bytes;
    stats->raw_input_u_bytes += matrix_bytes;
    stats->raw_input_r_bytes += rhs_bytes;
    if (coefficient_dirty) {
        stats->coefficient_rebuilds++;
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
        stats->streaming_effective_rhs_count +=
            5 * p->lines *
            (p->cells + (p->cells > 1 ? 2 * (p->cells - 1) : 0));
    } else {
        stats->coefficient_reuses++;
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
        }
        stats->progress_poll_calls += polls + 1;
        stats->progress_poll_busy += polls;
        stats->progress_poll_useful++;
        stats->guest_completion_record_checks += polls + 1;
        for (size_t line = 0; line < p->lines; ++line) {
            stats->progress_wait_calls++;
            if (coeff_preprocess_wait(tokens[line]) !=
                COEFF_PRE_STATUS_COMPLETE)
                all_valid = 0;
        }
    }

    if (strcmp(cfg->coeff_stream_ubar_consumer, "direct"))
        stats->streaming_forward_finish_cycle = read_cycle_counter();
    if (coefficient_dirty) {
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
        memcpy(dq_star, p->dq_star_raw_ref, rhs_bytes);
        memcpy(dq, p->dq_raw_ref, rhs_bytes);
    }

    const uint64_t compute_finish = read_cycle_counter();
    stats->streaming_total_cycles += compute_finish - total_start;
    // Stable-record verification and aggregation are reporting work.  Keep
    // them outside the measured solve so the controller-to-backward handoff
    // is not separated by per-cell guest bookkeeping.
    for (size_t line = 0; coefficient_dirty && line < p->lines; ++line) {
      for (size_t cell = 0; cell < p->cells; ++cell) {
        const CoeffPreprocessRecord *record =
            &records[line * p->cells + cell];
        if (record->status != COEFF_PRE_STATUS_COMPLETE ||
            !record->dq_star_ready || record->forward_consumer_error ||
            record->generation != descs[line].generation + cell ||
            record->line_id != line || record->cell_id != cell)
            all_valid = 0;
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
    return all_valid;
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

static int
run_trsv5_raw(Problem *p, const Config *cfg, Stats *stats,
              double *dq_star, double *dq, int use_context)
{
    uint64_t interval = cfg->coefficient_update_interval ?
        cfg->coefficient_update_interval : cfg->sweeps;
    uint64_t completed = 0;
    int all_valid = 1;
    double *legacy_lu = p->lu_a;
    double *legacy_l = p->c_mat;
    double *legacy_u_bar = p->b_bar;
    while (completed < cfg->sweeps) {
        uint64_t chunk = cfg->sweeps - completed;
        if (chunk > interval)
            chunk = interval;
        int valid = !strcmp(cfg->coeff_preprocess_model, "event") ?
            prepare_raw_event(p, cfg, stats, NULL, 0) :
            prepare_raw_common(p, cfg, stats, 1);
        all_valid &= valid;
        if (!valid) {
            stats->common_preprocess_fallback_runs++;
            size_t bytes = p->lines * p->cells * N * sizeof(double);
            memcpy(dq_star, p->dq_star_raw_ref, bytes);
            memcpy(dq, p->dq_raw_ref, bytes);
            break;
        }
        Config chunk_cfg = *cfg;
        chunk_cfg.sweeps = chunk;
        p->lu_a = p->lu_d;
        p->c_mat = p->l_mat;
        p->b_bar = p->u_bar;
        patha_lusgs_solve(p, &chunk_cfg, stats, dq_star, dq,
                          1, 1, 1, use_context, 0);
        p->lu_a = legacy_lu;
        p->c_mat = legacy_l;
        p->b_bar = legacy_u_bar;
        completed += chunk;
    }
    stats->trsv5_raw_runtime_cycles = stats->total_cycles;
    stats->trsv5_raw_total_cycles =
        stats->common_preprocess_cycles + stats->trsv5_raw_runtime_cycles;
    stats->coefficient_update_interval = interval;
    return all_valid;
}

static int
run_pretransform_raw(Problem *p, const Config *cfg, Stats *stats,
                     double *dq_star, double *dq, int use_context,
                     int use_coeff3, PretransformCheck *check)
{
    ForwardLineContext *forward_ctx = use_context ?
        (ForwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*forward_ctx), "raw_pretransform_fwd_ctx") : NULL;
    BackwardLineContext *backward_ctx = use_context ?
        (BackwardLineContext *)xcalloc_raw(
            p->lines, sizeof(*backward_ctx), "raw_pretransform_bwd_ctx") : NULL;
    uint64_t interval = cfg->coefficient_update_interval ?
        cfg->coefficient_update_interval : cfg->sweeps;
    uint64_t completed = 0;
    int all_valid = 1;
    while (completed < cfg->sweeps) {
        uint64_t chunk = cfg->sweeps - completed;
        if (chunk > interval)
            chunk = interval;
        int event_coeff3 = use_coeff3 &&
            !strcmp(cfg->coeff_preprocess_model, "event");
        int common_valid = event_coeff3 ?
            prepare_raw_event(p, cfg, stats, check, 1) :
            prepare_raw_common(p, cfg, stats, use_coeff3 ? 0 : 1);
        int extra_valid = event_coeff3 ? common_valid : common_valid &&
            (use_coeff3 ? prepare_raw_coeff3(p, cfg, stats, check) :
             prepare_raw_extra_dual(p, cfg, stats, check));
        all_valid &= common_valid && extra_valid;
        if (!common_valid) {
            size_t bytes = p->lines * p->cells * N * sizeof(double);
            stats->common_preprocess_fallback_runs++;
            memcpy(dq_star, p->dq_star_raw_ref, bytes);
            memcpy(dq, p->dq_raw_ref, bytes);
            break;
        }
        if (!extra_valid) {
            double *legacy_lu = p->lu_a;
            double *legacy_l = p->c_mat;
            double *legacy_u_bar = p->b_bar;
            Config fallback_cfg = *cfg;
            fallback_cfg.sweeps = cfg->sweeps - completed;
            stats->pretransform_fallback_to_trsv5_raw++;
            p->lu_a = p->lu_d;
            p->c_mat = p->l_mat;
            p->b_bar = p->u_bar;
            uint64_t fallback_start = read_cycle_counter();
            patha_lusgs_solve(p, &fallback_cfg, stats, dq_star, dq,
                              1, 1, 1, use_context, 0);
            uint64_t fallback_end = read_cycle_counter();
            if (fallback_end >= fallback_start)
                stats->pretransform_raw_runtime_cycles +=
                    fallback_end - fallback_start;
            p->lu_a = legacy_lu;
            p->c_mat = legacy_l;
            p->b_bar = legacy_u_bar;
            completed = cfg->sweeps;
            break;
        }
        uint64_t hot_start = read_cycle_counter();
        Config chunk_cfg = *cfg;
        chunk_cfg.sweeps = chunk;
        for (uint64_t sweep = 0; sweep < chunk; sweep++) {
            if (use_context) {
                reset_forward_contexts(forward_ctx, p->lines);
                reset_backward_contexts(backward_ctx, p->lines);
            }
            pretransform_forward_sweep(p, &chunk_cfg, stats, dq_star,
                                       use_context, forward_ctx);
            pretransform_backward_sweep(p, &chunk_cfg, stats, dq_star, dq,
                                        use_context, backward_ctx, p->u_bar);
        }
        uint64_t hot_end = read_cycle_counter();
        if (hot_end >= hot_start)
            stats->pretransform_raw_runtime_cycles += hot_end - hot_start;
        completed += chunk;
    }
    stats->total_cycles = stats->pretransform_raw_runtime_cycles;
    stats->pretransform_raw_total_cycles = stats->common_preprocess_cycles +
        stats->extra_pretransform_cycles +
        stats->pretransform_raw_runtime_cycles;
    stats->coefficient_update_interval = interval;
    free(forward_ctx);
    free(backward_ctx);
    return all_valid;
}

static CompareResult
compare_solution(const Problem *p, const double *ref_base,
                 const double *got_base, double abs_tol, double rel_tol)
{
    CompareResult cmp;
    memset(&cmp, 0, sizeof(cmp));
    cmp.first_line = -1;
    cmp.first_cell = -1;
    cmp.first_lane = -1;

    for (size_t line = 0; line < p->lines; line++) {
        for (size_t cell = 0; cell < p->cells; cell++) {
            const double *ref = vec_const_at(ref_base, p, line, cell);
            const double *got = vec_const_at(got_base, p, line, cell);
            for (int lane = 0; lane < N; lane++) {
                double abs_err = fabs(got[lane] - ref[lane]);
                double denom = fmax(fabs(ref[lane]), 1.0e-14);
                double rel_err = abs_err / denom;
                if (abs_err > cmp.max_abs_error)
                    cmp.max_abs_error = abs_err;
                if (rel_err > cmp.max_rel_error)
                    cmp.max_rel_error = rel_err;
                if (abs_err > abs_tol && rel_err > rel_tol) {
                    if (cmp.mismatch_count == 0) {
                        cmp.first_line = (long)line;
                        cmp.first_cell = (long)cell;
                        cmp.first_lane = (long)lane;
                        cmp.first_ref = ref[lane];
                        cmp.first_got = got[lane];
                    }
                    cmp.mismatch_count++;
                }
            }
        }
    }
    return cmp;
}

static int
parse_uint64(const char *text, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0')
        return 0;
    *out = (uint64_t)value;
    return 1;
}

static int
parse_double_value(const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || !end || *end != '\0' || !isfinite(value))
        return 0;
    *out = value;
    return 1;
}

static int
parse_bool_arg(const char *text)
{
    if (!strcmp(text, "1") || !strcmp(text, "true") || !strcmp(text, "yes") ||
        !strcmp(text, "on"))
        return 1;
    if (!strcmp(text, "0") || !strcmp(text, "false") || !strcmp(text, "no") ||
        !strcmp(text, "off"))
        return 0;
    fprintf(stderr, "invalid boolean value: %s\n", text);
    exit(2);
}

static const char *
option_value(int argc, char **argv, int *idx, const char *arg,
             const char *name)
{
    size_t len = strlen(name);
    if (!strncmp(arg, name, len) && arg[len] == '=')
        return arg + len + 1;
    if (!strcmp(arg, name)) {
        if (*idx + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", name);
            exit(2);
        }
        (*idx)++;
        return argv[*idx];
    }
    return NULL;
}

static void
parse_args(int argc, char **argv, Config *cfg)
{
    cfg->sweeps = 1;
    cfg->lines = 1;
    cfg->cells = 17;
    cfg->interleave = 1;
    cfg->run_reference = 1;
    cfg->run_step1 = 1;
    cfg->run_step2 = 1;
    cfg->run_step2_core = 1;
    cfg->run_step2_core_forwarded = 1;
    cfg->run_step2_core_forwarded_context = 1;
    cfg->run_step2_core_forwarded_linebuf = 1;
    cfg->run_step2_pretransform = 1;
    cfg->run_step2_pretransform_context = 1;
    cfg->run_step2_pretransform_optprep = 1;
    cfg->run_step2_pretransform_optprep_context = 1;
    cfg->run_step2_trsv5_raw = 1;
    cfg->run_step2_trsv5_raw_context = 1;
    cfg->run_step2_pretransform_raw = 1;
    cfg->run_step2_pretransform_raw_context = 1;
    cfg->run_step2_pretransform_raw_optprep = 1;
    cfg->run_step2_pretransform_raw_optprep_context = 1;
    cfg->run_step2_pretransform_raw_stream = 0;
    cfg->linebuf_enable = 1;
    cfg->linebuf_entries = 16;
    cfg->trsm5_mrhs_enable = 1;
    cfg->trsm5_mrhs_lat = 100;
    cfg->trsm5_mrhs_count = 1;
    cfg->vec5_lat = 2;
    cfg->vec5_count = 1;
    cfg->pretransform_fallback_trsv5 = 1;
    cfg->pretransform_diag_epsilon = 1.0e-12;
    cfg->pretransform_per_sweep = 0;
    cfg->trsm5_inv_lbar_enable = 1;
    cfg->trsm5_inv_lbar_lat = 200;
    cfg->trsm5_inv_lbar_count = 1;
    cfg->pretransform_output_buffer_enable = 1;
    cfg->pretransform_output_buffer_depth = 4;
    cfg->pretransform_spm_slots = 2;
    cfg->pretransform_overlap_enable = 1;
    cfg->pretransform_use_barrier = 0;
    cfg->pretransform_validate = "full";
    cfg->lusgs_pretransform_auto = 0;
    cfg->lusgs_expected_sweeps = 1;
    cfg->estimated_break_even_sweeps = 0;
    cfg->trsm5_coeff3_enable = 1;
    cfg->trsm5_coeff3_lat = 300;
    cfg->trsm5_coeff3_count = 1;
    cfg->coeff_preprocess_spm_slots = 2;
    cfg->coeff_output_buffer_depth = 4;
    cfg->coefficient_update_interval = 0;
    cfg->coeff_preprocess_model = "coarse";
    cfg->lu5_model = "software";
    cfg->coeff_validation = "full";
    cfg->coeff_validation_sample_rate = 16;
    cfg->coeff3_partial_output = 1;
    cfg->lu_forwarding = 1;
    cfg->ubar_inplace = 0;
    cfg->coeff_cancel_test = 0;
    cfg->coeff_streaming_enable = 0;
    cfg->coeff_stream_line_autonomous = 0;
    cfg->coeff_stream_window = 8;
    cfg->coeff_stream_retire_mode = "auto";
    cfg->coeff_stream_boundary_mask = 1;
    cfg->coeff_stream_dinv_consumer = "off";
    cfg->coeff_stream_lbar_consumer = "off";
    cfg->coeff_stream_ubar_consumer = "off";
    cfg->coeff_column_fma_latency = 4;
    cfg->coeff_column_fma_ii = 1;
    cfg->coeff_column_fma_count = 1;
    cfg->coeff_column_fma_queue_depth = 5;
    cfg->forward_combine_latency = 1;
    cfg->forward_combine_ii = 1;
    cfg->forward_combine_count = 1;
    cfg->forward_combine_queue_depth = 2;
    cfg->check = 1;
    cfg->trace_enable = 0;
    cfg->trace_lines = 1;
    cfg->trace_cells = 4;
    cfg->trsv5_count = 1;
    cfg->trsv5_lat = 60;
    cfg->trsv5_mode = "zreg-coarse";
    cfg->trsv5_trace_enable = 0;
    cfg->trsv5_trace_solves = 16;
    cfg->trsv5_trace_file = "m5out/trsv5_trace.csv";
    cfg->patha_store_mode = "pred40";
    cfg->patha_store_stride = 40;
    cfg->patha_unroll = 8;
    cfg->patha_acc_mode = "internal-buffer";
    cfg->patha_buffer_depth = 2;
    cfg->patha_streaming = 0;
    cfg->patha_kernel = "baseline";

    int idx = 1;
    if (idx < argc && strncmp(argv[idx], "--", 2) != 0) {
        uint64_t sweeps = 0;
        if (!parse_uint64(argv[idx], &sweeps) || sweeps == 0) {
            fprintf(stderr, "invalid sweep/bench iteration count: %s\n",
                    argv[idx]);
            exit(2);
        }
        cfg->sweeps = sweeps;
        idx++;
    }

    if (idx + 6 < argc && strncmp(argv[idx], "--", 2) != 0) {
        cfg->patha_store_mode = argv[idx++];
        if (!parse_uint64(argv[idx++], &cfg->patha_store_stride) ||
            cfg->patha_store_stride == 0) {
            fprintf(stderr, "invalid Path A store stride\n");
            exit(2);
        }
        if (!parse_uint64(argv[idx++], &cfg->patha_unroll) ||
            cfg->patha_unroll == 0) {
            fprintf(stderr, "invalid Path A unroll\n");
            exit(2);
        }
        cfg->patha_acc_mode = argv[idx++];
        if (!parse_uint64(argv[idx++], &cfg->patha_buffer_depth) ||
            cfg->patha_buffer_depth == 0) {
            fprintf(stderr, "invalid Path A buffer depth\n");
            exit(2);
        }
        cfg->patha_streaming = parse_bool_arg(argv[idx++]);
        cfg->patha_kernel = argv[idx++];
    }

    for (; idx < argc; idx++) {
        const char *arg = argv[idx];
        const char *value;
        uint64_t parsed = 0;
        if ((value = option_value(argc, argv, &idx, arg,
                                  "--lusgs-lines")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-lines: %s\n", value);
                exit(2);
            }
            cfg->lines = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-cells")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-cells: %s\n", value);
                exit(2);
            }
            cfg->cells = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-interleave")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-interleave: %s\n", value);
                exit(2);
            }
            cfg->interleave = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-check")) != NULL) {
            cfg->check = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-mode")) != NULL) {
            cfg->run_reference = 0;
            cfg->run_step1 = 0;
            cfg->run_step2 = 0;
            cfg->run_step2_core = 0;
            cfg->run_step2_core_forwarded = 0;
            cfg->run_step2_core_forwarded_context = 0;
            cfg->run_step2_core_forwarded_linebuf = 0;
            cfg->run_step2_pretransform = 0;
            cfg->run_step2_pretransform_context = 0;
            cfg->run_step2_pretransform_optprep = 0;
            cfg->run_step2_pretransform_optprep_context = 0;
            cfg->run_step2_trsv5_raw = 0;
            cfg->run_step2_trsv5_raw_context = 0;
            cfg->run_step2_pretransform_raw = 0;
            cfg->run_step2_pretransform_raw_context = 0;
            cfg->run_step2_pretransform_raw_optprep = 0;
            cfg->run_step2_pretransform_raw_optprep_context = 0;
            cfg->run_step2_pretransform_raw_stream = 0;
            if (!strcmp(value, "all")) {
                cfg->run_reference = 1;
                cfg->run_step1 = 1;
                cfg->run_step2 = 1;
                cfg->run_step2_core = 1;
                cfg->run_step2_core_forwarded = 1;
                cfg->run_step2_core_forwarded_context = 1;
                cfg->run_step2_core_forwarded_linebuf = 1;
                cfg->run_step2_pretransform = 1;
                cfg->run_step2_pretransform_context = 1;
                cfg->run_step2_pretransform_optprep = 1;
                cfg->run_step2_pretransform_optprep_context = 1;
                cfg->run_step2_trsv5_raw = 1;
                cfg->run_step2_trsv5_raw_context = 1;
                cfg->run_step2_pretransform_raw = 1;
                cfg->run_step2_pretransform_raw_context = 1;
                cfg->run_step2_pretransform_raw_optprep = 1;
                cfg->run_step2_pretransform_raw_optprep_context = 1;
            } else if (!strcmp(value, "raw-compare")) {
                cfg->run_reference = 1;
                cfg->run_step2_trsv5_raw = 1;
                cfg->run_step2_pretransform_raw = 1;
                cfg->run_step2_pretransform_raw_optprep = 1;
            } else if (!strcmp(value, "raw-compare-context")) {
                cfg->run_reference = 1;
                cfg->run_step2_trsv5_raw_context = 1;
                cfg->run_step2_pretransform_raw_context = 1;
                cfg->run_step2_pretransform_raw_optprep_context = 1;
            } else if (!strcmp(value, "reference")) {
                cfg->run_reference = 1;
            } else if (!strcmp(value, "step1")) {
                cfg->run_step1 = 1;
            } else if (!strcmp(value, "step2")) {
                cfg->run_step2 = 1;
            } else if (!strcmp(value, "step2-core") ||
                       !strcmp(value, "step2_core") ||
                       !strcmp(value, "step2-fused")) {
                cfg->run_step2_core = 1;
            } else if (!strcmp(value, "step2-core-forwarded") ||
                       !strcmp(value, "step2_core_forwarded") ||
                       !strcmp(value, "step2-forwarded")) {
                cfg->run_step2_core_forwarded = 1;
            } else if (!strcmp(value, "step2-core-forwarded-context") ||
                       !strcmp(value, "step2_core_forwarded_context") ||
                       !strcmp(value, "step2-context")) {
                cfg->run_step2_core_forwarded_context = 1;
            } else if (!strcmp(value, "step2-core-forwarded-linebuf") ||
                       !strcmp(value, "step2_core_forwarded_linebuf") ||
                       !strcmp(value, "step2-linebuf")) {
                cfg->run_step2_core_forwarded_linebuf = 1;
            } else if (!strcmp(value, "step2-pretransform") ||
                       !strcmp(value, "step2_pretransform")) {
                cfg->run_step2_pretransform = 1;
            } else if (!strcmp(value, "step2-pretransform-context") ||
                       !strcmp(value, "step2_pretransform_context")) {
                cfg->run_step2_pretransform_context = 1;
            } else if (!strcmp(value, "step2-pretransform-optprep") ||
                       !strcmp(value, "step2_pretransform_optprep")) {
                cfg->run_step2_pretransform_optprep = 1;
            } else if (!strcmp(value,
                               "step2-pretransform-optprep-context") ||
                       !strcmp(value,
                               "step2_pretransform_optprep_context")) {
                cfg->run_step2_pretransform_optprep_context = 1;
            } else if (!strcmp(value, "step2-trsv5-raw")) {
                cfg->run_step2_trsv5_raw = 1;
            } else if (!strcmp(value, "step2-trsv5-raw-context")) {
                cfg->run_step2_trsv5_raw_context = 1;
            } else if (!strcmp(value, "step2-pretransform-raw")) {
                cfg->run_step2_pretransform_raw = 1;
            } else if (!strcmp(value, "step2-pretransform-raw-context")) {
                cfg->run_step2_pretransform_raw_context = 1;
            } else if (!strcmp(value, "step2-pretransform-raw-optprep")) {
                cfg->run_step2_pretransform_raw_optprep = 1;
            } else if (!strcmp(value,
                               "step2-pretransform-raw-optprep-context")) {
                cfg->run_step2_pretransform_raw_optprep_context = 1;
            } else if (!strcmp(value,
                               "step2-pretransform-raw-optprep-stream")) {
                cfg->run_step2_pretransform_raw_stream = 1;
            } else {
                fprintf(stderr, "invalid --lusgs-mode: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-trace-lines")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-trace-lines: %s\n", value);
                exit(2);
            }
            cfg->trace_lines = (size_t)parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-trace-cells")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-trace-cells: %s\n", value);
                exit(2);
            }
            cfg->trace_cells = (size_t)parsed;
        } else if (!strcmp(arg, "--lusgs-reference-only")) {
            cfg->run_reference = 1;
            cfg->run_step1 = 0;
            cfg->run_step2 = 0;
            cfg->run_step2_core = 0;
            cfg->run_step2_core_forwarded = 0;
            cfg->run_step2_core_forwarded_context = 0;
            cfg->run_step2_core_forwarded_linebuf = 0;
            cfg->run_step2_pretransform = 0;
            cfg->run_step2_pretransform_context = 0;
            cfg->run_step2_pretransform_optprep = 0;
            cfg->run_step2_pretransform_optprep_context = 0;
            cfg->run_step2_trsv5_raw = 0;
            cfg->run_step2_trsv5_raw_context = 0;
            cfg->run_step2_pretransform_raw = 0;
            cfg->run_step2_pretransform_raw_context = 0;
            cfg->run_step2_pretransform_raw_optprep = 0;
            cfg->run_step2_pretransform_raw_optprep_context = 0;
            cfg->run_step2_pretransform_raw_stream = 0;
        } else if (!strcmp(arg, "--lusgs-patha-only")) {
            cfg->run_reference = 0;
            cfg->run_step1 = 1;
            cfg->run_step2 = 0;
            cfg->run_step2_core = 0;
            cfg->run_step2_core_forwarded = 0;
            cfg->run_step2_core_forwarded_context = 0;
            cfg->run_step2_core_forwarded_linebuf = 0;
            cfg->run_step2_pretransform = 0;
            cfg->run_step2_pretransform_context = 0;
            cfg->run_step2_pretransform_optprep = 0;
            cfg->run_step2_pretransform_optprep_context = 0;
            cfg->run_step2_trsv5_raw = 0;
            cfg->run_step2_trsv5_raw_context = 0;
            cfg->run_step2_pretransform_raw = 0;
            cfg->run_step2_pretransform_raw_context = 0;
            cfg->run_step2_pretransform_raw_optprep = 0;
            cfg->run_step2_pretransform_raw_optprep_context = 0;
            cfg->run_step2_pretransform_raw_stream = 0;
            cfg->check = 0;
        } else if (!strcmp(arg, "--lusgs-step1")) {
            cfg->run_step1 = 1;
        } else if (!strcmp(arg, "--lusgs-step2")) {
            cfg->run_step2 = 1;
        } else if (!strcmp(arg, "--lusgs-step2-core")) {
            cfg->run_step2_core = 1;
        } else if (!strcmp(arg, "--lusgs-step2-core-forwarded")) {
            cfg->run_step2_core_forwarded = 1;
        } else if (!strcmp(arg, "--lusgs-step2-core-forwarded-context")) {
            cfg->run_step2_core_forwarded_context = 1;
        } else if (!strcmp(arg, "--lusgs-step2-core-forwarded-linebuf")) {
            cfg->run_step2_core_forwarded_linebuf = 1;
        } else if (!strcmp(arg, "--lusgs-step2-pretransform")) {
            cfg->run_step2_pretransform = 1;
        } else if (!strcmp(arg, "--lusgs-step2-pretransform-context")) {
            cfg->run_step2_pretransform_context = 1;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-linebuf-enable")) != NULL) {
            cfg->linebuf_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--lusgs-linebuf-entries")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --lusgs-linebuf-entries: %s\n",
                        value);
                exit(2);
            }
            cfg->linebuf_entries = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsm5-mrhs-enable")) != NULL) {
            cfg->trsm5_mrhs_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsm5-mrhs-lat")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsm5-mrhs-lat: %s\n", value);
                exit(2);
            }
            cfg->trsm5_mrhs_lat = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsm5-mrhs-count")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsm5-mrhs-count: %s\n", value);
                exit(2);
            }
            cfg->trsm5_mrhs_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--vec5-lat")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --vec5-lat: %s\n", value);
                exit(2);
            }
            cfg->vec5_lat = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--vec5-count")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --vec5-count: %s\n", value);
                exit(2);
            }
            cfg->vec5_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-fallback-trsv5")) != NULL) {
            cfg->pretransform_fallback_trsv5 = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-diag-epsilon")) != NULL) {
            if (!parse_double_value(value, &cfg->pretransform_diag_epsilon) ||
                cfg->pretransform_diag_epsilon <= 0.0) {
                fprintf(stderr, "invalid --pretransform-diag-epsilon: %s\n",
                        value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-per-sweep")) != NULL) {
            cfg->pretransform_per_sweep = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-mrhs-dual-enable")) != NULL) {
            cfg->trsm5_inv_lbar_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-mrhs-dual-lat")) != NULL) {
            if (!parse_uint64(value, &cfg->trsm5_inv_lbar_lat) ||
                cfg->trsm5_inv_lbar_lat == 0) {
                fprintf(stderr, "invalid --trsm5-mrhs-dual-lat: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-mrhs-dual-count")) != NULL) {
            if (!parse_uint64(value, &cfg->trsm5_inv_lbar_count) ||
                cfg->trsm5_inv_lbar_count == 0) {
                fprintf(stderr, "invalid --trsm5-mrhs-dual-count: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-output-buffer-enable")) != NULL) {
            cfg->pretransform_output_buffer_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-output-buffer-depth")) != NULL) {
            if (!parse_uint64(value, &cfg->pretransform_output_buffer_depth) ||
                cfg->pretransform_output_buffer_depth == 0 ||
                cfg->pretransform_output_buffer_depth > 8) {
                fprintf(stderr,
                        "invalid --pretransform-output-buffer-depth: %s\n",
                        value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-spm-slots")) != NULL) {
            if (!parse_uint64(value, &cfg->pretransform_spm_slots) ||
                (cfg->pretransform_spm_slots != 1 &&
                 cfg->pretransform_spm_slots != 2)) {
                fprintf(stderr, "invalid --pretransform-spm-slots: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-overlap-enable")) != NULL) {
            cfg->pretransform_overlap_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-use-barrier")) != NULL) {
            cfg->pretransform_use_barrier = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--pretransform-validate")) != NULL) {
            if (strcmp(value, "full") && strcmp(value, "fast")) {
                fprintf(stderr, "invalid --pretransform-validate: %s\n", value);
                exit(2);
            }
            cfg->pretransform_validate = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--lusgs-pretransform-auto")) != NULL ||
                   (value = option_value(argc, argv, &idx, arg,
                         "--lusgs-preprocess-auto")) != NULL) {
            cfg->lusgs_pretransform_auto = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--lusgs-expected-sweeps")) != NULL ||
                   (value = option_value(argc, argv, &idx, arg,
                         "--lusgs-coeff-expected-sweeps")) != NULL) {
            if (!parse_uint64(value, &cfg->lusgs_expected_sweeps) ||
                cfg->lusgs_expected_sweeps == 0) {
                fprintf(stderr, "invalid expected coefficient sweeps: %s\n",
                        value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-coeff3-enable")) != NULL) {
            cfg->trsm5_coeff3_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-coeff3-lat")) != NULL) {
            if (!parse_uint64(value, &cfg->trsm5_coeff3_lat) ||
                cfg->trsm5_coeff3_lat == 0) {
                fprintf(stderr, "invalid --trsm5-coeff3-lat: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--trsm5-coeff3-count")) != NULL) {
            if (!parse_uint64(value, &cfg->trsm5_coeff3_count) ||
                cfg->trsm5_coeff3_count == 0) {
                fprintf(stderr, "invalid --trsm5-coeff3-count: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-preprocess-spm-slots")) != NULL) {
            if (!parse_uint64(value, &cfg->coeff_preprocess_spm_slots) ||
                (cfg->coeff_preprocess_spm_slots != 1 &&
                 cfg->coeff_preprocess_spm_slots != 2)) {
                fprintf(stderr,
                        "invalid --coeff-preprocess-spm-slots: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-output-buffer-depth")) != NULL) {
            if (!parse_uint64(value, &cfg->coeff_output_buffer_depth) ||
                cfg->coeff_output_buffer_depth == 0 ||
                cfg->coeff_output_buffer_depth > 8) {
                fprintf(stderr,
                        "invalid --coeff-output-buffer-depth: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coefficient-update-interval")) != NULL) {
            if (!parse_uint64(value, &cfg->coefficient_update_interval)) {
                fprintf(stderr,
                        "invalid --coefficient-update-interval: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-preprocess-model")) != NULL) {
            if (strcmp(value, "coarse") && strcmp(value, "event")) {
                fprintf(stderr, "invalid --coeff-preprocess-model: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_preprocess_model = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--lu5-model")) != NULL) {
            if (strcmp(value, "software") && strcmp(value, "event")) {
                fprintf(stderr, "invalid --lu5-model: %s\n", value);
                exit(2);
            }
            cfg->lu5_model = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-validation")) != NULL) {
            if (strcmp(value, "performance") && strcmp(value, "sampled") &&
                strcmp(value, "full")) {
                fprintf(stderr, "invalid --coeff-validation: %s\n", value);
                exit(2);
            }
            cfg->coeff_validation = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-validation-sample-rate")) != NULL) {
            if (!parse_uint64(value, &cfg->coeff_validation_sample_rate) ||
                cfg->coeff_validation_sample_rate == 0) {
                fprintf(stderr,
                        "invalid --coeff-validation-sample-rate: %s\n",
                        value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff3-partial-output")) != NULL) {
            cfg->coeff3_partial_output = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--lu-forwarding")) != NULL) {
            cfg->lu_forwarding = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--ubar-inplace")) != NULL) {
            cfg->ubar_inplace = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-cancel-test")) != NULL) {
            cfg->coeff_cancel_test = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-streaming-enable")) != NULL) {
            cfg->coeff_streaming_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-line-autonomous")) != NULL) {
            cfg->coeff_stream_line_autonomous = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-window")) != NULL) {
            if (!parse_uint64(value, &cfg->coeff_stream_window) ||
                (cfg->coeff_stream_window != 1 &&
                 cfg->coeff_stream_window != 2 &&
                 cfg->coeff_stream_window != 4 &&
                 cfg->coeff_stream_window != 8)) {
                fprintf(stderr, "invalid --coeff-stream-window: %s\n", value);
                exit(2);
            }
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-retire-mode")) != NULL) {
            if (strcmp(value, "sync") && strcmp(value, "queue") &&
                strcmp(value, "auto")) {
                fprintf(stderr,
                        "invalid --coeff-stream-retire-mode: %s\n", value);
                exit(2);
            }
            cfg->coeff_stream_retire_mode = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-boundary-mask")) != NULL) {
            cfg->coeff_stream_boundary_mask = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-dinv-consumer")) != NULL) {
            if (strcmp(value, "off") && strcmp(value, "shadow") &&
                strcmp(value, "direct")) {
                fprintf(stderr,
                        "invalid --coeff-stream-dinv-consumer: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_stream_dinv_consumer = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-lbar-consumer")) != NULL) {
            if (strcmp(value, "off") && strcmp(value, "shadow") &&
                strcmp(value, "direct")) {
                fprintf(stderr,
                        "invalid --coeff-stream-lbar-consumer: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_stream_lbar_consumer = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-ubar-consumer")) != NULL) {
            if (strcmp(value, "off") && strcmp(value, "direct")) {
                fprintf(stderr,
                        "invalid --coeff-stream-ubar-consumer: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_stream_ubar_consumer = value;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-column-fma-latency")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 1024) {
                fprintf(stderr, "invalid --coeff-column-fma-latency: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_column_fma_latency = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-column-fma-ii")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 64) {
                fprintf(stderr, "invalid --coeff-column-fma-ii: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_column_fma_ii = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-column-fma-count")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 4) {
                fprintf(stderr, "invalid --coeff-column-fma-count: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_column_fma_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-column-fma-queue-depth")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 64) {
                fprintf(stderr,
                        "invalid --coeff-column-fma-queue-depth: %s\n",
                        value);
                exit(2);
            }
            cfg->coeff_column_fma_queue_depth = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--forward-combine-latency")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 64) {
                fprintf(stderr, "invalid --forward-combine-latency: %s\n",
                        value);
                exit(2);
            }
            cfg->forward_combine_latency = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--forward-combine-ii")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 64) {
                fprintf(stderr, "invalid --forward-combine-ii: %s\n",
                        value);
                exit(2);
            }
            cfg->forward_combine_ii = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--forward-combine-count")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 4) {
                fprintf(stderr, "invalid --forward-combine-count: %s\n",
                        value);
                exit(2);
            }
            cfg->forward_combine_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--forward-combine-queue-depth")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0 || parsed > 64) {
                fprintf(stderr,
                        "invalid --forward-combine-queue-depth: %s\n",
                        value);
                exit(2);
            }
            cfg->forward_combine_queue_depth = parsed;
        } else if (!strcmp(arg, "--lusgs-trace-enable")) {
            cfg->trace_enable = 1;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-count")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsv5-count: %s\n", value);
                exit(2);
            }
            cfg->trsv5_count = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-lat")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsv5-lat: %s\n", value);
                exit(2);
            }
            cfg->trsv5_lat = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-mode")) != NULL) {
            cfg->trsv5_mode = value;
        } else if (!strcmp(arg, "--trsv5-trace-enable")) {
            cfg->trsv5_trace_enable = 1;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-trace-solves")) != NULL) {
            if (!parse_uint64(value, &parsed) || parsed == 0) {
                fprintf(stderr, "invalid --trsv5-trace-solves: %s\n", value);
                exit(2);
            }
            cfg->trsv5_trace_solves = parsed;
        } else if ((value = option_value(argc, argv, &idx, arg,
                                         "--trsv5-trace-file")) != NULL) {
            cfg->trsv5_trace_file = value;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            exit(2);
        }
    }

    if (!cfg->run_reference && !cfg->run_step1 && !cfg->run_step2 &&
        !cfg->run_step2_core && !cfg->run_step2_core_forwarded &&
        !cfg->run_step2_core_forwarded_context &&
        !cfg->run_step2_core_forwarded_linebuf &&
        !cfg->run_step2_pretransform &&
        !cfg->run_step2_pretransform_context &&
        !cfg->run_step2_pretransform_optprep &&
        !cfg->run_step2_pretransform_optprep_context &&
        !cfg->run_step2_trsv5_raw &&
        !cfg->run_step2_trsv5_raw_context &&
        !cfg->run_step2_pretransform_raw &&
        !cfg->run_step2_pretransform_raw_context &&
        !cfg->run_step2_pretransform_raw_optprep &&
        !cfg->run_step2_pretransform_raw_optprep_context &&
        !cfg->run_step2_pretransform_raw_stream) {
        fprintf(stderr, "at least one LU-SGS mode must run\n");
        exit(2);
    }
    if (strcmp(cfg->trsv5_mode, "zreg-coarse")) {
        fprintf(stderr, "unsupported --trsv5-mode for Step2: %s\n",
                cfg->trsv5_mode);
        exit(2);
    }
    if (cfg->run_step2_core_forwarded_linebuf && !cfg->linebuf_enable) {
        fprintf(stderr,
                "step2-core-forwarded-linebuf requires --lusgs-linebuf-enable=1\n");
        exit(2);
    }
    if ((cfg->run_step2_pretransform ||
         cfg->run_step2_pretransform_context) && !cfg->trsm5_mrhs_enable) {
        fprintf(stderr,
                "pretransform modes require --trsm5-mrhs-enable=1\n");
        exit(2);
    }
    if ((cfg->run_step2_pretransform_optprep ||
         cfg->run_step2_pretransform_optprep_context) &&
        !cfg->trsm5_inv_lbar_enable) {
        fprintf(stderr,
                "optprep modes require --trsm5-mrhs-dual-enable=1\n");
        exit(2);
    }
    if ((cfg->run_step2_trsv5_raw || cfg->run_step2_trsv5_raw_context ||
         cfg->run_step2_pretransform_raw ||
         cfg->run_step2_pretransform_raw_context) &&
        !cfg->trsm5_mrhs_enable) {
        fprintf(stderr, "raw modes require --trsm5-mrhs-enable=1\n");
        exit(2);
    }
    if ((cfg->run_step2_pretransform_raw_optprep ||
         cfg->run_step2_pretransform_raw_optprep_context ||
         cfg->run_step2_pretransform_raw_stream) &&
        !cfg->trsm5_coeff3_enable) {
        fprintf(stderr, "raw optprep modes require --trsm5-coeff3-enable=1\n");
        exit(2);
    }
    if (cfg->ubar_inplace) {
        fprintf(stderr,
                "--ubar-inplace=1 is reserved; raw correctness requires 0\n");
        exit(2);
    }
    if (cfg->run_step2_pretransform_raw_stream !=
        cfg->coeff_streaming_enable) {
        fprintf(stderr,
                "stream mode and --coeff-streaming-enable=1 must be selected together\n");
        exit(2);
    }
    if (cfg->run_step2_pretransform_raw_stream &&
        (strcmp(cfg->coeff_preprocess_model, "event") ||
         strcmp(cfg->lu5_model, "event"))) {
        fprintf(stderr,
                "stream mode requires --coeff-preprocess-model=event and --lu5-model=event\n");
        exit(2);
    }
    if (cfg->run_step2_pretransform_raw_stream && cfg->lines != 1 &&
        !cfg->coeff_stream_line_autonomous) {
        fprintf(stderr,
                "multi-line stream mode requires line autonomy\n");
        exit(2);
    }
    if (cfg->run_step2_pretransform_raw_stream && cfg->sweeps > 1 &&
        (!cfg->coeff_stream_line_autonomous ||
         strcmp(cfg->coeff_stream_ubar_consumer, "direct"))) {
        fprintf(stderr,
                "multi-sweep stream reuse requires line autonomy and direct Ubar consumption\n");
        exit(2);
    }
    if (cfg->run_step2_pretransform_raw_stream && cfg->sweeps > 1 &&
        !cfg->coefficient_update_interval) {
        fprintf(stderr,
                "multi-sweep coefficient reuse requires an explicit non-zero --coefficient-update-interval stability promise\n");
        exit(2);
    }
    if (!cfg->run_step2_pretransform_raw_stream &&
        strcmp(cfg->coeff_stream_retire_mode, "auto")) {
        fprintf(stderr,
                "--coeff-stream-retire-mode only applies to streaming mode\n");
        exit(2);
    }
    if (!cfg->run_step2_pretransform_raw_stream &&
        !cfg->coeff_stream_boundary_mask) {
        fprintf(stderr,
                "--coeff-stream-boundary-mask only applies to streaming mode\n");
        exit(2);
    }
    if (!cfg->coeff_stream_boundary_mask &&
        strcmp(cfg->coeff_stream_retire_mode, "auto")) {
        fprintf(stderr,
                "--coeff-stream-boundary-mask=0 requires auto retire mode\n");
        exit(2);
    }
    if (strcmp(cfg->coeff_stream_dinv_consumer, "off") &&
        (!cfg->run_step2_pretransform_raw_stream ||
         strcmp(cfg->coeff_stream_retire_mode, "auto"))) {
        fprintf(stderr,
                "DInv consumer requires streaming auto-retire mode\n");
        exit(2);
    }
    if (strcmp(cfg->coeff_stream_lbar_consumer, "off") &&
        (strcmp(cfg->coeff_stream_dinv_consumer, "direct") ||
         !cfg->run_step2_pretransform_raw_stream ||
         strcmp(cfg->coeff_stream_retire_mode, "auto"))) {
        fprintf(stderr,
                "Lbar consumer requires streaming auto-retire and DInv direct\n");
        exit(2);
    }
    if (cfg->coeff_stream_line_autonomous &&
        (!cfg->run_step2_pretransform_raw_stream ||
         strcmp(cfg->coeff_stream_retire_mode, "auto") ||
         strcmp(cfg->coeff_stream_dinv_consumer, "direct") ||
         strcmp(cfg->coeff_stream_lbar_consumer, "direct") ||
         !cfg->coeff_stream_boundary_mask)) {
        fprintf(stderr,
                "line autonomy requires streaming auto-retire, boundary mask, and direct DInv/Lbar consumers\n");
        exit(2);
    }
    if (strcmp(cfg->coeff_stream_ubar_consumer, "off") &&
        (!cfg->coeff_stream_line_autonomous ||
         strcmp(cfg->coeff_stream_ubar_consumer, "direct"))) {
        fprintf(stderr,
                "Ubar direct consumer requires line autonomy\n");
        exit(2);
    }
    if (cfg->lusgs_pretransform_auto) {
        cfg->run_reference = cfg->check;
        cfg->run_step1 = 0;
        cfg->run_step2 = 0;
        cfg->run_step2_core = 0;
        cfg->run_step2_core_forwarded = 0;
        cfg->run_step2_core_forwarded_context = 0;
        cfg->run_step2_core_forwarded_linebuf = 0;
        cfg->run_step2_pretransform = 0;
        cfg->run_step2_pretransform_context = 0;
        cfg->run_step2_pretransform_optprep = 0;
        cfg->run_step2_pretransform_optprep_context = 0;
        cfg->run_step2_trsv5_raw = 0;
        cfg->run_step2_trsv5_raw_context = 0;
        cfg->run_step2_pretransform_raw = 0;
        cfg->run_step2_pretransform_raw_context = 0;
        cfg->run_step2_pretransform_raw_optprep = 0;
        cfg->run_step2_pretransform_raw_optprep_context = 0;
        cfg->run_step2_pretransform_raw_stream = 0;
        // Calibration uses two separate complete rounds. The selected round
        // is launched only after both current-configuration measurements.
        cfg->run_step2_trsv5_raw = 1;
        cfg->run_step2_pretransform_raw_optprep = 1;
    }
    if (cfg->interleave > cfg->lines)
        cfg->interleave = cfg->lines;
}

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

int
main(int argc, char **argv)
{
    Config cfg;
    Problem problem;
    Stats ref_stats;
    Stats step1_stats;
    Stats step2_stats;
    Stats step2_core_stats;
    Stats step2_core_forwarded_stats;
    Stats step2_core_forwarded_context_stats;
    Stats step2_core_forwarded_linebuf_stats;
    Stats step2_pretransform_stats;
    Stats step2_pretransform_context_stats;
    Stats step2_pretransform_optprep_stats;
    Stats step2_pretransform_optprep_context_stats;
    Stats raw_ref_stats = {0};
    Stats step2_trsv5_raw_stats = {0};
    Stats step2_trsv5_raw_context_stats = {0};
    Stats step2_pretransform_raw_stats = {0};
    Stats step2_pretransform_raw_context_stats = {0};
    Stats step2_pretransform_raw_optprep_stats = {0};
    Stats step2_pretransform_raw_optprep_context_stats = {0};
    Stats step2_pretransform_raw_stream_stats = {0};
    Stats auto_selected_stats = {0};
    PretransformCheck step2_pretransform_check;
    PretransformCheck step2_pretransform_context_check;
    PretransformCheck step2_pretransform_optprep_check;
    PretransformCheck step2_pretransform_optprep_context_check;
    PretransformCheck step2_pretransform_raw_check = {0};
    PretransformCheck step2_pretransform_raw_context_check = {0};
    PretransformCheck step2_pretransform_raw_optprep_check = {0};
    PretransformCheck step2_pretransform_raw_optprep_context_check = {0};
    PretransformCheck step2_pretransform_raw_stream_check = {0};
    PretransformCheck auto_selected_check = {0};
    CompareResult step1_cmp;
    CompareResult step2_cmp;
    CompareResult step2_vs_step1_cmp;
    CompareResult step2_core_cmp;
    CompareResult step2_core_vs_step2_cmp;
    CompareResult step2_core_forwarded_cmp;
    CompareResult step2_core_forwarded_vs_step2_core_cmp;
    CompareResult step2_core_forwarded_context_cmp;
    CompareResult step2_core_forwarded_context_vs_forwarded_cmp;
    CompareResult step2_core_forwarded_context_vs_core_cmp;
    CompareResult step2_core_forwarded_linebuf_cmp;
    CompareResult step2_core_forwarded_linebuf_vs_context_cmp;
    CompareResult step2_core_forwarded_linebuf_vs_forwarded_cmp;
    CompareResult step2_core_forwarded_linebuf_vs_core_cmp;
    CompareResult step2_pretransform_cmp;
    CompareResult step2_pretransform_vs_forwarded_cmp;
    CompareResult step2_pretransform_vs_context_cmp;
    CompareResult step2_pretransform_context_cmp;
    CompareResult step2_pretransform_context_vs_pretransform_cmp;
    CompareResult step2_pretransform_optprep_cmp;
    CompareResult step2_pretransform_optprep_vs_pretransform_cmp;
    CompareResult step2_pretransform_optprep_context_cmp;
    CompareResult step2_pretransform_optprep_context_vs_context_cmp;
    CompareResult step2_trsv5_raw_cmp = {0};
    CompareResult step2_trsv5_raw_context_cmp = {0};
    CompareResult step2_pretransform_raw_cmp = {0};
    CompareResult step2_pretransform_raw_context_cmp = {0};
    CompareResult step2_pretransform_raw_optprep_cmp = {0};
    CompareResult step2_pretransform_raw_optprep_context_cmp = {0};
    CompareResult step2_pretransform_raw_stream_cmp = {0};
    CompareResult auto_selected_cmp = {0};
    int auto_selection_ran = 0;
    int auto_selected_pre = 0;
    int auto_selected_valid = 1;
    memset(&ref_stats, 0, sizeof(ref_stats));
    memset(&step1_stats, 0, sizeof(step1_stats));
    memset(&step2_stats, 0, sizeof(step2_stats));
    memset(&step2_core_stats, 0, sizeof(step2_core_stats));
    memset(&step2_core_forwarded_stats, 0,
           sizeof(step2_core_forwarded_stats));
    memset(&step2_core_forwarded_context_stats, 0,
           sizeof(step2_core_forwarded_context_stats));
    memset(&step2_core_forwarded_linebuf_stats, 0,
           sizeof(step2_core_forwarded_linebuf_stats));
    memset(&step2_pretransform_stats, 0,
           sizeof(step2_pretransform_stats));
    memset(&step2_pretransform_context_stats, 0,
           sizeof(step2_pretransform_context_stats));
    memset(&step2_pretransform_optprep_stats, 0,
           sizeof(step2_pretransform_optprep_stats));
    memset(&step2_pretransform_optprep_context_stats, 0,
           sizeof(step2_pretransform_optprep_context_stats));
    memset(&step2_pretransform_check, 0, sizeof(step2_pretransform_check));
    memset(&step2_pretransform_context_check, 0,
           sizeof(step2_pretransform_context_check));
    memset(&step2_pretransform_optprep_check, 0,
           sizeof(step2_pretransform_optprep_check));
    memset(&step2_pretransform_optprep_context_check, 0,
           sizeof(step2_pretransform_optprep_context_check));
    memset(&step1_cmp, 0, sizeof(step1_cmp));
    memset(&step2_cmp, 0, sizeof(step2_cmp));
    memset(&step2_vs_step1_cmp, 0, sizeof(step2_vs_step1_cmp));
    memset(&step2_core_cmp, 0, sizeof(step2_core_cmp));
    memset(&step2_core_vs_step2_cmp, 0, sizeof(step2_core_vs_step2_cmp));
    memset(&step2_core_forwarded_cmp, 0,
           sizeof(step2_core_forwarded_cmp));
    memset(&step2_core_forwarded_vs_step2_core_cmp, 0,
           sizeof(step2_core_forwarded_vs_step2_core_cmp));
    memset(&step2_core_forwarded_context_cmp, 0,
           sizeof(step2_core_forwarded_context_cmp));
    memset(&step2_core_forwarded_context_vs_forwarded_cmp, 0,
           sizeof(step2_core_forwarded_context_vs_forwarded_cmp));
    memset(&step2_core_forwarded_context_vs_core_cmp, 0,
           sizeof(step2_core_forwarded_context_vs_core_cmp));
    memset(&step2_core_forwarded_linebuf_cmp, 0,
           sizeof(step2_core_forwarded_linebuf_cmp));
    memset(&step2_core_forwarded_linebuf_vs_context_cmp, 0,
           sizeof(step2_core_forwarded_linebuf_vs_context_cmp));
    memset(&step2_core_forwarded_linebuf_vs_forwarded_cmp, 0,
           sizeof(step2_core_forwarded_linebuf_vs_forwarded_cmp));
    memset(&step2_core_forwarded_linebuf_vs_core_cmp, 0,
           sizeof(step2_core_forwarded_linebuf_vs_core_cmp));
    memset(&step2_pretransform_cmp, 0, sizeof(step2_pretransform_cmp));
    memset(&step2_pretransform_vs_forwarded_cmp, 0,
           sizeof(step2_pretransform_vs_forwarded_cmp));
    memset(&step2_pretransform_vs_context_cmp, 0,
           sizeof(step2_pretransform_vs_context_cmp));
    memset(&step2_pretransform_context_cmp, 0,
           sizeof(step2_pretransform_context_cmp));
    memset(&step2_pretransform_context_vs_pretransform_cmp, 0,
           sizeof(step2_pretransform_context_vs_pretransform_cmp));
    memset(&step2_pretransform_optprep_cmp, 0,
           sizeof(step2_pretransform_optprep_cmp));
    memset(&step2_pretransform_optprep_vs_pretransform_cmp, 0,
           sizeof(step2_pretransform_optprep_vs_pretransform_cmp));
    memset(&step2_pretransform_optprep_context_cmp, 0,
           sizeof(step2_pretransform_optprep_context_cmp));
    memset(&step2_pretransform_optprep_context_vs_context_cmp, 0,
           sizeof(step2_pretransform_optprep_context_vs_context_cmp));
    step1_cmp.first_line = -1;
    step1_cmp.first_cell = -1;
    step1_cmp.first_lane = -1;
    step2_cmp.first_line = -1;
    step2_cmp.first_cell = -1;
    step2_cmp.first_lane = -1;
    step2_vs_step1_cmp.first_line = -1;
    step2_vs_step1_cmp.first_cell = -1;
    step2_vs_step1_cmp.first_lane = -1;
    step2_core_cmp.first_line = -1;
    step2_core_cmp.first_cell = -1;
    step2_core_cmp.first_lane = -1;
    step2_core_vs_step2_cmp.first_line = -1;
    step2_core_vs_step2_cmp.first_cell = -1;
    step2_core_vs_step2_cmp.first_lane = -1;
    step2_core_forwarded_cmp.first_line = -1;
    step2_core_forwarded_cmp.first_cell = -1;
    step2_core_forwarded_cmp.first_lane = -1;
    step2_core_forwarded_vs_step2_core_cmp.first_line = -1;
    step2_core_forwarded_vs_step2_core_cmp.first_cell = -1;
    step2_core_forwarded_vs_step2_core_cmp.first_lane = -1;
    step2_core_forwarded_context_cmp.first_line = -1;
    step2_core_forwarded_context_cmp.first_cell = -1;
    step2_core_forwarded_context_cmp.first_lane = -1;
    step2_core_forwarded_context_vs_forwarded_cmp.first_line = -1;
    step2_core_forwarded_context_vs_forwarded_cmp.first_cell = -1;
    step2_core_forwarded_context_vs_forwarded_cmp.first_lane = -1;
    step2_core_forwarded_context_vs_core_cmp.first_line = -1;
    step2_core_forwarded_context_vs_core_cmp.first_cell = -1;
    step2_core_forwarded_context_vs_core_cmp.first_lane = -1;
    step2_core_forwarded_linebuf_cmp.first_line = -1;
    step2_core_forwarded_linebuf_cmp.first_cell = -1;
    step2_core_forwarded_linebuf_cmp.first_lane = -1;
    step2_core_forwarded_linebuf_vs_context_cmp.first_line = -1;
    step2_core_forwarded_linebuf_vs_context_cmp.first_cell = -1;
    step2_core_forwarded_linebuf_vs_context_cmp.first_lane = -1;
    step2_core_forwarded_linebuf_vs_forwarded_cmp.first_line = -1;
    step2_core_forwarded_linebuf_vs_forwarded_cmp.first_cell = -1;
    step2_core_forwarded_linebuf_vs_forwarded_cmp.first_lane = -1;
    step2_core_forwarded_linebuf_vs_core_cmp.first_line = -1;
    step2_core_forwarded_linebuf_vs_core_cmp.first_cell = -1;
    step2_core_forwarded_linebuf_vs_core_cmp.first_lane = -1;
    step2_pretransform_cmp.first_line = -1;
    step2_pretransform_cmp.first_cell = -1;
    step2_pretransform_cmp.first_lane = -1;
    step2_pretransform_vs_forwarded_cmp.first_line = -1;
    step2_pretransform_vs_forwarded_cmp.first_cell = -1;
    step2_pretransform_vs_forwarded_cmp.first_lane = -1;
    step2_pretransform_vs_context_cmp.first_line = -1;
    step2_pretransform_vs_context_cmp.first_cell = -1;
    step2_pretransform_vs_context_cmp.first_lane = -1;
    step2_pretransform_context_cmp.first_line = -1;
    step2_pretransform_context_cmp.first_cell = -1;
    step2_pretransform_context_cmp.first_lane = -1;
    step2_pretransform_context_vs_pretransform_cmp.first_line = -1;
    step2_pretransform_context_vs_pretransform_cmp.first_cell = -1;
    step2_pretransform_context_vs_pretransform_cmp.first_lane = -1;
    step2_pretransform_optprep_cmp.first_line = -1;
    step2_pretransform_optprep_cmp.first_cell = -1;
    step2_pretransform_optprep_cmp.first_lane = -1;
    step2_pretransform_optprep_vs_pretransform_cmp.first_line = -1;
    step2_pretransform_optprep_vs_pretransform_cmp.first_cell = -1;
    step2_pretransform_optprep_vs_pretransform_cmp.first_lane = -1;
    step2_pretransform_optprep_context_cmp.first_line = -1;
    step2_pretransform_optprep_context_cmp.first_cell = -1;
    step2_pretransform_optprep_context_cmp.first_lane = -1;
    step2_pretransform_optprep_context_vs_context_cmp.first_line = -1;
    step2_pretransform_optprep_context_vs_context_cmp.first_cell = -1;
    step2_pretransform_optprep_context_vs_context_cmp.first_lane = -1;

    parse_args(argc, argv, &cfg);
    init_problem(&problem, cfg.lines, cfg.cells);
    int cancel_test_pass = cfg.coeff_cancel_test ?
        run_coeff_cancel_test(&problem, &cfg) : 1;

    printf("CFD LU-SGS Path A Step2 benchmark\n");
    printf("mode.reference = %d\n", cfg.run_reference);
    printf("mode.step1 = %d\n", cfg.run_step1);
    printf("mode.step2 = %d\n", cfg.run_step2);
    printf("mode.step2_core = %d\n", cfg.run_step2_core);
    printf("mode.step2_core_forwarded = %d\n",
           cfg.run_step2_core_forwarded);
    printf("mode.step2_core_forwarded_context = %d\n",
           cfg.run_step2_core_forwarded_context);
    printf("mode.step2_core_forwarded_linebuf = %d\n",
           cfg.run_step2_core_forwarded_linebuf);
    printf("mode.step2_pretransform = %d\n", cfg.run_step2_pretransform);
    printf("mode.step2_pretransform_context = %d\n",
           cfg.run_step2_pretransform_context);
    printf("mode.step2_pretransform_optprep = %d\n",
           cfg.run_step2_pretransform_optprep);
    printf("mode.step2_pretransform_optprep_context = %d\n",
           cfg.run_step2_pretransform_optprep_context);
    printf("mode.step2_trsv5_raw = %d\n", cfg.run_step2_trsv5_raw);
    printf("mode.step2_trsv5_raw_context = %d\n",
           cfg.run_step2_trsv5_raw_context);
    printf("mode.step2_pretransform_raw = %d\n",
           cfg.run_step2_pretransform_raw);
    printf("mode.step2_pretransform_raw_context = %d\n",
           cfg.run_step2_pretransform_raw_context);
    printf("mode.step2_pretransform_raw_optprep = %d\n",
           cfg.run_step2_pretransform_raw_optprep);
    printf("mode.step2_pretransform_raw_optprep_context = %d\n",
           cfg.run_step2_pretransform_raw_optprep_context);
    printf("mode.step2_pretransform_raw_optprep_stream = %d\n",
           cfg.run_step2_pretransform_raw_stream);
    printf("streaming.enable = %d\n", cfg.coeff_streaming_enable);
    printf("streaming.windowDepth = %lu\n",
           (unsigned long)cfg.coeff_stream_window);
    printf("streaming.retireMode = %s\n", cfg.coeff_stream_retire_mode);
    printf("streaming.boundaryMask = %d\n", cfg.coeff_stream_boundary_mask);
    printf("streaming.dinvConsumer = %s\n",
           cfg.coeff_stream_dinv_consumer);
    printf("streaming.lbarConsumer = %s\n",
           cfg.coeff_stream_lbar_consumer);
    printf("streaming.columnFmaLatency = %lu\n",
           (unsigned long)cfg.coeff_column_fma_latency);
    printf("streaming.columnFmaIi = %lu\n",
           (unsigned long)cfg.coeff_column_fma_ii);
    printf("streaming.columnFmaCount = %lu\n",
           (unsigned long)cfg.coeff_column_fma_count);
    printf("streaming.columnFmaQueueDepth = %lu\n",
           (unsigned long)cfg.coeff_column_fma_queue_depth);
    printf("streaming.forwardCombineLatency = %lu\n",
           (unsigned long)cfg.forward_combine_latency);
    printf("streaming.forwardCombineIi = %lu\n",
           (unsigned long)cfg.forward_combine_ii);
    printf("streaming.forwardCombineCount = %lu\n",
           (unsigned long)cfg.forward_combine_count);
    printf("streaming.forwardCombineQueueDepth = %lu\n",
           (unsigned long)cfg.forward_combine_queue_depth);
    printf("mode.legacyInputSemantics = prepared-LU-L-Ubar-R\n");
    printf("mode.rawInputSemantics = D-L-U-R\n");
    printf("mode.linebuf_enable = %d\n", cfg.linebuf_enable);
    printf("mode.linebuf_entries = %lu\n",
           (unsigned long)cfg.linebuf_entries);
    printf("mode.check = %d\n", cfg.check);
    printf("lusgs.pretransformAuto = %d\n", cfg.lusgs_pretransform_auto);
    printf("lusgs.expectedSweeps = %lu\n",
           (unsigned long)cfg.lusgs_expected_sweeps);
    printf("lusgs.estimatedBreakEvenSweeps = %lu\n",
           (unsigned long)cfg.estimated_break_even_sweeps);
    printf("lusgs.autoCalibrationTrsv = %d\n",
           cfg.lusgs_pretransform_auto && cfg.run_step2_trsv5_raw);
    printf("lusgs.autoCalibrationPre = %d\n",
           cfg.lusgs_pretransform_auto &&
           cfg.run_step2_pretransform_raw_optprep);

    if (cfg.run_reference)
        reference_solve(&problem, &ref_stats);
    const int any_raw = cfg.run_step2_trsv5_raw ||
        cfg.run_step2_trsv5_raw_context ||
        cfg.run_step2_pretransform_raw ||
        cfg.run_step2_pretransform_raw_context ||
        cfg.run_step2_pretransform_raw_optprep ||
        cfg.run_step2_pretransform_raw_optprep_context ||
        cfg.run_step2_pretransform_raw_stream;
    int raw_reference_valid = 1;
    if (any_raw && cfg.check)
        raw_reference_valid = raw_reference_solve(&problem, &raw_ref_stats);

    step1_stats.software_trsv_cycles =
        measure_software_trsv_cycles(&problem, cfg.sweeps);
    step1_stats.trsv5_latency_configured = cfg.trsv5_lat;
    step2_stats.trsv5_latency_configured = cfg.trsv5_lat;
    step2_core_stats.trsv5_latency_configured = cfg.trsv5_lat;
    step2_core_forwarded_stats.trsv5_latency_configured = cfg.trsv5_lat;
    step2_core_forwarded_context_stats.trsv5_latency_configured =
        cfg.trsv5_lat;
    step2_core_forwarded_linebuf_stats.trsv5_latency_configured =
        cfg.trsv5_lat;

    int step2_pretransform_valid = 1;
    int step2_pretransform_context_valid = 1;
    int step2_pretransform_optprep_valid = 1;
    int step2_pretransform_optprep_context_valid = 1;
    int step2_trsv5_raw_valid = 1;
    int step2_trsv5_raw_context_valid = 1;
    int step2_pretransform_raw_valid = 1;
    int step2_pretransform_raw_context_valid = 1;
    int step2_pretransform_raw_optprep_valid = 1;
    int step2_pretransform_raw_optprep_context_valid = 1;
    int step2_pretransform_raw_stream_valid = 1;

    if (cfg.run_step1) {
        if (!cfg.run_step2 && !cfg.run_step2_core &&
            !cfg.run_step2_core_forwarded &&
            !cfg.run_step2_core_forwarded_context &&
            !cfg.run_step2_core_forwarded_linebuf)
            gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg, &step1_stats,
                          problem.dq_star_step1, problem.dq_step1,
                          0, 0, 0, 0, 0);
        if (!cfg.run_step2 && !cfg.run_step2_core &&
            !cfg.run_step2_core_forwarded &&
            !cfg.run_step2_core_forwarded_context &&
            !cfg.run_step2_core_forwarded_linebuf)
            gem5_m5_dump_stats();
    }

    if (cfg.run_step2) {
        gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg, &step2_stats,
                          problem.dq_star_step2, problem.dq_step2,
                          1, 0, 0, 0, 0);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_core) {
        gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg, &step2_core_stats,
                          problem.dq_star_step2_core, problem.dq_step2_core,
                          1, 1, 0, 0, 0);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_core_forwarded) {
        step2_core_forwarded_stats.trsv5_auto_selected =
            cfg.lusgs_pretransform_auto;
        step2_core_forwarded_stats.expected_sweeps =
            cfg.lusgs_expected_sweeps;
        step2_core_forwarded_stats.estimated_break_even_sweeps =
            cfg.estimated_break_even_sweeps;
        gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg, &step2_core_forwarded_stats,
                          problem.dq_star_step2_core_forwarded,
                          problem.dq_step2_core_forwarded,
                          1, 1, 1, 0, 0);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_core_forwarded_context) {
        gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg,
                          &step2_core_forwarded_context_stats,
                          problem.dq_star_step2_core_forwarded_context,
                          problem.dq_step2_core_forwarded_context,
                          1, 1, 1, 1, 0);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_core_forwarded_linebuf) {
        gem5_m5_reset_stats();
        patha_lusgs_solve(&problem, &cfg,
                          &step2_core_forwarded_linebuf_stats,
                          problem.dq_star_step2_core_forwarded_linebuf,
                          problem.dq_step2_core_forwarded_linebuf,
                          1, 1, 1, 0, 1);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_pretransform) {
        gem5_m5_reset_stats();
        step2_pretransform_valid = pretransform_lusgs_solve(
            &problem, &cfg, &step2_pretransform_stats,
            problem.dq_star_step2_pretransform,
            problem.dq_step2_pretransform, 0,
            0, &step2_pretransform_check);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_pretransform_context) {
        gem5_m5_reset_stats();
        step2_pretransform_context_valid = pretransform_lusgs_solve(
            &problem, &cfg, &step2_pretransform_context_stats,
            problem.dq_star_step2_pretransform_context,
            problem.dq_step2_pretransform_context, 1,
            0, &step2_pretransform_context_check);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_pretransform_optprep) {
        gem5_m5_reset_stats();
        step2_pretransform_optprep_stats.expected_sweeps =
            cfg.lusgs_expected_sweeps;
        step2_pretransform_optprep_stats.estimated_break_even_sweeps =
            cfg.estimated_break_even_sweeps;
        step2_pretransform_optprep_stats.pretransform_auto_selected =
            cfg.lusgs_pretransform_auto;
        step2_pretransform_optprep_valid = pretransform_lusgs_solve(
            &problem, &cfg, &step2_pretransform_optprep_stats,
            problem.dq_star_step2_pretransform_optprep,
            problem.dq_step2_pretransform_optprep, 0,
            1, &step2_pretransform_optprep_check);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_pretransform_optprep_context) {
        gem5_m5_reset_stats();
        step2_pretransform_optprep_context_stats.expected_sweeps =
            cfg.lusgs_expected_sweeps;
        step2_pretransform_optprep_context_stats.estimated_break_even_sweeps =
            cfg.estimated_break_even_sweeps;
        step2_pretransform_optprep_context_valid = pretransform_lusgs_solve(
            &problem, &cfg, &step2_pretransform_optprep_context_stats,
            problem.dq_star_step2_pretransform_optprep_context,
            problem.dq_step2_pretransform_optprep_context, 1,
            1, &step2_pretransform_optprep_context_check);
        gem5_m5_dump_stats();
    }

    if (cfg.run_step2_trsv5_raw) {
        gem5_m5_reset_stats();
        step2_trsv5_raw_valid = run_trsv5_raw(
            &problem, &cfg, &step2_trsv5_raw_stats,
            problem.dq_star_trsv5_raw, problem.dq_trsv5_raw, 0);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_trsv5_raw_context) {
        gem5_m5_reset_stats();
        step2_trsv5_raw_context_valid = run_trsv5_raw(
            &problem, &cfg, &step2_trsv5_raw_context_stats,
            problem.dq_star_trsv5_raw_context,
            problem.dq_trsv5_raw_context, 1);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_pretransform_raw) {
        gem5_m5_reset_stats();
        step2_pretransform_raw_valid = run_pretransform_raw(
            &problem, &cfg, &step2_pretransform_raw_stats,
            problem.dq_star_pretransform_raw,
            problem.dq_pretransform_raw, 0, 0,
            &step2_pretransform_raw_check);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_pretransform_raw_context) {
        gem5_m5_reset_stats();
        step2_pretransform_raw_context_valid = run_pretransform_raw(
            &problem, &cfg, &step2_pretransform_raw_context_stats,
            problem.dq_star_pretransform_raw_context,
            problem.dq_pretransform_raw_context, 1, 0,
            &step2_pretransform_raw_context_check);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_pretransform_raw_optprep) {
        gem5_m5_reset_stats();
        step2_pretransform_raw_optprep_valid = run_pretransform_raw(
            &problem, &cfg, &step2_pretransform_raw_optprep_stats,
            problem.dq_star_pretransform_raw_optprep,
            problem.dq_pretransform_raw_optprep, 0, 1,
            &step2_pretransform_raw_optprep_check);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_pretransform_raw_optprep_context) {
        gem5_m5_reset_stats();
        step2_pretransform_raw_optprep_context_valid = run_pretransform_raw(
            &problem, &cfg,
            &step2_pretransform_raw_optprep_context_stats,
            problem.dq_star_pretransform_raw_optprep_context,
            problem.dq_pretransform_raw_optprep_context, 1, 1,
            &step2_pretransform_raw_optprep_context_check);
        gem5_m5_dump_stats();
    }
    if (cfg.run_step2_pretransform_raw_stream) {
        gem5_m5_reset_stats();
        step2_pretransform_raw_stream_valid = run_pretransform_raw_stream(
            &problem, &cfg, &step2_pretransform_raw_stream_stats,
            problem.dq_star_pretransform_raw_stream,
            problem.dq_pretransform_raw_stream,
            &step2_pretransform_raw_stream_check);
        gem5_m5_dump_stats();
    }

    step2_pretransform_raw_stats.fair_extra_pretransform_cycles =
        step2_pretransform_raw_stats.coefficient_rebuilds ?
        step2_pretransform_raw_stats.extra_pretransform_cycles /
        step2_pretransform_raw_stats.coefficient_rebuilds : 0;
    step2_pretransform_raw_context_stats.fair_extra_pretransform_cycles =
        step2_pretransform_raw_context_stats.coefficient_rebuilds ?
        step2_pretransform_raw_context_stats.extra_pretransform_cycles /
        step2_pretransform_raw_context_stats.coefficient_rebuilds : 0;

    if (cfg.run_step2_trsv5_raw && cfg.run_step2_pretransform_raw) {
        double trsv_per = (double)step2_trsv5_raw_stats.
            trsv5_raw_runtime_cycles / (double)cfg.sweeps;
        double pre_per = (double)step2_pretransform_raw_stats.
            pretransform_raw_runtime_cycles / (double)cfg.sweeps;
        double saving = trsv_per - pre_per;
        if (saving > 0.0) {
            uint64_t rebuilds = step2_pretransform_raw_stats.
                coefficient_rebuilds;
            uint64_t extra = rebuilds ? step2_pretransform_raw_stats.
                extra_pretransform_cycles / rebuilds : 0;
            step2_pretransform_raw_stats.fair_break_even_sweeps =
                (uint64_t)ceil((double)extra / saving);
        }
    }
    if (cfg.run_step2_trsv5_raw &&
        cfg.run_step2_pretransform_raw_optprep) {
        double trsv_per = (double)step2_trsv5_raw_stats.
            trsv5_raw_runtime_cycles / (double)cfg.sweeps;
        double pre_per = (double)step2_pretransform_raw_optprep_stats.
            pretransform_raw_runtime_cycles / (double)cfg.sweeps;
        uint64_t coeff_phase = step2_pretransform_raw_optprep_stats.
            extra_pretransform_cycles;
        uint64_t common_ubar =
            !strcmp(cfg.coeff_preprocess_model, "event") ?
            step2_trsv5_raw_stats.common_preprocess_cycles :
            step2_trsv5_raw_stats.common_ubar_cycles;
        double saving = trsv_per - pre_per;
        if (coeff_phase > common_ubar)
            step2_pretransform_raw_optprep_stats.
                fair_extra_pretransform_cycles = coeff_phase - common_ubar;
        uint64_t rebuilds = step2_pretransform_raw_optprep_stats.
            coefficient_rebuilds;
        uint64_t fair_extra = coeff_phase > common_ubar ?
            coeff_phase - common_ubar : 0;
        if (rebuilds)
            fair_extra /= rebuilds;
        step2_pretransform_raw_optprep_stats.
            fair_extra_pretransform_cycles = fair_extra;
        if (saving > 0.0 && fair_extra)
            step2_pretransform_raw_optprep_stats.fair_break_even_sweeps =
                (uint64_t)ceil((double)fair_extra / saving);
    }
    if (cfg.run_step2_trsv5_raw_context &&
        cfg.run_step2_pretransform_raw_context) {
        double trsv_per = (double)step2_trsv5_raw_context_stats.
            trsv5_raw_runtime_cycles / (double)cfg.sweeps;
        double pre_per = (double)step2_pretransform_raw_context_stats.
            pretransform_raw_runtime_cycles / (double)cfg.sweeps;
        double saving = trsv_per - pre_per;
        uint64_t rebuilds = step2_pretransform_raw_context_stats.
            coefficient_rebuilds;
        uint64_t extra = rebuilds ? step2_pretransform_raw_context_stats.
            extra_pretransform_cycles / rebuilds : 0;
        if (saving > 0.0)
            step2_pretransform_raw_context_stats.fair_break_even_sweeps =
                (uint64_t)ceil((double)extra / saving);
    }
    if (cfg.run_step2_trsv5_raw_context &&
        cfg.run_step2_pretransform_raw_optprep_context) {
        double trsv_per = (double)step2_trsv5_raw_context_stats.
            trsv5_raw_runtime_cycles / (double)cfg.sweeps;
        double pre_per = (double)step2_pretransform_raw_optprep_context_stats.
            pretransform_raw_runtime_cycles / (double)cfg.sweeps;
        uint64_t coeff_phase =
            step2_pretransform_raw_optprep_context_stats.
                extra_pretransform_cycles;
        uint64_t common_ubar =
            !strcmp(cfg.coeff_preprocess_model, "event") ?
            step2_trsv5_raw_context_stats.common_preprocess_cycles :
            step2_trsv5_raw_context_stats.common_ubar_cycles;
        double saving = trsv_per - pre_per;
        if (coeff_phase > common_ubar)
            step2_pretransform_raw_optprep_context_stats.
                fair_extra_pretransform_cycles = coeff_phase - common_ubar;
        uint64_t rebuilds =
            step2_pretransform_raw_optprep_context_stats.
                coefficient_rebuilds;
        uint64_t fair_extra = coeff_phase > common_ubar ?
            coeff_phase - common_ubar : 0;
        if (rebuilds)
            fair_extra /= rebuilds;
        step2_pretransform_raw_optprep_context_stats.
            fair_extra_pretransform_cycles = fair_extra;
        if (saving > 0.0 && fair_extra)
            step2_pretransform_raw_optprep_context_stats.
                fair_break_even_sweeps = (uint64_t)ceil(
                    (double)fair_extra / saving);
    }

    if (cfg.lusgs_pretransform_auto) {
        uint64_t measured_break_even =
            step2_pretransform_raw_optprep_stats.fair_break_even_sweeps;
        double trsv_per = (double)step2_trsv5_raw_stats.
            trsv5_raw_runtime_cycles / (double)cfg.sweeps;
        double pre_per = (double)step2_pretransform_raw_optprep_stats.
            pretransform_raw_runtime_cycles / (double)cfg.sweeps;
        double saving = trsv_per - pre_per;
        if (saving > 0.0 && measured_break_even == 0)
            measured_break_even = 1;
        auto_selected_pre = saving > 0.0 &&
            cfg.lusgs_expected_sweeps >= measured_break_even;
        cfg.estimated_break_even_sweeps = measured_break_even;
        auto_selection_ran = 1;

        gem5_m5_reset_stats();
        if (auto_selected_pre) {
            auto_selected_valid = run_pretransform_raw(
                &problem, &cfg, &auto_selected_stats,
                problem.dq_star_pretransform_raw_optprep,
                problem.dq_pretransform_raw_optprep, 0, 1,
                &auto_selected_check);
            auto_selected_stats.pretransform_auto_selected = 1;
        } else {
            auto_selected_valid = run_trsv5_raw(
                &problem, &cfg, &auto_selected_stats,
                problem.dq_star_trsv5_raw, problem.dq_trsv5_raw, 0);
            auto_selected_stats.trsv5_auto_selected = 1;
        }
        auto_selected_stats.expected_sweeps = cfg.lusgs_expected_sweeps;
        auto_selected_stats.estimated_break_even_sweeps = measured_break_even;
        auto_selected_stats.fair_break_even_sweeps = measured_break_even;
        auto_selected_stats.fair_extra_pretransform_cycles =
            step2_pretransform_raw_optprep_stats.
                fair_extra_pretransform_cycles;
        gem5_m5_dump_stats();
    }

    int counts_ok = 1;
    uint64_t mvm_per_dir_per_sweep =
        (uint64_t)cfg.lines * (uint64_t)(cfg.cells > 0 ? cfg.cells - 1 : 0);
    uint64_t expected_forward_mvm = mvm_per_dir_per_sweep * cfg.sweeps;
    uint64_t expected_backward_mvm = mvm_per_dir_per_sweep * cfg.sweeps;
    uint64_t expected_trsv =
        (uint64_t)cfg.lines * (uint64_t)cfg.cells * cfg.sweeps;
    uint64_t expected_all_cells =
        (uint64_t)cfg.lines * (uint64_t)cfg.cells * cfg.sweeps;
    uint64_t expected_linebuf_ordinary_cells = expected_forward_mvm;
    if (cfg.run_step1) {
        counts_ok &= step1_stats.patha_mvm_forward == expected_forward_mvm;
        counts_ok &= step1_stats.patha_mvm_backward == expected_backward_mvm;
        counts_ok &= step1_stats.software_trsv == expected_trsv;
        counts_ok &= step1_stats.hardware_trsv == 0;
        counts_ok &= step1_stats.software_trsv_hot_path == expected_trsv;
        counts_ok &= step1_stats.vector_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step1_stats.tmp_result_stores ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step1_stats.tmp_result_loads ==
                     expected_forward_mvm + expected_backward_mvm;
    }
    if (cfg.run_step2) {
        counts_ok &= step2_stats.patha_mvm_forward == expected_forward_mvm;
        counts_ok &= step2_stats.patha_mvm_backward == expected_backward_mvm;
        counts_ok &= step2_stats.hardware_trsv == expected_trsv;
        counts_ok &= step2_stats.software_trsv == 0;
        counts_ok &= step2_stats.software_trsv_hot_path == 0;
        counts_ok &= step2_stats.vector_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_stats.tmp_result_stores ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_stats.tmp_result_loads ==
                     expected_forward_mvm + expected_backward_mvm;
    }
    if (cfg.run_step2_core) {
        counts_ok &= step2_core_stats.patha_mvm_forward ==
                     expected_forward_mvm;
        counts_ok &= step2_core_stats.patha_mvm_backward ==
                     expected_backward_mvm;
        counts_ok &= step2_core_stats.hardware_trsv == expected_trsv;
        counts_ok &= step2_core_stats.software_trsv == 0;
        counts_ok &= step2_core_stats.software_trsv_hot_path == 0;
        counts_ok &= step2_core_stats.fused_forward_updates ==
                     expected_forward_mvm;
        counts_ok &= step2_core_stats.fused_backward_updates ==
                     expected_backward_mvm;
        counts_ok &= step2_core_stats.fused_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_core_stats.hardware_vector_sub ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_core_stats.vector_sub_ops == 0;
        counts_ok &= step2_core_stats.tmp_result_stores == 0;
        counts_ok &= step2_core_stats.tmp_result_loads == 0;
    }
    if (cfg.run_step2_core_forwarded) {
        counts_ok &= step2_core_forwarded_stats.patha_mvm_forward ==
                     expected_forward_mvm;
        counts_ok &= step2_core_forwarded_stats.patha_mvm_backward ==
                     expected_backward_mvm;
        counts_ok &= step2_core_forwarded_stats.hardware_trsv ==
                     expected_trsv;
        counts_ok &= step2_core_forwarded_stats.software_trsv == 0;
        counts_ok &=
            step2_core_forwarded_stats.software_trsv_hot_path == 0;
        counts_ok &= step2_core_forwarded_stats.fused_forward_updates ==
                     expected_forward_mvm;
        counts_ok &= step2_core_forwarded_stats.fused_backward_updates ==
                     expected_backward_mvm;
        counts_ok &= step2_core_forwarded_stats.fused_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_core_forwarded_stats.hardware_vector_sub ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &= step2_core_forwarded_stats.vector_sub_ops == 0;
        counts_ok &= step2_core_forwarded_stats.tmp_result_stores == 0;
        counts_ok &= step2_core_forwarded_stats.tmp_result_loads == 0;
        counts_ok &= step2_core_forwarded_stats.trsv5_rhs_forwarded ==
                     expected_forward_mvm;
        counts_ok &= step2_core_forwarded_stats.trsv5_rhs_spm_stage_elided ==
                     expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_stats
                .step2_core_forward_rhs_stack_store_elided ==
            expected_forward_mvm;
        counts_ok &= step2_core_forwarded_stats.trsv5_rhs_forward_consumed ==
                     expected_forward_mvm;
        counts_ok &= step2_core_forwarded_stats.trsv5_rhs_forward_invalid ==
                     0;
    }
    if (cfg.run_step2_core_forwarded_context) {
        counts_ok &=
            step2_core_forwarded_context_stats.patha_mvm_forward ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.patha_mvm_backward ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.hardware_trsv ==
            expected_trsv;
        counts_ok &=
            step2_core_forwarded_context_stats.software_trsv == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.software_trsv_hot_path == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.fused_forward_updates ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.fused_backward_updates ==
            expected_backward_mvm;
        counts_ok &= step2_core_forwarded_context_stats.fused_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.hardware_vector_sub ==
            expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.vector_sub_ops == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.tmp_result_stores == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.tmp_result_loads == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.trsv5_rhs_forwarded ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.trsv5_rhs_spm_stage_elided ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats
                .step2_core_forward_rhs_stack_store_elided ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.trsv5_rhs_forward_consumed ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.trsv5_rhs_forward_invalid == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.forward_context_hits ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.forward_context_misses == 0;
        counts_ok &= step2_core_forwarded_context_stats
                         .forward_dqstar_heap_vector_load_elided ==
                     expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.forward_context_vector_stages ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.forward_context_updates ==
            expected_trsv;
        counts_ok &=
            step2_core_forwarded_context_stats.forward_context_invalid == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.backward_context_hits ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.backward_context_misses == 0;
        counts_ok &=
            step2_core_forwarded_context_stats
                .backward_dq_heap_vector_load_elided ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats
                .backward_context_vector_stages ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.backward_context_updates ==
            expected_trsv;
        counts_ok &=
            step2_core_forwarded_context_stats.backward_context_invalid == 0;
        counts_ok &=
            step2_core_forwarded_context_stats.line_context_forwarding_enabled ==
            1;
        counts_ok &=
            step2_core_forwarded_context_stats.line_context_total_hits ==
            expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_context_stats.line_context_total_misses == 0;
    }
    if (cfg.run_step2_core_forwarded_linebuf) {
        counts_ok &=
            step2_core_forwarded_linebuf_stats.patha_mvm_forward ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.patha_mvm_backward ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.hardware_trsv ==
            expected_trsv;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.software_trsv == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.software_trsv_hot_path == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.fused_forward_updates ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.fused_backward_updates ==
            expected_backward_mvm;
        counts_ok &= step2_core_forwarded_linebuf_stats.fused_sub_ops ==
                     expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.hardware_vector_sub ==
            expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.vector_sub_ops == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.tmp_result_stores == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.tmp_result_loads == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.trsv5_rhs_forwarded ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.trsv5_rhs_spm_stage_elided ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats
                .step2_core_forward_rhs_stack_store_elided ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.trsv5_rhs_forward_consumed ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.trsv5_rhs_forward_invalid == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.forward_context_hits ==
            expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.forward_context_misses == 0;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .forward_dqstar_heap_vector_load_elided ==
                     expected_forward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.forward_context_vector_stages ==
            0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.forward_context_updates == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.forward_context_invalid == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.backward_context_hits ==
            expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.backward_context_misses == 0;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .backward_dq_heap_vector_load_elided ==
                     expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.backward_context_vector_stages ==
            0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.backward_context_updates == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.backward_context_invalid == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.line_context_forwarding_enabled ==
            1;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.line_context_total_hits ==
            expected_forward_mvm + expected_backward_mvm;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.line_context_total_misses == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_forward_reads ==
            expected_linebuf_ordinary_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_forward_writes ==
            expected_all_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_backward_reads ==
            expected_linebuf_ordinary_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_backward_writes ==
            expected_all_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_forward_hits ==
            expected_linebuf_ordinary_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_forward_misses == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_backward_hits ==
            expected_linebuf_ordinary_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_backward_misses == 0;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .linebuf_spm_vector_stages_elided ==
                     2U * expected_linebuf_ordinary_cells;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .linebuf_lmat_vec_loads_elided ==
                     2U * expected_linebuf_ordinary_cells;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .linebuf_context_memory_stores_elided ==
                     2U * expected_all_cells;
        counts_ok &= step2_core_forwarded_linebuf_stats
                         .linebuf_context_memory_loads_elided ==
                     2U * expected_linebuf_ordinary_cells;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_tag_conflicts == 0;
        counts_ok &=
            step2_core_forwarded_linebuf_stats.linebuf_invalid_reads == 0;
    }

    uint64_t preprocess_batches =
        (uint64_t)cfg.lines * (uint64_t)cfg.cells *
        (cfg.pretransform_per_sweep ? cfg.sweeps : 1U);
    uint64_t expected_pretransform_forward_mvm =
        expected_all_cells + expected_forward_mvm;
    uint64_t expected_pretransform_sub =
        expected_forward_mvm + expected_backward_mvm;
    if (cfg.run_step2_pretransform) {
        counts_ok &= step2_pretransform_stats.trsm5_mrhs_inv_batches ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_stats.trsm5_mrhs_lbar_batches ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_stats.trsm5_mrhs_ubar_batches == 0;
        counts_ok &= step2_pretransform_stats.ubar_reused_existing ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_stats.patha_mvm_forward ==
                     expected_pretransform_forward_mvm;
        counts_ok &= step2_pretransform_stats.patha_mvm_backward ==
                     expected_backward_mvm;
        counts_ok &= step2_pretransform_stats.pretransform_vector_sub ==
                     expected_pretransform_sub;
        counts_ok &= step2_pretransform_stats.pretransform_hot_loop_trsv5_elided ==
                     expected_all_cells;
        counts_ok &= step2_pretransform_stats.hardware_trsv == 0;
        counts_ok &= step2_pretransform_stats.pretransform_tmp_stores == 0;
        counts_ok &= step2_pretransform_stats.pretransform_tmp_loads == 0;
        counts_ok &= step2_pretransform_stats.pretransform_patha_invalid_slots == 0;
        counts_ok &= step2_pretransform_check.d_inv.mismatch_count == 0;
        counts_ok &= step2_pretransform_check.l_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_check.u_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_valid ||
                     step2_pretransform_stats.pretransform_fallback_runs == 1;
    }
    if (cfg.run_step2_pretransform_context) {
        counts_ok &= step2_pretransform_context_stats.trsm5_mrhs_inv_batches ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_context_stats.trsm5_mrhs_lbar_batches ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_context_stats.trsm5_mrhs_ubar_batches == 0;
        counts_ok &= step2_pretransform_context_stats.ubar_reused_existing ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_context_stats.patha_mvm_forward ==
                     expected_pretransform_forward_mvm;
        counts_ok &= step2_pretransform_context_stats.patha_mvm_backward ==
                     expected_backward_mvm;
        counts_ok &= step2_pretransform_context_stats.pretransform_vector_sub ==
                     expected_pretransform_sub;
        counts_ok &= step2_pretransform_context_stats.forward_context_hits ==
                     expected_forward_mvm;
        counts_ok &= step2_pretransform_context_stats.backward_context_hits ==
                     expected_backward_mvm;
        counts_ok &= step2_pretransform_context_stats.hardware_trsv == 0;
        counts_ok &= step2_pretransform_context_stats.pretransform_tmp_stores == 0;
        counts_ok &= step2_pretransform_context_stats.pretransform_tmp_loads == 0;
        counts_ok &= step2_pretransform_context_stats.pretransform_patha_invalid_slots == 0;
        counts_ok &= step2_pretransform_context_check.d_inv.mismatch_count == 0;
        counts_ok &= step2_pretransform_context_check.l_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_context_check.u_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_context_valid ||
                     step2_pretransform_context_stats.pretransform_fallback_runs == 1;
    }
    if (cfg.run_step2_pretransform_optprep) {
        counts_ok &= step2_pretransform_optprep_stats
                         .trsm5_inv_lbar_issued == preprocess_batches;
        counts_ok &= step2_pretransform_optprep_stats
                         .trsm5_inv_lbar_completed == preprocess_batches;
        counts_ok &= step2_pretransform_optprep_stats
                         .trsm5_inv_lbar_lu_reads == preprocess_batches;
        counts_ok &= step2_pretransform_optprep_stats.preprocess_lu_stages ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_optprep_stats.preprocess_c_stages ==
                     preprocess_batches;
        counts_ok &= step2_pretransform_optprep_stats
                         .preprocess_identity_stages == 0;
        counts_ok &= step2_pretransform_optprep_stats.patha_mvm_forward ==
                     expected_pretransform_forward_mvm;
        counts_ok &= step2_pretransform_optprep_stats.patha_mvm_backward ==
                     expected_backward_mvm;
        counts_ok &= step2_pretransform_optprep_stats.pretransform_vector_sub ==
                     expected_pretransform_sub;
        counts_ok &= step2_pretransform_optprep_stats.hardware_trsv == 0;
        counts_ok &= step2_pretransform_optprep_check.d_inv.mismatch_count == 0;
        counts_ok &= step2_pretransform_optprep_check.l_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_optprep_valid ||
                     step2_pretransform_optprep_stats
                             .pretransform_fallback_runs == 1;
    }
    if (cfg.run_step2_pretransform_optprep_context) {
        counts_ok &= step2_pretransform_optprep_context_stats
                         .trsm5_inv_lbar_issued == preprocess_batches;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .trsm5_inv_lbar_lu_reads == preprocess_batches;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .preprocess_identity_stages == 0;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .patha_mvm_forward == expected_pretransform_forward_mvm;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .patha_mvm_backward == expected_backward_mvm;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .pretransform_vector_sub == expected_pretransform_sub;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .forward_context_hits == expected_forward_mvm;
        counts_ok &= step2_pretransform_optprep_context_stats
                         .backward_context_hits == expected_backward_mvm;
        counts_ok &= step2_pretransform_optprep_context_check
                         .d_inv.mismatch_count == 0;
        counts_ok &= step2_pretransform_optprep_context_check
                         .l_bar.mismatch_count == 0;
        counts_ok &= step2_pretransform_optprep_context_valid ||
                     step2_pretransform_optprep_context_stats
                             .pretransform_fallback_runs == 1;
    }

    uint64_t raw_interval = cfg.coefficient_update_interval ?
        cfg.coefficient_update_interval : cfg.sweeps;
    uint64_t raw_rebuilds =
        (cfg.sweeps + raw_interval - 1) / raw_interval;
    uint64_t raw_cells = (uint64_t)cfg.lines * cfg.cells * raw_rebuilds;
    if (cfg.run_step2_trsv5_raw) {
        counts_ok &= step2_trsv5_raw_stats.lu5_factor_cells == raw_cells;
        counts_ok &= step2_trsv5_raw_stats.ubar_trsm_issued == raw_cells;
        counts_ok &= step2_trsv5_raw_stats.ubar_columns_solved ==
                     raw_cells * N;
        counts_ok &= step2_trsv5_raw_stats.ubar_reused_existing == 0;
        counts_ok &= step2_trsv5_raw_valid;
    }
    if (cfg.run_step2_trsv5_raw_context) {
        counts_ok &= step2_trsv5_raw_context_stats.lu5_factor_cells ==
                     raw_cells;
        counts_ok &= step2_trsv5_raw_context_stats.ubar_trsm_issued ==
                     raw_cells;
        counts_ok &= step2_trsv5_raw_context_stats.ubar_reused_existing == 0;
        counts_ok &= step2_trsv5_raw_context_valid;
    }
    if (cfg.run_step2_pretransform_raw) {
        counts_ok &= step2_pretransform_raw_stats.lu5_factor_cells ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_stats.ubar_trsm_issued ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_stats.trsm5_inv_lbar_issued ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_stats.ubar_reused_existing == 0;
        counts_ok &= step2_pretransform_raw_valid;
    }
    if (cfg.run_step2_pretransform_raw_context) {
        counts_ok &= step2_pretransform_raw_context_stats.lu5_factor_cells ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_context_stats.ubar_trsm_issued ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_context_stats
                         .trsm5_inv_lbar_issued == raw_cells;
        counts_ok &= step2_pretransform_raw_context_valid;
    }
    if (cfg.run_step2_pretransform_raw_optprep) {
        counts_ok &= step2_pretransform_raw_optprep_stats.lu5_factor_cells ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_optprep_stats.coeff3_issued ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_optprep_stats
                         .coeff3_columns_solved == raw_cells * 15U;
        counts_ok &= step2_pretransform_raw_optprep_stats
                         .ubar_trsm_issued == 0;
        counts_ok &= step2_pretransform_raw_optprep_stats
                         .ubar_reused_existing == 0;
        counts_ok &= step2_pretransform_raw_optprep_valid;
    }
    if (cfg.run_step2_pretransform_raw_optprep_context) {
        counts_ok &= step2_pretransform_raw_optprep_context_stats
                         .lu5_factor_cells == raw_cells;
        counts_ok &= step2_pretransform_raw_optprep_context_stats
                         .coeff3_issued == raw_cells;
        counts_ok &= step2_pretransform_raw_optprep_context_valid;
    }
    if (cfg.run_step2_pretransform_raw_stream) {
        const uint64_t stream_rhs_per_line = cfg.coeff_stream_boundary_mask ?
            5U * (cfg.cells +
                  (cfg.cells > 1 ? 2U * (cfg.cells - 1) : 0U)) :
            15U * cfg.cells;
        const uint64_t expected_stream_rhs = raw_rebuilds * cfg.lines *
            stream_rhs_per_line;
        counts_ok &= step2_pretransform_raw_stream_stats.lu5_factor_cells ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_stream_stats.coeff3_issued ==
                     raw_cells;
        counts_ok &= step2_pretransform_raw_stream_stats
                         .coeff3_columns_solved == expected_stream_rhs;
        counts_ok &= step2_pretransform_raw_stream_stats
                         .streaming_effective_rhs_count ==
                     expected_stream_rhs;
        const int direct_consumer =
            !strcmp(cfg.coeff_stream_dinv_consumer, "direct");
        const int direct_lbar_consumer =
            !strcmp(cfg.coeff_stream_lbar_consumer, "direct");
        const int direct_ubar_consumer =
            !strcmp(cfg.coeff_stream_ubar_consumer, "direct");
        counts_ok &= step2_pretransform_raw_stream_stats.patha_mvm_forward ==
                     (direct_lbar_consumer ? 0 : direct_consumer ?
                                        expected_forward_mvm :
                                        expected_pretransform_forward_mvm);
        counts_ok &= step2_pretransform_raw_stream_stats.patha_mvm_backward ==
                     (direct_ubar_consumer ? 0 : expected_backward_mvm);
        counts_ok &= step2_pretransform_raw_stream_stats.forward_context_hits ==
                     (direct_lbar_consumer ? 0 : expected_forward_mvm);
        counts_ok &= step2_pretransform_raw_stream_stats.backward_context_hits ==
                     (direct_ubar_consumer ? 0 : expected_backward_mvm);
        counts_ok &= step2_pretransform_raw_stream_stats
                         .event_requests_allocated == raw_cells;
        counts_ok &= step2_pretransform_raw_stream_stats
                         .event_requests_completed == raw_cells;
        counts_ok &= step2_pretransform_raw_stream_stats
                         .streaming_forward_cells_consumed ==
                     (uint64_t)cfg.lines * cfg.cells * cfg.sweeps;
        counts_ok &= step2_pretransform_raw_stream_stats.coefficient_reuses ==
                     cfg.sweeps - raw_rebuilds;
        if (!strcmp(cfg.coeff_stream_retire_mode, "auto")) {
            counts_ok &= step2_pretransform_raw_stream_stats
                             .auto_retired_requests == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .auto_retired_tokens == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .auto_retire_errors == 0;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .streaming_completion_reaper_polls == 0;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .terminal_wait_calls == 0;
        }
        if (strcmp(cfg.coeff_stream_dinv_consumer, "off")) {
            const uint64_t expected_columns = raw_cells * N;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .dinv_columns_consumed == expected_columns;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_accumulators_allocated == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_accumulators_completed == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_accumulator_missing_columns == 0;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_accumulator_double_consumes == 0;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_accumulator_stale_writes == 0;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .base_vector_publish_count ==
                         (direct_lbar_consumer ? 0 : raw_cells);
            if (!strcmp(cfg.coeff_stream_dinv_consumer, "shadow"))
                counts_ok &= step2_pretransform_raw_stream_stats
                                 .shadow_base_bitwise_mismatch == 0;
        }
        if (direct_consumer) {
            counts_ok &= step2_pretransform_raw_stream_stats
                             .dinv_matrix_drains_avoided == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .patha_dinv_mvm_eliminated ==
                         (uint64_t)cfg.lines * cfg.cells * cfg.sweeps;
        }
        if (strcmp(cfg.coeff_stream_lbar_consumer, "off")) {
            const uint64_t expected_lbar_columns =
                (raw_cells - raw_rebuilds * cfg.lines) * N;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .lbar_columns_consumed ==
                         expected_lbar_columns;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .correction_accumulators_completed ==
                         raw_cells - raw_rebuilds * cfg.lines;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .forward_handoffs_produced == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .forward_handoffs_consumed ==
                         raw_cells - raw_rebuilds * cfg.lines;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .forward_combine_completed ==
                         raw_cells - raw_rebuilds * cfg.lines;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .dqstar_publish_count == raw_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .consumer_scheduler_starvation_violations == 0;
            if (!strcmp(cfg.coeff_stream_lbar_consumer, "shadow")) {
                counts_ok &= step2_pretransform_raw_stream_stats
                                 .shadow_correction_bitwise_mismatch == 0;
                counts_ok &= step2_pretransform_raw_stream_stats
                                 .shadow_dqstar_bitwise_mismatch == 0;
            }
        }
        if (direct_lbar_consumer) {
            const uint64_t forward_cells =
                raw_cells - raw_rebuilds * cfg.lines;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .lbar_matrix_drains_avoided == forward_cells;
            counts_ok &= step2_pretransform_raw_stream_stats
                             .patha_lbar_mvm_eliminated == forward_cells;
        }
        counts_ok &= step2_pretransform_raw_stream_valid;
    }

    if (cfg.run_reference && cfg.run_step1 && cfg.check)
        step1_cmp = compare_solution(&problem, problem.dq_ref,
                                     problem.dq_step1, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2 && cfg.check)
        step2_cmp = compare_solution(&problem, problem.dq_ref,
                                     problem.dq_step2, 1.0e-10, 1.0e-10);
    if (cfg.run_step1 && cfg.run_step2 && cfg.check)
        step2_vs_step1_cmp = compare_solution(&problem, problem.dq_step1,
                                              problem.dq_step2, 1.0e-10,
                                              1.0e-10);
    if (cfg.run_reference && cfg.run_step2_core && cfg.check)
        step2_core_cmp = compare_solution(
            &problem, problem.dq_ref, problem.dq_step2_core,
            1.0e-10, 1.0e-10);
    if (cfg.run_step2 && cfg.run_step2_core && cfg.check)
        step2_core_vs_step2_cmp = compare_solution(
            &problem, problem.dq_step2, problem.dq_step2_core,
            1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_core_forwarded && cfg.check)
        step2_core_forwarded_cmp = compare_solution(
            &problem, problem.dq_ref, problem.dq_step2_core_forwarded,
            1.0e-10, 1.0e-10);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded && cfg.check)
        step2_core_forwarded_vs_step2_core_cmp = compare_solution(
            &problem, problem.dq_step2_core,
            problem.dq_step2_core_forwarded, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_core_forwarded_context &&
        cfg.check)
        step2_core_forwarded_context_cmp = compare_solution(
            &problem, problem.dq_ref,
            problem.dq_step2_core_forwarded_context, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core_forwarded &&
        cfg.run_step2_core_forwarded_context && cfg.check)
        step2_core_forwarded_context_vs_forwarded_cmp = compare_solution(
            &problem, problem.dq_step2_core_forwarded,
            problem.dq_step2_core_forwarded_context, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded_context &&
        cfg.check)
        step2_core_forwarded_context_vs_core_cmp = compare_solution(
            &problem, problem.dq_step2_core,
            problem.dq_step2_core_forwarded_context, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_core_forwarded_linebuf &&
        cfg.check)
        step2_core_forwarded_linebuf_cmp = compare_solution(
            &problem, problem.dq_ref,
            problem.dq_step2_core_forwarded_linebuf, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core_forwarded_context &&
        cfg.run_step2_core_forwarded_linebuf && cfg.check)
        step2_core_forwarded_linebuf_vs_context_cmp = compare_solution(
            &problem, problem.dq_step2_core_forwarded_context,
            problem.dq_step2_core_forwarded_linebuf, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core_forwarded &&
        cfg.run_step2_core_forwarded_linebuf && cfg.check)
        step2_core_forwarded_linebuf_vs_forwarded_cmp = compare_solution(
            &problem, problem.dq_step2_core_forwarded,
            problem.dq_step2_core_forwarded_linebuf, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded_linebuf &&
        cfg.check)
        step2_core_forwarded_linebuf_vs_core_cmp = compare_solution(
            &problem, problem.dq_step2_core,
            problem.dq_step2_core_forwarded_linebuf, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_pretransform && cfg.check)
        step2_pretransform_cmp = compare_solution(
            &problem, problem.dq_ref, problem.dq_step2_pretransform,
            1.0e-10, 1.0e-10);
    if (cfg.run_step2_core_forwarded && cfg.run_step2_pretransform && cfg.check)
        step2_pretransform_vs_forwarded_cmp = compare_solution(
            &problem, problem.dq_step2_core_forwarded,
            problem.dq_step2_pretransform, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_core_forwarded_context &&
        cfg.run_step2_pretransform && cfg.check)
        step2_pretransform_vs_context_cmp = compare_solution(
            &problem, problem.dq_step2_core_forwarded_context,
            problem.dq_step2_pretransform, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_pretransform_context && cfg.check)
        step2_pretransform_context_cmp = compare_solution(
            &problem, problem.dq_ref,
            problem.dq_step2_pretransform_context, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_pretransform && cfg.run_step2_pretransform_context &&
        cfg.check)
        step2_pretransform_context_vs_pretransform_cmp = compare_solution(
            &problem, problem.dq_step2_pretransform,
            problem.dq_step2_pretransform_context, 1.0e-10, 1.0e-10);
    if (cfg.run_reference && cfg.run_step2_pretransform_optprep && cfg.check)
        step2_pretransform_optprep_cmp = compare_solution(
            &problem, problem.dq_ref,
            problem.dq_step2_pretransform_optprep, 1.0e-10, 1.0e-10);
    if (cfg.run_step2_pretransform &&
        cfg.run_step2_pretransform_optprep && cfg.check)
        step2_pretransform_optprep_vs_pretransform_cmp = compare_solution(
            &problem, problem.dq_step2_pretransform,
            problem.dq_step2_pretransform_optprep, 1.0e-10, 1.0e-10);
    if (cfg.run_reference &&
        cfg.run_step2_pretransform_optprep_context && cfg.check)
        step2_pretransform_optprep_context_cmp = compare_solution(
            &problem, problem.dq_ref,
            problem.dq_step2_pretransform_optprep_context,
            1.0e-10, 1.0e-10);
    if (cfg.run_step2_pretransform_context &&
        cfg.run_step2_pretransform_optprep_context && cfg.check)
        step2_pretransform_optprep_context_vs_context_cmp = compare_solution(
            &problem, problem.dq_step2_pretransform_context,
            problem.dq_step2_pretransform_optprep_context,
            1.0e-10, 1.0e-10);

    if (cfg.check && raw_reference_valid) {
        if (cfg.run_step2_trsv5_raw)
            step2_trsv5_raw_cmp = compare_solution(
                &problem, problem.dq_raw_ref, problem.dq_trsv5_raw,
                1.0e-10, 1.0e-10);
        if (cfg.run_step2_trsv5_raw_context)
            step2_trsv5_raw_context_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_trsv5_raw_context, 1.0e-10, 1.0e-10);
        if (cfg.run_step2_pretransform_raw)
            step2_pretransform_raw_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_pretransform_raw, 1.0e-10, 1.0e-10);
        if (cfg.run_step2_pretransform_raw_context)
            step2_pretransform_raw_context_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_pretransform_raw_context, 1.0e-10, 1.0e-10);
        if (cfg.run_step2_pretransform_raw_optprep)
            step2_pretransform_raw_optprep_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_pretransform_raw_optprep, 1.0e-10, 1.0e-10);
        if (cfg.run_step2_pretransform_raw_optprep_context)
            step2_pretransform_raw_optprep_context_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_pretransform_raw_optprep_context,
                1.0e-10, 1.0e-10);
        if (cfg.run_step2_pretransform_raw_stream)
            step2_pretransform_raw_stream_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                problem.dq_pretransform_raw_stream, 1.0e-10, 1.0e-10);
        if (auto_selection_ran)
            auto_selected_cmp = compare_solution(
                &problem, problem.dq_raw_ref,
                auto_selected_pre ? problem.dq_pretransform_raw_optprep :
                                    problem.dq_trsv5_raw,
                1.0e-10, 1.0e-10);
    }

    int config_match = patha_default_config_match(&cfg);
    print_report(&cfg, &ref_stats, &step1_stats, &step2_stats,
                 &step2_core_stats, &step2_core_forwarded_stats,
                 &step2_core_forwarded_context_stats,
                 &step2_core_forwarded_linebuf_stats,
                 &step1_cmp, &step2_cmp,
                 &step2_vs_step1_cmp, &step2_core_cmp,
                 &step2_core_vs_step2_cmp, &step2_core_forwarded_cmp,
                 &step2_core_forwarded_vs_step2_core_cmp,
                 &step2_core_forwarded_context_cmp,
                 &step2_core_forwarded_context_vs_forwarded_cmp,
                 &step2_core_forwarded_context_vs_core_cmp,
                 &step2_core_forwarded_linebuf_cmp,
                 &step2_core_forwarded_linebuf_vs_context_cmp,
                 &step2_core_forwarded_linebuf_vs_forwarded_cmp,
                 &step2_core_forwarded_linebuf_vs_core_cmp,
                 counts_ok, config_match);
    if (cfg.run_step2_pretransform) {
        print_mode_stats("lusgs.step2_pretransform",
                         &step2_pretransform_stats);
        print_pretransform_mode("lusgs.step2_pretransform", &cfg,
                                &step2_pretransform_stats,
                                &step2_pretransform_check,
                                &step2_core_forwarded_stats,
                                &step2_core_forwarded_context_stats);
        print_compare("lusgs.step2_pretransform_vs_reference",
                      &step2_pretransform_cmp);
        print_compare("lusgs.step2_pretransform_vs_step2_core_forwarded",
                      &step2_pretransform_vs_forwarded_cmp);
        print_compare(
            "lusgs.step2_pretransform_vs_step2_core_forwarded_context",
            &step2_pretransform_vs_context_cmp);
    }
    if (cfg.run_step2_pretransform_context) {
        print_mode_stats("lusgs.step2_pretransform_context",
                         &step2_pretransform_context_stats);
        print_pretransform_mode("lusgs.step2_pretransform_context", &cfg,
                                &step2_pretransform_context_stats,
                                &step2_pretransform_context_check,
                                &step2_core_forwarded_stats,
                                &step2_core_forwarded_context_stats);
        print_compare("lusgs.step2_pretransform_context_vs_reference",
                      &step2_pretransform_context_cmp);
        print_compare(
            "lusgs.step2_pretransform_context_vs_step2_pretransform",
            &step2_pretransform_context_vs_pretransform_cmp);
    }
    if (cfg.run_step2_pretransform_optprep) {
        print_mode_stats("lusgs.step2_pretransform_optprep",
                         &step2_pretransform_optprep_stats);
        print_pretransform_mode("lusgs.step2_pretransform_optprep", &cfg,
                                &step2_pretransform_optprep_stats,
                                &step2_pretransform_optprep_check,
                                &step2_core_forwarded_stats,
                                &step2_core_forwarded_context_stats);
        print_compare("lusgs.step2_pretransform_optprep_vs_reference",
                      &step2_pretransform_optprep_cmp);
        print_compare(
            "lusgs.step2_pretransform_optprep_vs_step2_pretransform",
            &step2_pretransform_optprep_vs_pretransform_cmp);
    }
    if (cfg.run_step2_pretransform_optprep_context) {
        print_mode_stats("lusgs.step2_pretransform_optprep_context",
                         &step2_pretransform_optprep_context_stats);
        print_pretransform_mode(
            "lusgs.step2_pretransform_optprep_context", &cfg,
            &step2_pretransform_optprep_context_stats,
            &step2_pretransform_optprep_context_check,
            &step2_core_forwarded_stats,
            &step2_core_forwarded_context_stats);
        print_compare(
            "lusgs.step2_pretransform_optprep_context_vs_reference",
            &step2_pretransform_optprep_context_cmp);
        print_compare(
            "lusgs.step2_pretransform_optprep_context_vs_step2_pretransform_context",
            &step2_pretransform_optprep_context_vs_context_cmp);
    }
    if (cfg.run_step2_trsv5_raw)
        print_raw_mode("lusgs.step2_trsv5_raw", &cfg,
                       &step2_trsv5_raw_stats, &step2_trsv5_raw_cmp, NULL);
    if (cfg.run_step2_trsv5_raw_context)
        print_raw_mode("lusgs.step2_trsv5_raw_context", &cfg,
                       &step2_trsv5_raw_context_stats,
                       &step2_trsv5_raw_context_cmp, NULL);
    if (cfg.run_step2_pretransform_raw)
        print_raw_mode("lusgs.step2_pretransform_raw", &cfg,
                       &step2_pretransform_raw_stats,
                       &step2_pretransform_raw_cmp,
                       &step2_pretransform_raw_check);
    if (cfg.run_step2_pretransform_raw_context)
        print_raw_mode("lusgs.step2_pretransform_raw_context", &cfg,
                       &step2_pretransform_raw_context_stats,
                       &step2_pretransform_raw_context_cmp,
                       &step2_pretransform_raw_context_check);
    if (cfg.run_step2_pretransform_raw_optprep)
        print_raw_mode("lusgs.step2_pretransform_raw_optprep", &cfg,
                       &step2_pretransform_raw_optprep_stats,
                       &step2_pretransform_raw_optprep_cmp,
                       &step2_pretransform_raw_optprep_check);
    if (cfg.run_step2_pretransform_raw_optprep_context)
        print_raw_mode("lusgs.step2_pretransform_raw_optprep_context", &cfg,
                       &step2_pretransform_raw_optprep_context_stats,
                       &step2_pretransform_raw_optprep_context_cmp,
                       &step2_pretransform_raw_optprep_context_check);
    if (cfg.run_step2_pretransform_raw_stream)
        print_raw_mode("lusgs.step2_pretransform_raw_stream", &cfg,
                       &step2_pretransform_raw_stream_stats,
                       &step2_pretransform_raw_stream_cmp,
                       &step2_pretransform_raw_stream_check);
    if (auto_selection_ran) {
        print_raw_mode("lusgs.auto_selected", &cfg, &auto_selected_stats,
                       &auto_selected_cmp,
                       auto_selected_pre ? &auto_selected_check : NULL);
        printf("lusgs.autoSelectedTrsv = %d\n", !auto_selected_pre);
        printf("lusgs.autoSelectedPre = %d\n", auto_selected_pre);
        printf("lusgs.autoBreakEvenSweeps = %lu\n",
               (unsigned long)cfg.estimated_break_even_sweeps);
        printf("lusgs.autoExpectedSweeps = %lu\n",
               (unsigned long)cfg.lusgs_expected_sweeps);
        printf("lusgs.autoSelectionReason = %s\n",
               auto_selected_pre ? "expected-sweeps-at-or-above-break-even" :
                                   "expected-sweeps-below-break-even");
        printf("%s\n", auto_selected_pre ? "AUTO_SELECT_PRE" :
                                            "AUTO_SELECT_TRSV");
    }
    print_trace(&problem, &cfg);

    int pass = counts_ok && cancel_test_pass;
    if (cfg.run_reference && cfg.run_step1 && cfg.check)
        pass &= (step1_cmp.mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2 && cfg.check)
        pass &= (step2_cmp.mismatch_count == 0);
    if (cfg.run_step1 && cfg.run_step2 && cfg.check)
        pass &= (step2_vs_step1_cmp.mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2_core && cfg.check)
        pass &= (step2_core_cmp.mismatch_count == 0);
    if (cfg.run_step2 && cfg.run_step2_core && cfg.check)
        pass &= (step2_core_vs_step2_cmp.mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2_core_forwarded && cfg.check)
        pass &= (step2_core_forwarded_cmp.mismatch_count == 0);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded && cfg.check)
        pass &= (step2_core_forwarded_vs_step2_core_cmp.mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2_core_forwarded_context &&
        cfg.check)
        pass &= (step2_core_forwarded_context_cmp.mismatch_count == 0);
    if (cfg.run_step2_core_forwarded &&
        cfg.run_step2_core_forwarded_context && cfg.check)
        pass &= (step2_core_forwarded_context_vs_forwarded_cmp
                     .mismatch_count == 0);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded_context &&
        cfg.check)
        pass &= (step2_core_forwarded_context_vs_core_cmp.mismatch_count ==
                 0);
    if (cfg.run_reference && cfg.run_step2_core_forwarded_linebuf &&
        cfg.check)
        pass &= (step2_core_forwarded_linebuf_cmp.mismatch_count == 0);
    if (cfg.run_step2_core_forwarded_context &&
        cfg.run_step2_core_forwarded_linebuf && cfg.check)
        pass &= (step2_core_forwarded_linebuf_vs_context_cmp
                     .mismatch_count == 0);
    if (cfg.run_step2_core_forwarded &&
        cfg.run_step2_core_forwarded_linebuf && cfg.check)
        pass &= (step2_core_forwarded_linebuf_vs_forwarded_cmp
                     .mismatch_count == 0);
    if (cfg.run_step2_core && cfg.run_step2_core_forwarded_linebuf &&
        cfg.check)
        pass &= (step2_core_forwarded_linebuf_vs_core_cmp.mismatch_count ==
                 0);
    if (cfg.run_reference && cfg.run_step2_pretransform && cfg.check)
        pass &= (step2_pretransform_cmp.mismatch_count == 0);
    if (cfg.run_step2_core_forwarded && cfg.run_step2_pretransform && cfg.check)
        pass &= (step2_pretransform_vs_forwarded_cmp.mismatch_count == 0);
    if (cfg.run_step2_core_forwarded_context &&
        cfg.run_step2_pretransform && cfg.check)
        pass &= (step2_pretransform_vs_context_cmp.mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2_pretransform_context && cfg.check)
        pass &= (step2_pretransform_context_cmp.mismatch_count == 0);
    if (cfg.run_step2_pretransform && cfg.run_step2_pretransform_context &&
        cfg.check)
        pass &= (step2_pretransform_context_vs_pretransform_cmp
                     .mismatch_count == 0);
    if (cfg.run_reference && cfg.run_step2_pretransform_optprep && cfg.check)
        pass &= (step2_pretransform_optprep_cmp.mismatch_count == 0);
    if (cfg.run_step2_pretransform &&
        cfg.run_step2_pretransform_optprep && cfg.check)
        pass &= (step2_pretransform_optprep_vs_pretransform_cmp
                     .mismatch_count == 0);
    if (cfg.run_reference &&
        cfg.run_step2_pretransform_optprep_context && cfg.check)
        pass &= (step2_pretransform_optprep_context_cmp.mismatch_count == 0);
    if (cfg.run_step2_pretransform_context &&
        cfg.run_step2_pretransform_optprep_context && cfg.check)
        pass &= (step2_pretransform_optprep_context_vs_context_cmp
                     .mismatch_count == 0);
    if (any_raw && cfg.check)
        pass &= raw_reference_valid;
    if (cfg.run_step2_trsv5_raw && cfg.check)
        pass &= step2_trsv5_raw_cmp.mismatch_count == 0;
    if (cfg.run_step2_trsv5_raw_context && cfg.check)
        pass &= step2_trsv5_raw_context_cmp.mismatch_count == 0;
    if (cfg.run_step2_pretransform_raw && cfg.check)
        pass &= step2_pretransform_raw_cmp.mismatch_count == 0;
    if (cfg.run_step2_pretransform_raw_context && cfg.check)
        pass &= step2_pretransform_raw_context_cmp.mismatch_count == 0;
    if (cfg.run_step2_pretransform_raw_optprep && cfg.check)
        pass &= step2_pretransform_raw_optprep_cmp.mismatch_count == 0;
    if (cfg.run_step2_pretransform_raw_optprep_context && cfg.check)
        pass &= step2_pretransform_raw_optprep_context_cmp.mismatch_count == 0;
    if (cfg.run_step2_pretransform_raw_stream && cfg.check)
        pass &= step2_pretransform_raw_stream_valid &&
                step2_pretransform_raw_stream_cmp.mismatch_count == 0;
    if (auto_selection_ran && cfg.check)
        pass &= auto_selected_valid && auto_selected_cmp.mismatch_count == 0;
    if (cfg.run_step2_core)
        printf("%s\n", pass ? "LUSGS_STEP2_CORE_PASS" :
                               "LUSGS_STEP2_CORE_FAIL");
    if (cfg.run_step2_core_forwarded)
        printf("%s\n", pass ? "LUSGS_STEP2_CORE_FORWARDED_PASS" :
                               "LUSGS_STEP2_CORE_FORWARDED_FAIL");
    if (cfg.run_step2_core_forwarded_context)
        printf("%s\n", pass ?
               "LUSGS_STEP2_CORE_FORWARDED_CONTEXT_PASS" :
               "LUSGS_STEP2_CORE_FORWARDED_CONTEXT_FAIL");
    if (cfg.run_step2_core_forwarded_linebuf)
        printf("%s\n", pass ?
               "LUSGS_STEP2_CORE_FORWARDED_LINEBUF_PASS" :
               "LUSGS_STEP2_CORE_FORWARDED_LINEBUF_FAIL");
    if (cfg.run_step2_pretransform)
        printf("%s\n", pass ? "LUSGS_STEP2_PRETRANSFORM_PASS" :
                               "LUSGS_STEP2_PRETRANSFORM_FAIL");
    if (cfg.run_step2_pretransform_context)
        printf("%s\n", pass ? "LUSGS_STEP2_PRETRANSFORM_CONTEXT_PASS" :
                               "LUSGS_STEP2_PRETRANSFORM_CONTEXT_FAIL");
    if (cfg.run_step2_pretransform_optprep)
        printf("%s\n", pass ? "LUSGS_STEP2_PRETRANSFORM_OPTPREP_PASS" :
                               "LUSGS_STEP2_PRETRANSFORM_OPTPREP_FAIL");
    if (cfg.run_step2_pretransform_optprep_context)
        printf("%s\n", pass ?
               "LUSGS_STEP2_PRETRANSFORM_OPTPREP_CONTEXT_PASS" :
               "LUSGS_STEP2_PRETRANSFORM_OPTPREP_CONTEXT_FAIL");
    if (cfg.run_step2_trsv5_raw)
        printf("%s\n", pass ? "LUSGS_STEP2_TRSV5_RAW_PASS" :
                               "LUSGS_STEP2_TRSV5_RAW_FAIL");
    if (cfg.run_step2_trsv5_raw_context)
        printf("%s\n", pass ? "LUSGS_STEP2_TRSV5_RAW_CONTEXT_PASS" :
                               "LUSGS_STEP2_TRSV5_RAW_CONTEXT_FAIL");
    if (cfg.run_step2_pretransform_raw)
        printf("%s\n", pass ? "LUSGS_STEP2_PRETRANSFORM_RAW_PASS" :
                               "LUSGS_STEP2_PRETRANSFORM_RAW_FAIL");
    if (cfg.run_step2_pretransform_raw_context)
        printf("%s\n", pass ?
               "LUSGS_STEP2_PRETRANSFORM_RAW_CONTEXT_PASS" :
               "LUSGS_STEP2_PRETRANSFORM_RAW_CONTEXT_FAIL");
    if (cfg.run_step2_pretransform_raw_optprep)
        printf("%s\n", pass ?
               "LUSGS_STEP2_PRETRANSFORM_RAW_OPTPREP_PASS" :
               "LUSGS_STEP2_PRETRANSFORM_RAW_OPTPREP_FAIL");
    if (cfg.run_step2_pretransform_raw_optprep_context)
        printf("%s\n", pass ?
               "LUSGS_STEP2_PRETRANSFORM_RAW_OPTPREP_CONTEXT_PASS" :
               "LUSGS_STEP2_PRETRANSFORM_RAW_OPTPREP_CONTEXT_FAIL");
    if (cfg.run_step2_pretransform_raw_stream)
        printf("%s\n", pass ? "LUSGS_STEP2_STREAMING_PASS" :
                               "LUSGS_STEP2_STREAMING_FAIL");
    if (auto_selection_ran)
        printf("%s\n", pass ? "LUSGS_STEP2_AUTO_PASS" :
                               "LUSGS_STEP2_AUTO_FAIL");
    printf("%s\n", pass ? "LUSGS_STEP2_PASS" : "LUSGS_STEP2_FAIL");

    free_problem(&problem);
    return pass ? 0 : 1;
}
