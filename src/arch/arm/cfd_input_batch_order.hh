#ifndef __ARCH_ARM_CFD_INPUT_BATCH_ORDER_HH__
#define __ARCH_ARM_CFD_INPUT_BATCH_ORDER_HH__

namespace gem5 { namespace ArmISA {
struct CfdInputBatchOrder
{
    // A bounded ready selector/mux. Operands remain in the paid input slot;
    // existing per-request flags and the single read handle own publication.
    static constexpr unsigned ControlBytes = 128;
    static constexpr bool allowed(unsigned level, bool batch, bool lFirst,
                                  bool coldRPrefetch)
    {
        return level >= 13 && batch && lFirst && !coldRPrefetch;
    }
    // Return the physical input region, never an alternative storage offset.
    static constexpr int select(bool lPublished, bool uPublished, bool rPublished,
                                unsigned lBytes, unsigned uBytes, unsigned rBytes)
    {
        if (!lPublished) return lBytes == 200 ? 1 : -1;
        if (!rPublished && rBytes == 40) return 3;
        if (!uPublished && uBytes == 200) return 2;
        return -1;
    }
};
} }
#endif
