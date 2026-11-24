void
CfdCoeffPreprocessController::processSummarySnapshots(uint64_t now)
{
    auto release = [this](CfdSummarySnapshots::Entry &entry, bool dropped) {
        auto line = lineRequests.find(entry.owner);
        if (line != lineRequests.end()) {
            panic_if(!line->second->recordsPending,"CFD snapshot pending underflow");
            line->second->recordsPending--;
        }
        if (dropped) snapshotDropped++; else snapshotCompleted++;
        summarySnapshots.release(entry);
    };
    // Every engine is an explicit 2-cycle integer XOR/constant-multiply unit.
    // Engines access distinct, owned record entries, never the floating RF.
    for (unsigned index=0; index<summaryEngines.size(); ++index) {
        auto &engine = summaryEngines[index];
        if (!engine.ready(now)) continue;
        auto *entry = summarySnapshots.find(engine.token);
        panic_if(!entry || !entry->hashing,"CFD hash lost its snapshot");
        auto line = lineRequests.find(entry->owner);
        if (line == lineRequests.end() || line->second->abortPending) {
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=drop token=%llu owner=%llu engine=%u snapshot=1\n",
                    (unsigned long long)now,(unsigned long long)engine.token,
                    (unsigned long long)engine.owner,index);
            engine.cancel(now);
            release(*entry,true);
            continue;
        }
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=word token=%llu owner=%llu request=%llu index=%u word=%016llx before=%016llx after=%016llx engine=%u snapshot=1\n",
                (unsigned long long)now,(unsigned long long)engine.token,
                (unsigned long long)engine.owner,(unsigned long long)entry->words[0],
                engine.index,(unsigned long long)engine.word,
                (unsigned long long)engine.digest,(unsigned long long)engine.result,index);
        engine.commit(now);
        if (!engine.done()) {
            engine.issue(now,entry->words[engine.index]);
            continue;
        }
        panic_if(engine.digest != entry->expectedDigest,"CFD snapshot digest changed");
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=complete token=%llu owner=%llu request=%llu hash=%016llx engine=%u snapshot=1\n",
                (unsigned long long)now,(unsigned long long)engine.token,
                (unsigned long long)engine.owner,(unsigned long long)entry->words[0],
                (unsigned long long)engine.digest,index);
        // Temporary simulator view for the existing ABI/trace helper. Only the
        // 40-word snapshot is retained hardware state; no Request is kept here.
        Request view;
        unsigned word=0;
#define CFD_RECORD_FIELD(cpp, guest) view.record.cpp = entry->words[word++];
#include "arch/arm/cfd_compact_record_fields.inc"
#undef CFD_RECORD_FIELD
        view.token=entry->token; view.desc.generation=view.record.generation;
        view.desc.lineId=view.record.lineId; view.desc.cellId=view.record.cellId;
        view.stage=RequestStage::Complete;
        aggregateSummary(view,*line->second,engine.digest,true);
        engine=CfdSummaryHash{};
        release(*entry,false);
    }
    // Drop unissued snapshots immediately on abort. Registered hash operations
    // above must drain to a return boundary before their storage is reusable.
    for (auto &entry : summarySnapshots.entries) {
        if (!entry.live || entry.hashing) continue;
        auto line=lineRequests.find(entry.owner);
        if (line != lineRequests.end() && !line->second->abortPending) continue;
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SUMMARY_SNAPSHOT cycle=%llu event=drop token=%llu owner=%llu detached=%u\n",
                (unsigned long long)now,(unsigned long long)entry.token,
                (unsigned long long)entry.owner,entry.copied==CfdSummarySnapshots::Words);
        release(entry,true);
    }
    // Exactly one ingress transfer per tick, at most eight words (64 B).
    if (auto *entry=summarySnapshots.copyCandidate(now)) {
        auto *req=find(entry->token);
        panic_if(!req || req->stage != RequestStage::Complete,
                 "CFD snapshot copied a mutable or missing record");
        const unsigned first=entry->copied;
        for (unsigned n=0;n<CfdSummarySnapshots::CopyWidth;++n)
            entry->words[entry->copied++]=recordWord(req->record,first+n);
        if (env.traceEnable) {
            std::fprintf(stderr,"CFD_SUMMARY_SNAPSHOT cycle=%llu event=copy token=%llu owner=%llu first=%u words=8",
                (unsigned long long)now,(unsigned long long)entry->token,
                (unsigned long long)entry->owner,first);
            for (unsigned n=0;n<8;++n)
                std::fprintf(stderr," w%u=%016llx",n,(unsigned long long)entry->words[first+n]);
            std::fprintf(stderr,"\n");
        }
        if (entry->copied==CfdSummarySnapshots::Words) {
            entry->ready=now;
            trace(*req,"controller","REQUEST_SNAPSHOT_DETACH");
            trace(*req,"controller","REQUEST_AUTO_RETIRE");
            if (env.traceEnable)
                std::fprintf(stderr,"CFD_SUMMARY_SNAPSHOT cycle=%llu event=detach token=%llu owner=%llu request=%llu\n",
                    (unsigned long long)now,(unsigned long long)entry->token,
                    (unsigned long long)entry->owner,(unsigned long long)req->record.requestId);
            retireLineChild(entry->token,*req);
            onchipContexts.erase(entry->token);
            requests.erase(entry->token);
        }
    }
    for (unsigned index=0;index<summaryEngines.size();++index) {
        auto &engine=summaryEngines[index];
        if (engine.live) continue;
        auto *entry=summarySnapshots.readyCandidate(now);
        if (!entry) break;
        entry->hashing=true;
        engine.start(now,entry->token,entry->owner,entry->words[0]);
        if (env.traceEnable)
            std::fprintf(stderr,"CFD_SUMMARY_HASH cycle=%llu event=accept token=%llu owner=%llu request=%llu words=40 word_cycles=2 bytes=128 engine=%u snapshot=1\n",
                (unsigned long long)now,(unsigned long long)entry->token,
                (unsigned long long)entry->owner,(unsigned long long)entry->words[0],index);
    }
}
