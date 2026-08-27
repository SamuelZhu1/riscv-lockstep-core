// Memory-mapped I2C master: byte commands plus a bit-bang mode the driver
// uses for bus recovery (clocking SCL until a stuck slave releases SDA).
//
// Registers (word offsets from 0x0002_0100)
//   0x00 PRESCALE  clocks per quarter SCL period (50 MHz / 100 kHz / 4 = 125)
//   0x04 CMD       write: [0] START  [1] STOP  [2] WRITE TXDATA  [3] READ into RXDATA
//                         [4] NACK the byte read (master sends NACK instead of ACK)
//                  the engine runs START -> WRITE/READ -> STOP for the bits set
//   0x08 TXDATA    byte to send
//   0x0C RXDATA    last byte received
//   0x10 STATUS    [0] busy  [1] slave NACKed the last byte written
//                  [2] SDA line level  [3] SCL line level
//   0x14 BITBANG   [0] enable  [1] pull SCL low  [2] pull SDA low
//
// Each bit takes four quarter periods: set SDA while SCL is low, release
// SCL, sample SDA once SCL is really high (a slave may stretch the clock),
// then pull SCL low again.

module i2c_master (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        we,
    input  logic [5:0]  waddr,
    input  logic [31:0] wdata,
    input  logic [5:0]  raddr,
    output logic [31:0] rdata,
    output logic        scl_oe,
    output logic        sda_oe,
    input  logic        scl_in,
    input  logic        sda_in
);

    typedef enum logic [2:0] {S_IDLE, S_START, S_BIT, S_STOP} state_t;

    logic [15:0] prescale;
    logic [7:0]  txdata, rxdata;
    logic        rx_nack;
    logic        bb_en, bb_scl, bb_sda;

    // pending command bits
    logic        c_start, c_stop, c_write, c_read, c_nack;

    state_t      state;
    logic [1:0]  q;            // quarter within the current bit / condition
    logic [15:0] cnt;
    logic [3:0]  bitn;         // 8..0, bit 0 of the 9 is the ACK slot
    logic [7:0]  shreg;
    logic        reading;
    logic        scl_o, sda_o; // 1 = pull low

    logic busy;
    assign busy = (state != S_IDLE) || c_start || c_write || c_read || c_stop;

    // two-flop synchronizers for the bus lines
    logic [1:0] scl_s, sda_s;
    always_ff @(posedge clk) begin
        scl_s <= {scl_s[0], scl_in};
        sda_s <= {sda_s[0], sda_in};
    end
    logic scl_hi, sda_hi;
    assign scl_hi = scl_s[1];
    assign sda_hi = sda_s[1];

    always_comb begin
        case (raddr)
            6'h0:    rdata = {16'h0, prescale};
            6'h2:    rdata = {24'h0, txdata};
            6'h3:    rdata = {24'h0, rxdata};
            6'h4:    rdata = {28'h0, scl_hi, sda_hi, rx_nack, busy};
            6'h5:    rdata = {29'h0, bb_sda, bb_scl, bb_en};
            default: rdata = 32'h0;
        endcase
    end

    logic tick;
    assign tick = (cnt == 16'd0);

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            prescale <= 16'd125;
            txdata <= 8'h0; rxdata <= 8'h0; rx_nack <= 1'b0;
            bb_en <= 1'b0; bb_scl <= 1'b0; bb_sda <= 1'b0;
            c_start <= 1'b0; c_stop <= 1'b0; c_write <= 1'b0; c_read <= 1'b0; c_nack <= 1'b0;
            state <= S_IDLE; q <= 2'd0; cnt <= 16'd0; bitn <= 4'd0; shreg <= 8'h0; reading <= 1'b0;
            scl_o <= 1'b0; sda_o <= 1'b0;
        end else begin
            // register writes
            if (we) begin
                case (waddr)
                    6'h0: prescale <= (wdata[15:0] == 16'd0) ? 16'd1 : wdata[15:0];
                    6'h1: if (state == S_IDLE) begin
                        c_start <= wdata[0]; c_stop <= wdata[1]; c_write <= wdata[2];
                        c_read  <= wdata[3]; c_nack <= wdata[4];
                    end
                    6'h2: txdata <= wdata[7:0];
                    6'h5: begin bb_en <= wdata[0]; bb_scl <= wdata[1]; bb_sda <= wdata[2]; end
                    default: ;
                endcase
            end

            // quarter-period timer; holds while SCL is released but still low (stretching)
            if (state != S_IDLE) begin
                if (tick) begin
                    if (!(q == 2'd2 && !scl_o && !scl_hi)) cnt <= prescale - 16'd1;
                end else begin
                    cnt <= cnt - 16'd1;
                end
            end

            case (state)
                S_IDLE: begin
                    q <= 2'd0;
                    cnt <= prescale - 16'd1;
                    if (c_start) begin
                        state <= S_START; c_start <= 1'b0;
                    end else if (c_write || c_read) begin
                        state <= S_BIT; bitn <= 4'd8; reading <= c_read;
                        shreg <= c_read ? 8'hFF : txdata;
                        c_write <= 1'b0; c_read <= 1'b0;
                        if (c_write) rx_nack <= 1'b0;
                    end else if (c_stop) begin
                        state <= S_STOP; c_stop <= 1'b0;
                    end
                end

                // START (also a repeated START): release SDA, release SCL,
                // pull SDA low while SCL is high, pull SCL low
                S_START: if (tick && !(q == 2'd2 && !scl_hi)) begin
                    case (q)
                        2'd0: sda_o <= 1'b0;
                        2'd1: scl_o <= 1'b0;
                        2'd2: sda_o <= 1'b1;
                        2'd3: scl_o <= 1'b1;
                    endcase
                    q <= q + 2'd1;
                    if (q == 2'd3) state <= S_IDLE;
                end

                S_BIT: if (tick && !(q == 2'd2 && !scl_hi)) begin
                    case (q)
                        2'd0: begin
                            // drive the next bit while SCL is low
                            if (bitn != 4'd0) sda_o <= reading ? 1'b0 : ~shreg[7];
                            else              sda_o <= reading ? ~c_nack : 1'b0;  // ACK slot
                        end
                        2'd1: scl_o <= 1'b0;   // release SCL
                        2'd2: begin            // SCL high: sample
                            if (bitn != 4'd0) shreg <= {shreg[6:0], sda_hi};
                            else if (!reading) rx_nack <= sda_hi;
                        end
                        2'd3: begin
                            scl_o <= 1'b1;
                            if (bitn == 4'd0) begin
                                sda_o <= 1'b0;
                                if (reading) rxdata <= shreg;
                                state <= S_IDLE;
                                c_nack <= 1'b0;
                            end
                            bitn <= bitn - 4'd1;
                        end
                    endcase
                    q <= q + 2'd1;
                end

                // STOP: pull SDA low, release SCL, release SDA while SCL is high
                S_STOP: if (tick && !(q == 2'd2 && !scl_hi)) begin
                    case (q)
                        2'd0: sda_o <= 1'b1;
                        2'd1: scl_o <= 1'b0;
                        2'd2: sda_o <= 1'b0;
                        2'd3: ;
                    endcase
                    q <= q + 2'd1;
                    if (q == 2'd3) state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    // reset leaves the bus idle (both lines released)
    assign scl_oe = bb_en ? bb_scl : scl_o;
    assign sda_oe = bb_en ? bb_sda : sda_o;

endmodule
