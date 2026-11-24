// Dedicated, bounded LU trailing-row FMA bank; parent RHS bank is unchanged.
#ifndef CFD_LU_FMA_HH
#define CFD_LU_FMA_HH
#include "arch/arm/cfd_rhs_fma.hh"
namespace gem5 { namespace ArmISA {
struct CfdLuFma : CfdRhsFma {
    // The existing 512-byte rank1 state owns row tags and issue selection.
    // Three captured operands, result, owner and return control are charged
    // conservatively at 192 bytes for each of the five slots per lane.
    static uint64_t reservation(unsigned count) {
        if (count<4 || count>16) throw std::invalid_argument("LU FMA lane count");
        return uint64_t(count)*Latency*192+256;
    }
};
} }
#endif
