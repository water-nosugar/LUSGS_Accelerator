// Two registered common-LU operand latches, with one selector, ingress and
// fanout port each cycle. Each latch feeds two independent RHS MUL groups.
// Reuse starts only after BOTH FU input captures. Existing MUL pipeline tags
// then own immutable operands/results until their ordinary actual returns.
#ifndef __ARCH_ARM_CFD_RHS_PAIRS_HH__
#define __ARCH_ARM_CFD_RHS_PAIRS_HH__
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
class CfdRhsPairs {
  public:
    static constexpr unsigned Depth=2, Reservation=768;
    struct Slot {
        uint64_t id=0, token=0, epoch=0, request=0, cell=0;
        uint64_t selected=0, loadedCycle=0;
        unsigned k=0;
        std::array<unsigned,2> rhs{};
        std::array<double,5> lu{};
        std::array<double,2> factor{};
        bool loaded=false, issued=false;
        unsigned captured=0;
    };
    std::array<Slot,Depth> slots{};
    uint64_t nextId=1;
    uint64_t lastSelect=std::numeric_limits<uint64_t>::max();
    uint64_t lastLoad=std::numeric_limits<uint64_t>::max();
    uint64_t lastIssue=std::numeric_limits<uint64_t>::max();
    bool live() const { return slots[0].id || slots[1].id; }
    int select(uint64_t token,uint64_t epoch,uint64_t request,uint64_t cell,
               unsigned a,unsigned b,unsigned k,uint64_t now) {
        if (a<10 || b>=15 || a>=b || k>2 || !token)
            throw std::logic_error("invalid common-LU pair");
        if (lastSelect==now) return -1;
        for (unsigned i=0;i<Depth;++i) if (!slots[i].id) {
            auto &s=slots[i];s={};s.id=nextId++;s.token=token;
            s.epoch=epoch;s.request=request;s.cell=cell;s.k=k;
            s.rhs={a,b};s.selected=now;lastSelect=now;return i;
        }
        return -1;
    }
    bool canLoad(unsigned i,uint64_t now) const {
        const auto &s=slots.at(i);
        return s.id && !s.loaded && now>s.selected && lastLoad!=now;
    }
    void load(unsigned i,uint64_t now,const std::array<double,5> &lu,
              const std::array<double,2> &factor) {
        if (!canLoad(i,now)) throw std::logic_error("common-LU ingress not ready");
        auto &s=slots[i];s.lu=lu;s.factor=factor;s.loaded=true;
        s.loadedCycle=now;lastLoad=now;
    }
    bool ready(unsigned i,uint64_t now) const {
        const auto &s=slots.at(i);
        return s.id && s.loaded && !s.issued && now>s.loadedCycle && lastIssue!=now;
    }
    void issue(unsigned i,uint64_t now) {
        if (!ready(i,now)) throw std::logic_error("common-LU fanout not ready");
        slots[i].issued=true;lastIssue=now;
    }
    bool capture(unsigned i,uint64_t id,unsigned side) {
        auto &s=slots.at(i);
        if (s.id!=id || !s.issued || side>=2 || (s.captured&(1u<<side)))
            throw std::logic_error("stale common-LU input capture");
        s.captured|=1u<<side;
        if (s.captured==3) { s={};return true; }
        return false;
    }
    void cancel(unsigned i) {
        if (!slots.at(i).id || slots[i].issued)
            throw std::logic_error("in-flight common-LU cancellation");
        slots[i]={};
    }
};
static_assert(sizeof(CfdRhsPairs)<=512,"common-LU payload/control exceeds paid slots");
} }
#endif
