#include "arch/arm/cfd_lu_upper.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>

using gem5::ArmISA::CfdLuUpper;

template<class F> void rejects(F call) {
    bool failed=false;
    try { call(); } catch (const std::logic_error &) { failed=true; }
    assert(failed);
}

int main() {
    CfdLuUpper upper;
    std::array<double,5> initial{1,1e16,-1e16,3.25,-2.5};
    upper.begin(0,initial,10);
    assert(!upper.advanceReady(10));
    assert(upper.advanceReady(11) && upper.ready);
    rejects([&] { upper.begin(2,initial,12); });
    upper.begin(1,initial,12);
    upper.products.produce(0);
    rejects([&] { upper.begin(2,initial,13); });
    std::array<double,5> product{0,0,0.125,-0.25,0.75};
    upper.products.publish(0,product);
    upper.products.capture(0);
    assert(!upper.advanceReady(20));
    std::array<double,5> answer=initial;
    for (unsigned j=2;j<5;++j) {
        volatile double expected=initial[j]-product[j];
        answer[j]=expected;
    }
    upper.finishSubtract(0,answer,21);
    assert(upper.ready && upper.returned==21);
    assert(std::memcmp(upper.accumulator.data(),answer.data(),sizeof(answer))==0);
    rejects([&] { upper.finishSubtract(0,answer,22); });
    upper.begin(2,initial,22);
    upper.products.produce(0);upper.products.produce(1);
    rejects([&] { upper.products.produce(2); });
    upper.products.publish(1,product);
    assert(!upper.products.canSubtract());
    upper.products.publish(0,product);
    for (unsigned k=0;k<2;++k) {
        upper.products.capture(k);
        for (unsigned j=3;j<5;++j) {
            volatile double ordered=upper.accumulator[j]-product[j];
            answer[j]=ordered;
        }
        upper.finishSubtract(k,answer,30+4*k);
        assert(upper.ready==(k==1));
    }
    assert(CfdLuUpper::reservation(4)==1152);
    assert(sizeof(CfdLuUpper)<=256);
    std::cout << "LU_UPPER_MODEL_PASS ordered_returns=3 reservation=1152\n";
}
