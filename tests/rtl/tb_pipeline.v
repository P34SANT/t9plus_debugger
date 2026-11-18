`timescale 1ns/1ps
module tb_pipeline;
    reg clk = 0;
    always #5 clk = !clk;
    reg resetn = 0, select_swd = 0;
    reg [5:0] awaddr = 0, araddr = 0;
    reg [31:0] wdata = 0;
    reg [3:0] wstrb = 15;
    reg awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    wire awready, wready, bvalid, arready, rvalid;
    wire [31:0] rdata;
    wire aw0, w0, b0, ar0, r0, aw1, w1, b1, ar1, r1;
    wire [31:0] data0, data1;
    reg swo_input = 1;
    wire hw_enable, released, swclk, swdio_out, swdio_tri;
    reg [2:0] ack_value = 1;
    wire target_data = swd.state == 3 ? ack_value[swd.bit_idx] : 0;
    assign awready = select_swd ? aw1 : aw0;
    assign wready = select_swd ? w1 : w0;
    assign bvalid = select_swd ? b1 : b0;
    assign arready = select_swd ? ar1 : ar0;
    assign rvalid = select_swd ? r1 : r0;
    assign rdata = select_swd ? data1 : data0;
    axi_swo_engine #(.FIFO_DEPTH(16)) swo (
        .s_axi_aclk(clk), .s_axi_aresetn(resetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid && !select_swd),
        .s_axi_awready(aw0), .s_axi_wdata(wdata), .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid && !select_swd), .s_axi_wready(w0),
        .s_axi_bresp(), .s_axi_bvalid(b0), .s_axi_bready(bready && !select_swd),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid && !select_swd),
        .s_axi_arready(ar0), .s_axi_rdata(data0), .s_axi_rresp(),
        .s_axi_rvalid(r0), .s_axi_rready(rready && !select_swd),
        .swo_in_0(swo_input), .swo_in_1(1'b1)
    );
    axi_swd_engine swd (
        .s_axi_aclk(clk), .s_axi_aresetn(resetn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid && select_swd),
        .s_axi_awready(aw1), .s_axi_wdata(wdata), .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid && select_swd), .s_axi_wready(w1),
        .s_axi_bresp(), .s_axi_bvalid(b1), .s_axi_bready(bready && select_swd),
        .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid && select_swd),
        .s_axi_arready(ar1), .s_axi_rdata(data1), .s_axi_rresp(),
        .s_axi_rvalid(r1), .s_axi_rready(rready && select_swd),
        .swd_hw_en(hw_enable), .swd_released(released), .swclk_o(swclk),
        .swdio_o(swdio_out), .swdio_t(swdio_tri), .swdio_i(target_data)
    );
    task write_register;
        input [5:0] address;
        input [31:0] value;
        input integer gap;
        begin
            @(negedge clk); awaddr = address; awvalid = 1;
            while (!awready) @(negedge clk);
            @(negedge clk); awvalid = 0;
            repeat (gap) @(negedge clk);
            wdata = value; wvalid = 1;
            while (!wready) @(negedge clk);
            @(negedge clk); wvalid = 0;
            while (!bvalid) @(negedge clk);
            repeat (3) begin @(negedge clk); if (!bvalid) $fatal(1, "Lost write response"); end
            bready = 1; @(negedge clk); bready = 0;
        end
    endtask
    task read_register;
        input [5:0] address;
        input integer stall;
        output [31:0] value;
        reg [31:0] held;
        begin
            @(negedge clk); araddr = address; arvalid = 1;
            while (!arready) @(negedge clk);
            @(negedge clk); arvalid = 0;
            while (!rvalid) @(negedge clk);
            held = rdata;
            araddr = 8; arvalid = 1; // A second read must not be accepted while stalled.
            repeat (stall) begin
                @(negedge clk);
                if (!rvalid || rdata !== held || arready) $fatal(1, "Read backpressure violation");
            end
            arvalid = 0; value = rdata; rready = 1;
            @(negedge clk); rready = 0;
        end
    endtask
    task write_data_first;
        input [5:0] address;
        input [31:0] value;
        begin
            @(negedge clk); wdata = value; wvalid = 1;
            while (!wready) @(negedge clk);
            @(negedge clk); wvalid = 0;
            repeat (5) @(negedge clk);
            awaddr = address; awvalid = 1;
            while (!awready) @(negedge clk);
            @(negedge clk); awvalid = 0;
            while (!bvalid) @(negedge clk);
            bready = 1; @(negedge clk); bready = 0;
        end
    endtask
    task inject_byte;
        input [7:0] value;
        begin
            @(negedge clk);
            force swo.incoming = value;
            force swo.incoming_valid = 1;
            @(negedge clk); force swo.incoming_valid = 0;
            @(negedge clk); release swo.incoming_valid; release swo.incoming;
        end
    endtask
    task uart_byte;
        input [7:0] value;
        integer bitno;
        begin
            @(negedge clk); swo_input = 0;
            repeat (50) @(negedge clk);
            for (bitno = 0; bitno < 8; bitno = bitno + 1) begin
                swo_input = value[bitno];
                repeat (50) @(negedge clk);
            end
            swo_input = 1;
            repeat (60) @(negedge clk);
        end
    endtask
    reg [31:0] result;
    integer i, turn, data_phase, direction, ack_index;
    integer trn_first, trn_last, data_clocks, expected_data;
    always @(posedge swclk) begin
        if (swd.state == 2) trn_first = trn_first + 1;
        if (swd.state == 5 || swd.state == 6) trn_last = trn_last + 1;
        if (swd.state == 4 || swd.state == 7) data_clocks = data_clocks + 1;
    end
    initial begin
        #2000000; $fatal(1, "Simulation watchdog");
    end
    initial begin
        repeat (5) @(negedge clk); resetn = 1;
        write_data_first(4, 50);
        read_register(4, 3, result);
        if (result != 50) $fatal(1, "W-before-AW write failed");
        write_register(0, 3, 4);
        uart_byte(8'h55); uart_byte(8'hA5);
        read_register(8, 12, result);
        if (result !== 32'h155) $fatal(1, "UART first byte %h", result);
        read_register(8, 0, result);
        if (result !== 32'h1A5) $fatal(1, "UART second byte");
        read_register(20, 0, result);
        if (result != 0) $fatal(1, "Stalled AR popped extra FIFO bytes");
        for (i = 0; i < 16; i = i + 1) inject_byte(i);
        inject_byte(8'hEE);
        read_register(6'h2C, 0, result);
        if (result != 1) $fatal(1, "Overflow not counted");
        read_register(6'h20, 8, result);
        if (result !== 32'h03020100) $fatal(1, "Packed pop %h", result);
        read_register(20, 0, result);
        if (result != 12) $fatal(1, "Packed pop count");
        for (i = 16; i < 20; i = i + 1) inject_byte(i);
        read_register(8, 0, result);
        if (result != 32'h104) $fatal(1, "Wrapped FIFO");
        read_register(6'h20, 0, result);
        if (result !== 32'h08070605) $fatal(1, "Unaligned banked pop %h", result);
        // Full FIFO with a simultaneous accepted pop/push must stay full.
        write_register(0, 3, 0);
        for (i = 0; i < 16; i = i + 1) inject_byte(i);
        @(negedge clk); araddr = 8; arvalid = 1;
        force swo.incoming = 8'hFA; force swo.incoming_valid = 1;
        @(negedge clk); arvalid = 0; force swo.incoming_valid = 0;
        @(negedge clk); release swo.incoming; release swo.incoming_valid;
        while (!rvalid) @(negedge clk);
        if (rdata != 32'h100) $fatal(1, "Full read-first collision");
        rready = 1; @(negedge clk); rready = 0;
        read_register(20, 0, result);
        if (result != 16) $fatal(1, "Full simultaneous count");
        read_register(6'h2C, 0, result);
        if (result != 0) $fatal(1, "Accepted simultaneous push reported lost");
        select_swd = 1;
        read_register(24, 0, result);
        if (result != 32'h53574402 || !released) $fatal(1, "SWD identity/safe reset");
        write_register(4, 1, 3);
        write_register(8, 32'hA5, 2);
        write_register(20, 8, 0);
        write_register(0, 32'hC001, 0);
        repeat (100) @(negedge clk);
        read_register(28, 6, result);
        if (result != 1) $fatal(1, "Raw completion counter");
        write_register(0, 32'h8005, 0); // Read, zero idle cycles.
        repeat (250) @(negedge clk);
        read_register(16, 0, result);
        if (result[4:0] != 2) $fatal(1, "SWD read ACK/parity %h", result);
        read_register(28, 0, result);
        if (result != 2) $fatal(1, "Read completion counter");
        for (turn = 1; turn <= 4; turn = turn + 1)
            for (data_phase = 0; data_phase <= 1; data_phase = data_phase + 1)
                for (direction = 0; direction <= 1; direction = direction + 1)
                    for (ack_index = 0; ack_index < 3; ack_index = ack_index + 1) begin
                        ack_value = 1 << ack_index;
                        trn_first = 0; trn_last = 0; data_clocks = 0;
                        write_register(0, 32'h8001 | (direction << 2) |
                            ((turn - 1) << 18) | (data_phase << 20), 0);
                        repeat (400) @(negedge clk);
                        read_register(16, 0, result);
                        if (result[0] || result[3:1] != ack_value)
                            $fatal(1, "SWD configured ACK mismatch");
                        expected_data = (ack_value == 1 || data_phase) ? 33 : 0;
                        if (trn_first != turn || trn_last != turn || data_clocks != expected_data)
                            $fatal(1, "SWD phases: turn=%0d first=%0d last=%0d data=%0d expected=%0d",
                                turn, trn_first, trn_last, data_clocks, expected_data);
                    end
        write_register(0, 32'h28000, 0);
        if (!released) $fatal(1, "SWD did not release pins");
        $display("PASS: UART, banked FIFO, overflow, simultaneous push/pop, AXI stalls, SWD raw/read/release and 48 turnaround/data/ACK combinations");
        $finish;
    end
endmodule
