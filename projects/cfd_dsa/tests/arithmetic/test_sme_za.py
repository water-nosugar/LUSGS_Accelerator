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
        rtl = _support.REPO_ROOT / 'fpga/cfd_lusgs/rtl/sme_za_rs_issue.sv'
        if not verilator.exists() or not rtl.exists():
            self.skipTest('verilator-cli or sme_za_rs_issue.sv not available')
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <tuple>
#include <vector>
#include "arch/arm/cfd_sme_za.hh"
#include "Vsme_za_rs_issue.h"

using Sme = gem5::ArmISA::CfdSmeZa;

struct Cand {
    bool valid = false;
    uint8_t rows = 0, cols = 0;
    uint8_t kind = 0; // 0=Fma, 1=Mul, 2=Add
    uint8_t crit = 0;
    uint8_t prio = 0;
    uint8_t pes = 0;
    uint16_t reads = 0;
    bool ingress = false;
    bool bypass = false;
    uint8_t latch = 0;
    Sme::Phase phase = Sme::Rhs;
};

static void setBits(uint32_t *words, unsigned bitOffset, unsigned width, uint64_t value) {
    for (unsigned i = 0; i < width; ++i) {
        const unsigned idx = bitOffset + i;
        if ((value >> i) & 1ull)
            words[idx / 32] |= (1u << (idx % 32));
        else
            words[idx / 32] &= ~(1u << (idx % 32));
    }
}

static unsigned pop16(uint16_t m) { return __builtin_popcount(m); }

int main() {
    Vsme_za_rs_issue dut;

    // 1. Targeted case: low-priority Rhs Fmopa already booked at cycle 13 on PE row 1,
    //    cols 1..4; high-priority Forward ScaleRow arrives at cycle 9 (lat 4 -> cyc 13)
    //    alongside an identical-size non-colliding Forward ScaleRow on row 2.
    {
        Sme mk;
        Sme::Config cfg;
        cfg.arrayMode = Sme::ArrayMode::TwoDRs;
        cfg.lanes = 64;
        cfg.readPorts = 6;
        cfg.writePorts = 3;
        cfg.maxSegments = 3;
        cfg.mixedKind = true;
        cfg.ingressPorts = 3;
        cfg.fmaLatency = 5;
        cfg.mulLatency = 4;
        cfg.addLatency = 4;
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

        // gem5 says fwdCollide collides (conservative RTL-aligned rule), fwdClean does not.
        assert(mk.willCollideWriteback2d(fwdCollide));
        assert(!mk.willCollideWriteback2d(fwdClean));

        // Drive identical inputs to RTL: nominal_writeback for Mul (op=1, lat=4 -> cyc 13)
        // has row 1..4 cols 1..4 occupied from rhsOp (fma lat=5 -> cyc 13).
        uint64_t due13 = 0;
        for (unsigned r = 1; r <= 4; ++r)
            due13 |= (uint64_t(0x1eu) << (r * 8));
        dut.return_credit = 1;
        dut.mixed_kind = 1;
        dut.read_credits = 6;
        dut.ingress_credits = 3;
        for (int w = 0; w < 6; ++w) dut.nominal_writeback[w] = 0;
        setBits(dut.nominal_writeback.data(), 64, 64, due13); // op=1 (Mul) due at cyc 9+4=13
        dut.valid = 0x03; // cand 0 = fwdCollide (row 1), cand 1 = fwdClean (row 5)
        dut.rows = (uint64_t(1u << 1)) | (uint64_t(1u << 5) << 8);
        dut.cols = (uint64_t(0x1eu)) | (uint64_t(0x1eu) << 8);
        dut.kind = (1u) | (1u << 2); // Mul
        dut.critical_group = 0; // both critical_group=0
        dut.priority_rank = (1u) | (1u << 3); // both Forward prio=1
        dut.active_pes = (4ull) | (4ull << 7);
        for (int w = 0; w < 4; ++w) dut.fresh_reads[w] = 0;
        dut.uses_ingress = 0;
        dut.bypass = 0;
        dut.latch_id = 0;
        dut.eval();
        // Anchor must pick cand 1 (fwdClean, no conflict) first, then cand 0 as filler!
        assert(dut.grant_valid == 0x03);
        assert((dut.grant_index & 7u) == 1u);
        assert(((dut.grant_index >> 3) & 7u) == 0u);
    }

    // 2. 256 randomized 8-candidate mixed-kind lockstep trials comparing
    //    exact gem5 anchorBefore/fillerKey + conservative writeback conflict against RTL.
    std::mt19937 rng(20261123);
    for (int trial = 0; trial < 256; ++trial) {
        const bool retCredit = (trial % 16 != 0);
        const bool mixed = (trial & 1) != 0;
        const unsigned readCredits = 1 + (rng() % 6);
        const unsigned ingressCredits = rng() % 4;
        std::array<uint64_t, 3> nomWb{};
        for (int k = 0; k < 3; ++k)
            nomWb[k] = (1ull << (rng() % 64)) | (1ull << (rng() % 64)) | (1ull << (rng() % 64));

        std::array<Cand, 8> cands{};
        dut.return_credit = retCredit;
        dut.mixed_kind = mixed;
        dut.read_credits = readCredits;
        dut.ingress_credits = ingressCredits;
        for (int w = 0; w < 6; ++w) dut.nominal_writeback[w] = 0;
        for (int k = 0; k < 3; ++k) setBits(dut.nominal_writeback.data(), k * 64, 64, nomWb[k]);
        dut.valid = 0; dut.rows = 0; dut.cols = 0; dut.kind = 0;
        dut.critical_group = 0; dut.priority_rank = 0; dut.active_pes = 0;
        for (int w = 0; w < 4; ++w) dut.fresh_reads[w] = 0;
        dut.uses_ingress = 0; dut.bypass = 0; dut.latch_id = 0;

        for (unsigned i = 0; i < 8; ++i) {
            auto &c = cands[i];
            c.valid = (rng() % 4) != 0;
            c.rows = uint8_t(1u << (rng() % 8));
            if (i & 1) c.rows |= uint8_t(1u << (rng() % 8));
            c.cols = uint8_t(1u + (rng() % 255));
            c.kind = rng() % 3;
            c.crit = rng() % 3;
            c.prio = rng() % 7;
            c.pes = __builtin_popcount(c.rows) * __builtin_popcount(c.cols);
            c.reads = uint16_t((1u << (rng() % 16)) | (1u << (rng() % 16)));
            c.ingress = (rng() % 3) == 0;
            c.bypass = (rng() % 4) == 0;
            c.latch = rng() % 4;
            if (c.valid) dut.valid |= (1u << i);
            dut.rows |= (uint64_t(c.rows) << (i * 8));
            dut.cols |= (uint64_t(c.cols) << (i * 8));
            dut.kind |= (uint32_t(c.kind) << (i * 2));
            dut.critical_group |= (uint32_t(c.crit) << (i * 2));
            dut.priority_rank |= (uint32_t(c.prio) << (i * 3));
            dut.active_pes |= (uint64_t(c.pes) << (i * 7));
            setBits(dut.fresh_reads.data(), i * 16, 16, c.reads);
            if (c.ingress) dut.uses_ingress |= (1u << i);
            if (c.bypass) dut.bypass |= (1u << i);
            dut.latch_id |= (uint64_t(c.latch) << (i * 6));
        }
        dut.eval();

        // Exact gem5 commitSme2dBundle selection model over the 8-candidate window.
        auto wbConflict = [&](unsigned i) {
            if (!mixed || cands[i].bypass) return false;
            uint64_t m = 0;
            for (unsigned r = 0; r < 8; ++r)
                if (cands[i].rows & (1u << r))
                    m |= (uint64_t(cands[i].cols) << (r * 8));
            return (nomWb[cands[i].kind] & m) != 0;
        };
        std::array<int, 3> gem5Pick{-1, -1, -1};
        if (retCredit) {
            auto anchorBefore = [&](unsigned a, unsigned b) {
                if (mixed) {
                    return std::make_tuple(cands[a].crit, cands[a].prio, wbConflict(a), -int(cands[a].pes), a) <
                           std::make_tuple(cands[b].crit, cands[b].prio, wbConflict(b), -int(cands[b].pes), b);
                }
                return std::tie(cands[a].prio, a) < std::tie(cands[b].prio, b);
            };
            int anchor = -1;
            for (unsigned i = 0; i < 8; ++i) {
                if (!cands[i].valid || pop16(cands[i].reads) > readCredits ||
                    (cands[i].ingress && !ingressCredits))
                    continue;
                if (anchor < 0 || anchorBefore(i, unsigned(anchor)))
                    anchor = int(i);
            }
            if (anchor >= 0) {
                gem5Pick[0] = anchor;
                uint8_t usedRows = cands[anchor].rows;
                uint16_t usedReads = cands[anchor].reads;
                unsigned usedIng = cands[anchor].ingress ? 1u : 0u;
                for (unsigned slot = 1; slot < 3; ++slot) {
                    int best = -1;
                    auto fillerKey = [&](unsigned i) {
                        return std::make_tuple(cands[i].crit, wbConflict(i), -int(cands[i].pes), cands[i].prio, i);
                    };
                    for (unsigned i = 0; i < 8; ++i) {
                        if (!cands[i].valid) continue;
                        bool used = false, dupLatch = false;
                        for (unsigned k = 0; k < slot; ++k) {
                            if (gem5Pick[k] == int(i)) used = true;
                            if (gem5Pick[k] >= 0 && cands[i].bypass && cands[gem5Pick[k]].bypass &&
                                cands[i].latch == cands[gem5Pick[k]].latch)
                                dupLatch = true;
                        }
                        if (used || dupLatch) continue;
                        if (!mixed && cands[i].kind != cands[anchor].kind) continue;
                        if (cands[i].rows & usedRows) continue;
                        if (pop16(usedReads | cands[i].reads) > readCredits) continue;
                        if (usedIng + (cands[i].ingress ? 1u : 0u) > ingressCredits) continue;
                        if (best < 0 || fillerKey(i) < fillerKey(unsigned(best)))
                            best = int(i);
                    }
                    if (best < 0) break;
                    gem5Pick[slot] = best;
                    usedRows |= cands[best].rows;
                    usedReads |= cands[best].reads;
                    usedIng += cands[best].ingress ? 1u : 0u;
                }
            }
        }

        for (unsigned slot = 0; slot < 3; ++slot) {
            const bool rtlValid = ((dut.grant_valid >> slot) & 1u) != 0;
            const unsigned rtlIdx = (dut.grant_index >> (slot * 3)) & 7u;
            assert(rtlValid == (gem5Pick[slot] >= 0));
            if (rtlValid)
                assert(int(rtlIdx) == gem5Pick[slot]);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            cpp = out / 'rtl_gem5_equiv.cc'
            cpp.write_text(harness)
            vinc = Path('/home/zyy/.local/lib/python3.10/site-packages/verilator/include')
            subprocess.run([
                str(verilator), '--cc',
                '-Wno-WIDTHEXPAND', '-Wno-UNOPTFLAT',
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
