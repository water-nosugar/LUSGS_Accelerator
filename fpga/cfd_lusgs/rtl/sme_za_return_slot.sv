// SPDX-License-Identifier: BSD-3-Clause
// One reserved 8x8 result slot. The caller owns slot allocation and routes
// each pipeline completion here. Segments in a slot occupy disjoint PE rows.
// The caller grants retirement after ZA/forwarding write-port arbitration.
module sme_za_return_slot #(
    parameter SEGMENTS = 3,
    parameter WORD_BITS = 64,
    parameter TAG_BITS = 12
) (
    input  wire                      clk,
    input  wire                      reset,
    input  wire                      reserve_valid,
    output wire                      reserve_ready,
    input  wire [SEGMENTS-1:0]       reserve_segments,
    input  wire [SEGMENTS*8-1:0]     reserve_rows,
    input  wire [SEGMENTS*8-1:0]     reserve_cols,
    input  wire [SEGMENTS*TAG_BITS-1:0] reserve_tags,
    input  wire [SEGMENTS-1:0]       result_valid,
    output wire [SEGMENTS-1:0]       result_ready,
    input  wire [8*8*WORD_BITS-1:0]  result_data,
    output wire [SEGMENTS-1:0]       retire_valid,
    input  wire [SEGMENTS-1:0]       retire_ready,
    output reg  [SEGMENTS*8-1:0]     retire_rows,
    output reg  [SEGMENTS*8-1:0]     retire_cols,
    output reg  [SEGMENTS*TAG_BITS-1:0] retire_tags,
    output wire [8*8*WORD_BITS-1:0]  retire_data
);
    localparam ROW_BITS = 8 * WORD_BITS;
    reg [SEGMENTS-1:0] active, buffered;
    reg [ROW_BITS-1:0] payload [0:7];
    wire [SEGMENTS-1:0] arriving = result_valid & result_ready;
    wire [SEGMENTS-1:0] retiring = retire_valid & retire_ready;

    assign result_ready = reset ? 0 : active & ~buffered;
    assign retire_valid = reset ? 0 : buffered | arriving;
    // A drained slot can be reserved again on the same edge. Reservations
    // describe operations whose first result will arrive on a later edge.
    assign reserve_ready = !reset && ((active & ~retiring) == 0);

    always @(posedge clk) begin
        if (reset) begin
            active <= 0;
            buffered <= 0;
        end else if (reserve_valid && reserve_ready) begin
            active <= reserve_segments;
            buffered <= 0;
            retire_rows <= reserve_rows;
            retire_cols <= reserve_cols;
            retire_tags <= reserve_tags;
        end else begin
            active <= active & ~retiring;
            buffered <= (buffered | arriving) & ~retiring;
        end
    end

    genvar r, s;
    generate
        for (r = 0; r < 8; r = r + 1) begin : bank
            wire [SEGMENTS-1:0] incoming, hold_result;
            for (s = 0; s < SEGMENTS; s = s + 1) begin : segment
                assign incoming[s] = arriving[s] && retire_rows[s*8+r];
                assign hold_result[s] = incoming[s] && !retire_ready[s];
            end
            // Forward a fresh result to the write port in its due cycle.
            // Hold the whole row across stalls until its segment retires.
            assign retire_data[r*ROW_BITS +: ROW_BITS] = (|incoming) ?
                result_data[r*ROW_BITS +: ROW_BITS] : payload[r];
            always @(posedge clk)
                if (|hold_result)
                    payload[r] <= result_data[r*ROW_BITS +: ROW_BITS];
        end
    endgenerate
endmodule
