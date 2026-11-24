#include "arch/arm/cfd_local_vector.hh"
#include <cassert>
#include <iostream>
using namespace gem5::ArmISA;
int main() {
    for (unsigned context=0;context<3;++context) {
        for (bool feedback : {false,true}) {
            unsigned productReads=0,partialReads=0;
            for (int node=0;node<5;++node) {
                auto p=cfdMvmAccess(context,node,true,feedback,true,true,7,9);
                assert(p.destination==int(9*context)+node && p.globalReads==6);
                assert(p.owner==7 && p.epoch==9 && p.a==-1 && p.b==-1);
                if (!feedback && node==4) continue;
                auto hybrid=cfdMvmAccess(context,node,false,feedback,true,true,7,9);
                auto full=cfdMvmAccess(context,node,false,feedback,false,true,7,9);
                assert(hybrid.destination==-1);
                assert(hybrid.globalReads+hybrid.wordsA+hybrid.wordsB==10);
                for (int bank : {hybrid.a,hybrid.b})
                    assert(bank==-1 || (bank/9==int(context) && bank%9<5));
                productReads+=hybrid.globalReads;
                partialReads+=full.globalReads;
            }
            assert(productReads==(feedback ? 25u : 15u));
            assert(partialReads==(feedback ? 5u : 0u));
        }
    }
    bool invalid=false;
    try { cfdMvmAccess(3,0,true,false,true,true,1,1); }
    catch (const std::invalid_argument &) { invalid=true; }
    assert(invalid);
    CfdLocalVectorPorts rf; rf.configure(6);
    CfdLocalAccess a; a.a=0;a.b=1;
    assert(rf.canRead(0,a));rf.accept(0,a);
    assert(!rf.canRead(0,a));
    CfdLocalAccess b;b.a=2;b.b=3;
    assert(rf.canRead(0,b));rf.accept(0,b);
    assert(rf.write(3,0));assert(!rf.write(3,0));assert(rf.write(3,1));
    rf.release();rf.release();
    a.b=0;assert(!rf.canRead(4,a));
    a.a=a.b=-1;
    for (unsigned i=0;i<32;++i) rf.accept(5+i,a);
    assert(!rf.canRead(40,a));assert(rf.peak==32);
    for (unsigned i=0;i<32;++i)rf.release();
    assert(rf.live==0 && rf.canRead(41,a));
    bool bad=false;try{rf.release();}catch(const std::logic_error&){bad=true;}assert(bad);
    std::cout<<"LOCAL_VECTOR_MODEL_PASS\n";
}
