// Independent upper numerators; products and ordered subtracts remain paid.
#ifndef __ARCH_ARM_CFD_LU_UPPER_HH__
#define __ARCH_ARM_CFD_LU_UPPER_HH__
#include "arch/arm/cfd_lu_products.hh"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace gem5 { namespace ArmISA {
class CfdLuUpper {
  public:
    CfdLuProducts products;
    std::array<double,5> accumulator{};
    uint64_t eligible=0, returned=0;
    int column=-1;
    bool ready=false;

    static unsigned reservation(unsigned contexts) { return 256*contexts+128; }
    void begin(unsigned c,const std::array<double,5> &values,uint64_t now) {
        if (c>3 || int(c)!=column+1 || (column>=0 && !ready))
            throw std::logic_error("upper context reset before ordered drain");
        products.begin(c);
        column=c;accumulator=values;eligible=now+1;returned=0;ready=false;
    }
    bool advanceReady(uint64_t now) {
        if (ready || column<0 || now<eligible || products.nextSub!=products.terms)
            return false;
        ready=true;returned=now;return true;
    }
    void finishSubtract(unsigned k,const std::array<double,5> &values,uint64_t now) {
        if (column<0 || now<eligible || ready)
            throw std::logic_error("upper subtract outside a live context");
        const bool done=products.complete(k);
        accumulator=values;returned=now;ready=done;
    }
};
static_assert(sizeof(CfdLuUpper)<=256,"upper context exceeds charged reservation");
} }
#endif
