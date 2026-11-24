// Bounded RF-resident negative off-diagonal inverse of a unit upper U.
#ifndef CFD_RHS_UPPER_HH
#define CFD_RHS_UPPER_HH
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
namespace gem5 { namespace ArmISA {
struct CfdRhsUpper {
    static constexpr std::array<unsigned,6> Row{0,0,0,1,1,2};
    static constexpr std::array<unsigned,6> Col{2,3,4,3,4,4};
    struct Slot {
        uint64_t id=0,token=0,epoch=0,request=0,cell=0;
        std::array<double,6> value{};
        std::array<uint8_t,6> next{};
        uint8_t done=0,pending=0;
        bool cancelled=false;
    };
    static_assert(sizeof(Slot)<=256,"increase shared upper reservation");
    std::vector<Slot> slots;
    uint64_t nextId=1,issued=0,completed=0,dropped=0;
    unsigned live=0,peak=0;
    static unsigned index(unsigned i,unsigned j) {
        assert(i+1<j && j<5);
        return i==0 ? j-2 : i==1 ? j : 5;
    }
    static uint64_t reservation(unsigned count) {
        assert(4<=count && count<=32);return 256*uint64_t(count)+256;
    }
    int allocate(uint64_t token,uint64_t epoch,uint64_t request,uint64_t cell) {
        for(unsigned i=0;i<slots.size();++i)if(!slots[i].id) {
            auto &s=slots[i];s={};s.id=nextId++;s.token=token;s.epoch=epoch;
            s.request=request;s.cell=cell;
            for(unsigned e=0;e<6;++e)s.next[e]=Col[e]-1;
            ++live;peak=std::max(peak,live);return i;
        }
        return -1;
    }
    void release(unsigned index,uint64_t id) {
        auto &s=slots.at(index);
        assert(s.id==id && !s.pending && (s.cancelled || s.done==63) && live);
        s={};--live;
    }
    static bool ready(const Slot &s,unsigned i,unsigned j) {
        return j==i+1 || (s.done&(1u<<index(i,j)));
    }
    static double get(const Slot &s,const std::array<double,25> &lu,unsigned i,unsigned j) {
        assert(i<j && j<5 && ready(s,i,j));
        return j==i+1 ? lu[5*i+j] : s.value[index(i,j)];
    }
};
} }
#endif
