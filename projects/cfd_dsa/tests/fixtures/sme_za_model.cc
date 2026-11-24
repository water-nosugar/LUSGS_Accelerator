#include "arch/arm/cfd_sme_za.hh"

#include <cassert>

using Sme = gem5::ArmISA::CfdSmeZa;

int
main()
{
    Sme array;
    Sme::Config cfg;
    cfg.lanes = 6;
    cfg.readPorts = 3;
    cfg.writePorts = 3;
    cfg.depth = 2;
    array.configure(1, cfg);
    Sme::Row row{};
    array.tick(1);
    assert(array.load({0, 0}, 0x07, {11., 17., 23.}, Sme::Lu));
    assert(array.load({1, 0}, 0x01, {2.}, Sme::Lu));
    assert(array.load({2, 0}, 0x07, {3., 4., 5.}, Sme::Rhs));
    assert(!array.read({0, 0}, 0x07, row, Sme::Lu));
    array.tick(2);
    assert(array.subtractOuterRow({0, 0}, 0x05, {1, 0}, 0, {2, 0}, Sme::Lu));
    assert(!array.scale({0, 0}, 0x01, 2., Sme::Lu));
    // An independent column can issue while a masked outer product is live.
    assert(array.scale({0, 0}, 0x02, 2., Sme::Rhs));
    assert(!array.scale({2, 0}, 0x01, 2., Sme::Rhs));
    for (unsigned cycle = 3; cycle <= 4; ++cycle)
        array.tick(cycle);
    assert(!array.read({0, 0}, 0x02, row, Sme::Rhs));
    array.tick(5);
    assert(array.read({0, 0}, 0x02, row, Sme::Rhs) && row[1] == 34.);
    assert(!array.idle(0));
    array.tick(6);
    array.tick(7);
    assert(array.read({0, 0}, 0x07, row, Sme::Lu));
    assert(row[0] == 5. && row[1] == 34. && row[2] == 13.);
    assert(array.empty() && array.peakReturns() == 2);
    assert(array.statistics(Sme::Lu).returned[Sme::Fma] == 2);
    array.release(0);
    assert(!array.ready({0, 0}, 0x07));

    Sme bounded;
    cfg.writePorts = 1;
    bounded.configure(1, cfg);
    bounded.tick(1);
    assert(bounded.load({0, 0}, 0x01, {2.}, Sme::Forward));
    assert(!bounded.load({1, 0}, 0x01, {3.}, Sme::Backward));
    bounded.tick(2);
    assert(bounded.load({1, 0}, 0x01, {3.}, Sme::Backward));
    bounded.tick(3);
    assert(bounded.scale({0, 0}, 0x01, 2., Sme::Forward));
    assert(bounded.scale({1, 0}, 0x01, 2., Sme::Backward));
    for (unsigned cycle = 4; cycle <= 6; ++cycle)
        bounded.tick(cycle);
    assert(bounded.ready({0, 0}, 0x01));
    assert(!bounded.ready({1, 0}, 0x01));
    assert(!bounded.empty());
    bounded.tick(7);
    assert(bounded.read({1, 0}, 0x01, row, Sme::Backward) && row[0] == 6.);
    assert(bounded.statistics(Sme::Backward).returnWait == 1);
    assert(bounded.empty());

    Sme feed;
    cfg.ingressPorts = 1;
    feed.configure(1, cfg);
    feed.tick(1);
    assert(feed.ingressProduct({0, 0}, 0x1f, {1., 2., 3., 4., 5.}, 2.,
                               Sme::Backward));
    assert(!feed.ingressProduct({1, 0}, 0x01, {3.}, 2., Sme::Forward));
    assert(!feed.scale({1, 0}, 0x01, 2., Sme::Forward));
    feed.sample(Sme::Sweep);
    const auto &issue = feed.statistics(Sme::Sweep);
    assert(issue.issued[Sme::Backward] == 5 && issue.offered == 6);
    assert(issue.states[Sme::Partial] == 1 && issue.cycles == 1);
    feed.tick(2);
    assert(feed.ingressProduct({1, 0}, 0x01, {3.}, 2., Sme::Forward));
    feed.tick(3);
    assert(!feed.ready({0, 0}, 0x1f));
    feed.tick(4);
    assert(feed.read({0, 0}, 0x1f, row, Sme::Backward));
    assert(row[0] == 2. && row[4] == 10.);
    assert(feed.statistics(Sme::Backward).ingressWords == 5);

    Sme packed;
    cfg.writePorts = 3;
    cfg.mulLatency = 2;
    packed.configure(2, cfg);
    packed.tick(1);
    assert(packed.load({0, 0}, 0x07, {11., 17., 23.}, Sme::Rhs));
    assert(packed.load({0, 1}, 0x07, {2., 3., 4.}, Sme::Rhs));
    assert(packed.load({0, 2}, 0x07, {3., 4., 5.}, Sme::Rhs));
    assert(!packed.subtractElements({0, 0}, 0x05, {0, 1}, {0, 2}, Sme::Rhs));
    packed.tick(2);
    assert(packed.subtractElements({0, 0}, 0x05, {0, 1}, {0, 2}, Sme::Rhs));
    assert(!packed.scaleElements({0, 0}, 0x07, {0, 1}, Sme::Rhs));
    for (unsigned cycle = 3; cycle <= 6; ++cycle)
        packed.tick(cycle);
    assert(!packed.ready({0, 0}, 0x07));
    packed.tick(7);
    assert(packed.scaleElements({0, 0}, 0x07, {0, 1}, Sme::Rhs));
    packed.tick(8);
    assert(!packed.ready({0, 0}, 0x07));
    packed.tick(9);
    assert(packed.read({0, 0}, 0x07, row, Sme::Rhs));
    assert(row[0] == 10. && row[1] == 51. && row[2] == 12.);
    const auto &work = packed.statistics(Sme::Rhs);
    assert(work.returned[Sme::Fma] == 2 && work.returned[Sme::Mul] == 3);
    assert(work.zaReads == 6 && work.zaWrites == 5 && work.loadWords == 9);

    Sme vertical;
    vertical.configure(1, cfg);
    vertical.tick(1);
    assert(vertical.load({0, 2}, 0x01, {9.}, Sme::Lu));
    // The last selected bank conflicts. Earlier banks must stay untouched.
    assert(!vertical.loadVertical(0, 1, 0x07, {2., 3., 4.}, Sme::Lu));
    assert(!vertical.ready({0, 0}, 0x02));
    vertical.tick(2);
    assert(vertical.loadVertical(0, 1, 0x07, {2., 3., 4.}, Sme::Lu));
    assert(!vertical.loadVertical(1, 0, 0x01, {}, Sme::Lu));
    assert(!vertical.load({0, 0}, 0x01, {}, Sme::Lu));
    assert(!vertical.read({0, 2}, 0x02, row, Sme::Lu));
    assert(vertical.load({1, 0}, 0x07, {5., 11., 13.}, Sme::Rhs));
    vertical.tick(3);
    assert(vertical.read({0, 2}, 0x03, row, Sme::Lu) && row[0] == 9. && row[1] == 4.);
    assert(vertical.subtractOuterRow({1, 0}, 0x02, {1, 0}, 0, {0, 2}, Sme::Rhs));
    // A pending SME result in the target element blocks the entire slice.
    assert(!vertical.loadVertical(1, 1, 0x03, {7., 8.}, Sme::Lu));
    assert(!vertical.ready({1, 1}, 0x02));
    for (unsigned cycle = 4; cycle <= 8; ++cycle)
        vertical.tick(cycle);
    assert(vertical.read({1, 0}, 0x07, row, Sme::Rhs));
    assert(row[0] == 5. && row[1] == -9. && row[2] == 13.);
    assert(vertical.statistics(Sme::Lu).verticalLoads == 1);
    assert(vertical.statistics(Sme::Lu).zaWrites == 2);
    assert(vertical.statistics(Sme::Lu).loadWords == 4);
    vertical.release(0);
    assert(!vertical.ready({0, 2}, 0x03));

    Sme bypass;
    cfg.writePorts = 1;
    cfg.mulLatency = 1;
    bypass.configure(1, cfg, 0, 1);
    bypass.tick(1);
    assert(bypass.load({0, 5}, 0x01, {0.5}, Sme::Lu));
    assert(bypass.loadBypass(0, 0x07, {4., 10., 20.}, Sme::Rhs));
    assert(!bypass.readyBypass(0, 0x07));
    bypass.tick(2);
    assert(bypass.load({0, 0}, 0x06, {0., 3., 5.}, Sme::Lu));
    assert(bypass.scaleBypass(0, 0x01, {0, 5}, 0, Sme::Rhs));
    assert(!bypass.idleBypass(0));
    bypass.tick(3);
    // Rounding return wrote directly into latch 0 without using the single ZA write port.
    assert(bypass.idleBypass(0));
    assert(bypass.load({0, 1}, 0x01, {1.}, Sme::Lu));
    assert(bypass.subtractOuterBypass(0, 0x06, 0, {0, 0}, Sme::Rhs));
    for (unsigned cycle = 4; cycle <= 8; ++cycle)
        bypass.tick(cycle);
    assert(bypass.readBypass(0, 0x07, row, Sme::Rhs));
    assert(row[0] == 2. && row[1] == 4. && row[2] == 10.);
    const auto &bp = bypass.statistics(Sme::Rhs);
    assert(bp.zaWrites == 0 && bp.zaReads == 2);
    assert(bp.bypassWrites == 3 && bp.bypassReads == 3 && bp.returnWait == 0);
    bypass.releaseBypass(0);
    assert(!bypass.readyBypass(0, 0x07));

    // Warm inverse MVM (1-way FMA chain and 2-way split-FMA + ADD) via bypass latches.
    Sme inv;
    Sme::Config invCfg;
    invCfg.lanes = 10;
    invCfg.readPorts = 6;
    invCfg.writePorts = 3;
    invCfg.depth = 8;
    invCfg.mulLatency = 4;
    invCfg.fmaLatency = 5;
    invCfg.addLatency = 4;
    inv.configure(1, invCfg, 0, 2);
    // 5 columns of D^-1 loaded horizontally into rows 0..4 of tile 0.
    const double cols[5][5] = {
        {1., 2., 3., 4., 5.},
        {2., 1., 0., -1., 2.},
        {-1., 3., 2., 1., 0.},
        {0.5, -0.5, 1., 2., -1.},
        {3., 0., -2., 1., 4.},
    };
    const double rhsVec[5] = {2., -1., 3., 4., 0.5};
    double expected[5] = {};
    for (unsigned k = 0; k < 5; ++k)
        for (unsigned r = 0; r < 5; ++r)
            expected[r] += cols[k][r] * rhsVec[k];
    inv.tick(1);
    for (unsigned k = 0; k < 3; ++k) {
        Sme::Row c{};
        for (unsigned r = 0; r < 5; ++r)
            c[r] = cols[k][r];
        assert(inv.load({0, k}, 0x1f, c, Sme::Lu));
    }
    inv.tick(2);
    for (unsigned k = 3; k < 5; ++k) {
        Sme::Row c{};
        for (unsigned r = 0; r < 5; ++r)
            c[r] = cols[k][r];
        assert(inv.load({0, k}, 0x1f, c, Sme::Lu));
    }
    // Row 0..2 are ready on cycle 2; launch 1-way chain on latch 0.
    assert(inv.scaleRowToBypass(0, 0x1f, {0, 0}, rhsVec[0], Sme::Rhs));
    assert(!inv.fmaRowToBypass(0, 0x1f, {0, 1}, rhsVec[1], Sme::Rhs));
    uint64_t t = 2;
    for (unsigned k = 1; k < 5; ++k) {
        while (!inv.fmaRowToBypass(0, 0x1f, {0, k}, rhsVec[k], Sme::Rhs))
            inv.tick(++t);
    }
    while (!inv.readBypass(0, 0x1f, row, Sme::Rhs))
        inv.tick(++t);
    for (unsigned r = 0; r < 5; ++r)
        assert(std::fabs(row[r] - expected[r]) < 1e-12);
    const auto &inv1 = inv.statistics(Sme::Rhs);
    assert(inv1.zaWrites == 0 && inv1.zaReads == 5);
    assert(inv1.returned[Sme::Mul] == 5 && inv1.returned[Sme::Fma] == 20);
    assert(inv1.bypassWrites == 5 && inv1.bypassReads == 5 && inv1.returnWait == 0);
    inv.releaseBypass(0);

    // Now test 2-way split-FMA + ADD on latches 0 and 1 in parallel.
    inv.resetStats();
    inv.tick(++t);
    assert(inv.scaleRowToBypass(0, 0x1f, {0, 0}, rhsVec[0], Sme::Rhs));
    assert(inv.scaleRowToBypass(1, 0x1f, {0, 3}, rhsVec[3], Sme::Rhs));
    unsigned stepA = 1, stepB = 1;
    bool merged = false;
    while (!merged) {
        inv.tick(++t);
        if (stepA < 3 && inv.fmaRowToBypass(0, 0x1f, {0, stepA}, rhsVec[stepA], Sme::Rhs))
            ++stepA;
        if (stepB < 2 && inv.fmaRowToBypass(1, 0x1f, {0, 3 + stepB}, rhsVec[3 + stepB], Sme::Rhs))
            ++stepB;
        if (stepA == 3 && stepB == 2 &&
            inv.combineBypass(0, 0x1f, true, {0, 0}, true, {1, 0}, false, Sme::Rhs))
            merged = true;
    }
    while (!inv.readBypass(0, 0x1f, row, Sme::Rhs))
        inv.tick(++t);
    for (unsigned r = 0; r < 5; ++r)
        assert(std::fabs(row[r] - expected[r]) < 1e-12);
    const auto &inv2 = inv.statistics(Sme::Rhs);
    assert(inv2.zaWrites == 0 && inv2.zaReads == 5);
    assert(inv2.returned[Sme::Mul] == 10 && inv2.returned[Sme::Fma] == 15 &&
           inv2.returned[Sme::Add] == 5);
    assert(inv2.bypassWrites == 6 && inv2.bypassReads == 6 && inv2.returnWait == 0);
    inv.releaseBypass(0);
    inv.releaseBypass(1);
    inv.release(0);

    // ------------------------------------------------------------------
    // 2D 8x8 Outer-Product Array Unit Tests (Step 2A + Step 2B)
    // ------------------------------------------------------------------
    // 1. reservedBytes parity
    const uint64_t baseRes = Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 0, 1);
    const uint64_t twoDRes = baseRes + 32 * (640 - 128);
    static_assert(Sme::returnSlotBytes(0) == 128 && Sme::returnSlotBytes(1) == 640);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 1, 1) == twoDRes + 128);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 2, 3) == twoDRes + 256);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 3, 2) == twoDRes + 384);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 3, 3) == twoDRes + 512);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 3, 3, true, false) == twoDRes + 640);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 3, 3, false, true) == twoDRes + 640);
    assert(Sme::reservedBytes(4, 32, 2, 0, false, false, false, true, true, 0, 3, 3, true, true) == twoDRes + 768);

    // 2. 2d-base: single 4x4 FMOPA, single bundle/cycle, exact element results & writeback conflict
    {
        Sme arr;
        Sme::Config c2d;
        c2d.lanes = 64;
        c2d.readPorts = 6;
        c2d.writePorts = 8;
        c2d.depth = 8;
        c2d.fmaLatency = 5;
        c2d.mulLatency = 4;
        c2d.addLatency = 4;
        c2d.arrayMode = Sme::ArrayMode::TwoDBase;
        c2d.maxSegments = 1;
        arr.configure(2, c2d);

        arr.tick(1);
        // Load a 5x5 matrix into tile 0 (rows 0..4, cols 0..4)
        for (unsigned r = 0; r < 5; ++r) {
            Sme::Row vals{};
            for (unsigned c = 0; c < 5; ++c)
                vals[c] = double((r + 1) * 10 + (c + 1));
            assert(arr.load({0, r}, 0x1f, vals, Sme::Lu));
        }
        arr.tick(2);
        // Issue 4x4 FMOPA on rows {1..4} (0x1e), cols {1..4} (0x1e) using col 0 & row 0
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::Fmopa;
        seg.phase = Sme::Lu;
        seg.rowMask = 0x1e;
        seg.colMask = 0x1e;
        seg.dstTile = 0;
        seg.aPos = {0, 0};
        seg.aCol = 0;
        seg.bPos = {0, 0};
        assert(arr.issue2dBundle(&seg, 1));
        // Second bundle in same cycle must be rejected
        assert(!arr.issue2dBundle(&seg, 1));
        // Multi-segment in 2d-base must be rejected
        Sme::Segment2d two[2] = {seg, seg};
        assert(!arr.issue2dBundle(two, 2));

        // Issue a ScaleRow on row 0 cols {1..4} at cycle 3 (mulLatency=4 -> ready at cycle 7,
        // same cycle as FMOPA ready at 2+5=7; row 0 vs rows 1..4 are disjoint -> no PE conflict!)
        arr.tick(3);
        Sme::Segment2d sc{};
        sc.op = Sme::Op2d::ScaleRow;
        sc.phase = Sme::Rhs;
        sc.rowMask = 0x01;
        sc.colMask = 0x1e;
        sc.dstTile = 0;
        sc.dstRow = 0;
        sc.useScalar = true;
        sc.scalar = 0.5;
        assert(arr.issue2dBundle(&sc, 1));

        for (uint64_t cyc = 4; cyc <= 6; ++cyc)
            arr.tick(cyc);
        assert(!arr.ready({0, 1}, 0x1e));
        assert(!arr.ready({0, 0}, 0x1e));
        arr.tick(7);
        assert(arr.empty());
        assert(arr.ready({0, 1}, 0x1e) && arr.ready({0, 0}, 0x1e));
        assert(arr.statistics(Sme::Lu).zaReads == 2);
        for (unsigned r = 1; r < 5; ++r) {
            Sme::Row out{};
            assert(arr.read({0, r}, 0x1f, out, Sme::Lu));
            const double aVal = double((r + 1) * 10 + 1);
            for (unsigned c = 1; c < 5; ++c) {
                const double bVal = double(10 + (c + 1));
                const double orig = double((r + 1) * 10 + (c + 1));
                assert(std::fabs(out[c] - std::fma(-aVal, bVal, orig)) < 1e-12);
            }
        }
        const auto &s2d = arr.statistics2d();
        assert(s2d.bundlesTotal == 2 && s2d.bundlesBySeg[0] == 2 &&
               s2d.bundlesBySeg[1] == 0 && s2d.peActiveSum == 20);
        assert(s2d.rowFillHist[3] == 1 && s2d.rowFillHist[0] == 1);
        assert(arr.statistics(Sme::Lu).zaReads == 6);

        // Now test per-PE writeback collision:
        // Issue FMOPA on tile 1 rows {0..1} cols {0..1} at cyc 8 (ready 13)
        // and ScaleRow on tile 2 row 0 cols {0..1} at cyc 9 (ready 13) -> PE(0,0..1) collide!
        arr.tick(8);
        assert(arr.load({1, 0}, 0x07, {1., 2., 3.}, Sme::Lu));
        assert(arr.load({1, 1}, 0x07, {4., 5., 6.}, Sme::Lu));
        assert(arr.load({1, 2}, 0x07, {7., 8., 9.}, Sme::Lu));
        // Same-cycle external load to another tile on overlapping PE(0,0..1) must stall
        assert(!arr.load({2, 0}, 0x03, {10., 20.}, Sme::Rhs));
        arr.tick(9);
        assert(arr.load({2, 0}, 0x03, {10., 20.}, Sme::Rhs));
        arr.tick(10);
        Sme::Segment2d f1{};
        f1.op = Sme::Op2d::Fmopa;
        f1.phase = Sme::Lu;
        f1.rowMask = 0x03; // rows 0,1
        f1.colMask = 0x03; // cols 0,1
        f1.dstTile = 1;
        f1.aPos = {1, 0};
        f1.aCol = 2;
        f1.bPos = {1, 2};
        assert(arr.issue2dBundle(&f1, 1)); // ready at 10+5=15
        arr.tick(11);
        Sme::Segment2d m1{};
        m1.op = Sme::Op2d::ScaleRow;
        m1.phase = Sme::Rhs;
        m1.rowMask = 0x01; // row 0 -> collides with PE(0,0..1) of f1!
        m1.colMask = 0x03;
        m1.dstTile = 2;
        m1.dstRow = 0;
        m1.useScalar = true;
        m1.scalar = 3.0;
        assert(arr.issue2dBundle(&m1, 1)); // ready at 11+4=15
        for (uint64_t cyc = 12; cyc <= 15; ++cyc)
            arr.tick(cyc);
        // f1 retired on cyc 15; m1 must wait 1 cycle due to PE(0,0..1) writeback conflict
        assert(arr.ready({1, 0}, 0x03) && !arr.ready({2, 0}, 0x03));
        assert(arr.statistics(Sme::Rhs).returnWait == 1);
        // External load colliding on the same PE(0,0..1) in the same cycle must also block
        assert(!arr.load({3, 0}, 0x03, {1., 2.}, Sme::Lu));
        // Disjoint row PE(4,0..1) load succeeds in the same cycle
        assert(arr.load({3, 4}, 0x03, {1., 2.}, Sme::Lu));
        arr.tick(16);
        assert(arr.empty() && arr.ready({2, 0}, 0x03));
        Sme::Row chk{};
        assert(arr.read({2, 0}, 0x03, chk, Sme::Rhs));
        assert(chk[0] == 30. && chk[1] == 60.);
    }

    // 3. 2d-bd (BD-FMOPA): 4x4 + 3x3 + 1x1 zero cross-contamination & col-overlap rejection
    {
        Sme bd;
        Sme::Config cbd;
        cbd.lanes = 64;
        cbd.readPorts = 6;
        cbd.writePorts = 8;
        cbd.depth = 8;
        cbd.fmaLatency = 5;
        cbd.arrayMode = Sme::ArrayMode::TwoDBd;
        cbd.maxSegments = 3;
        bd.configure(3, cbd);

        // Load 3 tiles (0, 3, 6) with distinct values across all 8x8 entries
        for (unsigned cyc = 1; cyc <= 3; ++cyc) {
            bd.tick(cyc);
            unsigned tile = (cyc - 1) * 3;
            for (unsigned r = 0; r < 8; ++r) {
                Sme::Row vals{};
                for (unsigned c = 0; c < 8; ++c)
                    vals[c] = double((tile + 1) * 100 + r * 10 + c);
                assert(bd.load({tile, r}, 0xff, vals, Sme::Lu));
            }
        }
        bd.tick(4);
        // Seg 0: rows {0..3} cols {0..3} (4x4=16) on tile 0 (aCol=7, bRow=7)
        // Seg 1: rows {4..6} cols {4..6} (3x3=9) on tile 3 (aCol=0, bRow=0)
        // Seg 2: row {7} col {7} (1x1=1) on tile 6 (aCol=1, bRow=1)
        // Total = 16 + 9 + 1 = 26 PEs, all 8 rows & 8 cols disjoint!
        Sme::Segment2d segs[3]{};
        segs[0].op = Sme::Op2d::Fmopa; segs[0].phase = Sme::Lu;
        segs[0].rowMask = 0x0f; segs[0].colMask = 0x0f;
        segs[0].dstTile = 0; segs[0].aPos = {0, 0}; segs[0].aCol = 7; segs[0].bPos = {0, 7};

        segs[1].op = Sme::Op2d::Fmopa; segs[1].phase = Sme::Lu;
        segs[1].rowMask = 0x70; segs[1].colMask = 0x70;
        segs[1].dstTile = 3; segs[1].aPos = {3, 0}; segs[1].aCol = 0; segs[1].bPos = {3, 0};

        segs[2].op = Sme::Op2d::Fmopa; segs[2].phase = Sme::Lu;
        segs[2].rowMask = 0x80; segs[2].colMask = 0x80;
        segs[2].dstTile = 6; segs[2].aPos = {6, 0}; segs[2].aCol = 1; segs[2].bPos = {6, 1};

        // First verify overlapping colMask is rejected in 2d-bd
        Sme::Segment2d bad[2] = {segs[0], segs[1]};
        bad[1].colMask = 0x0f; // overlaps segs[0].colMask!
        assert(!bd.issue2dBundle(bad, 2));

        // Now issue valid 3-segment BD bundle
        assert(bd.issue2dBundle(segs, 3));
        assert(bd.statistics(Sme::Lu).zaReads == 6);
        for (uint64_t cyc = 5; cyc <= 9; ++cyc)
            bd.tick(cyc);
        assert(bd.empty());

        // Verify zero cross-contamination: off-block elements of tile 0 untouched!
        for (unsigned r = 0; r < 8; ++r) {
            Sme::Row out{};
            assert(bd.read({0, r}, 0xff, out, Sme::Lu));
            for (unsigned c = 0; c < 8; ++c) {
                const double orig = double(100 + r * 10 + c);
                if (r < 4 && c < 4) {
                    const double a = double(100 + r * 10 + 7);
                    const double b = double(100 + 70 + c);
                    assert(std::fabs(out[c] - std::fma(-a, b, orig)) < 1e-12);
                } else {
                    assert(out[c] == orig);
                }
            }
            if (r == 3)
                bd.tick(10); // replenish 6 read ports
        }
        const auto &s2d = bd.statistics2d();
        assert(s2d.bundlesTotal == 1 && s2d.bundlesBySeg[2] == 1 &&
               s2d.peActiveSum == 26 && s2d.rowFillHist[7] == 1);
    }

    // 4. 2d-rs (RS-FMOPA): 4x6 + 4x6 = 48 PEs with overlapping columns
    {
        Sme rs;
        Sme::Config crs;
        crs.lanes = 64;
        crs.readPorts = 6;
        crs.writePorts = 8;
        crs.depth = 8;
        crs.fmaLatency = 5;
        crs.arrayMode = Sme::ArrayMode::TwoDRs;
        crs.maxSegments = 2;
        rs.configure(2, crs);

        for (unsigned cyc = 1; cyc <= 2; ++cyc) {
            rs.tick(cyc);
            unsigned tile = (cyc - 1) * 3;
            for (unsigned r = 0; r < 8; ++r) {
                Sme::Row vals{};
                for (unsigned c = 0; c < 8; ++c)
                    vals[c] = double((tile + 1) * 50 + r * 8 + c);
                assert(rs.load({tile, r}, 0xff, vals, Sme::Rhs));
            }
        }
        rs.tick(3);
        // Seg 0: rows {0..3} (0x0f), cols {0..5} (0x3f) -> 24 PEs
        // Seg 1: rows {4..7} (0xf0), cols {0..5} (0x3f) -> 24 PEs (SAME cols!)
        Sme::Segment2d segs[2]{};
        segs[0].op = Sme::Op2d::Fmopa; segs[0].phase = Sme::Rhs;
        segs[0].rowMask = 0x0f; segs[0].colMask = 0x3f;
        segs[0].dstTile = 0; segs[0].aPos = {0, 0}; segs[0].aCol = 6; segs[0].bPos = {0, 6};

        segs[1].op = Sme::Op2d::Fmopa; segs[1].phase = Sme::Rhs;
        segs[1].rowMask = 0xf0; segs[1].colMask = 0x3f;
        segs[1].dstTile = 3; segs[1].aPos = {3, 0}; segs[1].aCol = 6; segs[1].bPos = {3, 0};

        assert(rs.issue2dBundle(segs, 2));
        for (uint64_t cyc = 4; cyc <= 8; ++cyc)
            rs.tick(cyc);
        assert(rs.empty());
        const auto &s2d = rs.statistics2d();
        assert(s2d.bundlesTotal == 1 && s2d.bundlesBySeg[1] == 1 &&
               s2d.peActiveSum == 48 && s2d.rowFillHist[7] == 1);
        // Spot check tile 3 row 5 col 3
        Sme::Row out{};
        assert(rs.read({3, 5}, 0x3f, out, Sme::Rhs));
        const double orig = double(4 * 50 + 5 * 8 + 3);
        const double a = double(4 * 50 + 5 * 8 + 6);
        const double b = double(4 * 50 + 0 * 8 + 3);
        assert(std::fabs(out[3] - std::fma(-a, b, orig)) < 1e-12);
    }
    // Row operations must compute and retire to the same physical row.
    {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDBase;
        config.lanes = 64;
        config.ingressPorts = 1;
        model.configure(1, config);
        model.tick(1);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::IngressRow;
        seg.rowMask = 2;
        seg.colMask = 1;
        seg.dstRow = 0;
        assert(!model.issue2dBundle(&seg, 1));
        seg.rowMask = 1;
        assert(model.issue2dBundle(&seg, 1));
    }

    // Eight independent factor rows plus a pivot row need nine read slices.
    {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDBase;
        config.lanes = 64;
        config.readPorts = 9;
        config.writePorts = 8;
        model.configure(1, config);
        model.tick(1);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::FmopaFactorRows;
        seg.rowMask = 0xff;
        seg.colMask = 1;
        seg.bPos = {1, 0};
        for (unsigned r = 0; r < Sme::Width; ++r) {
            assert(model.load({0, r}, 1, {2.0}, Sme::Lu));
            seg.factorPos[r] = {0, r};
        }
        model.tick(2);
        assert(model.load(seg.bPos, 1, {3.0}, Sme::Lu));
        model.tick(3);
        assert(model.issue2dBundle(&seg, 1));
        assert(model.freeReads() == 0);
        model.tick(3 + config.fmaLatency);
        for (unsigned r = 0; r < Sme::Width; ++r) {
            Sme::Row result{};
            assert(model.read({0, r}, 1, result, Sme::Lu));
            assert(result[0] == -4.0);
        }
    }

    // 5. Option A: mixed-kind 2d-rs bundle (Fmopa lat=5 + ScaleRow lat=4 + CombineRow lat=4)
    //    retires per-segment on its own latency and detects future writeback collisions.
    {
        Sme mk;
        Sme::Config cmk;
        cmk.lanes = 64;
        cmk.readPorts = 8;
        cmk.writePorts = 8;
        cmk.depth = 8;
        cmk.fmaLatency = 5;
        cmk.mulLatency = 4;
        cmk.addLatency = 4;
        cmk.arrayMode = Sme::ArrayMode::TwoDRs;
        cmk.maxSegments = 3;
        cmk.mixedKind = true;
        mk.configure(2, cmk);

        mk.tick(1);
        for (unsigned r = 0; r < 5; ++r)
            assert(mk.load({0, r}, 0x1f, {2., 3., 4., 5., 6.}, Sme::Lu));
        assert(mk.load({3, 5}, 0x1f, {10., 20., 30., 40., 50.}, Sme::Rhs));
        assert(mk.load({3, 6}, 0x1f, {1., 2., 3., 4., 5.}, Sme::Forward));
        assert(mk.load({3, 7}, 0x1f, {5., 4., 3., 2., 1.}, Sme::Forward));

        mk.tick(2);
        Sme::Segment2d segs[3]{};
        // Seg 0: Fmopa on rows {1..4} (lat=5 -> ready at cyc 7)
        segs[0].op = Sme::Op2d::Fmopa; segs[0].phase = Sme::Lu;
        segs[0].rowMask = 0x1e; segs[0].colMask = 0x1e;
        segs[0].dstTile = 0; segs[0].aPos = {0, 0}; segs[0].aCol = 0; segs[0].bPos = {0, 0};
        // Seg 1: ScaleRow on row 5 (lat=4 -> ready at cyc 6)
        segs[1].op = Sme::Op2d::ScaleRow; segs[1].phase = Sme::Rhs;
        segs[1].rowMask = 1u << 5; segs[1].colMask = 0x1f;
        segs[1].dstTile = 3; segs[1].dstRow = 5; segs[1].useScalar = true; segs[1].scalar = 2.0;
        // Seg 2: CombineRow on row 6 (lat=4 -> ready at cyc 6)
        segs[2].op = Sme::Op2d::CombineRow; segs[2].phase = Sme::Forward;
        segs[2].rowMask = 1u << 6; segs[2].colMask = 0x1f;
        segs[2].dstTile = 3; segs[2].dstRow = 6;
        segs[2].aPos = {3, 6}; segs[2].bPos = {3, 7}; segs[2].subtract = false;

        assert(mk.issue2dBundle(segs, 3));
        // On cyc 3, a ScaleRow (lat=4 -> ready cyc 7) on row 1 collides with Seg 0 (ready cyc 7)
        mk.tick(3);
        Sme::Segment2d probe{};
        probe.op = Sme::Op2d::ScaleRow; probe.phase = Sme::Rhs;
        probe.rowMask = 1u << 1; probe.colMask = 0x1e;
        probe.dstTile = 1; probe.dstRow = 1; probe.useScalar = true; probe.scalar = 1.0;
        assert(mk.willCollideWriteback2d(probe));
        probe.rowMask = 1u << 0; probe.dstRow = 0;
        assert(!mk.willCollideWriteback2d(probe));

        mk.tick(4);
        mk.tick(5);
        assert(!mk.ready({3, 5}, 0x1f) && !mk.ready({3, 6}, 0x1f) && !mk.ready({0, 1}, 0x1e));
        // Cycle 6: ScaleRow & CombineRow retire, while Fmopa (lat=5) is still in flight!
        mk.tick(6);
        assert(mk.ready({3, 5}, 0x1f) && mk.ready({3, 6}, 0x1f) && !mk.ready({0, 1}, 0x1e));
        assert(!mk.empty());
        Sme::Row out{};
        assert(mk.read({3, 5}, 0x1f, out, Sme::Rhs) && out[0] == 20. && out[4] == 100.);
        assert(mk.read({3, 6}, 0x1f, out, Sme::Forward) && out[0] == 6. && out[4] == 6.);
        // Cycle 7: Fmopa retires and pending2d drains completely.
        mk.tick(7);
        assert(mk.empty() && mk.ready({0, 1}, 0x1e));
        const auto &s2d = mk.statistics2d();
        assert(s2d.bundlesTotal == 1 && s2d.bundlesBySeg[2] == 1 &&
               s2d.peActiveSum == 26 && s2d.fmopaBundles == 1 && s2d.fmopaPeActive == 16);

        // Priority-aware writeback arbitration: an older Rhs Fmopa (lat=5, ready cyc 13)
        // and a newer Forward ScaleRow (lat=4, ready cyc 13) collide on row 1.
        // Forward must retire on cyc 13 while Rhs waits until cyc 14.
        mk.tick(8);
        assert(mk.load({1, 1}, 0x1e, {1., 2., 3., 4., 5.}, Sme::Forward));
        Sme::Segment2d rhsOp{};
        rhsOp.op = Sme::Op2d::Fmopa; rhsOp.phase = Sme::Rhs;
        rhsOp.rowMask = 0x1e; rhsOp.colMask = 0x1e;
        rhsOp.dstTile = 0; rhsOp.aPos = {0, 0}; rhsOp.aCol = 0; rhsOp.bPos = {0, 0};
        assert(mk.issue2dBundle(&rhsOp, 1));
        mk.tick(9);
        Sme::Segment2d fwdOp{};
        fwdOp.op = Sme::Op2d::ScaleRow; fwdOp.phase = Sme::Forward;
        fwdOp.rowMask = 1u << 1; fwdOp.colMask = 0x1e;
        fwdOp.dstTile = 1; fwdOp.dstRow = 1; fwdOp.useScalar = true; fwdOp.scalar = 3.0;
        // Conservative RTL-aligned writeback conflict prediction: any due
        // writeback on the same (cycle, PE) sets willCollideWriteback2d
        // regardless of phase priority, while actual retirement still honors
        // phase priority (Forward prio 1 beats Rhs prio 3).
        assert(mk.willCollideWriteback2d(fwdOp));
        Sme::Segment2d rhsProbe = fwdOp;
        rhsProbe.phase = Sme::Rhs;
        assert(mk.willCollideWriteback2d(rhsProbe));
        assert(mk.issue2dBundle(&fwdOp, 1));
        for (uint64_t cyc = 10; cyc <= 12; ++cyc)
            mk.tick(cyc);
        mk.tick(13);
        assert(mk.ready({1, 1}, 0x1e));
        assert(!mk.ready({0, 1}, 0x1e));
        assert(mk.statistics(Sme::Forward).returnWait == 0);
        assert(mk.statistics(Sme::Rhs).returnWait == 1);
        // Delayed rhsOp is carried forward to cyc 14; a probe targeting cyc 14 sees it.
        Sme::Segment2d delayedProbe{};
        delayedProbe.op = Sme::Op2d::ScaleRow; delayedProbe.phase = Sme::Rhs;
        delayedProbe.rowMask = 1u << 1; delayedProbe.colMask = 0x1e;
        delayedProbe.dstTile = 1; delayedProbe.dstRow = 1;
        delayedProbe.useScalar = true; delayedProbe.scalar = 1.0;
        // Probe lat=4 -> due at 17 (no conflict), while at cyc 10+4=14 check via lat=1 model below.
        assert(!mk.willCollideWriteback2d(delayedProbe));
        mk.tick(14);
        assert(mk.empty() && mk.ready({0, 1}, 0x1e));
    }

    // The independent divider shares the PE write port with 2D returns,
    // without stealing external ZA write ports or whole-row write locks.
    {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDRs;
        config.lanes = 64;
        config.writePorts = 1;
        config.maxSegments = 2;
        config.mixedKind = true;
        config.ingressPorts = 1;
        config.divLatency = 5;
        config.mulLatency = 4;
        model.configure(1, config);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::IngressRow;
        seg.phase = Sme::Backward;
        seg.rowMask = seg.colMask = 1;
        seg.dstTile = 1;
        seg.ingressValues[0] = 3.0;
        seg.scalar = 2.0;
        model.tick(1);
        assert(model.reciprocal({0, 0}, 0, 2.0, Sme::Lu));
        model.tick(2);
        // Conservative RTL-aligned prediction: Backward also sees the due
        // Div writeback on PE(0,0), while still beating Lu Div at retirement.
        assert(model.willCollideWriteback2d(seg));
        Sme::Segment2d luProbe = seg;
        luProbe.phase = Sme::Lu;
        assert(model.willCollideWriteback2d(luProbe));
        assert(model.issue2dBundle(&seg, 1));
        model.tick(6);
        assert(model.ready({1, 0}, 1) && !model.ready({0, 0}, 1));
        assert(model.statistics(Sme::Lu).returnWait == 1);
        assert(model.statistics(Sme::Lu).returnBlockedCycles == 1);
        model.tick(7);
        assert(model.empty() && model.ready({0, 0}, 1));
        // Div retired on PE(0,0) in cyc 7: external load on disjoint col 1 of same row
        // must still succeed (Div did not consume external writePorts or tile.written[0]).
        assert(!model.load({0, 0}, 0x01, {9.0}, Sme::Rhs)); // PE(0,0) busy
        assert(model.load({0, 0}, 0x02, {0.0, 7.0}, Sme::Rhs)); // disjoint col 1 OK!
        // Verify delayed writeback tracking (mulLat=1 -> probe at cyc N sees delayed cyc N+1)
        config.mulLatency = 1;
        config.fmaLatency = 2;
        config.addLatency = 1;
        model.configure(1, config);
        // Verify configure() cleared stale tiles/writebacks/nextDiv/stats cleanly.
        assert(!model.ready({0, 0}, 0x03));
        assert(model.statistics(Sme::Lu).returnWait == 0);
        model.tick(1);
        assert(model.load({0, 0}, 0x03, {1.0, 2.0}, Sme::Rhs));
        model.tick(2);
        Sme::Segment2d slow{};
        slow.op = Sme::Op2d::Fmopa; slow.phase = Sme::Rhs;
        slow.rowMask = 1; slow.colMask = 1; slow.dstTile = 0;
        slow.aPos = {0, 0}; slow.aCol = 1; slow.bPos = {0, 0};
        assert(model.issue2dBundle(&slow, 1)); // ready cyc 4
        model.tick(3);
        Sme::Segment2d fast{};
        fast.op = Sme::Op2d::IngressRow; fast.phase = Sme::Forward;
        fast.rowMask = 1; fast.colMask = 1; fast.dstTile = 1;
        fast.ingressValues[0] = 2.0; fast.scalar = 3.0;
        assert(model.issue2dBundle(&fast, 1)); // ready cyc 4 (beats slow)
        model.tick(4);
        // fast retired, slow delayed to cyc 5 -> mulLat=1 probe on cyc 4 (due cyc 5) must see it!
        Sme::Segment2d probe5 = fast;
        probe5.phase = Sme::Rhs;
        assert(model.willCollideWriteback2d(probe5));
        model.tick(5);
        assert(model.empty());
        model.resetStats();
        assert(!model.willCollideWriteback2d(probe5));
    }

    // Scalar bypass and 2D commands draw from the same return queue.
    {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDRs;
        config.lanes = 64;
        config.maxSegments = 2;
        config.mixedKind = true;
        config.ingressPorts = 2;
        config.depth = 1;
        model.configure(1, config, 0, 1);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::IngressRow;
        seg.rowMask = seg.colMask = 1;
        model.tick(1);
        assert(model.issue2dBundle(&seg, 1));
        assert(!model.ingressProductBypass(0, 1, {3.0}, 1.0, Sme::Rhs));
        assert(model.pendingReturns() == 1 && model.freeIngress() == 1);
        model.tick(1 + config.mulLatency);
        assert(model.ingressProductBypass(0, 1, {3.0}, 1.0, Sme::Rhs));
        assert(!model.issue2dBundle(&seg, 1));
        assert(model.pendingReturns() == 1);
    }

    // A rejected bundle must leave issue credits available to a smaller one.
    {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDBase;
        config.lanes = 6;
        config.ingressPorts = 1;
        model.configure(1, config);
        model.tick(1);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::IngressRow;
        seg.rowMask = 1;
        seg.colMask = 0x7f;
        assert(!model.issue2dBundle(&seg, 1));
        assert(model.freeLanes() == 6 && model.freeIngress() == 1);
        assert(model.empty() && !model.bundleIssued());
        seg.colMask = 0x3f;
        assert(model.issue2dBundle(&seg, 1));
    }
    // Two simultaneously delayed segments still count as one blocked cycle.
    // The homogeneous path charges every phase held by an atomic bundle.
    for (bool mixed : {false, true}) {
        Sme model;
        Sme::Config config;
        config.arrayMode = Sme::ArrayMode::TwoDRs;
        config.lanes = 64;
        config.maxSegments = 2;
        config.mixedKind = mixed;
        config.ingressPorts = 2;
        config.mulLatency = 5;
        config.addLatency = 4;
        model.configure(1, config);
        model.tick(1);
        assert(model.load({2, 0}, 1, {2.0}, Sme::Forward));
        std::array<Sme::Segment2d, 2> mul{}, add{};
        for (unsigned r = 0; r < 2; ++r) {
            mul[r].op = Sme::Op2d::IngressRow;
            mul[r].phase = Sme::Rhs;
            mul[r].rowMask = 1u << r;
            mul[r].colMask = 1;
            mul[r].dstRow = r;
            add[r].op = Sme::Op2d::CombineRow;
            add[r].phase = Sme::Forward;
            add[r].rowMask = 1u << r;
            add[r].colMask = 1;
            add[r].dstRow = r;
            add[r].dstTile = 1;
            add[r].aPos = add[r].bPos = {2, 0};
        }
        assert(model.issue2dBundle(mul.data(), mul.size()));
        model.tick(2);
        if (!mixed) {
            // Only the first segment overlaps the older bundle.
            // The other phase waits with it because retirement is atomic.
            add[1].rowMask = 4;
            add[1].dstRow = 2;
            add[1].phase = Sme::ReverseRhs;
        }
        assert(model.issue2dBundle(add.data(), add.size()));
        model.tick(6);
        model.sample(Sme::Sweep);
        assert(model.statistics(Sme::Sweep).blockedReturnCycles == 1);
        if (mixed) {
            assert(model.statistics(Sme::Rhs).returnWait == 2);
            assert(model.statistics(Sme::Rhs).returnBlockedCycles == 1);
        } else {
            assert(model.statistics(Sme::Forward).returnWait == 1);
            assert(model.statistics(Sme::Forward).returnBlockedCycles == 1);
            assert(model.statistics(Sme::ReverseRhs).returnWait == 0);
            assert(model.statistics(Sme::ReverseRhs).returnBlockedCycles == 1);
        }
        model.tick(7);
        assert(model.empty());
    }

}
