/*
 * cfd_dsa.hh -- CFD DSA Instruction Set Architecture (v4.0 SPM + Cross-Domain Regs)
 * ================================================================
 * Architecture: ARMv8-A + CFD-DSA Extension
 *
 * v4.0 Key Innovations:
 *   1. SPM (Scratchpad Memory) direct access via lmat5_spm
 *      - 40-byte stride (compact 5×FP64 layout)
 *      - Bypasses cache hierarchy, deterministic latency
 *
 *   2. Cross-domain register writes (dotp_row)
 *      - Computes FP64 result, bit-casts to uint64_t
 *      - Writes to X[16+lane_id] (integer register file)
 *      - Zero RAW dependencies between 5 dotp_row instructions
 *
 *   3. Cross-domain harvest (pack_acc)
 *      - Reads X[16]..X[20] as uint64_t bit patterns
 *      - Reinterprets as 5×FP64, packs into Z[zd]
 *
 * Register Allocation (v4.0):
 *   Z0-Z4:   Ping matrix rows
 *   Z5:      Ping vector
 *   Z6-Z10:  Pong matrix rows
 *   Z12:     Pong vector
 *   Z11:     Result register (Ping)
 *   Z13:     Result register (Pong)
 *   X16-X20: Accumulator slots (cross-domain, from dotp_row)
 *   X21/X22: Ping/Pong matrix SPM base pointers
 *   X23/X24: Ping/Pong vector SPM base pointers
 * ================================================================
 */

#ifndef __ARCH_ARM_INSTS_CFD_DSA_HH__
#define __ARCH_ARM_INSTS_CFD_DSA_HH__

#include "arch/arm/insts/static_inst.hh"
#include "arch/arm/regs/vec.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/mat.hh"

namespace gem5
{

namespace ArmISA
{

// Keep element-count and byte-count explicit to avoid unit mixups.
static constexpr int SPM_STRIDE_ELEMS = 5;                  // 5 FP64 values
static constexpr int SPM_STRIDE_BYTES = SPM_STRIDE_ELEMS * 8; // 40 bytes
// Legacy cache stride: 64 bytes (cache line aligned)
static constexpr int CACHE_STRIDE = 64;

class CFDDSABase : public ArmStaticInst
{
  public:
    CFDDSABase(const char *mnem, ExtMachInst machInst, OpClass op)
        : ArmStaticInst(mnem, machInst, op)
    {
        flags[IsVector] = 1;
    }
};

// ================================================================
// DSALmatRow -- Load one matrix row from memory (legacy cache mode)
// Encodes: [31:27]=00000 [26:25]=00 [24]=0 [23:21]=row [20]=1 [9:5]=rs1 [4:0]=zd
// Uses 64-byte stride, works with L1 cache
// ================================================================
class DSALmatRow : public CFDDSABase
{
  protected:
    RegIndex dest;
    RegIndex base;
    int8_t   row;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSALmatRow(ExtMachInst, RegIndex _dest, RegIndex _base, int8_t _row);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    Fault initiateAcc(ExecContext *, trace::InstRecord *) const override;
    Fault completeAcc(Packet *, ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSALmatRowSPM -- Load one matrix row from SPM (v4.0)
// Encodes: [31:27]=00000 [26:25]=00 [24]=0 [23:21]=row [20]=0 [9:5]=rs1 [4:0]=zd
// Uses 40-byte stride and reads from the SPM-mapped VA range.
// Address is derived from the runtime value in X[base], not register-id
// based hardcoding, so X21/X22/X23/X24 can all be used as legal bases.
// ================================================================
class DSALmatRowSPM : public CFDDSABase
{
  protected:
    RegIndex dest;      // Z register destination
    RegIndex base;      // X register: SPM base pointer
    int8_t   row;       // Row index (0-4)
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSALmatRowSPM(ExtMachInst, RegIndex _dest, RegIndex _base, int8_t _row);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    Fault initiateAcc(ExecContext *, trace::InstRecord *) const override;
    Fault completeAcc(Packet *, ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSALmatRowSPMLocal -- CPU-local timing SPM bypass path (v6.2)
//
// Same architectural semantics as lmat5_spm, but modeled as a local
// scratchpad/DSA port operation instead of a generic O3 LSQ/memory-system
// request.  The FU opLat/count provide timing and port pressure, while
// local_spm.* legacy stats provide byte/read coverage.
// ================================================================
class DSALmatRowSPMLocal : public CFDDSABase
{
  protected:
    RegIndex dest;
    RegIndex base;
    int8_t row;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSALmatRowSPMLocal(ExtMachInst, RegIndex _dest,
                       RegIndex _base, int8_t _row);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

bool cfdUseLocalSpmPort();

// ================================================================
// DSADotpRow -- Single-row dot product to INTEGER register (v4.0)
// Encodes: [31:27]=00000 [26:25]=00 [24]=1 [23]=0
//          [14:10]=zra [9:5]=zrb [4:0]=lane_id
//
// CROSS-DOMAIN WRITE: Writes FP64 result to X[16+lane_id] via bit-cast
// This enables zero RAW dependencies between 5 parallel dotp_row instructions.
// ================================================================
class DSADotpRow : public CFDDSABase
{
  protected:
    RegIndex zra, zrb;  // Source Z registers: matrix row, vector
    uint8_t  lane_id;   // Lane index (0-4) -> writes to X[16+lane_id]
    RegId srcRegIdxArr[2];   // Z[zra], Z[zrb]
    RegId destRegIdxArr[1];  // X[16+lane_id] (integer register!)

  public:
    DSADotpRow(ExtMachInst, RegIndex _zra, RegIndex _zrb, uint8_t _lane_id);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSAPackAcc -- Pack integer registers into vector register (v4.0 NEW)
// Encodes: [31:27]=00000 [26:25]=00 [24]=1 [23]=1
//          [9:5]=xn [4:0]=zd
//
// CROSS-DOMAIN READ: Reads X[xn]..X[xn+4] as uint64_t
// Reinterprets bit patterns as 5×FP64, packs into Z[zd]
// ================================================================
class DSAPackAcc : public CFDDSABase
{
  protected:
    RegIndex zd;        // Destination Z register
    RegIndex xn;        // Base X register (reads X[xn]..X[xn+4])
    static constexpr int NUM_LANES = 5;
    RegId srcRegIdxArr[NUM_LANES];   // X[xn], X[xn+1], ..., X[xn+4]
    RegId destRegIdxArr[1];           // Z[zd]

  public:
    DSAPackAcc(ExtMachInst, RegIndex _zd, RegIndex _xn);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
      std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// Pack the current Path A internal result and subtract it from a base vector.
// The base is addressed by an X register; the remaining integer sources are
// dependency tokens matching pack_acc.
class DSAPackSubAcc : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegIndex base;
    RegIndex xn;
    static constexpr int NUM_LANES = 5;
    RegId srcRegIdxArr[NUM_LANES + 1];
    RegId destRegIdxArr[1];

  public:
    DSAPackSubAcc(ExtMachInst, RegIndex _zd, RegIndex _base, RegIndex _xn);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// Path A streaming DSA instructions.
//
// These are Path A-only hardware-modeling instructions.  They do not replace
// or change lmat5_spm, dotp_row, or pack_acc.  Path C continues to use the
// public lmat5_spm -> Z register path.
// ================================================================
class DSAPathAStreamLoad5 : public CFDDSABase
{
  protected:
    uint8_t slot;
    uint8_t field;
    RegIndex base;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSAPathAStreamLoad5(ExtMachInst, uint8_t _slot,
                        uint8_t _field, RegIndex _base);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class DSAPathADotpStream : public CFDDSABase
{
  protected:
    uint8_t slot;
    uint8_t lane;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSAPathADotpStream(ExtMachInst, uint8_t _slot, uint8_t _lane);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class DSAPathAStore5Result : public CFDDSABase
{
  protected:
    uint8_t slot;
    RegIndex base;
    RegId srcRegIdxArr[6];
    RegId destRegIdxArr[2];

  public:
    DSAPathAStore5Result(ExtMachInst, uint8_t _slot, RegIndex _base);
    Fault initiateAcc(ExecContext *, trace::InstRecord *) const override;
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class DSAPathAStoreDrain : public CFDDSABase
{
  protected:
    RegIndex base;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[0];

  public:
    DSAPathAStoreDrain(ExtMachInst, RegIndex _base);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSATrsv5LuSPM -- fixed-size FP64 LU triangular solve.
//
// Encodes: [31:27]=00000 [26:25]=00 [24:23]=11 [22]=0 [21:20]=00
//          [19:15]=xlu [14:10]=xrhs [4:0]=zd
//
// Reads a compact 5x5 FP64 LU block from X[xlu], a 5-lane FP64 RHS from
// X[xrhs], and writes the solved vector to Z[zd].  It is deliberately
// independent of Path A's dotp/pack/result-buffer machinery.
// ================================================================
class DSATrsv5LuSPM : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegIndex luBase;
    RegIndex rhsBase;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSATrsv5LuSPM(ExtMachInst, RegIndex _zd,
                  RegIndex _luBase, RegIndex _rhsBase);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// TRSV5 variant whose RHS comes from an existing Z register rather than
// SPM_TRSV_RHS_BASE.  This is used by LU-SGS Step2-core forwarded mode:
// patha_pack_sub5 produces zrhs, then this instruction consumes it directly.
class DSATrsv5LuZRHS : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegIndex luBase;
    RegIndex zrhs;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSATrsv5LuZRHS(ExtMachInst, RegIndex _zd,
                   RegIndex _luBase, RegIndex _zrhs);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// Fixed 5x5 multi-RHS triangular solve used only by LU-SGS coefficient
// preprocessing.  It reads LU(D) and a 5x5 RHS block from SPM and writes the
// five solved columns to a separate 5x5 SPM output block.
class DSATrsm5MrhsSPM : public CFDDSABase
{
  protected:
    RegIndex luBase;
    RegIndex rhsBase;
    RegIndex outBase;
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[0];

  public:
    DSATrsm5MrhsSPM(ExtMachInst, RegIndex _luBase,
                    RegIndex _rhsBase, RegIndex _outBase);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// Cell-local dual-batch preprocessing operation.  LU(D) is read once, the
// identity RHS is generated internally, and D_inv/L_bar are produced with the
// exact shared TRSV5 column solve order.
class DSATrsm5InvLbarSPM : public CFDDSABase
{
  protected:
    RegIndex luBase;
    RegIndex cBase;
    RegIndex dInvBase;
    RegIndex lBarBase;
    RegId srcRegIdxArr[5];
    RegId destRegIdxArr[1];

  public:
    DSATrsm5InvLbarSPM(ExtMachInst, RegIndex _luBase,
                       RegIndex _cBase, RegIndex _dInvBase,
                       RegIndex _lBarBase);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr,
        const loader::SymbolTable *) const override;
};

// Cell-local three-batch coefficient preprocessing.  LU(D) is read once,
// identity columns are generated internally, and D_inv/L_bar/U_bar are
// produced with the shared TRSV5 column solve order.
class DSATrsm5Coeff3SPM : public CFDDSABase
{
  protected:
    RegId srcRegIdxArr[7];
    RegId destRegIdxArr[1];

  public:
    explicit DSATrsm5Coeff3SPM(ExtMachInst);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr,
        const loader::SymbolTable *) const override;
};

// Five-lane FP64 register subtract.  Lanes 5..7 are architecturally zeroed.
class DSAVec5SubZ : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegIndex zsrc0;
    RegIndex zsrc1;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSAVec5SubZ(ExtMachInst, RegIndex _zd,
                RegIndex _zsrc0, RegIndex _zsrc1);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// LU-SGS Step2 line-buffer forwarding.  These instructions replace the
// software ForwardLineContext/BackwardLineContext vector staging path with a
// small gem5-side hardware buffer indexed by line_id % entries.
class DSALineBufRead5 : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegIndex lineReg;
    RegIndex cellReg;
    bool forward;
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  public:
    DSALineBufRead5(ExtMachInst, RegIndex _zd, RegIndex _lineReg,
                    RegIndex _cellReg, bool _forward);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class DSALineBufWrite5 : public CFDDSABase
{
  protected:
    RegIndex zsrc;
    RegIndex lineReg;
    RegIndex cellReg;
    bool forward;
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[0];

  public:
    DSALineBufWrite5(ExtMachInst, RegIndex _zsrc, RegIndex _lineReg,
                     RegIndex _cellReg, bool _forward);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSALusgsLaunch / DSALusgsWait -- LU-SGS Step3 macro controller.
//
// Encodes in the [24:23]=11 [22]=0 [21:20]=00 space with explicit
// [19:15] function tags.  The tags keep these instructions disjoint from
// TRSV5 and the legacy pack_acc fallback.
//
//   launch: [19:15]=11111 [14:10]=xdesc [9:5]=00001 [4:0]=xtok
//   wait:   [19:15]=11110 [14:10]=xtok  [9:5]=00010 [4:0]=xstatus
//
// Both are non-speculative serialized operations.  Launch runs the coarse
// controller only when the instruction reaches the commit point; wait verifies
// and clears the completion token.
// ================================================================
class DSALusgsLaunch : public CFDDSABase
{
  protected:
    RegIndex tokenDest;
    RegIndex descBase;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSALusgsLaunch(ExtMachInst, RegIndex _tokenDest, RegIndex _descBase);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

class DSALusgsWait : public CFDDSABase
{
  protected:
    RegIndex statusDest;
    RegIndex tokenReg;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSALusgsWait(ExtMachInst, RegIndex _statusDest, RegIndex _tokenReg);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// Fine-grained coefficient preprocessing controller.  These operations are
// non-speculative, but deliberately avoid global serialize-after barriers:
// the returned token carries the request-local dependency.
class DSACoeffPreprocessLaunch : public CFDDSABase
{
  protected:
    RegIndex tokenDest;
    RegIndex descBase;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSACoeffPreprocessLaunch(ExtMachInst, RegIndex _tokenDest,
                             RegIndex _descBase);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr,
        const loader::SymbolTable *) const override;
};

class DSACoeffPreprocessWait : public CFDDSABase
{
  protected:
    RegIndex statusDest;
    RegIndex tokenReg;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSACoeffPreprocessWait(ExtMachInst, RegIndex _statusDest,
                           RegIndex _tokenReg);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr,
        const loader::SymbolTable *) const override;
};

class DSACoeffPreprocessCancel : public CFDDSABase
{
  protected:
    RegIndex statusDest;
    RegIndex tokenReg;
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  public:
    DSACoeffPreprocessCancel(ExtMachInst, RegIndex _statusDest,
                             RegIndex _tokenReg);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr,
        const loader::SymbolTable *) const override;
};

// ================================================================
// DSACFDSMEFmopa5Step -- restricted SME FMOPA-like k-step
// Encodes: [31:27]=00000 [26:25]=00 [24:23]=11 [22]=1 [21]=0
//          [20:18]=k [17:15]=101 [14:10]=zcol [9:5]=zvec [4:0]=0
//
// Path C: each step directly accumulates into architectural ZA.
//   left[0:4]=zcol[0:4], right[0]=zvec[k]
//   ZA[0:4,0] += left[0:4] * right[0]
// ================================================================
class DSACFDSMEFmopa5Step : public CFDDSABase
{
  protected:
    RegIndex zcol;
    RegIndex zvec;
    uint8_t k;
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];

  public:
    DSACFDSMEFmopa5Step(ExtMachInst, RegIndex _zcol,
                        RegIndex _zvec, uint8_t _k);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSACFDSMEMvm5PipeStep -- Path B ZA spatial partial-sum pipeline
// Encodes: [31:27]=00000 [26:25]=00 [24:23]=11 [22]=1 [21]=1
//          [20:18]=k [17:15]=110 [14:10]=zcol [9:5]=zvec [4:0]=0
//
// Path B: partial sums flow along ZA columns.
//   k=0: ZA[0:4,0] = zcol[0:4] * zvec[0]
//   k>0: ZA[0:4,k] = ZA[0:4,k-1] + zcol[0:4] * zvec[k]
// Final result is read from ZA[0:4,4] by the Path B MOVA path.
// ================================================================
class DSACFDSMEMvm5PipeStep : public CFDDSABase
{
  protected:
    RegIndex zcol;
    RegIndex zvec;
    uint8_t k;
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];

  public:
    DSACFDSMEMvm5PipeStep(ExtMachInst, RegIndex _zcol,
                          RegIndex _zvec, uint8_t _k);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSACFDSMEZaOuter5Step -- Path C selected-column outer-product
// spatial partial-sum pipeline
// Encodes: [31:27]=00000 [26:25]=00 [24:23]=11 [22]=1 [21]=0
//          [20:18]=k [17:15]=110 [14:10]=zcol [9:5]=zvec [4:0]=0
//
// Conceptual outer product: outer[i][j] = zcol[i] * zvec[j].
// Default effective update selects outer[:,k] and accumulates through ZA:
//   k=0: ZA[0:4,0] = zcol[0:4] * zvec[0]
//   k>0: ZA[0:4,k] = ZA[0:4,k-1] + zcol[0:4] * zvec[k]
// Final result is read from ZA[0:4,4] by the Path C outer MOVA path.
// ================================================================
class DSACFDSMEZaOuter5Step : public CFDDSABase
{
  protected:
    RegIndex zcol;
    RegIndex zvec;
    uint8_t k;
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];

  public:
    DSACFDSMEZaOuter5Step(ExtMachInst, RegIndex _zcol,
                          RegIndex _zvec, uint8_t _k);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSAVsetZero -- Zero a Z register (kept for compatibility)
// ================================================================
class DSAVsetZero : public CFDDSABase
{
  protected:
    RegIndex zrd;
    RegId srcRegIdxArr[0];
    RegId destRegIdxArr[1];

  public:
    DSAVsetZero(ExtMachInst, RegIndex _zrd);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

// ================================================================
// DSAGetAcc -- Harvest Shadow Accumulator (legacy, v3.0)
// Kept for backward compatibility
// ================================================================
class DSAGetAcc : public CFDDSABase
{
  protected:
    RegIndex zd;
    RegId srcRegIdxArr[5];
    RegId destRegIdxArr[1];

  public:
    DSAGetAcc(ExtMachInst, RegIndex _zd);
    Fault execute(ExecContext *, trace::InstRecord *) const override;
    std::string generateDisassembly(Addr, const loader::SymbolTable *) const override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_INSTS_CFD_DSA_HH__
