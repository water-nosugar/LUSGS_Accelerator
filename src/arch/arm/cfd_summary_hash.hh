#ifndef __ARCH_ARM_CFD_SUMMARY_HASH_HH__
#define __ARCH_ARM_CFD_SUMMARY_HASH_HH__

#include <cassert>
#include <cstdint>

namespace gem5 { namespace ArmISA {

// One registered, iterative metadata hash engine. A record remains owned by
// its existing request slot until all 40 words have returned. No record FIFO
// or combinational whole-record reduction is hidden in this timing model.
struct CfdSummaryHash
{
    static constexpr unsigned Words = 40;
    static constexpr unsigned WordCycles = 2;
    static constexpr uint64_t Seed = 14695981039346656037ULL;
    static constexpr uint64_t Prime = 1099511628211ULL;
    bool live = false, pending = false;
    unsigned index = 0;
    uint64_t token = 0, owner = 0, digest = Seed;
    uint64_t word = 0, result = 0, readyCycle = 0;

    void issue(uint64_t now, uint64_t value)
    {
        assert(live && !pending && index < Words);
        word = value;
        result = (digest ^ word) * Prime;
        readyCycle = now + WordCycles;
        pending = true;
    }
    void start(uint64_t now, uint64_t request, uint64_t line, uint64_t value)
    {
        assert(!live);
        *this = CfdSummaryHash{};
        live = true;
        token = request;
        owner = line;
        issue(now, value);
    }
    bool ready(uint64_t now) const
    { return live && pending && now >= readyCycle; }
    bool done() const { return live && !pending && index == Words; }
    void commit(uint64_t now)
    {
        assert(ready(now));
        digest = result;
        pending = false;
        ++index;
    }
    // Cancellation drains the outstanding registered operation first.
    bool cancel(uint64_t now)
    {
        if (!ready(now)) return false;
        *this = CfdSummaryHash{};
        return true;
    }
};
static_assert(sizeof(CfdSummaryHash) <= 128, "summary engine budget");

} }
#endif
