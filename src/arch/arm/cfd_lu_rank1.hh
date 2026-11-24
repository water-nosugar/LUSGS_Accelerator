// Right-looking Crout updates retain the original k order at every element.
// Two five-word product registers are reused; trailing accumulators remain
// in the existing owned matrix RF and pay its normal reads and real writes.
#ifndef __ARCH_ARM_CFD_LU_RANK1_HH__
#define __ARCH_ARM_CFD_LU_RANK1_HH__
#include <array>
#include <cstdint>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
class CfdLuRank1 {
  public:
    enum class Phase { Empty, Pivot, Reciprocal, Scale, RowsReady, Rows,
                       Finishing, Done, Cancelled };
    struct Product {
        uint64_t operation=0;
        unsigned row=0;
        bool ready=false;
        std::array<double,5> value{};
    };
    static constexpr unsigned reservation(unsigned contexts) {
        return 512*contexts+128;
    }
    uint64_t owner=0, epoch=0, eligible=0, pivotId=0, reciprocalReturned=0;
    uint64_t scaleId=0, fmaEligible=0, fusedLanes=0;
    uint64_t prefetchId=0, prefetchReturned=0, diagonalId=0;
    unsigned prefetchColumn=0;
    unsigned column=0, nextMul=0, nextSub=0, completed=0, inflight=0;
    Phase phase=Phase::Empty;
    bool foldReady=false;
    std::array<Product,2> products{};
    std::array<uint64_t,5> subtracts{};

    void bind(uint64_t token,uint64_t generation,uint64_t now,bool folded=false) {
        if (!token || inflight || (phase!=Phase::Empty && phase!=Phase::Done &&
            phase!=Phase::Cancelled)) fail("rank1 context reused while live");
        *this=CfdLuRank1{};
        owner=token;epoch=generation;foldReady=folded;eligible=now+(foldReady ? 0 : 1);phase=Phase::Pivot;
    }
    bool canPivot(uint64_t now) const { return phase==Phase::Pivot && now>=eligible; }
    bool canPrefetch() const {
        return phase==Phase::Rows && column<4 && !prefetchId &&
            diagonalId && (completed&(1u<<(column+1)));
    }
    void prefetch(uint64_t id) {
        if (!id || !canPrefetch()) fail("rank1 prefetch before actual diagonal row return");
        prefetchId=id;prefetchReturned=0;prefetchColumn=column+1;++inflight;
    }
    bool prefetched(uint64_t id,uint64_t now) {
        if (id==pivotId && !prefetchId) return reciprocal(id,now);
        if (!id || id!=prefetchId || prefetchReturned || !inflight)
            fail("rank1 stale prefetched reciprocal return");
        prefetchReturned=now;--inflight;
        return phase!=Phase::Cancelled;
    }
    void adoptPrefetch(uint64_t now) {
        if (!canPivot(now) || !prefetchId || prefetchColumn!=column)
            fail("rank1 prefetch adopted before column barrier");
        pivotId=prefetchId;reciprocalReturned=prefetchReturned;
        prefetchId=prefetchReturned=0;prefetchColumn=0;
        phase=column==4 ? Phase::Finishing : Phase::Reciprocal;eligible=now+1;
    }
    void pivot(uint64_t id,uint64_t now) {
        if (!id || !canPivot(now)) fail("rank1 pivot before registered column");
        pivotId=id;reciprocalReturned=0;++inflight;
        phase=column==4 ? Phase::Finishing : Phase::Reciprocal;
        eligible=now+1;
    }
    bool reciprocal(uint64_t id,uint64_t now) {
        if (!id || id!=pivotId || reciprocalReturned || !inflight)
            fail("rank1 stale reciprocal return");
        reciprocalReturned=now;--inflight;
        return phase!=Phase::Cancelled;
    }
    bool canScale() const { return phase==Phase::Reciprocal && reciprocalReturned; }
    void scale(uint64_t id) {
        if (!id || !canScale()) fail("rank1 normalization before real reciprocal");
        scaleId=id;++inflight;phase=Phase::Scale;
    }
    bool scaled(uint64_t id,uint64_t now) {
        if (!id || id!=scaleId || !inflight ||
            (phase!=Phase::Scale && phase!=Phase::Cancelled))
            fail("rank1 stale normalization return");
        --inflight;scaleId=0;
        if (phase==Phase::Cancelled) return false;
        phase=Phase::RowsReady;eligible=now+(foldReady ? 0 : 1);
        return true;
    }
    bool canBeginRows(uint64_t now) const {
        return phase==Phase::RowsReady && now>=eligible;
    }
    void beginRows(uint64_t now) {
        if (!canBeginRows(now) || inflight) fail("rank1 row publication before RF return");
        phase=Phase::Rows;nextMul=nextSub=column+1;completed=0;diagonalId=0;
    }
    int mulSlot() const {
        if (phase!=Phase::Rows || nextMul>=5) return -1;
        for (unsigned i=0;i<products.size();++i) if (!products[i].operation) return i;
        return -1;
    }
    int subSlot() const {
        if (phase!=Phase::Rows || nextSub>=5) return -1;
        for (unsigned i=0;i<products.size();++i)
            if (products[i].operation && products[i].ready && products[i].row==nextSub) return i;
        return -1;
    }
    void multiply(unsigned slot,unsigned row,uint64_t id) {
        if (!id || int(slot)!=mulSlot() || row!=nextMul)
            fail("rank1 product capacity/order exceeded");
        products.at(slot)=Product{id,row,false,{}};++nextMul;++inflight;
    }
    bool multiplied(unsigned slot,uint64_t id,const std::array<double,5> &value) {
        auto &p=products.at(slot);
        if (!id || p.operation!=id || p.ready || !inflight)
            fail("rank1 stale product return");
        --inflight;
        if (phase==Phase::Cancelled) { p=Product{};return false; }
        p.value=value;p.ready=true;return true;
    }
    void subtract(unsigned slot,unsigned row,uint64_t id) {
        if (!id || int(slot)!=subSlot() || row!=nextSub || subtracts.at(row))
            fail("rank1 subtraction before product return/order");
        products.at(slot)=Product{};subtracts[row]=id;++nextSub;++inflight;
    }
    bool canFma(uint64_t now) const {
        return phase==Phase::Rows && nextMul==nextSub && nextSub<5 && now>=fmaEligible;
    }
    void fused(unsigned row,uint64_t id,uint64_t now) {
        if (!id || !canFma(now) || row!=nextSub || subtracts.at(row))
            fail("rank1 FMA row capacity/order exceeded");
        for (const auto &p:products) if (p.operation)
            fail("rank1 fused row cannot borrow a live split product");
        subtracts[row]=id;++nextMul;++nextSub;++inflight;fmaEligible=now+1;
        fusedLanes+=4-column;
    }
    bool subtracted(unsigned row,uint64_t id,uint64_t now) {
        if (!id || subtracts.at(row)!=id || !inflight)
            fail("rank1 stale subtraction return");
        subtracts[row]=0;--inflight;
        if (phase==Phase::Cancelled) return false;
        completed|=1u<<row;
        if (row==column+1) diagonalId=id;
        const unsigned wanted=0x1f & ~((1u<<(column+1))-1);
        if (completed==wanted) {
            const unsigned prefetchPending=prefetchId && !prefetchReturned ? 1 : 0;
            if (inflight!=prefetchPending || nextMul!=5 || nextSub!=5)
                fail("rank1 incomplete trailing update");
            ++column;phase=Phase::Pivot;eligible=now+(foldReady ? 0 : 1);
        }
        return true;
    }
    bool canFinish(uint64_t now) const { return phase==Phase::Finishing && now>=eligible; }
    void finish(uint64_t now) {
        if (!canFinish(now)) fail("rank1 final publication before registered pivot");
        phase=Phase::Done;
    }
    void cancel() {
        if (phase==Phase::Empty || phase==Phase::Cancelled) return;
        phase=Phase::Cancelled;
        for (auto &p:products) if (p.ready) p=Product{};
    }
  private:
    static void fail(const char *message) { throw std::logic_error(message); }
};
static_assert(sizeof(CfdLuRank1)<=512,"rank1 LU context exceeds reservation");
} }
#endif
