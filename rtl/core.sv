// 5-stage pipelined RV32I core (IF ID EX MEM WB), machine mode only.
//
// Memories are synchronous (address registered at the clock edge, data back
// the next cycle) so the same RTL maps onto MAX 10 M9K blocks:
//   - fetch presents pc_next, so the instruction for pc arrives in IF
//   - loads present their address from EX, so the data arrives in MEM
//   - stores write at the end of MEM
// Hazards: full forwarding (EX/MEM -> EX, MEM/WB -> EX, WB -> ID), a one
// cycle load-use stall, branches and jumps resolved in EX with a two
// instruction flush, and a store-to-load bypass for a load that reads the
// word the store in MEM is writing in the same cycle.
// Exceptions (illegal instruction, ecall, ebreak, misaligned fetch target,
// misaligned load/store) are taken in EX, so everything older has already
// left EX and everything younger is flushed: traps are precise.
// Every retired instruction is reported on an RVFI-style port from WB.

module core (
    input  logic        clk,
    input  logic        rst_n,

    // instruction port (synchronous read)
    output logic [31:0] imem_addr,
    input  logic [31:0] imem_rdata,

    // data port: read address from EX (synchronous read), write from MEM
    output logic [31:0] dmem_raddr,
    output logic        dmem_re,
    input  logic [31:0] dmem_rdata,
    output logic [31:0] dmem_waddr,
    output logic [31:0] dmem_wdata,
    output logic [3:0]  dmem_wmask,

    // retirement trace (RVFI subset)
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

    // one bit per hazard event this cycle, for functional coverage
    output logic [15:0] cov
);

    // ------------------------------------------------------------------
    // opcodes and constants
    // ------------------------------------------------------------------
    localparam logic [6:0] OP_LUI = 7'h37, OP_AUIPC = 7'h17, OP_JAL = 7'h6F, OP_JALR = 7'h67,
                           OP_BRANCH = 7'h63, OP_LOAD = 7'h03, OP_STORE = 7'h23, OP_IMM = 7'h13,
                           OP_REG = 7'h33, OP_FENCE = 7'h0F, OP_SYSTEM = 7'h73;

    localparam logic [3:0] ALU_ADD = 4'd0, ALU_SUB = 4'd1, ALU_SLL = 4'd2, ALU_SLT = 4'd3,
                           ALU_SLTU = 4'd4, ALU_XOR = 4'd5, ALU_SRL = 4'd6, ALU_SRA = 4'd7,
                           ALU_OR = 4'd8, ALU_AND = 4'd9, ALU_PASSB = 4'd10;

    localparam logic [31:0] CAUSE_MISALIGNED_FETCH = 32'd0, CAUSE_ILLEGAL = 32'd2,
                            CAUSE_BREAKPOINT = 32'd3, CAUSE_MISALIGNED_LOAD = 32'd4,
                            CAUSE_MISALIGNED_STORE = 32'd6, CAUSE_ECALL = 32'd11;

    // ------------------------------------------------------------------
    // pipeline control
    // ------------------------------------------------------------------
    logic        stall;        // load-use: hold IF and ID, bubble into EX
    logic        redirect;     // taken branch / jump / trap / mret / fence.i in EX
    logic [31:0] redirect_pc;

    // ------------------------------------------------------------------
    // IF
    // ------------------------------------------------------------------
    logic [31:0] pc, pc_next;
    logic        if_valid;

    always_comb begin
        if (!rst_n)        pc_next = 32'h0;
        else if (redirect) pc_next = redirect_pc;
        else if (stall)    pc_next = pc;
        else               pc_next = pc + 32'd4;
    end
    assign imem_addr = pc_next;

    // The RAM latched address 0 during reset, so the first instruction is
    // already on imem_rdata in the first cycle out of reset.
    always_ff @(posedge clk) begin
        if (!rst_n) pc <= 32'h0;
        else        pc <= pc_next;
    end
    assign if_valid = rst_n;

    // ------------------------------------------------------------------
    // IF/ID
    // ------------------------------------------------------------------
    logic        id_valid;
    logic [31:0] id_pc, id_insn;

    always_ff @(posedge clk) begin
        if (!rst_n || redirect) begin
            id_valid <= 1'b0;
            id_pc    <= 32'h0;
            id_insn  <= 32'h0000_0013;
        end else if (!stall) begin
            id_valid <= if_valid;
            id_pc    <= pc;
            id_insn  <= imem_rdata;
        end
    end

    // ------------------------------------------------------------------
    // ID: decode
    // ------------------------------------------------------------------
    logic [6:0]  d_op;
    logic [4:0]  d_rd, d_rs1, d_rs2;
    logic [2:0]  d_f3;
    logic [6:0]  d_f7;
    logic [31:0] d_imm;
    logic        d_use1, d_use2, d_wr, d_load, d_store, d_branch, d_jal, d_jalr;
    logic        d_csr, d_ecall, d_ebreak, d_mret, d_fencei, d_illegal;
    logic        d_a_pc, d_b_imm;   // ALU operand selects
    logic [3:0]  d_alu;

    assign d_op  = id_insn[6:0];
    assign d_rd  = id_insn[11:7];
    assign d_f3  = id_insn[14:12];
    assign d_rs1 = id_insn[19:15];
    assign d_rs2 = id_insn[24:20];
    assign d_f7  = id_insn[31:25];

    always_comb begin
        d_imm = 32'h0;
        d_use1 = 1'b0; d_use2 = 1'b0; d_wr = 1'b0;
        d_load = 1'b0; d_store = 1'b0; d_branch = 1'b0; d_jal = 1'b0; d_jalr = 1'b0;
        d_csr = 1'b0; d_ecall = 1'b0; d_ebreak = 1'b0; d_mret = 1'b0; d_fencei = 1'b0;
        d_illegal = 1'b0;
        d_a_pc = 1'b0; d_b_imm = 1'b0;
        d_alu = ALU_ADD;

        case (d_op)
            OP_LUI: begin
                d_imm = {id_insn[31:12], 12'h0};
                d_wr = 1'b1; d_b_imm = 1'b1; d_alu = ALU_PASSB;
            end
            OP_AUIPC: begin
                d_imm = {id_insn[31:12], 12'h0};
                d_wr = 1'b1; d_a_pc = 1'b1; d_b_imm = 1'b1;
            end
            OP_JAL: begin
                d_imm = {{12{id_insn[31]}}, id_insn[19:12], id_insn[20], id_insn[30:21], 1'b0};
                d_wr = 1'b1; d_jal = 1'b1;
            end
            OP_JALR: begin
                d_imm = {{20{id_insn[31]}}, id_insn[31:20]};
                d_use1 = 1'b1; d_wr = 1'b1; d_jalr = 1'b1;
                if (d_f3 != 3'b000) d_illegal = 1'b1;
            end
            OP_BRANCH: begin
                d_imm = {{20{id_insn[31]}}, id_insn[7], id_insn[30:25], id_insn[11:8], 1'b0};
                d_use1 = 1'b1; d_use2 = 1'b1; d_branch = 1'b1;
                if (d_f3 == 3'b010 || d_f3 == 3'b011) d_illegal = 1'b1;
            end
            OP_LOAD: begin
                d_imm = {{20{id_insn[31]}}, id_insn[31:20]};
                d_use1 = 1'b1; d_wr = 1'b1; d_load = 1'b1; d_b_imm = 1'b1;
                if (d_f3 == 3'b011 || d_f3 == 3'b110 || d_f3 == 3'b111) d_illegal = 1'b1;
            end
            OP_STORE: begin
                d_imm = {{20{id_insn[31]}}, id_insn[31:25], id_insn[11:7]};
                d_use1 = 1'b1; d_use2 = 1'b1; d_store = 1'b1; d_b_imm = 1'b1;
                if (d_f3 > 3'b010) d_illegal = 1'b1;
            end
            OP_IMM: begin
                d_imm = {{20{id_insn[31]}}, id_insn[31:20]};
                d_use1 = 1'b1; d_wr = 1'b1; d_b_imm = 1'b1;
                case (d_f3)
                    3'b000: d_alu = ALU_ADD;
                    3'b010: d_alu = ALU_SLT;
                    3'b011: d_alu = ALU_SLTU;
                    3'b100: d_alu = ALU_XOR;
                    3'b110: d_alu = ALU_OR;
                    3'b111: d_alu = ALU_AND;
                    3'b001: begin d_alu = ALU_SLL; if (d_f7 != 7'h00) d_illegal = 1'b1; end
                    3'b101: begin
                        if (d_f7 == 7'h00)      d_alu = ALU_SRL;
                        else if (d_f7 == 7'h20) d_alu = ALU_SRA;
                        else                    d_illegal = 1'b1;
                    end
                endcase
            end
            OP_REG: begin
                d_use1 = 1'b1; d_use2 = 1'b1; d_wr = 1'b1;
                if (d_f7 == 7'h00) begin
                    case (d_f3)
                        3'b000: d_alu = ALU_ADD;
                        3'b001: d_alu = ALU_SLL;
                        3'b010: d_alu = ALU_SLT;
                        3'b011: d_alu = ALU_SLTU;
                        3'b100: d_alu = ALU_XOR;
                        3'b101: d_alu = ALU_SRL;
                        3'b110: d_alu = ALU_OR;
                        3'b111: d_alu = ALU_AND;
                    endcase
                end else if (d_f7 == 7'h20 && d_f3 == 3'b000) d_alu = ALU_SUB;
                else if (d_f7 == 7'h20 && d_f3 == 3'b101)     d_alu = ALU_SRA;
                else d_illegal = 1'b1;
            end
            OP_FENCE: begin
                if (d_f3 == 3'b001)      d_fencei = 1'b1;
                else if (d_f3 != 3'b000) d_illegal = 1'b1;
            end
            OP_SYSTEM: begin
                if (d_f3 == 3'b000) begin
                    if (id_insn == 32'h0000_0073)      d_ecall = 1'b1;
                    else if (id_insn == 32'h0010_0073) d_ebreak = 1'b1;
                    else if (id_insn == 32'h3020_0073) d_mret = 1'b1;
                    else if (id_insn == 32'h1050_0073) ;  // wfi: nop
                    else d_illegal = 1'b1;
                end else if (d_f3 == 3'b100) begin
                    d_illegal = 1'b1;
                end else begin
                    d_csr = 1'b1;
                    d_wr = 1'b1;
                    d_use1 = ~d_f3[2];                  // register form reads rs1
                    d_imm = {27'h0, d_rs1};              // zimm for the immediate form
                end
            end
            default: d_illegal = 1'b1;
        endcase
        if (d_illegal) begin
            d_use1 = 1'b0; d_use2 = 1'b0; d_wr = 1'b0;
            d_load = 1'b0; d_store = 1'b0; d_branch = 1'b0; d_jal = 1'b0; d_jalr = 1'b0; d_csr = 1'b0;
        end
    end

    // ------------------------------------------------------------------
    // register file (x0 hardwired), with WB -> ID bypass
    // ------------------------------------------------------------------
    logic [31:0] rf [1:31];
    logic        wb_we, wb_valid_wr_any;
    logic [4:0]  wb_rd;
    logic [31:0] wb_data;
    logic [31:0] d_rs1_val, d_rs2_val;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            for (int k = 1; k < 32; k++) rf[k] <= 32'h0;
        end else
`ifdef BUG_X0_WRITABLE
        if (wb_we) rf[wb_rd == 5'd0 ? 5'd1 : wb_rd] <= wb_data;  // x0 writes land in x1
`else
        if (wb_we && wb_rd != 5'd0) rf[wb_rd] <= wb_data;
`endif
    end

    always_comb begin
        d_rs1_val = (d_rs1 == 5'd0) ? 32'h0 : rf[d_rs1];
        d_rs2_val = (d_rs2 == 5'd0) ? 32'h0 : rf[d_rs2];
`ifndef BUG_NO_WB_BYPASS
        if (wb_we && wb_rd != 5'd0 && wb_rd == d_rs1) d_rs1_val = wb_data;
        if (wb_we && wb_rd != 5'd0 && wb_rd == d_rs2) d_rs2_val = wb_data;
`endif
    end

    // ------------------------------------------------------------------
    // ID/EX
    // ------------------------------------------------------------------
    logic        ex_valid;
    logic [31:0] ex_pc, ex_insn, ex_imm, ex_rs1_val, ex_rs2_val;
    logic [4:0]  ex_rd, ex_rs1, ex_rs2;
    logic [2:0]  ex_f3;
    logic        ex_use1, ex_use2, ex_wr, ex_load, ex_store, ex_branch, ex_jal, ex_jalr;
    logic        ex_csr, ex_ecall, ex_ebreak, ex_mret, ex_fencei, ex_illegal, ex_a_pc, ex_b_imm;
    logic [3:0]  ex_alu;

    always_ff @(posedge clk) begin
        if (!rst_n || redirect || stall) begin
            ex_valid <= 1'b0;
            ex_use1 <= 1'b0; ex_use2 <= 1'b0; ex_wr <= 1'b0; ex_load <= 1'b0; ex_store <= 1'b0;
            ex_branch <= 1'b0; ex_jal <= 1'b0; ex_jalr <= 1'b0; ex_csr <= 1'b0; ex_ecall <= 1'b0;
            ex_ebreak <= 1'b0; ex_mret <= 1'b0; ex_fencei <= 1'b0; ex_illegal <= 1'b0;
            ex_rd <= 5'd0; ex_rs1 <= 5'd0; ex_rs2 <= 5'd0;
        end else begin
            ex_valid   <= id_valid;
            ex_pc      <= id_pc;
            ex_insn    <= id_insn;
            ex_imm     <= d_imm;
            ex_rs1_val <= d_rs1_val;
            ex_rs2_val <= d_rs2_val;
            ex_rd      <= d_rd;
            ex_rs1     <= d_use1 ? d_rs1 : 5'd0;
            ex_rs2     <= d_use2 ? d_rs2 : 5'd0;
            ex_f3      <= d_f3;
            ex_use1    <= d_use1;   ex_use2   <= d_use2;   ex_wr     <= d_wr;
            ex_load    <= d_load;   ex_store  <= d_store;  ex_branch <= d_branch;
            ex_jal     <= d_jal;    ex_jalr   <= d_jalr;   ex_csr    <= d_csr;
            ex_ecall   <= d_ecall;  ex_ebreak <= d_ebreak; ex_mret   <= d_mret;
            ex_fencei  <= d_fencei; ex_illegal <= d_illegal;
            ex_a_pc    <= d_a_pc;   ex_b_imm  <= d_b_imm;  ex_alu    <= d_alu;
        end
    end

    // load-use hazard: ID needs a register the load in EX has not read yet
    always_comb begin
`ifdef BUG_NO_LOAD_USE_STALL
        stall = 1'b0;
`else
        stall = ex_valid && ex_load && ex_rd != 5'd0 && id_valid &&
                ((d_use1 && d_rs1 == ex_rd) || (d_use2 && d_rs2 == ex_rd));
`endif
    end

    // ------------------------------------------------------------------
    // EX: forwarding
    // ------------------------------------------------------------------
    logic        mem_valid, mem_wr, mem_load, mem_store, mem_trap;
    logic [4:0]  mem_rd;
    logic [31:0] mem_result;

    logic [31:0] fa, fb;          // forwarded operands
    logic [1:0]  fa_sel, fb_sel;  // 0 = register file, 1 = EX/MEM, 2 = MEM/WB

    // forwarding sources only count when the producing instruction really
    // writes a register (not a trap, not x0)
    logic exmem_fwd_ok, memwb_fwd_ok;
`ifdef BUG_FWD_X0
    assign exmem_fwd_ok = mem_valid && mem_wr && !mem_trap;
    assign memwb_fwd_ok = wb_valid_wr_any;
`else
    assign exmem_fwd_ok = mem_valid && mem_wr && !mem_trap && mem_rd != 5'd0;
    assign memwb_fwd_ok = wb_we && wb_rd != 5'd0;
`endif

    always_comb begin
        fa_sel = 2'd0;
        fb_sel = 2'd0;
`ifdef BUG_FWD_PRIORITY
        // wrong: older (MEM/WB) value wins over the newer one in EX/MEM
        if (memwb_fwd_ok && wb_rd == ex_rs1) fa_sel = 2'd2;
        else if (exmem_fwd_ok && mem_rd == ex_rs1) fa_sel = 2'd1;
        if (memwb_fwd_ok && wb_rd == ex_rs2) fb_sel = 2'd2;
        else if (exmem_fwd_ok && mem_rd == ex_rs2) fb_sel = 2'd1;
`else
        if (exmem_fwd_ok && mem_rd == ex_rs1) fa_sel = 2'd1;
        else if (memwb_fwd_ok && wb_rd == ex_rs1) fa_sel = 2'd2;
        if (exmem_fwd_ok && mem_rd == ex_rs2) fb_sel = 2'd1;
        else if (memwb_fwd_ok && wb_rd == ex_rs2) fb_sel = 2'd2;
`endif
        if (!ex_use1) fa_sel = 2'd0;
        if (!ex_use2) fb_sel = 2'd0;
`ifdef BUG_NO_FWD_EXMEM_RS1
        if (fa_sel == 2'd1) fa_sel = 2'd0;
`endif
`ifdef BUG_NO_FWD_EXMEM_RS2
        if (fb_sel == 2'd1) fb_sel = 2'd0;
`endif
`ifdef BUG_NO_FWD_MEMWB_RS1
        if (fa_sel == 2'd2) fa_sel = 2'd0;
`endif
`ifdef BUG_NO_FWD_MEMWB_RS2
        if (fb_sel == 2'd2) fb_sel = 2'd0;
`endif
        case (fa_sel)
            2'd1:    fa = mem_result;
            2'd2:    fa = wb_data;
            default: fa = ex_rs1_val;
        endcase
        case (fb_sel)
            2'd1:    fb = mem_result;
            2'd2:    fb = wb_data;
            default: fb = ex_rs2_val;
        endcase
        if (ex_rs1 == 5'd0) fa = 32'h0;
        if (ex_rs2 == 5'd0) fb = 32'h0;
    end

    // ------------------------------------------------------------------
    // EX: ALU, branch, address, CSR, exceptions
    // ------------------------------------------------------------------
    logic [31:0] alu_a, alu_b, alu_y;
    assign alu_a = ex_a_pc ? ex_pc : fa;
    assign alu_b = ex_b_imm ? ex_imm : fb;

    always_comb begin
        case (ex_alu)
            ALU_ADD:   alu_y = alu_a + alu_b;
            ALU_SUB:   alu_y = alu_a - alu_b;
            ALU_SLL:   alu_y = alu_a << alu_b[4:0];
`ifdef BUG_SLT_UNSIGNED
            ALU_SLT:   alu_y = {31'h0, alu_a < alu_b};
`else
            ALU_SLT:   alu_y = {31'h0, $signed(alu_a) < $signed(alu_b)};
`endif
            ALU_SLTU:  alu_y = {31'h0, alu_a < alu_b};
            ALU_XOR:   alu_y = alu_a ^ alu_b;
            ALU_SRL:   alu_y = alu_a >> alu_b[4:0];
`ifdef BUG_SRA_LOGICAL
            ALU_SRA:   alu_y = alu_a >> alu_b[4:0];
`else
            ALU_SRA:   alu_y = $unsigned($signed(alu_a) >>> alu_b[4:0]);
`endif
            ALU_OR:    alu_y = alu_a | alu_b;
            ALU_AND:   alu_y = alu_a & alu_b;
            ALU_PASSB: alu_y = alu_b;
            default:   alu_y = 32'h0;
        endcase
    end

    // branch condition
    logic br_taken;
    always_comb begin
        case (ex_f3)
            3'b000:  br_taken = (fa == fb);
            3'b001:  br_taken = (fa != fb);
            3'b100:  br_taken = $signed(fa) < $signed(fb);
            3'b101:  br_taken = $signed(fa) >= $signed(fb);
`ifdef BUG_BLTU_SIGNED
            3'b110:  br_taken = $signed(fa) < $signed(fb);
`else
            3'b110:  br_taken = fa < fb;
`endif
            3'b111:  br_taken = fa >= fb;
            default: br_taken = 1'b0;
        endcase
    end

    logic [31:0] jump_target;
    always_comb begin
        if (ex_jalr)
`ifdef BUG_JALR_NO_CLEAR_LSB
            jump_target = fa + ex_imm;
`else
            jump_target = (fa + ex_imm) & ~32'h1;
`endif
        else
            jump_target = ex_pc + ex_imm;
    end
    logic ex_takes_jump;
    assign ex_takes_jump = ex_jal || ex_jalr || (ex_branch && br_taken);

    // memory access size and alignment
    logic [31:0] ex_addr;
    logic [3:0]  ex_bmask;
    logic        ex_misaligned;
    assign ex_addr = fa + ex_imm;
    always_comb begin
        case (ex_f3[1:0])
            2'b00:   begin ex_bmask = 4'b0001 << ex_addr[1:0]; ex_misaligned = 1'b0; end
            2'b01:   begin
`ifdef BUG_SH_MASK
                ex_bmask = 4'b0011 << {ex_addr[1], 1'b0} >> 1;
`else
                ex_bmask = 4'b0011 << {ex_addr[1], 1'b0};
`endif
                ex_misaligned = ex_addr[0];
            end
            default: begin ex_bmask = 4'b1111; ex_misaligned = ex_addr[1:0] != 2'b00; end
        endcase
    end

    // CSRs
    logic [31:0] csr_mstatus, csr_mie, csr_mtvec, csr_mscratch, csr_mepc, csr_mcause, csr_mtval;
    logic [11:0] csr_addr;
    logic [31:0] csr_rdata, csr_src, csr_wdata;
    logic        csr_known, csr_writes, csr_illegal;
    assign csr_addr = ex_insn[31:20];
    assign csr_src  = ex_f3[2] ? ex_imm : fa;
    assign csr_writes = (ex_f3[1:0] == 2'b01) || (ex_insn[19:15] != 5'd0);

    always_comb begin
        csr_known = 1'b1;
        case (csr_addr)
            12'h300: csr_rdata = csr_mstatus;
            12'h301: csr_rdata = 32'h4000_0100;
            12'h304: csr_rdata = csr_mie;
            12'h305: csr_rdata = csr_mtvec;
            12'h340: csr_rdata = csr_mscratch;
            12'h341: csr_rdata = csr_mepc;
            12'h342: csr_rdata = csr_mcause;
            12'h343: csr_rdata = csr_mtval;
            12'h344: csr_rdata = 32'h0;
            12'hF14: csr_rdata = 32'h0;
            default: begin csr_rdata = 32'h0; csr_known = 1'b0; end
        endcase
        case (ex_f3[1:0])
            2'b01:   csr_wdata = csr_src;
            2'b10:   csr_wdata = csr_rdata | csr_src;
            default: csr_wdata = csr_rdata & ~csr_src;
        endcase
        csr_illegal = ex_csr && (!csr_known || (csr_writes && csr_addr[11:10] == 2'b11));
    end

    // exception detection
    logic        ex_trap;
    logic [31:0] ex_cause, ex_tval;
    always_comb begin
        ex_trap  = 1'b0;
        ex_cause = 32'h0;
        ex_tval  = 32'h0;
        if (ex_valid) begin
            if (ex_illegal || csr_illegal) begin
                ex_trap = 1'b1; ex_cause = CAUSE_ILLEGAL; ex_tval = ex_insn;
            end else if (ex_ecall) begin
                ex_trap = 1'b1; ex_cause = CAUSE_ECALL;
            end else if (ex_ebreak) begin
                ex_trap = 1'b1; ex_cause = CAUSE_BREAKPOINT;
            end else if (ex_takes_jump && jump_target[1:0] != 2'b00) begin
                ex_trap = 1'b1; ex_cause = CAUSE_MISALIGNED_FETCH; ex_tval = jump_target;
            end else if (ex_load && ex_misaligned) begin
                ex_trap = 1'b1; ex_cause = CAUSE_MISALIGNED_LOAD; ex_tval = ex_addr;
            end else if (ex_store && ex_misaligned) begin
                ex_trap = 1'b1; ex_cause = CAUSE_MISALIGNED_STORE; ex_tval = ex_addr;
            end
        end
    end

    // CSR state updates (CSR instructions and traps both happen in EX, in order)
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            csr_mstatus  <= 32'h0000_1800;
            csr_mie      <= 32'h0;
            csr_mtvec    <= 32'h0;
            csr_mscratch <= 32'h0;
            csr_mepc     <= 32'h0;
            csr_mcause   <= 32'h0;
            csr_mtval    <= 32'h0;
        end else if (ex_valid && ex_trap) begin
`ifdef BUG_MEPC_NEXT
            csr_mepc    <= ex_pc + 32'd4;
`else
            csr_mepc    <= ex_pc;
`endif
            csr_mcause  <= ex_cause;
            csr_mtval   <= ex_tval;
            csr_mstatus <= {csr_mstatus[31:13], 2'b11, csr_mstatus[10:8], csr_mstatus[3], csr_mstatus[6:4], 1'b0, csr_mstatus[2:0]};
        end else if (ex_valid && ex_mret) begin
            csr_mstatus <= {csr_mstatus[31:13], 2'b11, csr_mstatus[10:8], 1'b1, csr_mstatus[6:4], csr_mstatus[7], csr_mstatus[2:0]};
        end else if (ex_valid && ex_csr && csr_writes) begin
            case (csr_addr)
                12'h300: csr_mstatus  <= (csr_wdata & 32'h88) | 32'h1800;
                12'h304: csr_mie      <= csr_wdata & 32'h888;
                12'h305: csr_mtvec    <= csr_wdata & ~32'h3;
                12'h340: csr_mscratch <= csr_wdata;
                12'h341: csr_mepc     <= csr_wdata & ~32'h3;
                12'h342: csr_mcause   <= csr_wdata;
                12'h343: csr_mtval    <= csr_wdata;
                default: ;
            endcase
        end
    end

    // redirect: traps first, then mret, jumps, fence.i
    always_comb begin
        redirect    = 1'b0;
        redirect_pc = 32'h0;
        if (ex_valid) begin
            if (ex_trap)            begin redirect = 1'b1; redirect_pc = csr_mtvec; end
            else if (ex_mret)       begin redirect = 1'b1; redirect_pc = csr_mepc; end
            else if (ex_takes_jump) begin redirect = 1'b1; redirect_pc = jump_target; end
            else if (ex_fencei)     begin redirect = 1'b1; redirect_pc = ex_pc + 32'd4; end
        end
    end

    // result going down the pipe
    logic [31:0] ex_result, ex_next_pc;
    always_comb begin
        if (ex_jal || ex_jalr) ex_result = ex_pc + 32'd4;
        else if (ex_csr)       ex_result = csr_rdata;
        else                   ex_result = alu_y;
        ex_next_pc = redirect ? redirect_pc : ex_pc + 32'd4;
    end

    // data read address goes to memory from EX
    assign dmem_raddr = {ex_addr[31:2], 2'b00};
    assign dmem_re    = ex_valid && ex_load && !ex_trap;

    // store data in byte lanes
    logic [31:0] ex_store_data;
    always_comb begin
        case (ex_f3[1:0])
            2'b00:   ex_store_data = {4{fb[7:0]}};
            2'b01:   ex_store_data = {2{fb[15:0]}};
            default: ex_store_data = fb;
        endcase
    end

    // store-to-load bypass: a store in MEM writes the word this load reads at
    // the same edge, and the synchronous RAM would return the old data
    logic        mem_store_now;
    logic [31:0] mem_addr_w, mem_store_data;
    logic [3:0]  mem_bmask;
    logic        byp_hit;
`ifdef BUG_NO_STORE_BYPASS
    assign byp_hit = 1'b0;
`else
    assign byp_hit = dmem_re && mem_store_now && (mem_addr_w[31:2] == ex_addr[31:2]);
`endif

    // ------------------------------------------------------------------
    // EX/MEM
    // ------------------------------------------------------------------
    logic [31:0] mem_pc, mem_insn, mem_next_pc, mem_rs1_val, mem_rs2_val;
    logic [4:0]  mem_rs1, mem_rs2;
    logic [2:0]  mem_f3;
    logic [1:0]  mem_off;
    logic        mem_byp;
    logic [31:0] mem_byp_data;
    logic [3:0]  mem_byp_mask;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            mem_valid <= 1'b0;
            mem_wr <= 1'b0; mem_load <= 1'b0; mem_store <= 1'b0; mem_trap <= 1'b0;
            mem_rd <= 5'd0; mem_byp <= 1'b0;
        end else begin
            mem_valid      <= ex_valid;
            mem_pc         <= ex_pc;
            mem_insn       <= ex_insn;
            mem_next_pc    <= ex_next_pc;
            mem_trap       <= ex_valid && ex_trap;
            mem_wr         <= ex_valid && ex_wr && !ex_trap;
            mem_load       <= ex_valid && ex_load && !ex_trap;
            mem_store      <= ex_valid && ex_store && !ex_trap;
            mem_rd         <= ex_rd;
            mem_result     <= ex_result;
            mem_rs1        <= ex_trap ? 5'd0 : ex_rs1;
            mem_rs2        <= ex_trap ? 5'd0 : ex_rs2;
            mem_rs1_val    <= (ex_trap || ex_rs1 == 5'd0) ? 32'h0 : fa;
            mem_rs2_val    <= (ex_trap || ex_rs2 == 5'd0) ? 32'h0 : fb;
            mem_f3         <= ex_f3;
            mem_off        <= ex_addr[1:0];
            mem_addr_w     <= {ex_addr[31:2], 2'b00};
            mem_bmask      <= ex_bmask;
            mem_store_data <= ex_store_data;
            mem_byp        <= byp_hit;
            mem_byp_data   <= mem_store_data;
            mem_byp_mask   <= mem_bmask;
        end
    end

    // ------------------------------------------------------------------
    // MEM: store, load align
    // ------------------------------------------------------------------
    assign mem_store_now = mem_valid && mem_store;
    assign dmem_waddr = mem_addr_w;
    assign dmem_wdata = mem_store_data;
    assign dmem_wmask = mem_store_now ? mem_bmask : 4'b0000;

    logic [31:0] ld_word, ld_shift, ld_val;
    always_comb begin
        ld_word = dmem_rdata;
        if (mem_byp) begin
            if (mem_byp_mask[0]) ld_word[7:0]   = mem_byp_data[7:0];
            if (mem_byp_mask[1]) ld_word[15:8]  = mem_byp_data[15:8];
            if (mem_byp_mask[2]) ld_word[23:16] = mem_byp_data[23:16];
            if (mem_byp_mask[3]) ld_word[31:24] = mem_byp_data[31:24];
        end
        ld_shift = ld_word >> {mem_off, 3'b000};
        case (mem_f3)
`ifdef BUG_LB_ZERO_EXTEND
            3'b000:  ld_val = {24'h0, ld_shift[7:0]};
`else
            3'b000:  ld_val = {{24{ld_shift[7]}}, ld_shift[7:0]};
`endif
            3'b001:  ld_val = {{16{ld_shift[15]}}, ld_shift[15:0]};
            3'b100:  ld_val = {24'h0, ld_shift[7:0]};
            3'b101:  ld_val = {16'h0, ld_shift[15:0]};
            default: ld_val = ld_word;
        endcase
    end

    // ------------------------------------------------------------------
    // MEM/WB
    // ------------------------------------------------------------------
    logic        wb_valid, wb_trap, wb_load, wb_store, wb_wr;
    logic [31:0] wb_pc, wb_insn, wb_next_pc, wb_rs1_val, wb_rs2_val, wb_addr, wb_rword, wb_sdata;
    logic [4:0]  wb_rs1, wb_rs2;
    logic [3:0]  wb_bmask;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            wb_valid <= 1'b0;
            wb_wr <= 1'b0; wb_trap <= 1'b0; wb_load <= 1'b0; wb_store <= 1'b0;
            wb_rd <= 5'd0;
        end else begin
            wb_valid   <= mem_valid;
            wb_trap    <= mem_trap;
            wb_wr      <= mem_wr;
            wb_load    <= mem_load;
            wb_store   <= mem_store;
            wb_rd      <= mem_rd;
            wb_data    <= mem_load ? ld_val : mem_result;
            wb_pc      <= mem_pc;
            wb_insn    <= mem_insn;
            wb_next_pc <= mem_next_pc;
            wb_rs1     <= mem_rs1;
            wb_rs2     <= mem_rs2;
            wb_rs1_val <= mem_rs1_val;
            wb_rs2_val <= mem_rs2_val;
            wb_addr    <= mem_addr_w;
            wb_bmask   <= mem_bmask;
            wb_rword   <= ld_word;
            wb_sdata   <= mem_store_data;
        end
    end

    assign wb_we = wb_valid && wb_wr;
    assign wb_valid_wr_any = wb_valid && wb_wr;

    // ------------------------------------------------------------------
    // RVFI
    // ------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (!rst_n) rvfi_order <= 64'd0;
        else if (wb_valid) rvfi_order <= rvfi_order + 64'd1;
    end

    always_comb begin
        rvfi_valid     = wb_valid;
        rvfi_insn      = wb_insn;
        rvfi_trap      = wb_trap;
        rvfi_pc_rdata  = wb_pc;
        rvfi_pc_wdata  = wb_next_pc;
        rvfi_rs1_addr  = wb_rs1;
        rvfi_rs2_addr  = wb_rs2;
        rvfi_rs1_rdata = wb_rs1_val;
        rvfi_rs2_rdata = wb_rs2_val;
        rvfi_rd_addr   = (wb_wr && wb_rd != 5'd0) ? wb_rd : 5'd0;
        rvfi_rd_wdata  = (wb_wr && wb_rd != 5'd0) ? wb_data : 32'h0;
        rvfi_mem_addr  = (wb_load || wb_store) ? wb_addr : 32'h0;
        rvfi_mem_rmask = wb_load ? wb_bmask : 4'b0000;
        rvfi_mem_wmask = wb_store ? wb_bmask : 4'b0000;
        rvfi_mem_rdata = wb_load ? wb_rword : 32'h0;
        rvfi_mem_wdata = wb_store ? wb_sdata : 32'h0;
    end

    // ------------------------------------------------------------------
    // coverage events
    // ------------------------------------------------------------------
    always_comb begin
        cov = 16'h0;
        cov[0]  = ex_valid && fa_sel == 2'd1;                 // EX/MEM -> EX, rs1
        cov[1]  = ex_valid && fb_sel == 2'd1;                 // EX/MEM -> EX, rs2
        cov[2]  = ex_valid && fa_sel == 2'd2;                 // MEM/WB -> EX, rs1
        cov[3]  = ex_valid && fb_sel == 2'd2;                 // MEM/WB -> EX, rs2
        cov[4]  = stall;                                      // load-use stall
        cov[5]  = ex_valid && ex_branch && br_taken && !ex_trap;   // taken branch flush
        cov[6]  = ex_valid && ex_branch && !br_taken;         // not-taken branch
        cov[7]  = ex_valid && ex_jal && !ex_trap;             // jal flush
        cov[8]  = ex_valid && ex_jalr && !ex_trap;            // jalr flush
        cov[9]  = byp_hit;                                    // store -> load same word
        cov[10] = ex_valid && ex_trap;                        // exception taken
        cov[11] = ex_valid && ex_mret;                        // mret
        cov[12] = id_valid && wb_we && wb_rd != 5'd0 &&
                  ((d_use1 && wb_rd == d_rs1) || (d_use2 && wb_rd == d_rs2));  // WB -> ID bypass
        cov[13] = ex_valid && ex_wr && ex_rd == 5'd0 && !ex_trap;  // write to x0
        cov[14] = stall && redirect;                          // stall and flush same cycle
        cov[15] = ex_valid && ex_fencei;                      // fence.i
    end

endmodule
