// Dual-port RAM: port A reads instructions, port B reads and writes data.
// Both reads are synchronous and return the old word on a same-cycle
// write, which is how MAX 10 M9K blocks behave in mixed-port mode.
//
// In simulation (SIM_DPI_RAM) the storage lives in the C++ testbench so it
// can load a new program between runs without recompiling.

module ram_dp #(
    parameter int    WORDS     = 16384,
    parameter string INIT_FILE = ""
) (
    input  logic                     clk,
    input  logic [$clog2(WORDS)-1:0] a_addr,
    output logic [31:0]              a_rdata,
    input  logic [$clog2(WORDS)-1:0] b_raddr,
    output logic [31:0]              b_rdata,
    input  logic [$clog2(WORDS)-1:0] b_waddr,
    input  logic [31:0]              b_wdata,
    input  logic [3:0]               b_wmask
);

`ifdef SIM_DPI_RAM
    import "DPI-C" function int unsigned ram_read(input int unsigned word_addr);
    import "DPI-C" function void ram_write(input int unsigned word_addr, input int unsigned data,
                                           input int unsigned mask);

    // one block so both reads happen before the write: old-data semantics
    always_ff @(posedge clk) begin
        a_rdata <= ram_read({18'h0, a_addr});
        b_rdata <= ram_read({18'h0, b_raddr});
        if (b_wmask != 4'b0) ram_write({18'h0, b_waddr}, b_wdata, {28'h0, b_wmask});
    end
`else
    logic [31:0] mem [0:WORDS-1];
    initial if (INIT_FILE != "") $readmemh(INIT_FILE, mem);

    always_ff @(posedge clk) a_rdata <= mem[a_addr];

    always_ff @(posedge clk) begin
        b_rdata <= mem[b_raddr];
        if (b_wmask[0]) mem[b_waddr][7:0]   <= b_wdata[7:0];
        if (b_wmask[1]) mem[b_waddr][15:8]  <= b_wdata[15:8];
        if (b_wmask[2]) mem[b_waddr][23:16] <= b_wdata[23:16];
        if (b_wmask[3]) mem[b_waddr][31:24] <= b_wdata[31:24];
    end
`endif

endmodule
