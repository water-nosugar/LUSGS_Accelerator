#ifndef __ARCH_ARM_CFD_COEFF_PREPROCESS_CONTROLLER_HH__
#define __ARCH_ARM_CFD_COEFF_PREPROCESS_CONTROLLER_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5
{

class ExecContext;

namespace ArmISA
{

enum class CfdCoeffPreprocessStatus : uint64_t
{
    Complete = 0,
    Busy = 1,
    BadDescriptor = 2,
    BadAlignment = 3,
    QueueFull = 4,
    BadToken = 5,
    LuFailure = 6,
    GenerationMismatch = 7,
    InternalError = 8,
    Cancelled = 9,
    AlreadyComplete = 10,
    CancelPending = 11,
    DInvReady = 12,
    DInvLBarReady = 13,
    DInvLBarUBarReady = 14,
};

enum CfdCoeffPreprocessFlags : uint64_t
{
    CFD_COEFF_PRE_TRSV = 1ull << 0,
    CFD_COEFF_PRE_COEFF3 = 1ull << 1,
    CFD_COEFF_PRE_PARTIAL_OUTPUT = 1ull << 2,
    CFD_COEFF_PRE_LU_FORWARD = 1ull << 3,
    CFD_COEFF_PRE_PACKED_LU_INPUT = 1ull << 4,
    CFD_COEFF_PRE_STREAMING_PROGRESS = 1ull << 5,
    // An all-zero coefficient mask preserves the legacy COEFF3 contract:
    // generate DInv, Lbar, and Ubar.  Once any NEED bit is present the mask is
    // explicit and unrequested batches must not enter solve or drain queues.
    CFD_COEFF_PRE_NEED_DINV = 1ull << 6,
    CFD_COEFF_PRE_NEED_LBAR = 1ull << 7,
    CFD_COEFF_PRE_NEED_UBAR = 1ull << 8,
    // Streaming-only ownership transfer: after publishing the complete
    // guest record, the controller retires the request and releases token
    // storage without requiring a guest WAIT/reap instruction.
    CFD_COEFF_PRE_STREAMING_AUTO_RETIRE = 1ull << 9,
    // Stage B3 request-local DInv-column consumer.  The DIRECT bit changes
    // only the publication policy: DInv is still computed, but its 200-byte
    // matrix drain is replaced by a 40-byte base-vector publication.
    CFD_COEFF_PRE_DINV_BASE_CONSUMER = 1ull << 10,
    CFD_COEFF_PRE_DINV_DIRECT_BYPASS = 1ull << 11,
    // Stage B4 consumes Lbar columns with the same bounded ColumnFma5
    // resource and forwards the final dq_star between adjacent cells inside
    // the controller.  DIRECT suppresses the Lbar matrix/base-vector drains;
    // shadow keeps both legacy guest-visible outputs for bitwise checking.
    CFD_COEFF_PRE_LBAR_FORWARD_CONSUMER = 1ull << 12,
    CFD_COEFF_PRE_LBAR_DIRECT_BYPASS = 1ull << 13,
    // Test-only descriptor faults. Normal runners never set these bits.
    CFD_COEFF_PRE_TEST_HANDOFF_WRONG_CELL = 1ull << 14,
    CFD_COEFF_PRE_TEST_HANDOFF_WRONG_GENERATION = 1ull << 15,
    CFD_COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH = 1ull << 16,
    // Stage C1 line-level ownership.  The public descriptor remains 160 B:
    // reserved[0] is the R-vector base, reserved[1] is the bounded request
    // window, reserved[3] is the dq_star base, and reserved[4] is the cell
    // count.  Matrix addresses are line bases with tightly packed 5x5 cells.
    // The controller expands the line into ordinary auto-retired B4 requests.
    CFD_COEFF_PRE_LINE_AUTONOMOUS = 1ull << 17,
    // Stage C2/B5 keeps Ubar and dq_star in the line request and performs
    // the reverse wavefront with the shared ColumnFma5 resource.  In a line
    // descriptor reserved[2] is the final dq vector-array base.
    CFD_COEFF_PRE_LINE_BACKWARD_DIRECT = 1ull << 18,
    // Stage 7 cross-sweep contract. The guest publishes a coefficient
    // version in the parent line descriptor's cellId. CACHE_ENABLE permits
    // reuse only on an exact {lineId, cell-count, version} hit; DIRTY forces
    // a rebuild and atomically replaces that version after all child cells
    // complete. The controller never infers coefficient stability.
    CFD_COEFF_PRE_LINE_COEFF_CACHE = 1ull << 19,
    CFD_COEFF_PRE_LINE_COEFF_DIRTY = 1ull << 20,
};

// Guest-visible descriptor. Keep this layout mirrored in the Step2 benchmark.
struct CfdCoeffPreprocessDescriptor
{
    uint64_t version;
    uint64_t size;
    uint64_t flags;
    uint64_t generation;
    uint64_t lineId;
    uint64_t cellId;
    uint64_t dAddr;
    uint64_t lAddr;
    uint64_t uAddr;
    uint64_t luOutAddr;
    uint64_t dInvOutAddr;
    uint64_t lBarOutAddr;
    uint64_t uBarOutAddr;
    uint64_t recordAddr;
    double pivotEpsilon;
    uint64_t reserved[5];
};

// Per-request timestamps and counters written by the event controller.
struct CfdCoeffPreprocessRecord
{
    uint64_t requestId;
    uint64_t generation;
    uint64_t lineId;
    uint64_t cellId;
    uint64_t status;
    uint64_t slot;

    uint64_t issueCycle;
    uint64_t inputStartCycle;
    uint64_t inputDoneCycle;
    uint64_t luIssueCycle;
    uint64_t luFirstResultCycle;
    uint64_t luCompleteCycle;
    uint64_t solveIssueCycle;
    uint64_t firstColumnCycle;
    uint64_t dInvReadyCycle;
    uint64_t lBarReadyCycle;
    uint64_t uBarReadyCycle;
    uint64_t dInvColumnReadyCycle[5];
    uint64_t lBarColumnReadyCycle[5];
    uint64_t uBarColumnReadyCycle[5];
    uint64_t solveCompleteCycle;
    uint64_t dInvDrainStart;
    uint64_t dInvDrainDone;
    uint64_t lBarDrainStart;
    uint64_t lBarDrainDone;
    uint64_t uBarDrainStart;
    uint64_t uBarDrainDone;
    uint64_t luDrainStart;
    uint64_t luDrainDone;
    uint64_t completeCycle;

    uint64_t inputBytes;
    uint64_t inputWriteRequests;
    uint64_t outputBytes;
    uint64_t drainRequests;
    uint64_t luDivIssued;
    uint64_t luMulIssued;
    uint64_t luSubIssued;
    uint64_t coeffDivIssued;
    uint64_t coeffMulIssued;
    uint64_t coeffSubIssued;

    uint64_t stallInputSlot;
    uint64_t stallLuPending;
    uint64_t stallSolvePending;
    uint64_t stallOutputRing;
    uint64_t stallLuDivider;
    uint64_t stallLuMulSub;
    uint64_t stallCoeffDivider;
    uint64_t stallCoeffMulSub;
    uint64_t stallDependency;
    uint64_t stallSpmReadPort;
    uint64_t stallSpmWritePort;
    uint64_t stallSpmBank;
    uint64_t stallDrainQueue;

    uint64_t inputActiveCycles;
    uint64_t luActiveCycles;
    uint64_t solveActiveCycles;
    uint64_t drainActiveCycles;
    uint64_t inputLuOverlapCycles;
    uint64_t inputSolveOverlapCycles;
    uint64_t luSolveOverlapCycles;
    uint64_t computeDrainOverlapCycles;
    uint64_t threeWayOverlapCycles;
    uint64_t dividerBusyLaneCycles;
    uint64_t mulBusyLaneCycles;
    uint64_t subBusyLaneCycles;
    uint64_t maxOutputOccupancy;

    uint64_t schedulerScans;
    uint64_t schedulerSelections;
    uint64_t schedulerNoSelection;
    uint64_t readyRhsCountSum;
    uint64_t readyRhsCountMax;
    uint64_t cyclesWithNoReadyRhs;
    uint64_t dividerReadyCandidates;
    uint64_t mulSubReadyCandidates;
    uint64_t rhsStarvationCycles;
    uint64_t rhsMaxWaitCycles;
    uint64_t rhsWaitCycles;
    uint64_t identityBatchWaitCycles;
    uint64_t lBarBatchWaitCycles;
    uint64_t uBarBatchWaitCycles;
    uint64_t coeffDividerBusyLaneCycles;
    uint64_t coeffDividerIdleLaneCycles;
    uint64_t coeffMulBusyLaneCycles;
    uint64_t coeffSubBusyLaneCycles;
    uint64_t schedulerCycles;
    uint64_t luStepReadyCycle[5];
    uint64_t luStepFirstUseLatency[5];
    uint64_t earlySolveIssued;
    uint64_t earlySolveMissed;
    uint64_t trsmBlockedByLuStepCycles;
    uint64_t sameCellLuSolveOverlapCycles;
    uint64_t crossCellLuSolveOverlapCycles;
    uint64_t coeffSpmPacketIssued;
    uint64_t coeffSpmPacketCompleted;
    uint64_t coeffInputPacketBytes;
    uint64_t coeffDrainPacketBytes;
    uint64_t coeffSpmReadPortStalls;
    uint64_t coeffSpmWritePortStalls;
    uint64_t coeffSpmBankStalls;
    uint64_t coeffSpmOutstandingStalls;

    // Stage B1.5/B2.5 lifecycle and fixed-mask scheduler evidence.  These
    // fields are appended so the descriptor ABI and all existing encodings
    // remain unchanged.
    uint64_t autoRetired;
    uint64_t autoRetireError;
    uint64_t maskTableLookups;
    uint64_t rhsMaskSchedulerScans;
    uint64_t rhsMaskUsefulIssues;
    uint64_t rhsMaskEmptyScans;

    // Stage B3 DInv-column consumer and base publication evidence.  The
    // descriptor remains 160 bytes: reserved[0] is rhsAddr and reserved[1]
    // is baseOutAddr whenever DINV_BASE_CONSUMER is set.
    uint64_t rhsVectorIssueCycle;
    uint64_t rhsVectorReadyCycle;
    uint64_t baseComputeReadyCycle;
    uint64_t basePublishIssueCycle;
    uint64_t baseReadyCycle;
    uint64_t baseReady;
    uint64_t baseConsumerError;
    uint64_t baseConsumedColumnMask;
    uint64_t dInvColumnsReady;
    uint64_t dInvColumnsQueued;
    uint64_t dInvColumnsIssued;
    uint64_t dInvColumnsCompleted;
    uint64_t dInvColumnsConsumed;
    uint64_t dInvColumnsDuplicateRejected;
    uint64_t dInvColumnsGenerationRejected;
    uint64_t dInvColumnsOutOfOrderHeld;
    uint64_t columnFmaQueueMaxOccupancy;
    uint64_t columnFmaQueueFullStalls;
    uint64_t columnFmaIssueCycles;
    uint64_t columnFmaBusyCycles;
    uint64_t columnFmaCompletions;
    uint64_t columnFmaRetries;
    uint64_t columnFmaReadyButBlockedCycles;
    uint64_t baseAccumulatorsAllocated;
    uint64_t baseAccumulatorsCompleted;
    uint64_t baseAccumulatorsCancelled;
    uint64_t baseAccumulatorMissingColumns;
    uint64_t baseAccumulatorDoubleConsumes;
    uint64_t baseAccumulatorStaleWrites;
    uint64_t baseVectorPublishCount;
    uint64_t baseVectorPublishBytes;
    uint64_t baseVectorPublishStalls;
    uint64_t dInvMatrixDrainsAvoided;
    uint64_t dInvMatrixDrainBytesAvoided;
    uint64_t columnFmaQueueOccupancySum;
    uint64_t columnFmaQueueSampleCycles;

    // Stage B4 Lbar forward consumer, adjacent-cell handoff, combine, and
    // timed dq_star publication evidence.  reserved[2] is the shadow-only
    // correction output and reserved[3] is the final dq_star output.
    uint64_t dqStarReady;
    uint64_t dqStarReadyCycle;
    uint64_t dqStarPublishCycle;
    uint64_t lbarConsumedColumnMask;
    uint64_t correctionReadyCycle;
    uint64_t forwardCombineIssueCycle;
    uint64_t forwardCombineCompleteCycle;
    uint64_t forwardHandoffReadyCycle;
    uint64_t forwardHandoffConsumedCycle;
    uint64_t forwardConsumerError;
    uint64_t lbarColumnsReady;
    uint64_t lbarColumnsQueued;
    uint64_t lbarColumnsIssued;
    uint64_t lbarColumnsCompleted;
    uint64_t lbarColumnsConsumed;
    uint64_t lbarColumnsDuplicateRejected;
    uint64_t lbarColumnsGenerationRejected;
    uint64_t lbarColumnsOutOfOrderHeld;
    uint64_t correctionAccumulatorsAllocated;
    uint64_t correctionAccumulatorsCompleted;
    uint64_t correctionAccumulatorsCancelled;
    uint64_t correctionAccumulatorMissingColumns;
    uint64_t correctionAccumulatorDoubleConsumes;
    uint64_t correctionAccumulatorStaleWrites;
    uint64_t forwardHandoffsProduced;
    uint64_t forwardHandoffsConsumed;
    uint64_t forwardHandoffWaitCycles;
    uint64_t forwardHandoffMaxLive;
    uint64_t forwardHandoffOverwriteErrors;
    uint64_t forwardHandoffWrongCellErrors;
    uint64_t forwardHandoffWrongGenerationErrors;
    uint64_t forwardHandoffDoubleConsumes;
    uint64_t forwardHandoffMissingConsumes;
    uint64_t forwardCombineQueued;
    uint64_t forwardCombineIssued;
    uint64_t forwardCombineCompleted;
    uint64_t forwardCombineQueueMaxOccupancy;
    uint64_t forwardCombineQueueFullStalls;
    uint64_t forwardCombineBusyCycles;
    uint64_t forwardCombineReadyButBlockedCycles;
    uint64_t dqStarPublishCount;
    uint64_t dqStarPublishBytes;
    uint64_t dqStarPublishStalls;
    uint64_t baseStandalonePublishesAvoided;
    uint64_t baseStandalonePublishBytesAvoided;
    uint64_t lbarMatrixDrainsAvoided;
    uint64_t lbarMatrixDrainBytesAvoided;
    uint64_t lbarGuestReloadBytesAvoided;
    uint64_t pathaLbarMvmEliminated;
    uint64_t pathaLbarCustomInstructionsEliminated;
    uint64_t lbarConsumerReadyButBlockedCycles;
    uint64_t lbarConsumerWaitPreviousDqCycles;
    uint64_t lbarConsumerWaitColumnCycles;
    uint64_t lbarConsumerWaitEngineCycles;
    uint64_t consumerSchedulerAgingPromotions;
    uint64_t consumerSchedulerStarvationViolations;
    uint64_t frontierWaitDqStarCycles;
    uint64_t frontierWaitPreviousDqCycles;
    uint64_t frontierWaitLbarColumnsCycles;
    uint64_t frontierWaitConsumerEngineCycles;
    uint64_t frontierWaitDqStarPublishCycles;
};

static_assert(sizeof(CfdCoeffPreprocessDescriptor) == 160);

bool cfdCoeffPreprocessEventEnabled();
uint64_t cfdCoeffPreprocessLaunch(ExecContext *xc, Addr descriptorAddr);
uint64_t cfdCoeffPreprocessWait(uint64_t token);
uint64_t cfdCoeffPreprocessCancel(uint64_t token);

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_COEFF_PREPROCESS_CONTROLLER_HH__
