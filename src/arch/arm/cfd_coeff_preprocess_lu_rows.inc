// A visit permits at most one ordered SUB and one independent next-k MUL.
// Product-slot reuse follows input capture; FU/result credits still return
// only through completeOps, including local storage and publication latency.
void
CfdCoeffPreprocessController::processLuProducts(Request &req, uint64_t now)
{
    const int c=req.luCol;
    const bool upper=req.luPhase==LuPhase::UpperMul;
    const unsigned width=N-c-(upper ? 1 : 0);
    const uint64_t token=req.token,generation=req.desc.generation;
    auto &window=req.luProductWindow;
    for (bool mul : {false,true}) {
        if (mul ? !window.canProduce() : !window.canSubtract()) continue;
        const unsigned k=mul ? window.nextMul : window.nextSub;
        const bool broadcast=mul && env.preprocessBroadcast;
        std::array<double,N> left{},right{};
        for (unsigned target=c+(upper ? 1 : 0);target<N;++target) {
            left[target]=mul ? (upper ? req.lu[c*N+k] : req.lu[target*N+k]) : req.luTemps[target];
            right[target]=mul ? (upper ? req.lu[k*N+target] : req.lu[k*N+c]) : window.slots[k%2].value[target];
        }
        CfdLocalAccess access;
        access.owner=token;access.epoch=req.desc.generation;
        const int bank=luAheadBank(token,k%2);
        if (mul) access.destination=bank;
        else { access.a=bank;access.wordsA=width; }
        access.globalReads=mul ? (broadcast && width>1 ? width+1 : 2*width) : width;
        const uint64_t operation=nextRfTag;
        if (!issueLanes(mul ? sharedMul : sharedAdd,
            mul ? OpKind::SharedMul : OpKind::SharedAdd,now,width,
            mul ? OpKind::LuMul : OpKind::LuSub,
            [this,token,generation,c,k,upper,mul,left,right,operation] {
                auto *r=find(token);
                if (!r || r->stage==RequestStage::Failed || r->desc.generation!=generation) return;
                panic_if(r->luCol!=c,"CFD stale LU product column");
                std::array<double,N> values{};
                for (unsigned target=c+(upper ? 1 : 0);target<N;++target) {
                    volatile double value=mul ? left[target]*right[target] : left[target]-right[target];
                    values[target]=value;
                }
                if (mul) r->luProductWindow.publish(k,values);
                else {
                    for (unsigned target=c+(upper ? 1 : 0);target<N;++target)
                        r->luTemps[target]=values[target];
                    if (r->luProductWindow.complete(k)) {
                        noteLuReturn(*r,operation);
                        r->luPhase=upper ? LuPhase::DivideUpper : LuPhase::StoreLower;
                    }
                }
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_LU_PRODUCTS cycle=%llu event=%s token=%llu generation=%llu column=%d upper=%u k=%u slot=%u\n",
                        (unsigned long long)cycle(),mul ? "mul-ready" : "sub-ready",
                        (unsigned long long)token,(unsigned long long)r->desc.generation,c,upper,k,k%2);
            },broadcast,-1,0,false,0,&access)) {
            req.record.stallLuMulSub++;continue;
        }
        if (mul) window.produce(k);else window.capture(k);
        if (broadcast && width>1) {
            const double factor=upper ? req.lu[c*N+k] : req.lu[k*N+c];
            tracePreprocessBroadcast(req,"lu",upper ? "upper" : "lower",k,c,-1,width,factor);
        }
        if (mul) req.record.luMulIssued+=width;else req.record.luSubIssued+=width;
        trace(req,"lu",mul ? "LU_ROW_MUL_ISSUE" : "LU_ROW_SUB_ISSUE",-1,-1,k,c);
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_LU_PRODUCTS cycle=%llu event=%s token=%llu generation=%llu column=%d upper=%u k=%u slot=%u id=%llu bank=%d\n",
                (unsigned long long)now,mul ? "mul-issue" : "sub-issue",
                (unsigned long long)token,(unsigned long long)req.desc.generation,c,upper,k,k%2,
                (unsigned long long)(nextRfTag-1),bank);
    }
}

void
CfdCoeffPreprocessController::traceLuUpper(const Request &req,const char *event,
                                         int k,uint64_t id,int bank)
{
    if (env.traceEnable && req.desc.cellId<env.traceCells)
        std::fprintf(stderr,"CFD_LU_UPPER cycle=%llu event=%s token=%llu generation=%llu request=%llu cell=%llu column=%d k=%d slot=%d id=%llu bank=%d\n",
            (unsigned long long)cycle(),event,(unsigned long long)req.token,
            (unsigned long long)req.desc.generation,
            (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
            req.luCol,k,k<0 ? -1 : k%2,(unsigned long long)id,bank);
}

// Upper numerators depend only on earlier, already published columns. Their
// independent two-product window never changes ordered SUBs or RF/FU costs.
void
CfdCoeffPreprocessController::processLuUpper(Request &req,uint64_t now)
{
    auto &upper=req.luUpper;
    const unsigned c=req.luCol,width=N-c-1;
    if (c>=N-1 || upper.column!=int(c) || now<upper.eligible) return;
    if (upper.advanceReady(now)) traceLuUpper(req,"ready");
    if (upper.ready) return;
    auto &window=upper.products;
    for (bool mul : {false,true}) {
        if (mul ? !window.canProduce() : !window.canSubtract()) continue;
        const unsigned k=mul ? window.nextMul : window.nextSub;
        panic_if(!req.backwardStepReady[k],"CFD upper numerator read an unpublished LU column");
        const uint64_t token=req.token,generation=req.desc.generation;
        const int bank=luUpperBank(token,k%2);
        const bool broadcast=mul && env.preprocessBroadcast;
        std::array<double,N> left{},right{};
        for (unsigned j=c+1;j<N;++j) {
            left[j]=mul ? req.lu[c*N+k] : upper.accumulator[j];
            right[j]=mul ? req.lu[k*N+j] : window.slots[k%2].value[j];
        }
        CfdLocalAccess access;
        access.owner=token;access.epoch=generation;
        if (mul) access.destination=bank;
        else { access.a=bank;access.wordsA=width; }
        access.globalReads=mul ? (broadcast && width>1 ? width+1 : 2*width) : width;
        const uint64_t operation=nextRfTag;
        if (!issueLanes(mul ? sharedMul : sharedAdd,
            mul ? OpKind::SharedMul : OpKind::SharedAdd,now,width,
            mul ? OpKind::LuMul : OpKind::LuSub,
            [this,token,generation,c,k,mul,left,right,operation,bank] {
                auto *r=find(token);
                if (!r || r->stage==RequestStage::Failed || r->desc.generation!=generation) return;
                panic_if(r->luCol!=int(c) || r->luUpper.column!=int(c),"CFD stale upper numerator return");
                std::array<double,N> values=r->luUpper.accumulator;
                for (unsigned j=c+1;j<N;++j) {
                    volatile double value=mul ? left[j]*right[j] : left[j]-right[j];
                    values[j]=value;
                }
                if (mul) r->luUpper.products.publish(k,values);
                else r->luUpper.finishSubtract(k,values,cycle());
                traceLuUpper(*r,mul ? "mul-ready" : "sub-ready",k,operation,bank);
                if (!mul && r->luUpper.ready) traceLuUpper(*r,"ready",-1,operation);
            },broadcast,-1,0,false,0,&access)) {
            req.record.stallLuMulSub++;continue;
        }
        if (mul) window.produce(k);else window.capture(k);
        if (broadcast && width>1)
            tracePreprocessBroadcast(req,"lu","upper",k,c,-1,width,req.lu[c*N+k]);
        if (mul) req.record.luMulIssued+=width;else req.record.luSubIssued+=width;
        trace(req,"lu",mul ? "LU_ROW_MUL_ISSUE" : "LU_ROW_SUB_ISSUE",-1,-1,k,c);
        traceLuUpper(req,mul ? "mul-issue" : "sub-issue",k,operation,bank);
    }
}

// Crout targets within a column are independent. Each target retains its
// original k order; upper quotients share the same physical pipelined divider.
void
CfdCoeffPreprocessController::processLuRows(Request &req, uint64_t now)
{
    if (req.luVisitedCycle == now) return;
    req.luVisitedCycle = now;
    if (env.luRank1) { processLuRank1(req,now);return; }
    // Registered zero-term input selection returns at the start of its
    // eligible tick, before the primary path may join it. Arithmetic upper
    // issues still get only the resources left after the primary path.
    if (env.luUpperOverlap && req.luUpper.column==req.luCol &&
        req.luUpper.advanceReady(now)) traceLuUpper(req,"ready");
    processLuRowsPrimary(req,now);
    // Lower/normalization gets first access. The upper issue path may then
    // use remaining paid resources even while lower has an in-flight result.
    if (env.luUpperOverlap && req.stage!=RequestStage::Failed && !req.luDone)
        processLuUpper(req,now);
}

void
CfdCoeffPreprocessController::processLuRowsPrimary(Request &req,uint64_t now)
{
    if (req.luWaiting) { observe("lu_inflight_dependency"); req.record.stallDependency++; return; }
    const uint64_t token = req.token;
    unsigned folds=0;
    for (;;) {
    const int c = req.luCol;
    const LuPhase phase=req.luPhase;
    switch (req.luPhase) {
      case LuPhase::StartLower:
        for (int row = c; row < N; ++row) req.luTemps[row] = req.d[row*N+c];
        if (env.luUpperOverlap && c+1<N) {
            std::array<double,N> values{};
            for (unsigned j=c+1;j<N;++j) values[j]=req.d[c*N+j];
            req.luUpper.begin(c,values,now);
            traceLuUpper(req,"begin",-1,0,luUpperBank(token,0));
        }
        req.luK = 0;
        if (env.luProductAhead) req.luProductWindow.begin(c);
        req.luPhase = c ? LuPhase::LowerMul : LuPhase::StoreLower;
        break;
      case LuPhase::LowerMul:
      case LuPhase::UpperMul:
      case LuPhase::LowerSub:
      case LuPhase::UpperSub:
      {
        if (env.luUpperOverlap && req.luPhase==LuPhase::UpperMul) {
            if (!req.luUpper.ready) { observe("lu_upper_numerator_dependency");break; }
            req.luTemps=req.luUpper.accumulator;
            traceLuUpper(req,"join");
            req.luPhase=LuPhase::DivideUpper;
            break;
        }
        if (env.luProductAhead) { processLuProducts(req,now);break; }
        const bool upper = req.luPhase == LuPhase::UpperMul || req.luPhase == LuPhase::UpperSub;
        const bool mul = req.luPhase == LuPhase::LowerMul || req.luPhase == LuPhase::UpperMul;
        const unsigned width = N-c-(upper ? 1 : 0);
        const int k = req.luK;
        const uint64_t operation=nextRfTag;
        const bool broadcast = env.preprocessBroadcast && mul;
        const double factor = broadcast ? (upper ? req.lu[c*N+k] : req.lu[k*N+c]) : 0;
        std::array<double,N> operands{};
        if (broadcast)
            for (int target=c+(upper ? 1 : 0);target<N;++target)
                operands[target]=upper ? req.lu[k*N+target] : req.lu[target*N+k];
        CfdLocalAccess access;
        if (env.localProducts) {
            access.owner=token; access.epoch=req.desc.generation;
            const int bank=luProductBank(token);
            if (mul) access.destination=bank;
            else { access.a=bank; access.wordsA=width; }
            access.globalReads=mul ? (broadcast && width>1 ? width+1 : 2*width) : width;
        }
        std::array<double,N> leftInputs{},rightInputs{};
        for (int target=c+(upper ? 1 : 0);target<N;++target) {
            leftInputs[target]=mul ? (upper ? req.lu[c*N+k] : req.lu[target*N+k]) : req.luTemps[target];
            rightInputs[target]=mul ? (upper ? req.lu[k*N+target] : req.lu[k*N+c]) : req.luProducts[target];
        }
        req.luWaiting = true;
        const bool accepted = issueLanes(mul ? sharedMul : sharedAdd,
            mul ? OpKind::SharedMul : OpKind::SharedAdd, now, width,
            mul ? OpKind::LuMul : OpKind::LuSub,
            [this,token,c,k,upper,mul,broadcast,factor,operands,leftInputs,rightInputs,operation] {
                auto *r = find(token);
                if (!r || r->stage == RequestStage::Failed) return;
                for (int target = c+(upper ? 1 : 0); target < N; ++target) {
                    if (mul) {
                        // A common input is read once and physically fanned
                        // out. Preserve operand order and latch at issue.
                        volatile double v = broadcast ?
                            (upper ? factor*operands[target] : operands[target]*factor) :
                            leftInputs[target]*rightInputs[target];
                        r->luProducts[target] = v;
                    } else {
                        volatile double v = leftInputs[target]-rightInputs[target];
                        r->luTemps[target] = v;
                    }
                }
                r->luWaiting = false;
                if (mul) r->luPhase = upper ? LuPhase::UpperSub : LuPhase::LowerSub;
                else {
                    r->luK++;
                    if (r->luK==c) noteLuReturn(*r,operation);
                    r->luPhase = r->luK < c ? (upper ? LuPhase::UpperMul : LuPhase::LowerMul) :
                                                            (upper ? LuPhase::DivideUpper : LuPhase::StoreLower);
                }
            },broadcast,-1,0,false,0,env.localProducts ? &access : nullptr);
        if (!accepted) { req.luWaiting = false; req.record.stallLuMulSub++; }
        else {
            if (broadcast && width>1)
                tracePreprocessBroadcast(req,"lu",upper ? "upper" : "lower",k,c,-1,width,factor);
            if (mul) req.record.luMulIssued += width;
            else req.record.luSubIssued += width;
            trace(req,"lu",mul ? "LU_ROW_MUL_ISSUE" : "LU_ROW_SUB_ISSUE",-1,-1,k,c);
        }
        break;
      }
      case LuPhase::StoreLower:
        for (int row = c; row < N; ++row) req.lu[row*N+c] = req.luTemps[row];
        if (!req.record.luFirstResultCycle) req.record.luFirstResultCycle = now;
        req.luPhase = LuPhase::PivotCheck;
        break;
      case LuPhase::PivotCheck:
        if (!std::isfinite(req.lu[c*N+c]) || std::fabs(req.lu[c*N+c]) <= req.desc.pivotEpsilon) {
            markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"pivot");
            break;
        }
        if (env.luReciprocalShare && !req.reciprocalReady[c] &&
            !req.reciprocalPending[c]) {
            if (!issueDivide(luDiv,OpKind::LuDiv,now,token,1.0,req.lu[c*N+c],
                [this,token,c](double value) {
                    auto *r=find(token);
                    if (!r || r->stage==RequestStage::Failed) return;
                    r->reciprocal[c]=value;
                    r->reciprocalReady[c]=true;
                    r->reciprocalPending[c]=false;
                    trace(*r,"lu","LU_RECIP_READY",-1,-1,c);
                })) {
                req.record.stallLuDivider++;
                break;
            }
            req.reciprocalPending[c]=true;
            req.record.luDivIssued++;
            trace(req,"lu","LU_RECIP_DIV_ISSUE",-1,-1,c);
        }
        if (!req.forwardStepReady[c]) {
            req.forwardStepReady[c] = true;
            req.record.luStepReadyCycle[c] = now;
            trace(req,"lu","LU_STEP_READY",-1,-1,c,-1,"forward");
        }
        req.luPhase = c+1 < N ? LuPhase::StartUpper : LuPhase::NextColumn;
        break;
      case LuPhase::StartUpper:
        if (env.luUpperOverlap) {
            req.luDivIssued=req.luDivCompleted=0;
            req.luPhase=c ? LuPhase::UpperMul : LuPhase::DivideUpper;
            if (!c) {
                panic_if(!req.luUpper.ready,"CFD first upper input selected before publication");
                req.luTemps=req.luUpper.accumulator;
                traceLuUpper(req,"join");
            }
            break;
        }
        for (int j = c+1; j < N; ++j) req.luTemps[j] = req.d[c*N+j];
        req.luK = 0;
        if (env.luProductAhead) req.luProductWindow.begin(c);
        req.luDivIssued = req.luDivCompleted = 0;
        req.luPhase = c ? LuPhase::UpperMul : LuPhase::DivideUpper;
        break;
      case LuPhase::DivideUpper:
      {
        if (env.luReciprocalShare) {
            // The upper dot products are independent of the reciprocal.
            // Only their final normalization waits for its actual RF return.
            if (!req.reciprocalReady[c]) {
                observe("lu_reciprocal_dependency");
                break;
            }
            const unsigned width=N-c-1;
            const auto numerators=req.luTemps;
            const double reciprocal=req.reciprocal[c];
            const uint64_t operation=nextRfTag;
            req.luWaiting=true;
            if (!issueLanes(sharedMul,OpKind::SharedMul,now,width,OpKind::LuMul,
                [this,token,c,numerators,reciprocal,operation] {
                    auto *r=find(token);
                    if (!r || r->stage==RequestStage::Failed) return;
                    for (unsigned j=c+1;j<N;++j) {
                        volatile double value=numerators[j]*reciprocal;
                        r->luTemps[j]=value;
                        if (env.traceEnable)
                            std::fprintf(stderr,"CFD_LU_SCALE cycle=%llu request=%llu generation=%llu cell=%llu k=%d column=%u numerator=%a reciprocal=%a result=%a\n",
                                (unsigned long long)cycle(),(unsigned long long)r->record.requestId,
                                (unsigned long long)r->desc.generation,(unsigned long long)r->desc.cellId,
                                c,j,numerators[j],reciprocal,(double)value);
                    }
                    noteLuReturn(*r,operation);
                    r->luWaiting=false;
                    r->luPhase=LuPhase::StoreUpper;
                })) {
                req.luWaiting=false;
                req.record.stallLuMulSub++;
                break;
            }
            req.record.luMulIssued+=width;
            trace(req,"lu","LU_UPPER_SCALE_ISSUE",-1,-1,c,width);
            if (env.luUpperOverlap) traceLuUpper(req,"scale",-1,operation);
            break;
        }
        int j = c+1;
        while (j < N && (req.luDivIssued & (1u<<j))) j++;
        if (j == N) { observe("lu_divide_result_dependency"); break; }
        const double numerator = req.luTemps[j], denominator = req.lu[c*N+c];
        if (issueDivide(luDiv,OpKind::LuDiv,now,token,numerator,denominator,
            [this,token,c,j](double value) {
                auto *r = find(token);
                if (!r || r->stage == RequestStage::Failed) return;
                r->luTemps[j] = value;
                r->luDivCompleted |= 1u<<j;
                const uint8_t wanted = 0x1f & ~((1u<<(c+1))-1);
                if (r->luDivCompleted == wanted) r->luPhase = LuPhase::StoreUpper;
            })) {
            req.luDivIssued |= 1u<<j;
            req.record.luDivIssued++;
            trace(req,"lu","LU_DIV_ISSUE",-1,-1,c,j);
        } else req.record.stallLuDivider++;
        break;
      }
      case LuPhase::StoreUpper:
        for (int j = c+1; j < N; ++j) {
            if (!std::isfinite(req.luTemps[j])) {
                markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"upper");
                return;
            }
            req.lu[c*N+j] = req.luTemps[j];
        }
        req.luPhase = LuPhase::NextColumn;
        break;
      case LuPhase::NextColumn:
        req.backwardStepReady[c] = true;
        trace(req,"lu","LU_COLUMN_DONE",-1,-1,c);
        req.luCol++;
        req.luPhase = req.luCol < N ? LuPhase::StartLower : LuPhase::Done;
        break;
      case LuPhase::Done:
        req.record.luCompleteCycle = now;
        req.luDone = true;
        req.stage = req.solveStarted ? RequestStage::SolveActive : RequestStage::LuReady;
        trace(req,"lu","LU_COMPLETE");
        tracePrivateLu(req);
        break;
    }
    // Only register commit, index advance and operand selection can fold.
    // Pivot checks and all arithmetic issue/wait states end the visit. The
    // original visit guard and a hard three-transition limit remain in force.
    const bool bookkeeping=phase==LuPhase::StartLower || phase==LuPhase::StoreLower ||
        phase==LuPhase::StartUpper || phase==LuPhase::StoreUpper || phase==LuPhase::NextColumn;
    if (!env.luControlFold || !bookkeeping || folds==3 ||
        req.stage==RequestStage::Failed) break;
    ++folds;
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LU_CONTROL cycle=%llu event=fold token=%llu generation=%llu request=%llu cell=%llu column=%d next_column=%d from=%u to=%u index=%u parent=%llu returned=%llu\n",
            (unsigned long long)now,(unsigned long long)req.token,
            (unsigned long long)req.desc.generation,(unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.cellId,c,req.luCol,
            static_cast<unsigned>(phase),static_cast<unsigned>(req.luPhase),folds,
            (unsigned long long)req.luControlParent,(unsigned long long)req.luControlReturned);
    }
}
