// Split the original input read into nonoverlapping D and remaining-input
// jobs. Both contend for the existing finite queue, arbiter and bank ports.
// dRead fits the existing 128B OnchipContext reservation; no operand copy or
// extra DMA packet is introduced. Input slots remain owned through both reads.
void
CfdCoeffPreprocessController::traceInputSplit(
    const Request &req, const char *event,
    const std::shared_ptr<OnchipAccess> &access)
{
    if (env.traceEnable && req.desc.cellId < env.traceCells)
        std::fprintf(stderr,"CFD_INPUT_SPLIT cycle=%llu request=%llu generation=%llu cell=%llu owner=%llu event=%s slot=%d job=%llu bytes=%llu\n",
            (unsigned long long)cycle(),
            (unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.generation,
            (unsigned long long)req.desc.cellId,
            (unsigned long long)access->owner,event,req.slot,
            (unsigned long long)access->id,
            (unsigned long long)req.record.inputBytes);
}

void
CfdCoeffPreprocessController::prepareInputD(Request &req)
{
    if (req.inputDReady || req.dBytesCompleted != MatrixBytes) return;
    auto &read = onchipContexts[req.token].dRead;
    if (!read) {
        read = startOnchip(childLineTokens.at(req.token),
            inputOnchipAddr(req),N*N,8,false,2);
        if (read) traceInputSplit(req,"d_read",read);
    }
    if (!read || !read->done) return;
    req.inputDReady = true;
    traceInputSplit(req,"d_ready",read);
    trace(req,"input","INPUT_D_READY",-1,-1,-1,-1,
          "bank-load-complete","",read->id,read->base,MatrixBytes);
}

// Reuse the existing single rest-input read handle. Each batch occupies the
// finite SPM queue only after all of its own paid stores; no operand is read
// twice, and there is at most one rest-input read per request at a time.
void
CfdCoeffPreprocessController::prepareInputBatches(Request &req,uint64_t now)
{
    if (req.inputBatchLoadsDone) return;
    const bool priority=env.inputRBeforeU && req.inputMatrixCount==3 &&
        req.inputMatrices[1]==CfdSpmMatrix::L &&
        req.inputMatrices[2]==CfdSpmMatrix::U &&
        req.baseConsumerEnabled && !coldRhsInput(req);
    const unsigned regions=req.inputMatrixCount+
        (req.baseConsumerEnabled && !coldRhsInput(req) ? 1 : 0);
    if (!priority && req.inputBatchNext>=regions) { req.inputBatchLoadsDone=true;return; }
    auto &load=onchipContexts[req.token].inputRead;
    auto region=[&] {
        return req.inputBatchNext<req.inputMatrixCount ?
            req.inputMatrices[req.inputBatchNext] : CfdSpmMatrix::RhsVector;
    };
    auto indexOf=[](CfdSpmMatrix matrix) {
        return matrix==CfdSpmMatrix::L ? 0u : matrix==CfdSpmMatrix::U ? 1u : 2u;
    };
    auto emit=[&](const char *event,CfdSpmMatrix matrix,unsigned bytes) {
        if (env.traceEnable && req.desc.cellId<env.traceCells)
            std::fprintf(stderr,"CFD_INPUT_BATCH cycle=%llu request=%llu generation=%llu cell=%llu owner=%llu event=%s region=%u matrix=%s slot=%d job=%llu offset=%u bytes=%u\n",
                (unsigned long long)now,(unsigned long long)req.record.requestId,
                (unsigned long long)req.desc.generation,(unsigned long long)req.desc.cellId,
                (unsigned long long)load->owner,event,req.inputBatchNext,
                matrix==CfdSpmMatrix::L ? "L" : matrix==CfdSpmMatrix::U ? "U" : "R",req.slot,
                (unsigned long long)load->id,req.inputBatchNext*MatrixBytes,bytes);
    };
    if (load && load->done) {
        const auto matrix=region();
        const unsigned bytes=matrix==CfdSpmMatrix::RhsVector ? VectorBytes : MatrixBytes;
        if (matrix==CfdSpmMatrix::L) req.inputLowerReady=true;
        else if (matrix==CfdSpmMatrix::U) req.inputUpperReady=true;
        else { req.rhsVectorReady=true;req.record.rhsVectorReadyCycle=now; }
        emit("ready",matrix,bytes);
        trace(req,"input",matrix==CfdSpmMatrix::L ? "INPUT_L_READY" :
            matrix==CfdSpmMatrix::U ? "INPUT_U_READY" : "INPUT_R_READY",
            -1,-1,-1,-1,"bank-load-complete","",load->id,load->base,bytes);
        if (priority) {
            if (req.inputLowerReady && req.inputUpperReady && req.rhsVectorReady) {
                // Retain the final completed handle until normal input-slot drain.
                req.inputBatchLoadsDone=true;return;
            }
        } else {
            ++req.inputBatchNext;
            if (req.inputBatchNext>=regions) { req.inputBatchLoadsDone=true;return; }
        }
        load.reset();
    }
    if (load) return;
    if (priority) {
        const int selected=CfdInputBatchOrder::select(req.inputLowerReady,
            req.inputUpperReady,req.rhsVectorReady,req.inputBatchBytes[0],
            req.inputBatchBytes[1],req.inputBatchBytes[2]);
        if (selected<0) return;
        req.inputBatchNext=selected;
    }
    const auto matrix=region();
    const unsigned bytes=matrix==CfdSpmMatrix::RhsVector ? VectorBytes : MatrixBytes;
    if (req.inputBatchBytes[indexOf(matrix)]!=bytes) return;
    load=startOnchip(childLineTokens.at(req.token),
        inputOnchipAddr(req)+req.inputBatchNext*MatrixBytes,bytes/8,8,false,2);
    if (load) emit("read",matrix,bytes);
}

// The existing eight warm slots are otherwise unused on the direct path.
// They prefetch ONLY R; the original request window and RHS contexts still
// bound computation, and every read/store/load uses the normal DMA/SPM path.
bool
CfdCoeffPreprocessController::directRhsLookahead(const LineRequest &line) const
{
    return env.rhsLookahead && (coldRhsLookahead(line) ||
        (line.cells>1 && line.tileYOnly && line.coefficientCacheHit &&
         (line.desc.flags&CFD_COEFF_PRE_BASE_DIRECT_SOLVE)));
}

void
CfdCoeffPreprocessController::processDirectRhsPrefetch()
{
    for (auto &[token,ptr] : lineRequests) {
        auto &line=*ptr;
        if (!directRhsLookahead(line) || line.terminal) continue;
        const unsigned depth=std::min<uint64_t>(env.rhsLookahead,warmDepth(line));
        unsigned live=0;
        auto step=[this,&line](WarmSlot &slot) {
            const auto before=slot.rhs->issued;
            stepInterfaceTransfer(line.token,slot.rhs);
            if (!before && slot.rhs->issued) slot.rhsIssueCycle=cycle();
        };
        for (auto &slot : line.warm) {
            if (!slot.valid) continue;
            ++live; step(slot);
        }
        while (live<depth && line.warmNextInput<line.cells) {
            const uint64_t cell=line.warmNextInput;
            auto &slot=line.warm[cell%warmDepth(line)];
            // Cache hits may let cell1 consume before cell0. A free slot
            // elsewhere does not authorize overwriting the wrapped head.
            if (slot.valid) break;
            slot.valid=true; slot.cell=cell;
            slot.rhs=std::make_shared<InterfaceTransfer>();
            slot.rhs->interface=false; slot.rhs->kind="rhs";
            const unsigned rhsStride = line.desc.flags & CFD_COEFF_PRE_PACKED_COLD_INPUT ?
                PackedColdInputBytes : VectorBytes;
            slot.rhs->cell=cell; slot.rhs->addr=line.desc.reserved[0]+cell*rhsStride;
            slot.rhs->bytes.resize(VectorBytes);
            line.warmNextInput++; ++live;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_DIRECT_RHS cycle=%llu token=%llu cell=%llu event=allocate slot=%llu depth=%u mode=%s\n",
                    (unsigned long long)cycle(),(unsigned long long)token,
                    (unsigned long long)cell,(unsigned long long)(cell%warmDepth(line)),depth,
                    coldRhsLookahead(line) ? "cold" : "warm");
            step(slot);
        }
    }
}

void
CfdCoeffPreprocessController::consumeColdRhs(Request &req, uint64_t now)
{
    if (req.rhsVectorReady) return;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    auto &slot = line.warm[req.desc.cellId%warmDepth(line)];
    if (!slot.valid || slot.cell != req.desc.cellId ||
        slot.rhs->completed != VectorBytes || !warmRhsOnchipReady(line,slot))
        return;
    std::memcpy(req.rhsVector.data(),slot.rhs->bytes.data(),VectorBytes);
    req.rhsVectorReady = true;
    req.record.rhsVectorIssueCycle = slot.rhsIssueCycle;
    req.record.rhsVectorReadyCycle = now;
    trace(req,"input","INPUT_R_READY",-1,-1,-1,-1,
          "bank-load-complete","",slot.rhsLoad->id,
          warmOnchipAddr(line,slot.cell),VectorBytes);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_DIRECT_RHS cycle=%llu token=%llu cell=%llu event=consume request=%llu generation=%llu job=%llu dma_done=%llu mode=cold\n",
            (unsigned long long)now,(unsigned long long)line.token,
            (unsigned long long)slot.cell,(unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.generation,(unsigned long long)slot.rhsLoad->id,
            (unsigned long long)slot.rhs->lastResponse);
    // The existing request R register owns the value now. Coefficient DMA
    // keeps its original D/U/L order but stops before R, avoiding a second
    // read/store/load of the same vector. The finite prefetch slot can wrap.
    slot = WarmSlot{};
}

bool
CfdCoeffPreprocessController::consumeDirectRhs(Request &req,uint64_t now)
{
    if (!env.rhsLookahead || !req.coefficientCacheReuse) return false;
    auto &line=*lineRequests.at(childLineTokens.at(req.token));
    if (!directRhsLookahead(line)) return false;
    auto &slot=line.warm[req.desc.cellId%warmDepth(line)];
    if (!slot.valid || slot.cell!=req.desc.cellId ||
        slot.rhs->completed!=VectorBytes || !warmRhsOnchipReady(line,slot) ||
        !cachedRegistersReady(req)) {
        line.warmInputStalls++; req.record.stallDependency++;
        return true;
    }
    std::memcpy(req.rhsVector.data(),slot.rhs->bytes.data(),VectorBytes);
    req.inputCaptured=true; req.inputBytesIssued=VectorBytes;
    req.rhsVectorReady=true;
    req.inputOperandsReady=true;
    req.record.rhsVectorIssueCycle=slot.rhsIssueCycle;
    req.record.rhsVectorReadyCycle=now; req.record.inputDoneCycle=now;
    req.stage=RequestStage::InputReady;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_DIRECT_RHS cycle=%llu token=%llu cell=%llu event=consume request=%llu generation=%llu job=%llu dma_done=%llu\n",
            (unsigned long long)now,(unsigned long long)line.token,
            (unsigned long long)slot.cell,(unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.generation,(unsigned long long)slot.rhsLoad->id,
            (unsigned long long)slot.rhs->lastResponse);
    trace(req,"input","INPUT_TOKEN");
    slot=WarmSlot{};
    return true;
}

// Adjacent warm R fragments share a physical read, with independent input
// slots and SPM stores. One <=64B snapshot is retained per physical DMA packet;
// no request is admitted early and no operand bypasses its bank response.
bool
CfdCoeffPreprocessController::coalesceRhsInput(Request &head,uint64_t now)
{
    auto eligible = [](const Request &r) {
        return r.stage == RequestStage::InputFetching && r.slotOwned &&
            r.directBaseSolve && r.coefficientCacheReuse && !r.cacheUbarReload &&
            r.record.inputBytes == VectorBytes && r.inputBytesIssued < VectorBytes;
    };
    if (!env.rhsInputCoalesce || !env.dmaSpm || !env.onchipTiming ||
        !eligible(head)) return false;
    auto *spm = getCfdLocalSpm();
    panic_if(!spm,"CFD RHS burst without DMA");
    const Addr address = head.desc.reserved[0]+head.inputBytesIssued;
    const unsigned limit = spm->dmaChunkSize(tc,address,
        std::min<unsigned>(64,env.spmWriteWidth));
    struct Slice {
        Request *req = nullptr;
        unsigned offset = 0, bytes = 0, payloadOffset = 0;
        std::shared_ptr<OnchipAccess> local;
    };
    std::array<Slice,3> slices{};
    unsigned count = 0, bytes = 0;
    for (auto *r : admissionOrder()) {
        if (!eligible(*r) || childLineTokens.at(r->token)!=childLineTokens.at(head.token) ||
            r->desc.reserved[0]+r->inputBytesIssued != address+bytes) continue;
        auto &s = slices[count++];
        s.req=r; s.offset=r->inputBytesIssued; s.payloadOffset=bytes;
        s.bytes=std::min<unsigned>(VectorBytes-s.offset,limit-bytes);
        bytes+=s.bytes;
        if (bytes==limit || count==slices.size()) break;
    }
    if (count<2) return false;
    for (unsigned i=0;i<count;++i) {
        auto &s=slices[i];
        s.local=startOnchip(childLineTokens.at(s.req->token),
            inputOnchipAddr(*s.req)+s.offset,s.bytes/8,8,true,2,false);
        if (!s.local) {
            for (unsigned j=0;j<i;++j) cancelOnchip(slices[j].local);
            return true;
        }
    }
    auto payload=std::make_shared<std::vector<uint8_t>>(bytes);
    // Callbacks retain token identity, never a request pointer after cancel.
    struct CompletionSlice {
        uint64_t token;
        unsigned offset, bytes, payloadOffset;
        std::shared_ptr<OnchipAccess> local;
    };
    // Routing sidecar fits the 128B already reserved beside each OnchipAccess
    // (see its 256B static assertion). Every slice owns a queued SPM task;
    // even inactive tasks consume finite onchipDepth credits until DMA returns.
    static_assert(3*sizeof(CompletionSlice)<=128);
    std::array<CompletionSlice,3> completion{};
    for (unsigned i=0;i<count;++i) {
        auto &s=slices[i];
        completion[i]={s.req->token,s.offset,s.bytes,s.payloadOffset,s.local};
    }
    const auto packet=spm->issueDma(tc,false,address,bytes,payload->data(),
        env.spmOutstanding,[this,payload,completion,count,address,bytes](uint64_t id) {
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_RHS_BURST cycle=%llu packet=%llu event=response addr=0x%llx bytes=%u members=%u\n",
                    (unsigned long long)cycle(),(unsigned long long)id,
                    (unsigned long long)address,bytes,count);
            for (unsigned i=0;i<count;++i) {
                const auto s=completion[i];
                activateOnchip(s.local, [this,s,payload,id,address] {
                    completeInputPacket(s.token, s.offset, s.bytes, id,
                        address + s.payloadOffset, 0, 0,
                        payload->data() + s.payloadOffset);
                });
            }
        });
    if (!packet.accepted) {
        for (unsigned i=0;i<count;++i) cancelOnchip(slices[i].local);
        panic_if(!packet.outstandingFull,"CFD RHS burst translation failed");
        head.record.coeffSpmOutstandingStalls++;
        trace(head,"spm","SPM_PACKET_STALL_OUTSTANDING",-1,-1,-1,-1,
            "write","CoeffInput",0,address,bytes);
        return true;
    }
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_RHS_BURST cycle=%llu packet=%llu event=issue addr=0x%llx bytes=%u members=%u\n",
            (unsigned long long)now,(unsigned long long)packet.packetId,
            (unsigned long long)address,bytes,count);
    for (unsigned i=0;i<count;++i) {
        auto &s=slices[i]; auto &r=*s.req;
        traceSpmInputStore(r,s.local,s.offset,packet.packetId);
        r.inputCaptured=true; r.inputReadyCycle=now;
        if (!r.record.rhsVectorIssueCycle) r.record.rhsVectorIssueCycle=now;
        r.inputChunksIssued++; r.inputBytesIssued+=s.bytes;
        r.record.inputWriteRequests++; r.record.coeffSpmPacketIssued++;
        r.record.coeffInputPacketBytes+=s.bytes;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_RHS_SLICE cycle=%llu packet=%llu request=%llu generation=%llu cell=%llu offset=%u payload_offset=%u bytes=%u job=%llu slot=%u\n",
                (unsigned long long)now,(unsigned long long)packet.packetId,
                (unsigned long long)r.record.requestId,(unsigned long long)r.desc.generation,
                (unsigned long long)r.desc.cellId,s.offset,s.payloadOffset,s.bytes,
                (unsigned long long)s.local->id,r.slot);
        for (const char *stage : {"SPM_PACKET_ISSUE","SPM_PACKET_ACCEPT"})
            trace(r,"spm",stage,-1,-1,-1,-1,"write","CoeffInput",
                packet.packetId,address+s.payloadOffset,s.bytes,0,0);
    }
    return true;
}
