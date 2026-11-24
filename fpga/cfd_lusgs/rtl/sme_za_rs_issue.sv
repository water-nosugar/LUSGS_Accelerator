// SPDX-License-Identifier: BSD-3-Clause
// One RS bundle from a fixed window of scoreboard-ready candidates.
// A fresh read set uses window-local slice IDs, shared by equal ZA slices.
module sme_za_rs_issue #(
    parameter CANDIDATES = 8,
    parameter SEGMENTS = 3,
    parameter READ_IDS = 16,
    parameter LATCH_BITS = 6
) (
    input  wire                         return_credit,
    input  wire                         mixed_kind,
    input  wire [$clog2(READ_IDS+1)-1:0] read_credits,
    input  wire [2:0]                   ingress_credits,
    input  wire [3*64-1:0]              nominal_writeback,
    input  wire [CANDIDATES-1:0]        valid,
    input  wire [CANDIDATES*8-1:0]      rows,
    input  wire [CANDIDATES*8-1:0]      cols,
    input  wire [CANDIDATES*2-1:0]      kind,
    input  wire [CANDIDATES*2-1:0]      critical_group,
    input  wire [CANDIDATES*3-1:0]      priority_rank,
    input  wire [CANDIDATES*7-1:0]      active_pes,
    input  wire [CANDIDATES*READ_IDS-1:0] fresh_reads,
    input  wire [CANDIDATES-1:0]        uses_ingress,
    input  wire [CANDIDATES-1:0]        bypass,
    input  wire [CANDIDATES*LATCH_BITS-1:0] latch_id,
    output wire [SEGMENTS-1:0]          grant_valid,
    output wire [SEGMENTS*$clog2(CANDIDATES)-1:0] grant_index
);
    localparam INDEX_BITS = $clog2(CANDIDATES);
    localparam LEAVES = 1 << INDEX_BITS;
    localparam KEY_BITS = 14 + INDEX_BITS;
    localparam READ_COUNT_BITS = $clog2(READ_IDS+1);
    localparam READ_LEAVES = 1 << $clog2(READ_IDS);

    function automatic [READ_COUNT_BITS-1:0] read_count;
        input [READ_IDS-1:0] slices;
        reg [READ_COUNT_BITS-1:0] counts [1:2*READ_LEAVES-1];
        integer i;
        begin
            for (i = 0; i < READ_LEAVES; i = i + 1)
                counts[READ_LEAVES+i] = i < READ_IDS ? slices[i] : 0;
            for (i = READ_LEAVES-1; i > 0; i = i - 1)
                counts[i] = counts[2*i] + counts[2*i+1];
            read_count = counts[1];
        end
    endfunction

    wire [CANDIDATES-1:0] writeback_conflict;
    wire [7:0] used_rows [0:SEGMENTS];
    wire [READ_IDS-1:0] used_reads [0:SEGMENTS];
    wire [3:0] used_ingress [0:SEGMENTS];
    wire [INDEX_BITS-1:0] picked [0:SEGMENTS-1];
    wire [1:0] picked_kind [0:SEGMENTS-1];
    wire picked_bypass [0:SEGMENTS-1];
    wire [LATCH_BITS-1:0] picked_latch [0:SEGMENTS-1];
    assign used_rows[0] = 0;
    assign used_reads[0] = 0;
    assign used_ingress[0] = 0;

    genvar candidate, row, slot, leaf, node, prior;
    generate
        for (candidate = 0; candidate < CANDIDATES; candidate = candidate + 1) begin : hazard
            wire [1:0] op = kind[candidate*2 +: 2];
            wire [63:0] due = op == 0 ? nominal_writeback[0 +: 64] :
                op == 1 ? nominal_writeback[64 +: 64] : nominal_writeback[128 +: 64];
            wire [7:0] intersects;
            for (row = 0; row < 8; row = row + 1) begin : pe_row
                assign intersects[row] = rows[candidate*8+row] &&
                    (|(cols[candidate*8 +: 8] & due[row*8 +: 8]));
            end
            assign writeback_conflict[candidate] =
                mixed_kind && !bypass[candidate] && (|intersects);
        end

        for (slot = 0; slot < SEGMENTS; slot = slot + 1) begin : select_segment
            wire [KEY_BITS-1:0] rank_tree [1:2*LEAVES-1];
            for (leaf = 0; leaf < LEAVES; leaf = leaf + 1) begin : rank_candidate
                if (leaf < CANDIDATES) begin : present
                    localparam [INDEX_BITS-1:0] LEAF_INDEX = leaf;
                    wire [7:0] row_mask = rows[leaf*8 +: 8];
                    wire [1:0] op = kind[leaf*2 +: 2];
                    wire [2:0] rank = priority_rank[leaf*3 +: 3];
                    wire [1:0] critical = critical_group[leaf*2 +: 2];
                    wire [6:0] inverse_pes = ~active_pes[leaf*7 +: 7];
                    wire [READ_IDS-1:0] reads = fresh_reads[leaf*READ_IDS +: READ_IDS];
                    wire [SEGMENTS-1:0] duplicate_latch;
                    wire same_kind;
                    for (prior = 0; prior < SEGMENTS; prior = prior + 1) begin : compare_latch
                        if (prior < slot) begin : earlier
                            assign duplicate_latch[prior] = grant_valid[prior] &&
                                bypass[leaf] && picked_bypass[prior] &&
                                latch_id[leaf*LATCH_BITS +: LATCH_BITS] ==
                                picked_latch[prior];
                        end else begin : later
                            assign duplicate_latch[prior] = 1'b0;
                        end
                    end
                    if (slot == 0) begin : anchor
                        assign same_kind = 1'b1;
                    end else begin : filler
                        assign same_kind = mixed_kind || op == picked_kind[0];
                    end
                    wire eligible = return_credit && valid[leaf] && op != 3 &&
                        (|row_mask) && (|cols[leaf*8 +: 8]) &&
                        same_kind && !(|(row_mask & used_rows[slot])) &&
                        !(|duplicate_latch) &&
                        read_count(reads | used_reads[slot]) <= read_credits &&
                        used_ingress[slot] + {3'b0, uses_ingress[leaf]} <= ingress_credits;
                    if (slot == 0) begin : anchor_rank
                        assign rank_tree[LEAVES+leaf] = mixed_kind ?
                            {!eligible, critical, rank, writeback_conflict[leaf],
                             inverse_pes, LEAF_INDEX} :
                            {!eligible, 10'b0, rank, LEAF_INDEX};
                    end else begin : filler_rank
                        assign rank_tree[LEAVES+leaf] =
                            {!eligible, critical, writeback_conflict[leaf],
                             inverse_pes, rank, LEAF_INDEX};
                    end
                end else begin : unused_leaf
                    assign rank_tree[LEAVES+leaf] = {KEY_BITS{1'b1}};
                end
            end
            // Compare pairs in parallel. Only a winning index is routed
            // to the next slot's read set and row mask.
            for (node = 1; node < LEAVES; node = node + 1) begin : reduce_rank
                assign rank_tree[node] = rank_tree[2*node] < rank_tree[2*node+1] ?
                    rank_tree[2*node] : rank_tree[2*node+1];
            end
            assign grant_valid[slot] = !rank_tree[1][KEY_BITS-1];
            assign picked[slot] = grant_valid[slot] ? rank_tree[1][INDEX_BITS-1:0] : 0;
            assign grant_index[slot*INDEX_BITS +: INDEX_BITS] = picked[slot];

            reg [7:0] selected_rows;
            reg [READ_IDS-1:0] selected_reads;
            reg [LATCH_BITS-1:0] selected_latch;
            reg [1:0] selected_kind;
            reg selected_ingress, selected_bypass;
            integer source;
            always @* begin
                selected_rows = 0;
                selected_reads = 0;
                selected_latch = 0;
                selected_kind = 0;
                selected_ingress = 0;
                selected_bypass = 0;
                for (source = 0; source < CANDIDATES; source = source + 1) begin
                    if (grant_valid[slot] && picked[slot] == source) begin
                        selected_rows = selected_rows | rows[source*8 +: 8];
                        selected_reads = selected_reads |
                            fresh_reads[source*READ_IDS +: READ_IDS];
                        selected_latch = selected_latch |
                            latch_id[source*LATCH_BITS +: LATCH_BITS];
                        selected_kind = selected_kind | kind[source*2 +: 2];
                        selected_ingress = selected_ingress | uses_ingress[source];
                        selected_bypass = selected_bypass | bypass[source];
                    end
                end
            end
            assign picked_kind[slot] = selected_kind;
            assign picked_bypass[slot] = selected_bypass;
            assign picked_latch[slot] = selected_latch;
            assign used_rows[slot+1] = used_rows[slot] | selected_rows;
            assign used_reads[slot+1] = used_reads[slot] | selected_reads;
            assign used_ingress[slot+1] = used_ingress[slot] + {3'b0, selected_ingress};
        end
    endgenerate
endmodule
