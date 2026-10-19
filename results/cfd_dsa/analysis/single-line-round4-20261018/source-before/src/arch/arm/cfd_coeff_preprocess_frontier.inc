// Cold column products share the existing global credit limit and physical
// 5/5/1 cluster. Adds retire strictly in column order; rounding is unchanged.
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
        if (req->stage == RequestStage::Failed || req->stage == RequestStage::Complete) {
            discardColdProducts(*req);
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
                if (issueLanes(sharedAdd, OpKind::SharedAdd, now, N, OpKind::ColumnFma,
                    [this, token, generation, kind, next, batch, product] {
                        releaseColdProduct(product);
                        if (auto *r = find(token))
                            r->coldProducts[batch][next].reset();
                        completeColumnFma(token, generation, kind, next, product->value, cycle());
                    }))
                    product->addIssued = true;
            }
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
                })) continue;
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
