void
CfdCoeffPreprocessController::traceLuPivotRow(const Request &req,unsigned column,
    uint64_t rf,double a,double b,double c,bool fused,double result)
{
    if(!env.traceEnable || !env.luPivotPrefetch)return;
    std::fprintf(stderr,"CFD_LU_PIVOT_ROW cycle=%llu token=%llu generation=%llu request=%llu cell=%llu column=%u rf=%llu fused=%u a=%a b=%a c=%a result=%a\n",
        (unsigned long long)cycle(),(unsigned long long)req.token,
        (unsigned long long)req.desc.generation,(unsigned long long)req.record.requestId,
        (unsigned long long)req.desc.cellId,column,(unsigned long long)rf,fused,a,b,c,result);
}

void
CfdCoeffPreprocessController::processLuPivotPrefetch(uint64_t now)
{
    if(!env.luPivotPrefetch)return;
    for(auto *req:admissionOrder()) {
        if(req->stage!=RequestStage::LuActive)continue;
        const unsigned context=luProductBank(req->token)-27;
        auto &state=luRankStates.at(context);
        if(!state.canPrefetch())continue;
        DiagnosticOwner owner(diagnosticOwner,req->token);
        const unsigned c=state.column+1;
        const double diagonal=req->lu[c*N+c];
        if(!std::isfinite(diagonal) || std::fabs(diagonal)<=req->desc.pivotEpsilon)
            continue; // Normal pivot validation reports the failure at its barrier.
        const auto token=req->token,generation=req->desc.generation,operation=nextRfTag;
        const auto request=req->record.requestId,cell=req->desc.cellId,source=state.diagonalId;
        if(!issueDivide(luDiv,OpKind::LuDiv,now,token,1.0,diagonal,
            [this,token,generation,context,c,operation](double value) {
                auto &s=luRankStates.at(context);
                panic_if(s.owner!=token || s.epoch!=generation,"CFD stale LU prefetched pivot owner");
                const bool adopted=s.pivotId==operation && !s.prefetchId;
                const bool active=s.prefetched(operation,cycle());
                auto *r=find(token);
                const bool valid=active && r && r->stage!=RequestStage::Failed && r->desc.generation==generation;
                if(!valid)return;
                r->reciprocal[c]=value;r->reciprocalReady[c]=true;r->reciprocalPending[c]=false;
                trace(*r,"lu","LU_RECIP_READY",-1,-1,c);
                traceLuRank1(*r,adopted ? "reciprocal-ready" : "prefetch-ready",c,-1,-1,operation);
            })) continue;
        state.prefetch(operation);req->reciprocalPending[c]=true;++req->record.luDivIssued;
        trace(*req,"lu","LU_RECIP_DIV_ISSUE",-1,-1,c);
        traceLuRank1(*req,"pivot-prefetch",c,-1,-1,operation);
        if(env.traceEnable)
            std::fprintf(stderr,"CFD_LU_PIVOT_PREFETCH cycle=%llu token=%llu generation=%llu request=%llu cell=%llu column=%u rf=%llu source=%llu diagonal=%a\n",
                (unsigned long long)now,(unsigned long long)token,(unsigned long long)generation,
                (unsigned long long)request,(unsigned long long)cell,c,
                (unsigned long long)operation,(unsigned long long)source,diagonal);
        break; // One bounded global prefetch selector per controller cycle.
    }
}

void
CfdCoeffPreprocessController::traceLuRank1(const Request &req,const char *event,
    unsigned column,int row,int slot,uint64_t id,int bank)
{
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_LU_RANK1 cycle=%llu event=%s token=%llu generation=%llu request=%llu cell=%llu column=%u row=%d slot=%d id=%llu bank=%d\n",
            (unsigned long long)cycle(),event,(unsigned long long)req.token,
            (unsigned long long)req.desc.generation,(unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.cellId,column,row,slot,(unsigned long long)id,bank);
}

void
CfdCoeffPreprocessController::cancelLuRank1(Request &req)
{
    const auto it=std::find(localLuOwners.begin(),localLuOwners.end(),req.token);
    if (it==localLuOwners.end()) return;
    auto &state=luRankStates.at(it-localLuOwners.begin());
    if (state.owner!=req.token || state.epoch!=req.desc.generation ||
        state.phase==CfdLuRank1::Phase::Cancelled) return;
    state.cancel();
    traceLuRank1(req,"cancel",state.column);
}

void
CfdCoeffPreprocessController::processLuRank1(Request &req,uint64_t now)
{
    const unsigned context=luProductBank(req.token)-27;
    auto &state=luRankStates.at(context);
    const uint64_t token=req.token,generation=req.desc.generation;
    panic_if(state.owner!=token || state.epoch!=generation,"CFD stale rank1 LU owner");
    const unsigned c=state.column,width=N-c-1;
    req.luCol=c;
    if (state.canPivot(now)) {
        if (!std::isfinite(req.lu[c*N+c]) ||
            std::fabs(req.lu[c*N+c])<=req.desc.pivotEpsilon) {
            markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"pivot");return;
        }
        if(state.prefetchId) {
            state.adoptPrefetch(now);
            req.forwardStepReady[c]=true;req.record.luStepReadyCycle[c]=now;
            trace(req,"lu","LU_STEP_READY",-1,-1,c,-1,"forward");
            traceLuRank1(req,"pivot-adopt",c,-1,-1,state.pivotId);
            return;
        }
        const uint64_t operation=nextRfTag;
        if (!issueDivide(luDiv,OpKind::LuDiv,now,token,1.0,req.lu[c*N+c],
            [this,token,generation,context,c,operation](double value) {
                auto &s=luRankStates.at(context);
                panic_if(s.owner!=token || s.epoch!=generation,"CFD stale rank1 reciprocal owner");
                const bool active=s.reciprocal(operation,cycle());
                auto *r=find(token);
                if (!active || !r || r->stage==RequestStage::Failed) return;
                r->reciprocal[c]=value;r->reciprocalReady[c]=true;r->reciprocalPending[c]=false;
                trace(*r,"lu","LU_RECIP_READY",-1,-1,c);
                traceLuRank1(*r,"reciprocal-ready",c,-1,-1,operation);
            })) { req.record.stallLuDivider++;return; }
        state.pivot(operation,now);
        req.reciprocalPending[c]=true;req.record.luDivIssued++;
        if (!req.record.luFirstResultCycle) req.record.luFirstResultCycle=now;
        req.forwardStepReady[c]=true;req.record.luStepReadyCycle[c]=now;
        trace(req,"lu","LU_RECIP_DIV_ISSUE",-1,-1,c);
        trace(req,"lu","LU_STEP_READY",-1,-1,c,-1,"forward");
        traceLuRank1(req,"pivot",c,-1,-1,operation);
        return;
    }
    if (state.canScale()) {
        panic_if(!req.reciprocalReady[c],"CFD rank1 bypassed reciprocal RF return");
        std::array<double,N> numerators{};
        for (unsigned j=c+1;j<N;++j) numerators[j]=req.lu[c*N+j];
        const double reciprocal=req.reciprocal[c];
        const uint64_t operation=nextRfTag;
        // Normalization uses its existing FU/RF tag. Its owner/return identity
        // is retained in the bounded rank1 context; no local-bank tag is needed.
        if (!issueLanes(sharedMul,OpKind::SharedMul,now,width,OpKind::LuMul,
            [this,token,generation,context,c,operation,numerators,reciprocal] {
                auto &s=luRankStates.at(context);
                panic_if(s.owner!=token || s.epoch!=generation,"CFD stale rank1 normalization owner");
                const bool active=s.scaled(operation,cycle());
                auto *r=find(token);
                if (!active || !r || r->stage==RequestStage::Failed) return;
                for (unsigned j=c+1;j<N;++j) {
                    volatile double value=numerators[j]*reciprocal;
                    r->lu[c*N+j]=value;
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_LU_SCALE cycle=%llu request=%llu generation=%llu cell=%llu k=%u column=%u numerator=%a reciprocal=%a result=%a\n",
                            (unsigned long long)cycle(),(unsigned long long)r->record.requestId,
                            (unsigned long long)generation,(unsigned long long)r->desc.cellId,
                            c,j,numerators[j],reciprocal,(double)value);
                }
                traceLuRank1(*r,"scale-ready",c,-1,-1,operation);
            })) { req.record.stallLuMulSub++;return; }
        state.scale(operation);req.record.luMulIssued+=width;
        trace(req,"lu","LU_UPPER_SCALE_ISSUE",-1,-1,c,width);
        traceLuRank1(req,"scale",c,-1,-1,operation);
        return;
    }
    if (state.canFinish(now)) {
        state.finish(now);req.backwardStepReady[c]=true;
        trace(req,"lu","LU_COLUMN_DONE",-1,-1,c);
        req.luDone=true;req.record.luCompleteCycle=now;
        req.stage=req.solveStarted ? RequestStage::SolveActive : RequestStage::LuReady;
        trace(req,"lu","LU_COMPLETE");tracePrivateLu(req);
        traceLuRank1(req,"done",c);traceLuFmaMatrix(req,"factor");return;
    }
    if (state.canBeginRows(now)) {
        for (unsigned j=c+1;j<N;++j)
            if (!std::isfinite(req.lu[c*N+j])) {
                markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"upper");return;
            }
        state.beginRows(now);req.backwardStepReady[c]=true;
        trace(req,"lu","LU_COLUMN_DONE",-1,-1,c);
        traceLuRank1(req,"rows-begin",c);
    }
    if (state.phase!=CfdLuRank1::Phase::Rows) return;
    if (env.luFmaCount) { processLuFma(req,context,now);return; }
    // One independent next-row MUL and one ordered row SUB per context/tick.
    // Each matrix element is updated for k=0,1,2,3 in its original order.
    for (bool mul : {false,true}) {
        const int slot=mul ? state.mulSlot() : state.subSlot();
        if (slot<0) continue;
        const unsigned row=mul ? state.nextMul : state.nextSub;
        const int bank=luAheadBank(token,slot);
        std::array<double,N> left{},right{};
        for (unsigned j=c+1;j<N;++j) {
            left[j]=mul ? req.lu[row*N+c] : req.lu[row*N+j];
            right[j]=mul ? req.lu[c*N+j] : state.products[slot].value[j];
        }
        CfdLocalAccess access;
        access.owner=token;access.epoch=generation;
        const bool broadcast=mul && env.preprocessBroadcast;
        access.globalReads=mul ? (broadcast && width>1 ? width+1 : 2*width) : width;
        if (mul) access.destination=bank;
        else { access.a=bank;access.wordsA=width; }
        const uint64_t operation=nextRfTag;
        if (!issueLanes(mul ? sharedMul : sharedAdd,
            mul ? OpKind::SharedMul : OpKind::SharedAdd,now,width,
            mul ? OpKind::LuMul : OpKind::LuSub,
            [this,token,generation,context,c,row,slot,operation,mul,left,right,bank] {
                auto &s=luRankStates.at(context);
                panic_if(s.owner!=token || s.epoch!=generation,"CFD stale rank1 trailing update owner");
                std::array<double,N> values{};
                for (unsigned j=c+1;j<N;++j) {
                    volatile double value=mul ? left[j]*right[j] : left[j]-right[j];
                    values[j]=value;
                }
                const bool active=mul ? s.multiplied(slot,operation,values) :
                                        s.subtracted(row,operation,cycle());
                auto *r=find(token);
                if (!active || !r || r->stage==RequestStage::Failed) return;
                if (!mul)
                    for (unsigned j=c+1;j<N;++j) r->lu[row*N+j]=values[j];
                if(!mul && row==c+1)
                    traceLuPivotRow(*r,c+1,operation,left[c+1],right[c+1],0,false,values[c+1]);
                traceLuRank1(*r,mul ? "mul-ready" : "sub-ready",c,row,slot,operation,bank);
            },broadcast,-1,0,false,0,&access)) { req.record.stallLuMulSub++;continue; }
        if (mul) { state.multiply(slot,row,operation);req.record.luMulIssued+=width; }
        else { state.subtract(slot,row,operation);req.record.luSubIssued+=width; }
        trace(req,"lu",mul ? "LU_ROW_MUL_ISSUE" : "LU_ROW_SUB_ISSUE",-1,-1,c,row);
        traceLuRank1(req,mul ? "mul-issue" : "sub-issue",c,row,slot,operation,bank);
        if (broadcast && width>1)
            tracePreprocessBroadcast(req,"lu","rank1",c,row,-1,width,left[c+1]);
    }
}
