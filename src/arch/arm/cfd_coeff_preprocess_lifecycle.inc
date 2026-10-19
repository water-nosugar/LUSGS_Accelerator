// Terminal snapshots own DMA bytes independently of compute request credits.
void
CfdCoeffPreprocessController::abortLine(
    LineRequest &line, CfdCoeffPreprocessStatus status)
{
    if (line.abortPending)
        return;
    line.abortPending = true;
    line.terminal = true; // stop admission and new numerical/output work
    line.status = status;
    auto cache = lineCoeffCaches.find(line.desc.lineId);
    if (cache != lineCoeffCaches.end())
        cache->second.valid = false;
    forwardHandoffs.erase(line.desc.lineId);
    std::vector<uint64_t> children;
    for (const auto &[child, owner] : childLineTokens)
        if (owner == line.token)
            children.push_back(child);
    for (uint64_t child : children)
        cancel(child);
    // Already issued packets retain their own snapshots through callbacks.
    line.outputWrites.clear();
    line.warmWrites.clear();
    line.cachedForwardStarted = false;
    line.statusTransfer.reset();
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LINE_ABORT cycle=%llu token=%llu event=begin status=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)line.token,
            (unsigned long long)status);
}

void
CfdCoeffPreprocessController::processAbortedLines()
{
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        if (!line.abortPending || line.abortPublished)
            continue;
        auto *spm = getCfdLocalSpm();
        if (line.liveChildren || line.recordsPending || !pendingOps.empty() ||
            !sharedVectorAdds.empty() || sharedVectorsLive ||
            std::any_of(onchipQueue.begin(),onchipQueue.end(),
                [token](const auto &a) { return a->owner == token; }))
            continue;
        if (!line.statusTransfer) {
            if (spm && spm->dmaOutstanding())
                continue;
            if (env.timedInterface) {
                line.statusTransfer = std::make_shared<InterfaceTransfer>();
                auto &t = *line.statusTransfer;
                t.addr = line.desc.dInvOutAddr;
                t.write = true;
                t.kind = "abort-status";
                const uint64_t status = static_cast<uint64_t>(line.status);
                t.bytes.resize(sizeof(status));
                std::memcpy(t.bytes.data(), &status, sizeof(status));
            }
        }
        if (env.timedInterface) {
            stepInterfaceTransfer(token, line.statusTransfer);
            if (line.statusTransfer->completed != sizeof(uint64_t))
                continue;
        } else {
            const uint64_t status = static_cast<uint64_t>(line.status);
            proxy().writeBlob(line.desc.dInvOutAddr,&status,sizeof(status));
        }
        line.abortPublished = true;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_LINE_ABORT cycle=%llu token=%llu event=visible status=%llu dma_live=%llu records=%llu children=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)token,
                (unsigned long long)line.status,
                (unsigned long long)(spm ? spm->dmaOutstanding() : 0),
                (unsigned long long)line.recordsPending,(unsigned long long)line.liveChildren);
    }
}

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
