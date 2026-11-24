// Proposed RHS FMA contract, initially tested outside the active gem5 model.
#ifndef CFD_RHS_FMA_CONTRACT_HH
#define CFD_RHS_FMA_CONTRACT_HH
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
struct CfdRhsFma {
    static constexpr unsigned Latency=5, Ii=1, FactorFanoutControlBytes=128;
    static uint64_t reservation(unsigned count) {
        if (count<4 || count>16) throw std::invalid_argument("RHS FMA lane count");
        return uint64_t(count)*Latency*192+256;
    }
    static unsigned reads(unsigned width,bool broadcast) {
        if (!width || width>4) throw std::invalid_argument("RHS FMA width");
        return broadcast && width>1 ? 2*width+1 : 3*width;
    }
    static double evaluate(double coefficient,double solved,double accumulator) {
        return std::fma(-coefficient,solved,accumulator);
    }
};
} }
#endif
