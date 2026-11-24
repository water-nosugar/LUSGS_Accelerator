// All scalar divisions (LU, direct RHS, reciprocal generation) enter here.
// Operands and ownership are captured at request acceptance, never reread from
// mutable request storage at the result edge.
bool
CfdCoeffPreprocessController::issueDivide(ResourcePool &pool, OpKind kind,
    uint64_t now, uint64_t owner, double numerator, double denominator,
    std::function<void(double)> complete)
{
    if (!env.detailedDivider)
        return issue(pool,kind,now,[this,owner,numerator,denominator,complete] {
            auto *request = find(owner);
            if (!request || request->stage == RequestStage::Failed) return;
            volatile double result = numerator/denominator;
            complete(result);
        });
    if (env.resourceStats) {
        sharedDiv.attemptedNow = true;
        sharedDiv.attempts++;
    }
    const auto blocked = hardwareDivider.blocked();
    if (blocked != CfdFp64Divider::Block::None) {
        recordIssueRejection(kind, 2);
        if (env.resourceStats) {
            sharedDiv.rejected++;
            sharedDiv.rejectedAfterIssue += sharedDiv.issuedNow != 0;
        }
        observe(blocked == CfdFp64Divider::Block::Initiation ? "divider_ii" :
                blocked == CfdFp64Divider::Block::Capacity ? "divider_capacity" :
                                                          "divider_backpressure");
        return false;
    }
    if (rfEnabled() && !registerPorts.reserveReads(now,2)) {
        observe("rf_operand_ports");
        return false;
    }
    uint64_t a,b;
    std::memcpy(&a,&numerator,sizeof(a));
    std::memcpy(&b,&denominator,sizeof(b));
    const uint64_t tag = nextDividerTag++;
    CfdFp64Divider::Transaction transaction{tag,owner,a,b,now};
    panic_if(!hardwareDivider.accept(now,transaction),"CFD divider lost ready credit");
    if (env.resourceStats) sharedDiv.issuedNow++;
    sharedDiv.nextIssue[0] = now+env.clusterDivIi;
    pendingOps.push_back({now+env.clusterDivLatency,OpKind::SharedDiv,
        [this,tag,owner,complete] {
            panic_if(!dividerResponse || dividerResponse->tag != tag,
                     "CFD divider response tag mismatch");
            auto *request = find(owner);
            // Cancellation does not reset a shared FU or free in-flight credit.
            // Drain the tagged response, then discard it without writing state.
            if (!request || request->stage == RequestStage::Failed) return;
            double result;
            const auto bits = dividerResponse->value.bits;
            std::memcpy(&result,&bits,sizeof(result));
            complete(result);
        },1,tag});
    if (rfEnabled()) {
        auto &op = pendingOps.back();
        op.rfTag = nextRfTag++;
        op.caller = kind;
        op.rfReadWords = 2;
        traceRfIssue(op,now,"div",env.dividerCapacity+env.dividerResultDepth);
    }
    traceCluster("div",1,kind,now,env.clusterDivLatency);
    traceDivider(transaction,"accept",now);
    return true;
}

void
CfdCoeffPreprocessController::traceDivider(
    const CfdFp64Divider::Transaction &t,const char *event,uint64_t now,bool dropped)
{
    if (!env.traceEnable) return;
    if (!dividerTraceFp) {
        const std::string path = std::string(env.traceFile)+".divider.csv";
        dividerTraceFp = std::fopen(path.c_str(),"w");
        panic_if(!dividerTraceFp,"CFD cannot open divider trace");
        std::fprintf(dividerTraceFp,"cycle,event,tag,owner,a,b,result,flags,accepted,produced,core,outputs,dropped\n");
    }
    std::fprintf(dividerTraceFp,"%llu,%s,%llu,%llu,%016llx,%016llx,%016llx,%u,%llu,%llu,%zu,%zu,%u\n",
        (unsigned long long)now,event,(unsigned long long)t.tag,(unsigned long long)t.owner,
        (unsigned long long)t.a,(unsigned long long)t.b,(unsigned long long)t.value.bits,
        t.value.flags,(unsigned long long)t.accepted,(unsigned long long)t.produced,
        hardwareDivider.coreSize(),hardwareDivider.resultSize(),dropped);
    std::fflush(dividerTraceFp);
}
