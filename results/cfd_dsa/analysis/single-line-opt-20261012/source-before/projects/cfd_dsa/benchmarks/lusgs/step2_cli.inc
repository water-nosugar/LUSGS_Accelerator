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
    cfg->coeff_cache_contract_test = 0;
    cfg->coeff_streaming_enable = 0;
    cfg->coeff_stream_line_autonomous = 0;
    cfg->coeff_base_solve = "inverse";
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
                         "--coeff-cache-contract-test")) != NULL) {
            cfg->coeff_cache_contract_test = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-streaming-enable")) != NULL) {
            cfg->coeff_streaming_enable = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-stream-line-autonomous")) != NULL) {
            cfg->coeff_stream_line_autonomous = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-line-cache-allowed")) != NULL) {
            cfg->coeff_line_cache_allowed = parse_bool_arg(value);
        } else if ((value = option_value(argc, argv, &idx, arg,
                         "--coeff-base-solve")) != NULL) {
            if (strcmp(value, "inverse") && strcmp(value, "direct")) {
                fprintf(stderr, "invalid --coeff-base-solve: %s\n", value);
                exit(2);
            }
            cfg->coeff_base_solve = value;
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
    if (!strcmp(cfg->coeff_base_solve, "direct") &&
        (!cfg->run_step2_pretransform_raw_stream ||
         !cfg->coeff_stream_line_autonomous || cfg->lines != 1 ||
         strcmp(cfg->coeff_stream_ubar_consumer, "direct"))) {
        fprintf(stderr, "direct base solve requires one autonomous streaming "
                "line and direct Ubar\n");
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
