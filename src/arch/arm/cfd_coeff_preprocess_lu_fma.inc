void
CfdCoeffPreprocessController::traceLuFmaMatrix(const Request &req,const char *event)
{
    if (!env.traceEnable || !env.luFmaCount) return;
    std::fprintf(stderr,"CFD_LU_FMA_MATRIX cycle=%llu event=%s token=%llu generation=%llu request=%llu cell=%llu",
        (unsigned long long)cycle(),event,(unsigned long long)req.token,
        (unsigned long long)req.desc.generation,(unsigned long long)req.record.requestId,
        (unsigned long long)req.desc.cellId);
    for(unsigned i=0;i<N*N;++i)std::fprintf(stderr," v%u=%a",i,req.lu[i]);
    std::fprintf(stderr,"\n");
}

void
CfdCoeffPreprocessController::traceLuFma(const Request &req,unsigned column,
    unsigned row,const char *event,uint64_t rf,const std::array<double,N> &a,
    const std::array<double,N> &b,const std::array<double,N> &c,
    const std::array<double,N> &result)
{
    if (!env.traceEnable) return;
    const unsigned width=N-column-1;
    std::fprintf(stderr,"CFD_LU_FMA cycle=%llu event=%s rf=%llu token=%llu generation=%llu request=%llu cell=%llu column=%u row=%u width=%u",
        (unsigned long long)cycle(),event,(unsigned long long)rf,
        (unsigned long long)req.token,(unsigned long long)req.desc.generation,
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
        column,row,width);
    for(unsigned lane=0;lane<width;++lane)
        std::fprintf(stderr," a%u=%a b%u=%a c%u=%a r%u=%a",
            lane,a[lane],lane,b[lane],lane,c[lane],lane,result[lane]);
    std::fprintf(stderr,"\n");
}

void
CfdCoeffPreprocessController::processLuFma(Request &req,unsigned context,uint64_t now)
{
    auto &state=luRankStates.at(context);
    if (!state.canFma(now)) return;
    const unsigned c=state.column,row=state.nextSub,width=N-c-1;
    const uint64_t token=req.token,generation=req.desc.generation;
    const auto request=req.record.requestId,cell=req.desc.cellId;
    const bool broadcast=env.preprocessBroadcast;
    std::array<double,N> a{},b{},acc{};
    for(unsigned lane=0;lane<width;++lane) {
        const unsigned j=c+1+lane;
        a[lane]=req.lu[row*N+c];b[lane]=req.lu[c*N+j];acc[lane]=req.lu[row*N+j];
    }
    const uint64_t rf=nextRfTag;
    if (!issueLanes(sharedLuFma,OpKind::LuFma,now,width,OpKind::LuFma,
        [this,token,generation,request,cell,context,c,row,width,rf,a,b,acc] {
            auto &s=luRankStates.at(context);
            panic_if(s.owner!=token || s.epoch!=generation,"CFD stale LU FMA owner");
            std::array<double,N> result{};
            for(unsigned lane=0;lane<width;++lane)
                result[lane]=CfdLuFma::evaluate(a[lane],b[lane],acc[lane]);
            const bool active=s.subtracted(row,rf,cycle());
            auto *r=find(token);
            const bool valid=active && r && r->stage!=RequestStage::Failed &&
                r->desc.generation==generation;
            Request witness{};witness.token=token;witness.desc.generation=generation;
            witness.record.requestId=request;witness.desc.cellId=cell;
            traceLuFma(witness,c,row,valid ? "complete" : "drop",rf,a,b,acc,result);
            if (!valid) return;
            for(unsigned lane=0;lane<width;++lane)r->lu[row*N+c+1+lane]=result[lane];
            if(row==c+1)traceLuPivotRow(*r,c+1,rf,a[0],b[0],acc[0],true,result[0]);
            traceLuRank1(*r,"fma-ready",c,row,-1,rf);
        },broadcast,static_cast<int>(CfdLuFma::reads(width,env.rfBroadcast && broadcast)))) {
        req.record.stallLuMulSub++;return;
    }
    panic_if(nextRfTag!=rf+1,"CFD LU FMA lost physical RF tag");
    state.fused(row,rf,now);
    // Existing counters retain their mathematical operation convention;
    // CFD_RESOURCE_STATS and physical traces expose the actual FMA bank.
    req.record.luMulIssued+=width;req.record.luSubIssued+=width;
    traceLuFma(req,c,row,"issue",rf,a,b,acc,{});
    trace(req,"lu","LU_ROW_FMA_ISSUE",-1,-1,c,row);
    traceLuRank1(req,"fma-issue",c,row,-1,rf);
    if(broadcast && width>1)
        tracePreprocessBroadcast(req,"lu","rank1-fma",c,row,-1,width,a[0]);
}
