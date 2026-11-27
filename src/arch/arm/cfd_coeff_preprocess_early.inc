// Resident data becomes readable after its modeled SPM stores. Its external
// writeback owns independent snapshots; final completion still fences them.
bool
CfdCoeffPreprocessController::holdTailUbar(const Request &req) const
{
    if (!env.tailUbarOverlap || !req.directBaseSolve || req.coefficientCacheReuse)
        return false;
    const auto owner = childLineTokens.find(req.token);
    if (owner == childLineTokens.end()) return false;
    const auto &line = *lineRequests.at(owner->second);
    return !line.backwardStarted && req.desc.cellId+1 < line.cells &&
           req.desc.cellId+env.tailUbarOverlap+1 >= line.cells;
}

bool
CfdCoeffPreprocessController::forwardYReadable(
    const LineRequest &line,const LineTile &tile,uint64_t cell) const
{
    if (backwardTileReadable(tile,cell)) return true;
    if (!env.tailUbarOverlap || cell < tile.start || cell >= tile.start+tile.count)
        return false;
    // Only bounded, still-live request registers can supply a y-only commit.
    // There is no additional line-sized readiness or LU storage.
    for (const auto &[token,ptr] : requests) {
        const auto owner = childLineTokens.find(token);
        if (owner != childLineTokens.end() && owner->second == line.token &&
            ptr->desc.cellId == cell && ptr->localYPublished &&
            ptr->stage != RequestStage::Failed) return true;
    }
    return false;
}

void
CfdCoeffPreprocessController::prepareLocalY(Request &req)
{
    if (req.localYPublished || !req.tailUbarMarked || !req.dqStarReadyLocal)
        return;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    auto tile = forwardTile(line,req.desc.cellId);
    panic_if(!tile,"CFD deferred y outside its reserved forward tile");
    auto &store = onchipContexts[req.token].commit[0];
    if (!store) store = startOnchip(line.token,tileOnchipAddr(line,*tile)+
        (req.desc.cellId-tile->start)*VectorBytes,N,8,true,1);
    if (!store || !store->done) return;
    tile->y[req.desc.cellId-tile->start] = req.dqStar;
    req.localYPublished = true;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LOCAL_Y_READY cycle=%llu token=%llu request=%llu generation=%llu cell=%llu first=%llu storage=%u y_job=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)line.token,
            (unsigned long long)req.record.requestId,(unsigned long long)req.desc.generation,
            (unsigned long long)req.desc.cellId,(unsigned long long)tile->start,
            tile->storageIndex,(unsigned long long)store->id);
}

bool
CfdCoeffPreprocessController::backwardTileReadable(const LineTile &tile,uint64_t cell) const
{
    if (cell < tile.start || cell >= tile.start+tile.count) return false;
    if (tile.phase == LineTile::Phase::Resident) return true;
    const unsigned local = cell-tile.start;
    if (env.reverseTileReload && tile.phase == LineTile::Phase::Read &&
        (tile.u.empty() || env.tileOperandStream) &&
        tile.count*(tile.u.empty() ? N : N+N*N) <= tile.readyMask.size()*64) {
        // During a streamed reload the existing ready bitmap tracks words,
        // rather than forward cells. All five y words must be in SPM.
        for (unsigned word=local*N;word<(local+1)*N;++word)
            if (!(tile.readyMask[word/64] & (1ull<<(word%64)))) return false;
        return true;
    }
    return env.lineOptLevel >= 10 && tile.phase == LineTile::Phase::Forward &&
           (tile.readyMask[local/64] & (1ull<<(local%64)));
}

bool
CfdCoeffPreprocessController::backwardTileBound(const LineTile &tile,uint64_t cell) const
{
    if (backwardTileReadable(tile,cell)) return true;
    // Binding a reload range does not publish any of its data. Its per-word
    // store responses still gate the separate y and matrix operand reads.
    return env.backwardIndependentReady && cell >= tile.start &&
        cell < tile.start+tile.count && env.reverseTileReload &&
        tile.phase == LineTile::Phase::Read &&
        (tile.u.empty() || env.tileOperandStream) &&
        tile.count*(tile.u.empty() ? N : N+N*N) <= tile.readyMask.size()*64;
}

bool
CfdCoeffPreprocessController::tileColumnReadable(
    const LineTile &tile, uint64_t cell, unsigned column) const
{
    if (!backwardTileBound(tile,cell)) return false;
    if (tile.phase != LineTile::Phase::Read || tile.u.empty()) return true;
    const unsigned base = tile.count*N+(cell-tile.start)*N*N+column;
    for (unsigned row = 0; row < N; ++row) {
        const unsigned word = base+row*N;
        if (!(tile.readyMask[word/64] & (1ull<<(word%64)))) return false;
    }
    return true;
}

void
CfdCoeffPreprocessController::markLocalTileReady(LineRequest &line,
    const std::shared_ptr<LineTile> &tile,uint64_t cell,uint64_t yJob,uint64_t uJob,
    uint64_t cacheJob)
{
    panic_if(tile->phase != LineTile::Phase::Forward || cell < tile->start ||
             cell >= tile->start+tile->count,"CFD local result outside forward tile");
    const unsigned local = cell-tile->start;
    const uint64_t bit = 1ull<<(local%64);
    panic_if(tile->readyMask[local/64] & bit,"CFD local result published twice");
    tile->readyMask[local/64] |= bit;
    tile->readyCount++;
    recordForwardOutputDigest(line, cell, tile->y[local]);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LOCAL_READY cycle=%llu token=%llu cell=%llu first=%llu storage=%u y_job=%llu u_job=%llu cache_job=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)line.token,
            (unsigned long long)cell,(unsigned long long)tile->start,tile->storageIndex,
            (unsigned long long)yJob,(unsigned long long)uJob,(unsigned long long)cacheJob);
}

void
CfdCoeffPreprocessController::processLocalTileResults()
{
    if (!env.onchipTiming) return;
    for (auto &[token,ptr] : requests) {
        auto &req = *ptr;
        if (req.stage == RequestStage::Complete || req.stage == RequestStage::Failed)
            continue;
        // Ubar is immutable when its RHS batch completes. Commit it without
        // waiting for y; local-ready still fences both operands before use.
        prepareCachedUbar(req);
        if (env.tailUbarOverlap)
            prepareLocalY(req);
        if (env.lineOptLevel >= 10 && req.solveDone && req.dqStarReadyLocal)
            prepareOnchipCommit(req);
    }
}

bool
CfdCoeffPreprocessController::startEarlyBackward(LineRequest &line)
{
    if (line.backwardStarted) return false;
    std::shared_ptr<LineTile> tail,active;
    for (const auto &t : lineTiles(line)) {
        if (!t) continue;
        bool yReady = t->readyCount == t->count;
        if (env.tailUbarOverlap && !yReady) {
            yReady = true;
            for (uint64_t c=t->start;c<t->start+t->count;++c)
                yReady &= forwardYReadable(line,*t,c);
        }
        if (line.cells-1 >= t->start && line.cells-1 < t->start+t->count &&
            yReady && forwardYReadable(line,*t,line.cells-1)) tail=t;
        if (line.cells>1 && forwardYReadable(line,*t,line.cells-2) && yReady) active=t;
    }
    if (!tail || (line.cells>1 && !active)) return false;
    if (!line.tailRead) line.tailRead = startOnchip(line.token,
        tileOnchipAddr(line,*tail)+(line.cells-1-tail->start)*VectorBytes,N,8,false,0);
    if (!line.tailRead || !line.tailRead->done) return false;
    if (!active) active=tail;
    if (line.tile != active) {
        if (line.prefetchTile == active) {
            std::swap(line.tile, line.prefetchTile);
        } else {
            auto slot = std::find(line.aheadTiles.begin(), line.aheadTiles.end(), active);
            panic_if(slot == line.aheadTiles.end(), "CFD backward tile has no storage owner");
            std::swap(line.tile, *slot);
        }
    }
    const uint64_t now = cycle();
    const uint64_t fbReady =
        now + (env.smeContexts && env.smeFeedbackReg ? 1u : 0u);
    line.earlyBackward=true;
    line.backwardStarted=true;
    line.backwardStartCycle=now;
    line.backwardFeedbackReadyCycle=fbReady;
    line.backwardPartReadyCycle={fbReady, fbReady};
    line.backwardCell=static_cast<int64_t>(line.cells)-2;
    line.backwardValue=tail->y[line.cells-1-tail->start];
    line.backwardNext=line.backwardValue;
    publishLineOutput(line,line.cells-1);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_EARLY_BACKWARD cycle=%llu token=%llu tail=%llu first=%llu tail_job=%llu\n",
            (unsigned long long)now,(unsigned long long)line.token,
            (unsigned long long)tail->start,(unsigned long long)active->start,
            (unsigned long long)line.tailRead->id);
    return true;
}

void
CfdCoeffPreprocessController::commitReadyForwardTiles(LineRequest &line)
{
    for (const auto &t : lineTiles(line)) {
        if (!t || t->phase != LineTile::Phase::Forward || t->retired != t->count) continue;
        panic_if(t->readyCount != t->count,"CFD forward drain before local data ready");
        t->phase=LineTile::Phase::Resident;
        line.tileForwardCount++;
        if (env.traceEnable)
            std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=forward_commit "
                "first=%llu cells=%llu storage=%u last_write=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)t->start, (unsigned long long)t->count, t->storageIndex,
                (unsigned long long)t->lastWriteCycle);
    }
}

void
CfdCoeffPreprocessController::processEarlyLineTiles(LineRequest &line)
{
    commitReadyForwardTiles(line);
    if (env.tilePrefetchBuffers) {
        processAheadLineTiles(line);
        return;
    }
    // A buffer is reusable only after its old forward DMA responses. Reads
    // may consume locally ready data while those writes are still pending.
    auto prefetch = [&] {
        if (!line.tile->start || !line.prefetchTile) return;
        auto t=line.prefetchTile;
        if (t->phase == LineTile::Phase::Idle ||
            (t->phase == LineTile::Phase::Resident && t->start >= line.tile->start))
            beginTileRead(line,t,line.tile->start-env.lineTileCells);
    };
    prefetch();
    stepTileRead(line,line.tile);
    stepTileRead(line,line.prefetchTile);
    if (line.backwardCell < 0) return;
    const uint64_t cell=line.backwardCell;
    if (cell < line.tile->start && !line.backwardOpPending) {
        if (line.prefetchTile && backwardTileBound(*line.prefetchTile,cell)) {
            std::swap(line.tile,line.prefetchTile);
            if (line.prefetchTile->phase != LineTile::Phase::Forward)
                line.prefetchTile->phase=LineTile::Phase::Idle;
            prefetch();
        } else if (!line.prefetchTile && line.tile->phase == LineTile::Phase::Resident) {
            beginTileRead(line,line.tile,(cell/env.lineTileCells)*env.lineTileCells);
        }
    }
    if (!backwardTileReadable(*line.tile,cell)) line.tileWaitCycles++;
}

void
CfdCoeffPreprocessController::processAheadLineTiles(LineRequest &line)
{
    // Storage identities remain fixed across pointer rotation. A consumed
    // forward range cannot be overwritten until its external writes retire;
    // a reload cannot be overwritten while any DMA/SPM landing is pending.
    for (const auto &tile : lineTiles(line)) {
        if (!tile || tile == line.tile) continue;
        if (tile->phase == LineTile::Phase::Resident && tile->start > line.tile->start)
            tile->phase = LineTile::Phase::Idle;
    }
    auto fill = [&] {
        uint64_t first = line.tile->start;
        for (const auto &tile : lineTiles(line))
            if (tile && tile->phase != LineTile::Phase::Idle)
                first = std::min(first,tile->start);
        for (const auto &tile : lineTiles(line)) {
            if (!first) break;
            if (!tile || tile == line.tile || tile->phase != LineTile::Phase::Idle) continue;
            first -= std::min<uint64_t>(first,env.lineTileCells);
            beginTileRead(line,tile,first);
        }
    };
    fill();
    // Closest missing block issues first. All accesses still pass through
    // the one-packet DMA budget and the shared bank/port arbiter.
    auto ordered = lineTiles(line);
    std::sort(ordered.begin(),ordered.end(),[](const auto &a,const auto &b) {
        return a && (!b || a->start > b->start);
    });
    for (const auto &tile : ordered) stepTileRead(line,tile);
    if (line.backwardCell < 0) return;
    const uint64_t cell = line.backwardCell;
    if (cell < line.tile->start && !line.backwardOpPending) {
        auto swapReady = [&](std::shared_ptr<LineTile> &candidate) {
            if (candidate && backwardTileBound(*candidate,cell)) {
                std::swap(line.tile,candidate);
                return true;
            }
            return false;
        };
        bool swapped = swapReady(line.prefetchTile);
        for (auto &candidate : line.aheadTiles)
            if (!swapped) swapped = swapReady(candidate);
        if (swapped) {
            for (const auto &tile : lineTiles(line))
                if (tile && tile != line.tile && tile->phase == LineTile::Phase::Resident &&
                    tile->start > line.tile->start) tile->phase = LineTile::Phase::Idle;
            fill();
        }
    }
    if (!backwardTileReadable(*line.tile,cell)) line.tileWaitCycles++;
}
