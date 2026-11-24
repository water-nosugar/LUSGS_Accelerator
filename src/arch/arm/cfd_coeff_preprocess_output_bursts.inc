// Adjacent reverse-produced vectors share a real <=64B timing write. Existing
// output slots remain live through every response; callbacks own immutable
// packet bytes and bounded routing (128B extra per outstanding DMA credit).
void
CfdCoeffPreprocessController::processCoalescedLineOutputWrites(LineRequest &line)
{
    CfdLocalSpm *spm = getCfdLocalSpm();
    auto &queue = line.outputWrites;
    std::shared_ptr<LineOutputWrite> first;
    unsigned pending = 0;
    for (const auto &w : queue) {
        pending += VectorBytes-w->issued;
        if (w->issued < VectorBytes && (!first || w->cell < first->cell)) first=w;
    }
    if (!first) return;
    // A bounded 32-cycle assembly timeout also flushes isolated fragments.
    // Depth-one queues and the final point always flush immediately.
    if (pending < 64 && line.backwardCell >= 0 &&
        queue.size() < env.drainQueueDepth && cycle()-first->readyCycle < 32)
        return;
    const Addr address = line.desc.reserved[2]+first->cell*VectorBytes+first->issued;
    const unsigned limit = spm->dmaChunkSize(tc,address,std::min<unsigned>(64,env.drainWidth));
    auto packet = std::make_shared<LineOutputPacket>();
    Addr next = address;
    while (packet->bytes < limit && packet->count < packet->slices.size()) {
        auto it = std::find_if(queue.begin(),queue.end(),[&](const auto &w) {
            return w->issued < VectorBytes &&
                line.desc.reserved[2]+w->cell*VectorBytes+w->issued == next;
        });
        if (it == queue.end()) break;
        auto w = *it;
        const unsigned bytes = std::min<unsigned>(limit-packet->bytes,VectorBytes-w->issued);
        packet->slices[packet->count++] = {w,w->issued,bytes};
        std::memcpy(packet->payload.data()+packet->bytes,
            reinterpret_cast<const uint8_t *>(w->value.data())+w->issued,bytes);
        packet->bytes += bytes;
        next += bytes;
    }
    panic_if(!packet->bytes,"CFD empty output burst");
    const uint64_t token = line.token;
    const auto issued = spm->issueDma(tc,true,address,packet->bytes,packet->payload.data(),
        env.spmOutstanding,[this,token,packet,address](uint64_t id) {
            for (unsigned i=0;i<packet->count;++i)
                packet->slices[i].write->completed += packet->slices[i].bytes;
            auto it = lineRequests.find(token);
            if (it == lineRequests.end()) return;
            auto &line = *it->second;
            line.outputBytesCompleted += packet->bytes;
            line.outputPacketsCompleted++;
            line.outputLastResponseCycle = cycle();
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_OUTPUT_BURST cycle=%llu token=%llu packet=%llu event=response addr=0x%llx bytes=%u members=%u\n",
                    (unsigned long long)cycle(),(unsigned long long)token,
                    (unsigned long long)id,(unsigned long long)address,packet->bytes,packet->count);
        });
    if (!issued.accepted) {
        line.outputOutstandingStalls += issued.outstandingFull;
        panic_if(!issued.outstandingFull,"CFD output burst translation failed");
        return;
    }
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_OUTPUT_BURST cycle=%llu token=%llu packet=%llu event=issue addr=0x%llx bytes=%u members=%u\n",
            (unsigned long long)cycle(),(unsigned long long)token,
            (unsigned long long)issued.packetId,(unsigned long long)address,packet->bytes,packet->count);
    for (unsigned i=0;i<packet->count;++i) {
        auto &s = packet->slices[i];
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_OUTPUT_SLICE cycle=%llu token=%llu packet=%llu cell=%llu offset=%u payload_offset=%u bytes=%u\n",
                (unsigned long long)cycle(),(unsigned long long)token,
                (unsigned long long)issued.packetId,(unsigned long long)s.write->cell,
                s.offset,(unsigned)(line.desc.reserved[2]+s.write->cell*VectorBytes+s.offset-address),s.bytes);
        s.write->issued += s.bytes;
    }
    line.outputBytesIssued += packet->bytes;
    line.outputPacketsIssued++;
}
