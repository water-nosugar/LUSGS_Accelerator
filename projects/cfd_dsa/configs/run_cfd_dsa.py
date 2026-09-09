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
  --sme-fmopa-lat=<n>  ZA accumulator forwarding token latency (default: 1)
  --sme-fmopa-array-lat=<n> internal SME array latency label (default: 4)
  --sme-fmopa-count=<n>
  --sme-fmopa-pipelined=<true|false>
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
sme_fmopa_lat = 1
sme_fmopa_array_lat = 4
sme_fmopa_count = 1
sme_fmopa_pipelined = True
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
coeff_streaming_enable = False
coeff_stream_line_autonomous = False
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
                                'sme-fmopa-lat=', 'sme-fmopa-count=',
                                'sme-fmopa-array-lat=',
                                'sme-fmopa-pipelined=',
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
                                'coeff-streaming-enable=',
                                'coeff-stream-line-autonomous=',
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
            print("  --sme-fmopa-lat=<n>  ZA forwarding token latency: 1,2,3,4 (default: 1)")
            print("  --sme-fmopa-array-lat=<n> internal SME array latency label (default: 4)")
            print("  --sme-fmopa-count=<n> FMOPA step engines: 1 or 2 (default: 1)")
            print("  --sme-fmopa-pipelined=<true|false> (default: true)")
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
        elif opt == '--sme-fmopa-lat':
            sme_fmopa_lat = int(arg)
        elif opt == '--sme-fmopa-array-lat':
            sme_fmopa_array_lat = int(arg)
        elif opt == '--sme-fmopa-count':
            sme_fmopa_count = int(arg)
        elif opt == '--sme-fmopa-pipelined':
            sme_fmopa_pipelined = parse_bool(arg)
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
        elif opt == '--coeff-streaming-enable':
            coeff_streaming_enable = parse_bool(arg)
        elif opt == '--coeff-stream-line-autonomous':
            coeff_stream_line_autonomous = parse_bool(arg)
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
                           'frontier-aware'):
    print("ERROR: --coeff3-schedule must be column-major, step-major, "
          "round-robin-ready, ready-first, or frontier-aware")
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
if (coeff_line_mvm_model not in ('serial', 'split') or
        not 1 <= coeff_line_mul_lat <= 1024 or
        not 1 <= coeff_line_add_lat <= 1024 or
        not 1 <= coeff_line_product_depth <= 5):
    print("ERROR: line MVM requires serial/split, mul/add latency in [1,1024], "
          "and product depth in [1,5]")
    sys.exit(1)

os.environ['GEM5_CFD_LINE_MVM_SPLIT'] = (
    '1' if coeff_line_mvm_model == 'split' else '0')
os.environ['GEM5_CFD_RESOURCE_STATS'] = '1' if coeff_resource_stats else '0'
os.environ['GEM5_CFD_LINE_MUL_LAT'] = str(coeff_line_mul_lat)
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

def calc_theoretical_cycles(approach='CFD', cache_level='L1'):
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

# ================================================================
# Performance Analysis
# ================================================================
def _stat_value(stats_text, names, dtype=float):
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

def _add_stat(stats, stats_text, key, names, dtype=float):
    value = _stat_value(stats_text, names, dtype)
    if value is not None:
        stats[key] = value

def analyze_stats(stats_text):
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
        _add_stat(stats, stats_text, key, names, dtype)

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
        matld_extra = patha_matld_extra_per_run() if dotp else (
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

    if (patha_kernel != 'stream-dsa' and
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
            stats['sme_fmopa_token_lat'] = sme_fmopa_lat
            stats['sme_fmopa_array_lat'] = sme_fmopa_array_lat
            stats['sme_fmopa_count'] = sme_fmopa_count
            stats['sme_pipe_lat'] = sme_pipe_lat
            stats['sme_pipe_count'] = sme_pipe_count
            stats['sme_outer_lat'] = sme_outer_lat
            stats['sme_outer_count'] = sme_outer_count
            stats['sme_outer_full_array'] = 1 if sme_outer_full_array else 0
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
                sme_pipe_lat if stats.get('method') == 'SME_ZA_SPATIAL_PIPE'
                else sme_outer_lat if stats.get('method') == 'SME_ZA_OUTER_PIPE'
                else sme_fmopa_lat
            )
            token_compute_lb = THEORY['sme_fmopa_per_matvec'] * token_lat
            load_issue_lb = THEORY['cfd_matld_per_matvec']
            load_bandwidth_lb = (
                (THEORY['cfd_matld_per_matvec'] + max(1, local_spm_read_ports) - 1) //
                max(1, local_spm_read_ports)
            )
            stats['sme_forwarding_compute_lb'] = token_compute_lb
            stats['sme_load_bandwidth_lb'] = load_bandwidth_lb
            stats['sme_explicit_lmat_slots'] = load_issue_lb
            stats['sme_forwarding_combined_lb'] = max(
                token_compute_lb, load_issue_lb
            )
        else:
            stream_dsa_kernel = patha_kernel == 'stream-dsa'
            stats['expected_cfd_matld'] = (
                0 if stream_dsa_kernel else
                inferred_matvecs * THEORY['cfd_matld_per_matvec'] +
                patha_matld_extra_per_run()
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
            patha_physical = patha_read_count * patha_spm_read_width
            patha_result_store = (
                40 if patha_store_mode in ('pred40', 'store5') else 64
            )
            stats['implied_spm_read_bytes'] = patha_logical
            stats['patha_spm_mode'] = patha_spm_mode
            stats['patha_spm_read_width'] = patha_spm_read_width
            stats['patha_logical_bytes_read'] = patha_logical
            stats['patha_physical_bytes_read'] = patha_physical
            stats['patha_num_spm_reads'] = patha_read_count
            if patha_physical:
                stats['patha_payload_utilization'] = patha_logical / patha_physical
            if stats.get('implied_spm_read_bytes', 0):
                stats['patha_local_spm_coverage'] = (
                    patha_logical / stats['implied_spm_read_bytes'])
            stats['patha_store_mode'] = patha_store_mode
            stats['patha_store_stride'] = patha_store_stride
            stats['patha_acc_mode'] = patha_acc_mode
            stats['patha_result_buffer_depth'] = patha_result_buffer_depth
            stats['patha_dotp_lat'] = patha_dotp_lat
            stats['patha_dotp_count'] = patha_dotp_count
            stats['patha_pack_count'] = patha_pack_count
            stats['patha_unroll'] = patha_unroll
            stats['patha_streaming'] = int(patha_streaming)
            stats['patha_kernel'] = patha_kernel
            stats['patha_input_buffer_depth'] = patha_input_buffer_depth
            stats['patha_stream_read_ports'] = patha_stream_read_ports
            stats['patha_store_queue_depth'] = patha_store_queue_depth
            stats['patha_async_store'] = int(patha_async_store)
            stats['patha_result_store_bytes_per_matvec'] = patha_result_store
            stats['implied_result_write_bytes'] = inferred_matvecs * patha_result_store
            stats['patha_result_layout_bytes'] = inferred_matvecs * patha_store_stride
            stats['patha_store_width'] = patha_result_store
            stats['patha_store_instructions'] = (
                stats.get('patha_store5_committed', 0)
                if patha_store_mode == 'store5' else inferred_matvecs
            )
            stats['patha_pack_acc_count'] = stats.get(
                'cfd_pack_committed',
                0 if stream_dsa_kernel else
                inferred_matvecs * THEORY['cfd_pack_per_matvec'])
            stats['patha_pack_acc_per_matvec'] = (
                0 if stream_dsa_kernel else THEORY['cfd_pack_per_matvec']
            )
            stats['patha_matld_readports'] = (
                patha_stream_read_ports if stream_dsa_kernel else readport_count
            )
            stats['patha_spm_banks'] = local_spm_banks
            stats['patha_spm_bank_granularity'] = local_spm_bank_granularity
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

def print_performance_report(stats, requested_bench_iters=10000000):
    method = stats.get('method', 'DOTP_ROW')
    focus = report_focus
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
                print(f"    Path A SPM mode:    {stats.get('patha_spm_mode', patha_spm_mode)}")
                print(f"    Path A read width:  {stats.get('patha_spm_read_width', patha_spm_read_width)} B physical")
                print(f"    Path A read ports:  {stats.get('patha_matld_readports', readport_count)}")
                print(f"    Path A SPM banks:   {stats.get('patha_spm_banks', local_spm_banks)}")
                print(f"    Path A bank gran:   {stats.get('patha_spm_bank_granularity', local_spm_bank_granularity)} B")
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
        print(f"    Path A store mode:  {stats.get('patha_store_mode', patha_store_mode)}")
        print(f"    Path A store width: {stats.get('patha_store_width', result_store_bytes)} bytes")
        print(f"    Path A store stride:{stats.get('patha_store_stride', patha_store_stride)} bytes")
        print(f"    Path A acc mode:    {stats.get('patha_acc_mode', patha_acc_mode)}")
        print(f"    Path A buf depth:   {stats.get('patha_result_buffer_depth', patha_result_buffer_depth)}")
        print(f"    Path A dotp lat:    {stats.get('patha_dotp_lat', patha_dotp_lat)}")
        print(f"    Path A dotp count:  {stats.get('patha_dotp_count', patha_dotp_count)}")
        print(f"    Path A pack count:  {stats.get('patha_pack_count', patha_pack_count)}")
        print(f"    Path A unroll:      {stats.get('patha_unroll', patha_unroll)}")
        print(f"    Path A streaming:   {stats.get('patha_streaming', int(patha_streaming))} ({stats.get('patha_kernel', patha_kernel)})")
        print(f"    Path A input depth: {stats.get('patha_input_buffer_depth', patha_input_buffer_depth)}")
        print(f"    Path A stream ports:{stats.get('patha_stream_read_ports', patha_stream_read_ports)}")
        print(f"    Path A store queue: {stats.get('patha_store_queue_depth', patha_store_queue_depth)} async={stats.get('patha_async_store', int(patha_async_store))}")
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
            matld_extra = patha_matld_extra_per_run()
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
        print(f"    Internal array lat.: {stats.get('sme_fmopa_array_lat', sme_fmopa_array_lat)} cycles")
        print(f"    ZA token latency:    {stats.get('sme_fmopa_token_lat', sme_fmopa_lat)} cycles")
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
        print(f"    Pipe step latency:   {stats.get('sme_pipe_lat', sme_pipe_lat)} cycles")
        print(f"    Pipe engines:        {stats.get('sme_pipe_count', sme_pipe_count)}")
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
        print(f"    Outer step latency:  {stats.get('sme_outer_lat', sme_outer_lat)} cycles")
        print(f"    Outer engines:       {stats.get('sme_outer_count', sme_outer_count)}")
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
            if stats.get('patha_store_stride', patha_store_stride) == 40:
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

# ================================================================
# Stats File Parser
# ================================================================
def parse_stats_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    segments = re.findall(
        r'-+ Begin Simulation Statistics -+\n(.*?)\n-+ End Simulation Statistics\s+-+',
        content, re.DOTALL)
    if not segments:
        return analyze_stats(content)

    analyzed = [analyze_stats(seg) for seg in segments]

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
if report_focus != 'patha':
    print(f"SME FMOPA step: opLat={sme_fmopa_lat}, count={sme_fmopa_count}, "
          f"pipelined={sme_fmopa_pipelined}")
    print(f"SME FMOPA array latency label: {sme_fmopa_array_lat}")
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

fmopa_fu_configured = False
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
                if 'CFDSMEFmopaStep' in op_class:
                    fu.count = sme_fmopa_count
                    op_desc.opLat = sme_fmopa_lat
                    op_desc.pipelined = sme_fmopa_pipelined
                    fmopa_fu_configured = True
                elif 'CFDSMEPipeStep' in op_class:
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
if cpu_type == 'o3' and not fmopa_fu_configured:
    print("Warning: CFDSMEFmopaStep FUDesc was not found for sweep override")
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
# Stage 6b DMA joins the coherent CPU-side fabric so initialized guest
# buffers are visible without bypassing dirty L1 data.
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
            str(coefficient_update_interval),
            '--coeff-preprocess-model', coeff_preprocess_model,
            '--lu5-model', lu5_model,
            '--coeff-validation', coeff_validation,
            '--coeff-validation-sample-rate',
            str(coeff_validation_sample_rate),
            '--coeff-cancel-test', '1' if coeff_cancel_test else '0',
            '--coeff-streaming-enable',
            '1' if coeff_streaming_enable else '0',
            '--coeff-stream-line-autonomous',
            '1' if coeff_stream_line_autonomous else '0',
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
