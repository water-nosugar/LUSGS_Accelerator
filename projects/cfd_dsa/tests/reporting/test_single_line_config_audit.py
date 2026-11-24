"""Capacity boundaries and executed-resource consistency of the config audit."""
import _support
import copy
import json
from pathlib import Path
import tempfile
import unittest

from single_line_config_audit import (Effective, audit_configuration, audit_run,
    capacity_audit, command_arguments)


def fixture(cells=65):
    options = dict(coeff_line_storage='tiled', coeff_compute_model='shared',
        lusgs_cells=cells, coeff_input_slots=5, lu5_pending_depth=1,
        coeff3_pending_depth=1, coeff_output_buffer_depth=5,
        coeff_line_opt_level=13, coeff_rhs_row_parallel=True,
        coeff_rhs_product_depth=16, coeff_cluster_mul_count=5,
        coeff_cluster_mul_lat=3, coeff_cluster_add_lat=4,
        coeff_cluster_div_count=1, coeff_cluster_div_lat=12,
        coeff_cluster_div_ii=4, coeff_spm_outstanding=64,
        coeff_completion_depth=4, coeff_warm_write_depth=8,
        coeff_onchip_timing=True, coeff_onchip_depth=64,
        coeff_output_coalesce=True, coeff_line_tile_cells=16,
        coeff_line_tile_buffers=2, coeff_line_spm_bytes=196608,
        coeff_line_cache_cells=128, coeff_base_solve='direct',
        coeff_cache_policy='adaptive', coeff_line_cache_allowed=True,
        coeff_divider_model='pipelined', coeff_divider_inflight=0,
        coeff_divider_result_depth=2, coeff_divider_writeback_ii=1,
        lu5_count=4, coeff3_event_count=4,
        coeff_spm_banks=4, coeff_spm_bank_granularity=8,
        coeff_spm_read_ports=1, coeff_spm_write_ports=1,
        coeff_dma_cache_size=1024, coeff_dma_cache_mshrs=64,
        coeff_dma_cache_write_buffers=32, coeff_dma_cache_tag_lat=50,
        coeff_dma_cache_data_lat=50, coeff_dma_cache_response_lat=50)
    resolved = dict(options=options, clock='2GHz',
        controller_environment={'GEM5_CFD_TICKS_PER_CYCLE': '500'},
        memory={'model': 'SimpleMemory', 'latency': '30ns', 'bandwidth': '32GB/s'})
    config = {'system': {'clk_domain': {'clock': [500]},
        'coeff_dma_cache': {'type': 'Cache', 'size': 1024, 'assoc': 8,
            'mshrs': 64, 'write_buffers': 32, 'tag_latency': 50,
            'data_latency': 50, 'response_latency': 50, 'sequential_access': False},
        'mem_ctrl': {'type': 'SimpleMemory', 'path': 'system.mem_ctrl',
            'latency': 30000, 'bandwidth': '29.000000', 'range': '0:134217728'}}}
    return resolved, config


class ArgumentAuditTests(unittest.TestCase):
    def test_hardware_contract_distinguishes_bounded_ports_and_physical_area(self):
        resolved,config=fixture()
        resolved['options'].update(coeff_rf_read_words=16,coeff_rf_write_words=8,
            coeff_onchip_arb_tasks=4,coeff_rf_broadcast=True,coeff_rf_split_writeback=True)
        contract=audit_configuration(resolved,config)['hardware_contract']
        self.assertTrue(contract['fully_bounded_rf_and_arbiter'])
        self.assertTrue(contract['register_file']['scalar_broadcast'])
        self.assertFalse(contract['register_file']['split_issue'])
        self.assertIsNone(contract['spm']['physical_sram_bytes'])
        self.assertIsNone(contract['physical_area'])

    def test_timing_divider_does_not_claim_detailed_pipeline_storage(self):
        resolved, config = fixture()
        resolved['options']['coeff_divider_model'] = 'timing'
        result = audit_configuration(resolved, config)
        divider = result['arithmetic']['divider']
        self.assertIsNone(divider['effective_inflight_capacity'])
        self.assertIsNone(divider['result_fifo_depth'])
        self.assertIsNone(divider['writeback_ii_cycles'])

    def test_mixed_syntax_duplicate_overrides_and_negative_value(self):
        last, duplicates = command_arguments(['gem5.opt', '--redirect-stdout',
            '--coeff-line-tile-cells', '16', '--coeff-line-tile-cells=32',
            '--coefficient-update-interval', '-1'])
        self.assertEqual(last['--coeff-line-tile-cells'], '32')
        self.assertEqual(last['--coefficient-update-interval'], '-1')
        self.assertEqual(duplicates['--coeff-line-tile-cells']['values'], ['16', '32'])
        self.assertIs(last['--redirect-stdout'], True)


class CapacityAuditTests(unittest.TestCase):
    def test_tree_rf_scalar_slots_match_independent_storage_ledger(self):
        from check_single_line_storage import lifecycle_reserve
        for depth in [16,32,60]:
            changes=dict(coeff_line_spm_bytes=280000,coeff_local_products=True,
                coeff_rf_read_words=24,coeff_rf_write_words=12,coeff_rhs_product_depth=depth)
            before=self.capacity(**changes)
            after=self.capacity(**changes,coeff_rhs_tree_rf_temp=True)
            paid=16*depth+128
            self.assertEqual(after['reservation_bytes']-before['reservation_bytes'],paid)
            resolved,_=fixture();resolved['options'].update(changes)
            config={'--'+key.replace('_','-'):str(int(value)) if isinstance(value,bool) else str(value)
                    for key,value in resolved['options'].items()}
            self.assertEqual(lifecycle_reserve(dict(config,**{'--coeff-rhs-tree-rf-temp':'1'}))-
                             lifecycle_reserve(config),paid)

    def test_factor_bus_matches_storage_ledger_and_cache_capacity_boundary(self):
        from check_single_line_storage import lifecycle_reserve, fixed_control_reserve
        changes = dict(coeff_line_spm_bytes=270720, coeff_local_products=True,
            coeff_rhs_factor_reuse=True, coeff_rhs_factor_slots=8,
            coeff_rf_read_words=24, coeff_rf_write_words=12)
        before = self.capacity(**changes)
        after = self.capacity(**changes, coeff_rhs_factor_fanout=True)
        self.assertEqual(after['reservation_bytes']-before['reservation_bytes'],256)
        for enabled, expected in [(False,before),(True,after)]:
            resolved,_ = fixture()
            resolved['options'].update(changes,coeff_rhs_factor_fanout=enabled)
            config = {'--'+key.replace('_','-'):
                str(int(value)) if isinstance(value,bool) else str(value)
                for key,value in resolved['options'].items()}
            ledger = 8192*sum(int(config[key]) for key in ('--coeff-input-slots',
                '--lu5-pending-depth','--coeff3-pending-depth','--coeff-output-buffer-depth'))
            ledger += fixed_control_reserve(config)+lifecycle_reserve(config)
            ledger += 64*int(config['--coeff-rhs-product-depth'])+256
            self.assertEqual(ledger,expected['reservation_bytes'])
        boundary = before['reservation_bytes']+before['logical_tile_bytes']+600*65
        old = self.capacity(**dict(changes,coeff_line_spm_bytes=boundary))
        new = self.capacity(**dict(changes,coeff_line_spm_bytes=boundary),
                            coeff_rhs_factor_fanout=True)
        self.assertEqual(old['cache_mode'],'full')
        self.assertNotEqual(new['cache_mode'],'full')
        self.assertLessEqual(new['total_budgeted_bytes'],boundary)

    def test_timed_line_summary_budgets_state_without_extra_request_slots(self):
        old=self.capacity(1024,coeff_input_slots=4,coeff_line_cache_allowed=False)
        new=self.capacity(1024,coeff_input_slots=4,coeff_line_cache_allowed=False,
                          coeff_record_policy='line-summary')
        self.assertEqual(new['reservation_bytes'],old['reservation_bytes']+128)
        parts={r['name']:r['bytes'] for r in new['reservation_components']}
        self.assertEqual(parts['summary_hash_engine'],128)
        self.assertEqual(parts['request_state_slots'],next(r['bytes'] for r in old['reservation_components'] if r['name']=='request_state_slots'))

    def test_explicit_final_fifo_is_separate_and_fully_budgeted(self):
        old=self.capacity(1024,coeff_input_slots=4,coeff_line_cache_allowed=False)
        new=self.capacity(1024,coeff_input_slots=4,coeff_line_cache_allowed=False,
                          coeff_line_output_depth=64)
        self.assertEqual(new['reservation_bytes'],old['reservation_bytes']+64*64)
        parts={r['name']:r['bytes'] for r in new['reservation_components']}
        self.assertEqual(parts['base_control_feedback'],16384)
        self.assertEqual(parts['line_output_fifo'],4096)

    def test_final_fifo_can_exhaust_the_long_line_budget(self):
        resolved,config=fixture(1024)
        resolved['options'].update(coeff_input_slots=4,coeff_line_cache_allowed=False,
            coeff_tile_prefetch_buffers=4,coeff_onchip_depth=96,
            coeff_spm_outstanding=128,coeff_line_output_depth=128)
        with self.assertRaisesRegex(ValueError,'capacity exceeds'):
            audit_configuration(resolved,config)

    def test_extra_prefetch_payload_and_metadata_both_consume_capacity(self):
        result=self.capacity(1024,coeff_input_slots=4,coeff_line_cache_allowed=False,
                             coeff_tile_prefetch_buffers=2)
        self.assertEqual(result['reservation_bytes'],146560+2*512)
        self.assertEqual(result['logical_tile_bytes'],4*16*240)
        self.assertEqual(result['total_budgeted_bytes'],162944)

    def capacity(self, n=65, **changes):
        resolved, _ = fixture(n)
        resolved['options'].update(changes)
        return capacity_audit(Effective(resolved))

    def test_independent_component_sum_and_logical_versus_physical(self):
        result = self.capacity()
        expected = {'request_state_slots': 98304, 'base_control_feedback': 16384,
            'rhs_products': 1280, 'completion_records': 2048, 'lifecycle_fixed': 2048,
            'warm_write_queue': 2048, 'dma_snapshots': 4096,
            'onchip_access_queue': 16384, 'onchip_request_context': 1536,
            'onchip_fixed': 1024, 'backward_operand_slots': 1024,
            'local_readiness': 512, 'coalesced_output_snapshots': 8192}
        self.assertEqual({r['name']: r['bytes'] for r in result['reservation_components']}, expected)
        self.assertEqual(result['reservation_bytes'], 154880)
        self.assertEqual(result['total_budgeted_bytes'], 195160)
        self.assertEqual(result['logical_payload_bytes'], 40280)
        self.assertIsNone(result['physical_sram_bytes'])

    def test_cache_capacity_phase_transitions(self):
        for n, mode, resident in [(64, 'full', 1280), (65, 'full', 1280),
                (67, 'full', 1280), (68, 'partial', 7680),
                (85, 'partial', 7680), (86, 'off', 7680),
                (128, 'off', 7680), (129, 'off', 7680)]:
            with self.subTest(n=n):
                result = self.capacity(n)
                self.assertEqual(result['cache_mode'], mode)
                self.assertEqual(result['logical_tile_bytes'], resident)
                self.assertLessEqual(result['total_budgeted_bytes'], 196608)
        self.assertEqual(self.capacity()['cache_boundaries'],
                         {'full': 67, 'adaptive_any': 85, 'adaptive_partial': 85})

    def test_cache_disabled_cannot_silently_shrink_tiles(self):
        result = self.capacity(coeff_line_cache_allowed=False)
        self.assertEqual(result['cache_mode'], 'off')
        self.assertEqual(result['logical_tile_bytes'], 7680)
        self.assertEqual(result['total_budgeted_bytes'], 162560)

    def test_short_line_buffers_are_not_assumed_full(self):
        result = self.capacity(2)
        self.assertEqual(result['logical_tile_bytes'], 160)
        self.assertEqual(result['logical_coefficient_cache_bytes'], 1200)

    def test_extra_shared_multiplier_is_budgeted(self):
        result = self.capacity(coeff_cluster_mul_count=6)
        self.assertEqual(result['reservation_bytes'], 155008)
        self.assertEqual(next(r['bytes'] for r in result['reservation_components']
                              if r['name'] == 'extra_multiplier_pipeline'), 128)

    def test_direct_eight_seven_expansion_pays_for_both_pipelines(self):
        base=self.capacity()
        expanded=self.capacity(coeff_cluster_mul_count=8,coeff_cluster_add_count=7)
        self.assertEqual(expanded['reservation_bytes']-base['reservation_bytes'],640)

    def test_drain_descriptors_do_not_allocate_extra_request_contexts(self):
        base=self.capacity()
        expanded=self.capacity(coeff_drain_task_depth=12)
        self.assertEqual(expanded['reservation_bytes']-base['reservation_bytes'],448)
        def request_bytes(result):
            return next(r['bytes'] for r in result['reservation_components']
                        if r['name']=='request_state_slots')
        self.assertEqual(request_bytes(expanded),request_bytes(base))

    def test_over_capacity_rejected(self):
        resolved, config = fixture()
        resolved['options']['coeff_line_spm_bytes'] = 160000
        with self.assertRaisesRegex(ValueError, 'capacity exceeds'):
            audit_configuration(resolved, config)


class ExecutedResourceTests(unittest.TestCase):
    report = ('CFD_RESOURCE_STATS pool=shared_mul count=5 lat=3 ii=1\n'
              'CFD_RESOURCE_STATS pool=shared_add count=5 lat=4 ii=1\n'
              'CFD_RESOURCE_STATS pool=shared_div count=1 lat=12 ii=4\n'
              'CFD_LINE_STORAGE token=1 cells=65 reserved=154880 resident=1280 '
              'capacity=196608 cache_bytes=39000 tile_y_only=1\n')

    def test_legacy_divider_flag_does_not_override_shared_effective_ii(self):
        resolved, config = fixture()
        resolved['options']['lu5_div_ii'] = 12
        result = audit_configuration(resolved, config, self.report)
        self.assertEqual(result['arithmetic']['pools']['div']['initiation_interval_cycles'], 4)
        self.assertEqual(result['arithmetic']['divider']['effective_inflight_capacity'], 3)
        self.assertEqual(result['banked_spm']['peak_read_bytes_per_cycle'], 32)
        self.assertEqual(result['memory']['instantiated_simple_memories'][0]['bandwidth_ticks_per_byte'], '29.000000')
        self.assertEqual(result['observed_storage'][0]['cells'], 65)

    def test_reject_env_option_divergence(self):
        resolved, config = fixture()
        resolved['controller_environment']['GEM5_CFD_CLUSTER_DIV_II'] = '12'
        with self.assertRaisesRegex(ValueError, 'resolved option differs'):
            audit_configuration(resolved, config)

    def test_reject_observed_fu_mismatch(self):
        resolved, config = fixture()
        with self.assertRaisesRegex(ValueError, 'observed shared_div ii'):
            audit_configuration(resolved, config, self.report.replace('ii=4', 'ii=12'))

    def test_reject_observed_capacity_mismatch(self):
        resolved, config = fixture()
        with self.assertRaisesRegex(ValueError, 'resident differs'):
            audit_configuration(resolved, config, self.report.replace('resident=1280', 'resident=7680'))

    def test_reject_instantiated_endpoint_or_clock_divergence(self):
        resolved, config = fixture()
        for field, value in [('mshrs', 16), ('tag_latency', 1)]:
            changed = copy.deepcopy(config)
            changed['system']['coeff_dma_cache'][field] = value
            with self.assertRaisesRegex(ValueError, 'DMA cache '+field):
                audit_configuration(resolved, changed)
        changed = copy.deepcopy(config)
        changed['system']['clk_domain']['clock'] = [1000]
        with self.assertRaisesRegex(ValueError, 'ticks-per-cycle'):
            audit_configuration(resolved, changed)

    def test_non_strict_returns_failed_audit_for_archiving(self):
        resolved, config = fixture()
        result = audit_configuration(resolved, config,
            self.report.replace('reserved=154880', 'reserved=136448'), strict=False)
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(result['errors'])

    def test_controller_mode_must_match_resolved_profile(self):
        resolved, config = fixture()
        resolved['controller_environment']['GEM5_CFD_SHARED_CLUSTER'] = '0'
        with self.assertRaisesRegex(ValueError, 'controller mode'):
            audit_configuration(resolved, config)

    def test_run_integration_reads_artifacts_and_preserves_inputs(self):
        resolved, config = fixture()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            data = {'resolved_experiment.json': json.dumps(resolved),
                'config.json': json.dumps(config), 'simout.txt': self.report,
                'manifest.json': json.dumps({'command': ['gem5.opt',
                    '--coeff-cluster-div-ii=12', '--coeff-cluster-div-ii', '4']})}
            for name, text in data.items():
                (path/name).write_text(text)
            result = audit_run(path)
            self.assertEqual(result['status'], 'passed')
            source = result['parameter_sources']['coeff_cluster_div_ii']
            self.assertEqual(source['launch_value'], '4')
            self.assertTrue(source['origin'].startswith('explicit'))
            self.assertEqual(result['repeated_launch_arguments']['--coeff-cluster-div-ii']['effective'], '4')
            self.assertEqual({p.name: p.read_text() for p in path.iterdir()}, data)

    def test_ddr_primary_distinguished_from_auxiliary_simple_spm(self):
        resolved, config = fixture()
        resolved['memory'] = dict(model='MemCtrl', preset='DDR4_2400_8x8',
            geometry={'device_size': 1073741824, 'banks_per_rank': 16},
            timing_ticks={'tCK': 833, 'tRCD': 14161},
            interface={'page_policy': 'open_adaptive', 'read_buffer_size': 64},
            controller={'mem_sched_policy': 'frfcfs',
                        'static_frontend_latency_ticks': 10000})
        config['system']['spm'] = config['system'].pop('mem_ctrl')
        config['system']['spm']['path'] = 'system.spm'
        config['system']['dram'] = dict(type='MemCtrl', path='system.dram',
            mem_sched_policy='frfcfs', static_frontend_latency=10000,
            dram=dict(type='DRAMInterface', path='system.dram.dram',
                device_size=1073741824, banks_per_rank=16,
                tCK=833, tRCD=14161, page_policy='open_adaptive', read_buffer_size=64))
        result = audit_configuration(resolved, config)
        memory = result['memory']
        self.assertEqual(memory['primary_memory']['type'], 'MemCtrl')
        self.assertEqual(memory['instantiated_simple_memories'][0]['path'], 'system.spm')
        self.assertEqual(memory['instantiated_dram_interfaces'][0]['path'], 'system.dram.dram')
        self.assertEqual(len(memory['instantiated_memory_controllers']), 1)
        changed = copy.deepcopy(config)
        changed['system']['dram']['dram']['tRCD'] = 1
        with self.assertRaisesRegex(ValueError, 'timing_ticks.tRCD'):
            audit_configuration(resolved, changed)
        changed = copy.deepcopy(config)
        changed['system']['dram'] = changed['system']['spm']
        with self.assertRaisesRegex(ValueError, 'primary system.dram type'):
            audit_configuration(resolved, changed)

    def test_simple_snapshot_quantized_values_must_match_instantiated_memory(self):
        resolved, config = fixture()
        resolved['memory'].update(latency_ticks=30000, bandwidth_ticks_per_byte=29.0)
        audit_configuration(resolved, config)
        resolved['memory']['bandwidth_ticks_per_byte'] = 31.25
        with self.assertRaisesRegex(ValueError, 'bandwidth_ticks_per_byte'):
            audit_configuration(resolved, config)


if __name__ == '__main__':
    unittest.main()
