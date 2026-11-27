#include "arch/arm/cfd_coeff_preprocess_controller.hh"
#include "arch/arm/cfd_arithmetic_groups.hh"

#include <algorithm>
#include <tuple>
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
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpu/exec_context.hh"
#include "arch/arm/cfd_local_spm.hh"
#include "arch/arm/cfd_input_capacity.hh"
#include "arch/arm/cfd_input_batch_order.hh"
#include "arch/arm/cfd_fp64_divider.hh"
#include "arch/arm/cfd_register_ports.hh"
#include "arch/arm/cfd_local_vector.hh"
#include "arch/arm/cfd_rhs_pairs.hh"
#include "arch/arm/cfd_lu_products.hh"
#include "arch/arm/cfd_lu_upper.hh"
#include "arch/arm/cfd_lu_rank1.hh"
#include "arch/arm/cfd_rhs_factors.hh"
#include "arch/arm/cfd_rhs_fma.hh"
#include "arch/arm/cfd_lu_fma.hh"
#include "arch/arm/cfd_rhs_admission.hh"
#include "arch/arm/cfd_rhs_upper_stream.hh"
#include "arch/arm/cfd_spm_slot_rotation.hh"
#include "arch/arm/cfd_sme_za.hh"
#include "arch/arm/cfd_sme_2d_arbiter.hh"
#include "arch/arm/cfd_rhs_upper.hh"
#include "arch/arm/cfd_rhs_factor_fanout.hh"
#include "arch/arm/cfd_tile_stream.hh"
#include "arch/arm/cfd_summary_hash.hh"
#include "arch/arm/cfd_summary_snapshots.hh"
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
constexpr unsigned PackedColdInputBytes = 3 * MatrixBytes + VectorBytes;
static_assert(PackedColdInputBytes % 64 == 0);
constexpr uint64_t DescriptorVersion = 1;

static unsigned
completionEntryBytes(unsigned level)
{
    return level >= 11 ? 512 : 1024;
}

static size_t
recordBytes(uint64_t flags)
{
    return flags & CFD_COEFF_PRE_PACKED_RECORD ? sizeof(CfdCoeffPackedRecord) :
           flags & CFD_COEFF_PRE_COMPACT_RECORD ? sizeof(CfdCoeffCompactRecord) :
                                                 sizeof(CfdCoeffPreprocessRecord);
}

static CfdCoeffPackedRecord
packedRecord(const CfdCoeffPreprocessRecord &record)
{
    CfdCoeffPackedRecord packed{};
#define CFD_PACKED_FIELD(type, cpp, guest) \
    if constexpr (sizeof(type) < sizeof(uint64_t)) \
        panic_if(record.cpp > std::numeric_limits<type>::max(), "CFD packed record overflow: " #cpp); \
    packed.cpp = record.cpp;
#include "arch/arm/cfd_packed_record_fields.inc"
#undef CFD_PACKED_FIELD
    return packed;
}

static uint64_t
recordDigest(const CfdCoeffPreprocessRecord &record)
{
    uint64_t digest=14695981039346656037ULL;
#define CFD_RECORD_FIELD(cpp, guest) digest=(digest^record.cpp)*1099511628211ULL;
#include "arch/arm/cfd_compact_record_fields.inc"
#undef CFD_RECORD_FIELD
    return digest;
}

static uint64_t
recordWord(const CfdCoeffPreprocessRecord &record, unsigned word)
{
    unsigned index = 0;
#define CFD_RECORD_FIELD(cpp, guest) if (index++ == word) return record.cpp;
#include "arch/arm/cfd_compact_record_fields.inc"
#undef CFD_RECORD_FIELD
    panic("CFD summary word index outside compact record");
}

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
    RequestFrontierAging,
    RhsFrontier,
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
    if (value && std::strcmp(value, "rhs-frontier") == 0)
        return RhsSchedule::RhsFrontier;
    if (value && std::strcmp(value, "request-frontier-aging") == 0)
        return RhsSchedule::RequestFrontierAging;
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

enum class SmeUbarMode { External, Resident, Deferred };
enum class SmeWarmBaseMode { DirectLu = 0, InverseFma1 = 1, InverseFma2 = 2, Adaptive = 3 };
enum class SmeCacheBase : uint64_t { Direct, Inverse, GuardFallback };

struct EventEnv
{
    unsigned smeContexts = 0;
    bool smeEarlyPanels = false;
    bool smeChainSteps = false;
    bool smeSplitRhs = false;
    bool smePanelBOverlap = false;
    bool smeForwardTileRotate = false;
    unsigned sme2dCandidateWindow = 0;
    bool smeFeedbackReg = false;
    unsigned smeWarmRhsWidth = 0;
    bool smeWarmVerticalRhs = false;
    bool smeWarmSingleTile = false;
    bool smeWarmBypass = false;
    bool smeCriticalArb = false;
    bool smeWarmBackSplit = false;
    SmeWarmBaseMode smeWarmBaseMode = SmeWarmBaseMode::DirectLu;
    bool smeInvSparseIdentity = true;
    uint64_t smeInvExpThresh = 20;
    uint64_t smeReuseHint = 0;
    uint64_t smeAdaptiveK = 0;
    SmeUbarMode smeUbarMode = SmeUbarMode::External;
    unsigned smeReverseContexts = 0;
    unsigned smeUpperSplit = 3;
    bool smeReverseWavefront = false;
    CfdSmeZa::Config smeConfig;
    uint64_t ticksPerCycle = 500;
    uint32_t inputSlots = 2;
    bool extendedInputSlots = false;
    bool inputDEarly = false;
    bool coldRhsPrefetch = false;
    uint32_t luPendingDepth = 2;
    uint32_t solvePendingDepth = 2;
    uint32_t luCount = 1;
    uint32_t solveCount = 1;
    uint32_t outputDepth = 4;
    uint32_t drainTaskDepth = 4;
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
    uint32_t lineOutputDepth = 4;
    bool explicitLineOutputDepth = false;
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
    bool luReciprocalShare = false;
    bool luSupplyFirst = false;
    bool coldSummary = false;
    bool coldSummaryOnly = false;
    bool warmReciprocal = false;
    bool lineBaseAhead = true;
    bool lineEarlyBackward = true;
    bool lineMvmSplit = false;
    bool lineMvmTree = false;
    bool backwardFeedbackTree = false, rhsSupplyPriority = false, coldBTree = false;
    unsigned coldRhsTree = 0; // 0: legacy/b-only, 1: b+Lbar, 2: all, 3: Lbar only.
    unsigned tailUbarOverlap = 0;
    unsigned luLookahead = 0;
    bool warmForwardTree = false, coldForwardTree = false;
    bool warmRhsTree = false, ageDrains = false;
    uint64_t drainArbLatency = 1, drainArbIi = 1;
    bool dedicatedFinalSubtract = false;
    bool sharedCluster = false;
    bool privateLuOutput = false;
    unsigned rfReadWords = 0, rfWriteWords = 0;
    bool rfBroadcast = false, rfSplitWriteback = false;
    bool preprocessBroadcast = false;
    bool rhsPair = false;
    bool luProductAhead = false;
    bool luRank1 = false;
    bool rhsFactorReuse = false;
    unsigned rhsFmaCount = 0;
    unsigned luFmaCount = 0;
    bool rhsFrontierReserve = false;
    bool rhsUpperInverse = false;
    bool rhsUpperStream = false;
    bool luPivotPrefetch = false;
    unsigned spmSlotRotation = 0;
    unsigned rhsUpperSlots = 16;
    bool rhsFmaFactorFanout = false;
    bool rhsFactorFanout = false;
    bool rhsFactorFanoutFeedback = false;
    bool rhsTreeRfTemp = false;
    bool rhsTreeRfFirstColumn = false;
    unsigned rhsFactorSlots = 2;
    bool luControlFold = false;
    bool luUpperOverlap = false;
    bool inputBatchReady = false, inputLFirst = false, inputRBeforeU = false;
    bool laneMvm = false, laneLocal = false;
    unsigned laneIssueWidth = 10, laneSelectLatency = 1, laneRouteLatency = 1;
    unsigned backwardOperandDepth = 2;
    bool backwardIndependentReady = false;
    uint64_t onchipArbLatency = 1;
    unsigned onchipArbTasks = 0;
    uint64_t clusterMulLatency = 3;
    unsigned vectorIssueWidth = 1;
    bool vectorLocal = false, localProducts = false, demandSchedule = false;
    bool vectorProductOnly = false;
    unsigned localRouteLatency = 1;
    bool localCreditRelease = false;
    // Host-side trace observations; never used for arbitration or readiness.
    bool round1Diagnostics = false;
    unsigned summaryDepth = 0, summaryEngines = 1;
    unsigned clusterMulCount = N;
    unsigned clusterAddCount = N;
    CfdArithmeticGroups::Mode arithmeticMode = CfdArithmeticGroups::Legacy;
    unsigned arithmeticDepth = 12, arithmeticSelect = 1, arithmeticRoute = 1;
    unsigned arithmeticBorrowRoute = 1, arithmeticWidth = 4, arithmeticAge = 32;
    bool extraMulLuOnly = false;
    uint64_t clusterAddLatency = 4;
    uint64_t clusterDivLatency = 12;
    uint64_t clusterDivIi = 4;
    uint32_t clusterDivCount = 1;
    bool detailedDivider = false, iterativeDivider = false;
    unsigned dividerCapacity = 0, dividerResultDepth = 2;
    uint64_t dividerWritebackIi = 1;
    uint32_t clusterProductDepth = 8;
    bool tiledLine = false;
    unsigned lineOptLevel = 0;
    bool rhsRowParallel = false;
    unsigned rhsProductDepth = 60;
    unsigned completionDepth = 8, warmWriteDepth = 8;
    unsigned dataCommitDepth = 0;
    bool onchipTiming = false;
    unsigned onchipDepth = 64;
    unsigned onchipComputeReserve = 0;
    uint64_t onchipLatency = 1;
    bool dmaOutputPriority = true;
    bool adaptiveCache = false;
    bool warmYFifo = false;
    bool rhsInputCoalesce = false, reverseTileReload = false;
    bool tileOperandStream = false, outputCoalesce = false;
    unsigned rhsLookahead = 0;
    bool timedInterface = false;
    uint32_t interfaceWidth = 64;
    uint64_t lineSpmBytes = 131072;
    uint32_t lineTileCells = 16;
    uint32_t lineCacheCells = 0, tileBuffers = 1, tileDmaWidth = 40;
    unsigned tilePrefetchBuffers = 0;
    bool tileCellOrder = false;
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
    e.tiledLine = envBool("GEM5_CFD_LINE_TILED", false);
    e.lineOptLevel = envU64("GEM5_CFD_LINE_OPT_LEVEL", 0);
    e.dmaOutputPriority = envBool("GEM5_CFD_DMA_OUTPUT_PRIORITY", true);
    e.adaptiveCache = envBool("GEM5_CFD_ADAPTIVE_CACHE", false);
    e.warmYFifo = envBool("GEM5_CFD_WARM_Y_FIFO", false);
    e.rhsInputCoalesce = envBool("GEM5_CFD_RHS_INPUT_COALESCE", false);
    e.reverseTileReload = envBool("GEM5_CFD_REVERSE_TILE_RELOAD", false);
    e.tileOperandStream = envBool("GEM5_CFD_TILE_OPERAND_STREAM", false);
    e.outputCoalesce = envBool("GEM5_CFD_OUTPUT_COALESCE", false);
    e.rhsLookahead = envU64("GEM5_CFD_RHS_LOOKAHEAD",0);
    e.rhsRowParallel = envBool("GEM5_CFD_RHS_ROW_PARALLEL", false);
    e.rhsProductDepth = envU64("GEM5_CFD_RHS_PRODUCT_DEPTH",60);
    panic_if(e.rhsProductDepth<1 || e.rhsProductDepth>60,"CFD RHS product depth outside 1..60");
    e.completionDepth = envU64("GEM5_CFD_COMPLETION_DEPTH", 8);
    e.warmWriteDepth = envU64("GEM5_CFD_WARM_WRITE_DEPTH", 8);
    e.dataCommitDepth = envU64("GEM5_CFD_DATA_COMMIT_DEPTH", 0);
    e.onchipTiming = envBool("GEM5_CFD_ONCHIP_TIMING", e.lineOptLevel >= 8);
    e.onchipDepth = envU64("GEM5_CFD_ONCHIP_DEPTH", 64);
    e.onchipComputeReserve = envU64("GEM5_CFD_ONCHIP_COMPUTE_RESERVE", 0);
    panic_if(e.onchipComputeReserve >= e.onchipDepth,
             "CFD compute reserve must leave at least one DMA credit");
    e.onchipLatency = envU64("GEM5_CFD_ONCHIP_LATENCY", 1);
    e.timedInterface = envBool("GEM5_CFD_LINE_TIMED_INTERFACE", false);
    e.interfaceWidth = envU64("GEM5_CFD_LINE_INTERFACE_WIDTH", 64);
    e.lineSpmBytes = envU64("GEM5_CFD_LINE_SPM_BYTES", 131072);
    e.lineTileCells = envU64("GEM5_CFD_LINE_TILE_CELLS", 16);
    e.lineCacheCells = envU64("GEM5_CFD_LINE_CACHE_CELLS", 0);
    e.tileBuffers = envU64("GEM5_CFD_LINE_TILE_BUFFERS", 1);
    e.tilePrefetchBuffers = envU64("GEM5_CFD_TILE_PREFETCH_BUFFERS", 0);
    e.tileCellOrder = envBool("GEM5_CFD_TILE_CELL_ORDER", false);
    e.tileDmaWidth = envU64("GEM5_CFD_LINE_TILE_DMA_WIDTH", 40);
    e.sharedCluster = envBool("GEM5_CFD_SHARED_CLUSTER", false);
    e.clusterMulLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_MUL_LAT", 3));
    e.vectorIssueWidth = envU64("GEM5_CFD_VECTOR_ISSUE_WIDTH",1);
    e.vectorLocal = envBool("GEM5_CFD_VECTOR_LOCAL",false);
    e.vectorProductOnly = envBool("GEM5_CFD_VECTOR_PRODUCT_ONLY",false);
    e.localProducts = envBool("GEM5_CFD_LOCAL_PRODUCTS",false);
    e.demandSchedule = envBool("GEM5_CFD_DEMAND_SCHEDULE",false);
    e.localRouteLatency = envU64("GEM5_CFD_LOCAL_ROUTE_LATENCY",1);
    e.localCreditRelease = envBool("GEM5_CFD_LOCAL_CREDIT_RELEASE",false);
    e.round1Diagnostics = envBool("GEM5_CFD_ROUND1_DIAGNOSTICS",false);
    e.summaryDepth = envU64("GEM5_CFD_SUMMARY_DEPTH",0);
    e.summaryEngines = envU64("GEM5_CFD_SUMMARY_ENGINES",1);
    e.clusterMulCount = envU64("GEM5_CFD_CLUSTER_MUL_COUNT", N);
    e.clusterAddCount = envU64("GEM5_CFD_CLUSTER_ADD_COUNT", N);
    e.arithmeticMode = static_cast<CfdArithmeticGroups::Mode>(envU64("GEM5_CFD_ARITHMETIC_MODE",0));
    e.arithmeticDepth = envU64("GEM5_CFD_ARITHMETIC_DEPTH",12);
    e.arithmeticSelect = envU64("GEM5_CFD_ARITHMETIC_SELECT",1);
    e.arithmeticRoute = envU64("GEM5_CFD_ARITHMETIC_ROUTE",1);
    e.arithmeticBorrowRoute = envU64("GEM5_CFD_ARITHMETIC_BORROW_ROUTE",1);
    e.arithmeticWidth = envU64("GEM5_CFD_ARITHMETIC_WIDTH",4);
    e.arithmeticAge = envU64("GEM5_CFD_ARITHMETIC_AGE",32);
    e.extraMulLuOnly = envBool("GEM5_CFD_EXTRA_MUL_LU_ONLY", false);
    panic_if(e.arithmeticMode > CfdArithmeticGroups::Borrow ||
             (e.arithmeticMode == CfdArithmeticGroups::Legacy &&
              (e.clusterMulCount < N || e.clusterMulCount > 10 ||
               e.clusterAddCount < N || e.clusterAddCount > 10)) ||
             (e.arithmeticMode != CfdArithmeticGroups::Legacy &&
              (e.clusterMulCount < N || e.clusterMulCount > 8 ||
               e.clusterAddCount < N || e.clusterAddCount > 8 || e.extraMulLuOnly ||
               e.arithmeticDepth < 4 || e.arithmeticDepth > 32 ||
               !e.arithmeticSelect || e.arithmeticSelect > 8 ||
               e.arithmeticRoute > 8 || e.arithmeticBorrowRoute > 8 ||
               !e.arithmeticWidth || e.arithmeticWidth > 8 ||
               !e.arithmeticAge || e.arithmeticAge > 256)) ||
             (e.arithmeticMode >= CfdArithmeticGroups::Fixed &&
              (e.clusterMulCount <= N || e.clusterAddCount <= N)) ||
             (e.extraMulLuOnly && e.clusterMulCount != N+1),
             "CFD arithmetic counts, ownership or bounded timing are invalid");
    e.clusterAddLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_ADD_LAT", 4));
    e.clusterDivLatency = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_DIV_LAT", 12));
    e.clusterDivIi = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_DIV_II", 4));
    e.clusterDivCount = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_DIV_COUNT", e.coeffDivCount));
    e.dedicatedFinalSubtract = envBool(
        "GEM5_CFD_LINE_FINAL_SUBTRACT_DEDICATED", false);
    const char *dividerModel = std::getenv("GEM5_CFD_DIVIDER_MODEL");
    if (!dividerModel) dividerModel = "timing";
    panic_if(std::strcmp(dividerModel,"timing") && std::strcmp(dividerModel,"pipelined") &&
             std::strcmp(dividerModel,"iterative"), "CFD invalid divider model");
    e.detailedDivider = std::strcmp(dividerModel,"timing") != 0;
    e.iterativeDivider = std::strcmp(dividerModel,"iterative") == 0;
    e.dividerCapacity = envU64("GEM5_CFD_DIVIDER_INFLIGHT",0);
    e.dividerResultDepth = envU64("GEM5_CFD_DIVIDER_RESULT_DEPTH",2);
    e.dividerWritebackIi = envU64("GEM5_CFD_DIVIDER_WRITEBACK_II",1);
    e.clusterProductDepth = std::max<uint64_t>(1,
        envU64("GEM5_CFD_CLUSTER_PRODUCT_DEPTH", 8));
    e.resourceStats = envBool("GEM5_CFD_RESOURCE_STATS", false);
    e.lineBaseAhead = envBool("GEM5_CFD_LINE_BASE_AHEAD", true);
    e.lineEarlyBackward = envBool("GEM5_CFD_LINE_EARLY_BACKWARD", true);
    e.lineMvmSplit = envBool("GEM5_CFD_LINE_MVM_SPLIT", false);
    e.lineMvmTree = envBool("GEM5_CFD_LINE_MVM_TREE", false);
    e.backwardFeedbackTree = envBool("GEM5_CFD_BACKWARD_FEEDBACK_TREE", false);
    e.rhsSupplyPriority = envBool("GEM5_CFD_RHS_SUPPLY_PRIORITY", false);
    e.coldBTree = envBool("GEM5_CFD_COLD_B_TREE", false);
    e.coldRhsTree = envU64("GEM5_CFD_COLD_RHS_TREE", 0);
    e.tailUbarOverlap = envU64("GEM5_CFD_TAIL_UBAR_OVERLAP", 0);
    e.luLookahead = envU64("GEM5_CFD_LU_LOOKAHEAD", 0);
    e.warmForwardTree = envBool("GEM5_CFD_WARM_FORWARD_TREE", false);
    e.coldForwardTree = envBool("GEM5_CFD_COLD_FORWARD_TREE", false);
    e.warmRhsTree = envBool("GEM5_CFD_WARM_RHS_TREE", false);
    e.ageDrains = envBool("GEM5_CFD_AGE_DRAINS", false);
    e.drainArbLatency = envU64("GEM5_CFD_DRAIN_ARB_LATENCY", 1);
    e.drainArbIi = envU64("GEM5_CFD_DRAIN_ARB_II", 1);
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
    e.extendedInputSlots = envBool("GEM5_CFD_EXTENDED_INPUT_SLOTS", false);
    e.inputDEarly = envBool("GEM5_CFD_INPUT_D_EARLY", false);
    e.coldRhsPrefetch = envBool("GEM5_CFD_COLD_RHS_PREFETCH", false);
    const bool smeZaEnabled = envBool("GEM5_CFD_SME_ZA", false);
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
    e.drainTaskDepth = envU64("GEM5_CFD_DRAIN_TASK_DEPTH",e.outputDepth);
    panic_if(!e.drainTaskDepth || e.drainTaskDepth > 64,
             "CFD drain task depth must be 1..64");
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
    e.explicitLineOutputDepth = std::getenv("GEM5_CFD_LINE_OUTPUT_DEPTH") != nullptr;
    e.lineOutputDepth = envU64("GEM5_CFD_LINE_OUTPUT_DEPTH", e.drainQueueDepth);
    if (!e.lineOutputDepth)
        e.lineOutputDepth = e.drainQueueDepth;
    panic_if(e.explicitLineOutputDepth && e.lineOutputDepth > 128,
             "CFD final output FIFO depth must be in [1,128]");
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
    e.privateLuOutput = envBool("GEM5_CFD_PRIVATE_LU_OUTPUT", false);
    e.rfReadWords = envU64("GEM5_CFD_RF_READ_WORDS", 0);
    e.rfWriteWords = envU64("GEM5_CFD_RF_WRITE_WORDS", 0);
    e.rfBroadcast = envBool("GEM5_CFD_RF_BROADCAST", false);
    e.preprocessBroadcast = envBool("GEM5_CFD_PREPROCESS_BROADCAST", false);
    e.rhsPair = envBool("GEM5_CFD_RHS_PAIR", false);
    e.luProductAhead = envBool("GEM5_CFD_LU_PRODUCT_AHEAD", false);
    e.rhsFactorReuse = envBool("GEM5_CFD_RHS_FACTOR_REUSE", false);
    e.rhsFmaCount = envU64("GEM5_CFD_RHS_FMA_COUNT", 0);
    e.luFmaCount = envU64("GEM5_CFD_LU_FMA_COUNT", 0);
    e.rhsFrontierReserve = envBool("GEM5_CFD_RHS_FRONTIER_RESERVE", false);
    e.rhsUpperInverse=envBool("GEM5_CFD_RHS_UPPER_INVERSE",false);
    e.rhsUpperStream=envBool("GEM5_CFD_RHS_UPPER_STREAM",false);
    e.luPivotPrefetch=envBool("GEM5_CFD_LU_PIVOT_PREFETCH",false);
    e.spmSlotRotation=envU64("GEM5_CFD_SPM_SLOT_ROTATION",0);
    e.rhsUpperSlots=envU64("GEM5_CFD_RHS_UPPER_SLOTS",16);
    e.rhsFmaFactorFanout = envBool("GEM5_CFD_RHS_FMA_FACTOR_FANOUT", false);
    e.rhsFactorFanout = envBool("GEM5_CFD_RHS_FACTOR_FANOUT", false);
    e.rhsFactorFanoutFeedback = envBool("GEM5_CFD_RHS_FACTOR_FANOUT_FEEDBACK", false);
    e.rhsTreeRfTemp = envBool("GEM5_CFD_RHS_TREE_RF_TEMP", false);
    e.rhsTreeRfFirstColumn = envBool("GEM5_CFD_RHS_TREE_RF_FIRST_COLUMN", false);
    e.luRank1 = envBool("GEM5_CFD_LU_RANK1", false);
    e.rhsFactorSlots = envU64("GEM5_CFD_RHS_FACTOR_SLOTS", 2);
    panic_if((e.rhsFactorSlots!=2 && e.rhsFactorSlots!=4 && e.rhsFactorSlots!=8 &&
              e.rhsFactorSlots!=16 && e.rhsFactorSlots!=32 && e.rhsFactorSlots!=64) ||
             (!e.rhsFactorReuse && e.rhsFactorSlots!=2),
             "CFD factor slots must be 2, 4, 8, 16, 32 or 64 and expansion requires reuse");
    e.luControlFold = envBool("GEM5_CFD_LU_CONTROL_FOLD", false);
    e.luUpperOverlap = envBool("GEM5_CFD_LU_UPPER_OVERLAP", false);
    e.inputBatchReady = envBool("GEM5_CFD_INPUT_BATCH_READY", false);
    e.inputLFirst = envBool("GEM5_CFD_INPUT_L_FIRST", false);
    e.inputRBeforeU = envBool("GEM5_CFD_INPUT_R_BEFORE_U", false);
    e.rfSplitWriteback = envBool("GEM5_CFD_RF_SPLIT_WRITEBACK", false);
    e.laneMvm = envBool("GEM5_CFD_LANE_MVM", false);
    e.laneLocal = envBool("GEM5_CFD_LANE_LOCAL", false);
    e.laneIssueWidth = envU64("GEM5_CFD_LANE_ISSUE_WIDTH", 10);
    e.laneSelectLatency = envU64("GEM5_CFD_LANE_SELECT_LATENCY", 1);
    e.laneRouteLatency = envU64("GEM5_CFD_LANE_ROUTE_LATENCY", 1);
    e.backwardOperandDepth = envU64("GEM5_CFD_BACK_OPERAND_DEPTH", 2);
    e.backwardIndependentReady = envBool("GEM5_CFD_BACK_INDEPENDENT_READY", false);
    e.onchipArbLatency = envU64("GEM5_CFD_ONCHIP_ARB_LATENCY", 1);
    e.onchipArbTasks = envU64("GEM5_CFD_ONCHIP_ARB_TASKS", 0);
    e.luForwarding = envBool("GEM5_CFD_LU_FORWARDING", true);
    e.luSolveEarlyStart = envBool("GEM5_CFD_LU_SOLVE_EARLY_START", true);
    const char *spmModel = std::getenv("GEM5_CFD_COEFF_SPM_MODEL");
    e.packetSpm = spmModel && std::strcmp(spmModel, "packet") == 0;
    e.dmaSpm = spmModel && std::strcmp(spmModel, "dma") == 0;
    const char *divMode = std::getenv("GEM5_CFD_COEFF3_DIV_MODE");
    e.reciprocalSolve =
        divMode && std::strcmp(divMode, "reciprocal") == 0;
    e.luReciprocalShare = envBool("GEM5_CFD_LU_RECIPROCAL_SHARE",false);
    e.luSupplyFirst = envBool("GEM5_CFD_LU_SUPPLY_FIRST",false);
    e.coldSummary = envBool("GEM5_CFD_COLD_SUMMARY",false);
    e.coldSummaryOnly = envBool("GEM5_CFD_COLD_SUMMARY_ONLY",false);
    panic_if(e.coldSummaryOnly && !e.coldSummary, "CFD cold-only summary mode mismatch");
    panic_if(e.coldSummary && (!e.timedInterface || !e.tiledLine ||
        e.lineOptLevel < 11), "CFD line summary requires timed finite storage");
    const unsigned maxSummaryDepth = smeZaEnabled ? CfdSummarySnapshots::SmeMaxDepth : 8;
    const unsigned maxSummaryEngines = smeZaEnabled ? CfdSummarySnapshots::SmeMaxEngines : 4;
    panic_if(e.summaryDepth > maxSummaryDepth || !e.summaryEngines ||
        e.summaryEngines > maxSummaryEngines ||
        (e.summaryDepth && (!e.coldSummary || e.summaryDepth < e.summaryEngines)) ||
        (!e.summaryDepth && e.summaryEngines != 1),
        "CFD summary snapshots exceed the selected backend's bounded resources");
    panic_if(!CfdInputCapacity::allowed(e.lineOptLevel, e.inputSlots,
                                   e.extendedInputSlots, smeZaEnabled) ||
             (e.extendedInputSlots && (!e.tiledLine || !e.sharedCluster ||
              !e.onchipTiming || !e.dmaSpm)),
        "CFD input capacity requires valid bounded slots; extension requires level13 shared timed DMA");
    panic_if(e.localCreditRelease && !(e.vectorLocal || e.localProducts),
        "CFD local credit release requires a bounded local result destination");
    panic_if(e.vectorProductOnly && !e.vectorLocal,
        "CFD products-only MVM requires local vector storage");
    panic_if(e.luSupplyFirst && (!e.sharedCluster || !e.luLookahead ||
        e.lineOptLevel < 7),"CFD LU supply priority requires bounded lookahead");
    panic_if(e.luReciprocalShare && (!e.sharedCluster || !e.reciprocalSolve ||
        !e.detailedDivider || !e.rfReadWords || !e.rfWriteWords || !e.rhsRowParallel ||
        e.lineOptLevel < 7 || e.arithmeticMode != CfdArithmeticGroups::Legacy),
        "CFD LU reciprocal sharing requires row LU and direct shared arithmetic");
    panic_if(!e.vectorIssueWidth || e.vectorIssueWidth > 2 ||
        e.localRouteLatency > 8 ||
        ((e.vectorIssueWidth > 1 || e.vectorLocal || e.localProducts || e.demandSchedule) &&
         (!e.sharedCluster || !e.tiledLine || !e.onchipTiming || !e.rfReadWords ||
          !e.rfWriteWords || e.arithmeticMode != CfdArithmeticGroups::Legacy ||
          e.laneMvm || !e.lineMvmTree || !e.rhsRowParallel || e.lineOptLevel < 7)),
        "CFD dual vector issue/local storage requires finite direct shared tree resources");
    e.warmReciprocal = envBool("GEM5_CFD_WARM_RECIPROCAL", false);
    e.spmLayout = spmLayoutFromEnv();
    e.traceEnable = envBool("GEM5_CFD_COEFF_TRACE_ENABLE", false);
    panic_if(e.round1Diagnostics && (!e.traceEnable || !e.sharedCluster ||
        !e.rhsRowParallel || !e.rfReadWords || !e.rfWriteWords ||
        e.arithmeticMode != CfdArithmeticGroups::Legacy),
        "CFD round-one diagnostics require traced finite shared row arithmetic");
    panic_if((e.privateLuOutput && (!e.sharedCluster || !e.tiledLine || !e.luForwarding)) ||
        ((e.rfReadWords || e.rfWriteWords) && (!e.sharedCluster || e.dedicatedFinalSubtract)) ||
        (e.rfReadWords && e.rfReadWords < 2*N) ||
        (e.rfWriteWords && e.rfWriteWords < (e.rfSplitWriteback ? 1 : N)) ||
        !e.onchipArbLatency, "CFD unsupported private LU / RF / arbiter configuration");
    panic_if(e.tilePrefetchBuffers > 4 || (e.tilePrefetchBuffers &&
        (!e.tiledLine || e.tileBuffers != 2 || e.lineOptLevel < 10 || !e.onchipTiming)),
        "CFD extra tile prefetch requires timed early-backward double buffering");
    panic_if(e.tileCellOrder && (!e.tiledLine || !e.onchipTiming ||
             e.lineOptLevel < 10 || !e.reverseTileReload || !e.tileOperandStream),
             "CFD cell-ordered tile reads require timed reverse-stream y/Ubar reloads");
    panic_if(e.inputDEarly && (!e.tiledLine || !e.onchipTiming ||
             !e.dmaSpm || e.lineOptLevel < 8),
             "CFD early D input requires timed tiled DMA operands");
    panic_if(e.coldRhsPrefetch && (!e.inputDEarly || !e.sharedCluster ||
             !e.rhsRowParallel || !e.luForwarding || !e.luSolveEarlyStart ||
             !e.rhsLookahead || e.rhsLookahead > 8 || e.lineOptLevel < 13 ||
             !e.timedInterface || (!smeZaEnabled && (!e.rfReadWords || !e.rfWriteWords)) ||
             e.rhsSchedule != RhsSchedule::RhsFrontier),
             "CFD cold R prefetch requires early D, shared row/frontier and bounded R lookahead");
    panic_if((e.laneMvm && (!e.lineMvmTree || !e.sharedCluster || !e.tiledLine ||
             !e.onchipTiming || !e.rfReadWords || !e.rfWriteWords || e.lineProductDepth != N)) ||
             (e.laneLocal && !e.laneMvm) || !e.laneIssueWidth || e.laneIssueWidth > 10 ||
             !e.laneSelectLatency || e.laneSelectLatency > 16 ||
             !e.laneRouteLatency || e.laneRouteLatency > 16 ||
             e.backwardOperandDepth < 2 || e.backwardOperandDepth > 8 ||
             (e.backwardOperandDepth != 2 && (!e.onchipTiming || e.lineOptLevel < 9)),
             "CFD lane scheduling requires bounded RF, timed shared tiled tree and valid pipeline widths");
    panic_if(e.arithmeticMode != CfdArithmeticGroups::Legacy &&
             (!e.sharedCluster || !e.tiledLine || !e.onchipTiming ||
              !e.rfReadWords || !e.rfWriteWords || e.laneMvm ||
              e.preprocessBroadcast || e.dedicatedFinalSubtract),
             "CFD grouped arithmetic requires timed shared bounded RF and no other lane scheduler");
    panic_if(e.preprocessBroadcast && (!e.rfBroadcast || !e.rfReadWords ||
             !e.rfWriteWords || !e.sharedCluster || !e.rhsRowParallel || e.lineOptLevel < 7),
             "CFD preprocessing fanout requires the existing shared broadcast network, bounded RF and row scheduling");
    panic_if(e.rhsPair && (!e.localProducts || !e.rfBroadcast ||
             e.clusterMulCount < 8 || e.extraMulLuOnly ||
             e.rhsSchedule != RhsSchedule::RhsFrontier || e.laneMvm ||
             e.arithmeticMode != CfdArithmeticGroups::Legacy),
             "CFD common-LU pairs require local products, shared 8..10 MUL and RHS frontier");
    panic_if(e.luProductAhead && (!e.localProducts || !e.luReciprocalShare),
             "CFD LU product lookahead requires local row products and shared reciprocals");
    panic_if(e.luUpperOverlap && (!e.localProducts || !e.luReciprocalShare ||
              !e.sharedCluster || !e.rhsRowParallel || e.lineOptLevel<7 ||
              !e.rfReadWords || !e.rfWriteWords),
             "CFD upper overlap requires shared row LU, local products and reciprocals");
    panic_if(e.luRank1 && (!e.luProductAhead || !e.luReciprocalShare ||
             !e.sharedCluster || !e.rhsRowParallel || e.lineOptLevel<7 ||
             !e.rfReadWords || !e.rfWriteWords || e.luUpperOverlap ||
             e.arithmeticMode!=CfdArithmeticGroups::Legacy),
             "CFD rank1 LU requires paid dual products, shared RF/reciprocals and its independent control path");
    panic_if(e.inputRBeforeU && !CfdInputBatchOrder::allowed(e.lineOptLevel,
                 e.inputBatchReady,e.inputLFirst,e.coldRhsPrefetch),
             "CFD R bank priority requires level13 L-first batch input without cold R DMA prefetch");
    panic_if((e.inputBatchReady && (!e.inputDEarly || !e.onchipTiming ||
              !e.sharedCluster || !e.rhsRowParallel)) || (e.inputLFirst && !e.inputBatchReady),
             "CFD batch input publication requires early D, timed SPM and shared row RHS");
    panic_if(e.rhsUpperSlots<4 || e.rhsUpperSlots>32 ||
             (!e.rhsUpperInverse && e.rhsUpperSlots!=16) ||
             (e.rhsUpperInverse && !e.rhsFmaCount),
             "CFD shared upper requires FMA and 4..32 bounded owner slots");
    panic_if(e.rhsUpperStream && (!e.rhsUpperInverse || !e.rhsFmaCount || !e.luReciprocalShare ||
        e.rhsSchedule!=RhsSchedule::RhsFrontier || !e.rfReadWords || !e.rfWriteWords),
        "CFD upper streaming requires shared LU reciprocals, upper, FMA, frontier and finite RF");
    panic_if(e.spmSlotRotation>3 || (e.spmSlotRotation &&
        (!e.onchipTiming || e.spmBanks!=4 || e.spmBankGranularity!=8)),
        "CFD SPM slot rotation requires timed four-bank SPM with eight-byte granularity");
    panic_if(e.luPivotPrefetch && (!e.luRank1 || !e.luReciprocalShare ||
        !e.sharedCluster || !e.rfReadWords || !e.rfWriteWords),
        "CFD LU pivot prefetch requires rank1, shared LU reciprocals and paid RF");
    panic_if(e.rhsFrontierReserve && (!e.sharedCluster || e.lineOptLevel<13 ||
        e.rhsSchedule!=RhsSchedule::RhsFrontier || !e.rfReadWords || !e.rfWriteWords),
        "CFD RHS frontier reservation requires the bounded direct frontier scheduler and paid RF");
    panic_if(e.luFmaCount && (e.luFmaCount<4 || e.luFmaCount>16 || !e.luRank1 ||
        !e.sharedCluster || !e.rfReadWords || !e.rfWriteWords),
        "CFD LU FMA requires rank1, shared cluster and paid RF ports; count 4..16");
    panic_if(e.rhsFmaCount && (e.rhsFmaCount<4 || e.rhsFmaCount>16 ||
             !e.sharedCluster || !e.rhsRowParallel || !e.rfReadWords ||
             !e.rfWriteWords || e.arithmeticMode!=CfdArithmeticGroups::Legacy ||
             e.coldBTree || e.coldRhsTree || e.warmRhsTree || e.rhsPair ||
             (e.rhsFactorFanout && !e.rhsFmaFactorFanout) || e.rhsTreeRfTemp),
             "CFD RHS FMA requires 4..16 real lanes, legacy shared finite RF and ordered unpaired RHS");
    panic_if(e.rhsFmaFactorFanout && (!e.rhsFactorFanout || !e.rhsFactorReuse ||
        !e.rhsFmaCount || !e.rhsUpperInverse || !e.rfReadWords || !e.rfWriteWords),
        "CFD FMA factor fanout requires an enabled paid factor bus, upper inverse and RHS FMA");
    panic_if(e.rhsFactorReuse && (!e.localProducts || !e.luReciprocalShare ||
             e.rhsPair || e.rhsSchedule != RhsSchedule::RhsFrontier),
             "CFD RHS factor reuse requires local row products, reciprocals, RHS frontier and no pairs");
    panic_if(e.rhsFactorFanout && (!e.rhsFactorReuse || !e.rfReadWords ||
             !e.rfWriteWords || e.rhsPair),
             "CFD factor fanout requires paid finite RF, retained factors and no RHS pair");
    panic_if(e.rhsFactorFanoutFeedback && !e.rhsFactorFanout,
             "CFD feedback factor fanout policy requires an enabled factor bus");
    panic_if(e.rhsTreeRfTemp && (!e.localProducts || !e.rhsRowParallel ||
             !e.sharedCluster || !e.rfReadWords || !e.rfWriteWords ||
             e.arithmeticMode != CfdArithmeticGroups::Legacy),
             "CFD RHS tree RF temporary requires local products and legacy shared finite RF");
    panic_if(e.rhsTreeRfFirstColumn && !e.rhsTreeRfTemp,
             "CFD first-column RF policy requires RHS tree RF temporaries");
    panic_if(e.luControlFold && (!e.sharedCluster || !e.rhsRowParallel ||
             !e.localProducts || !e.luReciprocalShare || !e.rfReadWords ||
             !e.rfWriteWords || e.lineOptLevel<7 ||
             e.arithmeticMode!=CfdArithmeticGroups::Legacy),
             "CFD LU control folding requires legacy timed shared RF, local row products and reciprocals");
    panic_if(e.backwardIndependentReady && (!e.tiledLine || !e.onchipTiming ||
             e.lineOptLevel < 9 || !e.sharedCluster || !e.backwardFeedbackTree),
             "CFD independent backward readiness requires timed shared tiled feedback tree and operand slots");
    panic_if((e.backwardFeedbackTree && (!e.tiledLine || !e.lineMvmTree || !e.sharedCluster ||
                 e.dedicatedFinalSubtract || !e.onchipTiming)) ||
             ((e.coldBTree || e.coldRhsTree) && (!e.sharedCluster || !e.rhsRowParallel)) ||
             e.coldRhsTree > 3 ||
             e.luLookahead > 2 ||
             (e.luLookahead && (!e.tiledLine || !e.sharedCluster ||
                 e.lineOptLevel < 7 || e.rhsSchedule != RhsSchedule::RhsFrontier)) ||
             (e.tailUbarOverlap && (!e.tiledLine || !e.onchipTiming ||
                 !e.timedInterface || e.lineOptLevel < 10 ||
                 !e.sharedCluster || e.tailUbarOverlap >= e.solveCount ||
                 e.tailUbarOverlap >= e.inputSlots || e.tileBuffers != 2 ||
                 e.tailUbarOverlap >= e.lineTileCells)) ||
             (e.rhsSupplyPriority && e.rhsSchedule != RhsSchedule::RhsFrontier) ||
             (e.clusterMulCount != N && !e.sharedCluster),
             "CFD compute optimization requires the shared timed tree/frontier path");
    panic_if((e.coldForwardTree && (!e.warmForwardTree || e.laneMvm)) ||
             (e.warmForwardTree && (!e.lineMvmTree || !e.sharedCluster)) ||
             (e.warmRhsTree && (!e.rhsRowParallel || !e.sharedCluster)) ||
             ((e.warmForwardTree || e.warmRhsTree || e.ageDrains) &&
              (!e.tiledLine || !e.onchipTiming || !e.timedInterface)) ||
             !e.drainArbLatency || !e.drainArbIi,
             "CFD frontier optimization requires bounded shared timed resources");
    panic_if(e.dataCommitDepth && (!e.tiledLine || !e.onchipTiming ||
             !e.timedInterface || !e.dmaSpm || e.lineOptLevel < 11 ||
             e.dataCommitDepth > e.outputDepth),
             "CFD data commit credits require timed tiled DMA and reserved output slots");
    e.traceCells = std::max<uint64_t>(1,
        envU64("GEM5_CFD_COEFF_TRACE_CELLS", 4));
    e.traceFile = std::getenv("GEM5_CFD_COEFF_TRACE_FILE");
    if (!e.traceFile || !*e.traceFile)
        e.traceFile = "m5out/coeff_preprocess_trace.csv";
    const uint64_t warmRhsWidth = envU64("GEM5_CFD_SME_WARM_RHS_WIDTH", 0);
    panic_if(warmRhsWidth && (!smeZaEnabled || warmRhsWidth < 2 || warmRhsWidth > N),
             "CFD packed warm RHS requires SME and width 2..5");
    e.smeWarmRhsWidth = warmRhsWidth;
    e.smeWarmVerticalRhs = envBool("GEM5_CFD_SME_WARM_VERTICAL_RHS", false);
    panic_if(e.smeWarmVerticalRhs && (!smeZaEnabled || warmRhsWidth),
             "CFD vertical RHS requires the single-point SME ZA layout");
    e.smeWarmSingleTile = envBool("GEM5_CFD_SME_WARM_SINGLE_TILE", false);
    panic_if(e.smeWarmSingleTile && !e.smeWarmVerticalRhs,
             "CFD single-tile warm SME folding requires vertical RHS");
    e.smeWarmBypass = envBool("GEM5_CFD_SME_WARM_BYPASS", false);
    panic_if(e.smeWarmBypass && !e.smeWarmVerticalRhs,
             "CFD post-rounding warm SME bypass requires vertical RHS");
    e.smeCriticalArb = envBool("GEM5_CFD_SME_CRITICAL_ARB", false);
    panic_if(e.smeCriticalArb && !e.smeWarmVerticalRhs,
             "CFD critical-frontier SME arbitration requires vertical RHS");
    e.smeWarmBackSplit = envBool("GEM5_CFD_SME_WARM_BACK_SPLIT", false);
    panic_if(e.smeWarmBackSplit && !smeZaEnabled,
             "CFD warm backward row split requires SME");
    const uint64_t warmBaseMode = envU64("GEM5_CFD_SME_WARM_BASE_MODE", 0);
    panic_if(warmBaseMode > unsigned(SmeWarmBaseMode::Adaptive),
             "CFD unknown SME warm base mode");
    e.smeWarmBaseMode = static_cast<SmeWarmBaseMode>(warmBaseMode);
    e.smeInvSparseIdentity = envBool("GEM5_CFD_SME_INV_SPARSE_I5", true);
    e.smeInvExpThresh = envU64("GEM5_CFD_SME_INV_EXP_THRESH", 20);
    e.smeReuseHint = envU64("GEM5_CFD_SME_REUSE_HINT", 0);
    e.smeAdaptiveK = envU64("GEM5_CFD_SME_ADAPTIVE_K", 0);
    panic_if(e.smeInvExpThresh > 2048,
             "CFD SME inverse exponent threshold must be in 0..2048");
    panic_if(e.smeWarmBaseMode != SmeWarmBaseMode::DirectLu &&
             (!smeZaEnabled || !e.smeWarmBypass),
             "CFD SME inverse warm base mode requires SME and post-rounding bypass");
    if (smeZaEnabled) {
        e.smeContexts = envU64("GEM5_CFD_SME_CONTEXTS", 4);
        panic_if(e.smeWarmRhsWidth && !CfdSmeZa::warmRhsGroups(e.smeContexts),
                 "CFD packed warm RHS requires at least two SME contexts");
        e.smeEarlyPanels = envBool("GEM5_CFD_SME_EARLY_PANELS", false);
        e.smeChainSteps = envBool("GEM5_CFD_SME_CHAIN_STEPS", false);
        e.smeSplitRhs = envBool("GEM5_CFD_SME_SPLIT_RHS", false);
        const uint64_t upperMode = envU64("GEM5_CFD_SME_UBAR_MODE", 0);
        panic_if(upperMode > unsigned(SmeUbarMode::Deferred),
                 "CFD unknown SME Ubar mode");
        e.smeUbarMode = static_cast<SmeUbarMode>(upperMode);
        e.smeReverseContexts = envU64("GEM5_CFD_SME_REVERSE_CONTEXTS", 0);
        const uint64_t upperSplit = envU64("GEM5_CFD_SME_UPPER_SPLIT", 3);
        e.smeReverseWavefront = envBool("GEM5_CFD_SME_REVERSE_WAVEFRONT", false);
        panic_if(upperSplit < 3 || upperSplit > N ||
                 (!e.smeReverseContexts && (upperSplit != 3 || e.smeReverseWavefront)),
                 "CFD SME upper split must be 3..5 for deferred Ubar");
        e.smeUpperSplit = upperSplit;
        panic_if((e.smeUbarMode == SmeUbarMode::Deferred) !=
                 (e.smeReverseContexts != 0) ||
                 e.smeReverseContexts > e.backwardOperandDepth,
                 "CFD deferred Ubar needs bounded reverse ZA contexts");
        auto &s = e.smeConfig;
        s.lanes = envU64("GEM5_CFD_SME_LANES", 16);
        s.readPorts = envU64("GEM5_CFD_SME_READ_PORTS", 8);
        s.writePorts = envU64("GEM5_CFD_SME_WRITE_PORTS", 4);
        s.depth = envU64("GEM5_CFD_SME_DEPTH", 32);
        s.ingressPorts = envU64("GEM5_CFD_SME_INGRESS_PORTS", 0);
        s.fmaLatency = envU64("GEM5_CFD_SME_FMA_LATENCY", 5);
        const uint64_t arrayModeRaw = envU64("GEM5_CFD_SME_ARRAY_MODE", 0);
        panic_if(arrayModeRaw > unsigned(CfdSmeZa::ArrayMode::TwoDRs),
                 "CFD unknown SME array mode");
        s.arrayMode = static_cast<CfdSmeZa::ArrayMode>(arrayModeRaw);
        const unsigned defaultSeg =
            s.arrayMode == CfdSmeZa::ArrayMode::TwoDBd ? 3u :
            s.arrayMode == CfdSmeZa::ArrayMode::TwoDRs ? 2u : 1u;
        s.maxSegments = envU64("GEM5_CFD_SME_2D_SEGMENTS", defaultSeg);
        s.panelPack = envBool("GEM5_CFD_SME_2D_PANEL_PACK", false);
        s.mixedKind = envBool("GEM5_CFD_SME_2D_MIXED_KIND", false);
        e.smePanelBOverlap = envBool("GEM5_CFD_SME_2D_PANEL_B_OVERLAP", false);
        e.smeForwardTileRotate = envBool("GEM5_CFD_SME_FORWARD_TILE_ROTATE", false);
        e.sme2dCandidateWindow = envU64("GEM5_CFD_SME_2D_CANDIDATE_WINDOW", 0);
        e.smeFeedbackReg = envBool("GEM5_CFD_SME_FEEDBACK_REG", false);
        panic_if(s.arrayMode == CfdSmeZa::ArrayMode::OneD &&
                 (s.maxSegments != 1 || s.panelPack || s.mixedKind ||
                  e.smePanelBOverlap || e.sme2dCandidateWindow),
                 "CFD SME 2D options require a 2D array mode");
        panic_if(s.arrayMode != CfdSmeZa::ArrayMode::OneD && e.smeChainSteps,
                 "CFD SME chain-steps is only valid in 1D array mode");
        panic_if(e.smePanelBOverlap && !s.panelPack,
                 "CFD SME 2D panel-B overlap requires panel-pack");
        panic_if(e.smeForwardTileRotate &&
                 (!e.tilePrefetchBuffers || e.lineOptLevel < 10),
                 "CFD SME forward tile rotation requires prefetch tile buffers and opt level >= 10");
        panic_if(e.sme2dCandidateWindow > 32,
                 "CFD SME 2D candidate window must be in 0..32");
        panic_if(s.arrayMode != CfdSmeZa::ArrayMode::OneD && s.lanes != 64,
                 "CFD SME 2D array mode requires 64 PE lanes (8x8 grid)");
        panic_if(s.arrayMode == CfdSmeZa::ArrayMode::TwoDBase && s.maxSegments != 1,
                 "CFD SME 2d-base mode requires maxSegments == 1");
        panic_if(s.arrayMode == CfdSmeZa::ArrayMode::TwoDBd &&
                 (s.maxSegments < 2 || s.maxSegments > 4),
                 "CFD SME 2d-bd mode requires maxSegments in 2..4");
        panic_if(s.arrayMode == CfdSmeZa::ArrayMode::TwoDRs &&
                 (s.maxSegments < 2 || s.maxSegments > 3),
                 "CFD SME 2d-rs mode requires maxSegments in 2..3");
        panic_if(s.mixedKind && s.arrayMode != CfdSmeZa::ArrayMode::TwoDRs,
                 "CFD SME 2D mixed-kind issue requires 2d-rs mode");
        panic_if(s.panelPack && s.arrayMode != CfdSmeZa::ArrayMode::TwoDRs,
                 "CFD SME 2D panel-pack requires 2d-rs mode");
        panic_if(s.panelPack && e.smeSplitRhs,
                 "CFD SME 2D panel-pack requires split-rhs=0");
        s.mulLatency = e.clusterMulLatency;
        s.addLatency = e.clusterAddLatency;
        s.divLatency = e.clusterDivLatency;
        s.divIi = e.clusterDivIi;
        panic_if(s.fmaLatency < 1 || s.fmaLatency > 16,
                 "CFD SME FMA latency must be in 1..16");
        panic_if(e.smeWarmBackSplit && (e.smeContexts < 2 || !s.ingressPorts ||
                 !e.backwardIndependentReady || e.backwardOperandDepth < 2),
                 "CFD warm backward row split requires two contexts and timed operands");
        panic_if(e.smeContexts < 1 || e.smeContexts > 8 ||
                 s.lanes < 6 || s.lanes > 64 || s.readPorts < 3 ||
                 s.readPorts > 32 || s.writePorts < 1 || s.writePorts > 16 ||
                 s.depth < 1 || s.depth > 128 || s.ingressPorts > 4,
                 "CFD SME array capacity is outside its physical bounds");
        panic_if(!e.sharedCluster || !e.tiledLine || !e.timedInterface ||
                 !e.dmaSpm || !e.onchipTiming || !e.warmReciprocal ||
                 !e.reciprocalSolve || !e.luForwarding || e.lineOptLevel != 13 ||
                 (e.smeEarlyPanels && !e.inputBatchReady) || e.tailUbarOverlap,
                 "CFD SME requires level-13 tiled DMA and reciprocal LU");
    }
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
    StreamUpper,
};

enum class DrainType : uint8_t {
    Lu = 0, DInv = 1, LBar = 2, UBar = 3, BaseVector = 4,
    CorrectionVector = 5, DqStarVector = 6
};
enum class OpKind : uint8_t {
    LuDiv, LuMul, LuSub, Div, Mul, Sub, ColumnFma, ForwardCombine,
    LineProduct, LineAccumulate, SharedMul, SharedAdd, SharedDiv,
    FinalSubtract, SharedFma, LuFma
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
    bool backwardWaiting = false;
    RhsPhase phase = RhsPhase::ForwardDiv;
    int k = 0;
    int i = 1;
    double product = 0.0;
    uint64_t readySinceCycle = 0;
    uint8_t treeIssued = 0, treeReady = 0, normalizedMask = 0;
    std::array<double, N> value = {};
};

static_assert(sizeof(RhsState)==80,"streaming RHS state must fit its existing reservation");

static uint8_t
readyRhsTreeNodes(const RhsState &rhs)
{
    const unsigned terms = N-rhs.k-1;
    uint8_t ready = 1;
    if (terms >= 3 || (rhs.treeReady & 1)) ready |= 2;
    if ((rhs.treeReady & 3) == 3) ready |= 4;
    if (rhs.treeReady & 4) ready |= 8;
    return ready & ((1u << terms)-1) & ~rhs.treeIssued;
}

struct RhsRowProduct
{
    std::array<double, N> value{};
    double treePartial = 0;
    uint64_t token = 0;
    unsigned rhs = 0;
    bool valid = false;
    bool treePartialValid = false;
};
static_assert(sizeof(RhsRowProduct) * 60 <= 4096);

struct InterfaceTransfer
{
    Addr addr = 0;
    bool write = false;
    std::string kind;
    bool interface = true;
    uint64_t cell = 0;
    std::vector<uint8_t> bytes;
    uint64_t issued = 0, completed = 0;
    uint64_t packetsIssued = 0, packetsCompleted = 0, lastResponse = 0;
};

struct ColdColumnProduct
{
    std::array<double, N> value{};
    bool ready = false, addIssued = false, live = true;
};

struct OnchipAccess
{
    uint64_t id = 0, owner = 0, readyCycle = 0, eligibleCycle = 0;
    Addr base = 0;
    unsigned words = 0, stride = 8, next = 0, priority = 0;
    bool write = false, active = true, done = false;
    std::function<void()> completion;
};
static_assert(sizeof(OnchipAccess) + 128 <= 256);

struct OnchipContext
{
    std::shared_ptr<OnchipAccess> inputRead;
    std::shared_ptr<OnchipAccess> dRead;
    std::shared_ptr<OnchipAccess> cacheRead;
    bool cacheReadyTraced = false;
    std::array<std::shared_ptr<OnchipAccess>, 3> commit;
};
static_assert(sizeof(OnchipContext) <= 128);

struct DetachedRecord
{
    uint64_t owner = 0, token = 0, requestId = 0;
    uint64_t generation = 0, lineId = 0, cell = 0;
    std::shared_ptr<InterfaceTransfer> transfer;
};
static_assert(sizeof(DetachedRecord) + sizeof(InterfaceTransfer) +
              sizeof(CfdCoeffCompactRecord) <= 1024);
static_assert(sizeof(DetachedRecord) + sizeof(InterfaceTransfer) +
              sizeof(CfdCoeffPackedRecord) + 128 <= 512,
              "update packed completion reservation");

struct Request
{
    uint64_t token = 0;
    CfdCoeffPreprocessDescriptor desc = {};
    CfdCoeffPreprocessRecord record = {};
    std::shared_ptr<InterfaceTransfer> recordTransfer;
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
    uint16_t dBytesCompleted = 0;
    bool inputDReady = false, inputOperandsReady = false;
    std::array<uint16_t,3> inputBatchBytes{}; // L, U, R: paid store completions.
    uint8_t inputBatchNext = 1;
    bool inputLowerReady = false, inputUpperReady = false, inputBatchLoadsDone = false;
    bool luWaiting = false;
    uint64_t luControlParent = 0, luControlReturned = 0;
    bool luDrained = false;
    bool luDone = false;
    bool solveStarted = false;
    bool solveDone = false;
    bool smeDeferUpper = false, smeUpperPartial = false;
    int upperSlot = -1;
    bool solveParked = false;
    uint64_t rhsResumeReadyCycle = 0;
    unsigned rhsCount = 0;
    unsigned rhsSlots = 0;
    unsigned rhsCompleted = 0;
    unsigned schedulerCursor = 0;
    uint8_t requestedBatchMask = 0;
    uint8_t matrixDrainMask = 0;
    uint16_t rhsEnabledMask = 0;
    uint16_t rhsInitializedMask = 0;
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
    uint8_t drainReadyMask = 0;
    std::array<uint64_t, 7> drainReadySince = {};
    std::array<double, N * N> d = {};
    std::array<double, N * N> lower = {};
    std::array<double, N * N> upper = {};
    std::array<double, N * N> lu = {};
    std::array<double, N * N> dInv = {};
    std::array<double, N * N> lBar = {};
    std::array<double, N * N> uBar = {};
    bool baseConsumerEnabled = false;
    bool directBaseSolve = false;
    bool coefficientCacheReuse = false;
    bool cachedReciprocal = false;
    bool smeInvRequested = false;
    bool smeInvDone = false;
    bool smeInvUseCache = false;
    bool smeInvRejected = false;
    bool cacheUbarReload = false;
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
    bool tileDataCommitted = false;
    // A submitted request retains a reserved state slot and all DMA callback
    // identity, but no longer owns a compute-window credit. Never a new cache.
    bool dataCommitDetached = false;
    bool cacheUbarPublished = false;
    bool localTilePublished = false;
    bool localYPublished = false;
    bool tailUbarMarked = false, tailUbarReleased = false;
    std::array<std::array<std::shared_ptr<ColdColumnProduct>, N>, 2> coldProducts;
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
    std::array<double, N> luTemps{}, luProducts{};
    CfdLuProducts luProductWindow;
    CfdLuUpper luUpper;
    uint8_t luDivIssued = 0, luDivCompleted = 0;
    uint64_t luVisitedCycle = UINT64_MAX;
    std::array<RhsState, 15> rhs = {};
    std::array<double, N> reciprocal = {};
    std::array<bool, N> reciprocalReady = {};
    std::array<bool, N> reciprocalPending = {};
};
static_assert(sizeof(Request) <= 8192, "update reserved request-state bytes");

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
    uint8_t treeIssued = 0, treeReady = 0;
    std::array<std::array<double, N>, 4> partial = {};
    uint64_t memoryCell = UINT64_MAX;
    unsigned memoryKind = 3;
    std::array<std::shared_ptr<OnchipAccess>, N> loads;
    std::shared_ptr<OnchipAccess> yLoad;
    std::array<std::array<double,N>,N> columnOperands{};
    uint8_t loadedColumns = 0;
    // Ten bounded nodes: five products and up to five signed reduction nodes.
    std::array<uint8_t, 10> laneIssued{}, laneReady{};
    std::array<std::array<uint64_t, N>, 10> laneReadyCycle{};
    std::array<uint64_t, N> laneOperandCycle{};
    std::array<bool, N> laneOperands{};
};

// Own the bytes until every timing response returns. issueDma uses dataStatic,
// so callbacks retain this object even if a cancelled line is reaped.
struct LineOutputWrite
{
    uint64_t cell = 0;
    unsigned issued = 0;
    unsigned completed = 0;
    uint64_t readyCycle = 0;
    std::array<double, N> value = {};
};
static_assert(sizeof(LineOutputWrite) <= 64);

struct LineOutputPacket
{
    struct Slice {
        std::shared_ptr<LineOutputWrite> write;
        unsigned offset = 0, bytes = 0;
    };
    std::array<uint8_t, 64> payload{};
    std::array<Slice, 3> slices;
    unsigned count = 0, bytes = 0;
};
static_assert(sizeof(LineOutputPacket) <= 192,
              "update output packet payload/routing reservation");

// A tile owns DMA bytes through response completion. No array here grows with
// line length; DRAM backing addresses live in the guest line descriptor.
struct LineTile
{
    enum class Phase { Idle, Forward, Write, Resident, Read };
    Phase phase = Phase::Forward;
    unsigned storageIndex = 0;
    uint64_t start = 0, count = 0, retired = 0;
    uint64_t lastWriteCycle = 0;
    std::array<uint64_t, 16> readyMask{};
    uint64_t readyCount = 0;
    std::vector<std::array<double, N>> y;
    std::vector<std::array<double, N * N>> u;
    uint64_t issued[2] = {}, completed[2] = {};
};
static_assert(sizeof(LineTile) <= 512, "update prefetch tile metadata budget");

struct SmeUpperFactors
{
    static constexpr unsigned MaxWords = 13;
    std::array<double, MaxWords> value{};

    static unsigned lowerWords(unsigned split)
    {
        const unsigned tail = N-split;
        return tail*(tail+1)/2;
    }
    static unsigned words(unsigned split)
    {
        return lowerWords(split) + N*(N-1)/2;
    }
    // Reciprocals and the strict lower tail precede the strict upper rows.
    static unsigned upperIndex(unsigned row, unsigned col, unsigned split)
    {
        return lowerWords(split) + row*(2*N-1-row)/2 + col-row-1;
    }
};
static_assert(N == 5 && sizeof(SmeUpperFactors) == 104);

// Two register slots overlap a point's arithmetic with the next SPM read.
struct BackwardOperand
{
    uint64_t cell = UINT64_MAX;
    std::shared_ptr<LineTile> tile;
    std::shared_ptr<OnchipAccess> yLoad;
    std::array<std::shared_ptr<OnchipAccess>, N> columns;
    std::array<double, N> y{};
    std::array<std::array<double, N>, N> values{};
    SmeUpperFactors smeFactors;
    uint8_t readyColumns = 0, usedColumns = 0;
    bool yReady = false, smePackedReady = false;
};
static_assert(sizeof(BackwardOperand) <= 512);

// Fixed input/base credits; opt level 5 separates the bounded write queue.
struct WarmSlot
{
    uint64_t cell = 0;
    uint64_t rhsIssueCycle = 0;
    bool valid = false, baseReady = false;
    std::array<double, N> base{};
    std::shared_ptr<InterfaceTransfer> rhs, y;
    std::shared_ptr<OnchipAccess> rhsLoad;
    std::shared_ptr<LineTile> tile;
};
static_assert(sizeof(WarmSlot)<=128,"update bounded warm-slot reservation");

struct WarmWrite
{
    std::shared_ptr<InterfaceTransfer> transfer;
    std::shared_ptr<LineTile> tile;
    std::shared_ptr<OnchipAccess> localStore, localRead;
    bool localPublished = false;
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
    uint64_t dataCommitLive = 0;
    uint64_t completedChildren = 0;
    uint64_t committedChildren = 0;
    uint64_t coefficientVersion = 0;
    bool coefficientCacheEnabled = false;
    bool coefficientDirty = false;
    bool coefficientCacheHit = false;
    std::shared_ptr<LineTile> tile, prefetchTile;
    std::array<std::shared_ptr<LineTile>, 4> aheadTiles;
    uint64_t cacheAllocated = 0;
    bool tileYOnly = false;
    bool localUbar = false, deferUpper = false;
    Addr smeUpperBase = 0;
    uint64_t smeUpperBytes = 0, smeUpperStored = 0, smeReverseDone = 0;
    uint64_t smeWarmNextCell = 0;
    bool smeInversePolicy = false;
    uint64_t smeInvCells = 0, smeInvFallbackCells = 0, smeInvBuiltCells = 0;
    uint64_t smePackedInvCells = 0, smeInvDiscardedMul = 0, smeInvDiscardedFma = 0;
    std::vector<double> smeUpperFactors;
    std::shared_ptr<InterfaceTransfer> rhsTransfer, yTransfer, uTransfer;
    std::array<double, N> cachedRhs = {}, previousY = {};
    std::array<WarmSlot, 8> warm;
    std::deque<WarmWrite> warmWrites;
    uint64_t recordsPending = 0, recordDetachStalls = 0;
    uint64_t warmInputStalls = 0, warmOutputStalls = 0;
    uint64_t warmBaseStalls = 0, warmTileStalls = 0;
    std::shared_ptr<OnchipAccess> tailRead;
    std::array<BackwardOperand, 8> backwardOperands;
    uint64_t backwardOperandAllocated = 0, backwardOperandReleased = 0;
    std::array<double,N> backwardYOperand{};
    uint64_t warmNextInput = 0, warmAheadInputs = 0, warmWritesMax = 0;
    uint64_t cacheReadIssued = 0, cacheReadCompleted = 0;
    uint64_t cacheWriteIssued = 0, cacheWriteCompleted = 0;
    uint64_t tileWaitCycles = 0;
    uint64_t tileReserve = 0, tileAllocated = 0;
    uint64_t tileWriteIssued = 0, tileWriteCompleted = 0;
    uint64_t tileReadIssued = 0, tileReadCompleted = 0;
    uint64_t tilePacketsIssued = 0, tilePacketsCompleted = 0;
    uint64_t tileLastResponse = 0, tileStalls = 0;
    uint64_t tileForwardCount = 0, tileReloadCount = 0;
    uint64_t nextForwardTileCell = 0;
    uint64_t backwardFeedbackReadyCycle = 0;
    std::array<uint64_t, 2> backwardPartReadyCycle{};
    std::shared_ptr<InterfaceTransfer> statusTransfer;
    std::shared_ptr<InterfaceTransfer> summaryTransfer;
    CfdCoeffLineSummary summary;
    uint64_t interfaceReadIssued = 0, interfaceReadCompleted = 0;
    uint64_t interfaceWriteIssued = 0, interfaceWriteCompleted = 0;
    uint64_t interfacePacketsIssued = 0, interfacePacketsCompleted = 0;
    uint64_t interfaceLastResponse = 0;
    std::array<double, N> backwardNext = {}, backwardValue = {};
    bool cachedForwardStarted = false;
    bool cachedForwardOpPending = false;
    bool cachedFinishWaiting = false;
    bool cachedSubtractReady = false;
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
    std::deque<std::shared_ptr<LineOutputWrite>> outputWrites;
    uint64_t outputBytesIssued = 0;
    uint64_t outputBytesCompleted = 0;
    uint64_t outputPacketsIssued = 0;
    uint64_t outputPacketsCompleted = 0;
    uint64_t outputQueueMax = 0;
    uint64_t outputQueueStalls = 0;
    uint64_t outputOutstandingStalls = 0;
    uint64_t outputLastResponseCycle = 0;
    uint64_t outputDigest = 0;
    uint64_t forwardDigestCells = 0;
    uint64_t backwardDigestCells = 0;
    bool backwardDirect = false;
    bool backwardStarted = false;
    bool earlyBackward = false;
    bool backwardOpPending = false;
    bool backwardFinishWaiting = false;
    bool backwardSubtractReady = false;
    int64_t backwardCell = -1;
    uint8_t backwardColumn = 0;
    std::vector<std::array<double, N>> dqStar;
    std::vector<std::array<double, N>> dq;
    std::vector<std::array<double, N>> rhs;
    std::vector<std::array<double, N * N>> dInv;
    std::vector<std::array<double, N * N>> lu;
    std::vector<std::array<double, N * N>> lBar;
    std::vector<std::array<double, N * N>> uBar;
    std::array<double, N> cachedBaseAccumulator = {};
    std::array<double, N> cachedCorrectionAccumulator = {};
    std::array<double, N> backwardAccumulator = {};
    bool terminal = false;
    bool abortPending = false;
    bool abortPublished = false;
};

struct LineCoeffCache
{
    bool privateLuOutput = false;
    bool localUbar = false;
    bool valid = false;
    // Solve-form LU uses Crout off-diagonals and reciprocal diagonal entries.
    // A tagged SME entry reuses the same matrix slot for the explicit inverse.
    bool reciprocalDiagonal = false;
    bool hasUbar = true;
    Addr ubarBacking = 0;
    uint64_t version = 0;
    uint64_t cells = 0;
    bool directBaseSolve = false;
    std::vector<std::array<double, N * N>> lu;
    std::vector<std::array<double, N * N>> dInv;
    std::vector<std::array<double, N * N>> lBar;
    std::vector<std::array<double, N * N>> uBar;
    std::vector<SmeCacheBase> smeBaseTags;
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

struct ResourcePool;
struct PendingOp
{
    uint64_t completeCycle = 0;
    OpKind kind = OpKind::Mul;
    std::function<void()> complete;
    unsigned lanes = 1;
    uint64_t dividerTag = 0;
    ResourcePool *rfPool = nullptr;
    uint64_t rfTag = 0;
    OpKind caller = OpKind::Mul;
    unsigned rfReadWords = 0, returnedLanes = 0;
    unsigned localReads = 0, localRow = 0;
    bool localResult = false, scalarBroadcast = false;
    bool bankAccess = false, bankStored = false, fuReleased = false;
    CfdLocalAccess bank{};
    uint64_t bankPublish = 0;
};

struct SharedVectorAdd
{
    std::array<double, N> product;
    std::function<void(const std::array<double, N> &)> complete;
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
    uint64_t rfLive = 0, rfPeak = 0;
};

// Simulation-only observation scope: no functional state or extra hardware
// credits. A request can visit several schedulers in one cycle.
struct DiagnosticOwner
{
    uint64_t &owner;
    uint64_t previous;
    DiagnosticOwner(uint64_t &slot, uint64_t token)
        : owner(slot), previous(slot) { owner = token; }
    ~DiagnosticOwner() { owner = previous; }
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
static_assert(sizeof(DrainTask) <= 64,"update drain descriptor reservation");

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

struct SmePanel
{
    enum Phase { Load, Forward, Backward, Export, Done };
    Phase phase = Load;
    unsigned step = 0, row = 0;
    bool scaled = false;
    uint8_t inverseMul = 0, inverseFma = 0;
};

struct SmeMagnitude
{
    uint64_t bits = 0;

    void
    includeRow(const double *values)
    {
        // Unsigned FP64 magnitude bits sort in numeric order. Infinities
        // and NaNs sort above every finite value and fail the guard.
        for (unsigned i = 0; i < N; ++i) {
            uint64_t raw;
            std::memcpy(&raw, values + i, sizeof(raw));
            bits = std::max(bits, raw & (UINT64_MAX >> 1));
        }
    }

    bool nonzeroFinite() const { return bits && (bits >> 52) < 0x7ff; }

    int
    exponent() const
    {
        double magnitude;
        std::memcpy(&magnitude, &bits, sizeof(magnitude));
        return std::ilogb(magnitude);
    }
};

struct SmeCell
{
    uint64_t token = 0;
    unsigned loaded = 0, pivot = 0, row = 0, exported = 0;
    bool divided = false, scaled = false, reciprocalsLoaded = false;
    std::array<SmePanel, 3> panels;
    SmePanel invPanel;
    uint16_t pivotExpMin = 0xFFFF, pivotExpMax = 0;
    uint8_t invStepA = 0, invStepB = 0;
    bool invMerged = false;
    uint8_t panelPackUpperMask = 0;
    SmeMagnitude dMagnitude, invMagnitude;
};
static_assert(sizeof(SmeCell) <= 128);

struct SmeReversePanel
{
    uint64_t owner = 0, cell = 0;
    unsigned loaded = 0;
    SmePanel state;
    std::array<uint8_t, N> nextUpper{};
    bool upperStarted = false;
};
static_assert(sizeof(SmeReversePanel) <= 128);

struct SmeScan
{
    uint64_t owner = 0, cell = 0, finalIssue = 0;
    uint8_t loaded = 0, products = 0, nodes = 0;
    bool baseLoaded = false;
};
static_assert(sizeof(SmeScan) <= 128);

struct SmeWarmRhs
{
    uint64_t first = 0;
    std::array<uint64_t, N> requests{};
    unsigned count = 0, loaded = 0, issued = 0, exported = 0;
};
static_assert(sizeof(SmeWarmRhs) <= 128);

struct SmeSplitBackPoint
{
    uint64_t cell = UINT64_MAX;
    std::array<double, N> value{};
    std::array<uint16_t, 2> issued{};
    std::array<uint64_t, 2> partReadyCycle{};
    uint8_t returned = 0;
};
static_assert(sizeof(SmeSplitBackPoint) <= 128);

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
    CfdSmeZa smeZa;
    std::vector<SmeCell> smeCells;
    std::vector<SmeCell> smeWarmSingleCells;
    std::vector<SmeReversePanel> smeReversePanels;
    std::vector<SmeWarmRhs> smeWarmRhs;
    std::array<SmeSplitBackPoint, 2> smeSplitBack;
    SmeScan smeForward, smeBackward;
    uint64_t smeStart = 0, smeOwner = 0, smeForwardCells = 0;
    void processSmeRequests(uint64_t now);
    void processSmeCriticalRhs(uint64_t now);
    void bindSmeCachedLu(Request &, uint64_t now);
    void processSmeWarmRhs(LineRequest &, uint64_t now);
    void advanceSmeWarmRhs(LineRequest &, unsigned group, bool &gather, uint64_t now);
    bool releaseSmeWarmRhs(LineRequest &, unsigned group, const char *event);
    void traceSmeWarmRhs(const LineRequest &, unsigned group, const char *event,
                         unsigned index = 0) const;
    void processSmeSplitBack(LineRequest &);
    bool releaseSmeSplitBack(const LineRequest &, SmeSplitBackPoint &, const char *event);
    void traceSmeSplitBack(const LineRequest &, uint64_t cell, const char *event,
                           unsigned part = 0, unsigned op = 0) const;
    void advanceSmeLu(Request &, SmeCell &, unsigned tile, uint64_t now);
    void advanceSmeInversePanel(Request &, SmeCell &, unsigned luTile, uint64_t now);
    void advanceSmeInverseWarmRhs(Request &, SmeCell &, unsigned tile,
                                  unsigned slotIdx, uint64_t now,
                                  bool reserveForward = false);
    bool wantColdInverse(const LineRequest &line) const;
    bool smeUsesDualBypass() const
    {
        return env.smeWarmBaseMode == SmeWarmBaseMode::InverseFma2;
    }
    unsigned smeWarmContextCount() const
    {
        return env.smeWarmSingleTile ? 2 * env.smeContexts : env.smeContexts;
    }
    void advanceSmeVerticalRhs(Request &, SmePanel &, unsigned luTile,
                               unsigned slotIdx, uint64_t now);
    void advanceSmePanel(Request &, SmePanel &, unsigned luTile,
                         unsigned panel, uint8_t mask, uint64_t now);
    bool advanceSmeTriangular(SmePanel &, unsigned tile, unsigned luTile,
                              uint8_t mask, unsigned forwardEnd, bool stopAfterForward,
                              CfdSmeZa::Phase, CfdCoeffPreprocessRecord *);
    void processSmeReversePanels();
    bool advanceSmeReversePanel(LineRequest &, BackwardOperand &,
                                SmeReversePanel &, unsigned tile);
    bool advanceSmeReverseWavefront(SmeReversePanel &, unsigned tile);
    void traceSmeUpperMatrix(const LineRequest &, uint64_t cell, const char *kind,
                             const std::array<double, N*N> &matrix) const;
    unsigned smeUpperFactorWords() const
    {
        return env.smeReverseContexts ? SmeUpperFactors::words(env.smeUpperSplit) : 0;
    }
    Addr smeUpperAddr(const LineRequest &line, uint64_t cell) const
    {
        return line.smeUpperBase + cell * (MatrixBytes + 8*smeUpperFactorWords());
    }
    bool processSmeForward(uint64_t now);
    bool processSmeBackward(LineRequest &, uint64_t now);
    void stageSmeScanColumn(SmeScan &, unsigned tile, unsigned column,
                            const CfdSmeZa::Row &, double feedback,
                            CfdSmeZa::Phase);
    bool advanceSmeScan(SmeScan &, unsigned tile, CfdSmeZa::Phase,
                        const std::array<double, N> &feedback,
                        const std::array<double, N> *base, uint64_t now,
                        CfdSmeZa::Row &result);
    static uint64_t outputWordDigest(uint32_t phase, uint64_t cell,
                                     uint32_t lane, uint64_t bits)
    {
        uint64_t x = bits ^ ((uint64_t(phase) + 1ULL) * 0x9e3779b97f4a7c15ULL) ^
                     ((cell + 1ULL) * 0xbf58476d1ce4e5b9ULL) ^
                     ((uint64_t(lane) + 1ULL) * 0x94d049bb133111ebULL);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    void recordForwardOutputDigest(LineRequest &line, uint64_t cell,
                                   const std::array<double, N> &vec)
    {
        for (unsigned lane = 0; lane < N; ++lane) {
            uint64_t bits = 0;
            std::memcpy(&bits, &vec[lane], sizeof(bits));
            line.outputDigest += outputWordDigest(0u, cell, lane, bits);
        }
        ++line.forwardDigestCells;
    }
    void recordBackwardOutputDigest(LineRequest &line, uint64_t cell,
                                    const std::array<double, N> &vec)
    {
        for (unsigned lane = 0; lane < N; ++lane) {
            uint64_t bits = 0;
            std::memcpy(&bits, &vec[lane], sizeof(bits));
            line.outputDigest += outputWordDigest(1u, cell, lane, bits);
        }
        ++line.backwardDigestCells;
    }
    std::vector<Sme2dCandidateMeta> sme2dOfferMetas;
    std::vector<std::function<void()>> sme2dOfferCommits;
    std::unordered_set<uint64_t> sme2dCriticalOffered;
    bool offerSme2d(const CfdSmeZa::Segment2d &, unsigned priority,
                    unsigned critGroup, std::function<void()> commit);
    void commitSme2dBundle();
    void processSmeCriticalRhs2d(uint64_t now);
    void processSmeRequests2d(uint64_t now);
    void processSmeWarmRhs2d(LineRequest &, uint64_t now);
    void processSmeSplitBack2d(LineRequest &);
    void advanceSmeLu2d(Request &, SmeCell &, unsigned idx, unsigned tile,
                        uint64_t now, bool &offered);
    void advanceSmePanel2d(Request &, SmeCell &, unsigned idx, unsigned panelSlot,
                           unsigned luTile, unsigned panel, uint8_t mask,
                           uint64_t now, bool &offered);
    void advanceSmePackedPanelA2d(Request &, SmeCell &, unsigned idx,
                                  unsigned luTile, uint64_t now, bool &offered);
    void advanceSmePackedPanelB2d(Request &, SmeCell &, unsigned idx,
                                  unsigned luTile, uint64_t now, bool &offered);
    void advanceSmeInversePanel2d(Request &, SmeCell &, unsigned idx,
                                  unsigned luTile, uint64_t now, bool &offered);
    void advanceSmeVerticalRhs2d(Request &, SmeCell &, unsigned idx,
                                 unsigned luTile, uint64_t now,
                                 bool singleTilePool, unsigned priority,
                                 unsigned critGroup, bool &offered);
    void advanceSmeInverseWarmRhs2d(Request &, SmeCell &, unsigned idx,
                                    unsigned tile, uint64_t now,
                                    bool singleTilePool, unsigned priority,
                                    unsigned critGroup, bool &offered);
    bool advanceSmeReversePanel2d(LineRequest &, BackwardOperand &,
                                  SmeReversePanel &, unsigned panelIdx,
                                  unsigned tile);
    void stageSmeScanColumn2d(SmeScan &, unsigned tile, unsigned column,
                              const CfdSmeZa::Row &, double feedback,
                              CfdSmeZa::Phase);
    bool advanceSmeScan2d(SmeScan &, unsigned tile, CfdSmeZa::Phase,
                          const std::array<double, N> &feedback,
                          const std::array<double, N> *base, uint64_t now,
                          CfdSmeZa::Row &result);
    void sampleSme(const LineRequest &);
    void reportSme(const LineRequest &, uint64_t now, bool aborted);
    // Event counts (request/RHS visits), not mutually exclusive wall cycles.
    std::unordered_map<std::string, uint64_t> diagnostics;
    std::unordered_map<std::string, uint64_t> waitCycles, waitThisCycle;
    std::array<uint16_t, 10> rejectThisCycle{}, criticalRejectThisCycle{};
    std::array<std::array<uint64_t, 3>, 10> rejectCycles{}, criticalRejectCycles{};
    uint64_t diagnosticOwner = 0, diagnosticFrontier = 0;
    uint64_t frontierSamples = 0;
    std::unordered_map<std::string, uint64_t> frontierStates;
    void observe(const char *reason) {
        if (env.resourceStats) {
            diagnostics[reason]++;
            waitThisCycle[reason] = 1;
            if (diagnosticOwner)
                waitThisCycle[std::string(diagnosticOwner == diagnosticFrontier ?
                    "frontier_request/" : "background_request/")+reason] = 1;
        }
    }
    ThreadContext *tc = nullptr;
    uint64_t nextToken = 1;
    uint64_t nextRequestId = 1;
    uint64_t lastTiledGeneration = 0;
    bool configured = false;
    FILE *traceFp = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<Request>> requests;
    std::unordered_map<uint64_t, std::unique_ptr<LineRequest>> lineRequests;
    std::unordered_map<uint64_t, std::shared_ptr<InterfaceTransfer>> pendingLineLaunches;
    std::unordered_map<uint64_t, bool> cancelledLineLaunches;
    std::unordered_map<uint64_t, uint64_t> childLineTokens;
    std::unordered_map<uint64_t, LineCoeffCache> lineCoeffCaches;
    std::vector<bool> slots;
    std::vector<PendingOp> pendingOps;
    CfdRegisterPorts registerPorts;
    std::array<CfdRegisterPorts, N> lanePorts;
    CfdLocalVectorPorts localVectors;
    std::vector<uint64_t> localLuOwners;
    unsigned vectorMulGrants = 0, vectorAddGrants = 0;
    unsigned laneGrants = 0;
    uint64_t nextRfTag = 1;
    std::deque<DrainTask> drainQueue;
    std::vector<DrainTask> activeDrains;
    uint64_t drainTaskPeak = 0, drainActivePeak = 0;
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
    ResourcePool finalSubtract;
    ResourcePool sharedMul, sharedAdd, sharedDiv, sharedFma, sharedLuFma;
    CfdArithmeticGroups arithmeticGroups;
    std::array<std::function<void()>, CfdArithmeticGroups::MaxGroups> arithmeticCallbacks;
    unsigned arithmeticAcceptedNow = 0;
    uint64_t arithmeticIssued = 0, arithmeticCompleted = 0, arithmeticFull = 0;
    bool issueArithmeticGroup(ResourcePool &, unsigned, OpKind, bool,
                              uint64_t, std::function<void()>);
    void processArithmeticGroups(uint64_t now);
    void completeArithmeticGroups(uint64_t now);
    CfdFp64Divider hardwareDivider;
    std::optional<CfdFp64Divider::Transaction> dividerResponse;
    uint64_t nextDividerTag = 1, nextDividerWriteback = 0, dividerDropped = 0;
    unsigned dividerFlags = 0;
    FILE *dividerTraceFp = nullptr;
    std::array<RhsRowProduct, 60> rhsRowProducts;
    unsigned rhsTreeTempLive = 0, rhsTreeTempPeak = 0;
    CfdRhsPairs rhsPairs;
    CfdRhsUpper rhsUpper;
    CfdRhsFactors rhsFactors;
    CfdRhsFactorFanout rhsFactorFanout;
    std::vector<CfdLuRank1> luRankStates;
    void clearRhsFactors(Request &req, uint64_t now);
    void traceRhsFactor(const Request &req, unsigned slot, const char *event,
                        uint64_t now, uint64_t operation, unsigned rhs);
    void traceRhsFactorFanout(const Request &req, const char *event,
        const CfdRhsFactorFanout::Transfer &source, uint64_t now,
        uint64_t operation, unsigned rhs);
    bool selectRhsPair(Request &req,unsigned index,uint64_t now);
    void processRhsPairs(uint64_t now,std::unordered_map<uint64_t,bool> &selected);
    void cancelRhsPairs(Request &req);
    void traceRhsPair(const CfdRhsPairs::Slot &s,unsigned slot,
                      const char *event,uint64_t now) const;
    uint64_t rowGroupsIssued = 0, rowGroupsCompleted = 0;
    uint64_t rowGroupsPeak = 0, rowGroupsStalls = 0;
    std::deque<SharedVectorAdd> sharedVectorAdds;
    std::deque<DetachedRecord> detachedRecords;
    CfdSummaryHash summaryHash;
    CfdSummarySnapshots summarySnapshots;
    std::vector<CfdSummaryHash> summaryEngines;
    uint64_t snapshotIssued = 0, snapshotCompleted = 0, snapshotDropped = 0;
    bool summaryBusy() const { return summaryHash.live || summarySnapshots.live; }
    void processSummarySnapshots(uint64_t now);
    uint64_t detachedPeak = 0, detachedIssued = 0, detachedCompleted = 0;
    uint64_t detachedCursor = 0;
    uint64_t dataCommitIssued = 0, dataCommitReleased = 0;
    uint64_t dataCommitPeak = 0, dataCommitFull = 0;
    uint64_t drainArbNext = 0, drainArbGranted = 0;
    uint64_t drainArbWait = 0, drainArbMaxWait = 0, drainArbFull = 0;
    bool luBackgroundPass = false;
    bool luLookaheadPass = false;
    std::deque<std::shared_ptr<OnchipAccess>> onchipQueue;
    std::unordered_map<uint64_t, OnchipContext> onchipContexts;
    uint64_t onchipNextId = 1, onchipCreated = 0, onchipCompleted = 0, onchipCancelled = 0;
    uint64_t onchipPeak = 0, onchipFull = 0, onchipConflicts = 0, onchipSamples = 0;
    uint64_t onchipInactive = 0, onchipReserveStalls = 0;
    std::vector<uint64_t> onchipReads, onchipWrites;
    uint64_t sharedVectorsLive = 0, sharedVectorsMax = 0;
    uint64_t sharedVectorsIssued = 0, sharedVectorsCompleted = 0;
    uint64_t sharedProductStalls = 0;
    FILE *clusterTraceFp = nullptr;
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
    std::array<ResourcePool *, 16> resourcePools();
    void beginResourceStats(uint64_t now);
    void endResourceStats(bool quiescent);
    void recordIssueRejection(OpKind caller, unsigned resource);
    void sampleFrontierState();
    uint64_t launchDescriptor(const CfdCoeffPreprocessDescriptor &desc,
                             const LineCoeffCache *cached = nullptr);
    uint64_t launchLine(const CfdCoeffPreprocessDescriptor &desc, uint64_t reservedToken = 0);
    void processTimedLaunches();
    void stepInterfaceTransfer(uint64_t owner, const std::shared_ptr<InterfaceTransfer> &transfer);
    void pumpLineRequests();
    void initializeLineTile(LineRequest &line);
    void refillForwardTiles(LineRequest &line);
    void processLineTiles();
    void prepareLineCache(LineRequest &line);
    bool prepareCachedRhs(LineRequest &line);
    bool drainCachedCell(LineRequest &line);
    void stepWarmTransfers(LineRequest &line);
    void processDetachedRecords();
    void abortLine(LineRequest &line, CfdCoeffPreprocessStatus status);
    void processAbortedLines();
    std::shared_ptr<OnchipAccess> startOnchip(uint64_t owner, Addr base, unsigned words,
        unsigned stride, bool write, unsigned priority = 0, bool active = true);
    void cancelOnchip(const std::shared_ptr<OnchipAccess> &access);
    void activateOnchip(const std::shared_ptr<OnchipAccess> &access,
                        std::function<void()> completion);
    void processOnchip();
    Addr tileOnchipAddr(const LineRequest &line, const LineTile &tile) const;
    Addr warmOnchipAddr(const LineRequest &line, uint64_t cell) const;
    Addr inputOnchipAddr(const Request &req) const;
    void traceSpmInputStore(const Request &req,const std::shared_ptr<OnchipAccess> &access,
        uint64_t offset,uint64_t packet);
    bool warmRhsOnchipReady(LineRequest &line, WarmSlot &slot);
    bool onchipColumnReady(LineRequest &line, unsigned context, unsigned column);
    bool prepareOnchipCommit(Request &req);
    bool prepareCachedUbar(Request &req);
    void publishCachedUbar(LineRequest &line, Request &req, uint64_t job = 0);
    bool cachedRegistersReady(Request &req);
    void processBackwardOperands(LineRequest &line);
    bool backwardOperandReady(LineRequest &line, unsigned column);
    bool backwardYReady(LineRequest &line);
    void markBackwardColumnUse(LineRequest &, BackwardOperand &, unsigned column);
    void markBackwardYUse(LineRequest &, BackwardOperand &);
    bool tileColumnReadable(const LineTile &tile, uint64_t cell, unsigned column) const;
    void releaseBackwardOperands(LineRequest &line);
    bool backwardTileReadable(const LineTile &tile, uint64_t cell) const;
    bool backwardTileBound(const LineTile &tile, uint64_t cell) const;
    void markLocalTileReady(LineRequest &line, const std::shared_ptr<LineTile> &tile,
                           uint64_t cell, uint64_t yJob, uint64_t uJob,
                           uint64_t cacheJob = 0);
    void processLocalTileResults();
    bool startEarlyBackward(LineRequest &line);
    void commitReadyForwardTiles(LineRequest &line);
    void processEarlyLineTiles(LineRequest &line);
    unsigned warmDepth(const LineRequest &line) const {
        return std::min<uint64_t>(env.lineOptLevel >= 5 ? 8 : 4, line.window);
    }
    std::shared_ptr<LineTile> forwardTile(const LineRequest &line, uint64_t cell) const;
    std::array<std::shared_ptr<LineTile>, 6> lineTiles(const LineRequest &line) const;
    void processAheadLineTiles(LineRequest &line);
    bool cachedUbar(const LineRequest &line) const {
        return line.tileYOnly || (env.lineOptLevel >= 3 && line.coefficientCacheHit &&
            lineCoeffCaches.at(line.desc.lineId).hasUbar);
    }
    bool warmSummary(const LineRequest &line) const {
        return (env.coldSummary ? (!env.coldSummaryOnly || !line.coefficientCacheHit) :
                                 line.coefficientCacheHit) &&
            (line.desc.flags & CFD_COEFF_PRE_WARM_SUMMARY) &&
            (line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE);
    }
    void aggregateSummary(Request &req, LineRequest &line, uint64_t digest,
                          bool detached = false);
    unsigned smeCacheTagBytes() const {
        return env.smeWarmBaseMode == SmeWarmBaseMode::DirectLu ?
            0 : sizeof(SmeCacheBase);
    }
    unsigned cacheEntryBytes(const LineRequest &line) const {
        return line.cacheAllocated / line.cells;
    }
    Addr cacheEntryAddr(const LineRequest &line, uint64_t cell) const {
        return line.tileReserve + line.tileAllocated + cell * cacheEntryBytes(line);
    }
    void beginTileRead(LineRequest &line, const std::shared_ptr<LineTile> &tile, uint64_t first);
    void stepTileRead(LineRequest &line, const std::shared_ptr<LineTile> &tile);
    void retireTileChild(LineRequest &line, Request &req);
    void commitTileChild(LineRequest &line, Request &req);
    void publishLineCache(LineRequest &line);
    void writeGuestRecord(const Request &req);
    uint64_t tileIndex(const LineRequest &line, uint64_t cell) const;
    void retireLineChild(uint64_t childToken, Request &req);
    void processLineCachedForward(uint64_t now);
    void processLineBaseAhead(uint64_t now);
    void finishCachedForwardCell(LineRequest &line);
    void publishLineValues();
    bool stepLineSummary(LineRequest &line);
    void finishLineBackward(LineRequest &line);
    void finishLineBackwardCell(LineRequest &line, bool resultReady = false);
    void publishLineOutput(LineRequest &line, uint64_t cell);
    void processLineOutputWrites();
    void processCoalescedLineOutputWrites(LineRequest &line);
    void advanceLineMvm(LineRequest &line, unsigned context,
        const std::array<double, N * N> &matrix,
        const std::array<double, N> &vector, uint8_t readyMask, uint64_t now,
        std::function<void(LineRequest &, const std::array<double, N> &)> done,
        uint64_t registerRequest = 0);
    void advanceLaneMvm(LineRequest &line, unsigned context,
        const std::array<double, N*N> &matrix, const std::array<double, N> &vector,
        uint8_t readyMask, uint64_t now,
        std::function<void(LineRequest &, const std::array<double, N> &)> done,
        uint64_t registerRequest);
    void processWarmCorrection(Request &req, uint64_t now);
    void completeLineCachedForwardColumn(
        uint64_t lineToken, uint8_t phase, uint8_t column,
        const std::array<double, N> &product, uint64_t now);
    void processLineBackward(uint64_t now);
    void completeLineBackwardColumn(uint64_t lineToken, uint8_t column,
                                    const std::array<double, N> &product,
                                    uint64_t now);
    void tracePrivateLu(const Request &req) {
        if (localLu(req) && env.traceEnable)
            std::fprintf(stderr,"CFD_PRIVATE_LU cycle=%llu token=%llu request=%llu "
                "generation=%llu cell=%llu event=local-ready\n",
                (unsigned long long)cycle(),(unsigned long long)req.token,
                (unsigned long long)req.record.requestId,
                (unsigned long long)req.desc.generation,(unsigned long long)req.desc.cellId);
    }
    void completeOps(uint64_t now);
    bool rfEnabled() const { return env.rfReadWords || env.rfWriteWords; }
    bool localLu(const Request &req) const { return req.desc.flags & CFD_COEFF_PRE_LOCAL_LU; }
    const char *rfPoolName(const ResourcePool *pool) const {
        return pool == &sharedLuFma ? "lu_fma" : pool == &sharedFma ? "fma" : pool == &sharedMul ? "mul" : pool == &sharedAdd ? "add" : "div";
    }
    uint64_t rfCapacity(const ResourcePool &pool) const {
        return pool.nextIssue.size()*((pool.latency+pool.ii-1)/pool.ii);
    }
    uint64_t localReservation() const {
        return (env.vectorLocal || env.localProducts ? 4608 : 0) +
            (env.vectorLocal ? 1536 : 0) +
            (env.localProducts ? 128*env.luCount+256 : 0) +
            (env.luProductAhead ? CfdLuProducts::reservation(env.luCount) : 0) +
            (env.luUpperOverlap ? CfdLuUpper::reservation(env.luCount) : 0) +
            (env.luRank1 ? CfdLuRank1::reservation(env.luCount) : 0) +
            (env.rhsFactorReuse ? CfdRhsFactors::reservation(env.rhsFactorSlots) : 0) +
            (env.rhsFmaCount ? CfdRhsFma::reservation(env.rhsFmaCount) : 0) +
            (env.luFmaCount ? CfdLuFma::reservation(env.luFmaCount) : 0) +
            (env.rhsFrontierReserve ? CfdRhsAdmission::Reservation : 0) +
            (env.rhsUpperInverse ? CfdRhsUpper::reservation(env.rhsUpperSlots) : 0) +
            (env.rhsUpperStream ? CfdRhsUpperStream::ControlBytes : 0) +
            (env.luPivotPrefetch ? 128 : 0) +
            (env.inputRBeforeU ? CfdInputBatchOrder::ControlBytes : 0) +
            (env.spmSlotRotation ? CfdSpmSlotRotation::ControlBytes : 0) +
            (env.rhsFmaFactorFanout ? CfdRhsFma::FactorFanoutControlBytes : 0) +
            (env.rhsFactorFanout ? CfdRhsFactorFanout::Reservation : 0) +
            (env.rhsTreeRfTemp ? 16*env.rhsProductDepth+128 : 0) +
            (env.luControlFold ? 32*env.luCount+128 : 0) +
            (env.rhsPair ? CfdRhsPairs::Reservation : 0);
    }
    void noteLuReturn(Request &req, uint64_t id) {
        if (!env.luControlFold) return;
        req.luControlParent=id;
        req.luControlReturned=cycle();
    }
    int luProductBank(uint64_t token) const {
        auto it=std::find(localLuOwners.begin(),localLuOwners.end(),token);
        panic_if(it==localLuOwners.end(),"CFD missing local LU owner");
        return 27+(it-localLuOwners.begin());
    }
    int luAheadBank(uint64_t token, unsigned slot) const {
        const int first=luProductBank(token);
        return slot ? 27+env.luCount+env.rhsProductDepth+(first-27) : first;
    }
    int rhsFactorBank(unsigned slot) const {
        return 27+env.luCount+env.rhsProductDepth+
               (env.luProductAhead ? env.luCount : 0)+slot;
    }
    int luUpperBank(uint64_t token,unsigned slot) const {
        panic_if(slot>=2,"CFD upper product slot out of bounds");
        return 27+env.luCount+env.rhsProductDepth+
            (env.luProductAhead ? env.luCount : 0)+
            (env.rhsFactorReuse ? env.rhsFactorSlots : 0)+
            2*(luProductBank(token)-27)+slot;
    }
    void traceLuUpper(const Request &req,const char *event,int k=-1,
                      uint64_t id=0,int bank=-1);
    void processLuUpper(Request &req,uint64_t now);
    void processLuRank1(Request &req,uint64_t now);
    void processLuPivotPrefetch(uint64_t now);
    void traceLuPivotRow(const Request &req,unsigned column,uint64_t rf,
        double a,double b,double c,bool fused,double result);
    void processLuFma(Request &req,unsigned context,uint64_t now);
    void traceLuFmaMatrix(const Request &req,const char *event);
    void traceLuFma(const Request &req,unsigned column,unsigned row,const char *event,
        uint64_t rf,const std::array<double,N> &a,const std::array<double,N> &b,
        const std::array<double,N> &c,const std::array<double,N> &result);
    void cancelLuRank1(Request &req);
    void traceLuRank1(const Request &req,const char *event,unsigned column,
        int row=-1,int slot=-1,uint64_t id=0,int bank=-1);
    void processLuProducts(Request &req, uint64_t now);
    bool issueMvmVector(LineRequest &line, unsigned context, int node,
        bool multiply, bool feedback, uint64_t now, OpKind caller,
        std::function<void()> complete);
    void traceRfIssue(const PendingOp &op, uint64_t now, const char *pool, unsigned capacity);
    void tracePreprocessBroadcast(const Request &req, const char *family,
        const char *phase, int k, int column, int rhs, unsigned width, double factor);
    void traceRhsReadyGroups(const Request &req,
        const std::array<unsigned, 15> &order, unsigned count, uint64_t now);
    void assignInputSlots(uint64_t now);
    void processInputs(uint64_t now);
    bool coalesceRhsInput(Request &req, uint64_t now);
    bool directRhsLookahead(const LineRequest &line) const;
    bool coldRhsLookahead(const LineRequest &line) const {
        return env.coldRhsPrefetch && !line.coefficientCacheHit &&
            (line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE) &&
            !(line.desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT);
    }
    bool coldRhsInput(const Request &req) const {
        return env.coldRhsPrefetch && !req.coefficientCacheReuse &&
            req.directBaseSolve && childLineTokens.count(req.token) &&
            !(req.desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT);
    }
    void processDirectRhsPrefetch();
    bool consumeDirectRhs(Request &req,uint64_t now);
    void consumeColdRhs(Request &req, uint64_t now);
    bool splitInput(const Request &req) const {
        return env.inputDEarly && !req.coefficientCacheReuse &&
            !(req.desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT) &&
            req.record.inputBytes > MatrixBytes;
    }
    void prepareInputD(Request &req);
    bool batchInput(const Request &req) const {
        return env.inputBatchReady && req.directBaseSolve && splitInput(req);
    }
    bool batchRhsReady(const Request &req) const {
        return batchInput(req) &&
            ((req.rhsVectorReady && (req.rhsEnabledMask&0x1fu)) ||
             (req.inputLowerReady && (req.rhsEnabledMask&0x3e0u)));
    }
    void prepareInputBatches(Request &req, uint64_t now);
    void traceInputSplit(const Request &req, const char *event,
                         const std::shared_ptr<OnchipAccess> &access);
    void startLuRequests(uint64_t now);
    void processLu(uint64_t now);
    void processLuRows(Request &req, uint64_t now);
    void processLuRowsPrimary(Request &req, uint64_t now);
    void startSolveRequests(uint64_t now);
    void processSolve(uint64_t now);
    bool issueSolveRhs(Request &req, unsigned index, uint64_t now,
                       int pairSlot=-1,unsigned pairSide=0);
    bool holdTailUbar(const Request &req) const;
    bool forwardYReadable(const LineRequest &line, const LineTile &tile,
                          uint64_t cell) const;
    void prepareLocalY(Request &req);
    bool rhsTreeEnabled(const Request &req, unsigned index) const {
        const bool scoped = (env.coldRhsTree == 1 && index < 2*N) ||
            env.coldRhsTree == 2 ||
            (env.coldRhsTree == 3 && index >= N && index < 2*N);
        return req.directBaseSolve &&
            ((env.warmRhsTree && req.coefficientCacheReuse) ||
             (!req.coefficientCacheReuse &&
              ((env.coldBTree && index == 0) ||
               scoped)));
    }
    bool rhsUpperStreaming(const Request &req) const { return env.rhsUpperStream && req.upperSlot>=0; }
    bool issueSolveStreamNorm(Request &req,unsigned index,uint64_t now);
    void traceRhsNorm(const Request &req,unsigned index,unsigned k,const char *event,
        uint64_t rf,double a,double b,double result);
    bool issueSolveFma(Request &req, unsigned index, uint64_t now,bool streamBackward=false);
    void traceRhsFma(const Request &req, unsigned index, bool backward,
        int k, unsigned first, unsigned width, const char *event, uint64_t rf,
        const std::array<double,N> &a, const std::array<double,N> &b,
        const std::array<double,N> &c, const std::array<double,N> &result, bool upper=false, bool streaming=false);
    bool acquireRhsUpper(Request &req);
    void releaseRhsUpper(Request &req,bool cancelled);
    bool upperColumnReady(const Request &req,unsigned column) const;
    void processRhsUpper(uint64_t now,uint64_t frontierCell);
    void traceRhsUpper(const CfdRhsUpper::Slot &s,unsigned slot,const char *event,
        uint64_t rf=0,int entry=-1,int k=-1,double a=0,double b=0,double c=0,double result=0);
    bool issueSolveRows(Request &req, unsigned index, uint64_t now,
                        int pairSlot=-1,unsigned pairSide=0);
    bool issueRhsTree(Request &req, unsigned index, size_t slot, uint64_t now);
    void traceRhsTreeTemp(const Request &req, unsigned index, int k,
        size_t slot, const char *event, uint64_t rf, double value);
    void releaseRhsRows(RhsRowProduct &slot);
    void discardRhsRows(Request &req);
    void finalizeSolveRequest(Request &req, uint64_t now);
    void processColumnFma(uint64_t now);
    void processColdColumns(uint64_t now);
    void releaseColdProduct(const std::shared_ptr<ColdColumnProduct> &product);
    void discardColdProducts(Request &req);
    void completeColumnFma(uint64_t token, uint64_t generation,
                           ColumnFmaTaskKind kind, unsigned column,
                           const std::array<double, N> &product, uint64_t now);
    void processForwardHandoffs(uint64_t now);
    void produceForwardHandoff(Request &req, uint64_t now);
    void processForwardCombines(uint64_t now);
    void completeForwardCombine(uint64_t token, uint64_t generation,
                                uint64_t now);
    void processDrains(uint64_t now);
    void finishRequests(uint64_t now);
    void detachDataCommit(Request &req);
    void retireAutoRequests();
    void accountActivity();
    bool issue(ResourcePool &pool, OpKind kind, uint64_t now,
               std::function<void()> complete);
    bool issueDivide(ResourcePool &pool, OpKind kind, uint64_t now,
                     uint64_t owner, double numerator, double denominator,
                     std::function<void(double)> complete);
    void traceDivider(const CfdFp64Divider::Transaction &transaction,
                      const char *event, uint64_t now, bool dropped = false);
    bool issueLanes(ResourcePool &pool, OpKind kind, uint64_t now,
                    unsigned lanes, OpKind caller,
                    std::function<void()> complete, bool broadcast = false,
                    int readWords = -1, unsigned localReads = 0,
                    bool localResult = false, unsigned localRow = 0,
                    const CfdLocalAccess *bank = nullptr);
    bool issueColumnMvm(const std::array<double, N> &column, double scalar,
                        uint64_t now,
                        std::function<void(const std::array<double, N> &)> done);
    void processSharedVectorAdds(uint64_t now);
    void traceCluster(const char *pool, unsigned lanes, OpKind caller,
                      uint64_t now, uint64_t latency);
    bool enqueueDrain(Request &req, DrainType type, uint64_t now);
    bool admitDrain(Request &req, DrainType type, uint64_t now);
    void arbitrateDrains(uint64_t now);
    void finishDrain(DrainTask &task, uint64_t now);
    void completeInputPacket(uint64_t token, unsigned offset, unsigned bytes,
                             uint64_t packetId, Addr addr, unsigned bank,
                             unsigned port, const uint8_t *payload);
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
    void finishCoeffRhs(Request &req, uint64_t now);
    void markFailed(Request &req, CfdCoeffPreprocessStatus status,
                    const char *note);
    void publishProgress(Request &req);
    SETranslatingPortProxy proxy() const;
    Request *find(uint64_t token);
    std::vector<Request *> admissionOrder() const;
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

std::vector<Request *>
CfdCoeffPreprocessController::admissionOrder() const
{
    std::vector<Request *> candidates;
    candidates.reserve(requests.size());
    for (const auto &[token, req] : requests)
        candidates.push_back(req.get());
    if (env.lineOptLevel) {
        // The finite single-line window has monotonically increasing cells.
        // Oldest cell first is the actual recurrence frontier, at every
        // admission stage; no ordering depends on host hash iteration.
        std::sort(candidates.begin(), candidates.end(), [](auto *a, auto *b) {
            return std::tie(a->desc.cellId, a->record.requestId) <
                   std::tie(b->desc.cellId, b->record.requestId);
        });
    }
    return candidates;
}

void
CfdCoeffPreprocessController::writeGuestRecord(const Request &req)
{
    if (req.desc.flags & CFD_COEFF_PRE_PACKED_RECORD) {
        const auto packed=packedRecord(req.record);
        proxy().writeBlob(req.desc.recordAddr,&packed,sizeof(packed));
    } else if (req.desc.flags & CFD_COEFF_PRE_COMPACT_RECORD) {
        const auto compact = cfdCompactRecord(req.record);
        proxy().writeBlob(req.desc.recordAddr, &compact, sizeof(compact));
    } else {
        proxy().writeBlob(req.desc.recordAddr, &req.record, sizeof(req.record));
    }
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

Addr
inputDmaBase(const Request &req, CfdSpmMatrix matrix)
{
    switch (matrix) {
      case CfdSpmMatrix::D:
      case CfdSpmMatrix::Lu: return req.desc.dAddr;
      case CfdSpmMatrix::L: return req.desc.lAddr;
      case CfdSpmMatrix::U:
        return req.cacheUbarReload ? req.desc.uBarOutAddr : req.desc.uAddr;
      case CfdSpmMatrix::RhsVector: return req.desc.reserved[0];
      default: panic("CFD invalid DMA input region");
    }
}

uint8_t *
inputOperandData(Request &req, CfdSpmMatrix matrix)
{
    switch (matrix) {
      case CfdSpmMatrix::D:
      case CfdSpmMatrix::Lu: return reinterpret_cast<uint8_t *>(req.d.data());
      case CfdSpmMatrix::L: return reinterpret_cast<uint8_t *>(req.lower.data());
      case CfdSpmMatrix::U: return reinterpret_cast<uint8_t *>(req.upper.data());
      case CfdSpmMatrix::RhsVector:
        return reinterpret_cast<uint8_t *>(req.rhsVector.data());
      default: panic("CFD invalid DMA input snapshot");
    }
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
    if (env.smeContexts) {
        smeCells.resize(env.smeContexts);
        if (env.smeWarmSingleTile)
            smeWarmSingleCells.resize(2 * env.smeContexts);
        smeReversePanels.resize(env.smeReverseContexts);
        if (env.smeWarmRhsWidth)
            smeWarmRhs.resize(CfdSmeZa::warmRhsGroups(env.smeContexts));
        const unsigned warmContexts = smeWarmContextCount();
        const unsigned bypassCount = env.smeWarmBypass ?
            (smeUsesDualBypass() ? 2 * warmContexts : warmContexts) : 0;
        smeZa.configure(env.smeContexts, env.smeConfig,
                        env.smeReverseContexts, bypassCount);
    }
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
    init(finalSubtract, N, env.forwardCombineLatency, env.forwardCombineIi);
    init(sharedMul, env.clusterMulCount, env.clusterMulLatency, 1);
    init(sharedAdd, env.clusterAddCount, env.clusterAddLatency, 1);
    arithmeticGroups.depth = env.arithmeticDepth;
    init(sharedDiv, env.clusterDivCount, env.clusterDivLatency, env.clusterDivIi);
    init(sharedFma, env.rhsFmaCount, CfdRhsFma::Latency, CfdRhsFma::Ii);
    init(sharedLuFma, env.luFmaCount, CfdLuFma::Latency, CfdLuFma::Ii);
    if(env.rhsUpperInverse)rhsUpper.slots.resize(env.rhsUpperSlots);
    if (env.detailedDivider) {
        panic_if(!env.sharedCluster || env.clusterDivCount != 1 ||
                 !env.dividerResultDepth || env.dividerResultDepth > 16 ||
                 !env.dividerWritebackIi || env.dividerWritebackIi > 64 ||
                 env.dividerCapacity > 64 || env.clusterDivLatency > 4096 ||
                 env.clusterDivIi > 4096,
                 "CFD hardware divider requires one shared cluster core and nonzero buffers/ports");
        if (!env.dividerCapacity)
            env.dividerCapacity = env.iterativeDivider ? 1 :
                (env.clusterDivLatency+env.clusterDivIi-1)/env.clusterDivIi;
        panic_if(env.iterativeDivider && (env.dividerCapacity != 1 ||
                 env.clusterDivIi < env.clusterDivLatency),
                 "CFD iterative divider requires capacity=1 and II>=latency");
        hardwareDivider.configure({static_cast<unsigned>(env.clusterDivLatency),
            static_cast<unsigned>(env.clusterDivIi),env.dividerCapacity,
            env.dividerResultDepth,env.iterativeDivider},CfdFp64Divider::divide);
    }
    registerPorts.configure({env.rfReadWords,env.rfWriteWords});
    for (auto &ports : lanePorts) ports.configure({2,1});
    rhsFactors.configure(env.rhsFactorSlots);
    localVectors.configure(27+env.luCount+env.rhsProductDepth+
                           (env.luProductAhead ? env.luCount : 0)+
                           (env.rhsFactorReuse ? env.rhsFactorSlots : 0)+
                           (env.luUpperOverlap ? 2*env.luCount : 0));
    localLuOwners.assign(env.luCount,0);
    if (env.luRank1) luRankStates.resize(env.luCount);
    summarySnapshots.configure(env.summaryDepth);
    summaryEngines.resize(env.summaryDepth ? env.summaryEngines : 0);
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
    if (env.sharedCluster) {
        switch (kind) {
          case OpKind::LuDiv: case OpKind::Div:
            panic_if(env.detailedDivider,
                     "CFD detailed division must use the latched issueDivide interface");
            return issueLanes(sharedDiv, OpKind::SharedDiv, now, 1, kind,
                              std::move(complete));
          case OpKind::LuMul: case OpKind::Mul:
            return issueLanes(sharedMul, OpKind::SharedMul, now, 1, kind,
                              std::move(complete));
          case OpKind::LuSub: case OpKind::Sub:
            return issueLanes(sharedAdd, OpKind::SharedAdd, now, 1, kind,
                              std::move(complete));
          case OpKind::LineProduct:
            return issueLanes(sharedMul, OpKind::SharedMul, now, N, kind,
                              std::move(complete), true);
          case OpKind::LineAccumulate:
            return issueLanes(sharedAdd, OpKind::SharedAdd, now, N, kind,
                              std::move(complete));
          case OpKind::ForwardCombine:
            return issueLanes(env.dedicatedFinalSubtract ? finalSubtract : sharedAdd,
                              env.dedicatedFinalSubtract ? OpKind::FinalSubtract :
                                                           OpKind::SharedAdd,
                              now, N, kind, std::move(complete));
          default:
            panic("CFD unshared operation in shared cluster");
        }
    }
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

#include "cfd_coeff_preprocess_cluster.inc"
#include "cfd_coeff_preprocess_divider.inc"
#include "cfd_coeff_preprocess_frontier.inc"
#include "cfd_coeff_preprocess_rhs_rows.inc"
#include "cfd_coeff_preprocess_rhs_fma.inc"
#include "cfd_coeff_preprocess_rhs_upper_stream.inc"
#include "cfd_coeff_preprocess_rhs_upper.inc"

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
    if (env.timedInterface) {
        // This profile owns one line invocation and one descriptor staging slot.
        if (!pendingLineLaunches.empty() || !lineRequests.empty() ||
            !detachedRecords.empty() || summaryBusy() || !onchipQueue.empty())
            return 0;
        auto transfer = std::make_shared<InterfaceTransfer>();
        transfer->addr = descriptorAddr;
        transfer->kind = "descriptor";
        transfer->bytes.resize(sizeof(desc));
        const uint64_t token = nextToken++;
        pendingLineLaunches.emplace(token, transfer);
        scheduleTick();
        return token;
    }
    proxy().readBlob(descriptorAddr, &desc, sizeof(desc));
    if (desc.flags & CFD_COEFF_PRE_LINE_AUTONOMOUS)
        return launchLine(desc);
    if (desc.flags & (CFD_COEFF_PRE_LOCAL_UBAR | CFD_COEFF_PRE_PACKED_COLD_INPUT))
        return 0;
    return launchDescriptor(desc);
}

uint64_t
CfdCoeffPreprocessController::launchDescriptor(
    const CfdCoeffPreprocessDescriptor &desc, const LineCoeffCache *cached)
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
    const bool directBaseSolve =
        desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
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
        ((desc.flags & CFD_COEFF_PRE_LOCAL_LU) && (!env.privateLuOutput || !directBaseSolve || !env.luForwarding ||
            (desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT))) ||
        (directBase && !baseConsumer) ||
        (handoffFaults && (!lbarConsumer ||
                           handoffFaults == handoffFaultBits)) ||
        (holdDqStarPublish && !lbarConsumer) ||
        (lbarConsumer && (!baseConsumer || !directBase || !autoRetire ||
                          !desc.reserved[3])) ||
        (directLbar && !lbarConsumer) ||
        (directBaseSolve && (!baseConsumer || !directBase ||
                             !directLbar || desc.lineId != 0)) ||
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
    req->directBaseSolve = directBaseSolve;
    req->coefficientCacheReuse = cached != nullptr;
    req->directBaseBypass = directBase;
    req->lbarConsumerEnabled = lbarConsumer;
    req->directLbarBypass = directLbar;
    if (directBase)
        req->matrixDrainMask &= ~1u;
    if (directLbar)
        req->matrixDrainMask &= ~2u;
    if (desc.flags & CFD_COEFF_PRE_LOCAL_UBAR)
        req->matrixDrainMask &= ~4u;
    if (coeff3) {
        req->rhsSlots = 15;
        const RhsMaskPlan &plan = RhsMaskPlans[req->requestedBatchMask];
        req->rhsEnabledMask = plan.rhsMask;
        req->rhsCount = plan.rhsCount;
        if (directBaseSolve) {
            // Preserve Lbar/Ubar slot numbers and their original arithmetic.
            // Only the identity batch changes, from five RHS to one vector.
            req->rhsEnabledMask = (req->rhsEnabledMask & ~0x001fu) | 1u;
            req->rhsCount -= N - 1;
        }
        req->record.maskTableLookups++;
    } else {
        req->rhsSlots = 5;
        req->rhsCount = 5;
        req->rhsEnabledMask = 0x001f;
    }
    req->smeDeferUpper = needUBar && !cached && env.smeReverseContexts &&
        (desc.flags & CFD_COEFF_PRE_LOCAL_UBAR);
    if (req->smeDeferUpper) {
        req->rhsEnabledMask &= ~0x7c00u;
        req->rhsCount -= N;
    }
    if (!cached)
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::D;
    if (needUBar && !cached)
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::U;
    if (cached && !cached->hasUbar && needUBar) {
        req->cacheUbarReload = true;
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::U;
    }
    if (needLBar && !cached)
        req->inputMatrices[req->inputMatrixCount++] = CfdSpmMatrix::L;
    if ((env.inputLFirst || (desc.flags & CFD_COEFF_PRE_PACKED_COLD_INPUT)) &&
        !cached && req->directBaseSolve && req->inputMatrixCount == 3)
        std::swap(req->inputMatrices[1],req->inputMatrices[2]);
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
    if (cached) {
        panic_if(!directBaseSolve || desc.cellId >= cached->cells,
                 "CFD invalid internal LU cache reuse");
        const auto format = cached->smeBaseTags.empty() ? SmeCacheBase::Direct :
            cached->smeBaseTags[desc.cellId];
        req->smeInvUseCache = format == SmeCacheBase::Inverse;
        req->smeInvRejected = format == SmeCacheBase::GuardFallback;
        if (req->smeInvUseCache) {
            req->dInv = cached->lu[desc.cellId];
        } else {
            req->lu = cached->lu[desc.cellId];
            req->cachedReciprocal = cached->reciprocalDiagonal;
            if (req->cachedReciprocal) {
                for (unsigned k = 0; k < N; ++k)
                    req->reciprocal[k] = req->lu[k*N+k];
                req->reciprocalReady.fill(true);
            }
        }
        req->lBar = cached->lBar[desc.cellId];
        if (cached->hasUbar && desc.cellId < cached->uBar.size())
            req->uBar = cached->uBar[desc.cellId];
        req->requestedBatchMask = 1;
        req->matrixDrainMask = 0;
        if (env.tiledLine && needUBar && env.lineOptLevel < 3) {
            req->requestedBatchMask |= 4;
            req->matrixDrainMask = 4;
            req->record.uBarReadyCycle = cycle();
        }
        req->rhsEnabledMask = 1;
        req->rhsCount = 1;
        req->luDone = true;
        req->luDrained = !localLu(*req);
        req->drainQueued[static_cast<unsigned>(DrainType::Lu)] = !localLu(*req);
        req->drainDone[static_cast<unsigned>(DrainType::Lu)] = !localLu(*req);
        if (desc.cellId) {
            req->lbarColumnReadyMask = 0x1f;
            req->record.lbarColumnsReady = N;
            req->record.lBarReadyCycle = cycle();
            for (unsigned col = 0; col < N; ++col)
                req->record.lBarColumnReadyCycle[col] = cycle();
        }
    }
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
    // Finite autonomous lines validate one parent generation. Retaining one
    // per-cell history entry would itself create unbounded resident metadata.
    auto generation = env.tiledLine ? liveGeneration.end() : liveGeneration.find(generationKey);
    if (desc.generation == 0 ||
        (generation != liveGeneration.end() &&
         desc.generation <= generation->second)) {
        req->stage = RequestStage::Failed;
        req->status = CfdCoeffPreprocessStatus::GenerationMismatch;
        req->record.status = static_cast<uint64_t>(req->status);
        req->record.completeCycle = cycle();
        writeGuestRecord(*req);
    } else if (!env.tiledLine) {
        liveGeneration[generationKey] = desc.generation;
    }
    trace(*req, "controller", "REQUEST_ALLOC");
    requests.emplace(token, std::move(req));
    scheduleTick();
    return token;
}

uint64_t
CfdCoeffPreprocessController::launchLine(
    const CfdCoeffPreprocessDescriptor &desc, uint64_t reservedToken)
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
    const bool directBaseSolve =
        desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
    const bool localUbar = desc.flags & CFD_COEFF_PRE_LOCAL_UBAR;
    const bool packedCold = desc.flags & CFD_COEFF_PRE_PACKED_COLD_INPUT;
    if (desc.version != DescriptorVersion || desc.size != sizeof(desc) ||
        (desc.flags & required) != required || !cells || !window ||
        window > 64 || !desc.dAddr || !desc.lAddr || !desc.uAddr ||
        (env.smeContexts && window > CfdInputCapacity::ExtendedMaximum &&
         (!localUbar || !env.extendedInputSlots || window > env.inputSlots)) ||
        !desc.luOutAddr || !desc.dInvOutAddr ||
        !desc.uBarOutAddr || !desc.recordAddr ||
        !desc.reserved[0] || !desc.reserved[3] ||
        (backwardDirect && !desc.reserved[2]) ||
        (cacheEnabled && (!backwardDirect || !desc.cellId)) ||
        (coefficientDirty && !cacheEnabled) ||
        (env.smeContexts && !directBaseSolve) ||
        (localUbar && (!env.smeContexts || !directBaseSolve ||
                       env.smeUbarMode == SmeUbarMode::External)) ||
        (packedCold && (!env.smeContexts || !directBaseSolve ||
                        env.spmLayout != SpmLayout::Legacy ||
                        env.spmWriteWidth != 64 || env.coldRhsPrefetch ||
                        (desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT) ||
                        desc.dAddr % 64 ||
                        cells > (std::numeric_limits<Addr>::max() - desc.dAddr) /
                            PackedColdInputBytes ||
                        desc.lAddr != desc.dAddr + MatrixBytes ||
                        desc.uAddr != desc.dAddr + 2 * MatrixBytes ||
                        desc.reserved[0] != desc.dAddr + 3 * MatrixBytes)) ||
        (directBaseSolve && (!backwardDirect ||
                             desc.lineId != 0)) ||
        ((desc.flags & CFD_COEFF_PRE_LOCAL_LU) && (!env.privateLuOutput || !directBaseSolve || !backwardDirect ||
            (desc.flags & CFD_COEFF_PRE_PACKED_LU_INPUT))) ||
        (env.tailUbarOverlap && (!directBaseSolve ||
             window <= std::min<uint64_t>(env.tailUbarOverlap,cells-1))) ||
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
    if (env.tiledLine) {
        if (desc.lineId != 0 || desc.generation <= lastTiledGeneration ||
            std::any_of(lineRequests.begin(), lineRequests.end(),
                        [](const auto &entry) { return !entry.second->terminal ||
                            (entry.second->abortPending && !entry.second->abortPublished); }))
            return 0;
        lastTiledGeneration = desc.generation;
    }
    if (env.smeContexts) {
        smeZa.resetStats();
        smeStart = cycle();
        smeForwardCells = 0;
    }

    auto line = std::make_unique<LineRequest>();
    line->token = reservedToken ? reservedToken : nextToken++;
    if (env.smeContexts)
        smeOwner = line->token;
    line->desc = desc;
    line->cells = cells;
    line->window = std::min<uint64_t>(window, cells);
    line->backwardDirect = backwardDirect;
    line->coefficientVersion = desc.cellId;
    line->coefficientCacheEnabled = cacheEnabled;
    line->coefficientDirty = coefficientDirty;
    line->localUbar = localUbar;
    if (env.tiledLine) {
        initializeLineTile(*line);
        prepareLineCache(*line);
        line->smeInversePolicy = wantColdInverse(*line);
    } else {
        line->dqStar.resize(cells);
        line->dq.resize(cells);
        line->rhs.resize(cells);
        if (directBaseSolve)
            line->lu.resize(cells);
        else
            line->dInv.resize(cells);
        line->lBar.resize(cells);
        line->uBar.resize(cells);
        line->dqStarReady.resize(cells, false);
        line->uBarReady.resize(cells, 0);
    }
    if (line->coefficientCacheEnabled && !coefficientDirty) {
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
        if (cached != lineCoeffCaches.end() && cached->second.valid &&
            cached->second.version == desc.cellId &&
            cached->second.cells == cells &&
            cached->second.directBaseSolve == directBaseSolve &&
            cached->second.localUbar == localUbar &&
            cached->second.privateLuOutput == bool(desc.flags & CFD_COEFF_PRE_LOCAL_LU)) {
            line->coefficientCacheHit = true;
            if (!line->tile) {
                line->baseRing.resize(line->window);
                line->baseReady.resize(line->window, false);
                std::fill(line->uBarReady.begin(), line->uBarReady.end(), 0x1f);
                line->dInv = cached->second.dInv;
                line->lu = cached->second.lu;
                line->lBar = cached->second.lBar;
                line->uBar = cached->second.uBar;
                for (uint64_t cell = 0; !directBaseSolve && cell < cells; ++cell) {
                    proxy().readBlob(
                        desc.reserved[0] + cell * VectorBytes,
                        line->rhs[cell].data(), VectorBytes);
                }
            }
            line->cachedForwardStarted = !directBaseSolve;
        }
    }
    line->deferUpper = localUbar && env.smeReverseContexts &&
        !line->coefficientCacheHit && cells > 1;
    if (line->deferUpper)
        line->smeUpperFactors.resize((cells-1)*smeUpperFactorWords());
    const uint64_t token = line->token;
    if (packedCold && env.traceEnable)
        std::fprintf(stderr, "CFD_SME_PACKED_INPUT token=%llu generation=%llu "
            "base=0x%llx cells=%llu stride=%u cold=%u\n",
            (unsigned long long)token, (unsigned long long)desc.generation,
            (unsigned long long)desc.dAddr, (unsigned long long)cells,
            PackedColdInputBytes, !line->coefficientCacheHit);
    lineRequests.emplace(token, std::move(line));
    const uint64_t busy =
        static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
    if (!env.timedInterface)
        proxy().writeBlob(desc.dInvOutAddr, &busy, sizeof(busy));
    if (!lineRequests.at(token)->coefficientCacheHit || directBaseSolve)
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
        const bool direct = line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
        if (line.coefficientCacheHit && !direct)
            continue;
        const bool rotateForward = env.smeForwardTileRotate;
        while (!line.terminal && line.nextCell < line.cells &&
               line.liveChildren - line.dataCommitLive < line.window &&
               requests.size() < capacity) {
            const uint64_t cell = line.nextCell;
            if (line.tile) {
                const bool hasTile = rotateForward ? bool(forwardTile(line, cell)) :
                    (line.tile->phase == LineTile::Phase::Forward &&
                     (cell < line.tile->start + line.tile->count ||
                      (line.prefetchTile &&
                       line.prefetchTile->phase == LineTile::Phase::Forward &&
                       cell < line.prefetchTile->start + line.prefetchTile->count)));
                if (!hasTile)
                    break;
            }
            CfdCoeffPreprocessDescriptor child = line.desc;
            child.flags &= ~(CFD_COEFF_PRE_LINE_AUTONOMOUS |
                             CFD_COEFF_PRE_LINE_BACKWARD_DIRECT |
                             CFD_COEFF_PRE_LINE_COEFF_CACHE |
                             CFD_COEFF_PRE_LINE_COEFF_DIRTY);
            child.generation = line.desc.generation + cell;
            child.cellId = cell;
            const bool packed = child.flags & CFD_COEFF_PRE_PACKED_COLD_INPUT;
            const unsigned matrixStride = packed ? PackedColdInputBytes : MatrixBytes;
            child.dAddr += cell * matrixStride;
            child.lAddr += cell * matrixStride;
            child.uAddr += cell * matrixStride;
            child.luOutAddr += cell * MatrixBytes;
            if (child.dInvOutAddr)
                child.dInvOutAddr += cell * MatrixBytes;
            if (child.lBarOutAddr)
                child.lBarOutAddr += cell * MatrixBytes;
            child.uBarOutAddr += cell * MatrixBytes;
            child.recordAddr += cell * recordBytes(child.flags);

            child.reserved[0] += cell * (packed ? PackedColdInputBytes : VectorBytes);
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
            const auto *cached = line.coefficientCacheHit ?
                &lineCoeffCaches.at(line.desc.lineId) : nullptr;
            const uint64_t childToken = launchDescriptor(child, cached);
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
    uint64_t childToken, Request &req)
{
    auto owner = childLineTokens.find(childToken);
    if (owner == childLineTokens.end())
        return;
    auto lineIt = lineRequests.find(owner->second);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        if (req.dataCommitDetached) {
            panic_if(!line.dataCommitLive, "CFD data commit credit underflow");
            line.dataCommitLive--;
            dataCommitReleased++;
            req.dataCommitDetached = false;
            trace(req, "controller", "DATA_COMMIT_RELEASE");
        }
        if (line.tile) {
            retireTileChild(line, req);
            childLineTokens.erase(owner);
            return;
        }
        if (line.abortPending || req.status != CfdCoeffPreprocessStatus::Complete) {
            if (req.status != CfdCoeffPreprocessStatus::Complete)
                abortLine(line, req.status);
            if (line.liveChildren)
                line.liveChildren--;
            line.completedChildren++;
            childLineTokens.erase(owner);
            return;
        }
        const uint64_t cell = req.desc.cellId;
        if (cell < line.cells) {
            line.dqStar[cell] = req.dqStar;
            if (req.directBaseSolve)
                line.lu[cell] = req.lu;
            else
                line.dInv[cell] = req.dInv;
            line.lBar[cell] = req.lBar;
            line.uBar[cell] = req.uBar;
            line.dqStarReady[cell] = req.dqStarReadyLocal;
            if (!req.coefficientCacheReuse)
                line.uBarReady[cell] = (req.rhsDoneMask >> 10) & 0x1f;
        }
        if (line.liveChildren)
            line.liveChildren--;
        line.completedChildren++;
        if (line.completedChildren == line.cells) {
            line.childrenRetiredCycle = cycle();
            forwardHandoffs.erase(line.desc.lineId);
            if (line.coefficientCacheEnabled && !line.coefficientCacheHit) {
                LineCoeffCache cache;
                cache.valid = true;
                cache.version = line.coefficientVersion;
                cache.cells = line.cells;
                cache.directBaseSolve =
                    line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE;
                cache.lu = line.lu;
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
                    publishLineOutput(line, line.cells - 1);
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

#include "cfd_coeff_preprocess_tiles.inc"
#include "cfd_coeff_preprocess_interface.inc"

void
CfdCoeffPreprocessController::completeLineCachedForwardColumn(
    uint64_t lineToken, uint8_t phase, uint8_t column,
    const std::array<double, N> &product, uint64_t now)
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
    auto &accumulator = phase == 0 ?
        line.cachedBaseAccumulator : line.cachedCorrectionAccumulator;
    for (unsigned row = 0; row < N; ++row)
        accumulator[row] += product[row];
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
    auto tile = line.tile ? forwardTile(line, cell) : nullptr;
    panic_if(line.tile && !tile, "CFD warm result outside owned forward tiles");
    if (env.sharedCluster && cell && !line.cachedSubtractReady) {
        line.cachedFinishWaiting = true;
        return;
    }
    line.cachedSubtractReady = false;
    for (unsigned row = 0; row < N; ++row)
        (line.tile ? tile->y[cell-tile->start][row] : line.dqStar[cell][row]) = line.cachedBaseAccumulator[row] -
            (cell ? line.cachedCorrectionAccumulator[row] : 0.0);
    if (line.tile) {
        if (!line.tileYOnly)
            tile->u[cell-tile->start] = lineCoeffCaches.at(line.desc.lineId).uBar[cell];
        line.yTransfer = std::make_shared<InterfaceTransfer>();
        line.yTransfer->interface = false;
        line.yTransfer->cell = cell;
        line.yTransfer->kind = "y";
        line.yTransfer->write = true;
        line.yTransfer->addr = line.desc.reserved[3] + cell * VectorBytes;
        line.yTransfer->bytes.resize(VectorBytes);
        std::memcpy(line.yTransfer->bytes.data(), tile->y[cell-tile->start].data(), VectorBytes);
        if (cell+1 < line.cells && (!cachedUbar(line) || env.lineOptLevel < 3)) {
            line.uTransfer = std::make_shared<InterfaceTransfer>();
            line.uTransfer->interface = false;
            line.uTransfer->cell = cell;
            line.uTransfer->kind = "ubar";
            line.uTransfer->write = true;
            line.uTransfer->addr = line.desc.uBarOutAddr + cell * MatrixBytes;
            line.uTransfer->bytes.resize(MatrixBytes);
            const auto &u = line.tileYOnly ?
                lineCoeffCaches.at(line.desc.lineId).uBar[cell] : tile->u[cell-tile->start];
            std::memcpy(line.uTransfer->bytes.data(), u.data(), MatrixBytes);
        }
        if (env.lineOptLevel >= 3) {
            auto &slot = line.warm[cell % warmDepth(line)];
            panic_if(!slot.valid || slot.cell != cell || slot.y,
                     "CFD warm slot ownership violation");
            if (env.lineOptLevel >= 5) {
                panic_if(line.warmWrites.size() >= env.warmWriteDepth,
                         "CFD warm write credit was not reserved");
                line.warmWrites.push_back({std::move(line.yTransfer), tile, {}, {}});
                slot = WarmSlot{};
                if (env.traceEnable)
                    std::fprintf(stderr, "CFD_WARM_INPUT_RELEASE cycle=%llu token=%llu cell=%llu\n",
                        (unsigned long long)cycle(), (unsigned long long)line.token,
                        (unsigned long long)cell);
            } else {
                slot.y = std::move(line.yTransfer);
                slot.tile = tile;
                slot.baseReady = false;
            }
            line.previousY = tile->y[cell-tile->start];
            line.cachedBaseAccumulator.fill(0.0);
            line.cachedCorrectionAccumulator.fill(0.0);
            line.cachedForwardPhase = 0;
            line.cachedForwardColumn = 0;
            line.cachedForwardCell++;
            line.warmWritesMax = std::max<uint64_t>(line.warmWritesMax, env.lineOptLevel >= 5 ?
                line.warmWrites.size() : std::count_if(line.warm.begin(), line.warm.end(),
                              [](const auto &s) { return bool(s.y); }));
            if (line.cachedForwardCell == line.cells)
                line.cachedForwardStarted = false;
            return;
        }
        return; // legacy commit waits for y/Ubar responses
    }
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
        publishLineOutput(line, line.cells - 1);
    }
}

void
CfdCoeffPreprocessController::advanceLineMvm(
    LineRequest &line, unsigned context,
    const std::array<double, N * N> &matrix,
    const std::array<double, N> &vector, uint8_t readyMask, uint64_t now,
    std::function<void(LineRequest &, const std::array<double, N> &)> done,
    uint64_t registerRequest)
{
    if (env.laneMvm) {
        advanceLaneMvm(line,context,matrix,vector,readyMask,now,done,registerRequest);
        return;
    }
    auto &state = line.mvm[context];
    const uint64_t token = line.token;
    const uint64_t epoch = state.epoch;
    const uint64_t cell = context == 1 ? line.backwardCell :
        context == 2 ? line.baseCell : line.cachedForwardCell;
    // The inter-cell feedback remains sequential. Independent products run
    // ahead; the selected reduction explicitly determines FP64 add order.
    const bool feedback = env.backwardFeedbackTree && context == 1;
    if (feedback) {
        // Include y in the signed tree. Reuse the four existing partial
        // registers: a=y-p0, b=p1+p2, c=p3+p4, d=a-b, result=d-c.
        // Each node reserves all five physical ADD/SUB lanes. The final node
        // replaces (rather than bypasses) the normal final subtract.
        // Try all five ready nodes: a rejected RF-reading node must not hide
        // an independent local-reading node. The two-grant cap still applies.
        for (unsigned node = 0; node < 5; ++node) {
            const uint8_t bit = 1u << node;
            const bool ready = node == 0 ? state.ready[0] :
                node == 1 ? state.ready[1] && state.ready[2] :
                node == 2 ? state.ready[3] && state.ready[4] :
                node == 3 ? (state.treeReady & 3) == 3 :
                            (state.treeReady & 12) == 12;
            if ((state.treeIssued & bit) || !ready) continue;
            if (node == 0 && !backwardYReady(line)) continue;
            const auto left = node == 0 ? line.backwardYOperand :
                node == 1 ? state.products[1] :
                node == 2 ? state.products[3] : state.partial[node == 3 ? 0 : 3];
            const auto right = node == 0 ? state.products[0] :
                node == 1 ? state.products[2] :
                node == 2 ? state.products[4] : state.partial[node == 3 ? 1 : 2];
            if (!issueMvmVector(line,context,node,false,true,now,
                node == 4 ? OpKind::ForwardCombine : OpKind::LineAccumulate,
                [this,token,context,epoch,cell,node,left,right,done] {
                    auto it = lineRequests.find(token);
                    if (it == lineRequests.end() || it->second->terminal) return;
                    auto &line = *it->second;
                    auto &state = line.mvm[context];
                    if (state.epoch != epoch) return;
                    std::array<double,N> result;
                    for (unsigned row=0;row<N;++row) {
                        volatile double v = node == 1 || node == 2 ?
                            left[row]+right[row] : left[row]-right[row];
                        result[row] = v;
                    }
                    if (node < 4) state.partial[node] = result;
                    state.treeReady |= 1u << node;
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_MVM_FEEDBACK cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=complete node=%u\n",
                            (unsigned long long)cycle(),(unsigned long long)token,context,
                            (unsigned long long)epoch,(unsigned long long)cell,node);
                    if (node == 4) {
                        state = LineMvmState{};
                        state.epoch = epoch+1;
                        done(line,result);
                    }
                })) continue;
            state.treeIssued |= bit;
            if (node < 4) line.lineAdds++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_MVM_FEEDBACK cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=issue node=%u\n",
                    (unsigned long long)now,(unsigned long long)token,context,
                    (unsigned long long)epoch,(unsigned long long)cell,node);
        }
    } else if (env.lineMvmTree) {
        // ((p0+p1)+(p2+p3))+p4: four five-lane adds, three dependent
        // levels. The two first-level operations still compete for ONE bank.
        for (unsigned node = 0; node < 4; ++node) {
            const uint8_t bit = 1u << node;
            if ((state.treeIssued & bit) ||
                (node < 2 && !(state.ready[2*node] && state.ready[2*node+1])) ||
                (node == 2 && (state.treeReady & 3) != 3) ||
                (node == 3 && (!(state.treeReady & 4) || !state.ready[4])))
                continue;
            const auto left = node < 2 ? state.products[2 * node] :
                state.partial[node == 2 ? 0 : 2];
            const auto right = node < 2 ? state.products[2 * node + 1] :
                (node == 2 ? state.partial[1] : state.products[4]);
            if (!issueMvmVector(line,context,node,false,false,now,OpKind::LineAccumulate,
                [this, token, context, epoch, cell, node, left, right, done] {
                    auto it = lineRequests.find(token);
                    if (it == lineRequests.end() || it->second->terminal)
                        return;
                    auto &line = *it->second;
                    auto &state = line.mvm[context];
                    if (state.epoch != epoch)
                        return;
                    for (unsigned row = 0; row < N; ++row)
                        state.partial[node][row] = left[row] + right[row];
                    state.treeReady |= 1u << node;
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_MVM_TREE cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=complete node=%u\n",
                            (unsigned long long)cycle(),(unsigned long long)token,context,
                            (unsigned long long)epoch,(unsigned long long)cell,node);
                    if (node == 3) {
                        const auto result = state.partial[node];
                        state = LineMvmState{};
                        state.epoch = epoch + 1;
                        done(line, result);
                    }
                }))
                continue;
            state.treeIssued |= bit;
            line.lineAdds++;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_MVM_TREE cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=issue node=%u\n",
                    (unsigned long long)now,(unsigned long long)token,context,
                    (unsigned long long)epoch,(unsigned long long)cell,node);
        }
    }
    if (!env.lineMvmTree && !state.addPending && state.added < N &&
        state.ready[state.added]) {
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
    for (unsigned attempt=0;attempt<env.vectorIssueWidth;++attempt) {
    if (state.issued >= N)
        return;
    if (state.issued - state.added >= env.lineProductDepth) {
        line.lineBufferStalls++;
        return;
    }
    const unsigned col = state.issued;
    if (!(readyMask & (1u << col)))
        return;
    // Direct cached forward operands have already crossed SPM-to-RF through
    // cachedRegistersReady(). Reuse context 0; do not fetch a second copy.
    if (!registerRequest && !onchipColumnReady(line,context,col)) return;
    std::array<double, N> column;
    for (unsigned row = 0; row < N; ++row)
        column[row] = env.onchipTiming && !registerRequest ?
            state.columnOperands[col][row] : matrix[row * N + col];
    const double scalar = vector[col];
    if (!issueMvmVector(line,context,col,true,feedback,now,OpKind::LineProduct,
               [this, token, context, epoch, cell, col, column, scalar] {
            auto it = lineRequests.find(token);
            if (it == lineRequests.end() || it->second->terminal)
                return;
            auto &state = it->second->mvm[context];
            if (state.epoch != epoch)
                return;
            for (unsigned row = 0; row < N; ++row)
                state.products[col][row] = column[row] * scalar;
            state.ready[col] = true;
            if (env.traceEnable && env.lineMvmTree)
                std::fprintf(stderr,"CFD_MVM_PRODUCT cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=complete column=%u\n",
                    (unsigned long long)cycle(),(unsigned long long)token,context,
                    (unsigned long long)epoch,(unsigned long long)cell,col);
        }))
        return;
    state.issued++;
    if (env.traceEnable && env.lineMvmTree)
        std::fprintf(stderr,"CFD_MVM_PRODUCT cycle=%llu token=%llu context=%u epoch=%llu cell=%llu event=issue column=%u\n",
            (unsigned long long)now,(unsigned long long)token,context,
            (unsigned long long)epoch,(unsigned long long)cell,col);
    line.lineProducts++;
    line.lineBufferedMax = std::max<uint64_t>(line.lineBufferedMax,
                                             state.issued - state.added);
    }
}

void
CfdCoeffPreprocessController::processLineCachedForward(uint64_t now)
{
    for (auto &[lineToken, linePtr] : lineRequests) {
        LineRequest &line = *linePtr;
        if (line.tile && env.lineOptLevel >= 3 && line.coefficientCacheHit &&
            !(line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE) && !line.terminal)
            stepWarmTransfers(line);
        if (line.tile && line.cachedForwardStarted) {
            if (line.yTransfer || line.uTransfer) {
                drainCachedCell(line);
                continue;
            }
            if (!forwardTile(line, line.cachedForwardCell)) {
                line.warmTileStalls++;
                continue;
            }
            if (env.lineOptLevel >= 5 && line.warmWrites.size() >= env.warmWriteDepth) {
                line.warmOutputStalls++;
                continue;
            }
            if (!prepareCachedRhs(line))
                continue;
        }
        if (line.cachedForwardStarted && !line.terminal &&
            line.cachedFinishWaiting && !line.cachedForwardOpPending) {
            if (issue(forwardCombine, OpKind::ForwardCombine, now,
                [this, lineToken] {
                    auto it = lineRequests.find(lineToken);
                    if (it == lineRequests.end() || it->second->terminal)
                        return;
                    auto &line = *it->second;
                    line.cachedForwardOpPending = false;
                    line.cachedFinishWaiting = false;
                    line.cachedSubtractReady = true;
                    finishCachedForwardCell(line);
                }))
                line.cachedForwardOpPending = true;
            continue;
        }
        if (!line.cachedForwardStarted || line.terminal ||
            line.cachedForwardOpPending ||
            line.cachedForwardCell >= line.cells)
            continue;
        if (env.lineBaseAhead && (!line.tile || env.lineOptLevel >= 3)) {
            const auto slot = line.cachedForwardCell % line.window;
            if (line.tile) {
                const auto &warm = line.warm[line.cachedForwardCell %
                                            warmDepth(line)];
                if (!warm.baseReady) {
                    line.warmBaseStalls++;
                    continue;
                }
                line.cachedBaseAccumulator = warm.base;
            } else {
                if (!line.baseReady[slot])
                    continue;
                line.cachedBaseAccumulator = line.baseRing[slot];
            }
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
                phase == 0 ? (line.tile ? lineCoeffCaches.at(line.desc.lineId).dInv[cell] : line.dInv[cell]) :
                    (line.tile ? lineCoeffCaches.at(line.desc.lineId).lBar[cell] : line.lBar[cell]),
                phase == 0 ? (line.tile ? line.cachedRhs : line.rhs[cell]) :
                    (line.tile ? line.previousY : line.dqStar[cell - 1]),
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
        std::array<double, N> values;
        if (!onchipColumnReady(line,0,column)) continue;
        const auto cell = line.cachedForwardCell;
        const auto &matrix = phase == 0 ?
            (line.tile ? lineCoeffCaches.at(line.desc.lineId).dInv[cell] : line.dInv[cell]) :
            (line.tile ? lineCoeffCaches.at(line.desc.lineId).lBar[cell] : line.lBar[cell]);
        for (unsigned row = 0; row < N; ++row)
            values[row] = env.onchipTiming ? line.mvm[0].columnOperands[column][row] : matrix[row * N + column];
        const double scalar = phase == 0 ? (line.tile ? line.cachedRhs[column] : line.rhs[cell][column]) :
            (line.tile ? line.previousY[column] : line.dqStar[cell - 1][column]);
        if (!issueColumnMvm(values, scalar, now,
                   [this, lineToken, phase, column](const auto &product) {
                       completeLineCachedForwardColumn(
                           lineToken, phase, column, product, cycle());
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
        if ((line.tile && env.lineOptLevel < 3) || !line.cachedForwardStarted || line.terminal || line.basePending ||
            line.baseCell >= line.cells ||
            line.baseCell - line.cachedForwardCell >= line.window)
            continue;
        const uint64_t cell = line.baseCell;
        const uint8_t col = line.baseColumn;
        std::array<double, N> rhs;
        if (line.tile) {
            auto &slot = line.warm[cell % warmDepth(line)];
            if (!slot.valid || slot.cell != cell || slot.y ||
                slot.rhs->completed != VectorBytes)
                continue;
            if (!warmRhsOnchipReady(line,slot)) continue;
            std::memcpy(rhs.data(), slot.rhs->bytes.data(), VectorBytes);
        } else {
            rhs = line.rhs[cell];
        }
        const auto &matrix = line.tile ? lineCoeffCaches.at(line.desc.lineId).dInv[cell] :
                                        line.dInv[cell];
        if (env.lineMvmSplit) {
            const auto before = line.lineProducts;
            advanceLineMvm(line, 2, matrix, rhs, 0x1f,
                now, [this,cell](LineRequest &line, const auto &sum) {
                    if (line.tile) {
                        auto &slot = line.warm[cell % warmDepth(line)];
                        panic_if(!slot.valid || slot.cell != cell, "CFD stale warm base");
                        slot.base = sum;
                        slot.baseReady = true;
                    } else {
                        line.baseRing[cell % line.window] = sum;
                        line.baseReady[cell % line.window] = true;
                    }
                    line.baseCell++;
                });
            if (cell > line.cachedForwardCell)
                line.baseAheadColumns += line.lineProducts - before;
            continue;
        }
        if (!onchipColumnReady(line,2,col)) continue;
        std::array<double, N> values;
        for (unsigned row = 0; row < N; ++row)
            values[row] = env.onchipTiming ? line.mvm[2].columnOperands[col][row] : matrix[row * N + col];
        if (!issueColumnMvm(values, rhs[col], now,
            [this, token, cell, col](const auto &product) {
                auto it = lineRequests.find(token);
                if (it == lineRequests.end() || it->second->terminal)
                    return;
                auto &line = *it->second;
                auto &base = line.tile ? line.warm[cell % warmDepth(line)].base :
                                        line.baseRing[cell % line.window];
                if (!col)
                    base.fill(0.0);
                for (unsigned row = 0; row < N; ++row)
                    base[row] += product[row];
                line.basePending = false;
                if (++line.baseColumn == N) {
                    if (line.tile)
                        line.warm[cell % warmDepth(line)].baseReady = true;
                    else
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
        if (line.tile || line.terminal || !line.backwardDirect ||
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
        if (line.tile || line.terminal || !line.backwardDirect || line.backwardStarted ||
            !line.dqStarReady.back())
            continue;
        line.backwardStarted = true;
        line.backwardStartCycle = cycle();
        line.backwardCell = static_cast<int64_t>(line.cells) - 2;
        line.dq.back() = line.dqStar.back();
        publishLineOutput(line, line.cells - 1);
    }
}

bool
CfdCoeffPreprocessController::stepLineSummary(LineRequest &line)
{
    if (!env.timedInterface || !warmSummary(line))
        return true;
    if (line.abortPending || line.summary.completed != line.cells)
        return false;
    panic_if(line.summary.errors, "CFD failed warm line summary");
    if (!line.summaryTransfer) {
        line.summary.generation = line.desc.generation;
        line.summary.cells = line.cells;
        line.summaryTransfer = std::make_shared<InterfaceTransfer>();
        auto &t = *line.summaryTransfer;
        t.addr = line.desc.recordAddr;
        t.write = true;
        t.kind = "line-summary";
        t.bytes.resize(sizeof(line.summary));
        std::memcpy(t.bytes.data(),&line.summary,sizeof(line.summary));
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_RECORD_SUMMARY_SOURCE generation=%llu cells=%llu completed=%llu errors=%llu hash=%016llx\n",
                (unsigned long long)line.summary.generation,(unsigned long long)line.cells,
                (unsigned long long)line.summary.completed,(unsigned long long)line.summary.errors,
                (unsigned long long)line.summary.digest);
    }
    stepInterfaceTransfer(line.token,line.summaryTransfer);
    return line.summaryTransfer->completed == sizeof(line.summary);
}

void
CfdCoeffPreprocessController::finishLineBackward(LineRequest &line)
{
    if (line.terminal || !line.backwardStarted || line.backwardOpPending ||
        line.backwardCell >= 0)
        return;
    if (!line.backwardDoneCycle) {
        line.backwardDoneCycle = cycle();
        releaseBackwardOperands(line);
    }
    // Numeric completion is not software-visible completion: all child
    // drains and stable completion records must still retire successfully.
    if ((!line.coefficientCacheHit ||
         (line.desc.flags & CFD_COEFF_PRE_BASE_DIRECT_SOLVE)) &&
        line.completedChildren != line.cells)
        return;
    if (line.recordsPending)
        return;
    if (line.tile && (line.completedChildren != line.cells || !line.warmWrites.empty()))
        return;
    if (env.onchipTiming && std::any_of(onchipQueue.begin(),onchipQueue.end(),
        [&line](const auto &a) { return a->owner == line.token; })) return;
    // The internal feedback value may be ready long before its external
    // write completes. Never publish Complete on numerical completion alone.
    if (env.dmaSpm && (!line.outputWrites.empty() ||
        line.outputBytesCompleted != line.cells * VectorBytes))
        return;
    if (line.localUbar && !line.coefficientCacheHit)
        panic_if(line.smeUpperStored != line.cells-1 ||
                 (line.deferUpper && line.smeReverseDone != line.cells-1),
                 "CFD private Ubar work still live at line completion");
    if (env.timedInterface) {
        if (!stepLineSummary(line))
            return;
        if (!line.statusTransfer) {
            line.statusTransfer = std::make_shared<InterfaceTransfer>();
            auto &transfer = *line.statusTransfer;
            transfer.addr = line.desc.dInvOutAddr;
            transfer.write = true;
            transfer.kind = "status";
            const uint64_t complete = static_cast<uint64_t>(CfdCoeffPreprocessStatus::Complete);
            transfer.bytes.resize(sizeof(complete));
            std::memcpy(transfer.bytes.data(), &complete, sizeof(complete));
        }
        stepInterfaceTransfer(line.token, line.statusTransfer);
        if (line.statusTransfer->completed != line.statusTransfer->bytes.size())
            return;
    }
    line.status = CfdCoeffPreprocessStatus::Complete;
    if (line.localUbar && line.coefficientCacheEnabled && !line.coefficientCacheHit)
        publishLineCache(line);
    line.terminal = true;
    if (env.smeContexts)
        reportSme(line, cycle(), false);
    if (line.tile) {
        panic_if(!line.dqStar.empty() || !line.dq.empty() || !line.rhs.empty() ||
            !line.lu.empty() || !line.dInv.empty() || !line.lBar.empty() ||
            !line.uBar.empty() || line.tileReadIssued != line.tileReadCompleted ||
            line.tilePacketsIssued != line.tilePacketsCompleted ||
            line.cacheReadIssued != line.cacheReadCompleted ||
            line.cacheWriteIssued != line.cacheWriteCompleted,
            "CFD finite tile completion has unbounded arrays or in-flight reads");
        std::fprintf(stderr, "CFD_LINE_STORAGE token=%llu cells=%llu tile_cells=%u "
            "forward_tiles=%llu reload_tiles=%llu reserved=%llu resident=%llu "
            "capacity=%llu write_issued=%llu write_completed=%llu "
            "read_issued=%llu read_completed=%llu packets_issued=%llu "
            "packets_completed=%llu last_response=%llu stalls=%llu "
            "host_line_arrays=0 cache_bytes=%llu buffers=%u data_wait=%llu "
            "cache_read=%llu cache_write=%llu tile_y_only=%u prefetch_buffers=%u "
            "output_depth=%u explicit_output_budget=%u\n",
            (unsigned long long)line.token, (unsigned long long)line.cells,
            env.lineTileCells, (unsigned long long)line.tileForwardCount,
            (unsigned long long)line.tileReloadCount, (unsigned long long)line.tileReserve,
            (unsigned long long)line.tileAllocated, (unsigned long long)env.lineSpmBytes,
            (unsigned long long)line.tileWriteIssued, (unsigned long long)line.tileWriteCompleted,
            (unsigned long long)line.tileReadIssued, (unsigned long long)line.tileReadCompleted,
            (unsigned long long)line.tilePacketsIssued, (unsigned long long)line.tilePacketsCompleted,
            (unsigned long long)line.tileLastResponse, (unsigned long long)line.tileStalls,
            (unsigned long long)line.cacheAllocated, env.tileBuffers,
            (unsigned long long)line.tileWaitCycles,
            (unsigned long long)line.cacheReadCompleted, (unsigned long long)line.cacheWriteCompleted,
            line.tileYOnly, env.tilePrefetchBuffers, env.lineOutputDepth,
            env.explicitLineOutputDepth);
    }
    const uint64_t status = static_cast<uint64_t>(line.status);
    if (!env.timedInterface)
        proxy().writeBlob(line.desc.dInvOutAddr, &status, sizeof(status));
    if (env.timedInterface)
        std::fprintf(stderr, "CFD_INTERFACE_SUMMARY token=%llu read_issued=%llu "
            "read_completed=%llu write_issued=%llu write_completed=%llu "
            "packets_issued=%llu packets_completed=%llu last_response=%llu "
            "descriptor_bytes=%zu record_bytes=%zu status_bytes=8\n",
            (unsigned long long)line.token,
            (unsigned long long)line.interfaceReadIssued,
            (unsigned long long)line.interfaceReadCompleted,
            (unsigned long long)line.interfaceWriteIssued,
            (unsigned long long)line.interfaceWriteCompleted,
            (unsigned long long)line.interfacePacketsIssued,
            (unsigned long long)line.interfacePacketsCompleted,
            (unsigned long long)line.interfaceLastResponse,
            sizeof(CfdCoeffPreprocessDescriptor),
            recordBytes(line.desc.flags));
    if (env.lineOptLevel >= 5)
        std::fprintf(stderr,"CFD_LINE_LIFETIME phase=complete token=%llu records_pending=%llu record_stalls=%llu "
            "warm_input_depth=%u warm_output_depth=%u warm_output_peak=%llu "
            "warm_input_wait=%llu warm_base_wait=%llu warm_tile_wait=%llu warm_output_wait=%llu\n",
            (unsigned long long)line.token,(unsigned long long)line.recordsPending,
            (unsigned long long)line.recordDetachStalls,warmDepth(line),env.warmWriteDepth,
            (unsigned long long)line.warmWritesMax,(unsigned long long)line.warmInputStalls,
            (unsigned long long)line.warmBaseStalls,(unsigned long long)line.warmTileStalls,
            (unsigned long long)line.warmOutputStalls);
    if (!line.tile) {
        for (uint64_t cell = 0; cell < line.cells; ++cell)
            recordForwardOutputDigest(line, cell, line.dqStar[cell]);
    }
    panic_if(line.forwardDigestCells != line.cells ||
             line.backwardDigestCells != line.cells,
             "CFD line output digest cell count mismatch");
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
        char hashField[128];
        if (line.tile)
            std::snprintf(hashField, sizeof(hashField),
                          "result_hash_source=guest output_digest=%016llx",
                          (unsigned long long)line.outputDigest);
        else
            std::snprintf(hashField, sizeof(hashField),
                          "result_hash=%016llx output_digest=%016llx",
                          (unsigned long long)hash,
                          (unsigned long long)line.outputDigest);
        std::fprintf(stderr, "CFD_LINE_SCHEDULE token=%llu cache_hit=%d "
                     "base_ahead_columns=%llu backward_start=%llu "
                     "backward_done=%llu visible_done=%llu "
                     "children_retired=%llu %s "
                     "line_products=%llu line_adds=%llu buffered_max=%llu "
                     "buffer_stalls=%llu output_bytes_issued=%llu "
                     "output_bytes_completed=%llu output_packets_issued=%llu "
                     "output_packets_completed=%llu output_queue_max=%llu "
                     "output_queue_stalls=%llu output_outstanding_stalls=%llu "
                     "output_last_response=%llu\n",
                     (unsigned long long)line.token, line.coefficientCacheHit,
                     (unsigned long long)line.baseAheadColumns,
                     (unsigned long long)line.backwardStartCycle,
                     (unsigned long long)line.backwardDoneCycle,
                     (unsigned long long)cycle(),
                     (unsigned long long)line.childrenRetiredCycle,
                     hashField,
                     (unsigned long long)line.lineProducts,
                     (unsigned long long)line.lineAdds,
                     (unsigned long long)line.lineBufferedMax,
                     (unsigned long long)line.lineBufferStalls,
                     (unsigned long long)line.outputBytesIssued,
                     (unsigned long long)line.outputBytesCompleted,
                     (unsigned long long)line.outputPacketsIssued,
                     (unsigned long long)line.outputPacketsCompleted,
                     (unsigned long long)line.outputQueueMax,
                     (unsigned long long)line.outputQueueStalls,
                     (unsigned long long)line.outputOutstandingStalls,
                     (unsigned long long)line.outputLastResponseCycle);
    }
}

void
CfdCoeffPreprocessController::completeLineBackwardColumn(
    uint64_t lineToken, uint8_t column,
    const std::array<double, N> &product, uint64_t now)
{
    auto it = lineRequests.find(lineToken);
    if (it == lineRequests.end())
        return;
    LineRequest &line = *it->second;
    if (!line.backwardStarted || line.terminal || line.backwardCell < 0 ||
        column != line.backwardColumn)
        return;
    for (unsigned row = 0; row < N; ++row)
        line.backwardAccumulator[row] +=
            product[row];
    line.backwardOpPending = false;
    if (++line.backwardColumn < N)
        return;
    finishLineBackwardCell(line);
    (void)now;
}

void
CfdCoeffPreprocessController::finishLineBackwardCell(LineRequest &line, bool resultReady)
{
    if (env.sharedCluster && !line.backwardSubtractReady && !resultReady) {
        line.backwardFinishWaiting = true;
        return;
    }
    line.backwardSubtractReady = false;
    const size_t cell = static_cast<size_t>(line.backwardCell);
    for (unsigned row = 0; row < N; ++row)
        if (line.tile)
            line.backwardValue[row] = resultReady ? line.backwardAccumulator[row] :
                                     (env.onchipTiming ? line.backwardYOperand[row] :
                                      line.tile->y[tileIndex(line, cell)][row]) -
                                      line.backwardAccumulator[row];
        else
            line.dq[cell][row] = resultReady ? line.backwardAccumulator[row] :
                line.dqStar[cell][row] - line.backwardAccumulator[row];
    publishLineOutput(line, cell);
    if (line.tile)
        line.backwardNext = line.backwardValue;
    line.backwardFeedbackReadyCycle =
        cycle() + (env.smeContexts && env.smeFeedbackReg ? 1u : 0u);
    if (env.traceEnable)
        std::fprintf(stderr, "CFD_LINE_FEEDBACK token=%llu cell=%llu finish=%llu\n",
            (unsigned long long)line.token, (unsigned long long)cell,
            (unsigned long long)cycle());
    line.backwardAccumulator.fill(0.0);
    line.backwardColumn = 0;
    line.backwardCell--;
    finishLineBackward(line);
}

void
CfdCoeffPreprocessController::publishLineOutput(
    LineRequest &line, uint64_t cell)
{
    recordBackwardOutputDigest(line, cell,
                               line.tile ? line.backwardValue : line.dq[cell]);
    if (!env.dmaSpm) {
        proxy().writeBlob(line.desc.reserved[2] + cell * VectorBytes,
                         line.dq[cell].data(), VectorBytes);
        return;
    }
    panic_if(line.outputWrites.size() >= env.lineOutputDepth,
             "CFD line output queue reservation exceeded");
    auto write = std::make_shared<LineOutputWrite>();
    write->cell = cell;
    write->readyCycle = cycle();
    write->value = line.tile ? line.backwardValue : line.dq[cell];
    line.outputWrites.push_back(std::move(write));
    line.outputQueueMax = std::max<uint64_t>(
        line.outputQueueMax, line.outputWrites.size());
}

void
CfdCoeffPreprocessController::processLineOutputWrites()
{
    if (!env.dmaSpm)
        return;
    CfdLocalSpm *spm = getCfdLocalSpm();
    panic_if(!spm, "CFD line output DMA requires local SPM");
    for (auto &[token, ptr] : lineRequests) {
        auto &line = *ptr;
        auto &queue = line.outputWrites;
        // Responses may return out of order; retire only complete entries.
        queue.erase(std::remove_if(queue.begin(), queue.end(),
            [](const auto &write) { return write->completed == VectorBytes; }),
            queue.end());
        if (line.terminal)
            continue;
        if (env.outputCoalesce) {
            processCoalescedLineOutputWrites(line);
            continue;
        }
        auto candidate = std::find_if(queue.begin(), queue.end(),
            [](const auto &write) { return write->issued < VectorBytes; });
        if (candidate == queue.end())
            continue;
        auto write = *candidate;
        const Addr addr = line.desc.reserved[2] +
            write->cell * VectorBytes + write->issued;
        const unsigned bytes = spm->dmaChunkSize(tc, addr,
            std::min<unsigned>(env.drainWidth, VectorBytes - write->issued));
        auto *data = reinterpret_cast<uint8_t *>(write->value.data()) +
            write->issued;
        const auto packet = spm->issueDma(tc, true, addr, bytes, data,
            env.spmOutstanding, [this, token, write, bytes](uint64_t) {
                write->completed += bytes;
                auto it = lineRequests.find(token);
                if (it == lineRequests.end())
                    return;
                auto &line = *it->second;
                line.outputBytesCompleted += bytes;
                line.outputPacketsCompleted++;
                line.outputLastResponseCycle = cycle();
            });
        if (!packet.accepted) {
            line.outputOutstandingStalls += packet.outstandingFull;
            // Translation/chunk errors must not become an infinite retry.
            panic_if(!packet.outstandingFull,
                     "CFD line output DMA failed at address %#llx",
                     (unsigned long long)addr);
            continue;
        }
        write->issued += bytes;
        line.outputBytesIssued += bytes;
        line.outputPacketsIssued++;
    }
}

void
CfdCoeffPreprocessController::processLineBackward(uint64_t now)
{
    for (auto &[lineToken, linePtr] : lineRequests) {
        LineRequest &line = *linePtr;
        processBackwardOperands(line);
        if (!line.backwardStarted || line.terminal ||
            line.backwardOpPending)
            continue;
        if (line.backwardCell < 0) {
            finishLineBackward(line);
            continue;
        }
        if (env.smeWarmBackSplit && line.coefficientCacheHit) {
            processSmeSplitBack(line);
            continue;
        }
        if (line.tile && !backwardTileBound(*line.tile,line.backwardCell)) {
            line.tileStalls++;
            continue;
        }
        // A line has one sequential backward producer. Reserving space before
        // its next operation also bounds the eventual completion enqueue.
        if (env.dmaSpm && line.outputWrites.size() >= env.lineOutputDepth) {
            line.outputQueueStalls++;
            continue;
        }
        if (env.smeContexts) {
            if (!processSmeBackward(line, now) || !env.smeConfig.ingressPorts ||
                line.terminal || line.backwardCell < 0 ||
                line.outputWrites.size() >= env.lineOutputDepth)
                continue;
            const uint64_t next = line.backwardCell;
            const auto &operand = line.backwardOperands[next % env.backwardOperandDepth];
            // A warm operand slot already owns its timed y and Ubar reads.
            // It can use the returned feedback before the tile pointer rotates.
            const bool prefetched = line.coefficientCacheHit && env.backwardIndependentReady &&
                operand.cell == next && operand.yReady && operand.readyColumns == 0x1f;
            if (prefetched || backwardTileBound(*line.tile, next))
                processSmeBackward(line, now);
            continue;
        }
        if (line.backwardFinishWaiting) {
            if (issue(forwardCombine, OpKind::ForwardCombine, now,
                [this, lineToken] {
                    auto it = lineRequests.find(lineToken);
                    if (it == lineRequests.end() || it->second->terminal)
                        return;
                    auto &line = *it->second;
                    line.backwardOpPending = false;
                    line.backwardFinishWaiting = false;
                    line.backwardSubtractReady = true;
                    finishLineBackwardCell(line);
                }))
                line.backwardOpPending = true;
            continue;
        }
        if (env.lineMvmSplit) {
            const size_t cell = static_cast<size_t>(line.backwardCell);
            if (!line.tile && !line.dqStarReady[cell])
                continue;
            advanceLineMvm(line, 1,
                cachedUbar(line) ? lineCoeffCaches.at(line.desc.lineId).uBar[cell] :
                    line.tile ? line.tile->u[tileIndex(line, cell)] : line.uBar[cell],
                line.tile ? line.backwardNext : line.dq[cell + 1],
                line.tile ? 0x1f : line.uBarReady[cell], now,
                [this](LineRequest &line, const auto &sum) {
                    line.backwardAccumulator = sum;
                    finishLineBackwardCell(line, env.backwardFeedbackTree);
                });
            continue;
        }
        const uint8_t column = line.backwardColumn;
        if (!onchipColumnReady(line,1,column)) continue;
        if (!line.tile && (!line.dqStarReady[line.backwardCell] ||
            !(line.uBarReady[line.backwardCell] & (1u << column))))
            continue;
        std::array<double, N> values;
        for (unsigned row = 0; row < N; ++row)
            values[row] = env.onchipTiming ? line.mvm[1].columnOperands[column][row] : (cachedUbar(line) ? lineCoeffCaches.at(line.desc.lineId).uBar[line.backwardCell] :
                line.tile ? line.tile->u[tileIndex(line, line.backwardCell)] :
                line.uBar[line.backwardCell])[row * N + column];
        if (!issueColumnMvm(values, (line.tile ? line.backwardNext :
                line.dq[line.backwardCell + 1])[column], now,
                   [this, lineToken, column](const auto &product) {
                       completeLineBackwardColumn(
                           lineToken, column, product, cycle());
                   }))
            continue;
        line.backwardOpPending = true;
    }
}

uint64_t
CfdCoeffPreprocessController::wait(uint64_t token)
{
    if (pendingLineLaunches.count(token))
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::Busy);
    auto lineIt = lineRequests.find(token);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        if (!line.terminal || (line.abortPending && !line.abortPublished))
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
        if (tickEvent.scheduled() && pendingLineLaunches.empty() && !activeRequest && !activeLine &&
            pendingOps.empty() && !rhsPairs.live() && !rhsUpper.live && !arithmeticGroups.live && columnFmaQueue.empty() &&
            forwardCombineQueue.empty() && drainQueue.empty() &&
            activeDrains.empty() && detachedRecords.empty() && !summaryBusy() && onchipQueue.empty())
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
    if (pendingLineLaunches.count(token)) {
        cancelledLineLaunches[token] = true;
        scheduleTick();
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::CancelPending);
    }
    auto lineIt = lineRequests.find(token);
    if (lineIt != lineRequests.end()) {
        LineRequest &line = *lineIt->second;
        if (line.terminal)
            return static_cast<uint64_t>(
                CfdCoeffPreprocessStatus::AlreadyComplete);
        // A completion word already accepted by memory cannot be revoked.
        if (line.statusTransfer && line.statusTransfer->issued)
            return static_cast<uint64_t>(CfdCoeffPreprocessStatus::AlreadyComplete);
        abortLine(line, CfdCoeffPreprocessStatus::Cancelled);
        scheduleTick();
        return static_cast<uint64_t>(CfdCoeffPreprocessStatus::CancelPending);
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
    discardColdProducts(*req);
    discardRhsRows(*req);
    req->record.status = static_cast<uint64_t>(req->status);
    req->record.completeCycle = cycle();
    trace(*req, "controller", pending ? "CANCEL_PENDING" : "CANCEL_COMPLETE");
    if (!env.timedInterface)
        writeGuestRecord(*req);
    return static_cast<uint64_t>(pending ?
        CfdCoeffPreprocessStatus::CancelPending :
        CfdCoeffPreprocessStatus::Cancelled);
}

void
CfdCoeffPreprocessController::completeOps(uint64_t now)
{
    dividerResponse.reset();
    if (env.detailedDivider) {
        // Divider returns first. A held return never resets a producer or
        // releases a credit before its result has physically entered the RF.
        dividerResponse = hardwareDivider.tick(now,now >= nextDividerWriteback &&
            (!rfEnabled() || registerPorts.canWrite(now,1)));
        if (dividerResponse) {
            panic_if(rfEnabled() && !registerPorts.reserveWrite(now,1),
                     "CFD divider lost RF write credit");
            nextDividerWriteback = now+env.dividerWritebackIi;
            auto *owner = find(dividerResponse->owner);
            const bool dropped = !owner || owner->stage == RequestStage::Failed;
            dividerDropped += dropped;
            dividerFlags |= dividerResponse->value.flags;
            traceDivider(*dividerResponse,"result",now,dropped);
        }
    }
    std::vector<PendingOp> remaining;
    remaining.reserve(pendingOps.size());
    if (rfEnabled())
        std::stable_sort(pendingOps.begin(),pendingOps.end(),
            [](const PendingOp &a,const PendingOp &b) {
                return a.completeCycle < b.completeCycle;
            });
    bool returnBlocked = false;
    for (auto &op : pendingOps) {
        bool ready = op.dividerTag ?
            (dividerResponse && dividerResponse->tag == op.dividerTag) :
            op.completeCycle <= now;
        if (ready && op.bankAccess && op.bank.destination>=0) {
            if (!op.bankStored && localVectors.write(now,op.bank.destination)) {
                op.bankStored=true;
                op.bankPublish=now+env.localRouteLatency;
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_LOCAL_WRITE cycle=%llu id=%llu bank=%d words=%u owner=%llu epoch=%llu\n",
                        (unsigned long long)now,(unsigned long long)op.rfTag,
                        op.bank.destination,op.lanes,(unsigned long long)op.bank.owner,
                        (unsigned long long)op.bank.epoch);
                if (env.localCreditRelease && op.rfPool) {
                    panic_if(op.rfPool->rfLive < op.lanes,"CFD early local credit underflow");
                    op.rfPool->rfLive -= op.lanes;
                    op.fuReleased = true;
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_RF_RELEASE cycle=%llu id=%llu pool=%s lanes=%u nominal=%llu\n",
                            (unsigned long long)now,(unsigned long long)op.rfTag,
                            rfPoolName(op.rfPool),op.lanes,(unsigned long long)op.completeCycle);
                }
            }
            ready=op.bankStored && now>=op.bankPublish;
            if (!ready) observe(op.bankStored ? "local_vector_route" : "local_vector_write_ports");
        } else if (ready && op.localResult) {
            ready = lanePorts[op.localRow].reserveWrite(now,1);
            if (!ready) observe("lane_local_write_ports");
            if (ready && env.traceEnable)
                std::fprintf(stderr,"CFD_LANE_WRITE cycle=%llu id=%llu row=%u\n",
                    (unsigned long long)now,(unsigned long long)op.rfTag,op.localRow);
        } else if (ready && rfEnabled() && !op.dividerTag) {
            if (env.rfSplitWriteback && !returnBlocked) {
                const unsigned pending = op.lanes-op.returnedLanes;
                const auto width = registerPorts.configuration().writeWordsPerCycle;
                const unsigned grant = width ? std::min<uint64_t>(pending,
                    width-registerPorts.writesUsed()) : pending;
                if (grant) {
                    panic_if(!registerPorts.reserveWrite(now,grant),"CFD split RF write lost credit");
                    if (env.traceEnable)
                        std::fprintf(stderr,"CFD_RF_WRITE cycle=%llu id=%llu pool=%s lanes=%u offset=%u nominal=%llu\n",
                            (unsigned long long)now,(unsigned long long)op.rfTag,
                            rfPoolName(op.rfPool),grant,op.returnedLanes,
                            (unsigned long long)op.completeCycle);
                    op.returnedLanes += grant;
                }
                ready = op.returnedLanes == op.lanes;
            } else if (!env.rfSplitWriteback && !returnBlocked) {
                ready = registerPorts.reserveWrite(now,op.lanes);
            } else ready = false;
            if (!ready) {
                returnBlocked = true;
                observe("rf_result_ports");
            }
        }
        if (ready) {
            if (op.bankAccess) localVectors.release();
            if (op.rfTag) {
                if (op.rfPool && !op.fuReleased) {
                    panic_if(op.rfPool->rfLive < op.lanes,"CFD RF credit underflow");
                    op.rfPool->rfLive -= op.lanes;
                }
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_RF_COMPLETE cycle=%llu id=%llu pool=%s "
                        "lanes=%u nominal=%llu\n",(unsigned long long)now,
                        (unsigned long long)op.rfTag,rfPoolName(op.rfPool),op.lanes,
                        (unsigned long long)op.completeCycle);
            }
            op.complete();
        } else remaining.push_back(std::move(op));
    }
    pendingOps.swap(remaining);
    if (env.detailedDivider) {
        const size_t live = std::count_if(pendingOps.begin(),pendingOps.end(),
            [](const PendingOp &op) { return op.dividerTag != 0; });
        panic_if(live != hardwareDivider.coreSize()+hardwareDivider.resultSize(),
                 "CFD divider token/completion ownership mismatch");
    }
}

void
CfdCoeffPreprocessController::assignInputSlots(uint64_t now)
{
    for (Request *ptr : admissionOrder()) {
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
    for (Request *ptr : admissionOrder()) {
        Request &req = *ptr;
        // Input ownership is independent of compute state once D may start
        // LU early. Remaining DMA/store/load work must keep advancing.
        const bool split = splitInput(req);
        const bool coldR = coldRhsInput(req);
        const unsigned inputBytes = req.record.inputBytes-
            (coldR ? VectorBytes : 0);
        if (req.stage != RequestStage::InputFetching &&
            !(split && req.slotOwned && !req.inputOperandsReady &&
              req.stage != RequestStage::Failed &&
              req.stage != RequestStage::Complete))
            continue;
        if (split) prepareInputD(req);
        if (coldR) consumeColdRhs(req,now);
        if (batchInput(req)) {
            prepareInputBatches(req,now);
            if (!env.smeContexts && req.solveStarted && !req.solveParked) {
                initializeRhs(req);activateRhs(req);
            }
        }
        if (env.onchipTiming && req.coefficientCacheReuse) cachedRegistersReady(req);
        if (consumeDirectRhs(req,now)) continue;
        if (!req.inputCaptured) {
            if (!env.dmaSpm) {
                if (!req.coefficientCacheReuse)
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
            if (req.inputBytesIssued >= inputBytes &&
                req.inputChunksCompleted >= req.inputChunksIssued) {
                if (env.onchipTiming) {
                    auto &load = onchipContexts[req.token].inputRead;
                    const unsigned skip = split ? MatrixBytes : 0;
                    if (batchInput(req)) {
                        if (!req.inputBatchLoadsDone) continue;
                    } else if (!load && inputBytes > skip) {
                        load = startOnchip(childLineTokens.at(req.token),
                            inputOnchipAddr(req)+skip,
                            (inputBytes-skip)/8,8,false,2);
                        if (split && load) traceInputSplit(req,"rest_read",load);
                    }
                    if (inputBytes > skip && (!load || !load->done)) continue;
                    if (split && !req.inputDReady) continue;
                    if (coldR && !req.rhsVectorReady) continue;
                    if (!cachedRegistersReady(req)) continue;
                }
                if (req.cacheUbarReload)
                    req.uBar = req.upper;
                req.record.inputDoneCycle = now;
                if (req.baseConsumerEnabled && !coldR && !req.rhsVectorReady) {
                    req.rhsVectorReady = true;
                    req.record.rhsVectorReadyCycle = now;
                }
                req.inputOperandsReady = true;
                if (req.stage == RequestStage::InputFetching)
                    req.stage = RequestStage::InputReady;
                if (split) traceInputSplit(req,"input_ready",
                    inputBytes > MatrixBytes ? onchipContexts.at(req.token).inputRead :
                        onchipContexts.at(req.token).dRead);
                trace(req, "input", "INPUT_TOKEN");
                if (!env.smeContexts && (coldR || batchInput(req)) &&
                    req.solveStarted && !req.solveParked) {
                    initializeRhs(req);
                    activateRhs(req);
                }
                // Early LU cannot release the input landing slot. It is
                // reclaimable only after all other DMA and operand loads.
                if (split && req.record.luIssueCycle && req.slotOwned) {
                    slots[req.slot] = false;
                    req.slotOwned = false;
                    trace(req,"input","INPUT_SLOT_RELEASE");
                }
                continue;
            }
            if (req.inputBytesIssued >= inputBytes) {
                req.record.stallDependency++;
                continue;
            }
            if (coalesceRhsInput(req,now)) continue;
            CfdLocalSpm *spm = getCfdLocalSpm();
            if (!spm) {
                markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                           "missing-local-spm");
                continue;
            }
            const unsigned offset = req.inputBytesIssued;
            const unsigned matrixOffset = inputRegionOffset(req, offset);
            const unsigned regionRemaining = inputRegionBytes(req, offset) - matrixOffset;
            const CfdSpmMatrix matrix = inputMatrix(req, offset);
            unsigned bytes = std::min<unsigned>(
                env.spmWriteWidth, inputBytes - offset);
            const bool packed = !req.coefficientCacheReuse &&
                (req.desc.flags & CFD_COEFF_PRE_PACKED_COLD_INPUT);
            Addr addr = env.dmaSpm ? inputDmaBase(req, matrix) + matrixOffset :
                packetAddress(req, matrix, matrixOffset, offset, true);
            // A packed DMA beat may span adjacent fields. Skip an unused
            // boundary matrix by ending the beat at the preceding field.
            if (bytes > regionRemaining && (!packed ||
                inputDmaBase(req, inputMatrix(req, offset + regionRemaining)) !=
                    addr + regionRemaining))
                bytes = regionRemaining;
            if (env.spmLayout == SpmLayout::RowStriped &&
                matrix != CfdSpmMatrix::RhsVector)
                bytes = std::min<unsigned>(bytes,
                    N * sizeof(double) -
                    matrixOffset % (N * sizeof(double)));
            if (env.dmaSpm) {
                bytes = spm->dmaChunkSize(tc, addr, bytes);
                if (bytes && !req.record.rhsVectorIssueCycle &&
                    inputMatrix(req, offset + bytes - 1) == CfdSpmMatrix::RhsVector)
                    req.record.rhsVectorIssueCycle = now;
            }
            const uint64_t tok = req.token;
            auto payload = env.dmaSpm && env.lineOptLevel >= 5 ?
                std::make_shared<std::vector<uint8_t>>(bytes) : nullptr;
            panic_if(payload && bytes > 64, "CFD DMA staging exceeds its reserved slot");
            uint8_t *data = payload ? payload->data() : env.dmaSpm ?
                inputOperandData(req, matrix) + matrixOffset : nullptr;
            if (env.dmaSpm && spm->dmaOutstandingFull(env.spmOutstanding)) {
                spm->noteDmaOutstandingStall();
                req.record.coeffSpmOutstandingStalls++;
                trace(req, "spm", "SPM_PACKET_STALL_OUTSTANDING", -1,
                      -1, -1, -1, "write", "CoeffInput", 0, addr, bytes);
                continue;
            }
            auto packetMeta = std::make_shared<CfdSpmPacketIssue>();
            auto local = env.onchipTiming ? startOnchip(childLineTokens.at(tok),
                inputOnchipAddr(req)+offset,(bytes+7)/8,8,true,2,false) : nullptr;
            if (env.onchipTiming && !local) continue;
            auto completion =
                [this, tok, offset, bytes, addr, packetMeta, local, payload](
                    uint64_t packetId) {
                    auto publish = [this,tok,offset,bytes,packetId,addr,packetMeta,payload] {
                        completeInputPacket(tok, offset, bytes, packetId, addr,
                            packetMeta->bank, packetMeta->port,
                            payload ? payload->data() : nullptr);
                    };
                    if (local) {
                        activateOnchip(local, std::move(publish));
                    }
                    else publish();
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
                cancelOnchip(local);
                req.record.coeffSpmOutstandingStalls++;
                trace(req, "spm", "SPM_PACKET_STALL_OUTSTANDING", -1,
                      -1, -1, -1, "write", "CoeffInput", 0, addr, bytes);
                continue;
            }
            *packetMeta = packet;
            traceSpmInputStore(req,local,offset,packet.packetId);
            if (packed && env.traceEnable && req.desc.cellId < env.traceCells)
                std::fprintf(stderr, "CFD_SME_PACKED_PACKET cycle=%llu owner=%llu "
                    "request=%llu cell=%llu offset=%u slot=%d packet=%llu job=%llu\n",
                    (unsigned long long)now, (unsigned long long)local->owner,
                    (unsigned long long)req.record.requestId,
                    (unsigned long long)req.desc.cellId, offset, req.slot,
                    (unsigned long long)packet.packetId, (unsigned long long)local->id);
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
    uint64_t token, unsigned offset, unsigned bytes, uint64_t packetId, Addr addr,
    unsigned bank, unsigned port, const uint8_t *payload)
{
    Request *req = find(token);
    if (!req || req->stage == RequestStage::Failed)
        return;
    // Keep the packet and SPM write accounted once. Only the per-operand
    // byte counts are split, after the write job has retired.
    for (unsigned consumed = 0; consumed < bytes;) {
        const unsigned pos = offset + consumed;
        const CfdSpmMatrix matrix = inputMatrix(*req, pos);
        const unsigned regionOffset = inputRegionOffset(*req, pos);
        const unsigned regionBytes = inputRegionBytes(*req, pos);
        const unsigned size = std::min(bytes - consumed, regionBytes - regionOffset);
        if (payload)
            std::memcpy(inputOperandData(*req, matrix) + regionOffset,
                        payload + consumed, size);
        if (splitInput(*req) && matrix == CfdSpmMatrix::D) {
            req->dBytesCompleted += size;
            panic_if(req->dBytesCompleted > MatrixBytes,
                     "CFD duplicate D input completion");
        } else if (batchInput(*req)) {
            const int index = matrix == CfdSpmMatrix::L ? 0 :
                matrix == CfdSpmMatrix::U ? 1 : matrix == CfdSpmMatrix::RhsVector ? 2 : -1;
            if (index >= 0) {
                req->inputBatchBytes[index] += size;
                panic_if(req->inputBatchBytes[index] > regionBytes,
                         "CFD duplicate batch input completion");
            }
        }
        consumed += size;
    }
    req->inputChunksCompleted++;
    req->record.coeffSpmPacketCompleted++;
    trace(*req, "spm", "SPM_PACKET_RESPONSE", -1, -1, -1, -1,
          "write-complete", "CoeffInput", packetId, addr, bytes, bank,
          port);
}

void
CfdCoeffPreprocessController::startLuRequests(uint64_t now)
{
    for (auto &owner : localLuOwners) {
        if (!owner) continue;
        auto *r=find(owner);
        if ((!r || r->luDone || r->stage==RequestStage::Failed) &&
            (!env.luRank1 || !luRankStates.at(&owner-localLuOwners.data()).inflight) &&
            std::none_of(pendingOps.begin(),pendingOps.end(),[this,owner,bank=27+(&owner-localLuOwners.data())](const auto &op) {
                const int second=env.luProductAhead ? bank+env.luCount+env.rhsProductDepth : bank;
                const int upper=env.luUpperOverlap ? luUpperBank(owner,0) : -1;
                return op.bankAccess && op.bank.owner==owner &&
                    (op.bank.a==bank || op.bank.b==bank || op.bank.destination==bank ||
                     op.bank.a==second || op.bank.b==second || op.bank.destination==second ||
                     (upper>=0 && (op.bank.a==upper || op.bank.a==upper+1 ||
                       op.bank.b==upper || op.bank.b==upper+1 ||
                       op.bank.destination==upper || op.bank.destination==upper+1)));
            })) owner=0;
    }
    uint32_t active = 0, queued = 0;
    for (auto &[token, req] : requests)
        if (req->stage == RequestStage::LuActive)
            active++;
        else if (req->stage == RequestStage::LuQueued)
            queued++;
    for (Request *ptr : admissionOrder()) {
        Request &req = *ptr;
        const bool early = splitInput(req) && req.inputDReady &&
            req.stage == RequestStage::InputFetching;
        if (req.stage != RequestStage::InputReady &&
            req.stage != RequestStage::LuQueued && !early)
            continue;
        if (req.coefficientCacheReuse) {
            if (req.slotOwned) {
                slots[req.slot] = false;
                req.slotOwned = false;
                trace(req, "input", "INPUT_SLOT_RELEASE");
            }
            req.forwardStepReady.fill(true);
            req.backwardStepReady.fill(true);
            for (unsigned k = 0; k < N; ++k)
                req.record.luStepReadyCycle[k] = now;
            req.stage = RequestStage::LuReady;
            trace(req, "lu", "LU_CACHE_REUSE");
            tracePrivateLu(req);
            if (req.cachedReciprocal)
                trace(req,"lu","LU_RECIPROCAL_REUSE");
            continue;
        }
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
        if (env.localProducts) {
            auto free=std::find(localLuOwners.begin(),localLuOwners.end(),0);
            if (free==localLuOwners.end()) { observe("local_lu_owner_full"); continue; }
            *free=req.token;
        }
        if (req.stage == RequestStage::LuQueued)
            queued--;
        if (req.slotOwned && (!splitInput(req) || req.inputOperandsReady)) {
            slots[req.slot] = false;
            req.slotOwned = false;
            trace(req, "input", "INPUT_SLOT_RELEASE");
        }
        req.stage = RequestStage::LuActive;
        req.record.luIssueCycle = now;
        req.luPhase = LuPhase::StartLower;
        active++;
        trace(req, "lu", "LU_ISSUE");
        if (env.luRank1) {
            const unsigned index=luProductBank(req.token)-27;
            luRankStates.at(index).bind(req.token,req.desc.generation,now,env.luControlFold);
            // Rename the already paid D input into the owned matrix RF.
            // Later trailing SUBs write their real RF results in place.
            req.lu=req.d;
            traceLuRank1(req,"bind",0,-1,index,0,luAheadBank(req.token,0));
            traceLuFmaMatrix(req,"input");
        }
    }
}

void
CfdCoeffPreprocessController::processLu(uint64_t now)
{
    Request *frontier = nullptr;
    if (env.lineOptLevel >= 7) {
        for (Request *r : admissionOrder())
            if (!r->handoffProduced && r->stage != RequestStage::Complete &&
                r->stage != RequestStage::Failed) { frontier = r; break; }
    }
    std::vector<Request *> visit;
    for (auto &[token,ptr] : requests) visit.push_back(ptr.get());
    if (luLookaheadPass)
        std::sort(visit.begin(),visit.end(),[](auto *a,auto *b) {
            return a->desc.cellId < b->desc.cellId;
        });
    for (Request *ptr : visit) {
        Request &req = *ptr;
        const uint64_t token = req.token;
        if (req.stage != RequestStage::LuActive)
            continue;
        DiagnosticOwner owner(diagnosticOwner, req.token);
        if (env.lineOptLevel >= 7) {
            const bool ahead = env.luLookahead && frontier &&
                req.desc.lineId == frontier->desc.lineId &&
                req.desc.cellId > frontier->desc.cellId &&
                req.desc.cellId-frontier->desc.cellId <= env.luLookahead;
            if (luLookaheadPass ? !ahead : luBackgroundPass ?
                    (&req == frontier || ahead) : &req != frontier) continue;
            const auto mul = req.record.luMulIssued, add = req.record.luSubIssued,
                       div = req.record.luDivIssued;
            const auto fusedBefore=env.luFmaCount ? luRankStates.at(luProductBank(req.token)-27).fusedLanes : 0;
            processLuRows(req, now);
            const auto fused=env.luFmaCount ? luRankStates.at(luProductBank(req.token)-27).fusedLanes-fusedBefore : 0;
            if (luLookaheadPass && env.traceEnable &&
                (mul != req.record.luMulIssued || add != req.record.luSubIssued ||
                 div != req.record.luDivIssued))
                std::fprintf(stderr,"CFD_LU_LOOKAHEAD cycle=%llu request=%llu cell=%llu frontier=%llu mul=%llu add=%llu div=%llu lu_fma=%llu\n",
                    (unsigned long long)now,(unsigned long long)req.record.requestId,
                    (unsigned long long)req.desc.cellId,(unsigned long long)frontier->desc.cellId,
                    (unsigned long long)(req.record.luMulIssued-mul-fused),
                    (unsigned long long)(req.record.luSubIssued-add-fused),
                    (unsigned long long)(req.record.luDivIssued-div),
                    (unsigned long long)fused);
            continue;
        }
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
            if (!issueDivide(luDiv, OpKind::LuDiv, now, tok,
                req.luTemp,req.lu[req.luCol * N + req.luCol],
                [this, tok](double value) {
                Request *r = find(tok);
                if (!r) return;
                r->luTemp = value;
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
            tracePrivateLu(req);
            break;
        }
    }
}

void
CfdCoeffPreprocessController::initializeRhs(Request &req)
{
    for (unsigned rhs = 0; rhs < req.rhsSlots; ++rhs) {
        if (!(req.rhsEnabledMask & (1u << rhs)) ||
            (req.rhsInitializedMask & (1u << rhs)))
            continue;
        RhsState &state = req.rhs[rhs];
        const unsigned batch = rhsBatch(req, rhs);
        const unsigned col = rhsColumn(req, rhs);
        if (batchInput(req)) {
            // Only the current forward inputs may start early. Ubar must
            // not occupy a solve context while b/L are still in flight.
            if (batch==0 ? !req.rhsVectorReady : batch==1 ? !req.inputLowerReady :
                    (!req.inputUpperReady || !req.inputOperandsReady))
                continue;
        } else if (coldRhsInput(req) && (batch ? !req.inputOperandsReady :
                   !req.rhsVectorReady)) continue;
        for (unsigned row = 0; row < N; ++row) {
            if (batch == 0)
                state.value[row] = req.directBaseSolve ? req.rhsVector[row] :
                    (row == col ? 1.0 : 0.0);
            else if (batch == 1)
                state.value[row] = req.lower[row * N + col];
            else
                state.value[row] = req.upper[row * N + col];
        }
        state.phase = RhsPhase::ForwardDiv;
        state.k = 0;
        state.i = 1;
        req.rhsInitializedMask |= 1u << rhs;
        if (coldRhsInput(req) || batchInput(req)) trace(req,"solve","RHS_INPUT_INIT",batch,col);
    }
}

void
CfdCoeffPreprocessController::activateRhs(Request &req)
{
    const bool held = holdTailUbar(req);
    if (held && !req.tailUbarMarked) {
        req.tailUbarMarked = true;
        trace(req,"solve","TAIL_UBAR_HOLD");
    }
    if (!held && req.tailUbarMarked && !req.tailUbarReleased) {
        req.tailUbarReleased = true;
        trace(req,"solve","TAIL_UBAR_RELEASE");
    }
    unsigned active = 0;
    for (unsigned i = 0; i < req.rhsSlots; ++i)
        active += req.rhs[i].active && !req.rhs[i].done;
    while (active < env.rhsLanes) {
        const uint16_t candidates = req.rhsEnabledMask & req.rhsInitializedMask &
            ~req.rhsActivatedMask & ~req.rhsDoneMask & (held ? ~0x7c00u : 0x7fffu);
        if (!candidates)
            break;
        unsigned rhs = 0;
        while (!(candidates & (1u << rhs)))
            ++rhs;
        req.rhs[rhs].active = true;
        req.rhsActivatedMask |= 1u << rhs;
        if (req.tailUbarMarked && rhs >= 10)
            trace(req,"solve","TAIL_UBAR_ACTIVATE",rhs/5,rhs%5);
        active++;
    }
}

void
CfdCoeffPreprocessController::startSolveRequests(uint64_t now)
{
    uint32_t active = 0, queued = 0;
    for (auto &[token, req] : requests)
        if (req->solveStarted && !req->solveDone && !req->solveParked)
            active++;
        else if (req->stage == RequestStage::SolveQueued)
            queued++;
    const auto ordered=admissionOrder();
    Request *protectedBase=nullptr;
    if(env.rhsFrontierReserve) {
        for(auto *r:ordered) {
            if(r->directBaseSolve && !r->baseComputeComplete &&
               r->stage!=RequestStage::Failed && r->stage!=RequestStage::Complete) {
                protectedBase=r;break;
            }
        }
    }
    auto contextAvailable=[this,now,&active,protectedBase](const Request &candidate) {
        const bool waiting=protectedBase &&
            (!protectedBase->solveStarted || protectedBase->solveParked);
        const bool permit=!env.rhsFrontierReserve ? active<env.solveCount :
            CfdRhsAdmission::permit(active,env.solveCount,waiting,protectedBase==&candidate);
        if(!permit && active<env.solveCount) {
            observe("rhs_frontier_reserved_context");
            if(env.traceEnable)
                std::fprintf(stderr,"CFD_RHS_ADMISSION cycle=%llu event=hold token=%llu generation=%llu request=%llu cell=%llu protected_token=%llu protected_generation=%llu protected_request=%llu protected_cell=%llu active=%u capacity=%u bytes=%llu\n",
                    (unsigned long long)now,(unsigned long long)candidate.token,
                    (unsigned long long)candidate.desc.generation,(unsigned long long)candidate.record.requestId,
                    (unsigned long long)candidate.desc.cellId,(unsigned long long)protectedBase->token,
                    (unsigned long long)protectedBase->desc.generation,(unsigned long long)protectedBase->record.requestId,
                    (unsigned long long)protectedBase->desc.cellId,active,env.solveCount,
                    (unsigned long long)CfdRhsAdmission::Reservation);
        }
        return permit;
    };
    for (Request *ptr : ordered) {
        Request &req = *ptr;
        if (req.stage == RequestStage::Failed || req.stage == RequestStage::Complete)
            continue;
        if (req.solveStarted) {
            if (!req.solveParked || (!req.inputOperandsReady &&
                !(batchInput(req) && req.inputLowerReady)) ||
                now < req.rhsResumeReadyCycle)
                continue;
            if (!contextAvailable(req)) {
                observe("trsm_context_full");
                req.record.stallSolvePending++;
                continue;
            }
            initializeRhs(req);
            activateRhs(req);
            req.solveParked = false;
            ++active;
            trace(req,"solve","TRSM_INPUT_RESUME");
            continue;
        }
        if (splitInput(req) && !req.inputOperandsReady &&
            !(coldRhsInput(req) && req.rhsVectorReady) && !batchRhsReady(req))
            continue;
        const bool early = env.luSolveEarlyStart && env.luForwarding &&
            req.stage == RequestStage::LuActive && req.forwardStepReady[0];
        const bool normal = req.stage == RequestStage::LuReady ||
                            req.stage == RequestStage::SolveQueued;
        if (!early && !normal)
            continue;
        if (req.directBaseSolve && !req.rhsVectorReady && !batchRhsReady(req))
            continue;
        if (normal && !localLu(req) &&
            !req.drainQueued[static_cast<unsigned>(DrainType::Lu)] &&
            !enqueueDrain(req, DrainType::Lu, now))
            continue;
        if (!contextAvailable(req)) {
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
        if (!acquireRhsUpper(req)) continue;
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
    for (double value : state.value) {
        if (!std::isfinite(value)) {
            markFailed(req, CfdCoeffPreprocessStatus::InternalError,
                       "nonfinite-output");
            return;
        }
    }
    trace(req, "solve", "RHS_COMPLETE", batch, col);
    if (batch == 0 && req.directBaseSolve) {
        req.baseAccumulator = state.value;
        req.baseComputeComplete = true;
        req.record.baseComputeReadyCycle = now;
        req.record.baseAccumulatorsCompleted = 1;
        trace(req, "solve", "DIRECT_BASE_READY", batch, col);
    } else {
        auto &out = batch == 0 ? req.dInv :
            (batch == 1 ? req.lBar : req.uBar);
        for (unsigned row = 0; row < N; ++row)
            out[row * N + col] = state.value[row];
    }
    req.batchCompleted[batch]++;
    if (!req.record.firstColumnCycle)
        req.record.firstColumnCycle = now;
    if (batch == 0 && !req.directBaseSolve)
        req.record.dInvColumnReadyCycle[col] = now;
    else if (batch == 1)
        req.record.lBarColumnReadyCycle[col] = now;
    else if (batch == 2)
        req.record.uBarColumnReadyCycle[col] = now;
    if (batch == 0 && req.baseConsumerEnabled && !req.directBaseSolve) {
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
    if (!req.directBaseSolve && batchRequested(req, 0) &&
        req.batchCompleted[0] == 5 &&
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
    finishCoeffRhs(req, now);
    if (coldRhsInput(req) && batch == 0 && !req.solveDone &&
        !req.inputOperandsReady && (!batchInput(req) || req.rhsInitializedMask==1)) {
        // Only a fully completed b may park: no in-flight or partial RHS
        // state is spilled. b and LU already live in reserved request state;
        // coefficient RHS have not initialized or acquired execution credits.
        panic_if(req.rhsInitializedMask != 1,
                 "CFD cold RHS parked with coefficient work live");
        req.solveParked = true;
        req.rhsResumeReadyCycle = now+1;
        trace(req,"solve","TRSM_INPUT_PARK");
    }
    if (!env.smeContexts)
        activateRhs(req);
}

void
CfdCoeffPreprocessController::finishCoeffRhs(Request &req, uint64_t now)
{
    if (req.rhsCompleted != req.rhsCount ||
        (req.smeDeferUpper && !req.smeUpperPartial))
        return;
    req.record.solveCompleteCycle = now;
    req.solveDone = true;
    releaseRhsUpper(req, false);
    clearRhsFactors(req, now);
    trace(req, "solve", "COEFF3_COMPLETE");
}

void
CfdCoeffPreprocessController::completeColumnFma(
    uint64_t token, uint64_t generation, ColumnFmaTaskKind kind,
    unsigned column, const std::array<double, N> &product, uint64_t now)
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
        accumulator[row] += product[row];
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
    if ((req->desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE) && !env.timedInterface)
        writeGuestRecord(*req);
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
    if (env.lineOptLevel >= 4) {
        processColdColumns(now);
        return;
    }
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
        const bool accepted = issueColumnMvm(task.matrixColumn, task.scalar, now,
            [this, task](const auto &product) {
                completeColumnFma(task.token, task.generation,
                    task.kind, task.column, product,
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
    handoff.readyCycle =
        now + (env.smeContexts && env.smeFeedbackReg ? 1u : 0u);
    req.handoffProduced = true;
    if (env.smeContexts)
        smeForwardCells = req.desc.cellId + 1;
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
        if (now < handoff.readyCycle) {
            ++it;
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
        if (!env.smeContexts && req.desc.cellId && req.baseComputeComplete &&
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
            writeGuestRecord(req);
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

bool
CfdCoeffPreprocessController::issueSolveRhs(
    Request &req, unsigned index, uint64_t now,int pairSlot,unsigned pairSide)
{
    DiagnosticOwner owner(diagnosticOwner, req.token);
    RhsState &rhs = req.rhs[index];
    if (!rhs.active || rhs.done || rhs.waiting)
        return false;
    const uint64_t tok = req.token;
    const int k = rhs.k, i = rhs.i;
    const bool consumesLuStep =
        rhs.phase == RhsPhase::ForwardDiv ||
        rhs.phase == RhsPhase::ForwardMul ||
        rhs.phase == RhsPhase::LastDiv ||
        rhs.phase == RhsPhase::BackwardMul;
    // A new reciprocal pays off for multiple RHS. A warm solve can instead
    // reuse the already computed reciprocal in its encoded LU cache entry.
    const bool reciprocalSolve = req.cachedReciprocal ||
        (env.reciprocalSolve && (req.rhsCount > 1 || env.luReciprocalShare));
    bool selected = false;
    if (rhsUpperStreaming(req) && (rhs.phase==RhsPhase::ForwardDiv || rhs.phase==RhsPhase::LastDiv)) {
        selected=issueSolveStreamNorm(req,index,now);
    } else if (env.rhsFmaCount && (rhs.phase == RhsPhase::ForwardMul ||
        rhs.phase == RhsPhase::BackwardMul)) {
        selected = issueSolveFma(req,index,now);
    } else if (env.rhsRowParallel && (rhs.phase == RhsPhase::ForwardMul ||
        rhs.phase == RhsPhase::ForwardSub || rhs.phase == RhsPhase::BackwardMul ||
        rhs.phase == RhsPhase::BackwardSub)) {
        selected = issueSolveRows(req, index, now,pairSlot,pairSide);
    } else switch (rhs.phase) {
      case RhsPhase::ForwardDiv:
      case RhsPhase::LastDiv:
        if (reciprocalSolve && req.reciprocalPending[k]) {
            observe("rhs_reciprocal_dependency");
            req.record.stallDependency++;
            break;
        }
        rhs.waiting = true;
        if (reciprocalSolve && req.reciprocalReady[k]) {
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
        } else if (!issueDivide(coeffDiv, OpKind::Div, now, tok,
            reciprocalSolve ? 1.0 : rhs.value[k],req.lu[k * N + k],
            [this, tok, index, k, reciprocalSolve](double value) {
                Request *r = find(tok);
                if (!r) return;
                RhsState &s = r->rhs[index];
                if (reciprocalSolve) {
                    r->reciprocal[k] = value;
                    r->reciprocalReady[k] = true;
                    r->reciprocalPending[k] = false;
                    s.waiting = false;
                    return;
                }
                s.value[k] = value;
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
            if (reciprocalSolve)
                req.reciprocalPending[k] = true;
            req.record.coeffDivIssued++;
            selected = true;
            trace(req, "solve",
                  reciprocalSolve ? "RECIP_DIV_ISSUE" :
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
      case RhsPhase::StreamUpper:
        break;
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
        trace(req, "scheduler", "SCHED_SELECT", index / 5,
              index % 5, k, i);
    }
    if (!selected)
        observe("rhs_candidate_not_selected");
    return selected;
}

void
CfdCoeffPreprocessController::finalizeSolveRequest(
    Request &req, uint64_t now)
{
    if (req.stage == RequestStage::Failed ||
        req.stage == RequestStage::Complete)
        return;

    if (req.luDone && !localLu(req) &&
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
    if ((req.matrixDrainMask & 4u) && req.record.uBarReadyCycle &&
        !req.drainQueued[static_cast<unsigned>(DrainType::UBar)])
        enqueueDrain(req, DrainType::UBar, now);
    if (req.solveDone)
        req.stage = RequestStage::DrainPending;
}

void
CfdCoeffPreprocessController::processSolve(uint64_t now)
{
    std::vector<Request *> visit;
    for (auto &[token, ptr] : requests)
        visit.push_back(ptr.get());
    if (env.rhsSchedule == RhsSchedule::RequestFrontierAging) {
        auto oldestReady = [now](const Request *r) {
            uint64_t age = 0;
            for (const auto &s : r->rhs) {
                if ((s.phase == RhsPhase::ForwardDiv ||
                     s.phase == RhsPhase::ForwardMul ||
                     s.phase == RhsPhase::LastDiv) &&
                    !r->forwardStepReady[s.k])
                    continue;
                if (s.phase == RhsPhase::BackwardMul &&
                    !r->backwardStepReady[s.k])
                    continue;
                if (s.active && !s.done && !s.waiting && s.readySinceCycle)
                    age = std::max(age, now - s.readySinceCycle);
            }
            return age;
        };
        std::stable_sort(visit.begin(), visit.end(),
            [&oldestReady](const Request *a, const Request *b) {
                const auto aa = oldestReady(a), ba = oldestReady(b);
                if ((aa > 64) != (ba > 64))
                    return aa > 64;
                if (aa > 64 && aa != ba)
                    return aa > ba;
                const bool af = !a->dqStarReadyLocal &&
                    (a->desc.cellId == 0 || a->previousDqReady);
                const bool bf = !b->dqStarReadyLocal &&
                    (b->desc.cellId == 0 || b->previousDqReady);
                if (af != bf)
                    return af;
                return a->desc.cellId < b->desc.cellId;
            });
    }
    // Gather ready RHS from every active cell before consuming a shared FU.
    // This keeps the existing per-request scheduler unchanged for other modes.
    struct SolveCandidate { Request *req; unsigned index; bool streamBackward=false; };
    const bool frontierMode = env.rhsSchedule == RhsSchedule::RhsFrontier;
    std::vector<SolveCandidate> pending;
    std::vector<std::pair<Request *, unsigned>> scanned;
    for (Request *ptr : visit) {
        Request &req = *ptr;
        DiagnosticOwner owner(diagnosticOwner, req.token);
        if (!req.solveStarted || req.solveParked || req.stage == RequestStage::Failed)
            continue;
        if (req.tailUbarMarked && !req.tailUbarReleased)
            activateRhs(req);
        std::array<unsigned, 15> order = {};
        unsigned orderCount = 0,streamCount=0;
        uint16_t streamMask=0;
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
                if(frontierMode && rhsUpperStreaming(req) &&
                   CfdRhsUpperStream::ready(rhs.i,rhs.normalizedMask,rhs.backwardWaiting) &&
                   upperColumnReady(req,rhs.i)) {
                    pending.push_back({&req,index,true});++streamCount;streamMask|=1u<<index;
                }
                if(rhs.phase==RhsPhase::StreamUpper)continue;
                if (rhs.waiting) {
                    observe("rhs_inflight_dependency");
                    req.record.stallDependency++;
                    continue;
                }
                if (rhsTreeEnabled(req,index) &&
                    rhs.phase == RhsPhase::BackwardSub && !readyRhsTreeNodes(rhs)) {
                    observe("rhs_tree_dependency");
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
            unsigned uniqueReady=orderCount+streamCount;
            for(unsigned j=0;j<orderCount;++j)uniqueReady-=bool(streamMask&(1u<<order[j]));
            req.record.readyRhsCountSum += uniqueReady;
            if (env.round1Diagnostics)
                traceRhsReadyGroups(req, order, orderCount, now);
            req.record.readyRhsCountMax = std::max<uint64_t>(
                req.record.readyRhsCountMax, uniqueReady);
            if (!uniqueReady) {
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
              case RhsPhase::StreamUpper: return 4;
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
        } else if (env.rhsSchedule == RhsSchedule::FrontierAware ||
                   env.rhsSchedule == RhsSchedule::RequestFrontierAging) {
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

        if (frontierMode) {
            unsigned uniqueReady=orderCount+streamCount;
            for(unsigned j=0;j<orderCount;++j)uniqueReady-=bool(streamMask&(1u<<order[j]));
            scanned.emplace_back(&req, uniqueReady);
            for (unsigned j = 0; j < orderCount; ++j)
                pending.push_back({&req, order[j]});
            continue;
        }
        bool anySelection = false;
        for (unsigned orderIndex = 0; orderIndex < orderCount; ++orderIndex) {
            const unsigned index = order[orderIndex];
            if (issueSolveRhs(req, index, now))
                anySelection = true;
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
        finalizeSolveRequest(req, now);
    }
    if (!frontierMode)
        return;

    uint64_t frontierCell = std::numeric_limits<uint64_t>::max();
    for (const auto &[req, count] : scanned) {
        if (req->dqStarReadyLocal || req->stage == RequestStage::Failed ||
            (req->desc.cellId && !req->previousDqReady))
            continue;
        frontierCell = std::min(frontierCell, req->desc.cellId);
    }
    auto priority = [this, now, frontierCell](const SolveCandidate &candidate) {
        const Request &req = *candidate.req;
        const unsigned batch = rhsBatch(req, candidate.index);
        const unsigned column = rhsColumn(req, candidate.index);
        const bool current = req.desc.cellId == frontierCell;
        if (env.rhsSupplyPriority || env.demandSchedule) {
            const auto &rhs = req.rhs[candidate.index];
            // Bounded two-point b lookahead. A 256-cycle age promotion
            // prevents Ubar/background work from being permanently excluded.
            if (rhs.readySinceCycle && now-rhs.readySinceCycle > 256) return 0u;
            if (current && batch == 0 && !req.baseComputeComplete) return 1u;
            if (current && batch == 1 && !req.correctionComplete)
                return column == req.nextLbarColumnToConsume ? 2u : 3u;
            if (frontierCell != UINT64_MAX && req.desc.cellId > frontierCell &&
                req.desc.cellId-frontierCell <= 2 && batch == 0 &&
                !req.baseComputeComplete) return 4u;
            return 5u+batch;
        }
        if (current && batch == 1 && req.previousDqReady &&
            !req.correctionComplete) {
            if (column == req.nextLbarColumnToConsume)
                return 0u;
            return 2u;
        }
        if (current && batch == 0 && req.rhsVectorReady &&
            !req.baseComputeComplete) {
            if (column == req.nextDInvColumnToConsume)
                return 1u;
            return 3u;
        }
        const RhsState &rhs = req.rhs[candidate.index];
        if (rhs.readySinceCycle && now - rhs.readySinceCycle > 256)
            return 4u;
        return 5u + batch;
    };
    std::stable_sort(pending.begin(), pending.end(),
        [&priority](const SolveCandidate &a, const SolveCandidate &b) {
            const unsigned ap = priority(a), bp = priority(b);
            if (ap != bp)
                return ap < bp;
            if (a.req->desc.cellId != b.req->desc.cellId)
                return a.req->desc.cellId < b.req->desc.cellId;
            return a.index < b.index;
        });
    std::unordered_map<uint64_t, bool> selectedByToken;
    bool lookaheadProcessed = !env.luLookahead && !env.demandSchedule;
    auto lookahead = [this,now,&lookaheadProcessed] {
        if (lookaheadProcessed) return;
        if (env.demandSchedule) processLu(now);
        luLookaheadPass = true;
        processLu(now);
        luLookaheadPass = false;
        lookaheadProcessed = true;
    };
    bool upperProcessed=false;
    auto upper = [this,now,frontierCell,&upperProcessed] {
        if (upperProcessed) return;
        processRhsUpper(now,frontierCell);upperProcessed=true;
    };
    bool pairsProcessed=false;
    auto pairs = [this,now,&pairsProcessed,&selectedByToken] {
        if (pairsProcessed || !env.rhsPair) return;
        processRhsPairs(now,selectedByToken);
        pairsProcessed=true;
    };
    for (const auto &candidate : pending) {
        Request &req = *candidate.req;
        if (req.stage == RequestStage::Failed)
            continue;
        const unsigned rank = priority(candidate);
        if (rank>=2) upper();
        // Critical current b/Lbar (and aged work in supply-priority mode)
        // keep first refusal. Then supply the next bounded LU frontier before
        // spending every remaining issue slot on background RHS columns.
        if (rank >= 4) lookahead();
        if (rank >= 4) pairs();
        // A promoted LU pivot failure can synchronously cancel its whole line.
        if (req.stage == RequestStage::Failed) continue;
        if (env.rhsPair && selectRhsPair(req,candidate.index,now)) continue;
        if(candidate.streamBackward) {
            DiagnosticOwner owner(diagnosticOwner,req.token);
            const int column=req.rhs[candidate.index].i;
            if(!issueSolveFma(req,candidate.index,now,true))continue;
            trace(req,"scheduler","SCHED_STREAM_SELECT",rhsBatch(req,candidate.index),
                  rhsColumn(req,candidate.index),N-column-1);
        } else if (!issueSolveRhs(req, candidate.index, now))continue;
        selectedByToken[req.token] = true;
        if (env.rhsSupplyPriority)
            observe(rank == 0 ? "rhs_supply_aged_issue" :
                    rank == 1 ? "rhs_supply_current_b_issue" :
                    rank == 4 ? "rhs_supply_lookahead_b_issue" :
                                "rhs_supply_other_issue");
        else if (rank < 2)
            observe("rhs_frontier_exact_issue");
        else if (rank >= 4)
            observe("rhs_frontier_background_issue");
    }
    upper();
    lookahead();
    pairs();
    for (const auto &[req, count] : scanned) {
        if (count && !selectedByToken[req->token]) {
            req->record.schedulerNoSelection++;
            trace(*req, "scheduler", "SCHED_NO_SELECTION");
        }
        finalizeSolveRequest(*req, now);
    }
}

bool
CfdCoeffPreprocessController::admitDrain(
    Request &req, DrainType type, uint64_t now)
{
    DiagnosticOwner owner(diagnosticOwner, req.token);
    const unsigned idx = static_cast<unsigned>(type);
    if (req.drainQueued[idx])
        return true;
    if (drainQueue.size() + activeDrains.size() >= env.drainTaskDepth) {
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
    drainTaskPeak = std::max<uint64_t>(drainTaskPeak,drainQueue.size()+activeDrains.size());
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
    if (!env.timedInterface)
        writeGuestRecord(req);
}

void
CfdCoeffPreprocessController::processDrains(uint64_t now)
{
    if (env.ageDrains) arbitrateDrains(now);
    while (!drainQueue.empty() &&
           activeDrains.size() < env.drainOutstanding) {
        activeDrains.push_back(drainQueue.front());
        drainQueue.pop_front();
    }
    drainActivePeak = std::max<uint64_t>(drainActivePeak,activeDrains.size());
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
            auto payload = env.dmaSpm && env.lineOptLevel >= 5 ?
                std::make_shared<std::vector<uint8_t>>(bytes) : nullptr;
            if (payload) {
                panic_if(bytes > 64, "CFD DMA staging exceeds its reserved slot");
                std::memcpy(payload->data(),data,bytes);
                data = payload->data();
            }
            auto packetMeta = std::make_shared<CfdSpmPacketIssue>();
            auto completion =
                [this, tok, type, bytes, addr, matrix, packetMeta, payload](
                    uint64_t packetId) {
                    (void)payload; // The FIFO snapshot survives request cancellation.
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
        const bool done = (localLu(req) ? req.luDone :
            req.drainDone[static_cast<unsigned>(DrainType::Lu)]) &&
            (!req.smeInvRequested || req.smeInvDone) &&
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
        if (!done) {
            detachDataCommit(req);
            continue;
        }
        if (!prepareOnchipCommit(req)) continue;
        req.stage = RequestStage::Complete;
        req.status = CfdCoeffPreprocessStatus::Complete;
        req.record.status = static_cast<uint64_t>(req.status);
        req.record.completeCycle = now;
        if (req.slotOwned)
            slots[req.slot] = false;
        req.slotOwned = false;
        if (!env.timedInterface)
            writeGuestRecord(req);
        trace(req, "controller", "REQUEST_COMPLETE");
        if (env.lineOptLevel >= 2 && env.tiledLine) {
            auto &line = *lineRequests.at(childLineTokens.at(req.token));
            commitTileChild(line, req);
            req.tileDataCommitted = true;
            trace(req, "controller", "TILE_DATA_COMMIT");
        }
    }
}

void
CfdCoeffPreprocessController::aggregateSummary(
    Request &req, LineRequest &line, uint64_t digest, bool detached)
{
    panic_if(digest != recordDigest(req.record), "CFD timed summary digest mismatch");
    auto &s = line.summary;
    s.completed++;
    s.errors += req.record.autoRetireError;
    s.digest ^= digest;
    s.firstIssue = s.firstIssue ? std::min(s.firstIssue,req.record.issueCycle) : req.record.issueCycle;
    s.lastComplete = std::max(s.lastComplete,req.record.completeCycle);
    if (env.traceEnable)
        std::fprintf(stderr,"CFD_RECORD_DIGEST_SOURCE request=%llu generation=%llu hash=%016llx summary_owner=%llu\n",
            (unsigned long long)req.record.requestId,(unsigned long long)req.record.generation,
            (unsigned long long)digest,(unsigned long long)line.token);
    trace(req,"controller","REQUEST_SUMMARY");
    if (!detached) {
        trace(req,"controller","REQUEST_AUTO_RETIRE");
        retireLineChild(req.token,req);
    }
}

#include "cfd_coeff_preprocess_summary.inc"

void
CfdCoeffPreprocessController::retireAutoRequests()
{
    std::vector<uint64_t> retired;
    const uint64_t now = cycle();
    // A single in-flight word. The request itself owns the stable record;
    // keep that slot live until the last integer result has returned.
    if (env.summaryDepth) processSummarySnapshots(now);
    if (summaryHash.ready(now)) {
        auto r = requests.find(summaryHash.token);
        auto p = lineRequests.find(summaryHash.owner);
        if (r == requests.end() || p == lineRequests.end() || p->second->abortPending) {
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=drop token=%llu owner=%llu\n",
                    (unsigned long long)now,(unsigned long long)summaryHash.token,
                    (unsigned long long)summaryHash.owner);
            summaryHash.cancel(now);
        } else {
            auto &req = *r->second;
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=word token=%llu owner=%llu request=%llu index=%u word=%016llx before=%016llx after=%016llx\n",
                    (unsigned long long)now,(unsigned long long)summaryHash.token,
                    (unsigned long long)summaryHash.owner,(unsigned long long)req.record.requestId,
                    summaryHash.index,(unsigned long long)summaryHash.word,
                    (unsigned long long)summaryHash.digest,(unsigned long long)summaryHash.result);
            summaryHash.commit(now);
            if (summaryHash.done()) {
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=complete token=%llu owner=%llu request=%llu hash=%016llx\n",
                        (unsigned long long)now,(unsigned long long)summaryHash.token,
                        (unsigned long long)summaryHash.owner,(unsigned long long)req.record.requestId,
                        (unsigned long long)summaryHash.digest);
                aggregateSummary(req,*p->second,summaryHash.digest);
                retired.push_back(req.token);
                summaryHash = CfdSummaryHash{};
            } else {
                summaryHash.issue(now,recordWord(req.record,summaryHash.index));
            }
        }
    }
    Request *summaryCandidate = nullptr;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if ((summaryHash.live && summaryHash.token == token) ||
            summarySnapshots.find(token) ||
            std::find(retired.begin(),retired.end(),token) != retired.end())
            continue;
        if (!(req.desc.flags & CFD_COEFF_PRE_STREAMING_AUTO_RETIRE) ||
            (req.stage != RequestStage::Complete &&
             req.stage != RequestStage::Failed))
            continue;
        req.record.autoRetired = 1;
        req.record.autoRetireError =
            req.status == CfdCoeffPreprocessStatus::Complete ? 0 : 1;
        auto owner = childLineTokens.find(token);
        if (owner != childLineTokens.end()) {
            auto &line = *lineRequests.at(owner->second);
            if (warmSummary(line) && !line.abortPending) {
                if (env.coldSummary) {
                    if (env.summaryDepth) {
                        auto *snapshot = summarySnapshots.allocate(now,token,owner->second);
                        if (!snapshot) { observe("summary_snapshot_full"); continue; }
                        snapshot->expectedDigest = recordDigest(req.record);
                        line.recordsPending++;
                        snapshotIssued++;
                        if (env.traceEnable)
                            std::fprintf(stderr,"CFD_SUMMARY_SNAPSHOT cycle=%llu event=allocate token=%llu owner=%llu request=%llu slot=%zu depth=%u\n",
                                (unsigned long long)now,(unsigned long long)token,
                                (unsigned long long)owner->second,(unsigned long long)req.record.requestId,
                                snapshot-summarySnapshots.entries.data(),env.summaryDepth);
                        continue;
                    }
                    if (!summaryCandidate ||
                        std::tie(req.record.completeCycle,req.token) <
                        std::tie(summaryCandidate->record.completeCycle,summaryCandidate->token))
                        summaryCandidate = &req;
                    continue;
                }
                aggregateSummary(req,line,recordDigest(req.record));
                retired.push_back(token);
                continue;
            }
        }
        if (env.lineOptLevel >= 5) {
            if (detachedRecords.size() >= env.completionDepth) {
                lineRequests.at(childLineTokens.at(token))->recordDetachStalls++;
                continue;
            }
            panic_if(!(req.desc.flags & CFD_COEFF_PRE_COMPACT_RECORD),
                     "CFD detached completion requires the compact ABI");
            auto transfer = std::make_shared<InterfaceTransfer>();
            transfer->addr = req.desc.recordAddr;
            transfer->write = true;
            transfer->kind = "record";
            transfer->cell = req.desc.cellId;
            transfer->bytes.resize(recordBytes(req.desc.flags));
            if (req.desc.flags & CFD_COEFF_PRE_PACKED_RECORD) {
                const auto packed=packedRecord(req.record);
                std::memcpy(transfer->bytes.data(),&packed,sizeof(packed));
                if (env.traceEnable)
                    std::fprintf(stderr,"CFD_RECORD_DIGEST_SOURCE request=%llu generation=%llu hash=%016llx\n",
                        (unsigned long long)req.record.requestId,
                        (unsigned long long)req.record.generation,
                        (unsigned long long)recordDigest(req.record));
            } else {
                const auto compact=cfdCompactRecord(req.record);
                std::memcpy(transfer->bytes.data(),&compact,sizeof(compact));
            }
            const uint64_t owner = childLineTokens.at(token);
            detachedRecords.push_back({owner, token, req.record.requestId,
                req.desc.generation, req.desc.lineId, req.desc.cellId, transfer});
            lineRequests.at(owner)->recordsPending++;
            detachedIssued++;
            detachedPeak = std::max<uint64_t>(detachedPeak, detachedRecords.size());
            trace(req, "controller", "REQUEST_DETACH");
            retireLineChild(token, req);
            retired.push_back(token);
            continue;
        }
        // This is the terminal release publication.  It happens after the
        // cycle's activity accounting and before request/token destruction.
        if (env.timedInterface) {
            if (!req.recordTransfer) {
                req.recordTransfer = std::make_shared<InterfaceTransfer>();
                auto &transfer = *req.recordTransfer;
                transfer.addr = req.desc.recordAddr;
                transfer.write = true;
                transfer.kind = "record";
                transfer.cell = req.desc.cellId;
                if (req.desc.flags & CFD_COEFF_PRE_PACKED_RECORD) {
                    const auto packed=packedRecord(req.record);
                    transfer.bytes.resize(sizeof(packed));
                    std::memcpy(transfer.bytes.data(),&packed,sizeof(packed));
                } else if (req.desc.flags & CFD_COEFF_PRE_COMPACT_RECORD) {
                    const auto compact = cfdCompactRecord(req.record);
                    transfer.bytes.resize(sizeof(compact));
                    std::memcpy(transfer.bytes.data(), &compact, sizeof(compact));
                } else {
                    transfer.bytes.resize(sizeof(req.record));
                    std::memcpy(transfer.bytes.data(), &req.record, sizeof(req.record));
                }
            }
            stepInterfaceTransfer(childLineTokens.at(token), req.recordTransfer);
            if (req.recordTransfer->completed != req.recordTransfer->bytes.size())
                continue;
        } else {
            writeGuestRecord(req);
        }
        trace(req, "controller", "REQUEST_AUTO_RETIRE");
        retireLineChild(token, req);
        retired.push_back(token);
    }
    if (!summaryHash.live && summaryCandidate) {
        auto &req = *summaryCandidate;
        summaryHash.start(now,req.token,childLineTokens.at(req.token),recordWord(req.record,0));
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=accept token=%llu owner=%llu request=%llu words=%u word_cycles=%u bytes=128\n",
                (unsigned long long)now,(unsigned long long)req.token,
                (unsigned long long)summaryHash.owner,(unsigned long long)req.record.requestId,
                CfdSummaryHash::Words,CfdSummaryHash::WordCycles);
    }
    for (uint64_t token : retired)
        onchipContexts.erase(token);
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
    discardColdProducts(req);
    discardRhsRows(req);
    req.record.status = static_cast<uint64_t>(status);
    req.record.completeCycle = cycle();
    if (req.slotOwned)
        slots[req.slot] = false;
    req.slotOwned = false;
    if (!env.timedInterface)
        writeGuestRecord(req);
    trace(req, "controller", "REQUEST_FAILED", -1, -1, -1, -1, note);
    auto owner = childLineTokens.find(req.token);
    if (owner != childLineTokens.end()) {
        auto line = lineRequests.find(owner->second);
        if (line != lineRequests.end())
            abortLine(*line->second, status);
    }
}

void
CfdCoeffPreprocessController::accountActivity()
{
    bool input = false, lu = false, solve = false;
    for (auto &[token, ptr] : requests) {
        Request &req = *ptr;
        if (req.stage == RequestStage::InputFetching ||
            (splitInput(req) && req.slotOwned && !req.inputOperandsReady)) {
            req.record.inputActiveCycles++;
            input = true;
        }
        if (req.stage == RequestStage::LuActive) {
            req.record.luActiveCycles++;
            lu = true;
        }
        if (req.solveStarted && !req.solveDone && !req.solveParked) {
            req.record.solveActiveCycles++;
            solve = true;
        }
        if (req.stage == RequestStage::LuActive &&
            req.solveStarted && !req.solveDone && !req.solveParked)
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
                    !solveReq->solveDone && !solveReq->solveParked) {
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

#include "cfd_coeff_preprocess_diagnostics.inc"
#include "cfd_coeff_preprocess_lifecycle.inc"
#include "cfd_coeff_preprocess_lu_fma.inc"
#include "cfd_coeff_preprocess_lu_rank1.inc"
#include "cfd_coeff_preprocess_lu_rows.inc"
#include "cfd_coeff_preprocess_onchip.inc"
#include "cfd_coeff_preprocess_backward.inc"
#include "cfd_coeff_preprocess_lane_mvm.inc"
#include "cfd_coeff_preprocess_early.inc"
#include "cfd_coeff_preprocess_drain_arb.inc"
#include "cfd_coeff_preprocess_input_bursts.inc"
#include "cfd_coeff_preprocess_output_bursts.inc"
#include "cfd_coeff_preprocess_sme.inc"

void
CfdCoeffPreprocessController::processTick()
{
    const uint64_t now = cycle();
    laneGrants = 0;
    vectorMulGrants = vectorAddGrants = 0;
    arithmeticAcceptedNow = 0;
    if (rfEnabled()) registerPorts.tick(now);
    // Clear observations before completions can report backpressure.
    // Resource availability is still sampled after the actual returns.
    waitThisCycle.clear();
    rejectThisCycle.fill(0);
    criticalRejectThisCycle.fill(0);
    spmReadPortsAvail = env.spmReadPorts;
    spmWritePortsAvail = env.spmWritePorts;
    drainPortsAvail = env.drainPorts;
    std::fill(spmBanksUsed.begin(), spmBanksUsed.end(), false);
    if (env.smeContexts) {
        smeZa.tick(now);
        sme2dOfferMetas.clear();
        sme2dOfferCommits.clear();
        sme2dCriticalOffered.clear();
    }
    completeOps(now);
    completeArithmeticGroups(now);
    processOnchip();
    processTimedLaunches();
    beginResourceStats(now);
    processDirectRhsPrefetch();
    assignInputSlots(now);
    processInputs(now);
    if (!env.smeContexts)
        startLuRequests(now);
    if (env.smeContexts) {
        if (env.smeCriticalArb)
            processSmeCriticalRhs(now);
        processForwardHandoffs(now);
        if (processSmeForward(now) && env.smeConfig.ingressPorts) {
            processForwardHandoffs(now);
            processSmeForward(now);
        }
    } else if (env.sharedCluster) {
        startSolveRequests(now);
        // Ready feedback work gets the first opportunity to atomically take
        // five lanes. Scalar preprocessing fills the remaining issue slots.
        processSharedVectorAdds(now);
    } else {
        processLu(now);
        startSolveRequests(now);
        processSolve(now);
    }
    processForwardHandoffs(now);
    if (!env.smeContexts)
        processColumnFma(now);
    processForwardCombines(now);
    if (!env.smeContexts)
        processLineCachedForward(now);
    publishLineValues();
    processLocalTileResults();
    // A resident solution is on the recurrence's output path. Give it the
    // first chance at a newly freed DMA credit before background tile reads.
    // Keep one output DMA issue opportunity per tick in either ordering.
    const bool outputFirst = env.lineOptLevel >= 11 && env.dmaOutputPriority;
    if (outputFirst)
        processLineOutputWrites();
    processLineTiles();
    processLineBackward(now);
    if (env.smeReverseContexts)
        processSmeReversePanels();
    if (!outputFirst)
        processLineOutputWrites();
    if (env.smeContexts) {
        processSmeRequests(now);
        if (smeZa.is2d())
            commitSme2dBundle();
    } else if (env.sharedCluster) {
        if (!env.demandSchedule) processLu(now);
        if (env.luSupplyFirst && !env.demandSchedule) {
            // Feed the next recurrence before background RHS consumes the
            // arithmetic credits. The existing radius and per-request visit
            // guard bound both work and repeated scheduler opportunities.
            luLookaheadPass = true;
            processLu(now);
            luLookaheadPass = false;
        }
        processSolve(now);
        if (env.lineOptLevel >= 7) {
            luBackgroundPass = true;
            processLu(now);
            luBackgroundPass = false;
        }
    }
    // Current pivots and critical RHS get first refusal. Only a remaining
    // real divider/RF credit may read a returned next-column diagonal.
    if (!env.smeContexts) {
        if (env.luPivotPrefetch) processLuPivotPrefetch(now);
        processLineBaseAhead(now);
    }
    processDetachedRecords();
    // A zero-wait adjacent request can consume a handoff produced by a head
    // cell or combine completion in the same modeled controller cycle.
    processForwardHandoffs(now);
    processDrains(now);
    finishRequests(now);
    processArithmeticGroups(now);
    accountActivity();
    retireAutoRequests();
    // A summary describes retired forward requests. Ship it as soon as all
    // records are present, overlapping backward arithmetic and result writes.
    // Only the final status makes the whole sweep visible to software.
    for (auto &[token, line] : lineRequests)
        if (!line->terminal && warmSummary(*line))
            stepLineSummary(*line);
    processAbortedLines();
    if (smeOwner)
        sampleSme(*lineRequests.at(smeOwner));
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
    const bool more = !pendingLineLaunches.empty() || active || activeLine || !pendingOps.empty() || !smeZa.empty() || rhsPairs.live() || rhsUpper.live || arithmeticGroups.live || summaryBusy() ||
        !columnFmaQueue.empty() || !sharedVectorAdds.empty() ||
        !forwardCombineQueue.empty() ||
        !drainQueue.empty() ||
        !activeDrains.empty() || !detachedRecords.empty() || !onchipQueue.empty() ||
        std::any_of(lineRequests.begin(), lineRequests.end(), [](const auto &entry) {
            return entry.second->abortPending && !entry.second->abortPublished;
        });
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
