// SPDX-License-Identifier: BSD-3-Clause
// One 5x5 FP64 window inside ZA. The caller supplies the array's shared
// write grants, element scoreboard and external row-bank reservations.
module sme_za_slice_bank #(
    parameter ENABLE_VERTICAL = 1
) (
    input  wire          clk,
    input  wire          valid,
    output wire          ready,
    input  wire          vertical,
    input  wire [2:0]    slice,
    input  wire [4:0]    mask,
    input  wire [319:0]  write_data,
    input  wire          write_credit,
    input  wire          vertical_credit,
    input  wire [4:0]    bank_taken,
    input  wire [24:0]   element_busy,
    output wire [4:0]    bank_grant,
    input  wire [2:0]    read_row,
    output wire [319:0]  read_data
);
    wire [24:0] selected;
    wire [4:0] banks;
    reg [319:0] rows [0:4];

    assign ready = (slice < 5) && (|mask) && write_credit &&
                   (!vertical || (ENABLE_VERTICAL && vertical_credit)) &&
                   !(|(selected & element_busy)) && !(|(banks & bank_taken));
    assign bank_grant = (valid && ready) ? banks : 5'b0;
    assign read_data = (read_row < 5) ? rows[read_row] : 320'b0;

    // An accepted slice is visible on the next clock. The surrounding ZA
    // scoreboard owns validity; the data flops do not require a reset.
    genvar r, c;
    generate
        for (r = 0; r < 5; r = r + 1) begin : row_bank
            assign banks[r] = |selected[r*5 +: 5];
            for (c = 0; c < 5; c = c + 1) begin : element
                assign selected[r*5+c] = vertical ?
                    (ENABLE_VERTICAL && slice == c && mask[r]) :
                    (slice == r && mask[c]);
                wire [63:0] word = (ENABLE_VERTICAL && vertical) ?
                    write_data[r*64 +: 64] : write_data[c*64 +: 64];
                always @(posedge clk)
                    if (bank_grant[r] && selected[r*5+c])
                        rows[r][c*64 +: 64] <= word;
            end
        end
    endgenerate
endmodule
