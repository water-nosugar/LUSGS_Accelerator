// Two points can own partial q feedback. Each row group has its own ZA tile
// and executes the existing signed reduction without reassociation.
constexpr std::array<uint8_t, 2> SmeBackParts = {0x07, 0x18};
constexpr unsigned SmeBackFinal = 9;
constexpr unsigned SmeBackBase = 1u << 10;

void
CfdCoeffPreprocessController::traceSmeSplitBack(
    const LineRequest &line, uint64_t cell, const char *event,
    unsigned part, unsigned op) const
{
    if (env.traceEnable)
        std::fprintf(stderr, "CFD_SME_SPLIT_BACK token=%llu cycle=%llu cell=%llu "
            "event=%s part=%u op=%u\n",
            static_cast<unsigned long long>(line.token),
            static_cast<unsigned long long>(cycle()),
            static_cast<unsigned long long>(cell), event, part, op);
}

bool
CfdCoeffPreprocessController::releaseSmeSplitBack(
    const LineRequest &line, SmeSplitBackPoint &point, const char *event)
{
    const unsigned slot = point.cell % smeSplitBack.size();
    const unsigned firstTile = 2 * slot;
    const bool useBypass = env.smeWarmBypass && smeZa.bypassCount() >= 6;
    if (!smeZa.idle(firstTile) || !smeZa.idle(firstTile + 1))
        return false;
    if (useBypass && (!smeZa.idleBypass(3 * slot) ||
                      !smeZa.idleBypass(3 * slot + 1) ||
                      !smeZa.idleBypass(3 * slot + 2)))
        return false;
    smeZa.release(firstTile);
    smeZa.release(firstTile + 1);
    if (useBypass) {
        smeZa.releaseBypass(3 * slot);
        smeZa.releaseBypass(3 * slot + 1);
        smeZa.releaseBypass(3 * slot + 2);
    }
    traceSmeSplitBack(line, point.cell, event);
    point = {};
    return true;
}

void
CfdCoeffPreprocessController::processSmeSplitBack(LineRequest &line)
{
    if (smeZa.is2d()) {
        processSmeSplitBack2d(line);
        return;
    }
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

    // A returned group may supply the next point in the same tick. Every
    // group pays a ZA row read (or bypass latch read) even if its other rows
    // are still in flight.
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
                    smeZa.read({tileAt(cell, part), N}, mask, values, Sme::Backward);
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
                    smeZa.load({tile, N}, mask, values, Sme::Backward);
                if (loaded) {
                    issued |= SmeBackBase;
                    markBackwardYUse(line, operand);
                    traceSmeSplitBack(line, cell, "base", part);
                }
            }
            for (unsigned col = 0; col < N; ++col) {
                const unsigned bit = 1u << col;
                if ((issued & bit) || !(ready & operand.readyColumns & bit))
                    continue;
                Sme::Row values{};
                const auto &column = operand.values[col];
                std::copy(column.begin(), column.end(), values.begin());
                const int bpId = useBypass ? latchAt(cell, col) : -1;
                const bool launched = bpId >= 0 ?
                    smeZa.ingressProductBypass(bpId, mask, values, feedback[col],
                                               Sme::Backward) :
                    smeZa.ingressProduct({tile, col}, mask, values, feedback[col],
                                         Sme::Backward);
                if (launched) {
                    issued |= bit;
                    markBackwardColumnUse(line, operand, col);
                    traceSmeSplitBack(line, cell, "issue", part, col);
                }
            }
            for (unsigned i = 0; i < nodes.size(); ++i) {
                const auto &node = nodes[i];
                const unsigned op = N + i;
                const unsigned bit = 1u << op;
                if ((issued & bit) || (issued & node.needs) != node.needs)
                    continue;
                bool combined = false;
                if (useBypass) {
                    const int dstId = latchAt(cell, node.dst);
                    const int leftId = latchAt(cell, node.left);
                    const int rightId = latchAt(cell, node.right);
                    combined = smeZa.combineBypass(
                        dstId, mask,
                        leftId >= 0, leftId >= 0 ? Sme::Position{unsigned(leftId), 0} : Sme::Position{tile, node.left},
                        rightId >= 0, rightId >= 0 ? Sme::Position{unsigned(rightId), 0} : Sme::Position{tile, node.right},
                        node.subtract, Sme::Backward);
                } else {
                    combined = smeZa.combine({tile, node.dst}, mask, {tile, node.left},
                                             {tile, node.right}, node.subtract, Sme::Backward);
                }
                if (combined) {
                    issued |= bit;
                    traceSmeSplitBack(line, cell, "issue", part, op);
                }
            }
        }
    }
}
