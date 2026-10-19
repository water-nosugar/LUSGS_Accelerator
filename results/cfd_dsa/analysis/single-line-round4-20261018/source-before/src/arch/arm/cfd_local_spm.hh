#ifndef __ARCH_ARM_CFD_LOCAL_SPM_HH__
#define __ARCH_ARM_CFD_LOCAL_SPM_HH__

#include <cstdint>
#include <cstdio>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "mem/packet.hh"
#include "mem/port_wrapper.hh"
#include "params/CfdLocalSpm.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class ExecContext;
class System;
class ThreadContext;

namespace ArmISA
{

enum class CfdSpmRequester : uint8_t
{
    PathA,
    Trsv5,
    Mrhs,
    CoeffInput,
    CoeffCompute,
    CoeffDrain,
};

enum class CfdSpmMatrix : uint8_t
{
    Unknown,
    D,
    L,
    U,
    Lu,
    DInv,
    LBar,
    UBar,
    RhsVector,
    BaseVector,
    CorrectionVector,
    DqStarVector,
    Count,
};

struct CfdSpmPacketIssue
{
    uint64_t packetId = 0;
    uint64_t completionCycle = 0;
    uint64_t portWaitCycles = 0;
    uint64_t bankWaitCycles = 0;
    uint32_t bank = 0;
    uint32_t port = 0;
    bool accepted = false;
    bool outstandingFull = false;
};

struct CfdLusgsControllerRecord;

class CfdLocalSpm : public SimObject
{
  public:
    CfdLocalSpm(const CfdLocalSpmParams &params);

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    void recordRead(Addr ea, int row_id, unsigned logical_size,
                    unsigned physical_size,
                    uint64_t latency, uint64_t read_ports,
                    uint64_t outstanding_limit, uint64_t banks,
                    bool bank_conflict_enabled, bool realistic,
                    bool event_model, uint64_t z_wb_ports,
                    uint64_t queue_size, uint64_t bank_granularity);

    void recordZaZero(uint64_t seq_num);
    void recordZaExecute(uint64_t seq_num, const double *col, double scalar);
    void recordZaPipeExecute(uint64_t seq_num, uint8_t k,
                             const double *col, double scalar);
    void recordZaOuterExecute(uint64_t seq_num, uint8_t k,
                              const double *col, double scalar);
    void recordZaCommit(uint64_t seq_num);
    void recordZaSquash(uint64_t seq_num);
    bool readZaCol0(double *dst, unsigned lanes);
    bool readZaPathBFinalCol(double *dst, unsigned lanes);
    bool readZaPathCOuterFinalCol(double *dst, unsigned lanes);
    bool readZaMovaOverride(double *dst, unsigned lanes);
    void recordPathABufferAlloc(uint64_t live);
    void recordPathABufferFree(uint64_t live);
      void recordPathABufferWrite();
      void recordPathABufferPack();
      void recordPathAFusedSub();
      void recordPathABufferFullStall();
    void recordPathABufferOverlapCycle();
    void recordPathABufferPackWhileDotpCycle();
    void recordPathADotpIssue(uint64_t seq_num);
    void recordPathADotpBlockedByFuBusy();
    void recordPathADotpBlockedByInputNotReady();
    void recordPathADotpBlockedByResultBufferFull();
    void recordPathADotpBlockedByPackStoreBackpressure();
    void recordPathADotpReadyButNotIssued();
    void recordPathAStreamLoadPort(unsigned port, bool dual_issue_cycle);
    void recordPathAEarlyDotpOpportunity();
    void recordPathAEarlyDotpStart(bool before_all_fields,
                                   uint64_t field_ready_wait,
                                   uint64_t vector_ready_wait);
    void recordPathAEarlyDotpDetail(bool dependency_ready_before_all,
                                    bool execute_started_before_all,
                                    bool complete_before_all,
                                    uint64_t ready_wait_cycles);
    void recordPathAWholeSlotBarrierStall(uint64_t cycles);
    void recordPathAInputBufferAlloc(uint64_t live);
    void recordPathAInputBufferFree(uint64_t live);
    void recordPathAInputBufferFullStall();
    void recordPathAAsyncStoreEnqueue(uint64_t live);
    void recordPathAAsyncStoreCommit(uint64_t live);
    void recordPathAAsyncStoreQueueFullStall();
    void recordPathCLmatPort(unsigned port, bool dual_issue_cycle);
    void recordPathCEarlyOuterStart(bool before_all_columns,
                                    uint64_t column_ready_wait,
                                    uint64_t vector_ready_wait,
                                    uint64_t za_token_wait);
    void recordPathCOuterFuIssue(uint64_t busy_cycles);
    void recordPathCMovaBoundaryWait(uint64_t cycles);
    void recordCfdTrace(const char *path, const char *inst, uint64_t seq_num,
                        int slot, int field, int lane, int k,
                        uint64_t ready0, uint64_t ready1,
                        uint64_t all_ready, const char *note);
    void recordTrsv5Execute(uint64_t seq_num, uint64_t configured_latency);
    void recordTrsv5RhsForwardedExecute(uint64_t seq_num,
                                        uint64_t configured_latency);
    void recordTrsv5RhsForwardStall(uint64_t cycles);
    void recordTrsv5RhsForwardInvalid();
    void recordTrsm5MrhsExecute(uint64_t seq_num,
                                uint64_t configured_latency);
    void recordTrsm5InvLbarExecute(uint64_t seq_num,
                                   uint64_t configured_latency);
    void recordTrsm5Coeff3Execute(uint64_t seq_num,
                                  uint64_t configured_latency);
    void writeLineBuffer(bool forward, uint64_t line_id, uint64_t cell_id,
                         const double *value);
    bool readLineBuffer(bool forward, uint64_t line_id, uint64_t cell_id,
                        double *dst);
    void recordLineBufferStall(uint64_t cycles);
    void recordLusgsController(const CfdLusgsControllerRecord &record);
    CfdSpmPacketIssue issuePacket(
        CfdSpmRequester requester, bool write, Addr addr, unsigned bytes,
        uint64_t latency, uint64_t ports, uint64_t outstanding,
        uint64_t banks, uint64_t bank_granularity,
        CfdSpmMatrix matrix,
        std::function<void(uint64_t)> completion);
    CfdSpmPacketIssue issueDma(
        ThreadContext *tc, bool write, Addr vaddr, unsigned bytes,
        uint8_t *data, uint64_t outstanding,
        std::function<void(uint64_t)> completion);
    unsigned dmaChunkSize(
        ThreadContext *tc, Addr vaddr, unsigned requested) const;

  private:
    using ZaColumn = std::array<double, 5>;
    using ZaColumns = std::array<ZaColumn, 5>;

    struct ZaPendingUpdateEntry
    {
        uint64_t seqNum = 0;
        uint64_t inputToken = 0;
        uint64_t outputToken = 0;
        std::array<double, 5> value = {};
        ZaColumns cols = {};
        uint8_t pathBCol = 0;
        bool ready = false;
        bool committed = false;
        bool squashed = false;
        bool zero = false;
        bool pathB = false;
        bool pathCOuter = false;
    };

    struct PhysZAState
    {
        uint64_t physId = 0;
        uint64_t tokenId = 0;
        uint64_t producerSeqNum = 0;
        uint64_t inputToken = 0;
        std::array<double, 5> col0 = {};
        ZaColumns cols = {};
        uint8_t pathBCol = 0;
        bool ready = false;
        bool committed = false;
        bool squashed = false;
        bool zero = false;
        bool pathB = false;
        bool pathCOuter = false;
    };

    struct LocalSpmRequest
    {
        uint64_t requestId = 0;
        uint64_t seqNum = 0;
        Addr addr = 0;
        unsigned rowId = 0;
        unsigned bankId = 0;
        unsigned logicalBytes = 0;
        unsigned physicalBytes = 0;
        Tick issueTick = 0;
        Tick acceptTick = 0;
        Tick readStartTick = 0;
        Tick responseTick = 0;
        Tick zwbTick = 0;
        Tick completeTick = 0;
        bool accepted = false;
        bool readStarted = false;
        bool responded = false;
        bool zwbDone = false;
        bool completed = false;
        bool squashed = false;
        bool write = false;
        CfdSpmRequester requester = CfdSpmRequester::PathA;
        std::function<void(uint64_t)> completion;
    };

    struct LineBufferEntry
    {
        bool fwdValid = false;
        bool bwdValid = false;
        uint64_t lineId = 0;
        uint64_t fwdCellId = 0;
        uint64_t bwdCellId = 0;
        std::array<double, 5> prevDqStar = {};
        std::array<double, 5> nextDq = {};
    };

    void setCurrentZaValue(const std::array<double, 5> &value);
    void setCurrentZaCols(const ZaColumns &cols);
    std::array<double, 5> latestZaValue() const;
    ZaColumns latestZaCols() const;
    bool useZaRenameMode() const;
    PhysZAState *latestZaPhys();
    const PhysZAState *latestZaPhys() const;
    PhysZAState *allocateZaPhys(uint64_t seq_num, bool zero);
    void copyColumnToLegacyCol0(PhysZAState &state);
    void updateZaLiveStats();

    void resizeEventState(uint64_t read_ports, uint64_t banks,
                          uint64_t z_wb_ports, uint64_t queue_size);
    LocalSpmRequest &createLocalEventRequest(
        Addr ea, int row_id, unsigned logical_size, unsigned physical_size,
        uint64_t bank, uint64_t accept_cycle, uint64_t read_start_cycle,
        uint64_t response_cycle, uint64_t zwb_cycle, uint64_t complete_cycle,
        CfdSpmRequester requester, bool write,
        std::function<void(uint64_t)> completion);
    void scheduleLocalEvent(LocalSpmRequest &req, Tick when,
                            void (CfdLocalSpm::*handler)(uint64_t),
                            const char *name);
    LocalSpmRequest *findLocalEventRequest(uint64_t request_id);
    void onLocalEventAccept(uint64_t request_id);
    void onLocalEventReadStart(uint64_t request_id);
    void onLocalEventResponse(uint64_t request_id);
    void onLocalEventZwb(uint64_t request_id);
    void onLocalEventComplete(uint64_t request_id);
    void updateLocalEventLiveStats();
    void initTraceIfNeeded();
    void ensureLineBufferState();
    bool recvDmaTimingResp(PacketPtr pkt);
    void recvDmaReqRetry();
    void trySendDma();

    struct DmaSenderState : public Packet::SenderState
    {
        uint64_t packetId = 0;
        std::function<void(uint64_t)> completion;
    };

    std::vector<uint64_t> readPortNextFreeCycle;
    std::vector<uint64_t> writePortNextFreeCycle;
    std::vector<uint64_t> bankNextFreeCycle;
    std::vector<CfdSpmRequester> readPortLastRequester;
    std::vector<CfdSpmRequester> writePortLastRequester;
    std::vector<CfdSpmRequester> bankLastRequester;
    std::vector<uint64_t> zWbNextFreeCycle;
    std::vector<uint64_t> queueSlotFreeCycle;
    std::deque<LocalSpmRequest> localEventRequests;
    std::vector<std::unique_ptr<EventFunctionWrapper>> localEventCallbacks;
    std::vector<ZaPendingUpdateEntry> zaPendingEntries;
    std::vector<PhysZAState> zaPhysStates;
    std::vector<LineBufferEntry> lineBufferEntries;
    std::array<double, 5> zaCommittedCol0 = {};
    std::array<double, 5> zaCurrentCol0 = {};
    ZaColumns zaCommittedCols = {};
    ZaColumns zaCurrentCols = {};
    uint64_t zaToken = 0;
    uint64_t nextZaPhysId = 1;
    uint64_t archZaPhysId = 0;
    uint64_t zaPhysicalLive = 0;
    uint64_t nextLocalEventRequestId = 1;
    uint64_t localEventLiveRequestsCount = 0;
    uint64_t lastPathADotpIssueCycle = 0;
    uint64_t pathADotpBurstLen = 0;
    uint64_t pathADotpBurstCount = 0;
    bool seenPathADotpIssue = false;
    bool traceInitialized = false;
    bool traceEnabled = false;
    bool lineBufferInitialized = false;
    uint64_t traceEvents = 0;
    uint64_t traceMaxEvents = 0;
    FILE *traceFp = nullptr;
    System *system;
    RequestPortWrapper dmaPort;
    RequestorID dmaRequestorId;
    std::deque<PacketPtr> dmaPackets;
    PacketPtr dmaRetryPacket = nullptr;
    uint64_t nextDmaPacketId = 1;
    uint64_t dmaLivePackets = 0;

    statistics::Scalar bytesRead;
    statistics::Scalar logicalBytesRead;
    statistics::Scalar physicalBytesRead;
    statistics::Scalar numReads;
    statistics::Scalar readLatencyTotal;
    statistics::Scalar avgReadLatency;
    statistics::Scalar maxReadLatency;
    statistics::Scalar portBusyCycles;
    statistics::Scalar portConflictCycles;
    statistics::Scalar bankConflictCycles;
    statistics::Scalar queueFullCycles;
    statistics::Scalar zWritebackConflictCycles;
    statistics::Scalar requestToResponseTotal;
    statistics::Scalar avgRequestToResponse;
    statistics::Scalar responseToZWritebackTotal;
    statistics::Scalar avgZWritebackWait;
    statistics::Scalar maxZWritebackWait;
    statistics::Scalar outstandingReadsAvg;
    statistics::Scalar outstandingReadsMax;
    statistics::Scalar readsPerCycle;
    statistics::Scalar bytesPerCycle;
    statistics::Scalar logicalBytesPerCycle;
    statistics::Scalar physicalBytesPerCycle;

    statistics::Scalar zaTokensProduced;
    statistics::Scalar zaTokensConsumed;
    statistics::Scalar zaPendingUpdates;
    statistics::Scalar zaPendingQueueFull;
    statistics::Scalar zaSquashDiscards;
    statistics::Scalar zaCommitUpdates;
    statistics::Scalar zaMovaWaitCycles;
    statistics::Scalar zaNextZeroWaitCycles;
    statistics::Scalar zaForwardingStalls;
    statistics::Scalar zaRenameAllocs;
    statistics::Scalar zaRenameFrees;
    statistics::Scalar zaRenameRollbacks;
    statistics::Scalar zaPhysicalLiveMax;
    statistics::Scalar zaPhysicalLiveEnd;
    statistics::Scalar zaMovaRenameHits;
    statistics::Scalar zaMovaArchFallbacks;
    statistics::Scalar zaMovaBlockedNotReady;
    statistics::Scalar zaMovaReads;
    statistics::Scalar pathBPipeSteps;
    statistics::Scalar pathBPipePendingUpdates;
    statistics::Scalar pathBPipeCommitUpdates;
    statistics::Scalar pathBPipeSquashDiscards;
    statistics::Scalar pathBPipeCol0Writes;
    statistics::Scalar pathBPipeCol1Writes;
    statistics::Scalar pathBPipeCol2Writes;
    statistics::Scalar pathBPipeCol3Writes;
    statistics::Scalar pathBPipeCol4Writes;
    statistics::Scalar pathBPipeFinalReads;
    statistics::Scalar pathBPipeForwardHits;
    statistics::Scalar pathBPipeForwardStalls;
    statistics::Scalar pathCOuterSteps;
    statistics::Scalar pathCOuterPendingUpdates;
    statistics::Scalar pathCOuterCommitUpdates;
    statistics::Scalar pathCOuterSquashDiscards;
    statistics::Scalar pathCOuterCol0Writes;
    statistics::Scalar pathCOuterCol1Writes;
    statistics::Scalar pathCOuterCol2Writes;
    statistics::Scalar pathCOuterCol3Writes;
    statistics::Scalar pathCOuterCol4Writes;
    statistics::Scalar pathCOuterFinalReads;
    statistics::Scalar pathCOuterForwardHits;
    statistics::Scalar pathCOuterForwardStalls;
    statistics::Scalar pathCOuterMovaWaitCycles;
    statistics::Scalar pathCOuterNextZeroWaitCycles;
    statistics::Scalar localEventRequestsCreated;
    statistics::Scalar localEventRequestsAccepted;
    statistics::Scalar localEventRequestsCompleted;
    statistics::Scalar localEventRequestsSquashed;
    statistics::Scalar localEventAcceptEvents;
    statistics::Scalar localEventReadStartEvents;
    statistics::Scalar localEventResponseEvents;
    statistics::Scalar localEventZwbEvents;
    statistics::Scalar localEventCompleteEvents;
    statistics::Scalar localEventRetryEvents;
    statistics::Scalar localEventQueueFullEvents;
    statistics::Scalar localEventOutstandingFullEvents;
    statistics::Scalar localEventLiveRequests;
    statistics::Scalar localEventLiveRequestsMax;
    statistics::Scalar localEventLiveRequestsEnd;
    statistics::Scalar localEventIssueToAcceptTotal;
    statistics::Scalar localEventAcceptToReadStartTotal;
    statistics::Scalar localEventReadStartToResponseTotal;
    statistics::Scalar localEventResponseToZwbTotal;
    statistics::Scalar localEventZwbToCompleteTotal;
    statistics::Scalar localEventIssueToCompleteTotal;
    statistics::Scalar localEventAvgIssueToAccept;
    statistics::Scalar localEventAvgAcceptToReadStart;
    statistics::Scalar localEventAvgReadStartToResponse;
    statistics::Scalar localEventAvgResponseToZwb;
    statistics::Scalar localEventAvgZwbToComplete;
    statistics::Scalar localEventAvgIssueToComplete;
    statistics::Scalar coeffPacketIssued;
    statistics::Scalar coeffPacketCompleted;
    statistics::Scalar coeffPacketReadBytes;
    statistics::Scalar coeffPacketWriteBytes;
    statistics::Scalar coeffPacketOutstandingStalls;
    statistics::Scalar coeffPacketPortWaitCycles;
    statistics::Scalar coeffPacketBankWaitCycles;
    statistics::Vector coeffPacketBankAccesses;
    statistics::Vector coeffPacketBankConflicts;
    statistics::Vector coeffPacketBankWaitByBank;
    statistics::Vector coeffPacketBankMaxWait;
    statistics::Vector coeffPacketMatrixAccesses;
    statistics::Vector coeffPacketMatrixConflicts;
    statistics::Vector coeffPacketMatrixWaitCycles;
    statistics::Scalar coeffDmaIssued;
    statistics::Scalar coeffDmaCompleted;
    statistics::Scalar coeffDmaReadBytes;
    statistics::Scalar coeffDmaWriteBytes;
    statistics::Scalar coeffDmaOutstandingStalls;
    statistics::Scalar coeffDmaPortRetries;
    statistics::Scalar coeffDmaTranslationFailures;
    statistics::Scalar coeffDmaLiveMax;
    statistics::Scalar coeffRequestsDelayedByPathA;
    statistics::Scalar coeffRequestsDelayedByTrsv5;
    statistics::Scalar pathARequestsDelayedByCoeff;
    statistics::Scalar pathACoeffConflictCycles;
    statistics::Scalar trsvCoeffConflictCycles;
    statistics::Scalar vectorZwbRequests;
    statistics::Scalar vectorZwbLmatRequests;
    statistics::Scalar vectorZwbMovaRequests;
    statistics::Scalar vectorZwbConflicts;
    statistics::Scalar vectorZwbConflictCycles;
    statistics::Scalar vectorZwbWaitTotal;
    statistics::Scalar vectorZwbAvgWait;
    statistics::Scalar vectorZwbMaxWait;
    statistics::Scalar pathAResultBufferAllocs;
    statistics::Scalar pathAResultBufferFrees;
      statistics::Scalar pathAResultBufferWrites;
      statistics::Scalar pathAResultBufferPacks;
      statistics::Scalar pathAFusedSubOps;
      statistics::Scalar pathAResultBufferFullStalls;
    statistics::Scalar pathAResultBufferLiveMax;
    statistics::Scalar pathAResultBufferLiveEnd;
    statistics::Scalar pathAResultBufferOverlapCycles;
    statistics::Scalar pathAResultBufferPackWhileDotpCycles;
    statistics::Scalar pathADotpIssued;
    statistics::Scalar pathADotpIssueCycles;
    statistics::Scalar pathADotpIssueGaps;
    statistics::Scalar pathAMaxDotpIssueGap;
    statistics::Scalar pathAConsecutiveDotpBurstTotal;
    statistics::Scalar pathAConsecutiveDotpBurstMax;
    statistics::Scalar pathAConsecutiveDotpBurstAvg;
    statistics::Scalar pathADotpBlockedByFuBusy;
    statistics::Scalar pathADotpBlockedByInputNotReady;
    statistics::Scalar pathADotpBlockedByResultBufferFull;
    statistics::Scalar pathADotpBlockedByPackStoreBackpressure;
    statistics::Scalar pathADotpReadyButNotIssued;
    statistics::Scalar pathAEarlyDotpStartEvents;
    statistics::Scalar pathADotpIssuedBeforeAllFieldsReady;
    statistics::Scalar pathAEarlyReadyOpportunities;
    statistics::Scalar pathAEarlyReadyIssued;
    statistics::Scalar pathAEarlyReadyExecuteStarted;
    statistics::Scalar pathAEarlyReadyMissed;
    statistics::Scalar pathAEarlyReadyIssueEfficiency;
    statistics::Scalar pathAEarlyMissedDueToNotInIQ;
    statistics::Scalar pathAEarlyMissedDueToDotpFuBusy;
    statistics::Scalar pathAEarlyMissedDueToIssueWidth;
    statistics::Scalar pathAEarlyMissedDueToResultSlotBusy;
    statistics::Scalar pathAEarlyMissedDueToTokenNotReady;
    statistics::Scalar pathAEarlyMissedDueToOther;
    statistics::Scalar pathADotpDependencyReadyBeforeAllFields;
    statistics::Scalar pathADotpExecuteStartedBeforeAllFields;
    statistics::Scalar pathADotpCompletedBeforeAllFields;
    statistics::Scalar pathALoadComputeOverlapCycles;
    statistics::Scalar pathAFieldReadyWaitCycles;
    statistics::Scalar pathAWholeSlotBarrierStallCycles;
    statistics::Scalar pathAStreamReadPort0BusyCycles;
    statistics::Scalar pathAStreamReadPort1BusyCycles;
    statistics::Scalar pathADualLoadIssueCycles;
    statistics::Scalar pathADotpAfterFieldReadyTotal;
    statistics::Scalar pathADotpAfterVectorReadyTotal;
    statistics::Scalar pathADotpAvgAfterFieldReady;
    statistics::Scalar pathADotpAvgAfterVectorReady;
    statistics::Scalar pathAInputBufferAllocs;
    statistics::Scalar pathAInputBufferFrees;
    statistics::Scalar pathAInputBufferFullStalls;
    statistics::Scalar pathAInputBufferLiveMax;
    statistics::Scalar pathAInputBufferLiveEnd;
    statistics::Scalar pathAAsyncStoreEnqueues;
    statistics::Scalar pathAAsyncStoreCommits;
    statistics::Scalar pathAAsyncStoreQueueFullStalls;
    statistics::Scalar pathAAsyncStoreLiveMax;
    statistics::Scalar pathAAsyncStoreLiveEnd;
    statistics::Scalar pathCEarlyOuterStartEvents;
    statistics::Scalar pathCStepIssuedBeforeAllColumnsReady;
    statistics::Scalar pathCOuterFuIssueCount;
    statistics::Scalar pathCOuterFuBusyCycles;
    statistics::Scalar pathCOuterFuUtilization;
    statistics::Scalar pathCLoadOuterOverlapCycles;
    statistics::Scalar pathCColumnReadyWaitCycles;
    statistics::Scalar pathCZaTokenWaitCycles;
    statistics::Scalar pathCMovaBoundaryWaitCycles;
    statistics::Scalar pathCDualLoadIssueCycles;
    statistics::Scalar pathCLmatReadPort0BusyCycles;
    statistics::Scalar pathCLmatReadPort1BusyCycles;
    statistics::Scalar pathCStepAfterColumnReadyTotal;
    statistics::Scalar pathCStepAfterVectorReadyTotal;
    statistics::Scalar pathCStepAvgAfterColumnReady;
    statistics::Scalar pathCStepAvgAfterVectorReady;
    statistics::Scalar trsv5Issued;
    statistics::Scalar trsv5Completed;
    statistics::Scalar trsv5Squashed;
    statistics::Scalar trsv5BusyCycles;
    statistics::Scalar trsv5IdleCycles;
    statistics::Scalar trsv5FullStallCycles;
    statistics::Scalar trsv5DivOps;
    statistics::Scalar trsv5ForwardMulSubOps;
    statistics::Scalar trsv5BackwardMulSubOps;
    statistics::Scalar trsv5TotalMulSubOps;
    statistics::Scalar trsv5InputMatrixBytes;
    statistics::Scalar trsv5InputVectorBytes;
    statistics::Scalar trsv5OutputBytes;
    statistics::Scalar trsv5RhsForwarded;
    statistics::Scalar trsv5RhsSpmStageElided;
    statistics::Scalar trsv5RhsForwardStallCycles;
    statistics::Scalar trsv5RhsForwardInvalid;
    statistics::Scalar trsv5RhsForwardConsumed;
    statistics::Scalar trsv5LatencyConfigured;
    statistics::Scalar trsv5ObservedLatencyTotal;
    statistics::Scalar trsv5AverageObservedLatency;
    statistics::Scalar trsv5MaxInFlight;
    statistics::Scalar trsm5MrhsIssued;
    statistics::Scalar trsm5MrhsCompleted;
    statistics::Scalar trsm5MrhsInvalid;
    statistics::Scalar trsm5MrhsBusyCycles;
    statistics::Scalar trsm5MrhsStallCycles;
    statistics::Scalar trsm5MrhsColumnsSolved;
    statistics::Scalar trsm5MrhsInputLuBytes;
    statistics::Scalar trsm5MrhsInputRhsBytes;
    statistics::Scalar trsm5MrhsOutputBytes;
    statistics::Scalar trsm5MrhsLatencyConfigured;
    statistics::Scalar trsm5InvLbarIssued;
    statistics::Scalar trsm5InvLbarCompleted;
    statistics::Scalar trsm5InvLbarBusyCycles;
    statistics::Scalar trsm5InvLbarStallCycles;
    statistics::Scalar trsm5InvLbarLuReads;
    statistics::Scalar trsm5InvLbarColumnsSolved;
    statistics::Scalar trsm5InvLbarInputLuBytes;
    statistics::Scalar trsm5InvLbarInputCBytes;
    statistics::Scalar trsm5InvLbarOutputBytes;
    statistics::Scalar trsm5InvLbarLatencyConfigured;
    statistics::Scalar trsm5Coeff3Issued;
    statistics::Scalar trsm5Coeff3Completed;
    statistics::Scalar trsm5Coeff3BusyCycles;
    statistics::Scalar trsm5Coeff3StallCycles;
    statistics::Scalar trsm5Coeff3LuReads;
    statistics::Scalar trsm5Coeff3ColumnsSolved;
    statistics::Scalar trsm5Coeff3InputBytes;
    statistics::Scalar trsm5Coeff3OutputBytes;
    statistics::Scalar trsm5Coeff3LatencyConfigured;
    statistics::Scalar linebufForwardReads;
    statistics::Scalar linebufForwardWrites;
    statistics::Scalar linebufBackwardReads;
    statistics::Scalar linebufBackwardWrites;
    statistics::Scalar linebufForwardHits;
    statistics::Scalar linebufForwardMisses;
    statistics::Scalar linebufBackwardHits;
    statistics::Scalar linebufBackwardMisses;
    statistics::Scalar linebufTagConflicts;
    statistics::Scalar linebufInvalidReads;
    statistics::Scalar linebufStallCycles;
    statistics::Scalar lusgsControllerLaunches;
    statistics::Scalar lusgsControllerCommittedLaunches;
    statistics::Scalar lusgsControllerSquashedLaunches;
    statistics::Scalar lusgsControllerCompletedTasks;
    statistics::Scalar lusgsControllerFailedTasks;
    statistics::Scalar lusgsControllerBusyCycles;
    statistics::Scalar lusgsControllerTotalCycles;
    statistics::Scalar lusgsControllerForwardCells;
    statistics::Scalar lusgsControllerBackwardCells;
    statistics::Scalar lusgsControllerUpdatedQCells;
    statistics::Scalar lusgsControllerPathARequests;
    statistics::Scalar lusgsControllerTrsv5Requests;
    statistics::Scalar lusgsControllerVec5SubRequests;
    statistics::Scalar lusgsControllerVec5CopyRequests;
    statistics::Scalar lusgsControllerVec5AxpyRequests;
    statistics::Scalar lusgsControllerSpmReadRequests;
    statistics::Scalar lusgsControllerSpmWriteRequests;
    statistics::Scalar lusgsControllerPathAWaitCycles;
    statistics::Scalar lusgsControllerTrsv5WaitCycles;
    statistics::Scalar lusgsControllerVec5WaitCycles;
    statistics::Scalar lusgsControllerSpmWaitCycles;
    statistics::Scalar lusgsControllerWritebackWaitCycles;
    statistics::Scalar lusgsControllerForwardDependencyStalls;
    statistics::Scalar lusgsControllerBackwardDependencyStalls;
    statistics::Scalar lusgsControllerContextFullStalls;
    statistics::Scalar lusgsControllerNoReadyContextCycles;
    statistics::Scalar lusgsControllerArbiterStalls;
    statistics::Scalar lusgsControllerContextAlloc;
    statistics::Scalar lusgsControllerContextFree;
    statistics::Scalar lusgsControllerActiveContextCycles;
    statistics::Scalar lusgsControllerMaxActiveContexts;
    statistics::Scalar lusgsControllerContextSwitches;
    statistics::Scalar lusgsControllerLuBytes;
    statistics::Scalar lusgsControllerCBytes;
    statistics::Scalar lusgsControllerBbarBytes;
    statistics::Scalar lusgsControllerRhsBytes;
    statistics::Scalar lusgsControllerDqstarReadBytes;
    statistics::Scalar lusgsControllerDqstarWriteBytes;
    statistics::Scalar lusgsControllerDqReadBytes;
    statistics::Scalar lusgsControllerDqWriteBytes;
    statistics::Scalar lusgsControllerQReadBytes;
    statistics::Scalar lusgsControllerQWriteBytes;
    statistics::Scalar lusgsControllerTemporaryBytes;
    statistics::Scalar lusgsControllerTemporaryResultStores;
    statistics::Scalar lusgsControllerTemporaryResultLoads;
    statistics::Scalar lusgsControllerBufferReads;
    statistics::Scalar lusgsControllerBufferWrites;
    statistics::Scalar lusgsControllerDqstarExternalWrites;
    statistics::Scalar lusgsControllerDqstarExternalReads;
    statistics::Scalar lusgsControllerFinalDqWrites;
    statistics::Scalar lusgsControllerTileCoefficientBytes;
    statistics::Scalar lusgsControllerTileVectorBytes;
    statistics::Scalar lusgsControllerTileTemporaryBytes;
    statistics::Scalar lusgsControllerTileTotalBytes;
    statistics::Scalar lusgsControllerSpmCapacity;
    statistics::Scalar lusgsControllerTileLoads;
    statistics::Scalar lusgsControllerTilePrefetches;
    statistics::Scalar lusgsControllerPrefetchUseful;
    statistics::Scalar lusgsControllerPrefetchLate;
    statistics::Scalar lusgsControllerBufferSwap;
    statistics::Scalar lusgsControllerBufferConflictStalls;
    statistics::Scalar pathaArbiterCpuRequests;
    statistics::Scalar pathaArbiterLusgsRequests;
    statistics::Scalar pathaArbiterBusyStalls;
    statistics::Scalar pathaArbiterOwnershipCycles;
    statistics::Scalar lusgsEventLaunches;
    statistics::Scalar lusgsEventCompleted;
    statistics::Scalar lusgsEventFailed;
    statistics::Scalar lusgsEventActualCycles;
    statistics::Scalar lusgsEventEventsProcessed;
    statistics::Scalar lusgsEventSchedulerTicks;
    statistics::Scalar lusgsEventActiveCycles;
    statistics::Scalar lusgsEventIdleCycles;
    statistics::Scalar lusgsEventDecodedLaunches;
    statistics::Scalar lusgsEventCommittedLaunches;
    statistics::Scalar lusgsEventSquashedLaunches;
    statistics::Scalar lusgsEventSquashedWaits;
    statistics::Scalar lusgsEventStaleCompletions;
    statistics::Scalar lusgsEventUnexpectedCompletions;
    statistics::Scalar lusgsEventDuplicateCompletions;
    statistics::Scalar lusgsEventRequestIdMismatches;
    statistics::Scalar lusgsEventGenerationMismatches;
    statistics::Scalar lusgsEventWrongStateCompletions;
    statistics::Scalar lusgsEventPendingRequestAlloc;
    statistics::Scalar lusgsEventPendingRequestFree;
    statistics::Scalar lusgsEventPendingRequestFullStalls;
    statistics::Scalar lusgsEventMaxPendingRequests;
    statistics::Scalar lusgsEventCancelledRequests;
    statistics::Scalar lusgsEventLifecycleIdleCycles;
    statistics::Scalar lusgsEventLifecycleQueuedCycles;
    statistics::Scalar lusgsEventLifecycleRunningCycles;
    statistics::Scalar lusgsEventLifecycleCompletedNotReapedCycles;
    statistics::Scalar lusgsEventLifecycleErrorNotReapedCycles;
    statistics::Scalar lusgsEventTokensReaped;
    statistics::Scalar lusgsEventWaitInstructions;
    statistics::Scalar lusgsEventBusyPolls;
    statistics::Scalar lusgsEventSuccessfulWaits;
    statistics::Scalar lusgsEventErrorWaits;
    statistics::Scalar lusgsEventBadTokenWaits;
    statistics::Scalar lusgsEventTokenReapCycles;
    statistics::Scalar lusgsEventLaunchCommitTick;
    statistics::Scalar lusgsEventDescriptorSnapshotTick;
    statistics::Scalar lusgsEventFirstResourceIssueTick;
    statistics::Scalar lusgsEventTaskCompleteTick;
    statistics::Scalar lusgsEventSuccessfulWaitTick;
    statistics::Scalar lusgsEventTokenReapTick;
    statistics::Scalar lusgsEventWatchdogTimeouts;
    statistics::Scalar lusgsEventStateTransitions;
    statistics::Scalar lusgsEventNoProgressCycles;
    statistics::Scalar lusgsEventDoubleScheduleErrors;
    statistics::Scalar lusgsEventNoReadyContextCycles;
    statistics::Scalar lusgsEventPathaRequests;
    statistics::Scalar lusgsEventPathaAccepted;
    statistics::Scalar lusgsEventPathaRetries;
    statistics::Scalar lusgsEventPathaCompleted;
    statistics::Scalar lusgsEventPathaWaitCycles;
    statistics::Scalar lusgsEventPathaSlotFullStalls;
    statistics::Scalar lusgsEventPathaPackStalls;
    statistics::Scalar lusgsEventPathaStoreStalls;
    statistics::Scalar lusgsEventPathaMvmRequests;
    statistics::Scalar lusgsEventPathaMvmAccepted;
    statistics::Scalar lusgsEventPathaMvmRetries;
    statistics::Scalar lusgsEventPathaMvmCompleted;
    statistics::Scalar lusgsEventPathaMvmWaitCycles;
    statistics::Scalar lusgsEventPathaMatLdRequests;
    statistics::Scalar lusgsEventPathaMatLdAccepted;
    statistics::Scalar lusgsEventPathaMatLdRetries;
    statistics::Scalar lusgsEventPathaMatLdCompleted;
    statistics::Scalar lusgsEventPathaMatLdBusyCycles;
    statistics::Scalar lusgsEventPathaMatLdBytes;
    statistics::Scalar lusgsEventPathaDotpRequests;
    statistics::Scalar lusgsEventPathaDotpAccepted;
    statistics::Scalar lusgsEventPathaDotpRetries;
    statistics::Scalar lusgsEventPathaDotpCompleted;
    statistics::Scalar lusgsEventPathaDotpBusyCycles;
    statistics::Scalar lusgsEventPathaDotpPipelineOccupancy;
    statistics::Scalar lusgsEventPathaPackRequests;
    statistics::Scalar lusgsEventPathaPackAccepted;
    statistics::Scalar lusgsEventPathaPackRetries;
    statistics::Scalar lusgsEventPathaPackCompleted;
    statistics::Scalar lusgsEventPathaPackBusyCycles;
    statistics::Scalar lusgsEventPathaSlotAlloc;
    statistics::Scalar lusgsEventPathaSlotFree;
    statistics::Scalar lusgsEventPathaSlotOverwriteErrors;
    statistics::Scalar lusgsEventPathaEarlyConsumeErrors;
    statistics::Scalar lusgsEventPathaResultTransfers;
    statistics::Scalar lusgsEventPathaResultBytes;
    statistics::Scalar lusgsEventPathaLoadPhaseCycles;
    statistics::Scalar lusgsEventPathaDotpPhaseCycles;
    statistics::Scalar lusgsEventPathaPackPhaseCycles;
    statistics::Scalar lusgsEventPathaResultPhaseCycles;
    statistics::Scalar lusgsEventPathaAverageMvmLatency;
    statistics::Scalar lusgsEventPathaMinMvmLatency;
    statistics::Scalar lusgsEventPathaMaxMvmLatency;
    statistics::Scalar lusgsEventTrsvRequests;
    statistics::Scalar lusgsEventTrsvAccepted;
    statistics::Scalar lusgsEventTrsvRetries;
    statistics::Scalar lusgsEventTrsvCompleted;
    statistics::Scalar lusgsEventTrsvWaitCycles;
    statistics::Scalar lusgsEventTrsvBusyCycles;
    statistics::Scalar lusgsEventTrsvQueueFullStalls;
    statistics::Scalar lusgsEventTrsvMaxQueueDepth;
    statistics::Scalar lusgsEventTrsvDivRequests;
    statistics::Scalar lusgsEventTrsvDivCompleted;
    statistics::Scalar lusgsEventTrsvDivBusyCycles;
    statistics::Scalar lusgsEventTrsvFmaRequests;
    statistics::Scalar lusgsEventTrsvFmaCompleted;
    statistics::Scalar lusgsEventTrsvFmaBusyCycles;
    statistics::Scalar lusgsEventTrsvForwardCycles;
    statistics::Scalar lusgsEventTrsvBackwardCycles;
    statistics::Scalar lusgsEventTrsvDependencyWaitCycles;
    statistics::Scalar lusgsEventTrsvForwardedResults;
    statistics::Scalar lusgsEventTrsvAverageLatency;
    statistics::Scalar lusgsEventTrsvMinLatency;
    statistics::Scalar lusgsEventTrsvMaxLatency;
    statistics::Scalar lusgsEventTrsvDividerUtilization;
    statistics::Scalar lusgsEventTrsvFmaUtilization;
    statistics::Scalar lusgsEventVec5Requests;
    statistics::Scalar lusgsEventVec5Accepted;
    statistics::Scalar lusgsEventVec5Retries;
    statistics::Scalar lusgsEventVec5Completed;
    statistics::Scalar lusgsEventVec5WaitCycles;
    statistics::Scalar lusgsEventVec5QueueFullStalls;
    statistics::Scalar lusgsEventVec5BusyCycles;
    statistics::Scalar lusgsEventVec5MaxQueueDepth;
    statistics::Scalar lusgsEventVec5LaneOperations;
    statistics::Scalar lusgsEventVec5AverageLatency;
    statistics::Scalar lusgsEventVec5LaneUtilization;
    statistics::Scalar lusgsEventVec5CopyRequests;
    statistics::Scalar lusgsEventVec5CopyAccepted;
    statistics::Scalar lusgsEventVec5CopyCompleted;
    statistics::Scalar lusgsEventVec5CopyRetries;
    statistics::Scalar lusgsEventVec5CopyWaitCycles;
    statistics::Scalar lusgsEventVec5SubRequests;
    statistics::Scalar lusgsEventVec5SubAccepted;
    statistics::Scalar lusgsEventVec5SubCompleted;
    statistics::Scalar lusgsEventVec5SubRetries;
    statistics::Scalar lusgsEventVec5SubWaitCycles;
    statistics::Scalar lusgsEventVec5AxpyRequests;
    statistics::Scalar lusgsEventVec5AxpyAccepted;
    statistics::Scalar lusgsEventVec5AxpyCompleted;
    statistics::Scalar lusgsEventVec5AxpyRetries;
    statistics::Scalar lusgsEventVec5AxpyWaitCycles;
    statistics::Scalar lusgsEventContextAlloc;
    statistics::Scalar lusgsEventContextFree;
    statistics::Scalar lusgsEventMaxActiveContexts;
    statistics::Scalar lusgsEventContextSwitches;
    statistics::Scalar lusgsEventContextReadyCycles;
    statistics::Scalar lusgsEventContextWaitCycles;
    statistics::Scalar lusgsEventSpmReads;
    statistics::Scalar lusgsEventSpmWrites;
    statistics::Scalar lusgsEventSpmReadBytes;
    statistics::Scalar lusgsEventSpmWriteBytes;
    statistics::Scalar lusgsEventSpmPortConflicts;
    statistics::Scalar lusgsEventSpmBankConflicts;
    statistics::Scalar lusgsEventSpmQueueFull;
    statistics::Scalar lusgsEventSpmRetries;
    statistics::Scalar lusgsEventSpmAverageLatency;
    statistics::Scalar lusgsEventTileLoads;
    statistics::Scalar lusgsEventTileLoadCycles;
    statistics::Scalar lusgsEventTileComputeCycles;
    statistics::Scalar lusgsEventPrefetchUseful;
    statistics::Scalar lusgsEventPrefetchLate;
    statistics::Scalar lusgsEventBufferEmptyStalls;
    statistics::Scalar lusgsEventBufferFullStalls;
    statistics::Scalar lusgsControllerCyclesPerForwardCell;
    statistics::Scalar lusgsControllerCyclesPerBackwardCell;
    statistics::Scalar lusgsControllerCyclesPerFullCell;
    statistics::Scalar lusgsControllerCellsPer1000Cycles;
    statistics::Scalar lusgsControllerPathAUtilization;
    statistics::Scalar lusgsControllerTrsv5Utilization;
    statistics::Scalar lusgsControllerVec5Utilization;
};

CfdLocalSpm *getCfdLocalSpm();
uint64_t cfdDynInstSeqNum(ExecContext *xc);

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_LOCAL_SPM_HH__
