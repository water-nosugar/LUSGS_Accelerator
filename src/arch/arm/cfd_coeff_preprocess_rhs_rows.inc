// Independent forward row updates use spare scalar lanes. Backward products
// are generated together, but subtractions retain their original j order.
// Selected direct RHS may reduce (b-p1)-(p2+p3), then subtract p4. The two first
// operations use spare lanes in the SAME scalar adder bank. Partial sums
// reuse rhs.product and a consumed product word. Optional RF temporaries
// retain one separately budgeted scalar per slot through paid RF writes.
void
CfdCoeffPreprocessController::traceRhsTreeTemp(const Request &req,
    unsigned index, int k, size_t slot, const char *event, uint64_t rf, double value)
{
    if (!env.traceEnable || !env.rhsTreeRfTemp) return;
    std::fprintf(stderr,"CFD_RHS_TREE_RF_TEMP cycle=%llu event=%s rf=%llu token=%llu generation=%llu request=%llu cell=%llu rhs=%u k=%d slot=%zu value=%a\n",
        (unsigned long long)cycle(),event,(unsigned long long)rf,
        (unsigned long long)req.token,(unsigned long long)req.desc.generation,
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
        index,k,slot,value);
}

bool
CfdCoeffPreprocessController::issueRhsTree(
    Request &req,unsigned index,size_t slotIndex,uint64_t now)
{
    auto &rhs = req.rhs[index];
    auto &slot = rhsRowProducts[slotIndex];
    const int k = rhs.k;
    const unsigned terms = N-k-1;
    const unsigned nodes = terms;
    const uint64_t token = req.token, generation = req.desc.generation;
    bool issued = false;
    for (unsigned node=0;node<nodes;++node) {
        const uint8_t bit = 1u << node;
        if (rhs.treeIssued & bit) continue;
        if (!(readyRhsTreeNodes(rhs) & bit)) continue;
        const bool add = node == 1 && terms>=3;
        const bool rfSlot = env.rhsTreeRfTemp &&
            (!env.rhsTreeRfFirstColumn || index == N);
        const bool rfWrite = add && rfSlot;
        const bool rfRead = node == 2 && terms>=3 && rfSlot;
        panic_if(rfRead && !slot.treePartialValid,"CFD RHS tree RF temporary not published");
        const double left = node == 0 ? rhs.value[k] :
            add ? slot.value[k+2] : rhs.product;
        const double right = node == 0 ? slot.value[k+1] :
            add ? slot.value[k+3] : node == 1 ? slot.value[k+2] :
            node == 2 ? (rfRead ? slot.treePartial : slot.value[k+2]) : slot.value[k+4];
        CfdLocalAccess access;
        if (env.localProducts) {
            // Tree internal temporaries are still paid global RHS state.
            // Product-slot operands are one vector-bank read transaction.
            access.owner=token; access.epoch=generation;
            access.a=27+env.luCount+slotIndex;
            if (add && !rfWrite) access.destination=access.a;
            access.wordsA=add ? 2 : 1;
            access.globalReads=add ? 0 : 1;
            if (rfRead) {
                access.a=-1;access.wordsA=0;access.globalReads=2;
            }
        }
        const uint64_t rfTag=nextRfTag;
        if (!issueLanes(sharedAdd,OpKind::SharedAdd,now,1,OpKind::Sub,
            [this,token,generation,index,k,slotIndex,node,nodes,add,rfWrite,rfTag,left,right] {
                auto *r = find(token);
                if (!r || r->stage == RequestStage::Failed || r->desc.generation != generation) return;
                auto &s = r->rhs[index];
                auto &p = rhsRowProducts[slotIndex];
                panic_if(!p.valid || p.token != token || p.rhs != index || s.k != k,
                         "CFD stale RHS tree register");
                volatile double value = add ? left+right : left-right;
                if (rfWrite) {
                    panic_if(p.treePartialValid,"CFD RHS tree RF temporary overwritten");
                    p.treePartial=value;p.treePartialValid=true;
                    ++rhsTreeTempLive;rhsTreeTempPeak=std::max(rhsTreeTempPeak,rhsTreeTempLive);
                    panic_if(rhsTreeTempLive>env.rhsProductDepth,"CFD RHS RF temporary capacity exceeded");
                    traceRhsTreeTemp(*r,index,k,slotIndex,"write",rfTag,value);
                } else if (add) p.value[k+2] = value;
                else s.product = value;
                s.treeReady |= 1u << node;
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_RHS_TREE cycle=%llu request=%llu generation=%llu cell=%llu rhs=%u k=%d node=%u event=complete\n",
                        (unsigned long long)cycle(),(unsigned long long)r->record.requestId,
                        (unsigned long long)generation,(unsigned long long)r->desc.cellId,index,k,node);
                if (node+1 == nodes) {
                    s.value[k] = value;
                    releaseRhsRows(p);
                    s.treeIssued = s.treeReady = 0;
                    if (k == 0) s.phase = RhsPhase::Done;
                    else {
                        s.k--;
                        s.i = s.k+1;
                        s.phase = RhsPhase::BackwardMul;
                    }
                }
            },false,-1,0,false,0,env.localProducts ? &access : nullptr)) break;
        if (env.rhsTreeRfTemp)
            panic_if(nextRfTag!=rfTag+1,"CFD RHS tree RF temporary lost its physical tag");
        if (rfRead) {
            panic_if(!rhsTreeTempLive,"CFD RHS tree RF temporary live underflow");
            slot.treePartialValid=false;--rhsTreeTempLive;
            traceRhsTreeTemp(req,index,k,slotIndex,"read",rfTag,right);
        }
        rhs.treeIssued |= bit;
        req.record.coeffSubIssued++;
        issued = true;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_RHS_TREE cycle=%llu request=%llu generation=%llu cell=%llu rhs=%u k=%d node=%u event=issue\n",
                (unsigned long long)now,(unsigned long long)req.record.requestId,
                (unsigned long long)generation,(unsigned long long)req.desc.cellId,index,k,node);
    }
    return issued;
}

void
CfdCoeffPreprocessController::releaseRhsRows(RhsRowProduct &slot)
{
    if (!slot.valid) return;
    if (slot.treePartialValid) {
        auto *req=find(slot.token);
        panic_if(!env.rhsTreeRfTemp || !req || !rhsTreeTempLive,
                 "CFD unowned RHS tree RF temporary");
        traceRhsTreeTemp(*req,slot.rhs,req->rhs[slot.rhs].k,
            &slot-rhsRowProducts.data(),"clear",0,slot.treePartial);
        --rhsTreeTempLive;slot.treePartialValid=false;
    }
    slot.valid = false;
    rowGroupsCompleted++;
}

void
CfdCoeffPreprocessController::discardRhsRows(Request &req)
{
    cancelRhsPairs(req);
    for (auto &slot : rhsRowProducts)
        if (slot.valid && slot.token == req.token && !req.rhs[slot.rhs].waiting)
            releaseRhsRows(slot);
}

void
CfdCoeffPreprocessController::traceRhsFactor(const Request &req,unsigned slot,
    const char *event,uint64_t now,uint64_t operation,unsigned rhs)
{
    if (!env.traceEnable) return;
    const auto &s=rhsFactors.slots.at(slot);
    std::fprintf(stderr,"CFD_RHS_FACTOR cycle=%llu event=%s cache=%llu slot=%u bank=%d token=%llu generation=%llu request=%llu cell=%llu backward=%u k=%u width=%u rhs=%u id=%llu v0=%a v1=%a v2=%a v3=%a v4=%a\n",
        (unsigned long long)now,event,(unsigned long long)s.id,slot,rhsFactorBank(slot),
        (unsigned long long)s.token,(unsigned long long)s.epoch,
        (unsigned long long)req.record.requestId,(unsigned long long)req.desc.cellId,
        s.backward,s.k,N-s.k-1,rhs,(unsigned long long)operation,
        s.value[0],s.value[1],s.value[2],s.value[3],s.value[4]);
}

void
CfdCoeffPreprocessController::traceRhsFactorFanout(const Request &req,
    const char *event, const CfdRhsFactorFanout::Transfer &s, uint64_t now,
    uint64_t operation, unsigned rhs)
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_RHS_FACTOR_FANOUT cycle=%llu event=%s source=%llu id=%llu token=%llu generation=%llu request=%llu cell=%llu slot=%u bank=%u cache=%llu k=%u backward=%u width=%u rhs=%u v0=%a v1=%a v2=%a v3=%a v4=%a\n",
        (unsigned long long)now,event,(unsigned long long)s.sourceRf,
        (unsigned long long)operation,(unsigned long long)s.token,
        (unsigned long long)s.epoch,(unsigned long long)req.record.requestId,
        (unsigned long long)req.desc.cellId,s.slot,s.bank,(unsigned long long)s.cache,
        s.k,s.backward,s.width,rhs,s.value[0],s.value[1],s.value[2],s.value[3],s.value[4]);
}

void
CfdCoeffPreprocessController::clearRhsFactors(Request &req,uint64_t now)
{
    if (!env.rhsFactorReuse) return;
    for (unsigned i=0;i<env.rhsFactorSlots;++i) {
        const auto &s=rhsFactors.slots[i];
        if (s.id && s.token==req.token && s.epoch==req.desc.generation) {
            traceRhsFactor(req,i,"clear",now,0,0);
            if (env.rhsFactorFanout) rhsFactorFanout.invalidateSlot(i,s.id);
            rhsFactors.clear(i,s.id);
        }
    }
}

bool
CfdCoeffPreprocessController::issueSolveRows(Request &req, unsigned index, uint64_t now,
                                            int pairSlot,unsigned pairSide)
{
    auto &rhs = req.rhs[index];
    const int k = rhs.k;
    const bool multiply = rhs.phase == RhsPhase::ForwardMul || rhs.phase == RhsPhase::BackwardMul;
    const bool backward = rhs.phase == RhsPhase::BackwardMul || rhs.phase == RhsPhase::BackwardSub;
    const uint64_t token = req.token;
    const auto end = rhsRowProducts.begin()+env.rhsProductDepth;
    panic_if(k < 0 || k >= 4, "CFD invalid RHS row batch");
    auto it = multiply ?
        std::find_if(rhsRowProducts.begin(), end, [](const auto &s) { return !s.valid; }) :
        std::find_if(rhsRowProducts.begin(), end, [token,index](const auto &s) {
            return s.valid && s.token == token && s.rhs == index;
        });
    if (it == end) {
        panic_if(!multiply, "CFD RHS row product missing");
        rowGroupsStalls++;
        return false;
    }
    const size_t slotIndex = it-rhsRowProducts.begin();
    if (!multiply && backward && rhsTreeEnabled(req,index))
        return issueRhsTree(req,index,slotIndex,now);
    const unsigned width = multiply || !backward ? N-k-1 : 1;
    const auto *pair=pairSlot>=0 ? &rhsPairs.slots.at(pairSlot) : nullptr;
    panic_if(pair && (!pair->issued || !multiply || backward || pairSide>=2 ||
             pair->token!=token || pair->epoch!=req.desc.generation ||
             pair->k!=static_cast<unsigned>(k) || pair->rhs[pairSide]!=index),
             "CFD stale common-LU operand binding");
    const uint64_t pairId=pair ? pair->id : 0;
    const bool broadcast = (env.preprocessBroadcast || pair) && multiply && !backward;
    const bool retain=env.rhsFactorReuse && multiply && !req.coefficientCacheReuse;
    int factorSlot=retain ? rhsFactors.lookup(token,req.desc.generation,backward,k,now) : -1;
    const CfdRhsFactorFanout::Transfer *fanout=nullptr;
    // A latch hit must not serialize otherwise independent RHS operations
    // behind its single read port. The normal paid RF path remains available.
    if (factorSlot>=0 && !localVectors.banks.at(rhsFactorBank(factorSlot)).canRead(now,1)) {
        // Ubar may still offer a paid read to the bus. In feedback-only
        // mode it cannot consume the second receiver and pull background
        // operations ahead of the next b/Lbar dependency.
        if (env.rhsFactorFanout && (!env.rhsFactorFanoutFeedback || index<2*N))
            fanout=rhsFactorFanout.probe(rhsFactors.slots[factorSlot],factorSlot,
                rhsFactorBank(factorSlot),now,index,width);
        if (!fanout) factorSlot=-1;
    }
    const auto *retained=factorSlot>=0 ? &rhsFactors.slots[factorSlot] : nullptr;
    const uint64_t factorId=retained ? retained->id : 0;
    const uint64_t fanoutSource=fanout ? fanout->sourceRf : 0;
    const auto *factorValues=fanout ? &fanout->value : retained ? &retained->value : nullptr;
    const double factor = broadcast ? (pair ? pair->factor[pairSide] : rhs.value[k]) : 0;
    std::array<double,N> operands{};
    if (broadcast)
        for (unsigned row=k+1;row<N;++row)
            operands[row]=pair ? pair->lu[row] : factorValues ? (*factorValues)[row] : req.lu[row*N+k];
    CfdLocalAccess access;
    if (env.localProducts) {
        access.owner=token; access.epoch=req.desc.generation;
        const int bank=27+env.luCount+slotIndex;
        if (multiply) access.destination=bank;
        else { access.a=bank; access.wordsA=width; }
        access.globalReads=multiply ? (broadcast && width>1 ? width+1 : 2*width) : width;
        if (pair) { access.globalReads=0;access.rhsPair=pairId;access.pairSide=pairSide; }
        if (retained) {
            if (fanout) {
                access.rhsFactorSource=fanoutSource;
                access.rhsFactorWords=width;
            } else {
                access.a=rhsFactorBank(factorSlot);access.wordsA=width;
            }
            access.globalReads-=width;access.rhsFactor=factorId;
        }
    }
    std::array<double,N> leftInputs{},rightInputs{};
    for (unsigned row=k+1;row<N;++row) {
        leftInputs[row]=multiply ? (factorValues ? (*factorValues)[row] : backward ? req.lu[k*N+row] : req.lu[row*N+k]) : rhs.value[row];
        rightInputs[row]=multiply ? (backward ? rhs.value[row] : rhs.value[k]) : it->value[row];
    }
    const double subtractLeft=rhs.value[k], subtractRight=!multiply && backward ? it->value[rhs.i] : 0;
    rhs.waiting = true;
    const bool accepted = issueLanes(multiply ? sharedMul : sharedAdd,
        multiply ? OpKind::SharedMul : OpKind::SharedAdd, now, width,
        multiply ? OpKind::Mul : OpKind::Sub,
        [this,token,index,k,slotIndex,multiply,backward,broadcast,factor,operands,leftInputs,rightInputs,subtractLeft,subtractRight] {
            auto &slot = rhsRowProducts[slotIndex];
            panic_if(!slot.valid || slot.token != token || slot.rhs != index,
                     "CFD stale RHS row buffer");
            Request *r = find(token);
            if (!r || r->stage == RequestStage::Failed) {
                releaseRhsRows(slot);
                return;
            }
            auto &s = r->rhs[index];
            if (multiply) {
                for (unsigned row = k+1; row < N; ++row) {
                    volatile double value = broadcast ? operands[row]*factor :
                        leftInputs[row]*rightInputs[row];
                    slot.value[row] = value;
                }
                s.phase = backward ? RhsPhase::BackwardSub : RhsPhase::ForwardSub;
                s.treeIssued = s.treeReady = 0;
                if (backward && rhsTreeEnabled(*r,index) && env.traceEnable)
                    std::fprintf(stderr,"CFD_RHS_TREE cycle=%llu request=%llu generation=%llu cell=%llu rhs=%u k=%d node=-1 event=products_ready\n",
                        (unsigned long long)cycle(),(unsigned long long)r->record.requestId,
                        (unsigned long long)r->desc.generation,(unsigned long long)r->desc.cellId,index,k);
            } else if (!backward) {
                for (unsigned row = k+1; row < N; ++row) {
                    volatile double value = leftInputs[row]-rightInputs[row];
                    s.value[row] = value;
                }
                releaseRhsRows(slot);
                s.k++;
                s.phase = s.k == 4 ? RhsPhase::LastDiv : RhsPhase::ForwardDiv;
            } else {
                volatile double value = subtractLeft-subtractRight;
                s.value[k] = value;
                s.i++;
                if (s.i < 5) {
                    s.phase = RhsPhase::BackwardSub;
                } else {
                    releaseRhsRows(slot);
                    if (s.k == 0) s.phase = RhsPhase::Done;
                    else {
                        s.k--;
                        s.i = s.k+1;
                        s.phase = RhsPhase::BackwardMul;
                    }
                }
            }
            s.waiting = false;
        },broadcast,-1,0,false,0,env.localProducts ? &access : nullptr);
    if (!accepted) {
        rhs.waiting = false;
        req.record.stallCoeffMulSub++;
        return false;
    }
    if (multiply) {
        if (retained) {
            if (fanout) {
                const auto consumed=rhsFactorFanout.commit(*retained,factorSlot,
                    rhsFactorBank(factorSlot),now,index,width,fanoutSource,nextRfTag-1);
                traceRhsFactorFanout(req,"use",consumed,now,nextRfTag-1,index);
            } else {
                rhsFactors.capture(factorSlot,factorId,now);
                traceRhsFactor(req,factorSlot,"use",now,nextRfTag-1,index);
                if (env.rhsFactorFanout && rhsFactorFanout.offer(*retained,factorSlot,
                    rhsFactorBank(factorSlot),localVectors.banks.at(rhsFactorBank(factorSlot)),
                    now,nextRfTag-1,index))
                    traceRhsFactorFanout(req,"offer",rhsFactorFanout.source(now),
                        now,nextRfTag-1,index);
            }
        } else if (retain) {
            // Capture the coefficients already read for this accepted MUL.
            // No speculative load and no miss wait; a registered copy is
            // visible next cycle through one paid local vector read port.
            const int fill=rhsFactors.victim(token,req.desc.generation,backward,k,now);
            if (fill>=0 && localVectors.write(now,rhsFactorBank(fill))) {
                rhsFactors.fill(fill,token,req.desc.generation,backward,k,now,leftInputs);
                traceRhsFactor(req,fill,"fill",now,nextRfTag-1,index);
            }
        }
        if (env.preprocessBroadcast && broadcast && width>1)
            tracePreprocessBroadcast(req,"rhs","forward",k,-1,index,width,factor);
        it->valid = true;
        panic_if(it->treePartialValid,"CFD new product overwrote RHS RF temporary");
        it->token = token;
        it->rhs = index;
        rowGroupsIssued++;
        req.record.coeffMulIssued += width;
        if (backward && rhsTreeEnabled(req,index) && env.traceEnable)
            std::fprintf(stderr,"CFD_RHS_TREE cycle=%llu request=%llu generation=%llu cell=%llu rhs=%u k=%d node=-1 event=products_issue\n",
                (unsigned long long)now,(unsigned long long)req.record.requestId,
                (unsigned long long)req.desc.generation,(unsigned long long)req.desc.cellId,index,k);
        rowGroupsPeak = std::max<uint64_t>(rowGroupsPeak,
            std::count_if(rhsRowProducts.begin(), rhsRowProducts.end(), [](const auto &s) { return s.valid; }));
    } else {
        req.record.coeffSubIssued += width;
    }
    return true;
}

void
CfdCoeffPreprocessController::traceRhsPair(const CfdRhsPairs::Slot &s,
    unsigned slot,const char *event,uint64_t now) const
{
    if (!env.traceEnable) return;
    std::fprintf(stderr,"CFD_RHS_PAIR cycle=%llu event=%s id=%llu slot=%u token=%llu request=%llu generation=%llu cell=%llu k=%u rhs0=%u rhs1=%u width=%u selected=%llu loaded=%llu read_words=%u lu0=%a lu1=%a lu2=%a lu3=%a lu4=%a factor0=%a factor1=%a\n",
        (unsigned long long)now,event,(unsigned long long)s.id,slot,
        (unsigned long long)s.token,(unsigned long long)s.request,
        (unsigned long long)s.epoch,(unsigned long long)s.cell,s.k,s.rhs[0],s.rhs[1],N-s.k-1,
        (unsigned long long)s.selected,(unsigned long long)s.loadedCycle,
        std::strcmp(event,"load")==0 ? N-s.k+1 : 0,
        s.lu[0],s.lu[1],s.lu[2],s.lu[3],s.lu[4],s.factor[0],s.factor[1]);
}

bool
CfdCoeffPreprocessController::selectRhsPair(Request &req,unsigned index,uint64_t now)
{
    // Five fixed Ubar columns only: no critical b/Lbar enters this queue, and
    // an unpaired RHS always falls back to the existing issue path immediately.
    auto &a=req.rhs[index];
    if (!req.directBaseSolve || index<10 || index>=14 || !a.active || a.done ||
        a.waiting || a.phase!=RhsPhase::ForwardMul || a.k>2) return false;
    for (unsigned other=index+1;other<15;++other) {
        auto &b=req.rhs[other];
        if (!b.active || b.done || b.waiting || b.phase!=a.phase || b.k!=a.k ||
            !req.forwardStepReady[a.k]) continue;
        const int slot=rhsPairs.select(req.token,req.desc.generation,
            req.record.requestId,req.desc.cellId,index,other,a.k,now);
        if (slot<0) return false;
        a.waiting=b.waiting=true;
        traceRhsPair(rhsPairs.slots[slot],slot,"select",now);
        return true;
    }
    return false;
}

void
CfdCoeffPreprocessController::cancelRhsPairs(Request &req)
{
    releaseRhsUpper(req,true);
    if (env.luRank1) cancelLuRank1(req);
    clearRhsFactors(req,cycle());
    if (env.luUpperOverlap) traceLuUpper(req,"cancel");
    if (env.luControlFold && !env.luRank1 && env.traceEnable)
        std::fprintf(stderr,"CFD_LU_CONTROL cycle=%llu event=cancel token=%llu generation=%llu request=%llu cell=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)req.token,
            (unsigned long long)req.desc.generation,(unsigned long long)req.record.requestId,
            (unsigned long long)req.desc.cellId);
    if (env.luProductAhead && env.traceEnable)
        std::fprintf(stderr,"CFD_LU_PRODUCTS_CANCEL cycle=%llu token=%llu generation=%llu\n",
            (unsigned long long)cycle(),(unsigned long long)req.token,
            (unsigned long long)req.desc.generation);
    if (!env.rhsPair) return;
    for (unsigned i=0;i<CfdRhsPairs::Depth;++i) {
        const auto &s=rhsPairs.slots[i];
        if (!s.id || s.token!=req.token || s.issued) continue;
        traceRhsPair(s,i,"cancel",cycle());
        for (unsigned rhs : s.rhs) req.rhs[rhs].waiting=false;
        rhsPairs.cancel(i);
    }
}

void
CfdCoeffPreprocessController::processRhsPairs(uint64_t now,
    std::unordered_map<uint64_t,bool> &selected)
{
    for (unsigned i=0;i<CfdRhsPairs::Depth;++i) {
        auto &s=rhsPairs.slots[i];
        if (!s.id || s.issued) continue;
        auto *req=find(s.token);
        if (!req || req->stage==RequestStage::Failed || req->desc.generation!=s.epoch) {
            if (req) for (unsigned rhs : s.rhs) req->rhs[rhs].waiting=false;
            traceRhsPair(s,i,"cancel",now);rhsPairs.cancel(i);continue;
        }
        const unsigned width=N-s.k-1;
        if (rhsPairs.canLoad(i,now)) {
            // One RF vector plus two different RHS scalars. Capture operands
            // into paid registers; the MUL input fanout is available NEXT tick.
            if (!registerPorts.reserveReads(now,width+2)) {
                observe("rhs_pair_rf_ingress");continue;
            }
            std::array<double,N> lu{};
            for (unsigned row=s.k+1;row<N;++row) lu[row]=req->lu[row*N+s.k];
            rhsPairs.load(i,now,lu,{req->rhs[s.rhs[0]].value[s.k],
                                   req->rhs[s.rhs[1]].value[s.k]});
            traceRhsPair(s,i,"load",now);
        }
        if (!rhsPairs.ready(i,now)) continue;
        const unsigned lanes=2*width;
        const unsigned available=std::count_if(sharedMul.nextIssue.begin(),
            sharedMul.nextIssue.end(),[now](uint64_t next) { return next<=now; });
        const unsigned products=std::count_if(rhsRowProducts.begin(),
            rhsRowProducts.begin()+env.rhsProductDepth,[](const auto &p) { return !p.valid; });
        if (available<lanes || sharedMul.rfLive+lanes>rfCapacity(sharedMul) ||
            localVectors.live+2>CfdLocalVectorPorts::Tags || products<2) {
            observe("rhs_pair_mul_return_or_product_capacity");continue;
        }
        rhsPairs.issue(i,now);
        traceRhsPair(s,i,"issue",now);
        const auto captured=s;
        for (unsigned side=0;side<2;++side) {
            req->rhs[s.rhs[side]].waiting=false;
            panic_if(!issueSolveRhs(*req,s.rhs[side],now,i,side),
                     "CFD common-LU pair lost atomic MUL/product credits");
        }
        // issueSolveRows captured the common coefficients and RHS scalar
        // separately into each existing, credit-reserved MUL pipeline entry.
        // Recycling this ingress latch releases no FU/result/product credit.
        panic_if(rhsPairs.capture(i,captured.id,0) ||
                 !rhsPairs.capture(i,captured.id,1),"CFD paired input capture lost");
        traceRhsPair(captured,i,"release",now);
        selected[req->token]=true;
        observe("rhs_frontier_background_issue");
    }
}
