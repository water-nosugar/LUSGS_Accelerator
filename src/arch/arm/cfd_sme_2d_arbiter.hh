#ifndef __ARCH_ARM_CFD_SME_2D_ARBITER_HH__
#define __ARCH_ARM_CFD_SME_2D_ARBITER_HH__

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

#include "arch/arm/cfd_sme_za.hh"

namespace gem5::ArmISA
{

struct Sme2dCandidateMeta
{
    CfdSmeZa::Segment2d seg{};
    unsigned priority = 0;
    unsigned critGroup = 0;
    unsigned activePes = 0;
    unsigned orderIndex = 0;
};

struct Sme2dBundleChoice
{
    unsigned chosenCount = 0;
    std::array<unsigned, 4> chosen{};
};

inline Sme2dBundleChoice
selectSme2dBundle(CfdSmeZa &smeZa, const Sme2dCandidateMeta *offers,
                  unsigned offerCount, unsigned candidateWindow)
{
    Sme2dBundleChoice result{};
    if (!smeZa.is2d() || !offerCount)
        return result;

    const unsigned end = candidateWindow ?
        std::min<unsigned>(offerCount, candidateWindow) : offerCount;
    unsigned totalOfferedPes = 0;
    for (unsigned i = 0; i < end; ++i)
        totalOfferedPes += offers[i].activePes;
    smeZa.noteOffered2d(totalOfferedPes);

    if (smeZa.queueFull()) {
        auto best = std::min_element(offers, offers + end,
            [](const Sme2dCandidateMeta &a, const Sme2dCandidateMeta &b) {
                return std::tie(a.critGroup, a.priority, a.orderIndex) <
                       std::tie(b.critGroup, b.priority, b.orderIndex);
            });
        smeZa.noteStall2d(best->seg.phase, CfdSmeZa::Queue);
        return result;
    }

    const bool mixedKind = smeZa.config().mixedKind;
    struct Operands
    {
        CfdSmeZa::SegmentReads slices{};
        unsigned count = 0;
        bool writebackConflict = false;
    };
    std::vector<Operands> operands(end);
    for (unsigned i = 0; i < end; ++i) {
        auto &ops = operands[i];
        const auto &seg = offers[i].seg;
        CfdSmeZa::SegmentReads slices{};
        const unsigned count = smeZa.collectReads2d(seg, slices);
        for (unsigned r = 0; r < count; ++r)
            if (!smeZa.readSliceUsed(slices[r]))
                ops.slices[ops.count++] = slices[r];
        ops.writebackConflict = smeZa.willCollideWriteback2d(seg);
    }

    auto anchorBefore = [&](unsigned a, unsigned b) {
        const auto &oa = offers[a];
        const auto &ob = offers[b];
        if (mixedKind) {
            return std::make_tuple(oa.critGroup, oa.priority,
                                   operands[a].writebackConflict,
                                   -int(oa.activePes), oa.orderIndex) <
                   std::make_tuple(ob.critGroup, ob.priority,
                                   operands[b].writebackConflict,
                                   -int(ob.activePes), ob.orderIndex);
        }
        return std::tie(oa.priority, oa.orderIndex) <
               std::tie(ob.priority, ob.orderIndex);
    };

    const unsigned availLanes = smeZa.freeLanes();
    const unsigned availReads = smeZa.freeReads();
    const unsigned availIngress = smeZa.freeIngress();
    unsigned anchorIdx = end;
    unsigned blockedIdx = end;
    for (unsigned i = 0; i < end; ++i) {
        const auto &o = offers[i];
        const bool fitsLanes = o.activePes <= availLanes;
        const bool fitsIngress = availIngress ||
            !CfdSmeZa::opUsesIngress(o.seg.op);
        auto &best = fitsLanes && fitsIngress &&
            operands[i].count <= availReads ? anchorIdx : blockedIdx;
        if (best == end || anchorBefore(i, best))
            best = i;
    }
    if (anchorIdx == end) {
        const auto &blockedOffer = offers[blockedIdx];
        const auto &blocked = blockedOffer.seg;
        const auto stallReason = blockedOffer.activePes > availLanes ?
            CfdSmeZa::Lanes :
            CfdSmeZa::opUsesIngress(blocked.op) && !availIngress ?
            CfdSmeZa::Ingress : CfdSmeZa::ReadPort;
        smeZa.noteStall2d(blocked.phase, stallReason);
        return result;
    }

    std::array<CfdSmeZa::ReadSlice, 16> pickedReads{};
    unsigned pickedReadCount = 0;
    auto newReads = [&](unsigned i, bool claim) {
        const auto &ops = operands[i];
        unsigned count = 0;
        for (unsigned r = 0; r < ops.count; ++r) {
            bool seen = false;
            for (unsigned p = 0; p < pickedReadCount; ++p)
                seen |= pickedReads[p] == ops.slices[r];
            if (!seen) {
                if (claim)
                    pickedReads[pickedReadCount++] = ops.slices[r];
                ++count;
            }
        }
        return count;
    };

    std::vector<bool> usedOffer(end, false);
    result.chosen[result.chosenCount++] = anchorIdx;
    usedOffer[anchorIdx] = true;
    const auto &anchor = offers[anchorIdx];
    const auto kind = CfdSmeZa::opKind(anchor.seg.op);
    uint8_t usedRows = anchor.seg.rowMask;
    uint8_t usedCols = anchor.seg.colMask;
    unsigned usedLanes = anchor.activePes;
    unsigned usedIngress = CfdSmeZa::opUsesIngress(anchor.seg.op) ? 1u : 0u;
    newReads(anchorIdx, true);

    const auto mode = smeZa.config().arrayMode;
    const unsigned maxSeg = smeZa.config().maxSegments;
    auto fillerKey = [&](unsigned i) {
        const auto &o = offers[i];
        return std::make_tuple(o.critGroup, operands[i].writebackConflict,
                               -int(o.activePes), o.priority, o.orderIndex);
    };
    if (mode != CfdSmeZa::ArrayMode::TwoDBase && maxSeg > 1) {
        while (result.chosenCount < maxSeg && usedRows != 0xFFu) {
            unsigned bestCand = end;
            bool anyRowBlocked = false;
            bool anyColBlocked = false;
            bool anyReadBlocked = false;
            bool anyIngressBlocked = false;

            for (unsigned idx = 0; idx < end; ++idx) {
                if (usedOffer[idx])
                    continue;
                const auto &c = offers[idx];
                if (!mixedKind && CfdSmeZa::opKind(c.seg.op) != kind)
                    continue;
                if (CfdSmeZa::opIsBypass(c.seg.op)) {
                    bool dupLatch = false;
                    for (unsigned k = 0; k < result.chosenCount; ++k) {
                        const auto &prev = offers[result.chosen[k]].seg;
                        if (CfdSmeZa::opIsBypass(prev.op) &&
                            prev.dstTile == c.seg.dstTile) {
                            dupLatch = true;
                            break;
                        }
                    }
                    if (dupLatch)
                        continue;
                }
                if (c.seg.rowMask & usedRows) {
                    anyRowBlocked = true;
                    continue;
                }
                if (mode == CfdSmeZa::ArrayMode::TwoDBd &&
                    (c.seg.colMask & usedCols)) {
                    anyColBlocked = true;
                    continue;
                }
                if (usedLanes + c.activePes > availLanes)
                    continue;
                const unsigned needIng =
                    CfdSmeZa::opUsesIngress(c.seg.op) ? 1u : 0u;
                const unsigned needRd = newReads(idx, false);
                if (usedIngress + needIng > availIngress ||
                    pickedReadCount + needRd > availReads) {
                    anyIngressBlocked |= usedIngress + needIng > availIngress;
                    anyReadBlocked |= pickedReadCount + needRd > availReads;
                    continue;
                }
                if (bestCand == end || fillerKey(idx) < fillerKey(bestCand))
                    bestCand = idx;
            }
            if (bestCand == end) {
                if (anyReadBlocked)
                    smeZa.notePackStallReadPort();
                else if (anyIngressBlocked)
                    smeZa.notePackStallIngress();
                else if (anyColBlocked)
                    smeZa.notePackStallCol();
                else if (anyRowBlocked)
                    smeZa.notePackStallRow();
                break;
            }
            const auto &picked = offers[bestCand];
            result.chosen[result.chosenCount++] = bestCand;
            usedOffer[bestCand] = true;
            usedRows |= picked.seg.rowMask;
            usedCols |= picked.seg.colMask;
            usedLanes += picked.activePes;
            if (CfdSmeZa::opUsesIngress(picked.seg.op))
                ++usedIngress;
            newReads(bestCand, true);
        }
    }
    return result;
}

} // namespace gem5::ArmISA

#endif // __ARCH_ARM_CFD_SME_2D_ARBITER_HH__
