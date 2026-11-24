// Finite one-line storage. Each buffer has a fixed capacity. Forward child
// drains create DRAM backing. With a full coefficient cache, tiles hold only y;
// cold and warm reverse operands share the same committed Ubar cache cells.
void
CfdCoeffPreprocessController::initializeLineTile(LineRequest &line)
{
    static_assert(sizeof(Request) + sizeof(CfdCoeffPreprocessRecord) <= 8192,
                  "update finite request/terminal-record reservation");
    const uint64_t capacity = env.inputSlots + env.luPendingDepth +
        env.solvePendingDepth + env.outputDepth;
    line.tileReserve = capacity * 8192 + 16384 +
        (env.rhsRowParallel ? env.rhsProductDepth*64+256 : 0);
    // Keep the historical control allowance intact. Explicit final-output
    // snapshots are an additional, separately bounded FIFO (64 B per entry),
    // not free storage borrowed from the preprocessing drain queue.
    if (env.explicitLineOutputDepth)
        line.tileReserve += 64 * env.lineOutputDepth;
    // Conservative added multiplier operand/result pipeline reservation.
    line.tileReserve += 128*((24*env.clusterMulLatency+143)/128)*(env.clusterMulCount-N);
    line.tileReserve += 128*((24*env.clusterAddLatency+143)/128)*(env.clusterAddCount-N);
    if (env.drainTaskDepth > env.outputDepth)
        line.tileReserve += 64ull*(env.drainTaskDepth-env.outputDepth);
    if (env.coldSummary)
        line.tileReserve += 128*env.summaryEngines + 512*env.summaryDepth;
    if (env.arithmeticMode != CfdArithmeticGroups::Legacy)
        line.tileReserve += 256*env.arithmeticDepth;
    if (env.lineOptLevel >= 5)
        line.tileReserve += env.completionDepth * completionEntryBytes(env.lineOptLevel) + 2048 + env.warmWriteDepth * 256 + env.spmOutstanding * 64;
    if (env.onchipTiming)
        line.tileReserve += env.onchipDepth*256 + capacity*128 + 1024;
    if (env.lineOptLevel >= 9)
        line.tileReserve += 1024;
    if (env.lineOptLevel >= 10)
        line.tileReserve += 512;
    if (env.outputCoalesce)
        line.tileReserve += env.spmOutstanding * 128;
    // Each added buffer has a bounded range/phase tag, word-ready bitmap and
    // DMA counters as well as its separately counted data payload.
    line.tileReserve += 512 * env.tilePrefetchBuffers;
    line.tileReserve += env.laneMvm ? 4096 : 0;
    line.tileReserve += localReservation();
    if (env.smeContexts)
        line.tileReserve += CfdSmeZa::reservedBytes(
            env.smeContexts, env.smeConfig.depth, env.smeConfig.ingressPorts,
            env.smeReverseContexts, env.smeWarmRhsWidth != 0,
            env.smeWarmBackSplit, env.smeWarmSingleTile, env.smeWarmBypass,
            env.smeCriticalArb, static_cast<unsigned>(env.smeWarmBaseMode),
            static_cast<unsigned>(env.smeConfig.arrayMode),
            env.smeConfig.maxSegments, env.smeConfig.mixedKind,
            env.smeConfig.panelPack);
    line.tileReserve += 512 * (env.backwardOperandDepth-2);
    if (line.localUbar) {
        const unsigned stride = MatrixBytes + 8*smeUpperFactorWords();
        panic_if(line.tileReserve > env.lineSpmBytes ||
                 line.cells-1 > (env.lineSpmBytes-line.tileReserve)/stride,
                 "CFD private Ubar bank exceeds the line SPM budget");
        line.smeUpperBase = line.tileReserve;
        line.smeUpperBytes = (line.cells-1) * stride;
        line.tileReserve += line.smeUpperBytes;
    }
    const uint64_t cells = std::min<uint64_t>(env.lineTileCells, line.cells);
    const unsigned buffers = env.tileBuffers + env.tilePrefetchBuffers;
    const uint64_t yBytes = cells * VectorBytes * buffers;
    line.tileYOnly = line.localUbar || (line.coefficientCacheEnabled &&
        line.cells <= env.lineCacheCells &&
        line.tileReserve + yBytes + (3*MatrixBytes + smeCacheTagBytes()) *
            line.cells <= env.lineSpmBytes);
    line.tileAllocated = cells *
        (VectorBytes + (line.tileYOnly ? 0 : MatrixBytes)) * buffers;
    panic_if(!cells || line.tileReserve + line.tileAllocated > env.lineSpmBytes,
             "CFD finite line SPM cannot hold reserved state and tile buffers");
    auto make = [cells, yOnly = line.tileYOnly] {
        auto tile = std::make_shared<LineTile>();
        tile->count = cells;
        tile->y.resize(cells);
        if (!yOnly)
            tile->u.resize(cells);
        return tile;
    };
    line.tile = make();
    if (env.tileBuffers == 2) {
        line.prefetchTile = make();
        line.prefetchTile->storageIndex = 1;
        line.prefetchTile->start = cells;
        line.prefetchTile->count = std::min<uint64_t>(cells, line.cells-cells);
        if (!line.prefetchTile->count)
            line.prefetchTile->phase = LineTile::Phase::Idle;
    }
    for (unsigned i=0; i<env.tilePrefetchBuffers; ++i) {
        line.aheadTiles[i] = make();
        line.aheadTiles[i]->storageIndex = i+2;
        line.aheadTiles[i]->phase = LineTile::Phase::Idle;
    }
    if (env.smeForwardTileRotate) {
        line.tile->phase = LineTile::Phase::Idle;
        line.prefetchTile->phase = LineTile::Phase::Idle;
        refillForwardTiles(line);
    }
    if (env.traceEnable && env.tileCellOrder)
        std::fprintf(stderr,"CFD_TILE_READ_POLICY token=%llu cell_order=1 vector_bytes=%u matrix_bytes=%u\n",
            (unsigned long long)line.token,VectorBytes,MatrixBytes);
}

std::array<std::shared_ptr<LineTile>, 6>
CfdCoeffPreprocessController::lineTiles(const LineRequest &line) const
{
    return {line.tile, line.prefetchTile, line.aheadTiles[0], line.aheadTiles[1],
            line.aheadTiles[2], line.aheadTiles[3]};
}

void
CfdCoeffPreprocessController::refillForwardTiles(LineRequest &line)
{
    if (line.nextForwardTileCell == line.cells)
        return;
    // Reserve consecutive ranges in the existing physical buffers. Before
    // backward swaps their pointers, lineTiles() is in storage-index order.
    const auto tiles = lineTiles(line);
    const unsigned buffers = env.tileBuffers + env.tilePrefetchBuffers;
    while (line.nextForwardTileCell < line.cells) {
        const uint64_t first = line.nextForwardTileCell;
        auto &tile = *tiles[(first / env.lineTileCells) % buffers];
        // The count of retired children fences the external writes and any
        // local stores. A still-live range keeps its physical storage.
        if (tile.phase == LineTile::Phase::Forward)
            break;
        tile.start = first;
        tile.count = std::min<uint64_t>(env.lineTileCells, line.cells - first);
        tile.retired = 0;
        tile.lastWriteCycle = 0;
        tile.readyMask.fill(0);
        tile.readyCount = 0;
        tile.phase = LineTile::Phase::Forward;
        std::fill(tile.y.begin(), tile.y.end(), std::array<double, N>{});
        std::fill(tile.u.begin(), tile.u.end(), std::array<double, N*N>{});
        line.nextForwardTileCell += tile.count;
        if (env.traceEnable)
            std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=forward_allocate "
                "first=%llu cells=%llu storage=%u\n", (unsigned long long)cycle(),
                (unsigned long long)line.token, (unsigned long long)first,
                (unsigned long long)tile.count, tile.storageIndex);
    }
}

void
CfdCoeffPreprocessController::prepareLineCache(LineRequest &line)
{
    const bool direct = line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
    const bool reciprocalDiagonal = env.warmReciprocal && env.reciprocalSolve &&
        direct && (line.cells > 1 || env.luReciprocalShare || env.smeContexts);
    const auto available = env.lineSpmBytes-line.tileReserve-line.tileAllocated;
    const unsigned tagBytes = smeCacheTagBytes();
    const bool solveOnly = !line.localUbar && env.adaptiveCache && direct &&
        (3*MatrixBytes + tagBytes)*line.cells > available;
    const unsigned matrices = line.localUbar || solveOnly ? 2 : 3;
    const uint64_t entryBytes = matrices*MatrixBytes + tagBytes;
    line.coefficientCacheEnabled &= line.cells <= env.lineCacheCells &&
        entryBytes*line.cells <= available;
    if (!line.coefficientCacheEnabled && !line.localUbar) {
        lineCoeffCaches.clear();
        return;
    }
    if (line.coefficientCacheEnabled)
        line.cacheAllocated = entryBytes*line.cells;
    panic_if(line.tileYOnly && solveOnly, "CFD y-only tile without cached Ubar");
    auto it = lineCoeffCaches.find(line.desc.lineId);
    if (line.coefficientCacheEnabled && !line.coefficientDirty &&
        it != lineCoeffCaches.end() && it->second.valid &&
        it->second.localUbar == line.localUbar &&
        it->second.hasUbar == !solveOnly &&
        it->second.reciprocalDiagonal == reciprocalDiagonal &&
        (!solveOnly || it->second.ubarBacking == line.desc.uBarOutAddr) &&
        it->second.cells == line.cells && it->second.version == line.coefficientVersion &&
        it->second.directBaseSolve == direct &&
        it->second.privateLuOutput == bool(line.desc.flags & CFD_COEFF_PRE_LOCAL_LU))
        return;
    // Release the previous entry before allocating the next: one finite cache.
    lineCoeffCaches.clear();
    auto &cache = lineCoeffCaches[line.desc.lineId];
    cache.cells = line.cells;
    cache.version = line.coefficientVersion;
    cache.directBaseSolve = direct;
    cache.privateLuOutput = line.desc.flags & CFD_COEFF_PRE_LOCAL_LU;
    cache.localUbar = line.localUbar;
    cache.reciprocalDiagonal = reciprocalDiagonal;
    cache.hasUbar = !solveOnly;
    cache.ubarBacking = line.desc.uBarOutAddr;
    if (line.coefficientCacheEnabled) {
        (direct ? cache.lu : cache.dInv).resize(line.cells);
        if (tagBytes)
            cache.smeBaseTags.resize(line.cells, SmeCacheBase::Direct);
        cache.lBar.resize(line.cells);
    }
    if (cache.hasUbar)
        cache.uBar.resize(line.cells - line.localUbar);
}

void
CfdCoeffPreprocessController::publishCachedUbar(LineRequest &line, Request &req,
                                              uint64_t job)
{
    if (req.cacheUbarPublished)
        return;
    auto &cache = lineCoeffCaches.at(line.desc.lineId);
    panic_if(!cache.hasUbar || cache.version != line.coefficientVersion ||
             cache.cells != line.cells || line.abortPending,
             "CFD publishing Ubar into the wrong coefficient cache");
    cache.uBar[req.desc.cellId] = req.uBar;
    req.cacheUbarPublished = true;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_UBAR_CACHE_READY cycle=%llu token=%llu cell=%llu "
            "version=%llu job=%llu\n", (unsigned long long)cycle(),
            (unsigned long long)line.token,(unsigned long long)req.desc.cellId,
            (unsigned long long)line.coefficientVersion,(unsigned long long)job);
}

uint64_t
CfdCoeffPreprocessController::tileIndex(const LineRequest &line, uint64_t cell) const
{
    panic_if(!line.tile || cell < line.tile->start ||
             cell >= line.tile->start + line.tile->count, "CFD cell outside resident tile");
    return cell - line.tile->start;
}

void
CfdCoeffPreprocessController::commitTileChild(LineRequest &line, Request &req)
{
    if (req.status != CfdCoeffPreprocessStatus::Complete) {
        abortLine(line, req.status);
        return;
    }
    // An earlier failed child invalidates this invocation, including children
    // whose arithmetic happened to finish before cancellation reached them.
    if (line.abortPending)
        return;
    const uint64_t cell = req.desc.cellId;
    auto tile = line.tile;
    // Early backward may make the newer tile current while an older forward
    // child still awaits DMA completion. Either range can own this commit.
    if (cell < tile->start || cell >= tile->start + tile->count) {
        tile.reset();
        for (const auto &candidate : lineTiles(line))
            if (candidate && candidate->phase == LineTile::Phase::Forward &&
                cell >= candidate->start && cell < candidate->start+candidate->count) {
                tile = candidate;
                break;
            }
    }
    panic_if(!tile || tile->phase != LineTile::Phase::Forward || cell < tile->start ||
             cell >= tile->start+tile->count, "CFD retired child outside forward buffers");
    tile->y[cell-tile->start] = req.dqStar;
    if (!line.tileYOnly)
        tile->u[cell-tile->start] = req.uBar;
    tile->retired++;
    line.committedChildren++;
    const uint64_t bytes = VectorBytes + (!line.localUbar && cell+1 < line.cells &&
        (!line.coefficientCacheHit || env.lineOptLevel < 3) ? MatrixBytes : 0);
    line.tileWriteIssued += bytes;
    line.tileWriteCompleted += bytes;
    const uint64_t writeDone = std::max(
        req.record.dqStarPublishCycle,
        line.tileYOnly ? uint64_t(0) : req.record.uBarDrainDone);
    tile->lastWriteCycle = std::max(tile->lastWriteCycle, writeDone);
    line.tileLastResponse = std::max({line.tileLastResponse,
        req.record.dqStarPublishCycle, req.record.uBarDrainDone});
    if (line.coefficientCacheEnabled && !line.coefficientCacheHit) {
        auto &cache = lineCoeffCaches.at(line.desc.lineId);
        const bool inverse = req.smeInvRequested && req.smeInvDone &&
            !req.smeInvRejected;
        if (!cache.smeBaseTags.empty())
            cache.smeBaseTags[cell] = inverse ? SmeCacheBase::Inverse :
                req.smeInvRejected ? SmeCacheBase::GuardFallback : SmeCacheBase::Direct;
        (req.directBaseSolve ? cache.lu : cache.dInv)[cell] =
            req.directBaseSolve && !inverse ? req.lu : req.dInv;
        if (cache.reciprocalDiagonal && !inverse) {
            panic_if(!std::all_of(req.reciprocalReady.begin(),req.reciprocalReady.end(),
                         [](bool ready) { return ready; }),
                     "CFD reciprocal cache encoded before divider completion");
            for (unsigned k = 0; k < N; ++k)
                cache.lu[cell][k*N+k] = req.reciprocal[k];
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_LU_CACHE_FORMAT cycle=%llu token=%llu cell=%llu version=%llu format=reciprocal-diagonal\n",
                    (unsigned long long)cycle(),(unsigned long long)line.token,
                    (unsigned long long)cell,(unsigned long long)line.coefficientVersion);
        }
        cache.lBar[cell] = req.lBar;
        if (!line.localUbar && cache.hasUbar)
            publishCachedUbar(line, req);
        if (!line.localUbar && line.committedChildren == line.cells)
            publishLineCache(line);
    }
}

void
CfdCoeffPreprocessController::publishLineCache(LineRequest &line)
{
    lineCoeffCaches.at(line.desc.lineId).valid = true;
    if (env.traceEnable)
        std::fprintf(stderr, "CFD_LINE_CACHE_STORE line=%llu version=%llu cells=%llu\n",
            (unsigned long long)line.desc.lineId,
            (unsigned long long)line.coefficientVersion, (unsigned long long)line.cells);
}

void
CfdCoeffPreprocessController::retireTileChild(LineRequest &line, Request &req)
{
    if (!req.tileDataCommitted)
        commitTileChild(line, req);
    line.liveChildren--;
    line.completedChildren++;
    if (line.completedChildren == line.cells) {
        line.childrenRetiredCycle = cycle();
        forwardHandoffs.erase(line.desc.lineId);
    }
}

bool
CfdCoeffPreprocessController::prepareCachedRhs(LineRequest &line)
{
    if (env.lineOptLevel >= 3) {
        const auto cell = line.cachedForwardCell;
        auto &slot = line.warm[cell % warmDepth(line)];
        if (!slot.valid || slot.cell != cell || slot.rhs->completed != VectorBytes) {
            line.warmInputStalls++;
            return false;
        }
        if (!warmRhsOnchipReady(line,slot)) { line.warmInputStalls++; return false; }
        std::memcpy(line.cachedRhs.data(), slot.rhs->bytes.data(), VectorBytes);
        return true;
    }
    if (!line.rhsTransfer) {
        line.rhsTransfer = std::make_shared<InterfaceTransfer>();
        auto &t = *line.rhsTransfer;
        t.interface = false;
        t.kind = "rhs";
        t.cell = line.cachedForwardCell;
        t.addr = line.desc.reserved[0] + t.cell*VectorBytes;
        t.bytes.resize(VectorBytes);
    }
    stepInterfaceTransfer(line.token, line.rhsTransfer);
    if (line.rhsTransfer->completed != VectorBytes)
        return false;
    std::memcpy(line.cachedRhs.data(), line.rhsTransfer->bytes.data(), VectorBytes);
    return true;
}

void
CfdCoeffPreprocessController::stepWarmTransfers(LineRequest &line)
{
    const auto depth = warmDepth(line);
    for (auto it = line.warmWrites.begin(); it != line.warmWrites.end();) {
        if (env.onchipTiming) {
            const Addr address = tileOnchipAddr(line,*it->tile)+
                (it->transfer->cell-it->tile->start)*VectorBytes;
            if (!it->localStore) it->localStore = startOnchip(line.token,address,N,8,true,1);
            if (!it->localStore || !it->localStore->done) { ++it; continue; }
            if (env.lineOptLevel >= 10 && !it->localPublished) {
                markLocalTileReady(line,it->tile,it->transfer->cell,it->localStore->id,0);
                it->localPublished = true;
            }
            // The bounded WarmWrite already owns a result snapshot. A FIFO
            // route broadcasts that snapshot to SPM and DMA; no SRAM reread.
            if (!env.warmYFifo) {
                if (!it->localRead) it->localRead = startOnchip(line.token,address,N,8,false,1);
                if (!it->localRead || !it->localRead->done) { ++it; continue; }
            }
        }
        stepInterfaceTransfer(line.token, it->transfer);
        if (it->transfer->completed != VectorBytes) { ++it; continue; }
        panic_if(it->tile->phase != LineTile::Phase::Forward,
                 "CFD warm tile reused before its write response");
        it->tile->retired++;
        it->tile->lastWriteCycle = std::max(it->tile->lastWriteCycle, it->transfer->lastResponse);
        line.completedChildren++;
        line.nextCell++;
        line.tileWriteIssued += VectorBytes;
        line.tileWriteCompleted += VectorBytes;
        line.tileLastResponse = std::max(line.tileLastResponse, it->transfer->lastResponse);
        it = line.warmWrites.erase(it);
        if (line.completedChildren == line.cells) line.childrenRetiredCycle = cycle();
    }
    for (auto &slot : line.warm) {
        if (!slot.valid) continue;
        stepInterfaceTransfer(line.token, slot.rhs);
        if (!slot.y) continue;
        stepInterfaceTransfer(line.token, slot.y);
        if (slot.y->completed != VectorBytes) continue;
        panic_if(slot.tile->phase != LineTile::Phase::Forward,
                 "CFD warm buffer reused before write response");
        slot.tile->retired++;
        slot.tile->lastWriteCycle = std::max(slot.tile->lastWriteCycle, slot.y->lastResponse);
        line.completedChildren++;
        line.nextCell++;
        line.tileWriteIssued += VectorBytes;
        line.tileWriteCompleted += VectorBytes;
        line.tileLastResponse = std::max(line.tileLastResponse, slot.y->lastResponse);
        slot = WarmSlot{};
        if (line.completedChildren == line.cells)
            line.childrenRetiredCycle = cycle();
    }
    while (line.warmNextInput < line.cells &&
           line.warmNextInput < line.cachedForwardCell + depth) {
        auto &slot = line.warm[line.warmNextInput % depth];
        if (slot.valid) break;
        slot.cell = line.warmNextInput++;
        slot.valid = true;
        slot.rhs = std::make_shared<InterfaceTransfer>();
        slot.rhs->interface = false;
        slot.rhs->kind = "rhs";
        slot.rhs->cell = slot.cell;
        slot.rhs->addr = line.desc.reserved[0] + slot.cell*VectorBytes;
        slot.rhs->bytes.resize(VectorBytes);
        line.warmAheadInputs += slot.cell > line.cachedForwardCell;
        stepInterfaceTransfer(line.token, slot.rhs);
    }
}

bool
CfdCoeffPreprocessController::drainCachedCell(LineRequest &line)
{
    for (auto t : {line.yTransfer, line.uTransfer}) {
        if (!t) continue;
        stepInterfaceTransfer(line.token, t);
        if (t->completed != t->bytes.size())
            return false;
    }
    const uint64_t cell = line.cachedForwardCell;
    line.previousY = line.tile->y[tileIndex(line, cell)];
    line.tile->retired++;
    line.completedChildren++;
    line.nextCell++;
    const uint64_t bytes = VectorBytes + (cell+1 < line.cells ? MatrixBytes : 0);
    line.tileWriteIssued += bytes;
    line.tileWriteCompleted += bytes;
    line.tileLastResponse = std::max(line.tileLastResponse,
        std::max(line.yTransfer->lastResponse, line.uTransfer ? line.uTransfer->lastResponse : 0));
    line.rhsTransfer.reset(); line.yTransfer.reset(); line.uTransfer.reset();
    line.cachedBaseAccumulator.fill(0.0);
    line.cachedCorrectionAccumulator.fill(0.0);
    line.cachedForwardPhase = 0;
    line.cachedForwardCell++;
    if (line.cachedForwardCell == line.cells) {
        line.cachedForwardStarted = false;
        line.childrenRetiredCycle = cycle();
    }
    return true;
}

void
CfdCoeffPreprocessController::beginTileRead(LineRequest &line,
    const std::shared_ptr<LineTile> &tile, uint64_t first)
{
    tile->start = first;
    tile->count = std::min<uint64_t>(env.lineTileCells, line.cells-first);
    tile->phase = LineTile::Phase::Read;
    tile->readyMask.fill(0);
    tile->readyCount = 0;
    tile->issued[0] = tile->issued[1] = 0;
    tile->completed[0] = tile->completed[1] = 0;
    std::fill(tile->y.begin(), tile->y.end(), std::array<double, N>{});
    std::fill(tile->u.begin(), tile->u.end(), std::array<double, N*N>{});
    if (env.traceEnable)
        std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=reload_start "
            "first=%llu cells=%llu prefetch=%d storage=%u stream=%d\n", (unsigned long long)cycle(),
            (unsigned long long)line.token, (unsigned long long)first,
            (unsigned long long)tile->count, tile != line.tile, tile->storageIndex,
            env.reverseTileReload && (line.tileYOnly || env.tileOperandStream) &&
            tile->count*(line.tileYOnly ? N : N+N*N)<=tile->readyMask.size()*64);
}

void
CfdCoeffPreprocessController::stepTileRead(LineRequest &line,
    const std::shared_ptr<LineTile> &tile)
{
    if (!tile || tile->phase != LineTile::Phase::Read)
        return;
    CfdLocalSpm *spm = getCfdLocalSpm();
    panic_if(!spm, "CFD finite tiles require local SPM DMA");
    const uint64_t token = line.token;
    const uint64_t sizes[] = {tile->count*VectorBytes,
                             cachedUbar(line) ? 0 : tile->count*MatrixBytes};
    const bool cellOrdered = env.tileCellOrder && sizes[1] &&
        env.reverseTileReload && env.tileOperandStream &&
        tile->count*(N+N*N) <= tile->readyMask.size()*64;
    const unsigned firstRegion = cellOrdered ? CfdTileStream::firstRegion(
        tile->issued[0],tile->issued[1],sizes[0],sizes[1]) : cycle()&1;
    const uint64_t orderY = tile->issued[0], orderU = tile->issued[1];
    bool done = true;
    for (unsigned pass = 0; pass < 2; ++pass) {
        // Match issued cell coverage when enabled; otherwise retain legacy
        // packet alternation. The second region may use spare issue credits.
        const unsigned region = env.tileOperandStream && !line.tileYOnly ?
            (pass+firstRegion)%2 : pass;
        done &= tile->completed[region] == sizes[region];
        if (tile->issued[region] == sizes[region]) continue;
        const Addr base = region == 0 ? line.desc.reserved[3] : line.desc.uBarOutAddr;
        const unsigned stride = region == 0 ? VectorBytes : MatrixBytes;
        const bool streaming = env.reverseTileReload &&
            (line.tileYOnly || env.tileOperandStream) &&
            tile->count*(line.tileYOnly ? N : N+N*N)<=tile->readyMask.size()*64;
        uint64_t offset=tile->issued[region];
        unsigned bytes;
        if (streaming) {
            const uint64_t remaining=sizes[region]-tile->issued[region];
            bytes=std::min<uint64_t>({64,env.tileDmaWidth,remaining});
            // Find the largest suffix inside one translated cache-line/page
            // fragment. The splitter is bounded to eight 8-byte candidates.
            while (spm->dmaChunkSize(tc,base+tile->start*stride+remaining-bytes,bytes)!=bytes)
                bytes-=8;
            panic_if(!bytes,"CFD reverse tile splitter made no progress");
            offset=remaining-bytes;
        } else {
            bytes=spm->dmaChunkSize(tc,base+tile->start*stride+offset,
                std::min<uint64_t>(env.tileDmaWidth,sizes[region]-tile->issued[region]));
        }
        const Addr addr = base + tile->start*stride + offset;
        uint8_t *data = region == 0 ? reinterpret_cast<uint8_t *>(tile->y.data()) :
            reinterpret_cast<uint8_t *>(tile->u.data());
        data += offset;
        if (spm->dmaOutstandingFull(env.spmOutstanding)) {
            spm->noteDmaOutstandingStall();
            line.tileStalls++;
            continue;
        }
        auto local = env.onchipTiming ? startOnchip(token,tileOnchipAddr(line,*tile)+
            (region ? tile->y.size()*VectorBytes : 0)+offset,
            (bytes+7)/8,8,true,1,false) : nullptr;
        if (env.onchipTiming && !local) continue;
        const auto packet = spm->issueDma(tc, false, addr, bytes, data,
            env.spmOutstanding, [this, token, tile, region, bytes, local,offset,streaming](uint64_t id) {
                const uint64_t job=local ? local->id : 0;
                auto publish = [this,token,tile,region,bytes,id,offset,streaming,job] {
                tile->completed[region] += bytes;
                auto it = lineRequests.find(token);
                if (it == lineRequests.end()) return;
                auto &line = *it->second;
                line.tileReadCompleted += bytes;
                line.tilePacketsCompleted++;
                line.tileLastResponse = cycle();
                if (env.traceEnable)
                    std::fprintf(stderr, "CFD_TILE_DMA cycle=%llu token=%llu event=response "
                        "first=%llu region=%u bytes=%u packet=%llu offset=%llu job=%llu\n",
                        (unsigned long long)cycle(), (unsigned long long)token,
                        (unsigned long long)tile->start, region, bytes, (unsigned long long)id,
                        (unsigned long long)offset,(unsigned long long)job);
                if (streaming) {
                    const unsigned firstWord = region ? tile->count*N : 0;
                    for (unsigned word=offset/8;word<(offset+bytes)/8;++word) {
                        const unsigned index = firstWord+word;
                        const uint64_t bit=1ull<<(index%64);
                        panic_if(tile->readyMask[index/64]&bit,"CFD duplicate tile reload word");
                        tile->readyMask[index/64]|=bit;
                    }
                    if (region == 0)
                    for (unsigned cell=offset/VectorBytes;cell<=(offset+bytes-1)/VectorBytes;++cell) {
                        if (!backwardTileReadable(*tile,tile->start+cell)) continue;
                        tile->readyCount++;
                        if (env.traceEnable)
                            std::fprintf(stderr,"CFD_TILE_CELL_READY cycle=%llu token=%llu first=%llu cell=%llu\n",
                                (unsigned long long)cycle(),(unsigned long long)token,
                                (unsigned long long)tile->start,(unsigned long long)(tile->start+cell));
                    }
                }
                };
                if (local) {
                        activateOnchip(local, std::move(publish));
                    }
                else publish();
            });
        if (!packet.accepted) {
            cancelOnchip(local);
            panic_if(!packet.outstandingFull, "CFD tile DMA translation failed");
            line.tileStalls++;
            continue;
        }
        if (env.traceEnable && cellOrdered)
            std::fprintf(stderr,"CFD_TILE_ORDER cycle=%llu token=%llu first=%llu packet=%llu y_issued=%llu u_issued=%llu preferred=%u pass=%u region=%u\n",
                (unsigned long long)cycle(),(unsigned long long)token,
                (unsigned long long)tile->start,(unsigned long long)packet.packetId,
                (unsigned long long)orderY,(unsigned long long)orderU,
                firstRegion,pass,region);
        tile->issued[region] += bytes;
        line.tileReadIssued += bytes;
        line.tilePacketsIssued++;
        if (env.traceEnable)
            std::fprintf(stderr, "CFD_TILE_DMA cycle=%llu token=%llu event=issue "
                "first=%llu region=%u bytes=%u packet=%llu offset=%llu job=%llu\n",
                (unsigned long long)cycle(), (unsigned long long)token,
                (unsigned long long)tile->start, region, bytes, (unsigned long long)packet.packetId,
                (unsigned long long)offset,(unsigned long long)(local ? local->id : 0));
    }
    if (done) {
        tile->phase = LineTile::Phase::Resident;
        line.tileReloadCount++;
        if (env.traceEnable)
            std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=reload_ready "
                "first=%llu cells=%llu\n", (unsigned long long)cycle(),
                (unsigned long long)token, (unsigned long long)tile->start,
                (unsigned long long)tile->count);
    }
}

void
CfdCoeffPreprocessController::processLineTiles()
{
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        if (!line.tile || line.terminal) continue;
        if (env.lineOptLevel >= 10 && env.lineEarlyBackward &&
            (line.earlyBackward || startEarlyBackward(line))) {
            processEarlyLineTiles(line);
            continue;
        }
        if (env.lineOptLevel >= 10 && !env.lineEarlyBackward && line.backwardStarted) {
            processEarlyLineTiles(line);
            continue;
        }
        if (env.smeForwardTileRotate) {
            commitReadyForwardTiles(line);
            refillForwardTiles(line);
            if (!env.lineEarlyBackward && line.nextForwardTileCell == line.cells) {
                const auto tiles = lineTiles(line);
                const bool allCommitted = std::none_of(
                    tiles.begin(), tiles.end(), [](const auto &t) {
                        return t && t->phase == LineTile::Phase::Forward;
                    });
                if (allCommitted) {
                    std::shared_ptr<LineTile> tail, active;
                    for (const auto &t : tiles) {
                        if (!t || t->phase != LineTile::Phase::Resident)
                            continue;
                        if (line.cells - 1 >= t->start &&
                            line.cells - 1 < t->start + t->count)
                            tail = t;
                        if (line.cells > 1 && line.cells - 2 >= t->start &&
                            line.cells - 2 < t->start + t->count)
                            active = t;
                    }
                    panic_if(!tail || (line.cells > 1 && !active),
                             "CFD non-early rotated backward missing tail tile");
                    if (env.onchipTiming) {
                        if (!line.tailRead)
                            line.tailRead = startOnchip(
                                line.token,
                                tileOnchipAddr(line, *tail) +
                                    (line.cells - 1 - tail->start) * VectorBytes,
                                N, 8, false, 0);
                        if (!line.tailRead || !line.tailRead->done)
                            continue;
                    }
                    if (!active)
                        active = tail;
                    if (line.tile != active) {
                        if (line.prefetchTile == active) {
                            std::swap(line.tile, line.prefetchTile);
                        } else {
                            auto slot = std::find(
                                line.aheadTiles.begin(), line.aheadTiles.end(), active);
                            panic_if(slot == line.aheadTiles.end(),
                                     "CFD backward tile has no storage owner");
                            std::swap(line.tile, *slot);
                        }
                    }
                    const uint64_t now = cycle();
                    const uint64_t fbReady =
                        now + (env.smeContexts && env.smeFeedbackReg ? 1u : 0u);
                    line.backwardStarted = true;
                    line.backwardStartCycle = now;
                    line.backwardFeedbackReadyCycle = fbReady;
                    line.backwardPartReadyCycle = {fbReady, fbReady};
                    line.backwardCell = static_cast<int64_t>(line.cells) - 2;
                    line.backwardValue = tail->y[line.cells - 1 - tail->start];
                    line.backwardNext = line.backwardValue;
                    publishLineOutput(line, line.cells - 1);
                    if (env.traceEnable)
                        std::fprintf(stderr,
                            "CFD_BACKWARD_SEED cycle=%llu token=%llu tail=%llu "
                            "first=%llu tail_job=%llu early=0\n",
                            (unsigned long long)now, (unsigned long long)line.token,
                            (unsigned long long)tail->start,
                            (unsigned long long)active->start,
                            (unsigned long long)(line.tailRead ? line.tailRead->id : 0));
                    processEarlyLineTiles(line);
                }
            }
            continue;
        }
        auto tile = line.tile;
        if (tile->phase == LineTile::Phase::Forward && tile->retired == tile->count) {
            if (env.onchipTiming && tile->start+tile->count == line.cells) {
                if (!line.tailRead) line.tailRead = startOnchip(line.token,
                    tileOnchipAddr(line,*tile)+(tile->count-1)*VectorBytes,N,8,false,0);
                if (!line.tailRead || !line.tailRead->done) continue;
            }
            line.tileForwardCount++;
            if (env.traceEnable)
                std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=forward_commit "
                    "first=%llu cells=%llu storage=%u last_write=%llu\n",
                    (unsigned long long)cycle(),
                    (unsigned long long)token, (unsigned long long)tile->start,
                    (unsigned long long)tile->count, tile->storageIndex,
                    (unsigned long long)tile->lastWriteCycle);
            const uint64_t next = tile->start + tile->count;
            if (next < line.cells) {
                if (line.prefetchTile) {
                    std::swap(line.tile, line.prefetchTile);
                    const uint64_t first = line.tile->start+line.tile->count;
                    if (env.lineOptLevel >= 6 && first >= line.cells) {
                        // Retain the completed preceding tile for the first
                        // backward cells. All its write responses have arrived.
                        tile->phase = LineTile::Phase::Resident;
                        continue;
                    }
                    tile->start = first;
                    tile->count = std::min<uint64_t>(env.lineTileCells, line.cells-first);
                } else {
                    tile->start = next;
                    tile->count = std::min<uint64_t>(env.lineTileCells, line.cells-next);
                }
                tile->retired = 0;
                tile->lastWriteCycle = 0;
                tile->readyMask.fill(0);
                tile->readyCount = 0;
                tile->phase = tile->count ? LineTile::Phase::Forward : LineTile::Phase::Idle;
                std::fill(tile->y.begin(), tile->y.end(), std::array<double, N>{});
                std::fill(tile->u.begin(), tile->u.end(), std::array<double, N*N>{});
                continue;
            }
            tile->phase = LineTile::Phase::Resident;
            if (line.prefetchTile && !(env.lineOptLevel >= 6 &&
                line.prefetchTile->phase == LineTile::Phase::Resident))
                line.prefetchTile->phase = LineTile::Phase::Idle;
            const uint64_t now = cycle();
            const uint64_t fbReady =
                now + (env.smeContexts && env.smeFeedbackReg ? 1u : 0u);
            const uint64_t tailStart = tile->start;
            line.backwardStarted = true;
            line.backwardStartCycle = now;
            line.backwardFeedbackReadyCycle = fbReady;
            line.backwardPartReadyCycle = {fbReady, fbReady};
            line.backwardCell = static_cast<int64_t>(line.cells)-2;
            line.backwardValue = tile->y[tileIndex(line, line.cells-1)];
            line.backwardNext = line.backwardValue;
            publishLineOutput(line, line.cells-1);
            const bool swapPreceding = env.lineOptLevel >= 6 && line.prefetchTile &&
                tile->start && line.cells > 1 && line.cells - 2 < tile->start &&
                line.prefetchTile->phase == LineTile::Phase::Resident;
            if (swapPreceding) {
                std::swap(line.tile, line.prefetchTile);
                line.prefetchTile->phase = LineTile::Phase::Idle;
                if (env.traceEnable)
                    std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=retained_ready "
                        "first=%llu cells=%llu\n", (unsigned long long)now,
                        (unsigned long long)token, (unsigned long long)line.tile->start,
                        (unsigned long long)line.tile->count);
            }
            if (env.traceEnable && line.tailRead)
                std::fprintf(stderr,
                    "CFD_BACKWARD_SEED cycle=%llu token=%llu tail=%llu "
                    "first=%llu tail_job=%llu early=0\n",
                    (unsigned long long)now, (unsigned long long)line.token,
                    (unsigned long long)tailStart,
                    (unsigned long long)line.tile->start,
                    (unsigned long long)line.tailRead->id);
            if (env.lineOptLevel >= 10 && env.tilePrefetchBuffers) {
                processEarlyLineTiles(line);
                continue;
            }
            if (swapPreceding) {
                if (line.tile->start)
                    beginTileRead(line, line.prefetchTile, line.tile->start-env.lineTileCells);
            } else if (line.prefetchTile && tile->start &&
                       line.prefetchTile->phase != LineTile::Phase::Resident) {
                beginTileRead(line, line.prefetchTile, tile->start-env.lineTileCells);
            }
        }
        // DMA advances during the current tile's arithmetic, independently of
        // feedback readiness. Only swap buffers at a completed cell boundary.
        stepTileRead(line, line.tile);
        stepTileRead(line, line.prefetchTile);
        if (!line.backwardStarted || line.backwardCell < 0) continue;
        const uint64_t cell = line.backwardCell;
        if (cell < line.tile->start && !line.backwardOpPending) {
            if (line.prefetchTile) {
                if (line.prefetchTile->phase == LineTile::Phase::Resident) {
                    std::swap(line.tile, line.prefetchTile);
                    line.prefetchTile->phase = LineTile::Phase::Idle;
                    if (line.tile->start)
                        beginTileRead(line, line.prefetchTile, line.tile->start-env.lineTileCells);
                }
            } else if (line.tile->phase == LineTile::Phase::Resident) {
                beginTileRead(line, line.tile, (cell/env.lineTileCells)*env.lineTileCells);
            }
        }
        if (line.tile->phase != LineTile::Phase::Resident || cell < line.tile->start)
            line.tileWaitCycles++;
    }
}
