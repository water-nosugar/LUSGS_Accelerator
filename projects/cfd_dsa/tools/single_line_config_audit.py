#!/usr/bin/env python3
"""Audit effective single-line resources without treating a budget as SRAM area.

``audit_run(path, strict=True)`` is the runner integration API.  It reads the
resolved controller environment, instantiated gem5 config, launch arguments and
observed storage/resource reports.  It never changes experiment inputs.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import sys

CONFIGS = Path(__file__).resolve().parents[1] / 'configs'
if str(CONFIGS) not in sys.path:
    sys.path.insert(0, str(CONFIGS))
from single_line_contract import line_storage_bytes, input_slot_count_valid, input_r_priority_allowed

SOURCE = {
    'capacity': 'src/arch/arm/cfd_coeff_preprocess_tiles.inc:initializeLineTile',
    'cache': 'projects/cfd_dsa/configs/single_line_contract.py:line_storage_bytes',
    'arithmetic': 'src/arch/arm/cfd_coeff_preprocess_controller.cc:configure',
    'configuration': 'projects/cfd_dsa/configs/run_cfd_dsa.py',
}


def command_arguments(command):
    """Return last-wins long options and every occurrence (including equals)."""
    values, occurrences = {}, defaultdict(list)
    i = 0
    while i < len(command):
        arg = str(command[i])
        if arg.startswith('--') and arg != '--':
            key, equal, value = arg.partition('=')
            if not equal:
                if i+1 < len(command) and not str(command[i+1]).startswith('--'):
                    i += 1
                    value = str(command[i])
                else:
                    value = True
            values[key] = value
            occurrences[key].append(value)
        i += 1
    duplicates = {key: {'values': vals, 'effective': vals[-1],
                        'rule': 'last occurrence wins'}
                  for key, vals in occurrences.items() if len(vals) > 1}
    return values, duplicates


def _reports(text, prefix):
    return [dict(re.findall(r'(\w+)=([^\s]+)', line))
            for line in text.splitlines() if line.startswith(prefix+' ')]


class Effective:
    def __init__(self, resolved):
        self.options = resolved.get('options', {})
        self.env = resolved.get('controller_environment', {})
        self.sources = {}
        self.errors = []

    def get(self, key, env=None, default=None, cast=int):
        """The controller environment is authoritative for controller fields."""
        values = []
        if env in self.env:
            values.append(('controller_environment.'+env, self.env[env]))
        if key in self.options:
            values.append(('options.'+key, self.options[key]))
        if not values:
            if default is None:
                raise ValueError('missing effective parameter '+key)
            values.append(('controller implementation constant/default', default))
        def convert(value):
            if cast is bool:
                if str(value).lower() not in ('true', 'false', '1', '0'):
                    raise ValueError('invalid boolean '+key)
                return str(value).lower() in ('true', '1')
            return cast(value)
        converted = [convert(value) for _, value in values]
        if len(set(converted)) != 1:
            self.errors.append(key+': resolved option differs from controller environment')
        self.sources[key] = {'source': values[0][0], 'value': converted[0]}
        return converted[0]


def _backend_bit(value):
    value = str(value)
    if value in ('sme-za', '1'):
        return True
    if value in ('event-scalar', '0'):
        return False
    raise ValueError(f'invalid coefficient compute backend {value}')


def reservation_components(effective):
    """Independently decompose the existing conservative capacity contract."""
    e = effective
    slots = e.get('coeff_input_slots', 'GEM5_CFD_COEFF_INPUT_SLOTS')
    lu_pending = e.get('lu5_pending_depth', 'GEM5_CFD_LU5_PENDING_DEPTH')
    rhs_pending = e.get('coeff3_pending_depth', 'GEM5_CFD_COEFF3_PENDING_DEPTH')
    output = e.get('coeff_output_buffer_depth', 'GEM5_CFD_COEFF_OUTPUT_DEPTH')
    level = e.get('coeff_line_opt_level', 'GEM5_CFD_LINE_OPT_LEVEL')
    capacity = slots+lu_pending+rhs_pending+output
    components = []
    def add(name, amount, equation, role):
        components.append(dict(name=name, bytes=amount, equation=equation,
            classification='abstract_conservative_reservation', role=role,
            physical_implementation='unmapped; may require registers, FIFO, SRAM and control bits'))
    add('request_state_slots', capacity*8192,
        f'({slots}+{lu_pending}+{rhs_pending}+{output}) * 8192', 'live request/terminal state')
    add('base_control_feedback', 16384, '16384', 'control, MVM, neighbour feedback and interfaces')
    if e.get('coeff_record_policy',default='per-cell',cast=str) in ('line-summary','cold-summary'):
        engines=e.get('coeff_summary_engines','GEM5_CFD_SUMMARY_ENGINES',1)
        depth=e.get('coeff_summary_depth','GEM5_CFD_SUMMARY_DEPTH',0)
        sme = e.get('coeff_compute_backend', 'GEM5_CFD_SME_ZA', 'event-scalar', cast=_backend_bit)
        max_depth, max_engines = (24, 16) if sme else (8, 4)
        if (not 0 <= depth <= max_depth or not 1 <= engines <= max_engines or
                (depth and depth < engines) or (not depth and engines != 1)):
            e.errors.append('invalid bounded summary engines/snapshot configuration')
        add('summary_hash_engine',128*engines,f'128 * {engines}',
            'registered 40-word integer engines, 2 cycles per word; no FP units')
        if depth:
            add('summary_snapshots',512*depth,f'512 * {depth}',
                '40-word immutable snapshots, ownership, 64-byte ingress port, entry read mux and control; copy needs five ticks')
    # Old archived configurations predate the explicit FIFO allocation. Keep
    # their ledger interpretable without silently rewriting historical runs.
    if ('coeff_line_output_depth' in e.options or
            'GEM5_CFD_LINE_OUTPUT_DEPTH' in e.env):
        depth = e.get('coeff_line_output_depth', 'GEM5_CFD_LINE_OUTPUT_DEPTH')
        if not 1 <= depth <= 128:
            e.errors.append('final output FIFO depth must be in [1,128]')
        add('line_output_fifo', depth*64, f'{depth} * 64',
            'independent final-vector snapshots, held until DMA acknowledgement')
    parallel = e.get('coeff_rhs_row_parallel', 'GEM5_CFD_RHS_ROW_PARALLEL', False, bool)
    drain_depth=e.get('coeff_drain_task_depth','GEM5_CFD_DRAIN_TASK_DEPTH',output) or output
    if not 1 <= drain_depth <= 64:
        e.errors.append('drain descriptor depth must be 1..64')
    if drain_depth > output:
        add('extra_drain_descriptors',64*(drain_depth-output),
            f'64 * ({drain_depth} - {output})',
            'bounded drain descriptors; payload remains in existing owned request slots')
    if parallel:
        products = e.get('coeff_rhs_product_depth', 'GEM5_CFD_RHS_PRODUCT_DEPTH', 60)
        add('rhs_products', products*64+256, f'{products} * 64 + 256', 'RHS row products and control')
    mul = e.get('coeff_cluster_mul_count', 'GEM5_CFD_CLUSTER_MUL_COUNT', 5)
    mul_latency = e.get('coeff_cluster_mul_lat', 'GEM5_CFD_CLUSTER_MUL_LAT', 3)
    if mul > 5:
        add('extra_multiplier_pipeline', 128*((24*mul_latency+143)//128)*(mul-5),
            f'128 * floor((24 * {mul_latency} + 143) / 128) * ({mul} - 5)',
            'additional multiplier operands, pipeline and result state')
    add_count=e.get('coeff_cluster_add_count','GEM5_CFD_CLUSTER_ADD_COUNT',5)
    add_lat=e.get('coeff_cluster_add_lat','GEM5_CFD_CLUSTER_ADD_LAT',4)
    if add_count > 5:
        add('extra_add_pipeline',128*((24*add_lat+143)//128)*(add_count-5),
            f'128 * floor((24 * {add_lat} + 143) / 128) * ({add_count} - 5)',
            'additional add/sub operands, pipeline and result state')
    if e.get('coeff_arithmetic_mode',default='legacy',cast=str) != 'legacy':
        group_depth=e.get('coeff_arithmetic_depth','GEM5_CFD_ARITHMETIC_DEPTH',12)
        add('arithmetic_group_scoreboard',256*group_depth,f'256 * {group_depth}',
            'finite operation tags, stable operand ownership, returned fragments and route storage')
    vector_local=e.get('coeff_vector_local','GEM5_CFD_VECTOR_LOCAL',False,bool)
    product_only=e.get('coeff_vector_product_only','GEM5_CFD_VECTOR_PRODUCT_ONLY',False,bool)
    if product_only and not vector_local:
        e.errors.append('products-only MVM requires bounded local vector storage')
    local_products=e.get('coeff_local_products','GEM5_CFD_LOCAL_PRODUCTS',False,bool)
    if vector_local or local_products:
        add('local_vector_tags_control',4608,'32 * 128 + 512',
            '32 bounded operation/route tags, 1R1W vector-bank port control')
    if vector_local:
        add('local_mvm_registers',1536,'3 * 512',
            'three 9-vector contexts; products-only retains the full conservative mapping reservation')
    if local_products:
        contexts=e.get('lu5_count','GEM5_CFD_LU5_COUNT',4)
        add('local_lu_ownership',128*contexts+256,f'128 * {contexts} + 256',
            'finite LU product-bank owners; RHS product payload stays in paid row-product slots')
    if e.get('coeff_lu_product_ahead','GEM5_CFD_LU_PRODUCT_AHEAD',False,bool):
        contexts=e.get('lu5_count','GEM5_CFD_LU5_COUNT',4)
        add('lu_product_lookahead',128*contexts+128,f'128 * {contexts} + 128',
            'two product vectors per LU context, k/ready/capture tags and bounded MUL/SUB selection')
    if e.get('coeff_lu_upper_overlap','GEM5_CFD_LU_UPPER_OVERLAP',False,bool):
        contexts=e.get('lu5_count','GEM5_CFD_LU5_COUNT',4)
        add('lu_upper_overlap',256*contexts+128,f'256 * {contexts} + 128',
            'independent upper accumulator, two 1R1W product banks per LU context and bounded issue/join control')
    if e.get('coeff_lu_rank1','GEM5_CFD_LU_RANK1',False,bool):
        contexts=e.get('lu5_count','GEM5_CFD_LU5_COUNT',4)
        add('lu_rank1_control',512*contexts+128,f'512 * {contexts} + 128',
            'owned trailing-update scoreboard, two products, real-return identities and bounded selection; existing matrix RF retains accumulators')
    if e.get('coeff_rhs_factor_reuse','GEM5_CFD_RHS_FACTOR_REUSE',False,bool):
        slots=e.get('coeff_rhs_factor_slots','GEM5_CFD_RHS_FACTOR_SLOTS',2)
        assert slots in (2,4,8,16,32,64), 'invalid RHS factor latch depth'
        add('rhs_factor_latches',128*slots+256,f'{slots} * 128 + 256',
            'bounded temporal LU operand latches, identities, registered capture, 1R1W banks and selection')
    fma_count=e.get('coeff_rhs_fma_count','GEM5_CFD_RHS_FMA_COUNT',0)
    if fma_count:
        assert 4<=fma_count<=16, 'invalid FMA lane count'
        add('rhs_fma_pipeline',192*5*fma_count+256,f'192 * 5 * {fma_count} + 256',
            'five bounded pipeline/result slots per FMA lane, three captured FP64 operands, owner tags and control; RF writes remain paid')
    if e.get('coeff_rhs_frontier_reserve','GEM5_CFD_RHS_FRONTIER_RESERVE',False,bool):
        add('rhs_frontier_admission_control',256,'256',
            'one reserved existing solve-context credit for the oldest unfinished direct b; reuses bounded ordered request metadata, no added context or operand storage')
    lu_fma_count=e.get('coeff_lu_fma_count','GEM5_CFD_LU_FMA_COUNT',0)
    if lu_fma_count:
        assert 4<=lu_fma_count<=16 and e.get('coeff_lu_rank1','GEM5_CFD_LU_RANK1',False,bool)
        add('lu_fma_pipeline',192*5*lu_fma_count+256,f'192 * 5 * {lu_fma_count} + 256',
            'dedicated LU bank; five bounded captured-operand/result slots per lane; existing rank1 state and RF reads/writes remain fully charged')
    rotation=e.get('coeff_spm_slot_rotation','GEM5_CFD_SPM_SLOT_ROTATION',0)
    assert 0<=rotation<=3
    if rotation:
        assert e.get('coeff_spm_banks','GEM5_CFD_COEFF_SPM_BANKS',4)==4
        assert e.get('coeff_spm_bank_granularity','GEM5_CFD_COEFF_SPM_BANK_GRANULARITY',8)==8
        add('spm_slot_rotation_control',128,'128',
            'bounded start-bank address selection inside existing128-byte R and8192-byte ingress slots; no added bank, port, payload or queue entry')
    if e.get('coeff_input_r_before_u','GEM5_CFD_INPUT_R_BEFORE_U',False,bool):
        assert input_r_priority_allowed(level,
            e.get('coeff_input_batch_ready','GEM5_CFD_INPUT_BATCH_READY',False,bool),
            e.get('coeff_input_l_first','GEM5_CFD_INPUT_L_FIRST',False,bool),
            e.get('coeff_cold_rhs_prefetch','GEM5_CFD_COLD_RHS_PREFETCH',False,bool))
        add('input_r_bank_priority_control',128,'128',
            'bounded stored-R selector after L publication; existing physical input offsets, single rest-read handle and banks/ports; no new operand copies')
    if e.get('coeff_lu_pivot_prefetch','GEM5_CFD_LU_PIVOT_PREFETCH',False,bool):
        assert e.get('coeff_lu_rank1','GEM5_CFD_LU_RANK1',False,bool)
        assert e.get('coeff_lu_reciprocal_share','GEM5_CFD_LU_RECIPROCAL_SHARE',False,bool)
        add('lu_pivot_prefetch_control',128,'128',
            'one late reciprocal selector; one pending next pivot in each existing 512-byte rank1 context; existing divider/RF remain paid')
    streaming=e.get('coeff_rhs_upper_stream','GEM5_CFD_RHS_UPPER_STREAM',False,bool)
    if streaming:
        assert e.get('coeff_rhs_upper_inverse','GEM5_CFD_RHS_UPPER_INVERSE',False,bool)
        assert e.get('coeff_lu_reciprocal_share','GEM5_CFD_LU_RECIPROCAL_SHARE',False,bool)
        add('rhs_upper_stream_control',512,'512',
            'independent forward/upper selector control; existing RHS values and FMA/RF capacity, ready only after actual normalization RF return')
    upper=e.get('coeff_rhs_upper_inverse','GEM5_CFD_RHS_UPPER_INVERSE',False,bool)
    upper_slots=e.get('coeff_rhs_upper_slots','GEM5_CFD_RHS_UPPER_SLOTS',16)
    assert 4<=upper_slots<=32 and (upper or upper_slots==16)
    if upper:
        assert fma_count>0
        add('rhs_shared_upper',256*upper_slots+256,f'256 * {upper_slots} + 256',
            'six RF-resident transform/partial words per owner, finite owner/generation/ready/pending state, bounded six-way selector; original LU aliases still pay RF reads')
    tree_temp=e.get('coeff_rhs_tree_rf_temp','GEM5_CFD_RHS_TREE_RF_TEMP',False,bool)
    first_column=e.get('coeff_rhs_tree_rf_first_column','GEM5_CFD_RHS_TREE_RF_FIRST_COLUMN',False,bool)
    assert not first_column or tree_temp, 'first-column policy requires RHS RF temporaries'
    if tree_temp:
        assert e.get('coeff_local_products','GEM5_CFD_LOCAL_PRODUCTS',False,bool)
        assert e.get('coeff_rf_read_words','GEM5_CFD_RF_READ_WORDS',0)>0
        assert e.get('coeff_rf_write_words','GEM5_CFD_RF_WRITE_WORDS',0)>0
        depth=e.get('coeff_rhs_product_depth','GEM5_CFD_RHS_PRODUCT_DEPTH',60)
        add('rhs_tree_rf_temporaries',16*depth+128,f'16 * {depth} + 128',
            'one paid scalar RF partial per bounded row-product slot, valid bits and ownership/control; no arithmetic latency reduction')
    fma_fanout=e.get('coeff_rhs_fma_factor_fanout','GEM5_CFD_RHS_FMA_FACTOR_FANOUT',False,bool)
    if fma_fanout:
        assert e.get('coeff_rhs_factor_fanout','GEM5_CFD_RHS_FACTOR_FANOUT',False,bool)
        assert e.get('coeff_rhs_fma_count','GEM5_CFD_RHS_FMA_COUNT',0)>0
        assert e.get('coeff_rhs_upper_inverse','GEM5_CFD_RHS_UPPER_INVERSE',False,bool)
        add('rhs_fma_factor_fanout',128,'128','FMA factor operand mux control; existing 256B bus remains fully paid; no additional FMA lane, RF port or factor storage')
    fanout = e.get('coeff_rhs_factor_fanout','GEM5_CFD_RHS_FACTOR_FANOUT',False,bool)
    feedback = e.get('coeff_rhs_factor_fanout_feedback','GEM5_CFD_RHS_FACTOR_FANOUT_FEEDBACK',False,bool)
    assert not feedback or fanout, 'feedback factor bus policy requires an enabled bus'
    if fanout:
        assert e.get('coeff_rhs_factor_reuse','GEM5_CFD_RHS_FACTOR_REUSE',False,bool)
        assert not e.get('coeff_rhs_pair','GEM5_CFD_RHS_PAIR',False,bool)
        assert e.get('coeff_rf_read_words','GEM5_CFD_RF_READ_WORDS',0)>0
        assert e.get('coeff_rf_write_words','GEM5_CFD_RF_WRITE_WORDS',0)>0
        add('rhs_factor_fanout',256,'256',
            'one same-cycle registered-cache vector bus, two consumers including paid first bank read, owner/epoch/cache identity and admission control')
    if e.get('coeff_lu_control_fold','GEM5_CFD_LU_CONTROL_FOLD',False,bool):
        contexts=e.get('lu5_count','GEM5_CFD_LU5_COUNT',4)
        add('lu_control_folding',32*contexts+128,f'32 * {contexts} + 128',
            'bounded per-LU return identities and at most three bookkeeping transitions; no arithmetic or data-bank expansion')
    if e.get('coeff_rhs_pair','GEM5_CFD_RHS_PAIR',False,bool):
        add('rhs_pair_operands',768,'2 * 256 + 256',
            'two common-LU operand latches, owners, registered selector and ingress/fanout control; 1 select/load/pair issue per cycle')
    outstanding = e.get('coeff_spm_outstanding', 'GEM5_CFD_COEFF_SPM_OUTSTANDING')
    if level >= 5:
        completion = e.get('coeff_completion_depth', 'GEM5_CFD_COMPLETION_DEPTH', 8)
        entry = 512 if level >= 11 else 1024
        warm = e.get('coeff_warm_write_depth', 'GEM5_CFD_WARM_WRITE_DEPTH', 8)
        add('completion_records', completion*entry, f'{completion} * {entry}', 'bounded completion snapshots')
        add('lifecycle_fixed', 2048, '2048', 'lifecycle control')
        add('warm_write_queue', warm*256, f'{warm} * 256', 'warm write snapshots')
        add('dma_snapshots', outstanding*64, f'{outstanding} * 64', 'in-flight DMA payload snapshots')
    onchip = e.get('coeff_onchip_timing', 'GEM5_CFD_ONCHIP_TIMING', level >= 8, bool)
    if onchip:
        depth = e.get('coeff_onchip_depth', 'GEM5_CFD_ONCHIP_DEPTH', 64)
        add('onchip_access_queue', depth*256, f'{depth} * 256', 'unified SPM access tasks')
        add('onchip_request_context', capacity*128, f'{capacity} * 128', 'per-request access contexts')
        add('onchip_fixed', 1024, '1024', 'access arbitration/control')
    if level >= 9:
        add('backward_operand_slots', 1024, '2 * 512', 'two backward operand contexts')
    if level >= 10:
        add('local_readiness', 512, '512', 'local ready metadata')
    if e.get('coeff_output_coalesce', 'GEM5_CFD_OUTPUT_COALESCE', False, bool):
        add('coalesced_output_snapshots', outstanding*128, f'{outstanding} * 128',
            'coalesced output DMA contexts')
    ahead = e.get('coeff_tile_prefetch_buffers', 'GEM5_CFD_TILE_PREFETCH_BUFFERS', 0)
    if ahead:
        add('tile_prefetch_metadata', ahead*512, f'{ahead} * 512',
            'fixed buffer identities, ranges, phases, word readiness and DMA counters')
    if e.get('coeff_lane_mvm','GEM5_CFD_LANE_MVM',False,bool):
        add('lane_mvm_graph_local',4096,'4096',
            'three fixed 50-node row graphs, operand/result latches and local RF; conservative additional reservation')
    back_depth=e.get('coeff_back_operand_depth','GEM5_CFD_BACK_OPERAND_DEPTH',2)
    if back_depth>2:
        add('extra_backward_operands',512*(back_depth-2),f'({back_depth}-2) * 512',
            'bounded SPM-to-RF lookahead operands')
    return components


def capacity_audit(e, cells=None, cache_enabled=None):
    n = e.get('lusgs_cells') if cells is None else cells
    if n < 1:
        raise ValueError('line cell count must be positive')
    tile = e.get('coeff_line_tile_cells', 'GEM5_CFD_LINE_TILE_CELLS')
    buffers = e.get('coeff_line_tile_buffers', 'GEM5_CFD_LINE_TILE_BUFFERS')
    ahead = e.get('coeff_tile_prefetch_buffers', 'GEM5_CFD_TILE_PREFETCH_BUFFERS', 0)
    budget = e.get('coeff_line_spm_bytes', 'GEM5_CFD_LINE_SPM_BYTES')
    maximum = e.get('coeff_line_cache_cells', 'GEM5_CFD_LINE_CACHE_CELLS')
    direct = e.get('coeff_base_solve', default='direct', cast=str) == 'direct'
    policy = e.get('coeff_cache_policy', default='full', cast=str)
    enabled = (e.get('coeff_line_cache_allowed', default=False, cast=bool)
               if cache_enabled is None else cache_enabled)
    components = reservation_components(e)
    reserve = sum(item['bytes'] for item in components)
    resident, cache = line_storage_bytes(n, tile, buffers+ahead, maximum,
        budget-reserve, direct, policy, enabled)
    if reserve+resident+cache > budget:
        e.errors.append('capacity exceeds unified device budget')
    # Search the allocation policy itself: handles tiny lines, one/two tiles,
    # and maximum-cell cap, rather than assuming n >= tile for every setup.
    bounds = {'full': 0, 'adaptive_any': 0, 'adaptive_partial': 0}
    for count in range(1, max(0, min(maximum, max(0, budget-reserve)//400))+1):
        _, amount = line_storage_bytes(count, tile, buffers+ahead, maximum,
                                      budget-reserve, direct, 'adaptive', True)
        if amount == 600*count:
            bounds['full'] = count
        if amount:
            bounds['adaptive_any'] = count
        if amount == 400*count:
            bounds['adaptive_partial'] = count
    return dict(cells=n, budget_bytes=budget, reservation_bytes=reserve,
        reservation_components=components, logical_tile_bytes=resident,
        tile_cells=tile, tile_buffers=buffers,
        tile_prefetch_buffers=ahead,
        tile_y_only=(cache == 600*n), logical_coefficient_cache_bytes=cache,
        cache_mode='full' if cache == 600*n else 'partial' if cache else 'off',
        total_budgeted_bytes=reserve+resident+cache,
        free_budget_bytes=budget-reserve-resident-cache,
        logical_payload_bytes=resident+cache,
        cache_boundaries=bounds, cache_cell_limit=maximum,
        physical_sram_bytes=None, physical_register_bits=None, physical_fifo_bits=None,
        interpretation='Reservation is a bounded model budget, not a physical SRAM or synthesized area measurement.',
        source=SOURCE['capacity'], allocation_source=SOURCE['cache'])


def _objects(node, kind):
    if isinstance(node, dict):
        if node.get('type') == kind:
            yield node
        for value in node.values():
            yield from _objects(value, kind)
    elif isinstance(node, list):
        for value in node:
            yield from _objects(value, kind)


def _audit_dram_channels(e,spec,config,result):
    """Prove two or four complementary physical 64 B stripes and unchanged timing."""
    system=config.get('system',{});count=spec.get('channels')
    actual=system.get('dram_ctrls');snapshots=spec.get('controllers')
    result.update(primary_memory=actual,channel_count=count,
        bandwidth_interpretation='Each physical channel has its own unchanged DDR4 timing, bus and finite queues; totals are not DSA-ROI utilization.')
    if type(count) is not int or count not in (2,4) or spec.get('model')!='MemCtrl' or spec.get('interleave_bytes')!=64:
        e.errors.append('unsupported physical DRAM channel configuration');return result
    if not isinstance(actual,list) or len(actual)!=count or not isinstance(snapshots,list) or len(snapshots)!=count:
        e.errors.append('physical DRAM controller count differs from snapshot');return result
    if len(list(_objects(config,'MemCtrl')))!=count or system.get('dram'):
        e.errors.append('physical DRAM controllers are missing or duplicated')
    if system.get('cache_line_size')!=64:
        e.errors.append('DDR channel interleaving differs from cache-line width')
    spm=system.get('spm',{})
    if spm.get('conf_table_reported') is not False or spm.get('in_addr_map') is not True:
        e.errors.append('auxiliary SPM must remain mapped but excluded from the general SE allocator')
    result['allocator_policy']='DRAM-only general SE allocation; explicitly mapped auxiliary SPM'
    total=spec.get('mapped_size_bytes');ports=set();matches=set();capacity=bus=reads=writes=0
    mapped=0
    def compare(actual,key,wanted,label):
        value=actual.get(key)
        try:equal=float(value)==float(wanted)
        except (ValueError,TypeError):equal=value==wanted
        if not equal:e.errors.append('physical DRAM channel '+label+' differs from resolved snapshot')
    for i,(controller,snapshot) in enumerate(zip(actual,snapshots)):
        if controller.get('type')!='MemCtrl' or snapshot.get('model')!='MemCtrl' or snapshot.get('preset')!='DDR4_2400_8x8':
            e.errors.append('physical DRAM channel does not use the requested MemCtrl preset')
        if controller.get('path')!=snapshot.get('path'):
            e.errors.append('physical DRAM controller path differs from snapshot')
        if controller.get('clk_domain')!=system.get('clk_domain',{}).get('path','system.clk_domain'):
            e.errors.append('physical DRAM controller clock differs from the system clock')
        peer=controller.get('port',{}).get('peer','')
        if not peer.startswith('system.membus.mem_side_ports[') or peer in ports:
            e.errors.append('physical DRAM channels must use distinct memory-bus ports')
        ports.add(peer)
        child=controller.get('dram',{})
        if child.get('type')!='DRAMInterface' and not child.get('type','').startswith('DDR'):
            e.errors.append('physical channel has no instantiated DRAM interface')
        for group,required in [('geometry',{'device_size','device_bus_width','devices_per_rank','ranks_per_channel','banks_per_rank','bank_groups_per_rank','device_rowbuffer_size','burst_length'}),
                               ('timing_ticks',{'tCK','tBURST','tRCD','tCL','tRP','tRAS','tRFC','tREFI'}),
                               ('interface',{'read_buffer_size','write_buffer_size','addr_mapping','page_policy'})]:
            if not required.issubset(snapshot.get(group,{})):
                e.errors.append('physical DRAM snapshot omits required '+group)
        for group in ('geometry','timing_ticks','interface'):
            for key,value in snapshot.get(group,{}).items():compare(child,key,value,f'{i}.{group}.{key}')
            if snapshot.get(group)!=snapshots[0].get(group):
                e.errors.append('physical DRAM channels have unequal '+group)
        for key,value in snapshot.get('controller',{}).items():
            compare(controller,key.removesuffix('_ticks'),value,f'{i}.controller.{key}')
        if snapshot.get('controller')!=snapshots[0].get('controller'):
            e.errors.append('physical DRAM channel policies differ')
        try:
            address=[int(v,0) for v in child['range'].split(':')]
            start,end,match,*masks=address
            if start!=0 or end!=total or masks!=[1 << (6+bit) for bit in range(count.bit_length()-1)] or match!=i:
                e.errors.append('physical DRAM interleaving is not the complete complementary 64 B mapping')
            if child['range']!=snapshot.get('range') or snapshot.get('interleaving')!=dict(start=start,end=end,masks=masks,match=match):
                e.errors.append('physical DRAM interleaving differs from resolved snapshot')
            matches.add(match);channel_bytes=(end-start)//count;mapped+=channel_bytes
            if channel_bytes!=snapshot.get('mapped_size_bytes'):
                e.errors.append('physical DRAM channel mapped capacity differs')
            capacity+=int(child['device_size'])*int(child['devices_per_rank'])*int(child['ranks_per_channel'])
            width=int(child['device_bus_width'])*int(child['devices_per_rank']);bus+=width
            if width*int(child['burst_length'])//8!=64:
                e.errors.append('physical DRAM burst differs from channel stripe')
            reads+=int(child['read_buffer_size']);writes+=int(child['write_buffer_size'])
        except (ValueError,TypeError,KeyError):
            e.errors.append('malformed physical DRAM channel range/resource snapshot')
    if matches!=set(range(count)) or mapped!=total:
        e.errors.append('physical DRAM channel ranges do not cover the mapped window exactly once')
    windows=system.get('mem_ranges',[])
    if not windows or windows[0]!=f'0:{total}':
        e.errors.append('physical DRAM aggregate mapping differs from system window')
    for key,value in [('device_capacity_bytes',capacity),('data_bus_width_bits',bus),
                      ('read_buffer_entries',reads),('write_buffer_entries',writes)]:
        if spec.get(key)!=value:e.errors.append('physical DRAM aggregate '+key+' differs from snapshot')
    result.update(mapped_size_bytes=mapped,data_bus_width_bits=bus,device_capacity_bytes=capacity,
                  read_buffer_entries=reads,write_buffer_entries=writes)
    return result


def _audit_memory(e, resolved, config):
    """Verify the primary system.dram object, never confusing auxiliary SPM."""
    system = config.get('system', {})
    spec = resolved.get('memory', {})
    primary = system.get('dram', system.get('mem_ctrl'))
    simple = [dict(path=m.get('path'), type=m['type'],
        latency_ticks=m.get('latency'), latency_variation_ticks=m.get('latency_var'),
        bandwidth_ticks_per_byte=m.get('bandwidth'), range=m.get('range'))
        for m in _objects(config, 'SimpleMemory')]
    controllers = []
    for controller in _objects(config, 'MemCtrl'):
        child = controller.get('dram', {})
        controllers.append(dict(path=controller.get('path'), type=controller['type'],
            controller={key: value for key, value in controller.items()
                if key in spec.get('controller', {}) or key in ('mem_sched_policy',
                    'static_frontend_latency', 'static_backend_latency')},
            dram_interface={key: value for key, value in child.items()
                if key in ('type', 'path', 'range') or key.startswith('t') or
                key in spec.get('geometry', {}) or key in spec.get('interface', {})}))
    result = dict(requested=spec, primary_memory=primary,
        instantiated_simple_memories=simple, instantiated_memory_controllers=controllers,
        instantiated_dram_interfaces=list(_objects(config, 'DRAMInterface')),
        bandwidth_interpretation='gem5 SimpleMemory bandwidth is serialized ticks per byte; configuration strings are quantized during unit conversion.',
        detailed_ddr_calibrated=False,
        preset_identity='DDR preset names are Python subclasses; compare resolved geometry and timing to the instantiated DRAMInterface.')
    if spec.get('channels',1)!=1:
        return _audit_dram_channels(e,spec,config,result)
    result['channel_count']=1
    expected_model = spec.get('model')
    if expected_model:
        if not isinstance(primary, dict) or primary.get('type') != expected_model:
            e.errors.append('primary system.dram type differs from resolved memory model')
            return result
    if not primary:
        return result

    def compare(actual, key, expected, label):
        value = actual.get(key)
        try:
            match = float(value) == float(expected)
        except (ValueError, TypeError):
            match = value == expected
        if not match:
            e.errors.append('primary memory '+label+' differs from resolved snapshot')

    if expected_model == 'SimpleMemory':
        for source, target in (('latency_ticks', 'latency'),
                ('latency_variation_ticks', 'latency_var'),
                ('bandwidth_ticks_per_byte', 'bandwidth')):
            if source in spec:
                compare(primary, target, spec[source], source)
    elif expected_model == 'MemCtrl':
        child = primary.get('dram', {})
        child_type = child.get('type', '')
        if child_type != 'DRAMInterface' and not child_type.startswith('DDR'):
            e.errors.append('primary MemCtrl has no instantiated DRAMInterface')
        for group in ('geometry', 'timing_ticks', 'interface'):
            for key, value in spec.get(group, {}).items():
                compare(child, key, value, group+'.'+key)
        for key, value in spec.get('controller', {}).items():
            actual_key = key.removesuffix('_ticks')
            compare(primary, actual_key, value, 'controller.'+key)
    return result


def audit_input_capacity(e, storage, model):
    slots = e.get('coeff_input_slots', 'GEM5_CFD_COEFF_INPUT_SLOTS', 2)
    level = e.get('coeff_line_opt_level', 'GEM5_CFD_LINE_OPT_LEVEL', 0)
    extended = e.get('coeff_extended_input_slots', 'GEM5_CFD_EXTENDED_INPUT_SLOTS', 0)
    sme = e.get('coeff_compute_backend', 'GEM5_CFD_SME_ZA', 'event-scalar', cast=_backend_bit)
    if not input_slot_count_valid(level, slots, bool(extended), sme):
        e.errors.append('input slots exceed the selected bounded capacity')
    if extended and (storage != 'tiled' or model != 'shared'
            or not e.get('coeff_onchip_timing', 'GEM5_CFD_ONCHIP_TIMING', 0)
            or e.get('coeff_spm_model', 'GEM5_CFD_COEFF_SPM_MODEL', 'event', cast=str) != 'dma'):
        e.errors.append('extended input capacity requires shared tiled timed DMA')
    return dict(configured_slots=slots, extended=bool(extended),
                slot_limit=(32 if sme else 16) if extended else 8 if level >= 13 else 4,
                request_frame_bytes=8192, onchip_context_bytes_per_frame=128,
                reservation='existing capacity-dependent request/context formulas; no free landing data')


def audit_configuration(resolved, config, simout='', command=(), strict=True):
    """Pure JSON-to-JSON audit; ``config`` is instantiated gem5 config.json."""
    e = Effective(resolved)
    last, duplicates = command_arguments(command)
    storage = e.get('coeff_line_storage', default='full', cast=str)
    model = e.get('coeff_compute_model', default='legacy', cast=str)
    result = dict(schema_version=1, scope='single-line configuration audit',
        status='passed', errors=[], launch_arguments_last_wins=last,
        repeated_launch_arguments=duplicates,
        raw_options=resolved.get('options', {}),
        semantics='Effective controller environment and instantiated objects take precedence over inactive legacy flags.',
        capacity=capacity_audit(e) if storage == 'tiled' else None)
    result['input_capacity'] = audit_input_capacity(e, storage, model)
    if e.get('coeff_cold_forward_tree', 'GEM5_CFD_COLD_FORWARD_TREE', 0):
        if (not e.get('coeff_warm_forward_tree', 'GEM5_CFD_WARM_FORWARD_TREE', 0)
                or model != 'shared' or storage != 'tiled'
                or e.get('coeff_line_mvm_model', default='serial', cast=str) != 'tree'
                or e.get('coeff_base_solve', default='inverse', cast=str) != 'direct'
                or e.get('coeff_lane_mvm', 'GEM5_CFD_LANE_MVM', 0)):
            e.errors.append('cold forward tree requires the existing direct vector warm-tree resources')
        result['cold_forward_tree'] = dict(
            context='existing per-line MVM context 0', extra_fp_units=0,
            extra_operand_storage_bytes=0, extra_rf_ports=0,
            source_readiness='actual per-column Lbar RHS RF return',
            reduction='((p0+p1)+(p2+p3))+p4; four five-lane adds')
    for env_key, expected in (('GEM5_CFD_SHARED_CLUSTER', model == 'shared'),
                              ('GEM5_CFD_LINE_TILED', storage == 'tiled')):
        if env_key in e.env and str(e.env[env_key]).lower() not in (
                '1' if expected else '0', 'true' if expected else 'false'):
            e.errors.append(env_key+': controller mode differs from resolved options')
    if model == 'shared':
        pools = {}
        for name, count_key, count_env, count_default, lat_key, lat_env, ii_key, ii_env in (
            ('mul', 'coeff_cluster_mul_count', 'GEM5_CFD_CLUSTER_MUL_COUNT', 5,
             'coeff_cluster_mul_lat', 'GEM5_CFD_CLUSTER_MUL_LAT', None, None),
            ('add', 'coeff_cluster_add_count', 'GEM5_CFD_CLUSTER_ADD_COUNT', 5, 'coeff_cluster_add_lat', 'GEM5_CFD_CLUSTER_ADD_LAT', None, None),
            ('div', 'coeff_cluster_div_count', 'GEM5_CFD_CLUSTER_DIV_COUNT', 1,
             'coeff_cluster_div_lat', 'GEM5_CFD_CLUSTER_DIV_LAT', 'coeff_cluster_div_ii', 'GEM5_CFD_CLUSTER_DIV_II')):
            count = e.get(count_key, count_env, count_default) if count_key else count_default
            pools[name] = dict(count=count, latency_cycles=e.get(lat_key, lat_env),
                initiation_interval_cycles=e.get(ii_key, ii_env) if ii_key else 1)
        fma_count=e.get('coeff_rhs_fma_count','GEM5_CFD_RHS_FMA_COUNT',0)
        if fma_count:
            pools['fma']=dict(count=fma_count,latency_cycles=5,initiation_interval_cycles=1)
            for r in _reports(simout,'CFD_RHS_FMA_CAPACITY'):
                assert (int(r['count']),int(r['lat']),int(r['ii']),int(r['slots']),int(r['bytes']),int(r['live']))==(fma_count,5,1,5*fma_count,192*5*fma_count+256,0)
                assert 0<=int(r['peak'])<=5*fma_count
        lu_fma_count=e.get('coeff_lu_fma_count','GEM5_CFD_LU_FMA_COUNT',0)
        if lu_fma_count:
            pools['lu_fma']=dict(count=lu_fma_count,latency_cycles=5,initiation_interval_cycles=1)
            reports=_reports(simout,'CFD_LU_FMA_CAPACITY')
            assert reports, 'missing LU FMA capacity proof'
            for r in reports:
                assert (int(r['count']),int(r['lat']),int(r['ii']),int(r['slots']),int(r['bytes']),int(r['live']),int(r['issue_width_per_context']))==(lu_fma_count,5,1,5*lu_fma_count,192*5*lu_fma_count+256,0,1)
                assert 0<=int(r['peak'])<=5*lu_fma_count
        if e.get('coeff_rhs_upper_inverse','GEM5_CFD_RHS_UPPER_INVERSE',False,bool):
            slots=e.get('coeff_rhs_upper_slots','GEM5_CFD_RHS_UPPER_SLOTS',16)
            reports=_reports(simout,'CFD_RHS_UPPER_CAPACITY')
            assert reports, 'missing bounded upper transform capacity'
            for r in reports:
                assert (int(r['slots']),int(r['bytes']),int(r['live']),int(r['issue_width']))==(slots,256*slots+256,0,6)
                assert 0<=int(r['peak'])<=slots and int(r['issued'])==int(r['completed'])+int(r['dropped'])
        divider_model = e.get('coeff_divider_model', 'GEM5_CFD_DIVIDER_MODEL', 'timing', str)
        detailed_divider = divider_model in ('pipelined', 'iterative')
        div = pools['div']
        requested = e.get('coeff_divider_inflight', 'GEM5_CFD_DIVIDER_INFLIGHT', 0)
        divider = dict(model=divider_model, requested_inflight_capacity=requested,
            effective_inflight_capacity=(requested or (1 if divider_model == 'iterative' else
                (div['latency_cycles']+div['initiation_interval_cycles']-1)//div['initiation_interval_cycles']))
                if detailed_divider else None,
            result_fifo_depth=e.get('coeff_divider_result_depth', 'GEM5_CFD_DIVIDER_RESULT_DEPTH', 2) if detailed_divider else None,
            writeback_ii_cycles=e.get('coeff_divider_writeback_ii', 'GEM5_CFD_DIVIDER_WRITEBACK_II', 1) if detailed_divider else None,
            calibration='Cycle-level model; hardware timing/area uncalibrated')
        result['arithmetic'] = dict(pools=pools, divider=divider,
            lu_contexts=e.get('lu5_count', 'GEM5_CFD_LU5_COUNT'),
            rhs_contexts=e.get('coeff3_event_count', 'GEM5_CFD_COEFF3_COUNT'),
            lu_reciprocal_share=e.get('coeff_lu_reciprocal_share',
                'GEM5_CFD_LU_RECIPROCAL_SHARE',False,bool),
            lu_supply_first=e.get('coeff_lu_supply_first',
                'GEM5_CFD_LU_SUPPLY_FIRST',False,bool),
            source=SOURCE['arithmetic'])
        group_mode=e.get('coeff_arithmetic_mode',default='legacy',cast=str)
        mode_ids={'legacy':0,'shared':1,'fixed':2,'borrow':3}
        if group_mode not in mode_ids or int(e.env.get('GEM5_CFD_ARITHMETIC_MODE',0))!=mode_ids.get(group_mode):
            e.errors.append('arithmetic group mode disagrees with controller environment')
        if group_mode!='legacy':
            group=dict(mode=group_mode)
            for name,default in [('depth',12),('select',1),('route',1),('borrow_route',1),('width',4),('age',32)]:
                group[name]=e.get('coeff_arithmetic_'+name,'GEM5_CFD_ARITHMETIC_'+name.upper(),default)
            observed_groups=_reports(simout,'CFD_ARITHMETIC_SUMMARY')
            for observed_group in observed_groups:
                for name in ('depth','select','route','borrow_route','width','age'):
                    if int(observed_group[name])!=group[name]:e.errors.append('arithmetic group report differs: '+name)
                if int(observed_group['mode'])!=mode_ids[group_mode] or int(observed_group['live']) or int(observed_group['accepted'])!=int(observed_group['completed']):
                    e.errors.append('arithmetic group ownership did not drain')
            group['observed_reports']=len(observed_groups)
            result['arithmetic']['groups']=group
        observed = _reports(simout, 'CFD_RESOURCE_STATS')
        for row in observed:
            name = row.get('pool', '').removeprefix('shared_')
            if name in pools:
                for observed_key, normalized in (('count', 'count'), ('lat', 'latency_cycles'),
                                                  ('ii', 'initiation_interval_cycles')):
                    if int(row[observed_key]) != pools[name][normalized]:
                        e.errors.append('observed '+row['pool']+' '+observed_key+' differs from effective resource')
        result['arithmetic']['observed_resource_report_count'] = len(observed)
        result['inactive_parameter_notes'] = [
            'LU/RHS context counts are independent states sharing the arithmetic pools, not replicated arithmetic clusters.',
            'lu5_div_ii and coeff3_div_ii describe legacy pools; shared divider timing is cluster_div_lat/cluster_div_ii.',
            'Legacy local-SPM/TRSV/Vec5 settings do not define the timed single-line bank ports.']
    banks = e.get('coeff_spm_banks', 'GEM5_CFD_COEFF_SPM_BANKS', 4)
    grain = e.get('coeff_spm_bank_granularity', 'GEM5_CFD_COEFF_SPM_BANK_GRANULARITY', 64)
    reads = e.get('coeff_spm_read_ports', 'GEM5_CFD_COEFF_SPM_READ_PORTS', 1)
    writes = e.get('coeff_spm_write_ports', 'GEM5_CFD_COEFF_SPM_WRITE_PORTS', 1)
    # Bank issue code services one FP64 word per port; grain is address mapping.
    result['banked_spm'] = dict(banks=banks, address_interleave_grain_bytes=grain,
        word_bytes=8, read_ports_per_bank=reads, write_ports_per_bank=writes,
        peak_read_bytes_per_cycle=banks*reads*8,
        peak_write_bytes_per_cycle=banks*writes*8,
        physical_macro='unmapped', calibration='Uncalibrated SRAM/port/arbitration timing')
    system = config.get('system', {})
    clock = system.get('clk_domain', {}).get('clock', [])
    if isinstance(clock, list) and clock:
        period = int(clock[0])
        result['clock'] = dict(requested=resolved.get('clock'), instantiated_period_ticks=period)
        if 'GEM5_CFD_TICKS_PER_CYCLE' in e.env and int(e.env['GEM5_CFD_TICKS_PER_CYCLE']) != period:
            e.errors.append('controller ticks-per-cycle differs from instantiated clock')
    endpoint = system.get('coeff_dma_cache')
    if endpoint:
        names = {'size': 'coeff_dma_cache_size', 'mshrs': 'coeff_dma_cache_mshrs',
                 'write_buffers': 'coeff_dma_cache_write_buffers', 'tag_latency': 'coeff_dma_cache_tag_lat',
                 'data_latency': 'coeff_dma_cache_data_lat', 'response_latency': 'coeff_dma_cache_response_lat'}
        result['dma_cache'] = {key: endpoint.get(key) for key in ('type', 'size', 'assoc', 'mshrs',
            'write_buffers', 'tag_latency', 'data_latency', 'response_latency', 'sequential_access')}
        result['dma_cache']['included_in_unified_device_budget'] = False
        result['dma_cache']['physical_area'] = None
        for key, option in names.items():
            if option in e.options and int(endpoint[key]) != int(e.options[option]):
                e.errors.append('DMA cache '+key+' differs from resolved option')
    result['memory'] = _audit_memory(e, resolved, config)
    if '--dram-channels' in last:
        try:
            if int(last['--dram-channels'])!=result['memory']['channel_count']:
                e.errors.append('DRAM channel launch value differs from instantiated hardware')
        except (ValueError,TypeError,KeyError):
            e.errors.append('invalid DRAM channel launch value')
    rf_reads = e.get('coeff_rf_read_words','GEM5_CFD_RF_READ_WORDS',0)
    rf_writes = e.get('coeff_rf_write_words','GEM5_CFD_RF_WRITE_WORDS',0)
    arb_tasks = e.get('coeff_onchip_arb_tasks','GEM5_CFD_ONCHIP_ARB_TASKS',0)
    ahead = e.get('coeff_tile_prefetch_buffers','GEM5_CFD_TILE_PREFETCH_BUFFERS',0)
    result['hardware_contract'] = dict(
        status='bounded architectural contract; physical implementation uncalibrated',
        scalar_word_bits=64,
        spm=dict(address_aperture_bytes=e.get('coeff_line_spm_bytes','GEM5_CFD_LINE_SPM_BYTES'),
            logical_payload_bytes=result['capacity']['logical_payload_bytes'] if result['capacity'] else None,
            bank_read_words_per_cycle=reads,bank_write_words_per_cycle=writes,
            physical_sram_bytes=None,physical_macro=None),
        register_file=dict(read_words_per_cycle=rf_reads,write_words_per_cycle=rf_writes,
            operand_read_latency='included in FU input timing; no independent RF read stage',
            scalar_broadcast=e.get('coeff_rf_broadcast','GEM5_CFD_RF_BROADCAST',False,bool),
            preprocessing_scalar_broadcast=e.get('coeff_preprocess_broadcast','GEM5_CFD_PREPROCESS_BROADCAST',False,bool),
            rhs_pair_enabled=e.get('coeff_rhs_pair','GEM5_CFD_RHS_PAIR',False,bool),
            rhs_pair_path='two Ubar operand latches; one registered select, shared RF load, next-cycle dual-MUL fanout; both result destinations retain normal credits',
            preprocessing_fanout_timing='existing MUL input fanout and operand-select mux; included in assumed FU timing, not IP calibrated',
            split_writeback=e.get('coeff_rf_split_writeback','GEM5_CFD_RF_SPLIT_WRITEBACK',False,bool),
            split_issue=e.get('coeff_lane_mvm','GEM5_CFD_LANE_MVM',False,bool),
            split_issue_scope='line MVM/reduction including warm direct correction; cold LU/RHS keep existing scheduling',
            lane_local_banks=(5 if e.get('coeff_lane_local','GEM5_CFD_LANE_LOCAL',False,bool) else 0),
            local_ports_per_row=dict(read=2,write=1),
            lane_selection_width=e.get('coeff_lane_issue_width','GEM5_CFD_LANE_ISSUE_WIDTH',10),
            lane_selection_latency=e.get('coeff_lane_select_latency','GEM5_CFD_LANE_SELECT_LATENCY',1),
            lane_forwarding_latency=e.get('coeff_lane_route_latency','GEM5_CFD_LANE_ROUTE_LATENCY',1),
            forwarding='no zero-cost arithmetic bypass assumed',
            result_ownership='all group FU credits retained until every result lane is written'),
        arbiter=dict(latency_cycles=e.get('coeff_onchip_arb_latency','GEM5_CFD_ONCHIP_ARB_LATENCY',1),
            tasks_per_cycle=arb_tasks,extra_unbounded_task_width=(arb_tasks == 0),
            scope='SPM word-service grants; not complete controller selection timing',
            allocation_cancel_bandwidth_separately_modeled=False,
            physical_control_timing_calibrated=False),
        prefetch=dict(forward_buffers=e.get('coeff_line_tile_buffers','GEM5_CFD_LINE_TILE_BUFFERS'),
            independent_d_input=e.get('coeff_input_d_early','GEM5_CFD_INPUT_D_EARLY',False,bool),
            cold_r_prefetch=e.get('coeff_cold_rhs_prefetch','GEM5_CFD_COLD_RHS_PREFETCH',False,bool),
            cold_r_contract='existing <=8 R slots; no duplicate ingress R; b waits R+LU, L/U RHS wait coefficient read; completed b may park before coefficient initialization',
            rhs_park_resume='existing request b/LU storage, no partial RHS spill; unchanged execution credits; at least one cycle from park to resume',
            d_input_contract='D read waits all D stores; LU waits D read; RHS/slot release wait full input',
            d_input_storage='additional job pointer and ready/counter bits within existing 128B onchip context and 8192B request; job uses existing bounded access queue',
            extra_backward_buffers=ahead,extra_metadata_bytes=ahead*512,
            backward_operand_slots=e.get('coeff_back_operand_depth','GEM5_CFD_BACK_OPERAND_DEPTH',2),
            independent_backward_readiness=e.get('coeff_back_independent_ready','GEM5_CFD_BACK_INDEPENDENT_READY',False,bool),
            backward_y_dependency=('signed y-product node only; product columns keep their own SPM response fences'
                if e.get('coeff_back_independent_ready','GEM5_CFD_BACK_INDEPENDENT_READY',False,bool)
                else 'legacy joint y/column readiness'),
            reverse_read_order=('cell-coverage' if e.get('coeff_tile_cell_order','GEM5_CFD_TILE_CELL_ORDER',False,bool)
                else 'legacy'),
            ownership='fixed storage IDs; forward responses and reload landings fence overwrite'),
        final_output_fifo=dict(depth=e.get('coeff_line_output_depth','GEM5_CFD_LINE_OUTPUT_DEPTH',
                e.get('coeff_drain_queue_depth','GEM5_CFD_COEFF_DRAIN_QUEUE_DEPTH',4)),
            entry_bytes=64,independent_of_preprocessing_drain=True,
            explicitly_budgeted=('coeff_line_output_depth' in e.options or
                                'GEM5_CFD_LINE_OUTPUT_DEPTH' in e.env)),
        completion_policy=dict(policy=e.get('coeff_record_policy',default='per-cell',cast=str),
            timed_hash=(dict(engines=1,state_bytes=128,word_latency_cycles=2,
                word_ii_cycles=2,record_words=40,record_service_cycles=80,
                ownership='existing request retained until last word return',
                datapath='64-bit integer constant multiply/XOR, registered feedback and request metadata read mux',
                metadata_read_bandwidth='one 64-bit control word per two cycles; separate from floating-point RF',
                physical_area=None,calibrated=False)
                if e.get('coeff_record_policy',default='per-cell',cast=str) in ('line-summary','cold-summary') else None),
            aggregate_when=('cache miss only; warm requests retain per-cell records'
                if e.get('coeff_record_policy',default='per-cell',cast=str)=='cold-summary' else
                'all direct requests' if e.get('coeff_record_policy',default='per-cell',cast=str)=='line-summary' else
                'legacy per-cell or warm-only policy')),
        fully_bounded_rf_and_arbiter=bool(rf_reads and rf_writes and arb_tasks),
        parameter_provenance=dict(arithmetic='architecture assumption; latency/II/clock require IP or synthesis evidence',
            bank_ports='declared architectural ports; SRAM macro not selected',
            register_ports='scalar bandwidth contract; distributed/banked implementation not selected',
            dma_cache='instantiated gem5 cache; endpoint timing not hardware-calibrated',
            dram='gem5 preset or configured SimpleMemory; not a board measurement'),
        physical_area=None)
    rows = _reports(simout, 'CFD_LINE_STORAGE')
    result['observed_storage'] = []
    for row in rows:
        # Recovery fixtures may use shorter lines and different cache eligibility.
        # Audit their actual declared allocation, retaining the run-level prediction.
        expected = capacity_audit(e, int(row['cells']), int(row['cache_bytes']) > 0)
        checks = {'reserved': expected['reservation_bytes'], 'resident': expected['logical_tile_bytes'],
                  'capacity': expected['budget_bytes'], 'cache_bytes': expected['logical_coefficient_cache_bytes']}
        if 'tile_y_only' in row:
            checks['tile_y_only'] = int(expected['tile_y_only'])
        for key, value in checks.items():
            if int(row[key]) != value:
                e.errors.append('storage token '+row.get('token', '?')+' '+key+' differs from capacity contract')
        result['observed_storage'].append(dict(token=row.get('token'),
            cells=int(row['cells']), checked={key: int(row[key]) for key in checks}))
    # Explicit options sometimes have compatibility aliases; resolved values are
    # recorded rather than inferring missing defaults from launch arguments.
    aliases = {'coeff_onchip_depth': '--coeff-onchip-queue-depth',
               'coeff3_event_count': '--coeff3-event-count',
               'coeff_cache_policy': '--coeff-line-cache-policy'}
    for key, source in e.sources.items():
        flag = aliases.get(key, '--'+key.replace('_', '-'))
        source['launch_flag'] = flag
        source['launch_value'] = last.get(flag)
        source['origin'] = ('explicit launch argument; effective value recorded after configuration processing'
                            if flag in last else
                            'resolved profile/default/derived value; no matching explicit launch argument')
    result['parameter_sources'] = e.sources
    result['parameter_rationale'] = {
        'matrix_shape': 'Five variables define 5x5 FP64 matrices (200 B) and 5-element vectors (40 B).',
        'arithmetic_count': 'Five MUL and ADD lanes match vector width; this is a design choice, not an optimality proof.',
        'divider_inflight': 'Automatic pipelined capacity is ceil(latency / initiation interval); iterative capacity is one.',
        'banks_and_grain': '8 B bank grain interleaves FP64 words; bank count and independent 1R1W ports remain design parameters.',
        'capacity_reservations': 'Conservative model constants and bounded depths; physical storage mapping is outstanding.',
        'tile_context_queue_depths': 'Tuned finite structures; assess occupancy, blocking and marginal benefit by sensitivity experiments.',
        'cache_threshold': 'Derived jointly from reservations, y-only/full tiles, 600/400 B per cell and maximum-cell cap.',
        'memory_arithmetic_timing': 'Simulation assumptions requiring target-system or hardware calibration; not validated physical claims.'}
    result['calibration_gaps'] = [
        'Hardware arithmetic latency/II and target clock require IP or synthesis evidence.',
        'SRAM, register-file, result writeback and bypass costs require a physical resource mapping.',
        'Controller scans/priority selection are not a measured RTL arbitration critical path.',
        'DMA cache and system-memory timing require microbenchmarks and a detailed-memory sensitivity check.',
        'Unified conservative byte reservations must not be reported as physical SRAM area.']
    result['errors'] = sorted(set(e.errors))
    result['status'] = 'failed' if result['errors'] else 'passed'
    if strict and result['errors']:
        raise ValueError('; '.join(result['errors']))
    return result


def audit_run(path, strict=True):
    path = Path(path)
    resolved = json.loads((path/'resolved_experiment.json').read_text())
    config = json.loads((path/'config.json').read_text())
    manifest_path = path/'manifest.json'
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    simout_path = path/'simout.txt'
    simout = simout_path.read_text(errors='replace') if simout_path.exists() else ''
    return audit_configuration(resolved, config, simout, manifest.get('command', []), strict)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = audit_run(args.run_dir, strict=False)
    except (ValueError, KeyError, OSError) as exc:
        parser.exit(1, 'configuration audit failed: '+str(exc)+'\n')
    text = json.dumps(result, indent=2, sort_keys=True)+'\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
