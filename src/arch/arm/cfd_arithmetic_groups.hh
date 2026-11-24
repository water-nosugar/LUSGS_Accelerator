// Finite logical-operation scoreboard for the partition experiments.
// Physical FU/RF credits remain owned by the controller. A group publishes
// only after every physical fragment has returned and crossed its route.
#ifndef __ARCH_ARM_CFD_ARITHMETIC_GROUPS_HH__
#define __ARCH_ARM_CFD_ARITHMETIC_GROUPS_HH__

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5
{
class CfdArithmeticGroups
{
  public:
    enum Mode { Legacy = 0, Shared = 1, Fixed = 2, Borrow = 3 };
    static constexpr unsigned MaxGroups = 32;
    struct Group {
        uint64_t id = 0, accepted = 0, ready = 0, publish = 0;
        unsigned width = 0, issued = 0, returned = 0;
        unsigned caller = 0;
        bool mul = false, scan = false, broadcast = false, crossed = false;
        uint64_t owner = 0;
    };
    std::array<Group, MaxGroups> slots{};
    unsigned depth = 16, live = 0, peak = 0;
    uint64_t nextId = 1;

    bool canAdmit(bool scan, unsigned consumerReserve = 2) const
    {
        if (live >= depth) return false;
        if (scan) return true;
        const unsigned producers = std::count_if(slots.begin(),
            slots.begin()+depth, [](const Group &g) { return g.id && !g.scan; });
        return producers < depth-std::min(depth,consumerReserve);
    }

    int allocate(bool mul, bool scan, unsigned caller, unsigned width,
                 bool broadcast, uint64_t now, unsigned selectLatency,
                 uint64_t owner = 0)
    {
        assert(depth && depth <= MaxGroups && width && width <= 5);
        if (live == depth) return -1;
        for (unsigned i = 0; i < depth; ++i) {
            if (slots[i].id) continue;
            slots[i] = {nextId++, now, now+selectLatency, 0, width, 0, 0,
                        caller, mul, scan, broadcast, false, owner};
            peak = std::max(peak, ++live);
            return i;
        }
        assert(false);
        return -1;
    }

    std::vector<unsigned> readyOrder(uint64_t now, Mode mode,
                                     unsigned ageLimit) const
    {
        std::vector<unsigned> out;
        for (unsigned i=0; i<depth; ++i)
            if (slots[i].id && slots[i].ready <= now &&
                slots[i].issued < slots[i].width) out.push_back(i);
        std::sort(out.begin(),out.end(),[&](unsigned a,unsigned b) {
            const auto &x=slots[a], &y=slots[b];
            // Bounded waiting compensation precedes scan preference. FIFO
            // among aged groups prevents permanent producer starvation.
            if (mode == Borrow) {
                const bool oldX=now-x.accepted >= ageLimit;
                const bool oldY=now-y.accepted >= ageLimit;
                if (oldX != oldY) return oldX;
                if (!oldX && x.scan != y.scan) return x.scan;
            }
            return x.id < y.id;
        });
        return out;
    }

    static std::vector<unsigned> laneOrder(Mode mode, bool scan,
                                           unsigned count)
    {
        std::vector<unsigned> out;
        if (mode == Fixed) {
            for (unsigned i=scan ? 5 : 0; i<(scan ? count : 5); ++i)
                out.push_back(i);
        } else if (mode == Borrow && scan) {
            for (unsigned i=5; i<count; ++i) out.push_back(i);
            for (unsigned i=0; i<5; ++i) out.push_back(i);
        } else {
            for (unsigned i=0; i<count; ++i) out.push_back(i);
        }
        return out;
    }

    void returned(unsigned slot, unsigned offset, unsigned width,
                  uint64_t now, unsigned route, unsigned borrowRoute)
    {
        auto &g=slots.at(slot);
        assert(g.id && width && offset+width <= g.issued);
        const unsigned mask=((1u<<width)-1)<<offset;
        assert(!(g.returned & mask));
        g.returned |= mask;
        if (g.returned == (1u<<g.width)-1)
            g.publish=now+route+(g.crossed ? borrowRoute : 0);
    }

    bool publishable(unsigned i, uint64_t now) const
    {
        const auto &g=slots.at(i);
        return g.id && g.returned == (1u<<g.width)-1 && now >= g.publish;
    }
    void release(unsigned i)
    {
        assert(slots.at(i).id && live);
        slots[i]=Group{};
        --live;
    }
};
} // namespace gem5
#endif
