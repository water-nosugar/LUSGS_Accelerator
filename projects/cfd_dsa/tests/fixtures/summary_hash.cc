#include "arch/arm/cfd_summary_hash.hh"
#include <cstdio>
using gem5::ArmISA::CfdSummaryHash;
int main()
{
    CfdSummaryHash engine;
    uint64_t expected=14695981039346656037ULL;
    engine.start(100,1,9,0);
    for (unsigned i=0;i<40;++i) {
        const uint64_t at=102+2*i;
        assert(engine.live && engine.pending && engine.index==i);
        assert(!engine.ready(at-1) && engine.ready(at));
        expected=(expected^i)*1099511628211ULL;
        engine.commit(at);
        assert(engine.digest==expected && engine.done()==(i==39));
        if (i<39) engine.issue(at,i+1);
    }
    assert(engine.done() && engine.index==40);
    engine=CfdSummaryHash{};
    engine.start(200,2,9,123);
    assert(!engine.cancel(201) && engine.live);
    assert(engine.cancel(202) && !engine.live);
    engine.start(202,3,10,77);
    engine.commit(204);
    assert(engine.token==3 && engine.owner==10 && engine.index==1);
    assert(engine.digest==((14695981039346656037ULL^77)*1099511628211ULL));
    std::puts("SUMMARY_HASH_PASS");
}
