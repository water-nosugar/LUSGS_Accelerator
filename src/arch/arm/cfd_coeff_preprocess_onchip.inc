// The banked SPM stores ingress data, coefficient cache and resident tiles.
// Active LU/RHS and product accumulators are registers. Control/output FIFOs
// have their own bounded storage and are not charged as SPM operand accesses.
std::shared_ptr<OnchipAccess>
CfdCoeffPreprocessController::startOnchip(uint64_t owner, Addr base, unsigned words,
    unsigned stride, bool write, unsigned priority, bool active)
{
    if (onchipQueue.size() >= env.onchipDepth) { onchipFull++; return {}; }
    // Pending DMA writes own guaranteed response capacity, but may not use
    // the final compute credits. Already-active reads/stores share these
    // credits and continue to arbitrate through the same physical banks.
    if (!active && onchipQueue.size() >=
        env.onchipDepth-env.onchipComputeReserve) {
        onchipReserveStalls++;
        return {};
    }
    const bool aligned = base % 8 == 0 && stride % 8 == 0;
    panic_if(!words || !aligned || !env.onchipTiming,
             "CFD invalid onchip access");
    panic_if(base+(words-1)*stride+8 > env.lineSpmBytes,
             "CFD SPM access outside capacity");
    auto access = std::make_shared<OnchipAccess>();
    access->id = onchipNextId++;
    access->owner = owner;
    access->base = base;
    access->words = words;
    access->stride = stride;
    access->write = write;
    access->priority = priority;
    access->active = active;
    access->eligibleCycle = cycle()+env.onchipArbLatency;
    onchipQueue.push_back(access);
    onchipInactive += !active;
    onchipCreated++;
    onchipPeak = std::max<uint64_t>(onchipPeak,onchipQueue.size());
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_ONCHIP_START cycle=%llu id=%llu owner=%llu "
            "base=0x%llx words=%u stride=%u op=%s active=%u priority=%u\n",
            (unsigned long long)cycle(),(unsigned long long)access->id,
            (unsigned long long)owner,(unsigned long long)base,words,stride,
            write ? "W" : "R",active,priority);
    return access;
}

void
CfdCoeffPreprocessController::cancelOnchip(const std::shared_ptr<OnchipAccess> &access)
{
    if (!access) return;
    panic_if(access->next || access->active, "CFD cancelling an active SPM job");
    auto it = std::find(onchipQueue.begin(),onchipQueue.end(),access);
    panic_if(it == onchipQueue.end(), "CFD missing cancelled SPM job");
    onchipQueue.erase(it);
    panic_if(!onchipInactive, "CFD inactive SPM credit underflow");
    onchipInactive--;
    onchipCancelled++;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_ONCHIP_CANCEL cycle=%llu id=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)access->id);
}

void
CfdCoeffPreprocessController::activateOnchip(
    const std::shared_ptr<OnchipAccess> &access,
    std::function<void()> completion)
{
    panic_if(!access || access->active || !onchipInactive,
             "CFD invalid DMA response activation");
    onchipInactive--;
    access->active = true;
    access->eligibleCycle = cycle()+env.onchipArbLatency;
    access->completion = std::move(completion);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_ONCHIP_ACTIVATE cycle=%llu id=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)access->id);
}

void
CfdCoeffPreprocessController::processOnchip()
{
    if (!env.onchipTiming) return;
    onchipSamples++;
    if (onchipReads.empty()) {
        onchipReads.resize(env.spmBanks);
        onchipWrites.resize(env.spmBanks);
    }
    std::vector<std::function<void()>> completed;
    for (auto &a : onchipQueue) {
        if (a->readyCycle && a->readyCycle <= cycle()) {
            a->done = true;
            onchipCompleted++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_ONCHIP_COMPLETE cycle=%llu id=%llu owner=%llu\n",
                    (unsigned long long)cycle(),(unsigned long long)a->id,
                    (unsigned long long)a->owner);
            if (a->completion) completed.push_back(std::move(a->completion));
        }
    }
    onchipQueue.erase(std::remove_if(onchipQueue.begin(),onchipQueue.end(),
        [](const auto &a) { return a->done; }),onchipQueue.end());
    for (const auto &f : completed) f();
    std::vector<unsigned> reads(env.spmBanks,env.spmReadPorts);
    std::vector<unsigned> writes(env.spmBanks,env.spmWritePorts);
    std::vector<std::shared_ptr<OnchipAccess>> ready(onchipQueue.begin(),onchipQueue.end());
    std::stable_sort(ready.begin(),ready.end(),[](const auto &a,const auto &b) {
        return std::tie(a->priority,a->id) < std::tie(b->priority,b->id);
    });
    unsigned grantedTasks = 0;
    for (const auto &a : ready) {
        if (!a->active || a->readyCycle || cycle() < a->eligibleCycle) continue;
        if (env.onchipArbTasks && grantedTasks >= env.onchipArbTasks) break;
        const unsigned before = a->next;
        while (a->next < a->words) {
            const Addr address = a->base+a->next*a->stride;
            const unsigned bank = (address/env.spmBankGranularity)%env.spmBanks;
            auto &ports = a->write ? writes : reads;
            if (!ports[bank]) { onchipConflicts++; break; }
            ports[bank]--;
            (a->write ? onchipWrites : onchipReads)[bank]++;
            a->next++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_ONCHIP_ACCESS cycle=%llu id=%llu owner=%llu "
                    "op=%s addr=0x%llx bank=%u priority=%u\n",
                    (unsigned long long)cycle(),(unsigned long long)a->id,
                    (unsigned long long)a->owner,a->write ? "W" : "R",
                    (unsigned long long)address,bank,a->priority);
        }
        grantedTasks += a->next != before;
        if (a->next == a->words) a->readyCycle = cycle()+env.onchipLatency;
    }
}

Addr
CfdCoeffPreprocessController::tileOnchipAddr(const LineRequest &line,const LineTile &tile) const
{
    return line.tileReserve+tile.storageIndex*
        (tile.y.size()*VectorBytes+tile.u.size()*MatrixBytes);
}

Addr
CfdCoeffPreprocessController::inputOnchipAddr(const Request &req) const
{
    panic_if(req.slot<0,"CFD input SPM access without a slot");
    return CfdSpmSlotRotation::input(env.spmSlotRotation,req.slot);
}

void
CfdCoeffPreprocessController::traceSpmInputStore(const Request &req,
    const std::shared_ptr<OnchipAccess> &access,uint64_t offset,uint64_t packet)
{
    if(!env.traceEnable || !env.spmSlotRotation || !access)return;
    std::fprintf(stderr,"CFD_SPM_SLOT_STORE cycle=%llu request=%llu generation=%llu cell=%llu token=%llu owner=%llu slot=%d job=%llu offset=%llu packet=%llu\n",
        (unsigned long long)cycle(),(unsigned long long)req.record.requestId,
        (unsigned long long)req.desc.generation,(unsigned long long)req.desc.cellId,
        (unsigned long long)req.token,(unsigned long long)access->owner,req.slot,
        (unsigned long long)access->id,(unsigned long long)offset,(unsigned long long)packet);
}

Addr
CfdCoeffPreprocessController::warmOnchipAddr(const LineRequest &line,uint64_t cell) const
{
    const uint64_t base = (env.inputSlots+env.luPendingDepth+env.solvePendingDepth+env.outputDepth)*8192+
        16384+(env.rhsRowParallel ? env.rhsProductDepth*64+256 : 0)+env.completionDepth*completionEntryBytes(env.lineOptLevel);
    return base+CfdSpmSlotRotation::rhs(env.spmSlotRotation,cell%warmDepth(line));
}

bool
CfdCoeffPreprocessController::warmRhsOnchipReady(LineRequest &line,WarmSlot &slot)
{
    if (!env.onchipTiming) return true;
    if (!slot.rhsLoad) slot.rhsLoad = startOnchip(line.token,warmOnchipAddr(line,slot.cell),N,8,false,0);
    return slot.rhsLoad && slot.rhsLoad->done;
}

bool
CfdCoeffPreprocessController::onchipColumnReady(LineRequest &line,unsigned context,unsigned column)
{
    if (!env.onchipTiming) return true;
    if (context == 1 && env.lineOptLevel >= 9)
        return backwardOperandReady(line, column);
    const uint64_t cell = context == 1 ? line.backwardCell : context == 2 ? line.baseCell : line.cachedForwardCell;
    const unsigned kind = context == 1 ? 2 : context == 2 ? 0 : line.cachedForwardPhase;
    auto &state = line.mvm[context];
    if (state.memoryCell != cell || state.memoryKind != kind) {
        state.memoryCell = cell;
        state.memoryKind = kind;
        state.loads.fill({});
        state.yLoad.reset();
        state.loadedColumns = 0;
    }
    if (context == 1) {
        if (!state.yLoad)
            state.yLoad = startOnchip(line.token,tileOnchipAddr(line,*line.tile)+
                (cell-line.tile->start)*VectorBytes,N,8,false,0);
        if (!state.yLoad || !state.yLoad->done) return false;
        line.backwardYOperand = line.tile->y[cell-line.tile->start];
    }
    Addr base = context == 1 && !cachedUbar(line) ?
        tileOnchipAddr(line,*line.tile)+line.tile->y.size()*VectorBytes+(cell-line.tile->start)*MatrixBytes :
        cacheEntryAddr(line, cell) + smeCacheTagBytes() + kind*MatrixBytes;
    const std::array<double,N*N> *matrix = nullptr;
    if (context == 1 && !cachedUbar(line)) matrix = &line.tile->u[cell-line.tile->start];
    else {
        const auto &cache = lineCoeffCaches.at(line.desc.lineId);
        matrix = kind == 0 ? &cache.dInv[cell] : kind == 1 ? &cache.lBar[cell] : &cache.uBar[cell];
    }
    for (unsigned col = column; col < N; ++col) {
        auto &load = state.loads[col];
        if (!load) load = startOnchip(line.token,base+col*8,N,N*8,false,context == 2 ? 1 : 0);
        if (!load) break;
        if (load->done && !(state.loadedColumns & (1u<<col))) {
            for (unsigned row = 0; row < N; ++row) state.columnOperands[col][row] = (*matrix)[row*N+col];
            state.loadedColumns |= 1u<<col;
        }
    }
    return state.loadedColumns & (1u<<column);
}

bool
CfdCoeffPreprocessController::cachedRegistersReady(Request &req)
{
    if (!env.onchipTiming || !req.coefficientCacheReuse) return true;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    auto &memory = onchipContexts[req.token];
    if (!memory.cacheRead) memory.cacheRead = startOnchip(line.token,
        cacheEntryAddr(line, req.desc.cellId),
        (smeCacheTagBytes() + MatrixBytes*(req.desc.cellId ? 2 : 1))/8,
        8,false,0);
    if (!memory.cacheRead || !memory.cacheRead->done) return false;
    if (!memory.cacheReadyTraced) {
        memory.cacheReadyTraced = true;
        trace(req,"onchip","CACHE_REGISTER_READY",-1,-1,-1,-1,
              "RF-load", "Onchip",memory.cacheRead->id);
    }
    return true;
}

bool
CfdCoeffPreprocessController::prepareCachedUbar(Request &req)
{
    if (!env.onchipTiming)
        return true;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    if (!line.tileYOnly || line.coefficientCacheHit || req.cacheUbarPublished)
        return true;
    const uint64_t cell = req.desc.cellId;
    if (line.localUbar && cell+1 == line.cells)
        return true;
    const bool ready = req.smeDeferUpper ? req.smeUpperPartial && req.luDone :
        (cell+1 < line.cells ? req.record.uBarReadyCycle != 0 : req.solveDone);
    if (!ready)
        return false;
    auto &store = onchipContexts[req.token].commit[1];
    const Addr base = line.localUbar ? smeUpperAddr(line, cell) :
        cacheEntryAddr(line, cell) + smeCacheTagBytes() + 2*MatrixBytes;
    const unsigned words = N*N +
        (req.smeDeferUpper ? smeUpperFactorWords() : 0);
    if (!store)
        store = startOnchip(line.token,base,words,8,true,
            cell+1 < line.cells ? 1 : 2);
    if (!store || !store->done)
        return false;
    if (req.smeDeferUpper) {
        auto factors = line.smeUpperFactors.begin() + cell*smeUpperFactorWords();
        unsigned index = 0;
        for (unsigned k = env.smeUpperSplit; k < N; ++k)
            factors[index++] = req.reciprocal[k];
        for (unsigned row = env.smeUpperSplit+1; row < N; ++row)
            for (unsigned col = env.smeUpperSplit; col < row; ++col)
                factors[index++] = req.lu[row*N+col];
        for (unsigned row = 0; row < N; ++row)
            for (unsigned col = row+1; col < N; ++col)
                factors[index++] = req.lu[row*N+col];
        assert(index == smeUpperFactorWords());
        lineCoeffCaches.at(line.desc.lineId).uBar[cell] = req.uBar;
        req.cacheUbarPublished = true;
    } else {
        publishCachedUbar(line,req,store->id);
    }
    if (line.localUbar) {
        ++line.smeUpperStored;
        traceSmeUpperMatrix(line, cell, "d", req.d);
        traceSmeUpperMatrix(line, cell, "u", req.upper);
        if (!req.smeDeferUpper)
            traceSmeUpperMatrix(line, cell, "ubar", req.uBar);
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SME_UPPER_MEMORY cycle=%llu token=%llu cell=%llu "
                "event=store job=%llu words=%u\n", (unsigned long long)cycle(),
                (unsigned long long)line.token, (unsigned long long)cell,
                (unsigned long long)store->id, words);
    }
    return true;
}

bool
CfdCoeffPreprocessController::prepareOnchipCommit(Request &req)
{
    if (!env.onchipTiming) return true;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    auto tile = forwardTile(line,req.desc.cellId);
    panic_if(!tile,"CFD SPM commit outside a forward buffer");
    auto &memory = onchipContexts[req.token];
    const uint64_t cell = req.desc.cellId;
    const Addr base = tileOnchipAddr(line,*tile);
    if (!memory.commit[0]) memory.commit[0] = startOnchip(line.token,base+(cell-tile->start)*VectorBytes,N,8,true,1);
    if (!memory.commit[0] || !memory.commit[0]->done) return false;
    if (line.tileYOnly && cell+1 < line.cells && !prepareCachedUbar(req))
        return false;
    if (cell+1 < line.cells && !cachedUbar(line)) {
        if (!memory.commit[1]) memory.commit[1] = startOnchip(line.token,
            base+tile->y.size()*VectorBytes+(cell-tile->start)*MatrixBytes,N*N,8,true,1);
        if (!memory.commit[1] || !memory.commit[1]->done) return false;
    }
    if (env.lineOptLevel >= 10 && !req.localTilePublished) {
        const auto index = cell-tile->start;
        tile->y[index] = req.dqStar;
        if (!line.tileYOnly)
            tile->u[index] = req.uBar;
        markLocalTileReady(line,tile,cell,memory.commit[0]->id,
            !line.tileYOnly && memory.commit[1] ? memory.commit[1]->id : 0,
            line.tileYOnly && req.cacheUbarPublished && memory.commit[1] ?
                memory.commit[1]->id : 0);
        req.localTilePublished = true;
    }
    if (line.coefficientCacheEnabled && !line.coefficientCacheHit) {
        if (!req.luDone || (req.smeInvRequested && !req.smeInvDone)) return false;
        if (line.tileYOnly && !prepareCachedUbar(req)) return false;
        if (!memory.commit[2]) memory.commit[2] = startOnchip(line.token,
            cacheEntryAddr(line, cell),
            (cacheEntryBytes(line)-(line.tileYOnly && !line.localUbar ? MatrixBytes : 0))/8,
            8,true,2);
        if (!memory.commit[2] || !memory.commit[2]->done) return false;
    }
    return true;
}
