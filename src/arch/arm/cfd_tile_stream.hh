// Stateless ordering for the existing y/Ubar reverse-reload streams.
#ifndef __ARCH_ARM_CFD_TILE_STREAM_HH__
#define __ARCH_ARM_CFD_TILE_STREAM_HH__

#include <cassert>
#include <cstdint>

namespace gem5
{
namespace ArmISA
{
namespace CfdTileStream
{

inline unsigned
firstRegion(uint64_t yIssued, uint64_t uIssued,
            uint64_t yBytes, uint64_t uBytes)
{
    assert(yIssued <= yBytes && uIssued <= uBytes);
    assert(uBytes == 0 || uBytes == 5*yBytes);
    if (uIssued == uBytes) return 0;
    if (yIssued == yBytes) return 1;
    // One cell needs 40 B of y and 200 B of Ubar. Equal packet counts
    // advance y five times farther along the line. Compare cell coverage
    // without division or new counters; the existing DMA budget still gates
    // every packet. Multiplication by five is a fixed shift/add in hardware.
    return 5*yIssued > uIssued ? 1 : 0;
}

} // namespace CfdTileStream
} // namespace ArmISA
} // namespace gem5

#endif
