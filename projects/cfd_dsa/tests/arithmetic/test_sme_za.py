"""Native checks for masked ZA dependencies and finite writeback."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import _support

sys.path.insert(0, str(_support.REPO_ROOT / 'projects/cfd_dsa/configs'))
from check_single_line_output_dma import resolved_options
from check_single_line_storage import check_prefetch_lifetimes
from check_sme_za import check_panel_ingress
from single_line_contract import sme_za_reserved_bytes, sme_za_return_slot_bytes


class SmeZaTests(unittest.TestCase):
    def test_ab_manifest_must_match_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for key in ('coeff_sme_2d_mixed_kind', 'coeff_sme_2d_panel_pack'):
                with self.subTest(key=key):
                    flag = '--' + key.replace('_', '-')
                    (out / 'resolved_experiment.json').write_text(
                        json.dumps({'options': {key: True}}))
                    manifest = out / 'manifest.json'
                    manifest.write_text(json.dumps({'command': ['config.py', flag + '=0']}))
                    with self.assertRaisesRegex(ValueError, flag):
                        resolved_options(out)
                    manifest.write_text(json.dumps({'command': ['config.py', flag + '=ON']}))
                    self.assertEqual(resolved_options(out)[flag], '1')

    def test_fused_panel_a_requires_upper_before_early_load(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            trace = out / 'coeff_preprocess_trace.csv'
            header = 'stage,requestId,rhsBatch,rhsColumn,cell,note\n'
            inputs = 'INPUT_R_READY,1,,,1,\nINPUT_L_READY,1,,,1,\n'
            fused = 'SME_RHS_PANEL_LOAD,1,0,255,1,packed-a\n'
            trace.write_text(header + inputs + fused)
            with self.assertRaisesRegex(ValueError, 'before its timed input'):
                check_panel_ingress(out, True)
            trace.write_text(header + inputs + 'INPUT_U_READY,1,,,1,\n' + fused)
            self.assertEqual(check_panel_ingress(out, True, panel_pack=True)['packed_a'], 1)
            trace.write_text(header + inputs + 'SME_RHS_PANEL_LOAD,1,0,63,1,\n')
            self.assertEqual(check_panel_ingress(out, True)['early_rl'], 1)

    def test_fused_panel_b_can_load_unit_identity_before_lu(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            trace = out / 'coeff_preprocess_trace.csv'
            inputs = 'stage,requestId,rhsBatch,rhsColumn,cell,note\nINPUT_TOKEN,1,,,1,\n'
            fused = 'SME_RHS_PANEL_LOAD,1,1,7,1,packed-b\n'
            trace.write_text(inputs + fused)
            with self.assertRaisesRegex(ValueError, 'before LU completion'):
                check_panel_ingress(out, False, panel_pack=True)
            trace.write_text(inputs + 'LU_COMPLETE,1,,,1,\n' + fused)
            self.assertEqual(check_panel_ingress(out, False, panel_pack=True)['packed_b'], 1)

            identity = 'SME_RHS_PANEL_LOAD,1,1,255,1,packed-b-identity\n'
            published = 'SME_INV_COMPLETE,1,,,1,\n'
            lu_done = 'LU_COMPLETE,1,,,1,\n'
            fallback = 'SME_INV_GUARD_FALLBACK,1,,,1,\n'
            trace.write_text(inputs + identity + lu_done + published)
            self.assertEqual(check_panel_ingress(out, False, panel_pack=True)[
                'packed_b_before_lu'], 1)
            for invalid in (identity + published, identity + fallback + lu_done + published):
                trace.write_text(inputs + invalid)
                with self.assertRaisesRegex(ValueError, 'before a successful LU guard'):
                    check_panel_ingress(out, False, panel_pack=True)

    def test_native_masks_capacity_and_writeback(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'sme_za'
            subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(_support.REPO_ROOT / 'src'),
                str(_support.FIXTURES / 'sme_za_model.cc'), '-o', str(binary),
            ], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

    def test_2d_reserved_bytes_contract(self):
        base = sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 0, 1)
        self.assertEqual(sme_za_return_slot_bytes(0), 128)
        self.assertEqual(sme_za_return_slot_bytes(3), 640)
        two_d = base + 32 * (640 - 128)
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 1, 1),
            two_d + 128,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 2, 3),
            two_d + 256,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 3, 2),
            two_d + 384,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 3, 3),
            two_d + 512,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 3, 3, True, False),
            two_d + 640,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 3, 3, False, True),
            two_d + 640,
        )
        self.assertEqual(
            sme_za_reserved_bytes(4, 32, 2, 0, False, False, False, True, True, 0, 3, 3, True, True),
            two_d + 768,
        )

    def test_v35_new_flags_manifest_match(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            for key, val, bad, good, expected in (
                ('coeff_sme_2d_panel_b_overlap', True, '0', '1', '1'),
                ('coeff_sme_forward_tile_rotate', True, '0', 'ON', '1'),
                ('coeff_sme_2d_candidate_window', 8, '0', '8', '8'),
                ('coeff_sme_feedback_reg', True, '0', '1', '1'),
            ):
                with self.subTest(key=key):
                    flag = '--' + key.replace('_', '-')
                    (out / 'resolved_experiment.json').write_text(
                        json.dumps({'options': {key: val}}))
                    manifest = out / 'manifest.json'
                    manifest.write_text(json.dumps({'command': ['config.py', f'{flag}={bad}']}))
                    with self.assertRaisesRegex(ValueError, flag):
                        resolved_options(out)
                    manifest.write_text(json.dumps({'command': ['config.py', f'{flag}={good}']}))
                    self.assertEqual(resolved_options(out)[flag], expected)

    def test_prefetch_lifetimes_counterexamples(self):
        config = {
            '--lusgs-cells': '65',
            '--coeff-line-tile-cells': '32',
            '--coeff-line-tile-buffers': '2',
            '--coeff-tile-prefetch-buffers': '4',
            '--coeff-sme-forward-tile-rotate': '1',
        }
        # Build a clean rotated 65-cell sweep (token=1, 3 tiles: 0, 32, 64)
        # plus an aborted sweep (token=2, visible abort at cycle=500).
        lines = [
            'CFD_LINE_STORAGE token=1 cells=65 tile_cells=32 tile_y_only=1',
            'CFD_TILE_EVENT cycle=10 token=1 event=forward_allocate first=0 cells=32 storage=0',
            'CFD_TILE_EVENT cycle=10 token=1 event=forward_allocate first=32 cells=32 storage=1',
            'CFD_TILE_EVENT cycle=10 token=1 event=forward_allocate first=64 cells=1 storage=2',
        ]
        for cell in range(65):
            first = (cell // 32) * 32
            slot = (cell // 32) % 6
            lines.append(f'CFD_LOCAL_READY cycle={20 + cell} token=1 cell={cell} first={first} storage={slot}')
            lines.append(f'CFD_CACHE_DMA cycle={90 + cell} token=1 event=response kind=y cell={cell} bytes=40 packet={cell} job=0')
        lines += [
            'CFD_TILE_EVENT cycle=130 token=1 event=forward_commit first=0 cells=32 storage=0 last_write=121',
            'CFD_TILE_EVENT cycle=170 token=1 event=forward_commit first=32 cells=32 storage=1 last_write=153',
            'CFD_TILE_EVENT cycle=180 token=1 event=forward_commit first=64 cells=1 storage=2 last_write=154',
            # Aborted sweep token=2: allocates slot 0 and publishes 2 cells before abort at 500
            'CFD_TILE_EVENT cycle=400 token=2 event=forward_allocate first=0 cells=32 storage=0',
            'CFD_LOCAL_READY cycle=420 token=2 cell=0 first=0 storage=0',
            'CFD_LOCAL_READY cycle=425 token=2 cell=1 first=0 storage=0',
        ]
        good_text = '\n'.join(lines) + '\n'
        aborted = {'2': {'cycle': '500'}}
        schedules = {'1': {'token': '1'}}

        # Valid trace passes.
        check_prefetch_lifetimes(
            good_text, config, aborted=aborted, schedules=schedules, forward_rotate=True,
        )

        # Counterexample 1: deleting all forward_allocate events must fail.
        no_alloc = '\n'.join(l for l in lines if 'event=forward_allocate' not in l) + '\n'
        with self.assertRaises(AssertionError):
            check_prefetch_lifetimes(
                no_alloc, config, aborted=aborted, schedules=schedules, forward_rotate=True,
            )

        # Counterexample 2: invalid storage slot 99 on an aborted sweep must fail.
        bad_abort = good_text.replace(
            'token=2 event=forward_allocate first=0 cells=32 storage=0',
            'token=2 event=forward_allocate first=0 cells=32 storage=99',
        )
        with self.assertRaises(AssertionError):
            check_prefetch_lifetimes(
                bad_abort, config, aborted=aborted, schedules=schedules, forward_rotate=True,
            )

        # Counterexample 3: external write response later than physical slot reuse must fail.
        # Add a reuse of storage=0 via reload_start at cycle=140 while write response for cell 31 is 145.
        reuse_lines = list(lines) + [
            'CFD_TILE_EVENT cycle=140 token=1 event=reload_start first=0 cells=32 storage=0',
            'CFD_TILE_DMA cycle=141 token=1 event=issue first=0 region=0 bytes=1280 packet=999 offset=0 job=1',
            'CFD_TILE_DMA cycle=142 token=1 event=response first=0 region=0 bytes=1280 packet=999 offset=0 job=1',
        ]
        late_write_text = '\n'.join(reuse_lines).replace(
            'CFD_CACHE_DMA cycle=121 token=1 event=response kind=y cell=31 bytes=40 packet=31 job=0',
            'CFD_CACHE_DMA cycle=145 token=1 event=response kind=y cell=31 bytes=40 packet=31 job=0',
        ) + '\n'
        with self.assertRaises(AssertionError):
            check_prefetch_lifetimes(
                late_write_text, config, aborted=aborted, schedules=schedules, forward_rotate=True,
            )

    def test_gem5_rtl_writeback_conflict_and_grant_equivalence(self):
        verilator = Path('/home/zyy/.local/bin/verilator-cli')
        rtl_issue = _support.REPO_ROOT / 'fpga/cfd_lusgs/rtl/sme_za_rs_issue.sv'
        rtl_slot = _support.REPO_ROOT / 'fpga/cfd_lusgs/rtl/sme_za_return_slot.sv'
        if not verilator.exists() or not rtl_issue.exists() or not rtl_slot.exists():
            self.skipTest('verilator-cli or SME RTL modules not available')
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <tuple>
#include <vector>
#include "arch/arm/cfd_sme_za.hh"
#include "arch/arm/cfd_sme_2d_arbiter.hh"
#include "Vsme_za_rs_issue.h"
#include "Vsme_za_return_slot.h"

using Sme = gem5::ArmISA::CfdSmeZa;
using CandMeta = gem5::ArmISA::Sme2dCandidateMeta;

static void setBits(uint32_t *words, unsigned bitOffset, unsigned width, uint64_t value) {
    for (unsigned i = 0; i < width; ++i) {
        const unsigned idx = bitOffset + i;
        if ((value >> i) & 1ull)
            words[idx / 32] |= (1u << (idx % 32));
        else
            words[idx / 32] &= ~(1u << (idx % 32));
    }
}

int main() {
    Vsme_za_rs_issue dut;

    auto driveAndCompare = [&](Sme &mk, const std::vector<CandMeta> &offers) {
        const unsigned end = std::min<unsigned>(offers.size(), 8u);
        dut.return_credit = !mk.queueFull();
        dut.mixed_kind = mk.config().mixedKind;
        dut.read_credits = mk.freeReads();
        dut.ingress_credits = mk.freeIngress();
        for (int w = 0; w < 6; ++w) dut.nominal_writeback[w] = 0;
        setBits(dut.nominal_writeback.data(), 0, 64, mk.nominalWritebackBitmap2d(Sme::Fma));
        setBits(dut.nominal_writeback.data(), 64, 64, mk.nominalWritebackBitmap2d(Sme::Mul));
        setBits(dut.nominal_writeback.data(), 128, 64, mk.nominalWritebackBitmap2d(Sme::Add));

        dut.valid = 0; dut.rows = 0; dut.cols = 0; dut.kind = 0;
        dut.critical_group = 0; dut.priority_rank = 0; dut.active_pes = 0;
        for (int w = 0; w < 4; ++w) dut.fresh_reads[w] = 0;
        dut.uses_ingress = 0; dut.bypass = 0; dut.latch_id = 0;

        // Map unique fresh read slices in the 8-candidate window to 16-bit IDs.
        std::vector<Sme::ReadSlice> sliceDict;
        auto sliceBit = [&](const Sme::ReadSlice &s) -> uint16_t {
            for (unsigned idx = 0; idx < sliceDict.size(); ++idx)
                if (sliceDict[idx] == s)
                    return uint16_t(1u << idx);
            assert(sliceDict.size() < 16);
            const unsigned idx = sliceDict.size();
            sliceDict.push_back(s);
            return uint16_t(1u << idx);
        };

        for (unsigned i = 0; i < end; ++i) {
            const auto &o = offers[i];
            const auto &seg = o.seg;
            dut.valid |= (1u << i);
            dut.rows |= (uint64_t(seg.rowMask) << (i * 8));
            dut.cols |= (uint64_t(seg.colMask) << (i * 8));
            dut.kind |= (uint32_t(Sme::opKind(seg.op)) << (i * 2));
            dut.critical_group |= (uint32_t(o.critGroup & 3u) << (i * 2));
            dut.priority_rank |= (uint32_t(o.priority & 7u) << (i * 3));
            dut.active_pes |= (uint64_t(o.activePes & 0x7Fu) << (i * 7));

            Sme::SegmentReads slices{};
            const unsigned n = mk.collectReads2d(seg, slices);
            uint16_t readMask = 0;
            for (unsigned r = 0; r < n; ++r)
                if (!mk.readSliceUsed(slices[r]))
                    readMask |= sliceBit(slices[r]);
            setBits(dut.fresh_reads.data(), i * 16, 16, readMask);
            if (Sme::opUsesIngress(seg.op))
                dut.uses_ingress |= (1u << i);
            if (Sme::opIsBypass(seg.op)) {
                dut.bypass |= (1u << i);
                dut.latch_id |= (uint64_t(seg.dstTile & 0x3Fu) << (i * 6));
            }
        }
        dut.eval();

        // Call PRODUCTION gem5 2D bundle selector directly!
        const auto choice = gem5::ArmISA::selectSme2dBundle(
            mk, offers.data(), offers.size(), 8u);
        for (unsigned slot = 0; slot < 3; ++slot) {
            const bool rtlValid = ((dut.grant_valid >> slot) & 1u) != 0;
            const unsigned rtlIdx = (dut.grant_index >> (slot * 3)) & 7u;
            assert(rtlValid == (slot < choice.chosenCount));
            if (rtlValid)
                assert(rtlIdx == choice.chosen[slot]);
        }
        return choice;
    };

    // 1. Targeted case: low-priority Rhs Fmopa booked at cycle 13 on PE row 1..4;
    //    high-priority Forward ScaleRow arrives at cycle 9 alongside clean row 5.
    {
        Sme mk;
        Sme::Config cfg;
        cfg.arrayMode = Sme::ArrayMode::TwoDRs;
        cfg.lanes = 64; cfg.readPorts = 6; cfg.writePorts = 3;
        cfg.maxSegments = 3; cfg.mixedKind = true; cfg.ingressPorts = 3;
        cfg.fmaLatency = 5; cfg.mulLatency = 4; cfg.addLatency = 4;
        mk.configure(1, cfg);
        for (unsigned r = 0; r < 5; ++r) {
            mk.tick(1 + r);
            assert(mk.load({0, r}, 0x1f, {1., 2., 3., 4., 5.}, Sme::Lu));
        }
        mk.tick(8);
        assert(mk.load({1, 1}, 0x1e, {1., 2., 3., 4., 5.}, Sme::Forward));
        assert(mk.load({1, 5}, 0x1e, {1., 2., 3., 4., 5.}, Sme::Forward));
        Sme::Segment2d rhsOp{};
        rhsOp.op = Sme::Op2d::Fmopa; rhsOp.phase = Sme::Rhs;
        rhsOp.rowMask = 0x1e; rhsOp.colMask = 0x1e;
        rhsOp.dstTile = 0; rhsOp.aPos = {0, 0}; rhsOp.aCol = 0; rhsOp.bPos = {0, 0};
        assert(mk.issue2dBundle(&rhsOp, 1));
        mk.tick(9);

        Sme::Segment2d fwdCollide{};
        fwdCollide.op = Sme::Op2d::ScaleRow; fwdCollide.phase = Sme::Forward;
        fwdCollide.rowMask = 1u << 1; fwdCollide.colMask = 0x1e;
        fwdCollide.dstTile = 1; fwdCollide.dstRow = 1; fwdCollide.useScalar = true; fwdCollide.scalar = 2.0;
        Sme::Segment2d fwdClean = fwdCollide;
        fwdClean.rowMask = 1u << 5; fwdClean.dstRow = 5;

        assert(mk.willCollideWriteback2d(fwdCollide));
        assert(!mk.willCollideWriteback2d(fwdClean));

        std::vector<CandMeta> offers = {
            {fwdCollide, 1, 0, Sme::activePes2d(fwdCollide), 0},
            {fwdClean, 1, 0, Sme::activePes2d(fwdClean), 1},
        };
        const auto choice = driveAndCompare(mk, offers);
        assert(choice.chosenCount == 2);
        assert(choice.chosen[0] == 1u && choice.chosen[1] == 0u);
    }

    // 2. Multi-cycle stateful lockstep across real CfdSmeZa::tick() + selectSme2dBundle()
    //    + issue2dBundle() + Verilated sme_za_return_slot retirement (4 configs x 256 cycles).
    std::mt19937 rng(20261124);
    unsigned issuedBundles = 0, multiSegBundles = 0, fullQueueCycles = 0;
    for (unsigned modeIdx = 0; modeIdx < 4; ++modeIdx) {
        Sme mk;
        Sme::Config cfg;
        cfg.arrayMode = Sme::ArrayMode::TwoDRs;
        cfg.lanes = 64;
        cfg.readPorts = (modeIdx & 1) ? 4 : 6;
        cfg.writePorts = 3;
        cfg.maxSegments = 3;
        cfg.depth = 4; // small depth so queueFull backpressure is exercised dynamically
        cfg.mixedKind = (modeIdx < 3);
        cfg.ingressPorts = (modeIdx == 2) ? 1 : 3;
        cfg.fmaLatency = 5; cfg.mulLatency = 4; cfg.addLatency = 4;
        mk.configure(4, cfg, 0, 6);

        uint64_t cyc = 1;
        // Preload source tiles 0..11 and latches 0..5 with valid operands.
        for (unsigned t = 0; t < 12; ++t) {
            for (unsigned r = 0; r < 8; ++r) {
                mk.tick(cyc++);
                assert(mk.load({t, r}, 0xff, {1., 2., 3., 4., 5., 6., 7., 8.}, Sme::Lu));
            }
        }
        for (unsigned l = 0; l < 6; ++l) {
            mk.tick(cyc++);
            Sme::Segment2d ing{};
            ing.op = Sme::Op2d::IngressBypass; ing.phase = Sme::Forward;
            ing.dstTile = l; ing.dstRow = l & 7u; ing.rowMask = 1u << (l & 7u);
            ing.colMask = 0x1f; ing.ingressValues = {1., 1., 1., 1., 1., 0., 0., 0.};
            ing.scalar = 1.0;
            assert(mk.issue2dBundle(&ing, 1));
        }
        for (unsigned drain = 0; drain < 8; ++drain)
            mk.tick(cyc++);

        for (unsigned step = 0; step < 256; ++step, ++cyc) {
            mk.tick(cyc);
            if (mk.queueFull())
                ++fullQueueCycles;
            // Consume 0..2 read slices beforehand on some cycles to exercise readSliceUsed sharing
            if ((rng() % 3) == 0) {
                Sme::Row dummy{};
                mk.read({unsigned(rng() % 4), unsigned(rng() % 8)}, 0x1f, dummy, Sme::Rhs);
            }
            std::vector<CandMeta> offers;
            const unsigned attemptCount = 1 + (rng() % 12); // >8 tests candidateWindow=8 truncation
            for (unsigned a = 0; a < attemptCount; ++a) {
                Sme::Segment2d seg{};
                const unsigned pick = rng() % 6;
                const Sme::Phase phases[] = {Sme::Backward, Sme::Forward, Sme::Lu, Sme::Rhs};
                seg.phase = phases[rng() % 4];
                const unsigned dstTile = 4 + (rng() % 8); // keep tiles 0..3 clean read-only sources
                const unsigned srcTileA = rng() % 4;
                const unsigned srcTileB = rng() % 4;
                const unsigned r0 = rng() % 8;
                seg.colMask = uint8_t(1u + (rng() % 0x3fu));
                if (pick == 0 || pick == 1) {
                    seg.op = Sme::Op2d::Fmopa;
                    uint8_t rm = uint8_t(1u << r0);
                    if (rng() & 1) rm |= uint8_t(1u << ((r0 + 1) & 7u));
                    if ((rng() % 3) == 0) rm |= uint8_t(1u << ((r0 + 2) & 7u));
                    seg.rowMask = rm;
                    seg.dstTile = dstTile;
                    seg.aPos = {srcTileA, 0}; seg.aCol = rng() % 8;
                    seg.bPos = {srcTileB, unsigned(rng() % 8)};
                } else if (pick == 2) {
                    seg.op = Sme::Op2d::ScaleRow;
                    seg.dstTile = dstTile; seg.dstRow = r0; seg.rowMask = uint8_t(1u << r0);
                    seg.aPos = {srcTileA, unsigned(rng() % 8)}; seg.aCol = rng() % 8;
                } else if (pick == 3) {
                    seg.op = Sme::Op2d::CombineRow;
                    seg.dstTile = dstTile; seg.dstRow = r0; seg.rowMask = uint8_t(1u << r0);
                    seg.aPos = {srcTileA, unsigned(rng() % 8)};
                    seg.bPos = {srcTileB, unsigned(rng() % 8)};
                } else if (pick == 4) {
                    seg.op = Sme::Op2d::IngressRow;
                    seg.dstTile = dstTile; seg.dstRow = r0; seg.rowMask = uint8_t(1u << r0);
                    seg.ingressValues = {1., 2., 3., 4., 5., 6., 7., 8.}; seg.scalar = 0.5;
                } else {
                    seg.op = Sme::Op2d::ScaleRowToBypass;
                    seg.dstTile = rng() % 6; seg.dstRow = r0; seg.rowMask = uint8_t(1u << r0);
                    seg.bPos = {srcTileB, unsigned(rng() % 8)}; seg.scalar = 0.25;
                }
                if (!mk.ready2d(seg))
                    continue;
                CandMeta meta{};
                meta.seg = seg;
                meta.critGroup = rng() % 3;
                meta.priority = rng() % 7;
                meta.activePes = Sme::activePes2d(seg);
                meta.orderIndex = offers.size();
                offers.push_back(meta);
            }
            if (offers.empty())
                continue;
            const auto choice = driveAndCompare(mk, offers);
            if (choice.chosenCount > 0) {
                std::array<Sme::Segment2d, 4> bundle{};
                for (unsigned k = 0; k < choice.chosenCount; ++k)
                    bundle[k] = offers[choice.chosen[k]].seg;
                assert(mk.issue2dBundle(bundle.data(), choice.chosenCount));
                ++issuedBundles;
                if (choice.chosenCount > 1)
                    ++multiSegBundles;
            }
        }
    }
    assert(issuedBundles > 200 && multiSegBundles > 80 && fullQueueCycles > 0);

    // 3. Verilated sme_za_return_slot multi-cycle staggered arrival & stall retirement check.
    {
        Vsme_za_return_slot slot;
        auto stepClk = [&]() {
            slot.clk = 0; slot.eval();
            slot.clk = 1; slot.eval();
        };
        slot.reset = 1; slot.reserve_valid = 0; slot.result_valid = 0; slot.retire_ready = 0;
        for (int w = 0; w < 128; ++w) slot.result_data[w] = 0;
        stepClk();
        slot.reset = 0; slot.eval();
        assert(slot.reserve_ready == 1 && slot.retire_valid == 0);

        // Reserve 2 segments: seg0 rows 0x03 (Mul, arrives edge 1), seg1 rows 0x0c (Fma, arrives edge 2)
        slot.reserve_valid = 1;
        slot.reserve_segments = 0x3;
        slot.reserve_rows = 0x03u | (0x0cu << 8);
        slot.reserve_cols = 0x1fu | (0x1fu << 8);
        slot.reserve_tags = 0x101u | (0x202u << 12);
        stepClk();
        slot.reserve_valid = 0; slot.eval();
        assert(slot.reserve_ready == 0);

        // Seg 0 arrives, writeback port blocked (retire_ready = 0) -> buffered
        slot.result_valid = 0x1;
        slot.result_data[0] = 0xdeadbeefu;
        slot.retire_ready = 0x0;
        slot.eval();
        assert(slot.retire_valid == 0x1);
        stepClk();

        // Seg 1 arrives while Seg 0 retires and Seg 1 stays buffered, then Seg 1 retires allowing same-edge re-reserve
        slot.result_valid = 0x2;
        slot.result_data[32] = 0xcafebabeu;
        slot.retire_ready = 0x1;
        slot.eval();
        assert(slot.retire_valid == 0x3);
        assert(slot.retire_data[0] == 0xdeadbeefu && slot.retire_data[32] == 0xcafebabeu);
        stepClk();

        slot.result_valid = 0x0;
        slot.retire_ready = 0x2;
        slot.eval();
        assert(slot.retire_valid == 0x2 && slot.reserve_ready == 1);
        stepClk();
        slot.retire_ready = 0; slot.eval();
        assert(slot.retire_valid == 0 && slot.reserve_ready == 1);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            cpp = out / 'rtl_gem5_equiv.cc'
            cpp.write_text(harness)
            vinc = Path('/home/zyy/.local/lib/python3.10/site-packages/verilator/include')
            for rtl, prefix in ((rtl_issue, 'Vsme_za_rs_issue'), (rtl_slot, 'Vsme_za_return_slot')):
                subprocess.run([
                    str(verilator), '--cc',
                    '-Wno-WIDTHEXPAND', '-Wno-UNOPTFLAT',
                    '--prefix', prefix,
                    '-Mdir', str(out / 'obj'),
                    str(rtl),
                ], check=True, capture_output=True)
            v_cpps = sorted(str(p) for p in (out / 'obj').glob('*.cpp'))
            subprocess.run([
                'g++', '-std=c++17', '-O2',
                f'-I{out / "obj"}', f'-I{vinc}', f'-I{vinc / "vltstd"}',
                f'-I{_support.REPO_ROOT / "src"}',
                *v_cpps,
                str(vinc / 'verilated.cpp'),
                str(vinc / 'verilated_threads.cpp'),
                str(cpp), '-lpthread', '-o', str(out / 'rtl_gem5_equiv'),
            ], check=True, capture_output=True)
            res = subprocess.run([str(out / 'rtl_gem5_equiv')], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, msg=res.stdout + '\n' + res.stderr)
