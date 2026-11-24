// Prefetch paid SPM operands. Private staged panels can be finished ahead
// of the frontier; the recurrence still waits for adjacent-point feedback.
void
CfdCoeffPreprocessController::processBackwardOperands(LineRequest &line)
{
    if (env.lineOptLevel < 9 || !env.onchipTiming || !line.backwardStarted ||
        line.terminal || line.backwardCell < 0) return;
    for (unsigned ahead = 0; ahead < env.backwardOperandDepth; ++ahead) {
        const int64_t wanted = line.backwardCell-ahead;
        if (wanted < 0) break;
        const uint64_t cell = wanted;
        std::shared_ptr<LineTile> tile;
        for (const auto &candidate : lineTiles(line)) {
            if (candidate && backwardTileBound(*candidate,cell)) {
                tile = candidate;
                break;
            }
        }
        if (!tile) continue;
        auto &operand = line.backwardOperands[cell%env.backwardOperandDepth];
        if (operand.cell != cell) {
            if (operand.cell != UINT64_MAX) {
                panic_if(!operand.yReady || operand.readyColumns != 0x1f ||
                         operand.usedColumns != (env.backwardIndependentReady ? 0x3f : 0x1f),
                         "CFD overwriting live backward operands");
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=release\n",
                        (unsigned long long)cycle(),(unsigned long long)line.token,
                        (unsigned long long)operand.cell);
                line.backwardOperandReleased++;
            }
            operand = BackwardOperand{};
            operand.cell = cell;
            operand.tile = tile;
            line.backwardOperandAllocated++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=allocate tile=%llu slot=%llu\n",
                    (unsigned long long)cycle(),(unsigned long long)line.token,
                    (unsigned long long)cell,(unsigned long long)tile->start,
                    (unsigned long long)(cell%env.backwardOperandDepth));
        }
        const Addr base = tileOnchipAddr(line,*tile);
        const unsigned index = cell-tile->start;
        auto read = [this,&line,cell](std::shared_ptr<OnchipAccess> &job,
                Addr address,unsigned stride,int column,unsigned words = N) {
            if (job) return;
            job = startOnchip(line.token,address,words,stride,false,0);
            if (job && env.traceEnable)
                std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=read column=%d job=%llu\n",
                    (unsigned long long)cycle(),(unsigned long long)line.token,
                    (unsigned long long)cell,column,(unsigned long long)job->id);
        };
        if (backwardTileReadable(*tile,cell))
            read(operand.yLoad,base+index*VectorBytes,8,-1);
        if (operand.yLoad && operand.yLoad->done && !operand.yReady) {
            operand.y = tile->y[index];
            operand.yReady = true;
        }
        const Addr matrixBase = line.localUbar ? smeUpperAddr(line, cell) :
            cachedUbar(line) ?
            cacheEntryAddr(line, cell) + smeCacheTagBytes() + 2*MatrixBytes :
            base+tile->y.size()*VectorBytes+index*MatrixBytes;
        const auto &matrix = cachedUbar(line) ?
            lineCoeffCaches.at(line.desc.lineId).uBar[cell] : tile->u[index];
        if (line.deferUpper) {
            auto &load = operand.columns[0];
            const unsigned words = smeUpperFactorWords();
            read(load, matrixBase, 8, -2, N*N + words);
            if (load && load->done && !operand.smePackedReady) {
                for (unsigned row = 0; row < N; ++row)
                    std::copy_n(matrix.begin() + row*N, N, operand.values[row].begin());
                std::copy_n(line.smeUpperFactors.begin() + cell*words, words,
                            operand.smeFactors.value.begin());
                operand.smePackedReady = true;
            }
            continue;
        }
        for (unsigned col = 0; col < N; ++col) {
            if (!tileColumnReadable(*tile,cell,col)) continue;
            read(operand.columns[col],matrixBase+col*8,N*8,col);
            if (operand.columns[col] && operand.columns[col]->done &&
                !(operand.readyColumns & (1u<<col))) {
                for (unsigned row = 0; row < N; ++row)
                    operand.values[col][row] = matrix[row*N+col];
                operand.readyColumns |= 1u<<col;
            }
        }
    }
}

bool
CfdCoeffPreprocessController::backwardOperandReady(LineRequest &line,unsigned column)
{
    auto &operand = line.backwardOperands[line.backwardCell%env.backwardOperandDepth];
    if (operand.cell != static_cast<uint64_t>(line.backwardCell) ||
        (!env.backwardIndependentReady && !operand.yReady) ||
        !(operand.readyColumns & (1u<<column))) return false;
    if (operand.yReady) line.backwardYOperand = operand.y;
    line.mvm[1].columnOperands[column] = operand.values[column];
    markBackwardColumnUse(line, operand, column);
    return true;
}

void
CfdCoeffPreprocessController::markBackwardColumnUse(
    LineRequest &line, BackwardOperand &operand, unsigned column)
{
    if (!(operand.usedColumns & (1u<<column))) {
        operand.usedColumns |= 1u<<column;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=use column=%u job=%llu y_job=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)operand.cell,column,
                (unsigned long long)operand.columns[line.deferUpper ? 0 : column]->id,
                (unsigned long long)(operand.yReady ? operand.yLoad->id : 0));
    }
}

bool
CfdCoeffPreprocessController::backwardYReady(LineRequest &line)
{
    if (!env.backwardIndependentReady) return true;
    auto &operand = line.backwardOperands[line.backwardCell%env.backwardOperandDepth];
    if (operand.cell != static_cast<uint64_t>(line.backwardCell) || !operand.yReady)
        return false;
    line.backwardYOperand = operand.y;
    markBackwardYUse(line, operand);
    return true;
}

void
CfdCoeffPreprocessController::markBackwardYUse(LineRequest &line, BackwardOperand &operand)
{
    // Bit 5 was unused in the existing five-column consumption mask. Keep
    // the same bounded slots and account for y consumption before recycling.
    if (!(operand.usedColumns & 0x20)) {
        operand.usedColumns |= 0x20;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=y_use job=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)operand.cell,(unsigned long long)operand.yLoad->id);
    }
}

void
CfdCoeffPreprocessController::releaseBackwardOperands(LineRequest &line)
{
    if (env.lineOptLevel < 9) return;
    for (auto &operand : line.backwardOperands) {
        if (operand.cell == UINT64_MAX) continue;
        panic_if(!operand.yReady || operand.readyColumns != 0x1f ||
                 operand.usedColumns != (env.backwardIndependentReady ? 0x3f : 0x1f),
                 "CFD incomplete backward operand at completion");
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=release\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)operand.cell);
        line.backwardOperandReleased++;
        operand = BackwardOperand{};
    }
    panic_if(line.backwardOperandAllocated != line.backwardOperandReleased,
             "CFD backward operand credit leak");
}
