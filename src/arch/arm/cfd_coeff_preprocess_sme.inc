// Controller-owned SME microprograms. D uses the existing Crout format
// (non-unit L, unit U), including the reciprocal-diagonal warm cache.
constexpr uint8_t SmeVectorMask = (1u << N) - 1;

bool
CfdCoeffPreprocessController::wantColdInverse(const LineRequest &line) const
{
    if (!line.coefficientCacheEnabled ||
        env.smeWarmBaseMode == SmeWarmBaseMode::DirectLu)
        return false;
    if (env.smeWarmBaseMode == SmeWarmBaseMode::Adaptive) {
        return env.smeAdaptiveK && env.smeReuseHint >= env.smeAdaptiveK;
    }
    return true;
}

void
CfdCoeffPreprocessController::advanceSmeLu(
    Request &req, SmeCell &cell, unsigned tile, uint64_t now)
{
    using Sme = CfdSmeZa;
    if (req.coefficientCacheReuse && req.smeInvUseCache) {
        while (cell.loaded < N) {
            Sme::Row value{};
            for (unsigned r = 0; r < N; ++r)
                value[r] = req.dInv[r * N + cell.loaded];
            if (!smeZa.load({tile, cell.loaded}, SmeVectorMask, value, Sme::Lu))
                return;
            ++cell.loaded;
        }
        return;
    }
    const auto &input = req.coefficientCacheReuse ? req.lu : req.d;
    const Sme::Position reciprocals{tile, N};
    if (req.coefficientCacheReuse && env.smeWarmVerticalRhs &&
        (env.smeWarmSingleTile || env.smeWarmBypass)) {
        panic_if(!req.cachedReciprocal, "CFD SME warm LU requires reciprocals");
        if (!cell.reciprocalsLoaded) {
            Sme::Row value{};
            std::copy(req.reciprocal.begin(), req.reciprocal.end(), value.begin());
            cell.reciprocalsLoaded =
                smeZa.load(reciprocals, SmeVectorMask, value, Sme::Lu);
        }
        while (cell.loaded < N) {
            Sme::Row value{};
            std::copy_n(input.begin() + cell.loaded * N, N, value.begin());
            if (!smeZa.loadVertical(tile, cell.loaded, SmeVectorMask, value, Sme::Lu))
                return;
            ++cell.loaded;
        }
        return;
    }
    while (cell.loaded < N) {
        Sme::Row value{};
        std::copy_n(input.begin() + cell.loaded * N, N, value.begin());
        const bool loaded = env.smeWarmVerticalRhs && req.coefficientCacheReuse ?
            smeZa.loadVertical(tile, cell.loaded, SmeVectorMask, value, Sme::Lu) :
            smeZa.load({tile, cell.loaded}, SmeVectorMask, value, Sme::Lu);
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
            cell.reciprocalsLoaded =
                smeZa.load(reciprocals, SmeVectorMask, value, Sme::Lu);
        }
        return;
    }
    if (req.luDone)
        return;

    for (unsigned k = 0; k < N; ++k) {
        if (req.forwardStepReady[k] || !smeZa.ready(reciprocals, 1u << k))
            continue;
        req.forwardStepReady[k] = true;
        req.record.luStepReadyCycle[k] = now;
        if (!req.record.luFirstResultCycle)
            req.record.luFirstResultCycle = now;
        trace(req, "lu", "LU_STEP_READY", -1, -1, k, -1, "forward");
    }
    while (cell.pivot < N) {
        const unsigned k = cell.pivot;
        const Sme::Position diagonal{tile, k};
        const uint8_t tail = SmeVectorMask & ~((2u << k) - 1);
        if (!cell.divided) {
            Sme::Row value;
            if (!smeZa.read(diagonal, 1u << k, value, Sme::Lu))
                return;
            const double pivot = value[k];
            if (!std::isfinite(pivot) || std::fabs(pivot) <= req.desc.pivotEpsilon) {
                markFailed(req, CfdCoeffPreprocessStatus::LuFailure, "pivot");
                return;
            }
            if (!smeZa.reciprocal(reciprocals, k, pivot, Sme::Lu))
                return;
            uint64_t pbits = 0;
            std::memcpy(&pbits, &pivot, sizeof(pivot));
            const uint16_t pexp = static_cast<uint16_t>((pbits >> 52) & 0x7FFu);
            cell.pivotExpMin = std::min(cell.pivotExpMin, pexp);
            cell.pivotExpMax = std::max(cell.pivotExpMax, pexp);
            req.record.luDivIssued++;
            cell.divided = true;
            cell.row = k + 1;
        }
        if (tail && !cell.scaled) {
            if (!smeZa.scale(diagonal, tail, reciprocals, k, Sme::Lu))
                return;
            req.record.luMulIssued += N - k - 1;
            cell.scaled = true;
        }
        while (cell.row < N) {
            const Sme::Position target{tile, cell.row};
            if (!smeZa.subtractOuterRow(target, tail, target, k, diagonal,
                                        Sme::Lu))
                return;
            req.record.luMulIssued += N - k - 1;
            req.record.luSubIssued += N - k - 1;
            ++cell.row;
        }
        ++cell.pivot;
        cell.divided = cell.scaled = false;
        if (!env.smeChainSteps)
            break;
    }
    if (cell.pivot < N || !smeZa.ready(reciprocals, SmeVectorMask))
        return;
    Sme::Row value;
    while (cell.exported < N) {
        if (!smeZa.read({tile, cell.exported}, SmeVectorMask, value, Sme::Lu))
            return;
        std::copy_n(value.begin(), N, req.lu.begin() + cell.exported * N);
        ++cell.exported;
    }
    if (!smeZa.read(reciprocals, SmeVectorMask, value, Sme::Lu))
        return;
    std::copy_n(value.begin(), N, req.reciprocal.begin());
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

// For a 5x5 matrix, max-entry exponents bound the infinity-norm
// condition estimate: norm(D)*norm(Dinv) < 2^(eD + eInv + 7).
// Leave headroom for rounding in inverse construction and the warm MVM.
static bool
smeInverseSafe(const SmeCell &cell, bool checkCondition)
{
    const auto &inv = cell.invMagnitude;
    if (!inv.nonzeroFinite())
        return false;
    if (!checkCondition)
        return true;
    const auto &d = cell.dMagnitude;
    return d.nonzeroFinite() && d.exponent() + inv.exponent() + 7 <= 16;
}

void
CfdCoeffPreprocessController::advanceSmeInversePanel(
    Request &req, SmeCell &cell, unsigned luTile, uint64_t now)
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
    if (st.phase == SmePanel::Load) {
        if (st.row == 0) {
            if (!smeZa.idle(invTile))
                return;
            smeZa.release(invTile);
        }
        while (st.row < N) {
            Sme::Row value{};
            value[st.row] = env.smeInvSparseIdentity ? req.reciprocal[st.row] : 1.0;
            if (!smeZa.load({invTile, st.row}, SmeVectorMask, value, Sme::Rhs))
                return;
            ++st.row;
        }
        st.phase = SmePanel::Forward;
        st.step = 0;
        st.row = 1;
        st.scaled = env.smeInvSparseIdentity;
    }
    while (st.phase == SmePanel::Forward) {
        const unsigned k = st.step;
        const uint8_t scaleMask = env.smeInvSparseIdentity ?
            static_cast<uint8_t>((1u << k) - 1u) : SmeVectorMask;
        const uint8_t updateMask = env.smeInvSparseIdentity ?
            static_cast<uint8_t>((1u << (k + 1)) - 1u) : SmeVectorMask;
        if (!st.scaled) {
            if (scaleMask) {
                if (!smeZa.scale({invTile, k}, scaleMask, {luTile, N}, k, Sme::Rhs))
                    return;
                req.record.coeffMulIssued += __builtin_popcount(scaleMask);
            }
            st.scaled = true;
        }
        while (st.row < N) {
            if (!smeZa.subtractOuterRow({invTile, st.row}, updateMask,
                                        {luTile, st.row}, k, {invTile, k}, Sme::Rhs))
                return;
            const unsigned cols = __builtin_popcount(updateMask);
            req.record.coeffMulIssued += cols;
            req.record.coeffSubIssued += cols;
            ++st.row;
        }
        if (k + 1 == N) {
            st.phase = SmePanel::Backward;
            st.step = N - 1;
            st.row = 0;
        } else {
            ++st.step;
            st.row = st.step + 1;
            st.scaled = false;
        }
        if (!env.smeChainSteps)
            return;
    }
    while (st.phase == SmePanel::Backward) {
        const unsigned k = st.step;
        while (st.row < k) {
            if (!smeZa.subtractOuterRow({invTile, st.row}, SmeVectorMask,
                                        {luTile, st.row}, k, {invTile, k}, Sme::Rhs))
                return;
            req.record.coeffMulIssued += N;
            req.record.coeffSubIssued += N;
            ++st.row;
        }
        st.row = 0;
        if (--st.step == 0)
            st.phase = SmePanel::Export;
        if (!env.smeChainSteps)
            return;
    }
    if (st.phase == SmePanel::Export) {
        while (st.row < N) {
            Sme::Row value;
            if (!smeZa.read({invTile, st.row}, SmeVectorMask, value, Sme::Rhs))
                return;
            std::copy_n(value.begin(), N, req.dInv.begin() + st.row * N);
            cell.invMagnitude.includeRow(value.data());
            ++st.row;
        }
        st.phase = SmePanel::Done;
        req.smeInvDone = true;
        // 2048 is the explicit unguarded experiment mode. A nonfinite
        // inverse still cannot be published into the coefficient cache.
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
CfdCoeffPreprocessController::advanceSmeInverseWarmRhs(
    Request &req, SmeCell &cell, unsigned tile, unsigned slotIdx, uint64_t now,
    bool reserveForward)
{
    using Sme = CfdSmeZa;
    auto &st = cell.panels[0];
    if (st.phase == SmePanel::Done)
        return;
    const bool dual = smeUsesDualBypass();
    const unsigned idA = slotIdx;
    const unsigned idB = smeWarmContextCount() + slotIdx;
    auto canIssueFive = [this, reserveForward]() {
        return !reserveForward || smeZa.freeLanes() >= 2 * N;
    };
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
    if (st.phase == SmePanel::Forward) {
        if (!dual) {
            while (cell.invStepA < N) {
                if (!canIssueFive())
                    return;
                const unsigned k = cell.invStepA;
                const bool ok = k == 0 ?
                    smeZa.scaleRowToBypass(idA, SmeVectorMask, {tile, 0},
                                           req.rhsVector[0], Sme::Rhs) :
                    smeZa.fmaRowToBypass(idA, SmeVectorMask, {tile, k},
                                         req.rhsVector[k], Sme::Rhs);
                if (!ok)
                    return;
                req.record.coeffMulIssued += N;
                if (k != 0)
                    req.record.coeffSubIssued += N;
                ++cell.invStepA;
                if (!env.smeChainSteps)
                    return;
            }
            st.phase = SmePanel::Export;
        } else {
            if (cell.invStepA == 0 && canIssueFive()) {
                if (smeZa.scaleRowToBypass(idA, SmeVectorMask, {tile, 0},
                                           req.rhsVector[0], Sme::Rhs)) {
                    req.record.coeffMulIssued += N;
                    ++cell.invStepA;
                }
            }
            while (cell.invStepA >= 1 && cell.invStepA < 3 && canIssueFive()) {
                if (!smeZa.fmaRowToBypass(idA, SmeVectorMask, {tile, cell.invStepA},
                                          req.rhsVector[cell.invStepA], Sme::Rhs))
                    break;
                req.record.coeffMulIssued += N;
                req.record.coeffSubIssued += N;
                ++cell.invStepA;
                if (!env.smeChainSteps)
                    break;
            }
            if (cell.invStepB == 0 && canIssueFive()) {
                if (smeZa.scaleRowToBypass(idB, SmeVectorMask, {tile, 3},
                                           req.rhsVector[3], Sme::Rhs)) {
                    req.record.coeffMulIssued += N;
                    ++cell.invStepB;
                }
            }
            if (cell.invStepB == 1 && canIssueFive()) {
                if (smeZa.fmaRowToBypass(idB, SmeVectorMask, {tile, 4},
                                         req.rhsVector[4], Sme::Rhs)) {
                    req.record.coeffMulIssued += N;
                    req.record.coeffSubIssued += N;
                    ++cell.invStepB;
                }
            }
            if (cell.invStepA == 3 && cell.invStepB == 2 && !cell.invMerged &&
                canIssueFive()) {
                if (smeZa.combineBypass(idA, SmeVectorMask, true, {idA, 0},
                                        true, {idB, 0}, false, Sme::Rhs)) {
                    req.record.coeffSubIssued += N;
                    cell.invMerged = true;
                    st.phase = SmePanel::Export;
                }
            }
        }
    }
    if (st.phase == SmePanel::Export) {
        Sme::Row rhs;
        if (!smeZa.readBypass(idA, SmeVectorMask, rhs, Sme::Rhs))
            return;
        std::copy_n(rhs.begin(), N, req.rhs[0].value.begin());
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
CfdCoeffPreprocessController::advanceSmeVerticalRhs(
    Request &req, SmePanel &state, unsigned luTile, unsigned slotIdx, uint64_t now)
{
    using Sme = CfdSmeZa;
    const Sme::Position dst = env.smeWarmSingleTile ?
        Sme::Position{luTile, N + 1} : Sme::Position{luTile + 1, 0};
    if (state.phase == SmePanel::Load) {
        if (!req.inputOperandsReady && !(env.smeEarlyPanels && req.rhsVectorReady))
            return;
        Sme::Row rhs{};
        std::copy_n(req.rhsVector.begin(), N, rhs.begin());
        const bool loaded = env.smeWarmBypass ?
            smeZa.loadBypass(slotIdx, SmeVectorMask, rhs, Sme::Rhs) :
            smeZa.load(dst, SmeVectorMask, rhs, Sme::Rhs);
        if (!loaded)
            return;
        trace(req, "solve", "SME_RHS_PANEL_LOAD", 0, 1);
        req.solveStarted = true;
        req.record.solveIssueCycle = now;
        req.stage = RequestStage::SolveActive;
        trace(req, "solve", "COEFF3_ISSUE");
        state.phase = SmePanel::Forward;
    }
    while (state.phase == SmePanel::Forward || state.phase == SmePanel::Backward) {
        const unsigned k = state.step;
        const bool forward = state.phase == SmePanel::Forward;
        const uint8_t bit = 1u << k;
        if (forward && !state.scaled) {
            const bool scaled = env.smeWarmBypass ?
                smeZa.scaleBypass(slotIdx, bit, {luTile, N}, k, Sme::Rhs) :
                smeZa.scale(dst, bit, {luTile, N}, k, Sme::Rhs);
            if (!scaled)
                return;
            ++req.record.coeffMulIssued;
            state.scaled = true;
        }
        const uint8_t rows = forward ? SmeVectorMask & ~((bit << 1) - 1) : bit - 1;
        if (rows) {
            // ZA[dst,k] (or the post-rounding active latch) broadcasts on the
            // same outer-product FMA lanes as cold panels. ZA[luTile,k] holds
            // the original column k.
            const bool updated = env.smeWarmBypass ?
                smeZa.subtractOuterBypass(slotIdx, rows, k, {luTile, k}, Sme::Rhs) :
                smeZa.subtractOuterRow(dst, rows, dst, k, {luTile, k}, Sme::Rhs);
            if (!updated)
                return;
            const unsigned count = __builtin_popcount(rows);
            req.record.coeffMulIssued += count;
            req.record.coeffSubIssued += count;
        }
        state.scaled = false;
        if (forward && k + 1 == N)
            state.phase = SmePanel::Backward;
        else if (forward)
            ++state.step;
        else if (k == 1)
            state.phase = SmePanel::Export;
        else
            --state.step;
        if (!env.smeChainSteps)
            return;
    }
    if (state.phase == SmePanel::Export) {
        Sme::Row rhs;
        const bool exported = env.smeWarmBypass ?
            smeZa.readBypass(slotIdx, SmeVectorMask, rhs, Sme::Rhs) :
            smeZa.read(dst, SmeVectorMask, rhs, Sme::Rhs);
        if (!exported)
            return;
        std::copy_n(rhs.begin(), N, req.rhs[0].value.begin());
        state.phase = SmePanel::Done;
        rhsDone(req, 0, now);
    }
}

void
CfdCoeffPreprocessController::advanceSmePanel(
    Request &req, SmePanel &state, unsigned luTile, unsigned panel, uint8_t mask,
    uint64_t now)
{
    using Sme = CfdSmeZa;
    const unsigned tile = luTile + panel + 1;
    const bool defer = panel && req.smeDeferUpper;
    if (!mask || state.phase == SmePanel::Done)
        return;
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
            if (!smeZa.load({tile, state.row}, mask, value, Sme::Rhs))
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
        state.row = 1;
    }
    if (!advanceSmeTriangular(state, tile, luTile, mask,
            defer ? env.smeUpperSplit : N, defer, Sme::Rhs, &req.record))
        return;
    while (state.row < N) {
        Sme::Row value;
        if (!smeZa.read({tile, state.row}, mask, value, Sme::Rhs))
            return;
        if (defer) {
            if (!std::all_of(value.begin(), value.begin() + N,
                            [](double x) { return std::isfinite(x); })) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "nonfinite-upper-panel");
                return;
            }
            std::copy_n(value.begin(), N, req.uBar.begin() + state.row * N);
        } else {
            for (unsigned j = 0; j < Sme::Width; ++j)
                if (mask & (1u << j))
                    req.rhs[panel ? 10 + j : (j ? 4 + j : 0)].value[state.row] = value[j];
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

bool
CfdCoeffPreprocessController::advanceSmeTriangular(
    SmePanel &state, unsigned tile, unsigned luTile, uint8_t mask,
    unsigned forwardEnd, bool stopAfterForward, CfdSmeZa::Phase phase,
    CfdCoeffPreprocessRecord *record)
{
    using Sme = CfdSmeZa;
    const unsigned columns = __builtin_popcount(mask);
    auto factor = [=](unsigned row, unsigned col)
        -> std::pair<Sme::Position, unsigned> {
        if (phase != Sme::ReverseRhs)
            return {{luTile, row}, col};
        const unsigned split = env.smeUpperSplit;
        unsigned index;
        if (row == N) {
            index = col-split;
        } else if (row > col) {
            const unsigned localRow = row-split;
            index = N-split + localRow*(localRow-1)/2 + col-split;
        } else {
            index = SmeUpperFactors::upperIndex(row, col, split);
        }
        return {{tile, N + index / Sme::Width}, index % Sme::Width};
    };
    auto update = [&](unsigned row, unsigned k) {
        const auto [source, col] = factor(row, k);
        if (!smeZa.subtractOuterRow({tile, row}, mask, source, col,
                                    {tile, k}, phase))
            return false;
        if (record) {
            record->coeffMulIssued += columns;
            record->coeffSubIssued += columns;
        }
        return true;
    };
    while (state.phase == SmePanel::Forward) {
        const unsigned k = state.step;
        // Both panels can use a returned pivot before the rest of D is
        // factored. The masks wait only for the consumed factor elements.
        if (!state.scaled) {
            const auto [source, col] = factor(N, k);
            if (!smeZa.scale({tile, k}, mask, source, col, phase))
                return false;
            if (record)
                record->coeffMulIssued += columns;
            state.scaled = true;
        }
        while (state.row < N) {
            if (!update(state.row, k))
                return false;
            ++state.row;
        }
        if (k + 1 == forwardEnd) {
            state.phase = stopAfterForward ? SmePanel::Export : SmePanel::Backward;
            state.row = 0;
        } else {
            ++state.step;
            state.row = state.step + 1;
            state.scaled = false;
        }
        if (!env.smeChainSteps)
            return false;
    }
    while (state.phase == SmePanel::Backward) {
        const unsigned k = state.step;
        while (state.row < k) {
            if (!update(state.row, k))
                return false;
            ++state.row;
        }
        state.row = 0;
        if (--state.step == 0)
            state.phase = SmePanel::Export;
        if (!env.smeChainSteps)
            return false;
    }
    return state.phase == SmePanel::Export;
}

void
CfdCoeffPreprocessController::bindSmeCachedLu(Request &req, uint64_t now)
{
    req.stage = RequestStage::LuReady;
    req.forwardStepReady.fill(true);
    req.backwardStepReady.fill(true);
    std::fill_n(req.record.luStepReadyCycle, N, now);
    trace(req, "lu", "LU_CACHE_REUSE");
    tracePrivateLu(req);
    if (req.smeInvUseCache)
        trace(req, "lu", "SME_INV_CACHE_REUSE");
    else
        trace(req, "lu", "LU_RECIPROCAL_REUSE");
    if (req.smeInvRejected)
        ++lineRequests.at(smeOwner)->smeInvFallbackCells;
}

#include "cfd_coeff_preprocess_sme_warm.inc"
#include "cfd_coeff_preprocess_sme_split_back.inc"
#include "cfd_coeff_preprocess_sme_2d.inc"

void
CfdCoeffPreprocessController::processSmeCriticalRhs(uint64_t now)
{
    if (smeZa.is2d()) {
        processSmeCriticalRhs2d(now);
        return;
    }
    if (!smeOwner || !env.smeWarmVerticalRhs)
        return;
    const auto it = lineRequests.find(smeOwner);
    if (it == lineRequests.end() || it->second->terminal ||
        !it->second->coefficientCacheHit || it->second->backwardStarted)
        return;
    auto &pool = env.smeWarmSingleTile ? smeWarmSingleCells : smeCells;
    const unsigned stride = env.smeWarmSingleTile ? 1 : 3;
    const uint64_t horizon = smeForwardCells + 3;
    auto nextLanes = [](const SmePanel &st) -> unsigned {
        if (st.phase == SmePanel::Load || st.phase == SmePanel::Export ||
            st.phase == SmePanel::Done)
            return 0;
        if (st.phase == SmePanel::Forward)
            return !st.scaled ? 1u : (N - 1 - st.step);
        return st.step;
    };
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
        advanceSmeLu(req, *slot, tile, now);
        if (req.stage == RequestStage::Failed)
            continue;
        if (req.smeInvUseCache) {
            advanceSmeInverseWarmRhs(req, *slot, tile, idx, now, true);
        } else {
            const unsigned need = nextLanes(slot->panels[0]);
            if (need && smeZa.freeLanes() < N + need)
                continue;
            advanceSmeVerticalRhs(req, slot->panels[0], tile, idx, now);
        }
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
CfdCoeffPreprocessController::processSmeRequests(uint64_t now)
{
    if (smeZa.is2d()) {
        processSmeRequests2d(now);
        return;
    }
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
            processSmeWarmRhs(line, now);
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
            advanceSmeLu(req, *slot, idx, now);
            if (req.stage == RequestStage::Failed)
                continue;
            if (req.smeInvUseCache)
                advanceSmeInverseWarmRhs(req, *slot, idx, idx, now);
            else
                advanceSmeVerticalRhs(req, slot->panels[0], idx, idx, now);
            if (req.luDone && req.stage != RequestStage::Failed) {
                if (req.inputOperandsReady)
                    finalizeSolveRequest(req, now);
                if (req.solveDone)
                    releaseSingle(*slot, idx);
            }
        }
        return;
    }

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
        advanceSmeLu(req, *slot, tile, now);
        if (req.stage == RequestStage::Failed)
            continue;
        const uint8_t lower = (req.rhsEnabledMask >> 4) & (SmeVectorMask << 1);
        if (req.coefficientCacheReuse && req.smeInvUseCache)
            advanceSmeInverseWarmRhs(req, *slot, tile, idx, now);
        else if (env.smeWarmVerticalRhs && req.coefficientCacheReuse)
            advanceSmeVerticalRhs(req, slot->panels[0], tile, idx, now);
        else
            advanceSmePanel(req, slot->panels[0], tile, 0,
                            env.smeSplitRhs ? 1u : 1u | lower, now);
        if (req.stage == RequestStage::Failed)
            continue;
        if (env.smeSplitRhs)
            advanceSmePanel(req, slot->panels[2], tile, 0, lower, now);
        if (req.stage == RequestStage::Failed)
            continue;
        const uint8_t upper = req.smeDeferUpper ? SmeVectorMask :
            (req.rhsEnabledMask >> 10) & SmeVectorMask;
        advanceSmePanel(req, slot->panels[1], tile, 1, upper, now);
        const bool cellInvReady = !req.smeInvRequested || req.smeInvDone;
        if (req.luDone && req.stage != RequestStage::Failed &&
            (!req.solveDone || cellInvReady)) {
            if (req.inputOperandsReady)
                finalizeSolveRequest(req, now);
            if (req.solveDone && cellInvReady)
                release(*slot, idx, tile);
        }
    }

    // Pass 2 (low-priority background): build D^-1 on reused tile+1 without
    // delaying LU, [R|L], or forward sweep on any active context.
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
            const unsigned tile = 3 * idx;
            advanceSmeInversePanel(*req, cell, tile, now);
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
CfdCoeffPreprocessController::advanceSmeScan(
    SmeScan &scan, unsigned tile, CfdSmeZa::Phase phase,
    const std::array<double, N> &feedback, const std::array<double, N> *base,
    uint64_t now, CfdSmeZa::Row &result)
{
    if (smeZa.is2d())
        return advanceSmeScan2d(scan, tile, phase, feedback, base, now, result);
    constexpr unsigned BaseRow = N;
    if (base && !scan.baseLoaded) {
        CfdSmeZa::Row value{};
        std::copy(base->begin(), base->end(), value.begin());
        scan.baseLoaded = smeZa.load({tile, BaseRow}, SmeVectorMask, value, phase);
    }
    for (unsigned k = 0; k < N; ++k) {
        const unsigned bit = 1u << k;
        if ((scan.loaded & bit) && !(scan.products & bit) &&
            smeZa.scale({tile, k}, SmeVectorMask, feedback[k], phase))
            scan.products |= bit;
    }

    // Three levels: (p0+p1), (p2+p3), (base-p4) run independently.
    // Only the last write makes the next cell's feedback visible.
    auto combine = [this, &scan, tile, phase](
            unsigned node, unsigned dst, unsigned a, unsigned b, bool subtract) {
        if (!(scan.nodes & (1u << node)) &&
            smeZa.combine({tile, dst}, SmeVectorMask, {tile, a}, {tile, b},
                          subtract, phase))
            scan.nodes |= 1u << node;
    };
    if ((scan.products & 0x03) == 0x03)
        combine(0, 0, 0, 1, false);
    if ((scan.products & 0x0c) == 0x0c)
        combine(1, 2, 2, 3, false);
    if ((scan.products & 0x10) && scan.baseLoaded)
        combine(2, BaseRow, BaseRow, 4, true);
    if ((scan.nodes & 0x03) == 0x03)
        combine(3, 0, 0, 2, false);
    if ((scan.nodes & 0x0c) == 0x0c && !(scan.nodes & 0x10)) {
        combine(4, BaseRow, BaseRow, 0, true);
        if (scan.nodes & 0x10)
            scan.finalIssue = now;
    }
    return (scan.nodes & 0x10) &&
        smeZa.read({tile, BaseRow}, SmeVectorMask, result, phase);
}

void
CfdCoeffPreprocessController::stageSmeScanColumn(
    SmeScan &scan, unsigned tile, unsigned column, const CfdSmeZa::Row &value,
    double feedback, CfdSmeZa::Phase phase)
{
    if (smeZa.is2d()) {
        stageSmeScanColumn2d(scan, tile, column, value, feedback, phase);
        return;
    }
    const unsigned bit = 1u << column;
    if (env.smeConfig.ingressPorts) {
        if (smeZa.ingressProduct({tile, column}, SmeVectorMask, value,
                                 feedback, phase)) {
            scan.loaded |= bit;
            scan.products |= bit;
        }
    } else if (smeZa.load({tile, column}, SmeVectorMask, value, phase)) {
        scan.loaded |= bit;
    }
}

bool
CfdCoeffPreprocessController::processSmeForward(uint64_t now)
{
    auto &scan = smeForward;
    const unsigned tile = 3 * smeCells.size();
    Request *req = scan.owner ? find(scan.owner) : nullptr;
    if (scan.owner && (!req || req->stage == RequestStage::Failed)) {
        if (smeZa.idle(tile)) {
            smeZa.release(tile);
            scan = {};
        }
        return false;
    }
    if (!req) {
        for (Request *candidate : admissionOrder()) {
            if (candidate->stage != RequestStage::Failed &&
                candidate->desc.cellId && candidate->previousDqReady &&
                candidate->lbarColumnReadyMask && !candidate->dqStarReadyLocal &&
                cachedRegistersReady(*candidate)) {
                req = candidate;
                scan.owner = req->token;
                scan.cell = req->desc.cellId;
                break;
            }
        }
    }
    if (!req)
        return false;
    for (unsigned k = 0; k < N; ++k) {
        const unsigned bit = 1u << k;
        if (!(req->lbarColumnReadyMask & bit) || (scan.loaded & bit))
            continue;
        CfdSmeZa::Row value{};
        for (unsigned row = 0; row < N; ++row)
            value[row] = req->lBar[row * N + k];
        stageSmeScanColumn(scan, tile, k, value, req->previousDq[k],
                           CfdSmeZa::Forward);
    }
    CfdSmeZa::Row result;
    const auto *base = req->baseComputeComplete ? &req->baseAccumulator : nullptr;
    if (!advanceSmeScan(scan, tile, CfdSmeZa::Forward, req->previousDq,
                        base, now, result))
        return false;
    std::copy_n(result.begin(), N, req->dqStar.begin());
    req->correctionComplete = req->combineIssued = req->combineComplete = true;
    req->dqStarReadyLocal = true;
    auto &record = req->record;
    req->lbarColumnConsumedMask = SmeVectorMask;
    record.lbarConsumedColumnMask = SmeVectorMask;
    record.lbarColumnsIssued = record.lbarColumnsConsumed = N;
    record.lbarColumnsCompleted = N;
    record.correctionAccumulatorsCompleted = 1;
    record.correctionReadyCycle = record.forwardCombineIssueCycle = scan.finalIssue;
    record.forwardCombineIssued = record.forwardCombineCompleted = 1;
    record.forwardCombineCompleteCycle = record.dqStarReadyCycle = now;
    trace(*req, "forward-combine", "SME_FORWARD_COMPLETE");
    smeZa.release(tile);
    scan = {};
    produceForwardHandoff(*req, now);
    return true;
}

bool
CfdCoeffPreprocessController::processSmeBackward(LineRequest &line, uint64_t now)
{
    if (now < line.backwardFeedbackReadyCycle)
        return false;
    auto &scan = smeBackward;
    const unsigned tile = 3 * smeCells.size() + 1;
    const uint64_t cell = line.backwardCell;
    if (!scan.owner) {
        scan.owner = line.token;
        scan.cell = cell;
    }
    panic_if(scan.owner != line.token || scan.cell != cell,
             "CFD SME stale backward ZA owner");
    for (unsigned k = 0; k < N; ++k) {
        const unsigned bit = 1u << k;
        if ((scan.loaded & bit) || !onchipColumnReady(line, 1, k))
            continue;
        CfdSmeZa::Row value{};
        const auto &column = line.mvm[1].columnOperands[k];
        std::copy(column.begin(), column.end(), value.begin());
        stageSmeScanColumn(scan, tile, k, value, line.backwardNext[k],
                           CfdSmeZa::Backward);
    }
    const bool yReady = env.backwardIndependentReady ? backwardYReady(line) :
        scan.loaded != 0;
    CfdSmeZa::Row result;
    if (!advanceSmeScan(scan, tile, CfdSmeZa::Backward, line.backwardNext,
                        yReady ? &line.backwardYOperand : nullptr, now, result))
        return false;
    std::copy_n(result.begin(), N, line.backwardAccumulator.begin());
    line.lineProducts += N;
    line.lineAdds += N;
    smeZa.release(tile);
    scan = {};
    finishLineBackwardCell(line, true);
    return true;
}

bool
CfdCoeffPreprocessController::advanceSmeReverseWavefront(
    SmeReversePanel &panel, unsigned tile)
{
    using Sme = CfdSmeZa;
    auto &state = panel.state;
    if (!panel.upperStarted) {
        if (!advanceSmeTriangular(state, tile, tile, SmeVectorMask, N, true,
                                  Sme::ReverseRhs, nullptr))
            return false;
        state.phase = SmePanel::Backward;
        panel.upperStarted = true;
    }
    if (state.phase != SmePanel::Backward)
        return state.phase == SmePanel::Export;

    // A row keeps its FP64 subtraction order. A finalized pivot can be
    // consumed while other rows still have independent updates in flight.
    for (int row = N-2; row >= 0; --row) {
        auto &col = panel.nextUpper[row];
        if (col == row || panel.nextUpper[col] != col)
            continue;
        const unsigned index = SmeUpperFactors::upperIndex(row, col, env.smeUpperSplit);
        if (smeZa.subtractOuterRow({tile, unsigned(row)}, SmeVectorMask,
                {tile, N + index/Sme::Width}, index%Sme::Width, {tile, col},
                Sme::ReverseRhs))
            --col;
    }
    for (unsigned row = 0; row+1 < N; ++row)
        if (panel.nextUpper[row] != row)
            return false;
    state.phase = SmePanel::Export;
    state.row = 0;
    return true;
}

void
CfdCoeffPreprocessController::traceSmeUpperMatrix(
    const LineRequest &line, uint64_t cell, const char *kind,
    const std::array<double, N*N> &matrix) const
{
    if (!env.traceEnable)
        return;
    std::fprintf(stderr, "CFD_SME_UPPER_DATA token=%llu cell=%llu kind=%s values=",
        (unsigned long long)line.token, (unsigned long long)cell, kind);
    for (unsigned i = 0; i < matrix.size(); ++i)
        std::fprintf(stderr, "%s%a", i ? "," : "", matrix[i]);
    std::fputc('\n', stderr);
}

bool
CfdCoeffPreprocessController::advanceSmeReversePanel(
    LineRequest &line, BackwardOperand &operand, SmeReversePanel &panel,
    unsigned tile)
{
    using Sme = CfdSmeZa;
    auto &state = panel.state;
    if (state.phase == SmePanel::Load) {
        for (; panel.loaded < N+2; ++panel.loaded) {
            Sme::Row value{};
            unsigned words = N;
            if (panel.loaded < N) {
                std::copy_n(operand.values[panel.loaded].begin(), words, value.begin());
            } else {
                const unsigned offset = (panel.loaded-N)*Sme::Width;
                words = std::min(Sme::Width, smeUpperFactorWords()-offset);
                std::copy_n(operand.smeFactors.value.begin() + offset, words, value.begin());
            }
            if (!smeZa.load({tile, panel.loaded}, (1u << words)-1, value,
                            Sme::ReverseRhs))
                return false;
        }
        panel.upperStarted = env.smeUpperSplit == N;
        panel.nextUpper.fill(N-1);
        state.phase = panel.upperStarted ? SmePanel::Backward : SmePanel::Forward;
        state.step = panel.upperStarted ? N-1 : env.smeUpperSplit;
        state.row = panel.upperStarted ? 0 : state.step+1;
    }
    if (state.phase != SmePanel::Done) {
        const bool exportReady = env.smeReverseWavefront ?
            advanceSmeReverseWavefront(panel, tile) :
            advanceSmeTriangular(state, tile, tile, SmeVectorMask, N, false,
                                 Sme::ReverseRhs, nullptr);
        if (!exportReady)
            return false;
        for (; state.row < N; ++state.row) {
            Sme::Row row;
            if (!smeZa.read({tile, state.row}, SmeVectorMask, row, Sme::ReverseRhs))
                return false;
            for (unsigned col = 0; col < N; ++col) {
                if (!std::isfinite(row[col])) {
                    abortLine(line, CfdCoeffPreprocessStatus::InternalError);
                    return false;
                }
                operand.values[col][state.row] = row[col];
            }
        }
        state.phase = SmePanel::Done;
    }
    uint64_t storeId = 0;
    if (line.coefficientCacheEnabled) {
        // Only a reusable version needs a finalized SRAM copy. A cold-only
        // sweep consumes the exported columns from the same operand slot.
        auto &store = operand.columns[1];
        if (!store)
            store = startOnchip(line.token, smeUpperAddr(line, panel.cell),
                                N*N, 8, true, 1);
        if (!store)
            return false;
        // The on-chip job and its destination retain the write. The cold
        // feedback uses the exported registers, and the next generation
        // can see the cache only after all stores have retired.
        auto &upper = lineCoeffCaches.at(line.desc.lineId).uBar[panel.cell];
        for (unsigned row = 0; row < N; ++row)
            for (unsigned col = 0; col < N; ++col)
                upper[row*N+col] = operand.values[col][row];
        storeId = store->id;
    }
    operand.readyColumns = SmeVectorMask;
    ++line.smeReverseDone;
    if (env.traceEnable) {
        std::array<double, N*N> matrix;
        for (unsigned row = 0; row < N; ++row)
            for (unsigned col = 0; col < N; ++col)
                matrix[row*N+col] = operand.values[col][row];
        traceSmeUpperMatrix(line, panel.cell, "ubar", matrix);
        std::fprintf(stderr,"CFD_SME_UPPER_MEMORY cycle=%llu token=%llu cell=%llu "
            "event=finish job=%llu rewrite=%llu words=%u\n",
            (unsigned long long)cycle(), (unsigned long long)line.token,
            (unsigned long long)panel.cell, (unsigned long long)operand.columns[0]->id,
            (unsigned long long)storeId, N*N + smeUpperFactorWords());
    }
    return true;
}

void
CfdCoeffPreprocessController::processSmeReversePanels()
{
    const unsigned base = 3 * smeCells.size() + 2;
    for (unsigned i = 0; i < smeReversePanels.size(); ++i) {
        auto &panel = smeReversePanels[i];
        if (!panel.owner)
            continue;
        auto line = lineRequests.find(panel.owner);
        if ((line == lineRequests.end() || line->second->terminal) &&
            smeZa.idle(base + i)) {
            smeZa.release(base + i);
            panel = {};
        }
    }
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        if (line.terminal || !line.deferUpper || !line.backwardStarted)
            continue;
        // Nearer points get the first lane and port requests after feedback.
        for (unsigned ahead = 0; ahead < env.backwardOperandDepth; ++ahead) {
            const int64_t cell = line.backwardCell - ahead;
            if (cell < 0)
                break;
            auto &operand = line.backwardOperands[cell % env.backwardOperandDepth];
            if (operand.cell != uint64_t(cell) || !operand.smePackedReady ||
                operand.readyColumns)
                continue;
            auto it = std::find_if(smeReversePanels.begin(), smeReversePanels.end(),
                [&](const auto &p) { return p.owner == token && p.cell == uint64_t(cell); });
            if (it == smeReversePanels.end()) {
                it = std::find_if(smeReversePanels.begin(), smeReversePanels.end(),
                    [](const auto &p) { return !p.owner; });
                if (it == smeReversePanels.end())
                    continue;
                it->owner = token;
                it->cell = cell;
            }
            const unsigned panelIdx = std::distance(smeReversePanels.begin(), it);
            const unsigned tile = base + panelIdx;
            const bool done = smeZa.is2d() ?
                advanceSmeReversePanel2d(line, operand, *it, panelIdx, tile) :
                advanceSmeReversePanel(line, operand, *it, tile);
            if (done) {
                smeZa.release(tile);
                *it = {};
            }
            if (line.terminal)
                break;
        }
    }
}

void
CfdCoeffPreprocessController::sampleSme(const LineRequest &line)
{
    using Sme = CfdSmeZa;
    smeZa.sample(Sme::Sweep);
    if (line.cells < 17 || line.terminal)
        return;
    // Share a stable wall-clock denominator across the overlapping LU, RHS
    // and forward kernels. The reverse window is independent of these.
    const auto margin = line.cells / 8;
    if (line.backwardStarted) {
        if (line.backwardCell >= int64_t(margin) &&
            line.backwardCell < int64_t(line.cells - margin))
            smeZa.sample(Sme::BackSteady);
    } else if (smeForwardCells >= margin && smeForwardCells < line.cells - margin) {
        smeZa.sample(Sme::PrepSteady);
    }
}

void
CfdCoeffPreprocessController::reportSme(
    const LineRequest &line, uint64_t now, bool aborted)
{
    using Sme = CfdSmeZa;
    const auto &s = env.smeConfig;
    auto q = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    panic_if(smeOwner != line.token || !smeZa.empty(),
             "CFD SME retirement without a drained owner");
    panic_if(std::any_of(smeWarmRhs.begin(), smeWarmRhs.end(),
                         [](const auto &group) { return group.count; }),
             "CFD SME retirement left a live packed RHS group");
    panic_if(std::any_of(smeWarmSingleCells.begin(), smeWarmSingleCells.end(),
                         [](const auto &cell) { return cell.token != 0; }),
             "CFD SME retirement left a live single-tile warm context");
    panic_if(std::any_of(smeSplitBack.begin(), smeSplitBack.end(),
                         [](const auto &point) { return point.cell != UINT64_MAX; }),
             "CFD SME retirement left a live split backward point");
    sampleSme(line);
    smeOwner = 0;
    std::fprintf(stderr, "CFD_SME_CONFIG token=%llu start=%llu done=%llu "
        "aborted=%d lanes=%u reads=%u writes=%u contexts=%u depth=%u peak=%u ingress=%u "
        "za_bytes=%u reserved=%llu fma_latency=%u mul_latency=%u add_latency=%u "
        "div_latency=%u div_ii=%u early_panels=%d chain_steps=%d split_rhs=%d "
        "reverse_contexts=%u upper_split=%u reverse_wavefront=%d "
        "local_ubar=%d defer_upper=%d upper_base=%llu "
        "upper_bytes=%llu upper_stored=%llu reverse_done=%llu "
        "warm_rhs_width=%u warm_rhs_groups=%zu warm_back_split=%d warm_vertical_rhs=%d "
        "warm_single_tile=%d warm_bypass=%d warm_critical_arb=%d "
        "warm_base_mode=%u inv_sparse_i5=%d inv_exp_thresh=%llu inv_reuse_hint=%llu "
        "inv_cells=%llu inv_fallback_cells=%llu inv_built_cells=%llu "
        "inv_packed_cells=%llu inv_discarded_mul=%llu inv_discarded_fma=%llu "
        "inv_adaptive_k=%llu inv_policy_enabled=%d cache_tag_bytes=%u "
        "array_mode=%u segments_max=%u panel_pack=%d mixed_kind=%d "
        "panel_b_overlap=%d forward_tile_rotate=%d candidate_window=%u feedback_reg=%d "
        "return_slot_bytes=%u\n",
        q(line.token), q(smeStart), q(now),
        aborted, s.lanes, s.readPorts, s.writePorts, env.smeContexts, s.depth,
        smeZa.peakReturns(), s.ingressPorts,
        (3 * env.smeContexts + 2 + env.smeReverseContexts) * 512,
        q(Sme::reservedBytes(env.smeContexts, s.depth, s.ingressPorts,
                             env.smeReverseContexts, env.smeWarmRhsWidth != 0,
                             env.smeWarmBackSplit, env.smeWarmSingleTile,
                             env.smeWarmBypass, env.smeCriticalArb,
                             static_cast<unsigned>(env.smeWarmBaseMode),
                             static_cast<unsigned>(s.arrayMode), s.maxSegments,
                             s.mixedKind, s.panelPack)),
        s.fmaLatency,
        s.mulLatency, s.addLatency, s.divLatency, s.divIi,
        env.smeEarlyPanels, env.smeChainSteps, env.smeSplitRhs, env.smeReverseContexts,
        env.smeUpperSplit, env.smeReverseWavefront,
        line.localUbar, line.deferUpper, q(line.smeUpperBase), q(line.smeUpperBytes),
        q(line.smeUpperStored), q(line.smeReverseDone),
        env.smeWarmRhsWidth, smeWarmRhs.size(), env.smeWarmBackSplit, env.smeWarmVerticalRhs,
        env.smeWarmSingleTile, env.smeWarmBypass, env.smeCriticalArb,
        static_cast<unsigned>(env.smeWarmBaseMode), env.smeInvSparseIdentity,
        q(env.smeInvExpThresh), q(env.smeReuseHint),
        q(line.smeInvCells), q(line.smeInvFallbackCells), q(line.smeInvBuiltCells),
        q(line.smePackedInvCells), q(line.smeInvDiscardedMul), q(line.smeInvDiscardedFma),
        q(env.smeAdaptiveK), line.smeInversePolicy,
        line.cacheAllocated ? smeCacheTagBytes() : 0,
        static_cast<unsigned>(s.arrayMode), s.maxSegments, s.panelPack, s.mixedKind,
        env.smePanelBOverlap, env.smeForwardTileRotate, env.sme2dCandidateWindow,
        env.smeFeedbackReg,
        Sme::returnSlotBytes(static_cast<unsigned>(s.arrayMode)));
    if (smeZa.is2d()) {
        const auto &s2d = smeZa.statistics2d();
        const uint64_t sweepCycles = now - smeStart + 1;
        const uint64_t peCap = sweepCycles * 64ull;
        const uint64_t peIssueCap = s2d.bundlesTotal * 64ull;
        std::fprintf(stderr, "CFD_SME_2D_STATS token=%llu array_mode=%u "
            "segments_max=%u bundles_total=%llu bundles_seg1=%llu "
            "bundles_seg2=%llu bundles_seg3=%llu bundles_seg4=%llu "
            "pe_active_sum=%llu pe_capacity=%llu pe_issue_capacity=%llu "
            "row_fill_hist=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
            "pack_stall_row=%llu pack_stall_col=%llu pack_stall_readport=%llu "
            "pack_stall_ingress=%llu fmopa_bundles=%llu fmopa_pe_active=%llu\n",
            q(line.token), static_cast<unsigned>(s.arrayMode), s.maxSegments,
            q(s2d.bundlesTotal), q(s2d.bundlesBySeg[0]), q(s2d.bundlesBySeg[1]),
            q(s2d.bundlesBySeg[2]), q(s2d.bundlesBySeg[3]),
            q(s2d.peActiveSum), q(peCap), q(peIssueCap),
            q(s2d.rowFillHist[0]), q(s2d.rowFillHist[1]), q(s2d.rowFillHist[2]),
            q(s2d.rowFillHist[3]), q(s2d.rowFillHist[4]), q(s2d.rowFillHist[5]),
            q(s2d.rowFillHist[6]), q(s2d.rowFillHist[7]),
            q(s2d.packStallRow), q(s2d.packStallCol), q(s2d.packStallReadPort),
            q(s2d.packStallIngress), q(s2d.fmopaBundles), q(s2d.fmopaPeActive));
    }
    constexpr const char *names[] = {"lu", "rhs", "forward", "backward", "reverse_rhs"};
    for (unsigned i = 0; i < Sme::PhaseCount; ++i) {
        const auto &p = smeZa.statistics(static_cast<Sme::Phase>(i));
        std::fprintf(stderr, "CFD_SME_PHASE token=%llu phase=%s first=%llu "
            "last=%llu issue_cycles=%llu beats=%llu fma=%llu mul=%llu add=%llu div=%llu "
            "fma_returned=%llu mul_returned=%llu add_returned=%llu div_returned=%llu "
            "za_reads=%llu za_writes=%llu loaded_words=%llu ingress_words=%llu "
            "dependency=%llu read_port=%llu write_port=%llu lanes=%llu "
            "queue=%llu divider=%llu ingress_wait=%llu "
            "return_wait=%llu return_blocked_cycles=%llu "
            "vertical_loads=%llu bypass_reads=%llu bypass_writes=%llu\n",
            q(line.token), names[i], q(p.first), q(p.last),
            q(p.issueCycles), q(p.beats), q(p.issued[Sme::Fma]), q(p.issued[Sme::Mul]),
            q(p.issued[Sme::Add]), q(p.issued[Sme::Div]), q(p.returned[Sme::Fma]),
            q(p.returned[Sme::Mul]), q(p.returned[Sme::Add]), q(p.returned[Sme::Div]),
            q(p.zaReads), q(p.zaWrites), q(p.loadWords), q(p.ingressWords),
            q(p.stalls[Sme::Dependency]),
            q(p.stalls[Sme::ReadPort]), q(p.stalls[Sme::WritePort]),
            q(p.stalls[Sme::Lanes]), q(p.stalls[Sme::Queue]), q(p.stalls[Sme::Divider]),
            q(p.stalls[Sme::Ingress]), q(p.returnWait), q(p.returnBlockedCycles),
            q(p.verticalLoads),
            q(p.bypassReads), q(p.bypassWrites));
    }
    constexpr const char *windows[] = {"sweep", "prep_steady", "back_steady"};
    for (unsigned i = 0; i < Sme::WindowCount; ++i) {
        const auto &w = smeZa.statistics(static_cast<Sme::Window>(i));
        std::fprintf(stderr, "CFD_SME_WINDOW token=%llu window=%s cycles=%llu "
            "full=%llu partial=%llu ready_blocked=%llu dependency_only=%llu no_attempt=%llu "
            "lu=%llu rhs=%llu forward=%llu backward=%llu reverse_rhs=%llu "
            "offered=%llu capped_offer=%llu "
            "pending_sum=%llu queue_full=%llu return_blocked=%llu\n",
            q(line.token), windows[i], q(w.cycles), q(w.states[Sme::Full]),
            q(w.states[Sme::Partial]), q(w.states[Sme::ReadyBlocked]),
            q(w.states[Sme::DependencyOnly]), q(w.states[Sme::NoAttempt]),
            q(w.issued[Sme::Lu]), q(w.issued[Sme::Rhs]), q(w.issued[Sme::Forward]),
            q(w.issued[Sme::Backward]), q(w.issued[Sme::ReverseRhs]),
            q(w.offered), q(w.cappedOffer),
            q(w.pendingSum), q(w.fullQueueCycles), q(w.blockedReturnCycles));
    }
}
