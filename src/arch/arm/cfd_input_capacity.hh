#ifndef __ARCH_ARM_CFD_INPUT_CAPACITY_HH__
#define __ARCH_ARM_CFD_INPUT_CAPACITY_HH__

#include <cstdint>

namespace gem5 { namespace ArmISA {
struct CfdInputCapacity
{
    static constexpr unsigned BaseMaximum = 8;
    static constexpr unsigned ExtendedMaximum = 16;
    static constexpr unsigned SmeExtendedMaximum = 32;

    static constexpr bool allowed(unsigned level, uint64_t slots, bool extended,
                                  bool sme = false)
    {
        if (!slots) return false;
        if (extended)
            return level >= 13 && slots <= (sme ? SmeExtendedMaximum : ExtendedMaximum);
        return slots == 1 || slots == 2 || slots == 4 ||
            (level >= 13 && slots <= BaseMaximum);
    }
};
} }
#endif
