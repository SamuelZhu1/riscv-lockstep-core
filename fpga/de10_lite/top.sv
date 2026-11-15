// DE10-Lite top: the SoC at the board's 50 MHz clock, KEY0 as reset, the
// LEDs on LEDR[8:0], LEDR9 lit when a program reports pass, and the I2C
// bus on GPIO[0] (SDA) and GPIO[1] (SCL) as open drain with external
// 4.7k pull-ups to 3.3 V.
//
// Check Fmax in the TimeQuest report after the first compile. If the slow
// 85C corner misses 50 MHz, add an ALTPLL at 25 MHz and feed its output
// to clk below (and PRESCALE=63 for 100 kHz I2C).
module top (
    input  logic       MAX10_CLK1_50,
    input  logic [1:0] KEY,
    output logic [9:0] LEDR,
    inout  wire  [1:0] GPIO
);
    logic clk;
    assign clk = MAX10_CLK1_50;

    // reset: KEY0 is active low; synchronize the release
    logic [2:0] rst_sync;
    always_ff @(posedge clk) rst_sync <= {rst_sync[1:0], KEY[0]};
    logic rst_n;
    assign rst_n = rst_sync[2];

    logic        status_valid, console_valid, fault_valid;
    logic [31:0] status_data, fault_data;
    logic [7:0]  console_char;
    logic [9:0]  leds;
    logic        scl_oe, sda_oe;

    soc #(.INIT_FILE("program.hex")) u_soc (
        .clk, .rst_n,
        .status_valid, .status_data, .console_valid, .console_char,
        .fault_valid, .fault_data, .leds,
        .scl_oe, .sda_oe, .scl_in(GPIO[1]), .sda_in(GPIO[0]),
        .rvfi_valid(), .rvfi_order(), .rvfi_insn(), .rvfi_trap(), .rvfi_pc_rdata(), .rvfi_pc_wdata(),
        .rvfi_rs1_addr(), .rvfi_rs2_addr(), .rvfi_rs1_rdata(), .rvfi_rs2_rdata(),
        .rvfi_rd_addr(), .rvfi_rd_wdata(), .rvfi_mem_addr(), .rvfi_mem_rmask(), .rvfi_mem_wmask(),
        .rvfi_mem_rdata(), .rvfi_mem_wdata(), .cov()
    );

    // open drain: drive 0 or float
    assign GPIO[0] = sda_oe ? 1'b0 : 1'bz;
    assign GPIO[1] = scl_oe ? 1'b0 : 1'bz;

    // latch the result of TEST_STATUS so it stays visible
    logic passed, failed;
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            passed <= 1'b0;
            failed <= 1'b0;
        end else if (status_valid) begin
            passed <= status_data == 32'd1;
            failed <= status_data != 32'd1;
        end
    end
    assign LEDR = {passed, failed ? 9'h1FF : leds[8:0]};
endmodule
