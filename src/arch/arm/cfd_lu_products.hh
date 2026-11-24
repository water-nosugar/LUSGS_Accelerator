// Two bounded LU product vectors; consumers retain the original k order.
#ifndef __ARCH_ARM_CFD_LU_PRODUCTS_HH__
#define __ARCH_ARM_CFD_LU_PRODUCTS_HH__
#include <array>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
class CfdLuProducts {
  public:
    enum State { Empty, Producing, Ready };
    struct Slot {
        std::array<double,5> value{};
        int k = -1;
        State state = Empty;
    };
    std::array<Slot,2> slots{};
    unsigned terms = 0, nextMul = 0, nextSub = 0;
    bool subtractPending = false;
    static unsigned reservation(unsigned contexts) { return 128*contexts+128; }
    void begin(unsigned count) {
        if (count > 4 || subtractPending || nextSub != terms ||
            slots[0].state != Empty || slots[1].state != Empty)
            throw std::logic_error("LU product window reset before drain");
        terms=count; nextMul=nextSub=0;
    }
    bool canProduce() const {
        return nextMul < terms && slots[nextMul%2].state == Empty;
    }
    void produce(unsigned k) {
        if (k != nextMul || !canProduce())
            throw std::logic_error("LU product window overwrite/order");
        auto &s=slots[k%2];s.k=k;s.state=Producing;++nextMul;
    }
    void publish(unsigned k, const std::array<double,5> &value) {
        auto &s=slots.at(k%2);
        if (s.k != int(k) || s.state != Producing)
            throw std::logic_error("stale LU product return");
        s.value=value;s.state=Ready;
    }
    bool canSubtract() const {
        const auto &s=slots[nextSub%2];
        return !subtractPending && nextSub < terms &&
            s.k == int(nextSub) && s.state == Ready;
    }
    void capture(unsigned k) {
        if (k != nextSub || !canSubtract())
            throw std::logic_error("LU subtract bypassed product/order");
        slots[k%2].state=Empty;subtractPending=true;
    }
    bool complete(unsigned k) {
        if (!subtractPending || k != nextSub)
            throw std::logic_error("stale LU subtract return");
        subtractPending=false;++nextSub;
        return nextSub == terms;
    }
};
static_assert(sizeof(CfdLuProducts) <= 128, "LU window reservation too small");
} }
#endif
