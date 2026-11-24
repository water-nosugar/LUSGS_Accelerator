#include "arch/arm/cfd_register_ports.hh"
#include <cassert>
#include <iostream>
#include <limits>
#include <random>

using Ports = gem5::ArmISA::CfdRegisterPorts;

static void balanced(const Ports &p)
{
    assert(p.stats.samples == p.stats.readActiveCycles+p.stats.readIdleCycles);
    assert(p.stats.samples == p.stats.writeActiveCycles+p.stats.writeIdleCycles);
    assert(p.stats.readBlockedCycles <= p.stats.samples);
    assert(p.stats.writeBlockedCycles <= p.stats.samples);
    assert(p.stats.readRejects <= p.stats.readAttempts);
    assert(p.stats.writeRejects <= p.stats.writeAttempts);
}

int main()
{
    Ports p;
    p.configure({10,5});
    assert(p.canRead(100,10) && p.canWrite(100,5));
    assert(!p.readsUsed() && !p.writesUsed());
    assert(!p.stats.readAttempts && !p.stats.writeAttempts);
    assert(p.reserveReads(100,6));
    assert(!p.reserveReads(100,5));
    assert(!p.reserveReads(100,5));
    // A rejected vector does not reserve the four residual read credits.
    assert(p.readsUsed() == 6 && p.reserveReads(100,4));
    assert(p.readsUsed() == 10 && p.stats.readWords == 10);
    assert(p.stats.readRejects == 2 && p.stats.readBlockedCycles == 1);
    assert(p.reserveWrite(100,5));
    assert(!p.reserveWrite(100,1));
    assert(!p.canRead(100,1) && !p.canWrite(100,1));
    assert(p.stats.readAttempts == 4 && p.stats.writeAttempts == 2);
    // Same-cycle tick must not mint a second set of ports.
    p.tick(100);
    assert(!p.reserveWrite(100,1));
    assert(p.stats.writeBlockedCycles == 1 && p.stats.writeRejects == 2);
    p.tick(103);
    assert(p.stats.samples == 4 && p.stats.readIdleCycles == 3 &&
           p.stats.writeIdleCycles == 3);
    assert(p.reserveReads(103,10) && p.reserveWrite(103,5));
    assert(p.stats.peakReadWords == 10 && p.stats.peakWriteWords == 5);
    assert(p.stats.readActiveCycles == 2 && p.stats.writeActiveCycles == 2);
    balanced(p);

    const auto attempts = p.stats.readAttempts;
    try { p.reserveReads(102,1); assert(false); }
    catch (const std::logic_error &) {}
    assert(p.currentCycle() == 103 && p.stats.readAttempts == attempts);
    p.reset();
    assert(p.configuration().readWordsPerCycle == 10 &&
           p.configuration().writeWordsPerCycle == 5);
    assert(!p.currentCycle() && !p.stats.samples && !p.readsUsed());
    for (auto words : {0U,11U,std::numeric_limits<unsigned>::max()}) {
        try { p.reserveReads(1,words); assert(false); }
        catch (const std::invalid_argument &) {}
        assert(!p.currentCycle() && !p.stats.samples);
    }
    try { p.reserveWrite(1,6); assert(false); }
    catch (const std::invalid_argument &) {}
    try { p.canWrite(1,0); assert(false); }
    catch (const std::invalid_argument &) {}
    assert(!p.currentCycle());

    // Independent directions: an unconstrained legacy read side cannot turn
    // the write side into an unbounded result return path, or vice versa.
    p.configure({0,2});
    assert(p.reserveReads(7,std::numeric_limits<unsigned>::max()));
    assert(p.reserveReads(7,std::numeric_limits<unsigned>::max()));
    assert(p.readsUsed() == 2ULL*std::numeric_limits<unsigned>::max());
    assert(p.reserveWrite(7,2) && !p.reserveWrite(7,1));
    p.configure({2,0});
    assert(p.reserveReads(7,2) && !p.reserveReads(7,1));
    assert(p.reserveWrite(7,100) && p.reserveWrite(7,100));
    p.configure({0,0});
    assert(p.reserveReads(1,10) && p.reserveReads(1,10));
    assert(p.reserveWrite(1,5) && p.reserveWrite(1,5));
    balanced(p);

    // Contending scalar and five-word vector completions must not be
    // published before an actual write reservation. This deliberately holds
    // blocked results outside the port model, as the real FU must do.
    p.configure({10,5});
    unsigned pendingVector = 5, pendingScalar = 1, callbacks = 0;
    if (p.reserveWrite(10,pendingVector)) { pendingVector = 0; ++callbacks; }
    if (p.reserveWrite(10,pendingScalar)) { pendingScalar = 0; ++callbacks; }
    assert(callbacks == 1 && pendingScalar == 1 && p.writesUsed() == 5);
    assert(p.reserveWrite(11,pendingScalar)); pendingScalar = 0; ++callbacks;
    assert(callbacks == 2 && pendingScalar == 0 && pendingVector == 0);

    // Property stress: accepted words conserve a finite per-cycle budget.
    // Failed multiword demands leave every credit available to later callers.
    p.configure({14,7});
    std::mt19937 random(20261024);
    uint64_t readWords = 0, writeWords = 0;
    for (uint64_t cycle = 0; cycle < 1000; ++cycle) {
        unsigned acceptedRead = 0, acceptedWrite = 0;
        for (unsigned request = 0; request < 20; ++request) {
            const unsigned r = 1+random()%14, w = 1+random()%7;
            const auto rBefore = p.readsUsed(), wBefore = p.writesUsed();
            const bool canR = acceptedRead+r <= 14;
            const bool canW = acceptedWrite+w <= 7;
            assert(p.reserveReads(cycle,r) == canR);
            if (canR) acceptedRead += r;
            else assert(p.readsUsed() == rBefore);
            assert(p.reserveWrite(cycle,w) == canW);
            if (canW) acceptedWrite += w;
            else assert(p.writesUsed() == wBefore);
        }
        readWords += acceptedRead; writeWords += acceptedWrite;
        assert(p.readsUsed() == acceptedRead && p.writesUsed() == acceptedWrite);
        balanced(p);
    }
    assert(p.stats.samples == 1000 && p.stats.readWords == readWords &&
           p.stats.writeWords == writeWords);
    std::cout << "REGISTER_PORTS_MODEL_PASS atomic credits cycles directions reset probes contention\n";
}
