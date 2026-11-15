##
## t9_programmer.xdc - Master Constraints for STM32 SWD Programmer & Dual UART Pod
## Target Board: Antminer T9+ Control Board V1.2 (Zynq XC7Z010-1CLG400C)
## All IOs are 3.3V LVCMOS on Banks 34 & 35
##

##############################################################################
# J3 Header: STM32 SWD & Dual UART Port (3.3V Bank 34/35)
##############################################################################
# Pin 3: SWCLK
set_property PACKAGE_PIN P16 [get_ports {j3_swclk}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_swclk}]
set_property SLEW SLOW [get_ports {j3_swclk}]
set_property DRIVE 8 [get_ports {j3_swclk}]

# Pin 4: SWDIO (Bidirectional with pullup)
set_property PACKAGE_PIN P15 [get_ports {j3_swdio}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_swdio}]
set_property PULLUP true [get_ports {j3_swdio}]
set_property SLEW SLOW [get_ports {j3_swdio}]
set_property DRIVE 8 [get_ports {j3_swdio}]

# Pin 5: nRST (Target Reset - Open-Drain with pullup)
set_property PACKAGE_PIN T14 [get_ports {j3_nrst}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_nrst}]
set_property PULLUP true [get_ports {j3_nrst}]

# Pin 7: Primary Target UART0 TX (Output to Target RX)
set_property PACKAGE_PIN C20 [get_ports {j3_uart0_tx}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_uart0_tx}]

# Pin 8: Primary Target UART0 RX (Input from Target TX)
set_property PACKAGE_PIN B20 [get_ports {j3_uart0_rx}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_uart0_rx}]
set_property PULLUP true [get_ports {j3_uart0_rx}]

# Pin 11: Secondary Target UART1 TX (Output to Target RX2)
set_property PACKAGE_PIN P14 [get_ports {j3_uart1_tx}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_uart1_tx}]

# Pin 12: Secondary Target UART1 RX (Input from Target TX2)
set_property PACKAGE_PIN R14 [get_ports {j3_uart1_rx}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_uart1_rx}]
set_property PULLUP true [get_ports {j3_uart1_rx}]

##############################################################################
# J3 Header: Configurable Target I/O Pins
##############################################################################
# Pin 15: TARGET_IO0
set_property PACKAGE_PIN T15 [get_ports {j3_target_io0}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_target_io0}]

# Pin 17: TARGET_IO1
set_property PACKAGE_PIN T17 [get_ports {j3_target_io1}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_target_io1}]

# Pin 18: TARGET_IO2
set_property PACKAGE_PIN R18 [get_ports {j3_target_io2}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_target_io2}]

# Pin 21: TARGET_IO3
set_property PACKAGE_PIN M14 [get_ports {j3_target_io3}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_target_io3}]

# Pin 22: TARGET_IO4
set_property PACKAGE_PIN M15 [get_ports {j3_target_io4}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_target_io4}]

##############################################################################
# J4 Header: Target Pod #2 (ST-LINK/V2) SWD Interface (3.3V Bank 34/35)
##############################################################################
# Pin 3: SWCLK1
set_property PACKAGE_PIN V18 [get_ports {j4_swclk}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_swclk}]
set_property SLEW SLOW [get_ports {j4_swclk}]
set_property DRIVE 8 [get_ports {j4_swclk}]

# Pin 4: SWDIO1 (Bidirectional with pullup)
set_property PACKAGE_PIN V17 [get_ports {j4_swdio}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_swdio}]
set_property PULLUP true [get_ports {j4_swdio}]
set_property SLEW SLOW [get_ports {j4_swdio}]
set_property DRIVE 8 [get_ports {j4_swdio}]

# Pin 5: nRST1 (Target 2 Reset - Open-Drain with pullup)
set_property PACKAGE_PIN Y16 [get_ports {j4_nrst}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_nrst}]
set_property PULLUP true [get_ports {j4_nrst}]

# Pin 6: SWO1 (Target 2 Trace Input)
set_property PACKAGE_PIN T16 [get_ports {j4_swo}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_swo}]
set_property PULLUP true [get_ports {j4_swo}]

# Pin 7: TARGET_IO8
set_property PACKAGE_PIN E17 [get_ports {j4_target_io8}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io8}]

# Pin 8: TARGET_IO9
set_property PACKAGE_PIN D18 [get_ports {j4_target_io9}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io9}]

##############################################################################
# PL LEDs - Kept solid OFF (Bank 35)
##############################################################################
set_property PACKAGE_PIN F16 [get_ports {led_d7}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d7}]

set_property PACKAGE_PIN M17 [get_ports {led_d6}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d6}]

set_property PACKAGE_PIN M19 [get_ports {led_d5}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d5}]

set_property PACKAGE_PIN L19 [get_ports {led_d8}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d8}]

##############################################################################
# Bitstream Generation Options
##############################################################################
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 50 [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
