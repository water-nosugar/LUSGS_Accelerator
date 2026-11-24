#include "arch/arm/cfd_rhs_factors.hh"
#include <array>
#include <cassert>
#include <iostream>
#include <stdexcept>

using gem5::ArmISA::CfdRhsFactors;
template<class F> void rejects(F f) {
    bool failed=false;try { f(); } catch (const std::logic_error &) { failed=true; }
    assert(failed);
}

int main() {
    CfdRhsFactors cache;
    assert(cache.depth==2 && cache.ways()==2);
    for (unsigned depth : {2,4,8,16,32,64}) {
        cache.configure(depth);
        assert(cache.ways()==(depth<8 ? depth : 8));
        assert(cache.first(17)+cache.ways()<=depth);
    }
    rejects([&] { cache.configure(24); });
    const std::array<unsigned,8> sets{2,3,0,1,6,7,4,5};
    uint64_t now=1;
    std::array<double,5> values{0.125,-3.5,17,1e-18,1e18};
    for (unsigned owner=0;owner<8;++owner) {
        const uint64_t token=16+owner;
        assert(cache.first(token)==8*sets[owner]);
        for (bool backward : {false,true}) for (unsigned k=0;k<4;++k) {
            const int i=cache.victim(token,7,backward,k,now);
            assert(i>=0 && unsigned(i)>=8*sets[owner] && unsigned(i)<8*(sets[owner]+1));
            cache.fill(i,token,7,backward,k,now,values);
            assert(cache.lookup(token,7,backward,k,now)==-1);
            rejects([&] { cache.capture(i,cache.slots[i].id,now); });
            assert(cache.victim(1000,7,false,0,now)==-1);
            ++now;
            assert(cache.lookup(token,7,backward,k,now)==i);
        }
    }
    unsigned hits=0;
    for (uint64_t token=16;token<24;++token)
        for (bool backward : {false,true}) for (unsigned k=0;k<4;++k) {
            int i=cache.lookup(token,7,backward,k,now);
            assert(i>=0 && cache.slots[i].value==values);
            assert(cache.lookup(token,8,backward,k,now)==-1);
            cache.capture(i,cache.slots[i].id,now++);++hits;
        }
    assert(hits==64);
    const int conflict=cache.victim(24,7,false,0,now);
    assert(conflict>=24 && conflict<32);
    cache.fill(conflict,24,7,false,0,now++,values);
    for (uint64_t token=16;token<24;++token) {
        if (token==17) continue;
        for (bool backward : {false,true}) for (unsigned k=0;k<4;++k)
            assert(cache.lookup(token,7,backward,k,now)>=0);
    }
    rejects([&] { cache.configure(8); });
    for (unsigned i=0;i<cache.depth;++i) cache.clear(i,cache.slots[i].id);
    cache.configure(8);assert(cache.first(200)==0);
    assert(CfdRhsFactors::reservation(64)==8448);
    std::cout << "RHS_BANKED_FACTOR_MODEL_PASS keys=64 ways=8 ingress=1 bytes=8448\n";
}
