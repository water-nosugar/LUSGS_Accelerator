// Bounded temporal LU operand latches. First use pays the normal RF reads;
// its input capture can also populate one latch for use on a later cycle.
// Larger configurations hash the owner into eight-way sets, preserving a
// bounded lookup domain and the existing single registered fill per cycle.
#ifndef __ARCH_ARM_CFD_RHS_FACTORS_HH__
#define __ARCH_ARM_CFD_RHS_FACTORS_HH__
#include <array>
#include <cstdint>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
class CfdRhsFactors {
  public:
    static constexpr unsigned Depth=2, MaxDepth=64, Reservation=512;
    static constexpr unsigned reservation(unsigned depth) { return 128*depth+256; }
    struct Slot {
        uint64_t id=0, token=0, epoch=0, filled=0, used=0;
        unsigned k=0;
        bool backward=false;
        std::array<double,5> value{};
    };
    std::array<Slot,MaxDepth> slots{};
    unsigned depth=Depth;
    uint64_t nextId=1, lastFill=UINT64_MAX;
    void configure(unsigned count) {
        if (count!=2 && count!=4 && count!=8 && count!=16 && count!=32 && count!=64)
            throw std::logic_error("RHS factor depth must be 2, 4, 8, 16, 32 or 64");
        for (const auto &s : slots)
            if (s.id) throw std::logic_error("RHS factor reconfiguration while live");
        depth=count;
    }
    unsigned first(uint64_t token) const {
        return depth<=8 ? 0 : ((token^(token>>3))%(depth/8))*8;
    }
    unsigned ways() const { return depth<8 ? depth : 8; }
    int lookup(uint64_t token, uint64_t epoch, bool backward,
               unsigned k, uint64_t now) const {
        for (unsigned i=first(token);i<first(token)+ways();++i) {
            const auto &s=slots[i];
            if (s.id && s.token==token && s.epoch==epoch &&
                s.backward==backward && s.k==k && s.filled<now) return i;
        }
        return -1;
    }
    int victim(uint64_t token, uint64_t epoch, bool backward,
               unsigned k, uint64_t now) const {
        if (lastFill==now) return -1;
        int chosen=-1;
        for (unsigned i=first(token);i<first(token)+ways();++i) {
            const auto &s=slots[i];
            if (s.id && s.token==token && s.epoch==epoch &&
                s.backward==backward && s.k==k) return -1;
            if (!s.id) { if (chosen<0 || slots[chosen].id) chosen=i; }
            else if (s.used!=now && (chosen<0 ||
                     (slots[chosen].id && s.used<slots[chosen].used))) chosen=i;
        }
        return chosen;
    }
    void fill(unsigned index, uint64_t token, uint64_t epoch,
              bool backward, unsigned k, uint64_t now,
              const std::array<double,5> &value) {
        if (index>=depth || k>=4 || int(index)!=victim(token,epoch,backward,k,now))
            throw std::logic_error("RHS factor latch overwrite/ingress limit");
        slots.at(index)={nextId++,token,epoch,now,now,k,backward,value};
        lastFill=now;
    }
    void capture(unsigned index, uint64_t id, uint64_t now) {
        if (index>=depth) throw std::logic_error("RHS factor capture outside active depth");
        auto &s=slots.at(index);
        if (!id || s.id!=id || s.filled>=now || s.used==now)
            throw std::logic_error("stale/unregistered/duplicate RHS factor capture");
        s.used=now;
    }
    void clear(unsigned index, uint64_t id) {
        if (index>=depth) throw std::logic_error("RHS factor clear outside active depth");
        auto &s=slots.at(index);
        if (!id || s.id!=id) throw std::logic_error("stale RHS factor clear");
        s.id=0;
    }
};
static_assert(sizeof(CfdRhsFactors::Slot) <= 128 &&
              sizeof(CfdRhsFactors) <= CfdRhsFactors::reservation(CfdRhsFactors::MaxDepth),
              "RHS factor latches exceed reservation");
} }
#endif
