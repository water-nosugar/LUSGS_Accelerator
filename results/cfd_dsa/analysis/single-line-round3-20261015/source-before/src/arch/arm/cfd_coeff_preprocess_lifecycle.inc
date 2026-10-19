// Terminal snapshots own DMA bytes independently of compute request credits.
void
CfdCoeffPreprocessController::processDetachedRecords()
{
    if (detachedRecords.empty()) return;
    auto candidate = detachedRecords.end();
    for (auto it = detachedRecords.begin(); it != detachedRecords.end(); ++it) {
        if (it->transfer->issued == it->transfer->bytes.size()) continue;
        if (candidate == detachedRecords.end() ||
            (it->token > detachedCursor &&
             (candidate->token <= detachedCursor || it->token < candidate->token)) ||
            (it->token <= detachedCursor && candidate->token <= detachedCursor && it->token < candidate->token))
            candidate = it;
    }
    if (candidate != detachedRecords.end()) {
        const auto before = candidate->transfer->issued;
        stepInterfaceTransfer(candidate->owner, candidate->transfer);
        if (candidate->transfer->issued != before) detachedCursor = candidate->token;
    }
    for (auto it = detachedRecords.begin(); it != detachedRecords.end();) {
        if (it->transfer->completed != it->transfer->bytes.size()) { ++it; continue; }
        auto owner = lineRequests.find(it->owner);
        if (owner != lineRequests.end()) {
            auto &line = *owner->second;
            panic_if(!line.recordsPending, "CFD terminal credit underflow");
            line.recordsPending--;
            if (!line.recordsPending && line.completedChildren == line.cells)
                line.childrenRetiredCycle = cycle();
        }
        // Reconstruct trace identity only; no large request is retained.
        Request traceIdentity;
        traceIdentity.token = it->token;
        traceIdentity.record.requestId = it->requestId;
        traceIdentity.desc.generation = it->generation;
        traceIdentity.desc.lineId = it->lineId;
        traceIdentity.desc.cellId = it->cell;
        traceIdentity.stage = RequestStage::Complete;
        trace(traceIdentity, "controller", "REQUEST_AUTO_RETIRE");
        detachedCompleted++;
        it = detachedRecords.erase(it);
    }
}

std::shared_ptr<LineTile>
CfdCoeffPreprocessController::forwardTile(const LineRequest &line, uint64_t cell) const
{
    for (const auto &tile : {line.tile, line.prefetchTile}) {
        if (tile && tile->phase == LineTile::Phase::Forward &&
            cell >= tile->start && cell < tile->start+tile->count &&
            (tile == line.tile || env.lineOptLevel >= 5)) return tile;
    }
    return {};
}
