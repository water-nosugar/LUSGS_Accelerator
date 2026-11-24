#ifndef __ARCH_ARM_CFD_SUMMARY_SNAPSHOTS_HH__
#define __ARCH_ARM_CFD_SUMMARY_SNAPSHOTS_HH__

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5 { namespace ArmISA {

// Bounded metadata storage, independent of the floating-point RF. One 64-byte
// ingress port copies a completed, immutable record over five registered ticks.
// Hash engines each own a separate entry and its one-word read mux. An entry
// remains allocated through the last hash return (including cancellation).
class CfdSummarySnapshots
{
  public:
    static constexpr unsigned Words = 40, CopyWidth = 8, EntryBytes = 512;
    static constexpr unsigned SmeMaxDepth = 24, SmeMaxEngines = 16;
    struct Entry {
        bool live = false, hashing = false;
        unsigned copied = 0;
        uint64_t token = 0, owner = 0, allocated = 0, ready = 0, expectedDigest = 0;
        std::array<uint64_t, Words> words{};
    };
    static_assert(sizeof(Entry) <= EntryBytes, "summary snapshot budget");
    std::vector<Entry> entries;
    unsigned live = 0, peak = 0;
    void configure(unsigned depth) { assert(depth <= SmeMaxDepth); entries.resize(depth); }
    Entry *find(uint64_t token) {
        for (auto &e : entries) if (e.live && e.token == token) return &e;
        return nullptr;
    }
    Entry *allocate(uint64_t now, uint64_t token, uint64_t owner) {
        assert(!find(token));
        for (auto &e : entries) if (!e.live) {
            e = Entry{}; e.live = true; e.token = token; e.owner = owner;
            e.allocated = now; ++live; if (live > peak) peak = live;
            return &e;
        }
        return nullptr;
    }
    Entry *copyCandidate(uint64_t now) {
        Entry *chosen = nullptr;
        for (auto &e : entries)
            if (e.live && e.copied < Words && now > e.allocated &&
                (!chosen || e.allocated < chosen->allocated ||
                 (e.allocated == chosen->allocated && e.token < chosen->token))) chosen = &e;
        return chosen;
    }
    Entry *readyCandidate(uint64_t now) {
        Entry *chosen = nullptr;
        for (auto &e : entries)
            if (e.live && e.copied == Words && !e.hashing && now > e.ready &&
                (!chosen || e.ready < chosen->ready ||
                 (e.ready == chosen->ready && e.token < chosen->token))) chosen = &e;
        return chosen;
    }
    void release(Entry &e) { assert(e.live && live); e = Entry{}; --live; }
};

} }
#endif
