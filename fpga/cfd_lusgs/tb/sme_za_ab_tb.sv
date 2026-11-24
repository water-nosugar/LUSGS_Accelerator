// SPDX-License-Identifier: BSD-3-Clause
`timescale 1ns/1ps

module sme_za_ab_tb;
    parameter SEGMENTS = 3;
    localparam CANDIDATES = 8;
    localparam READ_IDS = 16;
    localparam INDEX_BITS = 3;
    localparam ROW_BITS = 8 * 64;

    reg return_credit, mixed_kind;
    reg [4:0] read_credits;
    reg [2:0] ingress_credits;
    reg [3*64-1:0] nominal_writeback;
    reg [CANDIDATES-1:0] valid, uses_ingress, bypass;
    reg [CANDIDATES*8-1:0] rows, cols;
    reg [CANDIDATES*2-1:0] kind, critical_group;
    reg [CANDIDATES*3-1:0] priority_rank;
    reg [CANDIDATES*7-1:0] active_pes;
    reg [CANDIDATES*READ_IDS-1:0] fresh_reads;
    reg [CANDIDATES*6-1:0] latch_id;
    wire [SEGMENTS-1:0] grant_valid;
    wire [SEGMENTS*INDEX_BITS-1:0] grant_index;

    sme_za_rs_issue #(.SEGMENTS(SEGMENTS)) issue (.*);

    reg clk = 0;
    always #5 clk = !clk;
    reg reset = 1;
    reg reserve_valid = 0;
    wire reserve_ready;
    reg [2:0] reserve_segments = 0, result_valid = 0, retire_ready = 0;
    reg [23:0] reserve_rows = 0, reserve_cols = 0;
    reg [35:0] reserve_tags = 0;
    reg [8*ROW_BITS-1:0] result_data = 0;
    wire [2:0] result_ready, retire_valid;
    wire [23:0] retire_rows, retire_cols;
    wire [35:0] retire_tags;
    wire [8*ROW_BITS-1:0] retire_data;
    sme_za_return_slot buffer_slot (.*);

    function automatic integer popcount(input reg [15:0] mask);
        integer i;
        begin
            popcount = 0;
            for (i = 0; i < 16; i++)
                if (mask[i])
                    popcount++;
        end
    endfunction

    function automatic bit collision(input integer candidate);
        integer row_number, op;
        begin
            collision = 0;
            op = int'(kind[candidate*2 +: 2]);
            if (mixed_kind && !bypass[candidate] && op < 3)
                for (row_number = 0; row_number < 8; row_number++)
                    collision |= rows[candidate*8+row_number] &&
                        (|(cols[candidate*8 +: 8] &
                           nominal_writeback[op*64+row_number*8 +: 8]));
        end
    endfunction

    task automatic check_selector;
        integer slot, cand, choice, prior, ingress_used, op, anchor_kind;
        integer chosen [0:SEGMENTS-1];
        integer unsigned key, best;
        reg [7:0] used_rows, row_mask;
        reg [READ_IDS-1:0] used_reads, read_mask;
        bit fits, shares_latch;
        begin
            #1;
            used_rows = 0;
            used_reads = 0;
            ingress_used = 0;
            anchor_kind = -1;
            for (slot = 0; slot < SEGMENTS; slot++) begin
                choice = -1;
                best = '1;
                for (cand = 0; cand < CANDIDATES; cand++) begin
                    row_mask = rows[cand*8 +: 8];
                    read_mask = fresh_reads[cand*READ_IDS +: READ_IDS];
                    op = int'(kind[cand*2 +: 2]);
                    shares_latch = 0;
                    for (prior = 0; prior < slot; prior++)
                        if (chosen[prior] >= 0)
                            shares_latch |= bypass[cand] && bypass[chosen[prior]] &&
                                latch_id[cand*6 +: 6] == latch_id[chosen[prior]*6 +: 6];
                    fits = return_credit && valid[cand] && op < 3 && row_mask &&
                        cols[cand*8 +: 8] && !(row_mask & used_rows) && !shares_latch &&
                        (slot == 0 || mixed_kind || anchor_kind == op) &&
                        popcount(read_mask | used_reads) <= int'(read_credits) &&
                        ingress_used + int'(uses_ingress[cand]) <= int'(ingress_credits);
                    if (slot == 0 && !mixed_kind)
                        key = int'(priority_rank[cand*3 +: 3]);
                    else if (slot == 0)
                        key = {critical_group[cand*2 +: 2], priority_rank[cand*3 +: 3],
                               collision(cand), ~active_pes[cand*7 +: 7]};
                    else
                        key = {critical_group[cand*2 +: 2], collision(cand),
                               ~active_pes[cand*7 +: 7], priority_rank[cand*3 +: 3]};
                    if (fits && key < best) begin
                        choice = cand;
                        best = key;
                    end
                end
                chosen[slot] = choice;
                if (grant_valid[slot] !== (choice >= 0))
                    $fatal(1, "slot %0d: selection validity differs", slot);
                if (choice >= 0) begin
                    if (grant_index[slot*INDEX_BITS +: INDEX_BITS] !== INDEX_BITS'(choice))
                        $fatal(1, "slot %0d: grant %0d, expected %0d", slot,
                               grant_index[slot*INDEX_BITS +: INDEX_BITS], choice);
                    if (slot == 0)
                        anchor_kind = int'(kind[choice*2 +: 2]);
                    used_rows |= rows[choice*8 +: 8];
                    used_reads |= fresh_reads[choice*READ_IDS +: READ_IDS];
                    ingress_used += int'(uses_ingress[choice]);
                end
            end
        end
    endtask

    task automatic step;
        begin
            @(posedge clk);
            #1;
        end
    endtask

    integer seed = 117;
    integer trial, i, mask;
    initial begin
        seed = $urandom(seed);
        for (trial = 0; trial < 2048; trial++) begin
            return_credit = (trial % 8 != 0);
            mixed_kind = trial[0];
            read_credits = $urandom_range(1, 6);
            ingress_credits = $urandom_range(0, 3);
            nominal_writeback = (192'b1 << $urandom_range(0,191)) |
                                (192'b1 << $urandom_range(0,191));
            valid = $urandom;
            uses_ingress = $urandom;
            bypass = $urandom;
            for (i = 0; i < CANDIDATES; i++) begin
                mask = 1 << $urandom_range(0,7);
                if (i % 2)
                    mask |= 1 << $urandom_range(0,7);
                rows[i*8 +: 8] = mask;
                cols[i*8 +: 8] = $urandom;
                kind[i*2 +: 2] = $urandom_range(0,2);
                critical_group[i*2 +: 2] = $urandom_range(0,2);
                priority_rank[i*3 +: 3] = $urandom_range(0,7);
                active_pes[i*7 +: 7] = popcount({8'b0, rows[i*8 +: 8]}) *
                                       popcount({8'b0, cols[i*8 +: 8]});
                fresh_reads[i*READ_IDS +: READ_IDS] =
                    (16'b1 << $urandom_range(0,15)) | (16'b1 << $urandom_range(0,15));
                latch_id[i*6 +: 6] = $urandom_range(0,2);
            end
            check_selector();
        end

        @(negedge clk);
        reset = 0;
        reserve_valid = 1;
        reserve_segments = 3'b111;
        reserve_rows = {8'h04, 8'h20, 8'h01};
        reserve_cols = {8'h07, 8'hf0, 8'hff};
        reserve_tags = {12'h412, 12'h403, 12'h234};
        step();
        reserve_valid = 0;
        result_valid = 3'b010;
        result_data[5*ROW_BITS +: ROW_BITS] = {8{64'h12345678abcdef00}};
        #1;
        if (retire_valid !== 3'b010 || reserve_ready ||
            retire_data[5*ROW_BITS +: ROW_BITS] !== {8{64'h12345678abcdef00}})
            $fatal(1, "fresh data or reservation state");
        step();
        result_valid = 3'b101;
        result_data[5*ROW_BITS +: ROW_BITS] = 0;
        result_data[0 +: ROW_BITS] = {8{64'h1020304050607080}};
        result_data[2*ROW_BITS +: ROW_BITS] = {8{64'h0f0e0d0c0b0a0908}};
        retire_ready = 3'b100;
        #1;
        if (retire_valid !== 3'b111 || retire_data[5*ROW_BITS +: ROW_BITS] !==
            {8{64'h12345678abcdef00}} || retire_data[2*ROW_BITS +: ROW_BITS] !==
            {8{64'h0f0e0d0c0b0a0908}})
            $fatal(1, "independent fresh and stalled results");
        step();
        result_valid = 0;
        result_data = 0;
        retire_ready = 3'b010;
        #1;
        if (retire_valid !== 3'b011 || reserve_ready ||
            retire_data[0 +: ROW_BITS] !== {8{64'h1020304050607080}} ||
            retire_tags !== reserve_tags || retire_cols !== reserve_cols)
            $fatal(1, "buffered result or metadata changed");
        step();
        retire_ready = 3'b001;
        reserve_valid = 1;
        reserve_segments = 3'b001;
        reserve_tags = {24'b0, 12'h013};
        #1;
        if (!reserve_ready || result_ready != 0)
            $fatal(1, "completed slot could not be reused");
        step();
        reserve_valid = 0;
        if (retire_valid !== 0 || result_ready !== 3'b001 ||
            retire_tags !== reserve_tags)
            $fatal(1, "reused slot has stale state");
        reset = 1;
        step();
        reset = 0;
        #1;
        if (!reserve_ready || retire_valid != 0 || result_ready != 0)
            $fatal(1, "reset left a live completion");
        $display("PASS %0d selector cases (S=%0d) and elastic 8x8 return", 2048, SEGMENTS);
        $finish;
    end
endmodule
