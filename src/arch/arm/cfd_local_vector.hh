// Bounded vector-register ports. Data lives in the existing owned contexts.
#ifndef __ARCH_ARM_CFD_LOCAL_VECTOR_HH__
#define __ARCH_ARM_CFD_LOCAL_VECTOR_HH__
#include <array>
#include <stdexcept>
#include <vector>
#include "arch/arm/cfd_register_ports.hh"
namespace gem5 { namespace ArmISA {
struct CfdLocalAccess {
    int a = -1, b = -1, destination = -1;
    unsigned wordsA = 0, wordsB = 0;
    unsigned globalReads = 0;
    uint64_t owner = 0, epoch = 0;
    uint64_t rhsPair = 0;
    uint64_t rhsFactor = 0;
    // Admission/trace witnesses, not retained factor operands. An admitted
    // MUL captures its inputs in the existing bounded FU operation latches.
    uint64_t rhsFactorSource = 0;
    unsigned rhsFactorWords = 0;
    unsigned pairSide = 0;
};
// Five-wide MVM operand placement. Products-only keeps the dependent tree
// partials in the shared RF: only independent products pay local publication.
// Local reads still require the producing owner/epoch and registered route.
inline CfdLocalAccess
cfdMvmAccess(unsigned context, int node, bool multiply, bool feedback,
             bool productsOnly, bool broadcast, uint64_t owner, uint64_t epoch)
{
    if (context >= 3 || node < 0 || node >= 5 ||
        (!multiply && !feedback && node >= 4))
        throw std::invalid_argument("invalid local MVM operation");
    CfdLocalAccess access;
    access.owner = owner;
    access.epoch = epoch;
    const int base = 9*context;
    if (multiply) {
        access.destination = base+node;
        access.globalReads = broadcast ? 6 : 10;
        return access;
    }
    const int left = feedback ?
        (node == 0 ? -1 : node == 1 ? 1 : node == 2 ? 3 : node == 3 ? 5 : 8) :
        (node < 2 ? 2*node : node == 2 ? 5 : 7);
    const int right = feedback ?
        (node == 0 ? 0 : node == 1 ? 2 : node == 2 ? 4 : node == 3 ? 6 : 7) :
        (node < 2 ? 2*node+1 : node == 2 ? 6 : 4);
    const bool localLeft = left >= 0 && (!productsOnly || left < 5);
    const bool localRight = !productsOnly || right < 5;
    access.a = localLeft ? base+left : -1;
    access.b = localRight ? base+right : -1;
    access.wordsA = localLeft ? 5 : 0;
    access.wordsB = localRight ? 5 : 0;
    access.globalReads = 10-access.wordsA-access.wordsB;
    if (!productsOnly && node != (feedback ? 4 : 3))
        access.destination = base+5+node;
    return access;
}
class CfdLocalVectorPorts {
  public:
    static constexpr unsigned Tags = 32;
    unsigned live = 0, peak = 0;
    std::vector<CfdRegisterPorts> banks;
    void configure(unsigned count) {
        banks.resize(count);
        for (auto &b : banks) b.configure({1,1});
    }
    bool canRead(uint64_t now, const CfdLocalAccess &a) {
        if (live == Tags) return false;
        if (a.a >= 0 && a.a == a.b) return false;
        for (int i : {a.a,a.b})
            if (i >= 0 && !banks.at(i).canRead(now,1)) return false;
        return true;
    }
    void accept(uint64_t now, const CfdLocalAccess &a) {
        if (!canRead(now,a)) throw std::logic_error("local vector credit lost");
        for (int i : {a.a,a.b})
            if (i >= 0 && !banks.at(i).reserveReads(now,1))
                throw std::logic_error("local vector read lost");
        ++live; peak=std::max(peak,live);
    }
    bool write(uint64_t now, unsigned bank) {
        return banks.at(bank).reserveWrite(now,1);
    }
    void release() {
        if (!live) throw std::logic_error("local vector tag underflow");
        --live;
    }
};
} }
#endif
