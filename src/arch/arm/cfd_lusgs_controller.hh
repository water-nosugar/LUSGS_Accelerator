#ifndef __ARCH_ARM_CFD_LUSGS_CONTROLLER_HH__
#define __ARCH_ARM_CFD_LUSGS_CONTROLLER_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5
{

class ExecContext;

namespace ArmISA
{

static constexpr uint32_t LUSGS_FLAG_WRITE_DQ = 1u << 0;
static constexpr uint32_t LUSGS_FLAG_UPDATE_Q = 1u << 1;
static constexpr uint32_t LUSGS_FLAG_CHECK_BOUNDS = 1u << 2;
static constexpr uint32_t LUSGS_FLAG_TRACE = 1u << 3;

struct CfdLusgsDescriptor
{
    uint64_t lu_a_base;
    uint64_t c_base;
    uint64_t bbar_base;
    uint64_t rhs_base;
    uint64_t dqstar_base;
    uint64_t dq_base;
    uint64_t q_base;
    uint32_t n_lines;
    uint32_t n_cells;
    uint32_t line_stride_bytes;
    uint32_t matrix_cell_stride_bytes;
    uint32_t vector_cell_stride_bytes;
    uint32_t flags;
    uint32_t tile_cells;
    double omega;
};

enum class CfdLusgsStatus : uint64_t
{
    Complete = 0,
    Busy = 1,
    BadDescriptor = 2,
    BadShape = 3,
    BadStride = 4,
    BadAlignment = 5,
    UnsupportedFlags = 6,
    BadToken = 7,
    UnsupportedStage = 8,
    SpmCapacity = 9,
    WatchdogTimeout = 10,
    UnexpectedCompletion = 11,
    InternalStateError = 12,
};

struct CfdLusgsControllerRecord
{
    uint64_t launches = 0;
    uint64_t committedLaunches = 0;
    uint64_t squashedLaunches = 0;
    uint64_t completedTasks = 0;
    uint64_t failedTasks = 0;
    uint64_t busyCycles = 0;
    uint64_t totalCycles = 0;
    uint64_t forwardCells = 0;
    uint64_t backwardCells = 0;
    uint64_t updatedQCells = 0;
    uint64_t pathaRequests = 0;
    uint64_t trsv5Requests = 0;
    uint64_t vec5SubRequests = 0;
    uint64_t vec5CopyRequests = 0;
    uint64_t vec5AxpyRequests = 0;
    uint64_t spmReadRequests = 0;
    uint64_t spmWriteRequests = 0;
    uint64_t pathaWaitCycles = 0;
    uint64_t trsv5WaitCycles = 0;
    uint64_t vec5WaitCycles = 0;
    uint64_t spmWaitCycles = 0;
    uint64_t writebackWaitCycles = 0;
    uint64_t forwardDependencyStalls = 0;
    uint64_t backwardDependencyStalls = 0;
    uint64_t contextFullStalls = 0;
    uint64_t noReadyContextCycles = 0;
    uint64_t arbiterStalls = 0;
    uint64_t contextAlloc = 0;
    uint64_t contextFree = 0;
    uint64_t activeContextCycles = 0;
    uint64_t maxActiveContexts = 0;
    uint64_t contextSwitches = 0;
    uint64_t luBytes = 0;
    uint64_t cBytes = 0;
    uint64_t bbarBytes = 0;
    uint64_t rhsBytes = 0;
    uint64_t dqstarReadBytes = 0;
    uint64_t dqstarWriteBytes = 0;
    uint64_t dqReadBytes = 0;
    uint64_t dqWriteBytes = 0;
    uint64_t qReadBytes = 0;
    uint64_t qWriteBytes = 0;
    uint64_t temporaryBytes = 0;
    uint64_t temporaryResultStores = 0;
    uint64_t temporaryResultLoads = 0;
    uint64_t controllerBufferReads = 0;
    uint64_t controllerBufferWrites = 0;
    uint64_t dqstarExternalWrites = 0;
    uint64_t dqstarExternalReads = 0;
    uint64_t finalDqWrites = 0;
    uint64_t tileCoefficientBytes = 0;
    uint64_t tileVectorBytes = 0;
    uint64_t tileTemporaryBytes = 0;
    uint64_t tileTotalBytes = 0;
    uint64_t spmCapacity = 0;
    uint64_t tileLoads = 0;
    uint64_t tilePrefetches = 0;
    uint64_t prefetchUseful = 0;
    uint64_t prefetchLate = 0;
    uint64_t bufferSwap = 0;
    uint64_t bufferConflictStalls = 0;
    uint64_t pathaArbiterCpuRequests = 0;
    uint64_t pathaArbiterLusgsRequests = 0;
    uint64_t pathaArbiterBusyStalls = 0;
    uint64_t pathaArbiterOwnershipCycles = 0;
    uint64_t eventLaunches = 0;
    uint64_t eventCompleted = 0;
    uint64_t eventFailed = 0;
    uint64_t eventActualCycles = 0;
    uint64_t eventEventsProcessed = 0;
    uint64_t eventSchedulerTicks = 0;
    uint64_t eventActiveCycles = 0;
    uint64_t eventIdleCycles = 0;
    uint64_t eventDecodedLaunches = 0;
    uint64_t eventCommittedLaunches = 0;
    uint64_t eventSquashedLaunches = 0;
    uint64_t eventSquashedWaits = 0;
    uint64_t eventStaleCompletions = 0;
    uint64_t eventUnexpectedCompletions = 0;
    uint64_t eventDuplicateCompletions = 0;
    uint64_t eventRequestIdMismatches = 0;
    uint64_t eventGenerationMismatches = 0;
    uint64_t eventWrongStateCompletions = 0;
    uint64_t eventPendingRequestAlloc = 0;
    uint64_t eventPendingRequestFree = 0;
    uint64_t eventPendingRequestFullStalls = 0;
    uint64_t eventMaxPendingRequests = 0;
    uint64_t eventCancelledRequests = 0;
    uint64_t eventLifecycleIdleCycles = 0;
    uint64_t eventLifecycleQueuedCycles = 0;
    uint64_t eventLifecycleRunningCycles = 0;
    uint64_t eventLifecycleCompletedNotReapedCycles = 0;
    uint64_t eventLifecycleErrorNotReapedCycles = 0;
    uint64_t eventTokensReaped = 0;
    uint64_t eventWaitInstructions = 0;
    uint64_t eventBusyPolls = 0;
    uint64_t eventSuccessfulWaits = 0;
    uint64_t eventErrorWaits = 0;
    uint64_t eventBadTokenWaits = 0;
    uint64_t eventTokenReapCycles = 0;
    uint64_t eventLaunchCommitTick = 0;
    uint64_t eventDescriptorSnapshotTick = 0;
    uint64_t eventFirstResourceIssueTick = 0;
    uint64_t eventTaskCompleteTick = 0;
    uint64_t eventSuccessfulWaitTick = 0;
    uint64_t eventTokenReapTick = 0;
    uint64_t eventWatchdogTimeouts = 0;
    uint64_t eventStateTransitions = 0;
    uint64_t eventNoProgressCycles = 0;
    uint64_t eventDoubleScheduleErrors = 0;
    uint64_t eventNoReadyContextCycles = 0;
    uint64_t eventPathaRequests = 0;
    uint64_t eventPathaAccepted = 0;
    uint64_t eventPathaRetries = 0;
    uint64_t eventPathaCompleted = 0;
    uint64_t eventPathaWaitCycles = 0;
    uint64_t eventPathaSlotFullStalls = 0;
    uint64_t eventPathaPackStalls = 0;
    uint64_t eventPathaStoreStalls = 0;
    uint64_t eventPathaMvmRequests = 0;
    uint64_t eventPathaMvmAccepted = 0;
    uint64_t eventPathaMvmRetries = 0;
    uint64_t eventPathaMvmCompleted = 0;
    uint64_t eventPathaMvmWaitCycles = 0;
    uint64_t eventPathaMatLdRequests = 0;
    uint64_t eventPathaMatLdAccepted = 0;
    uint64_t eventPathaMatLdRetries = 0;
    uint64_t eventPathaMatLdCompleted = 0;
    uint64_t eventPathaMatLdBusyCycles = 0;
    uint64_t eventPathaMatLdBytes = 0;
    uint64_t eventPathaDotpRequests = 0;
    uint64_t eventPathaDotpAccepted = 0;
    uint64_t eventPathaDotpRetries = 0;
    uint64_t eventPathaDotpCompleted = 0;
    uint64_t eventPathaDotpBusyCycles = 0;
    uint64_t eventPathaDotpPipelineOccupancy = 0;
    uint64_t eventPathaPackRequests = 0;
    uint64_t eventPathaPackAccepted = 0;
    uint64_t eventPathaPackRetries = 0;
    uint64_t eventPathaPackCompleted = 0;
    uint64_t eventPathaPackBusyCycles = 0;
    uint64_t eventPathaSlotAlloc = 0;
    uint64_t eventPathaSlotFree = 0;
    uint64_t eventPathaSlotOverwriteErrors = 0;
    uint64_t eventPathaEarlyConsumeErrors = 0;
    uint64_t eventPathaResultTransfers = 0;
    uint64_t eventPathaResultBytes = 0;
    uint64_t eventPathaLoadPhaseCycles = 0;
    uint64_t eventPathaDotpPhaseCycles = 0;
    uint64_t eventPathaPackPhaseCycles = 0;
    uint64_t eventPathaResultPhaseCycles = 0;
    double eventPathaAverageMvmLatency = 0;
    uint64_t eventPathaMinMvmLatency = 0;
    uint64_t eventPathaMaxMvmLatency = 0;
    uint64_t eventTrsvRequests = 0;
    uint64_t eventTrsvAccepted = 0;
    uint64_t eventTrsvRetries = 0;
    uint64_t eventTrsvCompleted = 0;
    uint64_t eventTrsvWaitCycles = 0;
    uint64_t eventTrsvBusyCycles = 0;
    uint64_t eventTrsvQueueFullStalls = 0;
    uint64_t eventTrsvMaxQueueDepth = 0;
    uint64_t eventTrsvDivRequests = 0;
    uint64_t eventTrsvDivCompleted = 0;
    uint64_t eventTrsvDivBusyCycles = 0;
    uint64_t eventTrsvFmaRequests = 0;
    uint64_t eventTrsvFmaCompleted = 0;
    uint64_t eventTrsvFmaBusyCycles = 0;
    uint64_t eventTrsvForwardCycles = 0;
    uint64_t eventTrsvBackwardCycles = 0;
    uint64_t eventTrsvDependencyWaitCycles = 0;
    uint64_t eventTrsvForwardedResults = 0;
    double eventTrsvAverageLatency = 0;
    uint64_t eventTrsvMinLatency = 0;
    uint64_t eventTrsvMaxLatency = 0;
    double eventTrsvDividerUtilization = 0;
    double eventTrsvFmaUtilization = 0;
    uint64_t eventVec5Requests = 0;
    uint64_t eventVec5Accepted = 0;
    uint64_t eventVec5Retries = 0;
    uint64_t eventVec5Completed = 0;
    uint64_t eventVec5WaitCycles = 0;
    uint64_t eventVec5QueueFullStalls = 0;
    uint64_t eventVec5BusyCycles = 0;
    uint64_t eventVec5MaxQueueDepth = 0;
    uint64_t eventVec5LaneOperations = 0;
    double eventVec5AverageLatency = 0;
    double eventVec5LaneUtilization = 0;
    uint64_t eventVec5CopyRequests = 0;
    uint64_t eventVec5CopyAccepted = 0;
    uint64_t eventVec5CopyCompleted = 0;
    uint64_t eventVec5CopyRetries = 0;
    uint64_t eventVec5CopyWaitCycles = 0;
    uint64_t eventVec5SubRequests = 0;
    uint64_t eventVec5SubAccepted = 0;
    uint64_t eventVec5SubCompleted = 0;
    uint64_t eventVec5SubRetries = 0;
    uint64_t eventVec5SubWaitCycles = 0;
    uint64_t eventVec5AxpyRequests = 0;
    uint64_t eventVec5AxpyAccepted = 0;
    uint64_t eventVec5AxpyCompleted = 0;
    uint64_t eventVec5AxpyRetries = 0;
    uint64_t eventVec5AxpyWaitCycles = 0;
    uint64_t eventContextAlloc = 0;
    uint64_t eventContextFree = 0;
    uint64_t eventMaxActiveContexts = 0;
    uint64_t eventContextSwitches = 0;
    uint64_t eventContextReadyCycles = 0;
    uint64_t eventContextWaitCycles = 0;
    uint64_t eventSpmReads = 0;
    uint64_t eventSpmWrites = 0;
    uint64_t eventSpmReadBytes = 0;
    uint64_t eventSpmWriteBytes = 0;
    uint64_t eventSpmPortConflicts = 0;
    uint64_t eventSpmBankConflicts = 0;
    uint64_t eventSpmQueueFull = 0;
    uint64_t eventSpmRetries = 0;
    double eventSpmAverageLatency = 0;
    uint64_t eventTileLoads = 0;
    uint64_t eventTileLoadCycles = 0;
    uint64_t eventTileComputeCycles = 0;
    uint64_t eventPrefetchUseful = 0;
    uint64_t eventPrefetchLate = 0;
    uint64_t eventBufferEmptyStalls = 0;
    uint64_t eventBufferFullStalls = 0;
    double cyclesPerForwardCell = 0;
    double cyclesPerBackwardCell = 0;
    double cyclesPerFullCell = 0;
    double cellsPer1000Cycles = 0;
    double pathaUtilization = 0;
    double trsv5Utilization = 0;
    double vec5Utilization = 0;
};

uint64_t cfdLusgsLaunch(ExecContext *xc, Addr descriptor_addr);
uint64_t cfdLusgsWait(uint64_t token);
bool cfdLusgsBusy();

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_LUSGS_CONTROLLER_HH__
