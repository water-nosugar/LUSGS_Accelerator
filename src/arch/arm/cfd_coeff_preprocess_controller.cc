#include "arch/arm/cfd_coeff_preprocess_controller.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpu/exec_context.hh"
#include "arch/arm/cfd_local_spm.hh"
#include "cpu/thread_context.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/process.hh"

namespace gem5
{
namespace ArmISA
{
namespace
{

constexpr unsigned N = 5;
constexpr unsigned MatrixBytes = N * N * sizeof(double);
constexpr unsigned VectorBytes = N * sizeof(double);
constexpr uint64_t DescriptorVersion = 1;

uint64_t
envU64(const char *name, uint64_t fallback)
{
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(value, &end, 0);
    return end && *end == '\0' ? parsed : fallback;
}

bool
envBool(const char *name, bool fallback)
{
    const char *value = std::getenv(name);
    if (!value)
        return fallback;
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "on") == 0;
}

enum class RhsSchedule
{
    ColumnMajor,
    StepMajor,
    RoundRobinReady,
    ReadyFirst,
    FrontierAware,
};

enum class SpmLayout
{
    Legacy,
    MatrixSeparated,
    RowStriped,
};

SpmLayout
spmLayoutFromEnv()
{
    const char *value = std::getenv("GEM5_CFD_COEFF_SPM_LAYOUT");
    if (value && std::strcmp(value, "matrix-separated") == 0)
        return SpmLayout::MatrixSeparated;
    if (value && std::strcmp(value, "row-striped") == 0)
        return SpmLayout::RowStriped;
    return SpmLayout::Legacy;
}

RhsSchedule
rhsScheduleFromEnv()
{
    const char *value = std::getenv("GEM5_CFD_COEFF3_SCHEDULE");
    if (value && std::strcmp(value, "step-major") == 0)
        return RhsSchedule::StepMajor;
    if (value && std::strcmp(value, "round-robin-ready") == 0)
        return RhsSchedule::RoundRobinReady;
    if (value && std::strcmp(value, "ready-first") == 0)
        return RhsSchedule::ReadyFirst;
    if (value && std::strcmp(value, "frontier-aware") == 0)
        return RhsSchedule::FrontierAware;
    return RhsSchedule::RoundRobinReady;
}

struct EventEnv
{
    uint64_t ticksPerCycle = 500;
    uint32_t inputSlots = 2;
    uint32_t luPendingDepth = 2;
    uint32_t solvePendingDepth = 2;
    uint32_t luCount = 1;
    uint32_t solveCount = 1;
    uint32_t outputDepth = 4;
    uint32_t rhsLanes = 5;
    RhsSchedule rhsSchedule = RhsSchedule::ColumnMajor;
    uint32_t spmWritePorts = 1;
    uint32_t spmReadPorts = 1;
    uint32_t spmBanks = 4;
    uint32_t spmBankGranularity = 64;
    uint32_t spmWriteWidth = 40;
    uint32_t spmOutstanding = 4;
    uint64_t sourceReadLatency = 2;
    uint64_t spmWriteLatency = 1;
    uint32_t luDivCount = 1;
    uint32_t luMulSubCount = 1;
    uint64_t luDivLatency = 12;
    uint64_t luDivIi = 12;
    uint64_t luMulLatency = 3;
    uint64_t luSubLatency = 4;
    uint32_t coeffDivCount = 1;
    uint32_t coeffMulSubCount = 5;
    uint64_t coeffDivLatency = 12;
    uint64_t coeffDivIi = 4;
    uint64_t coeffMulLatency = 3;
    uint64_t coeffSubLatency = 4;
    uint32_t drainWidth = 40;
    uint32_t drainPorts = 1;
    uint64_t drainLatency = 1;
    uint32_t drainOutstanding = 4;
    uint32_t drainQueueDepth = 4;
    uint64_t columnFmaLatency = 4;
    uint64_t columnFmaIi = 1;
    uint32_t columnFmaCount = 1;
    uint32_t columnFmaQueueDepth = 5;
    uint64_t forwardCombineLatency = 1;
    uint64_t forwardCombineIi = 1;
    uint32_t forwardCombineCount = 1;
    uint32_t forwardCombineQueueDepth = 2;
    bool partialOutput = true;
    bool luForwarding = true;
    bool luSolveEarlyStart = false;
    bool packetSpm = false;
    bool dmaSpm = false;
    bool reciprocalSolve = false;
    bool lineBaseAhead = true;
    bool lineEarlyBackward = true;
    bool lineMvmSplit = false;
    bool resourceStats = false;
    uint64_t lineMulLatency = 3;
    uint64_t lineAddLatency = 4;
    uint32_t lineProductDepth = 5;
    SpmLayout spmLayout = SpmLayout::Legacy;
    bool traceEnable = false;
    uint32_t traceCells = 4;
    const char *traceFile = "m5out/coeff_preprocess_trace.csv";
};

EventEnv
readEnv()
{
    EventEnv e;
    e.resourceStats = envBool("GEM5_CFD_RESOURCE_STATS", false);
    e.lineBaseAhead = envBool("GEM5_CFD_LINE_BASE_AHEAD", true);
    e.lineEarlyBackward = envBool("GEM5_CFD_LINE_EARLY_BACKWARD", true);
    e.lineMvmSplit = envBool("GEM5_CFD_LINE_MVM_SPLIT", false);
    e.lineMulLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LINE_MUL_LAT", 3));
    e.lineAddLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LINE_ADD_LAT", 4));
    e.lineProductDepth = std::min<uint64_t>(N, std::max<uint64_t>(1,
        envU64("GEM5_CFD_LINE_PRODUCT_DEPTH", 5)));
    e.ticksPerCycle = std::max<uint64_t>(1,
        envU64("GEM5_CFD_TICKS_PER_CYCLE", 500));
    e.inputSlots = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_INPUT_SLOTS", 2));
    e.luPendingDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_PENDING_DEPTH", 2));
    e.solvePendingDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_PENDING_DEPTH", 2));
    e.luCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_COUNT", 1));
    e.solveCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_COUNT", 1));
    e.outputDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_OUTPUT_DEPTH", 4));
    e.rhsLanes = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_RHS_LANES", 15));
    e.rhsLanes = std::min<uint32_t>(15, e.rhsLanes);
    e.rhsSchedule = rhsScheduleFromEnv();
    e.spmWritePorts = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SPM_WRITE_PORTS", 1));
    e.spmReadPorts = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SPM_READ_PORTS", 1));
    e.spmBanks = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SPM_BANKS", 4));
    e.spmBankGranularity = std::max<uint64_t>(8,
        envU64("GEM5_CFD_COEFF_SPM_BANK_GRANULARITY", 64));
    e.spmWriteWidth = std::max<uint64_t>(8,
        envU64("GEM5_CFD_COEFF_SPM_WRITE_WIDTH", 40));
    e.spmOutstanding = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SPM_OUTSTANDING", 4));
    e.sourceReadLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SOURCE_READ_LAT", 2));
    e.spmWriteLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_SPM_WRITE_LAT", 1));
    e.luDivCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_DIV_COUNT", 1));
    e.luMulSubCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_MULSUB_COUNT", 1));
    e.luDivLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_DIV_LAT", 12));
    e.luDivIi = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_DIV_II", e.luDivLatency));
    e.luMulLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_MUL_LAT", 3));
    e.luSubLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_LU5_SUB_LAT", 4));
    e.coeffDivCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_DIV_COUNT", 1));
    e.coeffMulSubCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_MULSUB_COUNT", 1));
    e.coeffDivLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_DIV_LAT", 12));
    e.coeffDivIi = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_DIV_II", 4));
    e.coeffMulLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_MUL_LAT", 3));
    e.coeffSubLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF3_SUB_LAT", 4));
    e.drainWidth = std::max<uint64_t>(8,
        envU64("GEM5_CFD_COEFF_DRAIN_WIDTH", 40));
    e.drainPorts = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_DRAIN_PORTS", 1));
    e.drainLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_DRAIN_LAT", 1));
    e.drainOutstanding = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_DRAIN_OUTSTANDING", 4));
    e.drainQueueDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_DRAIN_QUEUE_DEPTH", 4));
    e.columnFmaLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_COLUMN_FMA_LAT", 4));
    e.columnFmaIi = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_COLUMN_FMA_II", 1));
    e.columnFmaCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_COLUMN_FMA_COUNT", 1));
    e.columnFmaQueueDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_COLUMN_FMA_QUEUE_DEPTH", 5));
    e.forwardCombineLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_FORWARD_COMBINE_LAT", 1));
    e.forwardCombineIi = std::max<uint64_t>(1,
        envU64("GEM5_CFD_FORWARD_COMBINE_II", 1));
    e.forwardCombineCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_FORWARD_COMBINE_COUNT", 1));
    e.forwardCombineQueueDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_FORWARD_COMBINE_QUEUE_DEPTH", 2));
    e.partialOutput = envBool("GEM5_CFD_COEFF3_PARTIAL_OUTPUT", true);
    e.luForwarding = envBool("GEM5_CFD_LU_FORWARDING", true);
    e.luSolveEarlyStart = envBool("GEM5_CFD_LU_SOLVE_EARLY_START", true);
    const char *spmModel = std::getenv("GEM5_CFD_COEFF_SPM_MODEL");
    e.packetSpm = spmModel && std::strcmp(spmModel, "packet") == 0;
    e.dmaSpm = spmModel && std::strcmp(spmModel, "dma") == 0;
    const char *divMode = std::getenv("GEM5_CFD_COEFF3_DIV_MODE");
    e.reciprocalSolve =
        divMode && std::strcmp(divMode, "reciprocal") == 0;
    e.spmLayout = spmLayoutFromEnv();
    e.traceEnable = envBool("GEM5_CFD_COEFF_TRACE_ENABLE", false);
    e.traceCells = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_TRACE_CELLS", 4));
    e.traceFile = std::getenv("GEM5_CFD_COEFF_TRACE_FILE");
    if (!e.traceFile || !*e.traceFile)
        e.traceFile = "m5out/coeff_preprocess_trace.csv";
    return e;
}

enum class RequestStage
{
    WaitingSlot,
    InputFetching,
    InputReady,
    LuQueued,
    LuActive,
    LuReady,
    SolveQueued,
    SolveActive,
    DrainPending,
    Complete,
    Failed,
};

enum class LuPhase
{
    StartLower,
    LowerMul,
    LowerSub,
    StoreLower,
    PivotCheck,
    StartUpper,
    UpperMul,
    UpperSub,
    DivideUpper,
    StoreUpper,
    NextColumn,
    Done,
};

enum class RhsPhase
{
    ForwardDiv,
    ForwardMul,
    ForwardSub,
    LastDiv,
    BackwardMul,
    BackwardSub,
    Done,
};

enum class DrainType : uint8_t {
    Lu = 0, DInv = 1, LBar = 2, UBar = 3, BaseVector = 4,
    CorrectionVector = 5, DqStarVector = 6
};
enum class OpKind : uint8_t {
    LuDiv, LuMul, LuSub, Div, Mul, Sub, ColumnFma, ForwardCombine,
    LineProduct, LineAccumulate
};

enum class ColumnFmaTaskKind : uint8_t
{
    DInvBase,
    LbarCorrection,
};

struct RhsState
{
    bool active = false;
    bool waiting = false;
    bool done = false;
    RhsPhase phase = RhsPhase::ForwardDiv;
    int k = 0;
    int i = 1;
    double product = 0.0;
    uint64_t readySinceCycle = 0;
    std::array<double, N> value = {};
};

struct Request
{
    uint64_t token = 0;
    CfdCoeffPreprocessDescriptor desc = {};
    CfdCoeffPreprocessRecord record = {};
    CfdCoeffPreprocessStatus status = CfdCoeffPreprocessStatus::Busy;
    RequestStage stage = RequestStage::WaitingSlot;
    int slot = -1;
    bool slotOwned = false;
    unsigned inputChunk = 0;
    unsigned inputChunksIssued = 0;
    unsigned inputChunksCompleted = 0;
    unsigned inputChunks = 0;
    unsigned inputBytesIssued = 0;
    uint64_t inputReadyCycle = 0;
    bool inputCaptured = false;
    bool luWaiting = false;
    bool luDrained = false;
    bool luDone = false;
    bool solveStarted = false;
    bool solveDone = false;
    unsigned rhsCount = 0;
    unsigned rhsSlots = 0;
    unsigned rhsCompleted = 0;
    unsigned schedulerCursor = 0;
    uint8_t requestedBatchMask = 0;
    uint8_t matrixDrainMask = 0;
    uint16_t rhsEnabledMask = 0;
    uint16_t rhsActivatedMask = 0;
    uint16_t rhsDoneMask = 0;
    std::array<CfdSpmMatrix, 3> inputMatrices = {};
    unsigned inputMatrixCount = 0;
    std::array<unsigned, 3> batchCompleted = {};
    std::array<bool, N> forwardStepReady = {};
    std::array<bool, N> backwardStepReady = {};
    std::array<bool, N> luStepUsed = {};
    uint64_t luReloadReadyCycle = 0;
    std::array<bool, 7> drainQueued = {};
    std::array<bool, 7> drainDone = {};
    std::array<double, N * N> d = {};
    std::array<double, N * N> lower = {};
    std::array<double, N * N> upper = {};
    std::array<double, N * N> lu = {};
    std::array<double, N * N> dInv = {};
    std::array<double, N * N> lBar = {};
    std::array<double, N * N> uBar = {};
    bool baseConsumerEnabled = false;
    bool directBaseBypass = false;
    bool rhsVectorReady = false;
    bool baseAccumulatorInitialized = false;
    bool baseComputeComplete = false;
    bool basePublishComplete = false;
    uint8_t dInvColumnReadyMask = 0;
    uint8_t dInvColumnQueuedMask = 0;
    uint8_t dInvColumnIssuedMask = 0;
    uint8_t dInvColumnConsumedMask = 0;
    uint8_t dInvColumnOutOfOrderCountedMask = 0;
    uint8_t nextDInvColumnToConsume = 0;
    std::array<double, N> rhsVector = {};
    std::array<double, N> baseAccumulator = {};
    bool lbarConsumerEnabled = false;
    bool directLbarBypass = false;
    bool previousDqReady = false;
    bool correctionInitialized = false;
    bool correctionComplete = false;
    bool combineQueued = false;
    bool combineIssued = false;
    bool combineComplete = false;
    bool dqStarReadyLocal = false;
    bool dqStarPublishComplete = false;
    bool handoffProduced = false;
    uint8_t lbarColumnReadyMask = 0;
    uint8_t lbarColumnQueuedMask = 0;
    uint8_t lbarColumnIssuedMask = 0;
    uint8_t lbarColumnConsumedMask = 0;
    uint8_t lbarColumnOutOfOrderCountedMask = 0;
    uint8_t nextLbarColumnToConsume = 0;
    uint64_t firstLbarConsumerIssueCycle = 0;
    uint64_t previousDqReadyCycle = 0;
    std::array<double, N> previousDq = {};
    std::array<double, N> lbarCorrectionAccumulator = {};
    std::array<double, N> dqStar = {};
    LuPhase luPhase = LuPhase::StartLower;
    int luCol = 0;
    int luRow = 0;
    int luUpperCol = 1;
    int luK = 0;
    double luTemp = 0.0;
    double luProduct = 0.0;
    std::array<RhsState, 15> rhs = {};
    std::array<double, N> reciprocal = {};
    std::array<bool, N> reciprocalReady = {};
    std::array<bool, N> reciprocalPending = {};
};

// Each context is bounded to at most five unconsumed column products.  Three
// contexts per line: cached forward, backward, and background base.  These
// contexts share ONE five-lane multiplier and ONE five-lane adder globally.
struct LineMvmState
{
    uint64_t epoch = 0;
    unsigned issued = 0;
    unsigned added = 0;
    bool addPending = false;
    std::array<bool, N> ready = {};
    std::array<std::array<double, N>, N> products = {};
    std::array<double, N> sum = {};
};

struct LineRequest
{
    uint64_t token = 0;
    CfdCoeffPreprocessDescriptor desc = {};
    CfdCoeffPreprocessStatus status = CfdCoeffPreprocessStatus::Busy;
    uint64_t cells = 0;
    uint64_t window = 0;
    uint64_t nextCell = 0;
    uint64_t liveChildren = 0;
    uint64_t completedChildren = 0;
    uint64_t coefficientVersion = 0;
    bool coefficientCacheEnabled = false;
    bool coefficientDirty = false;
    bool coefficientCacheHit = false;
    bool cachedForwardStarted = false;
    bool cachedForwardOpPending = false;
    uint64_t cachedForwardCell = 0;
    uint8_t cachedForwardColumn = 0;
    uint8_t cachedForwardPhase = 0;
    uint64_t baseCell = 0;
    uint8_t baseColumn = 0;
    bool basePending = false;
    // Bounded by the descriptor's window; never reused across sweeps.
    std::vector<std::array<double, N>> baseRing;
    std::vector<bool> baseReady;
    std::vector<bool> dqStarReady;
    std::vector<uint8_t> uBarReady;
    uint64_t baseAheadColumns = 0;
    uint64_t backwardStartCycle = 0;
    uint64_t backwardDoneCycle = 0;
    uint64_t childrenRetiredCycle = 0;
    std::array<LineMvmState, 3> mvm;
    uint64_t lineProducts = 0;
    uint64_t lineAdds = 0;
    uint64_t lineBufferedMax = 0;
    uint64_t lineBufferStalls = 0;
    bool backwardDirect = false;
    bool backwardStarted = false;
    bool backwardOpPending = false;
    int64_t backwardCell = -1;
    uint8_t backwardColumn = 0;
    std::vector<std::array<double, N>> dqStar;
    std::vector<std::array<double, N>> dq;
    std::vector<std::array<double, N>> rhs;
    std::vector<std::array<double, N * N>> dInv;
    std::vector<std::array<double, N * N>> lBar;
    std::vector<std::array<double, N * N>> uBar;
    std::array<double, N> cachedBaseAccumulator = {};
    std::array<double, N> cachedCorrectionAccumulator = {};
    std::array<double, N> backwardAccumulator = {};
    bool terminal = false;
};

struct LineCoeffCache
{
    uint64_t version = 0;
    uint64_t cells = 0;
    std::vector<std::array<double, N * N>> dInv;
    std::vector<std::array<double, N * N>> lBar;
    std::vector<std::array<double, N * N>> uBar;
};

struct RhsMaskPlan
{
    uint16_t rhsMask;
    uint8_t rhsCount;
};

// Batch bits are DInv/Lbar/Ubar.  RHS bits are fixed by the public Stage B2
// mapping: 0..4 DInv, 5..9 Lbar, 10..14 Ubar.
static constexpr std::array<RhsMaskPlan, 8> RhsMaskPlans = {{
    {0x0000, 0}, {0x001f, 5}, {0x03e0, 5}, {0x03ff, 10},
    {0x7c00, 5}, {0x7c1f, 10}, {0x7fe0, 10}, {0x7fff, 15},
}};

static unsigned
rhsBatch(const Request &req, unsigned rhs)
{
    return (req.desc.flags & CFD_COEFF_PRE_COEFF3) ? rhs / N : 2;
}

static unsigned
rhsColumn(const Request &req, unsigned rhs)
{
    return (req.desc.flags & CFD_COEFF_PRE_COEFF3) ? rhs % N : rhs;
}

static bool
batchRequested(const Request &req, unsigned batch)
{
    return req.requestedBatchMask & (1u << batch);
}

struct PendingOp
{
    uint64_t completeCycle = 0;
    OpKind kind = OpKind::Mul;
    std::function<void()> complete;
};

struct ResourcePool
{
    uint64_t latency = 1;
    uint64_t ii = 1;
    std::vector<uint64_t> nextIssue;
    // Independent of descriptor/legacy ABI.  Sample once per controller tick,
    // never once per waiting request or per RHS.
    uint64_t samples = 0;
    uint64_t eligibleSlots = 0;
    uint64_t issues = 0;
    uint64_t issueCycles = 0;
    uint64_t blockedCycles = 0;
    uint64_t noAttemptCycles = 0;
    uint64_t activeCycles = 0;
    uint64_t occupancySum = 0;
    uint64_t occupancyMax = 0;
    uint64_t issuedNow = 0;
    bool attemptedNow = false;
    uint64_t attempts = 0;
    uint64_t rejected = 0;
    uint64_t rejectedAfterIssue = 0;
};

struct DrainTask
{
    uint64_t token = 0;
    DrainType type = DrainType::Lu;
    unsigned bytesDone = 0;
    unsigned bytesIssued = 0;
    uint64_t finishCycle = 0;
    bool started = false;
};

struct ColumnFmaTask
{
    ColumnFmaTaskKind kind = ColumnFmaTaskKind::DInvBase;
    uint64_t token = 0;
    uint64_t generation = 0;
    uint64_t lineId = 0;
    uint64_t cellId = 0;
    uint8_t column = 0;
    std::array<double, N> matrixColumn = {};
    double scalar = 0.0;
    uint64_t issueCycle = 0;
    uint64_t completeCycle = 0;
};

struct ForwardCombineTask
{
    uint64_t token = 0;
    uint64_t generation = 0;
    uint64_t lineId = 0;
    uint64_t cellId = 0;
};

struct ForwardDqHandoff
{
    bool valid = false;
    bool consumerAttached = false;
    bool consumed = false;
    uint64_t lineId = 0;
    uint64_t producerCell = 0;
    uint64_t producerGeneration = 0;
    uint64_t expectedConsumerCell = 0;
    std::array<double, N> value = {};
    uint64_t readyCycle = 0;
};

class CfdCoeffPreprocessController
{
  public:
    CfdCoeffPreprocessController();
    uint64_t launch(ExecContext *xc, Addr descriptorAddr);
    uint64_t wait(uint64_t token);
    uint64_t cancel(uint64_t token);

  private:
    EventFunctionWrapper tickEvent;
    EventEnv env;
    // Event counts (request/RHS visits), not mutually exclusive wall cycles.
    std::unordered_map<std::string, uint64_t> diagnostics;
    void observe(const char *reason) {
        if (env.resourceStats)
            diagnostics[reason]++;
    }
    ThreadContext *tc = nullptr;
    uint64_t nextToken = 1;
    uint64_t nextRequestId = 1;
    bool configured = false;
    FILE *traceFp = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<Request>> requests;
    std::unordered_map<uint64_t, std::unique_ptr<LineRequest>> lineRequests;
    std::unordered_map<uint64_t, uint64_t> childLineTokens;
    std::unordered_map<uint64_t, LineCoeffCache> lineCoeffCaches;
    std::vector<bool> slots;
    std::vector<PendingOp> pendingOps;
    std::deque<DrainTask> drainQueue;
    std::vector<DrainTask> activeDrains;
    std::deque<ColumnFmaTask> columnFmaQueue;
    std::deque<ForwardCombineTask> forwardCombineQueue;
    ResourcePool luDiv;
    ResourcePool luMul;
    ResourcePool luSub;
    ResourcePool coeffDiv;
    ResourcePool coeffMul;
    ResourcePool coeffSub;
    ResourcePool columnFma;
    ResourcePool lineProduct;
    ResourcePool lineAccumulate;
    ResourcePool forwardCombine;
    uint64_t columnFmaCursor = 0;
    std::unordered_map<uint64_t, ForwardDqHandoff> forwardHandoffs;
    std::unordered_map<uint64_t, uint64_t> liveGeneration;
    uint32_t spmReadPortsAvail = 0;
    uint32_t spmWritePortsAvail = 0;
    uint32_t drainPortsAvail = 0;
    std::vector<bool> spmBanksUsed;

    EventQueue *eventq() const;
    uint64_t cycle() const;
    void configure();
    void scheduleTick();
    void processTick();
    std::array<ResourcePool *, 10> resourcePools();
    void beginResourceStats(uint64_t now);
    void endResourceStats(bool quiescent);
    uint64_t launchDescriptor(const CfdCoeffPreprocessDescriptor &desc);
    uint64_t launchLine(const CfdCoeffPreprocessDescriptor &desc);
    void pumpLineRequests();
    void retireLineChild(uint64_t childToken, const Request &req);
    void processLineCachedForward(uint64_t now);
    void processLineBaseAhead(uint64_t now);
    void finishCachedForwardCell(LineRequest &line);
    void publishLineValues();
    void finishLineBackward(LineRequest &line);
    void finishLineBackwardCell(LineRequest &line);
    void advanceLineMvm(LineRequest &line, unsigned context,
        const std::array<double, N * N> &matrix,
        const std::array<double, N> &vector, uint8_t readyMask, uint64_t now,
        std::function<void(LineRequest &, const std::array<double, N> &)> done);
    void completeLineCachedForwardColumn(
        uint64_t lineToken, uint8_t phase, uint8_t column, uint64_t now);
    void processLineBackward(uint64_t now);
    void completeLineBackwardColumn(uint64_t lineToken, uint8_t column,
                                    uint64_t now);
    void completeOps(uint64_t now);
    void assignInputSlots(uint64_t now);
    void processInputs(uint64_t now);
    void startLuRequests(uint64_t now);
    void processLu(uint64_t now);
    void startSolveRequests(uint64_t now);
    void processSolve(uint64_t now);
    void processColumnFma(uint64_t now);
    void completeColumnFma(uint64_t token, uint64_t generation,
                           ColumnFmaTaskKind kind, unsigned column,
                           const std::array<double, N> &matrixColumn,
                           double scalar, uint64_t now);
    void processForwardHandoffs(uint64_t now);
    void produceForwardHandoff(Request &req, uint64_t now);
    void processForwardCombines(uint64_t now);
    void completeForwardCombine(uint64_t token, uint64_t generation,
                                uint64_t now);
    void processDrains(uint64_t now);
    void finishRequests(uint64_t now);
    void retireAutoRequests();
    void accountActivity();
    bool issue(ResourcePool &pool, OpKind kind, uint64_t now,
               std::function<void()> complete);
    bool enqueueDrain(Request &req, DrainType type, uint64_t now);
    void finishDrain(DrainTask &task, uint64_t now);
    void completeInputPacket(uint64_t token, unsigned bytes,
                             uint64_t packetId, Addr addr, unsigned bank,
                             unsigned port, CfdSpmMatrix matrix);
    void completeDrainPacket(uint64_t token, DrainType type, unsigned bytes,
                             uint64_t packetId, Addr addr, unsigned bank,
                             unsigned port, CfdSpmMatrix matrix);
    CfdSpmMatrix inputMatrix(const Request &req, unsigned offset) const;
    CfdSpmMatrix drainMatrix(DrainType type) const;
    unsigned inputRegionOffset(const Request &req, unsigned offset) const;
    unsigned inputRegionBytes(const Request &req, unsigned offset) const;
    unsigned drainBytes(DrainType type) const;
    Addr packetAddress(const Request &req, CfdSpmMatrix matrix,
                       unsigned matrixOffset, unsigned legacyOffset,
                       bool input) const;
    void initializeRhs(Request &req);
    void activateRhs(Request &req);
    void rhsDone(Request &req, unsigned rhs, uint64_t now);
    void markFailed(Request &req, CfdCoeffPreprocessStatus status,
                    const char *note);
    void publishProgress(Request &req);
    SETranslatingPortProxy proxy() const;
    Request *find(uint64_t token);
    void trace(const Request &req, const char *engine, const char *stage,
               int batch = -1, int column = -1, int k = -1, int row = -1,
               const char *note = "", const char *requester = "",
               uint64_t packetId = 0, Addr address = 0, unsigned size = 0,
               int bank = -1, int port = -1);
};

CfdCoeffPreprocessController::CfdCoeffPreprocessController()
    : tickEvent([this] { processTick(); }, "cfd_coeff_preprocess.tick")
{
}

EventQueue *
CfdCoeffPreprocessController::eventq() const
{
    EventQueue *q = curEventQueue();
    return q ? q : getEventQueue(0);
}

uint64_t
CfdCoeffPreprocessController::cycle() const
{
    return env.ticksPerCycle ? curTick() / env.ticksPerCycle : curTick();
}

SETranslatingPortProxy
CfdCoeffPreprocessController::proxy() const
{
    return SETranslatingPortProxy(tc, SETranslatingPortProxy::Never);
}

Request *
CfdCoeffPreprocessController::find(uint64_t token)
{
    auto it = requests.find(token);
    return it == requests.end() ? nullptr : it->second.get();
}

CfdSpmMatrix
CfdCoeffPreprocessController::inputMatrix(
    const Request &req, unsigned offset) const
{
    if (req.baseConsumerEnabled &&
        offset >= req.inputMatrixCount * MatrixBytes)
        return CfdSpmMatrix::RhsVector;
    const unsigned matrix = std::min<unsigned>(offset / MatrixBytes,
        req.inputMatrixCount - 1);
    if (matrix == 0 &&
        (req.desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT))
        return CfdSpmMatrix::Lu;
    return req.inputMatrices[matrix];
}

unsigned
CfdCoeffPreprocessController::inputRegionOffset(
    const Request &req, unsigned offset) const
{
    const unsigned matrixBytes = req.inputMatrixCount * MatrixBytes;
    return offset >= matrixBytes ? offset - matrixBytes : offset % MatrixBytes;
}

unsigned
CfdCoeffPreprocessController::inputRegionBytes(
    const Request &req, unsigned offset) const
{
    return inputMatrix(req, offset) == CfdSpmMatrix::RhsVector ?
        VectorBytes : MatrixBytes;
}

unsigned
CfdCoeffPreprocessController::drainBytes(DrainType type) const
{
    return type == DrainType::BaseVector ||
           type == DrainType::CorrectionVector ||
           type == DrainType::DqStarVector ? VectorBytes : MatrixBytes;
}

CfdSpmMatrix
CfdCoeffPreprocessController::drainMatrix(DrainType type) const
{
    switch (type) {
      case DrainType::Lu: return CfdSpmMatrix::Lu;
      case DrainType::DInv: return CfdSpmMatrix::DInv;
      case DrainType::LBar: return CfdSpmMatrix::LBar;
      case DrainType::UBar: return CfdSpmMatrix::UBar;
      case DrainType::BaseVector: return CfdSpmMatrix::BaseVector;
      case DrainType::CorrectionVector:
        return CfdSpmMatrix::CorrectionVector;
      case DrainType::DqStarVector: return CfdSpmMatrix::DqStarVector;
    }
    return CfdSpmMatrix::Unknown;
}

Addr
CfdCoeffPreprocessController::packetAddress(
    const Request &req, CfdSpmMatrix matrix, unsigned matrixOffset,
    unsigned legacyOffset, bool input) const
{
    (void)input;
    if (env.spmLayout == SpmLayout::Legacy) {
        const Addr base = req.slot * 0x800ull;
        return base + legacyOffset;
    }

    const uint64_t banks = std::max<uint32_t>(1, env.spmBanks);
    const uint64_t granularity =
        std::max<uint32_t>(8, env.spmBankGranularity);
    const uint64_t bankSpan = banks * granularity;
    const uint64_t matrixId = static_cast<unsigned>(matrix);
    const Addr slotBase = req.slot * 0x10000ull;
    if (env.spmLayout == SpmLayout::MatrixSeparated) {
        const uint64_t matrixSpan =
            ((MatrixBytes + bankSpan - 1) / bankSpan + 1) * bankSpan;
        return slotBase + matrixId * matrixSpan +
            (matrixId % banks) * granularity + matrixOffset;
    }

    const unsigned row = matrixOffset / (N * sizeof(double));
    const unsigned rowOffset = matrixOffset % (N * sizeof(double));
    const uint64_t matrixSpan = (N + 1) * bankSpan;
    return slotBase + matrixId * matrixSpan + row * bankSpan +
        ((row + matrixId) % banks) * granularity + rowOffset;
}

void
CfdCoeffPreprocessController::configure()
{
    if (configured)
        return;
    env = readEnv();
    slots.assign(env.inputSlots, false);
    spmBanksUsed.assign(env.spmBanks, false);
    auto init = [](ResourcePool &pool, uint32_t count, uint64_t latency,
                   uint64_t ii) {
        pool.latency = latency;
        pool.ii = ii;
        pool.nextIssue.assign(count, 0);
    };
    init(luDiv, env.luDivCount, env.luDivLatency, env.luDivIi);
    init(luMul, env.luMulSubCount, env.luMulLatency, 1);
    init(luSub, env.luMulSubCount, env.luSubLatency, 1);
    init(coeffDiv, env.coeffDivCount, env.coeffDivLatency, env.coeffDivIi);
    init(coeffMul, env.coeffMulSubCount, env.coeffMulLatency, 1);
    init(coeffSub, env.coeffMulSubCount, env.coeffSubLatency, 1);
    init(columnFma, env.columnFmaCount, env.columnFmaLatency,
         env.columnFmaIi);
    // Explicit experimental line engine, not a free reduction of ColumnFma
    // opLat.  Legacy request-local consumers retain their original FU.
    init(lineProduct, 1, env.lineMulLatency, 1);
    init(lineAccumulate, 1, env.lineAddLatency, 1);
    init(forwardCombine, env.forwardCombineCount, env.forwardCombineLatency,
         env.forwardCombineIi);
    configured = true;
}

void
CfdCoeffPreprocessController::scheduleTick()
{
    if (tickEvent.scheduled())
        return;
    eventq()->schedule(&tickEvent, curTick() + env.ticksPerCycle);
}

bool
CfdCoeffPreprocessController::issue(
    ResourcePool &pool, OpKind kind, uint64_t now,
    std::function<void()> complete)
{
    if (env.resourceStats) {
        pool.attemptedNow = true;
        pool.attempts++;
    }
    for (auto &next : pool.nextIssue) {
        if (next <= now) {
            if (env.resourceStats)
                pool.issuedNow++;
            next = now + pool.ii;
            pendingOps.push_back({now + pool.latency, kind,
                                  std::move(complete)});
            return true;
        }
    }
    if (env.resourceStats) {
        pool.rejected++;
        pool.rejectedAfterIssue += pool.issuedNow != 0;
    }
    return false;
}

void
CfdCoeffPreprocessController::trace(
    const Request &req, const char *engine, const char *stage, int batch,
    int column, int k, int row, const char *note, const char *requester,
    uint64_t packetId, Addr address, unsigned size, int bank, int port)
{
    if (!env.traceEnable || req.desc.cellId >= env.traceCells)
        return;
    if (!traceFp) {
        traceFp = std::fopen(env.traceFile, "w");
        if (!traceFp)
            return;
        std::fprintf(traceFp,
            "cycle,requestId,generation,line,cell,engine,stage,rhsBatch,"
            "rhsColumn,k,row,slot,queueOccupancy,readyToken,completionToken,"
            "note,requester,packetId,address,size,bank,port\n");
    }
    std::fprintf(traceFp,
        "%llu,%llu,%llu,%llu,%llu,%s,%s,%d,%d,%d,%d,%d,%zu,%llu,%llu,"
        "%s,%s,%llu,0x%llx,%u,%d,%d\n",
        (unsigned long long)cycle(),
        (unsigned long long)req.record.requestId,
        (unsigned long long)req.desc.generation,
        (unsigned long long)req.desc.lineId,
        (unsigned long long)req.desc.cellId, engine, stage, batch, column,
        k, row, req.slot, drainQueue.size() + activeDrains.size(),
        (unsigned long long)req.token,
        (unsigned long long)(req.stage == RequestStage::Complete ?
                             req.token : 0), note, requester,
        (unsigned long long)packetId, (unsigned long long)address, size,
        bank, port);
    std::fflush(traceFp);
}

uint64_t
CfdCoeffPreprocessController::launch(ExecContext *xc, Addr descriptorAddr)
{
    configure();
    if (!descriptorAddr || (descriptorAddr & 7))
        return 0;

    CfdCoeffPreprocessDescriptor desc = {};
    tc = xc->tcBase();
    proxy().readBlob(descriptorAddr, &desc, sizeof(desc));
    if (desc.flags & CFD_COEFF_PRE_LINE_AUTONOMOUS)
        return launchLine(desc);
    return launchDescriptor(desc);
}

uint64_t
CfdCoeffPreprocessController::launchDescriptor(
    const CfdCoeffPreprocessDescriptor &desc)
{
    if (requests.size() >= env.inputSlots + env.luPendingDepth +
                           env.solvePendingDepth + env.outputDepth)
        return 0;
    const bool coeff3 = desc.flags & CFD_COEFF_PRE_COEFF3;
    const bool autoRetire =
        desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE;
    const bool baseConsumer =
        desc.flags & CFD_COEFF_PRE_DINV_BASE_CONSUMER;
    const bool directBase =
        desc.flags & CFD_COEFF_PRE_DINV_DIRECT_BYPASS;
    const bool lbarConsumer =
        desc.flags & CFD_COEFF_PRE_LBAR_FORWARD_CONSUMER;
    const bool directLbar =
        desc.flags & CFD_COEFF_PRE_LBAR_DIRECT_BYPASS;
    const uint64_t handoffFaultBits =
        CFD_COEFF_PRE_TEST_HANDOFF_WRONG_CELL |
        CFD_COEFF_PRE_TEST_HANDOFF_WRONG_GENERATION;
    const uint64_t handoffFaults = desc.flags & handoffFaultBits;
    const bool holdDqStarPublish =
        desc.flags & CFD_COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH;
    const uint64_t maskBits = CFD_COEFF_PRE_NEED_DINV |
        CFD_COEFF_PRE_NEED_LBAR | CFD_COEFF_PRE_NEED_UBAR;
    const bool explicitMask = coeff3 && (desc.flags & maskBits);
    const bool needDInv = coeff3 &&
        (!explicitMask || (desc.flags & CFD_COEFF_PRE_NEED_DINV));
    const bool needLBar = coeff3 &&
        (!explicitMask || (desc.flags & CFD_COEFF_PRE_NEED_LBAR));
    const bool needUBar = !coeff3 || !explicitMask ||
        (desc.flags & CFD_COEFF_PRE_NEED_UBAR);
    if (desc.version != DescriptorVersion || desc.size != sizeof(desc) ||
        (!desc.flags || !(desc.flags & (CFD_COEFF_PRE_TRSV |
                                       CFD_COEFF_PRE_COEFF3))) ||
        !desc.dAddr || !desc.luOutAddr || !desc.recordAddr ||
        (autoRetire && (!coeff3 ||
                       !(desc.flags & CFD_COEFF_PRE_STREAMING_PROGRESS))) ||
        (directBase && !baseConsumer) ||
        (handoffFaults && (!lbarConsumer ||
                           handoffFaults == handoffFaultBits)) ||
        (holdDqStarPublish && !lbarConsumer) ||
        (lbarConsumer && (!baseConsumer || !directBase || !autoRetire ||
                          !desc.reserved[3])) ||
        (directLbar && !lbarConsumer) ||
        (baseConsumer && (!coeff3 || !autoRetire || !needDInv ||
                          !desc.reserved[0] ||
                          (!directLbar && !desc.reserved[1]))) ||
        (needUBar && (!desc.uAddr || !desc.uBarOutAddr)))
        return 0;
    if (coeff3 && ((needDInv && !directBase && !desc.dInvOutAddr) ||
                   (needLBar && (!desc.lAddr ||
                    (!directLbar && !desc.lBarOutAddr))) ||
                   (lbarConsumer && desc.cellId && !needLBar) ||
                   (lbarConsumer && !directLbar && desc.cellId &&
                    !desc.reserved[2])))
        return 0;
    const uint64_t addrs[] = {desc.dAddr, desc.lAddr, desc.uAddr,
        desc.luOutAddr, desc.dInvOutAddr, desc.lBarOutAddr,
        desc.uBarOutAddr, desc.recordAddr, desc.reserved[0],
        desc.reserved[1], desc.reserved[2], desc.reserved[3],
        desc.reserved[4]};
    for (uint64_t addr : addrs) {
        if (addr && (addr & 7))
            return 0;
    }

    auto req = std::make_unique<Request>();
    req->token = nextToken++;
    req->desc = desc;
    req->requestedBatchMask = (needDInv ? 1u : 0u) |
        (needLBar ? 2u : 0u) | (needUBar ? 4u : 0u);
    req->matrixDrainMask = req->requestedBatchMask;
    req->baseConsumerEnabled = baseConsumer;
    req->directBaseBypass = directBase;
    req->lbarConsumerEnabled = lbarConsumer;
    req->directLbarBypass = directLbar;
    if (directBase)
        req->matrixDrainMask &= ~1u;
    if (directLbar)
        req->matrixDrainMask &= ~2u;
    if (coeff3) {
        req->rhsSlots = 15;
        const RhsMaskPlan &plan = RhsMaskPlans[req->requestedBatchMask];
        req->rhsEnabledMask = plan.rhsMask;
        req->rhsCount = plan.rhsCount;
        req->record.maskTableLookups++;
    } else {
        req->rhsSlots = 5;
        req->rhsCount = 5;
        req->rhsEnabledMask = 0x001f;
    }
    req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::D;
    if (needUBar)
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::U;
    if (needLBar)
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::L;
    const unsigned inputBytes = req->inputMatrixCount * MatrixBytes +
        (baseConsumer ? VectorBytes : 0);
    req->inputChunks =
        (inputBytes + env.spmWriteWidth - 1) / env.spmWriteWidth;
    req->record.requestId = nextRequestId++;
    req->record.generation = desc.generation;
    req->record.lineId = desc.lineId;
    req->record.cellId = desc.cellId;
    req->record.issueCycle = cycle();
    req->record.status = static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
    req->record.inputBytes = inputBytes;
    if (baseConsumer) {
        req->baseAccumulator.fill(0.0);
        req->baseAccumulatorInitialized = true;
        req->record.baseAccumulatorsAllocated = 1;
        if (directBase) {
            req->record.dInvMatrixDrainsAvoided = 1;
            req->record.dInvMatrixDrainBytesAvoided = MatrixBytes;
        }
    }
    if (lbarConsumer && desc.cellId) {
        req->lbarCorrectionAccumulator.fill(0.0);
        req->correctionInitialized = true;
        req->record.correctionAccumulatorsAllocated = 1;
        if (directLbar) {
            req->record.baseStandalonePublishesAvoided = 1;
            req->record.baseStandalonePublishBytesAvoided = VectorBytes;
            req->record.lbarMatrixDrainsAvoided = 1;
            req->record.lbarMatrixDrainBytesAvoided = MatrixBytes;
            req->record.lbarGuestReloadBytesAvoided = MatrixBytes;
            req->record.pathaLbarMvmEliminated = 1;
            req->record.pathaLbarCustomInstructionsEliminated = 12;
        }
    } else if (lbarConsumer && !desc.cellId && directLbar) {
        req->record.baseStandalonePublishesAvoided = 1;
        req->record.baseStandalonePublishBytesAvoided = VectorBytes;
    }
    const uint64_t token = req->token;
    const uint64_t generationKey =
        (desc.lineId * 0x9e3779b97f4a7c15ull) ^ desc.cellId;
    auto generation = liveGeneration.find(generationKey);
    if (desc.generation == 0 ||
        (generation != liveGeneration.end() &&
         desc.generation <= generation->second)) {
        req->stage = RequestStage::Failed;
        req->status = CfdCoeffPreprocessStatus::GenerationMismatch;
        req->record.status = static_cast<uint64_t>(req->status);
        req->record.completeCycle = cycle();
        proxy().writeBlob(desc.recordAddr, &req->record,
                          sizeof(req->record));
    } else {
        liveGeneration[generationKey] = desc.generation;
    }
    trace(*req, "controller", "REQUEST_ALLOC");
    requests.emplace(token, std::move(req));
    scheduleTick();
    return token;
}

uint64_t
CfdCoeffPreprocessController::launchLine(
    const CfdCoeffPreprocessDescriptor &desc)
{
    const uint64_t cells = desc.reserved[4];
    const uint64_t window = desc.reserved[1];
    const uint64_t required = CFD_COEFF_PRE_COEFF3 |
        CFD_COEFF_PRE_STREAMING_PROGRESS |
        CFD_COEFF_PRE_STREAMING_AUTO_RETIRE |
        CFD_COEFF_PRE_DINV_BASE_CONSUMER |
        CFD_COEFF_PRE_DINV_DIRECT_BYPASS |
        CFD_COEFF_PRE_LBAR_FORWARD_CONSUMER |
        CFD_COEFF_PRE_LBAR_DIRECT_BYPASS;
    const bool backwardDirect =
        desc.flags & CFD_COEFF_PRE_LINE_BACKWARD_DIRECT;
    const bool cacheEnabled =
        desc.flags & CFD_COEFF_PRE_LINE_COEFF_CACHE;
    const bool coefficientDirty =
        desc.flags & CFD_COEFF_PRE_LINE_COEFF_DIRTY;
    if (desc.version != DescriptorVersion || desc.size != sizeof(desc) ||
        (desc.flags & required) != required || !cells || !window ||
        window > 64 || !desc.dAddr || !desc.lAddr || !desc.uAddr ||
        !desc.luOutAddr || !desc.dInvOutAddr ||
        !desc.uBarOutAddr || !desc.recordAddr ||
        !desc.reserved[0] || !desc.reserved[3] ||
        (backwardDirect && !desc.reserved[2]) ||
        (cacheEnabled && (!backwardDirect || !desc.cellId)) ||
        (coefficientDirty && !cacheEnabled) ||
        desc.generation == 0)
        return 0;
    const uint64_t addressFields[] = {
        desc.dAddr, desc.lAddr, desc.uAddr, desc.luOutAddr,
        desc.dInvOutAddr, desc.lBarOutAddr, desc.uBarOutAddr,
        desc.recordAddr, desc.reserved[0], desc.reserved[2],
        desc.reserved[3]
    };
    for (uint64_t addr : addressFields) {
        if (addr & 7)
            return 0;
    }

    auto line = std::make_unique<LineRequest>();
    line->token = nextToken++;
    line->desc = desc;
    line->cells = cells;
    line->window = std::min<uint64_t>(window, cells);
    line->backwardDirect = backwardDirect;
    line->coefficientVersion = desc.cellId;
    line->coefficientCacheEnabled = cacheEnabled;
    line->coefficientDirty = coefficientDirty;
    line->dqStar.resize(cells);
    line->dq.resize(cells);
    line->rhs.resize(cells);
    line->dInv.resize(cells);
    line->lBar.resize(cells);
    line->uBar.resize(cells);
    line->dqStarReady.resize(cells, false);
    line->uBarReady.resize(cells, 0);
    if (cacheEnabled && !coefficientDirty) {
        auto cached = lineCoeffCaches.find(desc.lineId);
        if (env.traceEnable) {
            std::fprintf(
                stderr,
                "CFD_LINE_CACHE_LOOKUP line=%llu version=%llu cells=%llu "
                "found=%d cached_version=%llu cached_cells=%llu\n",
                (unsigned long long)desc.lineId,
                (unsigned long long)desc.cellId,
                (unsigned long long)cells,
                cached != lineCoeffCaches.end(),
                (unsigned long long)(cached != lineCoeffCaches.end() ?
                                     cached->second.version : 0),
                (unsigned long long)(cached != lineCoeffCaches.end() ?
                                     cached->second.cells : 0));
            std::fflush(stderr);
        }
        if (cached != lineCoeffCaches.end() &&
            cached->second.version == desc.cellId &&
            cached->second.cells == cells) {
            line->coefficientCacheHit = true;
            line->baseRing.resize(line->window);
            line->baseReady.resize(line->window, false);
            std::fill(line->uBarReady.begin(), line->uBarReady.end(), 0x1f);
            line->dInv = cached->second.dInv;
            line->lBar = cached->second.lBar;
            line->uBar = cached->second.uBar;
            for (uint64_t cell = 0; cell < cells; ++cell) {
                proxy().readBlob(
                    desc.reserved[0] + cell * VectorBytes,
                    line->rhs[cell].data(), VectorBytes);
            }
            line->cachedForwardStarted = true;
        }
    }
    const uint64_t token = line->token;
    lineRequests.emplace(token, std::move(line));
    const uint64_t busy =
        static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
    proxy().writeBlob(desc.dInvOutAddr, &busy, sizeof(busy));
    if (!lineRequests.at(token)->coefficientCacheHit)
        pumpLineRequests();
    scheduleTick();
    return token;
}

void
CfdCoeffPreprocessController::pumpLineRequests()
{
    const size_t capacity = env.inputSlots + env.luPendingDepth +
        env.solvePendingDepth + env.outputDepth;
    for (auto &[lineToken, linePtr] : lineRequests) {
        LineRequest &line = *linePtr;
        // A version-cache hit is executed by processLineCachedForward() and
        // processLineBackward().  It must never be expanded into coefficient
        // rebuild children by a later generic refill tick.
        if (line.coefficientCacheHit)
            continue;
        while (!line.terminal && line.nextCell < line.cells &&
               line.liveChildren < line.window &&
               requests.size() < capacity) {
            const uint64_t cell = line.nextCell;
            CfdCoeffPreprocessDescriptor child = line.desc;
            child.flags &= ~(CFD_COEFF_PRE_LINE_AUTONOMOUS |
                             CFD_COEFF_PRE_LINE_BACKWARD_DIRECT |
                             CFD_COEFF_PRE_LINE_COEFF_CACHE |
                             CFD_COEFF_PRE_LINE_COEFF_DIRTY);
            child.generation = line.desc.generation + cell;
            child.cellId = cell;
            child.dAddr += cell * MatrixBytes;
            child.lAddr += cell * MatrixBytes;
            child.uAddr += cell * MatrixBytes;
            child.luOutAddr += cell * MatrixBytes;
            if (child.dInvOutAddr)
                child.dInvOutAddr += cell * MatrixBytes;
            if (child.lBarOutAddr)
                child.lBarOutAddr += cell * MatrixBytes;
            child.uBarOutAddr += cell * MatrixBytes;
            child.recordAddr += cell * sizeof(CfdCoeffPreprocessRecord);
            child.reserved[0] = line.desc.reserved[0] + cell * VectorBytes;
            child.reserved[1] = 0;
            child.reserved[2] = 0;
            child.reserved[3] =
                line.desc.reserved[3] + cell * VectorBytes;
            child.reserved[4] = 0;
            child.flags &= ~(CFD_COEFF_PRE_NEED_DINV |
                             CFD_COEFF_PRE_NEED_LBAR |
                             CFD_COEFF_PRE_NEED_UBAR);
            child.flags |= CFD_COEFF_PRE_NEED_DINV;
            if (cell)
                child.flags |= CFD_COEFF_PRE_NEED_LBAR;
            if (cell + 1 < line.cells)
                child.flags |= CFD_COEFF_PRE_NEED_UBAR;
            const uint64_t childToken = launchDescriptor(child);
            if (!childToken)
                break;
            childLineTokens.emplace(childToken, lineToken);
            line.nextCell++;
            line.liveChildren++;
        }
    }
}

void
CfdCoeffPreprocessController::retireLineChild(
    uint64_t childToken, const Request &req)
{
    auto owner = childLineTokens.find(childToken);
    if (owner == childLineTokens.end())
        return;
    auto lineIt = lineRequests.find(owner->second);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        const uint64_t cell = req.desc.cellId;
        if (cell < line.cells) {
            line.dqStar[cell] = req.dqStar;
            line.dInv[cell] = req.dInv;
            line.lBar[cell] = req.lBar;
            line.uBar[cell] = req.uBar;
            line.dqStarReady[cell] = req.dqStarReadyLocal;
            line.uBarReady[cell] = (req.rhsDoneMask >> 10) & 0x1f;
        }
        if (line.liveChildren)
            line.liveChildren--;
        line.completedChildren++;
        if (req.status != CfdCoeffPreprocessStatus::Complete) {
            // A terminal cell can leave the final forward handoff waiting
            // for a non-existent successor.  It belongs to this line
            // invocation and must not survive into a later coefficient
            // version.
            forwardHandoffs.erase(line.desc.lineId);
            line.status = req.status;
            line.terminal = true;
            const uint64_t status = static_cast<uint64_t>(line.status);
            proxy().writeBlob(
                line.desc.dInvOutAddr, &status, sizeof(status));
        } else if (line.completedChildren == line.cells) {
            line.childrenRetiredCycle = cycle();
            forwardHandoffs.erase(line.desc.lineId);
            if (line.coefficientCacheEnabled) {
                LineCoeffCache cache;
                cache.version = line.coefficientVersion;
                cache.cells = line.cells;
                cache.dInv = line.dInv;
                cache.lBar = line.lBar;
                cache.uBar = line.uBar;
                lineCoeffCaches[line.desc.lineId] = std::move(cache);
                if (env.traceEnable) {
                    std::fprintf(
                        stderr,
                        "CFD_LINE_CACHE_STORE line=%llu version=%llu "
                        "cells=%llu\n",
                        (unsigned long long)line.desc.lineId,
                        (unsigned long long)line.coefficientVersion,
                        (unsigned long long)line.cells);
                    std::fflush(stderr);
                }
            }
            if (line.backwardDirect) {
                // The numerical wavefront may already have finished.  Do not
                // restart it at retirement; visibility still gates completion.
                if (!line.backwardStarted) {
                    line.backwardStarted = true;
                    line.backwardStartCycle = cycle();
                    line.backwardCell = static_cast<int64_t>(line.cells) - 2;
                    line.dq[line.cells - 1] = line.dqStar[line.cells - 1];
                    proxy().writeBlob(
                        line.desc.reserved[2] +
                            (line.cells - 1) * VectorBytes,
                        line.dq[line.cells - 1].data(), VectorBytes);
                }
                finishLineBackward(line);
            } else {
                line.status = CfdCoeffPreprocessStatus::Complete;
                line.terminal = true;
                const uint64_t status =
                    static_cast<uint64_t>(line.status);
                proxy().writeBlob(
                    line.desc.dInvOutAddr, &status, sizeof(status));
            }
        }
    }
    childLineTokens.erase(owner);
}

void
CfdCoeffPreprocessController::completeLineCachedForwardColumn(
    uint64_t lineToken, uint8_t phase, uint8_t column, uint64_t now)
{
    auto it = lineRequests.find(lineToken);
    if (it == lineRequests.end())
        return;
    LineRequest &line = *it->second;
    if (!line.cachedForwardStarted || line.terminal ||
        line.cachedForwardCell >= line.cells ||
        phase != line.cachedForwardPhase ||
        column != line.cachedForwardColumn)
        return;
    const uint64_t cell = line.cachedForwardCell;
    const auto &matrix =
        phase == 0 ? line.dInv[cell] : line.lBar[cell];
    const double scalar = phase == 0 ?
        line.rhs[cell][column] : line.dqStar[cell - 1][column];
    auto &accumulator = phase == 0 ?
        line.cachedBaseAccumulator : line.cachedCorrectionAccumulator;
    for (unsigned row = 0; row < N; ++row)
        accumulator[row] += matrix[row * N + column] * scalar;
    line.cachedForwardOpPending = false;
    if (++line.cachedForwardColumn < N)
        return;

    line.cachedForwardColumn = 0;
    if (phase == 0 && cell) {
        line.cachedForwardPhase = 1;
        return;
    }
    finishCachedForwardCell(line);
    (void)now;
}

void
CfdCoeffPreprocessController::finishCachedForwardCell(LineRequest &line)
{
    const uint64_t cell = line.cachedForwardCell;
    for (unsigned row = 0; row < N; ++row)
        line.dqStar[cell][row] = line.cachedBaseAccumulator[row] -
            (cell ? line.cachedCorrectionAccumulator[row] : 0.0);
    line.dqStarReady[cell] = true;
    proxy().writeBlob(
        line.desc.reserved[3] + cell * VectorBytes,
        line.dqStar[cell].data(), VectorBytes);
    line.cachedBaseAccumulator.fill(0.0);
    line.cachedCorrectionAccumulator.fill(0.0);
    line.cachedForwardPhase = 0;
    if (env.lineBaseAhead)
        line.baseReady[cell % line.window] = false;
    line.cachedForwardCell++;
    if (line.cachedForwardCell == line.cells) {
        line.cachedForwardStarted = false;
        line.backwardStarted = true;
        line.backwardStartCycle = cycle();
        line.backwardCell = static_cast<int64_t>(line.cells) - 2;
        line.dq[line.cells - 1] = line.dqStar[line.cells - 1];
        proxy().writeBlob(
            line.desc.reserved[2] + (line.cells - 1) * VectorBytes,
            line.dq[line.cells - 1].data(), VectorBytes);
    }
}

void
CfdCoeffPreprocessController::advanceLineMvm(
    LineRequest &line, unsigned context,
    const std::array<double, N * N> &matrix,
    const std::array<double, N> &vector, uint8_t readyMask, uint64_t now,
    std::function<void(LineRequest &, const std::array<double, N> &)> done)
{
    auto &state = line.mvm[context];
    const uint64_t token = line.token;
    const uint64_t epoch = state.epoch;
    // The feedback chain is still sequential.  Only independent products can
    // run ahead; products are rounded to double before ordered addition.
    if (!state.addPending && state.added < N && state.ready[state.added]) {
        const unsigned col = state.added;
        if (issue(lineAccumulate, OpKind::LineAccumulate, now,
                  [this, token, context, epoch, col, done] {
                auto it = lineRequests.find(token);
                if (it == lineRequests.end() || it->second->terminal)
                    return;
                auto &line = *it->second;
                auto &state = line.mvm[context];
                if (state.epoch != epoch)
                    return;
                for (unsigned row = 0; row < N; ++row)
                    state.sum[row] += state.products[col][row];
                state.addPending = false;
                state.ready[col] = false;
                if (++state.added == N) {
                    const auto result = state.sum;
                    state = LineMvmState{};
                    state.epoch = epoch + 1;
                    done(line, result);
                }
            })) {
            state.addPending = true;
            line.lineAdds++;
        }
    }
    if (state.issued >= N)
        return;
    if (state.issued - state.added >= env.lineProductDepth) {
        line.lineBufferStalls++;
        return;
    }
    const unsigned col = state.issued;
    if (!(readyMask & (1u << col)))
        return;
    std::array<double, N> column;
    for (unsigned row = 0; row < N; ++row)
        column[row] = matrix[row * N + col];
    const double scalar = vector[col];
    if (!issue(lineProduct, OpKind::LineProduct, now,
               [this, token, context, epoch, col, column, scalar] {
            auto it = lineRequests.find(token);
            if (it == lineRequests.end() || it->second->terminal)
                return;
            auto &state = it->second->mvm[context];
            if (state.epoch != epoch)
                return;
            for (unsigned row = 0; row < N; ++row)
                state.products[col][row] = column[row] * scalar;
            state.ready[col] = true;
        }))
        return;
    state.issued++;
    line.lineProducts++;
    line.lineBufferedMax = std::max<uint64_t>(line.lineBufferedMax,
                                             state.issued - state.added);
}

void
CfdCoeffPreprocessController::processLineCachedForward(uint64_t now)
{
    for (auto &[lineToken, linePtr] : lineRequests) {
        LineRequest &line = *linePtr;
        if (!line.cachedForwardStarted || line.terminal ||
            line.cachedForwardOpPending ||
            line.cachedForwardCell >= line.cells)
            continue;
        if (env.lineBaseAhead) {
            const auto slot = line.cachedForwardCell % line.window;
            if (!line.baseReady[slot])
                continue;
            line.cachedBaseAccumulator = line.baseRing[slot];
            if (!line.cachedForwardCell) {
                finishCachedForwardCell(line);
                continue;
            }
            line.cachedForwardPhase = 1;
        }
        const uint8_t phase = line.cachedForwardPhase;
        const uint8_t column = line.cachedForwardColumn;
        if (env.lineMvmSplit) {
            const uint64_t cell = line.cachedForwardCell;
            advanceLineMvm(line, 0,
                phase == 0 ? line.dInv[cell] : line.lBar[cell],
                phase == 0 ? line.rhs[cell] : line.dqStar[cell - 1],
                0x1f, now, [this, phase](LineRequest &line, const auto &sum) {
                    if (phase == 0) {
                        line.cachedBaseAccumulator = sum;
                        if (line.cachedForwardCell) {
                            line.cachedForwardPhase = 1;
                            return;
                        }
                    } else {
                        line.cachedCorrectionAccumulator = sum;
                    }
                    finishCachedForwardCell(line);
                });
            continue;
        }
        if (!issue(columnFma, OpKind::ColumnFma, now,
                   [this, lineToken, phase, column] {
                       completeLineCachedForwardColumn(
                           lineToken, phase, column, cycle());
                   }))
            continue;
        line.cachedForwardOpPending = true;
    }
}

void
CfdCoeffPreprocessController::processLineBaseAhead(uint64_t now)
{
    if (!env.lineBaseAhead)
        return;
    // Called after all frontier consumers: background work uses spare issue
    // capacity, not another arithmetic unit.  Each sum retains column order.
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        if (!line.cachedForwardStarted || line.terminal || line.basePending ||
            line.baseCell >= line.cells ||
            line.baseCell - line.cachedForwardCell >= line.window)
            continue;
        const uint64_t cell = line.baseCell;
        const uint8_t col = line.baseColumn;
        if (env.lineMvmSplit) {
            const auto before = line.lineProducts;
            advanceLineMvm(line, 2, line.dInv[cell], line.rhs[cell], 0x1f,
                now, [cell](LineRequest &line, const auto &sum) {
                    line.baseRing[cell % line.window] = sum;
                    line.baseReady[cell % line.window] = true;
                    line.baseCell++;
                });
            if (cell > line.cachedForwardCell)
                line.baseAheadColumns += line.lineProducts - before;
            continue;
        }
        if (!issue(columnFma, OpKind::ColumnFma, now, [this, token, cell, col] {
                auto it = lineRequests.find(token);
                if (it == lineRequests.end() || it->second->terminal)
                    return;
                auto &line = *it->second;
                auto &base = line.baseRing[cell % line.window];
                if (!col)
                    base.fill(0.0);
                for (unsigned row = 0; row < N; ++row)
                    base[row] += line.dInv[cell][row * N + col] *
                                 line.rhs[cell][col];
                line.basePending = false;
                if (++line.baseColumn == N) {
                    line.baseReady[cell % line.window] = true;
                    line.baseColumn = 0;
                    line.baseCell++;
                }
            }))
            continue;
        line.basePending = true;
        line.baseAheadColumns += cell > line.cachedForwardCell;
    }
}

void
CfdCoeffPreprocessController::publishLineValues()
{
    if (!env.lineEarlyBackward)
        return;
    // Copy ready values into parent-owned storage, so child retirement cannot
    // invalidate a consumer.  Ownership is by unique token, not by lineId.
    for (const auto &[child, parent] : childLineTokens) {
        auto r = requests.find(child);
        auto p = lineRequests.find(parent);
        if (r == requests.end() || p == lineRequests.end())
            continue;
        const auto &req = *r->second;
        auto &line = *p->second;
        if (line.terminal || !line.backwardDirect ||
            req.stage == RequestStage::Failed)
            continue;
        const auto cell = req.desc.cellId;
        if (cell >= line.cells)
            continue;
        if (req.dqStarReadyLocal && !line.dqStarReady[cell]) {
            line.dqStar[cell] = req.dqStar;
            line.dqStarReady[cell] = true;
        }
        const uint8_t ready = (req.rhsDoneMask >> 10) & 0x1f;
        for (unsigned col = 0; col < N; ++col) {
            if ((ready & (1u << col)) &&
                !(line.uBarReady[cell] & (1u << col))) {
                for (unsigned row = 0; row < N; ++row)
                    line.uBar[cell][row * N + col] =
                        req.uBar[row * N + col];
            }
        }
        line.uBarReady[cell] |= ready;
    }
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        if (line.terminal || !line.backwardDirect || line.backwardStarted ||
            !line.dqStarReady.back())
            continue;
        line.backwardStarted = true;
        line.backwardStartCycle = cycle();
        line.backwardCell = static_cast<int64_t>(line.cells) - 2;
        line.dq.back() = line.dqStar.back();
        proxy().writeBlob(line.desc.reserved[2] +
                         (line.cells - 1) * VectorBytes,
                         line.dq.back().data(), VectorBytes);
    }
}

void
CfdCoeffPreprocessController::finishLineBackward(LineRequest &line)
{
    if (line.terminal || !line.backwardStarted || line.backwardOpPending ||
        line.backwardCell >= 0)
        return;
    if (!line.backwardDoneCycle)
        line.backwardDoneCycle = cycle();
    // Numeric completion is not software-visible completion: all child
    // drains and stable completion records must still retire successfully.
    if (!line.coefficientCacheHit && line.completedChildren != line.cells)
        return;
    line.status = CfdCoeffPreprocessStatus::Complete;
    line.terminal = true;
    const uint64_t status = static_cast<uint64_t>(line.status);
    proxy().writeBlob(line.desc.dInvOutAddr, &status, sizeof(status));
    if (env.traceEnable) {
        // Hash the actual FP64 bit patterns, not formatted decimal values.
        uint64_t hash = 14695981039346656037ULL;
        for (const auto *vectors : {&line.dqStar, &line.dq}) {
            for (const auto &vec : *vectors) {
                for (double value : vec) {
                    uint64_t bits;
                    std::memcpy(&bits, &value, sizeof(bits));
                    hash = (hash ^ bits) * 1099511628211ULL;
                }
            }
        }
        std::fprintf(stderr, "CFD_LINE_SCHEDULE token=%llu cache_hit=%d "
                     "base_ahead_columns=%llu backward_start=%llu "
                     "backward_done=%llu visible_done=%llu "
                     "children_retired=%llu result_hash=%016llx "
                     "line_products=%llu line_adds=%llu buffered_max=%llu "
                     "buffer_stalls=%llu\n",
                     (unsigned long long)line.token, line.coefficientCacheHit,
                     (unsigned long long)line.baseAheadColumns,
                     (unsigned long long)line.backwardStartCycle,
                     (unsigned long long)line.backwardDoneCycle,
                     (unsigned long long)cycle(),
                     (unsigned long long)line.childrenRetiredCycle,
                     (unsigned long long)hash,
                     (unsigned long long)line.lineProducts,
                     (unsigned long long)line.lineAdds,
                     (unsigned long long)line.lineBufferedMax,
                     (unsigned long long)line.lineBufferStalls);
    }
}

void
CfdCoeffPreprocessController::completeLineBackwardColumn(
    uint64_t lineToken, uint8_t column, uint64_t now)
{
    auto it = lineRequests.find(lineToken);
    if (it == lineRequests.end())
        return;
    LineRequest &line = *it->second;
    if (!line.backwardStarted || line.terminal || line.backwardCell < 0 ||
        column != line.backwardColumn)
        return;
    const size_t cell = static_cast<size_t>(line.backwardCell);
    const double scalar = line.dq[cell + 1][column];
    for (unsigned row = 0; row < N; ++row)
        line.backwardAccumulator[row] +=
            line.uBar[cell][row * N + column] * scalar;
    line.backwardOpPending = false;
    if (++line.backwardColumn < N)
        return;
    finishLineBackwardCell(line);
    (void)now;
}

void
CfdCoeffPreprocessController::finishLineBackwardCell(LineRequest &line)
{
    const size_t cell = static_cast<size_t>(line.backwardCell);
    for (unsigned row = 0; row < N; ++row)
        line.dq[cell][row] =
            line.dqStar[cell][row] - line.backwardAccumulator[row];
    proxy().writeBlob(
        line.desc.reserved[2] + cell * VectorBytes,
        line.dq[cell].data(), VectorBytes);
    line.backwardAccumulator.fill(0.0);
    line.backwardColumn = 0;
    line.backwardCell--;
    finishLineBackward(line);
}

void
CfdCoeffPreprocessController::processLineBackward(uint64_t now)
{
    for (auto &[lineToken, linePtr] : lineRequests) {
        LineRequest &line = *linePtr;
        if (!line.backwardStarted || line.terminal ||
            line.backwardOpPending)
            continue;
        if (line.backwardCell < 0) {
            finishLineBackward(line);
            continue;
        }
        if (env.lineMvmSplit) {
            const size_t cell = static_cast<size_t>(line.backwardCell);
            if (!line.dqStarReady[cell])
                continue;
            advanceLineMvm(line, 1, line.uBar[cell], line.dq[cell + 1],
                line.uBarReady[cell], now,
                [this](LineRequest &line, const auto &sum) {
                    line.backwardAccumulator = sum;
                    finishLineBackwardCell(line);
                });
            continue;
        }
        const uint8_t column = line.backwardColumn;
        if (!line.dqStarReady[line.backwardCell] ||
            !(line.uBarReady[line.backwardCell] & (1u << column)))
            continue;
        if (!issue(columnFma, OpKind::ColumnFma, now,
                   [this, lineToken, column] {
                       completeLineBackwardColumn(
                           lineToken, column, cycle());
                   }))
            continue;
        line.backwardOpPending = true;
    }
}

uint64_t
CfdCoeffPreprocessController::wait(uint64_t token)
{
    auto lineIt = lineRequests.find(token);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        if (!line.terminal)
            return static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
        const uint64_t status = static_cast<uint64_t>(line.status);
        lineRequests.erase(lineIt);
        const bool activeRequest = std::any_of(
            requests.begin(), requests.end(),
            [](const auto &entry) {
                return entry.second->stage != RequestStage::Complete &&
                    entry.second->stage != RequestStage::Failed;
            });
        const bool activeLine = std::any_of(
            lineRequests.begin(), lineRequests.end(),
            [](const auto &entry) { return !entry.second->terminal; });
        if (tickEvent.scheduled() && !activeRequest && !activeLine &&
            pendingOps.empty() && columnFmaQueue.empty() &&
            forwardCombineQueue.empty() && drainQueue.empty() &&
            activeDrains.empty())
            eventq()->deschedule(&tickEvent);
        return status;
    }
    Request *req = find(token);
    if (!req)
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::BadToken);
    if (req->stage != RequestStage::Complete &&
        req->stage != RequestStage::Failed) {
        if (req->desc.flags & CFD_COEFF_PRE_STREAMING_PROGRESS) {
            const bool dInv = req->drainDone[
                static_cast<unsigned>(DrainType::DInv)];
            const bool lBar = req->drainDone[
                static_cast<unsigned>(DrainType::LBar)];
            const bool uBar = req->drainDone[
                static_cast<unsigned>(DrainType::UBar)];
            if (dInv && lBar && uBar)
                return static_cast<uint64_t>(
                    CfdCoeffPreprocessStatus::DInvLBarUBarReady);
            if (dInv && lBar)
                return static_cast<uint64_t>(
                    CfdCoeffPreprocessStatus::DInvLBarReady);
            if (dInv)
                return static_cast<uint64_t>(
                    CfdCoeffPreprocessStatus::DInvReady);
        }
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
    }
    const uint64_t status = static_cast<uint64_t>(req->status);
    trace(*req, "controller", "REQUEST_REAP");
    requests.erase(token);
    return status;
}

uint64_t
CfdCoeffPreprocessController::cancel(uint64_t token)
{
    auto lineIt = lineRequests.find(token);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        if (line.terminal)
            return static_cast<uint64_t>(
                CfdCoeffPreprocessStatus::AlreadyComplete);
        line.terminal = true;
        line.status = CfdCoeffPreprocessStatus::Cancelled;
        const uint64_t status = static_cast<uint64_t>(line.status);
        proxy().writeBlob(line.desc.dInvOutAddr, &status, sizeof(status));
        std::vector<uint64_t> children;
        for (const auto &[child, owner] : childLineTokens) {
            if (owner == token)
                children.push_back(child);
        }
        for (uint64_t child : children)
            cancel(child);
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::Cancelled);
    }
    Request *req = find(token);
    if (!req)
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::BadToken);
    if (req->stage == RequestStage::Complete ||
        req->stage == RequestStage::Failed)
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::AlreadyComplete);

    const bool pending = req->stage == RequestStage::LuActive ||
        req->solveStarted ||
        std::any_of(activeDrains.begin(), activeDrains.end(),
            [token](const DrainTask &task) { return task.token == token; });
    if (req->slotOwned) {
        slots[req->slot] = false;
        req->slotOwned = false;
    }
    drainQueue.erase(std::remove_if(drainQueue.begin(), drainQueue.end(),
        [token](const DrainTask &task) { return task.token == token; }),
        drainQueue.end());
    activeDrains.erase(std::remove_if(activeDrains.begin(), activeDrains.end(),
        [token](const DrainTask &task) { return task.token == token; }),
        activeDrains.end());
    columnFmaQueue.erase(std::remove_if(
        columnFmaQueue.begin(), columnFmaQueue.end(),
        [token](const ColumnFmaTask &task) { return task.token == token; }),
        columnFmaQueue.end());
    forwardCombineQueue.erase(std::remove_if(
        forwardCombineQueue.begin(), forwardCombineQueue.end(),
        [token](const ForwardCombineTask &task) {
            return task.token == token;
        }), forwardCombineQueue.end());
    auto handoff = forwardHandoffs.find(req->desc.lineId);
    if (handoff != forwardHandoffs.end() && handoff->second.valid &&
        handoff->second.producerCell == req->desc.cellId &&
        handoff->second.producerGeneration == req->desc.generation &&
        !handoff->second.consumed)
        forwardHandoffs.erase(handoff);
    if (req->baseConsumerEnabled && !req->baseComputeComplete)
        req->record.baseAccumulatorsCancelled = 1;
    if (req->correctionInitialized && !req->correctionComplete)
        req->record.correctionAccumulatorsCancelled = 1;
    req->stage = RequestStage::Failed;
    req->status = CfdCoeffPreprocessStatus::Cancelled;
    req->record.status = static_cast<uint64_t>(req->status);
    req->record.completeCycle = cycle();
    trace(*req, "controller", pending ? "CANCEL_PENDING" : "CANCEL_COMPLETE");
    proxy().writeBlob(req->desc.recordAddr, &req->record, sizeof(req->record));
    return static_cast<uint64_t>(pending ?
        CfdCoeffPreprocessStatus::CancelPending :
        CfdCoeffPreprocessStatus::Cancelled);
}

void
CfdCoeffPreprocessController::completeOps(uint64_t now)
{
    std::vector<PendingOp> remaining;
    remaining.reserve(pendingOps.size());
    for (auto &op : pendingOps) {
        if (op.completeCycle <= now)
            op.complete();
        else
            remaining.push_back(std::move(op));
    }
    pendingOps.swap(remaining);
}

void
CfdCoeffPreprocessController::assignInputSlots(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage != RequestStage::WaitingSlot)
            continue;
        auto it = std::find(slots.begin(), slots.end(), false);
        if (it == slots.end()) {
            observe("input_slot_full");
            req.record.stallInputSlot++;
            continue;
        }
        req.slot = std::distance(slots.begin(), it);
        *it = true;
        req.slotOwned = true;
        req.record.slot = req.slot;
        req.record.inputStartCycle = now;
        req.stage = RequestStage::InputFetching;
        trace(req, "input", "INPUT_START");
    }
}

void
CfdCoeffPreprocessController::processInputs(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage != RequestStage::InputFetching)
            continue;
        if (!req.inputCaptured) {
            if (!env.dmaSpm) {
                proxy().readBlob(req.desc.dAddr, req.d.data(), MatrixBytes);
                if (batchRequested(req, 2))
                    proxy().readBlob(
                        req.desc.uAddr, req.upper.data(), MatrixBytes);
                if (batchRequested(req, 1))
                    proxy().readBlob(
                        req.desc.lAddr, req.lower.data(), MatrixBytes);
                if (req.baseConsumerEnabled) {
                    proxy().readBlob(
                        req.desc.reserved[0], req.rhsVector.data(),
                        VectorBytes);
                    req.record.rhsVectorIssueCycle = now;
                }
            }
            req.inputCaptured = true;
            req.inputReadyCycle =
                env.dmaSpm ? now : now + env.sourceReadLatency;
        }
        if (now < req.inputReadyCycle) {
            req.record.stallDependency++;
            continue;
        }
        if (env.packetSpm || env.dmaSpm) {
            if (req.inputBytesIssued >= req.record.inputBytes &&
                req.inputChunksCompleted >= req.inputChunksIssued) {
                req.record.inputDoneCycle = now;
                if (req.baseConsumerEnabled) {
                    req.rhsVectorReady = true;
                    req.record.rhsVectorReadyCycle = now;
                }
                req.stage = RequestStage::InputReady;
                trace(req, "input", "INPUT_TOKEN");
                continue;
            }
            if (req.inputBytesIssued >= req.record.inputBytes) {
                req.record.stallDependency++;
                continue;
            }
            CfdLocalSpm *spm = getCfdLocalSpm();
            if (!spm) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "missing-local-spm");
                continue;
            }
            const unsigned offset = req.inputBytesIssued;
            const unsigned matrixOffset = inputRegionOffset(req, offset);
            const unsigned regionBytes = inputRegionBytes(req, offset);
            unsigned bytes = std::min<unsigned>(
                env.spmWriteWidth, req.record.inputBytes - offset);
            bytes = std::min<unsigned>(bytes, regionBytes - matrixOffset);
            if (env.spmLayout == SpmLayout::RowStriped &&
                inputMatrix(req, offset) != CfdSpmMatrix::RhsVector)
                bytes = std::min<unsigned>(bytes,
                    N * sizeof(double) -
                    matrixOffset % (N * sizeof(double)));
            const CfdSpmMatrix matrix = inputMatrix(req, offset);
            Addr addr = packetAddress(
                req, matrix, matrixOffset, offset, true);
            uint8_t *data = nullptr;
            if (env.dmaSpm) {
                switch (matrix) {
                  case CfdSpmMatrix::D:
                  case CfdSpmMatrix::Lu:
                    addr = req.desc.dAddr + matrixOffset;
                    data = reinterpret_cast<uint8_t *>(req.d.data()) +
                        matrixOffset;
                    break;
                  case CfdSpmMatrix::L:
                    addr = req.desc.lAddr + matrixOffset;
                    data = reinterpret_cast<uint8_t *>(req.lower.data()) +
                        matrixOffset;
                    break;
                  case CfdSpmMatrix::U:
                    addr = req.desc.uAddr + matrixOffset;
                    data = reinterpret_cast<uint8_t *>(req.upper.data()) +
                        matrixOffset;
                    break;
                  case CfdSpmMatrix::RhsVector:
                    addr = req.desc.reserved[0] + matrixOffset;
                    data =
                        reinterpret_cast<uint8_t *>(req.rhsVector.data()) +
                        matrixOffset;
                    req.record.rhsVectorIssueCycle =
                        req.record.rhsVectorIssueCycle ?
                        req.record.rhsVectorIssueCycle : now;
                    break;
                  default:
                    break;
                }
                bytes = spm->dmaChunkSize(tc, addr, bytes);
            }
            const uint64_t tok = token;
            auto packetMeta = std::make_shared<CfdSpmPacketIssue>();
            auto completion =
                [this, tok, bytes, addr, matrix, packetMeta](
                    uint64_t packetId) {
                    completeInputPacket(tok, bytes, packetId, addr,
                        packetMeta->bank, packetMeta->port, matrix);
                };
            CfdSpmPacketIssue packet = env.dmaSpm ?
                spm->issueDma(tc, false, addr, bytes, data,
                    env.spmOutstanding, std::move(completion)) :
                spm->issuePacket(
                    CfdSpmRequester::CoeffInput, true, addr, bytes,
                    env.spmWriteLatency, env.spmWritePorts,
                    env.spmOutstanding, env.spmBanks,
                    env.spmBankGranularity, matrix, std::move(completion));
            if (!packet.accepted) {
                req.record.coeffSpmOutstandingStalls++;
                trace(req, "spm", "SPM_PACKET_STALL_OUTSTANDING", -1,
                      -1, -1, -1, "write", "CoeffInput", 0, addr, bytes);
                continue;
            }
            *packetMeta = packet;
            req.inputChunksIssued++;
            req.inputBytesIssued += bytes;
            req.record.inputWriteRequests++;
            req.record.coeffSpmPacketIssued++;
            req.record.coeffInputPacketBytes += bytes;
            req.record.coeffSpmWritePortStalls += packet.portWaitCycles;
            req.record.coeffSpmBankStalls += packet.bankWaitCycles;
            trace(req, "spm", "SPM_PACKET_ISSUE", -1, -1, -1, -1,
                  "write", "CoeffInput", packet.packetId, addr, bytes,
                  packet.bank, packet.port);
            trace(req, "spm", "SPM_PACKET_ACCEPT", -1, -1, -1, -1,
                  "write", "CoeffInput", packet.packetId, addr, bytes,
                  packet.bank, packet.port);
            if (packet.portWaitCycles)
                trace(req, "spm", "SPM_PACKET_STALL_PORT", -1, -1, -1,
                      -1, "write", "CoeffInput", packet.packetId, addr,
                      bytes, packet.bank, packet.port);
            if (packet.bankWaitCycles)
                trace(req, "spm", "SPM_PACKET_STALL_BANK", -1, -1, -1,
                      -1, "write", "CoeffInput", packet.packetId, addr,
                      bytes, packet.bank, packet.port);
            continue;
        }
        if (req.inputChunk >= req.inputChunks) {
            req.record.inputDoneCycle = now;
            if (req.baseConsumerEnabled) {
                req.rhsVectorReady = true;
                req.record.rhsVectorReadyCycle = now;
            }
            req.stage = RequestStage::InputReady;
            trace(req, "input", "INPUT_TOKEN");
            continue;
        }
        if (!spmWritePortsAvail) {
            req.record.stallSpmWritePort++;
            continue;
        }
        const uint64_t slotBase = req.slot * 0x800ull;
        const uint64_t addr = slotBase +
            static_cast<uint64_t>(req.inputChunk) * env.spmWriteWidth;
        const uint32_t bank =
            (addr / env.spmBankGranularity) % env.spmBanks;
        if (spmBanksUsed[bank]) {
            req.record.stallSpmBank++;
            continue;
        }
        spmBanksUsed[bank] = true;
        spmWritePortsAvail--;
        req.inputChunk++;
        req.record.inputWriteRequests++;
        req.inputReadyCycle = now + env.spmWriteLatency;
        if (req.inputChunk == req.inputChunks)
            trace(req, "input", "INPUT_U_DONE");
    }
}

void
CfdCoeffPreprocessController::completeInputPacket(
    uint64_t token, unsigned bytes, uint64_t packetId, Addr addr,
    unsigned bank, unsigned port, CfdSpmMatrix matrix)
{
    (void)bytes;
    (void)matrix;
    Request *req = find(token);
    if (!req || req->stage == RequestStage::Failed)
        return;
    req->inputChunksCompleted++;
    req->record.coeffSpmPacketCompleted++;
    trace(*req, "spm", "SPM_PACKET_RESPONSE", -1, -1, -1, -1,
          "write-complete", "CoeffInput", packetId, addr, bytes, bank,
          port);
}

void
CfdCoeffPreprocessController::startLuRequests(uint64_t now)
{
    uint32_t active = 0, queued = 0;
    for (auto &[token, req] : requests)
        if (req->stage == RequestStage::LuActive)
            active++;
        else if (req->stage == RequestStage::LuQueued)
            queued++;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage != RequestStage::InputReady &&
            req.stage != RequestStage::LuQueued)
            continue;
        if (req.desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT) {
            if (req.slotOwned) {
                slots[req.slot] = false;
                req.slotOwned = false;
                trace(req, "input", "INPUT_SLOT_RELEASE");
            }
            req.lu = req.d;
            req.luDone = true;
            req.record.luIssueCycle = now;
            req.record.luFirstResultCycle = now;
            req.record.luCompleteCycle = now;
            for (unsigned k = 0; k < N; ++k) {
                req.forwardStepReady[k] = true;
                req.backwardStepReady[k] = true;
                req.record.luStepReadyCycle[k] = now;
            }
            req.stage = RequestStage::LuReady;
            trace(req, "lu", "SOFTWARE_LU_READY");
            continue;
        }
        if (active >= env.luCount) {
            observe("lu_context_full");
            if (req.stage != RequestStage::LuQueued &&
                queued < env.luPendingDepth) {
                req.stage = RequestStage::LuQueued;
                queued++;
            }
            req.record.stallLuPending++;
            continue;
        }
        if (req.stage == RequestStage::LuQueued)
            queued--;
        if (req.slotOwned) {
            slots[req.slot] = false;
            req.slotOwned = false;
            trace(req, "input", "INPUT_SLOT_RELEASE");
        }
        req.stage = RequestStage::LuActive;
        req.record.luIssueCycle = now;
        req.luPhase = LuPhase::StartLower;
        active++;
        trace(req, "lu", "LU_ISSUE");
    }
}

void
CfdCoeffPreprocessController::processLu(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage != RequestStage::LuActive)
            continue;
        if (req.luWaiting) {
            req.record.stallDependency++;
            continue;
        }
        switch (req.luPhase) {
          case LuPhase::StartLower:
            req.luRow = req.luCol;
            req.luK = 0;
            req.luTemp = req.d[req.luRow * N + req.luCol];
            req.luPhase = req.luK < req.luCol ?
                LuPhase::LowerMul : LuPhase::StoreLower;
            break;
          case LuPhase::LowerMul:
          {
            req.luWaiting = true;
            const uint64_t tok = token;
            const int row = req.luRow, col = req.luCol, k = req.luK;
            if (!issue(luMul, OpKind::LuMul, now, [this, tok, row, col, k] {
                Request *r = find(tok);
                if (!r) return;
                volatile double p = r->lu[row * N + k] *
                                    r->lu[k * N + col];
                r->luProduct = p;
                r->luWaiting = false;
                r->luPhase = LuPhase::LowerSub;
            })) {
                req.luWaiting = false;
                req.record.stallLuMulSub++;
            } else {
                req.record.luMulIssued++;
                trace(req, "lu", "LU_MUL_ISSUE", -1, -1,
                      req.luK, req.luRow);
            }
            break;
          }
          case LuPhase::LowerSub:
          {
            req.luWaiting = true;
            const uint64_t tok = token;
            if (!issue(luSub, OpKind::LuSub, now, [this, tok] {
                Request *r = find(tok);
                if (!r) return;
                volatile double v = r->luTemp - r->luProduct;
                r->luTemp = v;
                r->luK++;
                r->luWaiting = false;
                r->luPhase = r->luK < r->luCol ?
                    LuPhase::LowerMul : LuPhase::StoreLower;
            })) {
                req.luWaiting = false;
                req.record.stallLuMulSub++;
            } else {
                req.record.luSubIssued++;
            }
            break;
          }
          case LuPhase::StoreLower:
            req.lu[req.luRow * N + req.luCol] = req.luTemp;
            if (!req.record.luFirstResultCycle)
                req.record.luFirstResultCycle = now;
            req.luRow++;
            if (req.luRow < static_cast<int>(N)) {
                req.luK = 0;
                req.luTemp = req.d[req.luRow * N + req.luCol];
                req.luPhase = req.luK < req.luCol ?
                    LuPhase::LowerMul : LuPhase::StoreLower;
            } else {
                req.luPhase = LuPhase::PivotCheck;
            }
            break;
          case LuPhase::PivotCheck:
            if (!std::isfinite(req.lu[req.luCol * N + req.luCol]) ||
                std::fabs(req.lu[req.luCol * N + req.luCol]) <=
                    req.desc.pivotEpsilon) {
                markFailed(req, CfdCoeffPreprocessStatus::LuFailure,
                           "pivot");
                break;
            }
            req.forwardStepReady[req.luCol] = true;
            req.record.luStepReadyCycle[req.luCol] = now;
            trace(req, "lu", "LU_STEP_READY", -1, -1, req.luCol,
                  -1, "forward");
            req.luUpperCol = req.luCol + 1;
            req.luPhase = req.luUpperCol < static_cast<int>(N) ?
                LuPhase::StartUpper : LuPhase::NextColumn;
            break;
          case LuPhase::StartUpper:
            req.luK = 0;
            req.luTemp = req.d[req.luCol * N + req.luUpperCol];
            req.luPhase = req.luK < req.luCol ?
                LuPhase::UpperMul : LuPhase::DivideUpper;
            break;
          case LuPhase::UpperMul:
          {
            req.luWaiting = true;
            const uint64_t tok = token;
            const int row = req.luCol, col = req.luUpperCol, k = req.luK;
            if (!issue(luMul, OpKind::LuMul, now, [this, tok, row, col, k] {
                Request *r = find(tok);
                if (!r) return;
                volatile double p = r->lu[row * N + k] *
                                    r->lu[k * N + col];
                r->luProduct = p;
                r->luWaiting = false;
                r->luPhase = LuPhase::UpperSub;
            })) {
                req.luWaiting = false;
                req.record.stallLuMulSub++;
            } else {
                req.record.luMulIssued++;
            }
            break;
          }
          case LuPhase::UpperSub:
          {
            req.luWaiting = true;
            const uint64_t tok = token;
            if (!issue(luSub, OpKind::LuSub, now, [this, tok] {
                Request *r = find(tok);
                if (!r) return;
                volatile double v = r->luTemp - r->luProduct;
                r->luTemp = v;
                r->luK++;
                r->luWaiting = false;
                r->luPhase = r->luK < r->luCol ?
                    LuPhase::UpperMul : LuPhase::DivideUpper;
            })) {
                req.luWaiting = false;
                req.record.stallLuMulSub++;
            } else {
                req.record.luSubIssued++;
            }
            break;
          }
          case LuPhase::DivideUpper:
          {
            req.luWaiting = true;
            const uint64_t tok = token;
            if (!issue(luDiv, OpKind::LuDiv, now, [this, tok] {
                Request *r = find(tok);
                if (!r) return;
                volatile double v = r->luTemp /
                    r->lu[r->luCol * N + r->luCol];
                r->luTemp = v;
                r->luWaiting = false;
                r->luPhase = LuPhase::StoreUpper;
            })) {
                req.luWaiting = false;
                req.record.stallLuDivider++;
            } else {
                req.record.luDivIssued++;
                trace(req, "lu", "LU_DIV_ISSUE", -1, -1,
                      req.luCol, req.luUpperCol);
            }
            break;
          }
          case LuPhase::StoreUpper:
            if (!std::isfinite(req.luTemp)) {
                markFailed(req, CfdCoeffPreprocessStatus::LuFailure,
                           "upper");
                break;
            }
            req.lu[req.luCol * N + req.luUpperCol] = req.luTemp;
            req.luUpperCol++;
            req.luPhase = req.luUpperCol < static_cast<int>(N) ?
                LuPhase::StartUpper : LuPhase::NextColumn;
            break;
          case LuPhase::NextColumn:
            req.backwardStepReady[req.luCol] = true;
            trace(req, "lu", "LU_COLUMN_DONE", -1, -1, req.luCol);
            req.luCol++;
            req.luPhase = req.luCol < static_cast<int>(N) ?
                LuPhase::StartLower : LuPhase::Done;
            break;
          case LuPhase::Done:
            req.record.luCompleteCycle = now;
            req.luDone = true;
            req.stage = req.solveStarted ? RequestStage::SolveActive :
                                           RequestStage::LuReady;
            trace(req, "lu", "LU_COMPLETE");
            break;
        }
    }
}

void
CfdCoeffPreprocessController::initializeRhs(Request &req)
{
    for (unsigned rhs = 0; rhs < req.rhsSlots; ++rhs) {
        if (!(req.rhsEnabledMask & (1u << rhs)))
            continue;
        RhsState &state = req.rhs[rhs];
        const unsigned batch = rhsBatch(req, rhs);
        const unsigned col = rhsColumn(req, rhs);
        for (unsigned row = 0; row < N; ++row) {
            if (batch == 0)
                state.value[row] = row == col ? 1.0 : 0.0;
            else if (batch == 1)
                state.value[row] = req.lower[row * N + col];
            else
                state.value[row] = req.upper[row * N + col];
        }
        state.phase = RhsPhase::ForwardDiv;
        state.k = 0;
        state.i = 1;
    }
}

void
CfdCoeffPreprocessController::activateRhs(Request &req)
{
    unsigned active = 0;
    for (unsigned i = 0; i < req.rhsSlots; ++i)
        active += req.rhs[i].active && !req.rhs[i].done;
    while (active < env.rhsLanes) {
        const uint16_t candidates = req.rhsEnabledMask &
            ~req.rhsActivatedMask & ~req.rhsDoneMask;
        if (!candidates)
            break;
        unsigned rhs = 0;
        while (!(candidates & (1u << rhs)))
            ++rhs;
        req.rhs[rhs].active = true;
        req.rhsActivatedMask |= 1u << rhs;
        active++;
    }
}

void
CfdCoeffPreprocessController::startSolveRequests(uint64_t now)
{
    uint32_t active = 0, queued = 0;
    for (auto &[token, req] : requests)
        if (req->solveStarted && !req->solveDone)
            active++;
        else if (req->stage == RequestStage::SolveQueued)
            queued++;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.solveStarted)
            continue;
        const bool early = env.luSolveEarlyStart && env.luForwarding &&
            req.stage == RequestStage::LuActive && req.forwardStepReady[0];
        const bool normal = req.stage == RequestStage::LuReady ||
                            req.stage == RequestStage::SolveQueued;
        if (!early && !normal)
            continue;
        if (normal &&
            !req.drainQueued[static_cast<unsigned>(DrainType::Lu)] &&
            !enqueueDrain(req, DrainType::Lu, now))
            continue;
        if (active >= env.solveCount) {
            observe("trsm_context_full");
            if (normal && req.stage != RequestStage::SolveQueued &&
                queued < env.solvePendingDepth) {
                req.stage = RequestStage::SolveQueued;
                queued++;
            }
            req.record.stallSolvePending++;
            if (early)
                req.record.earlySolveMissed++;
            continue;
        }
        if (!env.luForwarding) {
            if (!req.luDrained) {
                req.stage = RequestStage::SolveQueued;
                req.record.stallDependency++;
                continue;
            }
            if (!req.luReloadReadyCycle) {
                const uint64_t chunks =
                    (MatrixBytes + env.spmWriteWidth - 1) /
                    env.spmWriteWidth;
                req.luReloadReadyCycle = now + env.sourceReadLatency +
                    chunks * env.spmWriteLatency;
            }
            if (now < req.luReloadReadyCycle) {
                req.stage = RequestStage::SolveQueued;
                req.record.stallDependency++;
                continue;
            }
        }
        if (req.stage == RequestStage::SolveQueued)
            queued--;
        initializeRhs(req);
        activateRhs(req);
        if (!early)
            req.stage = RequestStage::SolveActive;
        req.solveStarted = true;
        req.record.solveIssueCycle = now;
        if (early) {
            req.record.earlySolveIssued++;
            trace(req, "solve", "TRSM_EARLY_ISSUE");
        }
        active++;
        trace(req, "solve", "COEFF3_ISSUE");
    }
}

void
CfdCoeffPreprocessController::rhsDone(
    Request &req, unsigned rhs, uint64_t now)
{
    RhsState &state = req.rhs[rhs];
    state.done = true;
    state.active = false;
    req.rhsDoneMask |= 1u << rhs;
    req.rhsCompleted++;
    const unsigned batch = rhsBatch(req, rhs);
    const unsigned col = rhsColumn(req, rhs);
    auto &out = batch == 0 ? req.dInv : (batch == 1 ? req.lBar : req.uBar);
    for (unsigned row = 0; row < N; ++row)
        out[row * N + col] = state.value[row];
    for (double value : state.value) {
        if (!std::isfinite(value)) {
            markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                       "nonfinite-output");
            return;
        }
    }
    req.batchCompleted[batch]++;
    if (!req.record.firstColumnCycle)
        req.record.firstColumnCycle = now;
    if (batch == 0)
        req.record.dInvColumnReadyCycle[col] = now;
    else if (batch == 1)
        req.record.lBarColumnReadyCycle[col] = now;
    else
        req.record.uBarColumnReadyCycle[col] = now;
    if (batch == 0 && req.baseConsumerEnabled) {
        const uint8_t bit = 1u << col;
        if (req.dInvColumnReadyMask & bit) {
            req.record.dInvColumnsDuplicateRejected++;
            req.record.baseConsumerError = 1;
            markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                       "duplicate-dinv-column-ready");
            return;
        }
        req.dInvColumnReadyMask |= bit;
        req.record.dInvColumnsReady++;
        if (col != req.nextDInvColumnToConsume) {
            req.dInvColumnOutOfOrderCountedMask |= bit;
            req.record.dInvColumnsOutOfOrderHeld++;
        }
    }
    if (batch == 1 && req.lbarConsumerEnabled && req.desc.cellId) {
        const uint8_t bit = 1u << col;
        if (req.lbarColumnReadyMask & bit) {
            req.record.lbarColumnsDuplicateRejected++;
            req.record.forwardConsumerError = 1;
            markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                       "duplicate-lbar-column-ready");
            return;
        }
        req.lbarColumnReadyMask |= bit;
        req.record.lbarColumnsReady++;
        if (col != req.nextLbarColumnToConsume) {
            req.lbarColumnOutOfOrderCountedMask |= bit;
            req.record.lbarColumnsOutOfOrderHeld++;
        }
    }
    if (batchRequested(req, 0) && req.batchCompleted[0] == 5 &&
        !req.record.dInvReadyCycle) {
        req.record.dInvReadyCycle = now;
        trace(req, "solve", "DINV_READY", 0);
    }
    if (batchRequested(req, 1) && req.batchCompleted[1] == 5 &&
        !req.record.lBarReadyCycle) {
        req.record.lBarReadyCycle = now;
        trace(req, "solve", "LBAR_READY", 1);
    }
    if (batchRequested(req, 2) && req.batchCompleted[2] == 5 &&
        !req.record.uBarReadyCycle) {
        req.record.uBarReadyCycle = now;
        trace(req, "solve", "UBAR_READY", 2);
    }
    if (req.rhsCompleted == req.rhsCount) {
        req.record.solveCompleteCycle = now;
        req.solveDone = true;
        trace(req, "solve", "COEFF3_COMPLETE");
    }
    activateRhs(req);
}

void
CfdCoeffPreprocessController::completeColumnFma(
    uint64_t token, uint64_t generation, ColumnFmaTaskKind kind,
    unsigned column, const std::array<double, N> &matrixColumn,
    double scalar, uint64_t now)
{
    Request *req = find(token);
    if (!req)
        return;
    if (req->desc.generation != generation) {
        if (kind == ColumnFmaTaskKind::DInvBase) {
            req->record.dInvColumnsGenerationRejected++;
            req->record.baseAccumulatorStaleWrites++;
            req->record.baseConsumerError = 1;
        } else {
            req->record.lbarColumnsGenerationRejected++;
            req->record.correctionAccumulatorStaleWrites++;
            req->record.forwardConsumerError = 1;
        }
        return;
    }
    if (req->stage == RequestStage::Failed ||
        req->stage == RequestStage::Complete)
        return;
    const bool dInv = kind == ColumnFmaTaskKind::DInvBase;
    const bool valid = dInv ?
        (req->rhsVectorReady && req->baseAccumulatorInitialized &&
         column == req->nextDInvColumnToConsume) :
        (req->previousDqReady && req->correctionInitialized &&
         column == req->nextLbarColumnToConsume);
    if (column >= N || !valid) {
        if (dInv)
            req->record.baseConsumerError = 1;
        else
            req->record.forwardConsumerError = 1;
        markFailed(*req, CfdCoeffPreprocessStatus::InternalError,
                   "invalid-column-fma-completion");
        return;
    }
    const uint8_t bit = 1u << column;
    const bool duplicate = dInv ? (req->dInvColumnConsumedMask & bit) :
                                  (req->lbarColumnConsumedMask & bit);
    if (duplicate) {
        if (dInv) {
            req->record.dInvColumnsDuplicateRejected++;
            req->record.baseAccumulatorDoubleConsumes++;
            req->record.baseConsumerError = 1;
        } else {
            req->record.lbarColumnsDuplicateRejected++;
            req->record.correctionAccumulatorDoubleConsumes++;
            req->record.forwardConsumerError = 1;
        }
        markFailed(*req, CfdCoeffPreprocessStatus::InternalError,
                   "duplicate-column-fma-completion");
        return;
    }

    // Match Path A exactly: one left-to-right FP64 multiply-plus-add for each
    // row, with k committed in the strict 0,1,2,3,4 order.
    std::array<double, N> &accumulator = dInv ?
        req->baseAccumulator : req->lbarCorrectionAccumulator;
    for (unsigned row = 0; row < N; ++row)
        accumulator[row] += matrixColumn[row] * scalar;
    if (dInv) {
        req->dInvColumnConsumedMask |= bit;
        req->record.baseConsumedColumnMask = req->dInvColumnConsumedMask;
        req->record.dInvColumnsCompleted++;
        req->record.dInvColumnsConsumed++;
        req->nextDInvColumnToConsume++;
    } else {
        req->lbarColumnConsumedMask |= bit;
        req->record.lbarConsumedColumnMask = req->lbarColumnConsumedMask;
        req->record.lbarColumnsCompleted++;
        req->record.lbarColumnsConsumed++;
        req->nextLbarColumnToConsume++;
    }
    req->record.columnFmaCompletions++;
    trace(*req, "column-fma", "COLUMN_FMA_COMPLETE",
          dInv ? 0 : 1, column);
    // The progress record is control metadata, not the base payload.  Publish
    // each committed column so cancel/generation stress tests can observe a
    // genuinely partial in-order accumulator without making the 40 B base
    // visible before its modeled drain completes.
    if (req->desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE)
        proxy().writeBlob(req->desc.recordAddr, &req->record,
                          sizeof(req->record));
    if (dInv && req->nextDInvColumnToConsume == N) {
        req->baseComputeComplete = true;
        req->record.baseComputeReadyCycle = now;
        req->record.baseAccumulatorsCompleted = 1;
        trace(*req, "column-fma", "BASE_ACCUMULATOR_COMPLETE");
    } else if (!dInv && req->nextLbarColumnToConsume == N) {
        req->correctionComplete = true;
        req->record.correctionReadyCycle = now;
        req->record.correctionAccumulatorsCompleted = 1;
        trace(*req, "column-fma", "CORRECTION_ACCUMULATOR_COMPLETE");
    }
}

void
CfdCoeffPreprocessController::processColumnFma(uint64_t now)
{
    auto eligibleDInv = [](const Request &req) {
        if (!req.baseConsumerEnabled || !req.rhsVectorReady ||
            req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete ||
            req.nextDInvColumnToConsume >= N)
            return false;
        const uint8_t bit = 1u << req.nextDInvColumnToConsume;
        return (req.dInvColumnReadyMask & bit) &&
            !(req.dInvColumnQueuedMask & bit) &&
            !(req.dInvColumnIssuedMask & bit) &&
            !(req.dInvColumnConsumedMask & bit);
    };
    auto eligibleLbar = [](const Request &req) {
        if (!req.lbarConsumerEnabled || !req.desc.cellId ||
            !req.previousDqReady || req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete ||
            req.nextLbarColumnToConsume >= N)
            return false;
        const uint8_t bit = 1u << req.nextLbarColumnToConsume;
        return (req.lbarColumnReadyMask & bit) &&
            !(req.lbarColumnQueuedMask & bit) &&
            !(req.lbarColumnIssuedMask & bit) &&
            !(req.lbarColumnConsumedMask & bit);
    };

    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!req.lbarConsumerEnabled || !req.desc.cellId ||
            req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete || req.correctionComplete)
            continue;
        if (!req.previousDqReady) {
            req.record.lbarConsumerWaitPreviousDqCycles++;
            req.record.frontierWaitPreviousDqCycles++;
        } else if (!(req.lbarColumnReadyMask &
                     (1u << req.nextLbarColumnToConsume))) {
            req.record.lbarConsumerWaitColumnCycles++;
            req.record.frontierWaitLbarColumnsCycles++;
        }
    }

    while (columnFmaQueue.size() < env.columnFmaQueueDepth) {
        Request *selected = nullptr;
        Request *wrapped = nullptr;
        ColumnFmaTaskKind selectedKind = ColumnFmaTaskKind::DInvBase;
        // Advance the true forward dependency first.  Within each class the
        // request-id cursor provides round-robin fairness across cells.
        for (auto &[token, ptr] : requests) {
            Request &req = *ptr;
            if (!eligibleLbar(req))
                continue;
            if (!selected || req.desc.cellId < selected->desc.cellId)
                selected = &req;
        }
        if (selected)
            selectedKind = ColumnFmaTaskKind::LbarCorrection;
        for (auto &[token, ptr] : requests) {
            Request &req = *ptr;
            if (selected || !eligibleDInv(req))
                continue;
            if (!wrapped || req.record.requestId < wrapped->record.requestId)
                wrapped = &req;
            if (req.record.requestId > columnFmaCursor &&
                (!selected ||
                 req.record.requestId < selected->record.requestId))
                selected = &req;
        }
        if (!selected)
            selected = wrapped;
        if (!selected)
            break;
        const bool dInv = selectedKind == ColumnFmaTaskKind::DInvBase;
        const unsigned column = dInv ? selected->nextDInvColumnToConsume :
                                       selected->nextLbarColumnToConsume;
        const uint8_t bit = 1u << column;
        ColumnFmaTask task;
        task.kind = selectedKind;
        task.token = selected->token;
        task.generation = selected->desc.generation;
        task.lineId = selected->desc.lineId;
        task.cellId = selected->desc.cellId;
        task.column = column;
        const auto &matrix = dInv ? selected->dInv : selected->lBar;
        for (unsigned row = 0; row < N; ++row)
            task.matrixColumn[row] = matrix[row * N + column];
        task.scalar = dInv ? selected->rhsVector[column] :
                             selected->previousDq[column];
        columnFmaQueue.push_back(task);
        if (dInv) {
            selected->dInvColumnQueuedMask |= bit;
            selected->record.dInvColumnsQueued++;
        } else {
            selected->lbarColumnQueuedMask |= bit;
            selected->record.lbarColumnsQueued++;
        }
        selected->record.columnFmaQueueMaxOccupancy = std::max<uint64_t>(
            selected->record.columnFmaQueueMaxOccupancy,
            columnFmaQueue.size());
        columnFmaCursor = selected->record.requestId;
        trace(*selected, "column-fma", "COLUMN_FMA_QUEUE",
              dInv ? 0 : 1, column);
    }

    if (columnFmaQueue.size() >= env.columnFmaQueueDepth) {
        for (auto &[token, ptr] : requests) {
            if (eligibleDInv(*ptr) || eligibleLbar(*ptr)) {
                ptr->record.columnFmaQueueFullStalls++;
                ptr->record.columnFmaRetries++;
                if (eligibleLbar(*ptr)) {
                    ptr->record.lbarConsumerReadyButBlockedCycles++;
                    ptr->record.frontierWaitConsumerEngineCycles++;
                }
                break;
            }
        }
    }

    while (!columnFmaQueue.empty()) {
        const ColumnFmaTask task = columnFmaQueue.front();
        Request *req = find(task.token);
        if (!req || req->stage == RequestStage::Failed ||
            req->desc.generation != task.generation) {
            columnFmaQueue.pop_front();
            continue;
        }
        const bool accepted = issue(columnFma, OpKind::ColumnFma, now,
            [this, task] {
                completeColumnFma(task.token, task.generation,
                    task.kind, task.column, task.matrixColumn, task.scalar,
                    cycle());
            });
        if (!accepted) {
            req->record.columnFmaReadyButBlockedCycles++;
            req->record.columnFmaRetries++;
            if (task.kind == ColumnFmaTaskKind::LbarCorrection) {
                req->record.lbarConsumerReadyButBlockedCycles++;
                req->record.lbarConsumerWaitEngineCycles++;
                req->record.frontierWaitConsumerEngineCycles++;
            }
            break;
        }
        const uint8_t bit = 1u << task.column;
        if (task.kind == ColumnFmaTaskKind::DInvBase) {
            req->dInvColumnQueuedMask &= ~bit;
            req->dInvColumnIssuedMask |= bit;
            req->record.dInvColumnsIssued++;
        } else {
            req->lbarColumnQueuedMask &= ~bit;
            req->lbarColumnIssuedMask |= bit;
            req->record.lbarColumnsIssued++;
            if (!req->firstLbarConsumerIssueCycle)
                req->firstLbarConsumerIssueCycle = now;
        }
        req->record.columnFmaIssueCycles++;
        trace(*req, "column-fma", "COLUMN_FMA_ISSUE",
              task.kind == ColumnFmaTaskKind::DInvBase ? 0 : 1,
              task.column);
        columnFmaQueue.pop_front();
    }

    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!req.baseConsumerEnabled || !req.baseComputeComplete ||
            req.directLbarBypass ||
            req.basePublishComplete ||
            req.drainQueued[static_cast<unsigned>(DrainType::BaseVector)])
            continue;
        if (!enqueueDrain(req, DrainType::BaseVector, now))
            req.record.baseVectorPublishStalls++;
    }
}

void
CfdCoeffPreprocessController::produceForwardHandoff(
    Request &req, uint64_t now)
{
    if (req.handoffProduced)
        return;
    auto &handoff = forwardHandoffs[req.desc.lineId];
    if (handoff.valid && !handoff.consumed) {
        req.record.forwardHandoffOverwriteErrors++;
        req.record.forwardConsumerError = 1;
        markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                   "forward-handoff-overwrite");
        return;
    }
    handoff = {};
    handoff.valid = true;
    handoff.lineId = req.desc.lineId;
    handoff.producerCell = req.desc.cellId;
    handoff.producerGeneration =
        req.desc.flags & CFD_COEFF_PRE_TEST_HANDOFF_WRONG_GENERATION ?
        std::numeric_limits<uint64_t>::max() : req.desc.generation;
    handoff.expectedConsumerCell = req.desc.cellId +
        (req.desc.flags & CFD_COEFF_PRE_TEST_HANDOFF_WRONG_CELL ? 2 : 1);
    handoff.value = req.dqStar;
    handoff.readyCycle = now;
    req.handoffProduced = true;
    req.record.forwardHandoffReadyCycle = now;
    req.record.forwardHandoffsProduced++;
    req.record.forwardHandoffMaxLive = 1;
    trace(req, "forward-handoff", "HANDOFF_PRODUCE");
}

void
CfdCoeffPreprocessController::processForwardHandoffs(uint64_t now)
{
    for (auto it = forwardHandoffs.begin(); it != forwardHandoffs.end();) {
        ForwardDqHandoff &handoff = it->second;
        if (!handoff.valid || handoff.consumed) {
            it = forwardHandoffs.erase(it);
            continue;
        }
        Request *consumer = nullptr;
        for (auto &[token, ptr] : requests) {
            if (ptr->desc.lineId == handoff.lineId &&
                ptr->desc.cellId == handoff.expectedConsumerCell &&
                ptr->lbarConsumerEnabled &&
                ptr->stage != RequestStage::Failed &&
                ptr->stage != RequestStage::Complete) {
                consumer = ptr.get();
                break;
            }
        }
        if (!consumer) {
            if (Request *producer = [&]() -> Request * {
                    for (auto &[token, ptr] : requests)
                        if (ptr->desc.lineId == handoff.lineId &&
                            ptr->desc.cellId == handoff.producerCell &&
                            ptr->desc.generation ==
                                handoff.producerGeneration)
                            return ptr.get();
                    return nullptr;
                }())
                producer->record.forwardHandoffWaitCycles++;
            ++it;
            continue;
        }
        if (consumer->desc.cellId != handoff.producerCell + 1) {
            consumer->record.forwardHandoffWrongCellErrors++;
            consumer->record.forwardConsumerError = 1;
            markFailed(*consumer, CfdCoeffPreprocessStatus::InternalError,
                       "forward-handoff-wrong-cell");
            it = forwardHandoffs.erase(it);
            continue;
        }
        if (consumer->desc.generation <= handoff.producerGeneration) {
            consumer->record.forwardHandoffWrongGenerationErrors++;
            consumer->record.forwardConsumerError = 1;
            markFailed(*consumer,
                       CfdCoeffPreprocessStatus::GenerationMismatch,
                       "forward-handoff-wrong-generation");
            it = forwardHandoffs.erase(it);
            continue;
        }
        if (consumer->previousDqReady) {
            consumer->record.forwardHandoffDoubleConsumes++;
            consumer->record.forwardConsumerError = 1;
            markFailed(*consumer, CfdCoeffPreprocessStatus::InternalError,
                       "forward-handoff-double-consume");
            it = forwardHandoffs.erase(it);
            continue;
        }
        consumer->previousDq = handoff.value;
        consumer->previousDqReady = true;
        consumer->previousDqReadyCycle = now;
        consumer->record.forwardHandoffConsumedCycle = now;
        consumer->record.forwardHandoffsConsumed++;
        consumer->record.forwardHandoffWaitCycles +=
            now - handoff.readyCycle;
        handoff.consumerAttached = true;
        handoff.consumed = true;
        trace(*consumer, "forward-handoff", "HANDOFF_CONSUME");
        it = forwardHandoffs.erase(it);
    }
}

void
CfdCoeffPreprocessController::completeForwardCombine(
    uint64_t token, uint64_t generation, uint64_t now)
{
    Request *req = find(token);
    if (!req || req->stage == RequestStage::Failed ||
        req->stage == RequestStage::Complete)
        return;
    if (req->desc.generation != generation || !req->baseComputeComplete ||
        !req->correctionComplete || req->combineComplete) {
        req->record.forwardConsumerError = 1;
        markFailed(*req, CfdCoeffPreprocessStatus::InternalError,
                   "invalid-forward-combine");
        return;
    }
    for (unsigned lane = 0; lane < N; ++lane)
        req->dqStar[lane] = req->baseAccumulator[lane] -
                            req->lbarCorrectionAccumulator[lane];
    req->combineComplete = true;
    req->dqStarReadyLocal = true;
    req->record.forwardCombineCompleteCycle = now;
    req->record.forwardCombineCompleted = 1;
    req->record.dqStarReadyCycle = now;
    produceForwardHandoff(*req, now);
    trace(*req, "forward-combine", "FORWARD_COMBINE_COMPLETE");
}

void
CfdCoeffPreprocessController::processForwardCombines(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!req.lbarConsumerEnabled || req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete)
            continue;
        if (!req.desc.cellId && req.baseComputeComplete &&
            !req.dqStarReadyLocal) {
            req.dqStar = req.baseAccumulator;
            req.dqStarReadyLocal = true;
            req.record.dqStarReadyCycle = now;
            produceForwardHandoff(req, now);
        }
        if (req.desc.cellId && req.baseComputeComplete &&
            req.correctionComplete && !req.combineQueued &&
            !req.combineIssued && !req.combineComplete) {
            if (forwardCombineQueue.size() >=
                env.forwardCombineQueueDepth) {
                req.record.forwardCombineQueueFullStalls++;
                req.record.frontierWaitConsumerEngineCycles++;
                continue;
            }
            forwardCombineQueue.push_back({req.token, req.desc.generation,
                req.desc.lineId, req.desc.cellId});
            req.combineQueued = true;
            req.record.forwardCombineQueued = 1;
            req.record.forwardCombineQueueMaxOccupancy =
                std::max<uint64_t>(
                    req.record.forwardCombineQueueMaxOccupancy,
                    forwardCombineQueue.size());
        }
    }
    while (!forwardCombineQueue.empty()) {
        const ForwardCombineTask task = forwardCombineQueue.front();
        Request *req = find(task.token);
        if (!req || req->stage == RequestStage::Failed ||
            req->desc.generation != task.generation) {
            forwardCombineQueue.pop_front();
            continue;
        }
        if (!issue(forwardCombine, OpKind::ForwardCombine, now,
                   [this, task] {
                       completeForwardCombine(task.token, task.generation,
                                              cycle());
                   })) {
            req->record.forwardCombineReadyButBlockedCycles++;
            req->record.frontierWaitConsumerEngineCycles++;
            break;
        }
        req->combineQueued = false;
        req->combineIssued = true;
        req->record.forwardCombineIssued = 1;
        req->record.forwardCombineIssueCycle = now;
        trace(*req, "forward-combine", "FORWARD_COMBINE_ISSUE");
        forwardCombineQueue.pop_front();
    }

    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!req.lbarConsumerEnabled || req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete)
            continue;
        if (req.dqStarReadyLocal && !req.dqStarPublishComplete)
            req.record.frontierWaitDqStarPublishCycles++;
        if (req.dqStarReadyLocal && !req.dqStarPublishComplete &&
            (req.desc.flags & CFD_COEFF_PRE_TEST_HOLD_DQSTAR_PUBLISH)) {
            // Test-only cancellation window: expose the internal phase in
            // the stable record while deliberately withholding publication.
            proxy().writeBlob(req.desc.recordAddr, &req.record,
                              sizeof(req.record));
            continue;
        }
        if (!req.directLbarBypass && req.desc.cellId &&
            req.correctionComplete &&
            !req.drainQueued[static_cast<unsigned>(
                DrainType::CorrectionVector)]) {
            if (!enqueueDrain(req, DrainType::CorrectionVector, now))
                req.record.dqStarPublishStalls++;
        }
        if (req.dqStarReadyLocal && !req.dqStarPublishComplete &&
            !req.drainQueued[static_cast<unsigned>(
                DrainType::DqStarVector)]) {
            if (!enqueueDrain(req, DrainType::DqStarVector, now))
                req.record.dqStarPublishStalls++;
        }
    }
}

void
CfdCoeffPreprocessController::processSolve(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!req.solveStarted)
            continue;
        std::array<unsigned, 15> order = {};
        unsigned orderCount = 0;
        if (!req.solveDone) {
            req.record.schedulerCycles++;
            req.record.schedulerScans += req.rhsCount;
            uint16_t scanMask = req.rhsEnabledMask &
                req.rhsActivatedMask & ~req.rhsDoneMask;
            while (scanMask) {
                unsigned index = 0;
                while (!(scanMask & (1u << index)))
                    ++index;
                scanMask &= ~(1u << index);
                req.record.rhsMaskSchedulerScans++;
                RhsState &rhs = req.rhs[index];
                if (!rhs.active || rhs.done)
                    continue;
                if (rhs.waiting) {
                    observe("rhs_inflight_dependency");
                    req.record.stallDependency++;
                    continue;
                }
                bool luReady = true;
                if (rhs.phase == RhsPhase::ForwardDiv ||
                    rhs.phase == RhsPhase::ForwardMul ||
                    rhs.phase == RhsPhase::LastDiv)
                    luReady = req.forwardStepReady[rhs.k];
                else if (rhs.phase == RhsPhase::BackwardMul)
                    luReady = req.backwardStepReady[rhs.k];
                if (!luReady) {
                    observe("rhs_lu_step_dependency");
                    req.record.trsmBlockedByLuStepCycles++;
                    req.record.earlySolveMissed++;
                    trace(req, "solve", "TRSM_WAIT_LU_STEP",
                          index / 5, index % 5, rhs.k);
                    continue;
                }
                if (!rhs.readySinceCycle)
                    rhs.readySinceCycle = now;
                const uint64_t wait = now - rhs.readySinceCycle;
                if (wait > 64)
                    req.record.rhsStarvationCycles++;
                order[orderCount++] = index;
                observe("rhs_candidate");
                if (rhs.phase == RhsPhase::ForwardDiv ||
                    rhs.phase == RhsPhase::LastDiv)
                    req.record.dividerReadyCandidates++;
                else if (rhs.phase != RhsPhase::Done)
                    req.record.mulSubReadyCandidates++;
            }
            req.record.readyRhsCountSum += orderCount;
            req.record.readyRhsCountMax = std::max<uint64_t>(
                req.record.readyRhsCountMax, orderCount);
            if (!orderCount) {
                req.record.cyclesWithNoReadyRhs++;
                req.record.schedulerNoSelection++;
                req.record.rhsMaskEmptyScans++;
                trace(req, "scheduler", "SCHED_NO_READY");
            } else {
                trace(req, "scheduler", "SCHED_SCAN");
            }
        }

        auto phaseRank = [](RhsPhase phase) {
            switch (phase) {
              case RhsPhase::ForwardDiv:
              case RhsPhase::LastDiv: return 0;
              case RhsPhase::ForwardMul:
              case RhsPhase::BackwardMul: return 1;
              case RhsPhase::ForwardSub:
              case RhsPhase::BackwardSub: return 2;
              case RhsPhase::Done: return 3;
            }
            return 4;
        };
        if (env.rhsSchedule == RhsSchedule::StepMajor) {
            std::stable_sort(order.begin(), order.begin() + orderCount,
                [&req, &phaseRank](unsigned a, unsigned b) {
                    const RhsState &lhs = req.rhs[a];
                    const RhsState &rhs = req.rhs[b];
                    if (lhs.k != rhs.k)
                        return lhs.k < rhs.k;
                    if (phaseRank(lhs.phase) != phaseRank(rhs.phase))
                        return phaseRank(lhs.phase) < phaseRank(rhs.phase);
                    return a < b;
                });
        } else if (env.rhsSchedule == RhsSchedule::RoundRobinReady) {
            std::stable_sort(order.begin(), order.begin() + orderCount,
                [&req](unsigned a, unsigned b) {
                    const unsigned count = std::max(1u, req.rhsSlots);
                    const unsigned da =
                        (a + count - req.schedulerCursor) % count;
                    const unsigned db =
                        (b + count - req.schedulerCursor) % count;
                    return da < db;
                });
        } else if (env.rhsSchedule == RhsSchedule::ReadyFirst) {
            std::stable_sort(order.begin(), order.begin() + orderCount,
                [&req, &phaseRank](unsigned a, unsigned b) {
                    const RhsState &lhs = req.rhs[a];
                    const RhsState &rhs = req.rhs[b];
                    if (phaseRank(lhs.phase) != phaseRank(rhs.phase))
                        return phaseRank(lhs.phase) < phaseRank(rhs.phase);
                    if (lhs.readySinceCycle != rhs.readySinceCycle)
                        return lhs.readySinceCycle < rhs.readySinceCycle;
                    return a < b;
                });
        } else if (env.rhsSchedule == RhsSchedule::FrontierAware) {
            std::stable_sort(order.begin(), order.begin() + orderCount,
                [&req, &phaseRank, now](unsigned a, unsigned b) {
                    const RhsState &lhs = req.rhs[a];
                    const RhsState &rhs = req.rhs[b];
                    const bool lhsAged = lhs.readySinceCycle &&
                        now - lhs.readySinceCycle > 64;
                    const bool rhsAged = rhs.readySinceCycle &&
                        now - rhs.readySinceCycle > 64;
                    if (lhsAged != rhsAged)
                        return lhsAged;
                    // DInv and Lbar unblock the forward frontier; Ubar is
                    // retained as background work until aging promotes it.
                    const unsigned lhsBatch = a / N;
                    const unsigned rhsBatch = b / N;
                    const unsigned lhsPriority = lhsBatch < 2 ? lhsBatch : 2;
                    const unsigned rhsPriority = rhsBatch < 2 ? rhsBatch : 2;
                    if (lhsPriority != rhsPriority)
                        return lhsPriority < rhsPriority;
                    if (phaseRank(lhs.phase) != phaseRank(rhs.phase))
                        return phaseRank(lhs.phase) < phaseRank(rhs.phase);
                    if (lhs.readySinceCycle != rhs.readySinceCycle)
                        return lhs.readySinceCycle < rhs.readySinceCycle;
                    return a < b;
                });
        }

        bool anySelection = false;
        for (unsigned orderIndex = 0; orderIndex < orderCount; ++orderIndex) {
            const unsigned index = order[orderIndex];
            RhsState &rhs = req.rhs[index];
            if (!rhs.active || rhs.done)
                continue;
            const uint64_t tok = token;
            const int k = rhs.k, i = rhs.i;
            const bool consumesLuStep =
                rhs.phase == RhsPhase::ForwardDiv ||
                rhs.phase == RhsPhase::ForwardMul ||
                rhs.phase == RhsPhase::LastDiv ||
                rhs.phase == RhsPhase::BackwardMul;
            bool selected = false;
            switch (rhs.phase) {
              case RhsPhase::ForwardDiv:
              case RhsPhase::LastDiv:
                if (env.reciprocalSolve && req.reciprocalPending[k]) {
                    observe("rhs_reciprocal_dependency");
                    req.record.stallDependency++;
                    break;
                }
                rhs.waiting = true;
                if (env.reciprocalSolve && req.reciprocalReady[k]) {
                    if (!issue(coeffMul, OpKind::Mul, now,
                        [this, tok, index, k] {
                            Request *r = find(tok);
                            if (!r) return;
                            RhsState &s = r->rhs[index];
                            volatile double v =
                                s.value[k] * r->reciprocal[k];
                            s.value[k] = v;
                            s.waiting = false;
                            if (k == 4) {
                                s.k = 3;
                                s.i = 4;
                                s.phase = RhsPhase::BackwardMul;
                            } else {
                                s.i = k + 1;
                                s.phase = RhsPhase::ForwardMul;
                            }
                        })) {
                        rhs.waiting = false;
                        req.record.stallCoeffMulSub++;
                    } else {
                        req.record.coeffMulIssued++;
                        selected = true;
                        trace(req, "solve", "RECIP_MUL_ISSUE", index / 5,
                              index % 5, k);
                    }
                } else if (!issue(coeffDiv, OpKind::Div, now,
                    [this, tok, index, k] {
                        Request *r = find(tok);
                        if (!r) return;
                        RhsState &s = r->rhs[index];
                        if (env.reciprocalSolve) {
                            volatile double reciprocal =
                                1.0 / r->lu[k * N + k];
                            r->reciprocal[k] = reciprocal;
                            r->reciprocalReady[k] = true;
                            r->reciprocalPending[k] = false;
                            s.waiting = false;
                            return;
                        }
                        volatile double v = s.value[k] / r->lu[k * N + k];
                        s.value[k] = v;
                        s.waiting = false;
                        if (k == 4) {
                            s.k = 3;
                            s.i = 4;
                            s.phase = RhsPhase::BackwardMul;
                        } else {
                            s.i = k + 1;
                            s.phase = RhsPhase::ForwardMul;
                        }
                    })) {
                    rhs.waiting = false;
                    req.record.stallCoeffDivider++;
                } else {
                    if (env.reciprocalSolve)
                        req.reciprocalPending[k] = true;
                    req.record.coeffDivIssued++;
                    selected = true;
                    trace(req, "solve",
                          env.reciprocalSolve ? "RECIP_DIV_ISSUE" :
                                                "DIV_ISSUE",
                          index / 5, index % 5, k);
                }
                break;
              case RhsPhase::ForwardMul:
              case RhsPhase::BackwardMul:
              {
                const bool backward = rhs.phase == RhsPhase::BackwardMul;
                rhs.waiting = true;
                if (!issue(coeffMul, OpKind::Mul, now,
                    [this, tok, index, k, i, backward] {
                        Request *r = find(tok);
                        if (!r) return;
                        RhsState &s = r->rhs[index];
                        volatile double p = backward ?
                            r->lu[k * N + i] * s.value[i] :
                            r->lu[i * N + k] * s.value[k];
                        s.product = p;
                        s.waiting = false;
                        s.phase = backward ? RhsPhase::BackwardSub :
                                             RhsPhase::ForwardSub;
                    })) {
                    rhs.waiting = false;
                    req.record.stallCoeffMulSub++;
                } else {
                    req.record.coeffMulIssued++;
                    selected = true;
                }
                break;
              }
              case RhsPhase::ForwardSub:
              case RhsPhase::BackwardSub:
              {
                const bool backward = rhs.phase == RhsPhase::BackwardSub;
                rhs.waiting = true;
                if (!issue(coeffSub, OpKind::Sub, now,
                    [this, tok, index, k, i, backward] {
                        Request *r = find(tok);
                        if (!r) return;
                        RhsState &s = r->rhs[index];
                        volatile double v = backward ?
                            s.value[k] - s.product :
                            s.value[i] - s.product;
                        if (backward)
                            s.value[k] = v;
                        else
                            s.value[i] = v;
                        s.waiting = false;
                        if (!backward) {
                            s.i++;
                            if (s.i < 5) {
                                s.phase = RhsPhase::ForwardMul;
                            } else {
                                s.k++;
                                s.phase = s.k == 4 ? RhsPhase::LastDiv :
                                                     RhsPhase::ForwardDiv;
                            }
                        } else {
                            s.i++;
                            if (s.i < 5) {
                                s.phase = RhsPhase::BackwardMul;
                            } else if (s.k == 0) {
                                s.phase = RhsPhase::Done;
                            } else {
                                s.k--;
                                s.i = s.k + 1;
                                s.phase = RhsPhase::BackwardMul;
                            }
                        }
                    })) {
                    rhs.waiting = false;
                    req.record.stallCoeffMulSub++;
                } else {
                    req.record.coeffSubIssued++;
                    selected = true;
                }
                break;
              }
              case RhsPhase::Done:
                rhsDone(req, index, now);
                selected = true;
                break;
            }
            if (selected) {
                observe("rhs_selected");
                if (consumesLuStep && !req.luStepUsed[k]) {
                    req.luStepUsed[k] = true;
                    req.record.luStepFirstUseLatency[k] =
                        now - req.record.luStepReadyCycle[k];
                }
                const uint64_t wait = rhs.readySinceCycle ?
                    now - rhs.readySinceCycle : 0;
                req.record.rhsWaitCycles += wait;
                req.record.rhsMaxWaitCycles = std::max(
                    req.record.rhsMaxWaitCycles, wait);
                const unsigned batch = rhsBatch(req, index);
                if (batch == 0)
                    req.record.identityBatchWaitCycles += wait;
                else if (batch == 1)
                    req.record.lBarBatchWaitCycles += wait;
                else
                    req.record.uBarBatchWaitCycles += wait;
                rhs.readySinceCycle = 0;
                req.schedulerCursor = (index + 1) % req.rhsSlots;
                req.record.schedulerSelections++;
                req.record.rhsMaskUsefulIssues++;
                anySelection = true;
                trace(req, "scheduler", "SCHED_SELECT", index / 5,
                      index % 5, k, i);
            }
            if (!selected)
                observe("rhs_candidate_not_selected");
            if (req.stage == RequestStage::Failed) {
                if (env.resourceStats)
                    diagnostics["rhs_aborted_unvisited"] +=
                        orderCount - orderIndex - 1;
                break;
            }
        }

        if (orderCount && !anySelection) {
            req.record.schedulerNoSelection++;
            trace(req, "scheduler", "SCHED_NO_SELECTION");
        }

        if (req.stage == RequestStage::Failed)
            continue;

        if (req.luDone &&
            !req.drainQueued[static_cast<unsigned>(DrainType::Lu)])
            enqueueDrain(req, DrainType::Lu, now);
        const bool partial = env.partialOutput ||
            (req.desc.flags & CFD_COEFF_PRE_PARTIAL_OUTPUT);
        if ((req.matrixDrainMask & 1u) && req.record.dInvReadyCycle &&
            !req.drainQueued[static_cast<unsigned>(DrainType::DInv)] &&
            (partial || req.solveDone))
            enqueueDrain(req, DrainType::DInv, now);
        if ((req.matrixDrainMask & 2u) && req.record.lBarReadyCycle &&
            !req.drainQueued[static_cast<unsigned>(DrainType::LBar)] &&
            (partial || req.solveDone))
            enqueueDrain(req, DrainType::LBar, now);
        if (batchRequested(req, 2) && req.record.uBarReadyCycle &&
            !req.drainQueued[static_cast<unsigned>(DrainType::UBar)])
            enqueueDrain(req, DrainType::UBar, now);
        if (req.solveDone)
            req.stage = RequestStage::DrainPending;
    }
}

bool
CfdCoeffPreprocessController::enqueueDrain(
    Request &req, DrainType type, uint64_t now)
{
    const unsigned idx = static_cast<unsigned>(type);
    if (req.drainQueued[idx])
        return true;
    if (drainQueue.size() + activeDrains.size() >= env.outputDepth) {
        observe("output_ring_full");
        req.record.stallOutputRing++;
        trace(req, "drain", "STALL_OUTPUT_RING");
        return false;
    }
    if (drainQueue.size() >= env.drainQueueDepth) {
        observe("drain_queue_full");
        req.record.stallDrainQueue++;
        trace(req, "drain", "STALL_DRAIN_QUEUE");
        return false;
    }
    drainQueue.push_back({req.token, type});
    req.drainQueued[idx] = true;
    req.record.drainRequests++;
    req.record.maxOutputOccupancy = std::max<uint64_t>(
        req.record.maxOutputOccupancy,
        drainQueue.size() + activeDrains.size());
    return true;
}

void
CfdCoeffPreprocessController::finishDrain(DrainTask &task, uint64_t now)
{
    Request *req = find(task.token);
    if (!req)
        return;
    const void *src = nullptr;
    Addr dst = 0;
    switch (task.type) {
      case DrainType::Lu: src = req->lu.data(); dst = req->desc.luOutAddr; break;
      case DrainType::DInv: src = req->dInv.data(); dst = req->desc.dInvOutAddr; break;
      case DrainType::LBar: src = req->lBar.data(); dst = req->desc.lBarOutAddr; break;
      case DrainType::UBar: src = req->uBar.data(); dst = req->desc.uBarOutAddr; break;
      case DrainType::BaseVector:
        src = req->baseAccumulator.data();
        dst = req->desc.reserved[1];
        break;
      case DrainType::CorrectionVector:
        src = req->lbarCorrectionAccumulator.data();
        dst = req->desc.reserved[2];
        break;
      case DrainType::DqStarVector:
        src = req->dqStar.data();
        dst = req->desc.reserved[3];
        break;
    }
    const unsigned outputBytes = drainBytes(task.type);
    if (!env.dmaSpm)
        proxy().writeBlob(dst, src, outputBytes);
    const unsigned idx = static_cast<unsigned>(task.type);
    req->drainDone[idx] = true;
    req->record.outputBytes += outputBytes;
    if (task.type == DrainType::Lu) {
        req->record.luDrainDone = now;
        req->luDrained = true;
        trace(*req, "drain", "DRAIN_LU_DONE");
    } else if (task.type == DrainType::DInv) {
        req->record.dInvDrainDone = now;
        trace(*req, "drain", "DRAIN_DINV_DONE");
    } else if (task.type == DrainType::LBar) {
        req->record.lBarDrainDone = now;
        trace(*req, "drain", "DRAIN_LBAR_DONE");
    } else if (task.type == DrainType::UBar) {
        req->record.uBarDrainDone = now;
        trace(*req, "drain", "DRAIN_UBAR_DONE");
    } else if (task.type == DrainType::BaseVector) {
        req->basePublishComplete = true;
        req->record.baseReady = 1;
        req->record.baseReadyCycle = now;
        req->record.baseVectorPublishCount = 1;
        req->record.baseVectorPublishBytes = VectorBytes;
        trace(*req, "drain", "DRAIN_BASE_VECTOR_DONE");
    } else if (task.type == DrainType::CorrectionVector) {
        trace(*req, "drain", "DRAIN_CORRECTION_VECTOR_DONE");
    } else {
        req->dqStarPublishComplete = true;
        req->record.dqStarReady = 1;
        req->record.dqStarPublishCycle = now;
        req->record.dqStarPublishCount = 1;
        req->record.dqStarPublishBytes = VectorBytes;
        trace(*req, "drain", "DRAIN_DQSTAR_VECTOR_DONE");
    }
    publishProgress(*req);
}

void
CfdCoeffPreprocessController::publishProgress(Request &req)
{
    if (!(req.desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE) ||
        req.stage == RequestStage::Complete ||
        req.stage == RequestStage::Failed)
        return;
    const bool dInv = req.drainDone[
        static_cast<unsigned>(DrainType::DInv)];
    const bool lBar = req.drainDone[
        static_cast<unsigned>(DrainType::LBar)];
    const bool uBar = req.drainDone[
        static_cast<unsigned>(DrainType::UBar)];
    CfdCoeffPreprocessStatus progress = CfdCoeffPreprocessStatus::Busy;
    if (dInv && lBar && uBar)
        progress = CfdCoeffPreprocessStatus::DInvLBarUBarReady;
    else if (dInv && lBar)
        progress = CfdCoeffPreprocessStatus::DInvLBarReady;
    else if (dInv)
        progress = CfdCoeffPreprocessStatus::DInvReady;
    if (progress == CfdCoeffPreprocessStatus::Busy &&
        !req.record.baseReady && !req.record.dqStarReady)
        return;
    if (progress != CfdCoeffPreprocessStatus::Busy)
        req.record.status = static_cast<uint64_t>(progress);
    // Coefficients are functionally written above.  The record publication is
    // therefore the release point observed by the guest's ordinary loads.
    proxy().writeBlob(req.desc.recordAddr, &req.record, sizeof(req.record));
}

void
CfdCoeffPreprocessController::processDrains(uint64_t now)
{
    while (!drainQueue.empty() &&
           activeDrains.size() < env.drainOutstanding) {
        activeDrains.push_back(drainQueue.front());
        drainQueue.pop_front();
    }
    for (auto &task : activeDrains) {
        Request *req = find(task.token);
        if (!req)
            continue;
        if (task.finishCycle) {
            if (task.finishCycle <= now)
                finishDrain(task, now);
            continue;
        }
        if (!task.started) {
            task.started = true;
            if (task.type == DrainType::Lu)
                req->record.luDrainStart = now;
            else if (task.type == DrainType::DInv)
                req->record.dInvDrainStart = now;
            else if (task.type == DrainType::LBar)
                req->record.lBarDrainStart = now;
            else if (task.type == DrainType::UBar)
                req->record.uBarDrainStart = now;
            else if (task.type == DrainType::BaseVector)
                req->record.basePublishIssueCycle = now;
            else if (task.type == DrainType::DqStarVector)
                req->record.dqStarPublishCycle = now;
            trace(*req, "drain", "DRAIN_START");
        }
        if (env.packetSpm || env.dmaSpm) {
            const unsigned totalBytes = drainBytes(task.type);
            if (task.bytesIssued >= totalBytes)
                continue;
            CfdLocalSpm *spm = getCfdLocalSpm();
            if (!spm) {
                markFailed(*req, CfdCoeffPreprocessStatus::InternalError,
                           "missing-local-spm");
                continue;
            }
            unsigned bytes = std::min<unsigned>(
                env.drainWidth, totalBytes - task.bytesIssued);
            if (env.spmLayout == SpmLayout::RowStriped &&
                task.type != DrainType::BaseVector &&
                task.type != DrainType::CorrectionVector &&
                task.type != DrainType::DqStarVector)
                bytes = std::min<unsigned>(bytes,
                    N * sizeof(double) -
                    task.bytesIssued % (N * sizeof(double)));
            const CfdSpmMatrix matrix = drainMatrix(task.type);
            const unsigned legacyOffset =
                static_cast<unsigned>(task.type) * 0x100u +
                task.bytesIssued;
            Addr addr = packetAddress(*req, matrix, task.bytesIssued,
                                      legacyOffset, false);
            uint8_t *data = nullptr;
            if (env.dmaSpm) {
                const void *src = nullptr;
                Addr dst = 0;
                switch (task.type) {
                  case DrainType::Lu:
                    src = req->lu.data();
                    dst = req->desc.luOutAddr;
                    break;
                  case DrainType::DInv:
                    src = req->dInv.data();
                    dst = req->desc.dInvOutAddr;
                    break;
                  case DrainType::LBar:
                    src = req->lBar.data();
                    dst = req->desc.lBarOutAddr;
                    break;
                  case DrainType::UBar:
                    src = req->uBar.data();
                    dst = req->desc.uBarOutAddr;
                    break;
                  case DrainType::BaseVector:
                    src = req->baseAccumulator.data();
                    dst = req->desc.reserved[1];
                    break;
                  case DrainType::CorrectionVector:
                    src = req->lbarCorrectionAccumulator.data();
                    dst = req->desc.reserved[2];
                    break;
                  case DrainType::DqStarVector:
                    src = req->dqStar.data();
                    dst = req->desc.reserved[3];
                    break;
                }
                addr = dst + task.bytesIssued;
                data = const_cast<uint8_t *>(
                    reinterpret_cast<const uint8_t *>(src)) +
                    task.bytesIssued;
                bytes = spm->dmaChunkSize(tc, addr, bytes);
            }
            const uint64_t tok = task.token;
            const DrainType type = task.type;
            auto packetMeta = std::make_shared<CfdSpmPacketIssue>();
            auto completion =
                [this, tok, type, bytes, addr, matrix, packetMeta](
                    uint64_t packetId) {
                    completeDrainPacket(tok, type, bytes, packetId, addr,
                        packetMeta->bank, packetMeta->port, matrix);
                };
            CfdSpmPacketIssue packet = env.dmaSpm ?
                spm->issueDma(tc, true, addr, bytes, data,
                    env.spmOutstanding, std::move(completion)) :
                spm->issuePacket(
                    CfdSpmRequester::CoeffDrain, false, addr, bytes,
                    env.drainLatency, env.drainPorts, env.spmOutstanding,
                    env.spmBanks, env.spmBankGranularity, matrix,
                    std::move(completion));
            if (!packet.accepted) {
                req->record.coeffSpmOutstandingStalls++;
                trace(*req, "spm", "SPM_PACKET_STALL_OUTSTANDING", -1,
                      -1, -1, -1, "read", "CoeffDrain", 0, addr, bytes);
                continue;
            }
            *packetMeta = packet;
            task.bytesIssued += bytes;
            req->record.coeffSpmPacketIssued++;
            req->record.coeffDrainPacketBytes += bytes;
            req->record.coeffSpmReadPortStalls += packet.portWaitCycles;
            req->record.coeffSpmBankStalls += packet.bankWaitCycles;
            trace(*req, "spm", "SPM_PACKET_ISSUE", -1, -1, -1, -1,
                  "read", "CoeffDrain", packet.packetId, addr, bytes,
                  packet.bank, packet.port);
            trace(*req, "spm", "SPM_PACKET_ACCEPT", -1, -1, -1, -1,
                  "read", "CoeffDrain", packet.packetId, addr, bytes,
                  packet.bank, packet.port);
            if (packet.portWaitCycles)
                trace(*req, "spm", "SPM_PACKET_STALL_PORT", -1, -1, -1,
                      -1, "read", "CoeffDrain", packet.packetId, addr,
                      bytes, packet.bank, packet.port);
            if (packet.bankWaitCycles)
                trace(*req, "spm", "SPM_PACKET_STALL_BANK", -1, -1, -1,
                      -1, "read", "CoeffDrain", packet.packetId, addr,
                      bytes, packet.bank, packet.port);
            continue;
        }
        if (!drainPortsAvail || !spmReadPortsAvail) {
            req->record.stallSpmReadPort++;
            continue;
        }
        const uint64_t addr = req->slot * 0x800ull +
            static_cast<unsigned>(task.type) * 0x100ull + task.bytesDone;
        const uint32_t bank =
            (addr / env.spmBankGranularity) % env.spmBanks;
        if (spmBanksUsed[bank]) {
            req->record.stallSpmBank++;
            continue;
        }
        spmBanksUsed[bank] = true;
        drainPortsAvail--;
        spmReadPortsAvail--;
        const unsigned totalBytes = drainBytes(task.type);
        task.bytesDone += std::min<unsigned>(env.drainWidth,
                                             totalBytes-task.bytesDone);
        if (task.bytesDone == totalBytes)
            task.finishCycle = now + env.drainLatency;
    }
    activeDrains.erase(std::remove_if(activeDrains.begin(), activeDrains.end(),
        [this](const DrainTask &task) {
            Request *req = find(task.token);
            return !req || req->drainDone[static_cast<unsigned>(task.type)];
        }), activeDrains.end());
}

void
CfdCoeffPreprocessController::completeDrainPacket(
    uint64_t token, DrainType type, unsigned bytes, uint64_t packetId,
    Addr addr, unsigned bank, unsigned port, CfdSpmMatrix matrix)
{
    (void)matrix;
    Request *req = find(token);
    if (!req || req->stage == RequestStage::Failed)
        return;
    for (auto &task : activeDrains) {
        if (task.token != token || task.type != type)
            continue;
        task.bytesDone += bytes;
        req->record.coeffSpmPacketCompleted++;
        trace(*req, "spm", "SPM_PACKET_RESPONSE", -1, -1, -1, -1,
              "read-complete", "CoeffDrain", packetId, addr, bytes, bank,
              port);
        if (task.bytesDone >= drainBytes(type))
            finishDrain(task, cycle());
        break;
    }
}

void
CfdCoeffPreprocessController::finishRequests(uint64_t now)
{
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage == RequestStage::Failed ||
            req.stage == RequestStage::Complete)
            continue;
        const bool done = req.drainDone[static_cast<unsigned>(DrainType::Lu)] &&
            (!(req.matrixDrainMask & 1u) ||
             req.drainDone[static_cast<unsigned>(DrainType::DInv)]) &&
            (!(req.matrixDrainMask & 2u) ||
             req.drainDone[static_cast<unsigned>(DrainType::LBar)]) &&
            (!(req.matrixDrainMask & 4u) ||
             req.drainDone[static_cast<unsigned>(DrainType::UBar)]) &&
            (!req.baseConsumerEnabled ||
             (req.baseComputeComplete &&
              (req.directLbarBypass ||
               (req.basePublishComplete &&
                req.drainDone[
                    static_cast<unsigned>(DrainType::BaseVector)])))) &&
            (!req.lbarConsumerEnabled ||
             (req.dqStarReadyLocal && req.dqStarPublishComplete &&
              req.drainDone[
                  static_cast<unsigned>(DrainType::DqStarVector)] &&
              (!req.desc.cellId ||
               (req.correctionComplete && req.combineComplete)) &&
              (req.directLbarBypass || !req.desc.cellId ||
               req.drainDone[static_cast<unsigned>(
                   DrainType::CorrectionVector)])));
        if (!done)
            continue;
        req.stage = RequestStage::Complete;
        req.status = CfdCoeffPreprocessStatus::Complete;
        req.record.status = static_cast<uint64_t>(req.status);
        req.record.completeCycle = now;
        if (req.slotOwned)
            slots[req.slot] = false;
        req.slotOwned = false;
        proxy().writeBlob(req.desc.recordAddr, &req.record,
                          sizeof(req.record));
        trace(req, "controller", "REQUEST_COMPLETE");
    }
}

void
CfdCoeffPreprocessController::retireAutoRequests()
{
    std::vector<uint64_t> retired;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (!(req.desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE) ||
            (req.stage != RequestStage::Complete &&
             req.stage != RequestStage::Failed))
            continue;
        req.record.autoRetired = 1;
        req.record.autoRetireError =
            req.status == CfdCoeffPreprocessStatus::Complete ? 0 : 1;
        // This is the terminal release publication.  It happens after the
        // cycle's activity accounting and before request/token destruction.
        proxy().writeBlob(req.desc.recordAddr, &req.record,
                          sizeof(req.record));
        trace(req, "controller", "REQUEST_AUTO_RETIRE");
        retireLineChild(token, req);
        retired.push_back(token);
    }
    for (uint64_t token : retired)
        requests.erase(token);
    pumpLineRequests();
}

void
CfdCoeffPreprocessController::markFailed(
    Request &req, CfdCoeffPreprocessStatus status, const char *note)
{
    if (req.baseConsumerEnabled && !req.baseComputeComplete)
        req.record.baseAccumulatorsCancelled = 1;
    if (req.correctionInitialized && !req.correctionComplete)
        req.record.correctionAccumulatorsCancelled = 1;
    req.stage = RequestStage::Failed;
    req.status = status;
    req.record.status = static_cast<uint64_t>(status);
    req.record.completeCycle = cycle();
    if (req.slotOwned)
        slots[req.slot] = false;
    req.slotOwned = false;
    proxy().writeBlob(req.desc.recordAddr, &req.record, sizeof(req.record));
    trace(req, "controller", "REQUEST_FAILED", -1, -1, -1, -1, note);
}

void
CfdCoeffPreprocessController::accountActivity()
{
    bool input = false, lu = false, solve = false;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage == RequestStage::InputFetching) {
            req.record.inputActiveCycles++;
            input = true;
        }
        if (req.stage == RequestStage::LuActive) {
            req.record.luActiveCycles++;
            lu = true;
        }
        if (req.solveStarted && !req.solveDone) {
            req.record.solveActiveCycles++;
            solve = true;
        }
        if (req.stage == RequestStage::LuActive &&
            req.solveStarted && !req.solveDone)
            req.record.sameCellLuSolveOverlapCycles++;
    }
    const bool drain = !activeDrains.empty();
    if (drain) {
        for (auto &task : activeDrains) {
            if (Request *req = find(task.token))
                req->record.drainActiveCycles++;
        }
    }
    Request *owner = nullptr;
    for (auto &[token, ptr] : requests) {
        if (!owner || ptr->record.requestId < owner->record.requestId)
            owner = ptr.get();
    }
    if (owner) {
        bool anyColumnConsumer = false;
        for (const auto &[token, req] : requests)
            anyColumnConsumer |= req->baseConsumerEnabled ||
                                 req->lbarConsumerEnabled;
        if (anyColumnConsumer) {
            owner->record.columnFmaQueueOccupancySum +=
                columnFmaQueue.size();
            owner->record.columnFmaQueueSampleCycles++;
        }
        bool crossCellLuSolve = false;
        for (const auto &[luToken, luReq] : requests) {
            if (luReq->stage != RequestStage::LuActive)
                continue;
            for (const auto &[solveToken, solveReq] : requests) {
                if (luToken != solveToken && solveReq->solveStarted &&
                    !solveReq->solveDone) {
                    crossCellLuSolve = true;
                    break;
                }
            }
            if (crossCellLuSolve)
                break;
        }
        owner->record.inputLuOverlapCycles += input && lu;
        owner->record.inputSolveOverlapCycles += input && solve;
        owner->record.luSolveOverlapCycles += lu && solve;
        owner->record.crossCellLuSolveOverlapCycles += crossCellLuSolve;
        owner->record.computeDrainOverlapCycles += (lu || solve) && drain;
        owner->record.threeWayOverlapCycles += input && (lu || solve) && drain;
        uint64_t coeffDivBusy = 0;
        uint64_t columnFmaBusy = 0;
        uint64_t forwardCombineBusy = 0;
        for (const auto &op : pendingOps) {
            owner->record.dividerBusyLaneCycles +=
                op.kind == OpKind::LuDiv || op.kind == OpKind::Div;
            owner->record.mulBusyLaneCycles +=
                op.kind == OpKind::LuMul || op.kind == OpKind::Mul;
            owner->record.subBusyLaneCycles +=
                op.kind == OpKind::LuSub || op.kind == OpKind::Sub;
            owner->record.coeffDividerBusyLaneCycles +=
                op.kind == OpKind::Div;
            owner->record.coeffMulBusyLaneCycles +=
                op.kind == OpKind::Mul;
            owner->record.coeffSubBusyLaneCycles +=
                op.kind == OpKind::Sub;
            coeffDivBusy += op.kind == OpKind::Div;
            columnFmaBusy += op.kind == OpKind::ColumnFma;
            forwardCombineBusy += op.kind == OpKind::ForwardCombine;
        }
        owner->record.columnFmaBusyCycles += std::min<uint64_t>(
            columnFmaBusy, env.columnFmaCount);
        owner->record.forwardCombineBusyCycles += std::min<uint64_t>(
            forwardCombineBusy, env.forwardCombineCount);
        if (solve && coeffDivBusy < env.coeffDivCount)
            owner->record.coeffDividerIdleLaneCycles +=
                env.coeffDivCount - coeffDivBusy;
    }
}

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

std::array<ResourcePool *, 10>
CfdCoeffPreprocessController::resourcePools()
{
    // Same order as OpKind; count pending operations without double counting
    // a pipeline-active cycle when latency > II.
    static_assert(static_cast<unsigned>(OpKind::LineAccumulate) == 9);
    return {&luDiv, &luMul, &luSub, &coeffDiv, &coeffMul, &coeffSub,
            &columnFma, &forwardCombine, &lineProduct, &lineAccumulate};
}

void
CfdCoeffPreprocessController::endResourceStats(bool quiescent)
{
    if (!env.resourceStats)
        return;
    std::array<uint64_t, 10> occupancy = {};
    for (const auto &op : pendingOps)
        occupancy[static_cast<unsigned>(op.kind)]++;
    const auto pools = resourcePools();
    const char *names[] = {"lu_div", "lu_mul", "lu_sub", "coeff_div",
        "coeff_mul", "coeff_sub", "column_fma", "forward_combine",
        "line_product", "line_accumulate"};
    for (unsigned i = 0; i < pools.size(); ++i) {
        auto &p = *pools[i];
        p.issues += p.issuedNow;
        p.issueCycles += p.issuedNow != 0;
        p.blockedCycles += !p.issuedNow && p.attemptedNow;
        p.noAttemptCycles += !p.issuedNow && !p.attemptedNow;
        p.activeCycles += occupancy[i] != 0;
        p.occupancySum += occupancy[i];
        p.occupancyMax = std::max(p.occupancyMax, occupancy[i]);
        if (!quiescent || (i >= 8 && !env.lineMvmSplit))
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
        for (const auto &[reason, count] : diagnostics)
            std::fprintf(stderr, "CFD_SCHED_DIAG reason=%s count=%llu\n",
                reason.c_str(), (unsigned long long)count);
    }
}

void
CfdCoeffPreprocessController::processTick()
{
    const uint64_t now = cycle();
    spmReadPortsAvail = env.spmReadPorts;
    spmWritePortsAvail = env.spmWritePorts;
    drainPortsAvail = env.drainPorts;
    std::fill(spmBanksUsed.begin(), spmBanksUsed.end(), false);
    completeOps(now);
    beginResourceStats(now);
    assignInputSlots(now);
    processInputs(now);
    startLuRequests(now);
    processLu(now);
    startSolveRequests(now);
    processSolve(now);
    processForwardHandoffs(now);
    processColumnFma(now);
    processForwardCombines(now);
    processLineCachedForward(now);
    publishLineValues();
    processLineBackward(now);
    processLineBaseAhead(now);
    // A zero-wait adjacent request can consume a handoff produced by a head
    // cell or combine completion in the same modeled controller cycle.
    processForwardHandoffs(now);
    processDrains(now);
    finishRequests(now);
    accountActivity();
    retireAutoRequests();
    bool active = false;
    for (auto &[token, req] : requests) {
        if (req->stage != RequestStage::Complete &&
            req->stage != RequestStage::Failed) {
            active = true;
            break;
        }
    }
    bool activeLine = false;
    for (const auto &[token, line] : lineRequests) {
        if (!line->terminal) {
            activeLine = true;
            break;
        }
    }
    const bool more = active || activeLine || !pendingOps.empty() ||
        !columnFmaQueue.empty() ||
        !forwardCombineQueue.empty() ||
        !drainQueue.empty() ||
        !activeDrains.empty();
    endResourceStats(!more);
    if (more)
        scheduleTick();
}

CfdCoeffPreprocessController controller;

} // anonymous namespace

bool
cfdCoeffPreprocessEventEnabled()
{
    const char *model = std::getenv("GEM5_CFD_COEFF_PREPROCESS_MODEL");
    return model && std::strcmp(model, "event") == 0;
}

uint64_t
cfdCoeffPreprocessLaunch(ExecContext *xc, Addr descriptorAddr)
{
    return controller.launch(xc, descriptorAddr);
}

uint64_t
cfdCoeffPreprocessWait(uint64_t token)
{
    return controller.wait(token);
}


uint64_t
cfdCoeffPreprocessCancel(uint64_t token)
{
    return controller.cancel(token);
}

} // namespace ArmISA
} // namespace gem5
