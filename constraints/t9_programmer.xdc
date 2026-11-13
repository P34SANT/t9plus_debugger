##
## t9_programmer.xdc - Master Constraints for STM32 SWD Programmer & Dual UART Pod
## Target Board: Antminer T9+ Control Board V1.2 (Zynq XC7Z010-1CLG400C)
## All IOs are 3.3V LVCMOS on Banks 34 & 35
##

##############################################################################
# Fan Headers: STM32 SWD & Primary UART Interface (3.3V Bank 34/35)
##############################################################################
# SWCLK: Fan Pin 4 (Shared across FAN3-FAN6, J18)
set_property PACKAGE_PIN J18 [get_ports {fan_swclk}]
set_property IOSTANDARD LVCMOS33 [get_ports {fan_swclk}]
set_property SLEW FAST [get_ports {fan_swclk}]

# SWDIO: FAN3 Pin 3 (F20) - Bidirectional with pull-up
set_property PACKAGE_PIN F20 [get_ports {fan_swdio}]
set_property IOSTANDARD LVCMOS33 [get_ports {fan_swdio}]
set_property PULLUP true [get_ports {fan_swdio}]
set_property SLEW FAST [get_ports {fan_swdio}]

# nRST: FAN4 Pin 3 (J20) - Target Reset (Open-Drain with pull-up)
set_property PACKAGE_PIN J20 [get_ports {fan_nrst}]
set_property IOSTANDARD LVCMOS33 [get_ports {fan_nrst}]
set_property PULLUP true [get_ports {fan_nrst}]

# Primary UART0 TX: FAN5 Pin 3 (G17) - Output to Target RX
set_property PACKAGE_PIN G17 [get_ports {fan_uart0_tx}]
set_property IOSTANDARD LVCMOS33 [get_ports {fan_uart0_tx}]

# Primary UART0 RX: FAN6 Pin 3 (H20) - Input from Target TX
set_property PACKAGE_PIN H20 [get_ports {fan_uart0_rx}]
set_property IOSTANDARD LVCMOS33 [get_ports {fan_uart0_rx}]
set_property PULLUP true [get_ports {fan_uart0_rx}]

##############################################################################
# Secondary UART on J3 Header (Optional)
##############################################################################
# Pin 11: Secondary Target UART1 TX
set_property PACKAGE_PIN P14 [get_ports {j3_uart1_tx}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_uart1_tx}]

# Pin 12: Secondary Target UART1 RX
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
# J4 Header: Additional Configurable Target I/O Pins
##############################################################################
# Pin 3: TARGET_IO5
set_property PACKAGE_PIN V18 [get_ports {j4_target_io5}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io5}]

# Pin 4: TARGET_IO6
set_property PACKAGE_PIN V17 [get_ports {j4_target_io6}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io6}]

# Pin 5: TARGET_IO7
set_property PACKAGE_PIN Y16 [get_ports {j4_target_io7}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io7}]

# Pin 6: TARGET_IO8
set_property PACKAGE_PIN T16 [get_ports {j4_target_io8}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io8}]

# Pin 7: TARGET_IO9
set_property PACKAGE_PIN E17 [get_ports {j4_target_io9}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_target_io9}]

##############################################################################
# PL LEDs (Bank 35) - Forced OFF (Driven 1'b0)
##############################################################################
# LED7 (F16)
set_property PACKAGE_PIN F16 [get_ports {led_d7}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d7}]

# D6 Green (M17)
set_property PACKAGE_PIN M17 [get_ports {led_d6}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d6}]

# D5 Red (M19)
set_property PACKAGE_PIN M19 [get_ports {led_d5}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d5}]

# LED8 (L19)
set_property PACKAGE_PIN L19 [get_ports {led_d8}]
set_property IOSTANDARD LVCMOS33 [get_ports {led_d8}]

##############################################################################
# Bitstream Generation Options
##############################################################################
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 50 [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
