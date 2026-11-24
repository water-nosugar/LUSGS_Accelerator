#include "arch/arm/cfd_summary_snapshots.hh"
#include "arch/arm/cfd_summary_hash.hh"
#include <cstdio>
using namespace gem5::ArmISA;
int main() {
    CfdSummarySnapshots q; q.configure(2);
    auto *first=q.allocate(10,1,9); auto *second=q.allocate(10,2,9);
    assert(first && second && !q.allocate(10,3,9));
    assert(!q.copyCandidate(10) && q.copyCandidate(11)==first);
    for (unsigned c=11;c<=15;++c) {
        auto *e=q.copyCandidate(c); assert(e==first);
        for (unsigned i=0;i<8;++i) e->words[e->copied++]=e->copied;
    }
    first->ready=15;
    assert(!q.readyCandidate(15) && q.readyCandidate(16)==first);
    first->hashing=true;
    assert(q.copyCandidate(16)==second && !q.readyCandidate(16));
    CfdSummaryHash hash;hash.start(16,first->token,first->owner,first->words[0]);
    for (unsigned i=0;i<40;++i) {
        assert(!hash.ready(17+2*i) && hash.ready(18+2*i));hash.commit(18+2*i);
        if (i<39) hash.issue(18+2*i,first->words[i+1]);
    }
    assert(hash.done() && q.live==2);
    q.release(*first);assert(q.live==1 && q.find(1)==nullptr && q.find(2)==second);
    auto *reused=q.allocate(96,3,10);assert(reused==first && !reused->copied && !reused->hashing);
    q.release(*second);q.release(*reused);assert(!q.live && q.peak==2);
    std::puts("SUMMARY_SNAPSHOTS_PASS");
}
