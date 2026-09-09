#include "arch/arm/cfd_local_spm.hh"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "arch/arm/cfd_lusgs_controller.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "sim/cur_tick.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5
{

namespace ArmISA
{

namespace
{

CfdLocalSpm *localSpmInstance = nullptr;

uint64_t
localEnvU64(const char *name, uint64_t default_value)
{
    if (const char *raw = std::getenv(name)) {
        char *end = nullptr;
        const uint64_t value = std::strtoull(raw, &end, 0);
        if (end && *end == '\0')
            return value;
    }
    return default_value;
}

} // anonymous namespace

CfdLocalSpm *
getCfdLocalSpm()
{
    return localSpmInstance;
}

uint64_t
cfdDynInstSeqNum(ExecContext *xc)
{
    if (auto *dyn = dynamic_cast<o3::DynInst *>(xc))
        return dyn->seqNum;
    return 0;
}

CfdLocalSpm::CfdLocalSpm(const CfdLocalSpmParams &params)
    : SimObject(params),
      system(params.system),
      dmaPort(name() + ".dma_port"),
      dmaRequestorId(system->getRequestorId(this, "coeff_dma")),
      ADD_STAT(bytesRead, statistics::units::Byte::get(),
               "Logical bytes read through CFD local SPM port"),
      ADD_STAT(logicalBytesRead, statistics::units::Byte::get(),
               "Logical lmat5_spm bytes read through local SPM"),
      ADD_STAT(physicalBytesRead, statistics::units::Byte::get(),
               "Physical SRAM bytes read through local SPM"),
      ADD_STAT(numReads, statistics::units::Count::get(),
               "Number of local SPM reads"),
      ADD_STAT(readLatencyTotal, statistics::units::Cycle::get(),
               "Total modeled local SPM read latency"),
      ADD_STAT(avgReadLatency,
               "Average modeled local SPM read latency"),
      ADD_STAT(maxReadLatency, statistics::units::Cycle::get(),
               "Maximum modeled local SPM read latency"),
      ADD_STAT(portBusyCycles, statistics::units::Cycle::get(),
               "Modeled local SPM read-port busy cycles"),
      ADD_STAT(portConflictCycles, statistics::units::Cycle::get(),
               "Modeled local SPM read-port conflict cycles"),
      ADD_STAT(bankConflictCycles, statistics::units::Cycle::get(),
               "Modeled local SPM bank conflict cycles"),
      ADD_STAT(queueFullCycles, statistics::units::Cycle::get(),
               "Modeled local SPM request queue full cycles"),
      ADD_STAT(zWritebackConflictCycles, statistics::units::Cycle::get(),
               "Modeled local SPM Z writeback conflict cycles"),
      ADD_STAT(requestToResponseTotal, statistics::units::Cycle::get(),
               "Total local SPM request-to-response cycles"),
      ADD_STAT(avgRequestToResponse,
               "Average local SPM request-to-response cycles"),
      ADD_STAT(responseToZWritebackTotal, statistics::units::Cycle::get(),
               "Total local SPM response-to-Z-writeback wait cycles"),
      ADD_STAT(avgZWritebackWait,
               "Average local SPM response-to-Z-writeback wait cycles"),
      ADD_STAT(maxZWritebackWait, statistics::units::Cycle::get(),
               "Maximum local SPM response-to-Z-writeback wait cycles"),
      ADD_STAT(outstandingReadsAvg,
               "Average modeled outstanding local SPM reads"),
      ADD_STAT(outstandingReadsMax, statistics::units::Count::get(),
               "Maximum modeled outstanding local SPM reads"),
      ADD_STAT(readsPerCycle, "Local SPM reads per CPU cycle"),
      ADD_STAT(bytesPerCycle, "Local SPM physical bytes per CPU cycle"),
      ADD_STAT(logicalBytesPerCycle, "Local SPM logical bytes per CPU cycle"),
      ADD_STAT(physicalBytesPerCycle, "Local SPM physical bytes per CPU cycle"),
      ADD_STAT(zaTokensProduced, statistics::units::Count::get(),
               "Modeled ZA accumulator tokens produced"),
      ADD_STAT(zaTokensConsumed, statistics::units::Count::get(),
               "Modeled ZA accumulator tokens consumed"),
      ADD_STAT(zaPendingUpdates, statistics::units::Count::get(),
               "Modeled ZA pending updates created"),
      ADD_STAT(zaPendingQueueFull, statistics::units::Count::get(),
               "Modeled ZA pending queue full events"),
      ADD_STAT(zaSquashDiscards, statistics::units::Count::get(),
               "Modeled ZA pending updates discarded on squash"),
      ADD_STAT(zaCommitUpdates, statistics::units::Count::get(),
               "Modeled ZA architectural commit updates"),
      ADD_STAT(zaMovaWaitCycles, statistics::units::Cycle::get(),
               "Modeled MOVA wait cycles for final ZA token"),
      ADD_STAT(zaNextZeroWaitCycles, statistics::units::Cycle::get(),
               "Modeled next ZERO wait cycles for current MOVA"),
      ADD_STAT(zaForwardingStalls, statistics::units::Cycle::get(),
               "Modeled ZA accumulator forwarding stall cycles"),
      ADD_STAT(zaRenameAllocs, statistics::units::Count::get(),
               "Modeled physical ZA rename allocations"),
      ADD_STAT(zaRenameFrees, statistics::units::Count::get(),
               "Modeled physical ZA rename frees"),
      ADD_STAT(zaRenameRollbacks, statistics::units::Count::get(),
               "Modeled physical ZA rename rollbacks"),
      ADD_STAT(zaPhysicalLiveMax, statistics::units::Count::get(),
               "Maximum live physical ZA states"),
      ADD_STAT(zaPhysicalLiveEnd, statistics::units::Count::get(),
               "Live physical ZA states at the current point"),
      ADD_STAT(zaMovaRenameHits, statistics::units::Count::get(),
               "MOVA reads satisfied from renamed physical ZA producer"),
      ADD_STAT(zaMovaArchFallbacks, statistics::units::Count::get(),
               "MOVA reads falling back to committed architectural ZA"),
      ADD_STAT(zaMovaBlockedNotReady, statistics::units::Count::get(),
               "MOVA reads blocked by not-ready physical ZA producer"),
      ADD_STAT(zaMovaReads, statistics::units::Count::get(),
               "Total MOVA reads of CFD ZA state"),
      ADD_STAT(pathBPipeSteps, statistics::units::Count::get(),
               "Path B ZA spatial-pipeline step executions"),
      ADD_STAT(pathBPipePendingUpdates, statistics::units::Count::get(),
               "Path B pending ZA column updates created"),
      ADD_STAT(pathBPipeCommitUpdates, statistics::units::Count::get(),
               "Path B ZA column updates committed"),
      ADD_STAT(pathBPipeSquashDiscards, statistics::units::Count::get(),
               "Path B ZA column updates discarded on squash"),
      ADD_STAT(pathBPipeCol0Writes, statistics::units::Count::get(),
               "Path B writes to ZA[0:4,0]"),
      ADD_STAT(pathBPipeCol1Writes, statistics::units::Count::get(),
               "Path B writes to ZA[0:4,1]"),
      ADD_STAT(pathBPipeCol2Writes, statistics::units::Count::get(),
               "Path B writes to ZA[0:4,2]"),
      ADD_STAT(pathBPipeCol3Writes, statistics::units::Count::get(),
               "Path B writes to ZA[0:4,3]"),
      ADD_STAT(pathBPipeCol4Writes, statistics::units::Count::get(),
               "Path B writes to ZA[0:4,4]"),
      ADD_STAT(pathBPipeFinalReads, statistics::units::Count::get(),
               "Path B MOVA reads of final ZA[0:4,4]"),
      ADD_STAT(pathBPipeForwardHits, statistics::units::Count::get(),
               "Path B steps satisfied from latest forwarded ZA column"),
      ADD_STAT(pathBPipeForwardStalls, statistics::units::Cycle::get(),
               "Path B modeled ZA column-forwarding stalls"),
      ADD_STAT(pathCOuterSteps, statistics::units::Count::get(),
               "Path C ZA outer selected-column step executions"),
      ADD_STAT(pathCOuterPendingUpdates, statistics::units::Count::get(),
               "Path C outer pending ZA column updates created"),
      ADD_STAT(pathCOuterCommitUpdates, statistics::units::Count::get(),
               "Path C outer ZA column updates committed"),
      ADD_STAT(pathCOuterSquashDiscards, statistics::units::Count::get(),
               "Path C outer ZA column updates discarded on squash"),
      ADD_STAT(pathCOuterCol0Writes, statistics::units::Count::get(),
               "Path C outer writes to ZA[0:4,0]"),
      ADD_STAT(pathCOuterCol1Writes, statistics::units::Count::get(),
               "Path C outer writes to ZA[0:4,1]"),
      ADD_STAT(pathCOuterCol2Writes, statistics::units::Count::get(),
               "Path C outer writes to ZA[0:4,2]"),
      ADD_STAT(pathCOuterCol3Writes, statistics::units::Count::get(),
               "Path C outer writes to ZA[0:4,3]"),
      ADD_STAT(pathCOuterCol4Writes, statistics::units::Count::get(),
               "Path C outer writes to ZA[0:4,4]"),
      ADD_STAT(pathCOuterFinalReads, statistics::units::Count::get(),
               "Path C outer MOVA reads of final ZA[0:4,4]"),
      ADD_STAT(pathCOuterForwardHits, statistics::units::Count::get(),
               "Path C outer steps satisfied from latest forwarded ZA column"),
      ADD_STAT(pathCOuterForwardStalls, statistics::units::Cycle::get(),
               "Path C outer modeled ZA column-forwarding stalls"),
      ADD_STAT(pathCOuterMovaWaitCycles, statistics::units::Cycle::get(),
               "Path C outer MOVA wait cycles for final column"),
      ADD_STAT(pathCOuterNextZeroWaitCycles, statistics::units::Cycle::get(),
               "Path C outer next-ZERO wait cycles"),
      ADD_STAT(localEventRequestsCreated, statistics::units::Count::get(),
               "Local SPM event-model requests created"),
      ADD_STAT(localEventRequestsAccepted, statistics::units::Count::get(),
               "Local SPM event-model requests accepted"),
      ADD_STAT(localEventRequestsCompleted, statistics::units::Count::get(),
               "Local SPM event-model requests completed"),
      ADD_STAT(localEventRequestsSquashed, statistics::units::Count::get(),
               "Local SPM event-model requests squashed"),
      ADD_STAT(localEventAcceptEvents, statistics::units::Count::get(),
               "Local SPM accept events processed"),
      ADD_STAT(localEventReadStartEvents, statistics::units::Count::get(),
               "Local SPM read-start events processed"),
      ADD_STAT(localEventResponseEvents, statistics::units::Count::get(),
               "Local SPM response events processed"),
      ADD_STAT(localEventZwbEvents, statistics::units::Count::get(),
               "Local SPM Z writeback events processed"),
      ADD_STAT(localEventCompleteEvents, statistics::units::Count::get(),
               "Local SPM complete events processed"),
      ADD_STAT(localEventRetryEvents, statistics::units::Count::get(),
               "Local SPM event-model retry/defer events"),
      ADD_STAT(localEventQueueFullEvents, statistics::units::Count::get(),
               "Local SPM event-model queue-full deferrals"),
      ADD_STAT(localEventOutstandingFullEvents, statistics::units::Count::get(),
               "Local SPM event-model outstanding-limit deferrals"),
      ADD_STAT(localEventLiveRequests, statistics::units::Count::get(),
               "Current live Local SPM event-model requests"),
      ADD_STAT(localEventLiveRequestsMax, statistics::units::Count::get(),
               "Maximum live Local SPM event-model requests"),
      ADD_STAT(localEventLiveRequestsEnd, statistics::units::Count::get(),
               "Live Local SPM event-model requests at stats sample"),
      ADD_STAT(localEventIssueToAcceptTotal, statistics::units::Cycle::get(),
               "Total Local SPM issue-to-accept cycles"),
      ADD_STAT(localEventAcceptToReadStartTotal, statistics::units::Cycle::get(),
               "Total Local SPM accept-to-read-start cycles"),
      ADD_STAT(localEventReadStartToResponseTotal,
               statistics::units::Cycle::get(),
               "Total Local SPM read-start-to-response cycles"),
      ADD_STAT(localEventResponseToZwbTotal, statistics::units::Cycle::get(),
               "Total Local SPM response-to-ZWB cycles"),
      ADD_STAT(localEventZwbToCompleteTotal, statistics::units::Cycle::get(),
               "Total Local SPM ZWB-to-complete cycles"),
      ADD_STAT(localEventIssueToCompleteTotal, statistics::units::Cycle::get(),
               "Total Local SPM issue-to-complete cycles"),
      ADD_STAT(localEventAvgIssueToAccept,
               "Average Local SPM issue-to-accept cycles"),
      ADD_STAT(localEventAvgAcceptToReadStart,
               "Average Local SPM accept-to-read-start cycles"),
      ADD_STAT(localEventAvgReadStartToResponse,
               "Average Local SPM read-start-to-response cycles"),
      ADD_STAT(localEventAvgResponseToZwb,
               "Average Local SPM response-to-ZWB cycles"),
      ADD_STAT(localEventAvgZwbToComplete,
               "Average Local SPM ZWB-to-complete cycles"),
      ADD_STAT(localEventAvgIssueToComplete,
               "Average Local SPM issue-to-complete cycles"),
      ADD_STAT(coeffPacketIssued, statistics::units::Count::get(),
               "Coefficient preprocessing packets issued to Local SPM"),
      ADD_STAT(coeffPacketCompleted, statistics::units::Count::get(),
               "Coefficient preprocessing Local SPM packets completed"),
      ADD_STAT(coeffPacketReadBytes, statistics::units::Byte::get(),
               "Coefficient preprocessing bytes read from Local SPM"),
      ADD_STAT(coeffPacketWriteBytes, statistics::units::Byte::get(),
               "Coefficient preprocessing bytes written to Local SPM"),
      ADD_STAT(coeffPacketOutstandingStalls, statistics::units::Cycle::get(),
               "Coefficient packet issue stalls from outstanding limit"),
      ADD_STAT(coeffPacketPortWaitCycles, statistics::units::Cycle::get(),
               "Coefficient packet wait cycles for Local SPM ports"),
      ADD_STAT(coeffPacketBankWaitCycles, statistics::units::Cycle::get(),
               "Coefficient packet wait cycles for Local SPM banks"),
      ADD_STAT(coeffPacketBankAccesses, statistics::units::Count::get(),
               "Coefficient packet accesses by Local SPM bank"),
      ADD_STAT(coeffPacketBankConflicts, statistics::units::Count::get(),
               "Coefficient packet conflicts by Local SPM bank"),
      ADD_STAT(coeffPacketBankWaitByBank, statistics::units::Cycle::get(),
               "Coefficient packet wait cycles by Local SPM bank"),
      ADD_STAT(coeffPacketBankMaxWait, statistics::units::Cycle::get(),
               "Maximum coefficient packet wait by Local SPM bank"),
      ADD_STAT(coeffPacketMatrixAccesses, statistics::units::Count::get(),
               "Coefficient packet accesses by matrix kind"),
      ADD_STAT(coeffPacketMatrixConflicts, statistics::units::Count::get(),
               "Coefficient packet conflicts by matrix kind"),
      ADD_STAT(coeffPacketMatrixWaitCycles, statistics::units::Cycle::get(),
               "Coefficient packet wait cycles by matrix kind"),
      ADD_STAT(coeffDmaIssued, statistics::units::Count::get(),
               "Stage 6b timing DMA packets accepted"),
      ADD_STAT(coeffDmaCompleted, statistics::units::Count::get(),
               "Stage 6b timing DMA responses completed"),
      ADD_STAT(coeffDmaReadBytes, statistics::units::Byte::get(),
               "Bytes read from guest memory by coefficient DMA"),
      ADD_STAT(coeffDmaWriteBytes, statistics::units::Byte::get(),
               "Bytes written to guest memory by coefficient DMA"),
      ADD_STAT(coeffDmaOutstandingStalls, statistics::units::Count::get(),
               "DMA submissions rejected by the outstanding limit"),
      ADD_STAT(coeffDmaPortRetries, statistics::units::Count::get(),
               "DMA timing requests retried after port backpressure"),
      ADD_STAT(coeffDmaTranslationFailures, statistics::units::Count::get(),
               "DMA guest virtual-address translation failures"),
      ADD_STAT(coeffDmaLiveMax, statistics::units::Count::get(),
               "Maximum queued plus in-flight coefficient DMA packets"),
      ADD_STAT(coeffRequestsDelayedByPathA, statistics::units::Cycle::get(),
               "Coefficient packet cycles delayed by Path A"),
      ADD_STAT(coeffRequestsDelayedByTrsv5, statistics::units::Cycle::get(),
               "Coefficient packet cycles delayed by TRSV5"),
      ADD_STAT(pathARequestsDelayedByCoeff, statistics::units::Cycle::get(),
               "Path A request cycles delayed by coefficient packets"),
      ADD_STAT(pathACoeffConflictCycles, statistics::units::Cycle::get(),
               "Shared Local SPM Path A/coefficient conflict cycles"),
      ADD_STAT(trsvCoeffConflictCycles, statistics::units::Cycle::get(),
               "Shared Local SPM TRSV5/coefficient conflict cycles"),
      ADD_STAT(vectorZwbRequests, statistics::units::Count::get(),
               "Unified vector writeback requests observed by CFD model"),
      ADD_STAT(vectorZwbLmatRequests, statistics::units::Count::get(),
               "Vector writeback requests from lmat5_spm local SPM reads"),
      ADD_STAT(vectorZwbMovaRequests, statistics::units::Count::get(),
               "Vector writeback requests from SME MOVA"),
      ADD_STAT(vectorZwbConflicts, statistics::units::Count::get(),
               "Unified vector writeback conflicts observed"),
      ADD_STAT(vectorZwbConflictCycles, statistics::units::Cycle::get(),
               "Unified vector writeback conflict cycles observed"),
      ADD_STAT(vectorZwbWaitTotal, statistics::units::Cycle::get(),
               "Total unified vector writeback wait cycles"),
      ADD_STAT(vectorZwbAvgWait,
               "Average unified vector writeback wait cycles"),
      ADD_STAT(vectorZwbMaxWait, statistics::units::Cycle::get(),
               "Maximum unified vector writeback wait cycles"),
      ADD_STAT(pathAResultBufferAllocs, statistics::units::Count::get(),
               "Path A internal result buffer slot allocations"),
      ADD_STAT(pathAResultBufferFrees, statistics::units::Count::get(),
               "Path A internal result buffer slot frees"),
      ADD_STAT(pathAResultBufferWrites, statistics::units::Count::get(),
               "Path A dotp_row writes into internal result buffer"),
      ADD_STAT(pathAResultBufferPacks, statistics::units::Count::get(),
               "Path A pack_acc reads from internal result buffer"),
      ADD_STAT(pathAFusedSubOps, statistics::units::Count::get(),
               "Path A pack-sub fused vector updates"),
      ADD_STAT(pathAResultBufferFullStalls, statistics::units::Count::get(),
               "Path A modeled internal result buffer full/reuse hazards"),
      ADD_STAT(pathAResultBufferLiveMax, statistics::units::Count::get(),
               "Maximum live Path A internal result buffer slots"),
      ADD_STAT(pathAResultBufferLiveEnd, statistics::units::Count::get(),
               "Live Path A internal result buffer slots at stats sample"),
      ADD_STAT(pathAResultBufferOverlapCycles, statistics::units::Cycle::get(),
               "Path A cycles/events with at least two live result buffer slots"),
      ADD_STAT(pathAResultBufferPackWhileDotpCycles,
               statistics::units::Cycle::get(),
               "Path A pack events while another result buffer slot remains live"),
      ADD_STAT(pathADotpIssued, statistics::units::Count::get(),
               "Path A dotp_row execute/issue observations"),
      ADD_STAT(pathADotpIssueCycles, statistics::units::Cycle::get(),
               "Path A cycles with at least one observed dotp_row issue"),
      ADD_STAT(pathADotpIssueGaps, statistics::units::Cycle::get(),
               "Path A idle cycles between consecutive dotp_row issues"),
      ADD_STAT(pathAMaxDotpIssueGap, statistics::units::Cycle::get(),
               "Path A maximum gap between consecutive dotp_row issues"),
      ADD_STAT(pathAConsecutiveDotpBurstTotal, statistics::units::Count::get(),
               "Path A total dotp_row instructions in consecutive bursts"),
      ADD_STAT(pathAConsecutiveDotpBurstMax, statistics::units::Count::get(),
               "Path A maximum consecutive dotp_row burst length"),
      ADD_STAT(pathAConsecutiveDotpBurstAvg,
               "Path A average consecutive dotp_row burst length"),
      ADD_STAT(pathADotpBlockedByFuBusy, statistics::units::Count::get(),
               "Path A modeled dotp stalls due to FU busy"),
      ADD_STAT(pathADotpBlockedByInputNotReady, statistics::units::Count::get(),
               "Path A modeled dotp stalls due to input not ready"),
      ADD_STAT(pathADotpBlockedByResultBufferFull,
               statistics::units::Count::get(),
               "Path A modeled dotp stalls due to result buffer full"),
      ADD_STAT(pathADotpBlockedByPackStoreBackpressure,
               statistics::units::Count::get(),
               "Path A modeled dotp stalls due to pack/store backpressure"),
      ADD_STAT(pathADotpReadyButNotIssued, statistics::units::Count::get(),
               "Path A modeled ready dotp cycles not issued"),
      ADD_STAT(pathAEarlyDotpStartEvents, statistics::units::Count::get(),
               "Path A dotp_stream issued with only vector/current row ready"),
      ADD_STAT(pathADotpIssuedBeforeAllFieldsReady,
               statistics::units::Count::get(),
               "Path A dotp_stream issued before all six input fields ready"),
      ADD_STAT(pathAEarlyReadyOpportunities,
               statistics::units::Count::get(),
               "Path A vector/current-row ready opportunities before full slot"),
      ADD_STAT(pathAEarlyReadyIssued,
               statistics::units::Count::get(),
               "Path A dotp observations whose dependencies were ready before full slot"),
      ADD_STAT(pathAEarlyReadyExecuteStarted,
               statistics::units::Count::get(),
               "Path A dotp execute observations before full slot ready"),
      ADD_STAT(pathAEarlyReadyMissed,
               statistics::units::Count::get(),
               "Path A early-ready opportunities not converted to before-full-slot execute"),
      ADD_STAT(pathAEarlyReadyIssueEfficiency,
               "Path A early-ready issue/execute efficiency"),
      ADD_STAT(pathAEarlyMissedDueToNotInIQ,
               statistics::units::Count::get(),
               "Path A early-ready misses attributed to late IQ visibility"),
      ADD_STAT(pathAEarlyMissedDueToDotpFuBusy,
               statistics::units::Count::get(),
               "Path A early-ready misses attributed to dotp FU pressure"),
      ADD_STAT(pathAEarlyMissedDueToIssueWidth,
               statistics::units::Count::get(),
               "Path A early-ready misses attributed to global issue width"),
      ADD_STAT(pathAEarlyMissedDueToResultSlotBusy,
               statistics::units::Count::get(),
               "Path A early-ready misses attributed to result slot pressure"),
      ADD_STAT(pathAEarlyMissedDueToTokenNotReady,
               statistics::units::Count::get(),
               "Path A early-ready misses attributed to token readiness"),
      ADD_STAT(pathAEarlyMissedDueToOther,
               statistics::units::Count::get(),
               "Path A early-ready misses without a more specific local cause"),
      ADD_STAT(pathADotpDependencyReadyBeforeAllFields,
               statistics::units::Count::get(),
               "Path A dotp dependencies became ready before all fields"),
      ADD_STAT(pathADotpExecuteStartedBeforeAllFields,
               statistics::units::Count::get(),
               "Path A dotp execute started before all fields"),
      ADD_STAT(pathADotpCompletedBeforeAllFields,
               statistics::units::Count::get(),
               "Path A dotp estimated complete before all fields"),
      ADD_STAT(pathALoadComputeOverlapCycles, statistics::units::Cycle::get(),
               "Path A observed load/compute overlap events"),
      ADD_STAT(pathAFieldReadyWaitCycles, statistics::units::Cycle::get(),
               "Path A current row/vector ready-to-dotp wait cycles"),
      ADD_STAT(pathAWholeSlotBarrierStallCycles,
               statistics::units::Cycle::get(),
               "Path A lane0 events that still looked whole-slot gated"),
      ADD_STAT(pathAStreamReadPort0BusyCycles,
               statistics::units::Cycle::get(),
               "Path A stream load observations using local read port 0"),
      ADD_STAT(pathAStreamReadPort1BusyCycles,
               statistics::units::Cycle::get(),
               "Path A stream load observations using local read port 1"),
      ADD_STAT(pathADualLoadIssueCycles, statistics::units::Cycle::get(),
               "Path A cycles with two stream loads observed"),
      ADD_STAT(pathADotpAfterFieldReadyTotal,
               statistics::units::Cycle::get(),
               "Path A dotp issue delay after current row became ready"),
      ADD_STAT(pathADotpAfterVectorReadyTotal,
               statistics::units::Cycle::get(),
               "Path A dotp issue delay after vector became ready"),
      ADD_STAT(pathADotpAvgAfterFieldReady,
               "Path A average dotp delay after current row ready"),
      ADD_STAT(pathADotpAvgAfterVectorReady,
               "Path A average dotp delay after vector ready"),
      ADD_STAT(pathAInputBufferAllocs, statistics::units::Count::get(),
               "Path A streaming input buffer allocations"),
      ADD_STAT(pathAInputBufferFrees, statistics::units::Count::get(),
               "Path A streaming input buffer frees"),
      ADD_STAT(pathAInputBufferFullStalls, statistics::units::Count::get(),
               "Path A streaming input buffer full stalls"),
      ADD_STAT(pathAInputBufferLiveMax, statistics::units::Count::get(),
               "Path A streaming input buffer maximum live entries"),
      ADD_STAT(pathAInputBufferLiveEnd, statistics::units::Count::get(),
               "Path A streaming input buffer live entries at stats sample"),
      ADD_STAT(pathAAsyncStoreEnqueues, statistics::units::Count::get(),
               "Path A async store queue enqueues"),
      ADD_STAT(pathAAsyncStoreCommits, statistics::units::Count::get(),
               "Path A async store queue commits"),
      ADD_STAT(pathAAsyncStoreQueueFullStalls, statistics::units::Count::get(),
               "Path A async store queue full stalls"),
      ADD_STAT(pathAAsyncStoreLiveMax, statistics::units::Count::get(),
               "Path A async store queue maximum live entries"),
      ADD_STAT(pathAAsyncStoreLiveEnd, statistics::units::Count::get(),
               "Path A async store queue live entries at stats sample"),
      ADD_STAT(pathCEarlyOuterStartEvents, statistics::units::Count::get(),
               "Path C outer step0 issued before the whole tile was loaded"),
      ADD_STAT(pathCStepIssuedBeforeAllColumnsReady,
               statistics::units::Count::get(),
               "Path C outer step issued before all columns were ready"),
      ADD_STAT(pathCOuterFuIssueCount,
               statistics::units::Count::get(),
               "Path C local outer FU issue observations"),
      ADD_STAT(pathCOuterFuBusyCycles,
               statistics::units::Cycle::get(),
               "Path C local outer FU busy cycles"),
      ADD_STAT(pathCOuterFuUtilization,
               "Path C local outer FU utilization over simulated cycles"),
      ADD_STAT(pathCLoadOuterOverlapCycles, statistics::units::Cycle::get(),
               "Path C observed lmat/outer overlap events"),
      ADD_STAT(pathCColumnReadyWaitCycles, statistics::units::Cycle::get(),
               "Path C current column/vector ready-to-step wait cycles"),
      ADD_STAT(pathCZaTokenWaitCycles, statistics::units::Cycle::get(),
               "Path C modeled wait for previous ZA token"),
      ADD_STAT(pathCMovaBoundaryWaitCycles, statistics::units::Cycle::get(),
               "Path C MOVA/boundary wait cycles"),
      ADD_STAT(pathCDualLoadIssueCycles, statistics::units::Cycle::get(),
               "Path C cycles with two lmat loads observed"),
      ADD_STAT(pathCLmatReadPort0BusyCycles,
               statistics::units::Cycle::get(),
               "Path C lmat observations using local read port 0"),
      ADD_STAT(pathCLmatReadPort1BusyCycles,
               statistics::units::Cycle::get(),
               "Path C lmat observations using local read port 1"),
      ADD_STAT(pathCStepAfterColumnReadyTotal,
               statistics::units::Cycle::get(),
               "Path C outer step delay after current column became ready"),
      ADD_STAT(pathCStepAfterVectorReadyTotal,
               statistics::units::Cycle::get(),
               "Path C outer step delay after vector became ready"),
      ADD_STAT(pathCStepAvgAfterColumnReady,
               "Path C average outer delay after current column ready"),
      ADD_STAT(pathCStepAvgAfterVectorReady,
               "Path C average outer delay after vector ready"),
      ADD_STAT(trsv5Issued, statistics::units::Count::get(),
               "TRSV5 instructions observed at execute"),
      ADD_STAT(trsv5Completed, statistics::units::Count::get(),
               "TRSV5 instructions completed by the coarse execute model"),
      ADD_STAT(trsv5Squashed, statistics::units::Count::get(),
               "TRSV5 instructions squashed after execute"),
      ADD_STAT(trsv5BusyCycles, statistics::units::Cycle::get(),
               "TRSV5 configured FU busy cycles accumulated per solve"),
      ADD_STAT(trsv5IdleCycles, statistics::units::Cycle::get(),
               "TRSV5 idle cycles observed by this coarse model"),
      ADD_STAT(trsv5FullStallCycles, statistics::units::Cycle::get(),
               "TRSV5 full/context stall cycles observed by this coarse model"),
      ADD_STAT(trsv5DivOps, statistics::units::Count::get(),
               "TRSV5 modeled FP64 divide operations"),
      ADD_STAT(trsv5ForwardMulSubOps, statistics::units::Count::get(),
               "TRSV5 modeled forward substitution multiply-subtract updates"),
      ADD_STAT(trsv5BackwardMulSubOps, statistics::units::Count::get(),
               "TRSV5 modeled backward substitution multiply-subtract updates"),
      ADD_STAT(trsv5TotalMulSubOps, statistics::units::Count::get(),
               "TRSV5 modeled total multiply-subtract updates"),
      ADD_STAT(trsv5InputMatrixBytes, statistics::units::Byte::get(),
               "TRSV5 LU matrix bytes read functionally"),
      ADD_STAT(trsv5InputVectorBytes, statistics::units::Byte::get(),
               "TRSV5 RHS vector bytes read functionally"),
      ADD_STAT(trsv5OutputBytes, statistics::units::Byte::get(),
               "TRSV5 result bytes produced to Z register"),
      ADD_STAT(trsv5RhsForwarded, statistics::units::Count::get(),
               "TRSV5 solves whose RHS came from a forwarded Z register"),
      ADD_STAT(trsv5RhsSpmStageElided, statistics::units::Count::get(),
               "TRSV5 RHS SPM staging operations elided by forwarding"),
      ADD_STAT(trsv5RhsForwardStallCycles, statistics::units::Cycle::get(),
               "TRSV5 RHS forwarding wait/stall cycles observed"),
      ADD_STAT(trsv5RhsForwardInvalid, statistics::units::Count::get(),
               "TRSV5 RHS forwarding invalid-source events"),
      ADD_STAT(trsv5RhsForwardConsumed, statistics::units::Count::get(),
               "TRSV5 forwarded RHS vectors consumed"),
      ADD_STAT(trsv5LatencyConfigured, statistics::units::Cycle::get(),
               "TRSV5 configured O3-visible FU latency"),
      ADD_STAT(trsv5ObservedLatencyTotal, statistics::units::Cycle::get(),
               "TRSV5 total coarse observed latency"),
      ADD_STAT(trsv5AverageObservedLatency,
               "TRSV5 average coarse observed latency"),
      ADD_STAT(trsv5MaxInFlight, statistics::units::Count::get(),
               "TRSV5 maximum in-flight operations observed by this model"),
      ADD_STAT(trsm5MrhsIssued, statistics::units::Count::get(),
               "TRSM5 multi-RHS preprocessing instructions executed"),
      ADD_STAT(trsm5MrhsCompleted, statistics::units::Count::get(),
               "TRSM5 multi-RHS preprocessing instructions completed"),
      ADD_STAT(trsm5MrhsInvalid, statistics::units::Count::get(),
               "TRSM5 multi-RHS invalid input or output events"),
      ADD_STAT(trsm5MrhsBusyCycles, statistics::units::Cycle::get(),
               "TRSM5 multi-RHS configured busy cycles"),
      ADD_STAT(trsm5MrhsStallCycles, statistics::units::Cycle::get(),
               "TRSM5 multi-RHS modeled stall cycles"),
      ADD_STAT(trsm5MrhsColumnsSolved, statistics::units::Count::get(),
               "TRSM5 multi-RHS independent columns solved"),
      ADD_STAT(trsm5MrhsInputLuBytes, statistics::units::Byte::get(),
               "TRSM5 multi-RHS LU input bytes"),
      ADD_STAT(trsm5MrhsInputRhsBytes, statistics::units::Byte::get(),
               "TRSM5 multi-RHS matrix RHS input bytes"),
      ADD_STAT(trsm5MrhsOutputBytes, statistics::units::Byte::get(),
               "TRSM5 multi-RHS matrix output bytes"),
      ADD_STAT(trsm5MrhsLatencyConfigured, statistics::units::Cycle::get(),
               "TRSM5 multi-RHS configured O3-visible latency"),
      ADD_STAT(trsm5InvLbarIssued, statistics::units::Count::get(),
               "TRSM5 INV/LBAR dual-batch instructions executed"),
      ADD_STAT(trsm5InvLbarCompleted, statistics::units::Count::get(),
               "TRSM5 INV/LBAR dual-batch instructions completed"),
      ADD_STAT(trsm5InvLbarBusyCycles, statistics::units::Cycle::get(),
               "TRSM5 INV/LBAR configured busy cycles"),
      ADD_STAT(trsm5InvLbarStallCycles, statistics::units::Cycle::get(),
               "TRSM5 INV/LBAR modeled stall cycles"),
      ADD_STAT(trsm5InvLbarLuReads, statistics::units::Count::get(),
               "TRSM5 INV/LBAR packed-LU reads"),
      ADD_STAT(trsm5InvLbarColumnsSolved, statistics::units::Count::get(),
               "TRSM5 INV/LBAR columns solved"),
      ADD_STAT(trsm5InvLbarInputLuBytes, statistics::units::Byte::get(),
               "TRSM5 INV/LBAR LU input bytes"),
      ADD_STAT(trsm5InvLbarInputCBytes, statistics::units::Byte::get(),
               "TRSM5 INV/LBAR C input bytes"),
      ADD_STAT(trsm5InvLbarOutputBytes, statistics::units::Byte::get(),
               "TRSM5 INV/LBAR D_inv/L_bar output bytes"),
      ADD_STAT(trsm5InvLbarLatencyConfigured,
               statistics::units::Cycle::get(),
               "TRSM5 INV/LBAR configured O3-visible latency"),
      ADD_STAT(trsm5Coeff3Issued, statistics::units::Count::get(),
               "TRSM5 three-batch coefficient instructions executed"),
      ADD_STAT(trsm5Coeff3Completed, statistics::units::Count::get(),
               "TRSM5 three-batch coefficient instructions completed"),
      ADD_STAT(trsm5Coeff3BusyCycles, statistics::units::Cycle::get(),
               "TRSM5 three-batch configured busy cycles"),
      ADD_STAT(trsm5Coeff3StallCycles, statistics::units::Cycle::get(),
               "TRSM5 three-batch modeled stall cycles"),
      ADD_STAT(trsm5Coeff3LuReads, statistics::units::Count::get(),
               "TRSM5 three-batch packed-LU reads"),
      ADD_STAT(trsm5Coeff3ColumnsSolved, statistics::units::Count::get(),
               "TRSM5 three-batch columns solved"),
      ADD_STAT(trsm5Coeff3InputBytes, statistics::units::Byte::get(),
               "TRSM5 three-batch LU/L/U input bytes"),
      ADD_STAT(trsm5Coeff3OutputBytes, statistics::units::Byte::get(),
               "TRSM5 three-batch D_inv/L_bar/U_bar output bytes"),
      ADD_STAT(trsm5Coeff3LatencyConfigured,
               statistics::units::Cycle::get(),
               "TRSM5 three-batch configured O3-visible latency"),
      ADD_STAT(linebufForwardReads, statistics::units::Count::get(),
               "LU-SGS line buffer forward reads"),
      ADD_STAT(linebufForwardWrites, statistics::units::Count::get(),
               "LU-SGS line buffer forward writes"),
      ADD_STAT(linebufBackwardReads, statistics::units::Count::get(),
               "LU-SGS line buffer backward reads"),
      ADD_STAT(linebufBackwardWrites, statistics::units::Count::get(),
               "LU-SGS line buffer backward writes"),
      ADD_STAT(linebufForwardHits, statistics::units::Count::get(),
               "LU-SGS line buffer forward hits"),
      ADD_STAT(linebufForwardMisses, statistics::units::Count::get(),
               "LU-SGS line buffer forward misses"),
      ADD_STAT(linebufBackwardHits, statistics::units::Count::get(),
               "LU-SGS line buffer backward hits"),
      ADD_STAT(linebufBackwardMisses, statistics::units::Count::get(),
               "LU-SGS line buffer backward misses"),
      ADD_STAT(linebufTagConflicts, statistics::units::Count::get(),
               "LU-SGS line buffer tag conflicts"),
      ADD_STAT(linebufInvalidReads, statistics::units::Count::get(),
               "LU-SGS line buffer invalid reads"),
      ADD_STAT(linebufStallCycles, statistics::units::Cycle::get(),
               "LU-SGS line buffer modeled stall cycles"),
      ADD_STAT(lusgsControllerLaunches, statistics::units::Count::get(),
               "LU-SGS controller launch instructions observed"),
      ADD_STAT(lusgsControllerCommittedLaunches, statistics::units::Count::get(),
               "LU-SGS controller launches executed at non-spec commit"),
      ADD_STAT(lusgsControllerSquashedLaunches, statistics::units::Count::get(),
               "LU-SGS controller launches squashed before execution"),
      ADD_STAT(lusgsControllerCompletedTasks, statistics::units::Count::get(),
               "LU-SGS controller tasks completed"),
      ADD_STAT(lusgsControllerFailedTasks, statistics::units::Count::get(),
               "LU-SGS controller tasks failed validation or arbitration"),
      ADD_STAT(lusgsControllerBusyCycles, statistics::units::Cycle::get(),
               "LU-SGS controller modeled busy cycles"),
      ADD_STAT(lusgsControllerTotalCycles, statistics::units::Cycle::get(),
               "LU-SGS controller modeled total task cycles"),
      ADD_STAT(lusgsControllerForwardCells, statistics::units::Count::get(),
               "LU-SGS controller forward cells processed"),
      ADD_STAT(lusgsControllerBackwardCells, statistics::units::Count::get(),
               "LU-SGS controller backward cells processed"),
      ADD_STAT(lusgsControllerUpdatedQCells, statistics::units::Count::get(),
               "LU-SGS controller Q-update cells processed"),
      ADD_STAT(lusgsControllerPathARequests, statistics::units::Count::get(),
               "LU-SGS controller Path A black-box MVM requests"),
      ADD_STAT(lusgsControllerTrsv5Requests, statistics::units::Count::get(),
               "LU-SGS controller TRSV5 black-box requests"),
      ADD_STAT(lusgsControllerVec5SubRequests, statistics::units::Count::get(),
               "LU-SGS controller Vector5 SUB requests"),
      ADD_STAT(lusgsControllerVec5CopyRequests, statistics::units::Count::get(),
               "LU-SGS controller Vector5 COPY requests"),
      ADD_STAT(lusgsControllerVec5AxpyRequests, statistics::units::Count::get(),
               "LU-SGS controller Vector5 AXPY requests"),
      ADD_STAT(lusgsControllerSpmReadRequests, statistics::units::Count::get(),
               "LU-SGS controller modeled SPM/read requests"),
      ADD_STAT(lusgsControllerSpmWriteRequests, statistics::units::Count::get(),
               "LU-SGS controller modeled SPM/write requests"),
      ADD_STAT(lusgsControllerPathAWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS controller cycles waiting for Path A resources"),
      ADD_STAT(lusgsControllerTrsv5WaitCycles, statistics::units::Cycle::get(),
               "LU-SGS controller cycles waiting for TRSV5 resources"),
      ADD_STAT(lusgsControllerVec5WaitCycles, statistics::units::Cycle::get(),
               "LU-SGS controller cycles waiting for Vector5 resources"),
      ADD_STAT(lusgsControllerSpmWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS controller cycles waiting for SPM/load resources"),
      ADD_STAT(lusgsControllerWritebackWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS controller cycles waiting for writeback"),
      ADD_STAT(lusgsControllerForwardDependencyStalls,
               statistics::units::Cycle::get(),
               "LU-SGS controller forward dependency stall cycles"),
      ADD_STAT(lusgsControllerBackwardDependencyStalls,
               statistics::units::Cycle::get(),
               "LU-SGS controller backward dependency stall cycles"),
      ADD_STAT(lusgsControllerContextFullStalls, statistics::units::Cycle::get(),
               "LU-SGS controller context-full stall cycles"),
      ADD_STAT(lusgsControllerNoReadyContextCycles,
               statistics::units::Cycle::get(),
               "LU-SGS controller no-ready-context cycles"),
      ADD_STAT(lusgsControllerArbiterStalls, statistics::units::Cycle::get(),
               "LU-SGS controller arbiter stall cycles"),
      ADD_STAT(lusgsControllerContextAlloc, statistics::units::Count::get(),
               "LU-SGS controller context allocations"),
      ADD_STAT(lusgsControllerContextFree, statistics::units::Count::get(),
               "LU-SGS controller context frees"),
      ADD_STAT(lusgsControllerActiveContextCycles,
               statistics::units::Cycle::get(),
               "LU-SGS controller active-context cycles"),
      ADD_STAT(lusgsControllerMaxActiveContexts,
               statistics::units::Count::get(),
               "LU-SGS controller maximum active contexts"),
      ADD_STAT(lusgsControllerContextSwitches, statistics::units::Count::get(),
               "LU-SGS controller context switches"),
      ADD_STAT(lusgsControllerLuBytes, statistics::units::Byte::get(),
               "LU-SGS controller LU bytes consumed"),
      ADD_STAT(lusgsControllerCBytes, statistics::units::Byte::get(),
               "LU-SGS controller C-matrix bytes consumed"),
      ADD_STAT(lusgsControllerBbarBytes, statistics::units::Byte::get(),
               "LU-SGS controller B_BAR matrix bytes consumed"),
      ADD_STAT(lusgsControllerRhsBytes, statistics::units::Byte::get(),
               "LU-SGS controller RHS bytes consumed"),
      ADD_STAT(lusgsControllerDqstarReadBytes, statistics::units::Byte::get(),
               "LU-SGS controller DQ_STAR bytes read"),
      ADD_STAT(lusgsControllerDqstarWriteBytes, statistics::units::Byte::get(),
               "LU-SGS controller DQ_STAR bytes written"),
      ADD_STAT(lusgsControllerDqReadBytes, statistics::units::Byte::get(),
               "LU-SGS controller DQ bytes read"),
      ADD_STAT(lusgsControllerDqWriteBytes, statistics::units::Byte::get(),
               "LU-SGS controller DQ bytes written"),
      ADD_STAT(lusgsControllerQReadBytes, statistics::units::Byte::get(),
               "LU-SGS controller Q bytes read"),
      ADD_STAT(lusgsControllerQWriteBytes, statistics::units::Byte::get(),
               "LU-SGS controller Q bytes written"),
      ADD_STAT(lusgsControllerTemporaryBytes, statistics::units::Byte::get(),
               "LU-SGS controller temporary result traffic bytes"),
      ADD_STAT(lusgsControllerTemporaryResultStores,
               statistics::units::Count::get(),
               "LU-SGS controller temporary 40B stores"),
      ADD_STAT(lusgsControllerTemporaryResultLoads,
               statistics::units::Count::get(),
               "LU-SGS controller temporary 40B loads"),
      ADD_STAT(lusgsControllerBufferReads, statistics::units::Count::get(),
               "LU-SGS controller private buffer reads"),
      ADD_STAT(lusgsControllerBufferWrites, statistics::units::Count::get(),
               "LU-SGS controller private buffer writes"),
      ADD_STAT(lusgsControllerDqstarExternalWrites,
               statistics::units::Count::get(),
               "LU-SGS controller external DQ_STAR writes"),
      ADD_STAT(lusgsControllerDqstarExternalReads,
               statistics::units::Count::get(),
               "LU-SGS controller external DQ_STAR reads"),
      ADD_STAT(lusgsControllerFinalDqWrites, statistics::units::Count::get(),
               "LU-SGS controller final DQ vector writes"),
      ADD_STAT(lusgsControllerTileCoefficientBytes,
               statistics::units::Byte::get(),
               "LU-SGS controller tile coefficient bytes"),
      ADD_STAT(lusgsControllerTileVectorBytes, statistics::units::Byte::get(),
               "LU-SGS controller tile vector bytes"),
      ADD_STAT(lusgsControllerTileTemporaryBytes,
               statistics::units::Byte::get(),
               "LU-SGS controller tile temporary bytes"),
      ADD_STAT(lusgsControllerTileTotalBytes, statistics::units::Byte::get(),
               "LU-SGS controller tile total bytes"),
      ADD_STAT(lusgsControllerSpmCapacity, statistics::units::Byte::get(),
               "LU-SGS controller modeled SPM capacity"),
      ADD_STAT(lusgsControllerTileLoads, statistics::units::Count::get(),
               "LU-SGS controller tile loads"),
      ADD_STAT(lusgsControllerTilePrefetches, statistics::units::Count::get(),
               "LU-SGS controller logical tile prefetches"),
      ADD_STAT(lusgsControllerPrefetchUseful, statistics::units::Count::get(),
               "LU-SGS controller useful logical prefetches"),
      ADD_STAT(lusgsControllerPrefetchLate, statistics::units::Count::get(),
               "LU-SGS controller late logical prefetches"),
      ADD_STAT(lusgsControllerBufferSwap, statistics::units::Count::get(),
               "LU-SGS controller logical SPM buffer swaps"),
      ADD_STAT(lusgsControllerBufferConflictStalls,
               statistics::units::Cycle::get(),
               "LU-SGS controller buffer-conflict stall cycles"),
      ADD_STAT(pathaArbiterCpuRequests, statistics::units::Count::get(),
               "Path A arbiter CPU-side requests observed"),
      ADD_STAT(pathaArbiterLusgsRequests, statistics::units::Count::get(),
               "Path A arbiter LU-SGS controller requests observed"),
      ADD_STAT(pathaArbiterBusyStalls, statistics::units::Cycle::get(),
               "Path A arbiter busy stall cycles"),
      ADD_STAT(pathaArbiterOwnershipCycles, statistics::units::Cycle::get(),
               "Path A arbiter LU-SGS ownership cycles"),
      ADD_STAT(lusgsEventLaunches, statistics::units::Count::get(),
               "LU-SGS Step4 event controller launches accepted"),
      ADD_STAT(lusgsEventCompleted, statistics::units::Count::get(),
               "LU-SGS Step4 event controller tasks completed"),
      ADD_STAT(lusgsEventFailed, statistics::units::Count::get(),
               "LU-SGS Step4 event controller tasks failed"),
      ADD_STAT(lusgsEventActualCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 actual event cycles from launch to completion"),
      ADD_STAT(lusgsEventEventsProcessed, statistics::units::Count::get(),
               "LU-SGS Step4 event callbacks processed"),
      ADD_STAT(lusgsEventSchedulerTicks, statistics::units::Cycle::get(),
               "LU-SGS Step4 scheduler tick events processed"),
      ADD_STAT(lusgsEventActiveCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 cycles with an active task"),
      ADD_STAT(lusgsEventIdleCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 cycles with no active work"),
      ADD_STAT(lusgsEventDecodedLaunches, statistics::units::Count::get(),
               "LU-SGS Step4 launch instructions decoded/observed"),
      ADD_STAT(lusgsEventCommittedLaunches, statistics::units::Count::get(),
               "LU-SGS Step4 launch instructions committed"),
      ADD_STAT(lusgsEventSquashedLaunches, statistics::units::Count::get(),
               "LU-SGS Step4 launch instructions squashed before commit"),
      ADD_STAT(lusgsEventSquashedWaits, statistics::units::Count::get(),
               "LU-SGS Step4 wait instructions squashed before commit"),
      ADD_STAT(lusgsEventStaleCompletions, statistics::units::Count::get(),
               "LU-SGS Step4 stale completion events discarded"),
      ADD_STAT(lusgsEventUnexpectedCompletions,
               statistics::units::Count::get(),
               "LU-SGS Step4 unexpected completion events"),
      ADD_STAT(lusgsEventDuplicateCompletions,
               statistics::units::Count::get(),
               "LU-SGS Step4 duplicate completion events"),
      ADD_STAT(lusgsEventRequestIdMismatches,
               statistics::units::Count::get(),
               "LU-SGS Step4 completion request-id mismatches"),
      ADD_STAT(lusgsEventGenerationMismatches,
               statistics::units::Count::get(),
               "LU-SGS Step4 completion task-generation mismatches"),
      ADD_STAT(lusgsEventWrongStateCompletions,
               statistics::units::Count::get(),
               "LU-SGS Step4 completions received in the wrong state"),
      ADD_STAT(lusgsEventPendingRequestAlloc,
               statistics::units::Count::get(),
               "LU-SGS Step4 pending request entries allocated"),
      ADD_STAT(lusgsEventPendingRequestFree,
               statistics::units::Count::get(),
               "LU-SGS Step4 pending request entries freed"),
      ADD_STAT(lusgsEventPendingRequestFullStalls,
               statistics::units::Count::get(),
               "LU-SGS Step4 pending request table full stalls"),
      ADD_STAT(lusgsEventMaxPendingRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4 maximum live pending requests"),
      ADD_STAT(lusgsEventCancelledRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4 pending requests cancelled"),
      ADD_STAT(lusgsEventLifecycleIdleCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 lifecycle Idle cycles"),
      ADD_STAT(lusgsEventLifecycleQueuedCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 lifecycle Queued cycles"),
      ADD_STAT(lusgsEventLifecycleRunningCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 lifecycle Running cycles"),
      ADD_STAT(lusgsEventLifecycleCompletedNotReapedCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 lifecycle CompletedNotReaped cycles"),
      ADD_STAT(lusgsEventLifecycleErrorNotReapedCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 lifecycle ErrorNotReaped cycles"),
      ADD_STAT(lusgsEventTokensReaped, statistics::units::Count::get(),
               "LU-SGS Step4 completion/error tokens reaped by wait"),
      ADD_STAT(lusgsEventWaitInstructions, statistics::units::Count::get(),
               "LU-SGS Step4 wait/poll instructions committed"),
      ADD_STAT(lusgsEventBusyPolls, statistics::units::Count::get(),
               "LU-SGS Step4 wait instructions that returned Busy"),
      ADD_STAT(lusgsEventSuccessfulWaits, statistics::units::Count::get(),
               "LU-SGS Step4 waits that reaped a completed task"),
      ADD_STAT(lusgsEventErrorWaits, statistics::units::Count::get(),
               "LU-SGS Step4 waits that reaped an errored task"),
      ADD_STAT(lusgsEventBadTokenWaits, statistics::units::Count::get(),
               "LU-SGS Step4 waits rejected as BadToken"),
      ADD_STAT(lusgsEventTokenReapCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 cycles from task completion to token reap"),
      ADD_STAT(lusgsEventLaunchCommitTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 launch commit tick"),
      ADD_STAT(lusgsEventDescriptorSnapshotTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 descriptor snapshot tick"),
      ADD_STAT(lusgsEventFirstResourceIssueTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 first non-descriptor resource issue tick"),
      ADD_STAT(lusgsEventTaskCompleteTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 task completion tick"),
      ADD_STAT(lusgsEventSuccessfulWaitTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 successful wait tick"),
      ADD_STAT(lusgsEventTokenReapTick,
               statistics::units::Tick::get(),
               "LU-SGS Step4 token reap tick"),
      ADD_STAT(lusgsEventWatchdogTimeouts,
               statistics::units::Count::get(),
               "LU-SGS Step4 watchdog timeouts"),
      ADD_STAT(lusgsEventStateTransitions,
               statistics::units::Count::get(),
               "LU-SGS Step4 state/lifecycle transitions"),
      ADD_STAT(lusgsEventNoProgressCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 no-progress cycles at watchdog trip"),
      ADD_STAT(lusgsEventDoubleScheduleErrors,
               statistics::units::Count::get(),
               "LU-SGS Step4 duplicate schedule attempts"),
      ADD_STAT(lusgsEventNoReadyContextCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 no-ready-context cycles"),
      ADD_STAT(lusgsEventPathaRequests, statistics::units::Count::get(),
               "LU-SGS Step4 Path A requests issued"),
      ADD_STAT(lusgsEventPathaAccepted, statistics::units::Count::get(),
               "LU-SGS Step4 Path A requests accepted"),
      ADD_STAT(lusgsEventPathaRetries, statistics::units::Count::get(),
               "LU-SGS Step4 Path A request retries"),
      ADD_STAT(lusgsEventPathaCompleted, statistics::units::Count::get(),
               "LU-SGS Step4 Path A requests completed"),
      ADD_STAT(lusgsEventPathaWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 Path A wait cycles"),
      ADD_STAT(lusgsEventPathaSlotFullStalls,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 Path A result-slot full stalls"),
      ADD_STAT(lusgsEventPathaPackStalls, statistics::units::Cycle::get(),
               "LU-SGS Step4 Path A pack stalls"),
      ADD_STAT(lusgsEventPathaStoreStalls, statistics::units::Cycle::get(),
               "LU-SGS Step4 Path A store stalls"),
      ADD_STAT(lusgsEventPathaMvmRequests, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A parent MVM requests"),
      ADD_STAT(lusgsEventPathaMvmAccepted, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A parent MVM requests accepted"),
      ADD_STAT(lusgsEventPathaMvmRetries, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A parent MVM request retries"),
      ADD_STAT(lusgsEventPathaMvmCompleted, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A parent MVM completions"),
      ADD_STAT(lusgsEventPathaMvmWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A parent MVM wait cycles"),
      ADD_STAT(lusgsEventPathaMatLdRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal MatLd requests"),
      ADD_STAT(lusgsEventPathaMatLdAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal MatLd accepted"),
      ADD_STAT(lusgsEventPathaMatLdRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal MatLd retries"),
      ADD_STAT(lusgsEventPathaMatLdCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal MatLd completions"),
      ADD_STAT(lusgsEventPathaMatLdBusyCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A internal MatLd busy cycles"),
      ADD_STAT(lusgsEventPathaMatLdBytes, statistics::units::Byte::get(),
               "LU-SGS Step4-B Path A internal MatLd bytes"),
      ADD_STAT(lusgsEventPathaDotpRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Dotp requests"),
      ADD_STAT(lusgsEventPathaDotpAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Dotp accepted"),
      ADD_STAT(lusgsEventPathaDotpRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Dotp retries"),
      ADD_STAT(lusgsEventPathaDotpCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Dotp completions"),
      ADD_STAT(lusgsEventPathaDotpBusyCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A internal Dotp busy cycles"),
      ADD_STAT(lusgsEventPathaDotpPipelineOccupancy,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A internal Dotp pipeline occupancy"),
      ADD_STAT(lusgsEventPathaPackRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Pack requests"),
      ADD_STAT(lusgsEventPathaPackAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Pack accepted"),
      ADD_STAT(lusgsEventPathaPackRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Pack retries"),
      ADD_STAT(lusgsEventPathaPackCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A internal Pack completions"),
      ADD_STAT(lusgsEventPathaPackBusyCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A internal Pack busy cycles"),
      ADD_STAT(lusgsEventPathaSlotAlloc, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A result slot allocations"),
      ADD_STAT(lusgsEventPathaSlotFree, statistics::units::Count::get(),
               "LU-SGS Step4-B Path A result slot frees"),
      ADD_STAT(lusgsEventPathaSlotOverwriteErrors,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A result slot overwrite errors"),
      ADD_STAT(lusgsEventPathaEarlyConsumeErrors,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A early result consume errors"),
      ADD_STAT(lusgsEventPathaResultTransfers,
               statistics::units::Count::get(),
               "LU-SGS Step4-B Path A 40B result-ready transfers"),
      ADD_STAT(lusgsEventPathaResultBytes, statistics::units::Byte::get(),
               "LU-SGS Step4-B Path A result-ready bytes"),
      ADD_STAT(lusgsEventPathaLoadPhaseCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A load phase cycles"),
      ADD_STAT(lusgsEventPathaDotpPhaseCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A dotp phase cycles"),
      ADD_STAT(lusgsEventPathaPackPhaseCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A pack phase cycles"),
      ADD_STAT(lusgsEventPathaResultPhaseCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A result phase cycles"),
      ADD_STAT(lusgsEventPathaAverageMvmLatency,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A average MVM latency"),
      ADD_STAT(lusgsEventPathaMinMvmLatency,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A minimum MVM latency"),
      ADD_STAT(lusgsEventPathaMaxMvmLatency,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-B Path A maximum MVM latency"),
      ADD_STAT(lusgsEventTrsvRequests, statistics::units::Count::get(),
               "LU-SGS Step4 TRSV5 requests issued"),
      ADD_STAT(lusgsEventTrsvAccepted, statistics::units::Count::get(),
               "LU-SGS Step4 TRSV5 requests accepted"),
      ADD_STAT(lusgsEventTrsvRetries, statistics::units::Count::get(),
               "LU-SGS Step4 TRSV5 request retries"),
      ADD_STAT(lusgsEventTrsvCompleted, statistics::units::Count::get(),
               "LU-SGS Step4 TRSV5 requests completed"),
      ADD_STAT(lusgsEventTrsvWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 TRSV5 wait cycles"),
      ADD_STAT(lusgsEventTrsvBusyCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 TRSV5 busy cycles"),
      ADD_STAT(lusgsEventTrsvQueueFullStalls,
               statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 queue-full stalls"),
      ADD_STAT(lusgsEventTrsvMaxQueueDepth,
               statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 maximum live queue depth"),
      ADD_STAT(lusgsEventTrsvDivRequests, statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 divide operations issued"),
      ADD_STAT(lusgsEventTrsvDivCompleted, statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 divide operations completed"),
      ADD_STAT(lusgsEventTrsvDivBusyCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 divider busy wait cycles"),
      ADD_STAT(lusgsEventTrsvFmaRequests, statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 multiply-subtract operations issued"),
      ADD_STAT(lusgsEventTrsvFmaCompleted, statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 multiply-subtract operations completed"),
      ADD_STAT(lusgsEventTrsvFmaBusyCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 FMA busy wait cycles"),
      ADD_STAT(lusgsEventTrsvForwardCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 forward-stage cycles"),
      ADD_STAT(lusgsEventTrsvBackwardCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 backward-stage cycles"),
      ADD_STAT(lusgsEventTrsvDependencyWaitCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 dependency wait cycles"),
      ADD_STAT(lusgsEventTrsvForwardedResults,
               statistics::units::Count::get(),
               "LU-SGS Step4-C TRSV5 forwarded intermediate results"),
      ADD_STAT(lusgsEventTrsvAverageLatency,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 average transaction latency"),
      ADD_STAT(lusgsEventTrsvMinLatency, statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 minimum transaction latency"),
      ADD_STAT(lusgsEventTrsvMaxLatency, statistics::units::Cycle::get(),
               "LU-SGS Step4-C TRSV5 maximum transaction latency"),
      ADD_STAT(lusgsEventTrsvDividerUtilization,
               statistics::units::Unspecified::get(),
               "LU-SGS Step4-C TRSV5 divider utilization"),
      ADD_STAT(lusgsEventTrsvFmaUtilization,
               statistics::units::Unspecified::get(),
               "LU-SGS Step4-C TRSV5 FMA utilization"),
      ADD_STAT(lusgsEventVec5Requests, statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 requests issued"),
      ADD_STAT(lusgsEventVec5Accepted, statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 requests accepted"),
      ADD_STAT(lusgsEventVec5Retries, statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 request retries"),
      ADD_STAT(lusgsEventVec5Completed, statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 requests completed"),
      ADD_STAT(lusgsEventVec5WaitCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 Vector5 wait cycles"),
      ADD_STAT(lusgsEventVec5QueueFullStalls,
               statistics::units::Count::get(),
               "LU-SGS Step4-C Vector5 queue-full stalls"),
      ADD_STAT(lusgsEventVec5BusyCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4-C Vector5 transaction busy cycles"),
      ADD_STAT(lusgsEventVec5MaxQueueDepth,
               statistics::units::Count::get(),
               "LU-SGS Step4-C Vector5 maximum live queue depth"),
      ADD_STAT(lusgsEventVec5LaneOperations,
               statistics::units::Count::get(),
               "LU-SGS Step4-C Vector5 lane operations completed"),
      ADD_STAT(lusgsEventVec5AverageLatency,
               statistics::units::Cycle::get(),
               "LU-SGS Step4-C Vector5 average request latency"),
      ADD_STAT(lusgsEventVec5LaneUtilization,
               statistics::units::Unspecified::get(),
               "LU-SGS Step4-C Vector5 lane utilization"),
      ADD_STAT(lusgsEventVec5CopyRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 COPY requests issued"),
      ADD_STAT(lusgsEventVec5CopyAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 COPY requests accepted"),
      ADD_STAT(lusgsEventVec5CopyCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 COPY requests completed"),
      ADD_STAT(lusgsEventVec5CopyRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 COPY request retries"),
      ADD_STAT(lusgsEventVec5CopyWaitCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 Vector5 COPY wait cycles"),
      ADD_STAT(lusgsEventVec5SubRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 SUB requests issued"),
      ADD_STAT(lusgsEventVec5SubAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 SUB requests accepted"),
      ADD_STAT(lusgsEventVec5SubCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 SUB requests completed"),
      ADD_STAT(lusgsEventVec5SubRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 SUB request retries"),
      ADD_STAT(lusgsEventVec5SubWaitCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 Vector5 SUB wait cycles"),
      ADD_STAT(lusgsEventVec5AxpyRequests,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 AXPY requests issued"),
      ADD_STAT(lusgsEventVec5AxpyAccepted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 AXPY requests accepted"),
      ADD_STAT(lusgsEventVec5AxpyCompleted,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 AXPY requests completed"),
      ADD_STAT(lusgsEventVec5AxpyRetries,
               statistics::units::Count::get(),
               "LU-SGS Step4 Vector5 AXPY request retries"),
      ADD_STAT(lusgsEventVec5AxpyWaitCycles,
               statistics::units::Cycle::get(),
               "LU-SGS Step4 Vector5 AXPY wait cycles"),
      ADD_STAT(lusgsEventContextAlloc, statistics::units::Count::get(),
               "LU-SGS Step4 context allocations"),
      ADD_STAT(lusgsEventContextFree, statistics::units::Count::get(),
               "LU-SGS Step4 context frees"),
      ADD_STAT(lusgsEventMaxActiveContexts, statistics::units::Count::get(),
               "LU-SGS Step4 maximum active contexts"),
      ADD_STAT(lusgsEventContextSwitches, statistics::units::Count::get(),
               "LU-SGS Step4 context switches"),
      ADD_STAT(lusgsEventContextReadyCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 context-ready cycles"),
      ADD_STAT(lusgsEventContextWaitCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 context wait cycles"),
      ADD_STAT(lusgsEventSpmReads, statistics::units::Count::get(),
               "LU-SGS Step4 event-level SPM read requests"),
      ADD_STAT(lusgsEventSpmWrites, statistics::units::Count::get(),
               "LU-SGS Step4 event-level SPM write requests"),
      ADD_STAT(lusgsEventSpmReadBytes, statistics::units::Byte::get(),
               "LU-SGS Step4 event-level SPM read bytes"),
      ADD_STAT(lusgsEventSpmWriteBytes, statistics::units::Byte::get(),
               "LU-SGS Step4 event-level SPM write bytes"),
      ADD_STAT(lusgsEventSpmPortConflicts, statistics::units::Count::get(),
               "LU-SGS Step4 SPM port conflicts"),
      ADD_STAT(lusgsEventSpmBankConflicts, statistics::units::Count::get(),
               "LU-SGS Step4 SPM bank conflicts"),
      ADD_STAT(lusgsEventSpmQueueFull, statistics::units::Count::get(),
               "LU-SGS Step4 SPM queue-full events"),
      ADD_STAT(lusgsEventSpmRetries, statistics::units::Count::get(),
               "LU-SGS Step4 SPM request retries"),
      ADD_STAT(lusgsEventSpmAverageLatency,
               "LU-SGS Step4 average SPM request latency"),
      ADD_STAT(lusgsEventTileLoads, statistics::units::Count::get(),
               "LU-SGS Step4 tile loads"),
      ADD_STAT(lusgsEventTileLoadCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 tile load cycles"),
      ADD_STAT(lusgsEventTileComputeCycles, statistics::units::Cycle::get(),
               "LU-SGS Step4 tile compute cycles"),
      ADD_STAT(lusgsEventPrefetchUseful, statistics::units::Count::get(),
               "LU-SGS Step4 useful tile prefetches"),
      ADD_STAT(lusgsEventPrefetchLate, statistics::units::Count::get(),
               "LU-SGS Step4 late tile prefetches"),
      ADD_STAT(lusgsEventBufferEmptyStalls, statistics::units::Cycle::get(),
               "LU-SGS Step4 buffer-empty stall cycles"),
      ADD_STAT(lusgsEventBufferFullStalls, statistics::units::Cycle::get(),
               "LU-SGS Step4 buffer-full stall cycles"),
      ADD_STAT(lusgsControllerCyclesPerForwardCell,
               "LU-SGS controller cycles per forward cell"),
      ADD_STAT(lusgsControllerCyclesPerBackwardCell,
               "LU-SGS controller cycles per backward cell"),
      ADD_STAT(lusgsControllerCyclesPerFullCell,
               "LU-SGS controller cycles per full cell"),
      ADD_STAT(lusgsControllerCellsPer1000Cycles,
               "LU-SGS controller cells per 1000 modeled cycles"),
      ADD_STAT(lusgsControllerPathAUtilization,
               "LU-SGS controller Path A utilization"),
      ADD_STAT(lusgsControllerTrsv5Utilization,
               "LU-SGS controller TRSV5 utilization"),
      ADD_STAT(lusgsControllerVec5Utilization,
               "LU-SGS controller Vector5 utilization")
{
    constexpr unsigned MaxTrackedBanks = 64;
    constexpr unsigned MatrixKinds =
        static_cast<unsigned>(CfdSpmMatrix::Count);
    coeffPacketBankAccesses.init(MaxTrackedBanks);
    coeffPacketBankConflicts.init(MaxTrackedBanks);
    coeffPacketBankWaitByBank.init(MaxTrackedBanks);
    coeffPacketBankMaxWait.init(MaxTrackedBanks);
    coeffPacketMatrixAccesses.init(MatrixKinds);
    coeffPacketMatrixConflicts.init(MatrixKinds);
    coeffPacketMatrixWaitCycles.init(MatrixKinds);
    dmaPort.setTimingCallbacks(
        [this](PacketPtr pkt) { return recvDmaTimingResp(pkt); },
        [this] { recvDmaReqRetry(); });
    localSpmInstance = this;
}

Port &
CfdLocalSpm::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma_port")
        return dmaPort;
    return SimObject::getPort(if_name, idx);
}

void
CfdLocalSpm::trySendDma()
{
    if (dmaRetryPacket || dmaPackets.empty())
        return;
    PacketPtr pkt = dmaPackets.front();
    if (dmaPort.sendTimingReq(pkt)) {
        dmaPackets.pop_front();
    } else {
        dmaRetryPacket = pkt;
        dmaPackets.pop_front();
        coeffDmaPortRetries++;
    }
}

void
CfdLocalSpm::recvDmaReqRetry()
{
    if (!dmaRetryPacket)
        return;
    PacketPtr pkt = dmaRetryPacket;
    if (!dmaPort.sendTimingReq(pkt)) {
        coeffDmaPortRetries++;
        return;
    }
    dmaRetryPacket = nullptr;
    trySendDma();
}

bool
CfdLocalSpm::recvDmaTimingResp(PacketPtr pkt)
{
    auto *state = dynamic_cast<DmaSenderState *>(pkt->senderState);
    panic_if(!state, "CFD coefficient DMA response lacks sender state");
    const uint64_t packetId = state->packetId;
    auto completion = std::move(state->completion);
    delete state;
    delete pkt;
    if (dmaLivePackets)
        dmaLivePackets--;
    coeffDmaCompleted++;
    if (completion)
        completion(packetId);
    trySendDma();
    return true;
}

CfdSpmPacketIssue
CfdLocalSpm::issueDma(
    ThreadContext *tc, bool write, Addr vaddr, unsigned bytes,
    uint8_t *data, uint64_t outstanding,
    std::function<void(uint64_t)> completion)
{
    CfdSpmPacketIssue result;
    if (!tc || !tc->getProcessPtr() || !bytes || !data)
        return result;
    const uint64_t limit = std::max<uint64_t>(1, outstanding);
    if (dmaLivePackets >= limit) {
        result.outstandingFull = true;
        coeffDmaOutstandingStalls++;
        return result;
    }
    auto process = tc->getProcessPtr();
    const Addr pageBytes = process->pTable->pageSize();
    if ((vaddr % pageBytes) + bytes > pageBytes) {
        coeffDmaTranslationFailures++;
        return result;
    }
    Addr paddr = 0;
    if (!process->pTable->translate(vaddr, paddr)) {
        coeffDmaTranslationFailures++;
        return result;
    }

    RequestPtr req = std::make_shared<Request>(
        paddr, bytes, Request::PHYSICAL, dmaRequestorId);
    PacketPtr pkt = new Packet(req, write ? MemCmd::WriteReq :
                                            MemCmd::ReadReq);
    pkt->dataStatic(data);
    auto *state = new DmaSenderState;
    state->packetId = nextDmaPacketId++;
    state->completion = std::move(completion);
    pkt->senderState = state;

    result.packetId = state->packetId;
    result.accepted = true;
    dmaPackets.push_back(pkt);
    dmaLivePackets++;
    coeffDmaIssued++;
    if (write)
        coeffDmaWriteBytes += bytes;
    else
        coeffDmaReadBytes += bytes;
    if (dmaLivePackets > coeffDmaLiveMax.value())
        coeffDmaLiveMax = dmaLivePackets;
    trySendDma();
    return result;
}

unsigned
CfdLocalSpm::dmaChunkSize(
    ThreadContext *tc, Addr vaddr, unsigned requested) const
{
    if (!tc || !tc->getProcessPtr() || !requested)
        return 0;
    const Addr pageBytes = tc->getProcessPtr()->pTable->pageSize();
    const Addr blockBytes = system->cacheLineSize();
    return std::min<Addr>({
        requested, pageBytes - vaddr % pageBytes,
        blockBytes - vaddr % blockBytes});
}

void
CfdLocalSpm::recordTrsv5Execute(uint64_t seq_num, uint64_t configured_latency)
{
    (void)seq_num;
    const uint64_t latency = std::max<uint64_t>(1, configured_latency);

    trsv5Issued++;
    trsv5Completed++;
    trsv5BusyCycles += latency;
    trsv5DivOps += 5;
    trsv5ForwardMulSubOps += 10;
    trsv5BackwardMulSubOps += 10;
    trsv5TotalMulSubOps += 20;
    trsv5InputMatrixBytes += 5 * 5 * sizeof(double);
    trsv5InputVectorBytes += 5 * sizeof(double);
    trsv5OutputBytes += 5 * sizeof(double);
    trsv5LatencyConfigured = latency;
    trsv5ObservedLatencyTotal += latency;
    trsv5AverageObservedLatency =
        trsv5Completed.value() ?
        trsv5ObservedLatencyTotal.value() / trsv5Completed.value() : 0;
    if (trsv5MaxInFlight.value() < 1)
        trsv5MaxInFlight = 1;
}

void
CfdLocalSpm::recordTrsv5RhsForwardedExecute(uint64_t seq_num,
                                            uint64_t configured_latency)
{
    (void)seq_num;
    const uint64_t latency = std::max<uint64_t>(1, configured_latency);

    trsv5Issued++;
    trsv5Completed++;
    trsv5BusyCycles += latency;
    trsv5DivOps += 5;
    trsv5ForwardMulSubOps += 10;
    trsv5BackwardMulSubOps += 10;
    trsv5TotalMulSubOps += 20;
    trsv5InputMatrixBytes += 5 * 5 * sizeof(double);
    trsv5OutputBytes += 5 * sizeof(double);
    trsv5RhsForwarded++;
    trsv5RhsSpmStageElided++;
    trsv5RhsForwardConsumed++;
    trsv5LatencyConfigured = latency;
    trsv5ObservedLatencyTotal += latency;
    trsv5AverageObservedLatency =
        trsv5Completed.value() ?
        trsv5ObservedLatencyTotal.value() / trsv5Completed.value() : 0;
    if (trsv5MaxInFlight.value() < 1)
        trsv5MaxInFlight = 1;
}

void
CfdLocalSpm::recordTrsv5RhsForwardStall(uint64_t cycles)
{
    trsv5RhsForwardStallCycles += cycles;
}

void
CfdLocalSpm::recordTrsv5RhsForwardInvalid()
{
    trsv5RhsForwardInvalid++;
}

void
CfdLocalSpm::recordTrsm5MrhsExecute(uint64_t seq_num,
                                    uint64_t configured_latency)
{
    (void)seq_num;
    const uint64_t latency = std::max<uint64_t>(1, configured_latency);
    trsm5MrhsIssued++;
    trsm5MrhsCompleted++;
    trsm5MrhsBusyCycles += latency;
    trsm5MrhsColumnsSolved += 5;
    trsm5MrhsInputLuBytes += 25 * sizeof(double);
    trsm5MrhsInputRhsBytes += 25 * sizeof(double);
    trsm5MrhsOutputBytes += 25 * sizeof(double);
    trsm5MrhsLatencyConfigured = latency;
}

void
CfdLocalSpm::recordTrsm5InvLbarExecute(uint64_t seq_num,
                                       uint64_t configured_latency)
{
    (void)seq_num;
    const uint64_t latency = std::max<uint64_t>(1, configured_latency);
    trsm5InvLbarIssued++;
    trsm5InvLbarCompleted++;
    trsm5InvLbarBusyCycles += latency;
    trsm5InvLbarLuReads++;
    trsm5InvLbarColumnsSolved += 10;
    trsm5InvLbarInputLuBytes += 25 * sizeof(double);
    trsm5InvLbarInputCBytes += 25 * sizeof(double);
    trsm5InvLbarOutputBytes += 50 * sizeof(double);
    trsm5InvLbarLatencyConfigured = latency;
}

void
CfdLocalSpm::recordTrsm5Coeff3Execute(uint64_t seq_num,
                                      uint64_t configured_latency)
{
    (void)seq_num;
    const uint64_t latency = std::max<uint64_t>(1, configured_latency);
    trsm5Coeff3Issued++;
    trsm5Coeff3Completed++;
    trsm5Coeff3BusyCycles += latency;
    trsm5Coeff3LuReads++;
    trsm5Coeff3ColumnsSolved += 15;
    trsm5Coeff3InputBytes += 75 * sizeof(double);
    trsm5Coeff3OutputBytes += 75 * sizeof(double);
    trsm5Coeff3LatencyConfigured = latency;
}

void
CfdLocalSpm::ensureLineBufferState()
{
    if (lineBufferInitialized)
        return;

    uint64_t entries =
        localEnvU64("GEM5_CFD_LUSGS_LINEBUF_ENTRIES", 16);
    entries = std::max<uint64_t>(1, entries);
    lineBufferEntries.assign(entries, LineBufferEntry{});
    lineBufferInitialized = true;
}

void
CfdLocalSpm::writeLineBuffer(bool forward, uint64_t line_id,
                             uint64_t cell_id, const double *value)
{
    ensureLineBufferState();
    if (lineBufferEntries.empty())
        return;

    LineBufferEntry &entry =
        lineBufferEntries[line_id % lineBufferEntries.size()];
    if ((entry.fwdValid || entry.bwdValid) && entry.lineId != line_id)
        linebufTagConflicts++;

    entry.lineId = line_id;
    if (forward) {
        entry.fwdValid = true;
        entry.fwdCellId = cell_id;
        for (int i = 0; i < 5; ++i)
            entry.prevDqStar[i] = value[i];
        linebufForwardWrites++;
    } else {
        entry.bwdValid = true;
        entry.bwdCellId = cell_id;
        for (int i = 0; i < 5; ++i)
            entry.nextDq[i] = value[i];
        linebufBackwardWrites++;
    }
}

bool
CfdLocalSpm::readLineBuffer(bool forward, uint64_t line_id,
                            uint64_t cell_id, double *dst)
{
    ensureLineBufferState();
    for (int i = 0; i < 5; ++i)
        dst[i] = 0.0;
    if (lineBufferEntries.empty()) {
        linebufInvalidReads++;
        if (forward)
            linebufForwardMisses++;
        else
            linebufBackwardMisses++;
        return false;
    }

    LineBufferEntry &entry =
        lineBufferEntries[line_id % lineBufferEntries.size()];
    if (forward)
        linebufForwardReads++;
    else
        linebufBackwardReads++;

    if ((entry.fwdValid || entry.bwdValid) && entry.lineId != line_id) {
        linebufTagConflicts++;
        linebufInvalidReads++;
        if (forward)
            linebufForwardMisses++;
        else
            linebufBackwardMisses++;
        return false;
    }

    const bool valid = forward ? entry.fwdValid : entry.bwdValid;
    const uint64_t stored_cell =
        forward ? entry.fwdCellId : entry.bwdCellId;
    if (!valid || stored_cell != cell_id) {
        linebufInvalidReads++;
        if (forward)
            linebufForwardMisses++;
        else
            linebufBackwardMisses++;
        return false;
    }

    const auto &value = forward ? entry.prevDqStar : entry.nextDq;
    for (int i = 0; i < 5; ++i)
        dst[i] = value[i];
    if (forward)
        linebufForwardHits++;
    else
        linebufBackwardHits++;
    return true;
}

void
CfdLocalSpm::recordLineBufferStall(uint64_t cycles)
{
    linebufStallCycles += cycles;
}

void
CfdLocalSpm::recordLusgsController(const CfdLusgsControllerRecord &r)
{
    lusgsControllerLaunches += r.launches;
    lusgsControllerCommittedLaunches += r.committedLaunches;
    lusgsControllerSquashedLaunches += r.squashedLaunches;
    lusgsControllerCompletedTasks += r.completedTasks;
    lusgsControllerFailedTasks += r.failedTasks;
    lusgsControllerBusyCycles += r.busyCycles;
    lusgsControllerTotalCycles += r.totalCycles;
    lusgsControllerForwardCells += r.forwardCells;
    lusgsControllerBackwardCells += r.backwardCells;
    lusgsControllerUpdatedQCells += r.updatedQCells;
    lusgsControllerPathARequests += r.pathaRequests;
    lusgsControllerTrsv5Requests += r.trsv5Requests;
    lusgsControllerVec5SubRequests += r.vec5SubRequests;
    lusgsControllerVec5CopyRequests += r.vec5CopyRequests;
    lusgsControllerVec5AxpyRequests += r.vec5AxpyRequests;
    lusgsControllerSpmReadRequests += r.spmReadRequests;
    lusgsControllerSpmWriteRequests += r.spmWriteRequests;
    lusgsControllerPathAWaitCycles += r.pathaWaitCycles;
    lusgsControllerTrsv5WaitCycles += r.trsv5WaitCycles;
    lusgsControllerVec5WaitCycles += r.vec5WaitCycles;
    lusgsControllerSpmWaitCycles += r.spmWaitCycles;
    lusgsControllerWritebackWaitCycles += r.writebackWaitCycles;
    lusgsControllerForwardDependencyStalls += r.forwardDependencyStalls;
    lusgsControllerBackwardDependencyStalls += r.backwardDependencyStalls;
    lusgsControllerContextFullStalls += r.contextFullStalls;
    lusgsControllerNoReadyContextCycles += r.noReadyContextCycles;
    lusgsControllerArbiterStalls += r.arbiterStalls;
    lusgsControllerContextAlloc += r.contextAlloc;
    lusgsControllerContextFree += r.contextFree;
    lusgsControllerActiveContextCycles += r.activeContextCycles;
    if (r.maxActiveContexts > lusgsControllerMaxActiveContexts.value())
        lusgsControllerMaxActiveContexts = r.maxActiveContexts;
    lusgsControllerContextSwitches += r.contextSwitches;
    lusgsControllerLuBytes += r.luBytes;
    lusgsControllerCBytes += r.cBytes;
    lusgsControllerBbarBytes += r.bbarBytes;
    lusgsControllerRhsBytes += r.rhsBytes;
    lusgsControllerDqstarReadBytes += r.dqstarReadBytes;
    lusgsControllerDqstarWriteBytes += r.dqstarWriteBytes;
    lusgsControllerDqReadBytes += r.dqReadBytes;
    lusgsControllerDqWriteBytes += r.dqWriteBytes;
    lusgsControllerQReadBytes += r.qReadBytes;
    lusgsControllerQWriteBytes += r.qWriteBytes;
    lusgsControllerTemporaryBytes += r.temporaryBytes;
    lusgsControllerTemporaryResultStores += r.temporaryResultStores;
    lusgsControllerTemporaryResultLoads += r.temporaryResultLoads;
    lusgsControllerBufferReads += r.controllerBufferReads;
    lusgsControllerBufferWrites += r.controllerBufferWrites;
    lusgsControllerDqstarExternalWrites += r.dqstarExternalWrites;
    lusgsControllerDqstarExternalReads += r.dqstarExternalReads;
    lusgsControllerFinalDqWrites += r.finalDqWrites;
    lusgsControllerTileCoefficientBytes += r.tileCoefficientBytes;
    lusgsControllerTileVectorBytes += r.tileVectorBytes;
    lusgsControllerTileTemporaryBytes += r.tileTemporaryBytes;
    if (r.tileTotalBytes)
        lusgsControllerTileTotalBytes = r.tileTotalBytes;
    if (r.spmCapacity)
        lusgsControllerSpmCapacity = r.spmCapacity;
    lusgsControllerTileLoads += r.tileLoads;
    lusgsControllerTilePrefetches += r.tilePrefetches;
    lusgsControllerPrefetchUseful += r.prefetchUseful;
    lusgsControllerPrefetchLate += r.prefetchLate;
    lusgsControllerBufferSwap += r.bufferSwap;
    lusgsControllerBufferConflictStalls += r.bufferConflictStalls;
    pathaArbiterCpuRequests += r.pathaArbiterCpuRequests;
    pathaArbiterLusgsRequests += r.pathaArbiterLusgsRequests;
    pathaArbiterBusyStalls += r.pathaArbiterBusyStalls;
    pathaArbiterOwnershipCycles += r.pathaArbiterOwnershipCycles;
    lusgsEventLaunches += r.eventLaunches;
    lusgsEventCompleted += r.eventCompleted;
    lusgsEventFailed += r.eventFailed;
    lusgsEventActualCycles += r.eventActualCycles;
    lusgsEventEventsProcessed += r.eventEventsProcessed;
    lusgsEventSchedulerTicks += r.eventSchedulerTicks;
    lusgsEventActiveCycles += r.eventActiveCycles;
    lusgsEventIdleCycles += r.eventIdleCycles;
    lusgsEventDecodedLaunches += r.eventDecodedLaunches;
    lusgsEventCommittedLaunches += r.eventCommittedLaunches;
    lusgsEventSquashedLaunches += r.eventSquashedLaunches;
    lusgsEventSquashedWaits += r.eventSquashedWaits;
    lusgsEventStaleCompletions += r.eventStaleCompletions;
    lusgsEventUnexpectedCompletions += r.eventUnexpectedCompletions;
    lusgsEventDuplicateCompletions += r.eventDuplicateCompletions;
    lusgsEventRequestIdMismatches += r.eventRequestIdMismatches;
    lusgsEventGenerationMismatches += r.eventGenerationMismatches;
    lusgsEventWrongStateCompletions += r.eventWrongStateCompletions;
    lusgsEventPendingRequestAlloc += r.eventPendingRequestAlloc;
    lusgsEventPendingRequestFree += r.eventPendingRequestFree;
    lusgsEventPendingRequestFullStalls += r.eventPendingRequestFullStalls;
    if (r.eventMaxPendingRequests > lusgsEventMaxPendingRequests.value())
        lusgsEventMaxPendingRequests = r.eventMaxPendingRequests;
    lusgsEventCancelledRequests += r.eventCancelledRequests;
    lusgsEventLifecycleIdleCycles += r.eventLifecycleIdleCycles;
    lusgsEventLifecycleQueuedCycles += r.eventLifecycleQueuedCycles;
    lusgsEventLifecycleRunningCycles += r.eventLifecycleRunningCycles;
    lusgsEventLifecycleCompletedNotReapedCycles +=
        r.eventLifecycleCompletedNotReapedCycles;
    lusgsEventLifecycleErrorNotReapedCycles +=
        r.eventLifecycleErrorNotReapedCycles;
    lusgsEventTokensReaped += r.eventTokensReaped;
    lusgsEventWaitInstructions += r.eventWaitInstructions;
    lusgsEventBusyPolls += r.eventBusyPolls;
    lusgsEventSuccessfulWaits += r.eventSuccessfulWaits;
    lusgsEventErrorWaits += r.eventErrorWaits;
    lusgsEventBadTokenWaits += r.eventBadTokenWaits;
    lusgsEventTokenReapCycles += r.eventTokenReapCycles;
    if (r.eventLaunchCommitTick)
        lusgsEventLaunchCommitTick = r.eventLaunchCommitTick;
    if (r.eventDescriptorSnapshotTick)
        lusgsEventDescriptorSnapshotTick = r.eventDescriptorSnapshotTick;
    if (r.eventFirstResourceIssueTick)
        lusgsEventFirstResourceIssueTick = r.eventFirstResourceIssueTick;
    if (r.eventTaskCompleteTick)
        lusgsEventTaskCompleteTick = r.eventTaskCompleteTick;
    if (r.eventSuccessfulWaitTick)
        lusgsEventSuccessfulWaitTick = r.eventSuccessfulWaitTick;
    if (r.eventTokenReapTick)
        lusgsEventTokenReapTick = r.eventTokenReapTick;
    lusgsEventWatchdogTimeouts += r.eventWatchdogTimeouts;
    lusgsEventStateTransitions += r.eventStateTransitions;
    lusgsEventNoProgressCycles += r.eventNoProgressCycles;
    lusgsEventDoubleScheduleErrors += r.eventDoubleScheduleErrors;
    lusgsEventNoReadyContextCycles += r.eventNoReadyContextCycles;
    lusgsEventPathaRequests += r.eventPathaRequests;
    lusgsEventPathaAccepted += r.eventPathaAccepted;
    lusgsEventPathaRetries += r.eventPathaRetries;
    lusgsEventPathaCompleted += r.eventPathaCompleted;
    lusgsEventPathaWaitCycles += r.eventPathaWaitCycles;
    lusgsEventPathaSlotFullStalls += r.eventPathaSlotFullStalls;
    lusgsEventPathaPackStalls += r.eventPathaPackStalls;
    lusgsEventPathaStoreStalls += r.eventPathaStoreStalls;
    lusgsEventPathaMvmRequests += r.eventPathaMvmRequests;
    lusgsEventPathaMvmAccepted += r.eventPathaMvmAccepted;
    lusgsEventPathaMvmRetries += r.eventPathaMvmRetries;
    lusgsEventPathaMvmCompleted += r.eventPathaMvmCompleted;
    lusgsEventPathaMvmWaitCycles += r.eventPathaMvmWaitCycles;
    lusgsEventPathaMatLdRequests += r.eventPathaMatLdRequests;
    lusgsEventPathaMatLdAccepted += r.eventPathaMatLdAccepted;
    lusgsEventPathaMatLdRetries += r.eventPathaMatLdRetries;
    lusgsEventPathaMatLdCompleted += r.eventPathaMatLdCompleted;
    lusgsEventPathaMatLdBusyCycles += r.eventPathaMatLdBusyCycles;
    lusgsEventPathaMatLdBytes += r.eventPathaMatLdBytes;
    lusgsEventPathaDotpRequests += r.eventPathaDotpRequests;
    lusgsEventPathaDotpAccepted += r.eventPathaDotpAccepted;
    lusgsEventPathaDotpRetries += r.eventPathaDotpRetries;
    lusgsEventPathaDotpCompleted += r.eventPathaDotpCompleted;
    lusgsEventPathaDotpBusyCycles += r.eventPathaDotpBusyCycles;
    lusgsEventPathaDotpPipelineOccupancy +=
        r.eventPathaDotpPipelineOccupancy;
    lusgsEventPathaPackRequests += r.eventPathaPackRequests;
    lusgsEventPathaPackAccepted += r.eventPathaPackAccepted;
    lusgsEventPathaPackRetries += r.eventPathaPackRetries;
    lusgsEventPathaPackCompleted += r.eventPathaPackCompleted;
    lusgsEventPathaPackBusyCycles += r.eventPathaPackBusyCycles;
    lusgsEventPathaSlotAlloc += r.eventPathaSlotAlloc;
    lusgsEventPathaSlotFree += r.eventPathaSlotFree;
    lusgsEventPathaSlotOverwriteErrors +=
        r.eventPathaSlotOverwriteErrors;
    lusgsEventPathaEarlyConsumeErrors += r.eventPathaEarlyConsumeErrors;
    lusgsEventPathaResultTransfers += r.eventPathaResultTransfers;
    lusgsEventPathaResultBytes += r.eventPathaResultBytes;
    lusgsEventPathaLoadPhaseCycles += r.eventPathaLoadPhaseCycles;
    lusgsEventPathaDotpPhaseCycles += r.eventPathaDotpPhaseCycles;
    lusgsEventPathaPackPhaseCycles += r.eventPathaPackPhaseCycles;
    lusgsEventPathaResultPhaseCycles += r.eventPathaResultPhaseCycles;
    if (r.eventPathaAverageMvmLatency)
        lusgsEventPathaAverageMvmLatency =
            r.eventPathaAverageMvmLatency;
    if (r.eventPathaMinMvmLatency)
        lusgsEventPathaMinMvmLatency = r.eventPathaMinMvmLatency;
    if (r.eventPathaMaxMvmLatency)
        lusgsEventPathaMaxMvmLatency = r.eventPathaMaxMvmLatency;
    lusgsEventTrsvRequests += r.eventTrsvRequests;
    lusgsEventTrsvAccepted += r.eventTrsvAccepted;
    lusgsEventTrsvRetries += r.eventTrsvRetries;
    lusgsEventTrsvCompleted += r.eventTrsvCompleted;
    lusgsEventTrsvWaitCycles += r.eventTrsvWaitCycles;
    lusgsEventTrsvBusyCycles += r.eventTrsvBusyCycles;
    lusgsEventTrsvQueueFullStalls += r.eventTrsvQueueFullStalls;
    if (r.eventTrsvMaxQueueDepth > lusgsEventTrsvMaxQueueDepth.value())
        lusgsEventTrsvMaxQueueDepth = r.eventTrsvMaxQueueDepth;
    lusgsEventTrsvDivRequests += r.eventTrsvDivRequests;
    lusgsEventTrsvDivCompleted += r.eventTrsvDivCompleted;
    lusgsEventTrsvDivBusyCycles += r.eventTrsvDivBusyCycles;
    lusgsEventTrsvFmaRequests += r.eventTrsvFmaRequests;
    lusgsEventTrsvFmaCompleted += r.eventTrsvFmaCompleted;
    lusgsEventTrsvFmaBusyCycles += r.eventTrsvFmaBusyCycles;
    lusgsEventTrsvForwardCycles += r.eventTrsvForwardCycles;
    lusgsEventTrsvBackwardCycles += r.eventTrsvBackwardCycles;
    lusgsEventTrsvDependencyWaitCycles += r.eventTrsvDependencyWaitCycles;
    lusgsEventTrsvForwardedResults += r.eventTrsvForwardedResults;
    if (r.eventTrsvAverageLatency)
        lusgsEventTrsvAverageLatency = r.eventTrsvAverageLatency;
    if (r.eventTrsvMinLatency)
        lusgsEventTrsvMinLatency = r.eventTrsvMinLatency;
    if (r.eventTrsvMaxLatency)
        lusgsEventTrsvMaxLatency = r.eventTrsvMaxLatency;
    if (r.eventTrsvDividerUtilization)
        lusgsEventTrsvDividerUtilization = r.eventTrsvDividerUtilization;
    if (r.eventTrsvFmaUtilization)
        lusgsEventTrsvFmaUtilization = r.eventTrsvFmaUtilization;
    lusgsEventVec5Requests += r.eventVec5Requests;
    lusgsEventVec5Accepted += r.eventVec5Accepted;
    lusgsEventVec5Retries += r.eventVec5Retries;
    lusgsEventVec5Completed += r.eventVec5Completed;
    lusgsEventVec5WaitCycles += r.eventVec5WaitCycles;
    lusgsEventVec5QueueFullStalls += r.eventVec5QueueFullStalls;
    lusgsEventVec5BusyCycles += r.eventVec5BusyCycles;
    if (r.eventVec5MaxQueueDepth > lusgsEventVec5MaxQueueDepth.value())
        lusgsEventVec5MaxQueueDepth = r.eventVec5MaxQueueDepth;
    lusgsEventVec5LaneOperations += r.eventVec5LaneOperations;
    if (r.eventVec5AverageLatency)
        lusgsEventVec5AverageLatency = r.eventVec5AverageLatency;
    if (r.eventVec5LaneUtilization)
        lusgsEventVec5LaneUtilization = r.eventVec5LaneUtilization;
    lusgsEventVec5CopyRequests += r.eventVec5CopyRequests;
    lusgsEventVec5CopyAccepted += r.eventVec5CopyAccepted;
    lusgsEventVec5CopyCompleted += r.eventVec5CopyCompleted;
    lusgsEventVec5CopyRetries += r.eventVec5CopyRetries;
    lusgsEventVec5CopyWaitCycles += r.eventVec5CopyWaitCycles;
    lusgsEventVec5SubRequests += r.eventVec5SubRequests;
    lusgsEventVec5SubAccepted += r.eventVec5SubAccepted;
    lusgsEventVec5SubCompleted += r.eventVec5SubCompleted;
    lusgsEventVec5SubRetries += r.eventVec5SubRetries;
    lusgsEventVec5SubWaitCycles += r.eventVec5SubWaitCycles;
    lusgsEventVec5AxpyRequests += r.eventVec5AxpyRequests;
    lusgsEventVec5AxpyAccepted += r.eventVec5AxpyAccepted;
    lusgsEventVec5AxpyCompleted += r.eventVec5AxpyCompleted;
    lusgsEventVec5AxpyRetries += r.eventVec5AxpyRetries;
    lusgsEventVec5AxpyWaitCycles += r.eventVec5AxpyWaitCycles;
    lusgsEventContextAlloc += r.eventContextAlloc;
    lusgsEventContextFree += r.eventContextFree;
    if (r.eventMaxActiveContexts > lusgsEventMaxActiveContexts.value())
        lusgsEventMaxActiveContexts = r.eventMaxActiveContexts;
    lusgsEventContextSwitches += r.eventContextSwitches;
    lusgsEventContextReadyCycles += r.eventContextReadyCycles;
    lusgsEventContextWaitCycles += r.eventContextWaitCycles;
    lusgsEventSpmReads += r.eventSpmReads;
    lusgsEventSpmWrites += r.eventSpmWrites;
    lusgsEventSpmReadBytes += r.eventSpmReadBytes;
    lusgsEventSpmWriteBytes += r.eventSpmWriteBytes;
    lusgsEventSpmPortConflicts += r.eventSpmPortConflicts;
    lusgsEventSpmBankConflicts += r.eventSpmBankConflicts;
    lusgsEventSpmQueueFull += r.eventSpmQueueFull;
    lusgsEventSpmRetries += r.eventSpmRetries;
    if (r.eventSpmAverageLatency)
        lusgsEventSpmAverageLatency = r.eventSpmAverageLatency;
    lusgsEventTileLoads += r.eventTileLoads;
    lusgsEventTileLoadCycles += r.eventTileLoadCycles;
    lusgsEventTileComputeCycles += r.eventTileComputeCycles;
    lusgsEventPrefetchUseful += r.eventPrefetchUseful;
    lusgsEventPrefetchLate += r.eventPrefetchLate;
    lusgsEventBufferEmptyStalls += r.eventBufferEmptyStalls;
    lusgsEventBufferFullStalls += r.eventBufferFullStalls;
    if (r.cyclesPerForwardCell)
        lusgsControllerCyclesPerForwardCell = r.cyclesPerForwardCell;
    if (r.cyclesPerBackwardCell)
        lusgsControllerCyclesPerBackwardCell = r.cyclesPerBackwardCell;
    if (r.cyclesPerFullCell)
        lusgsControllerCyclesPerFullCell = r.cyclesPerFullCell;
    if (r.cellsPer1000Cycles)
        lusgsControllerCellsPer1000Cycles = r.cellsPer1000Cycles;
    if (r.pathaUtilization)
        lusgsControllerPathAUtilization = r.pathaUtilization;
    if (r.trsv5Utilization)
        lusgsControllerTrsv5Utilization = r.trsv5Utilization;
    if (r.vec5Utilization)
        lusgsControllerVec5Utilization = r.vec5Utilization;
}

void
CfdLocalSpm::resizeEventState(uint64_t read_ports, uint64_t banks,
                              uint64_t z_wb_ports, uint64_t queue_size)
{
    const uint64_t ports = std::max<uint64_t>(1, read_ports);
    const uint64_t num_banks = std::max<uint64_t>(1, banks);
    const uint64_t wb_ports = std::max<uint64_t>(1, z_wb_ports);
    const uint64_t q_size = std::max<uint64_t>(1, queue_size);

    if (readPortNextFreeCycle.size() != ports)
        readPortNextFreeCycle.assign(ports, 0);
    if (readPortLastRequester.size() != ports)
        readPortLastRequester.assign(ports, CfdSpmRequester::PathA);
    if (bankNextFreeCycle.size() != num_banks)
        bankNextFreeCycle.assign(num_banks, 0);
    if (bankLastRequester.size() != num_banks)
        bankLastRequester.assign(num_banks, CfdSpmRequester::PathA);
    if (zWbNextFreeCycle.size() != wb_ports)
        zWbNextFreeCycle.assign(wb_ports, 0);
    if (queueSlotFreeCycle.size() != q_size)
        queueSlotFreeCycle.assign(q_size, 0);
}

void
CfdLocalSpm::recordPathABufferAlloc(uint64_t live)
{
    pathAResultBufferAllocs++;
    pathAResultBufferLiveEnd = live;
    if (live > pathAResultBufferLiveMax.value())
        pathAResultBufferLiveMax = live;
}

void
CfdLocalSpm::recordPathABufferFree(uint64_t live)
{
    pathAResultBufferFrees++;
    pathAResultBufferLiveEnd = live;
}

void
CfdLocalSpm::recordPathABufferWrite()
{
    pathAResultBufferWrites++;
}

void
CfdLocalSpm::recordPathABufferPack()
{
    pathAResultBufferPacks++;
}

void
CfdLocalSpm::recordPathAFusedSub()
{
    pathAFusedSubOps++;
}

void
CfdLocalSpm::recordPathABufferFullStall()
{
    pathAResultBufferFullStalls++;
}

void
CfdLocalSpm::recordPathABufferOverlapCycle()
{
    pathAResultBufferOverlapCycles++;
}

void
CfdLocalSpm::recordPathABufferPackWhileDotpCycle()
{
    pathAResultBufferPackWhileDotpCycles++;
}

static uint64_t cfdTicksPerCycle();

void
CfdLocalSpm::recordPathADotpIssue(uint64_t seq_num)
{
    (void)seq_num;
    const uint64_t cycle = curTick() / cfdTicksPerCycle();

    // m5_reset_stats clears statistics::Scalar values but not this helper's
    // private burst trackers.  Detect the first post-reset dotp and restart the
    // shadow tracker so the correctness precheck does not create a giant gap.
    if (pathADotpIssued.value() == 0) {
        seenPathADotpIssue = false;
        lastPathADotpIssueCycle = 0;
        pathADotpBurstLen = 0;
        pathADotpBurstCount = 0;
    }

    pathADotpIssued++;

    if (!seenPathADotpIssue) {
        seenPathADotpIssue = true;
        lastPathADotpIssueCycle = cycle;
        pathADotpBurstLen = 1;
        pathADotpIssueCycles++;
        pathAConsecutiveDotpBurstTotal = 1;
        pathAConsecutiveDotpBurstMax = 1;
        pathAConsecutiveDotpBurstAvg = 1;
        return;
    }

    if (cycle != lastPathADotpIssueCycle)
        pathADotpIssueCycles++;

    const uint64_t gap = cycle > lastPathADotpIssueCycle ?
        cycle - lastPathADotpIssueCycle : 0;
    if (gap > 1) {
        const uint64_t idle = gap - 1;
        pathADotpIssueGaps += idle;
        if (idle > pathAMaxDotpIssueGap.value())
            pathAMaxDotpIssueGap = idle;
        pathADotpBurstCount++;
        pathADotpBurstLen = 1;
    } else {
        pathADotpBurstLen++;
    }

    if (pathADotpBurstLen > pathAConsecutiveDotpBurstMax.value())
        pathAConsecutiveDotpBurstMax = pathADotpBurstLen;
    pathAConsecutiveDotpBurstTotal = pathADotpIssued.value();
    const uint64_t bursts = std::max<uint64_t>(1, pathADotpBurstCount + 1);
    pathAConsecutiveDotpBurstAvg = pathADotpIssued.value() / bursts;
    lastPathADotpIssueCycle = cycle;
}

void
CfdLocalSpm::recordPathADotpBlockedByFuBusy()
{
    pathADotpBlockedByFuBusy++;
}

void
CfdLocalSpm::recordPathADotpBlockedByInputNotReady()
{
    pathADotpBlockedByInputNotReady++;
}

void
CfdLocalSpm::recordPathADotpBlockedByResultBufferFull()
{
    pathADotpBlockedByResultBufferFull++;
}

void
CfdLocalSpm::recordPathADotpBlockedByPackStoreBackpressure()
{
    pathADotpBlockedByPackStoreBackpressure++;
}

void
CfdLocalSpm::recordPathADotpReadyButNotIssued()
{
    pathADotpReadyButNotIssued++;
}

void
CfdLocalSpm::recordPathAStreamLoadPort(unsigned port, bool dual_issue_cycle)
{
    if (port == 0)
        pathAStreamReadPort0BusyCycles++;
    else
        pathAStreamReadPort1BusyCycles++;
    if (dual_issue_cycle)
        pathADualLoadIssueCycles++;
}

void
CfdLocalSpm::recordPathAEarlyDotpOpportunity()
{
    pathAEarlyDotpStartEvents++;
    pathAEarlyReadyOpportunities++;
    pathALoadComputeOverlapCycles++;
}

void
CfdLocalSpm::recordPathAEarlyDotpStart(bool before_all_fields,
                                       uint64_t field_ready_wait,
                                       uint64_t vector_ready_wait)
{
    if (before_all_fields) {
        pathADotpIssuedBeforeAllFieldsReady++;
    }
    pathAFieldReadyWaitCycles += field_ready_wait + vector_ready_wait;
    pathADotpAfterFieldReadyTotal += field_ready_wait;
    pathADotpAfterVectorReadyTotal += vector_ready_wait;
    const auto denom =
        std::max<statistics::Counter>(1, pathADotpIssued.value());
    pathADotpAvgAfterFieldReady =
        pathADotpAfterFieldReadyTotal.value() / denom;
    pathADotpAvgAfterVectorReady =
        pathADotpAfterVectorReadyTotal.value() / denom;
}

void
CfdLocalSpm::recordPathAEarlyDotpDetail(bool dependency_ready_before_all,
                                        bool execute_started_before_all,
                                        bool complete_before_all,
                                        uint64_t ready_wait_cycles)
{
    if (!dependency_ready_before_all)
        return;

    pathADotpDependencyReadyBeforeAllFields++;
    pathAEarlyReadyIssued++;

    if (execute_started_before_all) {
        pathADotpExecuteStartedBeforeAllFields++;
        pathAEarlyReadyExecuteStarted++;
    } else {
        pathAEarlyReadyMissed++;
        /*
         * StaticInst execute does not see the O3 IQ/selection reason.  Use the
         * local evidence we do have: if operands were ready well before
         * execute, the miss came after token readiness, so classify it as
         * scheduling/FU/issue pressure instead of token pressure.  Deep
         * dispatch/IQ attribution requires an O3-stage probe.
         */
        if (ready_wait_cycles > 0)
            pathAEarlyMissedDueToIssueWidth++;
        else
            pathAEarlyMissedDueToOther++;
    }

    if (complete_before_all)
        pathADotpCompletedBeforeAllFields++;

    const auto opps =
        std::max<statistics::Counter>(1, pathAEarlyReadyOpportunities.value());
    pathAEarlyReadyIssueEfficiency =
        static_cast<double>(pathAEarlyReadyExecuteStarted.value()) / opps;
}

void
CfdLocalSpm::recordPathAWholeSlotBarrierStall(uint64_t cycles)
{
    pathAWholeSlotBarrierStallCycles += cycles;
}

void
CfdLocalSpm::recordPathAInputBufferAlloc(uint64_t live)
{
    pathAInputBufferAllocs++;
    pathAInputBufferLiveEnd = live;
    if (live > pathAInputBufferLiveMax.value())
        pathAInputBufferLiveMax = live;
}

void
CfdLocalSpm::recordPathAInputBufferFree(uint64_t live)
{
    pathAInputBufferFrees++;
    pathAInputBufferLiveEnd = live;
}

void
CfdLocalSpm::recordPathAInputBufferFullStall()
{
    pathAInputBufferFullStalls++;
}

void
CfdLocalSpm::recordPathAAsyncStoreEnqueue(uint64_t live)
{
    pathAAsyncStoreEnqueues++;
    pathAAsyncStoreLiveEnd = live;
    if (live > pathAAsyncStoreLiveMax.value())
        pathAAsyncStoreLiveMax = live;
}

void
CfdLocalSpm::recordPathAAsyncStoreCommit(uint64_t live)
{
    pathAAsyncStoreCommits++;
    pathAAsyncStoreLiveEnd = live;
}

void
CfdLocalSpm::recordPathAAsyncStoreQueueFullStall()
{
    pathAAsyncStoreQueueFullStalls++;
}

void
CfdLocalSpm::recordPathCLmatPort(unsigned port, bool dual_issue_cycle)
{
    if (port == 0)
        pathCLmatReadPort0BusyCycles++;
    else
        pathCLmatReadPort1BusyCycles++;
    if (dual_issue_cycle)
        pathCDualLoadIssueCycles++;
}

void
CfdLocalSpm::recordPathCEarlyOuterStart(bool before_all_columns,
                                        uint64_t column_ready_wait,
                                        uint64_t vector_ready_wait,
                                        uint64_t za_token_wait)
{
    if (before_all_columns) {
        pathCEarlyOuterStartEvents++;
        pathCStepIssuedBeforeAllColumnsReady++;
        pathCLoadOuterOverlapCycles++;
    }
    pathCColumnReadyWaitCycles += column_ready_wait + vector_ready_wait;
    pathCZaTokenWaitCycles += za_token_wait;
    pathCStepAfterColumnReadyTotal += column_ready_wait;
    pathCStepAfterVectorReadyTotal += vector_ready_wait;
    const auto denom =
        std::max<statistics::Counter>(1, pathCOuterSteps.value() + 1);
    pathCStepAvgAfterColumnReady =
        pathCStepAfterColumnReadyTotal.value() / denom;
    pathCStepAvgAfterVectorReady =
        pathCStepAfterVectorReadyTotal.value() / denom;
}

void
CfdLocalSpm::recordPathCOuterFuIssue(uint64_t busy_cycles)
{
    pathCOuterFuIssueCount++;
    pathCOuterFuBusyCycles += std::max<uint64_t>(1, busy_cycles);
    const uint64_t cycles = curTick() / cfdTicksPerCycle();
    if (cycles > 0) {
        pathCOuterFuUtilization =
            static_cast<double>(pathCOuterFuBusyCycles.value()) / cycles;
    }
}

void
CfdLocalSpm::recordPathCMovaBoundaryWait(uint64_t cycles)
{
    pathCMovaBoundaryWaitCycles += cycles;
}

void
CfdLocalSpm::initTraceIfNeeded()
{
    if (traceInitialized)
        return;

    traceInitialized = true;
    const char *enable = std::getenv("GEM5_CFD_TRACE_ENABLE");
    traceEnabled = enable && (
        std::strcmp(enable, "1") == 0 ||
        std::strcmp(enable, "true") == 0 ||
        std::strcmp(enable, "yes") == 0 ||
        std::strcmp(enable, "on") == 0);
    if (!traceEnabled)
        return;

    const char *max_events = std::getenv("GEM5_CFD_TRACE_EVENTS");
    if (max_events && *max_events) {
        traceMaxEvents = std::strtoull(max_events, nullptr, 0);
    } else {
        const char *matvecs = std::getenv("GEM5_CFD_TRACE_MATVECS");
        const uint64_t n = matvecs && *matvecs ?
            std::strtoull(matvecs, nullptr, 0) : 4;
        traceMaxEvents = std::max<uint64_t>(1, n) * 64;
    }

    const char *path = std::getenv("GEM5_CFD_TRACE_FILE");
    if (!path || !*path)
        path = "m5out/cfd_micro_trace.csv";
    traceFp = std::fopen(path, "w");
    if (!traceFp) {
        traceEnabled = false;
        return;
    }
    std::fprintf(traceFp,
        "cycle,seq,path,slot,inst,field,lane,k,ready0,ready1,all_ready,note\n");
    std::fflush(traceFp);
}

void
CfdLocalSpm::recordCfdTrace(const char *path, const char *inst,
                            uint64_t seq_num, int slot, int field,
                            int lane, int k, uint64_t ready0,
                            uint64_t ready1, uint64_t all_ready,
                            const char *note)
{
    initTraceIfNeeded();
    if (!traceEnabled || !traceFp || traceEvents >= traceMaxEvents)
        return;

    const uint64_t cycle = curTick() / cfdTicksPerCycle();
    std::fprintf(traceFp,
        "%llu,%llu,%s,%d,%s,%d,%d,%d,%llu,%llu,%llu,%s\n",
        static_cast<unsigned long long>(cycle),
        static_cast<unsigned long long>(seq_num),
        path ? path : "",
        slot,
        inst ? inst : "",
        field,
        lane,
        k,
        static_cast<unsigned long long>(ready0),
        static_cast<unsigned long long>(ready1),
        static_cast<unsigned long long>(all_ready),
        note ? note : "");
    traceEvents++;
    std::fflush(traceFp);
}

static uint64_t
cfdTicksPerCycle()
{
    const char *value = std::getenv("GEM5_CFD_TICKS_PER_CYCLE");
    if (!value || !*value)
        return 500;
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(value, &end, 0);
    return parsed == 0 ? 500 : parsed;
}

CfdLocalSpm::LocalSpmRequest &
CfdLocalSpm::createLocalEventRequest(
    Addr ea, int row_id, unsigned logical_size, unsigned physical_size,
    uint64_t bank, uint64_t accept_cycle, uint64_t read_start_cycle,
    uint64_t response_cycle, uint64_t zwb_cycle, uint64_t complete_cycle,
    CfdSpmRequester requester, bool write,
    std::function<void(uint64_t)> completion)
{
    LocalSpmRequest req;
    req.requestId = nextLocalEventRequestId++;
    req.addr = ea;
    req.rowId = row_id >= 0 ? static_cast<unsigned>(row_id) : 0;
    req.bankId = static_cast<unsigned>(bank);
    req.logicalBytes = logical_size;
    req.physicalBytes = physical_size;
    req.requester = requester;
    req.write = write;
    req.completion = std::move(completion);
    req.issueTick = curTick();
    const Tick ticks_per_cycle = cfdTicksPerCycle();
    req.acceptTick = accept_cycle * ticks_per_cycle;
    req.readStartTick = read_start_cycle * ticks_per_cycle;
    req.responseTick = response_cycle * ticks_per_cycle;
    req.zwbTick = zwb_cycle * ticks_per_cycle;
    req.completeTick = complete_cycle * ticks_per_cycle;

    localEventRequests.push_back(req);
    LocalSpmRequest &stored = localEventRequests.back();
    localEventRequestsCreated++;
    localEventLiveRequestsCount++;
    updateLocalEventLiveStats();

    if (stored.acceptTick > stored.issueTick) {
        localEventRetryEvents++;
        localEventQueueFullEvents++;
    }
    if (stored.readStartTick > stored.acceptTick)
        localEventRetryEvents++;

    scheduleLocalEvent(stored, stored.acceptTick,
                       &CfdLocalSpm::onLocalEventAccept,
                       "cfd-local-spm-accept");
    scheduleLocalEvent(stored, stored.readStartTick,
                       &CfdLocalSpm::onLocalEventReadStart,
                       "cfd-local-spm-read-start");
    scheduleLocalEvent(stored, stored.responseTick,
                       &CfdLocalSpm::onLocalEventResponse,
                       "cfd-local-spm-response");
    scheduleLocalEvent(stored, stored.zwbTick,
                       &CfdLocalSpm::onLocalEventZwb,
                       "cfd-local-spm-zwb");
    scheduleLocalEvent(stored, stored.completeTick,
                       &CfdLocalSpm::onLocalEventComplete,
                       "cfd-local-spm-complete");

    return stored;
}

void
CfdLocalSpm::scheduleLocalEvent(LocalSpmRequest &req, Tick when,
                                void (CfdLocalSpm::*handler)(uint64_t),
                                const char *name)
{
    const Tick event_tick = std::max(curTick(), when);
    auto *event = new EventFunctionWrapper(
        [this, request_id = req.requestId, handler]() {
            (this->*handler)(request_id);
        },
        name, true);
    schedule(event, event_tick);
}

CfdLocalSpm::LocalSpmRequest *
CfdLocalSpm::findLocalEventRequest(uint64_t request_id)
{
    if (request_id == 0 || request_id > localEventRequests.size())
        return nullptr;
    return &localEventRequests[request_id - 1];
}

void
CfdLocalSpm::updateLocalEventLiveStats()
{
    localEventLiveRequests = localEventLiveRequestsCount;
    localEventLiveRequestsEnd = localEventLiveRequestsCount;
    if (localEventLiveRequestsCount > localEventLiveRequestsMax.value())
        localEventLiveRequestsMax = localEventLiveRequestsCount;
}

void
CfdLocalSpm::onLocalEventAccept(uint64_t request_id)
{
    auto *req = findLocalEventRequest(request_id);
    if (!req || req->squashed || req->accepted)
        return;
    req->accepted = true;
    localEventAcceptEvents++;
    localEventRequestsAccepted++;
}

void
CfdLocalSpm::onLocalEventReadStart(uint64_t request_id)
{
    auto *req = findLocalEventRequest(request_id);
    if (!req || req->squashed || req->readStarted)
        return;
    req->readStarted = true;
    localEventReadStartEvents++;
}

void
CfdLocalSpm::onLocalEventResponse(uint64_t request_id)
{
    auto *req = findLocalEventRequest(request_id);
    if (!req || req->squashed || req->responded)
        return;
    req->responded = true;
    localEventResponseEvents++;
}

void
CfdLocalSpm::onLocalEventZwb(uint64_t request_id)
{
    auto *req = findLocalEventRequest(request_id);
    if (!req || req->squashed || req->zwbDone)
        return;
    req->zwbDone = true;
    localEventZwbEvents++;
}

void
CfdLocalSpm::onLocalEventComplete(uint64_t request_id)
{
    auto *req = findLocalEventRequest(request_id);
    if (!req || req->squashed || req->completed)
        return;

    req->completed = true;
    localEventCompleteEvents++;
    localEventRequestsCompleted++;
    if (localEventLiveRequestsCount > 0)
        localEventLiveRequestsCount--;
    updateLocalEventLiveStats();

    const Tick ticks_per_cycle = cfdTicksPerCycle();
    auto cycles_between = [ticks_per_cycle](Tick begin, Tick end) {
        return end > begin ? (end - begin) / ticks_per_cycle : 0;
    };

    const uint64_t issue_to_accept =
        cycles_between(req->issueTick, req->acceptTick);
    const uint64_t accept_to_read =
        cycles_between(req->acceptTick, req->readStartTick);
    const uint64_t read_to_response =
        cycles_between(req->readStartTick, req->responseTick);
    const uint64_t response_to_zwb =
        cycles_between(req->responseTick, req->zwbTick);
    const uint64_t zwb_to_complete =
        cycles_between(req->zwbTick, req->completeTick);
    const uint64_t issue_to_complete =
        cycles_between(req->issueTick, req->completeTick);

    localEventIssueToAcceptTotal += issue_to_accept;
    localEventAcceptToReadStartTotal += accept_to_read;
    localEventReadStartToResponseTotal += read_to_response;
    localEventResponseToZwbTotal += response_to_zwb;
    localEventZwbToCompleteTotal += zwb_to_complete;
    localEventIssueToCompleteTotal += issue_to_complete;

    const auto denom =
        std::max<statistics::Counter>(1, localEventRequestsCompleted.value());
    localEventAvgIssueToAccept =
        localEventIssueToAcceptTotal.value() / denom;
    localEventAvgAcceptToReadStart =
        localEventAcceptToReadStartTotal.value() / denom;
    localEventAvgReadStartToResponse =
        localEventReadStartToResponseTotal.value() / denom;
    localEventAvgResponseToZwb =
        localEventResponseToZwbTotal.value() / denom;
    localEventAvgZwbToComplete =
        localEventZwbToCompleteTotal.value() / denom;
    localEventAvgIssueToComplete =
        localEventIssueToCompleteTotal.value() / denom;
    if (req->requester == CfdSpmRequester::CoeffInput ||
        req->requester == CfdSpmRequester::CoeffCompute ||
        req->requester == CfdSpmRequester::CoeffDrain) {
        coeffPacketCompleted++;
    }
    if (req->completion)
        req->completion(req->requestId);
}

static uint64_t
selectLocalSpmBank(Addr ea, int row_id, uint64_t bank_grain,
                   uint64_t num_banks)
{
    const char *mapping = std::getenv("GEM5_CFD_LOCAL_SPM_BANK_MAPPING");
    const uint64_t addr_index = ea / std::max<uint64_t>(1, bank_grain);

    // The CFD SPM tile places the vector at +5*40B from a 4KB-aligned tile
    // base.  Treat it as row 5 for row-index-aware bank mappings.
    uint64_t logical_row = row_id >= 0 ? static_cast<uint64_t>(row_id) : 0;
    if ((ea & 0xfff) == 5 * 40)
        logical_row = 5;

    if (mapping && std::strcmp(mapping, "row") == 0)
        return logical_row % num_banks;
    if (mapping && std::strcmp(mapping, "xor") == 0)
        return (addr_index ^ logical_row) % num_banks;
    return addr_index % num_banks;
}

CfdSpmPacketIssue
CfdLocalSpm::issuePacket(
    CfdSpmRequester requester, bool write, Addr addr, unsigned bytes,
    uint64_t latency, uint64_t ports, uint64_t outstanding, uint64_t banks,
    uint64_t bank_granularity, CfdSpmMatrix matrix,
    std::function<void(uint64_t)> completion)
{
    CfdSpmPacketIssue result;
    const uint64_t portCount = std::max<uint64_t>(1, ports);
    const uint64_t bankCount = std::max<uint64_t>(1, banks);
    const uint64_t limit = std::max<uint64_t>(1, outstanding);
    if (localEventLiveRequestsCount >= limit) {
        result.outstandingFull = true;
        coeffPacketOutstandingStalls++;
        localEventOutstandingFullEvents++;
        return result;
    }

    auto &portState = write ? writePortNextFreeCycle :
                              readPortNextFreeCycle;
    auto &portOwner = write ? writePortLastRequester :
                              readPortLastRequester;
    if (portState.size() != portCount) {
        portState.assign(portCount, 0);
        portOwner.assign(portCount, requester);
    }
    if (bankNextFreeCycle.size() != bankCount) {
        bankNextFreeCycle.assign(bankCount, 0);
        bankLastRequester.assign(bankCount, requester);
    }

    const uint64_t now = curTick() / cfdTicksPerCycle();
    auto port = std::min_element(portState.begin(), portState.end());
    const unsigned portIndex = std::distance(portState.begin(), port);
    uint64_t start = std::max(now, *port);
    result.portWaitCycles = start - now;
    const uint64_t bank = selectLocalSpmBank(
        addr, -1, std::max<uint64_t>(1, bank_granularity), bankCount);
    const uint64_t beforeBank = start;
    start = std::max(start, bankNextFreeCycle[bank]);
    result.bankWaitCycles = start - beforeBank;
    const CfdSpmRequester portBlocker = portOwner[portIndex];
    const CfdSpmRequester bankBlocker = bankLastRequester[bank];
    *port = start + 1;
    portOwner[portIndex] = requester;
    bankNextFreeCycle[bank] = start + 1;
    bankLastRequester[bank] = requester;

    const uint64_t done = start + std::max<uint64_t>(1, latency);
    auto &request = createLocalEventRequest(
        addr, -1, bytes, bytes, bank, now, start, done, done, done,
        requester, write, std::move(completion));
    result.packetId = request.requestId;
    result.completionCycle = done;
    result.bank = bank;
    result.port = portIndex;
    result.accepted = true;
    coeffPacketIssued++;
    if (write)
        coeffPacketWriteBytes += bytes;
    else
        coeffPacketReadBytes += bytes;
    coeffPacketPortWaitCycles += result.portWaitCycles;
    coeffPacketBankWaitCycles += result.bankWaitCycles;
    constexpr unsigned MaxTrackedBanks = 64;
    if (bank < MaxTrackedBanks) {
        coeffPacketBankAccesses[bank]++;
        if (result.bankWaitCycles) {
            coeffPacketBankConflicts[bank]++;
            coeffPacketBankWaitByBank[bank] += result.bankWaitCycles;
            if (result.bankWaitCycles > coeffPacketBankMaxWait[bank].value())
                coeffPacketBankMaxWait[bank] = result.bankWaitCycles;
        }
    }
    const unsigned matrixIndex = static_cast<unsigned>(matrix);
    if (matrixIndex < static_cast<unsigned>(CfdSpmMatrix::Count)) {
        coeffPacketMatrixAccesses[matrixIndex]++;
        const uint64_t wait = result.portWaitCycles + result.bankWaitCycles;
        coeffPacketMatrixWaitCycles[matrixIndex] += wait;
        if (wait)
            coeffPacketMatrixConflicts[matrixIndex]++;
    }
    const bool coeffRequester = requester == CfdSpmRequester::CoeffInput ||
        requester == CfdSpmRequester::CoeffCompute ||
        requester == CfdSpmRequester::CoeffDrain;
    if (coeffRequester && portBlocker == CfdSpmRequester::PathA) {
        coeffRequestsDelayedByPathA += result.portWaitCycles;
        pathACoeffConflictCycles += result.portWaitCycles;
    }
    if (coeffRequester && bankBlocker == CfdSpmRequester::PathA) {
        coeffRequestsDelayedByPathA += result.bankWaitCycles;
        pathACoeffConflictCycles += result.bankWaitCycles;
    }
    if (coeffRequester && portBlocker == CfdSpmRequester::Trsv5) {
        coeffRequestsDelayedByTrsv5 += result.portWaitCycles;
        trsvCoeffConflictCycles += result.portWaitCycles;
    }
    if (coeffRequester && bankBlocker == CfdSpmRequester::Trsv5) {
        coeffRequestsDelayedByTrsv5 += result.bankWaitCycles;
        trsvCoeffConflictCycles += result.bankWaitCycles;
    }
    return result;
}

void
CfdLocalSpm::recordRead(Addr ea, int row_id, unsigned logical_size,
                        unsigned physical_size, uint64_t latency,
                        uint64_t read_ports, uint64_t outstanding_limit,
                        uint64_t banks, bool bank_conflict_enabled,
                        bool realistic, bool event_model,
                        uint64_t z_wb_ports, uint64_t queue_size,
                        uint64_t bank_granularity)
{
    const uint64_t ports = std::max<uint64_t>(1, read_ports);
    const uint64_t num_banks = std::max<uint64_t>(1, banks);
    const uint64_t wb_ports = std::max<uint64_t>(1, z_wb_ports);
    const uint64_t q_size = std::max<uint64_t>(1, queue_size);
    const uint64_t bank_grain = std::max<uint64_t>(1, bank_granularity);
    const uint64_t phys_size =
        std::max<uint64_t>(logical_size, physical_size);
    uint64_t modeled_latency = latency;

    bytesRead += logical_size;
    logicalBytesRead += logical_size;
    physicalBytesRead += phys_size;
    numReads++;

    if (event_model) {
        resizeEventState(ports, num_banks, wb_ports, q_size);

        const uint64_t now = curTick() / cfdTicksPerCycle();
        auto queue_it = std::min_element(queueSlotFreeCycle.begin(),
                                         queueSlotFreeCycle.end());
        const uint64_t accept_cycle = std::max(now, *queue_it);
        if (accept_cycle > now)
            queueFullCycles += accept_cycle - now;

        auto port_it = std::min_element(readPortNextFreeCycle.begin(),
                                        readPortNextFreeCycle.end());
        const unsigned port_index =
            std::distance(readPortNextFreeCycle.begin(), port_it);
        const CfdSpmRequester port_blocker =
            readPortLastRequester[port_index];
        uint64_t start_cycle = std::max(accept_cycle, *port_it);
        const uint64_t patha_port_wait = start_cycle - accept_cycle;
        if (start_cycle > accept_cycle)
            portConflictCycles += start_cycle - accept_cycle;

        const uint64_t bank =
            selectLocalSpmBank(ea, row_id, bank_grain, num_banks);
        const CfdSpmRequester bank_blocker = bankLastRequester[bank];
        if (bank_conflict_enabled) {
            const uint64_t before_bank = start_cycle;
            start_cycle = std::max(start_cycle, bankNextFreeCycle[bank]);
            if (start_cycle > before_bank)
                bankConflictCycles += start_cycle - before_bank;
        }
        const uint64_t patha_bank_wait =
            start_cycle - (accept_cycle + patha_port_wait);

        *port_it = start_cycle + 1;
        readPortLastRequester[port_index] = CfdSpmRequester::PathA;
        if (bank_conflict_enabled)
            bankNextFreeCycle[bank] = start_cycle + 1;
        if (bank_conflict_enabled)
            bankLastRequester[bank] = CfdSpmRequester::PathA;

        auto is_coeff = [](CfdSpmRequester requester) {
            return requester == CfdSpmRequester::CoeffInput ||
                   requester == CfdSpmRequester::CoeffCompute ||
                   requester == CfdSpmRequester::CoeffDrain;
        };
        if (is_coeff(port_blocker) && patha_port_wait) {
            pathARequestsDelayedByCoeff += patha_port_wait;
            pathACoeffConflictCycles += patha_port_wait;
        }
        if (bank_conflict_enabled && is_coeff(bank_blocker) &&
            patha_bank_wait) {
            pathARequestsDelayedByCoeff += patha_bank_wait;
            pathACoeffConflictCycles += patha_bank_wait;
        }

        const uint64_t response_cycle = start_cycle + latency;
        auto wb_it = std::min_element(zWbNextFreeCycle.begin(),
                                      zWbNextFreeCycle.end());
        const uint64_t z_wb_cycle = std::max(response_cycle, *wb_it);
        if (z_wb_cycle > response_cycle)
            zWritebackConflictCycles += z_wb_cycle - response_cycle;
        *wb_it = z_wb_cycle + 1;

        const uint64_t complete_cycle = z_wb_cycle;
        *queue_it = complete_cycle;
        modeled_latency = std::max<uint64_t>(1, complete_cycle - now);

        createLocalEventRequest(ea, row_id, logical_size, phys_size, bank,
                                accept_cycle, start_cycle, response_cycle,
                                z_wb_cycle, complete_cycle,
                                CfdSpmRequester::PathA, false, {});

        const uint64_t request_to_response =
            std::max<uint64_t>(0, response_cycle - now);
        const uint64_t zwb_wait =
            std::max<uint64_t>(0, z_wb_cycle - response_cycle);
        requestToResponseTotal += request_to_response;
        avgRequestToResponse =
            requestToResponseTotal.value() /
            std::max<statistics::Counter>(1, numReads.value());
        responseToZWritebackTotal += zwb_wait;
        avgZWritebackWait =
            responseToZWritebackTotal.value() /
            std::max<statistics::Counter>(1, numReads.value());
        if (zwb_wait > maxZWritebackWait.value())
            maxZWritebackWait = zwb_wait;
        vectorZwbRequests++;
        vectorZwbLmatRequests++;
        vectorZwbWaitTotal += zwb_wait;
        vectorZwbAvgWait =
            vectorZwbWaitTotal.value() /
            std::max<statistics::Counter>(1, vectorZwbRequests.value());
        if (zwb_wait > vectorZwbMaxWait.value())
            vectorZwbMaxWait = zwb_wait;
        if (zwb_wait > 0) {
            vectorZwbConflicts++;
            vectorZwbConflictCycles += zwb_wait;
        }

        uint64_t outstanding = 0;
        for (const auto slot_free : queueSlotFreeCycle) {
            if (slot_free > now)
                outstanding++;
        }
        outstandingReadsAvg = outstanding;
        if (outstanding > outstandingReadsMax.value())
            outstandingReadsMax = outstanding;

        portBusyCycles++;
        if (outstanding_limit <= 1 && latency > 1) {
            queueFullCycles += latency - 1;
            portConflictCycles += latency - 1;
            localEventOutstandingFullEvents++;
        }
        if (bank_conflict_enabled && num_banks == 1 && latency > 1)
            bankConflictCycles += latency - 1;
    } else {
        portBusyCycles += (latency + ports - 1) / ports;

        const uint64_t outstanding =
            std::min<uint64_t>(outstanding_limit, ports);
        outstandingReadsAvg = outstanding;
        if (outstanding > outstandingReadsMax.value())
            outstandingReadsMax = outstanding;

        if (bank_conflict_enabled) {
            const uint64_t bank =
                selectLocalSpmBank(ea, row_id, logical_size, num_banks);
        static Tick last_tick = 0;
        static uint64_t last_bank = ~0ULL;
        if (curTick() == last_tick && bank == last_bank)
            bankConflictCycles++;
        last_tick = curTick();
        last_bank = bank;
        }

        if (realistic) {
        static Tick last_read_tick = 0;
        static uint64_t reads_this_tick = 0;
        if (curTick() != last_read_tick) {
            last_read_tick = curTick();
            reads_this_tick = 0;
        }
        if (reads_this_tick >= ports)
            portConflictCycles++;
        reads_this_tick++;

        const uint64_t wb_ports = std::max<uint64_t>(1, z_wb_ports);
        static Tick last_wb_tick = 0;
        static uint64_t wb_this_tick = 0;
        if (curTick() != last_wb_tick) {
            last_wb_tick = curTick();
            wb_this_tick = 0;
        }
        if (wb_this_tick >= wb_ports)
            zWritebackConflictCycles++;
        wb_this_tick++;
        }
    }

    readLatencyTotal += modeled_latency;
    avgReadLatency =
        readLatencyTotal.value() /
        std::max<statistics::Counter>(1, numReads.value());
    if (modeled_latency > maxReadLatency.value())
        maxReadLatency = modeled_latency;
    readsPerCycle = ports;
    bytesPerCycle = ports * phys_size;
    physicalBytesPerCycle = ports * phys_size;
    logicalBytesPerCycle = ports * logical_size;
}

void
CfdLocalSpm::setCurrentZaValue(const std::array<double, 5> &value)
{
    zaCurrentCol0 = value;
    zaCurrentCols[0] = value;
}

void
CfdLocalSpm::setCurrentZaCols(const ZaColumns &cols)
{
    zaCurrentCols = cols;
    zaCurrentCol0 = cols[0];
}

bool
CfdLocalSpm::useZaRenameMode() const
{
    const char *mode = std::getenv("GEM5_CFD_ZA_STATE_MODE");
    return mode && std::strcmp(mode, "rename") == 0;
}

CfdLocalSpm::PhysZAState *
CfdLocalSpm::latestZaPhys()
{
    for (auto it = zaPhysStates.rbegin(); it != zaPhysStates.rend(); ++it) {
        if (!it->squashed && it->ready)
            return &(*it);
    }
    return nullptr;
}

const CfdLocalSpm::PhysZAState *
CfdLocalSpm::latestZaPhys() const
{
    for (auto it = zaPhysStates.rbegin(); it != zaPhysStates.rend(); ++it) {
        if (!it->squashed && it->ready)
            return &(*it);
    }
    return nullptr;
}

void
CfdLocalSpm::updateZaLiveStats()
{
    zaPhysicalLive = 0;
    for (const auto &state : zaPhysStates) {
        if (!state.squashed)
            zaPhysicalLive++;
    }
    zaPhysicalLiveEnd = zaPhysicalLive;
    if (zaPhysicalLive > zaPhysicalLiveMax.value())
        zaPhysicalLiveMax = zaPhysicalLive;
}

void
CfdLocalSpm::copyColumnToLegacyCol0(PhysZAState &state)
{
    state.col0 = state.cols[0];
}

CfdLocalSpm::PhysZAState *
CfdLocalSpm::allocateZaPhys(uint64_t seq_num, bool zero)
{
    PhysZAState state;
    state.physId = nextZaPhysId++;
    state.producerSeqNum = seq_num;
    state.inputToken = zaToken;
    state.tokenId = ++zaToken;
    state.ready = true;
    state.zero = zero;
    if (!zero) {
        if (const auto *prev = latestZaPhys()) {
            state.col0 = prev->col0;
            state.cols = prev->cols;
        } else {
            state.col0 = zaCommittedCol0;
            state.cols = zaCommittedCols;
        }
    }

    zaPhysStates.push_back(state);
    zaRenameAllocs++;
    updateZaLiveStats();
    return &zaPhysStates.back();
}

std::array<double, 5>
CfdLocalSpm::latestZaValue() const
{
    if (useZaRenameMode()) {
        if (const auto *phys = latestZaPhys())
            return phys->col0;
        return zaCommittedCol0;
    }

    for (auto it = zaPendingEntries.rbegin(); it != zaPendingEntries.rend();
         ++it) {
        if (!it->squashed && it->ready)
            return it->value;
    }
    return zaCurrentCol0;
}

CfdLocalSpm::ZaColumns
CfdLocalSpm::latestZaCols() const
{
    if (useZaRenameMode()) {
        if (const auto *phys = latestZaPhys())
            return phys->cols;
        return zaCommittedCols;
    }

    for (auto it = zaPendingEntries.rbegin(); it != zaPendingEntries.rend();
         ++it) {
        if (!it->squashed && it->ready)
            return it->cols;
    }
    return zaCurrentCols;
}

void
CfdLocalSpm::recordZaZero(uint64_t seq_num)
{
    if (useZaRenameMode()) {
        if (auto *phys = allocateZaPhys(seq_num, true))
            setCurrentZaCols(phys->cols);
        return;
    }

    ZaPendingUpdateEntry entry;
    entry.seqNum = seq_num;
    entry.inputToken = zaToken;
    entry.outputToken = ++zaToken;
    entry.value = {};
    entry.cols = {};
    entry.ready = true;
    entry.zero = true;
    zaPendingEntries.push_back(entry);
    setCurrentZaValue(entry.value);
    setCurrentZaCols(entry.cols);
}

void
CfdLocalSpm::recordZaExecute(uint64_t seq_num, const double *col,
                             double scalar)
{
    zaTokensConsumed++;
    zaTokensProduced++;
    zaPendingUpdates++;

    if (useZaRenameMode()) {
        PhysZAState *phys = allocateZaPhys(seq_num, false);
        for (int i = 0; i < 5; ++i)
            phys->col0[i] += col[i] * scalar;
        phys->cols[0] = phys->col0;
        setCurrentZaValue(phys->col0);
        return;
    }

    ZaPendingUpdateEntry entry;
    entry.seqNum = seq_num;
    entry.inputToken = zaToken;
    entry.outputToken = ++zaToken;
    entry.value = latestZaValue();
    for (int i = 0; i < 5; ++i)
        entry.value[i] += col[i] * scalar;
    entry.cols = latestZaCols();
    entry.cols[0] = entry.value;
    entry.ready = true;
    zaPendingEntries.push_back(entry);
    setCurrentZaValue(entry.value);
    setCurrentZaCols(entry.cols);
}

void
CfdLocalSpm::recordZaPipeExecute(uint64_t seq_num, uint8_t k,
                                 const double *col, double scalar)
{
    if (k > 4)
        return;

    zaTokensConsumed++;
    zaTokensProduced++;
    zaPendingUpdates++;
    pathBPipeSteps++;
    pathBPipePendingUpdates++;

    auto count_col_write = [this](uint8_t col_idx) {
        switch (col_idx) {
          case 0: pathBPipeCol0Writes++; break;
          case 1: pathBPipeCol1Writes++; break;
          case 2: pathBPipeCol2Writes++; break;
          case 3: pathBPipeCol3Writes++; break;
          case 4: pathBPipeCol4Writes++; break;
          default: break;
        }
    };

    if (useZaRenameMode()) {
        PhysZAState *phys = allocateZaPhys(seq_num, false);
        phys->pathB = true;
        phys->pathBCol = k;
        pathBPipeForwardHits++;
        for (int i = 0; i < 5; ++i) {
            const double prev = (k == 0) ? 0.0 : phys->cols[k - 1][i];
            phys->cols[k][i] = prev + col[i] * scalar;
        }
        copyColumnToLegacyCol0(*phys);
        count_col_write(k);
        setCurrentZaCols(phys->cols);
        return;
    }

    ZaPendingUpdateEntry entry;
    entry.seqNum = seq_num;
    entry.inputToken = zaToken;
    entry.outputToken = ++zaToken;
    entry.cols = latestZaCols();
    entry.pathB = true;
    entry.pathBCol = k;
    for (int i = 0; i < 5; ++i) {
        const double prev = (k == 0) ? 0.0 : entry.cols[k - 1][i];
        entry.cols[k][i] = prev + col[i] * scalar;
    }
    entry.value = entry.cols[0];
    entry.ready = true;
    zaPendingEntries.push_back(entry);
    pathBPipeForwardHits++;
    count_col_write(k);
    setCurrentZaCols(entry.cols);
}

void
CfdLocalSpm::recordZaOuterExecute(uint64_t seq_num, uint8_t k,
                                  const double *col, double scalar)
{
    if (k > 4)
        return;

    zaTokensConsumed++;
    zaTokensProduced++;
    zaPendingUpdates++;
    pathCOuterSteps++;
    pathCOuterPendingUpdates++;

    auto count_col_write = [this](uint8_t col_idx) {
        switch (col_idx) {
          case 0: pathCOuterCol0Writes++; break;
          case 1: pathCOuterCol1Writes++; break;
          case 2: pathCOuterCol2Writes++; break;
          case 3: pathCOuterCol3Writes++; break;
          case 4: pathCOuterCol4Writes++; break;
          default: break;
        }
    };

    if (useZaRenameMode()) {
        PhysZAState *phys = allocateZaPhys(seq_num, false);
        phys->pathCOuter = true;
        phys->pathBCol = k;
        pathCOuterForwardHits++;
        for (int i = 0; i < 5; ++i) {
            const double prev = (k == 0) ? 0.0 : phys->cols[k - 1][i];
            phys->cols[k][i] = prev + col[i] * scalar;
        }
        copyColumnToLegacyCol0(*phys);
        count_col_write(k);
        setCurrentZaCols(phys->cols);
        return;
    }

    ZaPendingUpdateEntry entry;
    entry.seqNum = seq_num;
    entry.inputToken = zaToken;
    entry.outputToken = ++zaToken;
    entry.cols = latestZaCols();
    entry.pathCOuter = true;
    entry.pathBCol = k;
    for (int i = 0; i < 5; ++i) {
        const double prev = (k == 0) ? 0.0 : entry.cols[k - 1][i];
        entry.cols[k][i] = prev + col[i] * scalar;
    }
    entry.value = entry.cols[0];
    entry.ready = true;
    zaPendingEntries.push_back(entry);
    pathCOuterForwardHits++;
    count_col_write(k);
    setCurrentZaCols(entry.cols);
}

void
CfdLocalSpm::recordZaCommit(uint64_t seq_num)
{
    if (useZaRenameMode()) {
        for (auto &state : zaPhysStates) {
            if (state.producerSeqNum == seq_num && !state.squashed) {
                state.committed = true;
                archZaPhysId = state.physId;
                zaCommittedCol0 = state.col0;
                zaCommittedCols = state.cols;
                zaCommitUpdates++;
                if (state.pathB)
                    pathBPipeCommitUpdates++;
                if (state.pathCOuter)
                    pathCOuterCommitUpdates++;

                for (auto &old : zaPhysStates) {
                    if (!old.squashed && old.physId != archZaPhysId &&
                        old.producerSeqNum <= seq_num) {
                        old.squashed = true;
                        zaRenameFrees++;
                    }
                }
                updateZaLiveStats();
                break;
            }
        }
        return;
    }

    for (auto &entry : zaPendingEntries) {
        if (entry.seqNum == seq_num && !entry.squashed) {
            entry.committed = true;
            zaCommittedCol0 = entry.value;
            zaCommittedCols = entry.cols;
            zaCommitUpdates++;
            if (entry.pathB)
                pathBPipeCommitUpdates++;
            if (entry.pathCOuter)
                pathCOuterCommitUpdates++;
            break;
        }
    }
}

void
CfdLocalSpm::recordZaSquash(uint64_t seq_num)
{
    if (useZaRenameMode()) {
        for (auto &state : zaPhysStates) {
            if (state.producerSeqNum == seq_num && !state.squashed) {
                if (state.pathB)
                    pathBPipeSquashDiscards++;
                if (state.pathCOuter)
                    pathCOuterSquashDiscards++;
                state.squashed = true;
                zaSquashDiscards++;
                zaRenameRollbacks++;
                zaRenameFrees++;
                break;
            }
        }
        updateZaLiveStats();
        setCurrentZaValue(latestZaValue());
        return;
    }

    for (auto &entry : zaPendingEntries) {
        if (entry.seqNum == seq_num && !entry.squashed) {
            entry.squashed = true;
            zaSquashDiscards++;
            if (entry.pathB)
                pathBPipeSquashDiscards++;
            if (entry.pathCOuter)
                pathCOuterSquashDiscards++;
            break;
        }
    }
    setCurrentZaValue(latestZaValue());
    setCurrentZaCols(latestZaCols());
}

bool
CfdLocalSpm::readZaCol0(double *dst, unsigned lanes)
{
    if (!dst || lanes == 0)
        return false;

    zaMovaReads++;
    vectorZwbRequests++;
    vectorZwbMovaRequests++;
    const auto value = latestZaValue();
    if (useZaRenameMode()) {
        if (const auto *phys = latestZaPhys()) {
            if (phys->ready)
                zaMovaRenameHits++;
            else
                zaMovaBlockedNotReady++;
        } else {
            zaMovaArchFallbacks++;
        }
    }

    const unsigned count = std::min<unsigned>(lanes, value.size());
    for (unsigned i = 0; i < count; ++i)
        dst[i] = value[i];
    return true;
}

bool
CfdLocalSpm::readZaPathBFinalCol(double *dst, unsigned lanes)
{
    if (!dst || lanes == 0)
        return false;

    zaMovaReads++;
    pathBPipeFinalReads++;
    vectorZwbRequests++;
    vectorZwbMovaRequests++;
    const auto cols = latestZaCols();
    if (useZaRenameMode()) {
        if (const auto *phys = latestZaPhys()) {
            if (phys->ready)
                zaMovaRenameHits++;
            else
                zaMovaBlockedNotReady++;
        } else {
            zaMovaArchFallbacks++;
        }
    }

    const unsigned count = std::min<unsigned>(lanes, cols[4].size());
    for (unsigned i = 0; i < count; ++i)
        dst[i] = cols[4][i];
    return true;
}

bool
CfdLocalSpm::readZaPathCOuterFinalCol(double *dst, unsigned lanes)
{
    if (!dst || lanes == 0)
        return false;

    zaMovaReads++;
    pathCOuterFinalReads++;
    vectorZwbRequests++;
    vectorZwbMovaRequests++;
    const auto cols = latestZaCols();
    if (useZaRenameMode()) {
        if (const auto *phys = latestZaPhys()) {
            if (phys->ready)
                zaMovaRenameHits++;
            else {
                zaMovaBlockedNotReady++;
                pathCOuterMovaWaitCycles++;
            }
        } else {
            zaMovaArchFallbacks++;
        }
    }

    const unsigned count = std::min<unsigned>(lanes, cols[4].size());
    for (unsigned i = 0; i < count; ++i)
        dst[i] = cols[4][i];
    return true;
}

bool
CfdLocalSpm::readZaMovaOverride(double *dst, unsigned lanes)
{
    const char *pathb = std::getenv("GEM5_CFD_PATHB_ENABLE");
    if (pathb && std::strcmp(pathb, "1") == 0)
        return readZaPathBFinalCol(dst, lanes);
    const char *pathc_outer = std::getenv("GEM5_CFD_PATHC_OUTER_ENABLE");
    if (pathc_outer && std::strcmp(pathc_outer, "1") == 0)
        return readZaPathCOuterFinalCol(dst, lanes);
    return readZaCol0(dst, lanes);
}

} // namespace ArmISA
} // namespace gem5
