// Six RF state words per owner; all ten precompute updates use the existing
// real FMA/RF path. No LU coefficient is copied without a paid operand read.
void
CfdCoeffPreprocessController::traceRhsUpper(const CfdRhsUpper::Slot &s,
    unsigned slot,const char *event,uint64_t rf,int entry,int k,
    double a,double b,double c,double result)
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_RHS_UPPER cycle=%llu event=%s id=%llu slot=%u token=%llu generation=%llu request=%llu cell=%llu rf=%llu entry=%d i=%d j=%d k=%d done=%u pending=%u cancelled=%u a=%a b=%a c=%a result=%a\n",
        (unsigned long long)cycle(),event,(unsigned long long)s.id,slot,
        (unsigned long long)s.token,(unsigned long long)s.epoch,
        (unsigned long long)s.request,(unsigned long long)s.cell,
        (unsigned long long)rf,entry,entry<0 ? -1 : int(CfdRhsUpper::Row[entry]),
        entry<0 ? -1 : int(CfdRhsUpper::Col[entry]),k,s.done,s.pending,s.cancelled,a,b,c,result);
}

bool
CfdCoeffPreprocessController::acquireRhsUpper(Request &req)
{
    if (!env.rhsUpperInverse || req.coefficientCacheReuse || req.rhsCount<=1)
        return true;
    panic_if(!req.directBaseSolve,"CFD shared upper requires direct RHS");
    if (req.upperSlot>=0) return true;
    const int slot=rhsUpper.allocate(req.token,req.desc.generation,
        req.record.requestId,req.desc.cellId);
    if (slot<0) { observe("rhs_upper_owner_full");return false; }
    req.upperSlot=slot;
    traceRhsUpper(rhsUpper.slots[slot],slot,"allocate");
    return true;
}

void
CfdCoeffPreprocessController::releaseRhsUpper(Request &req,bool cancelled)
{
    if (req.upperSlot<0) return;
    const unsigned slot=req.upperSlot;auto &s=rhsUpper.slots.at(slot);
    panic_if(s.token!=req.token || s.epoch!=req.desc.generation || !s.id,
             "CFD shared upper release lost owner");
    req.upperSlot=-1;
    if (cancelled) {
        s.cancelled=true;traceRhsUpper(s,slot,"cancel");
    } else panic_if(s.pending || s.done!=63 || !req.solveDone,
                    "CFD shared upper released before RHS/precompute drained");
    if (!s.pending) {
        traceRhsUpper(s,slot,"release");rhsUpper.release(slot,s.id);
    }
}

bool
CfdCoeffPreprocessController::upperColumnReady(const Request &req,unsigned j) const
{
    panic_if(req.upperSlot<0 || !j || j>=N,"CFD missing shared upper column");
    const auto &s=rhsUpper.slots.at(req.upperSlot);
    panic_if(s.cancelled || s.token!=req.token || s.epoch!=req.desc.generation,
             "CFD shared upper column lost owner");
    for(unsigned i=0;i<j;++i)
        if (!req.backwardStepReady[i] || !CfdRhsUpper::ready(s,i,j)) return false;
    return true;
}

void
CfdCoeffPreprocessController::processRhsUpper(uint64_t now,uint64_t frontierCell)
{
    if (!env.rhsUpperInverse) return;
    auto order=admissionOrder();
    std::stable_sort(order.begin(),order.end(),[frontierCell](const Request *a,const Request *b) {
        return (a->desc.cellId==frontierCell)>(b->desc.cellId==frontierCell);
    });
    unsigned grants=0;
    for(auto *req:order) {
        if (req->upperSlot<0 || req->stage==RequestStage::Failed) continue;
        const unsigned slot=req->upperSlot;auto &s=rhsUpper.slots.at(slot);
        DiagnosticOwner owner(diagnosticOwner,req->token);
        // The longest column gets first access to spare lanes. At most six
        // scalar selectors run per tick, after current b/Lbar first refusal.
        for(int entry=5;entry>=0 && grants<6;--entry) {
            const unsigned bit=1u<<entry;
            if ((s.done|s.pending)&bit) continue;
            const unsigned i=CfdRhsUpper::Row[entry],j=CfdRhsUpper::Col[entry],k=s.next[entry];
            if (!req->backwardStepReady[i] ||
                (k+1==j ? !req->backwardStepReady[k] : !CfdRhsUpper::ready(s,k,j))) continue;
            const double a=req->lu[i*N+k],b=CfdRhsUpper::get(s,req->lu,k,j),
                c=k==j-1 ? req->lu[i*N+j] : s.value[entry];
            const uint64_t id=s.id,rf=nextRfTag;
            if (!issueLanes(sharedFma,OpKind::SharedFma,now,1,OpKind::SharedFma,
                [this,slot,id,entry,k,rf,a,b,c] {
                    auto &state=rhsUpper.slots.at(slot);
                    panic_if(state.id!=id || !(state.pending&(1u<<entry)),
                             "CFD stale shared upper FMA");
                    auto *r=find(state.token);
                    const bool valid=!state.cancelled && r && r->stage!=RequestStage::Failed &&
                        r->desc.generation==state.epoch;
                    if (!valid && !state.cancelled) {
                        state.cancelled=true;traceRhsUpper(state,slot,"cancel");
                        if(r && r->upperSlot==int(slot))r->upperSlot=-1;
                    }
                    state.pending&=~(1u<<entry);
                    const double value=CfdRhsFma::evaluate(a,b,c);
                    if (valid) {
                        state.value[entry]=value;
                        if(k==CfdRhsUpper::Row[entry]+1)state.done|=1u<<entry;
                        else --state.next[entry];
                        ++rhsUpper.completed;
                    } else ++rhsUpper.dropped;
                    traceRhsUpper(state,slot,valid ? "complete" : "drop",rf,entry,k,a,b,c,value);
                    if(state.cancelled && !state.pending) {
                        traceRhsUpper(state,slot,"release");rhsUpper.release(slot,id);
                    }
                },false,3)) continue;
            panic_if(nextRfTag!=rf+1,"CFD shared upper lost physical FMA tag");
            s.pending|=bit;++rhsUpper.issued;++grants;
            traceRhsUpper(s,slot,"issue",rf,entry,k,a,b,c,0);
        }
    }
}
