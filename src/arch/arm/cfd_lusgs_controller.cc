#include "arch/arm/cfd_lusgs_controller.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "arch/arm/cfd_local_spm.hh"
#include "arch/arm/cfd_lusgs_event_controller.hh"
#include "arch/arm/cfd_lusgs_vec5.hh"
#include "arch/arm/cfd_trsv5_math.hh"
#include "cpu/exec_context.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace ArmISA
{

namespace
{

static constexpr uint32_t MatrixBytes = 5 * 5 * sizeof(double);
static constexpr uint32_t VectorBytes = 5 * sizeof(double);
static constexpr uint64_t LocalSpmCapacity = 96ull * 1024ull;

struct ControllerEnv
{
    char stage = 'A';
    uint32_t contexts = 1;
    uint32_t tileCells = 1;
    uint64_t pathaLatency = 43;
    uint64_t trsvLatency = 60;
    uint64_t vec5Latency = 4;
    bool traceEnable = false;
    uint32_t traceLines = 1;
    uint32_t traceCells = 4;
    const char *traceFile = "m5out/lusgs_controller_trace.csv";
};

struct ControllerTask
{
    uint64_t token = 0;
    CfdLusgsStatus status = CfdLusgsStatus::Complete;
    bool busyUntilWait = false;
};

using ControllerReport = CfdLusgsControllerRecord;

ControllerTask activeTask;
uint64_t nextToken = 1;

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

ControllerEnv
readEnv()
{
    ControllerEnv env;
    const char *stage = std::getenv("GEM5_CFD_LUSGS_STEP3_STAGE");
    if (stage && *stage)
        env.stage = *stage;
    env.contexts = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_CONTEXTS", env.stage == 'C' ? 8 : 1));
    env.tileCells = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_TILE_CELLS", 1));
    env.trsvLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_TRSV5_LAT", 60));
    env.vec5Latency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_VEC5_LAT", 4));
    env.pathaLatency = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_PATHA_LAT", 43));
    env.traceEnable = envBool("GEM5_CFD_LUSGS_CONTROLLER_TRACE_ENABLE", false);
    env.traceLines = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_CONTROLLER_TRACE_LINES", 1));
    env.traceCells = std::max<uint64_t>(
        1, envU64("GEM5_CFD_LUSGS_CONTROLLER_TRACE_CELLS", 4));
    env.traceFile = std::getenv("GEM5_CFD_LUSGS_CONTROLLER_TRACE_FILE");
    if (!env.traceFile || !*env.traceFile)
        env.traceFile = "m5out/lusgs_controller_trace.csv";
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

void
traceEvent(FILE *fp, uint64_t cycle, uint64_t task, uint32_t line,
           uint32_t cell, uint32_t tile, const char *phase,
           uint32_t context, const char *state, const char *op,
           const char *wait, const char *note)
{
    if (!fp)
        return;
    std::fprintf(fp,
        "%llu,%llu,%u,%u,%u,%s,%u,%s,%s,0,0,0,0,1,1,1,1,%s,0,%s\n",
        (unsigned long long)cycle, (unsigned long long)task,
        line, cell, tile, phase, context, state, op, wait, note);
}

void
recordReport(const ControllerReport &r)
{
    if (auto *local = getCfdLocalSpm())
        local->recordLusgsController(r);
}

CfdLusgsStatus
validateDescriptor(const CfdLusgsDescriptor &d, const ControllerEnv &env)
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
    if ((d.flags & LUSGS_FLAG_UPDATE_Q) && d.q_base == 0)
        return CfdLusgsStatus::BadDescriptor;
    if (env.stage == 'A' && d.n_lines != 1)
        return CfdLusgsStatus::UnsupportedStage;
    if (env.stage != 'A' && env.stage != 'B' && env.stage != 'C')
        return CfdLusgsStatus::UnsupportedStage;
    const uint64_t tile_cells = std::max<uint32_t>(1, d.tile_cells);
    const uint64_t tile_bytes =
        static_cast<uint64_t>(d.n_lines) * tile_cells *
        (3 * MatrixBytes + 4 * VectorBytes);
    if ((d.flags & LUSGS_FLAG_CHECK_BOUNDS) && tile_bytes > LocalSpmCapacity)
        return CfdLusgsStatus::SpmCapacity;
    return CfdLusgsStatus::Complete;
}

void
modelSchedule(const CfdLusgsDescriptor &d, const ControllerEnv &env,
              ControllerReport &r)
{
    const uint64_t cells = d.n_cells;
    const uint64_t lines = d.n_lines;
    const uint64_t patha = r.pathaRequests;
    const uint64_t trsv = r.trsv5Requests;
    const uint64_t vec5 = r.vec5SubRequests + r.vec5AxpyRequests +
                          r.vec5CopyRequests;
    const uint64_t patha_busy = patha * env.pathaLatency;
    const uint64_t trsv_busy = trsv * env.trsvLatency;
    const uint64_t vec5_busy = vec5 * env.vec5Latency;

    uint64_t sequential = trsv_busy + patha_busy;
    if (env.stage == 'A')
        sequential += r.vec5SubRequests * 5;
    else
        sequential += vec5_busy;
    sequential += r.spmReadRequests + r.spmWriteRequests;

    if (env.stage == 'C' && lines > 1) {
        const uint64_t active = std::min<uint64_t>(env.contexts, lines);
        const uint64_t resource_bound =
            std::max<uint64_t>(
                std::max<uint64_t>(patha_busy, trsv_busy),
                std::max<uint64_t>(vec5_busy, 1));
        const uint64_t dependency_bound =
            cells * (env.trsvLatency + env.pathaLatency + env.vec5Latency);
        r.totalCycles = std::max(resource_bound, dependency_bound);
        r.maxActiveContexts = active;
        r.activeContextCycles = r.totalCycles * active;
        r.contextSwitches = lines > 1 ? (lines * cells * 2 - 1) : 0;
        r.noReadyContextCycles = r.totalCycles > resource_bound ?
            r.totalCycles - resource_bound : 0;
    } else {
        r.totalCycles = std::max<uint64_t>(1, sequential);
        r.maxActiveContexts = 1;
        r.activeContextCycles = r.totalCycles;
    }

    r.busyCycles = r.totalCycles;
    r.pathaWaitCycles = patha_busy;
    r.trsv5WaitCycles = trsv_busy;
    r.vec5WaitCycles = vec5_busy;
    r.pathaArbiterOwnershipCycles = patha_busy;
    r.pathaArbiterLusgsRequests = patha;

    r.cyclesPerForwardCell =
        r.forwardCells ? static_cast<double>(r.totalCycles) / r.forwardCells : 0;
    r.cyclesPerBackwardCell =
        r.backwardCells ? static_cast<double>(r.totalCycles) / r.backwardCells : 0;
    const uint64_t full_cells = lines * cells;
    r.cyclesPerFullCell =
        full_cells ? static_cast<double>(r.totalCycles) / full_cells : 0;
    r.cellsPer1000Cycles =
        r.totalCycles ? (1000.0 * lines * cells) / r.totalCycles : 0;
    r.pathaUtilization =
        r.totalCycles ? static_cast<double>(patha_busy) / r.totalCycles : 0;
    r.trsv5Utilization =
        r.totalCycles ? static_cast<double>(trsv_busy) / r.totalCycles : 0;
    r.vec5Utilization =
        r.totalCycles ? static_cast<double>(vec5_busy) / r.totalCycles : 0;
}

CfdLusgsStatus
runTask(ExecContext *xc, uint64_t token, const CfdLusgsDescriptor &d,
        const ControllerEnv &env, ControllerReport &r)
{
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    FILE *trace = nullptr;
    if (env.traceEnable || (d.flags & LUSGS_FLAG_TRACE)) {
        trace = std::fopen(env.traceFile, "w");
        if (trace) {
            std::fprintf(trace,
                "cycle,task_id,line,cell,tile,phase,context_id,state,"
                "operation,patha_slot,patha_token,trsv5_token,vec5_token,"
                "dependency_ready,resource_ready,issue,complete,wait_reason,"
                "spm_buffer,note\n");
        }
    }

    const uint64_t total_cells =
        static_cast<uint64_t>(d.n_lines) * d.n_cells;
    r.spmCapacity = LocalSpmCapacity;
    r.forwardCells = total_cells;
    r.backwardCells = total_cells;
    r.trsv5Requests = total_cells;
    r.pathaRequests = 2 * static_cast<uint64_t>(d.n_lines) *
                      (d.n_cells > 0 ? d.n_cells - 1 : 0);
    r.spmReadRequests += r.trsv5Requests * 2 + r.pathaRequests * 2;
    r.spmWriteRequests += r.pathaRequests;
    r.luBytes = total_cells * MatrixBytes;
    r.cBytes = static_cast<uint64_t>(d.n_lines) *
               (d.n_cells > 0 ? d.n_cells - 1 : 0) * MatrixBytes;
    r.bbarBytes = r.cBytes;
    r.rhsBytes = total_cells * VectorBytes;
    r.dqstarWriteBytes = total_cells * VectorBytes;
    r.dqstarExternalWrites = total_cells;
    r.temporaryResultStores = r.pathaRequests;
    r.temporaryResultLoads = r.pathaRequests;
    r.temporaryBytes = r.pathaRequests * VectorBytes * 2;
    r.controllerBufferReads = r.pathaRequests;
    r.controllerBufferWrites = total_cells * 2;
    if (d.flags & LUSGS_FLAG_WRITE_DQ) {
        r.dqWriteBytes = total_cells * VectorBytes;
        r.finalDqWrites = total_cells;
    }
    if (d.flags & LUSGS_FLAG_UPDATE_Q) {
        r.updatedQCells = total_cells;
        r.qReadBytes = total_cells * VectorBytes;
        r.qWriteBytes = total_cells * VectorBytes;
        r.vec5AxpyRequests = total_cells;
    }
    if (env.stage == 'A') {
        r.vec5SubRequests = 0;
    } else {
        r.vec5SubRequests = r.pathaRequests;
        r.vec5CopyRequests = d.n_lines;
    }
    const uint64_t tile_cells = std::max<uint32_t>(1, d.tile_cells);
    r.tileCoefficientBytes =
        static_cast<uint64_t>(d.n_lines) * tile_cells * 3 * MatrixBytes;
    r.tileVectorBytes =
        static_cast<uint64_t>(d.n_lines) * tile_cells * 4 * VectorBytes;
    r.tileTemporaryBytes =
        static_cast<uint64_t>(d.n_lines) * 2 * VectorBytes;
    r.tileTotalBytes = r.tileCoefficientBytes + r.tileVectorBytes +
                       r.tileTemporaryBytes;
    r.tileLoads = (d.n_cells + tile_cells - 1) / tile_cells * d.n_lines;
    if (env.stage == 'C' && r.tileLoads > 1) {
        r.tilePrefetches = r.tileLoads - 1;
        r.prefetchUseful = r.tilePrefetches;
        r.bufferSwap = r.tilePrefetches;
    }
    r.contextAlloc = std::min<uint64_t>(env.contexts, d.n_lines);
    r.contextFree = r.contextAlloc;

    std::vector<std::array<double, 5>> dqstar(total_cells);
    std::vector<std::array<double, 5>> dq(total_cells);

    for (uint32_t line = 0; line < d.n_lines; ++line) {
        for (uint32_t cell = 0; cell < d.n_cells; ++cell) {
            const uint64_t idx = static_cast<uint64_t>(line) * d.n_cells + cell;
            std::array<double, 25> lu = {};
            std::array<double, 5> rhs = {};
            std::array<double, 5> tmp = {};
            std::array<double, 5> value = {};

            proxy.readBlob(matrixAddr(d.lu_a_base, d, line, cell),
                           lu.data(), MatrixBytes);
            proxy.readBlob(vectorAddr(d.rhs_base, d, line, cell),
                           rhs.data(), VectorBytes);
            if (cell > 0) {
                std::array<double, 25> cmat = {};
                proxy.readBlob(matrixAddr(d.c_base, d, line, cell),
                               cmat.data(), MatrixBytes);
                mvm5(cmat.data(), dqstar[idx - 1].data(), tmp.data());
                if (env.stage == 'A')
                    cfdVec5Sub(value.data(), rhs.data(), tmp.data());
                else
                    cfdVec5Sub(value.data(), rhs.data(), tmp.data());
            } else {
                cfdVec5Copy(value.data(), rhs.data());
            }
            cfdTrsv5Solve(lu.data(), value.data());
            dqstar[idx] = value;
            proxy.writeBlob(vectorAddr(d.dqstar_base, d, line, cell),
                            dqstar[idx].data(), VectorBytes);
            traceEvent(trace, idx, token, line, cell, cell / tile_cells,
                       "forward", line % std::max<uint32_t>(1, env.contexts),
                       "READY", cell ? "PATHA_VEC5_TRSV5" : "TRSV5",
                       "none", "functional coarse controller");
        }

        const uint64_t last =
            static_cast<uint64_t>(line) * d.n_cells + (d.n_cells - 1);
        dq[last] = dqstar[last];
        if (d.flags & LUSGS_FLAG_WRITE_DQ)
            proxy.writeBlob(vectorAddr(d.dq_base, d, line, d.n_cells - 1),
                            dq[last].data(), VectorBytes);

        for (uint32_t rev = d.n_cells - 1; rev > 0; --rev) {
            const uint32_t cell = rev - 1;
            const uint64_t idx = static_cast<uint64_t>(line) * d.n_cells + cell;
            std::array<double, 25> bbar = {};
            std::array<double, 5> tmp = {};
            proxy.readBlob(matrixAddr(d.bbar_base, d, line, cell),
                           bbar.data(), MatrixBytes);
            mvm5(bbar.data(), dq[idx + 1].data(), tmp.data());
            cfdVec5Sub(dq[idx].data(), dqstar[idx].data(), tmp.data());
            r.dqstarReadBytes += VectorBytes;
            r.dqstarExternalReads++;
            if (d.flags & LUSGS_FLAG_WRITE_DQ)
                proxy.writeBlob(vectorAddr(d.dq_base, d, line, cell),
                                dq[idx].data(), VectorBytes);
            traceEvent(trace, idx, token, line, cell, cell / tile_cells,
                       "backward", line % std::max<uint32_t>(1, env.contexts),
                       "READY", "PATHA_VEC5", "none",
                       "functional coarse controller");
        }

        if (d.flags & LUSGS_FLAG_UPDATE_Q) {
            for (uint32_t cell = 0; cell < d.n_cells; ++cell) {
                const uint64_t idx =
                    static_cast<uint64_t>(line) * d.n_cells + cell;
                std::array<double, 5> q = {};
                proxy.readBlob(vectorAddr(d.q_base, d, line, cell),
                               q.data(), VectorBytes);
                cfdVec5Axpy(q.data(), q.data(), d.omega, dq[idx].data());
                proxy.writeBlob(vectorAddr(d.q_base, d, line, cell),
                                q.data(), VectorBytes);
            }
        }
    }

    if (trace)
        std::fclose(trace);
    modelSchedule(d, env, r);
    return CfdLusgsStatus::Complete;
}

} // anonymous namespace

bool
cfdLusgsBusy()
{
    if (cfdLusgsEventEnabled())
        return cfdLusgsEventBusy();
    return activeTask.busyUntilWait;
}

uint64_t
cfdLusgsLaunch(ExecContext *xc, Addr descriptor_addr)
{
    if (cfdLusgsEventEnabled())
        return cfdLusgsEventLaunch(xc, descriptor_addr);

    ControllerReport report;
    report.launches = 1;
    report.committedLaunches = 1;

    if (activeTask.busyUntilWait) {
        report.failedTasks = 1;
        report.arbiterStalls = 1;
        recordReport(report);
        return 0;
    }

    if (descriptor_addr == 0 || !aligned8(descriptor_addr)) {
        report.failedTasks = 1;
        activeTask.status = descriptor_addr == 0 ?
            CfdLusgsStatus::BadDescriptor : CfdLusgsStatus::BadAlignment;
        recordReport(report);
        return 0;
    }

    CfdLusgsDescriptor desc = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(descriptor_addr, &desc, sizeof(desc));

    const ControllerEnv env = readEnv();
    CfdLusgsStatus status = validateDescriptor(desc, env);
    const uint64_t token = nextToken++;
    if (status == CfdLusgsStatus::Complete)
        status = runTask(xc, token, desc, env, report);

    activeTask.token = token;
    activeTask.status = status;
    activeTask.busyUntilWait = true;
    if (status == CfdLusgsStatus::Complete)
        report.completedTasks = 1;
    else
        report.failedTasks = 1;
    recordReport(report);
    return token;
}

uint64_t
cfdLusgsWait(uint64_t token)
{
    if (cfdLusgsEventEnabled())
        return cfdLusgsEventWait(token);

    if (!activeTask.busyUntilWait || token == 0 || token != activeTask.token)
        return static_cast<uint64_t>(CfdLusgsStatus::BadToken);

    const uint64_t status = static_cast<uint64_t>(activeTask.status);
    activeTask.busyUntilWait = false;
    activeTask.token = 0;
    activeTask.status = CfdLusgsStatus::Complete;
    return status;
}

} // namespace ArmISA
} // namespace gem5
