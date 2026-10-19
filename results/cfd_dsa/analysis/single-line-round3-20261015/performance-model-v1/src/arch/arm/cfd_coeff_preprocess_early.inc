// Resident data becomes readable after its modeled SPM stores. Its external
// writeback owns independent snapshots; final completion still fences them.
bool
CfdCoeffPreprocessController::backwardTileReadable(const LineTile &tile,uint64_t cell) const
{
    if (cell < tile.start || cell >= tile.start+tile.count) return false;
    if (tile.phase == LineTile::Phase::Resident) return true;
    const unsigned local = cell-tile.start;
    return env.lineOptLevel >= 10 && tile.phase == LineTile::Phase::Forward &&
           (tile.readyMask[local/64] & (1ull<<(local%64)));
}

void
CfdCoeffPreprocessController::markLocalTileReady(LineRequest &line,
    const std::shared_ptr<LineTile> &tile,uint64_t cell,uint64_t yJob,uint64_t uJob)
{
    panic_if(tile->phase != LineTile::Phase::Forward || cell < tile->start ||
             cell >= tile->start+tile->count,"CFD local result outside forward tile");
    const unsigned local = cell-tile->start;
    const uint64_t bit = 1ull<<(local%64);
    panic_if(tile->readyMask[local/64] & bit,"CFD local result published twice");
    tile->readyMask[local/64] |= bit;
    tile->readyCount++;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LOCAL_READY cycle=%llu token=%llu cell=%llu first=%llu storage=%u y_job=%llu u_job=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)line.token,
            (unsigned long long)cell,(unsigned long long)tile->start,tile->storageIndex,
            (unsigned long long)yJob,(unsigned long long)uJob);
}

void
CfdCoeffPreprocessController::processLocalTileResults()
{
    if (env.lineOptLevel < 10) return;
    for (auto &[token,ptr] : requests) {
        auto &req = *ptr;
        if (req.stage == RequestStage::Complete || req.stage == RequestStage::Failed ||
            !req.solveDone || !req.dqStarReadyLocal) continue;
        prepareOnchipCommit(req);
    }
}

bool
CfdCoeffPreprocessController::startEarlyBackward(LineRequest &line)
{
    if (line.backwardStarted) return false;
    std::shared_ptr<LineTile> tail,active;
    for (const auto &t : {line.tile,line.prefetchTile}) {
        if (!t) continue;
        if (line.cells-1 >= t->start && line.cells-1 < t->start+t->count &&
            t->readyCount == t->count && backwardTileReadable(*t,line.cells-1)) tail=t;
        if (line.cells>1 && backwardTileReadable(*t,line.cells-2) &&
            t->readyCount == t->count) active=t;
    }
    if (!tail || (line.cells>1 && !active)) return false;
    if (!line.tailRead) line.tailRead = startOnchip(line.token,
        tileOnchipAddr(line,*tail)+(line.cells-1-tail->start)*VectorBytes,N,8,false,0);
    if (!line.tailRead || !line.tailRead->done) return false;
    if (!active) active=tail;
    auto other = line.tile == active ? line.prefetchTile : line.tile;
    line.tile=active;
    line.prefetchTile=other;
    line.earlyBackward=true;
    line.backwardStarted=true;
    line.backwardStartCycle=cycle();
    line.backwardCell=static_cast<int64_t>(line.cells)-2;
    line.backwardValue=tail->y[line.cells-1-tail->start];
    line.backwardNext=line.backwardValue;
    publishLineOutput(line,line.cells-1);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_EARLY_BACKWARD cycle=%llu token=%llu tail=%llu first=%llu tail_job=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)line.token,
            (unsigned long long)tail->start,(unsigned long long)active->start,
            (unsigned long long)line.tailRead->id);
    return true;
}

void
CfdCoeffPreprocessController::processEarlyLineTiles(LineRequest &line)
{
    for (const auto &t : {line.tile,line.prefetchTile}) {
        if (!t || t->phase != LineTile::Phase::Forward || t->retired != t->count) continue;
        panic_if(t->readyCount != t->count,"CFD forward drain before local data ready");
        t->phase=LineTile::Phase::Resident;
        line.tileForwardCount++;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_TILE_EVENT cycle=%llu token=%llu event=forward_commit first=%llu cells=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)t->start,(unsigned long long)t->count);
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
        if (line.prefetchTile && backwardTileReadable(*line.prefetchTile,cell)) {
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
