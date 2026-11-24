void
CfdCoeffPreprocessController::traceRhsNorm(const Request &req,unsigned index,
    unsigned k,const char *event,uint64_t rf,double a,double b,double result)
{
    if(!env.traceEnable)return;
    std::fprintf(stderr,"CFD_RHS_NORM cycle=%llu event=%s rf=%llu token=%llu generation=%llu request=%llu cell=%llu rhs=%u k=%u a=%a b=%a result=%a\n",
        (unsigned long long)cycle(),event,(unsigned long long)rf,
        (unsigned long long)req.token,(unsigned long long)req.desc.generation,
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
        index,k,a,b,result);
}

bool
CfdCoeffPreprocessController::issueSolveStreamNorm(Request &req,unsigned index,uint64_t now)
{
    auto &rhs=req.rhs[index];const unsigned k=rhs.k;
    panic_if(!rhsUpperStreaming(req) || k>=N || rhs.waiting ||
        (rhs.phase!=RhsPhase::ForwardDiv && rhs.phase!=RhsPhase::LastDiv),
        "CFD invalid streaming normalization");
    if(!req.reciprocalReady[k] || req.reciprocalPending[k]) {
        observe("rhs_reciprocal_dependency");req.record.stallDependency++;return false;
    }
    const auto token=req.token,generation=req.desc.generation,
        request=req.record.requestId,cell=req.desc.cellId,rf=nextRfTag;
    const double a=rhs.value[k],b=req.reciprocal[k];
    if(!issueLanes(sharedMul,OpKind::SharedMul,now,1,OpKind::Mul,
        [this,token,generation,request,cell,index,k,rf,a,b] {
            volatile double rounded=a*b;const double result=rounded;
            auto *r=find(token);
            const bool valid=r && r->desc.generation==generation && r->stage!=RequestStage::Failed;
            Request witness{};witness.token=token;witness.desc.generation=generation;
            witness.record.requestId=request;witness.desc.cellId=cell;
            traceRhsNorm(witness,index,k,valid ? "complete" : "drop",rf,a,b,result);
            if(!valid)return;
            auto &s=r->rhs[index];
            panic_if(!s.waiting || s.k!=int(k) || (s.normalizedMask&(1u<<k)) ||
                (s.phase!=RhsPhase::ForwardDiv && s.phase!=RhsPhase::LastDiv),
                "CFD stale streaming normalization return");
            s.value[k]=result;s.normalizedMask|=1u<<k;s.waiting=false;
            s.phase=k==4 ? RhsPhase::StreamUpper : RhsPhase::ForwardMul;
        })) {
        req.record.stallCoeffMulSub++;return false;
    }
    panic_if(nextRfTag!=rf+1,"CFD streaming norm lost physical RF identity");
    rhs.waiting=true;++req.record.coeffMulIssued;
    traceRhsNorm(req,index,k,"issue",rf,a,b,0);
    trace(req,"solve","RECIP_MUL_ISSUE",index/5,index%5,k);
    return true;
}
