#include "arch/arm/cfd_lusgs_event_controller.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arch/arm/cfd_local_spm.hh"
#include "arch/arm/cfd_lusgs_controller.hh"
#include "arch/arm/cfd_lusgs_vec5.hh"
#include "arch/arm/cfd_trsv5_math.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace ArmISA
{

namespace
{

static constexpr uint32_t MatrixBytes = 5 * 5 * sizeof(double);
static constexpr uint32_t VectorBytes = 5 * sizeof(double);
static constexpr uint64_t LocalSpmCapacity = 96ull * 1024ull;

enum class EventState
{
    Idle,
    CommandQueued,
    ReadDescriptorReq,
    ReadDescriptorWait,
    ValidateDescriptor,
    AllocContext,
    ForwardInit,
    ForwardLoadReq,
    ForwardLoadWait,
    ForwardPathAReq,
    ForwardPathAWait,
    ForwardVec5Req,
    ForwardVec5Wait,
    ForwardTrsvReq,
    ForwardTrsvWait,
    ForwardDqstarWriteReq,
    ForwardDqstarWriteWait,
    ForwardNext,
    BackwardInit,
    BackwardLoadReq,
    BackwardLoadWait,
    BackwardPathAReq,
    BackwardPathAWait,
    BackwardVec5Req,
    BackwardVec5Wait,
    BackwardDqWriteReq,
    BackwardDqWriteWait,
    BackwardNext,
    QUpdateReq,
    QUpdateWait,
    TaskComplete,
    TaskError
};

enum class LusgsTaskLifecycle
{
    Idle,
    Queued,
    Running,
    CompletedNotReaped,
    ErrorNotReaped,
    Aborting
};

enum class LusgsPhase : uint32_t
{
    Task = 0,
    Forward = 1,
    Backward = 2,
    QUpdate = 3
};

enum class LusgsResourceType : uint32_t
{
    None = 0,
    DescriptorRead,
    SpmRead,
    PathA,
    Trsv5,
    Vec5Copy,
    Vec5Sub,
    Vec5Axpy,
    DqstarWrite,
    DqWrite
};

struct LusgsEventRequestHeader
{
    uint64_t requestId = 0;
    uint64_t taskToken = 0;
    uint64_t taskGeneration = 0;
    uint32_t contextId = 0;
    uint32_t line = 0;
    uint32_t cell = 0;
    uint32_t tile = 0;
    LusgsPhase phase = LusgsPhase::Task;
    LusgsResourceType resourceType = LusgsResourceType::None;
};

struct PendingLusgsRequest
{
    LusgsEventRequestHeader header;
    Tick issueTick = 0;
    Tick completionTick = 0;
    bool accepted = false;
    bool completed = false;
    bool cancelled = false;
    std::unique_ptr<EventFunctionWrapper> completionEvent;
};

struct LusgsContextState
{
    bool valid = false;
    uint32_t contextId = 0;
    uint32_t line = 0;
    uint32_t cell = 0;
    uint32_t tile = 0;
    LusgsPhase phase = LusgsPhase::Task;
    EventState state = EventState::Idle;
    uint64_t waitingRequestId = 0;
    LusgsResourceType waitingResource = LusgsResourceType::None;
};

enum class PathASlotState
{
    Free,
    Allocated,
    DotpInFlight,
    RowsReady,
    PackInFlight,
    ResultReady
};

struct PathAMvmRequest
{
    LusgsEventRequestHeader header;
    Addr matrixBase = 0;
    Addr vectorBase = 0;
    Addr resultBase = 0;
    uint8_t preferredSlot = 0;
    bool writeResultToMemory = false;
};

struct PathAMvmResponse
{
    LusgsEventRequestHeader header;
    uint8_t slot = 0;
    std::array<double, 5> result = {};
    Tick firstLoadIssueTick = 0;
    Tick lastLoadCompleteTick = 0;
    Tick firstDotpIssueTick = 0;
    Tick lastDotpCompleteTick = 0;
    Tick packIssueTick = 0;
    Tick completionTick = 0;
};

struct PathASlot
{
    PathASlotState state = PathASlotState::Free;
    uint64_t requestId = 0;
    std::array<double, 5> result = {};
    uint8_t readyMask = 0;
};

struct PathAMvmTransaction
{
    enum class State
    {
        Idle,
        WaitingSlot,
        LoadingInputs,
        WaitingInputs,
        IssuingDotp,
        WaitingDotp,
        WaitingPack,
        Packing,
        WaitingResult,
        Completed,
        Cancelled,
        Error
    };

    PathAMvmRequest request;
    State state = State::Idle;
    uint8_t slot = 0xff;
    uint8_t matldIssued = 0;
    uint8_t matldCompleted = 0;
    uint8_t dotpIssued = 0;
    uint8_t dotpCompleted = 0;
    bool packIssued = false;
    bool packCompleted = false;
    bool resultReady = false;
    std::array<std::array<double, 5>, 5> matrix = {};
    std::array<double, 5> vector = {};
    std::array<double, 5> result = {};
    std::array<bool, 5> rowReady = {};
    std::array<bool, 5> dotpInFlight = {};
    std::array<bool, 5> dotpDone = {};
    bool vectorReady = false;
    Tick issueTick = 0;
    Tick completionTick = 0;
    Tick firstLoadIssueTick = 0;
    Tick lastLoadCompleteTick = 0;
    Tick firstDotpIssueTick = 0;
    Tick lastDotpCompleteTick = 0;
    Tick packIssueTick = 0;
    Tick packCompleteTick = 0;
    Tick resultReadyTick = 0;
    bool valid = false;
};

enum class Trsv5State
{
    Free,
    WaitingInput,
    LoadingInput,
    ForwardDivide,
    ForwardUpdate,
    LastDivide,
    BackwardUpdate,
    ResultReady,
    Completed,
    Cancelled,
    Error
};

struct Trsv5EventRequest
{
    LusgsEventRequestHeader header;
    Addr luAddr = 0;
    Addr rhsAddr = 0;
    std::array<double, 25> lu = {};
    std::array<double, 5> rhs = {};
    bool inputCaptured = false;
    bool standalone = false;
};

struct Trsv5EventResponse
{
    LusgsEventRequestHeader header;
    std::array<double, 5> result = {};
    Tick issueTick = 0;
    Tick loadCompleteTick = 0;
    Tick forwardCompleteTick = 0;
    Tick backwardCompleteTick = 0;
    Tick completionTick = 0;
};

struct Trsv5Transaction
{
    Trsv5EventRequest request;
    Trsv5State state = Trsv5State::Free;
    std::array<double, 25> lu = {};
    std::array<double, 5> value = {};
    int k = 0;
    int nextI = 0;
    uint32_t stageFmasIssued = 0;
    uint32_t stageFmasCompleted = 0;
    uint32_t stageFmasExpected = 0;
    uint32_t fmasInFlight = 0;
    bool active = false;
    bool resourceWakeScheduled = false;
    Tick issueTick = 0;
    Tick loadCompleteTick = 0;
    Tick forwardStartTick = 0;
    Tick forwardCompleteTick = 0;
    Tick backwardStartTick = 0;
    Tick backwardCompleteTick = 0;
    Tick completionTick = 0;
    std::vector<EventFunctionWrapper *> events;
};

struct Vec5EventRequest
{
    LusgsEventRequestHeader header;
    CfdLusgsVec5Op operation = CfdLusgsVec5Op::Copy;
    std::array<double, 5> input0 = {};
    std::array<double, 5> input1 = {};
    double scalar = 0.0;
    bool standalone = false;
};

struct Vec5EventResponse
{
    LusgsEventRequestHeader header;
    CfdLusgsVec5Op operation = CfdLusgsVec5Op::Copy;
    std::array<double, 5> result = {};
    Tick issueTick = 0;
    Tick completionTick = 0;
};

struct Vec5Transaction
{
    enum class State
    {
        Free,
        Queued,
        Executing,
        ResultReady,
        Completed,
        Cancelled,
        Error
    };

    Vec5EventRequest request;
    State state = State::Free;
    std::array<double, 5> result = {};
    uint32_t nextLane = 0;
    uint32_t lanesIssued = 0;
    uint32_t lanesCompleted = 0;
    uint32_t lanesInFlight = 0;
    bool active = false;
    Tick issueTick = 0;
    Tick completionTick = 0;
    std::vector<EventFunctionWrapper *> events;
};

struct EventEnv
{
    char stage = 'A';
    uint32_t contexts = 1;
    uint32_t tileCells = 1;
    uint64_t ticksPerCycle = 500;
    uint64_t spmReadLatency = 2;
    uint64_t spmWriteLatency = 1;
    uint64_t pathaLatency = 43;
    uint64_t pathaMatldLatency = 1;
    uint32_t pathaMatldCount = 8;
    uint64_t pathaDotpLatency = 7;
    uint32_t pathaDotpCount = 1;
    uint64_t pathaPackLatency = 1;
    uint32_t pathaPackCount = 1;
    uint32_t pathaResultBufferDepth = 2;
    uint64_t pathaResultLatency = 1;
    uint64_t trsvLatency = 60;
    uint64_t vec5Latency = 4;
    uint32_t trsvEventCount = 1;
    uint32_t trsvDivCount = 1;
    uint32_t trsvFmaCount = 1;
    uint64_t trsvDivLatency = 4;
    uint64_t trsvFmaLatency = 3;
    uint64_t trsvLoadLatency = 2;
    uint64_t trsvResultLatency = 1;
    uint32_t trsvQueueDepth = 2;
    bool trsvForwarding = true;
    uint32_t vec5Count = 1;
    uint32_t vec5Lanes = 5;
    uint64_t vec5CopyLatency = 1;
    uint64_t vec5SubLatency = 3;
    uint64_t vec5AxpyLatency = 4;
    uint64_t vec5InitiationInterval = 1;
    uint32_t vec5QueueDepth = 2;
    uint64_t watchdogCycles = 0;
    bool watchdogTestHang = false;
    bool pathaReal = false;
    bool trsvReal = false;
    bool vec5Real = false;
    bool engineBackpressureTest = false;
    bool trsvTestHang = false;
    bool trsvTestStaleGeneration = false;
    bool trsvTestBadRequestId = false;
    bool vec5TestDuplicateCompletion = false;
    bool traceEnable = false;
    uint32_t traceLines = 1;
    uint32_t traceCells = 4;
    const char *traceFile = "m5out/lusgs_event_trace.csv";
};

uint64_t
envU64(const char *name, uint64_t fallback)
{
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    char *end = nullptr;
    const uint64_t parsed = std::strtoull(value, &end, 0);
    return (end && *end == '\0') ? parsed : fallback;
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

EventEnv
readEventEnv()
{
    EventEnv env;
    const char *stage = std::getenv("GEM5_CFD_LUSGS_STEP4_STAGE");
    if (!stage || !*stage)
        stage = std::getenv("GEM5_CFD_LUSGS_STEP3_STAGE");
    if (stage && *stage)
        env.stage = *stage;
    env.contexts = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_CONTEXTS", 1));
    env.tileCells = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TILE_CELLS", 1));
    env.ticksPerCycle = std::max<uint64_t>(
        1, envU64("GEM5_CFD_TICKS_PER_CYCLE", 500));
    env.spmReadLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_EVENT_SPM_READ_LAT", 2));
    env.spmWriteLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_EVENT_SPM_WRITE_LAT", 1));
    env.trsvLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_TRSV5_LAT", 60));
    env.vec5Latency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_LAT", 4));
    env.trsvEventCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_EVENT_COUNT", 1));
    env.trsvDivCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_DIV_COUNT", 1));
    env.trsvFmaCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_FMA_COUNT", 1));
    env.trsvDivLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_DIV_LAT", 4));
    env.trsvFmaLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_FMA_LAT", 3));
    env.trsvLoadLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_LOAD_LAT", 2));
    env.trsvResultLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_RESULT_LAT", 1));
    env.trsvQueueDepth = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TRSV5_QUEUE_DEPTH", 2));
    env.trsvForwarding = envBool(
        "GEM5_CFD_LUSGS_TRSV5_FORWARDING", true);
    env.vec5Count = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_EVENT_COUNT", 1));
    env.vec5Lanes = envU64("GEM5_CFD_LUSGS_VEC5_LANES", 5);
    if (env.vec5Lanes != 1 && env.vec5Lanes != 2 && env.vec5Lanes != 5)
        env.vec5Lanes = 5;
    env.vec5CopyLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_COPY_LAT", 1));
    env.vec5SubLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_SUB_LAT", 3));
    env.vec5AxpyLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_AXPY_LAT", 4));
    env.vec5InitiationInterval = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_INITIATION_INTERVAL", 1));
    env.vec5QueueDepth = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_QUEUE_DEPTH", 2));
    env.pathaLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_LAT", 43));
    env.pathaMatldLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_MATLD_LAT",
                  envU64("GEM5_CFD_LMAT_LAT", 1)));
    env.pathaMatldCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_MATLD_COUNT",
                  envU64("GEM5_CFD_READPORT_COUNT", 8)));
    env.pathaDotpLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_PATHA_DOTP_LAT", 7));
    env.pathaDotpCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_DOTP_COUNT",
                  envU64("GEM5_CFD_PATHA_DOTP_COUNT", 1)));
    env.pathaPackLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_PACK_LAT",
                  envU64("GEM5_CFD_PATHA_PACK_LAT", 1)));
    env.pathaPackCount = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_PACK_COUNT",
                  envU64("GEM5_CFD_PATHA_PACK_COUNT", 1)));
    env.pathaResultBufferDepth = std::max<uint64_t>(
        1, envU64("GEM5_CFD_PATHA_RESULT_BUFFER_DEPTH", 2));
    env.pathaResultLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_RESULT_LAT", 1));
    env.watchdogCycles = envU64(
        "GEM5_CFD_LUSGS_EVENT_WATCHDOG_CYCLES", 0);
    env.watchdogTestHang = envBool(
        "GEM5_CFD_LUSGS_EVENT_WATCHDOG_TEST_HANG", false);
    env.pathaReal = envBool("GEM5_CFD_LUSGS_STEP4_PATHA_REAL", false) ||
                    env.stage == 'B' || env.stage == 'C';
    env.trsvReal = envBool("GEM5_CFD_LUSGS_STEP4_TRSV_REAL", false) ||
                   env.stage == 'C';
    env.vec5Real = envBool("GEM5_CFD_LUSGS_STEP4_VEC5_REAL", false) ||
                   env.stage == 'C';
    env.engineBackpressureTest = envBool(
        "GEM5_CFD_LUSGS_STEP4_ENGINE_BACKPRESSURE_TEST", false);
    env.trsvTestHang = envBool("GEM5_CFD_LUSGS_TRSV5_TEST_HANG", false);
    env.trsvTestStaleGeneration = envBool(
        "GEM5_CFD_LUSGS_TRSV5_TEST_STALE_GENERATION", false);
    env.trsvTestBadRequestId = envBool(
        "GEM5_CFD_LUSGS_TRSV5_TEST_BAD_REQUEST_ID", false);
    env.vec5TestDuplicateCompletion = envBool(
        "GEM5_CFD_LUSGS_VEC5_TEST_DUPLICATE_COMPLETION", false);
    env.traceEnable = envBool("GEM5_CFD_LUSGS_EVENT_TRACE_ENABLE", false);
    env.traceLines = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_EVENT_TRACE_LINES", 1));
    env.traceCells = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_EVENT_TRACE_CELLS", 4));
    env.traceFile = std::getenv("GEM5_CFD_LUSGS_EVENT_TRACE_FILE");
    if (!env.traceFile || !*env.traceFile)
        env.traceFile = "m5out/lusgs_event_trace.csv";
    return env;
}

bool
aligned8(uint64_t addr)
{
    return (addr & 7ull) == 0;
}

Addr
matrixAddr(uint64_t base, const CfdLusgsDescriptor &d,
           uint32_t line, uint32_t cell)
{
    const uint64_t line_stride =
        d.line_stride_bytes ? d.line_stride_bytes :
        static_cast<uint64_t>(d.n_cells) * d.matrix_cell_stride_bytes;
    return base + static_cast<uint64_t>(line) * line_stride +
           static_cast<uint64_t>(cell) * d.matrix_cell_stride_bytes;
}

Addr
vectorAddr(uint64_t base, const CfdLusgsDescriptor &d,
           uint32_t line, uint32_t cell)
{
    const uint64_t line_stride =
        static_cast<uint64_t>(d.n_cells) * d.vector_cell_stride_bytes;
    return base + static_cast<uint64_t>(line) * line_stride +
           static_cast<uint64_t>(cell) * d.vector_cell_stride_bytes;
}

void
mvm5(const double *mat, const double *vec, double *out)
{
    for (int r = 0; r < 5; ++r) {
        double acc = 0.0;
        for (int c = 0; c < 5; ++c) {
            volatile double product = mat[r * 5 + c] * vec[c];
            acc += product;
        }
        out[r] = acc;
    }
}

const char *
stateName(EventState state)
{
    switch (state) {
      case EventState::Idle: return "IDLE";
      case EventState::CommandQueued: return "COMMAND_QUEUED";
      case EventState::ReadDescriptorReq: return "READ_DESCRIPTOR_REQ";
      case EventState::ReadDescriptorWait: return "READ_DESCRIPTOR_WAIT";
      case EventState::ValidateDescriptor: return "VALIDATE_DESCRIPTOR";
      case EventState::AllocContext: return "ALLOC_CONTEXT";
      case EventState::ForwardInit: return "FORWARD_INIT";
      case EventState::ForwardLoadReq: return "FORWARD_LOAD_REQ";
      case EventState::ForwardLoadWait: return "FORWARD_LOAD_WAIT";
      case EventState::ForwardPathAReq: return "FORWARD_PATHA_REQ";
      case EventState::ForwardPathAWait: return "FORWARD_PATHA_WAIT";
      case EventState::ForwardVec5Req: return "FORWARD_VEC5_REQ";
      case EventState::ForwardVec5Wait: return "FORWARD_VEC5_WAIT";
      case EventState::ForwardTrsvReq: return "FORWARD_TRSV_REQ";
      case EventState::ForwardTrsvWait: return "FORWARD_TRSV_WAIT";
      case EventState::ForwardDqstarWriteReq:
        return "FORWARD_DQSTAR_WRITE_REQ";
      case EventState::ForwardDqstarWriteWait:
        return "FORWARD_DQSTAR_WRITE_WAIT";
      case EventState::ForwardNext: return "FORWARD_NEXT";
      case EventState::BackwardInit: return "BACKWARD_INIT";
      case EventState::BackwardLoadReq: return "BACKWARD_LOAD_REQ";
      case EventState::BackwardLoadWait: return "BACKWARD_LOAD_WAIT";
      case EventState::BackwardPathAReq: return "BACKWARD_PATHA_REQ";
      case EventState::BackwardPathAWait: return "BACKWARD_PATHA_WAIT";
      case EventState::BackwardVec5Req: return "BACKWARD_VEC5_REQ";
      case EventState::BackwardVec5Wait: return "BACKWARD_VEC5_WAIT";
      case EventState::BackwardDqWriteReq: return "BACKWARD_DQ_WRITE_REQ";
      case EventState::BackwardDqWriteWait: return "BACKWARD_DQ_WRITE_WAIT";
      case EventState::BackwardNext: return "BACKWARD_NEXT";
      case EventState::QUpdateReq: return "Q_UPDATE_REQ";
      case EventState::QUpdateWait: return "Q_UPDATE_WAIT";
      case EventState::TaskComplete: return "TASK_COMPLETE";
      case EventState::TaskError: return "TASK_ERROR";
    }
    return "UNKNOWN";
}

const char *
lifecycleName(LusgsTaskLifecycle lifecycle)
{
    switch (lifecycle) {
      case LusgsTaskLifecycle::Idle: return "Idle";
      case LusgsTaskLifecycle::Queued: return "Queued";
      case LusgsTaskLifecycle::Running: return "Running";
      case LusgsTaskLifecycle::CompletedNotReaped:
        return "CompletedNotReaped";
      case LusgsTaskLifecycle::ErrorNotReaped: return "ErrorNotReaped";
      case LusgsTaskLifecycle::Aborting: return "Aborting";
    }
    return "Unknown";
}

const char *
phaseName(LusgsPhase phase)
{
    switch (phase) {
      case LusgsPhase::Task: return "task";
      case LusgsPhase::Forward: return "forward";
      case LusgsPhase::Backward: return "backward";
      case LusgsPhase::QUpdate: return "q_update";
    }
    return "unknown";
}

const char *
resourceTypeName(LusgsResourceType type)
{
    switch (type) {
      case LusgsResourceType::None: return "none";
      case LusgsResourceType::DescriptorRead: return "descriptor_spm_read";
      case LusgsResourceType::SpmRead: return "spm_read";
      case LusgsResourceType::PathA: return "patha_mvm";
      case LusgsResourceType::Trsv5: return "trsv5";
      case LusgsResourceType::Vec5Copy: return "vec5_copy";
      case LusgsResourceType::Vec5Sub: return "vec5_sub";
      case LusgsResourceType::Vec5Axpy: return "vec5_axpy";
      case LusgsResourceType::DqstarWrite: return "dqstar_write";
      case LusgsResourceType::DqWrite: return "dq_write";
    }
    return "unknown";
}

const char *
vec5OperationName(CfdLusgsVec5Op operation)
{
    switch (operation) {
      case CfdLusgsVec5Op::Copy: return "copy";
      case CfdLusgsVec5Op::Sub: return "sub";
      case CfdLusgsVec5Op::Axpy: return "axpy";
    }
    return "unknown";
}

bool
stateWaitsForResource(EventState state, LusgsResourceType type)
{
    switch (state) {
      case EventState::ReadDescriptorWait:
        return type == LusgsResourceType::DescriptorRead;
      case EventState::ForwardLoadWait:
      case EventState::BackwardLoadWait:
        return type == LusgsResourceType::SpmRead;
      case EventState::ForwardPathAWait:
      case EventState::BackwardPathAWait:
        return type == LusgsResourceType::PathA;
      case EventState::ForwardVec5Wait:
      case EventState::BackwardVec5Wait:
        return type == LusgsResourceType::Vec5Copy ||
               type == LusgsResourceType::Vec5Sub;
      case EventState::ForwardTrsvWait:
        return type == LusgsResourceType::Trsv5;
      case EventState::ForwardDqstarWriteWait:
        return type == LusgsResourceType::DqstarWrite;
      case EventState::BackwardDqWriteWait:
        return type == LusgsResourceType::DqWrite;
      case EventState::QUpdateWait:
        return type == LusgsResourceType::Vec5Axpy;
      default:
        return false;
    }
}

CfdLusgsStatus
validateDescriptor(const CfdLusgsDescriptor &d, const EventEnv &env)
{
    if (d.n_lines == 0 || d.n_cells == 0)
        return CfdLusgsStatus::BadShape;
    if (d.matrix_cell_stride_bytes < MatrixBytes ||
        d.vector_cell_stride_bytes < VectorBytes ||
        d.tile_cells == 0)
        return CfdLusgsStatus::BadStride;
    const uint32_t supported = LUSGS_FLAG_WRITE_DQ | LUSGS_FLAG_UPDATE_Q |
                               LUSGS_FLAG_CHECK_BOUNDS | LUSGS_FLAG_TRACE;
    if (d.flags & ~supported)
        return CfdLusgsStatus::UnsupportedFlags;
    const uint64_t addrs[] = {
        d.lu_a_base, d.c_base, d.bbar_base, d.rhs_base, d.dqstar_base,
        d.dq_base, d.q_base
    };
    for (uint64_t addr : addrs) {
        if (addr != 0 && !aligned8(addr))
            return CfdLusgsStatus::BadAlignment;
    }
    if (d.dqstar_base == 0 || d.rhs_base == 0 || d.lu_a_base == 0)
        return CfdLusgsStatus::BadDescriptor;
    if ((d.flags & LUSGS_FLAG_WRITE_DQ) && d.dq_base == 0)
        return CfdLusgsStatus::BadDescriptor;
    if ((d.flags & LUSGS_FLAG_UPDATE_Q) && d.q_base == 0)
        return CfdLusgsStatus::BadDescriptor;
    const uint64_t tile_bytes =
        static_cast<uint64_t>(d.n_lines) * d.tile_cells *
        (3 * MatrixBytes + 4 * VectorBytes);
    if ((d.flags & LUSGS_FLAG_CHECK_BOUNDS) && tile_bytes > LocalSpmCapacity)
        return CfdLusgsStatus::SpmCapacity;
    if ((env.stage != 'A' && env.stage != 'B' && env.stage != 'C') ||
        d.n_lines != 1 || env.contexts != 1 ||
        env.tileCells != 1 || d.tile_cells != 1)
        return CfdLusgsStatus::UnsupportedStage;
    if (env.pathaReal &&
        (d.c_base == 0 || d.bbar_base == 0 || d.dq_base == 0))
        return CfdLusgsStatus::BadDescriptor;
    return CfdLusgsStatus::Complete;
}

class CfdLusgsEventController
{
  public:
    CfdLusgsEventController();

    bool busy() const { return taskActive; }
    uint64_t launch(ExecContext *xc, Addr descriptor_addr);
    uint64_t wait(uint64_t token);

  private:
    void processTick();
    void processRequestComplete(uint64_t request_id);
    void scheduleTick(uint64_t cycles = 1);
    bool issueRequest(LusgsResourceType resource, uint64_t cycles,
                      EventState wait_state, bool schedule_completion = true);
    bool consumeRequest(LusgsResourceType resource);
    void cancelPendingRequests();
    void freeDeferredRequests();
    void resetPathAEngine();
    void configurePathAEngine();
    bool trySubmitPathAMvm(const PathAMvmRequest &req);
    void cancelPathAMvm(uint64_t request_id, uint64_t generation);
    void onPathAMatLdComplete(uint64_t request_id, uint8_t field);
    void onPathADotpComplete(uint64_t request_id, uint8_t row);
    void onPathAPackComplete(uint64_t request_id);
    void onPathAResultReady(uint64_t request_id);
    void tryIssuePathADotps(PathAMvmTransaction &tx);
    void tryIssuePathAPack(PathAMvmTransaction &tx);
    uint64_t allocatePathASubEvent(
        Tick when, std::function<void()> fn, const char *name);
    Tick reservePathAUnit(std::vector<Tick> &next_free, uint64_t latency,
                          uint64_t &busy_cycles, uint64_t &retries);
    void recordPathAMvmLatency(const PathAMvmTransaction &tx);
    void resetTrsv5Engine();
    bool canAcceptTrsv5() const;
    bool trySubmitTrsv5(const Trsv5EventRequest &req);
    void cancelTrsv5(uint64_t request_id, uint64_t generation);
    void startQueuedTrsv5();
    void startTrsv5(uint64_t request_id);
    void driveTrsv5(uint64_t request_id);
    void tryIssueTrsv5Divide(uint64_t request_id);
    void tryIssueTrsv5Fmas(uint64_t request_id);
    void onTrsv5LoadComplete(uint64_t request_id);
    void onTrsv5DivideComplete(uint64_t request_id, int k);
    void onTrsv5FmaComplete(uint64_t request_id, int k, int i,
                            bool backward);
    void onTrsv5ResultReady(uint64_t request_id);
    void scheduleTrsv5Drive(uint64_t request_id, uint64_t cycles,
                            const char *reason);
    EventFunctionWrapper *allocateTrsv5Event(
        uint64_t request_id, Tick when, std::function<void()> fn,
        const char *name);
    bool takeTrsv5Response(uint64_t request_id,
                           Trsv5EventResponse &response);
    void recordTrsv5Latency(const Trsv5Transaction &tx);
    void injectTrsv5ValidationCompletion(uint64_t request_id,
                                         bool stale_generation);
    void resetVec5Engine();
    bool canAcceptVec5() const;
    bool trySubmitVec5(const Vec5EventRequest &req);
    void cancelVec5(uint64_t request_id, uint64_t generation);
    void startQueuedVec5();
    void startVec5(uint64_t request_id);
    void issueVec5LaneGroup(uint64_t request_id);
    void onVec5LaneComplete(uint64_t request_id, uint32_t lane);
    EventFunctionWrapper *allocateVec5Event(
        uint64_t request_id, Tick when, std::function<void()> fn,
        const char *name);
    bool takeVec5Response(uint64_t request_id, Vec5EventResponse &response);
    uint64_t vec5OperationLatency(CfdLusgsVec5Op operation) const;
    void recordVec5Latency(const Vec5Transaction &tx);
    void recordVec5Issue(LusgsResourceType resource, uint64_t cycles);
    void recordVec5Complete(LusgsResourceType resource);
    LusgsContextState &context(uint32_t context_id = 0);
    void updateContextForState(EventState next);
    LusgsPhase phaseForState(EventState s) const;
    uint32_t cellForState(EventState s) const;
    uint32_t currentTile(uint32_t cell) const;
    uint64_t ticksToCycles(Tick ticks) const;
    void setLifecycle(LusgsTaskLifecycle next);
    void accountLifecycleCycles(Tick now);
    bool checkWatchdog();
    void markProgress(uint64_t request_id = 0);
    void transition(EventState next, const char *event_type,
                    const char *resource, const char *wait_reason = "");
    void trace(const char *event_type, const char *resource,
               uint64_t request_id, bool request_valid, bool resource_ready,
               bool accepted, bool retry, bool complete,
               bool dependency_ready, uint32_t queue_depth,
               const char *wait_reason, EventState next_state,
               const char *completion_validation = "ok",
               bool stale_event = false, bool cancelled = false,
               const char *operation = "", int stage_k = -1,
               int stage_i = -1, int lane = -1);
    bool traceCell() const;
    void ensureTraceOpen();
    SETranslatingPortProxy proxy() const;
    void finishTask(CfdLusgsStatus final_status);
    void recordReport();
    void clearTask();
    uint64_t cycle() const;

    EventQueue *eventq() const;

    EventFunctionWrapper tickEvent;

    EventState state = EventState::Idle;
    LusgsTaskLifecycle lifecycle = LusgsTaskLifecycle::Idle;
    EventEnv env;
    ThreadContext *tc = nullptr;
    Addr descAddr = 0;
    CfdLusgsDescriptor descriptorSnapshot = {};
    bool descriptorSnapshotValid = false;
    CfdLusgsStatus status = CfdLusgsStatus::Complete;
    bool taskActive = false;
    bool taskComplete = false;
    uint64_t token = 0;
    uint64_t nextToken = 1;
    uint64_t nextRequestId = 1;
    uint64_t taskGeneration = 0;
    uint64_t completedRequestId = 0;
    CfdLusgsStatus pendingCompletionError = CfdLusgsStatus::Complete;
    std::unordered_map<uint64_t, std::unique_ptr<PendingLusgsRequest>>
        pendingRequests;
    std::vector<uint64_t> deferredFreeRequests;
    std::vector<LusgsContextState> contexts;
    std::vector<PathASlot> pathaSlots;
    PathAMvmTransaction pathaTx;
    PathAMvmResponse pathaResponse;
    bool pathaResponseValid = false;
    std::vector<Tick> pathaMatldNextFree;
    std::vector<Tick> pathaDotpNextFree;
    std::vector<Tick> pathaPackNextFree;
    std::vector<std::unique_ptr<EventFunctionWrapper>> pathaEvents;
    uint64_t nextPathASubRequestId = 1;
    uint64_t pathaMvmLatencyTotal = 0;
    uint64_t pathaMvmLatencyCount = 0;
    uint64_t pathaMvmLatencyMin = 0;
    uint64_t pathaMvmLatencyMax = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Trsv5Transaction>>
        trsvTransactions;
    std::unordered_map<uint64_t, Trsv5EventResponse> trsvResponses;
    std::deque<uint64_t> trsvQueue;
    std::vector<Tick> trsvDividerNextFree;
    std::vector<Tick> trsvFmaNextFree;
    std::vector<std::unique_ptr<EventFunctionWrapper>> trsvEvents;
    uint32_t trsvActive = 0;
    uint64_t trsvLatencyTotal = 0;
    uint64_t trsvLatencyCount = 0;
    uint64_t trsvLatencyMin = 0;
    uint64_t trsvLatencyMax = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Vec5Transaction>>
        vec5Transactions;
    std::unordered_map<uint64_t, Vec5EventResponse> vec5Responses;
    std::deque<uint64_t> vec5Queue;
    std::vector<std::unique_ptr<EventFunctionWrapper>> vec5Events;
    uint32_t vec5Active = 0;
    uint64_t vec5LatencyTotal = 0;
    uint64_t vec5LatencyCount = 0;
    uint64_t vec5LaneCapacityCycles = 0;
    bool trsvBackpressureInjected = false;
    bool vec5BackpressureInjected = false;
    bool trsvHangInjected = false;
    bool trsvStaleGenerationInjected = false;
    bool trsvBadRequestIdInjected = false;
    bool vec5DuplicateCompletionInjected = false;
    Tick startTick = 0;
    Tick completeTick = 0;
    Tick successfulWaitTick = 0;
    Tick tokenReapTick = 0;
    Tick lastLifecycleTick = 0;
    Tick lastProgressTick = 0;
    EventState lastProgressState = EventState::Idle;
    uint64_t lastProgressRequestId = 0;
    bool watchdogHangInjected = false;
    FILE *traceFp = nullptr;

    uint32_t line = 0;
    uint32_t forwardCell = 0;
    uint32_t backwardCell = 0;
    uint32_t qCell = 0;
    std::vector<std::array<double, 5>> dqstar;
    std::vector<std::array<double, 5>> dq;
    std::array<double, 25> lu = {};
    std::array<double, 25> cmat = {};
    std::array<double, 25> bbar = {};
    std::array<double, 5> rhs = {};
    std::array<double, 5> tmp = {};
    std::array<double, 5> value = {};
    std::array<double, 5> qvec = {};
    CfdLusgsControllerRecord report;
};

CfdLusgsEventController::CfdLusgsEventController()
    : tickEvent([this] { processTick(); }, "cfd_lusgs_event.tick")
{
}

EventQueue *
CfdLusgsEventController::eventq() const
{
    EventQueue *q = curEventQueue();
    if (!q)
        q = getEventQueue(0);
    return q;
}

uint64_t
CfdLusgsEventController::cycle() const
{
    return env.ticksPerCycle ? curTick() / env.ticksPerCycle : curTick();
}

uint64_t
CfdLusgsEventController::ticksToCycles(Tick ticks) const
{
    return env.ticksPerCycle ? ticks / env.ticksPerCycle : ticks;
}

SETranslatingPortProxy
CfdLusgsEventController::proxy() const
{
    return SETranslatingPortProxy(tc, SETranslatingPortProxy::Never);
}

void
CfdLusgsEventController::scheduleTick(uint64_t cycles)
{
    EventQueue *q = eventq();
    assert(q);
    const Tick when = curTick() + std::max<uint64_t>(1, cycles) *
        env.ticksPerCycle;
    if (tickEvent.scheduled()) {
        if (when < tickEvent.when())
            q->reschedule(&tickEvent, when, true);
        return;
    } else {
        q->schedule(&tickEvent, when);
    }
}

LusgsContextState &
CfdLusgsEventController::context(uint32_t context_id)
{
    if (contexts.size() <= context_id)
        contexts.resize(context_id + 1);
    return contexts[context_id];
}

LusgsPhase
CfdLusgsEventController::phaseForState(EventState s) const
{
    const int state_id = static_cast<int>(s);
    if (s == EventState::QUpdateReq || s == EventState::QUpdateWait)
        return LusgsPhase::QUpdate;
    if (state_id >= static_cast<int>(EventState::BackwardInit) &&
        state_id <= static_cast<int>(EventState::BackwardNext))
        return LusgsPhase::Backward;
    if (state_id >= static_cast<int>(EventState::ForwardInit) &&
        state_id <= static_cast<int>(EventState::ForwardNext))
        return LusgsPhase::Forward;
    return LusgsPhase::Task;
}

uint32_t
CfdLusgsEventController::cellForState(EventState s) const
{
    const LusgsPhase phase = phaseForState(s);
    if (phase == LusgsPhase::Backward)
        return backwardCell;
    if (phase == LusgsPhase::QUpdate)
        return qCell;
    if (phase == LusgsPhase::Forward)
        return forwardCell;
    return 0;
}

uint32_t
CfdLusgsEventController::currentTile(uint32_t cell) const
{
    return descriptorSnapshot.tile_cells ?
        cell / descriptorSnapshot.tile_cells : 0;
}

void
CfdLusgsEventController::updateContextForState(EventState next)
{
    LusgsContextState &ctx = context(0);
    ctx.valid = true;
    ctx.contextId = 0;
    ctx.line = line;
    ctx.cell = cellForState(next);
    ctx.tile = currentTile(ctx.cell);
    ctx.phase = phaseForState(next);
    ctx.state = next;
}

void
CfdLusgsEventController::accountLifecycleCycles(Tick now)
{
    if (lastLifecycleTick == 0) {
        lastLifecycleTick = now;
        return;
    }
    const uint64_t cycles = ticksToCycles(now - lastLifecycleTick);
    switch (lifecycle) {
      case LusgsTaskLifecycle::Idle:
        report.eventLifecycleIdleCycles += cycles;
        break;
      case LusgsTaskLifecycle::Queued:
        report.eventLifecycleQueuedCycles += cycles;
        break;
      case LusgsTaskLifecycle::Running:
        report.eventLifecycleRunningCycles += cycles;
        break;
      case LusgsTaskLifecycle::CompletedNotReaped:
        report.eventLifecycleCompletedNotReapedCycles += cycles;
        break;
      case LusgsTaskLifecycle::ErrorNotReaped:
        report.eventLifecycleErrorNotReapedCycles += cycles;
        break;
      case LusgsTaskLifecycle::Aborting:
        break;
    }
    lastLifecycleTick = now;
}

void
CfdLusgsEventController::setLifecycle(LusgsTaskLifecycle next)
{
    if (lifecycle == next)
        return;
    accountLifecycleCycles(curTick());
    lifecycle = next;
    report.eventStateTransitions++;
}

void
CfdLusgsEventController::markProgress(uint64_t request_id)
{
    lastProgressTick = curTick();
    lastProgressState = state;
    lastProgressRequestId = request_id;
}

bool
CfdLusgsEventController::checkWatchdog()
{
    if (env.watchdogCycles == 0 || lifecycle != LusgsTaskLifecycle::Running)
        return false;
    const Tick limit = static_cast<Tick>(env.watchdogCycles) *
        env.ticksPerCycle;
    if (curTick() - lastProgressTick <= limit)
        return false;
    report.eventWatchdogTimeouts++;
    report.eventNoProgressCycles += ticksToCycles(curTick() - lastProgressTick);
    finishTask(CfdLusgsStatus::WatchdogTimeout);
    return true;
}

bool
CfdLusgsEventController::traceCell() const
{
    const int state_id = static_cast<int>(state);
    const bool backward_state =
        state_id >= static_cast<int>(EventState::BackwardInit) &&
        state_id <= static_cast<int>(EventState::BackwardNext);
    const uint32_t cell =
        backward_state ? backwardCell :
        state == EventState::QUpdateReq ||
        state == EventState::QUpdateWait ? qCell : forwardCell;
    return line < env.traceLines && cell < env.traceCells;
}

void
CfdLusgsEventController::ensureTraceOpen()
{
    if (traceFp || !env.traceEnable)
        return;
    traceFp = std::fopen(env.traceFile, "w");
    if (!traceFp)
        return;
    std::fprintf(traceFp,
        "tick,cycle,task_token,task_generation,context_id,line,cell,tile,"
        "phase,state,event_type,resource,request_id,request_type,operation,"
        "k,i,lane,"
        "request_valid,resource_ready,"
        "accepted,retry,complete,dependency_ready,queue_depth,bank,port,"
        "wait_reason,next_state,lifecycle,pending_request_count,"
        "descriptor_snapshot_valid,stale_event,cancelled,"
        "completion_validation,last_progress_tick\n");
}

void
CfdLusgsEventController::trace(const char *event_type, const char *resource,
                               uint64_t request_id, bool request_valid,
                               bool resource_ready, bool accepted, bool retry,
                               bool complete, bool dependency_ready,
                               uint32_t queue_depth,
                               const char *wait_reason,
                               EventState next_state,
                               const char *completion_validation,
                               bool stale_event, bool cancelled,
                               const char *operation, int stage_k,
                               int stage_i, int lane)
{
    ensureTraceOpen();
    if (!traceFp || !traceCell())
        return;
    const int state_id = static_cast<int>(state);
    const bool backward_state =
        state_id >= static_cast<int>(EventState::BackwardInit) &&
        state_id <= static_cast<int>(EventState::BackwardNext);
    const bool forward_state =
        state_id >= static_cast<int>(EventState::ForwardInit) &&
        state_id <= static_cast<int>(EventState::ForwardNext);
    const uint32_t cell =
        backward_state ? backwardCell :
        state == EventState::QUpdateReq ||
        state == EventState::QUpdateWait ? qCell : forwardCell;
    const LusgsPhase phase =
        state == EventState::QUpdateReq ||
        state == EventState::QUpdateWait ? LusgsPhase::QUpdate :
        backward_state ? LusgsPhase::Backward :
        forward_state ? LusgsPhase::Forward : LusgsPhase::Task;
    const char *request_type = resource;
    auto it = pendingRequests.find(request_id);
    if (it != pendingRequests.end())
        request_type = resourceTypeName(it->second->header.resourceType);
    std::fprintf(traceFp,
        "%llu,%llu,%llu,%llu,0,%u,%u,%u,%s,%s,%s,%s,%llu,%s,%s,%d,%d,%d,"
        "%u,%u,%u,%u,%u,%u,%u,0,0,%s,%s,%s,%zu,%u,%u,%u,%s,%llu\n",
        (unsigned long long)curTick(), (unsigned long long)cycle(),
        (unsigned long long)token, (unsigned long long)taskGeneration,
        line, cell, currentTile(cell), phaseName(phase), stateName(state), event_type,
        resource, (unsigned long long)request_id, request_type,
        operation, stage_k, stage_i, lane,
        request_valid ? 1 : 0, resource_ready ? 1 : 0,
        accepted ? 1 : 0, retry ? 1 : 0,
        complete ? 1 : 0, dependency_ready ? 1 : 0, queue_depth,
        wait_reason, stateName(next_state), lifecycleName(lifecycle),
        pendingRequests.size(), descriptorSnapshotValid ? 1 : 0,
        stale_event ? 1 : 0, cancelled ? 1 : 0, completion_validation,
        (unsigned long long)lastProgressTick);
}

void
CfdLusgsEventController::transition(EventState next,
                                    const char *event_type,
                                    const char *resource,
                                    const char *wait_reason)
{
    trace(event_type, resource, 0, false, true, false, false, false,
          true, taskActive ? 1 : 0, wait_reason, next);
    state = next;
    updateContextForState(next);
    markProgress();
    report.eventStateTransitions++;
    scheduleTick();
}

void
CfdLusgsEventController::recordVec5Issue(LusgsResourceType resource,
                                         uint64_t cycles)
{
    report.eventVec5Requests++;
    report.eventVec5Accepted++;
    report.eventVec5WaitCycles += cycles;
    switch (resource) {
      case LusgsResourceType::Vec5Copy:
        report.eventVec5CopyRequests++;
        report.eventVec5CopyAccepted++;
        report.eventVec5CopyWaitCycles += cycles;
        break;
      case LusgsResourceType::Vec5Sub:
        report.eventVec5SubRequests++;
        report.eventVec5SubAccepted++;
        report.eventVec5SubWaitCycles += cycles;
        break;
      case LusgsResourceType::Vec5Axpy:
        report.eventVec5AxpyRequests++;
        report.eventVec5AxpyAccepted++;
        report.eventVec5AxpyWaitCycles += cycles;
        break;
      default:
        break;
    }
}

void
CfdLusgsEventController::recordVec5Complete(LusgsResourceType resource)
{
    report.eventVec5Completed++;
    switch (resource) {
      case LusgsResourceType::Vec5Copy:
        report.eventVec5CopyCompleted++;
        break;
      case LusgsResourceType::Vec5Sub:
        report.eventVec5SubCompleted++;
        break;
      case LusgsResourceType::Vec5Axpy:
        report.eventVec5AxpyCompleted++;
        break;
      default:
        break;
    }
}

bool
CfdLusgsEventController::issueRequest(LusgsResourceType resource,
                                      uint64_t cycles,
                                      EventState wait_state,
                                      bool schedule_completion)
{
    static constexpr size_t MaxPendingRequests = 64;
    if (pendingRequests.size() >= MaxPendingRequests) {
        report.eventPendingRequestFullStalls++;
        pendingCompletionError = CfdLusgsStatus::InternalStateError;
        scheduleTick();
        return false;
    }

    const uint32_t cell = cellForState(wait_state);
    PendingLusgsRequest req;
    req.header.requestId = nextRequestId++;
    req.header.taskToken = token;
    req.header.taskGeneration = taskGeneration;
    req.header.contextId = 0;
    req.header.line = line;
    req.header.cell = cell;
    req.header.tile = currentTile(cell);
    req.header.phase = phaseForState(wait_state);
    req.header.resourceType = resource;
    req.issueTick = curTick();
    req.completionTick = curTick() + std::max<uint64_t>(1, cycles) *
        env.ticksPerCycle;
    req.accepted = true;

    const uint64_t request_id = req.header.requestId;
    req.completionEvent = std::make_unique<EventFunctionWrapper>(
        [this, request_id] { processRequestComplete(request_id); },
        "cfd_lusgs_event.request_complete");

    LusgsContextState &ctx = context(0);
    if (ctx.waitingRequestId != 0) {
        report.eventWrongStateCompletions++;
        pendingCompletionError = CfdLusgsStatus::InternalStateError;
        scheduleTick();
        return false;
    }

    pendingRequests.emplace(
        request_id, std::make_unique<PendingLusgsRequest>(std::move(req)));
    report.eventPendingRequestAlloc++;
    report.eventMaxPendingRequests = std::max<uint64_t>(
        report.eventMaxPendingRequests, pendingRequests.size());
    if (report.eventFirstResourceIssueTick == 0 &&
        resource != LusgsResourceType::DescriptorRead)
        report.eventFirstResourceIssueTick = curTick();

    state = wait_state;
    updateContextForState(wait_state);
    ctx.waitingRequestId = request_id;
    ctx.waitingResource = resource;
    markProgress(request_id);

    trace("create", resourceTypeName(resource), request_id, true, true,
          true, false, false, true, pendingRequests.size(), "",
          wait_state);
    trace("issue", resourceTypeName(resource), request_id, true, true,
          true, false, false, true, pendingRequests.size(), "",
          wait_state);
    trace("accepted", resourceTypeName(resource), request_id, true, true,
          true, false, false, true, pendingRequests.size(), "",
          wait_state);

    if (schedule_completion && env.watchdogTestHang && env.watchdogCycles > 0 &&
        !watchdogHangInjected &&
        resource != LusgsResourceType::DescriptorRead) {
        watchdogHangInjected = true;
        trace("test_hang", resourceTypeName(resource), request_id, true,
              false, true, false, false, true, pendingRequests.size(),
              "watchdog_test_hang", wait_state);
        scheduleTick(env.watchdogCycles + 1);
        return true;
    }

    if (schedule_completion) {
        EventQueue *q = eventq();
        assert(q);
        q->schedule(pendingRequests[request_id]->completionEvent.get(),
                    pendingRequests[request_id]->completionTick);
    }
    return true;
}

bool
CfdLusgsEventController::consumeRequest(LusgsResourceType resource)
{
    LusgsContextState &ctx = context(0);
    const uint64_t request_id = ctx.waitingRequestId;
    if (request_id == 0 || ctx.waitingResource != resource) {
        report.eventWrongStateCompletions++;
        finishTask(CfdLusgsStatus::InternalStateError);
        return false;
    }
    auto it = pendingRequests.find(request_id);
    if (it == pendingRequests.end()) {
        report.eventUnexpectedCompletions++;
        finishTask(CfdLusgsStatus::UnexpectedCompletion);
        return false;
    }
    PendingLusgsRequest &req = *it->second;
    if (!req.completed) {
        trace("wait", resourceTypeName(resource), request_id, false, false,
              false, false, false, true, pendingRequests.size(),
              "resource_wait", state);
        return false;
    }

    trace("consume", resourceTypeName(resource), request_id, true, true,
          false, false, true, true, pendingRequests.size(), "", state);
    trace("free", resourceTypeName(resource), request_id, false, true,
          false, false, true, true, pendingRequests.size(), "", state);
    pendingRequests.erase(it);
    report.eventPendingRequestFree++;
    ctx.waitingRequestId = 0;
    ctx.waitingResource = LusgsResourceType::None;
    completedRequestId = request_id;
    markProgress(request_id);
    return true;
}

void
CfdLusgsEventController::cancelPendingRequests()
{
    EventQueue *q = eventq();
    if (pathaTx.valid)
        cancelPathAMvm(pathaTx.request.header.requestId,
                       pathaTx.request.header.taskGeneration);
    for (const auto &entry : pendingRequests) {
        const LusgsEventRequestHeader &header = entry.second->header;
        if (header.resourceType == LusgsResourceType::Trsv5)
            cancelTrsv5(header.requestId, header.taskGeneration);
        else if (header.resourceType == LusgsResourceType::Vec5Copy ||
                 header.resourceType == LusgsResourceType::Vec5Sub ||
                 header.resourceType == LusgsResourceType::Vec5Axpy)
            cancelVec5(header.requestId, header.taskGeneration);
    }
    for (auto &entry : pendingRequests) {
        PendingLusgsRequest &req = *entry.second;
        if (req.cancelled)
            continue;
        req.cancelled = true;
        if (req.completionEvent && req.completionEvent->scheduled())
            q->deschedule(req.completionEvent.get());
        trace("cancel", resourceTypeName(req.header.resourceType),
              req.header.requestId, true, false, false, false, false, true,
              pendingRequests.size(), "", state, "cancelled", false, true);
        report.eventCancelledRequests++;
        report.eventPendingRequestFree++;
    }
    pendingRequests.clear();
    deferredFreeRequests.clear();
    for (auto &ctx : contexts) {
        ctx.waitingRequestId = 0;
        ctx.waitingResource = LusgsResourceType::None;
    }
}

void
CfdLusgsEventController::freeDeferredRequests()
{
    for (uint64_t request_id : deferredFreeRequests) {
        auto it = pendingRequests.find(request_id);
        if (it == pendingRequests.end())
            continue;
        trace("free", resourceTypeName(it->second->header.resourceType),
              request_id, false, true, false, false, true, true,
              pendingRequests.size(), "", state, "stale_free", true,
              it->second->cancelled);
        pendingRequests.erase(it);
        report.eventPendingRequestFree++;
    }
    deferredFreeRequests.clear();
}

void
CfdLusgsEventController::resetPathAEngine()
{
    pathaTx = {};
    pathaResponse = {};
    pathaResponseValid = false;
    pathaSlots.clear();
    pathaMatldNextFree.clear();
    pathaDotpNextFree.clear();
    pathaPackNextFree.clear();
    pathaEvents.clear();
    nextPathASubRequestId = 1;
    pathaMvmLatencyTotal = 0;
    pathaMvmLatencyCount = 0;
    pathaMvmLatencyMin = 0;
    pathaMvmLatencyMax = 0;
}

void
CfdLusgsEventController::configurePathAEngine()
{
    if (pathaSlots.size() != env.pathaResultBufferDepth)
        pathaSlots.assign(env.pathaResultBufferDepth, {});
    if (pathaMatldNextFree.size() != env.pathaMatldCount)
        pathaMatldNextFree.assign(env.pathaMatldCount, curTick());
    if (pathaDotpNextFree.size() != env.pathaDotpCount)
        pathaDotpNextFree.assign(env.pathaDotpCount, curTick());
    if (pathaPackNextFree.size() != env.pathaPackCount)
        pathaPackNextFree.assign(env.pathaPackCount, curTick());
}

uint64_t
CfdLusgsEventController::allocatePathASubEvent(
    Tick when, std::function<void()> fn, const char *name)
{
    EventQueue *q = eventq();
    assert(q);
    pathaEvents.push_back(std::make_unique<EventFunctionWrapper>(fn, name));
    q->schedule(pathaEvents.back().get(), when);
    return nextPathASubRequestId++;
}

Tick
CfdLusgsEventController::reservePathAUnit(std::vector<Tick> &next_free,
                                          uint64_t latency,
                                          uint64_t &busy_cycles,
                                          uint64_t &retries)
{
    configurePathAEngine();
    auto best = std::min_element(next_free.begin(), next_free.end());
    const Tick issue_tick = std::max(curTick(), *best);
    if (issue_tick > curTick()) {
        busy_cycles += ticksToCycles(issue_tick - curTick());
        retries++;
    }
    *best = issue_tick + env.ticksPerCycle;
    return issue_tick + std::max<uint64_t>(1, latency) * env.ticksPerCycle;
}

bool
CfdLusgsEventController::trySubmitPathAMvm(const PathAMvmRequest &req)
{
    configurePathAEngine();
    report.eventPathaMvmRequests++;
    report.eventPathaRequests++;
    report.pathaRequests++;
    report.pathaArbiterLusgsRequests++;

    if (pathaTx.valid && pathaTx.state != PathAMvmTransaction::State::Completed &&
        pathaTx.state != PathAMvmTransaction::State::Cancelled &&
        pathaTx.state != PathAMvmTransaction::State::Error) {
        report.eventPathaMvmRetries++;
        report.eventPathaRetries++;
        report.eventPathaSlotFullStalls++;
        report.arbiterStalls++;
        return false;
    }

    int slot_id = -1;
    for (size_t i = 0; i < pathaSlots.size(); ++i) {
        if (pathaSlots[i].state == PathASlotState::Free) {
            slot_id = static_cast<int>(i);
            break;
        }
    }
    if (slot_id < 0) {
        report.eventPathaSlotFullStalls++;
        report.eventPathaMvmRetries++;
        report.eventPathaRetries++;
        return false;
    }

    PathASlot &slot = pathaSlots[slot_id];
    if (slot.requestId != 0) {
        report.eventPathaSlotOverwriteErrors++;
        pendingCompletionError = CfdLusgsStatus::InternalStateError;
        scheduleTick();
        return false;
    }

    slot = {};
    slot.state = PathASlotState::Allocated;
    slot.requestId = req.header.requestId;
    report.eventPathaSlotAlloc++;

    pathaTx = {};
    pathaTx.valid = true;
    pathaTx.request = req;
    pathaTx.state = PathAMvmTransaction::State::LoadingInputs;
    pathaTx.slot = static_cast<uint8_t>(slot_id);
    pathaTx.issueTick = curTick();
    report.eventPathaMvmAccepted++;
    report.eventPathaAccepted++;

    for (uint8_t field = 0; field < 6; ++field) {
        report.eventPathaMatLdRequests++;
        uint64_t busy = 0;
        uint64_t retries = 0;
        const Tick complete_tick = reservePathAUnit(
            pathaMatldNextFree, env.pathaMatldLatency, busy, retries);
        report.eventPathaMatLdBusyCycles += busy;
        report.eventPathaMatLdRetries += retries;
        report.eventPathaMatLdAccepted++;
        report.eventPathaMatLdBytes += VectorBytes;
        pathaTx.matldIssued++;
        const Tick issue_tick =
            complete_tick - env.pathaMatldLatency * env.ticksPerCycle;
        if (pathaTx.firstLoadIssueTick == 0 ||
            issue_tick < pathaTx.firstLoadIssueTick)
            pathaTx.firstLoadIssueTick = issue_tick;
        trace("matld_issue", "patha_matld", req.header.requestId, true,
              true, true, retries != 0, false, true, field,
              "", state);
        allocatePathASubEvent(
            complete_tick,
            [this, request_id = req.header.requestId, field] {
                onPathAMatLdComplete(request_id, field);
            },
            "cfd_lusgs_patha.matld_complete");
    }
    return true;
}

void
CfdLusgsEventController::cancelPathAMvm(uint64_t request_id,
                                        uint64_t generation)
{
    if (!pathaTx.valid ||
        pathaTx.request.header.requestId != request_id ||
        pathaTx.request.header.taskGeneration != generation)
        return;
    pathaTx.state = PathAMvmTransaction::State::Cancelled;
    EventQueue *q = eventq();
    for (auto &event : pathaEvents) {
        if (event && event->scheduled())
            q->deschedule(event.get());
    }
    for (auto &slot : pathaSlots) {
        if (slot.requestId == request_id) {
            slot = {};
            report.eventPathaSlotFree++;
        }
    }
}

void
CfdLusgsEventController::onPathAMatLdComplete(uint64_t request_id,
                                              uint8_t field)
{
    report.eventEventsProcessed++;
    if (!pathaTx.valid || pathaTx.request.header.requestId != request_id ||
        pathaTx.state == PathAMvmTransaction::State::Cancelled)
        return;
    if (field < 5) {
        proxy().readBlob(pathaTx.request.matrixBase +
                         static_cast<Addr>(field) * VectorBytes,
                         pathaTx.matrix[field].data(), VectorBytes);
        if (pathaTx.request.header.phase == LusgsPhase::Forward)
            report.cBytes += VectorBytes;
        else if (pathaTx.request.header.phase == LusgsPhase::Backward)
            report.bbarBytes += VectorBytes;
        pathaTx.rowReady[field] = true;
    } else {
        proxy().readBlob(pathaTx.request.vectorBase,
                         pathaTx.vector.data(), VectorBytes);
        if (pathaTx.request.header.phase == LusgsPhase::Forward) {
            report.dqstarReadBytes += VectorBytes;
            report.dqstarExternalReads++;
        } else if (pathaTx.request.header.phase == LusgsPhase::Backward) {
            report.dqReadBytes += VectorBytes;
        }
        pathaTx.vectorReady = true;
    }
    pathaTx.matldCompleted++;
    pathaTx.lastLoadCompleteTick = curTick();
    report.eventPathaMatLdCompleted++;
    markProgress(request_id);
    trace("matld_complete", "patha_matld", request_id, true, true,
          false, false, true, true, field, "", state);
    tryIssuePathADotps(pathaTx);
}

void
CfdLusgsEventController::tryIssuePathADotps(PathAMvmTransaction &tx)
{
    if (!tx.valid || tx.state == PathAMvmTransaction::State::Cancelled ||
        !tx.vectorReady)
        return;
    tx.state = PathAMvmTransaction::State::IssuingDotp;
    PathASlot &slot = pathaSlots[tx.slot];
    slot.state = PathASlotState::DotpInFlight;
    for (uint8_t row = 0; row < 5; ++row) {
        if (!tx.rowReady[row] || tx.dotpInFlight[row] || tx.dotpDone[row])
            continue;
        report.eventPathaDotpRequests++;
        uint64_t busy = 0;
        uint64_t retries = 0;
        const Tick complete_tick = reservePathAUnit(
            pathaDotpNextFree, env.pathaDotpLatency, busy, retries);
        report.eventPathaDotpBusyCycles += busy;
        report.eventPathaDotpRetries += retries;
        report.eventPathaDotpAccepted++;
        report.eventPathaDotpPipelineOccupancy += env.pathaDotpLatency;
        tx.dotpIssued++;
        tx.dotpInFlight[row] = true;
        const Tick issue_tick =
            complete_tick - env.pathaDotpLatency * env.ticksPerCycle;
        if (tx.firstDotpIssueTick == 0 || issue_tick < tx.firstDotpIssueTick)
            tx.firstDotpIssueTick = issue_tick;
        trace("dotp_issue", "patha_dotp", tx.request.header.requestId,
              true, true, true, retries != 0, false, true, row, "",
              state);
        allocatePathASubEvent(
            complete_tick,
            [this, request_id = tx.request.header.requestId, row] {
                onPathADotpComplete(request_id, row);
            },
            "cfd_lusgs_patha.dotp_complete");
    }
    tx.state = PathAMvmTransaction::State::WaitingDotp;
}

void
CfdLusgsEventController::onPathADotpComplete(uint64_t request_id,
                                             uint8_t row)
{
    report.eventEventsProcessed++;
    if (!pathaTx.valid || pathaTx.request.header.requestId != request_id ||
        pathaTx.state == PathAMvmTransaction::State::Cancelled)
        return;
    double acc = 0.0;
    for (int c = 0; c < 5; ++c)
        acc += pathaTx.matrix[row][c] * pathaTx.vector[c];
    pathaTx.result[row] = acc;
    pathaTx.dotpDone[row] = true;
    pathaTx.dotpCompleted++;
    pathaTx.lastDotpCompleteTick = curTick();
    report.eventPathaDotpCompleted++;
    markProgress(request_id);
    PathASlot &slot = pathaSlots[pathaTx.slot];
    slot.result[row] = acc;
    slot.readyMask |= static_cast<uint8_t>(1u << row);
    trace("dotp_complete", "patha_dotp", request_id, true, true,
          false, false, true, true, row, "", state);
    if (slot.readyMask == 0x1f) {
        slot.state = PathASlotState::RowsReady;
        tryIssuePathAPack(pathaTx);
    }
}

void
CfdLusgsEventController::tryIssuePathAPack(PathAMvmTransaction &tx)
{
    if (!tx.valid || tx.packIssued)
        return;
    PathASlot &slot = pathaSlots[tx.slot];
    if (slot.readyMask != 0x1f) {
        report.eventPathaPackStalls++;
        return;
    }
    report.eventPathaPackRequests++;
    uint64_t busy = 0;
    uint64_t retries = 0;
    const Tick complete_tick = reservePathAUnit(
        pathaPackNextFree, env.pathaPackLatency, busy, retries);
    report.eventPathaPackBusyCycles += busy;
    report.eventPathaPackRetries += retries;
    report.eventPathaPackAccepted++;
    tx.packIssued = true;
    tx.packIssueTick = complete_tick - env.pathaPackLatency *
        env.ticksPerCycle;
    tx.state = PathAMvmTransaction::State::Packing;
    slot.state = PathASlotState::PackInFlight;
    trace("pack_issue", "patha_pack", tx.request.header.requestId,
          true, true, true, retries != 0, false, true, tx.slot, "",
          state);
    allocatePathASubEvent(
        complete_tick,
        [this, request_id = tx.request.header.requestId] {
            onPathAPackComplete(request_id);
        },
        "cfd_lusgs_patha.pack_complete");
}

void
CfdLusgsEventController::onPathAPackComplete(uint64_t request_id)
{
    report.eventEventsProcessed++;
    if (!pathaTx.valid || pathaTx.request.header.requestId != request_id ||
        pathaTx.state == PathAMvmTransaction::State::Cancelled)
        return;
    PathASlot &slot = pathaSlots[pathaTx.slot];
    if (slot.readyMask != 0x1f) {
        report.eventPathaEarlyConsumeErrors++;
        pendingCompletionError = CfdLusgsStatus::InternalStateError;
        scheduleTick();
        return;
    }
    pathaTx.packCompleted = true;
    pathaTx.packCompleteTick = curTick();
    pathaTx.state = PathAMvmTransaction::State::WaitingResult;
    report.eventPathaPackCompleted++;
    markProgress(request_id);
    trace("pack_complete", "patha_pack", request_id, true, true,
          false, false, true, true, pathaTx.slot, "", state);
    allocatePathASubEvent(
        curTick() + env.pathaResultLatency * env.ticksPerCycle,
        [this, request_id] { onPathAResultReady(request_id); },
        "cfd_lusgs_patha.result_ready");
}

void
CfdLusgsEventController::recordPathAMvmLatency(
    const PathAMvmTransaction &tx)
{
    const uint64_t latency = ticksToCycles(tx.completionTick - tx.issueTick);
    pathaMvmLatencyTotal += latency;
    pathaMvmLatencyCount++;
    if (pathaMvmLatencyMin == 0 || latency < pathaMvmLatencyMin)
        pathaMvmLatencyMin = latency;
    pathaMvmLatencyMax = std::max(pathaMvmLatencyMax, latency);
    report.eventPathaAverageMvmLatency =
        pathaMvmLatencyCount ?
        static_cast<double>(pathaMvmLatencyTotal) / pathaMvmLatencyCount :
        0.0;
    report.eventPathaMinMvmLatency = pathaMvmLatencyMin;
    report.eventPathaMaxMvmLatency = pathaMvmLatencyMax;
    report.eventPathaMvmWaitCycles += latency;
    report.eventPathaWaitCycles += latency;
    report.pathaWaitCycles += latency;
    report.eventPathaLoadPhaseCycles +=
        ticksToCycles(tx.lastLoadCompleteTick - tx.firstLoadIssueTick);
    report.eventPathaDotpPhaseCycles +=
        ticksToCycles(tx.lastDotpCompleteTick - tx.firstDotpIssueTick);
    report.eventPathaPackPhaseCycles +=
        ticksToCycles(tx.packCompleteTick - tx.packIssueTick);
    report.eventPathaResultPhaseCycles +=
        ticksToCycles(tx.resultReadyTick - tx.packCompleteTick);
}

void
CfdLusgsEventController::onPathAResultReady(uint64_t request_id)
{
    report.eventEventsProcessed++;
    if (!pathaTx.valid || pathaTx.request.header.requestId != request_id ||
        pathaTx.state == PathAMvmTransaction::State::Cancelled)
        return;
    PathASlot &slot = pathaSlots[pathaTx.slot];
    if (slot.readyMask != 0x1f || !pathaTx.packCompleted) {
        report.eventPathaEarlyConsumeErrors++;
        pendingCompletionError = CfdLusgsStatus::InternalStateError;
        scheduleTick();
        return;
    }
    slot.state = PathASlotState::ResultReady;
    pathaTx.resultReady = true;
    pathaTx.resultReadyTick = curTick();
    pathaTx.completionTick = curTick();
    pathaTx.state = PathAMvmTransaction::State::Completed;
    pathaResponse = {};
    pathaResponse.header = pathaTx.request.header;
    pathaResponse.slot = pathaTx.slot;
    pathaResponse.result = pathaTx.result;
    pathaResponse.firstLoadIssueTick = pathaTx.firstLoadIssueTick;
    pathaResponse.lastLoadCompleteTick = pathaTx.lastLoadCompleteTick;
    pathaResponse.firstDotpIssueTick = pathaTx.firstDotpIssueTick;
    pathaResponse.lastDotpCompleteTick = pathaTx.lastDotpCompleteTick;
    pathaResponse.packIssueTick = pathaTx.packIssueTick;
    pathaResponse.completionTick = curTick();
    pathaResponseValid = true;
    report.eventPathaResultTransfers++;
    report.eventPathaResultBytes += VectorBytes;
    report.eventPathaCompleted++;
    report.eventPathaMvmCompleted++;
    markProgress(request_id);
    report.pathaArbiterOwnershipCycles +=
        ticksToCycles(pathaTx.completionTick - pathaTx.issueTick);
    recordPathAMvmLatency(pathaTx);
    trace("result_ready", "patha_result", request_id, true, true,
          false, false, true, true, pathaTx.slot, "", state);
    trace("patha_mvm_complete", "patha_mvm", request_id, true, true,
          false, false, true, true, pathaTx.slot, "", state);
    slot = {};
    report.eventPathaSlotFree++;
    processRequestComplete(request_id);
}

void
CfdLusgsEventController::resetTrsv5Engine()
{
    EventQueue *q = eventq();
    for (auto &event : trsvEvents) {
        if (event && event->scheduled())
            q->deschedule(event.get());
    }
    trsvEvents.clear();
    trsvTransactions.clear();
    trsvResponses.clear();
    trsvQueue.clear();
    trsvDividerNextFree.clear();
    trsvFmaNextFree.clear();
    trsvActive = 0;
    trsvLatencyTotal = 0;
    trsvLatencyCount = 0;
    trsvLatencyMin = 0;
    trsvLatencyMax = 0;
}

bool
CfdLusgsEventController::canAcceptTrsv5() const
{
    return trsvTransactions.size() < env.trsvQueueDepth;
}

EventFunctionWrapper *
CfdLusgsEventController::allocateTrsv5Event(
    uint64_t request_id, Tick when, std::function<void()> fn,
    const char *name)
{
    auto event = std::make_unique<EventFunctionWrapper>(std::move(fn), name);
    EventFunctionWrapper *ptr = event.get();
    trsvEvents.push_back(std::move(event));
    auto it = trsvTransactions.find(request_id);
    if (it != trsvTransactions.end())
        it->second->events.push_back(ptr);
    eventq()->schedule(ptr, std::max<Tick>(when, curTick() + 1));
    return ptr;
}

bool
CfdLusgsEventController::trySubmitTrsv5(const Trsv5EventRequest &req)
{
    report.eventTrsvRequests++;
    report.trsv5Requests++;
    if (!canAcceptTrsv5()) {
        report.eventTrsvRetries++;
        report.eventTrsvQueueFullStalls++;
        trace("trsv_queue", "trsv5", req.header.requestId, true, false,
              false, true, false, true, trsvQueue.size(), "queue_full",
              state, "ok", false, false, "queue");
        return false;
    }

    auto tx = std::make_unique<Trsv5Transaction>();
    tx->request = req;
    tx->request.inputCaptured = false;
    tx->state = Trsv5State::WaitingInput;
    tx->issueTick = curTick();
    const uint64_t request_id = req.header.requestId;
    trsvTransactions.emplace(request_id, std::move(tx));
    trsvQueue.push_back(request_id);
    report.eventTrsvAccepted++;
    report.eventTrsvMaxQueueDepth = std::max<uint64_t>(
        report.eventTrsvMaxQueueDepth, trsvTransactions.size());
    trace("trsv_request", "trsv5", request_id, true, true, true, false,
          false, true, trsvQueue.size(), "", state, "ok", false, false,
          "submit");
    trace("trsv_queue", "trsv5", request_id, true, trsvActive <
          env.trsvEventCount, true, false, false, true, trsvQueue.size(),
          "queued", state, "ok", false, false, "queue");
    startQueuedTrsv5();
    return true;
}

void
CfdLusgsEventController::startQueuedTrsv5()
{
    while (trsvActive < env.trsvEventCount && !trsvQueue.empty()) {
        const uint64_t request_id = trsvQueue.front();
        trsvQueue.pop_front();
        auto it = trsvTransactions.find(request_id);
        if (it == trsvTransactions.end() ||
            it->second->state != Trsv5State::WaitingInput)
            continue;
        startTrsv5(request_id);
    }
}

void
CfdLusgsEventController::startTrsv5(uint64_t request_id)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    tx.active = true;
    tx.state = Trsv5State::LoadingInput;
    trsvActive++;
    trace("trsv_load", "trsv5_load", request_id, true, true, true,
          false, false, true, trsvQueue.size(), "", state, "ok", false,
          false, "load");
    allocateTrsv5Event(
        request_id, curTick() + env.trsvLoadLatency * env.ticksPerCycle,
        [this, request_id] { onTrsv5LoadComplete(request_id); },
        "cfd_lusgs_trsv5.load_complete");
}

void
CfdLusgsEventController::onTrsv5LoadComplete(uint64_t request_id)
{
    report.eventEventsProcessed++;
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (tx.state == Trsv5State::Cancelled)
        return;
    tx.lu = tx.request.lu;
    tx.value = tx.request.rhs;
    tx.request.inputCaptured = true;
    tx.loadCompleteTick = curTick();
    tx.forwardStartTick = curTick();
    tx.k = 0;
    tx.state = Trsv5State::ForwardDivide;
    markProgress(request_id);
    trace("trsv_load", "trsv5_load", request_id, true, true, false,
          false, true, true, trsvQueue.size(), "", state, "ok", false,
          false, "load_complete");
    driveTrsv5(request_id);
}

void
CfdLusgsEventController::scheduleTrsv5Drive(
    uint64_t request_id, uint64_t cycles, const char *reason)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end() || it->second->resourceWakeScheduled)
        return;
    Trsv5Transaction &tx = *it->second;
    tx.resourceWakeScheduled = true;
    report.eventTrsvDependencyWaitCycles += cycles;
    trace("trsv_queue", "trsv5_dependency", request_id, true, false,
          false, true, false, false, trsvQueue.size(), reason, state, "ok",
          false, false, "dependency");
    allocateTrsv5Event(
        request_id, curTick() + std::max<uint64_t>(1, cycles) *
        env.ticksPerCycle,
        [this, request_id] {
            auto it = trsvTransactions.find(request_id);
            if (it == trsvTransactions.end())
                return;
            it->second->resourceWakeScheduled = false;
            driveTrsv5(request_id);
        }, "cfd_lusgs_trsv5.resource_ready");
}

void
CfdLusgsEventController::driveTrsv5(uint64_t request_id)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    switch (it->second->state) {
      case Trsv5State::ForwardDivide:
      case Trsv5State::LastDivide:
        tryIssueTrsv5Divide(request_id);
        break;
      case Trsv5State::ForwardUpdate:
      case Trsv5State::BackwardUpdate:
        tryIssueTrsv5Fmas(request_id);
        break;
      default:
        break;
    }
}

void
CfdLusgsEventController::tryIssueTrsv5Divide(uint64_t request_id)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (trsvDividerNextFree.size() != env.trsvDivCount)
        trsvDividerNextFree.assign(env.trsvDivCount, 0);

    int unit = -1;
    Tick earliest = std::numeric_limits<Tick>::max();
    for (size_t idx = 0; idx < trsvDividerNextFree.size(); ++idx) {
        if (trsvDividerNextFree[idx] <= curTick()) {
            unit = idx;
            break;
        }
        earliest = std::min(earliest, trsvDividerNextFree[idx]);
    }
    if (unit < 0) {
        const uint64_t wait = std::max<uint64_t>(
            1, ticksToCycles(earliest - curTick()));
        report.eventTrsvDivBusyCycles += wait;
        if (!tx.resourceWakeScheduled) {
            tx.resourceWakeScheduled = true;
            trace("trsv_div_issue", "trsv5_divider", request_id, true,
                  false, false, true, false, true, trsvQueue.size(),
                  "divider_busy", state, "ok", false, false, "divide",
                  tx.k, tx.k, -1);
            allocateTrsv5Event(
                request_id, earliest,
                [this, request_id] {
                    auto it = trsvTransactions.find(request_id);
                    if (it == trsvTransactions.end())
                        return;
                    it->second->resourceWakeScheduled = false;
                    driveTrsv5(request_id);
                }, "cfd_lusgs_trsv5.divider_ready");
        }
        return;
    }

    const int k = tx.k;
    trsvDividerNextFree[unit] = curTick() + env.trsvDivLatency *
        env.ticksPerCycle;
    report.eventTrsvDivRequests++;
    trace("trsv_div_issue", "trsv5_divider", request_id, true, true,
          true, false, false, true, trsvQueue.size(), "", state, "ok",
          false, false, "divide", k, k, -1);
    if (env.trsvTestHang && !trsvHangInjected) {
        trsvHangInjected = true;
        trace("trsv_div_issue", "trsv5_divider", request_id, true, true,
              true, false, false, true, trsvQueue.size(),
              "test_completion_dropped", state, "ok", false, false,
              "divide_hang", k, k, -1);
        if (env.watchdogCycles)
            scheduleTick(env.watchdogCycles + 1);
        return;
    }
    allocateTrsv5Event(
        request_id, trsvDividerNextFree[unit],
        [this, request_id, k] { onTrsv5DivideComplete(request_id, k); },
        "cfd_lusgs_trsv5.divide_complete");
}

void
CfdLusgsEventController::onTrsv5DivideComplete(uint64_t request_id, int k)
{
    report.eventEventsProcessed++;
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (tx.state == Trsv5State::Cancelled || tx.k != k)
        return;
    tx.value[k] /= tx.lu[k * 5 + k];
    report.eventTrsvDivCompleted++;
    markProgress(request_id);
    trace("trsv_div_complete", "trsv5_divider", request_id, true, true,
          false, false, true, true, trsvQueue.size(), "", state, "ok",
          false, false, "divide", k, k, -1);

    if (k < 4) {
        tx.state = Trsv5State::ForwardUpdate;
        tx.nextI = k + 1;
        tx.stageFmasIssued = 0;
        tx.stageFmasCompleted = 0;
        tx.stageFmasExpected = 4 - k;
        tx.fmasInFlight = 0;
    } else {
        tx.forwardCompleteTick = curTick();
        report.eventTrsvForwardCycles += ticksToCycles(
            tx.forwardCompleteTick - tx.forwardStartTick);
        trace("trsv_forward_stage_complete", "trsv5", request_id, true,
              true, false, false, true, true, trsvQueue.size(), "", state,
              "ok", false, false, "forward", k, -1, -1);
        tx.backwardStartTick = curTick();
        tx.k = 3;
        tx.nextI = 4;
        tx.stageFmasIssued = 0;
        tx.stageFmasCompleted = 0;
        tx.stageFmasExpected = 1;
        tx.fmasInFlight = 0;
        tx.state = Trsv5State::BackwardUpdate;
    }

    if (env.trsvForwarding) {
        report.eventTrsvForwardedResults++;
        driveTrsv5(request_id);
    } else {
        scheduleTrsv5Drive(request_id, 1, "forwarding_disabled");
    }
}

void
CfdLusgsEventController::tryIssueTrsv5Fmas(uint64_t request_id)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (trsvFmaNextFree.size() != env.trsvFmaCount)
        trsvFmaNextFree.assign(env.trsvFmaCount, 0);
    const bool backward = tx.state == Trsv5State::BackwardUpdate;

    while (tx.nextI < 5 && (!backward || tx.fmasInFlight == 0)) {
        int unit = -1;
        Tick earliest = std::numeric_limits<Tick>::max();
        for (size_t idx = 0; idx < trsvFmaNextFree.size(); ++idx) {
            if (trsvFmaNextFree[idx] <= curTick()) {
                unit = idx;
                break;
            }
            earliest = std::min(earliest, trsvFmaNextFree[idx]);
        }
        if (unit < 0) {
            const uint64_t wait = std::max<uint64_t>(
                1, ticksToCycles(earliest - curTick()));
            report.eventTrsvFmaBusyCycles += wait;
            if (!tx.resourceWakeScheduled) {
                tx.resourceWakeScheduled = true;
                trace("trsv_fma_issue", "trsv5_fma", request_id, true,
                      false, false, true, false, true, trsvQueue.size(),
                      "fma_busy", state, "ok", false, false,
                      backward ? "backward" : "forward", tx.k, tx.nextI,
                      -1);
                allocateTrsv5Event(
                    request_id, earliest,
                    [this, request_id] {
                        auto it = trsvTransactions.find(request_id);
                        if (it == trsvTransactions.end())
                            return;
                        it->second->resourceWakeScheduled = false;
                        driveTrsv5(request_id);
                    }, "cfd_lusgs_trsv5.fma_ready");
            }
            return;
        }

        const int k = tx.k;
        const int i = tx.nextI++;
        tx.stageFmasIssued++;
        tx.fmasInFlight++;
        trsvFmaNextFree[unit] = curTick() + env.trsvFmaLatency *
            env.ticksPerCycle;
        report.eventTrsvFmaRequests++;
        trace("trsv_fma_issue", "trsv5_fma", request_id, true, true,
              true, false, false, true, trsvQueue.size(), "", state, "ok",
              false, false, backward ? "backward" : "forward", k, i, -1);
        allocateTrsv5Event(
            request_id, trsvFmaNextFree[unit],
            [this, request_id, k, i, backward] {
                onTrsv5FmaComplete(request_id, k, i, backward);
            }, "cfd_lusgs_trsv5.fma_complete");
        if (backward)
            return;
    }
}

void
CfdLusgsEventController::onTrsv5FmaComplete(
    uint64_t request_id, int k, int i, bool backward)
{
    report.eventEventsProcessed++;
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (tx.state == Trsv5State::Cancelled || tx.k != k)
        return;
    if (backward) {
        tx.value[k] = cfdTrsv5MulSub(
            tx.value[k], tx.lu[k * 5 + i], tx.value[i]);
    } else {
        tx.value[i] = cfdTrsv5MulSub(
            tx.value[i], tx.lu[i * 5 + k], tx.value[k]);
    }
    tx.fmasInFlight--;
    tx.stageFmasCompleted++;
    report.eventTrsvFmaCompleted++;
    markProgress(request_id);
    trace("trsv_fma_complete", "trsv5_fma", request_id, true, true,
          false, false, true, true, trsvQueue.size(), "", state, "ok",
          false, false, backward ? "backward" : "forward", k, i, -1);

    if (tx.stageFmasCompleted == tx.stageFmasExpected) {
        if (!backward) {
            trace("trsv_forward_stage_complete", "trsv5", request_id,
                  true, true, false, false, true, true, trsvQueue.size(),
                  "", state, "ok", false, false, "forward", k, -1, -1);
            tx.k++;
            tx.state = tx.k == 4 ? Trsv5State::LastDivide :
                                   Trsv5State::ForwardDivide;
        } else {
            trace("trsv_backward_stage_complete", "trsv5", request_id,
                  true, true, false, false, true, true, trsvQueue.size(),
                  "", state, "ok", false, false, "backward", k, -1, -1);
            if (k == 0) {
                tx.backwardCompleteTick = curTick();
                report.eventTrsvBackwardCycles += ticksToCycles(
                    tx.backwardCompleteTick - tx.backwardStartTick);
                tx.state = Trsv5State::ResultReady;
                allocateTrsv5Event(
                    request_id, curTick() + env.trsvResultLatency *
                    env.ticksPerCycle,
                    [this, request_id] { onTrsv5ResultReady(request_id); },
                    "cfd_lusgs_trsv5.result_ready");
                return;
            }
            tx.k--;
            tx.nextI = tx.k + 1;
            tx.stageFmasIssued = 0;
            tx.stageFmasCompleted = 0;
            tx.stageFmasExpected = 4 - tx.k;
            tx.fmasInFlight = 0;
        }
        if (env.trsvForwarding) {
            report.eventTrsvForwardedResults++;
            driveTrsv5(request_id);
        } else {
            scheduleTrsv5Drive(request_id, 1, "forwarding_disabled");
        }
        return;
    }
    tryIssueTrsv5Fmas(request_id);
}

void
CfdLusgsEventController::recordTrsv5Latency(const Trsv5Transaction &tx)
{
    const uint64_t latency = ticksToCycles(tx.completionTick - tx.issueTick);
    trsvLatencyTotal += latency;
    trsvLatencyCount++;
    if (trsvLatencyMin == 0 || latency < trsvLatencyMin)
        trsvLatencyMin = latency;
    trsvLatencyMax = std::max(trsvLatencyMax, latency);
    report.eventTrsvAverageLatency = static_cast<double>(trsvLatencyTotal) /
        trsvLatencyCount;
    report.eventTrsvMinLatency = trsvLatencyMin;
    report.eventTrsvMaxLatency = trsvLatencyMax;
    report.eventTrsvWaitCycles += latency;
    report.eventTrsvBusyCycles += latency;
    report.trsv5WaitCycles += latency;
}

void
CfdLusgsEventController::onTrsv5ResultReady(uint64_t request_id)
{
    report.eventEventsProcessed++;
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end())
        return;
    Trsv5Transaction &tx = *it->second;
    if (tx.state == Trsv5State::Cancelled)
        return;
    tx.completionTick = curTick();
    tx.state = Trsv5State::Completed;
    tx.active = false;
    const bool standalone = tx.request.standalone;
    Trsv5EventResponse response;
    response.header = tx.request.header;
    response.result = tx.value;
    response.issueTick = tx.issueTick;
    response.loadCompleteTick = tx.loadCompleteTick;
    response.forwardCompleteTick = tx.forwardCompleteTick;
    response.backwardCompleteTick = tx.backwardCompleteTick;
    response.completionTick = tx.completionTick;
    if (!standalone)
        trsvResponses[request_id] = response;
    report.eventTrsvCompleted++;
    recordTrsv5Latency(tx);
    if (trsvActive)
        trsvActive--;
    markProgress(request_id);
    trace("trsv_result_ready", "trsv5", request_id, true, true, false,
          false, true, true, trsvQueue.size(), "", state, "ok", false,
          false, "result");
    if (standalone) {
        trsvTransactions.erase(request_id);
        startQueuedTrsv5();
        scheduleTick();
        return;
    }
    startQueuedTrsv5();
    if (env.trsvTestStaleGeneration &&
        !trsvStaleGenerationInjected) {
        trsvStaleGenerationInjected = true;
        injectTrsv5ValidationCompletion(request_id, true);
    }
    if (env.trsvTestBadRequestId && !trsvBadRequestIdInjected) {
        trsvBadRequestIdInjected = true;
        injectTrsv5ValidationCompletion(request_id, false);
    }
    processRequestComplete(request_id);
}

void
CfdLusgsEventController::injectTrsv5ValidationCompletion(
    uint64_t request_id, bool stale_generation)
{
    auto source = pendingRequests.find(request_id);
    if (source == pendingRequests.end())
        return;

    PendingLusgsRequest injected;
    injected.header = source->second->header;
    injected.header.requestId = nextRequestId++;
    if (stale_generation) {
        injected.header.taskGeneration = taskGeneration > 0 ?
            taskGeneration - 1 : taskGeneration + 1;
    }
    injected.issueTick = curTick();
    injected.completionTick = curTick();
    injected.accepted = true;
    const uint64_t injected_id = injected.header.requestId;
    pendingRequests.emplace(
        injected_id,
        std::make_unique<PendingLusgsRequest>(std::move(injected)));
    report.eventPendingRequestAlloc++;
    report.eventMaxPendingRequests = std::max<uint64_t>(
        report.eventMaxPendingRequests, pendingRequests.size());
    trace("test_completion_inject", "trsv5", injected_id, true, true,
          true, false, false, true, pendingRequests.size(),
          stale_generation ? "stale_generation" : "bad_request_id",
          state);
    processRequestComplete(injected_id);
}

bool
CfdLusgsEventController::takeTrsv5Response(
    uint64_t request_id, Trsv5EventResponse &response)
{
    auto response_it = trsvResponses.find(request_id);
    auto tx_it = trsvTransactions.find(request_id);
    if (response_it == trsvResponses.end() || tx_it == trsvTransactions.end())
        return false;
    response = response_it->second;
    trsvResponses.erase(response_it);
    trsvTransactions.erase(tx_it);
    return true;
}

void
CfdLusgsEventController::cancelTrsv5(
    uint64_t request_id, uint64_t generation)
{
    auto it = trsvTransactions.find(request_id);
    if (it == trsvTransactions.end() ||
        it->second->request.header.taskGeneration != generation)
        return;
    Trsv5Transaction &tx = *it->second;
    tx.state = Trsv5State::Cancelled;
    for (auto *event : tx.events) {
        if (event && event->scheduled())
            eventq()->deschedule(event);
    }
    if (tx.active && trsvActive)
        trsvActive--;
    trsvQueue.erase(std::remove(trsvQueue.begin(), trsvQueue.end(), request_id),
                    trsvQueue.end());
    trsvResponses.erase(request_id);
    trsvTransactions.erase(it);
    startQueuedTrsv5();
}

void
CfdLusgsEventController::resetVec5Engine()
{
    EventQueue *q = eventq();
    for (auto &event : vec5Events) {
        if (event && event->scheduled())
            q->deschedule(event.get());
    }
    vec5Events.clear();
    vec5Transactions.clear();
    vec5Responses.clear();
    vec5Queue.clear();
    vec5Active = 0;
    vec5LatencyTotal = 0;
    vec5LatencyCount = 0;
    vec5LaneCapacityCycles = 0;
}

bool
CfdLusgsEventController::canAcceptVec5() const
{
    return vec5Transactions.size() < env.vec5QueueDepth;
}

uint64_t
CfdLusgsEventController::vec5OperationLatency(
    CfdLusgsVec5Op operation) const
{
    switch (operation) {
      case CfdLusgsVec5Op::Copy: return env.vec5CopyLatency;
      case CfdLusgsVec5Op::Sub: return env.vec5SubLatency;
      case CfdLusgsVec5Op::Axpy: return env.vec5AxpyLatency;
    }
    return env.vec5SubLatency;
}

EventFunctionWrapper *
CfdLusgsEventController::allocateVec5Event(
    uint64_t request_id, Tick when, std::function<void()> fn,
    const char *name)
{
    auto event = std::make_unique<EventFunctionWrapper>(std::move(fn), name);
    EventFunctionWrapper *ptr = event.get();
    vec5Events.push_back(std::move(event));
    auto it = vec5Transactions.find(request_id);
    if (it != vec5Transactions.end())
        it->second->events.push_back(ptr);
    eventq()->schedule(ptr, std::max<Tick>(when, curTick() + 1));
    return ptr;
}

bool
CfdLusgsEventController::trySubmitVec5(const Vec5EventRequest &req)
{
    report.eventVec5Requests++;
    if (!canAcceptVec5()) {
        report.eventVec5Retries++;
        report.eventVec5QueueFullStalls++;
        trace("vec5_queue", "vec5", req.header.requestId, true, false,
              false, true, false, true, vec5Queue.size(), "queue_full",
              state, "ok", false, false, vec5OperationName(req.operation));
        return false;
    }
    auto tx = std::make_unique<Vec5Transaction>();
    tx->request = req;
    tx->state = Vec5Transaction::State::Queued;
    tx->issueTick = curTick();
    const uint64_t request_id = req.header.requestId;
    vec5Transactions.emplace(request_id, std::move(tx));
    vec5Queue.push_back(request_id);
    report.eventVec5Accepted++;
    switch (req.operation) {
      case CfdLusgsVec5Op::Copy:
        report.eventVec5CopyRequests++;
        report.eventVec5CopyAccepted++;
        break;
      case CfdLusgsVec5Op::Sub:
        report.eventVec5SubRequests++;
        report.eventVec5SubAccepted++;
        break;
      case CfdLusgsVec5Op::Axpy:
        report.eventVec5AxpyRequests++;
        report.eventVec5AxpyAccepted++;
        break;
    }
    report.eventVec5MaxQueueDepth = std::max<uint64_t>(
        report.eventVec5MaxQueueDepth, vec5Transactions.size());
    trace("vec5_request", "vec5", request_id, true, true, true, false,
          false, true, vec5Queue.size(), "", state, "ok", false, false,
          vec5OperationName(req.operation));
    trace("vec5_queue", "vec5", request_id, true,
          vec5Active < env.vec5Count, true, false, false, true,
          vec5Queue.size(), "queued", state, "ok", false, false,
          vec5OperationName(req.operation));
    startQueuedVec5();
    return true;
}

void
CfdLusgsEventController::startQueuedVec5()
{
    while (vec5Active < env.vec5Count && !vec5Queue.empty()) {
        const uint64_t request_id = vec5Queue.front();
        vec5Queue.pop_front();
        auto it = vec5Transactions.find(request_id);
        if (it == vec5Transactions.end() ||
            it->second->state != Vec5Transaction::State::Queued)
            continue;
        startVec5(request_id);
    }
}

void
CfdLusgsEventController::startVec5(uint64_t request_id)
{
    auto it = vec5Transactions.find(request_id);
    if (it == vec5Transactions.end())
        return;
    Vec5Transaction &tx = *it->second;
    tx.state = Vec5Transaction::State::Executing;
    tx.active = true;
    vec5Active++;
    issueVec5LaneGroup(request_id);
}

void
CfdLusgsEventController::issueVec5LaneGroup(uint64_t request_id)
{
    auto it = vec5Transactions.find(request_id);
    if (it == vec5Transactions.end())
        return;
    Vec5Transaction &tx = *it->second;
    if (tx.state != Vec5Transaction::State::Executing || tx.nextLane >= 5)
        return;
    const uint32_t end = std::min<uint32_t>(5, tx.nextLane + env.vec5Lanes);
    const uint64_t latency = vec5OperationLatency(tx.request.operation);
    for (uint32_t lane = tx.nextLane; lane < end; ++lane) {
        tx.lanesIssued++;
        tx.lanesInFlight++;
        trace("vec5_lane_issue", "vec5_lane", request_id, true, true,
              true, false, false, true, vec5Queue.size(), "", state, "ok",
              false, false, vec5OperationName(tx.request.operation), -1, -1,
              lane);
        allocateVec5Event(
            request_id, curTick() + latency * env.ticksPerCycle,
            [this, request_id, lane] {
                onVec5LaneComplete(request_id, lane);
            }, "cfd_lusgs_vec5.lane_complete");
    }
    tx.nextLane = end;
    if (tx.nextLane < 5) {
        allocateVec5Event(
            request_id, curTick() + env.vec5InitiationInterval *
            env.ticksPerCycle,
            [this, request_id] { issueVec5LaneGroup(request_id); },
            "cfd_lusgs_vec5.group_issue");
    }
}

void
CfdLusgsEventController::recordVec5Latency(const Vec5Transaction &tx)
{
    const uint64_t latency = ticksToCycles(tx.completionTick - tx.issueTick);
    vec5LatencyTotal += latency;
    vec5LatencyCount++;
    vec5LaneCapacityCycles += env.vec5Lanes * std::max<uint64_t>(1, latency);
    report.eventVec5AverageLatency = static_cast<double>(vec5LatencyTotal) /
        vec5LatencyCount;
    report.eventVec5LaneUtilization = vec5LaneCapacityCycles ?
        static_cast<double>(report.eventVec5LaneOperations) /
        vec5LaneCapacityCycles : 0.0;
    report.eventVec5BusyCycles += latency;
    report.eventVec5WaitCycles += latency;
    report.vec5WaitCycles += latency;
    switch (tx.request.operation) {
      case CfdLusgsVec5Op::Copy:
        report.eventVec5CopyWaitCycles += latency;
        break;
      case CfdLusgsVec5Op::Sub:
        report.eventVec5SubWaitCycles += latency;
        break;
      case CfdLusgsVec5Op::Axpy:
        report.eventVec5AxpyWaitCycles += latency;
        break;
    }
}

void
CfdLusgsEventController::onVec5LaneComplete(
    uint64_t request_id, uint32_t lane)
{
    report.eventEventsProcessed++;
    auto it = vec5Transactions.find(request_id);
    if (it == vec5Transactions.end())
        return;
    Vec5Transaction &tx = *it->second;
    if (tx.state == Vec5Transaction::State::Cancelled || lane >= 5)
        return;
    switch (tx.request.operation) {
      case CfdLusgsVec5Op::Copy:
        tx.result[lane] = tx.request.input0[lane];
        break;
      case CfdLusgsVec5Op::Sub:
        tx.result[lane] = tx.request.input0[lane] - tx.request.input1[lane];
        break;
      case CfdLusgsVec5Op::Axpy:
        tx.result[lane] = tx.request.input0[lane] +
            tx.request.scalar * tx.request.input1[lane];
        break;
    }
    tx.lanesInFlight--;
    tx.lanesCompleted++;
    report.eventVec5LaneOperations++;
    markProgress(request_id);
    trace("vec5_lane_complete", "vec5_lane", request_id, true, true,
          false, false, true, true, vec5Queue.size(), "", state, "ok",
          false, false, vec5OperationName(tx.request.operation), -1, -1,
          lane);
    if (tx.lanesCompleted != 5)
        return;

    tx.completionTick = curTick();
    tx.state = Vec5Transaction::State::Completed;
    tx.active = false;
    const bool standalone = tx.request.standalone;
    Vec5EventResponse response;
    response.header = tx.request.header;
    response.operation = tx.request.operation;
    response.result = tx.result;
    response.issueTick = tx.issueTick;
    response.completionTick = tx.completionTick;
    if (!standalone)
        vec5Responses[request_id] = response;
    report.eventVec5Completed++;
    switch (tx.request.operation) {
      case CfdLusgsVec5Op::Copy:
        report.eventVec5CopyCompleted++;
        break;
      case CfdLusgsVec5Op::Sub:
        report.eventVec5SubCompleted++;
        break;
      case CfdLusgsVec5Op::Axpy:
        report.eventVec5AxpyCompleted++;
        break;
    }
    recordVec5Latency(tx);
    if (vec5Active)
        vec5Active--;
    trace("vec5_result_ready", "vec5", request_id, true, true, false,
          false, true, true, vec5Queue.size(), "", state, "ok", false,
          false, vec5OperationName(tx.request.operation));
    if (standalone) {
        vec5Transactions.erase(request_id);
        startQueuedVec5();
        scheduleTick();
        return;
    }
    startQueuedVec5();
    processRequestComplete(request_id);
    if (env.vec5TestDuplicateCompletion &&
        !vec5DuplicateCompletionInjected) {
        vec5DuplicateCompletionInjected = true;
        trace("test_completion_inject", "vec5", request_id, true, true,
              true, false, false, true, vec5Queue.size(),
              "duplicate_completion", state);
        processRequestComplete(request_id);
    }
}

bool
CfdLusgsEventController::takeVec5Response(
    uint64_t request_id, Vec5EventResponse &response)
{
    auto response_it = vec5Responses.find(request_id);
    auto tx_it = vec5Transactions.find(request_id);
    if (response_it == vec5Responses.end() || tx_it == vec5Transactions.end())
        return false;
    response = response_it->second;
    vec5Responses.erase(response_it);
    vec5Transactions.erase(tx_it);
    return true;
}

void
CfdLusgsEventController::cancelVec5(
    uint64_t request_id, uint64_t generation)
{
    auto it = vec5Transactions.find(request_id);
    if (it == vec5Transactions.end() ||
        it->second->request.header.taskGeneration != generation)
        return;
    Vec5Transaction &tx = *it->second;
    tx.state = Vec5Transaction::State::Cancelled;
    for (auto *event : tx.events) {
        if (event && event->scheduled())
            eventq()->deschedule(event);
    }
    if (tx.active && vec5Active)
        vec5Active--;
    vec5Queue.erase(std::remove(vec5Queue.begin(), vec5Queue.end(), request_id),
                    vec5Queue.end());
    vec5Responses.erase(request_id);
    vec5Transactions.erase(it);
    startQueuedVec5();
}

void
CfdLusgsEventController::processRequestComplete(uint64_t request_id)
{
    report.eventEventsProcessed++;
    auto it = pendingRequests.find(request_id);
    if (it == pendingRequests.end()) {
        report.eventUnexpectedCompletions++;
        report.eventStaleCompletions++;
        return;
    }

    PendingLusgsRequest &req = *it->second;
    const LusgsEventRequestHeader &h = req.header;
    bool stale = false;
    const char *validation = "ok";
    if (req.cancelled) {
        report.eventStaleCompletions++;
        stale = true;
        validation = "cancelled";
    } else if (req.completed) {
        report.eventDuplicateCompletions++;
        pendingCompletionError = CfdLusgsStatus::UnexpectedCompletion;
        validation = "duplicate";
    } else if (h.taskGeneration != taskGeneration || h.taskToken != token) {
        report.eventGenerationMismatches++;
        report.eventStaleCompletions++;
        stale = true;
        validation = "generation_mismatch";
    } else if (h.contextId >= contexts.size() || !contexts[h.contextId].valid) {
        report.eventUnexpectedCompletions++;
        pendingCompletionError = CfdLusgsStatus::UnexpectedCompletion;
        validation = "bad_context";
    } else if (contexts[h.contextId].waitingRequestId != h.requestId) {
        report.eventRequestIdMismatches++;
        pendingCompletionError = CfdLusgsStatus::UnexpectedCompletion;
        validation = "request_id_mismatch";
    } else if (contexts[h.contextId].waitingResource != h.resourceType ||
               !stateWaitsForResource(state, h.resourceType)) {
        report.eventWrongStateCompletions++;
        pendingCompletionError = CfdLusgsStatus::UnexpectedCompletion;
        validation = "wrong_state";
    }

    req.completed = true;
    trace("complete", resourceTypeName(h.resourceType), request_id, true,
          !stale, false, false, true, true, pendingRequests.size(), "",
          state, validation, stale, req.cancelled);
    if (stale) {
        deferredFreeRequests.push_back(request_id);
        scheduleTick();
        return;
    }
    markProgress(request_id);
    scheduleTick();
}

void
CfdLusgsEventController::recordReport()
{
    if (auto *local = getCfdLocalSpm())
        local->recordLusgsController(report);
}

void
CfdLusgsEventController::finishTask(CfdLusgsStatus final_status)
{
    cancelPendingRequests();
    if (final_status != CfdLusgsStatus::Complete) {
        resetTrsv5Engine();
        resetVec5Engine();
    }
    status = final_status;
    state = final_status == CfdLusgsStatus::Complete ?
        EventState::TaskComplete : EventState::TaskError;
    updateContextForState(state);
    taskComplete = true;
    completeTick = curTick();
    report.eventTaskCompleteTick = completeTick;
    setLifecycle(final_status == CfdLusgsStatus::Complete ?
                 LusgsTaskLifecycle::CompletedNotReaped :
                 LusgsTaskLifecycle::ErrorNotReaped);
    const uint64_t actual_cycles =
        env.ticksPerCycle ? (completeTick - startTick) / env.ticksPerCycle :
        (completeTick - startTick);
    report.busyCycles = actual_cycles;
    report.totalCycles = actual_cycles;
    report.activeContextCycles = actual_cycles;
    report.maxActiveContexts = std::max<uint64_t>(report.maxActiveContexts, 1);
    report.cyclesPerForwardCell =
        report.forwardCells ?
        static_cast<double>(actual_cycles) / report.forwardCells : 0;
    report.cyclesPerBackwardCell =
        report.backwardCells ?
        static_cast<double>(actual_cycles) / report.backwardCells : 0;
    const uint64_t full_cells =
        static_cast<uint64_t>(descriptorSnapshot.n_lines) *
        descriptorSnapshot.n_cells;
    report.cyclesPerFullCell =
        full_cells ? static_cast<double>(actual_cycles) / full_cells : 0;
    report.cellsPer1000Cycles =
        actual_cycles ? (1000.0 * full_cells) / actual_cycles : 0;
    report.pathaUtilization =
        actual_cycles ? static_cast<double>(report.pathaWaitCycles) /
        actual_cycles : 0;
    report.trsv5Utilization =
        actual_cycles ? static_cast<double>(report.trsv5WaitCycles) /
        actual_cycles : 0;
    report.vec5Utilization =
        actual_cycles ? static_cast<double>(report.vec5WaitCycles) /
        actual_cycles : 0;
    const double divider_capacity = static_cast<double>(actual_cycles) *
        env.trsvDivCount;
    const double fma_capacity = static_cast<double>(actual_cycles) *
        env.trsvFmaCount;
    report.eventTrsvDividerUtilization = divider_capacity ?
        static_cast<double>(report.eventTrsvDivRequests *
                            env.trsvDivLatency) / divider_capacity : 0.0;
    report.eventTrsvFmaUtilization = fma_capacity ?
        static_cast<double>(report.eventTrsvFmaRequests *
                            env.trsvFmaLatency) / fma_capacity : 0.0;
    if (final_status == CfdLusgsStatus::Complete) {
        report.completedTasks = 1;
        report.eventCompleted = 1;
    } else {
        report.failedTasks = 1;
        report.eventFailed = 1;
    }
    report.contextFree = report.contextAlloc;
    report.eventContextFree = report.eventContextAlloc;
    report.eventActualCycles = actual_cycles;
    report.eventActiveCycles = actual_cycles;
    report.eventMaxActiveContexts =
        std::max<uint64_t>(report.eventMaxActiveContexts, 1);
    report.eventContextReadyCycles = actual_cycles;
    const uint64_t spm_ops = report.eventSpmReads + report.eventSpmWrites;
    report.eventSpmAverageLatency =
        spm_ops ? static_cast<double>(report.spmWaitCycles +
                                      report.writebackWaitCycles) / spm_ops :
        0.0;
    trace("complete", "controller", 0, false, true, false, false,
          true, true, 1, "", state);
    markProgress();
    recordReport();
    if (traceFp) {
        std::fclose(traceFp);
        traceFp = nullptr;
    }
}

void
CfdLusgsEventController::clearTask()
{
    cancelPendingRequests();
    taskActive = false;
    taskComplete = false;
    tc = nullptr;
    descAddr = 0;
    descriptorSnapshot = {};
    descriptorSnapshotValid = false;
    status = CfdLusgsStatus::Complete;
    state = EventState::Idle;
    lifecycle = LusgsTaskLifecycle::Idle;
    token = 0;
    completedRequestId = 0;
    pendingCompletionError = CfdLusgsStatus::Complete;
    deferredFreeRequests.clear();
    contexts.clear();
    resetPathAEngine();
    resetTrsv5Engine();
    resetVec5Engine();
    startTick = 0;
    completeTick = 0;
    successfulWaitTick = 0;
    tokenReapTick = 0;
    lastLifecycleTick = 0;
    lastProgressTick = 0;
    lastProgressState = EventState::Idle;
    lastProgressRequestId = 0;
    watchdogHangInjected = false;
    trsvBackpressureInjected = false;
    vec5BackpressureInjected = false;
    dqstar.clear();
    dq.clear();
    if (traceFp) {
        std::fclose(traceFp);
        traceFp = nullptr;
    }
}

uint64_t
CfdLusgsEventController::launch(ExecContext *xc, Addr descriptor_addr)
{
    CfdLusgsControllerRecord launch_report;
    launch_report.launches = 1;
    launch_report.committedLaunches = 1;
    launch_report.eventLaunches = 1;
    launch_report.eventDecodedLaunches = 1;
    launch_report.eventCommittedLaunches = 1;
    if (taskActive || lifecycle != LusgsTaskLifecycle::Idle) {
        launch_report.failedTasks = 1;
        launch_report.eventFailed = 1;
        launch_report.arbiterStalls = 1;
        if (auto *local = getCfdLocalSpm())
            local->recordLusgsController(launch_report);
        return 0;
    }
    if (descriptor_addr == 0 || !aligned8(descriptor_addr)) {
        launch_report.failedTasks = 1;
        launch_report.eventFailed = 1;
        if (auto *local = getCfdLocalSpm())
            local->recordLusgsController(launch_report);
        return 0;
    }

    clearTask();
    env = readEventEnv();
    report = {};
    report.launches = 1;
    report.committedLaunches = 1;
    report.eventLaunches = 1;
    report.eventDecodedLaunches = 1;
    report.eventCommittedLaunches = 1;
    report.spmCapacity = LocalSpmCapacity;
    tc = xc->tcBase();
    descAddr = descriptor_addr;
    taskActive = true;
    taskComplete = false;
    token = nextToken++;
    taskGeneration++;
    startTick = curTick();
    report.eventLaunchCommitTick = startTick;
    completeTick = 0;
    state = EventState::CommandQueued;
    contexts.resize(1);
    updateContextForState(state);
    lastLifecycleTick = curTick();
    lifecycle = LusgsTaskLifecycle::Queued;
    markProgress();
    ensureTraceOpen();
    trace("launch", "command_queue", 0, true, true, true, false,
          false, true, 1, "", EventState::CommandQueued);
    scheduleTick();
    return token;
}

uint64_t
CfdLusgsEventController::wait(uint64_t wait_token)
{
    CfdLusgsControllerRecord wait_report;
    wait_report.eventWaitInstructions = 1;
    if (!taskActive || wait_token == 0 || wait_token != token)
    {
        wait_report.eventBadTokenWaits = 1;
        if (auto *local = getCfdLocalSpm())
            local->recordLusgsController(wait_report);
        return static_cast<uint64_t>(CfdLusgsStatus::BadToken);
    }
    if (!taskComplete) {
        wait_report.eventBusyPolls = 1;
        if (auto *local = getCfdLocalSpm())
            local->recordLusgsController(wait_report);
        return static_cast<uint64_t>(CfdLusgsStatus::Busy);
    }

    successfulWaitTick = curTick();
    tokenReapTick = curTick();
    wait_report.eventSuccessfulWaitTick = successfulWaitTick;
    wait_report.eventTokenReapTick = tokenReapTick;
    wait_report.eventTokensReaped = 1;
    wait_report.eventTokenReapCycles =
        completeTick ? ticksToCycles(curTick() - completeTick) : 0;
    if (status == CfdLusgsStatus::Complete) {
        wait_report.eventSuccessfulWaits = 1;
        wait_report.eventLifecycleCompletedNotReapedCycles =
            wait_report.eventTokenReapCycles;
    } else {
        wait_report.eventErrorWaits = 1;
        wait_report.eventLifecycleErrorNotReapedCycles =
            wait_report.eventTokenReapCycles;
    }
    if (auto *local = getCfdLocalSpm())
        local->recordLusgsController(wait_report);

    const uint64_t final_status = static_cast<uint64_t>(status);
    setLifecycle(LusgsTaskLifecycle::Idle);
    clearTask();
    return final_status;
}

void
CfdLusgsEventController::processTick()
{
    freeDeferredRequests();
    if (!taskActive || taskComplete)
        return;
    if (pendingCompletionError != CfdLusgsStatus::Complete) {
        finishTask(pendingCompletionError);
        return;
    }
    if (checkWatchdog())
        return;

    report.eventEventsProcessed++;
    report.eventSchedulerTicks++;

    switch (state) {
      case EventState::CommandQueued:
        setLifecycle(LusgsTaskLifecycle::Running);
        transition(EventState::ReadDescriptorReq, "dispatch",
                   "controller");
        return;

      case EventState::ReadDescriptorReq:
        report.spmReadRequests++;
        report.spmWaitCycles += env.spmReadLatency;
        report.eventSpmReads++;
        report.eventSpmReadBytes += sizeof(CfdLusgsDescriptor);
        issueRequest(LusgsResourceType::DescriptorRead, env.spmReadLatency,
                     EventState::ReadDescriptorWait);
        return;

      case EventState::ReadDescriptorWait:
        if (!consumeRequest(LusgsResourceType::DescriptorRead))
            return;
        if (context(0).waitingRequestId != 0)
            return;
        proxy().readBlob(descAddr, &descriptorSnapshot,
                         sizeof(descriptorSnapshot));
        descriptorSnapshotValid = true;
        report.eventDescriptorSnapshotTick = curTick();
        if (descriptorSnapshot.flags & LUSGS_FLAG_TRACE)
            env.traceEnable = true;
        transition(EventState::ValidateDescriptor, "response",
                   "descriptor_spm_read");
        return;

      case EventState::ValidateDescriptor: {
        const CfdLusgsStatus validation =
            validateDescriptor(descriptorSnapshot, env);
        if (validation != CfdLusgsStatus::Complete) {
            finishTask(validation);
            return;
        }
        const uint64_t total_cells =
            static_cast<uint64_t>(descriptorSnapshot.n_lines) *
            descriptorSnapshot.n_cells;
        dqstar.assign(total_cells, {});
        dq.assign(total_cells, {});
        report.forwardCells = total_cells;
        report.backwardCells = total_cells;
        if (descriptorSnapshot.flags & LUSGS_FLAG_UPDATE_Q)
            report.updatedQCells = total_cells;
        transition(EventState::AllocContext, "validate", "controller");
        return;
      }

      case EventState::AllocContext:
        report.contextAlloc = 1;
        report.eventContextAlloc = 1;
        report.maxActiveContexts = 1;
        report.eventMaxActiveContexts = 1;
        transition(EventState::ForwardInit, "alloc", "context");
        return;

      case EventState::ForwardInit:
        line = 0;
        forwardCell = 0;
        transition(EventState::ForwardLoadReq, "phase_start",
                   "controller");
        return;

      case EventState::ForwardLoadReq:
      {
        const bool load_c = forwardCell > 0 && !env.pathaReal;
        report.spmReadRequests += load_c ? 3 : 2;
        report.spmWaitCycles += env.spmReadLatency;
        report.eventSpmReads += load_c ? 3 : 2;
        report.eventSpmReadBytes +=
            MatrixBytes + VectorBytes + (load_c ? MatrixBytes : 0);
        issueRequest(LusgsResourceType::SpmRead, env.spmReadLatency,
                     EventState::ForwardLoadWait);
        return;
      }

      case EventState::ForwardLoadWait:
        if (!consumeRequest(LusgsResourceType::SpmRead))
            return;
        proxy().readBlob(matrixAddr(descriptorSnapshot.lu_a_base,
                                    descriptorSnapshot, line, forwardCell),
                         lu.data(), MatrixBytes);
        proxy().readBlob(vectorAddr(descriptorSnapshot.rhs_base,
                                    descriptorSnapshot, line, forwardCell),
                         rhs.data(), VectorBytes);
        report.luBytes += MatrixBytes;
        report.rhsBytes += VectorBytes;
        if (forwardCell > 0 && !env.pathaReal) {
            proxy().readBlob(matrixAddr(descriptorSnapshot.c_base,
                                        descriptorSnapshot, line, forwardCell),
                             cmat.data(), MatrixBytes);
            report.cBytes += MatrixBytes;
            transition(EventState::ForwardPathAReq, "response", "spm_read");
        } else if (forwardCell > 0) {
            transition(EventState::ForwardPathAReq, "response", "spm_read");
        } else {
            transition(EventState::ForwardVec5Req, "response", "spm_read");
        }
        return;

      case EventState::ForwardPathAReq:
        if (env.pathaReal) {
            if (context(0).waitingRequestId == 0) {
                if (!issueRequest(LusgsResourceType::PathA, 1,
                                  EventState::ForwardPathAWait, false))
                    return;
                const uint64_t request_id = context(0).waitingRequestId;
                PathAMvmRequest req;
                req.header.requestId = request_id;
                req.header.taskToken = token;
                req.header.taskGeneration = taskGeneration;
                req.header.contextId = 0;
                req.header.line = line;
                req.header.cell = forwardCell;
                req.header.tile = currentTile(forwardCell);
                req.header.phase = LusgsPhase::Forward;
                req.header.resourceType = LusgsResourceType::PathA;
                req.matrixBase = matrixAddr(descriptorSnapshot.c_base,
                                            descriptorSnapshot, line,
                                            forwardCell);
                req.vectorBase = vectorAddr(descriptorSnapshot.dqstar_base,
                                            descriptorSnapshot, line,
                                            forwardCell - 1);
                req.resultBase = 0;
                req.preferredSlot = forwardCell & 1;
                if (!trySubmitPathAMvm(req)) {
                    pendingRequests.erase(request_id);
                    report.eventPendingRequestFree++;
                    context(0).waitingRequestId = 0;
                    context(0).waitingResource = LusgsResourceType::None;
                    state = EventState::ForwardPathAReq;
                    scheduleTick(1);
                    return;
                }
            }
            return;
        }
        report.pathaRequests++;
        report.pathaWaitCycles += env.pathaLatency;
        report.pathaArbiterLusgsRequests++;
        report.pathaArbiterOwnershipCycles += env.pathaLatency;
        report.eventPathaRequests++;
        report.eventPathaAccepted++;
        report.eventPathaWaitCycles += env.pathaLatency;
        issueRequest(LusgsResourceType::PathA, env.pathaLatency,
                     EventState::ForwardPathAWait);
        return;

      case EventState::ForwardPathAWait:
        if (!consumeRequest(LusgsResourceType::PathA))
            return;
        if (env.pathaReal) {
            if (!pathaResponseValid ||
                pathaResponse.header.requestId != completedRequestId) {
                report.eventPathaEarlyConsumeErrors++;
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            tmp = pathaResponse.result;
            pathaResponseValid = false;
        } else {
            mvm5(cmat.data(), dqstar[forwardCell - 1].data(), tmp.data());
        }
        report.temporaryResultStores++;
        report.temporaryResultLoads++;
        report.temporaryBytes += VectorBytes * 2;
        report.controllerBufferReads++;
        report.eventPathaCompleted++;
        transition(EventState::ForwardVec5Req, "response", "patha_mvm");
        return;

      case EventState::ForwardVec5Req:
      {
        const LusgsResourceType resource =
            forwardCell > 0 ? LusgsResourceType::Vec5Sub :
                              LusgsResourceType::Vec5Copy;
        if (forwardCell > 0)
            report.vec5SubRequests++;
        else
            report.vec5CopyRequests++;
        if (env.vec5Real) {
            if (env.engineBackpressureTest && !vec5BackpressureInjected) {
                Vec5EventRequest blocker;
                blocker.header.requestId = nextRequestId++;
                blocker.header.taskToken = token;
                blocker.header.taskGeneration = taskGeneration;
                blocker.header.contextId = 0;
                blocker.header.line = line;
                blocker.header.cell = forwardCell;
                blocker.header.phase = LusgsPhase::Forward;
                blocker.header.resourceType = resource;
                blocker.operation = forwardCell > 0 ? CfdLusgsVec5Op::Sub :
                                                      CfdLusgsVec5Op::Copy;
                blocker.input0 = rhs;
                blocker.input1 = tmp;
                blocker.standalone = true;
                if (!trySubmitVec5(blocker)) {
                    finishTask(CfdLusgsStatus::InternalStateError);
                    return;
                }
                vec5BackpressureInjected = true;
                if (!trsvBackpressureInjected) {
                    Trsv5EventRequest trsv_blocker;
                    trsv_blocker.header.requestId = nextRequestId++;
                    trsv_blocker.header.taskToken = token;
                    trsv_blocker.header.taskGeneration = taskGeneration;
                    trsv_blocker.header.contextId = 0;
                    trsv_blocker.header.line = line;
                    trsv_blocker.header.cell = forwardCell;
                    trsv_blocker.header.phase = LusgsPhase::Forward;
                    trsv_blocker.header.resourceType =
                        LusgsResourceType::Trsv5;
                    trsv_blocker.lu = lu;
                    trsv_blocker.rhs = rhs;
                    trsv_blocker.standalone = true;
                    if (!trySubmitTrsv5(trsv_blocker)) {
                        finishTask(CfdLusgsStatus::InternalStateError);
                        return;
                    }
                    trsvBackpressureInjected = true;
                }
            }
            if (context(0).waitingRequestId != 0)
                return;
            if (!issueRequest(resource, 1, EventState::ForwardVec5Wait,
                              false))
                return;
            const uint64_t request_id = context(0).waitingRequestId;
            Vec5EventRequest req;
            req.header = pendingRequests.at(request_id)->header;
            req.operation = forwardCell > 0 ? CfdLusgsVec5Op::Sub :
                                              CfdLusgsVec5Op::Copy;
            req.input0 = rhs;
            if (forwardCell > 0)
                req.input1 = tmp;
            if (!trySubmitVec5(req)) {
                pendingRequests.erase(request_id);
                report.eventPendingRequestFree++;
                context(0).waitingRequestId = 0;
                context(0).waitingResource = LusgsResourceType::None;
                state = EventState::ForwardVec5Req;
            }
            return;
        }
        report.vec5WaitCycles += env.vec5Latency;
        recordVec5Issue(resource, env.vec5Latency);
        issueRequest(resource, env.vec5Latency, EventState::ForwardVec5Wait);
        return;
      }

      case EventState::ForwardVec5Wait:
      {
        const LusgsResourceType resource =
            forwardCell > 0 ? LusgsResourceType::Vec5Sub :
                              LusgsResourceType::Vec5Copy;
        if (!consumeRequest(resource))
            return;
        if (env.vec5Real) {
            Vec5EventResponse response;
            if (!takeVec5Response(completedRequestId, response)) {
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            value = response.result;
        } else {
            if (forwardCell > 0)
                cfdVec5Sub(value.data(), rhs.data(), tmp.data());
            else
                cfdVec5Copy(value.data(), rhs.data());
            recordVec5Complete(resource);
        }
        transition(EventState::ForwardTrsvReq, "response",
                   resourceTypeName(resource));
        return;
      }

      case EventState::ForwardTrsvReq:
        if (env.trsvReal) {
            if (env.engineBackpressureTest && !trsvBackpressureInjected) {
                Trsv5EventRequest blocker;
                blocker.header.requestId = nextRequestId++;
                blocker.header.taskToken = token;
                blocker.header.taskGeneration = taskGeneration;
                blocker.header.contextId = 0;
                blocker.header.line = line;
                blocker.header.cell = forwardCell;
                blocker.header.phase = LusgsPhase::Forward;
                blocker.header.resourceType = LusgsResourceType::Trsv5;
                blocker.lu = lu;
                blocker.rhs = value;
                blocker.standalone = true;
                if (!trySubmitTrsv5(blocker)) {
                    finishTask(CfdLusgsStatus::InternalStateError);
                    return;
                }
                trsvBackpressureInjected = true;
            }
            if (context(0).waitingRequestId != 0)
                return;
            if (!issueRequest(LusgsResourceType::Trsv5, 1,
                              EventState::ForwardTrsvWait, false))
                return;
            const uint64_t request_id = context(0).waitingRequestId;
            Trsv5EventRequest req;
            req.header = pendingRequests.at(request_id)->header;
            req.luAddr = matrixAddr(descriptorSnapshot.lu_a_base,
                                    descriptorSnapshot, line, forwardCell);
            req.lu = lu;
            req.rhs = value;
            if (!trySubmitTrsv5(req)) {
                pendingRequests.erase(request_id);
                report.eventPendingRequestFree++;
                context(0).waitingRequestId = 0;
                context(0).waitingResource = LusgsResourceType::None;
                state = EventState::ForwardTrsvReq;
            }
            return;
        }
        report.trsv5Requests++;
        report.trsv5WaitCycles += env.trsvLatency;
        report.eventTrsvRequests++;
        report.eventTrsvAccepted++;
        report.eventTrsvWaitCycles += env.trsvLatency;
        report.eventTrsvBusyCycles += env.trsvLatency;
        issueRequest(LusgsResourceType::Trsv5, env.trsvLatency,
                     EventState::ForwardTrsvWait);
        return;

      case EventState::ForwardTrsvWait:
        if (!consumeRequest(LusgsResourceType::Trsv5))
            return;
        if (env.trsvReal) {
            Trsv5EventResponse response;
            if (!takeTrsv5Response(completedRequestId, response)) {
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            value = response.result;
        } else {
            cfdTrsv5Solve(lu.data(), value.data());
            report.eventTrsvCompleted++;
        }
        dqstar[forwardCell] = value;
        report.controllerBufferWrites++;
        transition(EventState::ForwardDqstarWriteReq, "response", "trsv5");
        return;

      case EventState::ForwardDqstarWriteReq:
        proxy().writeBlob(vectorAddr(descriptorSnapshot.dqstar_base,
                                     descriptorSnapshot, line, forwardCell),
                          dqstar[forwardCell].data(), VectorBytes);
        report.spmWriteRequests++;
        report.writebackWaitCycles += env.spmWriteLatency;
        report.dqstarWriteBytes += VectorBytes;
        report.dqstarExternalWrites++;
        report.eventSpmWrites++;
        report.eventSpmWriteBytes += VectorBytes;
        issueRequest(LusgsResourceType::DqstarWrite, env.spmWriteLatency,
                     EventState::ForwardDqstarWriteWait);
        return;

      case EventState::ForwardDqstarWriteWait:
        if (!consumeRequest(LusgsResourceType::DqstarWrite))
            return;
        transition(EventState::ForwardNext, "response", "dqstar_write");
        return;

      case EventState::ForwardNext:
        forwardCell++;
        if (forwardCell < descriptorSnapshot.n_cells)
            transition(EventState::ForwardLoadReq, "next_cell",
                       "controller");
        else
            transition(EventState::BackwardInit, "phase_done",
                       "controller");
        return;

      case EventState::BackwardInit:
        backwardCell = descriptorSnapshot.n_cells - 1;
        transition(EventState::BackwardLoadReq, "phase_start",
                   "controller");
        return;

      case EventState::BackwardLoadReq:
        if (env.pathaReal) {
            if (backwardCell + 1 < descriptorSnapshot.n_cells)
                transition(EventState::BackwardPathAReq, "phase_continue",
                           "controller");
            else
                transition(EventState::BackwardVec5Req, "phase_continue",
                           "controller");
            return;
        }
        if (backwardCell + 1 < descriptorSnapshot.n_cells) {
            report.spmReadRequests++;
            report.eventSpmReads++;
            report.eventSpmReadBytes += MatrixBytes;
        }
        report.spmWaitCycles += env.spmReadLatency;
        issueRequest(LusgsResourceType::SpmRead, env.spmReadLatency,
                     EventState::BackwardLoadWait);
        return;

      case EventState::BackwardLoadWait:
        if (!consumeRequest(LusgsResourceType::SpmRead))
            return;
        if (backwardCell + 1 < descriptorSnapshot.n_cells) {
            proxy().readBlob(matrixAddr(descriptorSnapshot.bbar_base,
                                        descriptorSnapshot, line,
                                        backwardCell),
                             bbar.data(), MatrixBytes);
            report.bbarBytes += MatrixBytes;
            transition(EventState::BackwardPathAReq, "response", "spm_read");
        } else {
            transition(EventState::BackwardVec5Req, "response", "spm_read");
        }
        return;

      case EventState::BackwardPathAReq:
        if (env.pathaReal) {
            if (context(0).waitingRequestId == 0) {
                if (!issueRequest(LusgsResourceType::PathA, 1,
                                  EventState::BackwardPathAWait, false))
                    return;
                const uint64_t request_id = context(0).waitingRequestId;
                PathAMvmRequest req;
                req.header.requestId = request_id;
                req.header.taskToken = token;
                req.header.taskGeneration = taskGeneration;
                req.header.contextId = 0;
                req.header.line = line;
                req.header.cell = backwardCell;
                req.header.tile = currentTile(backwardCell);
                req.header.phase = LusgsPhase::Backward;
                req.header.resourceType = LusgsResourceType::PathA;
                req.matrixBase = matrixAddr(descriptorSnapshot.bbar_base,
                                            descriptorSnapshot, line,
                                            backwardCell);
                req.vectorBase = vectorAddr(descriptorSnapshot.dq_base,
                                            descriptorSnapshot, line,
                                            backwardCell + 1);
                req.resultBase = 0;
                req.preferredSlot = backwardCell & 1;
                if (!trySubmitPathAMvm(req)) {
                    pendingRequests.erase(request_id);
                    report.eventPendingRequestFree++;
                    context(0).waitingRequestId = 0;
                    context(0).waitingResource = LusgsResourceType::None;
                    state = EventState::BackwardPathAReq;
                    scheduleTick(1);
                    return;
                }
            }
            return;
        }
        report.pathaRequests++;
        report.pathaWaitCycles += env.pathaLatency;
        report.pathaArbiterLusgsRequests++;
        report.pathaArbiterOwnershipCycles += env.pathaLatency;
        report.eventPathaRequests++;
        report.eventPathaAccepted++;
        report.eventPathaWaitCycles += env.pathaLatency;
        issueRequest(LusgsResourceType::PathA, env.pathaLatency,
                     EventState::BackwardPathAWait);
        return;

      case EventState::BackwardPathAWait:
        if (!consumeRequest(LusgsResourceType::PathA))
            return;
        if (env.pathaReal) {
            if (!pathaResponseValid ||
                pathaResponse.header.requestId != completedRequestId) {
                report.eventPathaEarlyConsumeErrors++;
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            tmp = pathaResponse.result;
            pathaResponseValid = false;
        } else {
            mvm5(bbar.data(), dq[backwardCell + 1].data(), tmp.data());
        }
        report.temporaryResultStores++;
        report.temporaryResultLoads++;
        report.temporaryBytes += VectorBytes * 2;
        report.controllerBufferReads++;
        report.eventPathaCompleted++;
        transition(EventState::BackwardVec5Req, "response", "patha_mvm");
        return;

      case EventState::BackwardVec5Req:
      {
        const LusgsResourceType resource =
            backwardCell + 1 < descriptorSnapshot.n_cells ?
            LusgsResourceType::Vec5Sub : LusgsResourceType::Vec5Copy;
        if (backwardCell + 1 < descriptorSnapshot.n_cells)
            report.vec5SubRequests++;
        else
            report.vec5CopyRequests++;
        if (env.vec5Real) {
            if (context(0).waitingRequestId != 0)
                return;
            if (!issueRequest(resource, 1, EventState::BackwardVec5Wait,
                              false))
                return;
            const uint64_t request_id = context(0).waitingRequestId;
            Vec5EventRequest req;
            req.header = pendingRequests.at(request_id)->header;
            req.operation = backwardCell + 1 < descriptorSnapshot.n_cells ?
                CfdLusgsVec5Op::Sub : CfdLusgsVec5Op::Copy;
            req.input0 = dqstar[backwardCell];
            if (req.operation == CfdLusgsVec5Op::Sub)
                req.input1 = tmp;
            if (!trySubmitVec5(req)) {
                pendingRequests.erase(request_id);
                report.eventPendingRequestFree++;
                context(0).waitingRequestId = 0;
                context(0).waitingResource = LusgsResourceType::None;
                state = EventState::BackwardVec5Req;
            }
            return;
        }
        report.vec5WaitCycles += env.vec5Latency;
        recordVec5Issue(resource, env.vec5Latency);
        issueRequest(resource, env.vec5Latency, EventState::BackwardVec5Wait);
        return;
      }

      case EventState::BackwardVec5Wait:
      {
        const LusgsResourceType resource =
            backwardCell + 1 < descriptorSnapshot.n_cells ?
            LusgsResourceType::Vec5Sub : LusgsResourceType::Vec5Copy;
        if (!consumeRequest(resource))
            return;
        if (env.vec5Real) {
            Vec5EventResponse response;
            if (!takeVec5Response(completedRequestId, response)) {
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            dq[backwardCell] = response.result;
        } else {
            if (backwardCell + 1 < descriptorSnapshot.n_cells) {
                cfdVec5Sub(dq[backwardCell].data(),
                           dqstar[backwardCell].data(), tmp.data());
            } else {
                dq[backwardCell] = dqstar[backwardCell];
            }
            recordVec5Complete(resource);
        }
        if (backwardCell + 1 < descriptorSnapshot.n_cells) {
            report.dqstarReadBytes += VectorBytes;
            report.dqstarExternalReads++;
        }
        report.controllerBufferWrites++;
        transition(EventState::BackwardDqWriteReq, "response",
                   resourceTypeName(resource));
        return;
      }

      case EventState::BackwardDqWriteReq:
        if (descriptorSnapshot.flags & LUSGS_FLAG_WRITE_DQ) {
            proxy().writeBlob(vectorAddr(descriptorSnapshot.dq_base,
                                         descriptorSnapshot, line,
                                         backwardCell),
                              dq[backwardCell].data(), VectorBytes);
            report.spmWriteRequests++;
            report.dqWriteBytes += VectorBytes;
            report.finalDqWrites++;
            report.eventSpmWrites++;
            report.eventSpmWriteBytes += VectorBytes;
        }
        report.writebackWaitCycles += env.spmWriteLatency;
        issueRequest(LusgsResourceType::DqWrite, env.spmWriteLatency,
                     EventState::BackwardDqWriteWait);
        return;

      case EventState::BackwardDqWriteWait:
        if (!consumeRequest(LusgsResourceType::DqWrite))
            return;
        transition(EventState::BackwardNext, "response", "dq_write");
        return;

      case EventState::BackwardNext:
        if (backwardCell == 0) {
            qCell = 0;
            transition(EventState::QUpdateReq, "phase_done", "controller");
        } else {
            backwardCell--;
            transition(EventState::BackwardLoadReq, "next_cell",
                       "controller");
        }
        return;

      case EventState::QUpdateReq:
        if (!(descriptorSnapshot.flags & LUSGS_FLAG_UPDATE_Q) ||
            qCell >= descriptorSnapshot.n_cells) {
            finishTask(CfdLusgsStatus::Complete);
            return;
        }
        if (env.vec5Real) {
            if (context(0).waitingRequestId != 0)
                return;
            proxy().readBlob(vectorAddr(descriptorSnapshot.q_base,
                                        descriptorSnapshot, line, qCell),
                             qvec.data(), VectorBytes);
            report.qReadBytes += VectorBytes;
            report.vec5AxpyRequests++;
            report.spmReadRequests++;
            report.eventSpmReads++;
            report.eventSpmReadBytes += VectorBytes;
            if (!issueRequest(LusgsResourceType::Vec5Axpy, 1,
                              EventState::QUpdateWait, false))
                return;
            const uint64_t request_id = context(0).waitingRequestId;
            Vec5EventRequest req;
            req.header = pendingRequests.at(request_id)->header;
            req.operation = CfdLusgsVec5Op::Axpy;
            req.input0 = qvec;
            req.input1 = dq[qCell];
            req.scalar = descriptorSnapshot.omega;
            if (!trySubmitVec5(req)) {
                pendingRequests.erase(request_id);
                report.eventPendingRequestFree++;
                context(0).waitingRequestId = 0;
                context(0).waitingResource = LusgsResourceType::None;
                state = EventState::QUpdateReq;
            }
            return;
        } else {
            proxy().readBlob(vectorAddr(descriptorSnapshot.q_base,
                                        descriptorSnapshot, line, qCell),
                             qvec.data(), VectorBytes);
            cfdVec5Axpy(qvec.data(), qvec.data(), descriptorSnapshot.omega,
                        dq[qCell].data());
            proxy().writeBlob(vectorAddr(descriptorSnapshot.q_base,
                                         descriptorSnapshot, line, qCell),
                              qvec.data(), VectorBytes);
            report.qReadBytes += VectorBytes;
            report.qWriteBytes += VectorBytes;
            report.vec5AxpyRequests++;
            report.spmReadRequests++;
            report.spmWriteRequests++;
            report.vec5WaitCycles += env.vec5Latency;
            report.writebackWaitCycles += env.spmWriteLatency;
            recordVec5Issue(LusgsResourceType::Vec5Axpy, env.vec5Latency);
            report.eventSpmReads++;
            report.eventSpmWrites++;
            report.eventSpmReadBytes += VectorBytes;
            report.eventSpmWriteBytes += VectorBytes;
            issueRequest(LusgsResourceType::Vec5Axpy,
                         env.vec5Latency + env.spmWriteLatency,
                         EventState::QUpdateWait);
        }
        return;

      case EventState::QUpdateWait:
        if (!consumeRequest(LusgsResourceType::Vec5Axpy))
            return;
        if (env.vec5Real) {
            Vec5EventResponse response;
            if (!takeVec5Response(completedRequestId, response)) {
                finishTask(CfdLusgsStatus::InternalStateError);
                return;
            }
            qvec = response.result;
            proxy().writeBlob(vectorAddr(descriptorSnapshot.q_base,
                                         descriptorSnapshot, line, qCell),
                              qvec.data(), VectorBytes);
            report.qWriteBytes += VectorBytes;
            report.spmWriteRequests++;
            report.writebackWaitCycles += env.spmWriteLatency;
            report.eventSpmWrites++;
            report.eventSpmWriteBytes += VectorBytes;
        } else {
            recordVec5Complete(LusgsResourceType::Vec5Axpy);
        }
        qCell++;
        transition(EventState::QUpdateReq, "response", "q_update");
        return;

      case EventState::Idle:
      case EventState::TaskComplete:
      case EventState::TaskError:
        return;
    }
}

CfdLusgsEventController eventController;

} // anonymous namespace

bool
cfdLusgsEventEnabled()
{
    const char *impl = std::getenv("GEM5_CFD_LUSGS_CONTROLLER_IMPL");
    if (impl && (std::strcmp(impl, "event") == 0 ||
                 std::strcmp(impl, "step4") == 0 ||
                 std::strcmp(impl, "event-realistic") == 0))
        return true;
    return envBool("GEM5_CFD_LUSGS_STEP4_ENABLE", false);
}

bool
cfdLusgsEventBusy()
{
    return eventController.busy();
}

uint64_t
cfdLusgsEventLaunch(ExecContext *xc, Addr descriptor_addr)
{
    return eventController.launch(xc, descriptor_addr);
}

uint64_t
cfdLusgsEventWait(uint64_t token)
{
    return eventController.wait(token);
}

} // namespace ArmISA
} // namespace gem5
