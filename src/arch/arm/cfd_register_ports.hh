// Cycle-level, scalar-word register-file bandwidth accounting. No data storage.
#ifndef __ARCH_ARM_CFD_REGISTER_PORTS_HH__
#define __ARCH_ARM_CFD_REGISTER_PORTS_HH__

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace gem5
{
namespace ArmISA
{

class CfdRegisterPorts
{
  public:
    struct Config
    {
        // Zero selects the legacy unlimited direction independently. These are
        // FP64 scalar words per cycle, not bank count or memory-transfer bytes.
        unsigned readWordsPerCycle = 0;
        unsigned writeWordsPerCycle = 0;
    };

    struct Stats
    {
        uint64_t samples = 0;
        uint64_t readWords = 0, writeWords = 0;
        uint64_t readAttempts = 0, writeAttempts = 0;
        uint64_t readRejects = 0, writeRejects = 0;
        uint64_t readActiveCycles = 0, writeActiveCycles = 0;
        uint64_t readIdleCycles = 0, writeIdleCycles = 0;
        uint64_t readBlockedCycles = 0, writeBlockedCycles = 0;
        uint64_t peakReadWords = 0, peakWriteWords = 0;
    } stats;

    void configure(Config config)
    {
        cfg = config;
        reset();
    }

    // This class owns only cycle credits. Reset does not cancel operations or
    // release any external FU/result storage; callers must drain those first.
    void reset()
    {
        lastCycle.reset();
        reads = writes = 0;
        readBlocked = writeBlocked = false;
        stats = {};
    }

    const Config &configuration() const { return cfg; }
    uint64_t readsUsed() const { return reads; }
    uint64_t writesUsed() const { return writes; }
    std::optional<uint64_t> currentCycle() const { return lastCycle; }

    // Idempotent in the same cycle. Idle gaps are counted arithmetically; the
    // interval begins at the first observed cycle, not at global simulation 0.
    void tick(uint64_t now)
    {
        if (lastCycle && now < *lastCycle)
            throw std::logic_error("register port cycle moved backwards");
        if (lastCycle && now == *lastCycle)
            return;
        const uint64_t elapsed = lastCycle ? now - *lastCycle : 1;
        stats.samples += elapsed;
        stats.readIdleCycles += elapsed;
        stats.writeIdleCycles += elapsed;
        lastCycle = now;
        reads = writes = 0;
        readBlocked = writeBlocked = false;
    }

    // Probes consume no credits and record no attempt/rejection. They advance
    // the cycle just like tick(), so result-ready fall-through can safely ask
    // about a port before a producer returns a value. Once that value actually
    // transfers, reserveWrite() must still succeed before it is published.
    bool canRead(uint64_t now, unsigned words)
    {
        validate(words, cfg.readWordsPerCycle);
        tick(now);
        return available(reads, words, cfg.readWordsPerCycle);
    }
    bool canWrite(uint64_t now, unsigned words)
    {
        validate(words, cfg.writeWordsPerCycle);
        tick(now);
        return available(writes, words, cfg.writeWordsPerCycle);
    }

    bool reserveReads(uint64_t now, unsigned words)
    {
        validate(words, cfg.readWordsPerCycle);
        tick(now);
        ++stats.readAttempts;
        if (!available(reads, words, cfg.readWordsPerCycle)) {
            ++stats.readRejects;
            if (!readBlocked) {
                ++stats.readBlockedCycles;
                readBlocked = true;
            }
            return false;
        }
        if (!reads) {
            ++stats.readActiveCycles;
            --stats.readIdleCycles;
        }
        reads += words;
        stats.readWords += words;
        stats.peakReadWords = std::max(stats.peakReadWords, reads);
        return true;
    }

    bool reserveWrite(uint64_t now, unsigned words)
    {
        validate(words, cfg.writeWordsPerCycle);
        tick(now);
        ++stats.writeAttempts;
        if (!available(writes, words, cfg.writeWordsPerCycle)) {
            ++stats.writeRejects;
            if (!writeBlocked) {
                ++stats.writeBlockedCycles;
                writeBlocked = true;
            }
            return false;
        }
        if (!writes) {
            ++stats.writeActiveCycles;
            --stats.writeIdleCycles;
        }
        writes += words;
        stats.writeWords += words;
        stats.peakWriteWords = std::max(stats.peakWriteWords, writes);
        return true;
    }

  private:
    Config cfg;
    std::optional<uint64_t> lastCycle;
    uint64_t reads = 0, writes = 0;
    bool readBlocked = false, writeBlocked = false;

    static void validate(unsigned words, unsigned width)
    {
        if (!words || (width && words > width))
            throw std::invalid_argument(
                "atomic register demand must be positive and fit configured width");
    }

    static bool available(uint64_t used, unsigned words, unsigned width)
    {
        if (words > std::numeric_limits<uint64_t>::max() - used)
            throw std::overflow_error("register word counter overflow");
        return !width || words <= width - used;
    }
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_REGISTER_PORTS_HH__
