// Row-independent scheduling of the existing FP64 trees. This does not
// speculate across adjacent cells or change any row's floating-point order.
// The finite 3-context graph has 50 nodes/context, 10 global selection grants,
// a registered selection stage, and optionally five local 2R1W row banks.
void
CfdCoeffPreprocessController::advanceLaneMvm(
    LineRequest &line, unsigned context,
    const std::array<double,N*N> &matrix, const std::array<double,N> &vector,
    uint8_t readyMask, uint64_t now,
    std::function<void(LineRequest &,const std::array<double,N> &)> done,
    uint64_t registerRequest)
{
    auto &state = line.mvm[context];
    const uint64_t token = line.token, epoch = state.epoch;
    const uint64_t cell = context == 1 ? line.backwardCell :
        context == 2 ? line.baseCell : line.cachedForwardCell;
    const bool feedback = context == 1 && env.backwardFeedbackTree;
    const unsigned last = feedback ? 9 : 8;
    // At most lineProductDepth columns may own live product slots. Tree
    // consumers retain these slots through the cell, as in the old tree.
    for (unsigned col=0; col<std::min<unsigned>(N,env.lineProductDepth); ++col) {
        if (state.laneOperands[col] || !(readyMask & (1u<<col))) continue;
        if (!registerRequest && !onchipColumnReady(line,context,col)) continue;
        for (unsigned row=0;row<N;++row)
            if (!env.onchipTiming || registerRequest)
                state.columnOperands[col][row] = matrix[row*N+col];
        state.laneOperands[col] = true;
        state.laneOperandCycle[col] = now;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_LANE_OPERAND cycle=%llu token=%llu context=%u epoch=%llu cell=%llu column=%u\n",
                (unsigned long long)now,(unsigned long long)token,context,
                (unsigned long long)epoch,(unsigned long long)cell,col);
    }
    // Consumers first to shorten feedback and release pipeline credits.
    // Preprocessing still uses the SAME MUL/ADD pool after this scheduler.
    for (unsigned pass=0;pass<=last;++pass) {
        // Deep consumers first; products retain ascending column priority so
        // existing request-prefix accounting stays a truthful prefix.
        const int node=pass<last-4 ? last-pass : pass-(last-4);
        const bool mul = node < static_cast<int>(N);
        for (unsigned row=0;row<N;++row) {
            const uint8_t bit = 1u<<row;
            if (state.laneIssued[node] & bit) continue;
            int left=-1,right=-1;
            bool subtract=false;
            if (!mul) {
                const unsigned n=node-N;
                if (feedback) {
                    constexpr int lhs[5]={-1,1,3,5,8};
                    constexpr int rhs[5]={0,2,4,6,7};
                    left=lhs[n]; right=rhs[n]; subtract=n==0 || n>=3;
                } else {
                    constexpr int lhs[4]={0,2,5,7};
                    constexpr int rhs[4]={1,3,6,4};
                    left=lhs[n]; right=rhs[n];
                }
            }
            uint64_t ready=0;
            if (mul) {
                if (!state.laneOperands[node]) continue;
                ready=state.laneOperandCycle[node];
            } else {
                if ((left>=0 && !(state.laneReady[left]&bit)) ||
                    !(state.laneReady[right]&bit)) continue;
                if (feedback && left < 0 && !backwardYReady(line)) continue;
                ready=std::max(left>=0 ? state.laneReadyCycle[left][row] : 0,
                    state.laneReadyCycle[right][row]);
                if (feedback && left < 0 && env.backwardIndependentReady)
                    ready=std::max(ready,line.backwardOperands[
                        cell%env.backwardOperandDepth].yLoad->readyCycle);
            }
            if (now < ready+env.laneSelectLatency) continue;
            if (laneGrants >= env.laneIssueWidth) {
                observe("lane_selection_width");
                return;
            }
            auto value=[&state,row](int index) {
                return index<static_cast<int>(N) ? state.products[index][row] :
                    state.partial[index-N][row];
            };
            const double a=mul ? state.columnOperands[node][row] :
                left<0 ? line.backwardYOperand[row] : value(left);
            const double b=mul ? vector[node] : value(right);
            const unsigned localReads=env.laneLocal && !mul ? (left<0 ? 1 : 2) : 0;
            const unsigned globalReads=2-localReads;
            const bool localResult=env.laneLocal && node!=static_cast<int>(last);
            const auto caller=mul ? OpKind::LineProduct :
                feedback && node==static_cast<int>(last) ? OpKind::ForwardCombine :
                OpKind::LineAccumulate;
            if (!issueLanes(mul ? sharedMul : sharedAdd,
                mul ? OpKind::SharedMul : OpKind::SharedAdd,now,1,caller,
                [this,token,epoch,cell,context,node,row,a,b,mul,subtract,last,done] {
                    auto it=lineRequests.find(token);
                    if (it==lineRequests.end() || it->second->terminal) return;
                    auto &line=*it->second;
                    auto &state=line.mvm[context];
                    if (state.epoch!=epoch) return;
                    volatile double rounded=mul ? a*b : subtract ? a-b : a+b;
                    if (node<static_cast<int>(N)) state.products[node][row]=rounded;
                    else if (node<static_cast<int>(last)) state.partial[node-N][row]=rounded;
                    else state.sum[row]=rounded;
                    state.laneReady[node] |= 1u<<row;
                    state.laneReadyCycle[node][row]=cycle()+env.laneRouteLatency;
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_LANE_MVM cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=complete node=%d row=%u value=%a\n",
                            (unsigned long long)cycle(),(unsigned long long)token,context,
                            (unsigned long long)epoch,(unsigned long long)cell,node,row,double(rounded));
                    if (node==static_cast<int>(last) && state.laneReady[last]==0x1f) {
                        const auto result=state.sum;
                        state=LineMvmState{};
                        state.epoch=epoch+1;
                        done(line,result);
                    }
                },false,globalReads,localReads,localResult,row)) continue;
            laneGrants++;
            state.laneIssued[node] |= bit;
            if (mul && state.laneIssued[node]==0x1f) {
                line.lineProducts++;
                state.issued++;
                line.lineBufferedMax=std::max<uint64_t>(line.lineBufferedMax,state.issued);
            } else if (!mul && state.laneIssued[node]==0x1f &&
                       !(feedback && node==static_cast<int>(last))) line.lineAdds++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_LANE_MVM cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=issue node=%d row=%u id=%llu ready=%llu a=%a b=%a\n",
                    (unsigned long long)now,(unsigned long long)token,context,
                    (unsigned long long)epoch,(unsigned long long)cell,node,row,
                    (unsigned long long)(nextRfTag-1),(unsigned long long)ready,a,b);
        }
    }
}
