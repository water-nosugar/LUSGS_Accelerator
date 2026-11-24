#include "arch/arm/cfd_arithmetic_groups.hh"
#include <iostream>
using namespace gem5;
int main()
{
    CfdArithmeticGroups q; q.depth=2;
    int a=q.allocate(true,false,1,5,true,10,1);
    int b=q.allocate(false,true,7,5,false,10,1);
    assert(q.allocate(true,false,1,1,false,10,1)==-1);
    assert(q.readyOrder(10,q.Borrow,32).empty());
    assert(q.readyOrder(11,q.Borrow,32).front()==unsigned(b));
    assert(q.readyOrder(42,q.Borrow,32).front()==unsigned(a));
    auto pre=q.laneOrder(q.Fixed,false,8), scan=q.laneOrder(q.Fixed,true,8);
    assert(pre==std::vector<unsigned>({0,1,2,3,4}));
    assert(scan==std::vector<unsigned>({5,6,7}));
    assert(q.laneOrder(q.Borrow,true,7)==std::vector<unsigned>({5,6,0,1,2,3,4}));
    auto &g=q.slots[a]; g.issued=3;
    q.returned(a,0,3,14,1,1); assert(!q.publishable(a,100));
    g.issued=5;g.crossed=true;
    q.returned(a,3,2,16,1,1);
    assert(!q.publishable(a,17)); assert(q.publishable(a,18));
    const auto old=g.id;q.release(a);
    int c=q.allocate(true,false,1,1,false,19,1);
    assert(c==a && q.slots[c].id>old && q.live==2 && q.peak==2);
    q.release(b);q.release(c);assert(q.live==0);
    // Consumer credits are already being used: they must not be reserved
    // a second time against producers while physical entries remain free.
    CfdArithmeticGroups mixed; mixed.depth=4;
    mixed.allocate(true,true,8,1,false,1,1);
    mixed.allocate(false,true,7,1,false,1,1);
    mixed.allocate(true,false,1,1,false,1,1);
    assert(mixed.canAdmit(false));
    mixed.allocate(false,false,2,1,false,1,1);
    assert(!mixed.canAdmit(false) && !mixed.canAdmit(true));
    CfdArithmeticGroups producers; producers.depth=4;
    producers.allocate(true,false,1,1,false,1,1);
    producers.allocate(false,false,2,1,false,1,1);
    assert(!producers.canAdmit(false) && producers.canAdmit(true));
    std::cout << "ARITHMETIC_GROUPS_PASS\n";
}
