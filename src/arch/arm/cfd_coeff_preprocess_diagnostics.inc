void
CfdCoeffPreprocessController::beginResourceStats(uint64_t now)
{
    if (!env.resourceStats)
        return;
    for (auto *pool : resourcePools()) {
        pool->issuedNow = 0;
        pool->attemptedNow = false;
        pool->samples++;
        for (uint64_t next : pool->nextIssue)
            pool->eligibleSlots += next <= now;
    }
}

std::array<ResourcePool *, 13>
CfdCoeffPreprocessController::resourcePools()
{
    // Same order as OpKind; count pending operations without double counting
    // a pipeline-active cycle when latency > II.
    static_assert(static_cast<unsigned>(OpKind::SharedDiv) == 12);
    return {&luDiv, &luMul, &luSub, &coeffDiv, &coeffMul, &coeffSub,
            &columnFma, &forwardCombine, &lineProduct, &lineAccumulate,
            &sharedMul, &sharedAdd, &sharedDiv};
}

void
CfdCoeffPreprocessController::endResourceStats(bool quiescent)
{
    if (!env.resourceStats)
        return;
    std::array<uint64_t, 13> occupancy = {};
    for (const auto &op : pendingOps)
        occupancy[static_cast<unsigned>(op.kind)] += op.lanes;
    const auto pools = resourcePools();
    const char *names[] = {"lu_div", "lu_mul", "lu_sub", "coeff_div",
        "coeff_mul", "coeff_sub", "column_fma", "forward_combine",
        "line_product", "line_accumulate", "shared_mul", "shared_add", "shared_div"};
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
        if (env.onchipTiming) {
            std::fprintf(stderr,"CFD_ONCHIP_SUMMARY banks=%u grain=%u read_ports=%u "
                "write_ports=%u latency=%llu depth=%u samples=%llu created=%llu "
                "completed=%llu cancelled=%llu live=%zu peak=%llu full=%llu conflicts=%llu\n",
                env.spmBanks,env.spmBankGranularity,env.spmReadPorts,env.spmWritePorts,
                (unsigned long long)env.onchipLatency,env.onchipDepth,
                (unsigned long long)onchipSamples,(unsigned long long)onchipCreated,
                (unsigned long long)onchipCompleted,(unsigned long long)onchipCancelled,
                onchipQueue.size(),(unsigned long long)onchipPeak,
                (unsigned long long)onchipFull,(unsigned long long)onchipConflicts);
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
