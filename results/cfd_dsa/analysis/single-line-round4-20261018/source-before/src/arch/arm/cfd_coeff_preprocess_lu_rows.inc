// Crout targets within a column are independent. Each target retains its
// original k order; upper quotients share the same physical pipelined divider.
void
CfdCoeffPreprocessController::processLuRows(Request &req, uint64_t now)
{
    if (req.luVisitedCycle == now) return;
    req.luVisitedCycle = now;
    if (req.luWaiting) { req.record.stallDependency++; return; }
    const int c = req.luCol;
    const uint64_t token = req.token;
    switch (req.luPhase) {
      case LuPhase::StartLower:
        for (int row = c; row < N; ++row) req.luTemps[row] = req.d[row*N+c];
        req.luK = 0;
        req.luPhase = c ? LuPhase::LowerMul : LuPhase::StoreLower;
        break;
      case LuPhase::LowerMul:
      case LuPhase::UpperMul:
      case LuPhase::LowerSub:
      case LuPhase::UpperSub:
      {
        const bool upper = req.luPhase == LuPhase::UpperMul || req.luPhase == LuPhase::UpperSub;
        const bool mul = req.luPhase == LuPhase::LowerMul || req.luPhase == LuPhase::UpperMul;
        const unsigned width = N-c-(upper ? 1 : 0);
        const int k = req.luK;
        req.luWaiting = true;
        const bool accepted = issueLanes(mul ? sharedMul : sharedAdd,
            mul ? OpKind::SharedMul : OpKind::SharedAdd, now, width,
            mul ? OpKind::LuMul : OpKind::LuSub,
            [this,token,c,k,upper,mul] {
                auto *r = find(token);
                if (!r || r->stage == RequestStage::Failed) return;
                for (int target = c+(upper ? 1 : 0); target < N; ++target) {
                    if (mul) {
                        volatile double v = upper ? r->lu[c*N+k]*r->lu[k*N+target] :
                                                    r->lu[target*N+k]*r->lu[k*N+c];
                        r->luProducts[target] = v;
                    } else {
                        volatile double v = r->luTemps[target]-r->luProducts[target];
                        r->luTemps[target] = v;
                    }
                }
                r->luWaiting = false;
                if (mul) r->luPhase = upper ? LuPhase::UpperSub : LuPhase::LowerSub;
                else {
                    r->luK++;
                    r->luPhase = r->luK < c ? (upper ? LuPhase::UpperMul : LuPhase::LowerMul) :
                                                            (upper ? LuPhase::DivideUpper : LuPhase::StoreLower);
                }
            });
        if (!accepted) { req.luWaiting = false; req.record.stallLuMulSub++; }
        else {
            if (mul) req.record.luMulIssued += width;
            else req.record.luSubIssued += width;
            trace(req,"lu",mul ? "LU_ROW_MUL_ISSUE" : "LU_ROW_SUB_ISSUE",-1,-1,k,c);
        }
        break;
      }
      case LuPhase::StoreLower:
        for (int row = c; row < N; ++row) req.lu[row*N+c] = req.luTemps[row];
        if (!req.record.luFirstResultCycle) req.record.luFirstResultCycle = now;
        req.luPhase = LuPhase::PivotCheck;
        break;
      case LuPhase::PivotCheck:
        if (!std::isfinite(req.lu[c*N+c]) || std::fabs(req.lu[c*N+c]) <= req.desc.pivotEpsilon) {
            markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"pivot");
            break;
        }
        req.forwardStepReady[c] = true;
        req.record.luStepReadyCycle[c] = now;
        trace(req,"lu","LU_STEP_READY",-1,-1,c,-1,"forward");
        req.luPhase = c+1 < N ? LuPhase::StartUpper : LuPhase::NextColumn;
        break;
      case LuPhase::StartUpper:
        for (int j = c+1; j < N; ++j) req.luTemps[j] = req.d[c*N+j];
        req.luK = 0;
        req.luDivIssued = req.luDivCompleted = 0;
        req.luPhase = c ? LuPhase::UpperMul : LuPhase::DivideUpper;
        break;
      case LuPhase::DivideUpper:
      {
        int j = c+1;
        while (j < N && (req.luDivIssued & (1u<<j))) j++;
        if (j == N) break;
        const double numerator = req.luTemps[j], denominator = req.lu[c*N+c];
        if (issueLanes(sharedDiv,OpKind::SharedDiv,now,1,OpKind::LuDiv,
            [this,token,c,j,numerator,denominator] {
                auto *r = find(token);
                if (!r || r->stage == RequestStage::Failed) return;
                volatile double v = numerator/denominator;
                r->luTemps[j] = v;
                r->luDivCompleted |= 1u<<j;
                const uint8_t wanted = 0x1f & ~((1u<<(c+1))-1);
                if (r->luDivCompleted == wanted) r->luPhase = LuPhase::StoreUpper;
            })) {
            req.luDivIssued |= 1u<<j;
            req.record.luDivIssued++;
            trace(req,"lu","LU_DIV_ISSUE",-1,-1,c,j);
        } else req.record.stallLuDivider++;
        break;
      }
      case LuPhase::StoreUpper:
        for (int j = c+1; j < N; ++j) {
            if (!std::isfinite(req.luTemps[j])) {
                markFailed(req,CfdCoeffPreprocessStatus::LuFailure,"upper");
                return;
            }
            req.lu[c*N+j] = req.luTemps[j];
        }
        req.luPhase = LuPhase::NextColumn;
        break;
      case LuPhase::NextColumn:
        req.backwardStepReady[c] = true;
        trace(req,"lu","LU_COLUMN_DONE",-1,-1,c);
        req.luCol++;
        req.luPhase = req.luCol < N ? LuPhase::StartLower : LuPhase::Done;
        break;
      case LuPhase::Done:
        req.record.luCompleteCycle = now;
        req.luDone = true;
        req.stage = req.solveStarted ? RequestStage::SolveActive : RequestStage::LuReady;
        trace(req,"lu","LU_COMPLETE");
        break;
    }
}
