#include "arch/arm/cfd_fp64_divider.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>

using Divider = gem5::ArmISA::CfdFp64Divider;

static Divider::Transaction input(uint64_t tag) { return {tag,tag+100,tag+1,2,0}; }

int main()
{
    const auto fake = [](uint64_t a,uint64_t b) -> Divider::Value { return {a+b,0}; };
    Divider d;
    d.configure({12,4,3,2,false},fake);
    unsigned accepted=0, returned=0;
    for (uint64_t cycle=0; cycle<80; ++cycle) {
        const auto r = d.tick(cycle,true);
        if (r) {
            assert(r->tag == returned++ && r->owner == r->tag+100);
            assert(cycle-r->accepted == 12 && r->produced == cycle);
            assert(r->value.bits == r->a+r->b);
        }
        if (accepted<10 && d.ready()) {
            assert(cycle == accepted*4);
            auto t=input(accepted);
            assert(d.accept(cycle,t));
            t.a=999; // caller changing its storage cannot change latched operands
            assert(!d.accept(cycle,input(900)));
            accepted++;
        }
    }
    assert(returned==10 && d.empty() && d.stats.corePeak==3);
    assert(d.stats.coreOccupancy==120 && d.stats.computeCycles==120 &&
           d.stats.resultOccupancy==0 && d.stats.residenceCycles==120 &&
           d.stats.resultWaitCycles==0 && d.stats.maxResidence==12);
    // A reduced capacity throttles issue even after II expires.
    d.configure({12,4,1,1,false},fake);
    for (uint64_t cycle=0; cycle<=24; ++cycle) {
        const auto r=d.tick(cycle,true);
        if (cycle==0 || cycle==12) assert(d.accept(cycle,input(cycle)));
        if (cycle==4) assert(d.blocked()==Divider::Block::Capacity);
        if (cycle==12 || cycle==24) assert(r);
    }
    // An iterative core cannot have multiple computations in flight.
    try { d.configure({12,4,1,1,true},fake); assert(false); }
    catch (const std::invalid_argument &) {}
    d.configure({12,12,1,1,true},fake);
    for (uint64_t cycle=0; cycle<=24; ++cycle) {
        auto r=d.tick(cycle,true);
        if (cycle==0 || cycle==12) assert(d.accept(cycle,input(cycle)));
        else if (cycle<24) assert(!d.ready());
        if (cycle==12 || cycle==24) assert(r && r->produced==cycle);
    }
    // Full output FIFO freezes every core stage and input readiness. Responses
    // stay ordered; dequeue + refill and accepting a new request share an edge.
    d.configure({4,1,4,1,false},fake);
    accepted=returned=0;
    bool observedStall=false;
    for (uint64_t cycle=0; cycle<100; ++cycle) {
        auto r=d.tick(cycle,cycle>=15);
        if (r) {
            assert(r->tag==returned++ && r->value.bits==r->a+r->b);
            assert(cycle>=15);
        }
        if (cycle>=5 && cycle<15) {
            assert(d.blocked()==Divider::Block::Backpressure);
            assert(!d.accept(cycle,input(999)));
            observedStall=true;
        }
        if (accepted<8 && d.ready()) assert(d.accept(cycle,input(accepted++)));
        assert(d.coreSize()<=4 && d.resultSize()<=1);
    }
    assert(observedStall && returned==8 && d.stats.pipelineStalls==10);
    assert(d.stats.accepted==d.stats.returned && d.empty());
    assert(d.stats.coreOccupancy==d.stats.computeCycles &&
           d.stats.resultOccupancy==d.stats.resultWaitCycles &&
           d.stats.residenceCycles==d.stats.computeCycles+d.stats.resultWaitCycles);
    d.tick(1000,true); // idle gap between autonomous line requests is allowed
    const auto check=[](uint64_t a,uint64_t b,uint64_t bits,unsigned flags) {
        const auto r=Divider::divide(a,b); assert(r.bits==bits && r.flags==flags);
    };
    constexpr uint64_t one=UINT64_C(0x3ff0000000000000);
    constexpr uint64_t inf=UINT64_C(0x7ff0000000000000);
    constexpr uint64_t nan=UINT64_C(0x7ff8000000000000);
    check(one,UINT64_C(0x4008000000000000),UINT64_C(0x3fd5555555555555),1);
    check(one,0,inf,8); check(0,0,nan,16); check(inf,inf,nan,16);
    check(UINT64_C(0x7ff0000000000001),one,nan,16);
    check(1,UINT64_C(0x4000000000000000),0,3);
    check(UINT64_C(0x0010000000000000),UINT64_C(0x4000000000000000),UINT64_C(0x0008000000000000),0);
    check(UINT64_C(0x7fefffffffffffff),UINT64_C(0x3fe0000000000000),inf,5);
    softfloat_roundingMode=softfloat_round_min;
    softfloat_detectTininess=softfloat_tininess_beforeRounding;
    softfloat_exceptionFlags=16;
    check(one,one,one,0);
    assert(softfloat_roundingMode==softfloat_round_min && softfloat_exceptionFlags==16 &&
           softfloat_detectTininess==softfloat_tininess_beforeRounding);
    // Independent host-RNE check on finite random operands; numerical backend
    // also explicitly covers exceptions and denormals above.
    std::mt19937_64 rng(20261020);
    for (unsigned i=0;i<10000;++i) {
        uint64_t a=rng(), b=rng();
        if (((a>>52)&2047)==2047 || ((b>>52)&2047)==2047) continue;
        double x,y; std::memcpy(&x,&a,8); std::memcpy(&y,&b,8);
        volatile double z=x/y;
        double rounded=z; uint64_t expected; std::memcpy(&expected,&rounded,8);
        assert(Divider::divide(a,b).bits==expected);
    }
    std::cout << "FP64_DIVIDER_MODEL_PASS timing capacity backpressure iterative bits flags state_isolation\n";
}
