//
// axi_swd_engine.v - Hardware Accelerated SWD (Serial Wire Debug) Engine
// Target: Xilinx Zynq-7000 PL (XC7Z010-1CLG400C)
//
// Dynamic Frequency: 100 kHz up to 50 MHz via 16-bit prescaler
// Supports:
//  - 32-bit AP/DP SWD Transfers (Packet Header, Turnaround, ACK, Data, Parity, Idle Cycles)
//  - Raw Bit Sequences (1 to 32 bits per word) for Line Reset, Dormant-to-SWD switches
//  - Hardware / EMIO Multiplexing (Fail-safe software fallback)
//
`timescale 1ns / 1ps

module axi_swd_engine #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
)(
    // AXI4-Lite Clock & Reset
    input  wire                              s_axi_aclk,
    input  wire                              s_axi_aresetn,

    // AXI4-Lite Write Address Channel
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_awaddr,
    input  wire [2:0]                        s_axi_awprot,
    input  wire                              s_axi_awvalid,
    output reg                               s_axi_awready,

    // AXI4-Lite Write Data Channel
    input  wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  wire                              s_axi_wvalid,
    output reg                               s_axi_wready,

    // AXI4-Lite Write Response Channel
    output reg  [1:0]                        s_axi_bresp,
    output reg                               s_axi_bvalid,
    input  wire                              s_axi_bready,

    // AXI4-Lite Read Address Channel
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
    input  wire [2:0]                        s_axi_arprot,
    input  wire                              s_axi_arvalid,
    output reg                               s_axi_arready,

    // AXI4-Lite Read Data Channel
    output reg  [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
    output reg  [1:0]                        s_axi_rresp,
    output reg                               s_axi_rvalid,
    input  wire                              s_axi_rready,

    // Physical SWD Interface to Top-Level IOBUFs
    output wire                              swd_hw_en,
    output reg                               swclk_o,
    output reg                               swdio_o,
    output reg                               swdio_t, // 1 = Input (tristate), 0 = Output
    input  wire                              swdio_i
);

    // =========================================================================
    // Register Address Map (Base 0x43C00000)
    // =========================================================================
    localparam ADDR_CTRL    = 6'h00; // Control / Command
    localparam ADDR_CLK_DIV = 6'h04; // 16-bit Clock Prescaler
    localparam ADDR_WDATA   = 6'h08; // 32-bit Write Payload
    localparam ADDR_RDATA   = 6'h0C; // 32-bit Read Payload
    localparam ADDR_STATUS  = 6'h10; // Status / ACK / Parity
    localparam ADDR_RAW_LEN = 6'h14; // Raw Mode Bit Length (1-32)
    localparam ADDR_ID      = 6'h18; // Core ID: 0x53574401 ("SWD1")

    // Internal Registers
    reg [31:0] reg_ctrl;
    reg [15:0] reg_clk_div;
    reg [31:0] reg_wdata;
    reg [31:0] reg_rdata;
    reg [5:0]  reg_raw_len;

    // Status bits
    reg        fsm_busy;
    reg [2:0]  target_ack;
    reg        parity_err;
    reg        sticky_done;

    assign swd_hw_en = reg_ctrl[15]; // Bit 15 enables hardware multiplexer

    // =========================================================================
    // AXI4-Lite Slave Logic
    // =========================================================================
    reg fsm_start_pulse;
    reg aw_en;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_awready   <= 1'b0;
            s_axi_wready    <= 1'b0;
            s_axi_bvalid    <= 1'b0;
            s_axi_bresp     <= 2'b00;
            aw_en           <= 1'b1;
            reg_ctrl        <= 32'h00000800; // Idle cycles = 8, HW_EN = 0
            reg_clk_div     <= 16'd49;       // Default: 1.0 MHz (100MHz / (2 * 50))
            reg_wdata       <= 32'd0;
            reg_raw_len     <= 6'd32;
            fsm_start_pulse <= 1'b0;
        end else begin
            fsm_start_pulse <= 1'b0; // Default 1-cycle strobe

            // Write Address Ready Handshake
            if (~s_axi_awready && s_axi_awvalid && s_axi_wvalid && aw_en) begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
                aw_en         <= 1'b0;

                // Write Register Decoding
                case (s_axi_awaddr[5:0])
                    ADDR_CTRL: begin
                        reg_ctrl <= s_axi_wdata;
                        if (s_axi_wdata[0] && !fsm_busy) begin
                            fsm_start_pulse <= 1'b1;
                        end
                    end
                    ADDR_CLK_DIV: reg_clk_div <= s_axi_wdata[15:0];
                    ADDR_WDATA:   reg_wdata   <= s_axi_wdata;
                    ADDR_RAW_LEN: reg_raw_len <= s_axi_wdata[5:0];
                    default: ;
                endcase
            end else begin
                s_axi_awready <= 1'b0;
                s_axi_wready  <= 1'b0;
            end

            // Write Response
            if (s_axi_awready && s_axi_wready) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
            end else if (s_axi_bready && s_axi_bvalid) begin
                s_axi_bvalid <= 1'b0;
                aw_en        <= 1'b1;
            end

            // Clear START bit in reg_ctrl automatically when FSM finishes
            if (!fsm_busy) begin
                reg_ctrl[0] <= 1'b0;
            end
        end
    end

    // AXI Read Channel
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rresp   <= 2'b00;
            s_axi_rdata   <= 32'd0;
        end else begin
            if (~s_axi_arready && s_axi_arvalid) begin
                s_axi_arready <= 1'b1;
                s_axi_rvalid  <= 1'b1;
                s_axi_rresp   <= 2'b00;

                case (s_axi_araddr[5:0])
                    ADDR_CTRL:    s_axi_rdata <= reg_ctrl;
                    ADDR_CLK_DIV: s_axi_rdata <= {16'd0, reg_clk_div};
                    ADDR_WDATA:   s_axi_rdata <= reg_wdata;
                    ADDR_RDATA:   s_axi_rdata <= reg_rdata;
                    ADDR_STATUS:  s_axi_rdata <= {16'h5357, 10'd0, sticky_done, parity_err, target_ack, fsm_busy};
                    ADDR_RAW_LEN: s_axi_rdata <= {26'd0, reg_raw_len};
                    ADDR_ID:      s_axi_rdata <= 32'h53574401; // "SWD1"
                    default:      s_axi_rdata <= 32'd0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid  <= 1'b0;
                s_axi_arready <= 1'b0;
            end
        end
    end

    // =========================================================================
    // SWD Clock Prescaler Generator
    // =========================================================================
    reg [15:0] clk_cnt;
    wire tick = (clk_cnt >= reg_clk_div);

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn || !fsm_busy) begin
            clk_cnt <= 16'd0;
        end else begin
            if (tick)
                clk_cnt <= 16'd0;
            else
                clk_cnt <= clk_cnt + 1'b1;
        end
    end

    // =========================================================================
    // SWD Hardware State Machine (FSM)
    // =========================================================================
    localparam ST_IDLE          = 4'd0;
    localparam ST_HEADER        = 4'd1;
    localparam ST_TRN1          = 4'd2;
    localparam ST_ACK           = 4'd3;
    localparam ST_RDATA         = 4'd4;
    localparam ST_TRN_READ_END  = 4'd5;
    localparam ST_TRN2          = 4'd6;
    localparam ST_WDATA         = 4'd7;
    localparam ST_IDLE_CYCLES   = 4'd8;
    localparam ST_RAW           = 4'd9;

    reg [3:0]  state;
    reg [7:0]  header_byte;
    reg [5:0]  bit_idx;
    reg [31:0] shift_reg;
    reg [4:0]  idle_cnt;
    reg        phase; // 0 = SWCLK LOW phase, 1 = SWCLK HIGH phase
    reg        sample_parity;

    wire apndp     = reg_ctrl[1];
    wire rnw       = reg_ctrl[2];
    wire [1:0] a23 = reg_ctrl[4:3];
    wire hdr_par   = apndp ^ rnw ^ a23[0] ^ a23[1]; // Even parity of request fields
    wire [4:0] cfg_idle = (reg_ctrl[12:8] == 5'd0) ? 5'd8 : reg_ctrl[12:8];

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            state         <= ST_IDLE;
            fsm_busy      <= 1'b0;
            sticky_done   <= 1'b0;
            target_ack    <= 3'b000;
            parity_err    <= 1'b0;
            swclk_o       <= 1'b1; // Idle clock is HIGH
            swdio_o       <= 1'b1;
            swdio_t       <= 1'b0; // Driven
            phase         <= 1'b0;
            bit_idx       <= 6'd0;
            idle_cnt      <= 5'd0;
            header_byte   <= 8'd0;
            shift_reg     <= 32'd0;
            reg_rdata     <= 32'd0;
            sample_parity <= 1'b0;
        end else begin
            case (state)
                ST_IDLE: begin
                    swclk_o     <= 1'b1;
                    swdio_o     <= 1'b1;
                    swdio_t     <= 1'b0;
                    phase       <= 1'b0;
                    fsm_busy    <= 1'b0;

                    if (fsm_start_pulse) begin
                        fsm_busy    <= 1'b1;
                        sticky_done <= 1'b0;
                        parity_err  <= 1'b0;
                        target_ack  <= 3'b111; // Default to timeout

                        if (reg_ctrl[14]) begin
                            // Raw Mode
                            state     <= ST_RAW;
                            bit_idx   <= 6'd0;
                            shift_reg <= reg_wdata;
                        end else begin
                            // SWD 32-bit Frame
                            // Packet Header: [Start=1, APnDP, RnW, A2, A3, Parity, Stop=0, Park=1]
                            header_byte <= {1'b1, 1'b0, hdr_par, a23[1], a23[0], rnw, apndp, 1'b1};
                            bit_idx     <= 6'd0;
                            state       <= ST_HEADER;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 1: 8-bit Header Shift Out
                // -------------------------------------------------------------
                ST_HEADER: begin
                    if (tick) begin
                        if (!phase) begin
                            // Falling edge: set SWCLK LOW, drive data bit
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0; // Host output
                            swdio_o <= header_byte[bit_idx];
                            phase   <= 1'b1;
                        end else begin
                            // Rising edge: set SWCLK HIGH (Target samples on rising edge)
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (bit_idx == 6'd7) begin
                                state   <= ST_TRN1;
                                bit_idx <= 6'd0;
                            end else begin
                                bit_idx <= bit_idx + 1'b1;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 2: Turnaround 1 (Host releases SWDIO to input)
                // -------------------------------------------------------------
                ST_TRN1: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Tristate SWDIO (Target will drive)
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;
                            state   <= ST_ACK;
                            bit_idx <= 6'd0;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 3: Sample 3-bit ACK from Target (LSB First)
                // -------------------------------------------------------------
                ST_ACK: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Input
                            phase   <= 1'b1;
                        end else begin
                            // Target drove on rising edge or earlier; sample at end of HIGH phase
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;
                            target_ack[bit_idx[1:0]] <= swdio_i;

                            if (bit_idx == 6'd2) begin
                                bit_idx <= 6'd0;
                                // Check ACK on bit 2 sample
                                if ({swdio_i, target_ack[1], target_ack[0]} == 3'b001) begin
                                    // ACK == OK (1)
                                    if (rnw) begin
                                        // Read transfer: Target keeps driving data
                                        state <= ST_RDATA;
                                    end else begin
                                        // Write transfer: Turnaround to host driving
                                        state <= ST_TRN2;
                                    end
                                end else begin
                                    // ACK is WAIT, FAULT, or timeout: abort to idle cycles
                                    state    <= ST_TRN_READ_END;
                                    idle_cnt <= 5'd0;
                                end
                            end else begin
                                bit_idx <= bit_idx + 1'b1;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 4A: Target -> Host 32-bit Read Data + 1 Parity Bit
                // -------------------------------------------------------------
                ST_RDATA: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Host reading
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (bit_idx < 6'd32) begin
                                shift_reg[bit_idx[4:0]] <= swdio_i;
                                bit_idx <= bit_idx + 1'b1;
                            end else begin
                                // 33rd bit is Parity
                                sample_parity <= swdio_i;
                                reg_rdata     <= shift_reg;
                                parity_err    <= (swdio_i != (^shift_reg)); // Even parity check
                                state         <= ST_TRN_READ_END;
                                bit_idx       <= 6'd0;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Read Turnaround (Host takes back SWDIO line)
                // -------------------------------------------------------------
                ST_TRN_READ_END: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0; // Host output
                            swdio_o <= 1'b0; // Drive low for upcoming idle cycles
                            phase   <= 1'b1;
                        end else begin
                            swclk_o  <= 1'b1;
                            phase    <= 1'b0;
                            state    <= ST_IDLE_CYCLES;
                            idle_cnt <= 5'd0;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 4B: Host Turnaround for Write
                // -------------------------------------------------------------
                ST_TRN2: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0; // Host drives
                            swdio_o <= reg_wdata[0];
                            phase   <= 1'b1;
                        end else begin
                            swclk_o   <= 1'b1;
                            phase     <= 1'b0;
                            state     <= ST_WDATA;
                            bit_idx   <= 6'd0;
                            shift_reg <= reg_wdata;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Host -> Target 32-bit Write Data + 1 Parity Bit
                // -------------------------------------------------------------
                ST_WDATA: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0;
                            if (bit_idx < 6'd32)
                                swdio_o <= shift_reg[bit_idx[4:0]];
                            else
                                swdio_o <= ^shift_reg; // 33rd bit: Even Parity
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (bit_idx == 6'd32) begin
                                state    <= ST_IDLE_CYCLES;
                                idle_cnt <= 5'd0;
                                bit_idx  <= 6'd0;
                            end else begin
                                bit_idx  <= bit_idx + 1'b1;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 5: Configurable Idle Clock Cycles (SWDIO = 0)
                // -------------------------------------------------------------
                ST_IDLE_CYCLES: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0;
                            swdio_o <= 1'b0; // Drive LOW for ADIv5 bus sync
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (idle_cnt >= (cfg_idle - 1'b1)) begin
                                swdio_o     <= 1'b1; // Idle HIGH
                                state       <= ST_IDLE;
                                fsm_busy    <= 1'b0;
                                sticky_done <= 1'b1;
                            end else begin
                                idle_cnt <= idle_cnt + 1'b1;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Raw Bit Sequence Shift (Arbitrary bit sequences, e.g. Reset)
                // -------------------------------------------------------------
                ST_RAW: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            if (reg_ctrl[16]) begin
                                // Raw Read
                                swdio_t <= 1'b1;
                            end else begin
                                // Raw Write
                                swdio_t <= 1'b0;
                                swdio_o <= shift_reg[bit_idx[4:0]];
                            end
                            phase <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (reg_ctrl[16]) begin
                                shift_reg[bit_idx[4:0]] <= swdio_i;
                            end

                            if (bit_idx >= (reg_raw_len - 1'b1)) begin
                                if (reg_ctrl[16]) reg_rdata <= shift_reg;
                                swdio_o     <= 1'b1;
                                state       <= ST_IDLE;
                                fsm_busy    <= 1'b0;
                                sticky_done <= 1'b1;
                            end else begin
                                bit_idx <= bit_idx + 1'b1;
                            end
                        end
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
