#ifndef __ARCH_ARM_CFD_SPM_SLOT_ROTATION_HH__
#define __ARCH_ARM_CFD_SPM_SLOT_ROTATION_HH__
#include <cstdint>
namespace gem5 { namespace ArmISA {
struct CfdSpmSlotRotation {
    static constexpr uint64_t ControlBytes=128;
    static constexpr uint64_t shift(unsigned mode,unsigned scope,uint64_t slot) {
        return mode&scope ? 8*(slot&3) : 0;
    }
    static constexpr uint64_t input(unsigned mode,uint64_t slot) {
        return slot*8192+shift(mode,2,slot);
    }
    static constexpr uint64_t rhs(unsigned mode,uint64_t slot) {
        return slot*128+shift(mode,1,slot);
    }
};
static_assert(CfdSpmSlotRotation::shift(3,1,3)+40<=128);
static_assert(CfdSpmSlotRotation::shift(3,2,3)+640<=8192);
} }
#endif
