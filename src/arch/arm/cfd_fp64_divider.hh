// Bounded, clock-enabled divider microarchitecture model (no RTL dependency).
#ifndef __ARCH_ARM_CFD_FP64_DIVIDER_HH__
#define __ARCH_ARM_CFD_FP64_DIVIDER_HH__

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <softfloat.h>

namespace gem5 { namespace ArmISA {

class CfdFp64Divider
{
  public:
    struct Value { uint64_t bits; uint8_t flags; };
    struct Transaction {
        uint64_t tag, owner, a, b, accepted, produced = 0;
        Value value{};
    };
    struct Config {
        unsigned latency = 12, ii = 4, capacity = 3, resultDepth = 2;
        bool iterative = false;
    };
    enum class Block { None, Initiation, Capacity, Backpressure };
    struct Stats {
        uint64_t accepted = 0, produced = 0, returned = 0;
        uint64_t pipelineStalls = 0, outputStalls = 0;
        uint64_t coreOccupancy = 0, resultOccupancy = 0;
        uint64_t computeCycles = 0, resultWaitCycles = 0, residenceCycles = 0;
        uint64_t maxResidence = 0;
        size_t corePeak = 0, resultPeak = 0;
    } stats;
    using Evaluate = std::function<Value(uint64_t,uint64_t)>;

    static Value divide(uint64_t a, uint64_t b)
    {
        const auto oldRound = softfloat_roundingMode;
        const auto oldTiny = softfloat_detectTininess;
        const auto oldFlags = softfloat_exceptionFlags;
        softfloat_roundingMode = softfloat_round_near_even;
        softfloat_detectTininess = softfloat_tininess_afterRounding;
        softfloat_exceptionFlags = 0;
        uint64_t bits = f64_div({a},{b}).v;
        const auto flags = static_cast<uint8_t>(softfloat_exceptionFlags);
        softfloat_roundingMode = oldRound;
        softfloat_detectTininess = oldTiny;
        softfloat_exceptionFlags = oldFlags;
        // Architectural contract uses a canonical quiet NaN, not host payloads.
        if ((bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000))
            bits = UINT64_C(0x7ff8000000000000);
        return {bits,flags};
    }

    void configure(Config config, Evaluate evaluate)
    {
        if (!empty() || !evaluate || !config.latency || !config.ii || !config.capacity ||
            !config.resultDepth || (config.iterative &&
            (config.capacity != 1 || config.ii < config.latency)))
            throw std::invalid_argument("invalid divider hardware configuration");
        cfg = config;
        execute = std::move(evaluate);
        launchCountdown = 0;
        clockStalled = false;
        lastTick.reset();
        stats = {};
    }
    Block blocked() const
    {
        if (clockStalled) return Block::Backpressure;
        if (launchCountdown) return Block::Initiation;
        if (core.size() == cfg.capacity) return Block::Capacity;
        return Block::None;
    }
    bool ready() const { return blocked() == Block::None; }
    bool empty() const { return core.empty() && results.empty(); }
    size_t coreSize() const { return core.size(); }
    size_t resultSize() const { return results.size(); }
    const Transaction &front() const { return results.front(); }
    bool accept(uint64_t now, Transaction transaction)
    {
        if (!lastTick || *lastTick != now)
            throw std::logic_error("divider accept requires the current cycle tick");
        if (!ready()) return false;
        transaction.accepted = now;
        core.push_back({transaction,cfg.latency});
        launchCountdown = cfg.ii;
        stats.accepted++;
        stats.corePeak = std::max(stats.corePeak,core.size());
        return true;
    }
    // One tick before arbitration at this cycle. An empty result FIFO has a
    // fall-through bypass: a just-produced result may transfer at T+latency.
    // If its consumer is blocked, a full result FIFO stalls the entire core
    // and the launch countdown (global clock enable, no overwriting stages).
    std::optional<Transaction> tick(uint64_t now, bool outReady)
    {
        if (lastTick && now != *lastTick+1) {
            if (now <= *lastTick || !empty())
                throw std::logic_error("active divider ticks must be consecutive");
            const uint64_t skipped = now-*lastTick-1;
            launchCountdown -= std::min<uint64_t>(launchCountdown,skipped);
        }
        lastTick = now;
        // Sample the previous edge's occupancy before departures. Over a
        // drained run these sums equal the exact token residence intervals.
        stats.coreOccupancy += core.size();
        stats.resultOccupancy += results.size();
        std::optional<Transaction> response;
        if (!results.empty()) {
            if (outReady) { response = results.front(); results.pop_front(); }
            else stats.outputStalls++;
        }
        clockStalled = !core.empty() && core.front().remaining == 1 &&
                       results.size() == cfg.resultDepth;
        if (clockStalled) {
            stats.pipelineStalls++;
        } else {
            if (launchCountdown) --launchCountdown;
            for (auto &entry : core) --entry.remaining;
            if (!core.empty() && core.front().remaining == 0) {
                auto t = core.front().transaction;
                core.pop_front();
                t.produced = now;
                t.value = execute(t.a,t.b);
                stats.produced++;
                stats.computeCycles += now-t.accepted;
                if (outReady && !response && results.empty()) response = t;
                else results.push_back(t);
            }
        }
        stats.resultPeak = std::max(stats.resultPeak,results.size());
        if (response) {
            stats.returned++;
            stats.resultWaitCycles += now-response->produced;
            stats.residenceCycles += now-response->accepted;
            stats.maxResidence = std::max(stats.maxResidence,now-response->accepted);
        }
        return response;
    }

  private:
    struct Entry { Transaction transaction; unsigned remaining; };
    Config cfg;
    Evaluate execute;
    std::deque<Entry> core;
    std::deque<Transaction> results;
    unsigned launchCountdown = 0;
    bool clockStalled = false;
    std::optional<uint64_t> lastTick;
};

} } // namespace gem5::ArmISA
#endif
