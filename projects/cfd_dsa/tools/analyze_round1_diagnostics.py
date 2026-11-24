#!/usr/bin/env python3
"""Stream exact RHS/RF/Tile identities into first-round performance evidence.

This observer never changes scheduling. Ready-pair snapshots and per-operation
latencies overlap; neither may be summed into a claimed sweep-cycle saving.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
import math
from pathlib import Path
import re


FIELDS = re.compile(rb'(\w+)=([^\s]+)')
PREFIXES = {
    b'CFD_RHS_READY', b'CFD_RF_ISSUE', b'CFD_RF_OWNER', b'CFD_RF_WRITE',
    b'CFD_LOCAL_WRITE', b'CFD_RF_COMPLETE', b'CFD_MVM_BIND',
    b'CFD_LINE_FEEDBACK', b'CFD_LINE_SCHEDULE', b'CFD_DMA_PACKET',
    b'CFD_TILE_DMA', b'CFD_ONCHIP_START', b'CFD_ONCHIP_ACTIVATE',
    b'CFD_ONCHIP_ACCESS', b'CFD_ONCHIP_COMPLETE', b'CFD_ONCHIP_CANCEL',
    b'CFD_BACK_OPERAND',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


class Distribution:
    def __init__(self):
        self.values = Counter()

    def add(self, value):
        require(value >= -1e-9, 'negative measured interval')
        self.values[max(0, value)] += 1

    def summary(self):
        count = sum(self.values.values())
        if not count:
            return dict(count=0, mean=None, min=None, p50=None, p95=None, max=None)
        ordered = sorted(self.values)
        def percentile(fraction):
            rank = math.ceil(fraction*count)
            accumulated = 0
            for value in ordered:
                accumulated += self.values[value]
                if accumulated >= rank:
                    return value
        return dict(count=count, mean=sum(v*n for v, n in self.values.items())/count,
                    min=ordered[0], p50=percentile(.5), p95=percentile(.95), max=ordered[-1])


class Round1Replay:
    def __init__(self, ticks_per_cycle):
        require(ticks_per_cycle > 0, 'invalid controller clock')
        self.period = ticks_per_cycle
        self.counts = Counter()
        self.samples = defaultdict(Distribution)
        self.rf = {}
        self.last_rf_id = 0
        self.rf_counts = defaultdict(Counter)
        self.rf_samples = defaultdict(lambda: defaultdict(Distribution))
        self.rhs_cycles = defaultdict(set)
        self.rhs_histograms = defaultdict(Counter)
        self.rhs_seen_cycle = None
        self.rhs_seen = set()
        self.packets = {}
        self.tiles = {}
        self.jobs = {}
        self.finished_jobs = {}
        self.operands = {}
        self.scan = defaultdict(dict)
        self.feedback = {}
        self.windows = []

    def feed(self, line):
        prefix = line.partition(b' ')[0]
        if prefix not in PREFIXES:
            return
        row = {key.decode(): value.decode() for key, value in FIELDS.findall(line)}
        self.counts[prefix.decode()] += 1
        if prefix == b'CFD_RHS_READY':
            self.rhs_ready(row)
        elif prefix.startswith(b'CFD_RF_') or prefix == b'CFD_LOCAL_WRITE':
            self.rf_event(prefix, row)
        elif prefix == b'CFD_MVM_BIND':
            identity = int(row['id'])
            require(identity in self.rf and self.rf[identity]['cycle'] == int(row['cycle']),
                    'MVM binding does not match physical RF issue')
            op = self.rf[identity]
            require('binding' not in op, 'duplicate MVM binding')
            require((op['pool'] == 'mul') == (row['multiply'] == '1'), 'MVM binding pool mismatch')
            op['binding'] = row
        elif prefix == b'CFD_DMA_PACKET':
            self.dma_event(row)
        elif prefix == b'CFD_TILE_DMA':
            self.tile_event(row)
        elif prefix.startswith(b'CFD_ONCHIP_'):
            self.onchip_event(prefix, row)
        elif prefix == b'CFD_BACK_OPERAND' and row['event'] == 'read':
            job = self.jobs[int(row['job'])]
            require(job['op'] == 'R' and 'kind' not in job, 'backward read job binding mismatch')
            job.update(kind='backward_read', token=int(row['token']),
                       cell=int(row['cell']), column=int(row['column']))
        elif prefix == b'CFD_LINE_FEEDBACK':
            self.finish_cell(int(row['token']), int(row['cell']), int(row['finish']))
        elif prefix == b'CFD_LINE_SCHEDULE':
            self.windows.append({key: row[key] for key in (
                'token', 'cache_hit', 'backward_start', 'backward_done', 'visible_done')})

    def rhs_ready(self, row):
        cycle = int(row['cycle'])
        if cycle != self.rhs_seen_cycle:
            self.rhs_seen_cycle, self.rhs_seen = cycle, set()
        identity = (row['token'], row['generation'], row['phase'], row['k'])
        require(identity not in self.rhs_seen, 'duplicate RHS group snapshot')
        self.rhs_seen.add(identity)
        mask, base = int(row['mask']), int(row['base_mask'])
        width = int(row['width'])
        require(0 < mask < (1 << 15) and base & ~mask == 0, 'invalid RHS ready masks')
        require(width == 4-int(row['k']) and 1 <= width <= 4, 'invalid RHS step width')
        phase = row['phase']
        require(phase in ('forward', 'backward'), 'invalid RHS phase')
        count, background = mask.bit_count(), (mask & ~base).bit_count()
        self.rhs_histograms[phase][count] += 1
        self.counts['rhs_ready_group_snapshots'] += 1
        if count >= 2:
            self.rhs_cycles['ready_pair'].add(cycle)
        if background < 2:
            return
        self.rhs_cycles['background_pair'].add(cycle)
        self.counts[phase+'_background_pair_snapshots'] += 1
        # A hypothetical two-RHS factor-sharing datapath. These are necessary
        # instantaneous resource conditions, not an implemented pair grant.
        paired_reads = width+2 if phase == 'forward' else 3*width
        separate_reads = 2*(width+1) if phase == 'forward' and row['preprocess_broadcast'] == '1' and width > 1 else 4*width
        self.samples['paired_'+phase+'_read_saving_per_snapshot'].add(separate_reads-paired_reads)
        demands = dict(mul_free=2*width, read_free=paired_reads,
                       product_free=2, return_free=2*width)
        if row['local_products'] == '1':
            demands['local_tags_free'] = 2
        blocked = [name for name, demand in demands.items() if int(row[name]) < demand]
        for name in blocked:
            self.counts[phase+'_pair_blocked/'+name] += 1
        if not blocked:
            self.rhs_cycles[phase+'_resource_feasible'].add(cycle)
            self.counts[phase+'_resource_feasible_snapshots'] += 1

    def rf_event(self, prefix, row):
        identity, cycle = int(row['id']), int(row['cycle'])
        if prefix == b'CFD_RF_ISSUE':
            require(identity > self.last_rf_id and identity not in self.rf, 'duplicate/nonmonotonic RF identity')
            self.last_rf_id = identity
            op = dict(cycle=cycle, ready=int(row['ready']), pool=row['pool'],
                      caller=row['caller'], lanes=int(row['lanes']),
                      reads=int(row['read_words']), route=int(row.get('bank_route', 0)),
                      destination=int(row.get('bank_dst', -1)))
            require(op['ready'] >= cycle, 'negative arithmetic latency')
            self.rf[identity] = op
            return
        require(identity in self.rf, 'unknown RF identity')
        op = self.rf[identity]
        if prefix == b'CFD_RF_OWNER':
            require(cycle == op['cycle'], 'RF owner binding cycle mismatch')
            op['owner'] = int(row['owner'])
            op['frontier'] = int(row['frontier'])
        elif prefix in (b'CFD_RF_WRITE', b'CFD_LOCAL_WRITE'):
            require(cycle >= op['ready'], 'write before nominal completion')
            op.setdefault('first_write', cycle)
        elif prefix == b'CFD_RF_COMPLETE':
            require(cycle >= op['ready'], 'RF return before nominal completion')
            require(int(row['nominal']) == op['ready'], 'RF nominal identity mismatch')
            delay = cycle-op['ready']
            require(delay >= op['route'], 'local result returned before declared route')
            caller = op['caller']
            counts = self.rf_counts[caller]
            counts['operations'] += 1
            counts['scalar_operations'] += op['lanes']
            counts['read_words'] += op['reads']
            counts['delayed_operations'] += delay > 0
            counts['frontier_at_issue_operations'] += op.get('owner', 0) != 0 and op.get('owner') == op.get('frontier')
            self.rf_samples[caller]['actual_return_delay'].add(delay)
            if 'owner' in op:
                scope = 'frontier_at_issue' if op.get('owner', 0) != 0 and op.get('owner') == op.get('frontier') else 'other_at_issue'
                self.rf_samples[caller][scope+'_return_delay'].add(delay)
            self.rf_samples[caller]['delay_excluding_declared_local_route'].add(delay-op['route'])
            self.rf_samples[caller]['nominal_to_first_write'].add(op.get('first_write', cycle)-op['ready'])
            op['complete'] = cycle
            binding = op.get('binding')
            if binding and binding['context'] == '1':
                key = int(binding['token']), int(binding['cell'])
                node = ('mul' if binding['multiply'] == '1' else 'add', int(binding['node']))
                require(node not in self.scan[key], 'duplicate backward physical node')
                self.scan[key][node] = op
            del self.rf[identity]

    def dma_event(self, row):
        identity, tick = int(row['packet']), int(row['tick'])
        event = row['event']
        if event == 'queue':
            require(identity not in self.packets, 'duplicate DMA packet')
            self.packets[identity] = dict(queue=tick, bytes=int(row['bytes']), op=row['op'])
        elif event == 'send':
            packet = self.packets[identity]
            require('send' not in packet and tick >= packet['queue'], 'invalid DMA send')
            packet['send'] = tick
        elif event == 'response':
            packet = self.packets.pop(identity)
            require('send' in packet and tick >= packet['send'], 'DMA response before send')
            packet['response'] = tick

    def tile_event(self, row):
        identity, cycle, job_id = int(row['packet']), int(row['cycle']), int(row['job'])
        key = tuple(row[name] for name in ('token', 'first', 'region', 'bytes', 'offset', 'job'))
        if row['event'] == 'issue':
            require(identity not in self.tiles and identity in self.packets, 'tile packet identity mismatch')
            require(job_id in self.jobs and self.jobs[job_id]['op'] == 'W', 'tile has no SPM landing job')
            packet = self.packets[identity]
            require(packet['op'] == 'R' and packet['bytes'] == int(row['bytes']), 'tile packet payload mismatch')
            self.tiles[identity] = dict(key=key, raw=packet, issue=cycle)
            self.jobs[job_id].update(kind='tile_write', packet=identity)
        elif row['event'] == 'response':
            require(identity in self.tiles, 'unknown tile response')
            tile = self.tiles.pop(identity)
            require(tile['key'] == key, 'tile response owner/range mismatch')
            require(job_id in self.finished_jobs, 'tile response before SPM completion')
            job = self.finished_jobs.pop(job_id)
            require(job['complete'] == cycle, 'tile published outside actual SPM completion')
            raw = tile['raw']
            require('response' in raw, 'tile published before actual DMA response')
            self.samples['queue_to_send'].add((raw['send']-raw['queue'])/self.period)
            self.samples['send_to_dma_response'].add((raw['response']-raw['send'])/self.period)
            self.samples['dma_response_to_spm_ready'].add(cycle-raw['response']/self.period)
            self.samples['spm_activation_to_first_access'].add(job['first_access']-job['activate'])
            self.samples['spm_first_access_to_ready'].add(cycle-job['first_access'])
            self.samples['tile_issue_to_spm_ready'].add(cycle-tile['issue'])
            self.counts['tile_packets_joined'] += 1
            self.counts['tile_read_bytes_joined'] += int(row['bytes'])

    def onchip_event(self, prefix, row):
        identity, cycle = int(row['id']), int(row['cycle'])
        if prefix == b'CFD_ONCHIP_START':
            require(identity not in self.jobs, 'duplicate SPM job')
            job = dict(start=cycle, words=int(row['words']), op=row['op'], accessed=0)
            if row['active'] == '1':
                job['activate'] = cycle
            self.jobs[identity] = job
            return
        require(identity in self.jobs, 'unknown SPM job')
        job = self.jobs[identity]
        if prefix == b'CFD_ONCHIP_ACTIVATE':
            require('activate' not in job, 'duplicate SPM activation')
            job['activate'] = cycle
        elif prefix == b'CFD_ONCHIP_ACCESS':
            require('activate' in job and cycle >= job['activate'], 'SPM service before activation')
            require(row['op'] == job['op'], 'SPM direction mismatch')
            job.setdefault('first_access', cycle)
            job['accessed'] += 1
        elif prefix == b'CFD_ONCHIP_COMPLETE':
            require(job['accessed'] == job['words'], 'SPM completed without all physical words')
            job['complete'] = cycle
            kind = job.get('kind', 'other')
            if kind == 'tile_write':
                self.finished_jobs[identity] = job
            elif kind == 'backward_read':
                self.samples['backward_spm_read_queue'].add(job['first_access']-job['activate'])
                self.samples['backward_spm_read_service'].add(cycle-job['first_access'])
                operand_key = job['token'], job['cell'], job['column']
                require(operand_key not in self.operands, 'duplicate backward operand load')
                self.operands[operand_key] = cycle
            del self.jobs[identity]
        elif prefix == b'CFD_ONCHIP_CANCEL':
            require(job['accessed'] == 0, 'SPM cancellation after service')
            del self.jobs[identity]

    def finish_cell(self, token, cell, finish):
        key = token, cell
        self.feedback[key] = finish
        if key not in self.scan:
            return  # Old logs lack physical MVM bindings; report that coverage.
        nodes = self.scan.pop(key)
        require(set(nodes) == {(pool, node) for pool in ('mul', 'add') for node in range(5)},
                'backward feedback lacks its ten physical arithmetic nodes')
        require(nodes['add', 4]['complete'] == finish, 'feedback does not match actual final RF return')
        products = [nodes['mul', node] for node in range(5)]
        self.samples['first_product_to_feedback'].add(finish-min(op['cycle'] for op in products))
        parents = {0: [('mul', 0)], 1: [('mul', 1), ('mul', 2)],
                   2: [('mul', 3), ('mul', 4)], 3: [('add', 0), ('add', 1)],
                   4: [('add', 3), ('add', 2)]}
        for node, dependencies in parents.items():
            ready = max(nodes[parent]['complete'] for parent in dependencies)
            if node == 0:
                operand_key = token, cell, -1
                require(operand_key in self.operands, 'backward y readiness is not physically bound')
                ready = max(ready, self.operands[operand_key])
            issue = nodes['add', node]['cycle']
            require(issue >= ready, 'feedback consumed a physically unreturned parent')
            self.samples['feedback_node_'+str(node)+'_ready_to_issue'].add(issue-ready)
        predecessor = self.feedback.get((token, cell+1))
        for column, op in enumerate(products):
            operand_key = token, cell, column
            require(operand_key in self.operands, 'backward column has no physical read')
            ready = self.operands[operand_key]
            require(op['cycle'] >= ready, 'multiply used a column before its actual SPM read')
            self.samples['column_ready_to_multiply'].add(op['cycle']-ready)
        if predecessor is not None:
            first = min(products, key=lambda op: op['cycle'])
            column = int(first['binding']['node'])
            operand_ready = self.operands[token, cell, column]
            self.samples['predecessor_to_first_product'].add(first['cycle']-predecessor)
            self.samples['first_product_operand_late_after_feedback'].add(max(0, operand_ready-predecessor))
            self.samples['first_product_ready_to_issue'].add(first['cycle']-max(predecessor, operand_ready))
        for column in range(-1, 5):
            self.operands.pop((token, cell, column))
        self.counts['backward_cells_physically_bound'] += 1

    def result(self, require_drained=False):
        if require_drained:
            require(not self.rf and not self.jobs and not self.packets and not self.tiles and
                    not self.finished_jobs and not self.scan, 'observed operations did not drain')
        pairing = dict(
            ready_group_snapshots=self.counts['rhs_ready_group_snapshots'],
            ready_pair_cycle_count=len(self.rhs_cycles['ready_pair']),
            background_pair_cycle_count=len(self.rhs_cycles['background_pair']),
            forward_resource_feasible_cycle_count=len(self.rhs_cycles['forward_resource_feasible']),
            backward_resource_feasible_cycle_count=len(self.rhs_cycles['backward_resource_feasible']),
            ready_count_histograms={name: dict(counts) for name, counts in self.rhs_histograms.items()},
            snapshot_counts={key: count for key, count in self.counts.items() if '_pair_' in key or '_feasible_' in key},
            paired_forward_read_saving_per_snapshot=self.samples['paired_forward_read_saving_per_snapshot'].summary(),
            paired_backward_read_saving_per_snapshot=self.samples['paired_backward_read_saving_per_snapshot'].summary(),
            interpretation='Snapshots can repeat the same ready operations. Resource feasibility is a necessary pre-scheduler snapshot, excludes base RHS, and is not a pair grant or a predicted cycle saving.')
        callers = {}
        for caller, counts in self.rf_counts.items():
            callers[caller] = dict(counts)
            callers[caller].update({name: sample.summary() for name, sample in self.rf_samples[caller].items()})
        tile_names = ('queue_to_send', 'send_to_dma_response', 'dma_response_to_spm_ready',
                      'spm_activation_to_first_access', 'spm_first_access_to_ready',
                      'tile_issue_to_spm_ready', 'backward_spm_read_queue', 'backward_spm_read_service')
        feedback_names = ('first_product_to_feedback', 'column_ready_to_multiply',
                          'predecessor_to_first_product', 'first_product_operand_late_after_feedback',
                          'first_product_ready_to_issue',
                          *(f'feedback_node_{node}_ready_to_issue' for node in range(5)))
        reported_counts = dict(self.counts)
        for name in ('tile_packets_joined', 'tile_read_bytes_joined',
                     'backward_cells_physically_bound'):
            reported_counts.setdefault(name, 0)
        return dict(
            status='passed', ticks_per_controller_cycle=self.period, event_counts=reported_counts,
            rhs_pairing=pairing, rf=dict(by_caller=callers,
                owner_scope='Frontier at issue time only; not a criticality claim at actual result return.'),
            tile={name: self.samples[name].summary() for name in tile_names},
            feedback={name: self.samples[name].summary() for name in feedback_names},
            windows=self.windows,
            notes=['Operation latencies and ready snapshots overlap and are not additive sweep stalls.',
                   'Raw DMA response joins packet ID; Tile response joins actual SPM completion and job ID.',
                   'send_to_dma_response includes the cache/memory path; it is not isolated DDR latency.',
                   'First-product arrival is one necessary input, not all operands of the cell.'])


def analyze_case(path, require_diagnostics=True):
    path = Path(path).resolve()
    resolved = json.loads((path/'resolved_experiment.json').read_text())
    period = int(resolved['controller_environment']['GEM5_CFD_TICKS_PER_CYCLE'])
    replay = Round1Replay(period)
    raw = path/'simout.txt'
    archived = path/'simout.txt.gz'
    digest = hashlib.sha256()
    opener = raw.open if raw.exists() else lambda mode: gzip.open(archived, mode)
    with opener('rb') as source:
        for number, line in enumerate(source, 1):
            digest.update(line)
            try:
                replay.feed(line)
            except (ValueError, KeyError) as error:
                raise ValueError(f'{path.name}: line {number}: {error}') from error
    note = path/'simout.txt.archive.json'
    if note.exists():
        expected = json.loads(note.read_text())['original_sha256']
        require(digest.hexdigest() == expected, 'decoded simulation trace hash mismatch')
    if require_diagnostics:
        require(resolved['options'].get('coeff_round1_diagnostics') is True,
                'round-one diagnostics were not enabled in the effective configuration')
        require(replay.counts['CFD_RF_OWNER'] == replay.counts['CFD_RF_ISSUE'],
                'physical RF owner coverage is incomplete')
        # One-cell lines have no backward recurrence or possible RHS partner.
        cells = int(resolved['options']['lusgs_cells'])
        require(cells == 1 or replay.counts['CFD_MVM_BIND'] > 0, 'physical MVM bindings missing')
    result = replay.result(require_drained=True)
    result.update(path=str(path), decoded_simout_sha256=digest.hexdigest(),
                  diagnostics_required=require_diagnostics)
    summary_path = path/'summary.json'
    if summary_path.exists():
        summary = json.loads(summary_path.read_text())
        result['controller_cycles'] = summary['controller_cycles']
        expected_reads = summary['rf']['read_words']
        actual_reads = sum(row['read_words'] for row in result['rf']['by_caller'].values())
        require(actual_reads == expected_reads, 'RF word total differs from accepted independent replay')
        expected_bytes = summary.get('storage', {}).get('tile_read', None)
        result['accepted_summary_crosscheck'] = dict(rf_read_words=actual_reads,
            accepted_tile_read_bytes=expected_bytes)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-legacy', action='store_true')
    args = parser.parse_args()
    result = analyze_case(args.case, not args.allow_legacy)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(status=result['status'], case=args.case.name,
        controller_cycles=result.get('controller_cycles'),
        rhs_pair_cycles=result['rhs_pairing']['background_pair_cycle_count'],
        tile_packets=result['event_counts']['tile_packets_joined'],
        bound_backward_cells=result['event_counts']['backward_cells_physically_bound'])))


if __name__ == '__main__':
    main()
