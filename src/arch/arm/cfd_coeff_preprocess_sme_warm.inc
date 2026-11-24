// Cache-hit base solves share ZA rows across independent points. Each lane
// keeps the scalar Crout update order and its own cached factor.
struct SmeWarmRhsOp
{
    uint8_t row, pivot;
};

constexpr auto SmeWarmRhsProgram = [] {
    std::array<SmeWarmRhsOp, N * N> ops{};
    unsigned pc = 0;
    for (uint8_t k = 0; k < N; ++k) {
        ops[pc++] = {k, k};
        for (uint8_t row = k + 1; row < N; ++row)
            ops[pc++] = {row, k};
    }
    for (uint8_t k = N - 1; k > 0; --k)
        for (uint8_t row = 0; row < k; ++row)
            ops[pc++] = {row, k};
    return ops;
}();
static_assert(N + SmeWarmRhsProgram.size() <=
              CfdSmeZa::WarmRhsTiles * CfdSmeZa::Width);

void
CfdCoeffPreprocessController::traceSmeWarmRhs(
    const LineRequest &line, unsigned group, const char *event, unsigned index) const
{
    if (!env.traceEnable)
        return;
    const auto &batch = smeWarmRhs[group];
    std::fprintf(stderr, "CFD_SME_WARM_RHS token=%llu cycle=%llu group=%u "
        "first=%llu count=%u event=%s index=%u\n",
        static_cast<unsigned long long>(line.token),
        static_cast<unsigned long long>(cycle()), group,
        static_cast<unsigned long long>(batch.first), batch.count, event, index);
}

bool
CfdCoeffPreprocessController::releaseSmeWarmRhs(
    LineRequest &line, unsigned group, const char *event)
{
    const unsigned firstTile = group * CfdSmeZa::WarmRhsTiles;
    for (unsigned i = 0; i < CfdSmeZa::WarmRhsTiles; ++i)
        if (!smeZa.idle(firstTile + i))
            return false;
    for (unsigned i = 0; i < CfdSmeZa::WarmRhsTiles; ++i)
        smeZa.release(firstTile + i);
    traceSmeWarmRhs(line, group, event);
    smeWarmRhs[group] = {};
    return true;
}

void
CfdCoeffPreprocessController::advanceSmeWarmRhs(
    LineRequest &line, unsigned group, bool &gather, uint64_t now)
{
    using Sme = CfdSmeZa;
    auto &batch = smeWarmRhs[group];
    const uint8_t mask = (1u << batch.count) - 1;
    auto position = [group](unsigned row) -> Sme::Position {
        return {group * Sme::WarmRhsTiles + row / Sme::Width, row % Sme::Width};
    };
    std::array<Request *, N> points{};
    for (unsigned lane = 0; lane < batch.count; ++lane) {
        points[lane] = find(batch.requests[lane]);
        panic_if(!points[lane] || points[lane]->stage == RequestStage::Failed,
                 "CFD packed warm RHS lost a live point");
    }

    // One packed row may cross the request operand banks per tick. Its ZA
    // write uses the normal port and load-to-read scoreboard interlock.
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

    while (batch.issued < SmeWarmRhsProgram.size() && batch.loaded > N + batch.issued) {
        const auto [row, pivot] = SmeWarmRhsProgram[batch.issued];
        const bool scale = row == pivot;
        const auto factor = position(N + batch.issued);
        const bool issued = scale ? smeZa.scaleElements(position(row), mask, factor, Sme::Rhs) :
            smeZa.subtractElements(position(row), mask, factor, position(pivot), Sme::Rhs);
        if (!issued)
            return;
        traceSmeWarmRhs(line, group, "issue", batch.issued++);
        for (unsigned lane = 0; lane < batch.count; ++lane) {
            ++points[lane]->record.coeffMulIssued;
            points[lane]->record.coeffSubIssued += !scale;
        }
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
}

void
CfdCoeffPreprocessController::processSmeWarmRhs(LineRequest &line, uint64_t now)
{
    std::vector<unsigned> active;
    active.reserve(smeWarmRhs.size());
    for (unsigned group = 0; group < smeWarmRhs.size(); ++group) {
        if (!smeWarmRhs[group].count)
            continue;
        if (line.terminal)
            releaseSmeWarmRhs(line, group, "drop");
        else
            active.push_back(group);
    }
    if (line.terminal)
        return;
    std::sort(active.begin(), active.end(), [this](unsigned a, unsigned b) {
        return smeWarmRhs[a].first < smeWarmRhs[b].first;
    });
    bool gather = true;
    for (unsigned i = 0; i < active.size() && !line.terminal; ++i)
        advanceSmeWarmRhs(line, active[i], gather, now);
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
        advanceSmeWarmRhs(line, group, gather, now);
}
