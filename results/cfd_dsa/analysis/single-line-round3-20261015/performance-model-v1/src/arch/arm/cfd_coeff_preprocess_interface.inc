// The current guest ABI requires a parent descriptor, terminal per-cell
// records and a completion word. All data buffers survive through responses.
void
CfdCoeffPreprocessController::stepInterfaceTransfer(
    uint64_t owner, const std::shared_ptr<InterfaceTransfer> &transfer)
{
    auto &t = *transfer;
    if (t.issued == t.bytes.size())
        return;
    CfdLocalSpm *spm = getCfdLocalSpm();
    panic_if(!spm, "CFD timed interface requires local SPM DMA");
    const Addr addr = t.addr + t.issued;
    const unsigned bytes = spm->dmaChunkSize(tc, addr,
        std::min<uint64_t>(env.interfaceWidth, t.bytes.size()-t.issued));
    auto local = env.onchipTiming && !t.interface && !t.write ?
        startOnchip(owner,warmOnchipAddr(*lineRequests.at(owner),t.cell)+t.issued,
                    (bytes+7)/8,8,true,0,false) : nullptr;
    if (env.onchipTiming && !t.interface && !t.write && !local) return;
    const auto packet = spm->issueDma(tc, t.write, addr, bytes,
        t.bytes.data()+t.issued, env.spmOutstanding,
        [this, owner, transfer, bytes, local](uint64_t packetId) {
            auto publish = [this,owner,transfer,bytes,packetId] {
            auto &t = *transfer;
            t.completed += bytes;
            t.packetsCompleted++;
            t.lastResponse = cycle();
            auto it = lineRequests.find(owner);
            if (it != lineRequests.end()) {
                auto &line = *it->second;
                if (t.interface) {
                    (t.write ? line.interfaceWriteCompleted : line.interfaceReadCompleted) += bytes;
                    line.interfacePacketsCompleted++;
                    line.interfaceLastResponse = cycle();
                } else {
                    (t.write ? line.cacheWriteCompleted : line.cacheReadCompleted) += bytes;
                }
            }
            if (env.traceEnable)
                std::fprintf(stderr, "%s cycle=%llu token=%llu "
                    "event=response kind=%s cell=%llu bytes=%u packet=%llu\n",
                    t.interface ? "CFD_INTERFACE_DMA" : "CFD_CACHE_DMA",
                    (unsigned long long)cycle(), (unsigned long long)owner,
                    t.kind.c_str(), (unsigned long long)t.cell, bytes, (unsigned long long)packetId);
            };
            if (local) {
                        local->active = true;
                        local->eligibleCycle = cycle()+1;
                        local->completion = std::move(publish);
                        if (env.traceEnable)
                            std::fprintf(stderr,"CFD_ONCHIP_ACTIVATE cycle=%llu id=%llu\n",
                                (unsigned long long)cycle(),(unsigned long long)local->id);
                    }
            else publish();
        });
    if (!packet.accepted) {
        cancelOnchip(local);
        panic_if(!packet.outstandingFull, "CFD interface DMA translation failed");
        return;
    }
    t.issued += bytes;
    t.packetsIssued++;
    auto it = lineRequests.find(owner);
    if (it != lineRequests.end()) {
        auto &line = *it->second;
        if (t.interface) {
            (t.write ? line.interfaceWriteIssued : line.interfaceReadIssued) += bytes;
            line.interfacePacketsIssued++;
        } else {
            (t.write ? line.cacheWriteIssued : line.cacheReadIssued) += bytes;
        }
    }
    if (env.traceEnable)
        std::fprintf(stderr, "%s cycle=%llu token=%llu event=issue "
            "kind=%s cell=%llu bytes=%u packet=%llu\n",
            t.interface ? "CFD_INTERFACE_DMA" : "CFD_CACHE_DMA",
                    (unsigned long long)cycle(), (unsigned long long)owner,
            t.kind.c_str(), (unsigned long long)t.cell, bytes, (unsigned long long)packet.packetId);
}

void
CfdCoeffPreprocessController::processTimedLaunches()
{
    std::vector<uint64_t> started;
    for (const auto &[token, transfer] : pendingLineLaunches) {
        if (transfer->completed != transfer->bytes.size()) {
            stepInterfaceTransfer(token, transfer);
            continue;
        }
        CfdCoeffPreprocessDescriptor desc;
        std::memcpy(&desc, transfer->bytes.data(), sizeof(desc));
        panic_if(!(desc.flags & CFD_COEFF_PRE_LINE_AUTONOMOUS) ||
            launchLine(desc, token) != token, "CFD invalid timed line descriptor");
        auto &line = *lineRequests.at(token);
        line.interfaceReadIssued = transfer->issued;
        line.interfaceReadCompleted = transfer->completed;
        line.interfacePacketsIssued = transfer->packetsIssued;
        line.interfacePacketsCompleted = transfer->packetsCompleted;
        line.interfaceLastResponse = transfer->lastResponse;
        started.push_back(token);
    }
    for (uint64_t token : started)
        pendingLineLaunches.erase(token);
}
