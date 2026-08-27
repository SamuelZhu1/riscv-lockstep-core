// Simulation top: SoC, SFP+ EEPROM model, and the open-drain I2C bus.
module tb_top (
    input  logic        clk,
    input  logic        rst_n,

    output logic        status_valid,
    output logic [31:0] status_data,
    output logic        console_valid,
    output logic [7:0]  console_char,

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
    output logic [15:0] cov,

    output logic        i2c_scl,
    output logic        i2c_sda,
    output logic        fault_nack_used,
    output logic        stuck_active
);
    logic        m_scl_oe, m_sda_oe, s_sda_oe;
    logic        fault_valid;
    logic [31:0] fault_data;
    logic [9:0]  leds;

    // wired-AND bus with pull-ups
    assign i2c_scl = ~m_scl_oe;
    assign i2c_sda = ~(m_sda_oe | s_sda_oe);

    soc u_soc (
        .clk, .rst_n,
        .status_valid, .status_data, .console_valid, .console_char,
        .fault_valid, .fault_data, .leds,
        .scl_oe(m_scl_oe), .sda_oe(m_sda_oe), .scl_in(i2c_scl), .sda_in(i2c_sda),
        .rvfi_valid, .rvfi_order, .rvfi_insn, .rvfi_trap, .rvfi_pc_rdata, .rvfi_pc_wdata,
        .rvfi_rs1_addr, .rvfi_rs2_addr, .rvfi_rs1_rdata, .rvfi_rs2_rdata,
        .rvfi_rd_addr, .rvfi_rd_wdata, .rvfi_mem_addr, .rvfi_mem_rmask, .rvfi_mem_wmask,
        .rvfi_mem_rdata, .rvfi_mem_wdata, .cov
    );

    // FAULT register: [7:0] NACK the next N address bytes, [8] arm a stuck SDA
    i2c_eeprom u_sfp (
        .clk, .rst_n,
        .scl(i2c_scl), .sda(i2c_sda), .sda_oe(s_sda_oe),
        .fault_nack     (fault_valid ? fault_data[7:0] : 8'd0),
        .fault_stuck_arm(fault_valid && fault_data[8]),
        .fault_nack_used,
        .stuck_active
    );
endmodule
