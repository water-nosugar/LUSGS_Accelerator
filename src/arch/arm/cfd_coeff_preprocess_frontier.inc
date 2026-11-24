// Ordered column products share the existing global credit limit and physical
// 5/5/1 cluster. Direct correction may reuse the already reserved line tree
// context. Cold operands remain gated by each actual Lbar RHS return; no
// operand copy, new product bank, arithmetic unit or RF port is introduced.
void
CfdCoeffPreprocessController::processWarmCorrection(Request &req, uint64_t now)
{
    if (req.correctionComplete) return;
    auto &line = *lineRequests.at(childLineTokens.at(req.token));
    auto &state = line.mvm[0];
    if (state.memoryCell != req.desc.cellId) {
        panic_if(state.issued,"CFD overwriting live direct forward context");
        state.memoryCell = req.desc.cellId;
        line.cachedForwardCell = req.desc.cellId;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_FORWARD_TREE cycle=%llu token=%llu request=%llu generation=%llu cell=%llu epoch=%llu event=begin mode=%s\n",
                (unsigned long long)now,(unsigned long long)line.token,
                (unsigned long long)req.record.requestId,(unsigned long long)req.desc.generation,
                (unsigned long long)req.desc.cellId,(unsigned long long)state.epoch,
                req.coefficientCacheReuse ? "cached" : "cold");
    }
    panic_if(line.cachedForwardCell != req.desc.cellId,
             "CFD overlapping direct forward tree contexts");
    const uint64_t token = req.token, generation = req.desc.generation;
    advanceLineMvm(line,0,req.lBar,req.previousDq,req.lbarColumnReadyMask,now,
        [this,token,generation](LineRequest &line,const auto &sum) {
            auto *r = find(token);
            if (!r || r->stage == RequestStage::Failed || r->desc.generation != generation) return;
            r->lbarCorrectionAccumulator = sum;
            r->correctionComplete = true;
            r->nextLbarColumnToConsume = N;
            r->lbarColumnConsumedMask = 0x1f;
            r->record.lbarConsumedColumnMask = 0x1f;
            r->record.lbarColumnsCompleted = N;
            r->record.lbarColumnsConsumed = N;
            r->record.columnFmaCompletions = N;
            r->record.correctionReadyCycle = cycle();
            r->record.correctionAccumulatorsCompleted = 1;
            trace(*r,"column-tree","CORRECTION_ACCUMULATOR_COMPLETE");
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_FORWARD_TREE cycle=%llu token=%llu request=%llu generation=%llu cell=%llu epoch=%llu event=complete mode=%s\n",
                    (unsigned long long)cycle(),(unsigned long long)line.token,
                    (unsigned long long)r->record.requestId,(unsigned long long)generation,
                    (unsigned long long)r->desc.cellId,(unsigned long long)(line.mvm[0].epoch-1),
                    r->coefficientCacheReuse ? "cached" : "cold");
        },req.token);
    const unsigned count = state.issued;
    req.lbarColumnIssuedMask = (1u << count)-1;
    req.record.lbarColumnsQueued = count;
    req.record.lbarColumnsIssued = count;
    req.record.columnFmaIssueCycles = count;
    if (count && !req.firstLbarConsumerIssueCycle) req.firstLbarConsumerIssueCycle = now;
}

void
CfdCoeffPreprocessController::releaseColdProduct(
    const std::shared_ptr<ColdColumnProduct> &product)
{
    if (!product->live) return;
    panic_if(!sharedVectorsLive, "CFD cold product credit underflow");
    product->live = false;
    sharedVectorsLive--;
    sharedVectorsCompleted++;
}

void
CfdCoeffPreprocessController::discardColdProducts(Request &req)
{
    for (auto &batch : req.coldProducts) {
        for (auto &product : batch) {
            if (!product) continue;
            // In-flight multiply/add callbacks own their bytes and release
            // credits on completion even after the request is cancelled.
            if (product->ready && !product->addIssued)
                releaseColdProduct(product);
            product.reset();
        }
    }
}

void
CfdCoeffPreprocessController::processColdColumns(uint64_t now)
{
    for (Request *req : admissionOrder()) {
        DiagnosticOwner owner(diagnosticOwner, req->token);
        if (req->stage == RequestStage::Failed || req->stage == RequestStage::Complete) {
            discardColdProducts(*req);
            continue;
        }
        if (((env.warmForwardTree && req->coefficientCacheReuse) ||
             (env.coldForwardTree && !req->coefficientCacheReuse)) && req->directBaseSolve &&
            req->lbarConsumerEnabled && req->desc.cellId && req->previousDqReady &&
            req->correctionInitialized && cachedRegistersReady(*req)) {
            processWarmCorrection(*req,now);
            continue;
        }
        // The current forward correction is served before independent base
        // products. Each arithmetic issue still uses the global shared bank.
        for (unsigned batch : {1u, 0u}) {
            const bool inverse = batch == 0;
            if (inverse ? (!req->baseConsumerEnabled || req->directBaseSolve ||
                           !req->rhsVectorReady || !req->baseAccumulatorInitialized) :
                          (!req->lbarConsumerEnabled || !req->desc.cellId ||
                           !req->previousDqReady || !req->correctionInitialized))
                continue;
            if (!inverse && !cachedRegistersReady(*req)) continue;
            const unsigned next = inverse ? req->nextDInvColumnToConsume : req->nextLbarColumnToConsume;
            if (next >= N) continue;
            auto &products = req->coldProducts[batch];
            auto product = products[next];
            const auto token = req->token, generation = req->desc.generation;
            const auto kind = inverse ? ColumnFmaTaskKind::DInvBase : ColumnFmaTaskKind::LbarCorrection;
            if (product && product->ready && !product->addIssued) {
                CfdLocalAccess access;
                if (env.vectorLocal && !inverse) {
                    access.a=next; access.wordsA=N; access.globalReads=N;
                    access.owner=token; access.epoch=generation;
                }
                if (issueLanes(sharedAdd, OpKind::SharedAdd, now, N, OpKind::ColumnFma,
                    [this, token, generation, kind, next, batch, product] {
                        releaseColdProduct(product);
                        if (auto *r = find(token))
                            r->coldProducts[batch][next].reset();
                        completeColumnFma(token, generation, kind, next, product->value, cycle());
                    },false,-1,0,false,0,env.vectorLocal && !inverse ? &access : nullptr))
                    product->addIssued = true;
            }
            for (unsigned attempt=0;attempt<env.vectorIssueWidth;++attempt) {
            if (env.vectorIssueWidth>1 && vectorMulGrants>=env.vectorIssueWidth) break;
            const auto mask = inverse ? req->dInvColumnReadyMask : req->lbarColumnReadyMask;
            unsigned col = next;
            while (col < N && products[col]) col++;
            if (col == N || !(mask & (1u << col))) continue;
            if (sharedVectorsLive >= env.clusterProductDepth) {
                sharedProductStalls++;
                continue;
            }
            const auto &matrix = inverse ? req->dInv : req->lBar;
            const auto scalar = inverse ? req->rhsVector[col] : req->previousDq[col];
            std::array<double, N> column;
            for (unsigned row = 0; row < N; ++row) column[row] = matrix[row*N+col];
            auto pending = std::make_shared<ColdColumnProduct>();
            CfdLocalAccess access;
            if (env.vectorLocal && !inverse) {
                access.destination=col;access.globalReads=env.rfBroadcast ? N+1 : 2*N;
                access.owner=token;access.epoch=generation;
            }
            if (!issueLanes(sharedMul, OpKind::SharedMul, now, N, OpKind::ColumnFma,
                [this, token, generation, pending, column, scalar] {
                    auto *r = find(token);
                    if (!r || r->desc.generation != generation || r->stage == RequestStage::Failed) {
                        releaseColdProduct(pending);
                        return;
                    }
                    for (unsigned row = 0; row < N; ++row) {
                        volatile double rounded = column[row]*scalar;
                        pending->value[row] = rounded;
                    }
                    pending->ready = true;
                },true,-1,0,false,0,env.vectorLocal && !inverse ? &access : nullptr)) break;
            if (env.vectorIssueWidth>1) ++vectorMulGrants;
            products[col] = pending;
            sharedVectorsLive++;
            sharedVectorsIssued++;
            sharedVectorsMax = std::max(sharedVectorsMax, sharedVectorsLive);
            const uint8_t bit = 1u << col;
            if (inverse) {
                req->dInvColumnIssuedMask |= bit;
                req->record.dInvColumnsQueued++;
                req->record.dInvColumnsIssued++;
            } else {
                req->lbarColumnIssuedMask |= bit;
                req->record.lbarColumnsQueued++;
                req->record.lbarColumnsIssued++;
                if (!req->firstLbarConsumerIssueCycle) req->firstLbarConsumerIssueCycle = now;
            }
            req->record.columnFmaIssueCycles++;
            trace(*req, "column-fma", "COLUMN_FMA_QUEUE", batch, col);
            trace(*req, "column-fma", "COLUMN_FMA_ISSUE", batch, col);
            }
        }
    }
}
