// Seven ready bits per RESERVED request slot, not an unbounded output queue.
// One oldest eligible descriptor is granted per II. The descriptor becomes
// eligible only after the modeled arbitration latency; DMA responses still
// own the output state slot until processDrains retires it.
bool
CfdCoeffPreprocessController::enqueueDrain(Request &req,DrainType type,uint64_t now)
{
    if (!env.ageDrains) return admitDrain(req,type,now);
    const unsigned kind = static_cast<unsigned>(type);
    if (req.drainQueued[kind]) return true;
    const uint8_t bit = 1u << kind;
    if (!(req.drainReadyMask & bit)) {
        req.drainReadyMask |= bit;
        req.drainReadySince[kind] = now;
        trace(req,"drain-arb","DRAIN_READY",-1,-1,kind);
    }
    return false;
}

void
CfdCoeffPreprocessController::arbitrateDrains(uint64_t now)
{
    if (now < drainArbNext) return;
    Request *chosen = nullptr;
    unsigned kind = 0;
    for (auto *req : admissionOrder()) {
        if (req->stage == RequestStage::Failed || req->stage == RequestStage::Complete)
            continue;
        for (unsigned i=0;i<7;++i) {
            if (!(req->drainReadyMask & (1u << i)) ||
                now-req->drainReadySince[i] < env.drainArbLatency) continue;
            if (!chosen || std::tie(req->drainReadySince[i],req->token,i) <
                           std::tie(chosen->drainReadySince[kind],chosen->token,kind)) {
                chosen = req;
                kind = i;
            }
        }
    }
    if (!chosen) return;
    if (!admitDrain(*chosen,static_cast<DrainType>(kind),now)) {
        drainArbFull++;
        return;
    }
    const uint64_t wait = now-chosen->drainReadySince[kind];
    chosen->drainReadyMask &= ~(1u << kind);
    drainArbNext = now+env.drainArbIi;
    drainArbGranted++;
    drainArbWait += wait;
    drainArbMaxWait = std::max(drainArbMaxWait,wait);
    trace(*chosen,"drain-arb","DRAIN_GRANT",-1,-1,kind);
}
