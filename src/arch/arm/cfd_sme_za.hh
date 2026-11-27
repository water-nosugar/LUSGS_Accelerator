// SPDX-License-Identifier: BSD-3-Clause
#ifndef __ARCH_ARM_CFD_SME_ZA_HH__
#define __ARCH_ARM_CFD_SME_ZA_HH__

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

namespace gem5::ArmISA
{

// A controller-owned SME array. Each command fragment updates a masked row
// of a logical 512-bit ZA tile. Fragments from all kernels share the same
// physical lanes, ports, scoreboard and bounded return queue.
class CfdSmeZa
{
  public:
    static constexpr unsigned Width = 8;
    static constexpr unsigned WarmRhsTiles = 4;
    using Row = std::array<double, Width>;

    enum Phase { Lu, Rhs, Forward, Backward, ReverseRhs, PhaseCount };
    enum Kind { Fma, Mul, Add, Div, KindCount };
    enum Stall { Dependency, ReadPort, WritePort, Lanes, Queue, Divider, Ingress,
                 StallCount };
    enum Window { Sweep, PrepSteady, BackSteady, WindowCount };
    enum IssueState { Full, Partial, ReadyBlocked, DependencyOnly, NoAttempt,
                      IssueStateCount };
    enum class ArrayMode : unsigned
    {
        OneD = 0,
        TwoDBase = 1,
        TwoDBd = 2,
        TwoDRs = 3,
    };
    enum class Op2d : uint8_t
    {
        Fmopa,
        FmopaFactorRows,
        ScaleRow,
        CombineRow,
        IngressRow,
        ScaleElements,
        SubtractElements,
        ScaleBypass,
        SubtractOuterBypass,
        ScaleRowToBypass,
        FmaRowToBypass,
        IngressBypass,
        CombineBypass,
    };

    struct Position
    {
        unsigned tile;
        unsigned row;
    };

    struct ReadSlice
    {
        bool isCol = false;
        unsigned tile = 0;
        unsigned idx = 0;
        bool operator==(const ReadSlice &o) const
        {
            return isCol == o.isCol && tile == o.tile && idx == o.idx;
        }
    };

    // One independent factor slice per PE row and one pivot slice.
    using SegmentReads = std::array<ReadSlice, Width + 1>;

    struct Segment2d
    {
        Op2d op = Op2d::Fmopa;
        Phase phase = Lu;
        uint8_t rowMask = 0;
        uint8_t colMask = 0;
        unsigned dstTile = 0;
        unsigned dstRow = 0;
        Position aPos{};
        unsigned aCol = 0;
        Position bPos{};
        bool useScalar = false;
        bool subtract = false;
        bool aIsBypass = false;
        bool bIsBypass = false;
        double scalar = 0.0;
        Row ingressValues{};
        std::array<Position, Width> factorPos{};
        std::array<uint8_t, Width> factorCol{};
    };

    struct Stats2d
    {
        uint64_t bundlesTotal = 0;
        std::array<uint64_t, 4> bundlesBySeg{};
        uint64_t peActiveSum = 0;
        std::array<uint64_t, Width> rowFillHist{};
        uint64_t packStallRow = 0;
        uint64_t packStallCol = 0;
        uint64_t packStallReadPort = 0;
        uint64_t packStallIngress = 0;
        uint64_t fmopaBundles = 0;
        uint64_t fmopaPeActive = 0;
    };

    struct Config
    {
        unsigned lanes = 16;
        unsigned readPorts = 8;
        unsigned writePorts = 4;
        unsigned depth = 32;
        unsigned ingressPorts = 0;
        unsigned fmaLatency = 5;
        unsigned mulLatency = 3;
        unsigned addLatency = 4;
        unsigned divLatency = 12;
        unsigned divIi = 4;
        ArrayMode arrayMode = ArrayMode::OneD;
        unsigned maxSegments = 1;
        bool panelPack = false;
        bool mixedKind = false;
    };

    struct Stats
    {
        std::array<uint64_t, KindCount> issued{};
        std::array<uint64_t, KindCount> returned{};
        std::array<uint64_t, StallCount> stalls{};
        uint64_t first = 0, last = 0, issueCycles = 0, beats = 0;
        uint64_t zaReads = 0, zaWrites = 0, loadWords = 0, ingressWords = 0;
        uint64_t returnWait = 0, returnBlockedCycles = 0, verticalLoads = 0;
        uint64_t bypassReads = 0, bypassWrites = 0;
    };

    struct WindowStats
    {
        std::array<uint64_t, IssueStateCount> states{};
        std::array<uint64_t, PhaseCount> issued{};
        uint64_t cycles = 0, offered = 0, cappedOffer = 0;
        uint64_t blockedReturnCycles = 0, pendingSum = 0, fullQueueCycles = 0;
    };

    static constexpr unsigned
    returnSlotBytes(unsigned arrayMode)
    {
        // One full FP64 result grid per 2D bundle, plus 128 B of tags and
        // control. Scalar returns share the same pool of logical slots.
        return 128 + (arrayMode ? Width * Width * sizeof(double) : 0);
    }

    static constexpr uint64_t
    reservedBytes(unsigned contexts, unsigned depth, unsigned ingressPorts,
                  unsigned reverseContexts = 0, bool warmRhs = false,
                  bool warmBackSplit = false, bool warmSingleTile = false,
                  bool warmBypass = false, bool criticalArb = false,
                  unsigned warmBaseMode = 0,
                  unsigned arrayMode = 0, unsigned maxSegments = 1,
                  bool mixedKind = false, bool panelPack = false)
    {
        const unsigned warmContexts = warmSingleTile ? 2 * contexts : contexts;
        const uint64_t arrayExtra =
            (arrayMode == 1 ? 128 :
             arrayMode == 2 ? 256 :
             arrayMode == 3 ? 256 + (maxSegments > 1 ? (maxSegments - 1) * 128 : 0) : 0) +
            (mixedKind ? 128 : 0) + (panelPack ? 128 : 0);
        return (3 * contexts + 2) * 576 + (contexts + 2) * 128 +
               uint64_t(depth) * returnSlotBytes(arrayMode) +
               ingressPorts * 128 + 512 +
               reverseContexts * (576 + 128) +
               (warmRhs ? warmRhsGroups(contexts) * 128 + 256 : 0) +
               (warmBackSplit ? 512 : 0) +
               (warmSingleTile ? contexts * 128 : 0) +
               (warmBypass ? warmContexts * 128 : 0) +
               (criticalArb ? 128 : 0) +
               (warmBaseMode ? 128 : 0) +
               (warmBaseMode == 2 ? warmContexts * 128 : 0) +
               arrayExtra;
    }

    static constexpr unsigned warmRhsGroups(unsigned contexts)
    { return 3 * contexts / WarmRhsTiles; }

    void
    configure(unsigned contexts, const Config &config,
              unsigned reverseContexts = 0, unsigned bypassCount = 0)
    {
        cfg = config;
        tiles.assign(3 * contexts + 2 + reverseContexts, {});
        latches.assign(bypassCount, {});
        pending.clear();
        pending2d.clear();
        writebacksDue.clear();
        pending.reserve(cfg.depth);
        pending2d.reserve(cfg.depth);
        stats = {};
        windows = {};
        stats2d = {};
        peWriteUsed.fill(0);
        returnBlockedByPhase.fill(false);
        cycle = 0;
        nextDiv = 0;
        peak = 0;
    }

    void
    resetStats()
    {
        assert(pending.empty() && pending2d.empty());
        writebacksDue.clear();
        peWriteUsed.fill(0);
        nextDiv = 0;
        stats = {};
        windows = {};
        stats2d = {};
        peak = 0;
    }

    void
    tick(uint64_t now)
    {
        cycle = now;
        lanes = cfg.lanes;
        reads = cfg.readPorts;
        writes = cfg.writePorts;
        ingress = cfg.ingressPorts;
        verticalWriteUsed = false;
        bundleIssuedNow = false;
        peWriteUsed.fill(0);
        issuedNow = {};
        offeredNow = 0;
        dependencyNow = blockedReturnNow = false;
        returnBlockedByPhase.fill(false);
        if (cfg.mixedKind)
            writebacksDue.erase(writebacksDue.begin(), writebacksDue.upper_bound(now));
        for (auto &tile : tiles) {
            tile.read.fill(false);
            tile.written.fill(false);
            tile.colRead.fill(false);
            tile.loadBlock.fill(0);
        }
        for (auto &latch : latches)
            latch.loadBlock = 0;
        if (cfg.mixedKind) {
            // The divider competes for the same PE write port as 2D results.
            // Within a priority, retain oldest-first 2D segment order.
            for (unsigned priority = 0; priority < 4; ++priority) {
                retireRows(priority);
                for (auto &ret : pending2d) {
                    for (unsigned i = 0; i < ret.count; ++i) {
                        auto &sub = ret.subs[i];
                        if (sub.retired || sub.ready > cycle ||
                            writebackPriority(sub.phase) != priority)
                            continue;
                        if (!sub.isBypass && !peWriteAvailable(sub.rowMask, sub.colMask)) {
                            ++stats[sub.phase].returnWait;
                            noteReturnBlocked(sub.phase);
                            reserveWriteback(cycle + 1, sub.rowMask, sub.colMask, sub.phase);
                            continue;
                        }
                        retireSegment(ret, sub);
                    }
                }
            }
            for (auto it = pending2d.begin(); it != pending2d.end();) {
                bool allRetired = true;
                for (unsigned i = 0; i < it->count; ++i)
                    allRetired &= it->subs[i].retired;
                if (allRetired)
                    it = pending2d.erase(it);
                else
                    ++it;
            }
            return;
        }

        retireRows(0);
        for (auto it = pending2d.begin(); it != pending2d.end();) {
            if (it->ready > cycle) {
                ++it;
                continue;
            }
            unsigned conflict = 0;
            while (conflict < it->count) {
                const auto &sub = it->subs[conflict];
                if (!sub.isBypass && !peWriteAvailable(sub.rowMask, sub.colMask))
                    break;
                ++conflict;
            }
            if (conflict < it->count) {
                // Preserve the legacy bundle count. Every phase held by this
                // atomic return contributes to the per-cycle counter.
                ++stats[it->subs[conflict].phase].returnWait;
                for (unsigned i = 0; i < it->count; ++i)
                    noteReturnBlocked(it->subs[i].phase);
                ++it;
                continue;
            }
            for (unsigned i = 0; i < it->count; ++i)
                retireSegment(*it, it->subs[i]);
            it = pending2d.erase(it);
        }
    }

    void
    sample(Window window)
    {
        auto &s = windows[window];
        s.cycles++;
        s.offered += offeredNow;
        s.cappedOffer += offeredNow < cfg.lanes ? offeredNow : cfg.lanes;
        const unsigned liveReturns = pending.size() + pending2d.size();
        s.pendingSum += liveReturns;
        s.fullQueueCycles += liveReturns == cfg.depth;
        s.blockedReturnCycles += blockedReturnNow;
        for (unsigned i = 0; i < PhaseCount; ++i)
            s.issued[i] += issuedNow[i];
        const auto state = !lanes ? Full : lanes < cfg.lanes ? Partial :
            offeredNow ? ReadyBlocked : dependencyNow ? DependencyOnly : NoAttempt;
        s.states[state]++;
    }

    bool
    ready(Position p, uint8_t mask) const
    {
        const auto &tile = tiles[p.tile];
        return (tile.valid[p.row] & mask) == mask &&
            !((tile.busy[p.row] | tile.loadBlock[p.row]) & mask);
    }

    bool
    idle(unsigned tile) const
    {
        for (auto mask : tiles[tile].busy)
            if (mask)
                return false;
        return true;
    }

    bool empty() const { return pending.empty() && pending2d.empty(); }
    unsigned pendingReturns() const { return pending.size() + pending2d.size(); }
    unsigned bypassCount() const { return latches.size(); }
    unsigned freeLanes() const { return lanes; }
    unsigned freeReads() const { return reads; }
    unsigned freeIngress() const { return ingress; }
    bool queueFull() const { return pendingReturns() >= cfg.depth; }
    bool is2d() const { return cfg.arrayMode != ArrayMode::OneD; }
    bool bundleIssued() const { return bundleIssuedNow; }
    const Config &config() const { return cfg; }
    const Stats2d &statistics2d() const { return stats2d; }
    void noteOffered2d(unsigned pes) { offeredNow += pes; }
    void noteDependency2d(Phase phase)
    {
        dependencyNow = true;
        stats[phase].stalls[Dependency]++;
    }
    void noteStall2d(Phase phase, Stall reason) { stats[phase].stalls[reason]++; }
    void notePackStallRow() { stats2d.packStallRow++; }
    void notePackStallCol() { stats2d.packStallCol++; }
    void notePackStallIngress() { stats2d.packStallIngress++; }
    void notePackStallReadPort() { stats2d.packStallReadPort++; }

    static constexpr Kind
    opKind(Op2d op)
    {
        switch (op) {
          case Op2d::Fmopa:
          case Op2d::FmopaFactorRows:
          case Op2d::SubtractElements:
          case Op2d::SubtractOuterBypass:
          case Op2d::FmaRowToBypass:
            return Fma;
          case Op2d::ScaleRow:
          case Op2d::IngressRow:
          case Op2d::ScaleElements:
          case Op2d::ScaleBypass:
          case Op2d::ScaleRowToBypass:
          case Op2d::IngressBypass:
            return Mul;
          case Op2d::CombineRow:
          case Op2d::CombineBypass:
            return Add;
        }
        return Fma;
    }

    static constexpr bool
    opIsBypass(Op2d op)
    {
        switch (op) {
          case Op2d::ScaleBypass:
          case Op2d::SubtractOuterBypass:
          case Op2d::ScaleRowToBypass:
          case Op2d::FmaRowToBypass:
          case Op2d::IngressBypass:
          case Op2d::CombineBypass:
            return true;
          default:
            return false;
        }
    }

    static constexpr bool
    opUsesIngress(Op2d op)
    {
        return op == Op2d::IngressRow || op == Op2d::IngressBypass;
    }

    static unsigned
    activePes2d(const Segment2d &seg)
    {
        return words(seg.rowMask) * words(seg.colMask);
    }

    bool
    readSliceUsed(const ReadSlice &s) const
    {
        return s.isCol ? tiles[s.tile].colRead[s.idx] : tiles[s.tile].read[s.idx];
    }

    unsigned
    collectReads2d(const Segment2d &seg, SegmentReads &out) const
    {
        unsigned n = 0;
        auto push = [&](ReadSlice s) {
            for (unsigned i = 0; i < n; ++i)
                if (out[i] == s)
                    return;
            assert(n < out.size());
            out[n++] = s;
        };
        switch (seg.op) {
          case Op2d::Fmopa:
            push({true, seg.aPos.tile, seg.aCol});
            push({false, seg.bPos.tile, seg.bPos.row});
            break;
          case Op2d::FmopaFactorRows:
            for (unsigned r = 0; r < Width; ++r)
                if (seg.rowMask & (1u << r))
                    push({false, seg.factorPos[r].tile, seg.factorPos[r].row});
            push({false, seg.bPos.tile, seg.bPos.row});
            break;
          case Op2d::ScaleRow:
            if (!seg.useScalar)
                push({false, seg.aPos.tile, seg.aPos.row});
            break;
          case Op2d::CombineRow:
            push({false, seg.aPos.tile, seg.aPos.row});
            push({false, seg.bPos.tile, seg.bPos.row});
            break;
          case Op2d::IngressRow:
          case Op2d::IngressBypass:
            break;
          case Op2d::ScaleElements:
            push({false, seg.aPos.tile, seg.aPos.row});
            break;
          case Op2d::SubtractElements:
            push({false, seg.aPos.tile, seg.aPos.row});
            push({false, seg.bPos.tile, seg.bPos.row});
            break;
          case Op2d::ScaleBypass:
            push({false, seg.aPos.tile, seg.aPos.row});
            break;
          case Op2d::SubtractOuterBypass:
          case Op2d::ScaleRowToBypass:
          case Op2d::FmaRowToBypass:
            push({false, seg.bPos.tile, seg.bPos.row});
            break;
          case Op2d::CombineBypass:
            if (!seg.aIsBypass)
                push({false, seg.aPos.tile, seg.aPos.row});
            if (!seg.bIsBypass)
                push({false, seg.bPos.tile, seg.bPos.row});
            break;
        }
        return n;
    }

    bool
    ready2d(const Segment2d &seg) const
    {
        if (!seg.rowMask || !seg.colMask)
            return false;
        if (seg.op != Op2d::Fmopa && seg.op != Op2d::FmopaFactorRows &&
            (seg.dstRow >= Width || seg.rowMask != (1u << seg.dstRow)))
            return false;
        switch (seg.op) {
          case Op2d::Fmopa:
            for (unsigned r = 0; r < Width; ++r) {
                if (!(seg.rowMask & (1u << r)))
                    continue;
                if (!ready({seg.dstTile, r}, seg.colMask) ||
                    !ready({seg.aPos.tile, r}, static_cast<uint8_t>(1u << seg.aCol)))
                    return false;
            }
            return ready(seg.bPos, seg.colMask);
          case Op2d::FmopaFactorRows:
            for (unsigned r = 0; r < Width; ++r) {
                if (!(seg.rowMask & (1u << r)))
                    continue;
                if (!ready({seg.dstTile, r}, seg.colMask) ||
                    !ready(seg.factorPos[r],
                           static_cast<uint8_t>(1u << seg.factorCol[r])))
                    return false;
            }
            return ready(seg.bPos, seg.colMask);
          case Op2d::ScaleRow:
            if (!ready({seg.dstTile, seg.dstRow}, seg.colMask))
                return false;
            return seg.useScalar ||
                ready(seg.aPos, static_cast<uint8_t>(1u << seg.aCol));
          case Op2d::CombineRow:
            return !((tiles[seg.dstTile].busy[seg.dstRow] |
                      tiles[seg.dstTile].loadBlock[seg.dstRow]) & seg.colMask) &&
                ready(seg.aPos, seg.colMask) && ready(seg.bPos, seg.colMask);
          case Op2d::IngressRow:
            return !((tiles[seg.dstTile].busy[seg.dstRow] |
                      tiles[seg.dstTile].loadBlock[seg.dstRow]) & seg.colMask);
          case Op2d::ScaleElements:
            return ready({seg.dstTile, seg.dstRow}, seg.colMask) &&
                ready(seg.aPos, seg.colMask);
          case Op2d::SubtractElements:
            return ready({seg.dstTile, seg.dstRow}, seg.colMask) &&
                ready(seg.aPos, seg.colMask) && ready(seg.bPos, seg.colMask);
          case Op2d::ScaleBypass:
            return readyBypass(seg.dstTile, seg.colMask) &&
                ready(seg.aPos, static_cast<uint8_t>(1u << seg.aCol));
          case Op2d::SubtractOuterBypass:
            return readyBypass(seg.dstTile,
                               static_cast<uint8_t>(seg.colMask | (1u << seg.aCol))) &&
                ready(seg.bPos, seg.colMask);
          case Op2d::ScaleRowToBypass:
            return !((latches[seg.dstTile].busy |
                      latches[seg.dstTile].loadBlock) & seg.colMask) &&
                ready(seg.bPos, seg.colMask);
          case Op2d::FmaRowToBypass:
            return readyBypass(seg.dstTile, seg.colMask) &&
                ready(seg.bPos, seg.colMask);
          case Op2d::IngressBypass:
            return !((latches[seg.dstTile].busy |
                      latches[seg.dstTile].loadBlock) & seg.colMask);
          case Op2d::CombineBypass:
            return !((latches[seg.dstTile].busy |
                      latches[seg.dstTile].loadBlock) & seg.colMask) &&
                (seg.aIsBypass ? readyBypass(seg.aPos.tile, seg.colMask) :
                                 ready(seg.aPos, seg.colMask)) &&
                (seg.bIsBypass ? readyBypass(seg.bPos.tile, seg.colMask) :
                                 ready(seg.bPos, seg.colMask));
        }
        return false;
    }

    bool
    willCollideWriteback2d(const Segment2d &seg) const
    {
        if (!cfg.mixedKind || opIsBypass(seg.op))
            return false;
        const uint64_t mask = peMask(seg.rowMask, seg.colMask);
        return (nominalWritebackBitmap2d(opKind(seg.op)) & mask) != 0;
    }

    uint64_t
    nominalWritebackBitmap2d(Kind kind) const
    {
        if (!cfg.mixedKind)
            return 0;
        const auto due = writebacksDue.find(cycle + segmentLatency(kind));
        if (due == writebacksDue.end())
            return 0;
        // Match sme_za_rs_issue.sv: issue-time writeback conflict checks the
        // phase-agnostic nominal occupancy bitmap, while retirement still
        // arbitrates writeback slots by phase priority.
        uint64_t combined = 0;
        for (uint64_t booked : due->second)
            combined |= booked;
        return combined;
    }

    unsigned
    pendingByPhase(Phase phase) const
    {
        unsigned count = 0;
        for (const auto &ret : pending)
            if (ret.phase == phase)
                ++count;
        for (const auto &ret : pending2d)
            for (unsigned i = 0; i < ret.count; ++i)
                if (!ret.subs[i].retired && ret.subs[i].phase == phase)
                    ++count;
        return count;
    }

    unsigned
    pendingLanesByPhase(Phase phase) const
    {
        unsigned total = 0;
        for (const auto &ret : pending)
            if (ret.phase == phase)
                total += words(ret.mask);
        for (const auto &ret : pending2d)
            for (unsigned i = 0; i < ret.count; ++i)
                if (!ret.subs[i].retired && ret.subs[i].phase == phase)
                    total += ret.subs[i].activePes;
        return total;
    }

    bool
    issue2dBundle(const Segment2d *segs, unsigned count)
    {
        assert(is2d());
        if (!count || count > cfg.maxSegments || count > 4 || bundleIssuedNow)
            return false;
        const Kind kind = opKind(segs[0].op);
        if (queueFull())
            return stall(segs[0].phase, Queue);
        uint8_t usedRows = 0, usedCols = 0;
        unsigned needIngress = 0, totalPes = 0;
        // Rows are disjoint, and each segment needs at most one
        // additional slice per occupied row plus one pivot slice.
        std::array<ReadSlice, 16> freshReads{};
        unsigned freshCount = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto &seg = segs[i];
            if ((!cfg.mixedKind && opKind(seg.op) != kind) || !seg.rowMask || !seg.colMask)
                return false;
            if (seg.rowMask & usedRows)
                return false;
            if (cfg.arrayMode == ArrayMode::TwoDBd && (seg.colMask & usedCols))
                return false;
            if (opIsBypass(seg.op)) {
                for (unsigned j = 0; j < i; ++j)
                    if (opIsBypass(segs[j].op) && segs[j].dstTile == seg.dstTile)
                        return false;
            }
            if (!ready2d(seg)) {
                dependencyNow = true;
                return stall(seg.phase, Dependency);
            }
            totalPes += activePes2d(seg);
            usedRows |= seg.rowMask;
            usedCols |= seg.colMask;
            if (opUsesIngress(seg.op))
                ++needIngress;
            SegmentReads segReads{};
            const unsigned n = collectReads2d(seg, segReads);
            for (unsigned r = 0; r < n; ++r) {
                if (readSliceUsed(segReads[r]))
                    continue;
                bool seen = false;
                for (unsigned f = 0; f < freshCount; ++f)
                    seen |= freshReads[f] == segReads[r];
                if (!seen)
                    freshReads[freshCount++] = segReads[r];
            }
        }
        if (totalPes > lanes)
            return stall(segs[0].phase, Lanes);
        if (needIngress > ingress)
            return stall(segs[0].phase, Ingress);
        if (freshCount > reads)
            return stall(segs[0].phase, ReadPort);

        // Commit ZA read ports, attributing each newly opened slice to the
        // first segment in the bundle that consumes it.
        for (unsigned i = 0; i < count; ++i) {
            const auto &seg = segs[i];
            SegmentReads segReads{};
            const unsigned n = collectReads2d(seg, segReads);
            for (unsigned r = 0; r < n; ++r) {
                const auto &s = segReads[r];
                if (readSliceUsed(s))
                    continue;
                if (s.isCol)
                    tiles[s.tile].colRead[s.idx] = true;
                else
                    tiles[s.tile].read[s.idx] = true;
                --reads;
                stats[seg.phase].zaReads++;
            }
        }
        ingress -= needIngress;

        Return2d ret{};
        ret.count = count;

        // Compute all segment outputs before mutating scoreboard busy bits.
        for (unsigned i = 0; i < count; ++i) {
            const auto &seg = segs[i];
            const Kind segKind = opKind(seg.op);
            auto &sub = ret.subs[i];
            sub.dstTile = seg.dstTile;
            sub.dstRow = seg.dstRow;
            sub.rowMask = seg.rowMask;
            sub.colMask = seg.colMask;
            sub.activePes = activePes2d(seg);
            sub.phase = seg.phase;
            sub.kind = segKind;
            sub.ready = cycle + segmentLatency(segKind);
            sub.isBypass = opIsBypass(seg.op);
            sub.retired = false;
            if (sub.ready > ret.ready)
                ret.ready = sub.ready;

            switch (seg.op) {
              case Op2d::Fmopa: {
                const Row &bRow = row(seg.bPos);
                for (unsigned r = 0; r < Width; ++r) {
                    if (!(seg.rowMask & (1u << r)))
                        continue;
                    const double aVal = tiles[seg.aPos.tile].data[r][seg.aCol];
                    const Row &dRow = tiles[seg.dstTile].data[r];
                    for (unsigned c = 0; c < Width; ++c)
                        if (seg.colMask & (1u << c))
                            ret.data[r][c] = std::fma(-aVal, bRow[c], dRow[c]);
                }
                break;
              }
              case Op2d::FmopaFactorRows: {
                const Row &bRow = row(seg.bPos);
                for (unsigned r = 0; r < Width; ++r) {
                    if (!(seg.rowMask & (1u << r)))
                        continue;
                    const double aVal = row(seg.factorPos[r])[seg.factorCol[r]];
                    const Row &dRow = tiles[seg.dstTile].data[r];
                    for (unsigned c = 0; c < Width; ++c)
                        if (seg.colMask & (1u << c))
                            ret.data[r][c] = std::fma(-aVal, bRow[c], dRow[c]);
                }
                break;
              }
              case Op2d::ScaleRow: {
                const double factor = seg.useScalar ? seg.scalar :
                    row(seg.aPos)[seg.aCol];
                const Row &dRow = tiles[seg.dstTile].data[seg.dstRow];
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = dRow[c] * factor;
                break;
              }
              case Op2d::CombineRow: {
                const Row &va = row(seg.aPos);
                const Row &vb = row(seg.bPos);
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = seg.subtract ? va[c] - vb[c]
                                                               : va[c] + vb[c];
                break;
              }
              case Op2d::IngressRow: {
                stats[seg.phase].ingressWords += sub.activePes;
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = seg.ingressValues[c] * seg.scalar;
                break;
              }
              case Op2d::ScaleElements: {
                const Row &dRow = tiles[seg.dstTile].data[seg.dstRow];
                const Row &fRow = row(seg.aPos);
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = dRow[c] * fRow[c];
                break;
              }
              case Op2d::SubtractElements: {
                const Row &dRow = tiles[seg.dstTile].data[seg.dstRow];
                const Row &fRow = row(seg.aPos);
                const Row &pRow = row(seg.bPos);
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = std::fma(-fRow[c], pRow[c], dRow[c]);
                break;
              }
              case Op2d::ScaleBypass: {
                const double factor = row(seg.aPos)[seg.aCol];
                const Row &src = latches[seg.dstTile].data;
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = src[c] * factor;
                stats[seg.phase].bypassReads++;
                break;
              }
              case Op2d::SubtractOuterBypass: {
                const double mult = latches[seg.dstTile].data[seg.aCol];
                const Row &fRow = row(seg.bPos);
                const Row &src = latches[seg.dstTile].data;
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = std::fma(-mult, fRow[c], src[c]);
                stats[seg.phase].bypassReads++;
                break;
              }
              case Op2d::ScaleRowToBypass: {
                const Row &src = row(seg.bPos);
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = src[c] * seg.scalar;
                break;
              }
              case Op2d::FmaRowToBypass: {
                const Row &src = row(seg.bPos);
                const Row &acc = latches[seg.dstTile].data;
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = std::fma(src[c], seg.scalar, acc[c]);
                stats[seg.phase].bypassReads++;
                break;
              }
              case Op2d::IngressBypass: {
                stats[seg.phase].ingressWords += sub.activePes;
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = seg.ingressValues[c] * seg.scalar;
                break;
              }
              case Op2d::CombineBypass: {
                const Row &va = seg.aIsBypass ? latches[seg.aPos.tile].data : row(seg.aPos);
                const Row &vb = seg.bIsBypass ? latches[seg.bPos.tile].data : row(seg.bPos);
                for (unsigned c = 0; c < Width; ++c)
                    if (seg.colMask & (1u << c))
                        ret.data[seg.dstRow][c] = seg.subtract ? va[c] - vb[c]
                                                               : va[c] + vb[c];
                if (seg.aIsBypass)
                    stats[seg.phase].bypassReads++;
                if (seg.bIsBypass)
                    stats[seg.phase].bypassReads++;
                break;
              }
            }
        }

        // Mark busy scoreboard bits and accumulate per-phase / 2D stats.
        lanes -= totalPes;
        bundleIssuedNow = true;
        for (unsigned i = 0; i < count; ++i) {
            const auto &sub = ret.subs[i];
            if (sub.isBypass) {
                latches[sub.dstTile].busy |= sub.colMask;
            } else {
                reserveWriteback(sub.ready, sub.rowMask, sub.colMask, sub.phase);
                for (unsigned r = 0; r < Width; ++r)
                    if (sub.rowMask & (1u << r))
                        tiles[sub.dstTile].busy[r] |= sub.colMask;
            }
            auto &s = stats[sub.phase];
            s.issued[sub.kind] += sub.activePes;
            issuedNow[sub.phase] += sub.activePes;
            if (!s.beats)
                s.first = cycle;
            if (!s.beats || s.last != cycle)
                s.issueCycles++;
            s.last = cycle;
            s.beats++;
        }
        stats2d.bundlesTotal++;
        stats2d.bundlesBySeg[count - 1]++;
        stats2d.peActiveSum += totalPes;
        bool hasFmopa = false;
        unsigned fmopaPes = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (segs[i].op == Op2d::Fmopa || segs[i].op == Op2d::FmopaFactorRows) {
                hasFmopa = true;
                fmopaPes += ret.subs[i].activePes;
            }
        }
        if (hasFmopa) {
            stats2d.fmopaBundles++;
            stats2d.fmopaPeActive += fmopaPes;
        }
        const unsigned filledRows = words(usedRows);
        assert(filledRows >= 1 && filledRows <= Width);
        stats2d.rowFillHist[filledRows - 1]++;

        pending2d.push_back(ret);
        const unsigned liveReturns = pendingReturns();
        if (liveReturns > peak)
            peak = liveReturns;
        return true;
    }

    void
    release(unsigned tile)
    {
        assert(idle(tile));
        tiles[tile].valid.fill(0);
    }

    bool
    readyBypass(unsigned id, uint8_t mask) const
    {
        const auto &latch = latches[id];
        return (latch.valid & mask) == mask &&
            !((latch.busy | latch.loadBlock) & mask);
    }

    bool idleBypass(unsigned id) const { return !latches[id].busy; }

    void
    releaseBypass(unsigned id)
    {
        assert(idleBypass(id));
        latches[id].valid = 0;
    }

    bool
    loadBypass(unsigned id, uint8_t mask, const Row &values, Phase phase)
    {
        auto &latch = latches[id];
        if (latch.busy & mask)
            return stall(phase, Dependency);
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                latch.data[j] = values[j];
        latch.valid |= mask;
        latch.loadBlock |= mask;
        stats[phase].loadWords += words(mask);
        stats[phase].bypassWrites++;
        return true;
    }

    bool
    readBypass(unsigned id, uint8_t mask, Row &values, Phase phase)
    {
        if (!readyBypass(id, mask))
            return stall(phase, Dependency);
        values = latches[id].data;
        stats[phase].bypassReads++;
        return true;
    }

    bool
    scaleBypass(unsigned id, uint8_t mask, Position factor, unsigned column,
                Phase phase)
    {
        assert(mask);
        const uint8_t fmask = 1u << column;
        if (!readyBypass(id, mask) || !ready(factor, fmask)) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!readOperands({{factor, fmask}}, phase))
            return false;
        const double scaleFactor = row(factor)[column];
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = latches[id].data[j] * scaleFactor;
        stats[phase].bypassReads++;
        enqueueBypass(id, mask, result, Mul, phase, cfg.mulLatency);
        return true;
    }

    bool
    subtractOuterBypass(unsigned id, uint8_t mask, unsigned pivotColumn,
                        Position factorRow, Phase phase)
    {
        assert(mask);
        const uint8_t needMask = mask | (1u << pivotColumn);
        if (!readyBypass(id, needMask) || !ready(factorRow, mask)) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!readOperands({{factorRow, mask}}, phase))
            return false;
        const double multiplier = latches[id].data[pivotColumn];
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = std::fma(-multiplier, row(factorRow)[j],
                                     latches[id].data[j]);
        stats[phase].bypassReads++;
        enqueueBypass(id, mask, result, Fma, phase, cfg.fmaLatency);
        return true;
    }

    bool
    scaleRowToBypass(unsigned id, uint8_t mask, Position srcRow, double scalar,
                     Phase phase)
    {
        assert(mask);
        if (((latches[id].busy | latches[id].loadBlock) & mask) ||
            !ready(srcRow, mask)) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!readOperands({{srcRow, mask}}, phase))
            return false;
        const Row &src = row(srcRow);
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = src[j] * scalar;
        enqueueBypass(id, mask, result, Mul, phase, cfg.mulLatency);
        return true;
    }

    bool
    fmaRowToBypass(unsigned id, uint8_t mask, Position srcRow, double scalar,
                   Phase phase)
    {
        assert(mask);
        if (!readyBypass(id, mask) || !ready(srcRow, mask)) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!readOperands({{srcRow, mask}}, phase))
            return false;
        const Row &src = row(srcRow);
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = std::fma(src[j], scalar, latches[id].data[j]);
        stats[phase].bypassReads++;
        enqueueBypass(id, mask, result, Fma, phase, cfg.fmaLatency);
        return true;
    }

    bool
    ingressProductBypass(unsigned id, uint8_t mask, const Row &values,
                         double factor, Phase phase)
    {
        assert(mask);
        if ((latches[id].busy | latches[id].loadBlock) & mask) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!ingress)
            return stall(phase, Ingress);
        --ingress;
        stats[phase].ingressWords += active;
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = values[j] * factor;
        enqueueBypass(id, mask, result, Mul, phase, cfg.mulLatency);
        return true;
    }

    bool
    combineBypass(unsigned dstId, uint8_t mask, bool aIsBypass, Position aPos,
                  bool bIsBypass, Position bPos, bool subtract, Phase phase)
    {
        assert(mask);
        bool dependent = (latches[dstId].busy | latches[dstId].loadBlock) & mask;
        dependent |= aIsBypass ? !readyBypass(aPos.tile, mask) : !ready(aPos, mask);
        dependent |= bIsBypass ? !readyBypass(bPos.tile, mask) : !ready(bPos, mask);
        if (dependent) {
            dependencyNow = true;
            return stall(phase, Dependency);
        }
        const unsigned active = words(mask);
        offeredNow += active;
        if (active > lanes)
            return stall(phase, Lanes);
        if (queueFull())
            return stall(phase, Queue);
        if (!aIsBypass && !bIsBypass) {
            if (!readOperands({{aPos, mask}, {bPos, mask}}, phase))
                return false;
        } else if (!aIsBypass) {
            if (!readOperands({{aPos, mask}}, phase))
                return false;
        } else if (!bIsBypass) {
            if (!readOperands({{bPos, mask}}, phase))
                return false;
        }
        const Row &va = aIsBypass ? latches[aPos.tile].data : row(aPos);
        const Row &vb = bIsBypass ? latches[bPos.tile].data : row(bPos);
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = subtract ? va[j] - vb[j] : va[j] + vb[j];
        if (aIsBypass)
            stats[phase].bypassReads++;
        if (bIsBypass)
            stats[phase].bypassReads++;
        enqueueBypass(dstId, mask, result, Add, phase, cfg.addLatency);
        return true;
    }

    bool
    load(Position p, uint8_t mask, const Row &values, Phase phase)
    {
        if (tiles[p.tile].busy[p.row] & mask)
            return stall(phase, Dependency);
        if (is2d() && (peWriteUsed[p.row] & mask))
            return stall(phase, WritePort);
        if (!write(p, mask, values, phase))
            return stall(phase, WritePort);
        if (is2d())
            peWriteUsed[p.row] |= mask;
        tiles[p.tile].loadBlock[p.row] |= mask;
        stats[phase].loadWords += words(mask);
        return true;
    }

    // A vertical ZA slice uses one wide write grant and one element write
    // enable in each selected row bank. Reserve all banks atomically.
    bool
    loadVertical(unsigned tileIndex, unsigned column, uint8_t rowMask,
                 const Row &values, Phase phase)
    {
        assert(column < Width && rowMask);
        auto &tile = tiles[tileIndex];
        const uint8_t bit = 1u << column;
        if (!writes || verticalWriteUsed)
            return stall(phase, WritePort);
        for (unsigned r = 0; r < Width; ++r) {
            if (!(rowMask & (1u << r)))
                continue;
            if (tile.busy[r] & bit)
                return stall(phase, Dependency);
            if (tile.written[r] || (is2d() && (peWriteUsed[r] & bit)))
                return stall(phase, WritePort);
        }
        for (unsigned r = 0; r < Width; ++r) {
            if (rowMask & (1u << r)) {
                tile.data[r][column] = values[r];
                tile.valid[r] |= bit;
                tile.loadBlock[r] |= bit;
                tile.written[r] = true;
                if (is2d())
                    peWriteUsed[r] |= bit;
            }
        }
        --writes;
        verticalWriteUsed = true;
        auto &s = stats[phase];
        ++s.verticalLoads;
        ++s.zaWrites;
        s.loadWords += words(rowMask);
        return true;
    }

    bool
    read(Position p, uint8_t mask, Row &values, Phase phase)
    {
        if (!ready(p, mask))
            return stall(phase, Dependency);
        if (!readOperands({{p, mask}}, phase))
            return false;
        values = row(p);
        return true;
    }

    bool
    reciprocal(Position dst, unsigned column, double pivot, Phase phase)
    {
        const uint8_t mask = 1u << column;
        if (!canIssue(dst, mask, Div, phase))
            return false;
        Row result{};
        result[column] = 1.0 / pivot;
        enqueue(dst, mask, result, Div, phase, cfg.divLatency);
        nextDiv = cycle + cfg.divIi;
        return true;
    }

    bool
    scale(Position dst, uint8_t mask, Position factor, unsigned column,
          Phase phase)
    {
        if (!canIssue(dst, mask, Mul, phase,
                      {{dst, mask}, {factor, uint8_t(1u << column)}}))
            return false;
        issueScale(dst, mask, row(factor)[column], phase);
        return true;
    }

    bool
    scale(Position dst, uint8_t mask, double factor, Phase phase)
    {
        if (!canIssue(dst, mask, Mul, phase, {{dst, mask}}))
            return false;
        issueScale(dst, mask, factor, phase);
        return true;
    }

    bool
    scaleElements(Position dst, uint8_t mask, Position factors, Phase phase)
    {
        if (!canIssue(dst, mask, Mul, phase, {{dst, mask}, {factors, mask}}))
            return false;
        Row result{};
        for (unsigned lane = 0; lane < Width; ++lane)
            if (mask & (1u << lane))
                result[lane] = row(dst)[lane] * row(factors)[lane];
        enqueue(dst, mask, result, Mul, phase, cfg.mulLatency);
        return true;
    }

    // The already returned SPM coefficient column and registered feedback
    // enter the array directly. The product still needs an actual ZA return.
    bool
    ingressProduct(Position dst, uint8_t mask, const Row &values, double factor,
                   Phase phase)
    {
        if (!canIssue(dst, mask, Mul, phase))
            return false;
        if (!ingress)
            return stall(phase, Ingress);
        --ingress;
        stats[phase].ingressWords += words(mask);
        issueScale(dst, mask, values, factor, phase);
        return true;
    }

    bool
    subtractOuterRow(Position dst, uint8_t mask, Position columnSource,
                     unsigned column, Position pivotRow, Phase phase)
    {
        if (!canIssue(dst, mask, Fma, phase,
                      {{dst, mask}, {columnSource, uint8_t(1u << column)},
                       {pivotRow, mask}}))
            return false;
        const double multiplier = row(columnSource)[column];
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = std::fma(-multiplier, row(pivotRow)[j], row(dst)[j]);
        enqueue(dst, mask, result, Fma, phase, cfg.fmaLatency);
        return true;
    }

    bool
    combine(Position dst, uint8_t mask, Position a, Position b,
            bool subtract, Phase phase)
    {
        if (!canIssue(dst, mask, Add, phase, {{a, mask}, {b, mask}}))
            return false;
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = subtract ? row(a)[j] - row(b)[j]
                                     : row(a)[j] + row(b)[j];
        enqueue(dst, mask, result, Add, phase, cfg.addLatency);
        return true;
    }

    bool
    subtractElements(Position dst, uint8_t mask, Position factors,
                     Position pivot, Phase phase)
    {
        if (!canIssue(dst, mask, Fma, phase,
                      {{dst, mask}, {factors, mask}, {pivot, mask}}))
            return false;
        Row result{};
        for (unsigned lane = 0; lane < Width; ++lane)
            if (mask & (1u << lane))
                result[lane] = std::fma(-row(factors)[lane], row(pivot)[lane],
                                        row(dst)[lane]);
        enqueue(dst, mask, result, Fma, phase, cfg.fmaLatency);
        return true;
    }

    const Stats &statistics(Phase phase) const { return stats[phase]; }
    const WindowStats &statistics(Window window) const { return windows[window]; }
    unsigned peakReturns() const { return peak; }

  private:
    struct Tile
    {
        std::array<Row, Width> data{};
        std::array<uint8_t, Width> valid{}, busy{}, loadBlock{};
        std::array<bool, Width> read{}, written{}, colRead{};
    };
    struct ActiveLatch
    {
        Row data{};
        uint8_t valid = 0, busy = 0, loadBlock = 0;
    };
    struct Return
    {
        uint64_t ready;
        Position target;
        Row result;
        uint8_t mask;
        Kind kind;
        Phase phase;
        bool isBypass = false;
    };
    struct Return2dSub
    {
        unsigned dstTile = 0;
        unsigned dstRow = 0;
        uint8_t rowMask = 0;
        uint8_t colMask = 0;
        unsigned activePes = 0;
        Phase phase = Lu;
        Kind kind = Fma;
        uint64_t ready = 0;
        bool isBypass = false;
        bool retired = false;
    };
    struct Return2d
    {
        uint64_t ready = 0;
        unsigned count = 0;
        std::array<Return2dSub, 4> subs{};
        std::array<Row, Width> data{};
    };
    struct Operand
    {
        Position p;
        uint8_t mask;
    };
    static_assert(sizeof(Tile) <= 576);
    static_assert(sizeof(ActiveLatch) <= 128);
    static_assert(sizeof(Return) <= 128);

    Config cfg;
    std::vector<Tile> tiles;
    std::vector<ActiveLatch> latches;
    std::vector<Return> pending;
    std::vector<Return2d> pending2d;
    // Nominal and delayed completions per priority level, indexed once per due
    // cycle instead of scanning every pending result for every candidate.
    std::map<uint64_t, std::array<uint64_t, 4>> writebacksDue;
    std::array<Stats, PhaseCount> stats{};
    std::array<WindowStats, WindowCount> windows{};
    Stats2d stats2d{};
    std::array<unsigned, PhaseCount> issuedNow{};
    std::array<uint8_t, Width> peWriteUsed{};
    std::array<bool, PhaseCount> returnBlockedByPhase{};
    uint64_t cycle = 0, nextDiv = 0;
    unsigned lanes = 0, reads = 0, writes = 0, ingress = 0, peak = 0;
    unsigned offeredNow = 0;
    bool dependencyNow = false, blockedReturnNow = false, verticalWriteUsed = false;
    bool bundleIssuedNow = false;

    static unsigned words(uint8_t mask) { return __builtin_popcount(mask); }
    const Row &row(Position p) const { return tiles[p.tile].data[p.row]; }

    static unsigned
    writebackPriority(Phase phase)
    {
        switch (phase) {
          case Backward: return 0;
          case Forward: return 1;
          case Lu: return 2;
          default: return 3;
        }
    }

    unsigned
    segmentLatency(Kind kind) const
    {
        return kind == Fma ? cfg.fmaLatency : kind == Mul ? cfg.mulLatency :
            cfg.addLatency;
    }

    static uint64_t
    peMask(uint8_t rows, uint8_t cols)
    {
        uint64_t mask = 0;
        for (unsigned r = 0; r < Width; ++r)
            if (rows & (1u << r))
                mask |= uint64_t(cols) << (r * Width);
        return mask;
    }

    void
    reserveWriteback(uint64_t ready, uint8_t rows, uint8_t cols, Phase phase)
    {
        if (cfg.mixedKind)
            writebacksDue[ready][writebackPriority(phase)] |= peMask(rows, cols);
    }

    bool
    peWriteAvailable(uint8_t rows, uint8_t cols) const
    {
        for (unsigned r = 0; r < Width; ++r)
            if ((rows & (1u << r)) && (peWriteUsed[r] & cols))
                return false;
        return true;
    }

    void
    noteReturnBlocked(Phase phase)
    {
        blockedReturnNow = true;
        if (!returnBlockedByPhase[phase]) {
            ++stats[phase].returnBlockedCycles;
            returnBlockedByPhase[phase] = true;
        }
    }

    void
    writeBypass(unsigned tile, const Row &values, uint8_t mask, Phase phase)
    {
        auto &latch = latches[tile];
        for (unsigned c = 0; c < Width; ++c)
            if (mask & (1u << c))
                latch.data[c] = values[c];
        latch.valid |= mask;
        latch.busy &= ~mask;
        ++stats[phase].bypassWrites;
    }

    void
    retireRows(unsigned priority)
    {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->ready > cycle ||
                (cfg.mixedKind && writebackPriority(it->phase) != priority)) {
                ++it;
                continue;
            }
            if (it->isBypass) {
                writeBypass(it->target.tile, it->result, it->mask, it->phase);
            } else if (is2d()) {
                const auto p = it->target;
                if (peWriteUsed[p.row] & it->mask) {
                    ++stats[it->phase].returnWait;
                    noteReturnBlocked(it->phase);
                    reserveWriteback(cycle + 1, uint8_t(1u << p.row), it->mask, it->phase);
                    ++it;
                    continue;
                }
                auto &tile = tiles[p.tile];
                for (unsigned j = 0; j < Width; ++j)
                    if (it->mask & (1u << j))
                        tile.data[p.row][j] = it->result[j];
                tile.valid[p.row] |= it->mask;
                tile.busy[p.row] &= ~it->mask;
                peWriteUsed[p.row] |= it->mask;
                ++stats[it->phase].zaWrites;
            } else {
                const auto p = it->target;
                if (!write(p, it->mask, it->result, it->phase)) {
                    ++stats[it->phase].returnWait;
                    noteReturnBlocked(it->phase);
                    ++it;
                    continue;
                }
                peWriteUsed[p.row] |= it->mask;
                tiles[p.tile].busy[p.row] &= ~it->mask;
            }
            stats[it->phase].returned[it->kind] += words(it->mask);
            it = pending.erase(it);
        }
    }

    void
    retireSegment(const Return2d &ret, Return2dSub &sub)
    {
        if (sub.isBypass) {
            writeBypass(sub.dstTile, ret.data[sub.dstRow], sub.colMask, sub.phase);
        } else {
            auto &tile = tiles[sub.dstTile];
            for (unsigned r = 0; r < Width; ++r) {
                if (!(sub.rowMask & (1u << r)))
                    continue;
                for (unsigned c = 0; c < Width; ++c)
                    if (sub.colMask & (1u << c))
                        tile.data[r][c] = ret.data[r][c];
                tile.valid[r] |= sub.colMask;
                tile.busy[r] &= ~sub.colMask;
                peWriteUsed[r] |= sub.colMask;
            }
            ++stats[sub.phase].zaWrites;
        }
        stats[sub.phase].returned[sub.kind] += sub.activePes;
        sub.retired = true;
    }

    bool
    stall(Phase phase, Stall reason)
    {
        stats[phase].stalls[reason]++;
        return false;
    }

    bool
    write(Position p, uint8_t mask, const Row &values, Phase phase)
    {
        auto &tile = tiles[p.tile];
        if (!writes || tile.written[p.row])
            return false;
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                tile.data[p.row][j] = values[j];
        tile.valid[p.row] |= mask;
        tile.written[p.row] = true;
        --writes;
        stats[phase].zaWrites++;
        return true;
    }

    bool
    readOperands(std::initializer_list<Operand> operands, Phase phase)
    {
        // The same ZA row may broadcast to multiple lanes or fragments.
        std::array<Position, 3> fresh{};
        unsigned count = 0;
        for (const auto &operand : operands) {
            const auto p = operand.p;
            if (tiles[p.tile].read[p.row])
                continue;
            bool seen = false;
            for (unsigned i = 0; i < count; ++i)
                seen |= fresh[i].tile == p.tile && fresh[i].row == p.row;
            if (!seen)
                fresh[count++] = p;
        }
        if (count > reads)
            return stall(phase, ReadPort);
        for (unsigned i = 0; i < count; ++i)
            tiles[fresh[i].tile].read[fresh[i].row] = true;
        reads -= count;
        stats[phase].zaReads += count;
        return true;
    }

    bool
    canIssue(Position dst, uint8_t mask, Kind kind, Phase phase,
             std::initializer_list<Operand> operands = {})
    {
        assert(mask);
        bool dependent = (tiles[dst.tile].busy[dst.row] |
                          tiles[dst.tile].loadBlock[dst.row]) & mask;
        for (const auto &operand : operands)
            dependent |= !ready(operand.p, operand.mask);
        if (dependent) {
            dependencyNow |= kind != Div;
            return stall(phase, Dependency);
        }
        if (kind == Div) {
            if (cycle < nextDiv)
                return stall(phase, Divider);
        } else {
            const unsigned active = words(mask);
            offeredNow += active;
            if (active > lanes)
                return stall(phase, Lanes);
        }
        if (queueFull())
            return stall(phase, Queue);
        return readOperands(operands, phase);
    }

    void
    enqueue(Position dst, uint8_t mask, const Row &result, Kind kind,
            Phase phase, unsigned latency)
    {
        auto &s = stats[phase];
        const unsigned active = words(mask);
        s.issued[kind] += active;
        if (kind != Div) {
            lanes -= active;
            issuedNow[phase] += active;
            if (!s.beats)
                s.first = cycle;
            if (!s.beats || s.last != cycle)
                s.issueCycles++;
            s.last = cycle;
            s.beats++;
        }
        tiles[dst.tile].busy[dst.row] |= mask;
        const uint64_t ready = cycle + latency;
        reserveWriteback(ready, uint8_t(1u << dst.row), mask, phase);
        pending.push_back({ready, dst, result, mask, kind, phase, false});
        if (pendingReturns() > peak)
            peak = pendingReturns();
    }

    void
    enqueueBypass(unsigned id, uint8_t mask, const Row &result, Kind kind,
                  Phase phase, unsigned latency)
    {
        auto &s = stats[phase];
        const unsigned active = words(mask);
        s.issued[kind] += active;
        lanes -= active;
        issuedNow[phase] += active;
        if (!s.beats)
            s.first = cycle;
        if (!s.beats || s.last != cycle)
            s.issueCycles++;
        s.last = cycle;
        s.beats++;
        latches[id].busy |= mask;
        pending.push_back({cycle + latency, {id, 0}, result, mask, kind, phase, true});
        if (pendingReturns() > peak)
            peak = pendingReturns();
    }

    void
    issueScale(Position dst, uint8_t mask, double factor, Phase phase)
    {
        issueScale(dst, mask, row(dst), factor, phase);
    }

    void
    issueScale(Position dst, uint8_t mask, const Row &values, double factor,
               Phase phase)
    {
        Row result{};
        for (unsigned j = 0; j < Width; ++j)
            if (mask & (1u << j))
                result[j] = values[j] * factor;
        enqueue(dst, mask, result, Mul, phase, cfg.mulLatency);
    }
};

} // namespace gem5::ArmISA

#endif // __ARCH_ARM_CFD_SME_ZA_HH__
