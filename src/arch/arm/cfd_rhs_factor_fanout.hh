// One same-cycle factor vector wire, including its paid first consumer.
// Optional bounded hardware; first registered cache read remains fully paid.
#ifndef __ARCH_ARM_CFD_RHS_FACTOR_FANOUT_HH__
#define __ARCH_ARM_CFD_RHS_FACTOR_FANOUT_HH__
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include "arch/arm/cfd_register_ports.hh"
#include "arch/arm/cfd_rhs_factors.hh"

namespace gem5 { namespace ArmISA {

class CfdRhsFactorFanout {
  public:
    static constexpr unsigned Reservation=256, Consumers=2;
    struct Transfer {
        uint64_t cycle=0, sourceRf=0, token=0, epoch=0, cache=0;
        unsigned slot=0, bank=0, k=0, width=0, sourceRhs=0;
        bool backward=false;
        std::array<double,5> value{};
    };

    const Transfer &source(uint64_t now) const {
        if (!valid || !claimed || now!=lastCycle || now!=wire.cycle)
            throw std::logic_error("expired factor fanout source");
        return wire;
    }

    // Called only AFTER an accepted cache-hit operation has paid its ordinary
    // local bank read and CfdRhsFactors::capture has recorded that use. A fill
    // cannot serve as the source: filled < now must still hold. The independent
    // integration checker must additionally bind sourceRf to that physical op.
    bool offer(const CfdRhsFactors::Slot &s, unsigned slot, unsigned bank,
               const CfdRegisterPorts &port, uint64_t now, uint64_t sourceRf,
               unsigned rhs) {
        if (!s.id || !s.token || s.k>=4 || slot>=CfdRhsFactors::MaxDepth ||
            s.filled>=now || s.used!=now || !sourceRf || rhs>=15 ||
            port.configuration().readWordsPerCycle!=1 ||
            !port.currentCycle() || *port.currentCycle()!=now ||
            port.readsUsed()!=1)
            throw std::logic_error("factor fanout source is not a paid registered read");
        if (haveCycle && now<lastCycle)
            throw std::logic_error("factor fanout cycle moved backwards");
        if (!haveCycle || now>lastCycle) {
            haveCycle=true;lastCycle=now;claimed=false;valid=false;consumers=0;
        }
        if (claimed) {
            if (sourceRf==wire.sourceRf)
                throw std::logic_error("factor source offered twice");
            return false;
        }
        wire={now,sourceRf,s.token,s.epoch,s.id,slot,bank,s.k,4-s.k,rhs,
              s.backward,s.value};
        claimed=valid=true;
        consumers=1;
        return true;
    }

    // Probe is side-effect free. A failed FU/global-RF admission must not
    // consume the second receiver. No bank credit is reserved here.
    const Transfer *probe(const CfdRhsFactors::Slot &s, unsigned slot,
                          unsigned bank, uint64_t now, unsigned rhs,
                          unsigned width) const {
        if (!valid || !claimed || consumers>=Consumers ||
            now!=lastCycle || now!=wire.cycle || rhs>=15 || rhs==wire.sourceRhs ||
            s.id!=wire.cache || s.token!=wire.token || s.epoch!=wire.epoch ||
            s.k!=wire.k || s.backward!=wire.backward || s.filled>=now ||
            s.used!=now || slot!=wire.slot || bank!=wire.bank ||
            width!=wire.width)
            return nullptr;
        return &wire;
    }

    // Commit only after the second MUL has actually been admitted. Its own
    // row/scalar operands and FU/result credits remain paid by the caller.
    // Returned values are copied into existing bounded operation latches.
    Transfer commit(const CfdRhsFactors::Slot &s, unsigned slot, unsigned bank,
                    uint64_t now, unsigned rhs, unsigned width,
                    uint64_t expectedSource, uint64_t consumerRf) {
        const auto *source=probe(s,slot,bank,now,rhs,width);
        if (!source || source->sourceRf!=expectedSource || consumerRf<=expectedSource)
            throw std::logic_error("stale or excess factor fanout receiver");
        // Host assertion, not a data-dependent arbitration or extra hardware
        // comparator: a live cache identity must never mutate its payload.
        if (std::memcmp(s.value.data(),wire.value.data(),sizeof wire.value)!=0)
            throw std::logic_error("factor payload mutated under its cache identity");
        ++consumers;
        return wire;
    }

    void invalidateOwner(uint64_t owner, uint64_t epoch) {
        if (claimed && wire.token==owner && wire.epoch==epoch)
            valid=false;
        // A cancelled transfer still used this cycle's physical bus slot.
    }
    void invalidateSlot(unsigned slot, uint64_t cache) {
        if (claimed && wire.slot==slot && wire.cache==cache)
            valid=false;
    }

  private:
    Transfer wire{};
    uint64_t lastCycle=0;
    unsigned consumers=0;
    bool haveCycle=false, claimed=false, valid=false;
};

static_assert(sizeof(CfdRhsFactorFanout)<=CfdRhsFactorFanout::Reservation,
              "factor broadcast wire exceeds its finite reservation");
} }
#endif
