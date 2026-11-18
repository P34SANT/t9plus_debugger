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
    output wire                              s_axi_awready,

    // AXI4-Lite Write Data Channel
    input  wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  wire                              s_axi_wvalid,
    output wire                              s_axi_wready,

    // AXI4-Lite Write Response Channel
    output wire [1:0]                        s_axi_bresp,
    output wire                              s_axi_bvalid,
    input  wire                              s_axi_bready,

    // AXI4-Lite Read Address Channel
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
    input  wire [2:0]                        s_axi_arprot,
    input  wire                              s_axi_arvalid,
    output wire                              s_axi_arready,

    // AXI4-Lite Read Data Channel
    output wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
    output wire [1:0]                        s_axi_rresp,
    output wire                              s_axi_rvalid,
    input  wire                              s_axi_rready,

    // Physical SWD Interface to Top-Level IOBUFs
    output wire                              swd_hw_en,
    output wire                              swd_released,
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
    reg [31:0] completed;

    assign swd_hw_en = reg_ctrl[15]; // Bit 15 enables hardware multiplexer
    assign swd_released = reg_ctrl[17];

    // =========================================================================
    // AXI4-Lite Slave Logic
    // =========================================================================
    reg fsm_start_pulse;
    wire wr_en, rd_en;
    wire [C_S_AXI_ADDR_WIDTH-1:0] wr_addr, rd_addr;
    wire [31:0] wr_data;
    wire [3:0] wr_strb;
    reg [31:0] read_data;
    reg read_valid;
    axil_register_slave #(.ADDR_WIDTH(C_S_AXI_ADDR_WIDTH)) bus (
        .clk(s_axi_aclk), .resetn(s_axi_aresetn),
        .awaddr(s_axi_awaddr), .awvalid(s_axi_awvalid), .awready(s_axi_awready),
        .wdata(s_axi_wdata), .wstrb(s_axi_wstrb), .wvalid(s_axi_wvalid), .wready(s_axi_wready),
        .bresp(s_axi_bresp), .bvalid(s_axi_bvalid), .bready(s_axi_bready),
        .araddr(s_axi_araddr), .arvalid(s_axi_arvalid), .arready(s_axi_arready),
        .rdata(s_axi_rdata), .rresp(s_axi_rresp), .rvalid(s_axi_rvalid), .rready(s_axi_rready),
        .wr_en(wr_en), .wr_addr(wr_addr), .wr_data(wr_data), .wr_strb(wr_strb),
        .rd_en(rd_en), .rd_addr(rd_addr), .rd_data(read_data), .rd_valid(read_valid)
    );
    function [31:0] merge_bytes;
        input [31:0] old_value, new_value;
        input [3:0] strobe;
        integer i;
        begin
            merge_bytes = old_value;
            for (i = 0; i < 4; i = i + 1)
                if (strobe[i]) merge_bytes[i*8 +: 8] = new_value[i*8 +: 8];
        end
    endfunction

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            reg_ctrl        <= 32'h00020800; // Released, idle cycles = 8
            reg_clk_div     <= 16'd49;       // Default: 1.0 MHz (100MHz / (2 * 50))
            reg_wdata       <= 32'd0;
            reg_raw_len     <= 6'd32;
            fsm_start_pulse <= 1'b0;
        end else begin
            fsm_start_pulse <= 1'b0; // Default 1-cycle strobe

            if (wr_en) begin
                case (wr_addr[5:0])
                    ADDR_CTRL: begin
                        if (!fsm_busy || (wr_strb[2] && wr_data[17]))
                            reg_ctrl <= merge_bytes(reg_ctrl, wr_data, wr_strb);
                        if (wr_strb[0] && wr_data[0] && !fsm_busy) begin
                            fsm_start_pulse <= 1'b1;
                        end
                    end
                    ADDR_CLK_DIV: if (!fsm_busy) reg_clk_div <= merge_bytes({16'd0,reg_clk_div}, wr_data, wr_strb);
                    ADDR_WDATA: if (!fsm_busy) reg_wdata <= merge_bytes(reg_wdata, wr_data, wr_strb);
                    ADDR_RAW_LEN: if (!fsm_busy && wr_strb[0] && wr_data[5:0] >= 1 && wr_data[5:0] <= 32)
                                      reg_raw_len <= wr_data[5:0];
                    default: ;
                endcase
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
            read_valid <= 0;
            read_data <= 0;
        end else begin
            read_valid <= rd_en;
            if (rd_en) begin
                case (rd_addr[5:0])
                    ADDR_CTRL:    read_data <= reg_ctrl;
                    ADDR_CLK_DIV: read_data <= {16'd0, reg_clk_div};
                    ADDR_WDATA:   read_data <= reg_wdata;
                    ADDR_RDATA:   read_data <= reg_rdata;
                    ADDR_STATUS:  read_data <= {16'h5357, 10'd0, sticky_done, parity_err, target_ack, fsm_busy};
                    ADDR_RAW_LEN: read_data <= {26'd0, reg_raw_len};
                    ADDR_ID:      read_data <= 32'h53574402;
                    6'h1C:        read_data <= completed;
                    default:      read_data <= 32'd0;
                endcase
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
    reg [2:0] turn_count;

    wire apndp     = reg_ctrl[1];
    wire rnw       = reg_ctrl[2];
    wire [1:0] a23 = reg_ctrl[4:3];
    wire hdr_par   = apndp ^ rnw ^ a23[0] ^ a23[1]; // Even parity of request fields
    wire [4:0] cfg_idle = reg_ctrl[12:8];
    wire [2:0] cfg_turnaround = {1'b0, reg_ctrl[19:18]} + 1;
    wire cfg_data_phase = reg_ctrl[20];

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            state         <= ST_IDLE;
            fsm_busy      <= 1'b0;
            sticky_done   <= 1'b0;
            target_ack    <= 3'b000;
            parity_err    <= 1'b0;
            swclk_o       <= 1'b1; // Idle clock is HIGH
            swdio_o       <= 1'b0; // Idle data is LOW
            swdio_t       <= 1'b0; // Driven
            phase         <= 1'b0;
            bit_idx       <= 6'd0;
            idle_cnt      <= 5'd0;
            header_byte   <= 8'd0;
            shift_reg     <= 32'd0;
            reg_rdata     <= 32'd0;
            sample_parity <= 1'b0;
            turn_count <= 0;
            completed <= 0;
        end else begin
            case (state)
                ST_IDLE: begin
                    swclk_o     <= 1'b1;
                    swdio_o     <= 1'b0;
                    swdio_t     <= 1'b0;
                    phase       <= 1'b0;
                    fsm_busy    <= 1'b0;

                    if (fsm_start_pulse) begin
                        fsm_busy    <= 1'b1;
                        sticky_done <= 1'b0;
                        parity_err  <= 1'b0;
                        target_ack  <= 3'b111; // Default to timeout
                        turn_count <= 0;

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
                            swdio_t <= 1'b1; // Tristate SWDIO (Host releases line)
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1; // Rising edge: Target assumes control
                            phase   <= 1'b0;
                            if (turn_count + 1 >= cfg_turnaround) begin
                                state <= ST_ACK; bit_idx <= 0; turn_count <= 0;
                            end else turn_count <= turn_count + 1;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 3: Sample 3-bit ACK from Target (LSB First)
                // -------------------------------------------------------------
                ST_ACK: begin
                    if (tick) begin
                        if (!phase) begin
                            // Clock LOW: Target output has settled from previous rising edge. Sample SWDIO here!
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Input
                            target_ack[bit_idx[1:0]] <= swdio_i;
                            phase   <= 1'b1;
                        end else begin
                            // Clock HIGH: Target prepares next bit
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (bit_idx == 6'd2) begin
                                bit_idx <= 6'd0;
                                // target_ack was captured during LOW phase
                                if (target_ack == 3'b001) begin
                                    // ACK == OK (1)
                                    if (rnw) begin
                                        // Read transfer: Target keeps driving data immediately
                                        state <= ST_RDATA;
                                    end else begin
                                        // Write transfer: Turnaround so host can drive
                                        state     <= ST_TRN2;
                                        shift_reg <= reg_wdata;
                                    end
                                end else begin
                                    if (cfg_data_phase && (target_ack == 2 || target_ack == 4)) begin
                                        if (rnw) state <= ST_RDATA; /* discard invalid data */
                                        else begin state <= ST_TRN2; shift_reg <= 0; end
                                    end else state <= ST_TRN_READ_END;
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
                            // Clock LOW: Target data is stable. Sample here!
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Host reading
                            if (bit_idx < 6'd32) begin
                                shift_reg[bit_idx[4:0]] <= swdio_i;
                            end else begin
                                sample_parity <= swdio_i;
                            end
                            phase <= 1'b1;
                        end else begin
                            // Clock HIGH: Target shifts next bit
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (bit_idx == 6'd32) begin
                                // 33rd bit was Parity, captured during LOW phase
                                reg_rdata  <= shift_reg;
                                parity_err <= (sample_parity != (^shift_reg)); // Even parity check
                                state      <= ST_TRN_READ_END;
                                bit_idx    <= 6'd0;
                                idle_cnt   <= 5'd0;
                            end else begin
                                bit_idx <= bit_idx + 1'b1;
                            end
                        end
                    end
                end

                // -------------------------------------------------------------
                // Read Turnaround (Line transitions Target -> Host)
                // -------------------------------------------------------------
                ST_TRN_READ_END: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Tristate during turnaround cycle!
                            phase   <= 1'b1;
                        end else begin
                            swclk_o  <= 1'b1; // Target releases bus
                            phase    <= 1'b0;
                            if (turn_count + 1 >= cfg_turnaround) begin
                                state <= ST_IDLE_CYCLES; idle_cnt <= 0; turn_count <= 0;
                            end else turn_count <= turn_count + 1;
                        end
                    end
                end

                // -------------------------------------------------------------
                // Phase 4B: Host Turnaround for Write (Line transitions Target -> Host)
                // -------------------------------------------------------------
                ST_TRN2: begin
                    if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b1; // Tristate during turnaround cycle!
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1; // Target releases bus
                            phase   <= 1'b0;
                            if (turn_count + 1 >= cfg_turnaround) begin
                                state <= ST_WDATA; bit_idx <= 0; turn_count <= 0;
                            end else turn_count <= turn_count + 1;
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
                    if (cfg_idle == 0) begin
                        swdio_o <= 0; swdio_t <= 0;
                        state <= ST_IDLE; fsm_busy <= 0; sticky_done <= 1;
                        completed <= completed + 1;
                    end else if (tick) begin
                        if (!phase) begin
                            swclk_o <= 1'b0;
                            swdio_t <= 1'b0;
                            swdio_o <= 1'b0; // Drive LOW for ADIv5 bus sync
                            phase   <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (cfg_idle == 5'd0 || idle_cnt >= (cfg_idle - 1'b1)) begin
                                swdio_o     <= 1'b0; // Retain LOW
                                state       <= ST_IDLE;
                                fsm_busy    <= 1'b0;
                                sticky_done <= 1'b1;
                                completed <= completed + 1;
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
                                // Raw Read: sample while clock is LOW
                                swdio_t <= 1'b1;
                                shift_reg[bit_idx[4:0]] <= swdio_i;
                            end else begin
                                // Raw Write
                                swdio_t <= 1'b0;
                                swdio_o <= shift_reg[bit_idx[4:0]];
                            end
                            phase <= 1'b1;
                        end else begin
                            swclk_o <= 1'b1;
                            phase   <= 1'b0;

                            if (reg_raw_len == 6'd0 || bit_idx >= (reg_raw_len - 1'b1)) begin
                                if (reg_ctrl[16]) reg_rdata <= shift_reg;
                                state       <= ST_IDLE;
                                fsm_busy    <= 1'b0;
                                sticky_done <= 1'b1;
                                completed <= completed + 1;
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
