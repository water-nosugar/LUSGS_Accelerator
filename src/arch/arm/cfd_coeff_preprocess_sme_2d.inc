// 2D 8x8 outer-product SME microprograms and complementary-phase bundle
// scheduler (2d-base / 2d-bd BD-FMOPA / 2d-rs RS-FMOPA).
constexpr std::array<uint8_t, 8> Sme2dOffsetTable = {0, 4, 3, 7, 1, 5, 2, 6};

static inline uint8_t
sme2dRotl8(uint8_t mask, unsigned off)
{
    off &= 7u;
    return static_cast<uint8_t>(((mask << off) | (mask >> (8u - off))) & 0xFFu);
}

static inline unsigned
sme2dRowOff(CfdSmeZa::ArrayMode mode, unsigned slotIdx)
{
    return mode == CfdSmeZa::ArrayMode::TwoDBase ? 0u : Sme2dOffsetTable[slotIdx & 7u];
}

static inline unsigned
sme2dColOff(CfdSmeZa::ArrayMode mode, unsigned slotIdx)
{
    return mode == CfdSmeZa::ArrayMode::TwoDBd ? Sme2dOffsetTable[slotIdx & 7u] : 0u;
}

static inline CfdSmeZa::Row
sme2dRotRow(const CfdSmeZa::Row &src, uint8_t logicalMask, unsigned colOff)
{
    if (!colOff)
        return src;
    CfdSmeZa::Row dst{};
    for (unsigned c = 0; c < CfdSmeZa::Width; ++c)
        if (logicalMask & (1u << c))
            dst[(c + colOff) & 7u] = src[c];
    return dst;
}

static inline CfdSmeZa::Row
sme2dUnrotRow(const CfdSmeZa::Row &src, uint8_t logicalMask, unsigned colOff)
{
    if (!colOff)
        return src;
    CfdSmeZa::Row dst{};
    for (unsigned c = 0; c < CfdSmeZa::Width; ++c)
        if (logicalMask & (1u << c))
            dst[c] = src[(c + colOff) & 7u];
    return dst;
}

static inline CfdSmeZa::Row
sme2dRotRowVert(const CfdSmeZa::Row &src, uint8_t logicalRowMask, unsigned rowOff)
{
    if (!rowOff)
        return src;
    CfdSmeZa::Row dst{};
    for (unsigned r = 0; r < CfdSmeZa::Width; ++r)
        if (logicalRowMask & (1u << r))
            dst[(r + rowOff) & 7u] = src[r];
    return dst;
}

bool
CfdCoeffPreprocessController::offerSme2d(
    const CfdSmeZa::Segment2d &seg, unsigned priority, unsigned critGroup,
    std::function<void()> commit)
{
    if (!smeZa.ready2d(seg)) {
        smeZa.noteDependency2d(seg.phase);
        return false;
    }
    Sme2dOffer offer;
    offer.seg = seg;
    offer.priority = priority;
    offer.critGroup = critGroup;
    offer.activePes = CfdSmeZa::activePes2d(seg);
    offer.orderIndex = sme2dOffers.size();
    offer.commit = std::move(commit);
    sme2dOffers.push_back(std::move(offer));
    return true;
}

void
CfdCoeffPreprocessController::advanceSmeLu2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned tile, uint64_t now,
    bool &offered)
{
    using Sme = CfdSmeZa;
    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, idx);
    const unsigned colOff = sme2dColOff(mode, idx);
    const uint8_t pVecRowMask = sme2dRotl8(SmeVectorMask, rowOff);
    const uint8_t pVecColMask = sme2dRotl8(SmeVectorMask, colOff);

    if (req.coefficientCacheReuse && req.smeInvUseCache) {
        while (cell.loaded < N) {
            Sme::Row value{};
            for (unsigned r = 0; r < N; ++r)
                value[r] = req.dInv[r * N + cell.loaded];
            const unsigned pr = (cell.loaded + rowOff) & 7u;
            if (!smeZa.load({tile, pr}, pVecColMask,
                            sme2dRotRow(value, SmeVectorMask, colOff), Sme::Lu))
                return;
            ++cell.loaded;
        }
        return;
    }
    const auto &input = req.coefficientCacheReuse ? req.lu : req.d;
    const Sme::Position reciprocals{tile, (N + rowOff) & 7u};
    if (req.coefficientCacheReuse && env.smeWarmVerticalRhs &&
        (env.smeWarmSingleTile || env.smeWarmBypass)) {
        panic_if(!req.cachedReciprocal, "CFD SME warm LU requires reciprocals");
        if (!cell.reciprocalsLoaded) {
            Sme::Row value{};
            std::copy(req.reciprocal.begin(), req.reciprocal.end(), value.begin());
            cell.reciprocalsLoaded = smeZa.load(
                reciprocals, pVecColMask,
                sme2dRotRow(value, SmeVectorMask, colOff), Sme::Lu);
        }
        while (cell.loaded < N) {
            Sme::Row value{};
            std::copy_n(input.begin() + cell.loaded * N, N, value.begin());
            const unsigned pc = (cell.loaded + colOff) & 7u;
            if (!smeZa.loadVertical(tile, pc, pVecRowMask,
                                    sme2dRotRowVert(value, SmeVectorMask, rowOff),
                                    Sme::Lu))
                return;
            ++cell.loaded;
        }
        return;
    }
    while (cell.loaded < N) {
        Sme::Row value{};
        std::copy_n(input.begin() + cell.loaded * N, N, value.begin());
        const bool loaded = (env.smeWarmVerticalRhs && req.coefficientCacheReuse) ?
            smeZa.loadVertical(tile, (cell.loaded + colOff) & 7u, pVecRowMask,
                               sme2dRotRowVert(value, SmeVectorMask, rowOff),
                               Sme::Lu) :
            smeZa.load({tile, (cell.loaded + rowOff) & 7u}, pVecColMask,
                       sme2dRotRow(value, SmeVectorMask, colOff), Sme::Lu);
        if (!loaded)
            return;
        if (!req.coefficientCacheReuse && env.smeWarmBaseMode != SmeWarmBaseMode::DirectLu &&
            env.smeInvExpThresh < 2048)
            cell.dMagnitude.includeRow(value.data());
        ++cell.loaded;
    }
    if (req.coefficientCacheReuse) {
        panic_if(!req.cachedReciprocal, "CFD SME warm LU requires reciprocals");
        if (!cell.reciprocalsLoaded) {
            Sme::Row value{};
            std::copy(req.reciprocal.begin(), req.reciprocal.end(), value.begin());
            cell.reciprocalsLoaded = smeZa.load(
                reciprocals, pVecColMask,
                sme2dRotRow(value, SmeVectorMask, colOff), Sme::Lu);
        }
        return;
    }
    if (req.luDone)
        return;

    for (unsigned k = 0; k < N; ++k) {
        const unsigned pkCol = (k + colOff) & 7u;
        if (req.forwardStepReady[k] || !smeZa.ready(reciprocals, 1u << pkCol))
            continue;
        req.forwardStepReady[k] = true;
        req.record.luStepReadyCycle[k] = now;
        if (!req.record.luFirstResultCycle)
            req.record.luFirstResultCycle = now;
        trace(req, "lu", "LU_STEP_READY", -1, -1, k, -1, "forward");
    }

    if (cell.pivot < N) {
        const unsigned k = cell.pivot;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        const Sme::Position diagonal{tile, pkRow};
        const uint8_t tail = SmeVectorMask & ~((2u << k) - 1);
        if (!cell.divided) {
            Sme::Row value;
            if (!smeZa.read(diagonal, static_cast<uint8_t>(1u << pkCol), value, Sme::Lu))
                return;
            const double pivot = value[pkCol];
            if (!std::isfinite(pivot) || std::fabs(pivot) <= req.desc.pivotEpsilon) {
                markFailed(req, CfdCoeffPreprocessStatus::LuFailure, "pivot");
                return;
            }
            if (!smeZa.reciprocal(reciprocals, pkCol, pivot, Sme::Lu))
                return;
            uint64_t pbits = 0;
            std::memcpy(&pbits, &pivot, sizeof(pivot));
            const uint16_t pexp = static_cast<uint16_t>((pbits >> 52) & 0x7FFu);
            cell.pivotExpMin = std::min(cell.pivotExpMin, pexp);
            cell.pivotExpMax = std::max(cell.pivotExpMax, pexp);
            req.record.luDivIssued++;
            cell.divided = true;
            cell.row = k + 1;
            if (!tail) {
                ++cell.pivot;
                cell.divided = cell.scaled = false;
            }
        } else if (tail && !cell.scaled) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::ScaleRow;
                seg.phase = Sme::Lu;
                seg.rowMask = static_cast<uint8_t>(1u << pkRow);
                seg.colMask = sme2dRotl8(tail, colOff);
                seg.dstTile = tile;
                seg.dstRow = pkRow;
                seg.aPos = reciprocals;
                seg.aCol = pkCol;
                const uint64_t tok = req.token;
                if (offerSme2d(seg, 3, 1, [this, tok, idx, k]() {
                    if (auto *r = find(tok))
                        r->record.luMulIssued += N - k - 1;
                    smeCells[idx].scaled = true;
                }))
                    offered = true;
            }
        } else if (tail && cell.scaled) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::Fmopa;
                seg.phase = Sme::Lu;
                seg.rowMask = sme2dRotl8(tail, rowOff);
                seg.colMask = sme2dRotl8(tail, colOff);
                seg.dstTile = tile;
                seg.aPos = {tile, 0};
                seg.aCol = pkCol;
                seg.bPos = diagonal;
                const uint64_t tok = req.token;
                const unsigned ops = (N - k - 1) * (N - k - 1);
                if (offerSme2d(seg, 3, 1, [this, tok, idx, ops]() {
                    if (auto *r = find(tok)) {
                        r->record.luMulIssued += ops;
                        r->record.luSubIssued += ops;
                    }
                    auto &c = smeCells[idx];
                    ++c.pivot;
                    c.divided = c.scaled = false;
                }))
                    offered = true;
            }
        }
    }

    if (cell.pivot < N || !smeZa.ready(reciprocals, pVecColMask))
        return;
    Sme::Row value;
    while (cell.exported < N) {
        const unsigned pr = (cell.exported + rowOff) & 7u;
        if (!smeZa.read({tile, pr}, pVecColMask, value, Sme::Lu))
            return;
        const auto unrot = sme2dUnrotRow(value, SmeVectorMask, colOff);
        std::copy_n(unrot.begin(), N, req.lu.begin() + cell.exported * N);
        ++cell.exported;
    }
    if (!smeZa.read(reciprocals, pVecColMask, value, Sme::Lu))
        return;
    const auto unrotRec = sme2dUnrotRow(value, SmeVectorMask, colOff);
    std::copy_n(unrotRec.begin(), N, req.reciprocal.begin());
    req.reciprocalReady.fill(true);
    req.backwardStepReady.fill(true);
    req.luDone = true;
    req.record.luCompleteCycle = now;
    const uint16_t expSpread = cell.pivotExpMax >= cell.pivotExpMin ?
        (cell.pivotExpMax - cell.pivotExpMin) : 0;
    if (smeOwner) {
        auto lineIt = lineRequests.find(smeOwner);
        if (lineIt != lineRequests.end() && lineIt->second->smeInversePolicy) {
            if (expSpread <= env.smeInvExpThresh) {
                req.smeInvRequested = true;
            } else {
                req.smeInvRejected = true;
                lineIt->second->smeInvFallbackCells++;
                trace(req, "lu", "SME_INV_GUARD_FALLBACK", -1, -1, expSpread);
            }
        }
    }
    req.stage = req.solveStarted ? RequestStage::SolveActive : RequestStage::LuReady;
    trace(req, "lu", "LU_COMPLETE");
    tracePrivateLu(req);
}

void
CfdCoeffPreprocessController::advanceSmePanel2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned panelSlot,
    unsigned luTile, unsigned panel, uint8_t mask, uint64_t now, bool &offered)
{
    using Sme = CfdSmeZa;
    auto &state = cell.panels[panelSlot];
    const unsigned tile = luTile + panel + 1;
    const bool defer = panel && req.smeDeferUpper;
    if (!mask || state.phase == SmePanel::Done)
        return;

    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, idx);
    const unsigned colOff = sme2dColOff(mode, idx);
    const uint8_t pMask = sme2dRotl8(mask, colOff);

    if (state.phase == SmePanel::Load) {
        const bool panelReady = panel ? req.inputUpperReady :
            (!(mask & 1u) || req.rhsVectorReady) &&
            (!(mask & ~1u) || req.inputLowerReady);
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && panelReady))
            return;
        while (state.row < N) {
            Sme::Row value{};
            if (panel) {
                std::copy_n(req.upper.begin() + state.row * N, N, value.begin());
            } else {
                if (mask & 1u)
                    value[0] = req.rhsVector[state.row];
                if (mask & ~1u)
                    std::copy_n(req.lower.begin() + state.row * N, N,
                                value.begin() + 1);
            }
            const unsigned pr = (state.row + rowOff) & 7u;
            if (!smeZa.load({tile, pr}, pMask,
                            sme2dRotRow(value, mask, colOff), Sme::Rhs))
                return;
            if (!state.row)
                trace(req, "solve", "SME_RHS_PANEL_LOAD", panel, mask);
            ++state.row;
        }
        if (!req.solveStarted) {
            req.solveStarted = true;
            req.record.solveIssueCycle = now;
            if (req.luDone)
                req.stage = RequestStage::SolveActive;
            trace(req, "solve", "COEFF3_ISSUE");
        }
        state.phase = SmePanel::Forward;
        state.step = 0;
        state.row = 1;
        state.scaled = false;
    }

    const unsigned forwardEnd = defer ? env.smeUpperSplit : N;
    const unsigned cols = __builtin_popcount(mask);
    const unsigned priority = panel ? 6u : 4u;
    const uint64_t tok = req.token;

    if (state.phase == SmePanel::Forward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        const Sme::Position reciprocals{luTile, (N + rowOff) & 7u};
        if (!state.scaled) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::ScaleRow;
                seg.phase = Sme::Rhs;
                seg.rowMask = static_cast<uint8_t>(1u << pkRow);
                seg.colMask = pMask;
                seg.dstTile = tile;
                seg.dstRow = pkRow;
                seg.aPos = reciprocals;
                seg.aCol = pkCol;
                if (offerSme2d(seg, priority, 1, [this, tok, idx, panelSlot, k, cols, defer]() {
                    if (auto *r = find(tok))
                        r->record.coeffMulIssued += cols;
                    auto &st = smeCells[idx].panels[panelSlot];
                    st.scaled = true;
                    if (k + 1 == N) {
                        st.phase = defer ? SmePanel::Export : SmePanel::Backward;
                        st.row = 0;
                    }
                }))
                    offered = true;
            }
            return;
        }
        if (k + 1 < N) {
            if (!offered) {
                const uint8_t logicalRows = SmeVectorMask & ~((2u << k) - 1);
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::Fmopa;
                seg.phase = Sme::Rhs;
                seg.rowMask = sme2dRotl8(logicalRows, rowOff);
                seg.colMask = pMask;
                seg.dstTile = tile;
                seg.aPos = {luTile, 0};
                seg.aCol = pkCol;
                seg.bPos = {tile, pkRow};
                const unsigned ops = (N - k - 1) * cols;
                if (offerSme2d(seg, priority, 1, [this, tok, idx, panelSlot, k, ops, forwardEnd, defer]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += ops;
                        r->record.coeffSubIssued += ops;
                    }
                    auto &st = smeCells[idx].panels[panelSlot];
                    if (k + 1 == forwardEnd) {
                        st.phase = defer ? SmePanel::Export : SmePanel::Backward;
                        st.row = 0;
                    } else {
                        ++st.step;
                        st.row = st.step + 1;
                        st.scaled = false;
                    }
                }))
                    offered = true;
            }
            return;
        }
    }

    if (state.phase == SmePanel::Backward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        if (!offered) {
            const uint8_t logicalRows = static_cast<uint8_t>((1u << k) - 1u);
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::Fmopa;
            seg.phase = Sme::Rhs;
            seg.rowMask = sme2dRotl8(logicalRows, rowOff);
            seg.colMask = pMask;
            seg.dstTile = tile;
            seg.aPos = {luTile, 0};
            seg.aCol = pkCol;
            seg.bPos = {tile, pkRow};
            const unsigned ops = k * cols;
            if (offerSme2d(seg, priority, 1, [this, tok, idx, panelSlot, ops]() {
                if (auto *r = find(tok)) {
                    r->record.coeffMulIssued += ops;
                    r->record.coeffSubIssued += ops;
                }
                auto &st = smeCells[idx].panels[panelSlot];
                st.row = 0;
                if (--st.step == 0)
                    st.phase = SmePanel::Export;
            }))
                offered = true;
        }
        return;
    }

    if (state.phase != SmePanel::Export)
        return;

    while (state.row < N) {
        Sme::Row value;
        const unsigned pr = (state.row + rowOff) & 7u;
        if (!smeZa.read({tile, pr}, pMask, value, Sme::Rhs))
            return;
        const auto unrot = sme2dUnrotRow(value, mask, colOff);
        if (defer) {
            if (!std::all_of(unrot.begin(), unrot.begin() + N,
                             [](double x) { return std::isfinite(x); })) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "nonfinite-upper-panel");
                return;
            }
            std::copy_n(unrot.begin(), N, req.uBar.begin() + state.row * N);
        } else {
            for (unsigned j = 0; j < Sme::Width; ++j)
                if (mask & (1u << j))
                    req.rhs[panel ? 10 + j : (j ? 4 + j : 0)].value[state.row] = unrot[j];
        }
        ++state.row;
    }
    state.phase = SmePanel::Done;
    if (defer) {
        req.smeUpperPartial = true;
        trace(req, "solve", "SME_UPPER_PARTIAL", panel);
        finishCoeffRhs(req, now);
        return;
    }
    for (unsigned j = 0; j < Sme::Width && req.stage != RequestStage::Failed; ++j)
        if (mask & (1u << j))
            rhsDone(req, panel ? 10 + j : (j ? 4 + j : 0), now);
}

// Option B (panel-pack=1): 8-column full-width Panel A [R(1) | L(5) | U0..1(2)]
// on tile+1 for interior cold cells (2d-rs, colOff == 0).
void
CfdCoeffPreprocessController::advanceSmePackedPanelA2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned luTile, uint64_t now,
    bool &offered)
{
    using Sme = CfdSmeZa;
    auto &state = cell.panels[0];
    if (state.phase == SmePanel::Done)
        return;

    const unsigned tile = luTile + 1;
    const bool defer = req.smeDeferUpper;
    const unsigned upperEnd = defer ? env.smeUpperSplit : N;
    const unsigned rowOff = sme2dRowOff(env.smeConfig.arrayMode, idx);
    constexpr uint8_t FullMask = 0xFFu;
    constexpr uint8_t BaseMask = 0x3Fu; // [R | L0..4]

    if (state.phase == SmePanel::Load) {
        const bool panelReady = req.rhsVectorReady && req.inputLowerReady &&
            req.inputUpperReady;
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && panelReady))
            return;
        while (state.row < N) {
            Sme::Row value{};
            value[0] = req.rhsVector[state.row];
            std::copy_n(req.lower.begin() + state.row * N, N, value.begin() + 1);
            value[6] = req.upper[state.row * N + 0];
            value[7] = req.upper[state.row * N + 1];
            const unsigned pr = (state.row + rowOff) & 7u;
            if (!smeZa.load({tile, pr}, FullMask, value, Sme::Rhs))
                return;
            if (!state.row)
                trace(req, "solve", "SME_RHS_PANEL_LOAD", 0, FullMask,
                      -1, -1, "packed-a");
            ++state.row;
        }
        if (!req.solveStarted) {
            req.solveStarted = true;
            req.record.solveIssueCycle = now;
            if (req.luDone)
                req.stage = RequestStage::SolveActive;
            trace(req, "solve", "COEFF3_ISSUE");
        }
        state.phase = SmePanel::Forward;
        state.step = 0;
        state.row = 1;
        state.scaled = false;
    }

    const uint64_t tok = req.token;
    if (state.phase == SmePanel::Forward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const uint8_t stepMask = k < upperEnd ? FullMask : BaseMask;
        const unsigned cols = __builtin_popcount(stepMask);
        if (!state.scaled) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::ScaleRow;
                seg.phase = Sme::Rhs;
                seg.rowMask = static_cast<uint8_t>(1u << pkRow);
                seg.colMask = stepMask;
                seg.dstTile = tile;
                seg.dstRow = pkRow;
                seg.aPos = {luTile, (N + rowOff) & 7u};
                seg.aCol = k;
                if (offerSme2d(seg, 4, 1, [this, tok, idx, k, cols]() {
                    if (auto *r = find(tok))
                        r->record.coeffMulIssued += cols;
                    auto &st = smeCells[idx].panels[0];
                    st.scaled = true;
                    if (k + 1 == N) {
                        st.phase = SmePanel::Backward;
                        st.row = 0;
                    }
                }))
                    offered = true;
            }
            return;
        }
        if (k + 1 < N) {
            if (!offered) {
                const uint8_t logicalRows = SmeVectorMask & ~((2u << k) - 1);
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::Fmopa;
                seg.phase = Sme::Rhs;
                seg.rowMask = sme2dRotl8(logicalRows, rowOff);
                seg.colMask = stepMask;
                seg.dstTile = tile;
                seg.aPos = {luTile, 0};
                seg.aCol = k;
                seg.bPos = {tile, pkRow};
                const unsigned ops = (N - k - 1) * cols;
                if (offerSme2d(seg, 4, 1, [this, tok, idx, ops]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += ops;
                        r->record.coeffSubIssued += ops;
                    }
                    auto &st = smeCells[idx].panels[0];
                    ++st.step;
                    st.row = st.step + 1;
                    st.scaled = false;
                }))
                    offered = true;
            }
            return;
        }
    }

    if (state.phase == SmePanel::Backward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const uint8_t bwdMask = defer ? BaseMask : FullMask;
        const unsigned cols = __builtin_popcount(bwdMask);
        if (!offered) {
            const uint8_t logicalRows = static_cast<uint8_t>((1u << k) - 1u);
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::Fmopa;
            seg.phase = Sme::Rhs;
            seg.rowMask = sme2dRotl8(logicalRows, rowOff);
            seg.colMask = bwdMask;
            seg.dstTile = tile;
            seg.aPos = {luTile, 0};
            seg.aCol = k;
            seg.bPos = {tile, pkRow};
            const unsigned ops = k * cols;
            if (offerSme2d(seg, 4, 1, [this, tok, idx, ops]() {
                if (auto *r = find(tok)) {
                    r->record.coeffMulIssued += ops;
                    r->record.coeffSubIssued += ops;
                }
                auto &st = smeCells[idx].panels[0];
                st.row = 0;
                if (--st.step == 0)
                    st.phase = SmePanel::Export;
            }))
                offered = true;
        }
        return;
    }

    if (state.phase != SmePanel::Export)
        return;

    while (state.row < N) {
        Sme::Row value;
        const unsigned pr = (state.row + rowOff) & 7u;
        if (!smeZa.read({tile, pr}, FullMask, value, Sme::Rhs))
            return;
        req.rhs[0].value[state.row] = value[0];
        for (unsigned j = 0; j < N; ++j)
            req.rhs[5 + j].value[state.row] = value[1 + j];
        if (defer) {
            if (!std::isfinite(value[6]) || !std::isfinite(value[7])) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "nonfinite-upper-panel");
                return;
            }
            req.uBar[state.row * N + 0] = value[6];
            req.uBar[state.row * N + 1] = value[7];
        } else {
            req.rhs[10].value[state.row] = value[6];
            req.rhs[11].value[state.row] = value[7];
        }
        ++state.row;
    }
    state.phase = SmePanel::Done;
    cell.panelPackUpperMask |= 1u;
    rhsDone(req, 0, now);
    for (unsigned j = 0; j < N && req.stage != RequestStage::Failed; ++j)
        rhsDone(req, 5 + j, now);
    if (req.stage == RequestStage::Failed)
        return;
    if (!defer) {
        rhsDone(req, 10, now);
        if (req.stage != RequestStage::Failed)
            rhsDone(req, 11, now);
    } else if (cell.panelPackUpperMask == 3u) {
        req.smeUpperPartial = true;
        trace(req, "solve", "SME_UPPER_PARTIAL", 1);
        finishCoeffRhs(req, now);
    }
}

// Option B (panel-pack=1): 8-column full-width Panel B [U2..4(3) | I5(5)]
// on tile+2 for interior cold cells (or 3-column [U2..4] when inverse is off).
void
CfdCoeffPreprocessController::advanceSmePackedPanelB2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned luTile, uint64_t now,
    bool &offered)
{
    using Sme = CfdSmeZa;
    auto &state = cell.panels[1];
    if (state.phase == SmePanel::Done)
        return;

    const unsigned tile = luTile + 2;
    const bool defer = req.smeDeferUpper;
    const unsigned upperEnd = defer ? env.smeUpperSplit : N;
    const unsigned rowOff = sme2dRowOff(env.smeConfig.arrayMode, idx);
    // The caller only packs cold panels when the line requests inverse caching.
    // When panel-B overlap is enabled, seed the unit identity before LU
    // resolves the per-cell guard; otherwise wait for LU completion and preload
    // reciprocals in sparse mode.
    const bool overlapB = env.smePanelBOverlap;
    const bool withInv = overlapB ? (!req.luDone || req.smeInvRequested) : req.smeInvRequested;
    constexpr uint8_t UpperMask = 0x07u; // cols 0..2 = U2..4
    constexpr uint8_t InvMask = 0xF8u;   // cols 3..7 = I5

    if (state.phase == SmePanel::Load) {
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && req.inputUpperReady))
            return;
        if (!overlapB && !req.luDone)
            return;
        const uint8_t loadMask = withInv ? static_cast<uint8_t>(UpperMask | InvMask) : UpperMask;
        while (state.row < N) {
            Sme::Row value{};
            value[0] = req.upper[state.row * N + 2];
            value[1] = req.upper[state.row * N + 3];
            value[2] = req.upper[state.row * N + 4];
            if (withInv)
                value[3 + state.row] = (!overlapB && env.smeInvSparseIdentity) ?
                    req.reciprocal[state.row] : 1.0;
            const unsigned pr = (state.row + rowOff) & 7u;
            if (!smeZa.load({tile, pr}, loadMask, value, Sme::Rhs))
                return;
            if (!state.row)
                trace(req, "solve", "SME_RHS_PANEL_LOAD", 1, loadMask,
                      -1, -1, (overlapB && withInv) ? "packed-b-identity" : "packed-b");
            ++state.row;
        }
        if (!req.solveStarted) {
            req.solveStarted = true;
            req.record.solveIssueCycle = now;
            if (req.luDone)
                req.stage = RequestStage::SolveActive;
            trace(req, "solve", "COEFF3_ISSUE");
        }
        state.phase = SmePanel::Forward;
        state.step = 0;
        state.row = 1;
        state.scaled = false;
    }

    // A rejected inverse can stop here if the deferred upper prefix is done.
    // Any already-issued inverse work still retires through the shared ZA.
    if (defer && !withInv &&
        (state.phase == SmePanel::Backward ||
         (state.phase == SmePanel::Forward && state.step >= upperEnd))) {
        state.phase = SmePanel::Export;
        state.row = 0;
    }
    const uint64_t tok = req.token;
    if (state.phase == SmePanel::Forward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const uint8_t uScale = k < upperEnd ? UpperMask : 0u;
        const uint8_t iScale = !withInv ? 0u :
            (env.smeInvSparseIdentity ?
             static_cast<uint8_t>((overlapB ? ((1u << (k + 1)) - 1u) : ((1u << k) - 1u)) << 3) :
             InvMask);
        const uint8_t scaleMask = static_cast<uint8_t>(uScale | iScale);

        if (!state.scaled) {
            if (!scaleMask) {
                state.scaled = true;
            } else {
                if (!offered) {
                    const unsigned cols = __builtin_popcount(scaleMask);
                    const unsigned invCols = __builtin_popcount(iScale);
                    Sme::Segment2d seg{};
                    seg.op = Sme::Op2d::ScaleRow;
                    seg.phase = Sme::Rhs;
                    seg.rowMask = static_cast<uint8_t>(1u << pkRow);
                    seg.colMask = scaleMask;
                    seg.dstTile = tile;
                    seg.dstRow = pkRow;
                    seg.aPos = {luTile, (N + rowOff) & 7u};
                    seg.aCol = k;
                    if (offerSme2d(seg, 5, 1, [this, tok, idx, k, cols, invCols, defer, withInv]() {
                        if (auto *r = find(tok))
                            r->record.coeffMulIssued += cols;
                        auto &st = smeCells[idx].panels[1];
                        st.inverseMul += invCols;
                        st.scaled = true;
                        if (k + 1 == N) {
                            st.phase = (defer && !withInv) ? SmePanel::Export : SmePanel::Backward;
                            st.row = 0;
                        }
                    }))
                        offered = true;
                }
                return;
            }
        }
        if (k + 1 < N) {
            const uint8_t uUpd = k < upperEnd ? UpperMask : 0u;
            const uint8_t iUpd = !withInv ? 0u :
                (env.smeInvSparseIdentity ?
                 static_cast<uint8_t>(((1u << (k + 1)) - 1u) << 3) : InvMask);
            const uint8_t updMask = static_cast<uint8_t>(uUpd | iUpd);
            if (!offered) {
                const uint8_t logicalRows = SmeVectorMask & ~((2u << k) - 1);
                const unsigned ops = (N - k - 1) * __builtin_popcount(updMask);
                const unsigned invOps = (N - k - 1) * __builtin_popcount(iUpd);
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::Fmopa;
                seg.phase = Sme::Rhs;
                seg.rowMask = sme2dRotl8(logicalRows, rowOff);
                seg.colMask = updMask;
                seg.dstTile = tile;
                seg.aPos = {luTile, 0};
                seg.aCol = k;
                seg.bPos = {tile, pkRow};
                if (offerSme2d(seg, 5, 1, [this, tok, idx, k, ops, invOps, upperEnd, defer, withInv]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += ops;
                        r->record.coeffSubIssued += ops;
                    }
                    auto &st = smeCells[idx].panels[1];
                    st.inverseFma += invOps;
                    if (defer && !withInv && k + 1 == upperEnd) {
                        st.phase = SmePanel::Export;
                        st.row = 0;
                    } else {
                        ++st.step;
                        st.row = st.step + 1;
                        st.scaled = false;
                    }
                }))
                    offered = true;
            }
            return;
        }
    }

    if (state.phase == SmePanel::Backward) {
        // Finish the forward overlap before waiting for the pivot guard.
        if (!req.luDone)
            return;
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const uint8_t bwdMask = static_cast<uint8_t>(
            (defer ? 0u : UpperMask) | (withInv ? InvMask : 0u));
        const unsigned cols = __builtin_popcount(bwdMask);
        if (!offered) {
            const uint8_t logicalRows = static_cast<uint8_t>((1u << k) - 1u);
            const unsigned ops = k * cols;
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::Fmopa;
            seg.phase = Sme::Rhs;
            seg.rowMask = sme2dRotl8(logicalRows, rowOff);
            seg.colMask = bwdMask;
            seg.dstTile = tile;
            seg.aPos = {luTile, 0};
            seg.aCol = k;
            seg.bPos = {tile, pkRow};
            if (offerSme2d(seg, 5, 1, [this, tok, idx, ops]() {
                if (auto *r = find(tok)) {
                    r->record.coeffMulIssued += ops;
                    r->record.coeffSubIssued += ops;
                }
                auto &st = smeCells[idx].panels[1];
                st.row = 0;
                if (--st.step == 0)
                    st.phase = SmePanel::Export;
            }))
                offered = true;
        }
        return;
    }

    if (state.phase != SmePanel::Export)
        return;

    const uint8_t expMask = withInv ? static_cast<uint8_t>(UpperMask | InvMask) : UpperMask;
    while (state.row < N) {
        Sme::Row value;
        const unsigned pr = (state.row + rowOff) & 7u;
        if (!smeZa.read({tile, pr}, expMask, value, Sme::Rhs))
            return;
        if (defer) {
            if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
                !std::isfinite(value[2])) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "nonfinite-upper-panel");
                return;
            }
            req.uBar[state.row * N + 2] = value[0];
            req.uBar[state.row * N + 3] = value[1];
            req.uBar[state.row * N + 4] = value[2];
        } else {
            req.rhs[12].value[state.row] = value[0];
            req.rhs[13].value[state.row] = value[1];
            req.rhs[14].value[state.row] = value[2];
        }
        if (withInv) {
            std::copy_n(value.begin() + 3, N, req.dInv.begin() + state.row * N);
            cell.invMagnitude.includeRow(value.data() + 3);
        }
        ++state.row;
    }
    state.phase = SmePanel::Done;
    cell.panelPackUpperMask |= 2u;
    if (withInv) {
        cell.invPanel.phase = SmePanel::Done;
        req.smeInvDone = true;
        req.smeInvRejected = !smeInverseSafe(cell, env.smeInvExpThresh < 2048);
        if (smeOwner) {
            auto lineIt = lineRequests.find(smeOwner);
            if (lineIt != lineRequests.end()) {
                auto &line = *lineIt->second;
                ++line.smeInvBuiltCells;
                if (overlapB)
                    ++line.smePackedInvCells;
                if (req.smeInvRejected)
                    ++line.smeInvFallbackCells;
                else
                    ++line.smeInvCells;
            }
        }
        if (req.smeInvRejected)
            trace(req, "solve", "SME_INV_CONDITION_FALLBACK");
        trace(req, "solve", "SME_INV_COMPLETE");
    } else {
        auto &line = *lineRequests.at(smeOwner);
        line.smeInvDiscardedMul += state.inverseMul;
        line.smeInvDiscardedFma += state.inverseFma;
    }
    if (!defer) {
        for (unsigned j = 12; j < 15 && req.stage != RequestStage::Failed; ++j)
            rhsDone(req, j, now);
    } else if (cell.panelPackUpperMask == 3u) {
        req.smeUpperPartial = true;
        trace(req, "solve", "SME_UPPER_PARTIAL", 1);
        finishCoeffRhs(req, now);
    }
}

void
CfdCoeffPreprocessController::advanceSmeInversePanel2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned luTile, uint64_t now,
    bool &offered)
{
    using Sme = CfdSmeZa;
    auto &st = cell.invPanel;
    if (!req.smeInvRequested || req.smeInvDone || st.phase == SmePanel::Done)
        return;
    const uint8_t lower = (req.rhsEnabledMask >> 4) & (SmeVectorMask << 1);
    if (!req.luDone || cell.panels[0].phase != SmePanel::Done ||
        (env.smeSplitRhs && lower && cell.panels[2].phase != SmePanel::Done))
        return;

    const unsigned invTile = luTile + 1;
    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, idx);
    const unsigned colOff = sme2dColOff(mode, idx);
    const uint8_t pVecColMask = sme2dRotl8(SmeVectorMask, colOff);

    if (st.phase == SmePanel::Load) {
        if (st.row == 0) {
            if (!smeZa.idle(invTile))
                return;
            smeZa.release(invTile);
        }
        while (st.row < N) {
            Sme::Row value{};
            value[st.row] = env.smeInvSparseIdentity ? req.reciprocal[st.row] : 1.0;
            const unsigned pr = (st.row + rowOff) & 7u;
            if (!smeZa.load({invTile, pr}, pVecColMask,
                            sme2dRotRow(value, SmeVectorMask, colOff), Sme::Rhs))
                return;
            ++st.row;
        }
        st.phase = SmePanel::Forward;
        st.step = 0;
        st.row = 1;
        st.scaled = env.smeInvSparseIdentity;
    }

    const uint64_t tok = req.token;
    if (st.phase == SmePanel::Forward) {
        const unsigned k = st.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        const uint8_t scaleMask = env.smeInvSparseIdentity ?
            static_cast<uint8_t>((1u << k) - 1u) : SmeVectorMask;
        const uint8_t updateMask = env.smeInvSparseIdentity ?
            static_cast<uint8_t>((1u << (k + 1)) - 1u) : SmeVectorMask;
        if (!st.scaled) {
            if (!scaleMask) {
                st.scaled = true;
            } else {
                if (!offered) {
                    Sme::Segment2d seg{};
                    seg.op = Sme::Op2d::ScaleRow;
                    seg.phase = Sme::Rhs;
                    seg.rowMask = static_cast<uint8_t>(1u << pkRow);
                    seg.colMask = sme2dRotl8(scaleMask, colOff);
                    seg.dstTile = invTile;
                    seg.dstRow = pkRow;
                    seg.aPos = {luTile, (N + rowOff) & 7u};
                    seg.aCol = pkCol;
                    const unsigned cols = __builtin_popcount(scaleMask);
                    if (offerSme2d(seg, 7, 2, [this, tok, idx, k, cols]() {
                        if (auto *r = find(tok))
                            r->record.coeffMulIssued += cols;
                        auto &s = smeCells[idx].invPanel;
                        s.scaled = true;
                        if (k + 1 == N) {
                            s.phase = SmePanel::Backward;
                            s.step = N - 1;
                            s.row = 0;
                        }
                    }))
                        offered = true;
                }
                return;
            }
        }
        if (k + 1 < N) {
            if (!offered) {
                const uint8_t logicalRows = SmeVectorMask & ~((2u << k) - 1);
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::Fmopa;
                seg.phase = Sme::Rhs;
                seg.rowMask = sme2dRotl8(logicalRows, rowOff);
                seg.colMask = sme2dRotl8(updateMask, colOff);
                seg.dstTile = invTile;
                seg.aPos = {luTile, 0};
                seg.aCol = pkCol;
                seg.bPos = {invTile, pkRow};
                const unsigned ops = (N - k - 1) * __builtin_popcount(updateMask);
                if (offerSme2d(seg, 7, 2, [this, tok, idx, ops]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += ops;
                        r->record.coeffSubIssued += ops;
                    }
                    auto &s = smeCells[idx].invPanel;
                    ++s.step;
                    s.row = s.step + 1;
                    s.scaled = false;
                }))
                    offered = true;
            }
            return;
        }
    }

    if (st.phase == SmePanel::Backward) {
        const unsigned k = st.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        if (!offered) {
            const uint8_t logicalRows = static_cast<uint8_t>((1u << k) - 1u);
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::Fmopa;
            seg.phase = Sme::Rhs;
            seg.rowMask = sme2dRotl8(logicalRows, rowOff);
            seg.colMask = pVecColMask;
            seg.dstTile = invTile;
            seg.aPos = {luTile, 0};
            seg.aCol = pkCol;
            seg.bPos = {invTile, pkRow};
            const unsigned ops = k * N;
            if (offerSme2d(seg, 7, 2, [this, tok, idx, ops]() {
                if (auto *r = find(tok)) {
                    r->record.coeffMulIssued += ops;
                    r->record.coeffSubIssued += ops;
                }
                auto &s = smeCells[idx].invPanel;
                s.row = 0;
                if (--s.step == 0)
                    s.phase = SmePanel::Export;
            }))
                offered = true;
        }
        return;
    }

    if (st.phase == SmePanel::Export) {
        while (st.row < N) {
            Sme::Row value;
            const unsigned pr = (st.row + rowOff) & 7u;
            if (!smeZa.read({invTile, pr}, pVecColMask, value, Sme::Rhs))
                return;
            const auto unrot = sme2dUnrotRow(value, SmeVectorMask, colOff);
            std::copy_n(unrot.begin(), N, req.dInv.begin() + st.row * N);
            cell.invMagnitude.includeRow(unrot.data());
            ++st.row;
        }
        st.phase = SmePanel::Done;
        req.smeInvDone = true;
        req.smeInvRejected = !smeInverseSafe(cell, env.smeInvExpThresh < 2048);
        if (smeOwner) {
            auto lineIt = lineRequests.find(smeOwner);
            if (lineIt != lineRequests.end()) {
                auto &line = *lineIt->second;
                ++line.smeInvBuiltCells;
                if (req.smeInvRejected)
                    ++line.smeInvFallbackCells;
                else
                    ++line.smeInvCells;
            }
        }
        if (req.smeInvRejected)
            trace(req, "solve", "SME_INV_CONDITION_FALLBACK");
        trace(req, "solve", "SME_INV_COMPLETE");
    }
}

void
CfdCoeffPreprocessController::advanceSmeVerticalRhs2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned luTile,
    uint64_t now, bool singleTilePool, unsigned priority, unsigned critGroup,
    bool &offered)
{
    using Sme = CfdSmeZa;
    auto &state = cell.panels[0];
    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, idx);
    const unsigned colOff = sme2dColOff(mode, idx);
    const uint8_t pVecColMask = sme2dRotl8(SmeVectorMask, colOff);
    const Sme::Position dst = env.smeWarmSingleTile ?
        Sme::Position{luTile, (N + 1 + rowOff) & 7u} :
        Sme::Position{luTile + 1, rowOff};

    if (state.phase == SmePanel::Load) {
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && req.rhsVectorReady))
            return;
        Sme::Row rhs{};
        std::copy_n(req.rhsVector.begin(), N, rhs.begin());
        const auto rotRhs = sme2dRotRow(rhs, SmeVectorMask, colOff);
        const bool loaded = env.smeWarmBypass ?
            smeZa.loadBypass(idx, pVecColMask, rotRhs, Sme::Rhs) :
            smeZa.load(dst, pVecColMask, rotRhs, Sme::Rhs);
        if (!loaded)
            return;
        trace(req, "solve", "SME_RHS_PANEL_LOAD", 0, 1);
        req.solveStarted = true;
        req.record.solveIssueCycle = now;
        req.stage = RequestStage::SolveActive;
        trace(req, "solve", "COEFF3_ISSUE");
        state.phase = SmePanel::Forward;
    }

    const uint64_t tok = req.token;
    if (state.phase == SmePanel::Forward || state.phase == SmePanel::Backward) {
        const unsigned k = state.step;
        const bool forward = state.phase == SmePanel::Forward;
        const unsigned pkRow = (k + rowOff) & 7u;
        const unsigned pkCol = (k + colOff) & 7u;
        const uint8_t bit = static_cast<uint8_t>(1u << k);
        const unsigned physRow = env.smeWarmBypass ? rowOff : dst.row;

        if (forward && !state.scaled) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = env.smeWarmBypass ? Sme::Op2d::ScaleBypass : Sme::Op2d::ScaleRow;
                seg.phase = Sme::Rhs;
                seg.rowMask = static_cast<uint8_t>(1u << physRow);
                seg.colMask = static_cast<uint8_t>(1u << pkCol);
                seg.dstTile = env.smeWarmBypass ? idx : dst.tile;
                seg.dstRow = physRow;
                seg.aPos = {luTile, (N + rowOff) & 7u};
                seg.aCol = pkCol;
                if (offerSme2d(seg, priority, critGroup,
                    [this, tok, idx, singleTilePool, k]() {
                        if (auto *r = find(tok))
                            ++r->record.coeffMulIssued;
                        auto &st = (singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx]).panels[0];
                        if (k + 1 == N) {
                            st.phase = SmePanel::Backward;
                            st.scaled = false;
                        } else {
                            st.scaled = true;
                        }
                    }))
                    offered = true;
            }
            return;
        }
        const uint8_t rows = forward ?
            static_cast<uint8_t>(SmeVectorMask & ~((bit << 1) - 1)) :
            static_cast<uint8_t>(bit - 1);
        if (rows) {
            if (!offered) {
                Sme::Segment2d seg{};
                seg.op = env.smeWarmBypass ? Sme::Op2d::SubtractOuterBypass : Sme::Op2d::Fmopa;
                seg.phase = Sme::Rhs;
                seg.rowMask = static_cast<uint8_t>(1u << physRow);
                seg.colMask = sme2dRotl8(rows, colOff);
                seg.dstTile = env.smeWarmBypass ? idx : dst.tile;
                seg.dstRow = physRow;
                seg.aPos = {dst.tile, 0};
                seg.aCol = pkCol;
                seg.bPos = {luTile, pkRow};
                const unsigned count = __builtin_popcount(rows);
                if (offerSme2d(seg, priority, critGroup,
                    [this, tok, idx, singleTilePool, forward, k, count]() {
                        if (auto *r = find(tok)) {
                            r->record.coeffMulIssued += count;
                            r->record.coeffSubIssued += count;
                        }
                        auto &st = (singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx]).panels[0];
                        st.scaled = false;
                        if (forward)
                            ++st.step;
                        else if (k == 1)
                            st.phase = SmePanel::Export;
                        else
                            --st.step;
                    }))
                    offered = true;
            }
            return;
        }
    }

    if (state.phase == SmePanel::Export) {
        Sme::Row rhs;
        const bool exported = env.smeWarmBypass ?
            smeZa.readBypass(idx, pVecColMask, rhs, Sme::Rhs) :
            smeZa.read(dst, pVecColMask, rhs, Sme::Rhs);
        if (!exported)
            return;
        const auto unrot = sme2dUnrotRow(rhs, SmeVectorMask, colOff);
        std::copy_n(unrot.begin(), N, req.rhs[0].value.begin());
        state.phase = SmePanel::Done;
        rhsDone(req, 0, now);
    }
}

void
CfdCoeffPreprocessController::advanceSmeInverseWarmRhs2d(
    Request &req, SmeCell &cell, unsigned idx, unsigned tile, uint64_t now,
    bool singleTilePool, unsigned priority, unsigned critGroup, bool &offered)
{
    using Sme = CfdSmeZa;
    auto &st = cell.panels[0];
    if (st.phase == SmePanel::Done)
        return;

    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, idx);
    const unsigned colOff = sme2dColOff(mode, idx);
    const uint8_t pVecColMask = sme2dRotl8(SmeVectorMask, colOff);
    const bool dual = smeUsesDualBypass();
    const unsigned idA = idx;
    const unsigned idB = smeWarmContextCount() + idx;
    const unsigned rowA = rowOff;
    const unsigned rowB = (rowOff + 1) & 7u;

    if (st.phase == SmePanel::Load) {
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && req.rhsVectorReady))
            return;
        trace(req, "solve", "SME_RHS_PANEL_LOAD", 0, 1);
        req.solveStarted = true;
        req.record.solveIssueCycle = now;
        req.stage = RequestStage::SolveActive;
        trace(req, "solve", "COEFF3_ISSUE");
        st.phase = SmePanel::Forward;
        cell.invStepA = 0;
        cell.invStepB = 0;
        cell.invMerged = false;
    }

    const uint64_t tok = req.token;
    if (st.phase == SmePanel::Forward) {
        if (!dual) {
            if (cell.invStepA < N && !offered) {
                const unsigned k = cell.invStepA;
                Sme::Segment2d seg{};
                seg.op = k == 0 ? Sme::Op2d::ScaleRowToBypass : Sme::Op2d::FmaRowToBypass;
                seg.phase = Sme::Rhs;
                seg.rowMask = static_cast<uint8_t>(1u << rowA);
                seg.colMask = pVecColMask;
                seg.dstTile = idA;
                seg.dstRow = rowA;
                seg.bPos = {tile, (k + rowOff) & 7u};
                seg.scalar = req.rhsVector[k];
                if (offerSme2d(seg, priority, critGroup,
                    [this, tok, idx, singleTilePool, k]() {
                        if (auto *r = find(tok)) {
                            r->record.coeffMulIssued += N;
                            if (k != 0)
                                r->record.coeffSubIssued += N;
                        }
                        auto &c = singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx];
                        if (++c.invStepA == N)
                            c.panels[0].phase = SmePanel::Export;
                    }))
                    offered = true;
            }
            return;
        }
        // Dual chain: offer ready chain A and/or chain B (distinct rows rowA/rowB),
        // skipping if this cell was already offered in the critical pass.
        if (offered)
            return;
        bool dualOffered = false;
        if (cell.invStepA < 3) {
            const unsigned k = cell.invStepA;
            Sme::Segment2d seg{};
            seg.op = k == 0 ? Sme::Op2d::ScaleRowToBypass : Sme::Op2d::FmaRowToBypass;
            seg.phase = Sme::Rhs;
            seg.rowMask = static_cast<uint8_t>(1u << rowA);
            seg.colMask = pVecColMask;
            seg.dstTile = idA;
            seg.dstRow = rowA;
            seg.bPos = {tile, (k + rowOff) & 7u};
            seg.scalar = req.rhsVector[k];
            if (offerSme2d(seg, priority, critGroup,
                [this, tok, idx, singleTilePool, k]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += N;
                        if (k != 0)
                            r->record.coeffSubIssued += N;
                    }
                    auto &c = singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx];
                    ++c.invStepA;
                }))
                dualOffered = true;
        }
        if (cell.invStepB < 2) {
            const unsigned k = 3 + cell.invStepB;
            Sme::Segment2d seg{};
            seg.op = cell.invStepB == 0 ? Sme::Op2d::ScaleRowToBypass : Sme::Op2d::FmaRowToBypass;
            seg.phase = Sme::Rhs;
            seg.rowMask = static_cast<uint8_t>(1u << rowB);
            seg.colMask = pVecColMask;
            seg.dstTile = idB;
            seg.dstRow = rowB;
            seg.bPos = {tile, (k + rowOff) & 7u};
            seg.scalar = req.rhsVector[k];
            if (offerSme2d(seg, priority, critGroup,
                [this, tok, idx, singleTilePool, isFma=(cell.invStepB != 0)]() {
                    if (auto *r = find(tok)) {
                        r->record.coeffMulIssued += N;
                        if (isFma)
                            r->record.coeffSubIssued += N;
                    }
                    auto &c = singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx];
                    ++c.invStepB;
                }))
                dualOffered = true;
        }
        if (cell.invStepA == 3 && cell.invStepB == 2 && !cell.invMerged) {
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::CombineBypass;
            seg.phase = Sme::Rhs;
            seg.rowMask = static_cast<uint8_t>(1u << rowA);
            seg.colMask = pVecColMask;
            seg.dstTile = idA;
            seg.dstRow = rowA;
            seg.aIsBypass = true;
            seg.aPos = {idA, 0};
            seg.bIsBypass = true;
            seg.bPos = {idB, 0};
            seg.subtract = false;
            if (offerSme2d(seg, priority, critGroup,
                [this, tok, idx, singleTilePool]() {
                    if (auto *r = find(tok))
                        r->record.coeffSubIssued += N;
                    auto &c = singleTilePool ? smeWarmSingleCells[idx] : smeCells[idx];
                    c.invMerged = true;
                    c.panels[0].phase = SmePanel::Export;
                }))
                dualOffered = true;
        }
        if (dualOffered)
            offered = true;
        return;
    }

    if (st.phase == SmePanel::Export) {
        Sme::Row rhs;
        if (!smeZa.readBypass(idA, pVecColMask, rhs, Sme::Rhs))
            return;
        const auto unrot = sme2dUnrotRow(rhs, SmeVectorMask, colOff);
        std::copy_n(unrot.begin(), N, req.rhs[0].value.begin());
        st.phase = SmePanel::Done;
        if (smeOwner) {
            auto lineIt = lineRequests.find(smeOwner);
            if (lineIt != lineRequests.end())
                lineIt->second->smeInvCells++;
        }
        rhsDone(req, 0, now);
    }
}

void
CfdCoeffPreprocessController::processSmeCriticalRhs2d(uint64_t now)
{
    if (!smeOwner || !env.smeWarmVerticalRhs)
        return;
    const auto it = lineRequests.find(smeOwner);
    if (it == lineRequests.end() || it->second->terminal ||
        !it->second->coefficientCacheHit || it->second->backwardStarted)
        return;
    auto &pool = env.smeWarmSingleTile ? smeWarmSingleCells : smeCells;
    const unsigned stride = env.smeWarmSingleTile ? 1 : 3;
    const uint64_t horizon = smeForwardCells + 3;
    for (Request *ptr : admissionOrder()) {
        auto &req = *ptr;
        if (req.stage == RequestStage::Failed || req.stage == RequestStage::Complete ||
            req.solveDone || req.desc.cellId > horizon)
            continue;
        auto slot = std::find_if(pool.begin(), pool.end(),
            [&req](const SmeCell &c) { return c.token == req.token; });
        if (slot == pool.end())
            continue;
        const unsigned idx = std::distance(pool.begin(), slot);
        const unsigned tile = stride * idx;
        bool offered = false;
        advanceSmeLu2d(req, *slot, idx, tile, now, offered);
        if (req.stage == RequestStage::Failed)
            continue;
        if (req.smeInvUseCache) {
            advanceSmeInverseWarmRhs2d(req, *slot, idx, tile, now,
                                       env.smeWarmSingleTile, 2, 0, offered);
        } else {
            advanceSmeVerticalRhs2d(req, *slot, idx, tile, now,
                                    env.smeWarmSingleTile, 2, 0, offered);
        }
        if (offered)
            sme2dCriticalOffered.insert(req.token);
        if (req.luDone && req.stage != RequestStage::Failed && req.solveDone) {
            if (req.inputOperandsReady)
                finalizeSolveRequest(req, now);
            bool idleAll = true;
            for (unsigned i = 0; i < stride; ++i)
                idleAll &= smeZa.idle(tile + i);
            if (env.smeWarmBypass) {
                idleAll &= smeZa.idleBypass(idx);
                if (smeUsesDualBypass())
                    idleAll &= smeZa.idleBypass(smeWarmContextCount() + idx);
            }
            if (idleAll) {
                for (unsigned i = 0; i < stride; ++i)
                    smeZa.release(tile + i);
                if (env.smeWarmBypass) {
                    smeZa.releaseBypass(idx);
                    if (smeUsesDualBypass())
                        smeZa.releaseBypass(smeWarmContextCount() + idx);
                }
                *slot = {};
            }
        }
    }
}

void
CfdCoeffPreprocessController::processSmeRequests2d(uint64_t now)
{
    auto bypassIdle = [this](unsigned idx) {
        return smeZa.idleBypass(idx) &&
            (!smeUsesDualBypass() || smeZa.idleBypass(smeWarmContextCount() + idx));
    };
    auto bypassRelease = [this](unsigned idx) {
        smeZa.releaseBypass(idx);
        if (smeUsesDualBypass())
            smeZa.releaseBypass(smeWarmContextCount() + idx);
    };
    auto release = [this, &bypassIdle, &bypassRelease](
            SmeCell &cell, unsigned idx, unsigned tile) {
        if (!smeZa.idle(tile) || !smeZa.idle(tile + 1) || !smeZa.idle(tile + 2) ||
            (env.smeWarmBypass && !env.smeWarmSingleTile && !bypassIdle(idx)))
            return;
        for (unsigned i = 0; i < 3; ++i)
            smeZa.release(tile + i);
        if (env.smeWarmBypass && !env.smeWarmSingleTile)
            bypassRelease(idx);
        cell = {};
    };
    auto releaseSingle = [this, &bypassIdle, &bypassRelease](
            SmeCell &cell, unsigned idx) {
        if (!smeZa.idle(idx) || (env.smeWarmBypass && !bypassIdle(idx)))
            return;
        smeZa.release(idx);
        if (env.smeWarmBypass)
            bypassRelease(idx);
        cell = {};
    };
    for (unsigned i = 0; i < smeCells.size(); ++i) {
        auto &cell = smeCells[i];
        if (cell.token) {
            const auto *req = find(cell.token);
            if (!req || req->stage == RequestStage::Failed)
                release(cell, i, 3 * i);
        }
    }
    for (unsigned i = 0; i < smeWarmSingleCells.size(); ++i) {
        auto &cell = smeWarmSingleCells[i];
        if (cell.token) {
            const auto *req = find(cell.token);
            if (!req || req->stage == RequestStage::Failed)
                releaseSingle(cell, i);
        }
    }
    const unsigned backwardTile = 3 * smeCells.size() + 1;
    if (smeBackward.owner) {
        const auto line = lineRequests.find(smeBackward.owner);
        if ((line == lineRequests.end() || line->second->terminal) &&
            smeZa.idle(backwardTile)) {
            smeZa.release(backwardTile);
            smeBackward = {};
        }
    }
    if (smeOwner && (env.smeWarmBackSplit || !smeWarmRhs.empty())) {
        auto &line = *lineRequests.at(smeOwner);
        if (line.terminal)
            for (auto &point : smeSplitBack)
                if (point.cell != UINT64_MAX)
                    releaseSmeSplitBack(line, point, "drop");
        if (line.coefficientCacheHit && !smeWarmRhs.empty()) {
            processSmeWarmRhs2d(line, now);
            return;
        }
    }
    if (smeOwner && env.smeWarmSingleTile &&
        lineRequests.at(smeOwner)->coefficientCacheHit) {
        for (Request *ptr : admissionOrder()) {
            auto &req = *ptr;
            if (req.stage == RequestStage::Failed || req.stage == RequestStage::Complete)
                continue;
            if (req.solveDone && req.luDone) {
                if (req.inputOperandsReady)
                    finalizeSolveRequest(req, now);
                continue;
            }
            auto slot = std::find_if(smeWarmSingleCells.begin(), smeWarmSingleCells.end(),
                [&req](const SmeCell &cell) { return cell.token == req.token; });
            if (slot == smeWarmSingleCells.end()) {
                if (req.stage != RequestStage::InputReady)
                    continue;
                slot = std::find_if(smeWarmSingleCells.begin(), smeWarmSingleCells.end(),
                    [](const SmeCell &cell) { return !cell.token; });
                if (slot == smeWarmSingleCells.end()) {
                    observe("sme_context_full");
                    continue;
                }
                slot->token = req.token;
                if (req.slotOwned && req.inputOperandsReady) {
                    slots[req.slot] = false;
                    req.slotOwned = false;
                    trace(req, "input", "INPUT_SLOT_RELEASE");
                }
                bindSmeCachedLu(req, now);
            }
            const unsigned idx = std::distance(smeWarmSingleCells.begin(), slot);
            bool offered = sme2dCriticalOffered.count(req.token) != 0;
            advanceSmeLu2d(req, *slot, idx, idx, now, offered);
            if (req.stage == RequestStage::Failed)
                continue;
            if (req.smeInvUseCache)
                advanceSmeInverseWarmRhs2d(req, *slot, idx, idx, now, true, 4, 1, offered);
            else
                advanceSmeVerticalRhs2d(req, *slot, idx, idx, now, true, 4, 1, offered);
            if (req.luDone && req.stage != RequestStage::Failed) {
                if (req.inputOperandsReady)
                    finalizeSolveRequest(req, now);
                if (req.solveDone)
                    releaseSingle(*slot, idx);
            }
        }
        return;
    }

    std::vector<uint8_t> cellOffered(smeCells.size(), 0);
    for (Request *ptr : admissionOrder()) {
        auto &req = *ptr;
        if (req.stage == RequestStage::Failed || req.stage == RequestStage::Complete)
            continue;
        const bool invReady = !req.smeInvRequested || req.smeInvDone;
        if (req.solveDone && req.luDone && invReady) {
            if (req.inputOperandsReady)
                finalizeSolveRequest(req, now);
            continue;
        }
        auto slot = std::find_if(smeCells.begin(), smeCells.end(),
            [&req](const SmeCell &cell) { return cell.token == req.token; });
        if (slot == smeCells.end()) {
            if (req.stage != RequestStage::InputReady &&
                !(splitInput(req) && req.inputDReady &&
                  req.stage == RequestStage::InputFetching))
                continue;
            slot = std::find_if(smeCells.begin(), smeCells.end(),
                [](const SmeCell &cell) { return !cell.token; });
            if (slot == smeCells.end()) {
                observe("sme_context_full");
                continue;
            }
            slot->token = req.token;
            if (req.slotOwned && req.inputOperandsReady) {
                slots[req.slot] = false;
                req.slotOwned = false;
                trace(req, "input", "INPUT_SLOT_RELEASE");
            }
            if (req.coefficientCacheReuse) {
                bindSmeCachedLu(req, now);
            } else {
                req.stage = RequestStage::LuActive;
                req.record.luIssueCycle = now;
                trace(req, "lu", "LU_ISSUE");
            }
        }
        const unsigned idx = std::distance(smeCells.begin(), slot);
        const unsigned tile = 3 * idx;
        bool luOffered = false;
        advanceSmeLu2d(req, *slot, idx, tile, now, luOffered);
        if (req.stage == RequestStage::Failed)
            continue;
        const uint8_t lower = (req.rhsEnabledMask >> 4) & (SmeVectorMask << 1);
        const uint8_t upper = req.smeDeferUpper ? SmeVectorMask :
            (req.rhsEnabledMask >> 10) & SmeVectorMask;
        bool panel0Offered = sme2dCriticalOffered.count(req.token) != 0;
        bool panel1Offered = false;
        const bool lineInvPolicy = smeOwner && lineRequests.at(smeOwner)->smeInversePolicy;
        const bool usePacked = env.smeConfig.panelPack && lineInvPolicy &&
            !req.coefficientCacheReuse && lower && upper;
        if (req.coefficientCacheReuse && req.smeInvUseCache) {
            advanceSmeInverseWarmRhs2d(req, *slot, idx, tile, now, false, 4, 1, panel0Offered);
        } else if (env.smeWarmVerticalRhs && req.coefficientCacheReuse) {
            advanceSmeVerticalRhs2d(req, *slot, idx, tile, now, false, 4, 1, panel0Offered);
        } else if (usePacked) {
            advanceSmePackedPanelA2d(req, *slot, idx, tile, now, panel0Offered);
            if (req.stage == RequestStage::Failed)
                continue;
            advanceSmePackedPanelB2d(req, *slot, idx, tile, now, panel1Offered);
        } else {
            advanceSmePanel2d(req, *slot, idx, 0, tile, 0,
                              env.smeSplitRhs ? 1u : (1u | lower), now, panel0Offered);
            if (req.stage == RequestStage::Failed)
                continue;
            if (env.smeSplitRhs)
                advanceSmePanel2d(req, *slot, idx, 2, tile, 0, lower, now, panel0Offered);
            if (req.stage == RequestStage::Failed)
                continue;
            advanceSmePanel2d(req, *slot, idx, 1, tile, 1, upper, now, panel1Offered);
        }
        if (req.stage == RequestStage::Failed)
            continue;
        cellOffered[idx] = panel0Offered;
        const bool cellInvReady = !req.smeInvRequested || req.smeInvDone;
        if (req.luDone && req.stage != RequestStage::Failed &&
            (!req.solveDone || cellInvReady)) {
            if (req.inputOperandsReady)
                finalizeSolveRequest(req, now);
            if (req.solveDone && cellInvReady)
                release(*slot, idx, tile);
        }
    }

    // Pass 2 (low-priority background): build D^-1 on reused tile+1
    // (skipped for packed interior cells where Panel B on tile+2 owns I5).
    if (env.smeWarmBaseMode != SmeWarmBaseMode::DirectLu) {
        for (unsigned idx = 0; idx < smeCells.size(); ++idx) {
            auto &cell = smeCells[idx];
            if (!cell.token)
                continue;
            Request *req = find(cell.token);
            if (!req || req->stage == RequestStage::Failed ||
                req->stage == RequestStage::Complete ||
                !req->smeInvRequested || req->smeInvDone)
                continue;
            const uint8_t lower = (req->rhsEnabledMask >> 4) & (SmeVectorMask << 1);
            const uint8_t upper = req->smeDeferUpper ? SmeVectorMask :
                (req->rhsEnabledMask >> 10) & SmeVectorMask;
            const bool lineInvPolicy = smeOwner && lineRequests.at(smeOwner)->smeInversePolicy;
            if (env.smeConfig.panelPack && lineInvPolicy &&
                !req->coefficientCacheReuse && lower && upper)
                continue;
            const unsigned tile = 3 * idx;
            bool offered = cellOffered[idx] != 0;
            advanceSmeInversePanel2d(*req, cell, idx, tile, now, offered);
            if (req->stage == RequestStage::Failed)
                continue;
            if (req->luDone && req->solveDone && req->smeInvDone) {
                if (req->inputOperandsReady)
                    finalizeSolveRequest(*req, now);
                release(cell, idx, tile);
            }
        }
    }
}

bool
CfdCoeffPreprocessController::advanceSmeScan2d(
    SmeScan &scan, unsigned tile, CfdSmeZa::Phase phase,
    const std::array<double, N> &feedback, const std::array<double, N> *base,
    uint64_t now, CfdSmeZa::Row &result)
{
    using Sme = CfdSmeZa;
    const unsigned baseRow = N + (scan.cell & 1u);
    const unsigned priority = phase == Sme::Backward ? 0u : 1u;

    if (base && !scan.baseLoaded) {
        Sme::Row value{};
        std::copy(base->begin(), base->end(), value.begin());
        scan.baseLoaded = smeZa.load({tile, baseRow}, SmeVectorMask, value, phase);
    }

    for (unsigned k = 0; k < N; ++k) {
        const unsigned bit = 1u << k;
        if ((scan.loaded & bit) && !(scan.products & bit)) {
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::ScaleRow;
            seg.phase = phase;
            seg.rowMask = static_cast<uint8_t>(1u << k);
            seg.colMask = SmeVectorMask;
            seg.dstTile = tile;
            seg.dstRow = k;
            seg.useScalar = true;
            seg.scalar = feedback[k];
            offerSme2d(seg, priority, 0, [this, phase, bit]() {
                auto &s = phase == Sme::Backward ? smeBackward : smeForward;
                s.products |= bit;
            });
        }
    }

    auto offerCombine = [this, &scan, tile, phase, priority, now](
            unsigned node, unsigned dst, unsigned a, unsigned b, bool subtract) {
        if (scan.nodes & (1u << node))
            return;
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::CombineRow;
        seg.phase = phase;
        seg.rowMask = static_cast<uint8_t>(1u << dst);
        seg.colMask = SmeVectorMask;
        seg.dstTile = tile;
        seg.dstRow = dst;
        seg.aPos = {tile, a};
        seg.bPos = {tile, b};
        seg.subtract = subtract;
        offerSme2d(seg, priority, 0, [this, phase, node, now]() {
            auto &s = phase == Sme::Backward ? smeBackward : smeForward;
            s.nodes |= (1u << node);
            if (node == 4)
                s.finalIssue = now;
        });
    };
    if ((scan.products & 0x03) == 0x03)
        offerCombine(0, 0, 0, 1, false);
    if ((scan.products & 0x0c) == 0x0c)
        offerCombine(1, 2, 2, 3, false);
    if ((scan.products & 0x10) && scan.baseLoaded)
        offerCombine(2, baseRow, baseRow, 4, true);
    if ((scan.nodes & 0x03) == 0x03)
        offerCombine(3, 0, 0, 2, false);
    if ((scan.nodes & 0x0c) == 0x0c && !(scan.nodes & 0x10))
        offerCombine(4, baseRow, baseRow, 0, true);

    return (scan.nodes & 0x10) &&
        smeZa.read({tile, baseRow}, SmeVectorMask, result, phase);
}

void
CfdCoeffPreprocessController::stageSmeScanColumn2d(
    SmeScan &scan, unsigned tile, unsigned column, const CfdSmeZa::Row &value,
    double feedback, CfdSmeZa::Phase phase)
{
    using Sme = CfdSmeZa;
    const unsigned bit = 1u << column;
    if (env.smeConfig.ingressPorts) {
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::IngressRow;
        seg.phase = phase;
        seg.rowMask = static_cast<uint8_t>(1u << column);
        seg.colMask = SmeVectorMask;
        seg.dstTile = tile;
        seg.dstRow = column;
        seg.ingressValues = value;
        seg.scalar = feedback;
        const unsigned priority = phase == Sme::Backward ? 0u : 1u;
        offerSme2d(seg, priority, 0, [this, phase, bit]() {
            auto &s = phase == Sme::Backward ? smeBackward : smeForward;
            s.loaded |= bit;
            s.products |= bit;
        });
    } else if (smeZa.load({tile, column}, SmeVectorMask, value, phase)) {
        scan.loaded |= bit;
    }
}

bool
CfdCoeffPreprocessController::advanceSmeReversePanel2d(
    LineRequest &line, BackwardOperand &operand, SmeReversePanel &panel,
    unsigned panelIdx, unsigned tile)
{
    using Sme = CfdSmeZa;
    auto &state = panel.state;
    const auto mode = env.smeConfig.arrayMode;
    const unsigned rowOff = sme2dRowOff(mode, panelIdx);
    const unsigned colOff = sme2dColOff(mode, panelIdx);
    const uint8_t pVecColMask = sme2dRotl8(SmeVectorMask, colOff);

    if (state.phase == SmePanel::Load) {
        for (; panel.loaded < N + 2; ++panel.loaded) {
            Sme::Row value{};
            const unsigned pr = (panel.loaded + rowOff) & 7u;
            if (panel.loaded < N) {
                std::copy_n(operand.values[panel.loaded].begin(), N, value.begin());
                if (!smeZa.load({tile, pr}, pVecColMask,
                                sme2dRotRow(value, SmeVectorMask, colOff),
                                Sme::ReverseRhs))
                    return false;
            } else {
                const unsigned offset = (panel.loaded - N) * Sme::Width;
                const unsigned w = std::min(Sme::Width, smeUpperFactorWords() - offset);
                std::copy_n(operand.smeFactors.value.begin() + offset, w, value.begin());
                if (!smeZa.load({tile, pr}, static_cast<uint8_t>((1u << w) - 1u),
                                value, Sme::ReverseRhs))
                    return false;
            }
        }
        panel.upperStarted = env.smeUpperSplit == N;
        panel.nextUpper.fill(N - 1);
        state.phase = panel.upperStarted ? SmePanel::Backward : SmePanel::Forward;
        state.step = panel.upperStarted ? N - 1 : env.smeUpperSplit;
        state.row = panel.upperStarted ? 0 : state.step + 1;
        state.scaled = false;
    }

    auto factorPosCol = [&](unsigned row, unsigned col)
        -> std::pair<Sme::Position, unsigned> {
        const unsigned split = env.smeUpperSplit;
        unsigned index;
        if (row == N) {
            index = col - split;
        } else if (row > col) {
            const unsigned localRow = row - split;
            index = N - split + localRow * (localRow - 1) / 2 + col - split;
        } else {
            index = SmeUpperFactors::upperIndex(row, col, split);
        }
        return {{tile, (N + index / Sme::Width + rowOff) & 7u}, index % Sme::Width};
    };

    if (state.phase == SmePanel::Forward) {
        const unsigned k = state.step;
        const unsigned pkRow = (k + rowOff) & 7u;
        if (!state.scaled) {
            const auto [src, col] = factorPosCol(N, k);
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::ScaleRow;
            seg.phase = Sme::ReverseRhs;
            seg.rowMask = static_cast<uint8_t>(1u << pkRow);
            seg.colMask = pVecColMask;
            seg.dstTile = tile;
            seg.dstRow = pkRow;
            seg.aPos = src;
            seg.aCol = col;
            offerSme2d(seg, 5, 1, [this, panelIdx, k]() {
                auto &p = smeReversePanels[panelIdx];
                p.state.scaled = true;
                if (k + 1 == N) {
                    p.state.phase = SmePanel::Backward;
                    p.state.row = 0;
                    p.upperStarted = true;
                }
            });
            return false;
        }
        if (k + 1 < N) {
            const uint8_t logicalRows = SmeVectorMask & ~((2u << k) - 1);
            Sme::Segment2d seg{};
            seg.op = Sme::Op2d::FmopaFactorRows;
            seg.phase = Sme::ReverseRhs;
            seg.rowMask = sme2dRotl8(logicalRows, rowOff);
            seg.colMask = pVecColMask;
            seg.dstTile = tile;
            seg.bPos = {tile, pkRow};
            for (unsigned r = k + 1; r < N; ++r) {
                const auto [fPos, fCol] = factorPosCol(r, k);
                const unsigned pr = (r + rowOff) & 7u;
                seg.factorPos[pr] = fPos;
                seg.factorCol[pr] = static_cast<uint8_t>(fCol);
            }
            offerSme2d(seg, 5, 1, [this, panelIdx]() {
                auto &p = smeReversePanels[panelIdx];
                ++p.state.step;
                p.state.row = p.state.step + 1;
                p.state.scaled = false;
            });
            return false;
        }
    }

    if (state.phase == SmePanel::Backward) {
        if (env.smeReverseWavefront) {
            // Group ready rows sharing the highest eligible pivot column col
            for (int col = int(N) - 1; col >= 1; --col) {
                if (panel.nextUpper[col] != col)
                    continue;
                uint8_t logicalRows = 0;
                for (int r = col - 1; r >= 0; --r)
                    if (panel.nextUpper[r] == col)
                        logicalRows |= static_cast<uint8_t>(1u << r);
                if (!logicalRows)
                    continue;
                Sme::Segment2d seg{};
                seg.op = Sme::Op2d::FmopaFactorRows;
                seg.phase = Sme::ReverseRhs;
                seg.rowMask = sme2dRotl8(logicalRows, rowOff);
                seg.colMask = pVecColMask;
                seg.dstTile = tile;
                seg.bPos = {tile, (unsigned(col) + rowOff) & 7u};
                for (unsigned r = 0; r < unsigned(col); ++r) {
                    if (!(logicalRows & (1u << r)))
                        continue;
                    const auto [fPos, fCol] = factorPosCol(r, unsigned(col));
                    const unsigned pr = (r + rowOff) & 7u;
                    seg.factorPos[pr] = fPos;
                    seg.factorCol[pr] = static_cast<uint8_t>(fCol);
                }
                if (offerSme2d(seg, 5, 1, [this, panelIdx, logicalRows]() {
                    auto &p = smeReversePanels[panelIdx];
                    for (unsigned r = 0; r < N; ++r)
                        if (logicalRows & (1u << r))
                            --p.nextUpper[r];
                    bool allDone = true;
                    for (unsigned r = 0; r + 1 < N; ++r)
                        if (p.nextUpper[r] != r)
                            allDone = false;
                    if (allDone) {
                        p.state.phase = SmePanel::Export;
                        p.state.row = 0;
                    }
                }))
                    return false;
            }
            return false;
        }
        const unsigned k = state.step;
        const uint8_t logicalRows = static_cast<uint8_t>((1u << k) - 1u);
        Sme::Segment2d seg{};
        seg.op = Sme::Op2d::FmopaFactorRows;
        seg.phase = Sme::ReverseRhs;
        seg.rowMask = sme2dRotl8(logicalRows, rowOff);
        seg.colMask = pVecColMask;
        seg.dstTile = tile;
        seg.bPos = {tile, (k + rowOff) & 7u};
        for (unsigned r = 0; r < k; ++r) {
            const auto [fPos, fCol] = factorPosCol(r, k);
            const unsigned pr = (r + rowOff) & 7u;
            seg.factorPos[pr] = fPos;
            seg.factorCol[pr] = static_cast<uint8_t>(fCol);
        }
        offerSme2d(seg, 5, 1, [this, panelIdx]() {
            auto &p = smeReversePanels[panelIdx];
            p.state.row = 0;
            if (--p.state.step == 0)
                p.state.phase = SmePanel::Export;
        });
        return false;
    }

    if (state.phase == SmePanel::Export) {
        for (; state.row < N; ++state.row) {
            Sme::Row row;
            const unsigned pr = (state.row + rowOff) & 7u;
            if (!smeZa.read({tile, pr}, pVecColMask, row, Sme::ReverseRhs))
                return false;
            const auto unrot = sme2dUnrotRow(row, SmeVectorMask, colOff);
            for (unsigned col = 0; col < N; ++col) {
                if (!std::isfinite(unrot[col])) {
                    abortLine(line, CfdCoeffPreprocessStatus::InternalError);
                    return false;
                }
                operand.values[col][state.row] = unrot[col];
            }
        }
        state.phase = SmePanel::Done;
    }

    uint64_t storeId = 0;
    if (line.coefficientCacheEnabled) {
        auto &store = operand.columns[1];
        if (!store)
            store = startOnchip(line.token, smeUpperAddr(line, panel.cell),
                                N * N, 8, true, 1);
        if (!store)
            return false;
        auto &upper = lineCoeffCaches.at(line.desc.lineId).uBar[panel.cell];
        for (unsigned row = 0; row < N; ++row)
            for (unsigned col = 0; col < N; ++col)
                upper[row * N + col] = operand.values[col][row];
        storeId = store->id;
    }
    operand.readyColumns = SmeVectorMask;
    ++line.smeReverseDone;
    if (env.traceEnable) {
        std::array<double, N * N> matrix;
        for (unsigned row = 0; row < N; ++row)
            for (unsigned col = 0; col < N; ++col)
                matrix[row * N + col] = operand.values[col][row];
        traceSmeUpperMatrix(line, panel.cell, "ubar", matrix);
        std::fprintf(stderr, "CFD_SME_UPPER_MEMORY cycle=%llu token=%llu cell=%llu "
            "event=finish job=%llu rewrite=%llu words=%u\n",
            (unsigned long long)cycle(), (unsigned long long)line.token,
            (unsigned long long)panel.cell, (unsigned long long)operand.columns[0]->id,
            (unsigned long long)storeId, N * N + smeUpperFactorWords());
    }
    return true;
}

void
CfdCoeffPreprocessController::processSmeWarmRhs2d(LineRequest &line, uint64_t now)
{
    using Sme = CfdSmeZa;
    std::array<unsigned, CfdSmeZa::warmRhsGroups(8)> active{};
    unsigned live = 0;
    for (unsigned group = 0; group < smeWarmRhs.size(); ++group) {
        if (!smeWarmRhs[group].count)
            continue;
        if (line.terminal)
            releaseSmeWarmRhs(line, group, "drop");
        else
            active[live++] = group;
    }
    if (line.terminal)
        return;
    std::sort(active.begin(), active.begin() + live, [this](unsigned a, unsigned b) {
        return smeWarmRhs[a].first < smeWarmRhs[b].first;
    });
    bool gather = true;
    auto advanceGroup = [&](unsigned group) {
        auto &batch = smeWarmRhs[group];
        const uint8_t mask = static_cast<uint8_t>((1u << batch.count) - 1u);
        auto position = [group](unsigned row) -> Sme::Position {
            return {group * Sme::WarmRhsTiles + row / Sme::Width, row % Sme::Width};
        };
        std::array<Request *, N> points{};
        for (unsigned lane = 0; lane < batch.count; ++lane) {
            points[lane] = find(batch.requests[lane]);
            panic_if(!points[lane] || points[lane]->stage == RequestStage::Failed,
                     "CFD packed warm RHS lost a live point");
        }
        constexpr unsigned TotalRows = N + SmeWarmRhsProgram.size();
        const unsigned load = batch.loaded;
        if (gather && load < TotalRows) {
            Sme::Row values{};
            for (unsigned lane = 0; lane < batch.count; ++lane) {
                const auto &req = *points[lane];
                if (load < N) {
                    values[lane] = req.rhsVector[load];
                } else {
                    const auto [row, pivot] = SmeWarmRhsProgram[load - N];
                    values[lane] = row == pivot ? req.reciprocal[row] :
                        req.lu[row * N + pivot];
                }
            }
            if (smeZa.load(position(load), mask, values, Sme::Rhs)) {
                traceSmeWarmRhs(line, group, "load", load);
                ++batch.loaded;
                gather = false;
                for (unsigned lane = 0; lane < batch.count; ++lane) {
                    auto &req = *points[lane];
                    if (!load)
                        trace(req, "solve", "SME_RHS_PANEL_LOAD", 0, 1);
                    if (batch.loaded == N) {
                        req.solveStarted = true;
                        req.record.solveIssueCycle = now;
                        req.stage = RequestStage::SolveActive;
                        trace(req, "solve", "COEFF3_ISSUE");
                    }
                }
            }
        }
        if (batch.issued < SmeWarmRhsProgram.size() && batch.loaded > N + batch.issued) {
            const auto [row, pivot] = SmeWarmRhsProgram[batch.issued];
            const bool isScale = row == pivot;
            const auto dstP = position(row);
            const auto factorP = position(N + batch.issued);
            Sme::Segment2d seg{};
            seg.op = isScale ? Sme::Op2d::ScaleElements : Sme::Op2d::SubtractElements;
            seg.phase = Sme::Rhs;
            seg.rowMask = static_cast<uint8_t>(1u << dstP.row);
            seg.colMask = mask;
            seg.dstTile = dstP.tile;
            seg.dstRow = dstP.row;
            seg.aPos = factorP;
            seg.bPos = position(pivot);
            const uint64_t lineTok = line.token;
            offerSme2d(seg, 4, 1, [this, lineTok, group, isScale]() {
                auto &b = smeWarmRhs[group];
                auto it = lineRequests.find(lineTok);
                if (it != lineRequests.end())
                    traceSmeWarmRhs(*it->second, group, "issue", b.issued);
                ++b.issued;
                for (unsigned lane = 0; lane < b.count; ++lane) {
                    if (auto *r = find(b.requests[lane])) {
                        ++r->record.coeffMulIssued;
                        r->record.coeffSubIssued += !isScale;
                    }
                }
            });
            return;
        }
        if (batch.issued != SmeWarmRhsProgram.size())
            return;
        while (batch.exported < N) {
            Sme::Row values;
            if (!smeZa.read(position(batch.exported), mask, values, Sme::Rhs))
                return;
            for (unsigned lane = 0; lane < batch.count; ++lane)
                points[lane]->rhs[0].value[batch.exported] = values[lane];
            traceSmeWarmRhs(line, group, "export", batch.exported++);
        }
        for (unsigned lane = 0; lane < batch.count && !line.terminal; ++lane) {
            auto &req = *points[lane];
            rhsDone(req, 0, now);
            finalizeSolveRequest(req, now);
        }
        panic_if(!releaseSmeWarmRhs(line, group, line.terminal ? "drop" : "release"),
                 "CFD packed warm RHS exported with pending ZA returns");
    };

    for (unsigned i = 0; i < live && !line.terminal; ++i)
        advanceGroup(active[i]);
    if (line.terminal || line.smeWarmNextCell == line.cells)
        return;

    auto free = std::find_if(smeWarmRhs.begin(), smeWarmRhs.end(),
                             [](const auto &group) { return !group.count; });
    if (free == smeWarmRhs.end())
        return;
    const uint64_t first = line.smeWarmNextCell;
    const auto tile = forwardTile(line, first);
    if (!tile)
        return;
    const unsigned count = std::min({uint64_t(env.smeWarmRhsWidth), line.window,
                                     tile->start + tile->count - first});
    if (line.nextCell < first + count)
        return;
    std::array<Request *, N> points{};
    for (const auto &entry : requests) {
        auto &req = *entry.second;
        const uint64_t cell = req.desc.cellId;
        if (req.coefficientCacheReuse && req.stage == RequestStage::InputReady &&
            req.inputOperandsReady && cell >= first && cell < first + count &&
            childLineTokens.at(req.token) == line.token)
            points[cell - first] = &req;
    }
    for (unsigned lane = 0; lane < count; ++lane)
        if (!points[lane])
            return;
    free->first = first;
    free->count = count;
    for (unsigned lane = 0; lane < count; ++lane) {
        auto &req = *points[lane];
        panic_if(!req.cachedReciprocal || req.rhsEnabledMask != 1,
                 "CFD packed warm RHS needs a cached reciprocal base solve");
        free->requests[lane] = req.token;
        if (req.slotOwned) {
            slots[req.slot] = false;
            req.slotOwned = false;
            trace(req, "input", "INPUT_SLOT_RELEASE");
        }
        bindSmeCachedLu(req, now);
    }
    line.smeWarmNextCell += count;
    const unsigned group = std::distance(smeWarmRhs.begin(), free);
    traceSmeWarmRhs(line, group, "bind");
    if (gather)
        advanceGroup(group);
}

void
CfdCoeffPreprocessController::processSmeSplitBack2d(LineRequest &line)
{
    using Sme = CfdSmeZa;
    const bool useBypass = env.smeWarmBypass && smeZa.bypassCount() >= 6;
    auto pointAt = [this](uint64_t cell) -> SmeSplitBackPoint & {
        return smeSplitBack[cell % smeSplitBack.size()];
    };
    auto tileAt = [this](uint64_t cell, unsigned part) -> unsigned {
        return 2 * (cell % smeSplitBack.size()) + part;
    };
    auto latchAt = [this](uint64_t cell, unsigned row) -> int {
        const unsigned base = 3 * (cell % smeSplitBack.size());
        if (row == 0) return base;
        if (row == 2) return base + 1;
        if (row == N) return base + 2;
        return -1;
    };
    auto zaRow = [this, useBypass](uint64_t cell, unsigned part, unsigned row) -> unsigned {
        const unsigned slot = cell % smeSplitBack.size();
        if (useBypass) {
            // Non-bypass rows {1, 3, 4} live in per-(slot, part) tiles.
            // Assign disjoint physical PE rows across slots and parts so
            // slot0/slot1 and part0/part1 can co-issue without row aliasing.
            static constexpr uint8_t map[2][2][3] = {
                {{1, 3, 4}, {3, 4, 1}},
                {{5, 0, 2}, {0, 2, 5}},
            };
            const unsigned idx = row == 1 ? 0u : row == 3 ? 1u : 2u;
            return map[slot & 1u][part & 1u][idx];
        }
        return (row + (slot ? 2u : 0u)) & 7u;
    };

    for (unsigned ahead = 0; ahead < smeSplitBack.size() &&
            ahead <= uint64_t(line.backwardCell); ++ahead) {
        const uint64_t cell = line.backwardCell - ahead;
        auto &point = pointAt(cell);
        if (point.cell != cell)
            continue;
        for (unsigned part = 0; part < SmeBackParts.size(); ++part) {
            const uint8_t mask = SmeBackParts[part];
            Sme::Row values;
            if (!(point.returned & mask) && (point.issued[part] & (1u << SmeBackFinal))) {
                const bool got = useBypass ?
                    smeZa.readBypass(latchAt(cell, N), mask, values, Sme::Backward) :
                    smeZa.read({tileAt(cell, part), zaRow(cell, part, N)}, mask, values, Sme::Backward);
                if (got) {
                    for (unsigned row = 0; row < N; ++row)
                        if (mask & (1u << row))
                            point.value[row] = values[row];
                    point.returned |= mask;
                    point.partReadyCycle[part] =
                        cycle() + (env.smeFeedbackReg ? 1u : 0u);
                    traceSmeSplitBack(line, cell, "feedback", part, SmeBackFinal);
                }
            }
        }
    }

    auto &head = pointAt(line.backwardCell);
    if (head.cell == uint64_t(line.backwardCell) && head.returned == SmeVectorMask) {
        if (line.outputWrites.size() >= env.lineOutputDepth) {
            ++line.outputQueueStalls;
        } else {
            line.backwardAccumulator = head.value;
            line.backwardPartReadyCycle = head.partReadyCycle;
            line.lineProducts += N;
            line.lineAdds += N;
            panic_if(!releaseSmeSplitBack(line, head, "commit"),
                     "CFD split backward committed before all returns");
            finishLineBackwardCell(line, true);
        }
    }
    if (line.terminal || line.backwardCell < 0)
        return;

    struct Reduce
    {
        unsigned needs, dst, left, right;
        bool subtract;
    };
    constexpr std::array<Reduce, 5> nodes = {{
        {0x03, 0, 0, 1, false},
        {0x0c, 2, 2, 3, false},
        {0x10 | SmeBackBase, N, N, 4, true},
        {(1u << 5) | (1u << 6), 0, 0, 2, false},
        {(1u << 7) | (1u << 8), N, N, 0, true},
    }};
    const uint64_t lineTok = line.token;

    for (unsigned ahead = 0; ahead < smeSplitBack.size() &&
            ahead <= uint64_t(line.backwardCell); ++ahead) {
        const uint64_t cell = line.backwardCell - ahead;
        auto &operand = line.backwardOperands[cell % env.backwardOperandDepth];
        if (operand.cell != cell)
            continue;
        auto &point = pointAt(cell);
        if (point.cell == UINT64_MAX) {
            point.cell = cell;
            traceSmeSplitBack(line, cell, "bind");
        }
        panic_if(point.cell != cell, "CFD split backward overwrote a live point");
        const auto &prior = pointAt(cell + 1);
        const uint64_t now = cycle();
        uint8_t ready = 0;
        if (!ahead) {
            for (unsigned p = 0; p < SmeBackParts.size(); ++p)
                if (now >= line.backwardPartReadyCycle[p])
                    ready |= SmeBackParts[p];
        } else if (prior.cell == cell + 1) {
            for (unsigned p = 0; p < SmeBackParts.size(); ++p)
                if ((prior.returned & SmeBackParts[p]) == SmeBackParts[p] &&
                    now >= prior.partReadyCycle[p])
                    ready |= SmeBackParts[p];
        }
        const auto &feedback = !ahead ? line.backwardNext : prior.value;

        for (unsigned part = 0; part < SmeBackParts.size(); ++part) {
            const unsigned tile = tileAt(cell, part);
            const uint8_t mask = SmeBackParts[part];
            auto &issued = point.issued[part];
            if (!(issued & SmeBackBase) && operand.yReady) {
                Sme::Row values{};
                std::copy(operand.y.begin(), operand.y.end(), values.begin());
                const bool loaded = useBypass ?
                    smeZa.loadBypass(latchAt(cell, N), mask, values, Sme::Backward) :
                    smeZa.load({tile, zaRow(cell, part, N)}, mask, values, Sme::Backward);
                if (loaded) {
                    issued |= SmeBackBase;
                    markBackwardYUse(line, operand);
                    traceSmeSplitBack(line, cell, "base", part);
                }
            }
            // Map all 6 split-backward bypass latches to distinct physical PE rows:
            // slot0->{0,2,5}, slot1->{6,7,4} (disjoint across both slots).
            constexpr std::array<uint8_t, 6> SplitBackBypassRow = {0, 2, 5, 6, 7, 4};
            for (unsigned col = 0; col < N; ++col) {
                const unsigned bit = 1u << col;
                if ((issued & bit) || !(ready & operand.readyColumns & bit))
                    continue;
                Sme::Row values{};
                const auto &column = operand.values[col];
                std::copy(column.begin(), column.end(), values.begin());
                const int bpId = useBypass ? latchAt(cell, col) : -1;
                Sme::Segment2d seg{};
                seg.op = bpId >= 0 ? Sme::Op2d::IngressBypass : Sme::Op2d::IngressRow;
                seg.phase = Sme::Backward;
                const unsigned physRow = bpId >= 0 ?
                    SplitBackBypassRow[unsigned(bpId)] : zaRow(cell, part, col);
                seg.rowMask = static_cast<uint8_t>(1u << physRow);
                seg.colMask = mask;
                seg.dstTile = bpId >= 0 ? unsigned(bpId) : tile;
                seg.dstRow = physRow;
                seg.ingressValues = values;
                seg.scalar = feedback[col];
                offerSme2d(seg, 0, 0, [this, lineTok, cell, part, col, bit]() {
                    auto it = lineRequests.find(lineTok);
                    if (it == lineRequests.end())
                        return;
                    auto &ln = *it->second;
                    auto &pt = smeSplitBack[cell % smeSplitBack.size()];
                    auto &opnd = ln.backwardOperands[cell % env.backwardOperandDepth];
                    pt.issued[part] |= bit;
                    markBackwardColumnUse(ln, opnd, col);
                    traceSmeSplitBack(ln, cell, "issue", part, col);
                });
            }
            for (unsigned i = 0; i < nodes.size(); ++i) {
                const auto &node = nodes[i];
                const unsigned op = N + i;
                const unsigned bit = 1u << op;
                if ((issued & bit) || (issued & node.needs) != node.needs)
                    continue;
                Sme::Segment2d seg{};
                seg.phase = Sme::Backward;
                seg.colMask = mask;
                seg.subtract = node.subtract;
                if (useBypass) {
                    const int dstId = latchAt(cell, node.dst);
                    const int leftId = latchAt(cell, node.left);
                    const int rightId = latchAt(cell, node.right);
                    const unsigned physRow = SplitBackBypassRow[unsigned(dstId)];
                    seg.op = Sme::Op2d::CombineBypass;
                    seg.rowMask = static_cast<uint8_t>(1u << physRow);
                    seg.dstTile = unsigned(dstId);
                    seg.dstRow = physRow;
                    seg.aIsBypass = leftId >= 0;
                    seg.aPos = leftId >= 0 ? Sme::Position{unsigned(leftId), 0} :
                        Sme::Position{tile, zaRow(cell, part, node.left)};
                    seg.bIsBypass = rightId >= 0;
                    seg.bPos = rightId >= 0 ? Sme::Position{unsigned(rightId), 0} :
                        Sme::Position{tile, zaRow(cell, part, node.right)};
                } else {
                    const unsigned dstPhys = zaRow(cell, part, node.dst);
                    seg.op = Sme::Op2d::CombineRow;
                    seg.rowMask = static_cast<uint8_t>(1u << dstPhys);
                    seg.dstTile = tile;
                    seg.dstRow = dstPhys;
                    seg.aPos = {tile, zaRow(cell, part, node.left)};
                    seg.bPos = {tile, zaRow(cell, part, node.right)};
                }
                offerSme2d(seg, 0, 0, [this, lineTok, cell, part, op, bit]() {
                    auto it = lineRequests.find(lineTok);
                    if (it == lineRequests.end())
                        return;
                    auto &ln = *it->second;
                    auto &pt = smeSplitBack[cell % smeSplitBack.size()];
                    pt.issued[part] |= bit;
                    traceSmeSplitBack(ln, cell, "issue", part, op);
                });
            }
        }
    }
}

void
CfdCoeffPreprocessController::commitSme2dBundle()
{
    if (!smeZa.is2d() || sme2dOffers.empty()) {
        sme2dOffers.clear();
        return;
    }
    if (!smeOwner) {
        sme2dOffers.clear();
        return;
    }
    auto ownerIt = lineRequests.find(smeOwner);
    if (ownerIt == lineRequests.end() || ownerIt->second->terminal) {
        sme2dOffers.clear();
        return;
    }

    const unsigned end = env.sme2dCandidateWindow ?
        std::min<unsigned>(sme2dOffers.size(), env.sme2dCandidateWindow) :
        sme2dOffers.size();
    unsigned totalOfferedPes = 0;
    for (unsigned i = 0; i < end; ++i)
        totalOfferedPes += sme2dOffers[i].activePes;
    smeZa.noteOffered2d(totalOfferedPes);

    if (smeZa.queueFull()) {
        auto best = std::min_element(sme2dOffers.begin(), sme2dOffers.begin() + end,
            [](const Sme2dOffer &a, const Sme2dOffer &b) {
                return std::tie(a.critGroup, a.priority, a.orderIndex) <
                       std::tie(b.critGroup, b.priority, b.orderIndex);
            });
        smeZa.noteStall2d(best->seg.phase, CfdSmeZa::Queue);
        sme2dOffers.clear();
        return;
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
        const auto &seg = sme2dOffers[i].seg;
        CfdSmeZa::SegmentReads slices{};
        const unsigned count = smeZa.collectReads2d(seg, slices);
        for (unsigned r = 0; r < count; ++r)
            if (!smeZa.readSliceUsed(slices[r]))
                ops.slices[ops.count++] = slices[r];
        ops.writebackConflict = smeZa.willCollideWriteback2d(seg);
    }

    auto anchorBefore = [&](unsigned a, unsigned b) {
        const auto &oa = sme2dOffers[a];
        const auto &ob = sme2dOffers[b];
        if (mixedKind) {
            return std::make_tuple(oa.critGroup, oa.priority, operands[a].writebackConflict,
                                   -int(oa.activePes), oa.orderIndex) <
                   std::make_tuple(ob.critGroup, ob.priority, operands[b].writebackConflict,
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
        const auto &o = sme2dOffers[i];
        const bool fitsLanes = o.activePes <= availLanes;
        const bool fitsIngress = availIngress || !CfdSmeZa::opUsesIngress(o.seg.op);
        auto &best = fitsLanes && fitsIngress && operands[i].count <= availReads ?
            anchorIdx : blockedIdx;
        if (best == end || anchorBefore(i, best))
            best = i;
    }
    if (anchorIdx == end) {
        const auto &blockedOffer = sme2dOffers[blockedIdx];
        const auto &blocked = blockedOffer.seg;
        const auto stallReason = blockedOffer.activePes > availLanes ? CfdSmeZa::Lanes :
            CfdSmeZa::opUsesIngress(blocked.op) && !availIngress ? CfdSmeZa::Ingress :
            CfdSmeZa::ReadPort;
        smeZa.noteStall2d(blocked.phase, stallReason);
        sme2dOffers.clear();
        return;
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

    std::array<unsigned, 4> chosen{};
    unsigned chosenCount = 0;
    std::vector<bool> usedOffer(sme2dOffers.size(), false);
    chosen[chosenCount++] = anchorIdx;
    usedOffer[anchorIdx] = true;
    const auto &anchor = sme2dOffers[anchorIdx];
    const auto kind = CfdSmeZa::opKind(anchor.seg.op);
    uint8_t usedRows = anchor.seg.rowMask;
    uint8_t usedCols = anchor.seg.colMask;
    unsigned usedLanes = anchor.activePes;
    unsigned usedIngress = CfdSmeZa::opUsesIngress(anchor.seg.op) ? 1u : 0u;
    newReads(anchorIdx, true);

    const auto mode = smeZa.config().arrayMode;
    const unsigned maxSeg = smeZa.config().maxSegments;
    auto fillerKey = [&](unsigned i) {
        const auto &o = sme2dOffers[i];
        return std::make_tuple(o.critGroup, operands[i].writebackConflict,
                               -int(o.activePes), o.priority, o.orderIndex);
    };
    if (mode != CfdSmeZa::ArrayMode::TwoDBase && maxSeg > 1) {
        while (chosenCount < maxSeg && usedRows != 0xFFu) {
            unsigned bestCand = end;
            bool anyRowBlocked = false;
            bool anyColBlocked = false;
            bool anyReadBlocked = false;
            bool anyIngressBlocked = false;

            for (unsigned idx = 0; idx < end; ++idx) {
                if (usedOffer[idx])
                    continue;
                const auto &c = sme2dOffers[idx];
                if (!mixedKind && CfdSmeZa::opKind(c.seg.op) != kind)
                    continue;
                if (CfdSmeZa::opIsBypass(c.seg.op)) {
                    bool dupLatch = false;
                    for (unsigned k = 0; k < chosenCount; ++k) {
                        const auto &prev = sme2dOffers[chosen[k]].seg;
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
                const unsigned needIng = CfdSmeZa::opUsesIngress(c.seg.op) ? 1u : 0u;
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
            const auto &picked = sme2dOffers[bestCand];
            chosen[chosenCount++] = bestCand;
            usedOffer[bestCand] = true;
            usedRows |= picked.seg.rowMask;
            usedCols |= picked.seg.colMask;
            usedLanes += picked.activePes;
            if (CfdSmeZa::opUsesIngress(picked.seg.op))
                ++usedIngress;
            newReads(bestCand, true);
        }
    }

    std::array<CfdSmeZa::Segment2d, 4> bundle{};
    for (unsigned i = 0; i < chosenCount; ++i)
        bundle[i] = sme2dOffers[chosen[i]].seg;
    const bool ok = smeZa.issue2dBundle(bundle.data(), chosenCount);
    panic_if(!ok, "CFD SME 2D validated bundle failed to issue");
    for (unsigned i = 0; i < chosenCount; ++i)
        sme2dOffers[chosen[i]].commit();
    sme2dOffers.clear();
}
