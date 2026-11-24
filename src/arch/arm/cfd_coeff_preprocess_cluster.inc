// One physical multiplier bank, one add/sub bank, and one shared divider.
// A vector reserves all five lanes atomically; scalar LU/RHS work reserves one.
bool
CfdCoeffPreprocessController::issueLanes(
    ResourcePool &pool, OpKind kind, uint64_t now, unsigned lanes,
    OpKind caller, std::function<void()> complete, bool broadcast,
    int readWords, unsigned localReads, bool localResult, unsigned localRow,
    const CfdLocalAccess *bank)
{
    if (env.arithmeticMode != CfdArithmeticGroups::Legacy &&
        (&pool == &sharedMul || &pool == &sharedAdd)) {
        panic_if(readWords >= 0 || localReads || localResult,
                 "CFD partition and local-lane models cannot be combined");
        return issueArithmeticGroup(pool,lanes,caller,broadcast,now,std::move(complete));
    }
    panic_if(env.detailedDivider && &pool == &sharedDiv,
             "CFD detailed divider cannot bypass operand latching and credits");
    if (bank && (bank->rhsFactorSource || bank->rhsFactorWords))
        panic_if(!env.rhsFactorFanout || caller!=OpKind::Mul ||
                 !((&pool==&sharedMul && bank->destination>=0) ||
                   (env.rhsFmaFactorFanout && &pool==&sharedFma &&
                    kind==OpKind::SharedFma && bank->destination<0)) ||
                 !bank->rhsFactor || !bank->rhsFactorSource ||
                 bank->rhsFactorWords!=lanes || lanes>4 ||
                 bank->a>=0 || bank->b>=0 || bank->wordsA || bank->wordsB,
                 "CFD invalid factor-bus operand admission");
    if (env.resourceStats) {
        pool.attemptedNow = true;
        pool.attempts++;
    }
    const bool lu = caller == OpKind::LuMul;
    const bool restricted = &pool == &sharedMul && env.extraMulLuOnly;
    const unsigned limit = restricted && !lu ? N : pool.nextIssue.size();
    const unsigned available = std::count_if(pool.nextIssue.begin(),
        pool.nextIssue.begin()+limit, [now](uint64_t next) { return next <= now; });
    if (available < lanes) {
        if (&pool == &sharedLuFma) observe("lu_fma_issue_lanes");
        else if (&pool == &sharedFma) observe("rhs_fma_issue_lanes");
        else recordIssueRejection(caller, &pool == &sharedMul ? 0 :
                                      &pool == &sharedDiv ? 2 : 1);
        if (env.resourceStats) {
            pool.rejected++;
            pool.rejectedAfterIssue += pool.issuedNow != 0;
        }
        return false;
    }
    if (rfEnabled() && pool.rfLive + lanes > rfCapacity(pool)) {
        observe("rf_fu_return_capacity");
        return false;
    }
    if (bank && !localVectors.canRead(now,*bank)) {
        observe("local_vector_read_or_tag");
        return false;
    }
    const unsigned reads = bank ? bank->globalReads : readWords >= 0 ? readWords :
        env.rfBroadcast && broadcast && lanes > 1 ? lanes+1 : 2*lanes;
    const bool fmaReads = ((&pool == &sharedFma && kind==OpKind::SharedFma &&
        (caller==OpKind::Mul || (caller==OpKind::SharedFma && env.rhsUpperInverse && lanes==1 && !broadcast))) ||
        (&pool == &sharedLuFma && kind==OpKind::LuFma && caller==OpKind::LuFma)) && !localReads && !localResult && !bank &&
        readWords==static_cast<int>(CfdRhsFma::reads(lanes,env.rfBroadcast && broadcast));
    panic_if((localReads || localResult || (readWords >= 0 && !fmaReads)) &&
             (!env.laneMvm || lanes != 1 || localRow >= N),
             "CFD local arithmetic used outside bounded lane scheduler");
    if (localReads && !lanePorts[localRow].canRead(now,localReads)) {
        observe("lane_local_read_ports");
        return false;
    }
    if (rfEnabled() && reads && !registerPorts.reserveReads(now,reads)) {
        observe("rf_operand_ports");
        return false;
    }
    if (localReads)
        panic_if(!lanePorts[localRow].reserveReads(now,localReads),"CFD local read credit lost");
    if (bank) localVectors.accept(now,*bank);
    unsigned reserved = 0;
    // LU borrows the extra reserved lane first; the original five lanes
    // remain shared. Other callers cannot consume a LU-only added lane.
    for (unsigned i=0;i<limit;++i) {
        auto &next = pool.nextIssue[restricted && lu ? limit-1-i : i];
        if (next <= now && reserved < lanes) {
            next = now + pool.ii;
            reserved++;
        }
    }
    if (env.resourceStats)
        pool.issuedNow += lanes;
    pendingOps.push_back({now + pool.latency, kind, std::move(complete), lanes});
    if (rfEnabled()) {
        auto &op = pendingOps.back();
        op.rfPool = &pool;
        op.rfTag = nextRfTag++;
        op.caller = caller;
        op.rfReadWords = reads;
        op.scalarBroadcast = (&pool == &sharedFma || &pool == &sharedLuFma) ? env.rfBroadcast && broadcast && lanes>1 : bank && bank->rhsFactor ?
            env.rfBroadcast && broadcast && lanes>1 : reads==lanes+1 && lanes>1;
        op.localReads = localReads;
        op.localResult = localResult;
        op.localRow = localRow;
        if (bank) { op.bankAccess=true; op.bank=*bank; }
        pool.rfLive += lanes;
        pool.rfPeak = std::max(pool.rfPeak,pool.rfLive);
        traceRfIssue(op,now,rfPoolName(&pool),rfCapacity(pool));
        // The source witness is needed at admission/trace time only. Inputs
        // are already captured by the existing operation callback/latches.
        op.bank.rhsFactorSource=0;
        op.bank.rhsFactorWords=0;
    }
    traceCluster(kind == OpKind::LuFma ? "lu_fma" : kind == OpKind::SharedFma ? "fma" : kind == OpKind::SharedMul ? "mul" :
                 kind == OpKind::SharedAdd ? "add" :
                 kind == OpKind::FinalSubtract ? "sub" : "div",
                 lanes, caller, now, pool.latency);
    return true;
}

bool
CfdCoeffPreprocessController::issueArithmeticGroup(ResourcePool &pool,
    unsigned width, OpKind caller, bool broadcast, uint64_t now,
    std::function<void()> complete)
{
    const bool scan=static_cast<unsigned>(caller)>=6;
    // Two group entries are reserved for consumers of the recurrence.
    if (arithmeticAcceptedNow >= env.arithmeticWidth ||
        (!scan && !arithmeticGroups.canAdmit(false))) {
        observe("arithmetic_admission");
        return false;
    }
    const int slot=arithmeticGroups.allocate(&pool==&sharedMul,scan,
        static_cast<unsigned>(caller),width,broadcast,now,env.arithmeticSelect,
        diagnosticOwner);
    if (slot < 0) {
        arithmeticFull++;
        observe("arithmetic_groups_full");
        return false;
    }
    arithmeticAcceptedNow++;
    arithmeticCallbacks[slot]=std::move(complete);
    const auto &g=arithmeticGroups.slots[slot];
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_ARITH_GROUP cycle=%llu event=accept id=%llu slot=%d pool=%s caller=%u width=%u scan=%u ready=%llu broadcast=%u\n",
            (unsigned long long)now,(unsigned long long)g.id,slot,
            g.mul ? "mul" : "add",g.caller,g.width,g.scan,
            (unsigned long long)g.ready,g.broadcast);
    return true;
}

void
CfdCoeffPreprocessController::processArithmeticGroups(uint64_t now)
{
    if (env.arithmeticMode == CfdArithmeticGroups::Legacy) return;
    unsigned grants=0;
    for (unsigned slot : arithmeticGroups.readyOrder(now,env.arithmeticMode,env.arithmeticAge)) {
        if (grants >= env.arithmeticWidth) break;
        auto &g=arithmeticGroups.slots[slot];
        DiagnosticOwner owner(diagnosticOwner,g.owner);
        auto &pool=g.mul ? sharedMul : sharedAdd;
        pool.attemptedNow=true;pool.attempts++;
        const auto order=CfdArithmeticGroups::laneOrder(env.arithmeticMode,g.scan,pool.nextIssue.size());
        std::vector<unsigned> lanes;
        for (unsigned i : order)
            if (pool.nextIssue[i]<=now && lanes.size()<g.width-g.issued)
                lanes.push_back(i);
        const unsigned capacity=rfCapacity(pool);
        const bool laneBlocked=lanes.empty();
        if (lanes.size()>capacity-pool.rfLive)
            lanes.resize(capacity-pool.rfLive);
        unsigned width=lanes.size();
        while (width) {
            const unsigned reads=env.rfBroadcast && g.broadcast && width>1 ? width+1 : 2*width;
            if (registerPorts.canRead(now,reads)) break;
            width--;
        }
        if (!width) {
            if (laneBlocked) {
                recordIssueRejection(static_cast<OpKind>(g.caller),g.mul ? 0 : 1);
                pool.rejected++;
                pool.rejectedAfterIssue += pool.issuedNow != 0;
                observe("arithmetic_lanes");
            } else {
                observe(lanes.empty() ? "rf_fu_return_capacity" : "rf_operand_ports");
            }
            continue;
        }
        lanes.resize(width);
        const unsigned reads=env.rfBroadcast && g.broadcast && width>1 ? width+1 : 2*width;
        panic_if(!registerPorts.reserveReads(now,reads),"CFD group lost RF read credits");
        unsigned mask=0;
        for (unsigned lane : lanes) {
            pool.nextIssue[lane]=now+1;mask|=1u<<lane;
            g.crossed |= env.arithmeticMode == CfdArithmeticGroups::Borrow &&
                         ((lane>=N) != g.scan);
        }
        const unsigned offset=g.issued;
        g.issued+=width;
        const uint64_t identity=g.id;
        pendingOps.push_back({now+pool.latency,g.mul ? OpKind::SharedMul : OpKind::SharedAdd,
            [this,slot,offset,width,identity] {
                panic_if(arithmeticGroups.slots[slot].id!=identity,"CFD stale arithmetic fragment");
                arithmeticGroups.returned(slot,offset,width,cycle(),env.arithmeticRoute,env.arithmeticBorrowRoute);
            },width});
        auto &op=pendingOps.back();
        op.rfPool=&pool;op.rfTag=nextRfTag++;op.caller=static_cast<OpKind>(g.caller);op.rfReadWords=reads;
        op.scalarBroadcast=reads==width+1 && width>1;
        pool.rfLive+=width;pool.rfPeak=std::max(pool.rfPeak,pool.rfLive);
        pool.issuedNow+=width;
        traceRfIssue(op,now,g.mul ? "mul" : "add",capacity);
        traceCluster(g.mul ? "mul" : "add",width,op.caller,now,pool.latency);
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_ARITH_GROUP cycle=%llu event=issue id=%llu rf=%llu offset=%u width=%u mask=%u crossed=%u\n",
                (unsigned long long)now,(unsigned long long)g.id,(unsigned long long)op.rfTag,
                offset,width,mask,g.crossed);
        grants++;arithmeticIssued++;
    }
}

void
CfdCoeffPreprocessController::completeArithmeticGroups(uint64_t now)
{
    if (env.arithmeticMode == CfdArithmeticGroups::Legacy) return;
    for (unsigned slot=0;slot<arithmeticGroups.depth;++slot) {
        if (!arithmeticGroups.publishable(slot,now)) continue;
        const auto g=arithmeticGroups.slots[slot];
        auto callback=std::move(arithmeticCallbacks[slot]);
        arithmeticGroups.release(slot);
        arithmeticCompleted++;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_ARITH_GROUP cycle=%llu event=complete id=%llu slot=%u crossed=%u\n",
                (unsigned long long)now,(unsigned long long)g.id,slot,g.crossed);
        callback();
    }
}

void
CfdCoeffPreprocessController::traceCluster(
    const char *pool, unsigned lanes, OpKind caller,
    uint64_t now, uint64_t latency)
{
    if (!env.traceEnable)
        return;
    if (!clusterTraceFp) {
        const std::string path = std::string(env.traceFile) + ".cluster.csv";
        clusterTraceFp = std::fopen(path.c_str(), "w");
        panic_if(!clusterTraceFp, "CFD cannot open shared cluster trace");
        std::fprintf(clusterTraceFp, "cycle,pool,lanes,caller,complete_cycle\n");
    }
    std::fprintf(clusterTraceFp, "%llu,%s,%u,%u,%llu\n",
        (unsigned long long)now, pool, lanes, static_cast<unsigned>(caller),
        (unsigned long long)(now + latency));
    std::fflush(clusterTraceFp);
}

bool
CfdCoeffPreprocessController::issueColumnMvm(
    const std::array<double, N> &column, double scalar, uint64_t now,
    std::function<void(const std::array<double, N> &)> done)
{
    auto multiply = [column, scalar] {
        std::array<double, N> product;
        for (unsigned row = 0; row < N; ++row) {
            volatile double rounded = column[row] * scalar;
            product[row] = rounded;
        }
        return product;
    };
    if (!env.sharedCluster)
        return issue(columnFma, OpKind::ColumnFma, now,
            [multiply, done] { done(multiply()); });
    if (sharedVectorsLive >= env.clusterProductDepth) {
        sharedProductStalls++;
        return false;
    }
    if (!issueLanes(sharedMul, OpKind::SharedMul, now, N, OpKind::ColumnFma,
        [this, multiply, done] {
            sharedVectorAdds.push_back({multiply(), done});
        }, true))
        return false;
    sharedVectorsLive++;
    sharedVectorsIssued++;
    sharedVectorsMax = std::max(sharedVectorsMax, sharedVectorsLive);
    return true;
}

void
CfdCoeffPreprocessController::processSharedVectorAdds(uint64_t now)
{
    if (!env.sharedCluster || sharedVectorAdds.empty())
        return;
    const auto task = sharedVectorAdds.front();
    if (!issueLanes(sharedAdd, OpKind::SharedAdd, now, N, OpKind::ColumnFma,
        [this, task] {
            panic_if(!sharedVectorsLive, "CFD shared product credit underflow");
            sharedVectorsLive--;
            sharedVectorsCompleted++;
            task.complete(task.product);
        }))
        return;
    sharedVectorAdds.pop_front();
}

void
CfdCoeffPreprocessController::traceRfIssue(const PendingOp &op,uint64_t now,
                                         const char *pool,unsigned capacity)
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_RF_ISSUE cycle=%llu id=%llu pool=%s lanes=%u caller=%u "
        "read_words=%u ready=%llu capacity=%u broadcast=%u local_reads=%u local_result=%u row=%u "
        "bank_access=%u bank_a=%d bank_b=%d bank_dst=%d bank_words_a=%u bank_words_b=%u bank_owner=%llu bank_epoch=%llu bank_route=%u pair_id=%llu pair_side=%u factor_id=%llu factor_source=%llu factor_words=%u\n",(unsigned long long)now,
        (unsigned long long)op.rfTag,pool,op.lanes,static_cast<unsigned>(op.caller),
        op.rfReadWords,(unsigned long long)op.completeCycle,capacity,
        op.scalarBroadcast,
        op.localReads,op.localResult,op.localRow,op.bankAccess,op.bank.a,op.bank.b,
        op.bank.destination,op.bank.wordsA,op.bank.wordsB,
        (unsigned long long)op.bank.owner,(unsigned long long)op.bank.epoch,
        op.bankAccess && op.bank.destination>=0 ? env.localRouteLatency : 0,
        (unsigned long long)op.bank.rhsPair,op.bank.pairSide,
        (unsigned long long)op.bank.rhsFactor,
        (unsigned long long)op.bank.rhsFactorSource,op.bank.rhsFactorWords);
    if (env.round1Diagnostics || op.caller==OpKind::LuFma || ((env.luControlFold || env.luRank1) &&
        static_cast<unsigned>(op.caller)<=static_cast<unsigned>(OpKind::LuSub)))
        std::fprintf(stderr,"CFD_RF_OWNER cycle=%llu id=%llu owner=%llu frontier=%llu\n",
            (unsigned long long)now,(unsigned long long)op.rfTag,
            (unsigned long long)diagnosticOwner,
            (unsigned long long)diagnosticFrontier);
}

bool
CfdCoeffPreprocessController::issueMvmVector(LineRequest &line,unsigned context,
    int node,bool multiply,bool feedback,uint64_t now,OpKind caller,
    std::function<void()> complete)
{
    auto &grants=multiply ? vectorMulGrants : vectorAddGrants;
    if (grants>=env.vectorIssueWidth) {
        observe("vector_selection_width");
        return false;
    }
    CfdLocalAccess access;
    if (env.vectorLocal) {
        access=cfdMvmAccess(context,node,multiply,feedback,env.vectorProductOnly,
            env.rfBroadcast,line.token,line.mvm[context].epoch);
    }
    const bool accepted=issueLanes(multiply ? sharedMul : sharedAdd,
        multiply ? OpKind::SharedMul : OpKind::SharedAdd,now,N,caller,
        std::move(complete),multiply,-1,0,false,0,env.vectorLocal ? &access : nullptr);
    if (accepted) {
        ++grants;
        if (env.round1Diagnostics)
            std::fprintf(stderr,"CFD_MVM_BIND cycle=%llu id=%llu token=%llu context=%u epoch=%llu cell=%llu multiply=%u feedback=%u node=%d\n",
                (unsigned long long)now,(unsigned long long)(nextRfTag-1),
                (unsigned long long)line.token,context,
                (unsigned long long)line.mvm[context].epoch,
                (unsigned long long)(context==1 ? line.backwardCell :
                    context==2 ? line.baseCell : line.cachedForwardCell),
                multiply,feedback,node);
    }
    return accepted;
}

void
CfdCoeffPreprocessController::tracePreprocessBroadcast(
    const Request &req,const char *family,const char *phase,int k,int column,
    int rhs,unsigned width,double factor)
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_PRE_BROADCAST cycle=%llu id=%llu request=%llu generation=%llu cell=%llu family=%s phase=%s k=%d column=%d rhs=%d lanes=%u factor=%a\n",
        (unsigned long long)cycle(),(unsigned long long)(nextRfTag-1),
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.generation,
        (unsigned long long)req.desc.cellId,family,phase,k,column,rhs,width,factor);
}
