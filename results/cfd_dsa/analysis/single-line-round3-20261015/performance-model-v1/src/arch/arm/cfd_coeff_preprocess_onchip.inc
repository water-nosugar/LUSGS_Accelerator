// The banked SPM stores ingress data, coefficient cache and resident tiles.
// Active LU/RHS and product accumulators are registers. Control/output FIFOs
// have their own bounded storage and are not charged as SPM operand accesses.
std::shared_ptr<OnchipAccess>
CfdCoeffPreprocessController::startOnchip(uint64_t owner, Addr base, unsigned words,
    unsigned stride, bool write, unsigned priority, bool active)
{
    if (onchipQueue.size() >= env.onchipDepth) { onchipFull++; return {}; }
    panic_if(!words || base % 8 || stride % 8 || !env.onchipTiming,
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
    access->eligibleCycle = cycle()+1;
    onchipQueue.push_back(access);
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
    onchipCancelled++;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_ONCHIP_CANCEL cycle=%llu id=%llu\n",
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
    for (const auto &a : ready) {
        if (!a->active || a->readyCycle || cycle() < a->eligibleCycle) continue;
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
        if (a->next == a->words) a->readyCycle = cycle()+env.onchipLatency;
    }
}

Addr
CfdCoeffPreprocessController::tileOnchipAddr(const LineRequest &line,const LineTile &tile) const
{
    return line.tileReserve+tile.storageIndex*tile.y.size()*(MatrixBytes+VectorBytes);
}

Addr
CfdCoeffPreprocessController::warmOnchipAddr(const LineRequest &line,uint64_t cell) const
{
    const uint64_t base = (env.inputSlots+env.luPendingDepth+env.solvePendingDepth+env.outputDepth)*8192+
        16384+(env.rhsRowParallel ? env.rhsProductDepth*64+256 : 0)+env.completionDepth*completionEntryBytes(env.lineOptLevel);
    return base+(cell%warmDepth(line))*128;
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
        line.tileReserve+line.tileAllocated+cell*3*MatrixBytes+kind*MatrixBytes;
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
        line.tileReserve+line.tileAllocated+req.desc.cellId*3*MatrixBytes,
        N*N*(req.desc.cellId ? 2 : 1),8,false,0);
    if (!memory.cacheRead || !memory.cacheRead->done) return false;
    if (!memory.cacheReadyTraced) {
        memory.cacheReadyTraced = true;
        trace(req,"onchip","CACHE_REGISTER_READY",-1,-1,-1,-1,
              "RF-load", "Onchip",memory.cacheRead->id);
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
    if (cell+1 < line.cells && !cachedUbar(line)) {
        if (!memory.commit[1]) memory.commit[1] = startOnchip(line.token,
            base+tile->y.size()*VectorBytes+(cell-tile->start)*MatrixBytes,N*N,8,true,1);
        if (!memory.commit[1] || !memory.commit[1]->done) return false;
    }
    if (env.lineOptLevel >= 10 && !req.localTilePublished) {
        const auto index = cell-tile->start;
        tile->y[index] = req.dqStar;
        tile->u[index] = req.uBar;
        markLocalTileReady(line,tile,cell,memory.commit[0]->id,
                          memory.commit[1] ? memory.commit[1]->id : 0);
        req.localTilePublished = true;
    }
    if (line.coefficientCacheEnabled && !line.coefficientCacheHit) {
        if (!memory.commit[2]) memory.commit[2] = startOnchip(line.token,
            line.tileReserve+line.tileAllocated+cell*3*MatrixBytes,3*N*N,8,true,2);
        if (!memory.commit[2] || !memory.commit[2]->done) return false;
    }
    return true;
}
