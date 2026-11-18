`timescale 1ns / 1ps
// One outstanding read/write. AW and W are accepted independently.
// The register file emits one rd_valid response for each rd_en pulse.
module axil_register_slave #(
    parameter ADDR_WIDTH = 6
) (
    input wire clk, resetn,
    input wire [ADDR_WIDTH-1:0] awaddr,
    input wire awvalid,
    output wire awready,
    input wire [31:0] wdata,
    input wire [3:0] wstrb,
    input wire wvalid,
    output wire wready,
    output reg [1:0] bresp,
    output reg bvalid,
    input wire bready,
    input wire [ADDR_WIDTH-1:0] araddr,
    input wire arvalid,
    output wire arready,
    output reg [31:0] rdata,
    output reg [1:0] rresp,
    output reg rvalid,
    input wire rready,
    output wire wr_en,
    output wire [ADDR_WIDTH-1:0] wr_addr,
    output wire [31:0] wr_data,
    output wire [3:0] wr_strb,
    output wire rd_en,
    output wire [ADDR_WIDTH-1:0] rd_addr,
    input wire [31:0] rd_data,
    input wire rd_valid
);
    reg aw_full, w_full, read_pending;
    reg [ADDR_WIDTH-1:0] saved_aw;
    reg [31:0] saved_wdata;
    reg [3:0] saved_wstrb;
    assign awready = !aw_full && !bvalid;
    assign wready = !w_full && !bvalid;
    assign wr_en = aw_full && w_full && !bvalid;
    assign wr_addr = saved_aw;
    assign wr_data = saved_wdata;
    assign wr_strb = saved_wstrb;
    assign arready = !read_pending && !rvalid;
    assign rd_en = arready && arvalid;
    assign rd_addr = araddr;

    always @(posedge clk) begin
        if (!resetn) begin
            aw_full <= 0; w_full <= 0; read_pending <= 0;
            saved_aw <= 0; saved_wdata <= 0; saved_wstrb <= 0;
            bvalid <= 0; bresp <= 0; rvalid <= 0; rresp <= 0; rdata <= 0;
        end else begin
            if (awready && awvalid) begin saved_aw <= awaddr; aw_full <= 1; end
            if (wready && wvalid) begin
                saved_wdata <= wdata; saved_wstrb <= wstrb; w_full <= 1;
            end
            if (wr_en) begin
                aw_full <= 0; w_full <= 0; bvalid <= 1; bresp <= 0;
            end else if (bvalid && bready) bvalid <= 0;
            if (rd_en) read_pending <= 1;
            if (read_pending && rd_valid) begin
                rdata <= rd_data; rresp <= 0; rvalid <= 1; read_pending <= 0;
            end else if (rvalid && rready) rvalid <= 0;
        end
    end
endmodule
