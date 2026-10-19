// Independent forward row updates use spare scalar lanes. Backward products
// are generated together, but subtractions retain their original j order.
void
CfdCoeffPreprocessController::releaseRhsRows(RhsRowProduct &slot)
{
    if (!slot.valid) return;
    slot.valid = false;
    rowGroupsCompleted++;
}

void
CfdCoeffPreprocessController::discardRhsRows(Request &req)
{
    for (auto &slot : rhsRowProducts)
        if (slot.valid && slot.token == req.token && !req.rhs[slot.rhs].waiting)
            releaseRhsRows(slot);
}

bool
CfdCoeffPreprocessController::issueSolveRows(Request &req, unsigned index, uint64_t now)
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
    const unsigned width = multiply || !backward ? N-k-1 : 1;
    rhs.waiting = true;
    const bool accepted = issueLanes(multiply ? sharedMul : sharedAdd,
        multiply ? OpKind::SharedMul : OpKind::SharedAdd, now, width,
        multiply ? OpKind::Mul : OpKind::Sub,
        [this,token,index,k,slotIndex,multiply,backward] {
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
                    volatile double value = backward ? r->lu[k*N+row]*s.value[row] :
                                                       r->lu[row*N+k]*s.value[k];
                    slot.value[row] = value;
                }
                s.phase = backward ? RhsPhase::BackwardSub : RhsPhase::ForwardSub;
            } else if (!backward) {
                for (unsigned row = k+1; row < N; ++row) {
                    volatile double value = s.value[row]-slot.value[row];
                    s.value[row] = value;
                }
                releaseRhsRows(slot);
                s.k++;
                s.phase = s.k == 4 ? RhsPhase::LastDiv : RhsPhase::ForwardDiv;
            } else {
                volatile double value = s.value[k]-slot.value[s.i];
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
        });
    if (!accepted) {
        rhs.waiting = false;
        req.record.stallCoeffMulSub++;
        return false;
    }
    if (multiply) {
        it->valid = true;
        it->token = token;
        it->rhs = index;
        rowGroupsIssued++;
        req.record.coeffMulIssued += width;
        rowGroupsPeak = std::max<uint64_t>(rowGroupsPeak,
            std::count_if(rhsRowProducts.begin(), rhsRowProducts.end(), [](const auto &s) { return s.valid; }));
    } else {
        req.record.coeffSubIssued += width;
    }
    return true;
}
