/*
 * cfd_dsa.cc -- CFD DSA Instruction Implementation (v4.0 SPM + Cross-Domain Regs)
 * ================================================================
 * Architecture: ARMv8-A + CFD-DSA Extension
 *
 * v4.0 Key Innovations:
 *
 * 1. SPM (Scratchpad Memory) Direct Access
 *    - lmat5_spm reads from SPM with 40-byte stride
 *    - Bypasses cache hierarchy, deterministic latency
 *    - Memory address: X[base] + row * 40
 *
 * 2. Cross-Domain Register Write (dotp_row)
 *    - Computes FP64 dot product
 *    - Bit-casts double to uint64_t
 *    - Writes to X[16+lane_id] (integer register)
 *    - Zero RAW dependencies between 5 parallel dotp_row
 *
 * 3. Cross-Domain Register Read (pack_acc)
 *    - Reads X[xn]..X[xn+4] as uint64_t
 *    - Reinterprets bit patterns as 5×FP64
 *    - Packs into Z[zd]
 *
 * Register Allocation (v4.0):
 *   Z0-Z4:   Ping matrix rows
 *   Z5:      Ping vector
 *   Z6-Z10:  Pong matrix rows
 *   Z12:     Pong vector
 *   Z11:     Result register (Ping)
 *   Z13:     Result register (Pong)
 *   X16-X20: Accumulator slots (from dotp_row)
 *   X21/X22: Ping/Pong matrix SPM base pointers
 *   X23/X24: Ping/Pong vector SPM base pointers
 * ================================================================
 */

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <deque>
#include <vector>

#include "base/statistics.hh"
#include "arch/arm/regs/mat.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/vec.hh"
#include "arch/generic/memhelpers.hh"
#include "cpu/exec_context.hh"
#include "cpu/op_class.hh"
#include "cpu/thread_context.hh"
#include "mem/packet.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/cur_tick.hh"

#include "arch/arm/cfd_local_spm.hh"
#include "arch/arm/cfd_coeff_preprocess_controller.hh"
#include "arch/arm/cfd_lusgs_controller.hh"
#include "arch/arm/cfd_lusgs_vec5.hh"
#include "arch/arm/cfd_trsv5_math.hh"
#include "cfd_dsa.hh"
#include "base/logging.hh"

namespace gem5
{

namespace ArmISA
{

// ─── Cross-Domain Accumulator Base ───────────────────────────────
// X[16]..X[20] hold the 5 partial results from dotp_row
// This replaces the Shadow Accumulator map from v3.0
// ─────────────────────────────────────────────────────────────────
// Note: We don't need a map anymore - results go directly to X regs

// Base register for accumulator slots
static constexpr RegIndex ACC_BASE_REG = 16;  // X16..X20
static constexpr int SME_MVM_DIM = 5;

namespace
{

const char *
cfdEnv(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value ? value : fallback;
}

uint64_t
cfdEnvU64(const char *name, uint64_t fallback)
{
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    char *end = nullptr;
    uint64_t parsed = std::strtoull(value, &end, 0);
    return (end && *end == '\0') ? parsed : fallback;
}

bool
cfdEnvBool(const char *name, bool fallback)
{
    const char *value = std::getenv(name);
    if (!value)
        return fallback;
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "on") == 0;
}

uint64_t
localSpmLatencyCycles()
{
    return cfdEnvU64("GEM5_CFD_LOCAL_SPM_LAT", 2);
}

uint64_t
localSpmReadPorts()
{
    return cfdEnvU64("GEM5_CFD_LOCAL_SPM_READ_PORTS", 1);
}

uint64_t
trsv5LatencyCycles()
{
    return cfdEnvU64("GEM5_CFD_TRSV5_LAT", 60);
}

uint64_t
trsm5MrhsLatencyCycles()
{
    return cfdEnvU64("GEM5_CFD_TRSM5_MRHS_LAT", 100);
}

uint64_t
trsm5InvLbarLatencyCycles()
{
    return cfdEnvU64("GEM5_CFD_TRSM5_INV_LBAR_LAT", 200);
}

uint64_t
trsm5Coeff3LatencyCycles()
{
    return cfdEnvU64("GEM5_CFD_TRSM5_COEFF3_LAT", 300);
}

uint64_t
localSpmBanks()
{
    return cfdEnvU64("GEM5_CFD_LOCAL_SPM_BANKS", 4);
}

bool
localSpmBankConflictEnabled()
{
    return cfdEnvBool("GEM5_CFD_LOCAL_SPM_BANK_CONFLICT", true);
}

bool
cfdUseRealisticLocalSpm()
{
    return std::strcmp(cfdEnv("GEM5_CFD_SPM_ACCESS_MODE", "generic"),
                       "local-realistic") == 0;
}

bool
cfdUseEventLocalSpm()
{
    const char *mode = cfdEnv("GEM5_CFD_SPM_ACCESS_MODE", "generic");
    return std::strcmp(mode, "local-event") == 0 ||
           std::strcmp(mode, "local-flow") == 0 ||
           std::strcmp(mode, "local-realistic-event") == 0;
}

uint64_t
localSpmZWritebackPorts()
{
    return cfdEnvU64("GEM5_CFD_LOCAL_SPM_Z_WB_PORTS", 1);
}

bool
pathAUseInternalBuffer()
{
    return std::strcmp(cfdEnv("GEM5_CFD_PATHA_ACC_MODE", "internal-buffer"),
                       "internal-buffer") == 0;
}

bool
pathAStreamingEnabled()
{
    return cfdEnvBool("GEM5_CFD_PATHA_STREAMING", false) &&
           std::strcmp(cfdEnv("GEM5_CFD_PATHA_KERNEL", "baseline"),
                       "stream-dsa") != 0;
}

bool
pathAAsyncStoreEnabled()
{
    return cfdEnvBool("GEM5_CFD_PATHA_ASYNC_STORE", false);
}

RegIndex
pathAStreamTokenRegForSlotLane(unsigned slot_id, unsigned lane_id);

uint64_t
pathAResultBufferDepth()
{
    return std::clamp<uint64_t>(
        cfdEnvU64("GEM5_CFD_PATHA_RESULT_BUFFER_DEPTH", 2), 1, 4);
}

struct PathAResultSlot
{
    bool valid = false;
    std::array<bool, 5> ready = {};
    std::array<uint64_t, 5> seq = {};
    std::array<double, 5> lane = {};
};

struct PathAInputSlot
{
    bool valid = false;
    std::array<bool, 6> ready = {};
    std::array<bool, 5> schedulableRecorded = {};
    std::array<std::array<double, 5>, 5> row = {};
    std::array<double, 5> vec = {};
};

std::array<std::deque<PathAResultSlot>, 4> pathAResultQueues;
std::array<PathAInputSlot, 4> pathAInputSlots;
std::array<std::array<uint64_t, 6>, 4> pathAFieldReadyCycles = {};
std::array<uint64_t, 4> pathAAllFieldsReadyCycles = {};
uint64_t pathAResultSlotsLive = 0;
uint64_t pathAInputSlotsLive = 0;

std::array<std::array<bool, 6>, 2> pathCInputReady = {};
std::array<std::array<uint64_t, 6>, 2> pathCInputReadyCycles = {};

uint64_t
cfdCurrentCycle()
{
    const uint64_t ticks = cfdEnvU64("GEM5_CFD_TICKS_PER_CYCLE", 500);
    return curTick() / std::max<uint64_t>(1, ticks);
}

uint64_t
pathAStreamReadPorts()
{
    return std::clamp<uint64_t>(
        cfdEnvU64("GEM5_CFD_PATHA_STREAM_READ_PORTS",
                  localSpmReadPorts()),
        1, 2);
}

uint64_t
pathADotpLatency()
{
    return std::max<uint64_t>(1, cfdEnvU64("GEM5_CFD_PATHA_DOTP_LAT", 1));
}

bool
pathCOuterEnabled()
{
    return cfdEnvBool("GEM5_CFD_PATHC_OUTER_ENABLE", false);
}

void
recordPathAStreamLoadIssue()
{
    static uint64_t last_cycle = ~0ULL;
    static uint64_t loads_this_cycle = 0;
    static bool dual_recorded = false;

    const uint64_t cycle = cfdCurrentCycle();
    if (cycle != last_cycle) {
        last_cycle = cycle;
        loads_this_cycle = 0;
        dual_recorded = false;
    }

    const uint64_t ports = pathAStreamReadPorts();
    const unsigned port = static_cast<unsigned>(
        std::min<uint64_t>(loads_this_cycle, ports - 1));
    loads_this_cycle++;
    const bool dual = ports >= 2 && loads_this_cycle >= 2 && !dual_recorded;
    if (dual)
        dual_recorded = true;

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordPathAStreamLoadPort(port, dual);
}

void
recordPathCLmatIssue()
{
    if (!pathCOuterEnabled())
        return;

    static uint64_t last_cycle = ~0ULL;
    static uint64_t loads_this_cycle = 0;
    static bool dual_recorded = false;

    const uint64_t cycle = cfdCurrentCycle();
    if (cycle != last_cycle) {
        last_cycle = cycle;
        loads_this_cycle = 0;
        dual_recorded = false;
    }

    const uint64_t ports = std::clamp<uint64_t>(localSpmReadPorts(), 1, 2);
    const unsigned port = static_cast<unsigned>(
        std::min<uint64_t>(loads_this_cycle, ports - 1));
    loads_this_cycle++;
    const bool dual = ports >= 2 && loads_this_cycle >= 2 && !dual_recorded;
    if (dual)
        dual_recorded = true;

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordPathCLmatPort(port, dual);
}

RegIndex
pathAInputTokenRegForSlotField(unsigned slot_id, unsigned field_id)
{
    if (field_id < 5)
        return pathAStreamTokenRegForSlotLane(slot_id, field_id);
    return slot_id == 0 ? 10 : 15;
}

RegIndex
pathAReuseTokenRegForSlot(unsigned slot_id)
{
    return slot_id == 0 ? 13 : 14;
}

unsigned
pathASlotForDotp(RegIndex zra, RegIndex zrb)
{
    const uint64_t depth = pathAResultBufferDepth();
    const bool pong = (zrb == 12) || (zra >= 6 && zra <= 10);
    if (!pong)
        return 0;
    return depth > 1 ? 1 : 0;
}

unsigned
pathASlotForPack(RegIndex zd)
{
    const uint64_t depth = pathAResultBufferDepth();
    const bool pong = (zd == 13);
    if (!pong)
        return 0;
    return depth > 1 ? 1 : 0;
}

RegIndex
pathATokenRegForSlotLane(unsigned slot_id, unsigned lane_id)
{
    static constexpr std::array<RegIndex, 5> ping = {16, 17, 18, 19, 20};
    // Avoid X21-X24 because the benchmark uses them as SPM base registers.
    // Avoid X29/X30 because frame pointer/link register interactions make them
    // poor inline-asm clobber targets.  These are dependency tokens only.
    static constexpr std::array<RegIndex, 5> pong = {25, 26, 27, 28, 15};
    const auto &regs = slot_id == 0 ? ping : pong;
    return regs[std::min<unsigned>(lane_id, 4)];
}

RegIndex
pathAStreamTokenRegForSlotLane(unsigned slot_id, unsigned lane_id)
{
    static constexpr std::array<RegIndex, 5> ping = {16, 17, 18, 19, 20};
    static constexpr std::array<RegIndex, 5> pong = {5, 6, 7, 8, 9};
    const auto &regs = slot_id == 0 ? ping : pong;
    return regs[std::min<unsigned>(lane_id, 4)];
}

void
pathAPopReadSlot(unsigned slot_id)
{
    auto &queue = pathAResultQueues[slot_id];
    if (!queue.empty() && pathAResultSlotsLive > 0) {
        queue.pop_front();
        pathAResultSlotsLive--;
        if (auto *local_spm = getCfdLocalSpm())
            local_spm->recordPathABufferFree(pathAResultSlotsLive);
    }
}

PathAResultSlot &
pathAWriteSlot(unsigned slot_id, unsigned lane_id)
{
    auto &queue = pathAResultQueues[slot_id];
    const uint64_t depth = pathAResultBufferDepth();

    if (queue.empty() || queue.back().ready[lane_id]) {
        if (queue.size() >= depth) {
            if (auto *local_spm = getCfdLocalSpm())
                local_spm->recordPathABufferFullStall();
            if (pathAResultSlotsLive > 0) {
                queue.pop_front();
                pathAResultSlotsLive--;
                if (auto *local_spm = getCfdLocalSpm())
                    local_spm->recordPathABufferFree(pathAResultSlotsLive);
            }
        }
        queue.emplace_back();
        queue.back().valid = true;
        pathAResultSlotsLive++;
        if (auto *local_spm = getCfdLocalSpm()) {
            local_spm->recordPathABufferAlloc(pathAResultSlotsLive);
            if (pathAResultSlotsLive >= 2)
                local_spm->recordPathABufferOverlapCycle();
        }
    }
    return queue.back();
}

void
recordLocalSpmRead(Addr ea, int row_id, unsigned size)
{
    const uint64_t lat = localSpmLatencyCycles();
    const uint64_t ports = std::max<uint64_t>(1, localSpmReadPorts());
    const uint64_t read_width =
        std::max<uint64_t>(size, cfdEnvU64("GEM5_CFD_LOCAL_SPM_READ_WIDTH",
                                           SPM_STRIDE_BYTES));
    CfdLocalSpm *local_spm = getCfdLocalSpm();
    if (local_spm) {
        local_spm->recordRead(
            ea, row_id, size, read_width, lat, ports,
            cfdEnvU64("GEM5_CFD_LOCAL_SPM_OUTSTANDING", 4),
            localSpmBanks(), localSpmBankConflictEnabled(),
            cfdUseRealisticLocalSpm(), cfdUseEventLocalSpm(),
            localSpmZWritebackPorts(),
            cfdEnvU64("GEM5_CFD_LOCAL_SPM_QUEUE_SIZE", 4),
            cfdEnvU64("GEM5_CFD_LOCAL_SPM_BANK_GRANULARITY", 64));
    }
}

} // anonymous namespace

bool
cfdUseLocalSpmPort()
{
    const char *mode = cfdEnv("GEM5_CFD_SPM_ACCESS_MODE", "generic");
    return std::strcmp(mode, "local") == 0 ||
           std::strcmp(mode, "local-ideal") == 0 ||
           std::strcmp(mode, "local-realistic") == 0 ||
           std::strcmp(mode, "local-event") == 0 ||
           std::strcmp(mode, "local-flow") == 0 ||
           std::strcmp(mode, "local-realistic-event") == 0;
}

// ================================================================
// DSALmatRow (Legacy cache mode - unchanged from v3.0)
// ================================================================

struct LmatRowBuffer {
    uint8_t bytes[64];
};

DSALmatRow::DSALmatRow(ExtMachInst machInst, RegIndex _dest,
                       RegIndex _base, int8_t _row)
    : CFDDSABase("lmat5", machInst, CFDDSAMatLdOp),
      dest(_dest), base(_base), row(_row)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;
    flags[IsLoad] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[base];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[dest];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSALmatRow::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    Addr base_addr = xc->getRegOperand(this, 0);
    Addr EA = base_addr + row * CACHE_STRIDE;
    if (traceData)
        traceData->setMem(EA, CACHE_STRIDE, 0);
    uint8_t req_size_dummy[CACHE_STRIDE];
    return initiateMemRead(xc, traceData, EA, req_size_dummy, Request::Flags(0));
}

Fault DSALmatRow::completeAcc(Packet *pkt, ExecContext *xc,
                               trace::InstRecord *traceData) const
{
    const uint8_t *pkt_data = pkt->getConstPtr<uint8_t>();
    VecRegContainer result;
    result.zero();
    double *vdata = result.as<double>();
    const double *src = reinterpret_cast<const double *>(pkt_data);
    for (int i = 0; i < 5; i++)
        vdata[i] = src[i];
    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

Fault DSALmatRow::execute(ExecContext *, trace::InstRecord *) const
{
    panic("DSALmatRow::execute() should never be called. "
          "Use initiateAcc()/completeAcc().\n");
}

std::string DSALmatRow::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "lmat5\tz" << (int)dest << ", [x" << (int)base
       << ", #" << ((int)row * CACHE_STRIDE) << "]";
    return ss.str();
}

// ================================================================
// DSALmatRowSPM — Direct SPM-mapped read path (v4.0)
// ================================================================
//
// Reads 5×FP64 from SPM at address (X[base] + row * 40)
// 40-byte stride = compact layout, no padding
// ================================================================

DSALmatRowSPM::DSALmatRowSPM(ExtMachInst machInst, RegIndex _dest,
                              RegIndex _base, int8_t _row)
    : CFDDSABase("lmat5_spm", machInst, CFDDSAMatLdOp),
      dest(_dest), base(_base), row(_row)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;
    flags[IsLoad] = 1;
    // Timing SPM path: the address is issued through the CPU LSQ/memory
    // system as an uncacheable read, so system.spm observes all 40B lmat
    // accesses instead of relying solely on committed-instruction inference.

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[base];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[dest];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSALmatRowSPM::execute(ExecContext *xc,
                              trace::InstRecord *traceData) const
{
    Addr base_val = xc->getRegOperand(this, 0);
    Addr EA = base_val +
              static_cast<Addr>(row) * SPM_STRIDE_BYTES;
    if (traceData)
        traceData->setMem(EA, SPM_STRIDE_BYTES, 0);

    double row_data[5] = {};
    const std::vector<bool> byte_enable(SPM_STRIDE_BYTES, true);
    Fault fault = xc->readMem(EA, reinterpret_cast<uint8_t *>(row_data),
                              SPM_STRIDE_BYTES, Request::UNCACHEABLE,
                              byte_enable);
    if (fault != NoFault)
        return fault;

    VecRegContainer result;
    result.zero();
    double *vdata = result.as<double>();
    for (int i = 0; i < 5; i++)
        vdata[i] = row_data[i];

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

Fault DSALmatRowSPM::initiateAcc(ExecContext *xc,
                                  trace::InstRecord *traceData) const
{
    Addr base_val = xc->getRegOperand(this, 0);
    Addr EA = base_val +
              static_cast<Addr>(row) * SPM_STRIDE_BYTES;
    if (traceData)
        traceData->setMem(EA, SPM_STRIDE_BYTES, 0);

    uint8_t req_size_dummy[SPM_STRIDE_BYTES];
    return initiateMemRead(xc, traceData, EA, req_size_dummy,
                           Request::UNCACHEABLE);
}

Fault DSALmatRowSPM::completeAcc(Packet *pkt, ExecContext *xc,
                                  trace::InstRecord *traceData) const
{
    if (pkt == nullptr)
        return NoFault;

    const uint8_t *pkt_data = pkt->getConstPtr<uint8_t>();
    VecRegContainer result;
    result.zero();
    double *vdata = result.as<double>();
    const double *src = reinterpret_cast<const double *>(pkt_data);
    for (int i = 0; i < 5; i++)
        vdata[i] = src[i];

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string DSALmatRowSPM::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "lmat5_spm\tz" << (int)dest << ", [x" << (int)base
       << ", #" << ((int)row * SPM_STRIDE_BYTES) << "]";
    return ss.str();
}

// ================================================================
// DSALmatRowSPMLocal — CPU-local timing SPM bypass path (v6.2)
// ================================================================

DSALmatRowSPMLocal::DSALmatRowSPMLocal(ExtMachInst machInst, RegIndex _dest,
                                       RegIndex _base, int8_t _row)
    : CFDDSABase("lmat5_spm.local", machInst, CFDDSAMatLdOp),
      dest(_dest), base(_base), row(_row)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;
    // Deliberately not IsLoad: the local scratchpad port is modeled as a
    // CPU-local DSA FU path with CFDDASMatLd opLat/count, not as an LSQ/cache
    // memory transaction.  The read remains timing-aware through FU latency
    // and local_spm.* stats.

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[base];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[dest];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSALmatRowSPMLocal::execute(ExecContext *xc,
                            trace::InstRecord *traceData) const
{
    Addr base_val = xc->getRegOperand(this, 0);
    Addr EA = base_val +
              static_cast<Addr>(row) * SPM_STRIDE_BYTES;
    if (traceData)
        traceData->setMem(EA, SPM_STRIDE_BYTES, 0);

    double row_data[5] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(EA, row_data, sizeof(row_data));
    recordLocalSpmRead(EA, row, sizeof(row_data));
    recordPathCLmatIssue();

    if (pathCOuterEnabled()) {
        const bool ping = base == 21 || base == 23;
        const bool pong = base == 22 || base == 24;
        if (ping || pong) {
            const unsigned slot = ping ? 0 : 1;
            const bool is_vec = base == 23 || base == 24;
            const unsigned field = is_vec ? 5 : std::min<int>(row, 4);
            if (is_vec) {
                pathCInputReady[slot] = {};
                pathCInputReadyCycles[slot] = {};
            }
            pathCInputReady[slot][field] = true;
            pathCInputReadyCycles[slot][field] = cfdCurrentCycle();
            if (auto *local_spm = getCfdLocalSpm()) {
                local_spm->recordCfdTrace(
                    "PathC", "lmat5_spm", cfdDynInstSeqNum(xc), slot,
                    static_cast<int>(field), -1, -1,
                    pathCInputReadyCycles[slot][field], 0, 0,
                    is_vec ? "vector_ready" : "column_ready");
            }
        }
    }

    VecRegContainer result;
    result.zero();
    double *vdata = result.as<double>();
    for (int i = 0; i < 5; i++)
        vdata[i] = row_data[i];

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSALmatRowSPMLocal::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "lmat5_spm.local\tz" << (int)dest << ", [x" << (int)base
       << ", #" << ((int)row * SPM_STRIDE_BYTES) << "]";
    return ss.str();
}

// ================================================================
// DSADotpRow — Cross-Domain Write to Integer Register (v4.0)
// ================================================================
//
// Computes dot product of matrix row Z[zra] and vector Z[zrb]
// Writes FP64 result to X[16+lane_id] via bit-cast
//
// Key insight: Writing to integer register breaks RAW dependency!
// 5 dotp_row instructions can execute in parallel with zero dependencies
// ================================================================

DSADotpRow::DSADotpRow(ExtMachInst machInst,
                       RegIndex _zra, RegIndex _zrb, uint8_t _lane_id)
    : CFDDSABase("dotp_row", machInst, CFDDSADotpOp),
      zra(_zra), zrb(_zrb), lane_id(_lane_id)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    // 2 source Z regs: matrix row + vector
    _numSrcRegs = 2;
    srcRegIdxArr[0] = vecRegClass[zra];
    srcRegIdxArr[1] = vecRegClass[zrb];

    // 1 destination INTEGER reg.  Baseline preserves X[16+lane_id].  Optional
    // Path A streaming mode uses ping/pong-specific dependency tokens so
    // pack_acc for one slot does not wait on the other slot's dotp producers.
    _numDestRegs = 1;
    const unsigned slot_id =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathASlotForDotp(zra, zrb) : 0;
    const RegIndex token_reg =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathATokenRegForSlotLane(slot_id, lane_id) :
        ACC_BASE_REG + lane_id;
    destRegIdxArr[0] = intRegClass[token_reg];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSADotpRow::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordPathADotpIssue(cfdDynInstSeqNum(xc));

    // Read matrix row from Z register
    VecRegContainer reg_mat;
    xc->getRegOperand(this, 0, &reg_mat);
    const double *mat = reg_mat.as<const double>();

    // Read vector from Z register
    VecRegContainer reg_vec;
    xc->getRegOperand(this, 1, &reg_vec);
    const double *vec = reg_vec.as<const double>();

    // Compute dot product: sum(mat[i] * vec[i]) for i = 0..4
    double dot_product = 0.0;
    for (int j = 0; j < 5; j++) {
        dot_product += mat[j] * vec[j];
    }

    // CROSS-DOMAIN WRITE: Bit-cast FP64 to uint64_t
    // This is the key to breaking RAW dependencies!
    uint64_t result_bits;
    std::memcpy(&result_bits, &dot_product, sizeof(uint64_t));

    if (pathAUseInternalBuffer()) {
        const unsigned slot_id = pathASlotForDotp(zra, zrb);
        auto &slot = pathAWriteSlot(slot_id, lane_id);
        slot.lane[lane_id] = dot_product;
        slot.ready[lane_id] = true;
        slot.seq[lane_id] = cfdDynInstSeqNum(xc);
        if (auto *local_spm = getCfdLocalSpm()) {
            local_spm->recordPathABufferWrite();
            if (pathAResultSlotsLive >= 2)
                local_spm->recordPathABufferOverlapCycle();
        }
    }

    // Write to X[16+lane_id] - this is an INTEGER register write
    // In internal-buffer mode this remains as a dependency/compatibility
    // token for O3 wakeup; pack_acc reads the DSA buffer for the actual data.
    xc->setRegOperand(this, 0, &result_bits);

    return NoFault;
}

std::string DSADotpRow::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    const unsigned slot_id =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathASlotForDotp(zra, zrb) : 0;
    const RegIndex token_reg =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathATokenRegForSlotLane(slot_id, lane_id) :
        ACC_BASE_REG + lane_id;
    ss << "dotp_row\tz" << (int)zra << ", z" << (int)zrb
       << ", #" << (int)lane_id
       << " -> x" << (int)token_reg;
    return ss.str();
}

// ================================================================
// DSAPackAcc — Cross-Domain Read: Pack X Registers to Z Register (v4.0 NEW)
// ================================================================
//
// Reads X[xn]..X[xn+4] as uint64_t
// Reinterprets bit patterns as 5×FP64
// Packs into Z[zd]
//
// This bridges the integer register domain back to vector register domain
// ================================================================

DSAPackAcc::DSAPackAcc(ExtMachInst machInst, RegIndex _zd, RegIndex _xn)
    : CFDDSABase("pack_acc", machInst, CFDDSAPackOp),
      zd(_zd), xn(_xn)
{
    _numSrcRegs = NUM_LANES;
    const unsigned slot_id =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathASlotForPack(zd) : 0;
    for (int i = 0; i < NUM_LANES; i++) {
        const RegIndex token_reg =
            (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
            pathATokenRegForSlotLane(slot_id, i) :
            _xn + i;
        srcRegIdxArr[i] = intRegClass[token_reg];
    }

    // 1 destination Z reg
    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSAPackAcc::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    // Read 5 uint64_t values from X[xn]..X[xn+4]
    uint64_t acc_bits[NUM_LANES];
    for (int i = 0; i < NUM_LANES; i++) {
        acc_bits[i] = 0;
        xc->getRegOperand(this, i, &acc_bits[i]);
    }

    // CROSS-DOMAIN READ: Bit-cast uint64_t to FP64
    // Reinterpret the bit patterns as floating-point values
    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();

    if (pathAUseInternalBuffer()) {
        const unsigned slot_id = pathASlotForPack(zd);
        const auto &queue = pathAResultQueues[slot_id];
        const PathAResultSlot empty_slot;
        const auto &slot = queue.empty() ? empty_slot : queue.front();
        bool complete = slot.valid;
        for (int i = 0; i < NUM_LANES; i++)
            complete = complete && slot.ready[i];

        if (complete) {
            for (int i = 0; i < NUM_LANES; i++)
                rd[i] = slot.lane[i];
            if (auto *local_spm = getCfdLocalSpm()) {
                local_spm->recordPathABufferPack();
                if (pathAResultSlotsLive >= 2)
                    local_spm->recordPathABufferPackWhileDotpCycle();
            }
            pathAPopReadSlot(slot_id);
        } else {
            // Benchmark-oriented safety net: O3 dependencies should make this
            // path unreachable in the steady Path A kernel.  Falling back to
            // X regs preserves regression correctness if a future schedule
            // exposes an incomplete internal slot.
            if (auto *local_spm = getCfdLocalSpm())
                local_spm->recordPathABufferFullStall();
            for (int i = 0; i < NUM_LANES; i++)
                std::memcpy(&rd[i], &acc_bits[i], sizeof(double));
        }
    } else {
        for (int i = 0; i < NUM_LANES; i++) {
            std::memcpy(&rd[i], &acc_bits[i], sizeof(double));
        }
    }

    // Write to Z[zd] (the only destination)
    xc->setRegOperand(this, 0, &result);

    return NoFault;
}

std::string DSAPackAcc::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "pack_acc\tz" << (int)zd << ", x" << (int)xn;
    return ss.str();
}

DSAPackSubAcc::DSAPackSubAcc(ExtMachInst machInst, RegIndex _zd,
                             RegIndex _base, RegIndex _xn)
    : CFDDSABase("patha_pack_sub5", machInst, CFDDSAPackOp),
      zd(_zd), base(_base), xn(_xn)
{
    _numSrcRegs = NUM_LANES + 1;
    srcRegIdxArr[0] = intRegClass[base];
    const unsigned slot_id =
        (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
        pathASlotForPack(zd) : 0;
    for (int i = 0; i < NUM_LANES; i++) {
        const RegIndex token_reg =
            (pathAUseInternalBuffer() && pathAStreamingEnabled()) ?
            pathATokenRegForSlotLane(slot_id, i) :
            xn + i;
        srcRegIdxArr[i + 1] = intRegClass[token_reg];
    }

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSAPackSubAcc::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    const Addr base_ea = xc->getRegOperand(this, 0);
    if (traceData)
        traceData->setMem(base_ea, 5 * sizeof(double), 0);
    double base_data[NUM_LANES] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(base_ea, base_data, sizeof(base_data));

    uint64_t acc_bits[NUM_LANES] = {};
    for (int i = 0; i < NUM_LANES; i++)
        xc->getRegOperand(this, i + 1, &acc_bits[i]);

    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();

    if (pathAUseInternalBuffer()) {
        const unsigned slot_id = pathASlotForPack(zd);
        const auto &queue = pathAResultQueues[slot_id];
        const PathAResultSlot empty_slot;
        const auto &slot = queue.empty() ? empty_slot : queue.front();
        bool complete = slot.valid;
        for (int i = 0; i < NUM_LANES; i++)
            complete = complete && slot.ready[i];

        if (complete) {
            for (int i = 0; i < NUM_LANES; i++)
                rd[i] = base_data[i] - slot.lane[i];
            if (auto *local_spm = getCfdLocalSpm()) {
                local_spm->recordPathABufferPack();
                if (pathAResultSlotsLive >= 2)
                    local_spm->recordPathABufferPackWhileDotpCycle();
            }
            pathAPopReadSlot(slot_id);
        } else {
            if (auto *local_spm = getCfdLocalSpm())
                local_spm->recordPathABufferFullStall();
            for (int i = 0; i < NUM_LANES; i++) {
                double acc = 0.0;
                std::memcpy(&acc, &acc_bits[i], sizeof(double));
                rd[i] = base_data[i] - acc;
            }
        }
    } else {
        for (int i = 0; i < NUM_LANES; i++) {
            double acc = 0.0;
            std::memcpy(&acc, &acc_bits[i], sizeof(double));
            rd[i] = base_data[i] - acc;
        }
    }

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordPathAFusedSub();
    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSAPackSubAcc::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "patha_pack_sub5\tz" << (int)zd << ", [x" << (int)base << "]"
       << ", x" << (int)xn;
    return ss.str();
}

// ================================================================
// Path A streaming DSA instructions
// ================================================================

DSAPathAStreamLoad5::DSAPathAStreamLoad5(ExtMachInst machInst,
                                         uint8_t _slot, uint8_t _field,
                                         RegIndex _base)
    : CFDDSABase("patha_stream_ld5", machInst, CFDDSAStreamLdOp),
      slot(_slot), field(_field), base(_base)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 2;
    srcRegIdxArr[0] = intRegClass[base];
    srcRegIdxArr[1] =
        intRegClass[pathAReuseTokenRegForSlot(slot)];

    _numDestRegs = 1;
    destRegIdxArr[0] =
        intRegClass[pathAInputTokenRegForSlotField(slot, field)];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSAPathAStreamLoad5::execute(ExecContext *xc,
                             trace::InstRecord *traceData) const
{
    Addr base_val = xc->getRegOperand(this, 0);
    uint64_t ignored = 0;
    xc->getRegOperand(this, 1, &ignored);
    const uint8_t row = field < 5 ? field : 0;
    Addr EA = base_val + static_cast<Addr>(row) * SPM_STRIDE_BYTES;
    if (traceData)
        traceData->setMem(EA, SPM_STRIDE_BYTES, 0);

    double payload[5] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(EA, payload, sizeof(payload));
    recordLocalSpmRead(EA, row, sizeof(payload));
    recordPathAStreamLoadIssue();

    auto &entry = pathAInputSlots[slot];
    if (!entry.valid) {
        entry = PathAInputSlot{};
        pathAFieldReadyCycles[slot] = {};
        pathAAllFieldsReadyCycles[slot] = 0;
        entry.valid = true;
        pathAInputSlotsLive++;
        if (auto *local_spm = getCfdLocalSpm())
            local_spm->recordPathAInputBufferAlloc(pathAInputSlotsLive);
    }

    bool was_all_fields_ready = true;
    for (bool ready : entry.ready)
        was_all_fields_ready = was_all_fields_ready && ready;

    if (field < 5) {
        for (int i = 0; i < 5; ++i)
            entry.row[field][i] = payload[i];
    } else {
        for (int i = 0; i < 5; ++i)
            entry.vec[i] = payload[i];
    }
    entry.ready[field] = true;
    pathAFieldReadyCycles[slot][field] = cfdCurrentCycle();

    bool all_fields_ready = true;
    for (bool ready : entry.ready)
        all_fields_ready = all_fields_ready && ready;
    if (all_fields_ready && !was_all_fields_ready)
        pathAAllFieldsReadyCycles[slot] = cfdCurrentCycle();
    if (!all_fields_ready && entry.ready[5]) {
        for (int lane = 0; lane < 5; ++lane) {
            if (!entry.schedulableRecorded[lane] && entry.ready[lane]) {
                entry.schedulableRecorded[lane] = true;
                if (auto *local_spm = getCfdLocalSpm())
                    local_spm->recordPathAEarlyDotpOpportunity();
            }
        }
    }

    const uint64_t token = cfdDynInstSeqNum(xc);
    if (auto *local_spm = getCfdLocalSpm()) {
        local_spm->recordCfdTrace(
            "PathA", "stream_ld", token, slot, field, -1, -1,
            pathAFieldReadyCycles[slot][field], 0,
            pathAAllFieldsReadyCycles[slot],
            all_fields_ready ? "all_fields_ready" : "field_ready");
    }
    xc->setRegOperand(this, 0, &token);
    return NoFault;
}

std::string
DSAPathAStreamLoad5::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "patha_stream_ld5\tslot" << (int)slot
       << ", field" << (int)field << ", [x" << (int)base << "]";
    return ss.str();
}

DSAPathADotpStream::DSAPathADotpStream(ExtMachInst machInst,
                                       uint8_t _slot, uint8_t _lane)
    : CFDDSABase("patha_dotp_stream", machInst, CFDDSADotpOp),
      slot(_slot), lane(_lane)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 2;
    srcRegIdxArr[0] =
        intRegClass[pathAInputTokenRegForSlotField(slot, lane)];
    srcRegIdxArr[1] =
        intRegClass[pathAInputTokenRegForSlotField(slot, 5)];

    _numDestRegs = 1;
    destRegIdxArr[0] =
        intRegClass[pathAStreamTokenRegForSlotLane(slot, lane)];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSAPathADotpStream::execute(ExecContext *xc,
                            trace::InstRecord *traceData) const
{
    uint64_t ignored = 0;
    xc->getRegOperand(this, 0, &ignored);
    xc->getRegOperand(this, 1, &ignored);

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordPathADotpIssue(cfdDynInstSeqNum(xc));

    const auto &entry = pathAInputSlots[slot];
    if (!entry.valid || !entry.ready[lane] || !entry.ready[5]) {
        if (auto *local_spm = getCfdLocalSpm())
            local_spm->recordPathADotpBlockedByInputNotReady();
        uint64_t token = 0;
        xc->setRegOperand(this, 0, &token);
        return NoFault;
    }

    bool all_fields_ready = true;
    for (bool ready : entry.ready)
        all_fields_ready = all_fields_ready && ready;
    const uint64_t now = cfdCurrentCycle();
    const uint64_t row_ready_cycle = pathAFieldReadyCycles[slot][lane];
    const uint64_t vec_ready_cycle = pathAFieldReadyCycles[slot][5];
    const uint64_t dep_ready_cycle = std::max(row_ready_cycle, vec_ready_cycle);
    const uint64_t all_ready_cycle = pathAAllFieldsReadyCycles[slot];
    const uint64_t row_wait =
        row_ready_cycle && now > row_ready_cycle ? now - row_ready_cycle : 0;
    const uint64_t vec_wait =
        vec_ready_cycle && now > vec_ready_cycle ? now - vec_ready_cycle : 0;
    const bool dep_ready_before_all =
        dep_ready_cycle &&
        ((!all_fields_ready) ||
         (all_ready_cycle && dep_ready_cycle < all_ready_cycle));
    const bool execute_before_all =
        !all_fields_ready || (all_ready_cycle && now < all_ready_cycle);
    const bool complete_before_all =
        all_ready_cycle && now + pathADotpLatency() <= all_ready_cycle;
    if (auto *local_spm = getCfdLocalSpm()) {
        local_spm->recordPathAEarlyDotpStart(
            !all_fields_ready, row_wait, vec_wait);
        local_spm->recordPathAEarlyDotpDetail(
            dep_ready_before_all, execute_before_all, complete_before_all,
            dep_ready_cycle && now > dep_ready_cycle ?
            now - dep_ready_cycle : 0);
        if (lane == 0 && all_fields_ready)
            local_spm->recordPathAWholeSlotBarrierStall(1);
        local_spm->recordCfdTrace(
            "PathA", "dotp_stream", cfdDynInstSeqNum(xc), slot, -1, lane, -1,
            row_ready_cycle, vec_ready_cycle, all_ready_cycle,
            execute_before_all ? "execute_before_all" : "execute_after_all");
    }

    double dot_product = 0.0;
    for (int j = 0; j < 5; ++j)
        dot_product += entry.row[lane][j] * entry.vec[j];

    auto &result_slot = pathAWriteSlot(slot, lane);
    result_slot.lane[lane] = dot_product;
    result_slot.ready[lane] = true;
    result_slot.seq[lane] = cfdDynInstSeqNum(xc);
    if (auto *local_spm = getCfdLocalSpm()) {
        local_spm->recordPathABufferWrite();
        if (pathAResultSlotsLive >= 2)
            local_spm->recordPathABufferOverlapCycle();
    }

    uint64_t result_bits;
    std::memcpy(&result_bits, &dot_product, sizeof(result_bits));
    xc->setRegOperand(this, 0, &result_bits);
    return NoFault;
}

std::string
DSAPathADotpStream::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "patha_dotp_stream\tslot" << (int)slot
       << ", #" << (int)lane;
    return ss.str();
}

DSAPathAStore5Result::DSAPathAStore5Result(ExtMachInst machInst,
                                           uint8_t _slot, RegIndex _base)
    : CFDDSABase("patha_store5_result", machInst, CFDDSAStore5Op),
      slot(_slot), base(_base)
{
    // CPU-local DSA store path.  This writes result memory functionally from
    // the Path A result buffer while participating in O3 store ordering.
    if (!pathAAsyncStoreEnabled())
        flags[IsStore] = 1;

    _numSrcRegs = 6;
    srcRegIdxArr[0] = intRegClass[base];
    for (int i = 0; i < 5; ++i)
        srcRegIdxArr[i + 1] =
            intRegClass[pathAStreamTokenRegForSlotLane(slot, i)];

    _numDestRegs = 2;
    destRegIdxArr[0] = intRegClass[base];
    destRegIdxArr[1] =
        intRegClass[pathAReuseTokenRegForSlot(slot)];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

static void
pathAPrepareStore5Payload(const StaticInst *inst, ExecContext *xc,
                          trace::InstRecord *traceData, uint8_t slot,
                          Addr &dst, double payload[5])
{
    dst = xc->getRegOperand(inst, 0);
    uint64_t acc_bits[5] = {};
    for (int i = 0; i < 5; ++i)
        xc->getRegOperand(inst, i + 1, &acc_bits[i]);

    const auto &queue = pathAResultQueues[slot];
    const PathAResultSlot empty_slot;
    const auto &result_slot = queue.empty() ? empty_slot : queue.front();
    bool complete = result_slot.valid;
    for (int i = 0; i < 5; ++i)
        complete = complete && result_slot.ready[i];

    if (complete) {
        for (int i = 0; i < 5; ++i)
            payload[i] = result_slot.lane[i];
    } else {
        for (int i = 0; i < 5; ++i)
            std::memcpy(&payload[i], &acc_bits[i], sizeof(double));
        // The architectural dependency token path is authoritative for the
        // fallback payload.  This counter catches internal FIFO bookkeeping
        // drift while preserving correctness for diagnostic runs.
        if (auto *local_spm = getCfdLocalSpm())
            local_spm->recordPathADotpBlockedByPackStoreBackpressure();
    }

    if (traceData)
        traceData->setMem(dst, SPM_STRIDE_BYTES, 0);

    if (auto *local_spm = getCfdLocalSpm()) {
        local_spm->recordPathAAsyncStoreEnqueue(1);
        local_spm->recordPathAAsyncStoreCommit(0);
        if (complete)
            local_spm->recordPathABufferPack();
    }
    if (complete)
        pathAPopReadSlot(slot);

    auto &input = pathAInputSlots[slot];
    if (input.valid && pathAInputSlotsLive > 0) {
        input = PathAInputSlot{};
        pathAFieldReadyCycles[slot] = {};
        pathAAllFieldsReadyCycles[slot] = 0;
        pathAInputSlotsLive--;
        if (auto *local_spm = getCfdLocalSpm())
            local_spm->recordPathAInputBufferFree(pathAInputSlotsLive);
    }

    Addr next = dst + SPM_STRIDE_BYTES;
    const uint64_t reuse_token = cfdDynInstSeqNum(xc);
    if (auto *local_spm = getCfdLocalSpm()) {
        local_spm->recordCfdTrace(
            "PathA", "store5_result", reuse_token, slot, -1, -1, -1,
            0, 0, 0, "slot_release");
    }
    xc->setRegOperand(inst, 0, &next);
    xc->setRegOperand(inst, 1, &reuse_token);
}

Fault
DSAPathAStore5Result::initiateAcc(ExecContext *xc,
                                  trace::InstRecord *traceData) const
{
    Addr dst = 0;
    double payload[5] = {};
    pathAPrepareStore5Payload(this, xc, traceData, slot, dst, payload);

    const std::vector<bool> byte_enable(SPM_STRIDE_BYTES, true);
    return xc->writeMem(reinterpret_cast<uint8_t *>(payload), sizeof(payload),
                        dst, Request::Flags(0), nullptr, byte_enable);
}

Fault
DSAPathAStore5Result::execute(ExecContext *xc,
                              trace::InstRecord *traceData) const
{
    Addr dst = 0;
    double payload[5] = {};
    pathAPrepareStore5Payload(this, xc, traceData, slot, dst, payload);

    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.writeBlob(dst, payload, sizeof(payload));
    return NoFault;
}

std::string
DSAPathAStore5Result::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "patha_store5_result\tslot" << (int)slot
       << ", [x" << (int)base << "], #40";
    return ss.str();
}

DSAPathAStoreDrain::DSAPathAStoreDrain(ExtMachInst machInst, RegIndex _base)
    : CFDDSABase("patha_store_drain", machInst, IntAluOp),
      base(_base)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[base];
    _numDestRegs = 0;

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSAPathAStoreDrain::execute(ExecContext *xc,
                            trace::InstRecord *traceData) const
{
    // Read the post-incremented result pointer so the drain depends on the
    // final async store5 in the stream.  The serialize/non-spec flags make this
    // a once-per-kernel visibility point rather than a per-matvec hot-path
    // penalty.
    uint64_t ignored = 0;
    xc->getRegOperand(this, 0, &ignored);
    return NoFault;
}

std::string
DSAPathAStoreDrain::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "patha_store_drain\tx" << (int)base;
    return ss.str();
}

// ================================================================
// DSATrsv5LuSPM — independent FP64 TRSV5 instruction (Step2)
// ================================================================

DSATrsv5LuSPM::DSATrsv5LuSPM(ExtMachInst machInst, RegIndex _zd,
                             RegIndex _luBase, RegIndex _rhsBase)
    : CFDDSABase("trsv5_lu_spm", machInst, CFDDSATrsv5Op),
      zd(_zd), luBase(_luBase), rhsBase(_rhsBase)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 2;
    srcRegIdxArr[0] = intRegClass[luBase];
    srcRegIdxArr[1] = intRegClass[rhsBase];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSATrsv5LuSPM::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    const Addr lu_ea = xc->getRegOperand(this, 0);
    const Addr rhs_ea = xc->getRegOperand(this, 1);
    if (traceData)
        traceData->setMem(lu_ea, 5 * SPM_STRIDE_BYTES, 0);

    double lu[5][5] = {};
    double value[5] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(lu_ea, lu, sizeof(lu));
    proxy.readBlob(rhs_ea, value, sizeof(value));

    cfdTrsv5Solve(&lu[0][0], value);

    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();
    for (int i = 0; i < 5; ++i)
        rd[i] = value[i];

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordTrsv5Execute(cfdDynInstSeqNum(xc),
                                      trsv5LatencyCycles());

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSATrsv5LuSPM::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "trsv5_lu_spm\tz" << (int)zd
       << ", [x" << (int)luBase << "], [x" << (int)rhsBase << "]";
    return ss.str();
}

// ================================================================
// DSATrsv5LuZRHS — TRSV5 with a forwarded Z-register RHS.
// ================================================================

DSATrsv5LuZRHS::DSATrsv5LuZRHS(ExtMachInst machInst, RegIndex _zd,
                               RegIndex _luBase, RegIndex _zrhs)
    : CFDDSABase("trsv5_lu_spm_zrhs", machInst, CFDDSATrsv5Op),
      zd(_zd), luBase(_luBase), zrhs(_zrhs)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 2;
    srcRegIdxArr[0] = intRegClass[luBase];
    srcRegIdxArr[1] = vecRegClass[zrhs];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSATrsv5LuZRHS::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    const Addr lu_ea = xc->getRegOperand(this, 0);
    if (traceData)
        traceData->setMem(lu_ea, 5 * SPM_STRIDE_BYTES, 0);

    double lu[5][5] = {};
    double value[5] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(lu_ea, lu, sizeof(lu));

    VecRegContainer rhs_reg;
    xc->getRegOperand(this, 1, &rhs_reg);
    const double *rhs = rhs_reg.as<const double>();
    for (int i = 0; i < 5; ++i)
        value[i] = rhs[i];

    cfdTrsv5Solve(&lu[0][0], value);

    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();
    for (int i = 0; i < 5; ++i)
        rd[i] = value[i];

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordTrsv5RhsForwardedExecute(cfdDynInstSeqNum(xc),
                                                  trsv5LatencyCycles());

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSATrsv5LuZRHS::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "trsv5_lu_spm_zrhs\tz" << (int)zd
       << ", [x" << (int)luBase << "], z" << (int)zrhs;
    return ss.str();
}

// ================================================================
// DSATrsm5MrhsSPM — fixed 5x5 multi-RHS preprocessing solve.
// ================================================================

DSATrsm5MrhsSPM::DSATrsm5MrhsSPM(ExtMachInst machInst, RegIndex _luBase,
                                 RegIndex _rhsBase, RegIndex _outBase)
    : CFDDSABase("trsm5_mrhs_spm", machInst, CFDDSATrsm5MrhsOp),
      luBase(_luBase), rhsBase(_rhsBase), outBase(_outBase)
{
    flags[IsFloating] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    _numSrcRegs = 3;
    srcRegIdxArr[0] = intRegClass[luBase];
    srcRegIdxArr[1] = intRegClass[rhsBase];
    srcRegIdxArr[2] = intRegClass[outBase];
    _numDestRegs = 0;

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSATrsm5MrhsSPM::execute(ExecContext *xc,
                         trace::InstRecord *traceData) const
{
    const Addr lu_ea = xc->getRegOperand(this, 0);
    const Addr rhs_ea = xc->getRegOperand(this, 1);
    const Addr out_ea = xc->getRegOperand(this, 2);
    if (traceData)
        traceData->setMem(lu_ea, 25 * sizeof(double), 0);

    double lu[25] = {};
    double rhs[25] = {};
    double out[25] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(lu_ea, lu, sizeof(lu));
    proxy.readBlob(rhs_ea, rhs, sizeof(rhs));

    for (int col = 0; col < 5; ++col) {
        double value[5];
        for (int row = 0; row < 5; ++row)
            value[row] = rhs[row * 5 + col];
        cfdTrsv5Solve(lu, value);
        for (int row = 0; row < 5; ++row)
            out[row * 5 + col] = value[row];
    }
    proxy.writeBlob(out_ea, out, sizeof(out));

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordTrsm5MrhsExecute(cfdDynInstSeqNum(xc),
                                         trsm5MrhsLatencyCycles());
    return NoFault;
}

std::string
DSATrsm5MrhsSPM::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "trsm5_mrhs_spm\t[x" << (int)outBase << "], [x"
       << (int)luBase << "], [x" << (int)rhsBase << "]";
    return ss.str();
}

// ================================================================
// DSATrsm5InvLbarSPM -- one-LU-read dual-batch coefficient solve.
// ================================================================

DSATrsm5InvLbarSPM::DSATrsm5InvLbarSPM(
    ExtMachInst machInst, RegIndex _luBase, RegIndex _cBase,
    RegIndex _dInvBase, RegIndex _lBarBase)
    : CFDDSABase("trsm5_inv_lbar_spm", machInst,
                 CFDDSATrsm5InvLbarOp),
      luBase(_luBase), cBase(_cBase), dInvBase(_dInvBase),
      lBarBase(_lBarBase)
{
    flags[IsFloating] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    _numSrcRegs = 5;
    srcRegIdxArr[0] = intRegClass[luBase];
    srcRegIdxArr[1] = intRegClass[cBase];
    srcRegIdxArr[2] = intRegClass[dInvBase];
    srcRegIdxArr[3] = intRegClass[lBarBase];
    srcRegIdxArr[4] = intRegClass[26];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[25];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSATrsm5InvLbarSPM::execute(ExecContext *xc,
                            trace::InstRecord *traceData) const
{
    const Addr lu_ea = xc->getRegOperand(this, 0);
    const Addr c_ea = xc->getRegOperand(this, 1);
    const Addr d_inv_ea = xc->getRegOperand(this, 2);
    const Addr l_bar_ea = xc->getRegOperand(this, 3);
    const uint64_t input_token = xc->getRegOperand(this, 4);
    if (traceData)
        traceData->setMem(lu_ea, 25 * sizeof(double), 0);

    double lu[25] = {};
    double c[25] = {};
    double d_inv[25] = {};
    double l_bar[25] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(lu_ea, lu, sizeof(lu));
    proxy.readBlob(c_ea, c, sizeof(c));

    for (int col = 0; col < 5; ++col) {
        double value[5] = {};
        value[col] = 1.0;
        cfdTrsv5Solve(lu, value);
        for (int row = 0; row < 5; ++row)
            d_inv[row * 5 + col] = value[row];
    }
    for (int col = 0; col < 5; ++col) {
        double value[5];
        for (int row = 0; row < 5; ++row)
            value[row] = c[row * 5 + col];
        cfdTrsv5Solve(lu, value);
        for (int row = 0; row < 5; ++row)
            l_bar[row * 5 + col] = value[row];
    }

    proxy.writeBlob(d_inv_ea, d_inv, sizeof(d_inv));
    proxy.writeBlob(l_bar_ea, l_bar, sizeof(l_bar));
    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordTrsm5InvLbarExecute(
            cfdDynInstSeqNum(xc), trsm5InvLbarLatencyCycles());
    xc->setRegOperand(this, 0, input_token + 1);
    return NoFault;
}

std::string
DSATrsm5InvLbarSPM::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "trsm5_inv_lbar_spm\t[x" << (int)dInvBase << "], [x"
       << (int)lBarBase << "], [x" << (int)luBase << "], [x"
       << (int)cBase << "]";
    return ss.str();
}

// ================================================================
// DSATrsm5Coeff3SPM -- one-LU-read INV/LBAR/UBAR coefficient solve.
// ================================================================

DSATrsm5Coeff3SPM::DSATrsm5Coeff3SPM(ExtMachInst machInst)
    : CFDDSABase("trsm5_coeff3_spm", machInst, CFDDSATrsm5Coeff3Op)
{
    flags[IsFloating] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    // Fixed interface: x10=LU, x11=L, x12=U, x13=D_inv, x14=L_bar,
    // x15=U_bar, x26=input token, x25=completion token.  x16..x20 are
    // intentionally avoided because Path A owns that accumulator window.
    _numSrcRegs = 7;
    for (int i = 0; i < 6; ++i)
        srcRegIdxArr[i] = intRegClass[10 + i];
    srcRegIdxArr[6] = intRegClass[26];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[25];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSATrsm5Coeff3SPM::execute(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    Addr addr[6];
    for (int i = 0; i < 6; ++i)
        addr[i] = xc->getRegOperand(this, i);
    const uint64_t input_token = xc->getRegOperand(this, 6);
    if (traceData)
        traceData->setMem(addr[0], 25 * sizeof(double), 0);

    double lu[25] = {};
    double lower[25] = {};
    double upper[25] = {};
    double d_inv[25] = {};
    double l_bar[25] = {};
    double u_bar[25] = {};
    SETranslatingPortProxy proxy(xc->tcBase(), SETranslatingPortProxy::Never);
    proxy.readBlob(addr[0], lu, sizeof(lu));
    proxy.readBlob(addr[1], lower, sizeof(lower));
    proxy.readBlob(addr[2], upper, sizeof(upper));

    for (int batch = 0; batch < 3; ++batch) {
        double *out = batch == 0 ? d_inv : (batch == 1 ? l_bar : u_bar);
        const double *rhs = batch == 1 ? lower : upper;
        for (int col = 0; col < 5; ++col) {
            double value[5] = {};
            for (int row = 0; row < 5; ++row)
                value[row] = batch == 0 ? (row == col ? 1.0 : 0.0) :
                    rhs[row * 5 + col];
            cfdTrsv5Solve(lu, value);
            for (int row = 0; row < 5; ++row)
                out[row * 5 + col] = value[row];
        }
    }

    proxy.writeBlob(addr[3], d_inv, sizeof(d_inv));
    proxy.writeBlob(addr[4], l_bar, sizeof(l_bar));
    proxy.writeBlob(addr[5], u_bar, sizeof(u_bar));
    if (auto *local_spm = getCfdLocalSpm())
        local_spm->recordTrsm5Coeff3Execute(
            cfdDynInstSeqNum(xc), trsm5Coeff3LatencyCycles());
    xc->setRegOperand(this, 0, input_token + 1);
    return NoFault;
}

std::string
DSATrsm5Coeff3SPM::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    return "trsm5_coeff3_spm\t[x13], [x14], [x15], [x10], [x11], [x12]";
}

// ================================================================
// DSAVec5SubZ — register-resident five-lane FP64 subtract.
// ================================================================

DSAVec5SubZ::DSAVec5SubZ(ExtMachInst machInst, RegIndex _zd,
                         RegIndex _zsrc0, RegIndex _zsrc1)
    : CFDDSABase("vec5_sub_z", machInst, CFDDSAVec5Op),
      zd(_zd), zsrc0(_zsrc0), zsrc1(_zsrc1)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;
    _numSrcRegs = 2;
    srcRegIdxArr[0] = vecRegClass[zsrc0];
    srcRegIdxArr[1] = vecRegClass[zsrc1];
    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSAVec5SubZ::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    VecRegContainer lhs_reg;
    VecRegContainer rhs_reg;
    VecRegContainer result;
    xc->getRegOperand(this, 0, &lhs_reg);
    xc->getRegOperand(this, 1, &rhs_reg);
    result.zero();
    cfdVec5Sub(result.as<double>(), lhs_reg.as<const double>(),
               rhs_reg.as<const double>());
    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSAVec5SubZ::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vec5_sub_z\tz" << (int)zd << ", z" << (int)zsrc0
       << ", z" << (int)zsrc1;
    return ss.str();
}

// ================================================================
// DSALineBufRead5 / DSALineBufWrite5 — LU-SGS line-buffer forwarding.
// ================================================================

DSALineBufRead5::DSALineBufRead5(ExtMachInst machInst, RegIndex _zd,
                                 RegIndex _lineReg, RegIndex _cellReg,
                                 bool _forward)
    : CFDDSABase(_forward ? "linebuf_rd_fwd5" : "linebuf_rd_bwd5",
                 machInst, CFDDSALineBufOp),
      zd(_zd), lineReg(_lineReg), cellReg(_cellReg), forward(_forward)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 2;
    srcRegIdxArr[0] = intRegClass[lineReg];
    srcRegIdxArr[1] = intRegClass[cellReg];

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSALineBufRead5::execute(ExecContext *xc,
                         trace::InstRecord *traceData) const
{
    (void)traceData;
    const uint64_t line_id = xc->getRegOperand(this, 0);
    const uint64_t cell_id = xc->getRegOperand(this, 1);

    double value[5] = {};
    if (auto *local_spm = getCfdLocalSpm())
        local_spm->readLineBuffer(forward, line_id, cell_id, value);

    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();
    for (int i = 0; i < 5; ++i)
        rd[i] = value[i];

    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string
DSALineBufRead5::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << (forward ? "linebuf_rd_fwd5" : "linebuf_rd_bwd5")
       << "\tz" << (int)zd << ", x" << (int)lineReg
       << ", x" << (int)cellReg;
    return ss.str();
}

DSALineBufWrite5::DSALineBufWrite5(ExtMachInst machInst, RegIndex _zsrc,
                                   RegIndex _lineReg, RegIndex _cellReg,
                                   bool _forward)
    : CFDDSABase(_forward ? "linebuf_wr_fwd5" : "linebuf_wr_bwd5",
                 machInst, CFDDSALineBufOp),
      zsrc(_zsrc), lineReg(_lineReg), cellReg(_cellReg), forward(_forward)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 3;
    srcRegIdxArr[0] = intRegClass[lineReg];
    srcRegIdxArr[1] = intRegClass[cellReg];
    srcRegIdxArr[2] = vecRegClass[zsrc];

    _numDestRegs = 0;

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSALineBufWrite5::execute(ExecContext *xc,
                          trace::InstRecord *traceData) const
{
    (void)traceData;
    const uint64_t line_id = xc->getRegOperand(this, 0);
    const uint64_t cell_id = xc->getRegOperand(this, 1);

    VecRegContainer src_reg;
    xc->getRegOperand(this, 2, &src_reg);
    const double *src = src_reg.as<const double>();

    if (auto *local_spm = getCfdLocalSpm())
        local_spm->writeLineBuffer(forward, line_id, cell_id, src);

    return NoFault;
}

std::string
DSALineBufWrite5::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << (forward ? "linebuf_wr_fwd5" : "linebuf_wr_bwd5")
       << "\tz" << (int)zsrc << ", x" << (int)lineReg
       << ", x" << (int)cellReg;
    return ss.str();
}

// ================================================================
// LU-SGS Step3 macro-controller launch/wait
// ================================================================

DSALusgsLaunch::DSALusgsLaunch(ExtMachInst machInst, RegIndex _tokenDest,
                               RegIndex _descBase)
    : CFDDSABase("cfd_lusgs_launch", machInst, CFDDSALusgsCtrlOp),
      tokenDest(_tokenDest), descBase(_descBase)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[descBase];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[tokenDest];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSALusgsLaunch::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    const Addr desc_ea = xc->getRegOperand(this, 0);
    if (traceData)
        traceData->setMem(desc_ea, sizeof(CfdLusgsDescriptor), 0);
    const uint64_t token = cfdLusgsLaunch(xc, desc_ea);
    xc->setRegOperand(this, 0, &token);
    return NoFault;
}

std::string
DSALusgsLaunch::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfd_lusgs_launch\tx" << (int)tokenDest
       << ", [x" << (int)descBase << "]";
    return ss.str();
}

DSALusgsWait::DSALusgsWait(ExtMachInst machInst, RegIndex _statusDest,
                           RegIndex _tokenReg)
    : CFDDSABase("cfd_lusgs_wait", machInst, CFDDSALusgsCtrlOp),
      statusDest(_statusDest), tokenReg(_tokenReg)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;
    flags[IsSerializeBefore] = 1;
    flags[IsSerializeAfter] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[tokenReg];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[statusDest];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSALusgsWait::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    const uint64_t token = xc->getRegOperand(this, 0);
    const uint64_t status = cfdLusgsWait(token);
    xc->setRegOperand(this, 0, &status);
    return NoFault;
}

std::string
DSALusgsWait::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfd_lusgs_wait\tx" << (int)statusDest
       << ", x" << (int)tokenReg;
    return ss.str();
}

// ================================================================
// Fine-grained coefficient preprocessing controller launch/wait
// ================================================================

DSACoeffPreprocessLaunch::DSACoeffPreprocessLaunch(
    ExtMachInst machInst, RegIndex _tokenDest, RegIndex _descBase)
    : CFDDSABase("cfd_coeff_pre_launch", machInst, CFDDSALusgsCtrlOp),
      tokenDest(_tokenDest), descBase(_descBase)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[descBase];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[tokenDest];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACoeffPreprocessLaunch::execute(
    ExecContext *xc, trace::InstRecord *traceData) const
{
    const Addr desc_ea = xc->getRegOperand(this, 0);
    if (traceData)
        traceData->setMem(desc_ea, sizeof(CfdCoeffPreprocessDescriptor), 0);
    const uint64_t token = cfdCoeffPreprocessEventEnabled() ?
        cfdCoeffPreprocessLaunch(xc, desc_ea) : 0;
    xc->setRegOperand(this, 0, &token);
    return NoFault;
}

std::string
DSACoeffPreprocessLaunch::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfd_coeff_pre_launch\tx" << (int)tokenDest
       << ", [x" << (int)descBase << "]";
    return ss.str();
}

DSACoeffPreprocessWait::DSACoeffPreprocessWait(
    ExtMachInst machInst, RegIndex _statusDest, RegIndex _tokenReg)
    : CFDDSABase("cfd_coeff_pre_wait", machInst, CFDDSALusgsCtrlOp),
      statusDest(_statusDest), tokenReg(_tokenReg)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;

    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[tokenReg];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[statusDest];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACoeffPreprocessWait::execute(
    ExecContext *xc, trace::InstRecord *traceData) const
{
    const uint64_t token = xc->getRegOperand(this, 0);
    const uint64_t status = cfdCoeffPreprocessWait(token);
    xc->setRegOperand(this, 0, &status);
    return NoFault;
}

std::string
DSACoeffPreprocessWait::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfd_coeff_pre_wait\tx" << (int)statusDest
       << ", x" << (int)tokenReg;
    return ss.str();
}

DSACoeffPreprocessCancel::DSACoeffPreprocessCancel(
    ExtMachInst machInst, RegIndex _statusDest, RegIndex _tokenReg)
    : CFDDSABase("cfd_coeff_cancel", machInst, CFDDSALusgsCtrlOp),
      statusDest(_statusDest), tokenReg(_tokenReg)
{
    flags[IsInteger] = 1;
    flags[IsNonSpeculative] = 1;
    _numSrcRegs = 1;
    srcRegIdxArr[0] = intRegClass[tokenReg];
    _numDestRegs = 1;
    destRegIdxArr[0] = intRegClass[statusDest];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACoeffPreprocessCancel::execute(
    ExecContext *xc, trace::InstRecord *traceData) const
{
    const uint64_t token = xc->getRegOperand(this, 0);
    const uint64_t status = cfdCoeffPreprocessCancel(token);
    xc->setRegOperand(this, 0, &status);
    return NoFault;
}

std::string
DSACoeffPreprocessCancel::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfd_coeff_cancel\tx" << (int)statusDest
       << ", x" << (int)tokenReg;
    return ss.str();
}

// ================================================================
// CFD restricted FMOPA-like k-step using official SME ZA state
// ================================================================

DSACFDSMEFmopa5Step::DSACFDSMEFmopa5Step(ExtMachInst machInst,
                                         RegIndex _zcol,
                                         RegIndex _zvec,
                                         uint8_t _k)
    : CFDDSABase("cfdsme_fmopa5_step", machInst, CFDSMEFmopaStepOp),
      zcol(_zcol), zvec(_zvec), k(_k)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    // Sources: zcol, zvec, and current architectural ZA accumulator.
    _numSrcRegs = 3;
    srcRegIdxArr[0] = vecRegClass[zcol];
    srcRegIdxArr[1] = vecRegClass[zvec];
    srcRegIdxArr[2] = matRegClass[0];

    // Dest: architectural ZA accumulator consumed by official SME MOVA.
    _numDestRegs = 1;
    destRegIdxArr[0] = matRegClass[0];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACFDSMEFmopa5Step::execute(ExecContext *xc,
                             trace::InstRecord *traceData) const
{
    const uint64_t svcr = xc->tcBase()->readMiscReg(MISCREG_SVCR);
    if ((svcr & 0x2) == 0)
        return smeAccessTrap(EL1, 0b11);

    VecRegContainer reg_col;
    VecRegContainer reg_vec;
    MatRegContainer za;
    xc->getRegOperand(this, 0, &reg_col);
    xc->getRegOperand(this, 1, &reg_vec);
    xc->getRegOperand(this, 2, &za);

    const double *col = reg_col.as<const double>();
    const double *vec = reg_vec.as<const double>();
    const double scalar = vec[k];

    // Strict SME outer-product step with sparse right vector:
    // left[0:4]=col[0:4], right[0]=vec[k], so only ZA[0:4,0] changes.
    auto tile = getTile<double>(za, 0);
    for (int i = 0; i < SME_MVM_DIM; i++)
        tile[i][0] += col[i] * scalar;

    // Pending-update accounting hook.  O3 matrix-register rename carries the
    // speculative ZA value in a physical matrix register; commit/squash hooks
    // retire or discard the pending update without making FMOPA non-spec.
    if (CfdLocalSpm *local_spm = getCfdLocalSpm())
        local_spm->recordZaExecute(cfdDynInstSeqNum(xc), col, scalar);

    xc->setRegOperand(this, 0, &za);
    return NoFault;
}

std::string
DSACFDSMEFmopa5Step::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfdsme_fmopa5_step\tz" << (int)zcol
       << ", z" << (int)zvec << ", #" << (int)k
       << " -> ZA[0:4,0]";
    return ss.str();
}

// ================================================================
// Path B ZA spatial partial-sum pipeline step.
// ================================================================

DSACFDSMEMvm5PipeStep::DSACFDSMEMvm5PipeStep(ExtMachInst machInst,
                                             RegIndex _zcol,
                                             RegIndex _zvec,
                                             uint8_t _k)
    : CFDDSABase("cfdsme_mvm5_pipe_step", machInst, CFDSMEPipeStepOp),
      zcol(_zcol), zvec(_zvec), k(_k)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 3;
    srcRegIdxArr[0] = vecRegClass[zcol];
    srcRegIdxArr[1] = vecRegClass[zvec];
    srcRegIdxArr[2] = matRegClass[0];

    _numDestRegs = 1;
    destRegIdxArr[0] = matRegClass[0];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACFDSMEMvm5PipeStep::execute(ExecContext *xc,
                               trace::InstRecord *traceData) const
{
    const uint64_t svcr = xc->tcBase()->readMiscReg(MISCREG_SVCR);
    if ((svcr & 0x2) == 0)
        return smeAccessTrap(EL1, 0b11);

    VecRegContainer reg_col;
    VecRegContainer reg_vec;
    MatRegContainer za;
    xc->getRegOperand(this, 0, &reg_col);
    xc->getRegOperand(this, 1, &reg_vec);
    xc->getRegOperand(this, 2, &za);

    const double *col = reg_col.as<const double>();
    const double *vec = reg_vec.as<const double>();
    const double scalar = vec[k];

    // Path B keeps partial sums inside ZA but streams them spatially across
    // columns instead of accumulating every k-step into ZA[:,0].
    auto tile = getTile<double>(za, 0);
    for (int i = 0; i < SME_MVM_DIM; i++) {
        const double prev = (k == 0) ? 0.0 : tile[i][k - 1];
        tile[i][k] = prev + col[i] * scalar;
    }

    if (CfdLocalSpm *local_spm = getCfdLocalSpm())
        local_spm->recordZaPipeExecute(cfdDynInstSeqNum(xc), k, col, scalar);

    xc->setRegOperand(this, 0, &za);
    return NoFault;
}

std::string
DSACFDSMEMvm5PipeStep::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfdsme_mvm5_pipe_step\tz" << (int)zcol
       << ", z" << (int)zvec << ", #" << (int)k
       << " -> ZA[0:4," << (int)k << "]";
    return ss.str();
}

// ================================================================
// Path C selected-column outer-product spatial pipeline step.
// ================================================================

DSACFDSMEZaOuter5Step::DSACFDSMEZaOuter5Step(ExtMachInst machInst,
                                             RegIndex _zcol,
                                             RegIndex _zvec,
                                             uint8_t _k)
    : CFDDSABase("cfdsme_za_outer5_step", machInst, CFDSMEZaOuterStepOp),
      zcol(_zcol), zvec(_zvec), k(_k)
{
    flags[IsVector] = 1;
    flags[IsFloating] = 1;

    _numSrcRegs = 3;
    srcRegIdxArr[0] = vecRegClass[zcol];
    srcRegIdxArr[1] = vecRegClass[zvec];
    srcRegIdxArr[2] = matRegClass[0];

    _numDestRegs = 1;
    destRegIdxArr[0] = matRegClass[0];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault
DSACFDSMEZaOuter5Step::execute(ExecContext *xc,
                               trace::InstRecord *traceData) const
{
    const uint64_t svcr = xc->tcBase()->readMiscReg(MISCREG_SVCR);
    if ((svcr & 0x2) == 0)
        return smeAccessTrap(EL1, 0b11);

    VecRegContainer reg_col;
    VecRegContainer reg_vec;
    MatRegContainer za;
    xc->getRegOperand(this, 0, &reg_col);
    xc->getRegOperand(this, 1, &reg_vec);
    xc->getRegOperand(this, 2, &za);

    const double *col = reg_col.as<const double>();
    const double *vec = reg_vec.as<const double>();
    const double scalar = vec[k];

    // Conceptually, each step forms outer[i][j] = A[:,k][i] * x[j].
    // The default Path C hardware model selects only outer[:,k], then
    // accumulates that selected column with the previous ZA partial sum.
    auto tile = getTile<double>(za, 0);
    for (int i = 0; i < SME_MVM_DIM; i++) {
        const double prev = (k == 0) ? 0.0 : tile[i][k - 1];
        tile[i][k] = prev + col[i] * scalar;
    }

    if (CfdLocalSpm *local_spm = getCfdLocalSpm()) {
        const unsigned slot = (zvec == 12) ? 1 : 0;
        bool all_columns_ready = pathCInputReady[slot][5];
        for (int field = 0; field < 5; ++field)
            all_columns_ready = all_columns_ready &&
                                pathCInputReady[slot][field];
        const uint64_t now = cfdCurrentCycle();
        const uint64_t col_ready_cycle = pathCInputReadyCycles[slot][k];
        const uint64_t vec_ready_cycle = pathCInputReadyCycles[slot][5];
        const uint64_t col_wait =
            col_ready_cycle && now > col_ready_cycle ?
            now - col_ready_cycle : 0;
        const uint64_t vec_wait =
            vec_ready_cycle && now > vec_ready_cycle ?
            now - vec_ready_cycle : 0;
        local_spm->recordPathCEarlyOuterStart(
            !all_columns_ready, col_wait, vec_wait, 0);
        local_spm->recordPathCOuterFuIssue(
            std::max<uint64_t>(1, cfdEnvU64("GEM5_CFD_SME_OUTER_LAT", 1)));
        local_spm->recordCfdTrace(
            "PathC", "za_outer_step", cfdDynInstSeqNum(xc), slot, -1, -1, k,
            col_ready_cycle, vec_ready_cycle, 0,
            all_columns_ready ? "execute_after_all_columns" :
                                "execute_before_all_columns");
        local_spm->recordZaOuterExecute(cfdDynInstSeqNum(xc), k, col, scalar);
    }

    xc->setRegOperand(this, 0, &za);
    return NoFault;
}

std::string
DSACFDSMEZaOuter5Step::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "cfdsme_za_outer5_step\tz" << (int)zcol
       << ", z" << (int)zvec << ", #" << (int)k
       << " -> selected outer ZA[0:4," << (int)k << "]";
    return ss.str();
}

// ================================================================
// DSAGetAcc — Legacy Shadow Accumulator Harvest (v3.0, kept for compatibility)
// ================================================================

DSAGetAcc::DSAGetAcc(ExtMachInst machInst, RegIndex _zd)
    : CFDDSABase("get_acc", machInst, IntAluOp),
      zd(_zd)
{
    _numSrcRegs = 5;
    for (int i = 0; i < 5; i++) {
        srcRegIdxArr[i] = intRegClass[16 + i];  // X16..X20 (v4.0 convention)
    }

    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zd];

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSAGetAcc::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    // Read from X16-X20 (for compatibility with v4.0)
    uint64_t acc_bits[5];
    for (int i = 0; i < 5; i++) {
        acc_bits[i] = 0;
        xc->getRegOperand(this, i, &acc_bits[i]);
    }

    VecRegContainer result;
    result.zero();
    double *rd = result.as<double>();

    for (int i = 0; i < 5; i++) {
        std::memcpy(&rd[i], &acc_bits[i], sizeof(double));
    }

    // Write to Z[zd] (the only destination)
    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string DSAGetAcc::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "get_acc\tz" << (int)zd;
    return ss.str();
}

// ================================================================
// DSAVsetZero — Zero a Z register (kept for compatibility)
// ================================================================

DSAVsetZero::DSAVsetZero(ExtMachInst machInst, RegIndex _zrd)
    : CFDDSABase("vset_zero", machInst, IntAluOp),
      zrd(_zrd)
{
    _numSrcRegs = 0;
    _numDestRegs = 1;
    destRegIdxArr[0] = vecRegClass[zrd];
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
}

Fault DSAVsetZero::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    VecRegContainer result;
    result.zero();
    xc->setRegOperand(this, 0, &result);
    return NoFault;
}

std::string DSAVsetZero::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vset_zero\tz" << (int)zrd;
    return ss.str();
}

} // namespace ArmISA
} // namespace gem5
