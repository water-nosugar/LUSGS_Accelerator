// SPDX-License-Identifier: BSD-3-Clause
`timescale 1ns/1ps
module sme_za_slice_bank_tb;
    parameter ENABLE_VERTICAL = 1;
    reg clk = 0;
    always #5 clk = !clk;

    reg valid, vertical, write_credit, vertical_credit;
    reg [2:0] slice, read_row;
    reg [4:0] mask, bank_taken;
    reg [24:0] element_busy;
    reg [319:0] write_data;
    wire ready;
    wire [4:0] bank_grant;
    wire [319:0] read_data;
    sme_za_slice_bank #(.ENABLE_VERTICAL(ENABLE_VERTICAL)) dut (.*);

    reg [319:0] expected [0:4];
    integer operations = 0;
    integer mode, s, m, n;
    task check_write;
        integer i, r, c;
        reg allow;
        reg [4:0] touched;
        begin
            @(negedge clk);
            for (i = 0; i < 5; i = i + 1)
                write_data[64*i +: 64] = {32'hc0ffee00 + i, 32'h1000 + operations};
            allow = (slice < 5) && (mask != 0) && write_credit &&
                    (!vertical || (ENABLE_VERTICAL && vertical_credit));
            touched = 0;
            for (i = 0; i < 5; i = i + 1)
                if (slice < 5 && mask[i]) begin
                    r = vertical ? i : slice;
                    c = vertical ? slice : i;
                    touched[r] = 1;
                    if (bank_taken[r] || element_busy[r*5+c])
                        allow = 0;
                end
            #1;
            if (ready !== allow || bank_grant !== ((allow && valid) ? touched : 5'b0))
                $fatal(1, "grant mismatch at transaction %0d", operations);
            if (valid && allow)
                for (i = 0; i < 5; i = i + 1)
                    if (mask[i]) begin
                        r = vertical ? i : slice;
                        c = vertical ? slice : i;
                        expected[r][64*c +: 64] = write_data[64*i +: 64];
                    end
            @(posedge clk);
            // Inspect all rows after the write edge, before the next edge.
            for (r = 0; r < 5; r = r + 1) begin
                read_row = r;
                #0.2;
                if (read_data !== expected[r])
                    $fatal(1, "data mismatch at transaction %0d row %0d", operations, r);
            end
            operations = operations + 1;
        end
    endtask

    initial begin
        valid = 1; vertical = 0; write_credit = 1; vertical_credit = 1;
        slice = 7; mask = 0; bank_taken = 0; element_busy = 0;
        read_row = 0; write_data = 0;
        for (mode = 0; mode <= 1; mode = mode + 1)
            for (s = 0; s < 6; s = s + 1)
                for (m = 0; m < 32; m = m + 1)
                    for (n = 0; n < 6; n = n + 1) begin
                        vertical = mode; slice = s; mask = m;
                        valid = n != 3; write_credit = n != 4; vertical_credit = n != 5;
                        bank_taken = n == 1 ? (5'b1 << ((s + m) % 5)) : 0;
                        element_busy = n == 2 ? (25'b1 << ((s + m) % 25)) : 0;
                        check_write;
                    end
        $display("SME_ZA_SLICE_BANK_PASS vertical=%0d transactions=%0d", ENABLE_VERTICAL, operations);
        $finish;
    end
endmodule
