// Explicit, bounded three-input FP64 FMA pipeline. Each group holds its
// captured operands until the real global RF write grants its return.
void
CfdCoeffPreprocessController::traceRhsFma(const Request &req, unsigned index,
    bool backward, int k, unsigned first, unsigned width, const char *event,
    uint64_t rf, const std::array<double,N> &a, const std::array<double,N> &b,
    const std::array<double,N> &c, const std::array<double,N> &result, bool upper, bool streaming)
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_RHS_FMA cycle=%llu event=%s rf=%llu token=%llu generation=%llu request=%llu cell=%llu rhs=%u backward=%u k=%d first=%u width=%u",
        (unsigned long long)cycle(),event,(unsigned long long)rf,
        (unsigned long long)req.token,(unsigned long long)req.desc.generation,
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
        index,backward,k,first,width);
    std::fprintf(stderr," upper_inverse=%u streaming=%u",upper,streaming);
    for (unsigned lane=0;lane<width;++lane)
        std::fprintf(stderr," a%u=%a b%u=%a c%u=%a r%u=%a",
            lane,a[lane],lane,b[lane],lane,c[lane],lane,result[lane]);
    std::fprintf(stderr,"\n");
}

bool
CfdCoeffPreprocessController::issueSolveFma(Request &req, unsigned index, uint64_t now,bool streamBackward)
{
    auto &rhs=req.rhs[index];
    const bool streaming=rhsUpperStreaming(req);
    const bool backward=streamBackward || rhs.phase==RhsPhase::BackwardMul;
    const int k=streamBackward ? N-rhs.i-1 : rhs.k;
    if(streamBackward && (!streaming || !CfdRhsUpperStream::ready(rhs.i,rhs.normalizedMask,rhs.backwardWaiting)))return false;
    const bool transformed=backward && req.upperSlot>=0;
    panic_if(k<0 || k>=4 || (backward && !transformed && (rhs.i<=k || rhs.i>=N)),
             "CFD invalid RHS FMA topology");
    const unsigned first=transformed ? 0 : backward ? rhs.i : k+1;
    const unsigned upperColumn=N-k-1;
    if (transformed && !upperColumnReady(req,upperColumn)) {
        observe("rhs_upper_dependency");return false;
    }
    const unsigned width=backward && !transformed ? 1 : N-k-1;
    const bool broadcast=env.preprocessBroadcast && (!backward || transformed);
    const uint64_t token=req.token, generation=req.desc.generation;
    const bool retain=env.rhsFactorReuse && (!backward || transformed) && !req.coefficientCacheReuse;
    int factorSlot=retain ? rhsFactors.lookup(token,generation,backward,k,now) : -1;
    const CfdRhsFactorFanout::Transfer *fanout=nullptr;
    if (factorSlot>=0 && !localVectors.banks.at(rhsFactorBank(factorSlot)).canRead(now,1)) {
        if(env.rhsFmaFactorFanout && (!env.rhsFactorFanoutFeedback || index<2*N))
            fanout=rhsFactorFanout.probe(rhsFactors.slots[factorSlot],factorSlot,
                rhsFactorBank(factorSlot),now,index,width);
        if(!fanout)factorSlot=-1; // Fully paid global RF fallback.
    }
    const uint64_t fanoutSource=fanout ? fanout->sourceRf : 0;
    const auto *retained=factorSlot>=0 ? &rhsFactors.slots[factorSlot] : nullptr;
    const uint64_t factorId=retained ? retained->id : 0;
    std::array<double,N> a{},b{},c{},factorInputs{};
    for (unsigned lane=0;lane<width;++lane) {
        const unsigned row=first+lane;
        const unsigned factorRow=transformed ? k+1+lane : row;
        a[lane]=fanout ? fanout->value[factorRow] : retained ? retained->value[factorRow] : transformed ?
            CfdRhsUpper::get(rhsUpper.slots.at(req.upperSlot),req.lu,row,upperColumn) :
            backward ? req.lu[k*N+row] : req.lu[row*N+k];
        b[lane]=rhs.value[transformed ? upperColumn : backward ? row : k];
        c[lane]=rhs.value[backward && !transformed ? k : row];
        factorInputs[factorRow]=a[lane];
    }
    const unsigned reads=CfdRhsFma::reads(width,env.rfBroadcast && broadcast);
    CfdLocalAccess access;
    if (retained) {
        access.owner=token;access.epoch=generation;
        if(fanout) {
            access.rhsFactorSource=fanoutSource;access.rhsFactorWords=width;
        } else {access.a=rhsFactorBank(factorSlot);access.wordsA=width;}
        access.globalReads=reads-width;access.rhsFactor=factorId;
    }
    const uint64_t rf=nextRfTag;
    // On failed requests the physically paid operation still drains. Capture
    // the identity independently so the drop witness survives retirement.
    const auto request=req.record.requestId, cell=req.desc.cellId;
    const bool accepted=issueLanes(sharedFma,OpKind::SharedFma,now,width,OpKind::Mul,
        [this,token,generation,request,cell,index,k,first,width,backward,transformed,streaming,streamBackward,rf,a,b,c] {
            std::array<double,N> result{};
            for (unsigned lane=0;lane<width;++lane)
                result[lane]=CfdRhsFma::evaluate(a[lane],b[lane],c[lane]);
            auto *r=find(token);
            const bool valid=r && r->stage!=RequestStage::Failed && r->desc.generation==generation;
            if (r && r->desc.generation==generation)
                traceRhsFma(*r,index,backward,k,first,width,valid ? "complete" : "drop",rf,a,b,c,result,transformed,streaming);
            else if (env.traceEnable) {
                // traceRhsFma reads identity only; a temporary avoids borrowing
                // any live request with a different generation.
                Request witness{};witness.token=token;witness.desc.generation=generation;
                witness.desc.cellId=cell;witness.record.requestId=request;
                traceRhsFma(witness,index,backward,k,first,width,"drop",rf,a,b,c,result,transformed,streaming);
            }
            if (!valid) return;
            auto &s=r->rhs[index];
            if(streamBackward) {
                panic_if(!s.backwardWaiting || s.i!=int(width) ||
                    (s.normalizedMask&((1u<<(width+1))-1))!=((1u<<(width+1))-1),
                    "CFD stale streaming upper return");
            } else panic_if(!s.waiting || s.k!=k || s.phase!=(backward ? RhsPhase::BackwardMul : RhsPhase::ForwardMul),
                           "CFD stale RHS FMA return");
            for (unsigned lane=0;lane<width;++lane)
                s.value[backward && !transformed ? k : first+lane]=result[lane];
            if(streamBackward) {
                s.backwardWaiting=false;++s.i;
                if(s.i==N) {
                    panic_if(s.waiting || s.normalizedMask!=31 || s.phase!=RhsPhase::StreamUpper,
                             "CFD upper stream completed before forward RF returns");
                    s.phase=RhsPhase::Done;
                }
                return;
            }
            s.waiting=false;
            if (!backward) {
                ++s.k;s.phase=s.k==4 ? RhsPhase::LastDiv : RhsPhase::ForwardDiv;
            } else if (transformed) {
                if(k==0)s.phase=RhsPhase::Done;
                else {--s.k;s.i=s.k+1;s.phase=RhsPhase::BackwardMul;}
            } else if (++s.i<N) {
                s.phase=RhsPhase::BackwardMul;
            } else if (k==0) {
                s.phase=RhsPhase::Done;
            } else {
                --s.k;s.i=s.k+1;s.phase=RhsPhase::BackwardMul;
            }
        },broadcast,retained ? -1 : static_cast<int>(reads),0,false,0,
        retained ? &access : nullptr);
    if (!accepted) {
        req.record.stallCoeffMulSub++;
        return false;
    }
    panic_if(nextRfTag!=rf+1,"CFD RHS FMA lost physical RF identity");
    if(streamBackward)rhs.backwardWaiting=true;else rhs.waiting=true;
    req.record.coeffMulIssued+=width;req.record.coeffSubIssued+=width;
    traceRhsFma(req,index,backward,k,first,width,"issue",rf,a,b,c,{},transformed,streaming);
    if (broadcast && width>1)
        tracePreprocessBroadcast(req,"rhs",transformed ? "upper" : "forward",k,-1,index,width,b[0]);
    if (retained) {
        if(fanout) {
            const auto consumed=rhsFactorFanout.commit(*retained,factorSlot,
                rhsFactorBank(factorSlot),now,index,width,fanoutSource,rf);
            traceRhsFactorFanout(req,"use",consumed,now,rf,index);
        } else {
            rhsFactors.capture(factorSlot,factorId,now);
            traceRhsFactor(req,factorSlot,"use",now,rf,index);
            if(env.rhsFmaFactorFanout && rhsFactorFanout.offer(*retained,factorSlot,
                rhsFactorBank(factorSlot),localVectors.banks.at(rhsFactorBank(factorSlot)),
                now,rf,index))
                traceRhsFactorFanout(req,"offer",rhsFactorFanout.source(now),now,rf,index);
        }
    } else if (retain) {
        // Only coefficients from these admitted RF reads may populate a
        // registered 1R1W factor bank; backward scalar FMAs pay their own reads.
        const int fill=rhsFactors.victim(token,generation,backward,k,now);
        if (fill>=0 && localVectors.write(now,rhsFactorBank(fill))) {
            rhsFactors.fill(fill,token,generation,backward,k,now,factorInputs);
            traceRhsFactor(req,fill,"fill",now,rf,index);
        }
    }
    return true;
}
