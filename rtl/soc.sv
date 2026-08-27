// Core + 64 KB unified RAM + memory-mapped devices.
//
// Memory map
//   0x0000_0000 - 0x0000_FFFF  RAM (aliases up to 0x0001_FFFF)
//   0x0002_0000                TEST_STATUS  (write 1 = pass, (n<<1)|1 = fail test n)
//   0x0002_0004                LEDS         (write, low 10 bits drive the board LEDs)
//   0x0002_0008                CYCLE        (read, free-running cycle counter)
//   0x0002_000C                CONSOLE      (write a character)
//   0x0002_0010                FAULT        (write, testbench-only I2C fault injection)
//   0x0002_0100 - 0x0002_01FF  I2C controller (see i2c_master.sv)
//
// RAM and MMIO reads are both synchronous: address in at the edge, data
// out the next cycle, matching the core's MEM stage timing.

module soc #(
    parameter string INIT_FILE = ""
) (
    input  logic        clk,
    input  logic        rst_n,

    output logic        status_valid,
    output logic [31:0] status_data,
    output logic        console_valid,
    output logic [7:0]  console_char,
    output logic        fault_valid,
    output logic [31:0] fault_data,
    output logic [9:0]  leds,

    // I2C, open drain: *_oe = 1 pulls the line low
    output logic        scl_oe,
    output logic        sda_oe,
    input  logic        scl_in,
    input  logic        sda_in,

    // trace and coverage, passed through for the testbench
    output logic        rvfi_valid,
    output logic [63:0] rvfi_order,
    output logic [31:0] rvfi_insn,
    output logic        rvfi_trap,
    output logic [31:0] rvfi_pc_rdata,
    output logic [31:0] rvfi_pc_wdata,
    output logic [4:0]  rvfi_rs1_addr,
    output logic [4:0]  rvfi_rs2_addr,
    output logic [31:0] rvfi_rs1_rdata,
    output logic [31:0] rvfi_rs2_rdata,
    output logic [4:0]  rvfi_rd_addr,
    output logic [31:0] rvfi_rd_wdata,
    output logic [31:0] rvfi_mem_addr,
    output logic [3:0]  rvfi_mem_rmask,
    output logic [3:0]  rvfi_mem_wmask,
    output logic [31:0] rvfi_mem_rdata,
    output logic [31:0] rvfi_mem_wdata,
    output logic [15:0] cov
);

    logic [31:0] imem_addr, imem_rdata;
    logic [31:0] dmem_raddr, dmem_rdata, dmem_waddr, dmem_wdata;
    logic        dmem_re;
    logic [3:0]  dmem_wmask;

    core u_core (
        .clk, .rst_n,
        .imem_addr, .imem_rdata,
        .dmem_raddr, .dmem_re, .dmem_rdata, .dmem_waddr, .dmem_wdata, .dmem_wmask,
        .rvfi_valid, .rvfi_order, .rvfi_insn, .rvfi_trap, .rvfi_pc_rdata, .rvfi_pc_wdata,
        .rvfi_rs1_addr, .rvfi_rs2_addr, .rvfi_rs1_rdata, .rvfi_rs2_rdata,
        .rvfi_rd_addr, .rvfi_rd_wdata, .rvfi_mem_addr, .rvfi_mem_rmask, .rvfi_mem_wmask,
        .rvfi_mem_rdata, .rvfi_mem_wdata,
        .cov
    );

    // ------------------------------------------------------------------
    // RAM
    // ------------------------------------------------------------------
    logic        ram_we;
    logic [31:0] ram_rdata;
    assign ram_we = (dmem_wmask != 4'b0) && (dmem_waddr[31:17] == 15'd0);

    ram_dp #(.WORDS(16384), .INIT_FILE(INIT_FILE)) u_ram (
        .clk,
        .a_addr (imem_addr[15:2]),
        .a_rdata(imem_rdata),
        .b_raddr(dmem_raddr[15:2]),
        .b_rdata(ram_rdata),
        .b_waddr(dmem_waddr[15:2]),
        .b_wdata(dmem_wdata),
        .b_wmask(ram_we ? dmem_wmask : 4'b0)
    );

    // ------------------------------------------------------------------
    // MMIO
    // ------------------------------------------------------------------
    logic        mmio_we;
    assign mmio_we = (dmem_wmask != 4'b0) && (dmem_waddr[31:17] != 15'd0);

    logic [31:0] cycle;
    always_ff @(posedge clk) begin
        if (!rst_n) cycle <= 32'd0;
        else        cycle <= cycle + 32'd1;
    end

    always_ff @(posedge clk) begin
        if (!rst_n) leds <= 10'd0;
        else if (mmio_we && dmem_waddr[15:0] == 16'h0004) leds <= dmem_wdata[9:0];
    end

    assign status_valid  = mmio_we && dmem_waddr[15:0] == 16'h0000;
    assign status_data   = dmem_wdata;
    assign console_valid = mmio_we && dmem_waddr[15:0] == 16'h000C;
    assign console_char  = dmem_wdata[7:0];
    assign fault_valid   = mmio_we && dmem_waddr[15:0] == 16'h0010;
    assign fault_data    = dmem_wdata;

    // I2C controller
    logic        i2c_sel_w, i2c_sel_r;
    logic [31:0] i2c_rdata;
    assign i2c_sel_w = mmio_we && dmem_waddr[15:8] == 8'h01;

    i2c_master u_i2c (
        .clk, .rst_n,
        .we    (i2c_sel_w),
        .waddr (dmem_waddr[7:2]),
        .wdata (dmem_wdata),
        .raddr (dmem_raddr[7:2]),
        .rdata (i2c_rdata),
        .scl_oe, .sda_oe, .scl_in, .sda_in
    );

    // registered read mux (same latency as the RAM)
    logic        rd_is_mmio;
    logic [31:0] mmio_rdata_q;
    always_ff @(posedge clk) begin
        rd_is_mmio <= dmem_raddr[31:17] != 15'd0;
        case (dmem_raddr[15:8])
            8'h00:   mmio_rdata_q <= (dmem_raddr[7:0] == 8'h08) ? cycle :
                                     (dmem_raddr[7:0] == 8'h04) ? {22'h0, leds} : 32'h0;
            8'h01:   mmio_rdata_q <= i2c_rdata;
            default: mmio_rdata_q <= 32'h0;
        endcase
    end
    assign dmem_rdata = rd_is_mmio ? mmio_rdata_q : ram_rdata;

endmodule
