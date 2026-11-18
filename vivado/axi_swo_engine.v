`timescale 1ns / 1ps
// SWO v2: synchronous banked BRAM FIFO, atomic packed pops, loss telemetry.
module axi_swo_engine #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6,
    parameter integer FIFO_DEPTH = 32768
) (
    input wire s_axi_aclk, s_axi_aresetn,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_awaddr,
    input wire [2:0] s_axi_awprot,
    input wire s_axi_awvalid,
    output wire s_axi_awready,
    input wire [31:0] s_axi_wdata,
    input wire [3:0] s_axi_wstrb,
    input wire s_axi_wvalid,
    output wire s_axi_wready,
    output wire [1:0] s_axi_bresp,
    output wire s_axi_bvalid,
    input wire s_axi_bready,
    input wire [C_S_AXI_ADDR_WIDTH-1:0] s_axi_araddr,
    input wire [2:0] s_axi_arprot,
    input wire s_axi_arvalid,
    output wire s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0] s_axi_rresp,
    output wire s_axi_rvalid,
    input wire s_axi_rready,
    input wire swo_in_0, swo_in_1
);
    localparam PTR_W = $clog2(FIFO_DEPTH);
    localparam BANK_W = PTR_W - 2;
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
    reg enabled, channel, invert;
    reg [31:0] divisor;
    wire flush = wr_en && wr_addr == 0 && wr_strb[0] && wr_data[1];
    integer lane;
    reg [31:0] next_divisor;
    always @* begin
        next_divisor = divisor;
        for (lane = 0; lane < 4; lane = lane + 1)
            if (wr_strb[lane]) next_divisor[lane*8 +: 8] = wr_data[lane*8 +: 8];
    end
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin enabled <= 0; channel <= 0; invert <= 0; divisor <= 50; end
        else if (wr_en) begin
            if (wr_addr == 0 && wr_strb[0]) begin
                enabled <= wr_data[0]; channel <= wr_data[2]; invert <= wr_data[3];
            end
            if (wr_addr == 4 && next_divisor >= 10) divisor <= next_divisor;
        end
    end

    (* ASYNC_REG = "TRUE" *) reg [2:0] synchronizer;
    always @(posedge s_axi_aclk)
        if (!s_axi_aresetn) synchronizer <= 3'b111;
        else synchronizer <= {synchronizer[1:0], (channel ? swo_in_1 : swo_in_0) ^ invert};
    wire falling = synchronizer[2] && !synchronizer[1];
    reg [1:0] rx_state;
    reg [31:0] ticks;
    reg [2:0] bit_index;
    reg [7:0] shift, incoming;
    reg incoming_valid, framing;
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn || flush || !enabled) begin
            rx_state <= 0; ticks <= 0; bit_index <= 0; shift <= 0;
            incoming <= 0; incoming_valid <= 0; framing <= 0;
        end else begin
            incoming_valid <= 0; framing <= 0;
            case (rx_state)
                0: if (falling) begin rx_state <= 1; ticks <= 0; end
                1: if (ticks >= (divisor >> 1) - 1) begin
                    ticks <= 0;
                    if (!synchronizer[1]) begin rx_state <= 2; bit_index <= 0; end
                    else rx_state <= 0;
                end else ticks <= ticks + 1;
                2: if (ticks >= divisor - 1) begin
                    ticks <= 0; shift[bit_index] <= synchronizer[1];
                    if (bit_index == 7) rx_state <= 3;
                    else bit_index <= bit_index + 1;
                end else ticks <= ticks + 1;
                3: if (ticks >= divisor - 1) begin
                    ticks <= 0; rx_state <= 0;
                    if (synchronizer[1]) begin incoming <= shift; incoming_valid <= 1; end
                    else framing <= 1;
                end else ticks <= ticks + 1;
            endcase
        end
    end

    (* ram_style = "block" *) reg [7:0] bank0 [0:FIFO_DEPTH/4-1];
    (* ram_style = "block" *) reg [7:0] bank1 [0:FIFO_DEPTH/4-1];
    (* ram_style = "block" *) reg [7:0] bank2 [0:FIFO_DEPTH/4-1];
    (* ram_style = "block" *) reg [7:0] bank3 [0:FIFO_DEPTH/4-1];
    reg [PTR_W-1:0] write_ptr, read_ptr;
    reg [PTR_W:0] count, high_water;
    reg overflow;
    reg [31:0] total, dropped, frame_errors, edges;
    wire pop_request = rd_en && (rd_addr == 6'h08 || rd_addr == 6'h20);
    wire [2:0] requested = rd_addr == 6'h20 ? 4 : 1;
    wire [2:0] pop_count = pop_request ? (count < requested ? count[2:0] : requested) : 0;
    wire push = incoming_valid && (count < FIFO_DEPTH || pop_count != 0);
    wire [PTR_W:0] next_count = count + (push ? 1 : 0) - pop_count;
    reg [7:0] b0, b1, b2, b3;
    wire [BANK_W-1:0] base_index = read_ptr[PTR_W-1:2];
    wire [BANK_W-1:0] index0 = base_index + (read_ptr[1:0] > 0);
    wire [BANK_W-1:0] index1 = base_index + (read_ptr[1:0] > 1);
    wire [BANK_W-1:0] index2 = base_index + (read_ptr[1:0] > 2);
    wire [BANK_W-1:0] index3 = base_index;
    always @(posedge s_axi_aclk) begin
        if (push && !flush && s_axi_aresetn) begin
            case (write_ptr[1:0])
                0: bank0[write_ptr[PTR_W-1:2]] <= incoming;
                1: bank1[write_ptr[PTR_W-1:2]] <= incoming;
                2: bank2[write_ptr[PTR_W-1:2]] <= incoming;
                3: bank3[write_ptr[PTR_W-1:2]] <= incoming;
            endcase
        end
        if (rd_en) begin
            b0 <= bank0[index0]; b1 <= bank1[index1];
            b2 <= bank2[index2]; b3 <= bank3[index3];
        end
    end
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn || flush) begin
            write_ptr <= 0; read_ptr <= 0; count <= 0; high_water <= 0;
            overflow <= 0; total <= 0; dropped <= 0; frame_errors <= 0; edges <= 0;
        end else begin
            count <= next_count;
            if (push) write_ptr <= write_ptr + 1;
            if (pop_count != 0) read_ptr <= read_ptr + pop_count;
            if (next_count > high_water) high_water <= next_count;
            if (incoming_valid) total <= total + 1;
            if (incoming_valid && !push) begin dropped <= dropped + 1; overflow <= 1; end
            if (framing) frame_errors <= frame_errors + 1;
            if (enabled && falling) edges <= edges + 1;
        end
    end
    reg pending_read;
    reg [5:0] saved_addr;
    reg [1:0] saved_align;
    reg [2:0] saved_valid, last_pop_count;
    reg [31:0] packed_word;
    always @* case (saved_align)
        0: packed_word = {b3,b2,b1,b0};
        1: packed_word = {b0,b3,b2,b1};
        2: packed_word = {b1,b0,b3,b2};
        3: packed_word = {b2,b1,b0,b3};
    endcase
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            pending_read <= 0; read_valid <= 0; read_data <= 0;
            saved_addr <= 0; saved_align <= 0; saved_valid <= 0; last_pop_count <= 0;
        end else begin
            pending_read <= rd_en;
            read_valid <= pending_read;
            if (rd_en) begin
                saved_addr <= rd_addr; saved_align <= read_ptr[1:0];
                saved_valid <= (rd_addr == 6'h0C) ? (count != 0) : pop_count;
            end
            if (pending_read) begin
                case (saved_addr)
                    6'h00: read_data <= {28'd0, invert, channel, 1'b0, enabled};
                    6'h04: read_data <= divisor;
                    6'h08, 6'h0C: read_data <= saved_valid ? {23'd0,1'b1,packed_word[7:0]} : 0;
                    6'h10: read_data <= {28'd0, count == FIFO_DEPTH, count == 0, overflow, enabled};
                    6'h14: read_data <= count;
                    6'h18: read_data <= 32'h53574F02;
                    6'h1C: read_data <= total;
                    6'h20: begin read_data <= packed_word; last_pop_count <= saved_valid; end
                    6'h24: read_data <= last_pop_count;
                    6'h28: read_data <= high_water;
                    6'h2C: read_data <= dropped;
                    6'h30: read_data <= frame_errors;
                    6'h34: read_data <= edges;
                    6'h38: read_data <= {29'd0, rx_state, synchronizer[1]};
                    6'h3C: read_data <= FIFO_DEPTH;
                    default: read_data <= 0;
                endcase
            end
        end
    end
endmodule
