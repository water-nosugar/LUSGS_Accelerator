#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include "arch/arm/cfd_rhs_factor_fanout.hh"
#include "arch/arm/cfd_rhs_fma.hh"
using namespace gem5::ArmISA;
int main()
{
    uint64_t saved=0, operations=0;
    for(unsigned k=0;k<4;++k)for(unsigned iteration=0;iteration<1000;++iteration) {
        CfdRhsFactors cache;cache.configure(8);
        CfdRhsFactorFanout bus;
        CfdRegisterPorts local, global;
        local.configure({1,1});global.configure({32,16});
        std::array<double,5> factors{};
        for(unsigned i=0;i<5;++i)factors[i]=(iteration+1)*0.000125+(i+1)*0.03125;
        cache.fill(0,7,9,false,k,0,factors);
        auto &slot=cache.slots[0];
        const unsigned width=4-k, reads=CfdRhsFma::reads(width,true)-width;
        assert(local.reserveReads(1,1) && global.reserveReads(1,reads));
        cache.capture(0,slot.id,1);
        assert(bus.offer(slot,0,55,local,1,10,5));
        const auto *wire=bus.probe(slot,0,55,1,6,width);assert(wire);
        assert(global.reserveReads(1,reads));
        auto captured=bus.commit(slot,0,55,1,6,width,10,11);
        for(unsigned lane=0;lane<width;++lane) {
            const double rhs=0.125*(iteration+2), accumulator=lane*0.25-0.75;
            const double actual=CfdRhsFma::evaluate(captured.value[k+1+lane],rhs,accumulator);
            const double expected=std::fma(-factors[k+1+lane],rhs,accumulator);
            assert(std::memcmp(&actual,&expected,sizeof actual)==0);++operations;
        }
        assert(!bus.probe(slot,0,55,1,7,width));
        assert(local.readsUsed()==1 && global.readsUsed()==2*reads);
        saved+=width;
    }
    assert(saved==10000 && operations==10000);
    std::cout << "FMA_FACTOR_FANOUT_PASS exact=10000 saved_global_words=10000\n";
}
