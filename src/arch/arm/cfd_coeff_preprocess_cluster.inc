// One physical multiplier bank, one add/sub bank, and one shared divider.
// A vector reserves all five lanes atomically; scalar LU/RHS work reserves one.
bool
CfdCoeffPreprocessController::issueLanes(
    ResourcePool &pool, OpKind kind, uint64_t now, unsigned lanes,
    OpKind caller, std::function<void()> complete)
{
    if (env.resourceStats) {
        pool.attemptedNow = true;
        pool.attempts++;
    }
    const unsigned available = std::count_if(pool.nextIssue.begin(),
        pool.nextIssue.end(), [now](uint64_t next) { return next <= now; });
    if (available < lanes) {
        if (env.resourceStats) {
            pool.rejected++;
            pool.rejectedAfterIssue += pool.issuedNow != 0;
        }
        return false;
    }
    unsigned reserved = 0;
    for (auto &next : pool.nextIssue) {
        if (next <= now && reserved < lanes) {
            next = now + pool.ii;
            reserved++;
        }
    }
    if (env.resourceStats)
        pool.issuedNow += lanes;
    pendingOps.push_back({now + pool.latency, kind, std::move(complete), lanes});
    traceCluster(kind == OpKind::SharedMul ? "mul" :
                 (kind == OpKind::SharedAdd ? "add" : "div"),
                 lanes, caller, now, pool.latency);
    return true;
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
        }))
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
