#include "arch/arm/cfd_rhs_pairs.hh"
#include <cassert>
#include <iostream>
using gem5::ArmISA::CfdRhsPairs;
template<class F> void rejects(F f) {
    bool failed=false;
    try { f(); } catch (const std::logic_error &) { failed=true; }
    assert(failed);
}
int main() {
    CfdRhsPairs p;
    const int a=p.select(7,11,9,0,10,11,0,0);
    assert(a==0 && p.select(8,12,10,1,10,11,0,0)==-1);
    const int b=p.select(8,12,10,1,12,14,1,1);
    assert(b==1 && p.select(9,13,11,2,10,11,2,2)==-1);
    std::array<double,5> lu{0,1,2,3,4};
    std::array<double,2> factors{2,3};
    rejects([&]{p.load(a,0,lu,factors);}); // selection is registered
    p.load(a,1,lu,factors);
    assert(!p.ready(a,1) && p.ready(a,2)); // operand route is registered
    rejects([&]{p.load(b,1,lu,factors);}); // one ingress per cycle
    p.load(b,2,lu,factors);
    p.issue(a,2);
    rejects([&]{p.issue(b,2);});
    rejects([&]{p.cancel(a);}); // in-flight operands cannot be recycled
    const auto identity=p.slots[a].id;
    const auto pipeline=p.slots[a]; // independent FU input registers
    rejects([&]{p.capture(a,identity+1,0);});
    assert(!p.capture(a,identity,0));
    rejects([&]{p.capture(a,identity,0);});
    assert(p.capture(a,identity,1));
    const int c=p.select(9,13,11,2,10,14,2,3);
    assert(c==a && p.slots[c].id!=identity);
    rejects([&]{p.capture(c,identity,1);});
    assert(pipeline.lu==lu && pipeline.factor==factors);
    p.cancel(c); p.cancel(b);
    assert(!p.slots[0].id && !p.slots[1].id);
    rejects([&]{p.select(1,1,1,0,0,11,0,4);}); // b is never paired
    rejects([&]{p.select(1,1,1,0,5,11,0,4);}); // Lbar is never paired
    rejects([&]{p.select(1,1,1,0,10,10,0,4);});
    rejects([&]{p.select(1,1,1,0,10,11,3,4);});
    std::cout << "RHS_PAIR_MODEL_PASS\n";
}
