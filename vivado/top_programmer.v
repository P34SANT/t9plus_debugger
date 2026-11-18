//
// top_programmer.v - Top-Level Wrapper for Antminer T9+ STM32 Programmer & Dual UART Pod
// Target: Xilinx Zynq XC7Z010-1CLG400C (Antminer T9+ Control Board V1.2)
// All target signals routed to J3 and J4 expansion headers.
//
`timescale 1ns / 1ps

module top_programmer (
    // Fixed DDR and PS I/O
    inout  wire [14:0] DDR_0_addr,
    inout  wire [2:0]  DDR_0_ba,
    inout  wire        DDR_0_cas_n,
    inout  wire        DDR_0_ck_n,
    inout  wire        DDR_0_ck_p,
    inout  wire        DDR_0_cke,
    inout  wire        DDR_0_cs_n,
    inout  wire [3:0]  DDR_0_dm,
    inout  wire [31:0] DDR_0_dq,
    inout  wire [3:0]  DDR_0_dqs_n,
    inout  wire [3:0]  DDR_0_dqs_p,
    inout  wire        DDR_0_odt,
    inout  wire        DDR_0_ras_n,
    inout  wire        DDR_0_reset_n,
    inout  wire        DDR_0_we_n,
    inout  wire        FIXED_IO_0_ddr_vrn,
    inout  wire        FIXED_IO_0_ddr_vrp,
    inout  wire [53:0] FIXED_IO_0_mio,
    inout  wire        FIXED_IO_0_ps_clk,
    inout  wire        FIXED_IO_0_ps_porb,
    inout  wire        FIXED_IO_0_ps_srstb,

    // J3 Header: STM32 SWD & Primary UART Interface (3.3V Bank 34/35)
    inout wire         j3_swclk,       // J3 Pin 3 (P16), tri-stated on disconnect
    inout  wire        j3_swdio,       // J3 Pin 4  (P15) - STM32 SWDIO
    inout  wire        j3_nrst,        // J3 Pin 5  (T14) - STM32 nRST (Reset)
    inout  wire        j3_swo,         // J3 Pin 6  (T10) - Target 1 SWO Trace Input / BOOT0
    output wire        j3_uart0_tx,    // J3 Pin 7  (C20) - Primary UART TX (To STM32 RX)
    input  wire        j3_uart0_rx,    // J3 Pin 8  (B20) - Primary UART RX (From STM32 TX)

    // Secondary UART on J3 Header (3.3V Bank 34)
    output wire        j3_uart1_tx,    // J3 Pin 11 (P14) - Secondary UART TX
    input  wire        j3_uart1_rx,    // J3 Pin 12 (R14) - Secondary UART RX

    // J3 Header: Configurable Target I/O Pins (3.3V Bank 34)
    inout  wire        j3_target_io0,  // J3 Pin 15 (T15)
    inout  wire        j3_target_io1,  // J3 Pin 17 (T17)
    inout  wire        j3_target_io2,  // J3 Pin 18 (R18)
    inout  wire        j3_target_io3,  // J3 Pin 21 (M14)
    inout  wire        j3_target_io4,  // J3 Pin 22 (M15)

    // J4 Header: Pod #2 (ST-LINK/V2) SWD & Target Interface (3.3V Bank 34/35)
    inout wire         j4_swclk,       // J4 Pin 3 (V18), tri-stated on disconnect
    inout  wire        j4_swdio,       // J4 Pin 4  (V17) - Target 2 SWDIO
    inout  wire        j4_nrst,        // J4 Pin 5  (Y16) - Target 2 nRST (Reset)
    input  wire        j4_swo,         // J4 Pin 6  (T16) - Target 2 SWO Trace Input
    inout  wire        j4_target_io8,  // J4 Pin 7  (E17) - Auxiliary Target 2 I/O
    inout  wire        j4_target_io9,  // J4 Pin 8  (D18) - Auxiliary Target 2 I/O

    // PL User LEDs - Kept solid OFF (3.3V Bank 35, Active-High 1'b0 = OFF)
    output wire        led_d7,         // LED7 (F16)
    output wire        led_d6,         // D6 Green (M17)
    output wire        led_d5,         // D5 Red (M19)
    output wire        led_d8          // LED8 (L19)
);

    // Internal wires connecting to Block Design
    wire        clk_100mhz;
    wire [15:0] emio_gpio_o;
    wire [15:0] emio_gpio_i;
    wire [15:0] emio_gpio_t;
    wire        ps_uart0_tx_int;
    wire        ps_uart0_rx_int;
    wire        pl_uart1_tx_int;
    wire        pl_uart1_rx_int;

    // Connect Block Design instance
    design_programmer design_inst (
        .DDR_0_addr(DDR_0_addr),
        .DDR_0_ba(DDR_0_ba),
        .DDR_0_cas_n(DDR_0_cas_n),
        .DDR_0_ck_n(DDR_0_ck_n),
        .DDR_0_ck_p(DDR_0_ck_p),
        .DDR_0_cke(DDR_0_cke),
        .DDR_0_cs_n(DDR_0_cs_n),
        .DDR_0_dm(DDR_0_dm),
        .DDR_0_dq(DDR_0_dq),
        .DDR_0_dqs_n(DDR_0_dqs_n),
        .DDR_0_dqs_p(DDR_0_dqs_p),
        .DDR_0_odt(DDR_0_odt),
        .DDR_0_ras_n(DDR_0_ras_n),
        .DDR_0_reset_n(DDR_0_reset_n),
        .DDR_0_we_n(DDR_0_we_n),
        .FIXED_IO_0_ddr_vrn(FIXED_IO_0_ddr_vrn),
        .FIXED_IO_0_ddr_vrp(FIXED_IO_0_ddr_vrp),
        .FIXED_IO_0_mio(FIXED_IO_0_mio),
        .FIXED_IO_0_ps_clk(FIXED_IO_0_ps_clk),
        .FIXED_IO_0_ps_porb(FIXED_IO_0_ps_porb),
        .FIXED_IO_0_ps_srstb(FIXED_IO_0_ps_srstb),

        .clk_100mhz(clk_100mhz),
        .emio_gpio_o(emio_gpio_o),
        .emio_gpio_i(emio_gpio_i),
        .emio_gpio_t(emio_gpio_t),

        .ps_uart0_tx(ps_uart0_tx_int),
        .ps_uart0_rx(ps_uart0_rx_int),

        .pl_uart1_tx(pl_uart1_tx_int),
        .pl_uart1_rx(pl_uart1_rx_int),

        .swd_hw_en_0(swd_hw_en_0),
        .swd_released_0(swd_released_0),
        .hw_swclk_o_0(hw_swclk_o_0),
        .hw_swdio_o_0(hw_swdio_o_0),
        .hw_swdio_t_0(hw_swdio_t_0),
        .hw_swdio_i_0(hw_swdio_i_0),

        .swd_hw_en_1(swd_hw_en_1),
        .swd_released_1(swd_released_1),
        .hw_swclk_o_1(hw_swclk_o_1),
        .hw_swdio_o_1(hw_swdio_o_1),
        .hw_swdio_t_1(hw_swdio_t_1),
        .hw_swdio_i_1(hw_swdio_i_1),

        .swo_in_0(pin_j3_swo_i),
        .swo_in_1(j4_swo)
    );

    // =========================================================================
    // Target Pod #1 SWD Interface (Header J3):
    // J3 Pin 3 (P16): SWCLK0
    // J3 Pin 4 (P15): SWDIO0 (Bidirectional IOBUF)
    // J3 Pin 5 (T14): nRST0  (Bidirectional Open-Drain IOBUF)
    // J3 Pin 6 (T10): SWO0 / BOOT0 (Bidirectional IOBUF)
    // =========================================================================
    wire swd_hw_en_0;
    wire swd_released_0;
    wire hw_swclk_o_0;
    wire hw_swdio_o_0;
    wire hw_swdio_t_0;
    wire hw_swdio_i_0;
    wire pin_swdio_i_0;
    wire pin_j3_swo_i;

    assign hw_swdio_i_0   = pin_swdio_i_0;
    IOBUF iobuf_swclk_0 (
        .IO(j3_swclk), .O(emio_gpio_i[0]), .I(swd_hw_en_0 ? hw_swclk_o_0 : emio_gpio_o[0]),
        .T(swd_hw_en_0 ? swd_released_0 : emio_gpio_t[0])
    );

    IOBUF iobuf_swdio_0 (
        .IO(j3_swdio),
        .O (pin_swdio_i_0),
        .I (swd_hw_en_0 ? hw_swdio_o_0 : emio_gpio_o[1]),
        .T (swd_hw_en_0 ? (swd_released_0 | hw_swdio_t_0) : emio_gpio_t[1])
    );
    assign emio_gpio_i[1] = pin_swdio_i_0;

    IOBUF iobuf_nrst_0 (
        .IO(j3_nrst),
        .O (emio_gpio_i[2]),
        .I (emio_gpio_o[2]),
        .T (emio_gpio_t[2])
    );

    IOBUF iobuf_j3_pin6 (
        .IO(j3_swo),
        .O (pin_j3_swo_i),
        .I (emio_gpio_o[3]),
        .T (emio_gpio_t[3])
    );
    assign emio_gpio_i[3] = pin_j3_swo_i;

    // =========================================================================
    // Target Pod #2 SWD Interface (Header J4):
    // J4 Pin 3 (V18): SWCLK1 (Driven by axi_swd_1 @ 0x43C10000)
    // J4 Pin 4 (V17): SWDIO1 (Bidirectional IOBUF)
    // J4 Pin 5 (Y16): nRST1  (Bidirectional Open-Drain IOBUF)
    // =========================================================================
    wire swd_hw_en_1;
    wire swd_released_1;
    wire hw_swclk_o_1;
    wire hw_swdio_o_1;
    wire hw_swdio_t_1;
    wire hw_swdio_i_1;
    wire pin_swdio_i_1;

    assign hw_swdio_i_1   = pin_swdio_i_1;
    IOBUF iobuf_swclk_1 (
        .IO(j4_swclk), .O(emio_gpio_i[9]), .I(swd_hw_en_1 ? hw_swclk_o_1 : emio_gpio_o[9]),
        .T(swd_hw_en_1 ? swd_released_1 : emio_gpio_t[9])
    );

    IOBUF iobuf_swdio_1 (
        .IO(j4_swdio),
        .O (pin_swdio_i_1),
        .I (swd_hw_en_1 ? hw_swdio_o_1 : emio_gpio_o[10]),
        .T (swd_hw_en_1 ? (swd_released_1 | hw_swdio_t_1) : emio_gpio_t[10])
    );
    assign emio_gpio_i[10] = pin_swdio_i_1;

    IOBUF iobuf_nrst_1 (
        .IO(j4_nrst),
        .O (emio_gpio_i[11]),
        .I (emio_gpio_o[11]),
        .T (emio_gpio_t[11])
    );

    // =========================================================================
    // Configurable Target I/Os:
    // EMIO 4..8  (Linux Lines 58..62) -> J3 Pins 15, 17, 18, 21, 22
    // EMIO 12..13(Linux Lines 66..67) -> J4 Pins 7, 8
    // =========================================================================
    IOBUF iobuf_target0 (.IO(j3_target_io0), .O(emio_gpio_i[4]), .I(emio_gpio_o[4]), .T(emio_gpio_t[4]));
    IOBUF iobuf_target1 (.IO(j3_target_io1), .O(emio_gpio_i[5]), .I(emio_gpio_o[5]), .T(emio_gpio_t[5]));
    IOBUF iobuf_target2 (.IO(j3_target_io2), .O(emio_gpio_i[6]), .I(emio_gpio_o[6]), .T(emio_gpio_t[6]));
    IOBUF iobuf_target3 (.IO(j3_target_io3), .O(emio_gpio_i[7]), .I(emio_gpio_o[7]), .T(emio_gpio_t[7]));
    IOBUF iobuf_target4 (.IO(j3_target_io4), .O(emio_gpio_i[8]), .I(emio_gpio_o[8]), .T(emio_gpio_t[8]));

    IOBUF iobuf_target8 (.IO(j4_target_io8), .O(emio_gpio_i[12]), .I(emio_gpio_o[12]), .T(emio_gpio_t[12]));
    IOBUF iobuf_target9 (.IO(j4_target_io9), .O(emio_gpio_i[13]), .I(emio_gpio_o[13]), .T(emio_gpio_t[13]));

    assign emio_gpio_i[15:14] = 2'b00;

    // =========================================================================
    // UART Connections:
    // UART0 routed to J3 Pin 7 (TX) and J3 Pin 8 (RX)
    // UART1 routed to J3 Pin 11 (TX) and J3 Pin 12 (RX)
    // =========================================================================
    assign j3_uart0_tx     = ps_uart0_tx_int;
    assign ps_uart0_rx_int = j3_uart0_rx;

    assign j3_uart1_tx     = pl_uart1_tx_int;
    assign pl_uart1_rx_int = j3_uart1_rx;

    // =========================================================================
    // PL User LEDs: Solid OFF (Active-Low: 1'b1 = OFF)
    // =========================================================================
    assign led_d7 = 1'b1;
    assign led_d6 = 1'b1;
    assign led_d5 = 1'b1;
    assign led_d8 = 1'b1;

endmodule
