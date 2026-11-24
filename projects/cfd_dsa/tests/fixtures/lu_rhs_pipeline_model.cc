#include "arch/arm/cfd_lu_products.hh"
#include "arch/arm/cfd_rhs_factors.hh"
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <vector>
using namespace gem5::ArmISA;
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch (const std::logic_error &) { caught=true; }
    assert(caught);
}
int main() {
    CfdLuProducts w;w.begin(4);
    std::array<double,5> acc{1e16,1.,-1e16,-0.,7.},ref=acc;
    std::array<std::array<double,5>,4> products{};
    for (unsigned k=0;k<4;++k)
        for (unsigned row=0;row<5;++row) {
            volatile double p=(k==0 ? 1e16 : k==1 ? 1. : k==2 ? -1e16 : 2.)*(row+1.);
            products[k][row]=p;
            volatile double a=ref[row]-p;ref[row]=a;
        }
    struct Op { unsigned at;std::function<void()> done; };
    std::vector<Op> pending;
    unsigned finish=0;
    for (unsigned now=0;now<64;++now) {
        for (auto it=pending.begin();it!=pending.end();) {
            if (it->at==now) { auto callback=it->done;it=pending.erase(it);callback(); }
            else ++it;
        }
        if (w.nextSub==4) { finish=now;break; }
        if (w.canSubtract()) {
            const unsigned k=w.nextSub;
            const auto left=acc,right=w.slots[k%2].value;
            w.capture(k);
            rejects([&] { w.capture(k); });
            pending.push_back({now+4,[&,left,right,k] {
                for (unsigned row=0;row<5;++row) {
                    volatile double v=left[row]-right[row];acc[row]=v;
                }
                w.complete(k);
            }});
        }
        if (w.canProduce()) {
            unsigned k=w.nextMul;w.produce(k);
            pending.push_back({now+4,[&,k] { w.publish(k,products[k]); }});
        }
    }
    assert(finish==20 && pending.empty());
    assert(std::memcmp(acc.data(),ref.data(),sizeof(acc))==0);
    w.begin(1);w.produce(0);
    rejects([&] { w.produce(0); });
    rejects([&] { w.begin(1); });
    rejects([&] { w.capture(0); });
    rejects([&] { w.publish(2,products[0]); });
    w.publish(0,products[0]);
    const auto captured=w.slots[0].value;w.capture(0);w.complete(0);
    rejects([&] { w.complete(0); });
    w.begin(1);w.produce(0);w.publish(0,products[3]);
    assert(captured==products[0]); // Old input remains immutable after reuse.

    CfdRhsFactors c;
    assert(c.lookup(7,9,false,0,0)<0);
    int slot=c.victim(7,9,false,0,0);assert(slot==0);
    c.fill(slot,7,9,false,0,0,products[0]);
    assert(c.lookup(7,9,false,0,0)<0); // Registered one-cycle capture.
    assert(c.victim(8,9,false,0,0)<0); // One fill per cycle.
    assert(c.lookup(7,10,false,0,1)<0 && c.lookup(8,9,false,0,1)<0);
    assert(c.lookup(7,9,true,0,1)<0 && c.lookup(7,9,false,1,1)<0);
    auto old=c.slots[slot];c.capture(slot,old.id,1);
    rejects([&] { c.capture(slot,old.id,1); });
    rejects([&] { c.capture(slot,old.id+1,2); });
    int second=c.victim(8,9,true,3,1);assert(second==1);
    c.fill(second,8,9,true,3,1,products[1]);
    assert(c.victim(10,9,false,0,1)<0);
    auto second_id=c.slots[second].id;
    c.capture(second,second_id,2);
    assert(c.victim(10,9,false,0,2)==0);
    c.fill(0,10,9,false,0,2,products[2]);
    assert(c.lookup(7,9,false,0,3)<0 && old.value==products[0]);
    rejects([&] { c.clear(0,old.id); });
    c.clear(0,c.slots[0].id);c.clear(1,second_id);
    assert(!c.slots[0].id && !c.slots[1].id);
    for (unsigned depth : {2u,4u,8u}) {
        CfdRhsFactors expanded;
        expanded.configure(depth);
        assert(CfdRhsFactors::reservation(depth)==128*depth+256);
        for (unsigned i=0;i<depth;++i) {
            const auto t=10+i;
            const int selected=expanded.victim(100+i,99,false,0,t);
            assert(selected==int(i));
            expanded.fill(selected,100+i,99,false,0,t,products[i%4]);
            assert(expanded.lookup(100+i,99,false,0,t)<0);
            rejects([&] { expanded.configure(depth); });
        }
        for (unsigned i=0;i<depth;++i) {
            assert(expanded.lookup(100+i,99,false,0,30)==int(i));
            expanded.capture(i,expanded.slots[i].id,30);
            rejects([&] { expanded.capture(i,expanded.slots[i].id,30); });
        }
        assert(expanded.victim(999,99,false,0,30)<0);
        rejects([&] { expanded.capture(depth,1,31); });
        rejects([&] { expanded.clear(depth,1); });
        assert(expanded.victim(999,99,false,0,31)==0);
        const auto saved=expanded.slots[0];
        expanded.fill(0,999,99,false,0,31,products[0]);
        rejects([&] { expanded.capture(0,saved.id,32); });
        for (unsigned i=0;i<depth;++i) expanded.clear(i,expanded.slots[i].id);
        expanded.configure(2);
    }
    for (unsigned invalid : {0u,1u,3u,5u,9u}) rejects([&] { c.configure(invalid); });
    std::cout<<"LU_RHS_PIPELINE_MODEL_PASS cycles="<<finish<<"\n";
}
