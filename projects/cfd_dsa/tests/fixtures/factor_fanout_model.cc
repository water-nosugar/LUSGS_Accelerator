// Independent protocol and FP64 payload tests for the bounded factor bus.
// This fixture is included in the scoped model and checker fingerprint.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include "arch/arm/cfd_rhs_factor_fanout.hh"

using namespace gem5::ArmISA;

static uint64_t bits(double x) {
    uint64_t u;
    std::memcpy(&u, &x, sizeof u);
    return u;
}
static double value(uint64_t u) {
    double x;
    std::memcpy(&x, &u, sizeof x);
    return x;
}
template<class F> static void rejects(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::logic_error &) { failed = true; }
    assert(failed);
}

struct Fixture {
    CfdRhsFactors cache;
    CfdRegisterPorts bank;
    CfdRegisterPorts rf;
    unsigned slot;
    unsigned bankIndex = 71;
    uint64_t now;
    Fixture(uint64_t cycle, uint64_t owner, uint64_t epoch, unsigned k,
            bool backward, const std::array<double,5> &payload, unsigned depth=8)
      : now(cycle) {
        cache.configure(depth);
        bank.configure({1,1});
        rf.configure({16,12});
        const unsigned width = 4-k;
        // First fill pays the original RF input read and local write. A bus
        // may not use this fill as a same-cycle registered-cache read.
        assert(rf.reserveReads(now-1, backward ? 2*width : width+1));
        assert(bank.reserveWrite(now-1, 1));
        const int victim = cache.victim(owner, epoch, backward, k, now-1);
        assert(victim >= 0);
        slot = victim;
        cache.fill(slot, owner, epoch, backward, k, now-1, payload);
    }
    CfdRhsFactors::Slot &s() { return cache.slots.at(slot); }
    void paidRead() {
        assert(bank.reserveReads(now,1));
        assert(rf.reserveReads(now,s().backward ? 4-s().k : 1));
        cache.capture(slot,s().id,now);
    }
};

static void protocol() {
    const std::array<double,5> a{{-0.0,1.0,-2.0,3.0,4.0}};
    CfdRhsFactorFanout bus;
    Fixture f(10,1,9,0,false,a);
    // No registered use and no paid bank read: rejected even with an ID.
    rejects([&] { bus.offer(f.s(),f.slot,f.bankIndex,f.bank,10,100,0); });
    f.cache.capture(f.slot,f.s().id,10);
    rejects([&] { bus.offer(f.s(),f.slot,f.bankIndex,f.bank,10,100,0); });
    assert(f.bank.reserveReads(10,1));
    assert(bus.offer(f.s(),f.slot,f.bankIndex,f.bank,10,100,0));
    assert(bus.source(10).sourceRf==100);
    rejects([&] { bus.source(11); });
    assert(!f.bank.canRead(10,1));
    assert(bus.probe(f.s(),f.slot,f.bankIndex,10,1,4));
    // A failed FU/RF admission must leave the bus available to another RHS.
    assert(bus.probe(f.s(),f.slot,f.bankIndex,10,2,4));
    assert(!bus.probe(f.s(),f.slot,f.bankIndex,10,0,4));
    assert(!bus.probe(f.s(),f.slot,f.bankIndex,10,1,3));
    assert(!bus.probe(f.s(),f.slot+1,f.bankIndex,10,1,4));
    assert(!bus.probe(f.s(),f.slot,f.bankIndex+1,10,1,4));
    assert(!bus.probe(f.s(),f.slot,f.bankIndex,11,1,4));
    auto bad = f.s();
    ++bad.token; assert(!bus.probe(bad,f.slot,f.bankIndex,10,1,4));
    bad=f.s(); ++bad.epoch; assert(!bus.probe(bad,f.slot,f.bankIndex,10,1,4));
    bad=f.s(); ++bad.id; assert(!bus.probe(bad,f.slot,f.bankIndex,10,1,4));
    bad=f.s(); bad.backward=true; assert(!bus.probe(bad,f.slot,f.bankIndex,10,1,4));
    bad=f.s(); bad.k=1; assert(!bus.probe(bad,f.slot,f.bankIndex,10,1,3));
    rejects([&] { bus.commit(f.s(),f.slot,f.bankIndex,10,1,4,99,101); });
    rejects([&] { bus.commit(f.s(),f.slot,f.bankIndex,10,1,4,100,100); });
    bad=f.s(); bad.value[3]=99;
    rejects([&] { bus.commit(bad,f.slot,f.bankIndex,10,1,4,100,101); });
    auto transfer=bus.commit(f.s(),f.slot,f.bankIndex,10,1,4,100,101);
    assert(transfer.sourceRf==100 && transfer.cycle==10 && transfer.width==4);
    for (unsigned i=0;i<5;++i) assert(bits(transfer.value[i])==bits(a[i]));
    assert(f.bank.readsUsed()==1);
    assert(!bus.probe(f.s(),f.slot,f.bankIndex,10,2,4));
    rejects([&] { bus.commit(f.s(),f.slot,f.bankIndex,10,2,4,100,102); });
    rejects([&] { bus.offer(f.s(),f.slot,f.bankIndex,f.bank,9,103,1); });

    Fixture g(20,2,10,2,true,a);
    g.paidRead();
    assert(bus.offer(g.s(),g.slot,g.bankIndex,g.bank,20,200,3));
    Fixture other(20,3,10,2,true,a);
    other.paidRead();
    assert(!bus.offer(other.s(),other.slot,other.bankIndex+1,other.bank,20,201,4));
    bus.invalidateOwner(99,10); // An unrelated cancellation does not clear it.
    assert(bus.probe(g.s(),g.slot,g.bankIndex,20,4,2));
    bus.invalidateOwner(g.s().token,g.s().epoch);
    rejects([&] { bus.source(20); });
    assert(!bus.probe(g.s(),g.slot,g.bankIndex,20,4,2));
    // Cancellation cannot refund the one physical bus transfer for this cycle.
    assert(!bus.offer(other.s(),other.slot,other.bankIndex+1,other.bank,20,202,4));
    other.now=21;other.paidRead();
    assert(bus.offer(other.s(),other.slot,other.bankIndex+1,other.bank,21,203,4));
    bus.invalidateSlot(other.slot,other.s().id+1);
    assert(bus.probe(other.s(),other.slot,other.bankIndex+1,21,5,2));
    bus.invalidateSlot(other.slot,other.s().id);
    assert(!bus.probe(other.s(),other.slot,other.bankIndex+1,21,5,2));
    Fixture rewind(19,4,10,2,true,a);
    rewind.paidRead();
    rejects([&] { bus.offer(rewind.s(),rewind.slot,72,rewind.bank,19,204,4); });

    CfdRhsFactorFanout fresh;
    Fixture unpublished(30,8,1,1,false,a);
    unpublished.s().filled=30;
    unpublished.s().used=30;
    assert(unpublished.bank.reserveReads(30,1));
    rejects([&] { fresh.offer(unpublished.s(),unpublished.slot,71,unpublished.bank,30,300,0); });
}

int main() {
    protocol();
    std::mt19937_64 rng(0x127af20261030ULL);
    CfdRhsFactorFanout bus;
    uint64_t baselineReads=0, actualReads=0, savedWords=0;
    bool covered[6][4][2]{};
    for (unsigned i=0;i<10000;++i) {
        std::array<double,5> original{};
        for (auto &x : original) {
            uint64_t raw=rng();
            if (((raw>>52)&0x7ff)==0x7ff) raw^=uint64_t(1)<<52;
            x=value(raw);
        }
        const unsigned k=i%4,width=4-k;
        const bool backward=(i/4)&1;
        const uint64_t now=100+4*uint64_t(i),tag=1000+3*uint64_t(i);
        const unsigned depthIndex=(i/8)%6;
        const unsigned depth=std::array<unsigned,6>{{2,4,8,16,32,64}}[depthIndex];
        covered[depthIndex][k][backward]=true;
        Fixture f(now,17+i,19+i/3,k,backward,original,depth);
        f.paidRead();
        const unsigned rhs=i%15, second=(rhs+1)%15;
        assert(bus.offer(f.s(),f.slot,f.bankIndex,f.bank,now,tag,rhs));
        const auto *preview=bus.probe(f.s(),f.slot,f.bankIndex,now,second,width);
        assert(preview && preview->sourceRf==tag);
        assert(!f.bank.canRead(now,1));
        // The second RHS still pays its own ordinary row/scalar RF operands.
        assert(f.rf.reserveReads(now,backward ? width : 1));
        const auto transfer=bus.commit(f.s(),f.slot,f.bankIndex,now,second,width,tag,tag+1);
        assert(f.bank.stats.readWords==1 && f.bank.stats.writeWords==1);
        assert(!bus.probe(f.s(),f.slot,f.bankIndex,now,(second+1)%15,width));
        for (unsigned j=0;j<5;++j) assert(bits(transfer.value[j])==bits(original[j]));
        for (unsigned j=k+1;j<5;++j) {
            volatile double reference=original[j]*0.00390625;
            volatile double received=transfer.value[j]*0.00390625;
            assert(bits(reference)==bits(received));
        }
        const unsigned firstFill=backward ? 2*width : width+1;
        const unsigned rowRead=backward ? width : 1;
        baselineReads+=firstFill+2*rowRead+width;
        actualReads+=f.rf.stats.readWords;
        savedWords+=width;
        assert(!bus.probe(f.s(),f.slot,f.bankIndex,now+1,second,width));
    }
    assert(baselineReads-actualReads==savedWords);
    for (const auto &depth : covered)
        for (const auto &column : depth)
            for (bool direction : column) assert(direction);
    std::cout<<"PASS factor fanout prototype: 10000 exact payloads; saved_words="
             <<savedWords<<"; bytes="<<sizeof(CfdRhsFactorFanout)<<"\n";
}
