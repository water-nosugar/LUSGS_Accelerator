// Finite one-line storage. Each buffer has a fixed capacity. Forward child
// drains create DRAM backing; reverse operands come from timing DMA responses.
void
CfdCoeffPreprocessController::initializeLineTile(LineRequest &line)
{
    static_assert(sizeof(Request) + sizeof(CfdCoeffPreprocessRecord) <= 8192,
                  "update finite request/terminal-record reservation");
    const uint64_t capacity = env.inputSlots + env.luPendingDepth +
        env.solvePendingDepth + env.outputDepth;
    line.tileReserve = capacity * 8192 + 16384 + (env.rhsRowParallel ? 4096 : 0);
    const uint64_t cells = std::min<uint64_t>(env.lineTileCells, line.cells);
    line.tileAllocated = cells * (MatrixBytes + VectorBytes) * env.tileBuffers;
    panic_if(!cells || line.tileReserve + line.tileAllocated > env.lineSpmBytes,
             "CFD finite line SPM cannot hold reserved state and tile buffers");
    auto make = [cells] {
        auto tile = std::make_shared<LineTile>();
        tile->count = cells;
        tile->y.resize(cells);
        tile->u.resize(cells);
        return tile;
    };
    line.tile = make();
    if (env.tileBuffers == 2) {
        line.prefetchTile = make();
        line.prefetchTile->start = cells;
        line.prefetchTile->count = std::min<uint64_t>(cells, line.cells-cells);
        if (!line.prefetchTile->count)
            line.prefetchTile->phase = LineTile::Phase::Idle;
    }
}

void
CfdCoeffPreprocessController::prepareLineCache(LineRequest &line)
{
    line.coefficientCacheEnabled &= line.cells <= env.lineCacheCells &&
        line.tileReserve + line.tileAllocated + 3*MatrixBytes*line.cells <= env.lineSpmBytes;
    if (!line.coefficientCacheEnabled) {
        lineCoeffCaches.clear();
        return;
    }
    line.cacheAllocated = 3*MatrixBytes*line.cells;
    auto it = lineCoeffCaches.find(line.desc.lineId);
    const bool direct = line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
    if (!line.coefficientDirty && it != lineCoeffCaches.end() && it->second.valid &&
        it->second.cells == line.cells && it->second.version == line.coefficientVersion &&
        it->second.directBaseSolve == direct)
        return;
    // Release the previous entry before allocating the next: one finite cache.
    lineCoeffCaches.clear();
    auto &cache = lineCoeffCaches[line.desc.lineId];
    cache.cells = line.cells;
    cache.version = line.coefficientVersion;
    cache.directBaseSolve = direct;
    (direct ? cache.lu : cache.dInv).resize(line.cells);
    cache.lBar.resize(line.cells);
    cache.uBar.resize(line.cells);
}

uint64_t
CfdCoeffPreprocessController::tileIndex(const LineRequest &line, uint64_t cell) const
{
    panic_if(!line.tile || cell < line.tile->start ||
             cell >= line.tile->start + line.tile->count, "CFD cell outside resident tile");
    return cell - line.tile->start;
}

void
CfdCoeffPreprocessController::commitTileChild(LineRequest &line, const Request &req)
{
    const uint64_t cell = req.desc.cellId;
    auto tile = line.tile;
    if (cell >= tile->start + tile->count)
        tile = line.prefetchTile;
    panic_if(!tile || tile->phase != LineTile::Phase::Forward || cell < tile->start ||
             cell >= tile->start+tile->count, "CFD retired child outside forward buffers");
    tile->y[cell-tile->start] = req.dqStar;
    tile->u[cell-tile->start] = req.uBar;
    tile->retired++;
    line.committedChildren++;
    const uint64_t bytes = VectorBytes + (cell+1 < line.cells && !cachedUbar(line) ? MatrixBytes : 0);
    line.tileWriteIssued += bytes;
    line.tileWriteCompleted += bytes;
    line.tileLastResponse = std::max({line.tileLastResponse,
        req.record.dqStarPublishCycle, req.record.uBarDrainDone});
    if (line.coefficientCacheEnabled && !line.coefficientCacheHit) {
        auto &cache = lineCoeffCaches.at(line.desc.lineId);
        (req.directBaseSolve ? cache.lu : cache.dInv)[cell] = req.directBaseSolve ? req.lu : req.dInv;
        cache.lBar[cell] = req.lBar;
        cache.uBar[cell] = req.uBar;
        if (line.committedChildren == line.cells) {
            cache.valid = true;
            if (env.traceEnable)
                std::fprintf(stderr, "CFD_LINE_CACHE_STORE line=%llu version=%llu cells=%llu\n",
                    (unsigned long long)line.desc.lineId,
                    (unsigned long long)line.coefficientVersion, (unsigned long long)line.cells);
        }
    }
    if (req.status != CfdCoeffPreprocessStatus::Complete) {
        line.status = req.status;
        line.terminal = true;
        const uint64_t status = static_cast<uint64_t>(line.status);
        proxy().writeBlob(line.desc.dInvOutAddr, &status, sizeof(status));
        forwardHandoffs.erase(line.desc.lineId);
    }
}

void
CfdCoeffPreprocessController::retireTileChild(LineRequest &line, const Request &req)
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
        const auto &slot = line.warm[cell % std::min<uint64_t>(4, line.window)];
        if (!slot.valid || slot.cell != cell || slot.rhs->completed != VectorBytes)
            return false;
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
    const auto depth = std::min<uint64_t>(4, line.window);
    for (auto &slot : line.warm) {
        if (!slot.valid) continue;
        stepInterfaceTransfer(line.token, slot.rhs);
        if (!slot.y) continue;
        stepInterfaceTransfer(line.token, slot.y);
        if (slot.y->completed != VectorBytes) continue;
        panic_if(slot.tile->phase != LineTile::Phase::Forward,
                 "CFD warm buffer reused before write response");
        slot.tile->retired++;
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
    tile->issued[0] = tile->issued[1] = 0;
    tile->completed[0] = tile->completed[1] = 0;
    std::fill(tile->y.begin(), tile->y.end(), std::array<double, N>{});
    std::fill(tile->u.begin(), tile->u.end(), std::array<double, N*N>{});
    if (cachedUbar(line)) {
        const auto &cache = lineCoeffCaches.at(line.desc.lineId);
        for (uint64_t i = 0; i < tile->count; ++i)
            tile->u[i] = cache.uBar[first+i];
    }
    if (env.traceEnable)
        std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=reload_start "
            "first=%llu cells=%llu prefetch=%d\n", (unsigned long long)cycle(),
            (unsigned long long)line.token, (unsigned long long)first,
            (unsigned long long)tile->count, tile == line.prefetchTile);
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
    bool done = true;
    for (unsigned region = 0; region < 2; ++region) {
        done &= tile->completed[region] == sizes[region];
        if (tile->issued[region] == sizes[region]) continue;
        const Addr base = region == 0 ? line.desc.reserved[3] : line.desc.uBarOutAddr;
        const unsigned stride = region == 0 ? VectorBytes : MatrixBytes;
        const Addr addr = base + tile->start*stride + tile->issued[region];
        const unsigned bytes = spm->dmaChunkSize(tc, addr,
            std::min<uint64_t>(env.tileDmaWidth, sizes[region]-tile->issued[region]));
        uint8_t *data = region == 0 ? reinterpret_cast<uint8_t *>(tile->y.data()) :
            reinterpret_cast<uint8_t *>(tile->u.data());
        data += tile->issued[region];
        const auto packet = spm->issueDma(tc, false, addr, bytes, data,
            env.spmOutstanding, [this, token, tile, region, bytes](uint64_t id) {
                tile->completed[region] += bytes;
                auto it = lineRequests.find(token);
                if (it == lineRequests.end()) return;
                auto &line = *it->second;
                line.tileReadCompleted += bytes;
                line.tilePacketsCompleted++;
                line.tileLastResponse = cycle();
                if (env.traceEnable)
                    std::fprintf(stderr, "CFD_TILE_DMA cycle=%llu token=%llu event=response "
                        "first=%llu region=%u bytes=%u packet=%llu\n",
                        (unsigned long long)cycle(), (unsigned long long)token,
                        (unsigned long long)tile->start, region, bytes, (unsigned long long)id);
            });
        if (!packet.accepted) {
            panic_if(!packet.outstandingFull, "CFD tile DMA translation failed");
            line.tileStalls++;
            continue;
        }
        tile->issued[region] += bytes;
        line.tileReadIssued += bytes;
        line.tilePacketsIssued++;
        if (env.traceEnable)
            std::fprintf(stderr, "CFD_TILE_DMA cycle=%llu token=%llu event=issue "
                "first=%llu region=%u bytes=%u packet=%llu\n",
                (unsigned long long)cycle(), (unsigned long long)token,
                (unsigned long long)tile->start, region, bytes, (unsigned long long)packet.packetId);
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
        auto tile = line.tile;
        if (tile->phase == LineTile::Phase::Forward && tile->retired == tile->count) {
            line.tileForwardCount++;
            if (env.traceEnable)
                std::fprintf(stderr, "CFD_TILE_EVENT cycle=%llu token=%llu event=forward_commit "
                    "first=%llu cells=%llu\n", (unsigned long long)cycle(),
                    (unsigned long long)token, (unsigned long long)tile->start,
                    (unsigned long long)tile->count);
            const uint64_t next = tile->start + tile->count;
            if (next < line.cells) {
                if (line.prefetchTile) {
                    std::swap(line.tile, line.prefetchTile);
                    const uint64_t first = line.tile->start+line.tile->count;
                    tile->start = first;
                    tile->count = std::min<uint64_t>(env.lineTileCells, line.cells-first);
                } else {
                    tile->start = next;
                    tile->count = std::min<uint64_t>(env.lineTileCells, line.cells-next);
                }
                tile->retired = 0;
                tile->phase = tile->count ? LineTile::Phase::Forward : LineTile::Phase::Idle;
                std::fill(tile->y.begin(), tile->y.end(), std::array<double, N>{});
                std::fill(tile->u.begin(), tile->u.end(), std::array<double, N*N>{});
                continue;
            }
            tile->phase = LineTile::Phase::Resident;
            if (line.prefetchTile) line.prefetchTile->phase = LineTile::Phase::Idle;
            line.backwardStarted = true;
            line.backwardStartCycle = cycle();
            line.backwardCell = static_cast<int64_t>(line.cells)-2;
            line.backwardValue = tile->y[tileIndex(line, line.cells-1)];
            line.backwardNext = line.backwardValue;
            publishLineOutput(line, line.cells-1);
            if (line.prefetchTile && tile->start)
                beginTileRead(line, line.prefetchTile, tile->start-env.lineTileCells);
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
