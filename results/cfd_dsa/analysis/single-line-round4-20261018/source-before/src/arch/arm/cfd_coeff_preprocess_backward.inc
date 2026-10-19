// Operand prefetch does not evaluate another point's recurrence. Only SPM
// reads run ahead; products wait for the actual adjacent-point feedback.
void
CfdCoeffPreprocessController::processBackwardOperands(LineRequest &line)
{
    if (env.lineOptLevel < 9 || !env.onchipTiming || !line.backwardStarted ||
        line.terminal || line.backwardCell < 0) return;
    for (int ahead = 0; ahead < 2; ++ahead) {
        const int64_t wanted = line.backwardCell-ahead;
        if (wanted < 0) break;
        const uint64_t cell = wanted;
        std::shared_ptr<LineTile> tile;
        for (const auto &candidate : {line.tile,line.prefetchTile}) {
            if (candidate && backwardTileReadable(*candidate,cell)) {
                tile = candidate;
                break;
            }
        }
        if (!tile) continue;
        auto &operand = line.backwardOperands[cell%2];
        if (operand.cell != cell) {
            if (operand.cell != UINT64_MAX) {
                panic_if(!operand.yReady || operand.readyColumns != 0x1f ||
                         operand.usedColumns != 0x1f,
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
                    (unsigned long long)(cell%2));
        }
        const Addr base = tileOnchipAddr(line,*tile);
        const unsigned index = cell-tile->start;
        auto read = [this,&line,cell](std::shared_ptr<OnchipAccess> &job,
                                      Addr address,unsigned stride,int column) {
            if (job) return;
            job = startOnchip(line.token,address,N,stride,false,0);
            if (job && env.traceEnable)
                std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=read column=%d job=%llu\n",
                    (unsigned long long)cycle(),(unsigned long long)line.token,
                    (unsigned long long)cell,column,(unsigned long long)job->id);
        };
        read(operand.yLoad,base+index*VectorBytes,8,-1);
        const Addr matrixBase = cachedUbar(line) ?
            line.tileReserve+line.tileAllocated+cell*3*MatrixBytes+2*MatrixBytes :
            base+tile->y.size()*VectorBytes+index*MatrixBytes;
        const auto &matrix = cachedUbar(line) ?
            lineCoeffCaches.at(line.desc.lineId).uBar[cell] : tile->u[index];
        for (unsigned col = 0; col < N; ++col) {
            read(operand.columns[col],matrixBase+col*8,N*8,col);
            if (operand.columns[col] && operand.columns[col]->done &&
                !(operand.readyColumns & (1u<<col))) {
                for (unsigned row = 0; row < N; ++row)
                    operand.values[col][row] = matrix[row*N+col];
                operand.readyColumns |= 1u<<col;
            }
        }
        if (operand.yLoad && operand.yLoad->done && !operand.yReady) {
            operand.y = tile->y[index];
            operand.yReady = true;
        }
    }
}

bool
CfdCoeffPreprocessController::backwardOperandReady(LineRequest &line,unsigned column)
{
    auto &operand = line.backwardOperands[line.backwardCell%2];
    if (operand.cell != static_cast<uint64_t>(line.backwardCell) || !operand.yReady ||
        !(operand.readyColumns & (1u<<column))) return false;
    line.backwardYOperand = operand.y;
    line.mvm[1].columnOperands[column] = operand.values[column];
    if (!(operand.usedColumns & (1u<<column))) {
        operand.usedColumns |= 1u<<column;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_BACK_OPERAND cycle=%llu token=%llu cell=%llu event=use column=%u job=%llu y_job=%llu\n",
                (unsigned long long)cycle(),(unsigned long long)line.token,
                (unsigned long long)operand.cell,column,
                (unsigned long long)operand.columns[column]->id,
                (unsigned long long)operand.yLoad->id);
    }
    return true;
}

void
CfdCoeffPreprocessController::releaseBackwardOperands(LineRequest &line)
{
    if (env.lineOptLevel < 9) return;
    for (auto &operand : line.backwardOperands) {
        if (operand.cell == UINT64_MAX) continue;
        panic_if(!operand.yReady || operand.readyColumns != 0x1f || operand.usedColumns != 0x1f,
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
