// Testbench model of an SFP+ module's management interface: two I2C
// slaves at 0x50 (A0h, identity EEPROM) and 0x51 (A2h, diagnostics), each
// 256 bytes with an auto-incrementing offset. Contents come from the C++
// harness so tests can check the decoded values.
//
// Fault injection (set by the harness when firmware writes FAULT):
//   fault_nack  : NACK the next N address bytes, like a module that is
//                 still busy after power-up
//   fault_stuck : on the next byte read, pull SDA low and keep holding it
//                 after the transfer ends, the way a slave that lost its
//                 place mid-byte waits for clocks; it lets go after the
//                 master clocks SCL 9 times on an otherwise idle bus

module i2c_eeprom (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       scl,        // resolved bus levels
    input  logic       sda,
    output logic       sda_oe,     // 1 = pull SDA low
    input  logic [7:0] fault_nack,
    input  logic       fault_stuck_arm,
    output logic       fault_nack_used,
    output logic       stuck_active
);
    import "DPI-C" function int unsigned eeprom_read(input int unsigned dev, input int unsigned off);

    function automatic logic [7:0] rom(input logic dev, input logic [7:0] off);
        logic [31:0] v;
        v = eeprom_read({31'h0, dev}, {24'h0, off});
        return v[7:0];
    endfunction

    typedef enum logic [2:0] {IDLE, ADDR, ACK_ADDR, WR_BYTE, ACK_WR, RD_BYTE, ACK_RD, STUCK} st_t;

    st_t        st;
    logic       scl_q, sda_q;
    logic [3:0] bitc;
    logic [7:0] sh;
    logic       dev_a2;
    logic       rw;
    logic       got_offset;
    logic [7:0] offset;
    logic [7:0] nack_left;
    logic       stuck_armed;
    logic [4:0] stuck_clocks;
    logic [7:0] first_byte;
    logic [6:0] idle_cnt;

    wire scl_rise = scl && !scl_q;
    wire scl_fall = !scl && scl_q;
    wire start_c  = scl && scl_q && sda_q && !sda;
    wire stop_c   = scl && scl_q && !sda_q && sda;

    assign stuck_active = (st == STUCK);
    always_comb first_byte = rom(dev_a2, offset);

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            st <= IDLE; scl_q <= 1'b1; sda_q <= 1'b1; sda_oe <= 1'b0;
            bitc <= 4'd0; sh <= 8'h0; dev_a2 <= 1'b0; rw <= 1'b0; got_offset <= 1'b0; offset <= 8'h0;
            nack_left <= 8'd0; stuck_armed <= 1'b0; stuck_clocks <= 5'd0; idle_cnt <= 7'd0; fault_nack_used <= 1'b0;
        end else begin
            scl_q <= scl;
            sda_q <= sda;
            fault_nack_used <= 1'b0;
            if (fault_nack != 8'd0) nack_left <= fault_nack;
            if (fault_stuck_arm) stuck_armed <= 1'b1;

            if (st == STUCK) begin
                // holds SDA low through the rest of the read and the STOP; only
                // once the master has let SCL sit high (transfer over) does it
                // count clocks, and 9 of those free it
                sda_oe <= 1'b1;
                if (scl) begin
                    if (idle_cnt != 7'd127) idle_cnt <= idle_cnt + 7'd1;
                end else if (idle_cnt != 7'd127) idle_cnt <= 7'd0;
                if (idle_cnt == 7'd127 && scl_rise) stuck_clocks <= stuck_clocks + 5'd1;
                if (stuck_clocks == 5'd9) begin
                    sda_oe <= 1'b0;
                    st <= IDLE;
                    stuck_clocks <= 5'd0;
                    idle_cnt <= 7'd0;
                end
            end else if (start_c) begin
                st <= ADDR; bitc <= 4'd0; sda_oe <= 1'b0;
            end else if (stop_c) begin
                st <= IDLE; sda_oe <= 1'b0;
            end else begin
                case (st)
                    ADDR, WR_BYTE: if (scl_rise) begin
                        sh <= {sh[6:0], sda};
                        bitc <= bitc + 4'd1;
                    end else if (scl_fall && bitc == 4'd8) begin
                        bitc <= 4'd0;
                        if (st == ADDR) begin
                            if (sh[7:1] == 7'h50 || sh[7:1] == 7'h51) begin
                                if (nack_left != 8'd0) begin
                                    nack_left <= nack_left - 8'd1;
                                    fault_nack_used <= 1'b1;
                                    st <= IDLE;          // no ACK: master sees NACK
                                end else begin
                                    dev_a2 <= sh[1];
                                    rw <= sh[0];
                                    if (!sh[0]) got_offset <= 1'b0;
                                    sda_oe <= 1'b1;      // ACK
                                    st <= ACK_ADDR;
                                end
                            end else st <= IDLE;
                        end else begin
                            if (!got_offset) begin offset <= sh; got_offset <= 1'b1; end
                            else offset <= offset + 8'd1;  // writes are accepted and dropped
                            sda_oe <= 1'b1;
                            st <= ACK_WR;
                        end
                    end

                    ACK_ADDR, ACK_WR: if (scl_fall) begin
                        if (st == ACK_ADDR && rw) begin
                            sh <= rom(dev_a2, offset);
                            if (stuck_armed) begin
                                stuck_armed <= 1'b0;
                                st <= STUCK;
                                sda_oe <= 1'b1;
                            end else begin
                                sda_oe <= ~first_byte[7];
                                st <= RD_BYTE;
                                bitc <= 4'd1;
                            end
                        end else begin
                            sda_oe <= 1'b0;
                            st <= WR_BYTE;
                            bitc <= 4'd0;
                        end
                    end

                    RD_BYTE: if (scl_fall) begin
                        if (bitc == 4'd8) begin
                            sda_oe <= 1'b0;        // release for the master's ACK
                            st <= ACK_RD;
                        end else begin
                            sda_oe <= ~sh[3'd7 - bitc[2:0]];
                            bitc <= bitc + 4'd1;
                        end
                    end

                    // master's ACK/NACK, sampled while SCL is high
                    ACK_RD: if (scl_rise) begin
                        if (sda) begin
                            st <= IDLE;            // NACK: master is done
                        end else begin
                            offset <= offset + 8'd1;
                            sh <= rom(dev_a2, offset + 8'd1);
                            bitc <= 4'd0;          // next fall drives bit 7
                            st <= RD_BYTE;
                        end
                    end

                    default: ;
                endcase
            end
        end
    end

endmodule
