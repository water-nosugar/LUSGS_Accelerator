#!/usr/bin/env python3
"""
run_cfd_dsa.py -- gem5 configuration script for CFD DSA simulation
================================================================
Usage:
  ./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_dsa.py \
      --binary=build/cfd_dsa/aarch64/test_cfd_extreme_arm

Options:
  --binary=<path>     test binary (default: test_cfd_extreme_arm)
  --max-ticks=<n>     max simulation ticks (default: 1e12)
  --cpu=<type>         cpu type: timing (default), o3, minor
  --stats=<file>       parse existing stats file instead of running
  --warmup-iters=<n>   warmup iterations (default: 100000)
  --bench-iters=<n>    benchmark iterations (default: 10000000)
  --sme-pipe-lat=<n>    Path B ZA spatial pipeline step latency (default: 1)
  --sme-pipe-count=<n>  Path B ZA spatial pipeline engines (default: 1)
  --sme-pipe-pipelined=<true|false>
  --sme-outer-lat=<n>   Path C selected-column outer step latency (default: 1)
  --sme-outer-count=<n> Path C selected-column outer engines (default: 1)
  --sme-outer-pipelined=<true|false>
  --sme-outer-full-array=<true|false> optional future full-array label (default: false)
  --spm-latency=<lat>   SPM SimpleMemory latency (default: 1ns)
  --spm-bandwidth=<bw>  SPM SimpleMemory bandwidth (default: 64GB/s)
  --spm-access-mode=<generic|local|local-ideal|local-realistic|local-event>
  --lmat-lat=<n>        lmat5_spm FU issue latency (default: 1)
  --readport-count=<n>  ReadPort FU count (default: 8)
  --local-spm-lat=<n>   local SPM read latency in cycles (default: 2)
  --local-spm-read-ports=<n> local SPM read ports (default: 1)
  --local-spm-read-width=<n> local SPM physical read width in bytes (default: 64; compact40 uses 40)
  --local-spm-outstanding=<n> local SPM outstanding reads (default: 4)
  --local-spm-queue-size=<n> local SPM request queue slots (default: 4)
  --local-spm-banks=<n> local SPM banks (default: 4)
  --local-spm-bank-granularity=<n> local SPM bank mapping bytes (default: 64)
  --local-spm-bank-mapping=<addr|row|xor> local SPM bank mapping (default: addr)
  --local-spm-bank-conflict=<true|false>
  --local-spm-z-wb-ports=<n> local SPM Z writeback ports (default: 1)
  --local-spm-real-backpressure=<true|false>
  --za-state-mode=<simple|pending|shadow|rename>
  --patha-spm-mode=<direct|local-event-report>
  --patha-spm-read-width=<n> Path A physical read width, >=40 (default: 40)
  --patha-store-mode=<full64|pred40> Path A result store mode (default: pred40)
  --patha-store-stride=<40|64> Path A result layout stride
  --patha-acc-mode=<internal-buffer|xregs>
  --patha-result-buffer-depth=<1|2|4>
  --patha-dotp-lat=<n>
  --patha-dotp-count=<n>
  --patha-pack-count=<n>
  --patha-unroll=<2|4|8>
  --patha-streaming=<0|1>
  --patha-kernel=<baseline|stream-scheduled|stream-dsa>
  --patha-input-buffer-depth=<2|3|4>
  --patha-stream-read-ports=<1|2>
  --patha-store-queue-depth=<2|4|8>
  --patha-async-store=<0|1>
  --patha-matld-readports=<n>
  --trsv5-count=<n>     TRSV5 FU count (default: 1)
  --trsv5-lat=<n>       TRSV5 FU coarse result latency (default: 60)
  --trsv5-mode=<mode>   TRSV5 model label (default: zreg-coarse)
  --trsv5-trace-enable  Enable benchmark-level TRSV5 trace
  --trsv5-trace-solves=<n> TRSV5 trace solve budget
  --trsv5-trace-file=<path> TRSV5 trace output path
  --trsm5-mrhs-enable=<0|1> Enable LU-SGS coefficient preprocessing
  --trsm5-mrhs-lat=<n> TRSM5 multi-RHS coarse latency (default: 100)
  --trsm5-mrhs-count=<n> TRSM5 multi-RHS FU count (default: 1)
  --trsm5-mrhs-dual-enable=<0|1> Enable one-LU-read INV/LBAR preprocessing
  --trsm5-mrhs-dual-lat=<n> INV/LBAR dual-batch coarse latency (default: 200)
  --trsm5-mrhs-dual-count=<n> INV/LBAR dual-batch FU count (default: 1)
  --pretransform-output-buffer-enable=<0|1>
  --pretransform-output-buffer-depth=<1..8>
  --pretransform-spm-slots=<1|2>
  --pretransform-use-barrier=<0|1>
  --pretransform-validate=<full|fast>
  --lusgs-pretransform-auto=<0|1>
  --lusgs-expected-sweeps=<n>
  --lusgs-preprocess-auto=<0|1> alias for pretransform-auto
  --lusgs-coeff-expected-sweeps=<n> alias for expected-sweeps
  --vec5-lat=<n> Step2 pretransform register Vector5 latency (default: 2)
  --lusgs-lines=<n>       LU-SGS Step1 line count (default: 1)
  --lusgs-cells=<n>       LU-SGS Step1 cells per line (default: 17)
  --lusgs-interleave=<n>  LU-SGS Step1 independent-line interleave (default: 1)
  --lusgs-mode=<mode>     LU-SGS Step2 mode, including step2-pretransform[-context]
  --lusgs-linebuf-enable=<0|1> Enable Step2 line-buffer forwarding mode support
  --lusgs-linebuf-entries=<n> Step2 line-buffer entries (default: 16)
  --lusgs-reference-only  Run only the software LU-SGS reference
  --lusgs-patha-only      Run only the Path A LU-SGS implementation
  --lusgs-check=<0|1>     Compare Path A LU-SGS against the reference
  --lusgs-trace-enable    Print a small LU-SGS solution trace
  --lusgs-trace-lines=<n> Trace line count (default: 1)
  --lusgs-trace-cells=<n> Trace cell count (default: 4)
  --cfd-trace-enable      enable a small CFD micro-trace
  --cfd-trace-matvecs=<n> trace budget in matvecs (default: 4)
  --cfd-trace-file=<path> output path for the CFD micro-trace
  --mova-lat=<n>        official SME MOVA FU latency (default: 1)
  --iq-entries=<n>      O3 issue queue entries (default: 64)
  --rob-entries=<n>     O3 ROB entries (default: 160)
================================================================
"""
import m5
from m5.objects import *
from m5.objects.IQUnit import IQUnit
import os, sys, getopt, re

sys.dont_write_bytecode = True
config_dir = os.path.dirname(os.path.abspath(__file__))
if config_dir not in sys.path:
    sys.path.insert(0, config_dir)
from _paths import binary, result

report_focus = os.environ.get('GEM5_CFD_REPORT_FOCUS', 'auto').lower()
if report_focus not in ('auto', 'patha', 'pathb', 'pathc'):
    report_focus = 'auto'
invoked_by_split_wrapper = os.environ.get('GEM5_CFD_RUN_WRAPPER', '') != ''

binary_path = binary('test_cfd_extreme_arm')
max_ticks   = int(1e12)
cpu_type    = 'o3'
stats_file  = None
warmup_iters = 100000
bench_iters  = 10000000
bench_iters_user_set = False
# Defaults used only when parsing archived fixed-ZA FMOPA statistics.
sme_fmopa_lat = 1
sme_fmopa_array_lat = 4
sme_fmopa_count = 1
sme_pipe_lat = 1
sme_pipe_count = 1
sme_pipe_pipelined = True
sme_outer_lat = 1
sme_outer_count = 1
sme_outer_pipelined = True
sme_outer_full_array = False
spm_latency = '1ns'
spm_bandwidth = '64GB/s'
spm_access_mode = 'generic'
lmat_lat = 1
lmat_pipelined = True
readport_count = 8
local_spm_lat = 2
local_spm_read_ports = 1
local_spm_read_width = 64
local_spm_outstanding = 4
local_spm_queue_size = 4
local_spm_banks = 4
local_spm_bank_granularity = 64
local_spm_bank_mapping = 'addr'
local_spm_bank_conflict = True
local_spm_bypass_lsq = True
local_spm_z_wb_ports = 1
local_spm_real_backpressure = False
za_state_mode = 'simple'
patha_spm_mode = 'direct'
patha_spm_read_width = 40
patha_store_mode = 'pred40'
patha_store_stride = None
patha_acc_mode = 'internal-buffer'
patha_result_buffer_depth = 2
patha_dotp_lat = 7
patha_dotp_count = 1
patha_pack_count = 1
patha_pack_lat = 1
patha_unroll = 8
patha_streaming = False
patha_kernel = 'baseline'
patha_input_buffer_depth = 2
patha_stream_read_ports = 1
patha_store_queue_depth = 4
patha_async_store = False
patha_matld_readports = None
trsv5_count = 1
trsv5_lat = 60
trsv5_mode = 'zreg-coarse'
trsv5_trace_enable = False
trsv5_trace_solves = 16
trsv5_trace_file = result('trsv5_trace.csv')
trsm5_mrhs_enable = True
trsm5_mrhs_lat = 100
trsm5_mrhs_count = 1
trsm5_mrhs_dual_enable = True
trsm5_mrhs_dual_lat = 200
trsm5_mrhs_dual_count = 1
trsm5_coeff3_enable = True
trsm5_coeff3_lat = 300
trsm5_coeff3_count = 1
pretransform_vec5_lat = 2
pretransform_fallback_trsv5 = True
pretransform_diag_epsilon = 1.0e-12
pretransform_per_sweep = False
pretransform_output_buffer_enable = True
pretransform_output_buffer_depth = 4
pretransform_spm_slots = 2
pretransform_overlap_enable = True
pretransform_use_barrier = False
pretransform_validate = 'full'
coeff_preprocess_spm_slots = 2
coeff_output_buffer_depth = 4
coefficient_update_interval = 0
coeff_preprocess_model = 'coarse'
lu5_model = 'software'
coeff_validation = 'full'
coeff_validation_sample_rate = 16
coeff_input_slots = 2
lu5_count = 1
lu5_pending_depth = 2
lu5_div_count = 1
lu5_div_lat = 12
lu5_div_ii = 12
lu5_mulsub_count = 1
lu5_mul_lat = 3
lu5_sub_lat = 4
coeff3_event_count = 1
coeff3_pending_depth = 2
coeff3_rhs_lanes = 15
coeff3_schedule = 'round-robin-ready'
coeff3_div_count = 1
coeff3_div_lat = 12
coeff3_div_ii = 4
coeff3_div_mode = 'divide'
coeff_mulsub_count = 1
coeff_mul_lat = 3
coeff_sub_lat = 4
coeff3_partial_output = True
lu_forwarding = True
lu_solve_early_start = True
coeff_spm_read_ports = 1
coeff_spm_write_ports = 1
coeff_spm_write_width = 40
coeff_spm_model = 'internal'
coeff_spm_layout = 'legacy'
coeff_spm_outstanding = 4
coeff_spm_banks = 4
coeff_spm_bank_granularity = 64
coeff_source_read_lat = 2
coeff_spm_write_lat = 1
coeff_drain_width = 40
coeff_drain_ports = 1
coeff_drain_lat = 1
coeff_drain_outstanding = 4
coeff_drain_queue_depth = 4
coeff_preprocess_trace_enable = False
coeff_preprocess_trace_cells = 4
coeff_preprocess_trace_file = result('coeff_preprocess_trace.csv')
coeff_cancel_test = False
coeff_cache_contract_test = False
coeff_streaming_enable = False
coeff_stream_line_autonomous = False
coeff_base_solve = 'inverse'
coeff_compute_model = 'legacy'
coeff_line_storage = 'full'
coeff_line_opt_level = 0
coeff_rhs_row_parallel = False
coeff_completion_depth = 8
coeff_warm_write_depth = 8
coeff_onchip_timing = None
coeff_onchip_latency = 1
coeff_onchip_depth = 64
coeff_line_tile_cells = 16
coeff_line_cache_cells = 0
coeff_line_tile_buffers = 1
coeff_line_tile_dma_width = 40
coeff_line_cache_allowed = True
coeff_line_spm_bytes = 131072
coeff_interface_timing = False
coeff_interface_width = 64
coeff_cluster_mul_lat = 3
coeff_cluster_add_lat = 4
coeff_cluster_div_lat = 12
coeff_cluster_div_ii = 4
coeff_cluster_product_depth = 8
coeff_line_base_ahead = True
coeff_line_early_backward = True
coeff_line_mvm_model = 'serial'
coeff_resource_stats = False
coeff_line_mul_lat = 3
coeff_line_add_lat = 4
coeff_line_product_depth = 5
coeff_stream_window = 8
coeff_stream_retire_mode = 'auto'
coeff_stream_boundary_mask = True
coeff_stream_dinv_consumer = 'off'
coeff_stream_lbar_consumer = 'off'
coeff_stream_ubar_consumer = 'off'
coeff_column_fma_latency = 4
coeff_column_fma_ii = 1
coeff_column_fma_count = 1
coeff_column_fma_queue_depth = 5
forward_combine_latency = 1
forward_combine_ii = 1
forward_combine_count = 1
forward_combine_queue_depth = 2
ubar_inplace = False
lusgs_pretransform_auto = False
lusgs_expected_sweeps = 1
lusgs_lines = 1
lusgs_cells = 17
lusgs_interleave = 1
lusgs_mode = 'all'
lusgs_linebuf_enable = True
lusgs_linebuf_entries = 16
lusgs_reference_only = False
lusgs_patha_only = False
lusgs_check = True
lusgs_trace_enable = False
lusgs_trace_lines = 1
lusgs_trace_cells = 4
lusgs_step3_stage = 'A'
lusgs_contexts = 1
lusgs_tile_cells = 1
lusgs_update_q = False
lusgs_omega = 1.0
lusgs_controller_count = 1
lusgs_vec5_count = 1
lusgs_vec5_lat = 4
lusgs_controller_trace_enable = False
lusgs_controller_trace_lines = 1
lusgs_controller_trace_cells = 4
lusgs_controller_trace_file = result('lusgs_controller_trace.csv')
lusgs_step4_enable = False
lusgs_event_trace_enable = False
lusgs_event_trace_lines = 1
lusgs_event_trace_cells = 4
lusgs_event_trace_file = result('lusgs_event_trace.csv')
lusgs_step4_watchdog_cycles = 0
lusgs_step4_patha_real = False
lusgs_step4_stage = None
lusgs_step4_trsv_real = False
lusgs_step4_vec5_real = False
trsv5_event_count = 1
trsv5_div_count = 1
trsv5_fma_count = 1
trsv5_div_lat = 4
trsv5_fma_lat = 3
trsv5_load_lat = 2
trsv5_result_lat = 1
trsv5_forwarding = True
trsv5_queue_depth = 2
vec5_event_count = 1
vec5_lanes = 5
vec5_copy_lat = 1
vec5_sub_lat = 3
vec5_axpy_lat = 4
vec5_initiation_interval = 1
vec5_queue_depth = 2
lusgs_step4_engine_backpressure_test = False
dram_size = '16MB'
lusgs_step4_snapshot_test = False
lusgs_step4_lifecycle_test = False
lusgs_step4_wrong_path_launch = False
lusgs_step4_wrong_path_wait = False
lusgs_step4_overlap_test = False
lusgs_step4_overlap_iters = 100000
lusgs_error_case = ''
cfd_trace_enable = False
cfd_trace_matvecs = 4
cfd_trace_file = result('cfd_micro_trace.csv')
mova_lat = 1
iq_entries = 64
rob_entries = 160

def parse_bool(value):
    value = value.lower()
    if value in ('1', 'true', 'yes', 'on'):
        return True
    if value in ('0', 'false', 'no', 'off'):
        return False
    raise ValueError(value)

try:
    opts, args = getopt.getopt(sys.argv[1:],
                               'hb:m:c:s:w:',
                                ['help', 'binary=', 'max-ticks=', 'cpu=', 'stats=',
                                'warmup-iters=', 'bench-iters=',
                                'sme-pipe-lat=', 'sme-pipe-count=',
                                'sme-pipe-pipelined=',
                                'sme-outer-lat=', 'sme-outer-count=',
                                'sme-outer-pipelined=',
                                'sme-outer-full-array=',
                                'spm-latency=', 'spm-bandwidth=',
                                'spm-access-mode=',
                                'lmat-lat=', 'readport-count=', 'mova-lat=',
                                'local-spm-lat=', 'local-spm-read-ports=',
                                'local-spm-read-width=',
                                'local-spm-outstanding=',
                                'local-spm-queue-size=',
                                'local-spm-banks=',
                                'local-spm-bank-granularity=',
                                'local-spm-bank-mapping=',
                                'local-spm-bank-conflict=',
                                'local-spm-bypass-lsq=',
                                'local-spm-z-wb-ports=',
                                'local-spm-real-backpressure=',
                                'za-state-mode=',
                                'patha-spm-mode=',
                                'patha-spm-read-width=',
                                'patha-store-mode=',
                                'patha-store-stride=',
                                'patha-acc-mode=',
                                'patha-result-buffer-depth=',
                                'patha-dotp-lat=',
                                'patha-dotp-count=',
                                'patha-pack-count=',
                                'patha-pack-lat=',
                                'patha-matld-count=',
                                'patha-matld-lat=',
                                'patha-unroll=',
                                'patha-streaming=',
                                'patha-kernel=',
                                'patha-input-buffer-depth=',
                                'patha-stream-read-ports=',
                                'patha-store-queue-depth=',
                                'patha-async-store=',
                                'patha-matld-readports=',
                                'trsv5-count=',
                                'trsv5-lat=',
                                'trsv5-mode=',
                                'trsv5-trace-enable',
                                'trsv5-trace-solves=',
                                'trsv5-trace-file=',
                                'trsm5-mrhs-enable=',
                                'trsm5-mrhs-lat=',
                                'trsm5-mrhs-count=',
                                'trsm5-mrhs-dual-enable=',
                                'trsm5-mrhs-dual-lat=',
                                'trsm5-mrhs-dual-count=',
                                'trsm5-coeff3-enable=',
                                'trsm5-coeff3-lat=',
                                'trsm5-coeff3-count=',
                                'vec5-lat=',
                                'pretransform-fallback-trsv5=',
                                'pretransform-diag-epsilon=',
                                'pretransform-per-sweep=',
                                'pretransform-output-buffer-enable=',
                                'pretransform-output-buffer-depth=',
                                'pretransform-spm-slots=',
                                'pretransform-overlap-enable=',
                                'pretransform-use-barrier=',
                                'pretransform-validate=',
                                'coeff-preprocess-spm-slots=',
                                'coeff-output-buffer-depth=',
                                'coefficient-update-interval=',
                                'coeff-preprocess-model=',
                                'lu5-model=',
                                'coeff-validation=',
                                'coeff-validation-sample-rate=',
                                'coeff-input-slots=',
                                'lu5-count=',
                                'lu5-pending-depth=',
                                'lu5-div-count=',
                                'lu5-div-lat=',
                                'lu5-div-ii=',
                                'lu5-mulsub-count=',
                                'lu5-mul-lat=',
                                'lu5-sub-lat=',
                                'coeff3-count=',
                                'coeff3-pending-depth=',
                                'coeff3-rhs-lanes=',
                                'coeff3-schedule=',
                                'coeff3-div-count=',
                                'coeff3-div-lat=',
                                'coeff3-div-ii=',
                                'coeff3-div-mode=',
                                'coeff-mulsub-count=',
                                'coeff-mul-lat=',
                                'coeff-sub-lat=',
                                'coeff3-partial-output=',
                                'lu-forwarding=',
                                'lu-solve-early-start=',
                                'coeff-spm-read-ports=',
                                'coeff-spm-write-ports=',
                                'coeff-spm-write-width=',
                                'coeff-spm-model=',
                                'coeff-spm-layout=',
                                'coeff-spm-outstanding=',
                                'coeff-spm-banks=',
                                'coeff-spm-bank-granularity=',
                                'coeff-source-read-lat=',
                                'coeff-spm-write-lat=',
                                'coeff-drain-width=',
                                'coeff-drain-ports=',
                                'coeff-drain-lat=',
                                'coeff-drain-outstanding=',
                                'coeff-drain-queue-depth=',
                                'coeff-preprocess-trace-enable=',
                                'coeff-preprocess-trace-cells=',
                                'coeff-preprocess-trace-file=',
                                'coeff-cancel-test=',
                                'coeff-cache-contract-test=',
                                'coeff-streaming-enable=',
                                'coeff-stream-line-autonomous=',
                                'coeff-base-solve=',
                                'coeff-compute-model=',
                                'coeff-line-storage=',
                                'coeff-line-opt-level=',
                                'coeff-rhs-row-parallel=',
                                'coeff-completion-depth=',
                                'coeff-warm-write-depth=',
                                'coeff-onchip-timing=',
                                'coeff-onchip-latency=',
                                'coeff-onchip-queue-depth=',
                                'coeff-line-tile-cells=',
                                'coeff-line-cache-cells=',
                                'coeff-line-tile-buffers=',
                                'coeff-line-tile-dma-width=',
                                'coeff-line-spm-bytes=',
                                'coeff-interface-timing=',
                                'coeff-interface-width=',
                                'coeff-cluster-mul-lat=',
                                'coeff-cluster-add-lat=',
                                'coeff-cluster-div-lat=',
                                'coeff-cluster-div-ii=',
                                'coeff-cluster-product-depth=',
                                'coeff-line-base-ahead=',
                                'coeff-line-early-backward=',
                                'coeff-line-mvm-model=',
                                'coeff-resource-stats=',
                                'coeff-line-mul-lat=',
                                'coeff-line-add-lat=',
                                'coeff-line-product-depth=',
                                'coeff-stream-window=',
                                'coeff-stream-retire-mode=',
                                'coeff-stream-boundary-mask=',
                                'coeff-stream-dinv-consumer=',
                                'coeff-stream-lbar-consumer=',
                                'coeff-stream-ubar-consumer=',
                                'coeff-column-fma-latency=',
                                'coeff-column-fma-ii=',
                                'coeff-column-fma-count=',
                                'coeff-column-fma-queue-depth=',
                                'forward-combine-latency=',
                                'forward-combine-ii=',
                                'forward-combine-count=',
                                'forward-combine-queue-depth=',
                                'ubar-inplace=',
                                'lusgs-pretransform-auto=',
                                'lusgs-expected-sweeps=',
                                'lusgs-preprocess-auto=',
                                'lusgs-coeff-expected-sweeps=',
                                'lusgs-lines=',
                                'lusgs-cells=',
                                'lusgs-interleave=',
                                'lusgs-mode=',
                                'lusgs-linebuf-enable=',
                                'lusgs-linebuf-entries=',
                                'lusgs-reference-only',
                                'lusgs-patha-only',
                                'lusgs-check=',
                                'lusgs-trace-enable',
                                'lusgs-trace-lines=',
                                'lusgs-trace-cells=',
                                'lusgs-step3-stage=',
                                'lusgs-contexts=',
                                'lusgs-tile-cells=',
                                'lusgs-update-q=',
                                'lusgs-omega=',
                                'lusgs-controller-count=',
                                'lusgs-vec5-count=',
                                'lusgs-vec5-lat=',
                                'lusgs-controller-trace-enable',
                                'lusgs-controller-trace-lines=',
                                'lusgs-controller-trace-cells=',
                                'lusgs-controller-trace-file=',
                                'lusgs-step4-enable=',
                                'lusgs-event-trace-enable',
                                'lusgs-event-trace-lines=',
                                'lusgs-event-trace-cells=',
                                'lusgs-event-trace-file=',
                                'lusgs-step4-watchdog-cycles=',
                                'lusgs-step4-patha-real=',
                                'lusgs-step4-stage=',
                                'lusgs-step4-trsv-real=',
                                'lusgs-step4-vec5-real=',
                                'trsv5-event-count=',
                                'trsv5-div-count=',
                                'trsv5-fma-count=',
                                'trsv5-div-lat=',
                                'trsv5-fma-lat=',
                                'trsv5-load-lat=',
                                'trsv5-result-lat=',
                                'trsv5-forwarding=',
                                'trsv5-queue-depth=',
                                'vec5-lanes=',
                                'vec5-count=',
                                'vec5-copy-lat=',
                                'vec5-sub-lat=',
                                'vec5-axpy-lat=',
                                'vec5-initiation-interval=',
                                'vec5-queue-depth=',
                                'dram-size=',
                                'lusgs-step4-engine-backpressure-test',
                                'lusgs-step4-snapshot-test',
                                'lusgs-step4-lifecycle-test',
                                'lusgs-step4-wrong-path-launch',
                                'lusgs-step4-wrong-path-wait',
                                'lusgs-step4-overlap-test',
                                'lusgs-step4-overlap-iters=',
                                'lusgs-error-case=',
                                'cfd-trace-enable',
                                'cfd-trace-matvecs=',
                                'cfd-trace-file=',
                                'iq-entries=', 'rob-entries='])
    for opt, arg in opts:
        if opt in ('-h', '--help'):
            print("Usage: run_cfd_dsa.py [options]")
            print("  --binary=<path>     test binary (default: test_cfd_extreme_arm)")
            print("  --max-ticks=<n>      max simulation ticks (default: 1e12)")
            print("  --cpu=<type>         cpu type: timing, o3, minor (default: o3)")
            print("  --stats=<file>       parse existing stats file instead of running")
            print("  --warmup-iters=<n>   warmup iterations (default: 100000)")
            print("  --bench-iters=<n>    benchmark iterations (default: 10000000)")
            print("  --sme-pipe-lat=<n>   Path B ZA spatial pipeline step latency (default: 1)")
            print("  --sme-pipe-count=<n> Path B ZA spatial pipeline engine count (default: 1)")
            print("  --sme-pipe-pipelined=<true|false> (default: true)")
            print("  --sme-outer-lat=<n>  Path C ZA outer selected-column step latency (default: 1)")
            print("  --sme-outer-count=<n> Path C ZA outer engines (default: 1)")
            print("  --sme-outer-pipelined=<true|false> (default: true)")
            print("  --sme-outer-full-array=<true|false> optional future full-array label (default: false)")
            print("  --spm-latency=<lat>   SPM latency, e.g. 1ns, 500ps (default: 1ns)")
            print("  --spm-bandwidth=<bw>  SPM bandwidth, e.g. 64GB/s (default: 64GB/s)")
            print("  --spm-access-mode=<generic|local|local-ideal|local-realistic|local-event> (default: generic)")
            print("  --lmat-lat=<n>        lmat5_spm FU issue latency (default: 1)")
            print("  --readport-count=<n>  ReadPort FU count (default: 8)")
            print("  --local-spm-lat=<n>   local SPM read latency cycles (default: 2)")
            print("  --local-spm-read-ports=<n> local SPM read ports (default: 1)")
            print("  --local-spm-read-width=<n> local SPM physical read width bytes (default: 64; compact40 uses 40)")
            print("  --local-spm-outstanding=<n> local SPM outstanding reads (default: 4)")
            print("  --local-spm-queue-size=<n> local SPM request queue slots (default: 4)")
            print("  --local-spm-banks=<n> local SPM banks (default: 4)")
            print("  --local-spm-bank-granularity=<n> local SPM bank mapping bytes (default: 64)")
            print("  --local-spm-bank-mapping=<addr|row|xor> local SPM bank mapping (default: addr)")
            print("  --local-spm-bank-conflict=<true|false> (default: true)")
            print("  --local-spm-z-wb-ports=<n> local Z writeback ports (default: 1)")
            print("  --local-spm-real-backpressure=<true|false> (default: false)")
            print("  --za-state-mode=<simple|pending|shadow|rename> (default: simple)")
            print("  --patha-spm-mode=<direct|local-event-report> Path A SPM model label (default: direct)")
            print("  --patha-spm-read-width=<n> Path A physical read width bytes, >=40 (default: 40)")
            print("  --patha-store-mode=<full64|pred40|store5> Path A result store mode (default: pred40)")
            print("  --patha-store-stride=<40|64> Path A result layout stride (default: 40 for pred40, 64 for full64)")
            print("  --patha-acc-mode=<internal-buffer|xregs> Path A accumulator mode (default: internal-buffer)")
            print("  --patha-result-buffer-depth=<1|2|4> Path A internal result buffer depth (default: 2)")
            print("  --patha-dotp-lat=<n> Path A CFDDSADotp FU latency (default: 7; 5/6/7 recommended sweep)")
            print("  --patha-dotp-count=<n> Path A CFDDSADotp FU count (default: 1)")
            print("  --patha-pack-count=<n> Path A CFDDSAPack FU count (default: 1)")
            print("  --patha-unroll=<2|4|8> Path A benchmark unroll label (default: 8)")
            print("  --patha-streaming=<0|1> Enable optional Path A streaming DSA dependency mode (default: 0)")
            print("  --patha-kernel=<baseline|stream-scheduled|stream-dsa> Path A benchmark kernel (default: baseline)")
            print("  --patha-input-buffer-depth=<2|3|4> Path A streaming input buffer depth (default: 2)")
            print("  --patha-stream-read-ports=<1|2> Path A streaming read ports label (default: 1)")
            print("  --patha-store-queue-depth=<2|4|8> Path A async store queue depth label (default: 4)")
            print("  --patha-async-store=<0|1> Enable optional Path A async store model label (default: 0)")
            print("  --patha-matld-readports=<n> Path A CFDDSAMatLd FU count override")
            print("  --trsv5-count=<n>    TRSV5 FU count (default: 1)")
            print("  --trsv5-lat=<n>      TRSV5 coarse O3-visible latency (default: 60)")
            print("  --trsv5-mode=<mode>  TRSV5 model label (default: zreg-coarse)")
            print("  --trsv5-trace-enable Write a benchmark-level TRSV5 trace")
            print("  --trsv5-trace-solves=<n> TRSV5 trace solve budget (default: 16)")
            print("  --trsv5-trace-file=<path> TRSV5 trace output path")
            print("  --lusgs-lines=<n>     LU-SGS Step1 line count (default: 1)")
            print("  --lusgs-cells=<n>     LU-SGS Step1 cells per line (default: 17)")
            print("  --lusgs-interleave=<n> LU-SGS independent-line interleave (default: 1)")
            print("  --lusgs-mode=<mode>    LU-SGS Step2 mode, including step2-pretransform[-context] and step2-pretransform-optprep[-context]")
            print("  --lusgs-linebuf-enable=<0|1> Enable Step2 line-buffer mode support (default: 1)")
            print("  --lusgs-linebuf-entries=<n> Step2 line-buffer entries (default: 16)")
            print("  --lusgs-reference-only Run only the software LU-SGS reference")
            print("  --lusgs-patha-only     Run only the Path A LU-SGS implementation")
            print("  --lusgs-check=<0|1>    Compare Path A LU-SGS against the reference (default: 1)")
            print("  --lusgs-trace-enable   Print a small LU-SGS solution trace")
            print("  --lusgs-trace-lines=<n> LU-SGS trace line count (default: 1)")
            print("  --lusgs-trace-cells=<n> LU-SGS trace cell count (default: 4)")
            print("  --lusgs-step3-stage=<A|B|C> LU-SGS macro-controller stage")
            print("  --lusgs-contexts=<n>  LU-SGS Step3 line contexts")
            print("  --lusgs-tile-cells=<n> LU-SGS Step3 tile cell count")
            print("  --lusgs-update-q=<0|1> Enable Step3 Q update")
            print("  --lusgs-omega=<f>     Step3 Q update relaxation factor")
            print("  --lusgs-controller-count=<n> Step3 controller FU count")
            print("  --lusgs-vec5-count=<n> Step3 Vector5 FU count")
            print("  --lusgs-vec5-lat=<n> Step3 Vector5 coarse latency")
            print("  --lusgs-controller-trace-enable Write Step3 controller CSV trace")
            print("  --lusgs-controller-trace-lines=<n> Step3 trace line budget")
            print("  --lusgs-controller-trace-cells=<n> Step3 trace cell budget")
            print("  --lusgs-controller-trace-file=<path> Step3 trace output path")
            print("  --lusgs-step4-enable=<0|1> Enable Step4 event controller")
            print("  --lusgs-event-trace-enable Write Step4 event CSV trace")
            print("  --lusgs-event-trace-lines=<n> Step4 trace line budget")
            print("  --lusgs-event-trace-cells=<n> Step4 trace cell budget")
            print("  --lusgs-event-trace-file=<path> Step4 trace output path")
            print("  --lusgs-error-case=<name> Run one Step3 descriptor/token error case")
            print("  --dram-size=<size>    DRAM capacity (default: 16MB)")
            print("  --cfd-trace-enable    Write a short CFD micro-trace")
            print("  --cfd-trace-matvecs=<n> CFD micro-trace matvec budget (default: 4)")
            print("  --cfd-trace-file=<path> CFD micro-trace output path")
            print("  --mova-lat=<n>        MatrixMov FU latency (default: 1)")
            print("  --iq-entries=<n>      O3 issue queue entries (default: 64)")
            print("  --rob-entries=<n>     O3 ROB entries (default: 160)")
            sys.exit(0)
        elif opt in ('-b', '--binary'):
            binary_path = arg
        elif opt in ('-m', '--max-ticks'):
            max_ticks = int(float(arg))
        elif opt in ('-c', '--cpu'):
            cpu_type = arg
        elif opt in ('-s', '--stats'):
            stats_file = arg
        elif opt in ('-w', '--warmup-iters'):
            warmup_iters = int(arg)
        elif opt == '--bench-iters':
            bench_iters = int(arg)
            bench_iters_user_set = True
        elif opt == '--sme-pipe-lat':
            sme_pipe_lat = int(arg)
        elif opt == '--sme-pipe-count':
            sme_pipe_count = int(arg)
        elif opt == '--sme-pipe-pipelined':
            sme_pipe_pipelined = parse_bool(arg)
        elif opt == '--sme-outer-lat':
            sme_outer_lat = int(arg)
        elif opt == '--sme-outer-count':
            sme_outer_count = int(arg)
        elif opt == '--sme-outer-pipelined':
            sme_outer_pipelined = parse_bool(arg)
        elif opt == '--sme-outer-full-array':
            sme_outer_full_array = parse_bool(arg)
        elif opt == '--spm-latency':
            spm_latency = arg
        elif opt == '--spm-bandwidth':
            spm_bandwidth = arg
        elif opt == '--spm-access-mode':
            spm_access_mode = arg
        elif opt == '--lmat-lat':
            lmat_lat = int(arg)
        elif opt == '--readport-count':
            readport_count = int(arg)
        elif opt == '--local-spm-lat':
            local_spm_lat = int(arg)
        elif opt == '--local-spm-read-ports':
            local_spm_read_ports = int(arg)
        elif opt == '--local-spm-read-width':
            local_spm_read_width = int(arg)
        elif opt == '--local-spm-outstanding':
            local_spm_outstanding = int(arg)
        elif opt == '--local-spm-queue-size':
            local_spm_queue_size = int(arg)
        elif opt == '--local-spm-banks':
            local_spm_banks = int(arg)
        elif opt == '--local-spm-bank-granularity':
            local_spm_bank_granularity = int(arg)
        elif opt == '--local-spm-bank-mapping':
            local_spm_bank_mapping = arg
        elif opt == '--local-spm-bank-conflict':
            local_spm_bank_conflict = parse_bool(arg)
        elif opt == '--local-spm-bypass-lsq':
            local_spm_bypass_lsq = parse_bool(arg)
        elif opt == '--local-spm-z-wb-ports':
            local_spm_z_wb_ports = int(arg)
        elif opt == '--local-spm-real-backpressure':
            local_spm_real_backpressure = parse_bool(arg)
        elif opt == '--za-state-mode':
            za_state_mode = arg
        elif opt == '--patha-spm-mode':
            patha_spm_mode = arg
        elif opt == '--patha-spm-read-width':
            patha_spm_read_width = int(arg)
        elif opt == '--patha-store-mode':
            patha_store_mode = arg
        elif opt == '--patha-store-stride':
            patha_store_stride = int(arg)
        elif opt == '--patha-acc-mode':
            patha_acc_mode = arg
        elif opt == '--patha-result-buffer-depth':
            patha_result_buffer_depth = int(arg)
        elif opt == '--patha-dotp-lat':
            patha_dotp_lat = int(arg)
        elif opt == '--patha-dotp-count':
            patha_dotp_count = int(arg)
        elif opt == '--patha-pack-count':
            patha_pack_count = int(arg)
        elif opt == '--patha-pack-lat':
            patha_pack_lat = int(arg)
        elif opt == '--patha-matld-count':
            readport_count = int(arg)
            patha_matld_readports = int(arg)
        elif opt == '--patha-matld-lat':
            lmat_lat = int(arg)
        elif opt == '--patha-unroll':
            patha_unroll = int(arg)
        elif opt == '--patha-streaming':
            patha_streaming = parse_bool(arg)
        elif opt == '--patha-kernel':
            patha_kernel = arg
        elif opt == '--patha-input-buffer-depth':
            patha_input_buffer_depth = int(arg)
        elif opt == '--patha-stream-read-ports':
            patha_stream_read_ports = int(arg)
        elif opt == '--patha-store-queue-depth':
            patha_store_queue_depth = int(arg)
        elif opt == '--patha-async-store':
            patha_async_store = parse_bool(arg)
        elif opt == '--patha-matld-readports':
            patha_matld_readports = int(arg)
        elif opt == '--trsv5-count':
            trsv5_count = int(arg)
        elif opt == '--trsv5-lat':
            trsv5_lat = int(arg)
        elif opt == '--trsv5-mode':
            trsv5_mode = arg
        elif opt == '--trsv5-trace-enable':
            trsv5_trace_enable = True
        elif opt == '--trsv5-trace-solves':
            trsv5_trace_solves = int(arg)
        elif opt == '--trsv5-trace-file':
            trsv5_trace_file = arg
        elif opt == '--trsm5-mrhs-enable':
            trsm5_mrhs_enable = parse_bool(arg)
        elif opt == '--trsm5-mrhs-lat':
            trsm5_mrhs_lat = int(arg)
        elif opt == '--trsm5-mrhs-count':
            trsm5_mrhs_count = int(arg)
        elif opt == '--trsm5-mrhs-dual-enable':
            trsm5_mrhs_dual_enable = parse_bool(arg)
        elif opt == '--trsm5-mrhs-dual-lat':
            trsm5_mrhs_dual_lat = int(arg)
        elif opt == '--trsm5-mrhs-dual-count':
            trsm5_mrhs_dual_count = int(arg)
        elif opt == '--trsm5-coeff3-enable':
            trsm5_coeff3_enable = parse_bool(arg)
        elif opt == '--trsm5-coeff3-lat':
            trsm5_coeff3_lat = int(arg)
        elif opt == '--trsm5-coeff3-count':
            trsm5_coeff3_count = int(arg)
        elif opt == '--vec5-lat':
            pretransform_vec5_lat = int(arg)
        elif opt == '--pretransform-fallback-trsv5':
            pretransform_fallback_trsv5 = parse_bool(arg)
        elif opt == '--pretransform-diag-epsilon':
            pretransform_diag_epsilon = float(arg)
        elif opt == '--pretransform-per-sweep':
            pretransform_per_sweep = parse_bool(arg)
        elif opt == '--pretransform-output-buffer-enable':
            pretransform_output_buffer_enable = parse_bool(arg)
        elif opt == '--pretransform-output-buffer-depth':
            pretransform_output_buffer_depth = int(arg)
        elif opt == '--pretransform-spm-slots':
            pretransform_spm_slots = int(arg)
        elif opt == '--pretransform-overlap-enable':
            pretransform_overlap_enable = parse_bool(arg)
        elif opt == '--pretransform-use-barrier':
            pretransform_use_barrier = parse_bool(arg)
        elif opt == '--pretransform-validate':
            pretransform_validate = arg
        elif opt == '--coeff-preprocess-spm-slots':
            coeff_preprocess_spm_slots = int(arg)
        elif opt == '--coeff-output-buffer-depth':
            coeff_output_buffer_depth = int(arg)
        elif opt == '--coefficient-update-interval':
            coefficient_update_interval = int(arg)
        elif opt == '--coeff-preprocess-model':
            coeff_preprocess_model = arg
        elif opt == '--lu5-model':
            lu5_model = arg
        elif opt == '--coeff-validation':
            coeff_validation = arg
        elif opt == '--coeff-validation-sample-rate':
            coeff_validation_sample_rate = int(arg)
        elif opt == '--coeff-input-slots':
            coeff_input_slots = int(arg)
        elif opt == '--lu5-count':
            lu5_count = int(arg)
        elif opt == '--lu5-pending-depth':
            lu5_pending_depth = int(arg)
        elif opt == '--lu5-div-count':
            lu5_div_count = int(arg)
        elif opt == '--lu5-div-lat':
            lu5_div_lat = int(arg)
        elif opt == '--lu5-div-ii':
            lu5_div_ii = int(arg)
        elif opt == '--lu5-mulsub-count':
            lu5_mulsub_count = int(arg)
        elif opt == '--lu5-mul-lat':
            lu5_mul_lat = int(arg)
        elif opt == '--lu5-sub-lat':
            lu5_sub_lat = int(arg)
        elif opt == '--coeff3-count':
            coeff3_event_count = int(arg)
        elif opt == '--coeff3-pending-depth':
            coeff3_pending_depth = int(arg)
        elif opt == '--coeff3-rhs-lanes':
            coeff3_rhs_lanes = int(arg)
        elif opt == '--coeff3-schedule':
            coeff3_schedule = arg
        elif opt == '--coeff3-div-count':
            coeff3_div_count = int(arg)
        elif opt == '--coeff3-div-lat':
            coeff3_div_lat = int(arg)
        elif opt == '--coeff3-div-ii':
            coeff3_div_ii = int(arg)
        elif opt == '--coeff3-div-mode':
            coeff3_div_mode = arg
        elif opt == '--coeff-mulsub-count':
            coeff_mulsub_count = int(arg)
        elif opt == '--coeff-mul-lat':
            coeff_mul_lat = int(arg)
        elif opt == '--coeff-sub-lat':
            coeff_sub_lat = int(arg)
        elif opt == '--coeff3-partial-output':
            coeff3_partial_output = parse_bool(arg)
        elif opt == '--lu-forwarding':
            lu_forwarding = parse_bool(arg)
        elif opt == '--lu-solve-early-start':
            lu_solve_early_start = parse_bool(arg)
        elif opt == '--coeff-spm-read-ports':
            coeff_spm_read_ports = int(arg)
        elif opt == '--coeff-spm-write-ports':
            coeff_spm_write_ports = int(arg)
        elif opt == '--coeff-spm-write-width':
            coeff_spm_write_width = int(arg)
        elif opt == '--coeff-spm-model':
            coeff_spm_model = arg
        elif opt == '--coeff-spm-layout':
            coeff_spm_layout = arg
        elif opt == '--coeff-spm-outstanding':
            coeff_spm_outstanding = int(arg)
        elif opt == '--coeff-spm-banks':
            coeff_spm_banks = int(arg)
        elif opt == '--coeff-spm-bank-granularity':
            coeff_spm_bank_granularity = int(arg)
        elif opt == '--coeff-source-read-lat':
            coeff_source_read_lat = int(arg)
        elif opt == '--coeff-spm-write-lat':
            coeff_spm_write_lat = int(arg)
        elif opt == '--coeff-drain-width':
            coeff_drain_width = int(arg)
        elif opt == '--coeff-drain-ports':
            coeff_drain_ports = int(arg)
        elif opt == '--coeff-drain-lat':
            coeff_drain_lat = int(arg)
        elif opt == '--coeff-drain-outstanding':
            coeff_drain_outstanding = int(arg)
        elif opt == '--coeff-drain-queue-depth':
            coeff_drain_queue_depth = int(arg)
        elif opt == '--coeff-preprocess-trace-enable':
            coeff_preprocess_trace_enable = parse_bool(arg)
        elif opt == '--coeff-preprocess-trace-cells':
            coeff_preprocess_trace_cells = int(arg)
        elif opt == '--coeff-preprocess-trace-file':
            coeff_preprocess_trace_file = arg
        elif opt == '--coeff-cancel-test':
            coeff_cancel_test = parse_bool(arg)
        elif opt == '--coeff-cache-contract-test':
            coeff_cache_contract_test = parse_bool(arg)
        elif opt == '--coeff-streaming-enable':
            coeff_streaming_enable = parse_bool(arg)
        elif opt == '--coeff-stream-line-autonomous':
            coeff_stream_line_autonomous = parse_bool(arg)
        elif opt == '--coeff-base-solve':
            coeff_base_solve = arg
        elif opt == '--coeff-compute-model':
            coeff_compute_model = arg
        elif opt == '--coeff-line-storage':
            coeff_line_storage = arg
        elif opt == '--coeff-line-opt-level':
            coeff_line_opt_level = int(arg)
        elif opt == '--coeff-rhs-row-parallel':
            coeff_rhs_row_parallel = parse_bool(arg)
        elif opt == '--coeff-completion-depth':
            coeff_completion_depth = int(arg)
        elif opt == '--coeff-warm-write-depth':
            coeff_warm_write_depth = int(arg)
        elif opt == '--coeff-onchip-timing':
            coeff_onchip_timing = parse_bool(arg)
        elif opt == '--coeff-onchip-latency':
            coeff_onchip_latency = int(arg)
        elif opt == '--coeff-onchip-queue-depth':
            coeff_onchip_depth = int(arg)
        elif opt == '--coeff-line-tile-cells':
            coeff_line_tile_cells = int(arg)
        elif opt == '--coeff-line-cache-cells':
            coeff_line_cache_cells = int(arg)
        elif opt == '--coeff-line-tile-buffers':
            coeff_line_tile_buffers = int(arg)
        elif opt == '--coeff-line-tile-dma-width':
            coeff_line_tile_dma_width = int(arg)
        elif opt == '--coeff-line-spm-bytes':
            coeff_line_spm_bytes = int(arg)
        elif opt == '--coeff-interface-timing':
            coeff_interface_timing = parse_bool(arg)
        elif opt == '--coeff-interface-width':
            coeff_interface_width = int(arg)
        elif opt == '--coeff-cluster-mul-lat':
            coeff_cluster_mul_lat = int(arg)
        elif opt == '--coeff-cluster-add-lat':
            coeff_cluster_add_lat = int(arg)
        elif opt == '--coeff-cluster-div-lat':
            coeff_cluster_div_lat = int(arg)
        elif opt == '--coeff-cluster-div-ii':
            coeff_cluster_div_ii = int(arg)
        elif opt == '--coeff-cluster-product-depth':
            coeff_cluster_product_depth = int(arg)
        elif opt == '--coeff-line-base-ahead':
            coeff_line_base_ahead = parse_bool(arg)
        elif opt == '--coeff-line-early-backward':
            coeff_line_early_backward = parse_bool(arg)
        elif opt == '--coeff-line-mvm-model':
            coeff_line_mvm_model = arg
        elif opt == '--coeff-resource-stats':
            coeff_resource_stats = parse_bool(arg)
        elif opt == '--coeff-line-mul-lat':
            coeff_line_mul_lat = int(arg)
        elif opt == '--coeff-line-add-lat':
            coeff_line_add_lat = int(arg)
        elif opt == '--coeff-line-product-depth':
            coeff_line_product_depth = int(arg)
        elif opt == '--coeff-stream-window':
            coeff_stream_window = int(arg)
        elif opt == '--coeff-stream-retire-mode':
            coeff_stream_retire_mode = arg
        elif opt == '--coeff-stream-boundary-mask':
            coeff_stream_boundary_mask = parse_bool(arg)
        elif opt == '--coeff-stream-dinv-consumer':
            coeff_stream_dinv_consumer = arg
        elif opt == '--coeff-stream-lbar-consumer':
            coeff_stream_lbar_consumer = arg
        elif opt == '--coeff-stream-ubar-consumer':
            coeff_stream_ubar_consumer = arg
        elif opt == '--coeff-column-fma-latency':
            coeff_column_fma_latency = int(arg)
        elif opt == '--coeff-column-fma-ii':
            coeff_column_fma_ii = int(arg)
        elif opt == '--coeff-column-fma-count':
            coeff_column_fma_count = int(arg)
        elif opt == '--coeff-column-fma-queue-depth':
            coeff_column_fma_queue_depth = int(arg)
        elif opt == '--forward-combine-latency':
            forward_combine_latency = int(arg)
        elif opt == '--forward-combine-ii':
            forward_combine_ii = int(arg)
        elif opt == '--forward-combine-count':
            forward_combine_count = int(arg)
        elif opt == '--forward-combine-queue-depth':
            forward_combine_queue_depth = int(arg)
        elif opt == '--ubar-inplace':
            ubar_inplace = parse_bool(arg)
        elif opt in ('--lusgs-pretransform-auto',
                      '--lusgs-preprocess-auto'):
            lusgs_pretransform_auto = parse_bool(arg)
        elif opt in ('--lusgs-expected-sweeps',
                      '--lusgs-coeff-expected-sweeps'):
            lusgs_expected_sweeps = int(arg)
        elif opt == '--lusgs-lines':
            lusgs_lines = int(arg)
        elif opt == '--lusgs-cells':
            lusgs_cells = int(arg)
        elif opt == '--lusgs-interleave':
            lusgs_interleave = int(arg)
        elif opt == '--lusgs-mode':
            lusgs_mode = arg
        elif opt == '--lusgs-linebuf-enable':
            lusgs_linebuf_enable = parse_bool(arg)
        elif opt == '--lusgs-linebuf-entries':
            lusgs_linebuf_entries = int(arg)
        elif opt == '--lusgs-reference-only':
            lusgs_reference_only = True
        elif opt == '--lusgs-patha-only':
            lusgs_patha_only = True
        elif opt == '--lusgs-check':
            lusgs_check = parse_bool(arg)
        elif opt == '--lusgs-trace-enable':
            lusgs_trace_enable = True
        elif opt == '--lusgs-trace-lines':
            lusgs_trace_lines = int(arg)
        elif opt == '--lusgs-trace-cells':
            lusgs_trace_cells = int(arg)
        elif opt == '--lusgs-step3-stage':
            lusgs_step3_stage = arg.upper()
        elif opt == '--lusgs-contexts':
            lusgs_contexts = int(arg)
        elif opt == '--lusgs-tile-cells':
            lusgs_tile_cells = int(arg)
        elif opt == '--lusgs-update-q':
            lusgs_update_q = parse_bool(arg)
        elif opt == '--lusgs-omega':
            lusgs_omega = float(arg)
        elif opt == '--lusgs-controller-count':
            lusgs_controller_count = int(arg)
        elif opt == '--lusgs-vec5-count':
            lusgs_vec5_count = int(arg)
        elif opt == '--lusgs-vec5-lat':
            lusgs_vec5_lat = int(arg)
        elif opt == '--lusgs-controller-trace-enable':
            lusgs_controller_trace_enable = True
        elif opt == '--lusgs-controller-trace-lines':
            lusgs_controller_trace_lines = int(arg)
        elif opt == '--lusgs-controller-trace-cells':
            lusgs_controller_trace_cells = int(arg)
        elif opt == '--lusgs-controller-trace-file':
            lusgs_controller_trace_file = arg
        elif opt == '--lusgs-step4-enable':
            lusgs_step4_enable = parse_bool(arg)
        elif opt == '--lusgs-event-trace-enable':
            lusgs_event_trace_enable = True
        elif opt == '--lusgs-event-trace-lines':
            lusgs_event_trace_lines = int(arg)
        elif opt == '--lusgs-event-trace-cells':
            lusgs_event_trace_cells = int(arg)
        elif opt == '--lusgs-event-trace-file':
            lusgs_event_trace_file = arg
        elif opt == '--lusgs-step4-watchdog-cycles':
            lusgs_step4_watchdog_cycles = int(arg)
        elif opt == '--lusgs-step4-patha-real':
            lusgs_step4_patha_real = parse_bool(arg)
        elif opt == '--lusgs-step4-stage':
            lusgs_step4_stage = arg.upper()
        elif opt == '--lusgs-step4-trsv-real':
            lusgs_step4_trsv_real = parse_bool(arg)
        elif opt == '--lusgs-step4-vec5-real':
            lusgs_step4_vec5_real = parse_bool(arg)
        elif opt == '--trsv5-event-count':
            trsv5_event_count = int(arg)
        elif opt == '--trsv5-div-count':
            trsv5_div_count = int(arg)
        elif opt == '--trsv5-fma-count':
            trsv5_fma_count = int(arg)
        elif opt == '--trsv5-div-lat':
            trsv5_div_lat = int(arg)
        elif opt == '--trsv5-fma-lat':
            trsv5_fma_lat = int(arg)
        elif opt == '--trsv5-load-lat':
            trsv5_load_lat = int(arg)
        elif opt == '--trsv5-result-lat':
            trsv5_result_lat = int(arg)
        elif opt == '--trsv5-forwarding':
            trsv5_forwarding = parse_bool(arg)
        elif opt == '--trsv5-queue-depth':
            trsv5_queue_depth = int(arg)
        elif opt == '--vec5-lanes':
            vec5_lanes = int(arg)
        elif opt == '--vec5-count':
            vec5_event_count = int(arg)
        elif opt == '--vec5-copy-lat':
            vec5_copy_lat = int(arg)
        elif opt == '--vec5-sub-lat':
            vec5_sub_lat = int(arg)
        elif opt == '--vec5-axpy-lat':
            vec5_axpy_lat = int(arg)
        elif opt == '--vec5-initiation-interval':
            vec5_initiation_interval = int(arg)
        elif opt == '--vec5-queue-depth':
            vec5_queue_depth = int(arg)
        elif opt == '--dram-size':
            dram_size = arg
        elif opt == '--lusgs-step4-engine-backpressure-test':
            lusgs_step4_engine_backpressure_test = True
        elif opt == '--lusgs-step4-snapshot-test':
            lusgs_step4_snapshot_test = True
        elif opt == '--lusgs-step4-lifecycle-test':
            lusgs_step4_lifecycle_test = True
        elif opt == '--lusgs-step4-wrong-path-launch':
            lusgs_step4_wrong_path_launch = True
        elif opt == '--lusgs-step4-wrong-path-wait':
            lusgs_step4_wrong_path_wait = True
        elif opt == '--lusgs-step4-overlap-test':
            lusgs_step4_overlap_test = True
        elif opt == '--lusgs-step4-overlap-iters':
            lusgs_step4_overlap_iters = int(arg)
        elif opt == '--lusgs-error-case':
            lusgs_error_case = arg
        elif opt == '--cfd-trace-enable':
            cfd_trace_enable = True
        elif opt == '--cfd-trace-matvecs':
            cfd_trace_matvecs = int(arg)
        elif opt == '--cfd-trace-file':
            cfd_trace_file = arg
        elif opt == '--mova-lat':
            mova_lat = int(arg)
        elif opt == '--iq-entries':
            iq_entries = int(arg)
        elif opt == '--rob-entries':
            rob_entries = int(arg)
except getopt.GetoptError as e:
    print(f"Error: {e}")
    sys.exit(1)

if spm_access_mode == 'local':
    # Backward-compatible alias for the v6.2 upper-bound local model.
    spm_access_mode = 'local-ideal'

local_event_modes = ('local-event', 'local-flow', 'local-realistic-event')
if spm_access_mode not in (
        'generic', 'local-ideal', 'local-realistic', *local_event_modes):
    print("ERROR: --spm-access-mode must be generic, local-ideal, "
          "local-realistic, local-event, local-flow, or local-realistic-event")
    sys.exit(1)
if za_state_mode == 'shadow':
    # Explicit spelling for the older pending/shadow model.
    za_state_mode = 'pending'

if za_state_mode not in ('simple', 'pending', 'rename'):
    print("ERROR: --za-state-mode must be simple, pending/shadow, or rename")
    sys.exit(1)
if local_spm_bank_mapping not in ('addr', 'row', 'xor'):
    print("ERROR: --local-spm-bank-mapping must be addr, row, or xor")
    sys.exit(1)
if local_spm_read_width < 40:
    print("ERROR: --local-spm-read-width must be >= 40 for lmat5_spm payload")
    sys.exit(1)
if patha_spm_mode not in ('direct', 'local-event-report'):
    print("ERROR: --patha-spm-mode must be direct or local-event-report")
    sys.exit(1)
if patha_spm_read_width < 40:
    print("ERROR: --patha-spm-read-width must be >= 40 for lmat5_spm payload")
    sys.exit(1)
if patha_store_mode not in ('full64', 'pred40', 'store5'):
    print("ERROR: --patha-store-mode must be full64, pred40, or store5")
    sys.exit(1)
if patha_store_stride is None:
    patha_store_stride = 40 if patha_store_mode in ('pred40', 'store5') else 64
if patha_store_mode == 'full64' and patha_store_stride != 64:
    print("ERROR: --patha-store-stride must be 64 for full64 stores")
    sys.exit(1)
if patha_store_mode == 'pred40' and patha_store_stride not in (40, 64):
    print("ERROR: --patha-store-stride must be 40 or 64 for pred40 stores")
    sys.exit(1)
if patha_store_mode == 'store5' and patha_store_stride != 40:
    print("ERROR: --patha-store-stride must be 40 for store5 stores")
    sys.exit(1)
if patha_acc_mode not in ('internal-buffer', 'xregs'):
    print("ERROR: --patha-acc-mode must be internal-buffer or xregs")
    sys.exit(1)
if patha_result_buffer_depth not in (1, 2, 4):
    print("ERROR: --patha-result-buffer-depth must be 1, 2, or 4")
    sys.exit(1)
if patha_dotp_lat <= 0:
    print("ERROR: --patha-dotp-lat must be positive")
    sys.exit(1)
if patha_dotp_lat < 5:
    print("WARNING: --patha-dotp-lat < 5 is an aggressive experiment; "
          "do not use it as the Path A tapeout or balanced result.")
if patha_dotp_count not in (1, 2, 4):
    print("ERROR: --patha-dotp-count must be 1, 2, or 4")
    sys.exit(1)
if patha_pack_count not in (1, 2):
    print("ERROR: --patha-pack-count must be 1 or 2")
    sys.exit(1)
if patha_pack_lat <= 0:
    print("ERROR: --patha-pack-lat must be > 0")
    sys.exit(1)
if patha_unroll not in (2, 4, 8):
    print("ERROR: --patha-unroll must be 2, 4, or 8")
    sys.exit(1)
if patha_kernel not in ('baseline', 'stream-scheduled', 'stream-dsa'):
    print("ERROR: --patha-kernel must be baseline, stream-scheduled, or stream-dsa")
    sys.exit(1)
if patha_input_buffer_depth not in (2, 3, 4):
    print("ERROR: --patha-input-buffer-depth must be 2, 3, or 4")
    sys.exit(1)
if patha_stream_read_ports not in (1, 2):
    print("ERROR: --patha-stream-read-ports must be 1 or 2")
    sys.exit(1)
if patha_store_queue_depth not in (2, 4, 8):
    print("ERROR: --patha-store-queue-depth must be 2, 4, or 8")
    sys.exit(1)
if trsv5_count <= 0:
    print("ERROR: --trsv5-count must be positive")
    sys.exit(1)
if trsv5_lat <= 1:
    print("ERROR: --trsv5-lat must be greater than 1 for Step2 coarse latency modeling")
    sys.exit(1)
if trsv5_mode not in ('zreg-coarse',):
    print("ERROR: --trsv5-mode must be zreg-coarse")
    sys.exit(1)
if trsv5_trace_solves <= 0:
    print("ERROR: --trsv5-trace-solves must be positive")
    sys.exit(1)
if trsm5_mrhs_lat <= 0 or trsm5_mrhs_count <= 0:
    print("ERROR: --trsm5-mrhs-lat/count must be positive")
    sys.exit(1)
if trsm5_mrhs_dual_lat <= 0 or trsm5_mrhs_dual_count <= 0:
    print("ERROR: --trsm5-mrhs-dual-lat/count must be positive")
    sys.exit(1)
if trsm5_coeff3_lat <= 0 or trsm5_coeff3_count <= 0:
    print("ERROR: --trsm5-coeff3-lat/count must be positive")
    sys.exit(1)
if pretransform_vec5_lat <= 0:
    print("ERROR: --vec5-lat must be positive")
    sys.exit(1)
if pretransform_diag_epsilon <= 0.0:
    print("ERROR: --pretransform-diag-epsilon must be positive")
    sys.exit(1)
if lusgs_lines <= 0:
    print("ERROR: --lusgs-lines must be positive")
    sys.exit(1)
if lusgs_cells <= 0:
    print("ERROR: --lusgs-cells must be positive")
    sys.exit(1)
if lusgs_interleave <= 0:
    print("ERROR: --lusgs-interleave must be positive")
    sys.exit(1)
if lusgs_reference_only and lusgs_patha_only:
    print("ERROR: --lusgs-reference-only and --lusgs-patha-only are mutually exclusive")
    sys.exit(1)
if lusgs_mode not in ('all', 'raw-compare', 'raw-compare-context',
                      'reference', 'step1', 'step2', 'step2-core',
                      'step2-core-forwarded',
                      'step2-core-forwarded-context',
                      'step2-core-forwarded-linebuf',
                      'step2-pretransform',
                      'step2-pretransform-context',
                      'step2-pretransform-optprep',
                      'step2-pretransform-optprep-context',
                      'step2-trsv5-raw',
                      'step2-trsv5-raw-context',
                      'step2-pretransform-raw',
                      'step2-pretransform-raw-context',
                      'step2-pretransform-raw-optprep',
                      'step2-pretransform-raw-optprep-context',
                      'step2-pretransform-raw-optprep-stream'):
    print("ERROR: unsupported --lusgs-mode")
    sys.exit(1)
if lusgs_linebuf_entries <= 0:
    print("ERROR: --lusgs-linebuf-entries must be positive")
    sys.exit(1)
if lusgs_mode in ('all', 'step2-core-forwarded-linebuf') and not lusgs_linebuf_enable:
    print("ERROR: LU-SGS line-buffer mode requires --lusgs-linebuf-enable=1")
    sys.exit(1)
if lusgs_mode in ('all', 'raw-compare', 'raw-compare-context',
                  'step2-pretransform',
                  'step2-pretransform-context') and not trsm5_mrhs_enable:
    print("ERROR: pretransform modes require --trsm5-mrhs-enable=1")
    sys.exit(1)
if lusgs_mode in ('all', 'step2-pretransform-optprep',
                  'step2-pretransform-optprep-context') and not trsm5_mrhs_dual_enable:
    print("ERROR: optimized pretransform modes require --trsm5-mrhs-dual-enable=1")
    sys.exit(1)
if lusgs_mode in ('all', 'raw-compare', 'raw-compare-context',
                  'step2-pretransform-raw-optprep',
                  'step2-pretransform-raw-optprep-context',
                  'step2-pretransform-raw-optprep-stream') and not trsm5_coeff3_enable:
    print("ERROR: raw optprep modes require --trsm5-coeff3-enable=1")
    sys.exit(1)
if pretransform_output_buffer_depth <= 0 or pretransform_output_buffer_depth > 8:
    print("ERROR: --pretransform-output-buffer-depth must be in [1, 8]")
    sys.exit(1)
if pretransform_spm_slots not in (1, 2):
    print("ERROR: --pretransform-spm-slots must be 1 or 2")
    sys.exit(1)
if coeff_preprocess_spm_slots not in (1, 2):
    print("ERROR: --coeff-preprocess-spm-slots must be 1 or 2")
    sys.exit(1)
if coeff_output_buffer_depth <= 0 or coeff_output_buffer_depth > 8:
    print("ERROR: --coeff-output-buffer-depth must be in [1, 8]")
    sys.exit(1)
if coefficient_update_interval < 0:
    print("ERROR: --coefficient-update-interval must be non-negative")
    sys.exit(1)
if coeff_preprocess_model not in ('coarse', 'event'):
    print("ERROR: --coeff-preprocess-model must be coarse or event")
    sys.exit(1)
if lu5_model not in ('software', 'event'):
    print("ERROR: --lu5-model must be software or event")
    sys.exit(1)
if coeff_validation not in ('performance', 'sampled', 'full'):
    print("ERROR: --coeff-validation must be performance, sampled, or full")
    sys.exit(1)
if coeff_validation_sample_rate <= 0:
    print("ERROR: --coeff-validation-sample-rate must be positive")
    sys.exit(1)
if coeff_stream_window not in (1, 2, 4, 8):
    print("ERROR: --coeff-stream-window must be 1, 2, 4, or 8")
    sys.exit(1)
if coeff_stream_retire_mode not in ('sync', 'queue', 'auto'):
    print("ERROR: --coeff-stream-retire-mode must be sync, queue, or auto")
    sys.exit(1)
if coeff_stream_dinv_consumer not in ('off', 'shadow', 'direct'):
    print("ERROR: --coeff-stream-dinv-consumer must be off, shadow, or direct")
    sys.exit(1)
if coeff_stream_lbar_consumer not in ('off', 'shadow', 'direct'):
    print("ERROR: --coeff-stream-lbar-consumer must be off, shadow, or direct")
    sys.exit(1)
if coeff_stream_ubar_consumer not in ('off', 'direct'):
    print("ERROR: --coeff-stream-ubar-consumer must be off or direct")
    sys.exit(1)
if not 1 <= coeff_column_fma_latency <= 1024:
    print("ERROR: --coeff-column-fma-latency must be in [1,1024]")
    sys.exit(1)
if not 1 <= coeff_column_fma_ii <= 64:
    print("ERROR: --coeff-column-fma-ii must be in [1,64]")
    sys.exit(1)
if not 1 <= coeff_column_fma_count <= 4:
    print("ERROR: --coeff-column-fma-count must be in [1,4]")
    sys.exit(1)
if not 1 <= coeff_column_fma_queue_depth <= 64:
    print("ERROR: --coeff-column-fma-queue-depth must be in [1,64]")
    sys.exit(1)
if not 1 <= forward_combine_latency <= 64:
    print("ERROR: --forward-combine-latency must be in [1,64]")
    sys.exit(1)
if not 1 <= forward_combine_ii <= 64:
    print("ERROR: --forward-combine-ii must be in [1,64]")
    sys.exit(1)
if not 1 <= forward_combine_count <= 4:
    print("ERROR: --forward-combine-count must be in [1,4]")
    sys.exit(1)
if not 1 <= forward_combine_queue_depth <= 64:
    print("ERROR: --forward-combine-queue-depth must be in [1,64]")
    sys.exit(1)
if not coeff_stream_boundary_mask and coeff_stream_retire_mode != 'auto':
    print("ERROR: --coeff-stream-boundary-mask=0 is an auto-retire-only isolation mode")
    sys.exit(1)
stream_mode = lusgs_mode == 'step2-pretransform-raw-optprep-stream'
if not 0 <= coeff_line_opt_level <= 8 or (coeff_line_opt_level and
        (coeff_line_storage != 'tiled' or not coeff_interface_timing)):
    print("ERROR: --coeff-line-opt-level must be 0..8; optimization requires tiled timing DMA")
    sys.exit(1)
if coeff_onchip_timing is None:
    coeff_onchip_timing = coeff_line_opt_level >= 8
if coeff_onchip_timing and (not 1 <= coeff_spm_banks <= 32 or not 1 <= coeff_spm_read_ports <= 8 or not 1 <= coeff_spm_write_ports <= 8 or coeff_spm_bank_granularity < 8 or coeff_spm_bank_granularity % 8):
    print("ERROR: onchip banks require 1..32 banks, 1..8 ports and a granularity divisible by 8")
    sys.exit(1)
if not 1 <= coeff_onchip_depth <= 128 or not 1 <= coeff_onchip_latency <= 32 or (coeff_onchip_timing and coeff_line_opt_level < 5):
    print("ERROR: onchip timing requires level >=5, queue depth 1..128 and latency 1..32")
    sys.exit(1)
if not 1 <= coeff_completion_depth <= 32 or not 1 <= coeff_warm_write_depth <= 32:
    print("ERROR: completion and warm write depths must be in [1,32]")
    sys.exit(1)
if coeff_rhs_row_parallel and (coeff_line_storage != 'tiled' or
        coeff_compute_model != 'shared' or not coeff_interface_timing):
    print("ERROR: parallel RHS rows require the tiled shared timing model")
    sys.exit(1)
if coeff_line_storage not in ('full', 'tiled'):
    print("ERROR: --coeff-line-storage must be full or tiled")
    sys.exit(1)
if not 1 <= coeff_line_tile_cells <= 1024 or coeff_line_spm_bytes <= 0:
    print("ERROR: finite tile requires tile cells in [1,1024] and positive SPM bytes")
    sys.exit(1)
if not 1 <= coeff_interface_width <= 1024 or (coeff_interface_timing and coeff_line_storage != 'tiled'):
    print("ERROR: timed interface requires tiled storage and width in [1,1024]")
    sys.exit(1)
if coeff_line_storage == 'tiled':
    if (not stream_mode or not coeff_stream_line_autonomous or lusgs_lines != 1
            or coeff_stream_ubar_consumer != 'direct' or coeff_spm_model != 'dma'
            or coeff_compute_model != 'shared'):
        print("ERROR: tiled baseline requires one autonomous shared DMA line, "
              "and direct Ubar")
        sys.exit(1)
    reserve = 8192 * (coeff_input_slots + lu5_pending_depth +
                      coeff3_pending_depth + coeff_output_buffer_depth) + 16384
    reserve += 4096 if coeff_rhs_row_parallel else 0
    if coeff_line_opt_level >= 5:
        reserve += coeff_completion_depth * 1024 + 2048 + coeff_warm_write_depth * 256 + coeff_spm_outstanding * 64
    if coeff_onchip_timing:
        reserve += coeff_onchip_depth*256 + 128*(coeff_input_slots+lu5_pending_depth+coeff3_pending_depth+coeff_output_buffer_depth) + 1024
    if coeff_line_tile_buffers not in (1, 2) or coeff_line_cache_cells < 0 or not 1 <= coeff_line_tile_dma_width <= 1024:
        print("ERROR: tile buffers must be 1 or 2, cache cells nonnegative and DMA width in [1,1024]")
        sys.exit(1)
    resident = 240 * min(coeff_line_tile_cells, lusgs_cells) * coeff_line_tile_buffers
    coeff_line_cache_allowed = (lusgs_cells <= coeff_line_cache_cells and
        reserve + resident + 600 * lusgs_cells <= coeff_line_spm_bytes)
    if coeff_cache_contract_test and (not coeff_line_cache_allowed or coeff_line_cache_cells < 3):
        print("ERROR: finite cache contract requires capacity for all fixture coefficients")
        sys.exit(1)
    if reserve + resident > coeff_line_spm_bytes:
        print(f"ERROR: finite SPM needs at least {reserve + resident} bytes "
              f"({reserve} reserved + {resident} tile bytes)")
        sys.exit(1)
if coeff_compute_model not in ('legacy', 'shared'):
    print("ERROR: --coeff-compute-model must be legacy or shared")
    sys.exit(1)
if coeff_compute_model == 'shared' and (
        not stream_mode or not coeff_stream_line_autonomous or lusgs_lines != 1
        or coeff_stream_ubar_consumer != 'direct'):
    print("ERROR: shared cluster requires one autonomous streaming line and direct Ubar")
    sys.exit(1)
for name, value in (
        ('mul-lat', coeff_cluster_mul_lat), ('add-lat', coeff_cluster_add_lat),
        ('div-lat', coeff_cluster_div_lat), ('div-ii', coeff_cluster_div_ii),
        ('product-depth', coeff_cluster_product_depth)):
    if not 1 <= value <= 64:
        print(f"ERROR: --coeff-cluster-{name} must be in [1,64]")
        sys.exit(1)
if coeff_base_solve not in ('inverse', 'direct'):
    print("ERROR: --coeff-base-solve must be inverse or direct")
    sys.exit(1)
if coeff_base_solve == 'direct' and (
        not stream_mode or not coeff_stream_line_autonomous or lusgs_lines != 1
        or coeff_stream_ubar_consumer != 'direct'):
    print("ERROR: direct base solve requires one autonomous streaming line, "
          "and direct Ubar")
    sys.exit(1)
if coeff_stream_dinv_consumer != 'off' and (
        not stream_mode or coeff_stream_retire_mode != 'auto'):
    print("ERROR: DInv consumer requires streaming auto-retire mode")
    sys.exit(1)
if coeff_stream_lbar_consumer != 'off' and (
        coeff_stream_dinv_consumer != 'direct' or
        not stream_mode or coeff_stream_retire_mode != 'auto'):
    print("ERROR: Lbar consumer requires streaming auto-retire and DInv direct")
    sys.exit(1)
if coeff_stream_line_autonomous and (
        not stream_mode or coeff_stream_retire_mode != 'auto' or
        coeff_stream_dinv_consumer != 'direct' or
        coeff_stream_lbar_consumer != 'direct' or
        not coeff_stream_boundary_mask):
    print("ERROR: line autonomy requires streaming auto-retire, boundary mask, and direct DInv/Lbar consumers")
    sys.exit(1)
if coeff_stream_ubar_consumer != 'off' and not coeff_stream_line_autonomous:
    print("ERROR: Ubar direct consumer requires line autonomy")
    sys.exit(1)
if not stream_mode and not coeff_stream_boundary_mask:
    print("ERROR: --coeff-stream-boundary-mask only applies to streaming mode")
    sys.exit(1)
if stream_mode != coeff_streaming_enable:
    print("ERROR: streaming mode and --coeff-streaming-enable=1 must be selected together")
    sys.exit(1)
if stream_mode and (coeff_preprocess_model != 'event' or lu5_model != 'event'):
    print("ERROR: streaming mode requires event coefficient preprocessing and event LU")
    sys.exit(1)
if stream_mode and lusgs_lines != 1 and not coeff_stream_line_autonomous:
    print("ERROR: multi-line streaming requires --coeff-stream-line-autonomous=1")
    sys.exit(1)
if stream_mode and bench_iters > 1:
    if (not coeff_stream_line_autonomous or
            coeff_stream_ubar_consumer != 'direct' or
            coefficient_update_interval <= 0):
        print("ERROR: multi-sweep streaming requires line autonomy, direct "
              "Ubar consumption, and a non-zero coefficient update interval")
        sys.exit(1)
if coeff_input_slots not in (1, 2, 4):
    print("ERROR: --coeff-input-slots must be 1, 2, or 4")
    sys.exit(1)
if coeff3_rhs_lanes not in (1, 3, 5, 15):
    print("ERROR: --coeff3-rhs-lanes must be 1, 3, 5, or 15")
    sys.exit(1)
if coeff3_schedule not in ('column-major', 'step-major',
                           'round-robin-ready', 'ready-first',
                           'frontier-aware', 'request-frontier-aging',
                           'rhs-frontier'):
    print("ERROR: --coeff3-schedule must be column-major, step-major, "
          "round-robin-ready, ready-first, frontier-aware, or "
          "request-frontier-aging, rhs-frontier")
    sys.exit(1)
if coeff3_div_mode not in ('divide', 'reciprocal'):
    print("ERROR: --coeff3-div-mode must be divide or reciprocal")
    sys.exit(1)
if coeff_spm_model not in ('internal', 'packet', 'dma'):
    print("ERROR: --coeff-spm-model must be internal, packet, or dma")
    sys.exit(1)
if coeff_spm_layout not in ('legacy', 'matrix-separated', 'row-striped'):
    print("ERROR: --coeff-spm-layout must be legacy, matrix-separated, "
          "or row-striped")
    sys.exit(1)
positive_coeff_resources = (
    lu5_count, lu5_pending_depth, lu5_div_count, lu5_div_lat, lu5_div_ii,
    lu5_mulsub_count, lu5_mul_lat, lu5_sub_lat, coeff3_event_count,
    coeff3_pending_depth, coeff3_div_count, coeff3_div_lat, coeff3_div_ii,
    coeff_mulsub_count, coeff_mul_lat, coeff_sub_lat, coeff_spm_read_ports,
    coeff_spm_write_ports, coeff_spm_write_width, coeff_spm_outstanding,
    coeff_spm_banks,
    coeff_spm_bank_granularity, coeff_source_read_lat, coeff_spm_write_lat,
    coeff_drain_width, coeff_drain_ports, coeff_drain_lat,
    coeff_drain_outstanding, coeff_drain_queue_depth,
    coeff_preprocess_trace_cells)
if any(value <= 0 for value in positive_coeff_resources):
    print("ERROR: event coefficient preprocessing resources must be positive")
    sys.exit(1)
if coeff_drain_width not in (40, 64):
    print("ERROR: --coeff-drain-width must be 40 or 64")
    sys.exit(1)
if ubar_inplace:
    print("ERROR: --ubar-inplace=1 is reserved for later optimization")
    sys.exit(1)
if pretransform_validate not in ('full', 'fast'):
    print("ERROR: --pretransform-validate must be full or fast")
    sys.exit(1)
if lusgs_expected_sweeps <= 0:
    print("ERROR: --lusgs-expected-sweeps must be positive")
    sys.exit(1)
if lusgs_trace_lines <= 0:
    print("ERROR: --lusgs-trace-lines must be positive")
    sys.exit(1)
if lusgs_trace_cells <= 0:
    print("ERROR: --lusgs-trace-cells must be positive")
    sys.exit(1)
if lusgs_step3_stage not in ('A', 'B', 'C'):
    print("ERROR: --lusgs-step3-stage must be A, B, or C")
    sys.exit(1)
if lusgs_step4_stage is None:
    lusgs_step4_stage = lusgs_step3_stage
if lusgs_step4_stage not in ('A', 'B', 'C'):
    print("ERROR: --lusgs-step4-stage must be A, B, or C")
    sys.exit(1)
if lusgs_step4_stage in ('B', 'C'):
    lusgs_step4_patha_real = True
if lusgs_step4_stage == 'C':
    lusgs_step4_trsv_real = True
    lusgs_step4_vec5_real = True
if min(trsv5_event_count, trsv5_div_count, trsv5_fma_count,
       trsv5_div_lat, trsv5_fma_lat, trsv5_load_lat,
       trsv5_result_lat, trsv5_queue_depth, vec5_event_count,
       vec5_copy_lat, vec5_sub_lat, vec5_axpy_lat,
       vec5_initiation_interval, vec5_queue_depth) <= 0:
    print("ERROR: Step4-C resource counts, latencies, and queue depths must be positive")
    sys.exit(1)
if vec5_lanes not in (1, 2, 5):
    print("ERROR: --vec5-lanes must be 1, 2, or 5")
    sys.exit(1)
if lusgs_contexts <= 0:
    print("ERROR: --lusgs-contexts must be positive")
    sys.exit(1)
if lusgs_tile_cells <= 0:
    print("ERROR: --lusgs-tile-cells must be positive")
    sys.exit(1)
if lusgs_controller_count <= 0:
    print("ERROR: --lusgs-controller-count must be positive")
    sys.exit(1)
if lusgs_vec5_count <= 0:
    print("ERROR: --lusgs-vec5-count must be positive")
    sys.exit(1)
if lusgs_vec5_lat <= 0:
    print("ERROR: --lusgs-vec5-lat must be positive")
    sys.exit(1)
if lusgs_controller_trace_lines <= 0 or lusgs_controller_trace_cells <= 0:
    print("ERROR: --lusgs-controller-trace-lines/cells must be positive")
    sys.exit(1)
if lusgs_event_trace_lines <= 0 or lusgs_event_trace_cells <= 0:
    print("ERROR: --lusgs-event-trace-lines/cells must be positive")
    sys.exit(1)


def patha_matld_extra_per_run():
    # The true pred40_stride40 unroll8 kernels do not use a benchmark
    # prologue load.  The correctness precheck still contributes one extra
    # ping preload, hence +6 rather than the legacy +12.
    if (patha_unroll == 8 and patha_store_mode == 'pred40' and
            patha_store_stride == 40):
        return 6
    return THEORY['dotp_matld_extra_per_run']
if patha_matld_readports is not None:
    readport_count = patha_matld_readports

patha_binaries = ('test_cfd_extreme_arm',
                  'test_cfd_extreme_perf',
                  'test_cfd_extreme_perf_arm',
                  'test_cfd_patha_extreme_arm',
                  'test_cfd_patha_extreme_perf',
                  'test_cfd_patha_extreme_perf_arm')
lusgs_binaries = ('test_cfd_lusgs_patha_step1',
                  'test_cfd_lusgs_patha_step1_arm',
                  'test_cfd_lusgs_patha_step2',
                  'test_cfd_lusgs_patha_step2_arm',
                  'test_cfd_lusgs_patha_step3',
                  'test_cfd_lusgs_patha_step3_arm',
                  'test_cfd_lusgs_patha_step4',
                  'test_cfd_lusgs_patha_step4_arm')
lusgs_step2_binaries = ('test_cfd_lusgs_patha_step2',
                        'test_cfd_lusgs_patha_step2_arm')
lusgs_step3_binaries = ('test_cfd_lusgs_patha_step3',
                        'test_cfd_lusgs_patha_step3_arm')
lusgs_step4_binaries = ('test_cfd_lusgs_patha_step4',
                        'test_cfd_lusgs_patha_step4_arm')
trsv5_binaries = ('test_cfd_trsv5',
                  'test_cfd_trsv5_arm',
                  'test_cfd_lusgs_patha_step2',
                  'test_cfd_lusgs_patha_step2_arm',
                  'test_cfd_lusgs_patha_step3',
                  'test_cfd_lusgs_patha_step3_arm',
                  'test_cfd_lusgs_patha_step4',
                  'test_cfd_lusgs_patha_step4_arm')
is_lusgs_binary = os.path.basename(binary_path) in lusgs_binaries
is_lusgs_step2_binary = os.path.basename(binary_path) in lusgs_step2_binaries
is_lusgs_step3_binary = os.path.basename(binary_path) in lusgs_step3_binaries
is_lusgs_step4_binary = os.path.basename(binary_path) in lusgs_step4_binaries
is_trsv5_binary = os.path.basename(binary_path) in trsv5_binaries
if is_lusgs_step4_binary:
    lusgs_step4_enable = True
if (is_lusgs_binary or is_trsv5_binary) and not bench_iters_user_set:
    bench_iters = 1
is_patha_binary = os.path.basename(binary_path) in patha_binaries or is_lusgs_binary
if is_patha_binary and patha_spm_mode == 'direct':
    # Path A's documented Direct-SPM upper-bound uses the same CPU-local
    # lmat5_spm implementation as local-ideal, but keeps its own read-width and
    # read-port reporting knobs.  This avoids accidentally measuring the generic
    # uncacheable LSQ/cache/xbar/membus path when Path A asks for "direct".
    if spm_access_mode == 'generic':
        spm_access_mode = 'local-ideal'
    local_spm_read_width = patha_spm_read_width
    local_spm_read_ports = (
        patha_stream_read_ports if patha_kernel == 'stream-dsa'
        else readport_count
    )
    local_spm_lat = lmat_lat
if spm_access_mode in ('local-ideal', 'local-realistic') + local_event_modes:
    lmat_lat = local_spm_lat
    readport_count = local_spm_read_ports
if spm_access_mode == 'local-realistic':
    local_spm_real_backpressure = True
    # Approximate the SRAM response queue + Z writeback arbitration cost in the
    # O3-visible FU latency.  v6.2 local-ideal deliberately does not apply this.
    lmat_lat = local_spm_lat + 1 + max(0, 2 - local_spm_z_wb_ports)
    readport_count = local_spm_read_ports
elif spm_access_mode in local_event_modes:
    local_spm_real_backpressure = True
    # local-event keeps v6.3 intact and introduces a separate, conservative
    # O3-visible local SRAM response/Z writeback model.  FU latency controls
    # dependent Z wakeup; non-pipelined mode is used only for constrained queue
    # or single-bank stress configurations.
    zwb_penalty = max(0, 2 - local_spm_z_wb_ports)
    queue_penalty = 1 if local_spm_queue_size <= 1 else 0
    bank_penalty = 1 if (local_spm_bank_conflict and local_spm_banks <= 1) else 0
    outstanding_penalty = 1 if local_spm_outstanding <= 1 else 0
    lmat_lat = (
        local_spm_lat + 1 + zwb_penalty +
        queue_penalty + bank_penalty + outstanding_penalty
    )
    readport_count = local_spm_read_ports
    lmat_pipelined = not (
        local_spm_outstanding <= 1 or
        local_spm_queue_size <= 1 or
        (local_spm_bank_conflict and local_spm_banks <= 1)
    )

os.environ['GEM5_CFD_SPM_ACCESS_MODE'] = spm_access_mode
os.environ['GEM5_CFD_PATHB_ENABLE'] = (
    '1' if report_focus == 'pathb' or
    os.environ.get('GEM5_CFD_RUN_WRAPPER', '') == 'pathb' else '0')
os.environ['GEM5_CFD_PATHC_OUTER_ENABLE'] = (
    '1' if report_focus == 'pathc' or
    os.environ.get('GEM5_CFD_RUN_WRAPPER', '') == 'pathc' else '0')
os.environ['GEM5_CFD_SME_OUTER_FULL_ARRAY'] = (
    '1' if sme_outer_full_array else '0')
os.environ['GEM5_CFD_LOCAL_SPM_LAT'] = str(local_spm_lat)
os.environ['GEM5_CFD_LOCAL_SPM_READ_PORTS'] = str(local_spm_read_ports)
os.environ['GEM5_CFD_LOCAL_SPM_READ_WIDTH'] = str(local_spm_read_width)
os.environ['GEM5_CFD_LOCAL_SPM_OUTSTANDING'] = str(local_spm_outstanding)
os.environ['GEM5_CFD_LOCAL_SPM_QUEUE_SIZE'] = str(local_spm_queue_size)
os.environ['GEM5_CFD_LOCAL_SPM_BANKS'] = str(local_spm_banks)
os.environ['GEM5_CFD_LOCAL_SPM_BANK_GRANULARITY'] = str(local_spm_bank_granularity)
os.environ['GEM5_CFD_LOCAL_SPM_BANK_MAPPING'] = local_spm_bank_mapping
os.environ['GEM5_CFD_LOCAL_SPM_BANK_CONFLICT'] = (
    'true' if local_spm_bank_conflict else 'false')
os.environ['GEM5_CFD_LOCAL_SPM_BYPASS_LSQ'] = (
    'true' if local_spm_bypass_lsq else 'false')
os.environ['GEM5_CFD_LOCAL_SPM_Z_WB_PORTS'] = str(local_spm_z_wb_ports)
os.environ['GEM5_CFD_LOCAL_SPM_REAL_BACKPRESSURE'] = (
    'true' if local_spm_real_backpressure else 'false')
os.environ['GEM5_CFD_ZA_STATE_MODE'] = za_state_mode
os.environ['GEM5_CFD_ZA_STATE_MODE'] = za_state_mode
os.environ['GEM5_CFD_TICKS_PER_CYCLE'] = '500'
os.environ['GEM5_CFD_LMAT_LAT'] = str(lmat_lat)
os.environ['GEM5_CFD_READPORT_COUNT'] = str(readport_count)
os.environ['GEM5_CFD_PATHA_DOTP_LAT'] = str(patha_dotp_lat)
os.environ['GEM5_CFD_PATHA_DOTP_COUNT'] = str(patha_dotp_count)
os.environ['GEM5_CFD_PATHA_PACK_LAT'] = str(patha_pack_lat)
os.environ['GEM5_CFD_PATHA_PACK_COUNT'] = str(patha_pack_count)
os.environ['GEM5_CFD_SME_OUTER_LAT'] = str(sme_outer_lat)
os.environ['GEM5_CFD_TRACE_ENABLE'] = '1' if cfd_trace_enable else '0'
os.environ['GEM5_CFD_TRACE_MATVECS'] = str(cfd_trace_matvecs)
os.environ['GEM5_CFD_TRACE_FILE'] = cfd_trace_file
os.environ['GEM5_CFD_PATHA_ACC_MODE'] = patha_acc_mode
os.environ['GEM5_CFD_PATHA_RESULT_BUFFER_DEPTH'] = str(patha_result_buffer_depth)
os.environ['GEM5_CFD_PATHA_STREAMING'] = '1' if patha_streaming else '0'
os.environ['GEM5_CFD_PATHA_KERNEL'] = patha_kernel
os.environ['GEM5_CFD_PATHA_INPUT_BUFFER_DEPTH'] = str(patha_input_buffer_depth)
os.environ['GEM5_CFD_PATHA_STREAM_READ_PORTS'] = str(patha_stream_read_ports)
os.environ['GEM5_CFD_PATHA_STORE_QUEUE_DEPTH'] = str(patha_store_queue_depth)
os.environ['GEM5_CFD_PATHA_ASYNC_STORE'] = '1' if patha_async_store else '0'
os.environ['GEM5_CFD_TRSV5_LAT'] = str(trsv5_lat)
os.environ['GEM5_CFD_TRSV5_MODE'] = trsv5_mode
os.environ['GEM5_CFD_TRSM5_MRHS_ENABLE'] = (
    '1' if trsm5_mrhs_enable else '0')
os.environ['GEM5_CFD_TRSM5_MRHS_LAT'] = str(trsm5_mrhs_lat)
os.environ['GEM5_CFD_TRSM5_INV_LBAR_LAT'] = str(trsm5_mrhs_dual_lat)
os.environ['GEM5_CFD_TRSM5_COEFF3_LAT'] = str(trsm5_coeff3_lat)
if (coeff_line_mvm_model not in ('serial', 'split', 'tree') or
        not 1 <= coeff_line_mul_lat <= 1024 or
        not 1 <= coeff_line_add_lat <= 1024 or
        not 1 <= coeff_line_product_depth <= 5 or
        (coeff_line_mvm_model == 'tree' and coeff_line_product_depth != 5)):
    print("ERROR: line MVM requires serial/split/tree, mul/add latency in [1,1024], "
          "and product depth in [1,5] (tree requires 5)")
    sys.exit(1)

os.environ['GEM5_CFD_LINE_MVM_SPLIT'] = (
    '1' if coeff_line_mvm_model in ('split', 'tree') else '0')
os.environ['GEM5_CFD_LINE_MVM_TREE'] = '1' if coeff_line_mvm_model == 'tree' else '0'
os.environ['GEM5_CFD_RESOURCE_STATS'] = '1' if coeff_resource_stats else '0'
os.environ['GEM5_CFD_LINE_MUL_LAT'] = str(coeff_line_mul_lat)
os.environ['GEM5_CFD_SHARED_CLUSTER'] = '1' if coeff_compute_model == 'shared' else '0'
os.environ['GEM5_CFD_LINE_TILED'] = '1' if coeff_line_storage == 'tiled' else '0'
os.environ['GEM5_CFD_LINE_OPT_LEVEL'] = str(coeff_line_opt_level)
os.environ['GEM5_CFD_RHS_ROW_PARALLEL'] = '1' if coeff_rhs_row_parallel else '0'
os.environ['GEM5_CFD_COMPLETION_DEPTH'] = str(coeff_completion_depth)
os.environ['GEM5_CFD_WARM_WRITE_DEPTH'] = str(coeff_warm_write_depth)
os.environ['GEM5_CFD_ONCHIP_TIMING'] = '1' if coeff_onchip_timing else '0'
os.environ['GEM5_CFD_ONCHIP_LATENCY'] = str(coeff_onchip_latency)
os.environ['GEM5_CFD_ONCHIP_DEPTH'] = str(coeff_onchip_depth)
os.environ['GEM5_CFD_LINE_TIMED_INTERFACE'] = '1' if coeff_interface_timing else '0'
os.environ['GEM5_CFD_LINE_INTERFACE_WIDTH'] = str(coeff_interface_width)
os.environ['GEM5_CFD_LINE_SPM_BYTES'] = str(coeff_line_spm_bytes)
os.environ['GEM5_CFD_LINE_TILE_CELLS'] = str(coeff_line_tile_cells)
os.environ['GEM5_CFD_LINE_CACHE_CELLS'] = str(coeff_line_cache_cells)
os.environ['GEM5_CFD_LINE_TILE_BUFFERS'] = str(coeff_line_tile_buffers)
os.environ['GEM5_CFD_LINE_TILE_DMA_WIDTH'] = str(coeff_line_tile_dma_width)
os.environ['GEM5_CFD_CLUSTER_MUL_LAT'] = str(coeff_cluster_mul_lat)
os.environ['GEM5_CFD_CLUSTER_ADD_LAT'] = str(coeff_cluster_add_lat)
os.environ['GEM5_CFD_CLUSTER_DIV_LAT'] = str(coeff_cluster_div_lat)
os.environ['GEM5_CFD_CLUSTER_DIV_II'] = str(coeff_cluster_div_ii)
os.environ['GEM5_CFD_CLUSTER_PRODUCT_DEPTH'] = str(coeff_cluster_product_depth)
os.environ['GEM5_CFD_LINE_ADD_LAT'] = str(coeff_line_add_lat)
os.environ['GEM5_CFD_LINE_PRODUCT_DEPTH'] = str(coeff_line_product_depth)
os.environ['GEM5_CFD_COEFF_PREPROCESS_MODEL'] = coeff_preprocess_model
os.environ['GEM5_CFD_LINE_BASE_AHEAD'] = '1' if coeff_line_base_ahead else '0'
os.environ['GEM5_CFD_LINE_EARLY_BACKWARD'] = (
    '1' if coeff_line_early_backward else '0')
os.environ['GEM5_CFD_LU5_MODEL'] = lu5_model
os.environ['GEM5_CFD_COEFF_INPUT_SLOTS'] = str(coeff_input_slots)
os.environ['GEM5_CFD_LU5_COUNT'] = str(lu5_count)
os.environ['GEM5_CFD_LU5_PENDING_DEPTH'] = str(lu5_pending_depth)
os.environ['GEM5_CFD_LU5_DIV_COUNT'] = str(lu5_div_count)
os.environ['GEM5_CFD_LU5_DIV_LAT'] = str(lu5_div_lat)
os.environ['GEM5_CFD_LU5_DIV_II'] = str(lu5_div_ii)
os.environ['GEM5_CFD_LU5_MULSUB_COUNT'] = str(lu5_mulsub_count)
os.environ['GEM5_CFD_LU5_MUL_LAT'] = str(lu5_mul_lat)
os.environ['GEM5_CFD_LU5_SUB_LAT'] = str(lu5_sub_lat)
os.environ['GEM5_CFD_COEFF3_COUNT'] = str(coeff3_event_count)
os.environ['GEM5_CFD_COEFF3_PENDING_DEPTH'] = str(coeff3_pending_depth)
os.environ['GEM5_CFD_COEFF3_RHS_LANES'] = str(coeff3_rhs_lanes)
os.environ['GEM5_CFD_COEFF3_SCHEDULE'] = coeff3_schedule
os.environ['GEM5_CFD_COEFF3_DIV_COUNT'] = str(coeff3_div_count)
os.environ['GEM5_CFD_COEFF3_DIV_LAT'] = str(coeff3_div_lat)
os.environ['GEM5_CFD_COEFF3_DIV_II'] = str(coeff3_div_ii)
os.environ['GEM5_CFD_COEFF3_DIV_MODE'] = coeff3_div_mode
os.environ['GEM5_CFD_COEFF3_MULSUB_COUNT'] = str(coeff_mulsub_count)
os.environ['GEM5_CFD_COEFF3_MUL_LAT'] = str(coeff_mul_lat)
os.environ['GEM5_CFD_COEFF3_SUB_LAT'] = str(coeff_sub_lat)
os.environ['GEM5_CFD_COEFF3_PARTIAL_OUTPUT'] = (
    '1' if coeff3_partial_output else '0')
os.environ['GEM5_CFD_LU_FORWARDING'] = '1' if lu_forwarding else '0'
os.environ['GEM5_CFD_COEFF_SPM_MODEL'] = coeff_spm_model
os.environ['GEM5_CFD_COEFF_SPM_LAYOUT'] = coeff_spm_layout
os.environ['GEM5_CFD_COEFF_SPM_OUTSTANDING'] = str(coeff_spm_outstanding)
os.environ['GEM5_CFD_LU_SOLVE_EARLY_START'] = (
    '1' if lu_solve_early_start else '0')
os.environ['GEM5_CFD_COEFF_SPM_READ_PORTS'] = str(coeff_spm_read_ports)
os.environ['GEM5_CFD_COEFF_SPM_WRITE_PORTS'] = str(coeff_spm_write_ports)
os.environ['GEM5_CFD_COEFF_SPM_WRITE_WIDTH'] = str(coeff_spm_write_width)
os.environ['GEM5_CFD_COEFF_SPM_BANKS'] = str(coeff_spm_banks)
os.environ['GEM5_CFD_COEFF_SPM_BANK_GRANULARITY'] = str(
    coeff_spm_bank_granularity)
os.environ['GEM5_CFD_COEFF_SOURCE_READ_LAT'] = str(coeff_source_read_lat)
os.environ['GEM5_CFD_COEFF_SPM_WRITE_LAT'] = str(coeff_spm_write_lat)
os.environ['GEM5_CFD_COEFF_OUTPUT_DEPTH'] = str(coeff_output_buffer_depth)
os.environ['GEM5_CFD_COEFF_DRAIN_WIDTH'] = str(coeff_drain_width)
os.environ['GEM5_CFD_COEFF_DRAIN_PORTS'] = str(coeff_drain_ports)
os.environ['GEM5_CFD_COEFF_DRAIN_LAT'] = str(coeff_drain_lat)
os.environ['GEM5_CFD_COEFF_DRAIN_OUTSTANDING'] = str(
    coeff_drain_outstanding)
os.environ['GEM5_CFD_COEFF_DRAIN_QUEUE_DEPTH'] = str(
    coeff_drain_queue_depth)
os.environ['GEM5_CFD_COEFF_COLUMN_FMA_LAT'] = str(
    coeff_column_fma_latency)
os.environ['GEM5_CFD_COEFF_COLUMN_FMA_II'] = str(coeff_column_fma_ii)
os.environ['GEM5_CFD_COEFF_COLUMN_FMA_COUNT'] = str(coeff_column_fma_count)
os.environ['GEM5_CFD_COEFF_COLUMN_FMA_QUEUE_DEPTH'] = str(
    coeff_column_fma_queue_depth)
os.environ['GEM5_CFD_FORWARD_COMBINE_LAT'] = str(forward_combine_latency)
os.environ['GEM5_CFD_FORWARD_COMBINE_II'] = str(forward_combine_ii)
os.environ['GEM5_CFD_FORWARD_COMBINE_COUNT'] = str(forward_combine_count)
os.environ['GEM5_CFD_FORWARD_COMBINE_QUEUE_DEPTH'] = str(
    forward_combine_queue_depth)
os.environ['GEM5_CFD_COEFF_TRACE_ENABLE'] = (
    '1' if coeff_preprocess_trace_enable else '0')
os.environ['GEM5_CFD_COEFF_TRACE_CELLS'] = str(
    coeff_preprocess_trace_cells)
os.environ['GEM5_CFD_COEFF_TRACE_FILE'] = coeff_preprocess_trace_file
os.environ['GEM5_CFD_LUSGS_LINEBUF_ENABLE'] = (
    '1' if lusgs_linebuf_enable else '0')
os.environ['GEM5_CFD_LUSGS_LINEBUF_ENTRIES'] = str(lusgs_linebuf_entries)
os.environ['GEM5_CFD_LUSGS_STEP3_STAGE'] = lusgs_step3_stage
os.environ['GEM5_CFD_LUSGS_CONTEXTS'] = str(lusgs_contexts)
os.environ['GEM5_CFD_LUSGS_TILE_CELLS'] = str(lusgs_tile_cells)
os.environ['GEM5_CFD_LUSGS_VEC5_LAT'] = str(lusgs_vec5_lat)
os.environ['GEM5_CFD_LUSGS_CONTROLLER_TRACE_ENABLE'] = (
    '1' if lusgs_controller_trace_enable else '0')
os.environ['GEM5_CFD_LUSGS_CONTROLLER_TRACE_LINES'] = str(
    lusgs_controller_trace_lines)
os.environ['GEM5_CFD_LUSGS_CONTROLLER_TRACE_CELLS'] = str(
    lusgs_controller_trace_cells)
os.environ['GEM5_CFD_LUSGS_CONTROLLER_TRACE_FILE'] = (
    lusgs_controller_trace_file)
os.environ['GEM5_CFD_LUSGS_STEP4_ENABLE'] = '1' if lusgs_step4_enable else '0'
os.environ['GEM5_CFD_LUSGS_CONTROLLER_IMPL'] = (
    'event' if lusgs_step4_enable else 'functional')
os.environ['GEM5_CFD_LUSGS_EVENT_TRACE_ENABLE'] = (
    '1' if lusgs_event_trace_enable else '0')
os.environ['GEM5_CFD_LUSGS_EVENT_TRACE_LINES'] = str(
    lusgs_event_trace_lines)
os.environ['GEM5_CFD_LUSGS_EVENT_TRACE_CELLS'] = str(
    lusgs_event_trace_cells)
os.environ['GEM5_CFD_LUSGS_EVENT_TRACE_FILE'] = lusgs_event_trace_file
os.environ['GEM5_CFD_LUSGS_EVENT_WATCHDOG_CYCLES'] = str(
    lusgs_step4_watchdog_cycles)
os.environ['GEM5_CFD_LUSGS_EVENT_WATCHDOG_TEST_HANG'] = (
    '1' if lusgs_error_case == 'watchdog' else '0')
os.environ['GEM5_CFD_LUSGS_TRSV5_TEST_HANG'] = (
    '1' if lusgs_error_case in ('trsv-watchdog',
                                'trsv-watchdog-then-legal') else '0')
os.environ['GEM5_CFD_LUSGS_TRSV5_TEST_STALE_GENERATION'] = (
    '1' if lusgs_error_case == 'trsv-stale-generation' else '0')
os.environ['GEM5_CFD_LUSGS_TRSV5_TEST_BAD_REQUEST_ID'] = (
    '1' if lusgs_error_case == 'trsv-bad-request-id-then-legal' else '0')
os.environ['GEM5_CFD_LUSGS_VEC5_TEST_DUPLICATE_COMPLETION'] = (
    '1' if lusgs_error_case == 'vec5-duplicate-then-legal' else '0')
os.environ['GEM5_CFD_LUSGS_STEP4_PATHA_REAL'] = (
    '1' if lusgs_step4_patha_real else '0')
os.environ['GEM5_CFD_LUSGS_STEP4_STAGE'] = lusgs_step4_stage
os.environ['GEM5_CFD_LUSGS_STEP4_TRSV_REAL'] = (
    '1' if lusgs_step4_trsv_real else '0')
os.environ['GEM5_CFD_LUSGS_STEP4_VEC5_REAL'] = (
    '1' if lusgs_step4_vec5_real else '0')
os.environ['GEM5_CFD_LUSGS_TRSV5_EVENT_COUNT'] = str(trsv5_event_count)
os.environ['GEM5_CFD_LUSGS_TRSV5_DIV_COUNT'] = str(trsv5_div_count)
os.environ['GEM5_CFD_LUSGS_TRSV5_FMA_COUNT'] = str(trsv5_fma_count)
os.environ['GEM5_CFD_LUSGS_TRSV5_DIV_LAT'] = str(trsv5_div_lat)
os.environ['GEM5_CFD_LUSGS_TRSV5_FMA_LAT'] = str(trsv5_fma_lat)
os.environ['GEM5_CFD_LUSGS_TRSV5_LOAD_LAT'] = str(trsv5_load_lat)
os.environ['GEM5_CFD_LUSGS_TRSV5_RESULT_LAT'] = str(trsv5_result_lat)
os.environ['GEM5_CFD_LUSGS_TRSV5_FORWARDING'] = (
    '1' if trsv5_forwarding else '0')
os.environ['GEM5_CFD_LUSGS_TRSV5_QUEUE_DEPTH'] = str(trsv5_queue_depth)
os.environ['GEM5_CFD_LUSGS_VEC5_EVENT_COUNT'] = str(vec5_event_count)
os.environ['GEM5_CFD_LUSGS_VEC5_LANES'] = str(vec5_lanes)
os.environ['GEM5_CFD_LUSGS_VEC5_COPY_LAT'] = str(vec5_copy_lat)
os.environ['GEM5_CFD_LUSGS_VEC5_SUB_LAT'] = str(vec5_sub_lat)
os.environ['GEM5_CFD_LUSGS_VEC5_AXPY_LAT'] = str(vec5_axpy_lat)
os.environ['GEM5_CFD_LUSGS_VEC5_INITIATION_INTERVAL'] = str(
    vec5_initiation_interval)
os.environ['GEM5_CFD_LUSGS_VEC5_QUEUE_DEPTH'] = str(vec5_queue_depth)
os.environ['GEM5_CFD_LUSGS_STEP4_ENGINE_BACKPRESSURE_TEST'] = (
    '1' if lusgs_step4_engine_backpressure_test else '0')
os.environ['GEM5_CFD_LUSGS_PATHA_MATLD_LAT'] = str(lmat_lat)
os.environ['GEM5_CFD_LUSGS_PATHA_MATLD_COUNT'] = str(readport_count)
os.environ['GEM5_CFD_LUSGS_PATHA_DOTP_COUNT'] = str(patha_dotp_count)
os.environ['GEM5_CFD_LUSGS_PATHA_PACK_LAT'] = str(patha_pack_lat)
os.environ['GEM5_CFD_LUSGS_PATHA_PACK_COUNT'] = str(patha_pack_count)

# ================================================================
# Theoretical Performance Baseline (v4.0 SPM + Cross-Domain Regs)
# ================================================================
# NEW ARCHITECTURE:
#   SPM (Scratchpad Memory): Local SRAM, no cache coherence overhead
#   DMA: Explicit memory transfer from main memory to SPM
#   dotp_row: Write to X16-X20 (int regs), zero RAW dependencies
#   pack_acc: Harvest X16-X20 -> Z register
#
# Theoretical reference:
#   - documented target: 8 cycles/matvec
#   - steady lower bound with 1 pipelined dotp engine: 6 cycles/matvec
# lmat5_spm now uses the CPU timing memory path, so SPM read traffic is
# observable through system.spm when the SPM address range is mapped.
# ================================================================
from cfd_reporting import THEORY, bind as bind_reporting
_reporting = bind_reporting(globals())
analyze_stats = _reporting.analyze_stats
parse_stats_file = _reporting.parse_stats_file
print_performance_report = _reporting.print_performance_report
calc_theoretical_cycles = _reporting.calc_theoretical_cycles
if stats_file:
    print(f"=== Parsing existing stats: {stats_file} ===")
    if not invoked_by_split_wrapper:
        print("NOTE: run_cfd_dsa.py is a compatibility entry; prefer "
              "run_cfd_patha.py, run_cfd_pathb.py, or run_cfd_pathc.py "
              "for path-specific reports.")
    stats = parse_stats_file(stats_file)
    print_performance_report(stats, bench_iters)
    sys.exit(0)

# ================================================================
# System Setup
# ================================================================
print("=== CFD DSA -- gem5 Simulation ===")
if not invoked_by_split_wrapper:
    print("NOTE: run_cfd_dsa.py is a compatibility entry; prefer "
          "run_cfd_patha.py for Path A, run_cfd_pathb.py for Path B, "
          "or run_cfd_pathc.py for Path C.")
print(f"Binary:     {binary_path}")
print(f"Max ticks:  {max_ticks}")
print(f"CPU type:   {cpu_type}")
print(f"CPU freq:   2GHz")
print(f"Bench iters:{bench_iters}")
if report_focus == 'pathb':
    print(f"SME PathB pipe: opLat={sme_pipe_lat}, count={sme_pipe_count}, "
          f"pipelined={sme_pipe_pipelined}")
if report_focus == 'pathc':
    print(f"SME PathC outer: opLat={sme_outer_lat}, count={sme_outer_count}, "
          f"pipelined={sme_outer_pipelined}, full_array={sme_outer_full_array}")
print(f"O3 window:  IQ={iq_entries}, ROB={rob_entries}")
print(f"SPM mode:   {spm_access_mode}")
if spm_access_mode in ('local-ideal', 'local-realistic') + local_event_modes:
    print("Local SPM:  "
          f"lat={local_spm_lat}, ports={local_spm_read_ports}, "
          f"width={local_spm_read_width}B, outstanding={local_spm_outstanding}, "
          f"queue={local_spm_queue_size}, banks={local_spm_banks}, "
          f"bank_gran={local_spm_bank_granularity}B, "
          f"bank_mapping={local_spm_bank_mapping}, "
          f"bank_conflict={local_spm_bank_conflict}, "
          f"z_wb_ports={local_spm_z_wb_ports}, "
          f"real_backpressure={local_spm_real_backpressure}")
if report_focus != 'patha':
    print(f"ZA state:   {za_state_mode}")
if report_focus != 'pathc':
    print("Path A:     "
          f"spm_mode={patha_spm_mode}, read_width={patha_spm_read_width}B, "
          f"store_mode={patha_store_mode}, store_stride={patha_store_stride}B, "
          f"acc_mode={patha_acc_mode}, buf_depth={patha_result_buffer_depth}, "
          f"dotp_lat={patha_dotp_lat}, dotp_count={patha_dotp_count}, "
          f"pack_count={patha_pack_count}, "
          f"unroll={patha_unroll}, "
          f"streaming={int(patha_streaming)}, kernel={patha_kernel}, "
          f"input_depth={patha_input_buffer_depth}, "
          f"stream_ports={patha_stream_read_ports}, "
          f"store_q={patha_store_queue_depth}, "
          f"async_store={int(patha_async_store)}, "
          f"matld_readports={patha_matld_readports if patha_matld_readports is not None else readport_count}")
if is_lusgs_binary:
    print("LU-SGS:     "
          f"lines={lusgs_lines}, cells={lusgs_cells}, "
          f"interleave={lusgs_interleave}, mode={lusgs_mode}, "
          f"reference_only={int(lusgs_reference_only)}, "
          f"patha_only={int(lusgs_patha_only)}, check={int(lusgs_check)}, "
          f"linebuf={int(lusgs_linebuf_enable)}, "
          f"linebuf_entries={lusgs_linebuf_entries}")
if is_trsv5_binary:
    print("TRSV5:      "
          f"lat={trsv5_lat}, count={trsv5_count}, mode={trsv5_mode}, "
          f"trace={int(trsv5_trace_enable)}")
if is_lusgs_step3_binary:
    print("LU-SGS S3:  "
          f"stage={lusgs_step3_stage}, contexts={lusgs_contexts}, "
          f"tile_cells={lusgs_tile_cells}, update_q={int(lusgs_update_q)}, "
          f"omega={lusgs_omega}, ctrl_count={lusgs_controller_count}, "
          f"vec5_count={lusgs_vec5_count}, vec5_lat={lusgs_vec5_lat}")
if is_lusgs_step4_binary:
    print("LU-SGS S4:  "
          f"event={int(lusgs_step4_enable)}, stage={lusgs_step4_stage}, "
          f"contexts={lusgs_contexts}, tile_cells={lusgs_tile_cells}, "
          f"patha_real={int(lusgs_step4_patha_real)}, "
          f"trsv_real={int(lusgs_step4_trsv_real)}, "
          f"vec5_real={int(lusgs_step4_vec5_real)}, "
          f"event_trace={int(lusgs_event_trace_enable)}")
    if lusgs_step4_stage == 'C':
        print("LU-SGS S4C: "
              f"trsv engines/div/fma={trsv5_event_count}/"
              f"{trsv5_div_count}/{trsv5_fma_count}, "
              f"div/fma/load/result lat={trsv5_div_lat}/"
              f"{trsv5_fma_lat}/{trsv5_load_lat}/{trsv5_result_lat}, "
              f"trsv_q={trsv5_queue_depth}, vec5 count/lanes="
              f"{vec5_event_count}/{vec5_lanes}, vec5_q={vec5_queue_depth}")
print(f"lmat5_spm:  opLat={lmat_lat}, ReadPort count={readport_count}, "
      f"pipelined={lmat_pipelined}")
if report_focus != 'patha':
    print(f"MOVA:       opLat={mova_lat}")

system = System()
system.clk_domain = SrcClockDomain(clock='2GHz', voltage_domain=VoltageDomain())
system.mem_mode = 'timing'
system.cache_line_size = 64

# Memory configuration: parameterized DRAM + SPM (0x70000000, 256KB)
# Using two non-overlapping SimpleMemory devices for explicit timing.
# PhysicalMemory covers both ranges for functional/debug access.
SPM_BASE = 0x70000000
SPM_SIZE = 0x40000  # 256KB (generous SPM allocation)

system.mem_ranges = [AddrRange(dram_size),
                     AddrRange(SPM_BASE, size=SPM_SIZE)]

system.membus = SystemXBar(width=64, clk_domain=system.clk_domain)
system.system_port = system.membus.cpu_side_ports

# DRAM controller: parameterized capacity, 30ns latency
system.dram = SimpleMemory(
    range=AddrRange(dram_size),
    bandwidth='32GB/s',
    latency='30ns'
)
system.dram.port = system.membus.mem_side_ports

# Create ISA with 512-bit SVE VL (4 quadwords = 8 doubles).
# 5xFP64 requires >=320 bits, so 128-bit default is insufficient.
SVE_VL_SE_QUADS = 4
if SVE_VL_SE_QUADS < 3:
    print("ERROR: sve_vl_se must be >= 3 (>=384 bits) for 5xFP64 lanes.")
    sys.exit(1)
cpu_isa = ArmISA(sve_vl_se=SVE_VL_SE_QUADS, sme_vl_se=SVE_VL_SE_QUADS)
if cpu_type == 'o3':
    system.cpu = O3CPU(
        isa=[cpu_isa],

        # IQ/ROB are exposed for bottleneck sweeps. Path C is sensitive to
        # keeping lmat, ZERO/FMOPA/MOVA, and stores visible together.
        instQueues=IQUnit(numEntries=iq_entries),

        # LQ/SQ/ROB: balanced windows
        LQEntries=64,
        SQEntries=64,
        numROBEntries=rob_entries,
        numPhysIntRegs=512,
        numPhysVecRegs=512,
        numPhysMatRegs=256,

        # Wide pipeline
        fetchWidth=8,
        decodeWidth=8,
        renameWidth=8,
        issueWidth=8,
        wbWidth=8,
        commitWidth=8
    )
elif cpu_type == 'minor':

    system.cpu = MinorCPU(isa=[cpu_isa])
else:
    system.cpu = ArmTimingSimpleCPU(isa=[cpu_isa])

pipe_fu_configured = False
outer_fu_configured = False
lmat_fu_configured = False
dotp_fu_configured = False
pack_fu_configured = False
patha_stream_ld_fu_configured = False
patha_store5_fu_configured = False
trsv5_fu_configured = False
trsm5_mrhs_fu_configured = False
trsm5_mrhs_dual_fu_configured = False
trsm5_coeff3_fu_configured = False
vec5_fu_configured = False
lusgs_ctrl_fu_configured = False
mova_fu_configured = False
if cpu_type == 'o3':
    fu_pools = []
    for iq in system.cpu.instQueues:
        if hasattr(iq, 'fuPool'):
            fu_pools.append(iq.fuPool)
    for fu_pool in fu_pools:
        for fu in fu_pool.FUList:
            for op_desc in getattr(fu, 'opList', []):
                op_class = str(op_desc.opClass)
                if 'CFDSMEPipeStep' in op_class:
                    fu.count = sme_pipe_count
                    op_desc.opLat = sme_pipe_lat
                    op_desc.pipelined = sme_pipe_pipelined
                    pipe_fu_configured = True
                elif 'CFDSMEZaOuterStep' in op_class:
                    fu.count = sme_outer_count
                    op_desc.opLat = sme_outer_lat
                    op_desc.pipelined = sme_outer_pipelined
                    outer_fu_configured = True
                elif 'CFDDSADotp' in op_class:
                    fu.count = patha_dotp_count
                    op_desc.opLat = patha_dotp_lat
                    op_desc.pipelined = True
                    dotp_fu_configured = True
                elif 'CFDDSAPack' in op_class:
                    fu.count = patha_pack_count
                    op_desc.opLat = patha_pack_lat
                    op_desc.pipelined = True
                    pack_fu_configured = True
                elif 'CFDDSAStreamLd' in op_class:
                    fu.count = patha_stream_read_ports
                    op_desc.opLat = lmat_lat
                    op_desc.pipelined = True
                    patha_stream_ld_fu_configured = True
                elif 'CFDDSAStore5' in op_class:
                    fu.count = max(1, min(patha_store_queue_depth, 2))
                    op_desc.opLat = 1
                    op_desc.pipelined = True
                    patha_store5_fu_configured = True
                elif 'CFDDSATrsv5' in op_class:
                    fu.count = trsv5_count
                    op_desc.opLat = trsv5_lat
                    op_desc.pipelined = True
                    trsv5_fu_configured = True
                elif 'CFDDSATrsm5Mrhs' in op_class:
                    fu.count = trsm5_mrhs_count
                    op_desc.opLat = trsm5_mrhs_lat
                    op_desc.pipelined = True
                    trsm5_mrhs_fu_configured = True
                elif 'CFDDSATrsm5InvLbar' in op_class:
                    fu.count = trsm5_mrhs_dual_count
                    op_desc.opLat = trsm5_mrhs_dual_lat
                    op_desc.pipelined = True
                    trsm5_mrhs_dual_fu_configured = True
                elif 'CFDDSATrsm5Coeff3' in op_class:
                    fu.count = trsm5_coeff3_count
                    op_desc.opLat = trsm5_coeff3_lat
                    op_desc.pipelined = True
                    trsm5_coeff3_fu_configured = True
                elif 'CFDDSAVec5' in op_class:
                    fu.count = (vec5_event_count if is_lusgs_step2_binary
                                else lusgs_vec5_count)
                    op_desc.opLat = (pretransform_vec5_lat
                                     if is_lusgs_step2_binary
                                     else lusgs_vec5_lat)
                    op_desc.pipelined = True
                    vec5_fu_configured = True
                elif 'CFDDSALusgsCtrl' in op_class:
                    fu.count = lusgs_controller_count
                    op_desc.opLat = 1
                    op_desc.pipelined = False
                    lusgs_ctrl_fu_configured = True
                elif 'CFDDSAMatLd' in op_class:
                    fu.count = readport_count
                    op_desc.opLat = lmat_lat
                    op_desc.pipelined = lmat_pipelined
                    lmat_fu_configured = True
                elif op_class == 'MatrixMov':
                    op_desc.opLat = mova_lat
                    op_desc.pipelined = True
                    mova_fu_configured = True
if cpu_type == 'o3' and report_focus == 'pathb' and not pipe_fu_configured:
    print("Warning: CFDSMEPipeStep FUDesc was not found for Path B sweep override")
if cpu_type == 'o3' and report_focus == 'pathc' and not outer_fu_configured:
    print("Warning: CFDSMEZaOuterStep FUDesc was not found for Path C sweep override")
if cpu_type == 'o3' and not lmat_fu_configured:
    print("Warning: CFDDSAMatLd FUDesc was not found for sweep override")
if cpu_type == 'o3' and not dotp_fu_configured:
    print("Warning: CFDDSADotp FUDesc was not found for Path A sweep override")
if cpu_type == 'o3' and not pack_fu_configured:
    print("Warning: CFDDSAPack FUDesc was not found for Path A sweep override")
if cpu_type == 'o3' and patha_streaming and not patha_stream_ld_fu_configured:
    print("Warning: CFDDSAStreamLd FUDesc was not found for Path A streaming override")
if cpu_type == 'o3' and patha_streaming and not patha_store5_fu_configured:
    print("Warning: CFDDSAStore5 FUDesc was not found for Path A streaming override")
if cpu_type == 'o3' and is_trsv5_binary and not trsv5_fu_configured:
    print("Warning: CFDDSATrsv5 FUDesc was not found for TRSV5 override")
if cpu_type == 'o3' and is_lusgs_step2_binary and not trsm5_mrhs_fu_configured:
    print("Warning: CFDDSATrsm5Mrhs FUDesc was not found for Step2 override")
if cpu_type == 'o3' and is_lusgs_step2_binary and not trsm5_mrhs_dual_fu_configured:
    print("Warning: CFDDSATrsm5InvLbar FUDesc was not found for Step2 override")
if cpu_type == 'o3' and is_lusgs_step2_binary and not trsm5_coeff3_fu_configured:
    print("Warning: CFDDSATrsm5Coeff3 FUDesc was not found for Step2 override")
if cpu_type == 'o3' and (is_lusgs_step3_binary or is_lusgs_step4_binary) and not vec5_fu_configured:
    print("Warning: CFDDSAVec5 FUDesc was not found for LU-SGS override")
if cpu_type == 'o3' and (is_lusgs_step3_binary or is_lusgs_step4_binary) and not lusgs_ctrl_fu_configured:
    print("Warning: CFDDSALusgsCtrl FUDesc was not found for LU-SGS override")
if cpu_type == 'o3' and not mova_fu_configured:
    print("Warning: MatrixMov FUDesc was not found for sweep override")
system.cpu.clk_domain = system.clk_domain
system.cpu.local_spm = CfdLocalSpm()
system.cpu.createInterruptController()

class L1ICache(Cache):
    size = '32KiB'; assoc = 8; tag_latency = 1; data_latency = 1
    response_latency = 2; mshrs = 32; tgts_per_mshr = 20
    is_read_only = True
    def connectCPU(self, cpu): self.cpu_side = cpu.icache_port
    def connectBus(self, bus): self.mem_side = bus.cpu_side_ports

class L1DCache(Cache):
    size = '32KiB'; assoc = 8; tag_latency = 1; data_latency = 1
    response_latency = 2; mshrs = 128; tgts_per_mshr = 20
    def connectCPU(self, cpu): self.cpu_side = cpu.dcache_port
    def connectBus(self, bus): self.mem_side = bus.cpu_side_ports

class L2Cache(Cache):
    size = '512KiB'; assoc = 16; tag_latency = 4; data_latency = 4
    response_latency = 4; mshrs = 128; tgts_per_mshr = 20
    def connectCPUSideBus(self, bus): self.cpu_side = bus.mem_side_ports
    def connectMemSideBus(self, bus): self.mem_side = bus.cpu_side_ports

system.l1i = L1ICache()
system.l1d = L1DCache()
system.l1i.connectCPU(system.cpu)
system.l1d.connectCPU(system.cpu)

system.toL2Bus = L2XBar(width=64, clk_domain=system.clk_domain)
system.l1i.connectBus(system.toL2Bus)
system.l1d.connectBus(system.toL2Bus)
# A non-caching DMA endpoint emits partial WriteReq packets. They must
# enter a cache before the coherent fabric: a bare WriteReq cannot snoop
# and invalidate a dirty CPU line. Match configs/common/Caches.py IOCache
# parameters so both reads and writes use the classic coherence protocol.
if coeff_spm_model == 'dma':
    system.coeff_dma_cache = Cache(
        size='1KiB', assoc=8, tag_latency=50, data_latency=50,
        response_latency=50, mshrs=20, tgts_per_mshr=12,
        addr_ranges=system.mem_ranges)
    system.cpu.local_spm.dma_port = system.coeff_dma_cache.cpu_side
    system.coeff_dma_cache.mem_side = system.toL2Bus.cpu_side_ports
else:
    system.cpu.local_spm.dma_port = system.toL2Bus.cpu_side_ports

system.l2 = L2Cache()
system.l2.connectCPUSideBus(system.toL2Bus)
system.l2.connectMemSideBus(system.membus)

# ================================================================
# SPM (Scratchpad Memory) Configuration
# ================================================================
# SPM: Local SRAM with dedicated address range at 0x70000000. lmat5_spm
# uses normal timing reads to this uncacheable range, allowing system.spm
# read-byte stats to match committed CFDDSAMatLd * 40B.
# ================================================================

# SPM as dedicated SimpleMemory device (uncacheable, deterministic, 1ns latency)
system.spm = SimpleMemory(
    range=AddrRange(SPM_BASE, size=SPM_SIZE),
    bandwidth=spm_bandwidth,
    latency=spm_latency
)
system.spm.port = system.membus.mem_side_ports

print(f"\n=== Memory Configuration ===")
print(f"DRAM:        {dram_size} @ 30ns")
print(f"SPM Size:    {SPM_SIZE/1024:.0f} KB")
print(f"SPM Address: 0x{SPM_BASE:08X} - 0x{SPM_BASE + SPM_SIZE - 1:08X}")
print(f"SPM Latency: {spm_latency} deterministic")
print(f"SPM Bandwidth:{spm_bandwidth}")
print(f"=========================\n")

if not os.path.exists(binary_path):
    print(f"ERROR: Binary '{binary_path}' not found.")
    sys.exit(1)

process = Process()
process_cmd = [binary_path, str(bench_iters)]
if is_patha_binary:
    process_cmd.append(patha_store_mode)
    process_cmd.append(str(patha_store_stride))
    process_cmd.append(str(patha_unroll))
    process_cmd.append(patha_acc_mode)
    process_cmd.append(str(patha_result_buffer_depth))
    process_cmd.append('1' if patha_streaming else '0')
    process_cmd.append(patha_kernel)
if is_trsv5_binary:
    process_cmd.extend([
        '--trsv5-lat', str(trsv5_lat),
        '--trsv5-count', str(trsv5_count),
        '--trsv5-mode', trsv5_mode,
        '--trsv5-trace-solves', str(trsv5_trace_solves),
        '--trsv5-trace-file', trsv5_trace_file,
    ])
    if trsv5_trace_enable:
        process_cmd.append('--trsv5-trace-enable')
if is_lusgs_binary:
    process_cmd.extend([
        '--lusgs-lines', str(lusgs_lines),
        '--lusgs-cells', str(lusgs_cells),
        '--lusgs-interleave', str(lusgs_interleave),
        '--lusgs-check', '1' if lusgs_check else '0',
        '--lusgs-trace-lines', str(lusgs_trace_lines),
        '--lusgs-trace-cells', str(lusgs_trace_cells),
        '--lusgs-linebuf-enable', '1' if lusgs_linebuf_enable else '0',
        '--lusgs-linebuf-entries', str(lusgs_linebuf_entries),
    ])
    if is_lusgs_step2_binary:
        process_cmd.extend([
            '--lusgs-mode', lusgs_mode,
            '--trsm5-mrhs-enable', '1' if trsm5_mrhs_enable else '0',
            '--trsm5-mrhs-lat', str(trsm5_mrhs_lat),
            '--trsm5-mrhs-count', str(trsm5_mrhs_count),
            '--trsm5-mrhs-dual-enable',
            '1' if trsm5_mrhs_dual_enable else '0',
            '--trsm5-mrhs-dual-lat', str(trsm5_mrhs_dual_lat),
            '--trsm5-mrhs-dual-count', str(trsm5_mrhs_dual_count),
            '--trsm5-coeff3-enable',
            '1' if trsm5_coeff3_enable else '0',
            '--trsm5-coeff3-lat', str(trsm5_coeff3_lat),
            '--trsm5-coeff3-count', str(trsm5_coeff3_count),
            '--vec5-lat', str(pretransform_vec5_lat),
            '--vec5-count', str(vec5_event_count),
            '--pretransform-fallback-trsv5',
            '1' if pretransform_fallback_trsv5 else '0',
            '--pretransform-diag-epsilon', str(pretransform_diag_epsilon),
            '--pretransform-per-sweep',
            '1' if pretransform_per_sweep else '0',
            '--pretransform-output-buffer-enable',
            '1' if pretransform_output_buffer_enable else '0',
            '--pretransform-output-buffer-depth',
            str(pretransform_output_buffer_depth),
            '--pretransform-spm-slots', str(pretransform_spm_slots),
            '--pretransform-overlap-enable',
            '1' if pretransform_overlap_enable else '0',
            '--pretransform-use-barrier',
            '1' if pretransform_use_barrier else '0',
            '--pretransform-validate', pretransform_validate,
            '--coeff-preprocess-spm-slots',
            str(coeff_preprocess_spm_slots),
            '--coeff-output-buffer-depth',
            str(coeff_output_buffer_depth),
            '--coefficient-update-interval',
            str(coefficient_update_interval if coeff_line_cache_allowed else 1),
            '--coeff-preprocess-model', coeff_preprocess_model,
            '--lu5-model', lu5_model,
            '--coeff-validation', coeff_validation,
            '--coeff-validation-sample-rate',
            str(coeff_validation_sample_rate),
            '--coeff-cancel-test', '1' if coeff_cancel_test else '0',
            '--coeff-cache-contract-test', '1' if coeff_cache_contract_test else '0',
            '--coeff-streaming-enable',
            '1' if coeff_streaming_enable else '0',
            '--coeff-stream-line-autonomous',
            '1' if coeff_stream_line_autonomous else '0',
            '--coeff-base-solve', coeff_base_solve,
            '--coeff-line-cache-allowed', '1' if coeff_line_cache_allowed else '0',
            '--coeff-line-opt-level', str(coeff_line_opt_level),
            '--coeff-stream-window', str(coeff_stream_window),
            '--coeff-stream-retire-mode', coeff_stream_retire_mode,
            '--coeff-stream-boundary-mask',
            '1' if coeff_stream_boundary_mask else '0',
            '--coeff-stream-dinv-consumer', coeff_stream_dinv_consumer,
            '--coeff-stream-lbar-consumer', coeff_stream_lbar_consumer,
            '--coeff-stream-ubar-consumer', coeff_stream_ubar_consumer,
            '--coeff-column-fma-latency', str(coeff_column_fma_latency),
            '--coeff-column-fma-ii', str(coeff_column_fma_ii),
            '--coeff-column-fma-count', str(coeff_column_fma_count),
            '--coeff-column-fma-queue-depth',
            str(coeff_column_fma_queue_depth),
            '--forward-combine-latency', str(forward_combine_latency),
            '--forward-combine-ii', str(forward_combine_ii),
            '--forward-combine-count', str(forward_combine_count),
            '--forward-combine-queue-depth',
            str(forward_combine_queue_depth),
            '--coeff3-partial-output',
            '1' if coeff3_partial_output else '0',
            '--lu-forwarding', '1' if lu_forwarding else '0',
            '--ubar-inplace', '1' if ubar_inplace else '0',
            '--lusgs-preprocess-auto',
            '1' if lusgs_pretransform_auto else '0',
            '--lusgs-coeff-expected-sweeps', str(lusgs_expected_sweeps),
        ])
    if is_lusgs_step3_binary or is_lusgs_step4_binary:
        guest_lusgs_stage = (lusgs_step4_stage if is_lusgs_step4_binary
                             else lusgs_step3_stage)
        process_cmd.extend([
            '--lusgs-step3-stage', guest_lusgs_stage,
            '--lusgs-contexts', str(lusgs_contexts),
            '--lusgs-tile-cells', str(lusgs_tile_cells),
            '--lusgs-update-q', '1' if lusgs_update_q else '0',
            '--lusgs-omega', str(lusgs_omega),
            '--lusgs-controller-count', str(lusgs_controller_count),
            '--lusgs-vec5-count', str(lusgs_vec5_count),
            '--lusgs-vec5-lat', str(lusgs_vec5_lat),
            '--lusgs-controller-trace-lines',
            str(lusgs_controller_trace_lines),
            '--lusgs-controller-trace-cells',
            str(lusgs_controller_trace_cells),
            '--lusgs-controller-trace-file',
            lusgs_controller_trace_file,
        ])
        if lusgs_controller_trace_enable:
            process_cmd.append('--lusgs-controller-trace-enable')
        if is_lusgs_step4_binary:
            process_cmd.extend([
                '--lusgs-event-trace-lines', str(lusgs_event_trace_lines),
                '--lusgs-event-trace-cells', str(lusgs_event_trace_cells),
                '--lusgs-event-trace-file', lusgs_event_trace_file,
            ])
            if lusgs_event_trace_enable:
                process_cmd.append('--lusgs-event-trace-enable')
            process_cmd.extend([
                '--lusgs-step4-patha-real',
                '1' if lusgs_step4_patha_real else '0',
                '--lusgs-step4-trsv-real',
                '1' if lusgs_step4_trsv_real else '0',
                '--lusgs-step4-vec5-real',
                '1' if lusgs_step4_vec5_real else '0',
            ])
            if lusgs_step4_snapshot_test:
                process_cmd.append('--lusgs-step4-snapshot-test')
            if lusgs_step4_lifecycle_test:
                process_cmd.append('--lusgs-step4-lifecycle-test')
            if lusgs_step4_wrong_path_launch:
                process_cmd.append('--lusgs-step4-wrong-path-launch')
            if lusgs_step4_wrong_path_wait:
                process_cmd.append('--lusgs-step4-wrong-path-wait')
            if lusgs_step4_overlap_test:
                process_cmd.append('--lusgs-step4-overlap-test')
            process_cmd.extend([
                '--lusgs-step4-overlap-iters',
                str(lusgs_step4_overlap_iters),
            ])
        if lusgs_error_case:
            process_cmd.extend(['--lusgs-error-case', lusgs_error_case])
    if lusgs_reference_only:
        process_cmd.append('--lusgs-reference-only')
    if lusgs_patha_only:
        process_cmd.append('--lusgs-patha-only')
    if lusgs_trace_enable:
        process_cmd.append('--lusgs-trace-enable')
process.cmd = process_cmd
system.cpu.workload = process
system.cpu.createThreads()
system.workload = SEWorkload.init_compatible(binary_path)

root = Root(full_system=False, system=system)
m5.instantiate()

# Map benchmark SPM VA directly to SPM PA. Marking the PTE uncacheable keeps
# DMA-style stores coherent with SPM semantics in SE mode.
process.map(SPM_BASE, SPM_BASE, SPM_SIZE, cacheable=False)
print(f"Mapped SPM VA 0x{SPM_BASE:08X} -> PA 0x{SPM_BASE:08X} ({SPM_SIZE//1024} KB, uncacheable)")

print("Starting simulation...")
exit_event = m5.simulate(max_ticks)
print(f"\nSimulation complete: {exit_event.getCause()} @ tick {m5.curTick()}")
m5.stats.dump()
stats_path = os.path.join(m5.options.outdir, 'stats.txt')
print(f"Stats saved to {stats_path}")

if os.path.exists(stats_path):
    stats = parse_stats_file(stats_path)
    print_performance_report(stats, bench_iters)
