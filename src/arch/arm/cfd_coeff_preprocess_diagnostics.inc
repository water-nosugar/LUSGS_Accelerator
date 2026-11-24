void
CfdCoeffPreprocessController::traceRhsReadyGroups(const Request &req,
    const std::array<unsigned, 15> &order, unsigned count, uint64_t now)
{
    if (!env.traceEnable || req.desc.cellId >= env.traceCells) return;
    // Snapshot only: each mask contains already-ready RHS before this request
    // consumes any issue credits. No RHS is delayed to form a pair. Groups
    // from different requests/steps must never be treated as sharing factors.
    std::array<uint16_t, 8> masks{}, baseMasks{};
    for (unsigned p=0; p<count; ++p) {
        const unsigned index=order[p];
        const auto &rhs=req.rhs[index];
        const bool backward=rhs.phase==RhsPhase::BackwardMul;
        if (rhs.phase!=RhsPhase::ForwardMul && !backward) continue;
        const unsigned group=(backward ? 4 : 0)+rhs.k;
        masks[group] |= 1u<<index;
        if (rhsBatch(req,index)==0) baseMasks[group] |= 1u<<index;
    }
    const unsigned mulFree=std::count_if(sharedMul.nextIssue.begin(),
        sharedMul.nextIssue.end(),[now](uint64_t next) { return next<=now; });
    const unsigned productFree=std::count_if(rhsRowProducts.begin(),
        rhsRowProducts.begin()+env.rhsProductDepth,
        [](const auto &s) { return !s.valid; });
    const auto readWidth=registerPorts.configuration().readWordsPerCycle;
    const unsigned readFree=readWidth-registerPorts.readsUsed();
    for (unsigned group=0; group<masks.size(); ++group) {
        if (!masks[group]) continue;
        std::fprintf(stderr,"CFD_RHS_READY cycle=%llu token=%llu generation=%llu cell=%llu phase=%s k=%u width=%u mask=%u base_mask=%u frontier=%u mul_free=%u read_free=%u product_free=%u return_free=%u local_tags_free=%u preprocess_broadcast=%u local_products=%u\n",
            (unsigned long long)now,(unsigned long long)req.token,
            (unsigned long long)req.desc.generation,
            (unsigned long long)req.desc.cellId,group<4 ? "forward" : "backward",
            group%4,N-group%4-1,masks[group],baseMasks[group],
            req.token==diagnosticFrontier,mulFree,readFree,productFree,
            static_cast<unsigned>(rfCapacity(sharedMul)-sharedMul.rfLive),
            CfdLocalVectorPorts::Tags-localVectors.live,
            env.preprocessBroadcast,env.localProducts);
    }
}

void
CfdCoeffPreprocessController::recordIssueRejection(OpKind caller, unsigned resource)
{
    const unsigned index = static_cast<unsigned>(caller);
    if (!env.resourceStats || index >= 10) return;
    rejectThisCycle[index] |= 1u << resource;
    if (diagnosticOwner && diagnosticOwner == diagnosticFrontier)
        criticalRejectThisCycle[index] |= 1u << resource;
}

void
CfdCoeffPreprocessController::sampleFrontierState()
{
    // One exclusive end-of-tick state per sample, not an additive attribution
    // of critical-path latency. FU rejection unions are reported separately.
    const char *state = "admission_or_interface";
    auto *req = diagnosticFrontier ? find(diagnosticFrontier) : nullptr;
    if (req) {
        if (req->handoffProduced) state = "forward_progress";
        else if (req->stage == RequestStage::WaitingSlot ||
                 req->stage == RequestStage::InputFetching ||
                 (splitInput(*req) && !req->inputOperandsReady))
            state = "input_dma_spm";
        else if (!req->solveStarted) state = "lu_or_solve_context";
        else if (!req->baseComputeComplete) {
            state = "rhs_b_compute";
            for (unsigned index=0;index<req->rhsSlots;++index) {
                const auto &rhs = req->rhs[index];
                if (rhsBatch(*req,index) != 0 || !rhs.active || rhs.done || rhs.waiting)
                    continue;
                const bool forward = rhs.phase == RhsPhase::ForwardDiv ||
                    rhs.phase == RhsPhase::ForwardMul || rhs.phase == RhsPhase::LastDiv;
                if ((forward && !req->forwardStepReady[rhs.k]) ||
                    (rhs.phase == RhsPhase::BackwardMul && !req->backwardStepReady[rhs.k])) {
                    state = "rhs_b_lu_factor";
                    break;
                }
            }
        } else if (req->desc.cellId && !req->previousDqReady)
            state = "neighbor_feedback";
        else if (!req->correctionComplete && req->desc.cellId)
            state = "lbar_correction";
        else if (!req->dqStarReadyLocal) state = "forward_combine_or_publish";
        else state = "commit";
    } else {
        for (const auto &[token,line] : lineRequests) {
            if (line->backwardStarted && !line->terminal) {
                state = line->backwardCell < 0 ? "final_visibility" : "backward_feedback";
                if (env.smeContexts && line->backwardCell >= 0) {
                    const auto cell = line->backwardCell;
                    const auto &operand = line->backwardOperands[cell%env.backwardOperandDepth];
                    if (line->outputWrites.size() >= env.lineOutputDepth)
                        state = "backward_output_queue";
                    else if (!backwardTileBound(*line->tile, cell))
                        state = "backward_tile";
                    else if (operand.cell != uint64_t(cell) || !operand.yReady ||
                             (line->deferUpper ? !operand.smePackedReady :
                              operand.readyColumns != 0x1f))
                        state = "backward_spm";
                    else if (line->deferUpper && operand.readyColumns != 0x1f)
                        state = "backward_upper";
                }
                break;
            }
        }
    }
    frontierSamples++;
    frontierStates[state]++;
}

void
CfdCoeffPreprocessController::beginResourceStats(uint64_t now)
{
    if (!env.resourceStats)
        return;
    diagnosticFrontier = 0;
    for (const auto *req : admissionOrder()) {
        if (!req->handoffProduced && req->stage != RequestStage::Complete &&
            req->stage != RequestStage::Failed) {
            diagnosticFrontier = req->token;
            break;
        }
    }
    for (auto *pool : resourcePools()) {
        pool->issuedNow = 0;
        pool->attemptedNow = false;
        pool->samples++;
        if (env.detailedDivider && pool == &sharedDiv)
            pool->eligibleSlots += hardwareDivider.ready();
        else
            for (uint64_t next : pool->nextIssue)
                pool->eligibleSlots += next <= now;
    }
}

std::array<ResourcePool *, 16>
CfdCoeffPreprocessController::resourcePools()
{
    // Same order as OpKind; count pending operations without double counting
    // a pipeline-active cycle when latency > II.
    static_assert(static_cast<unsigned>(OpKind::SharedDiv) == 12);
    static_assert(static_cast<unsigned>(OpKind::FinalSubtract) == 13);
    static_assert(static_cast<unsigned>(OpKind::SharedFma) == 14);
    return {&luDiv, &luMul, &luSub, &coeffDiv, &coeffMul, &coeffSub,
            &columnFma, &forwardCombine, &lineProduct, &lineAccumulate,
            &sharedMul, &sharedAdd, &sharedDiv, &finalSubtract, &sharedFma, &sharedLuFma};
}

void
CfdCoeffPreprocessController::endResourceStats(bool quiescent)
{
    if (!env.resourceStats)
        return;
    for (const auto &[reason,unused] : waitThisCycle) waitCycles[reason]++;
    for (unsigned caller=0;caller<10;++caller)
        for (unsigned resource=0;resource<3;++resource) {
            rejectCycles[caller][resource] += (rejectThisCycle[caller] >> resource)&1;
            criticalRejectCycles[caller][resource] +=
                (criticalRejectThisCycle[caller] >> resource)&1;
        }
    sampleFrontierState();
    std::array<uint64_t, 16> occupancy = {};
    for (const auto &op : pendingOps)
        occupancy[static_cast<unsigned>(op.kind)] += op.lanes;
    const auto pools = resourcePools();
    const char *names[] = {"lu_div", "lu_mul", "lu_sub", "coeff_div",
        "coeff_mul", "coeff_sub", "column_fma", "forward_combine",
        "line_product", "line_accumulate", "shared_mul", "shared_add", "shared_div",
        "shared_sub", "shared_fma", "shared_lu_fma"};
    for (unsigned i = 0; i < pools.size(); ++i) {
        auto &p = *pools[i];
        p.issues += p.issuedNow;
        p.issueCycles += p.issuedNow != 0;
        p.blockedCycles += !p.issuedNow && p.attemptedNow;
        p.noAttemptCycles += !p.issuedNow && !p.attemptedNow;
        p.activeCycles += occupancy[i] != 0;
        p.occupancySum += occupancy[i];
        p.occupancyMax = std::max(p.occupancyMax, occupancy[i]);
        if (!quiescent || (env.sharedCluster ? i < 10 :
            (i >= 10 || (i >= 8 && !env.lineMvmSplit))))
            continue;
        if (i == static_cast<unsigned>(OpKind::LuFma) && !env.luFmaCount)
            continue;
        if (i == static_cast<unsigned>(OpKind::SharedFma) && !env.rhsFmaCount)
            continue;
        if (i == static_cast<unsigned>(OpKind::FinalSubtract) &&
            !env.dedicatedFinalSubtract)
            continue;
        // Cumulative snapshots across controller-active windows.  Consumers
        // select the last snapshot per pool, NOT the sum of snapshots.
        std::fprintf(stderr, "CFD_RESOURCE_STATS pool=%s count=%zu lat=%llu "
            "ii=%llu samples=%llu eligible_slots=%llu issued=%llu "
            "issue_cycles=%llu blocked_cycles=%llu no_attempt_cycles=%llu "
            "active_cycles=%llu occupancy_sum=%llu occupancy_max=%llu\n",
            names[i], p.nextIssue.size(), (unsigned long long)p.latency,
            (unsigned long long)p.ii, (unsigned long long)p.samples,
            (unsigned long long)p.eligibleSlots, (unsigned long long)p.issues,
            (unsigned long long)p.issueCycles,
            (unsigned long long)p.blockedCycles,
            (unsigned long long)p.noAttemptCycles,
            (unsigned long long)p.activeCycles,
            (unsigned long long)p.occupancySum,
            (unsigned long long)p.occupancyMax);
        std::fprintf(stderr, "CFD_ISSUE_DIAG pool=%s attempts=%llu "
            "rejected=%llu rejected_after_issue=%llu\n", names[i],
            (unsigned long long)p.attempts,
            (unsigned long long)p.rejected,
            (unsigned long long)p.rejectedAfterIssue);
    }
    if (quiescent) {
        if(env.rhsFmaFactorFanout)
            std::fprintf(stderr,"CFD_RHS_FMA_FACTOR_FANOUT_CAPACITY control_bytes=128 bus_bytes=256 receivers=2 vectors_per_cycle=1 extra_fma=0 extra_ports=0\n");
        if(env.spmSlotRotation)
            std::fprintf(stderr,"CFD_SPM_SLOT_ROTATION_CAPACITY mode=%u bytes=128 banks=4 grain=8 input_stride=8192 rhs_stride=128 max_shift=24 extra_ports=0 extra_units=0\n",
                env.spmSlotRotation);
        if(env.luPivotPrefetch)
            std::fprintf(stderr,"CFD_LU_PIVOT_PREFETCH_CAPACITY contexts=%u per_context=1 state_bytes=%zu context_reserved=512 control_bytes=128 selector_width=1 extra_units=0\n",
                env.luCount,sizeof(CfdLuRank1));
        if(env.luFmaCount)
            std::fprintf(stderr,"CFD_LU_FMA_CAPACITY count=%u lat=%u ii=%u slots=%llu bytes=%llu live=%llu peak=%llu issue_width_per_context=1\n",
                env.luFmaCount,CfdLuFma::Latency,CfdLuFma::Ii,
                (unsigned long long)rfCapacity(sharedLuFma),
                (unsigned long long)CfdLuFma::reservation(env.luFmaCount),
                (unsigned long long)sharedLuFma.rfLive,(unsigned long long)sharedLuFma.rfPeak);
        if(env.rhsUpperStream)
            std::fprintf(stderr,"CFD_RHS_UPPER_STREAM_CAPACITY bytes=%llu rhs_state_bytes=%zu forward_groups=4 upper_groups=4 independent_chains=2\n",
                (unsigned long long)CfdRhsUpperStream::ControlBytes,sizeof(RhsState));
        if(env.rhsUpperInverse)
            std::fprintf(stderr,"CFD_RHS_UPPER_CAPACITY slots=%u bytes=%llu live=%u peak=%u issued=%llu completed=%llu dropped=%llu issue_width=6\n",
                env.rhsUpperSlots,(unsigned long long)CfdRhsUpper::reservation(env.rhsUpperSlots),
                rhsUpper.live,rhsUpper.peak,(unsigned long long)rhsUpper.issued,
                (unsigned long long)rhsUpper.completed,(unsigned long long)rhsUpper.dropped);
        if (env.rhsFmaCount)
            std::fprintf(stderr,"CFD_RHS_FMA_CAPACITY count=%u lat=%u ii=%u slots=%llu bytes=%llu live=%llu peak=%llu\n",
                env.rhsFmaCount,CfdRhsFma::Latency,CfdRhsFma::Ii,
                (unsigned long long)rfCapacity(sharedFma),
                (unsigned long long)CfdRhsFma::reservation(env.rhsFmaCount),
                (unsigned long long)sharedFma.rfLive,(unsigned long long)sharedFma.rfPeak);
        if (env.summaryDepth)
            std::fprintf(stderr,"CFD_SUMMARY_SNAPSHOTS depth=%u engines=%u copy_words=8 entry_bytes=512 peak=%u live=%u issued=%llu completed=%llu dropped=%llu\n",
                env.summaryDepth,env.summaryEngines,summarySnapshots.peak,summarySnapshots.live,
                (unsigned long long)snapshotIssued,(unsigned long long)snapshotCompleted,
                (unsigned long long)snapshotDropped);
        if (env.vectorLocal || env.localProducts)
            std::fprintf(stderr,"CFD_LOCAL_VECTOR banks=%zu read_groups=1 write_groups=1 tags=32 peak=%u live=%u route=%u bytes=%llu vector_width=%u products_only=%u factor_fanout=%u factor_fanout_bytes=%u factor_fanout_feedback=%u tree_rf_temp=%u tree_rf_temp_bytes=%u tree_rf_temp_live=%u tree_rf_temp_peak=%u tree_rf_first_column=%u\n",
                localVectors.banks.size(),localVectors.peak,localVectors.live,
                env.localRouteLatency,(unsigned long long)localReservation(),env.vectorIssueWidth,
                env.vectorProductOnly,env.rhsFactorFanout,
                env.rhsFactorFanout ? CfdRhsFactorFanout::Reservation : 0,
                env.rhsFactorFanoutFeedback,env.rhsTreeRfTemp,
                env.rhsTreeRfTemp ? 16*env.rhsProductDepth+128 : 0,
                rhsTreeTempLive,rhsTreeTempPeak,env.rhsTreeRfFirstColumn);
        std::fprintf(stderr,"CFD_DRAIN_CAPACITY depth=%u active_limit=%u extra_bytes=%u peak=%llu active_peak=%llu live=%zu\n",
            env.drainTaskDepth,env.drainOutstanding,
            64*(env.drainTaskDepth>env.outputDepth ? env.drainTaskDepth-env.outputDepth : 0),
            (unsigned long long)drainTaskPeak,(unsigned long long)drainActivePeak,
            drainQueue.size()+activeDrains.size());
        if (env.arithmeticMode != CfdArithmeticGroups::Legacy)
            std::fprintf(stderr,"CFD_ARITHMETIC_SUMMARY mode=%u depth=%u width=%u select=%u route=%u borrow_route=%u age=%u accepted=%llu fragments=%llu completed=%llu live=%u peak=%u full=%llu\n",
                unsigned(env.arithmeticMode),env.arithmeticDepth,env.arithmeticWidth,
                env.arithmeticSelect,env.arithmeticRoute,env.arithmeticBorrowRoute,env.arithmeticAge,
                (unsigned long long)(arithmeticGroups.nextId-1),(unsigned long long)arithmeticIssued,
                (unsigned long long)arithmeticCompleted,arithmeticGroups.live,arithmeticGroups.peak,
                (unsigned long long)arithmeticFull);
        for (const auto &[reason,count] : waitCycles)
            std::fprintf(stderr,"CFD_WAIT_CYCLES reason=%s cycles=%llu samples=%llu\n",
                reason.c_str(),(unsigned long long)count,
                (unsigned long long)sharedMul.samples);
        for (unsigned caller=0;caller<10;++caller)
            for (unsigned resource=0;resource<3;++resource)
                std::fprintf(stderr,"CFD_FU_WAIT caller=%u resource=%u cycles=%llu frontier_cycles=%llu samples=%llu\n",
                    caller,resource,(unsigned long long)rejectCycles[caller][resource],
                    (unsigned long long)criticalRejectCycles[caller][resource],
                    (unsigned long long)sharedMul.samples);
        for (const auto &[state,count] : frontierStates)
            std::fprintf(stderr,"CFD_FRONTIER_STATE state=%s cycles=%llu samples=%llu\n",
                state.c_str(),(unsigned long long)count,
                (unsigned long long)frontierSamples);
        if (env.ageDrains)
            std::fprintf(stderr,"CFD_DRAIN_ARB latency=%llu ii=%llu granted=%llu wait_sum=%llu wait_max=%llu full=%llu\n",
                (unsigned long long)env.drainArbLatency,(unsigned long long)env.drainArbIi,
                (unsigned long long)drainArbGranted,(unsigned long long)drainArbWait,
                (unsigned long long)drainArbMaxWait,(unsigned long long)drainArbFull);
        if (env.dataCommitDepth) {
            uint64_t live = 0;
            for (const auto &[token,line] : lineRequests) live += line->dataCommitLive;
            std::fprintf(stderr,"CFD_DATA_COMMIT depth=%u issued=%llu released=%llu live=%llu peak=%llu full=%llu capacity=%llu\n",
                env.dataCommitDepth,(unsigned long long)dataCommitIssued,
                (unsigned long long)dataCommitReleased,(unsigned long long)live,
                (unsigned long long)dataCommitPeak,(unsigned long long)dataCommitFull,
                (unsigned long long)(env.inputSlots+env.luPendingDepth+env.solvePendingDepth+env.outputDepth));
        }
        if (env.detailedDivider) {
            const auto &d = hardwareDivider.stats;
            std::fprintf(stderr,"CFD_DIVIDER_SUMMARY model=%s lat=%llu ii=%llu "
                "capacity=%u result_depth=%u writeback_ii=%llu accepted=%llu "
                "produced=%llu returned=%llu core_live=%zu result_live=%zu "
                "core_peak=%zu result_peak=%zu pipeline_stalls=%llu output_stalls=%llu "
                "core_occupancy=%llu result_occupancy=%llu compute_cycles=%llu "
                "result_wait_cycles=%llu residence_cycles=%llu max_residence=%llu "
                "dropped=%llu flags=%u\n",
                env.iterativeDivider ? "iterative" : "pipelined",
                (unsigned long long)env.clusterDivLatency,
                (unsigned long long)env.clusterDivIi,env.dividerCapacity,
                env.dividerResultDepth,(unsigned long long)env.dividerWritebackIi,
                (unsigned long long)d.accepted,(unsigned long long)d.produced,
                (unsigned long long)d.returned,hardwareDivider.coreSize(),
                hardwareDivider.resultSize(),d.corePeak,d.resultPeak,
                (unsigned long long)d.pipelineStalls,(unsigned long long)d.outputStalls,
                (unsigned long long)d.coreOccupancy,(unsigned long long)d.resultOccupancy,
                (unsigned long long)d.computeCycles,(unsigned long long)d.resultWaitCycles,
                (unsigned long long)d.residenceCycles,(unsigned long long)d.maxResidence,
                (unsigned long long)dividerDropped,dividerFlags);
        }
        if (env.onchipTiming) {
            std::fprintf(stderr,"CFD_ONCHIP_SUMMARY banks=%u grain=%u read_ports=%u "
                "write_ports=%u latency=%llu depth=%u samples=%llu created=%llu "
                "completed=%llu cancelled=%llu live=%zu peak=%llu full=%llu conflicts=%llu "
                "compute_reserve=%u reserve_stalls=%llu inactive=%llu\n",
                env.spmBanks,env.spmBankGranularity,env.spmReadPorts,env.spmWritePorts,
                (unsigned long long)env.onchipLatency,env.onchipDepth,
                (unsigned long long)onchipSamples,(unsigned long long)onchipCreated,
                (unsigned long long)onchipCompleted,(unsigned long long)onchipCancelled,
                onchipQueue.size(),(unsigned long long)onchipPeak,
                (unsigned long long)onchipFull,(unsigned long long)onchipConflicts,
                env.onchipComputeReserve,(unsigned long long)onchipReserveStalls,
                (unsigned long long)onchipInactive);
            for (unsigned bank = 0; bank < env.spmBanks; ++bank)
                std::fprintf(stderr,"CFD_ONCHIP_BANK bank=%u read_words=%llu write_words=%llu\n",
                    bank,(unsigned long long)onchipReads[bank],(unsigned long long)onchipWrites[bank]);
        }
        if (env.lineOptLevel >= 5)
            std::fprintf(stderr, "CFD_COMPLETION_QUEUE depth=%u bytes=%u peak=%llu "
                "issued=%llu completed=%llu live=%zu\n", env.completionDepth,
                env.completionDepth*completionEntryBytes(env.lineOptLevel), (unsigned long long)detachedPeak,
                (unsigned long long)detachedIssued, (unsigned long long)detachedCompleted,
                detachedRecords.size());
        if (env.rhsRowParallel)
            std::fprintf(stderr, "CFD_RHS_ROWS capacity=%u bytes=%u peak=%llu "
                "issued=%llu completed=%llu live=%zu stalls=%llu\n",
                env.rhsProductDepth,env.rhsProductDepth*64+256,
                (unsigned long long)rowGroupsPeak, (unsigned long long)rowGroupsIssued,
                (unsigned long long)rowGroupsCompleted,
                static_cast<size_t>(std::count_if(rhsRowProducts.begin(), rhsRowProducts.end(),
                              [](const auto &s) { return s.valid; })),
                (unsigned long long)rowGroupsStalls);
        if (env.sharedCluster)
            std::fprintf(stderr, "CFD_CLUSTER_BUFFER depth=%u max=%llu "
                "issued=%llu completed=%llu live=%llu stalls=%llu\n",
                env.clusterProductDepth, (unsigned long long)sharedVectorsMax,
                (unsigned long long)sharedVectorsIssued,
                (unsigned long long)sharedVectorsCompleted,
                (unsigned long long)sharedVectorsLive,
                (unsigned long long)sharedProductStalls);
        for (const auto &[reason, count] : diagnostics)
            std::fprintf(stderr, "CFD_SCHED_DIAG reason=%s count=%llu\n",
                reason.c_str(), (unsigned long long)count);
    }
}
