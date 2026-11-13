##
## t9_v12_pins.xdc - Master Vivado Constraints for Antminer T9+ Control Board V1.2
## Board: Bitmain Antminer T9+ (Ctrl_C92)
## SoC: Xilinx Zynq-7000 XC7Z010-1CLG400C
##
## Verified against opensatellite/antminer_t9 and Bitmain XC7010 V1.0/V1.2 Schematics
## All PL IO pins are 3.3V LVCMOS on Bank 34 and Bank 35.
##

##############################################################################
# Onboard User LEDs (3.3V Bank 35)
##############################################################################
# D5 Red LED (Channel 1, Bit 0)
set_property PACKAGE_PIN M19 [get_ports {leds_2bits[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds_2bits[0]}]

# D6 Green LED (Channel 1, Bit 1)
set_property PACKAGE_PIN M17 [get_ports {leds_2bits[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds_2bits[1]}]

# Additional PL User LEDs (Channel 2)
# LED7 (Bit 0)
set_property PACKAGE_PIN F16 [get_ports {leds_ext_2bits[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds_ext_2bits[0]}]

# LED8 (Bit 1)
set_property PACKAGE_PIN L19 [get_ports {leds_ext_2bits[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds_ext_2bits[1]}]

##############################################################################
# Onboard Oscillator Clocks (Bank 34, 3.3V)
##############################################################################
# Y2 TCXO -> R49 -> L12P Bank 34 K17
set_property PACKAGE_PIN K17 [get_ports {clk_y2}]
set_property IOSTANDARD LVCMOS33 [get_ports {clk_y2}]

# Y3 TCXO -> R51 -> L13P Bank 34 H16
set_property PACKAGE_PIN H16 [get_ports {clk_y3}]
set_property IOSTANDARD LVCMOS33 [get_ports {clk_y3}]

# Uncomment below to define timing clocks in Vivado:
# create_clock -period 20.000 -name clk_y2 [get_ports {clk_y2}]
# create_clock -period 20.000 -name clk_y3 [get_ports {clk_y3}]

##############################################################################
# Fan Header GPIO (Bank 34, 3.3V)
##############################################################################
# FAN6
set_property PACKAGE_PIN H20 [get_ports {fan6_pin3}]
set_property PACKAGE_PIN J18 [get_ports {fan6_pin4}]
# FAN5
set_property PACKAGE_PIN G17 [get_ports {fan5_pin3}]
# FAN4
set_property PACKAGE_PIN J20 [get_ports {fan4_pin3}]
# FAN3
set_property PACKAGE_PIN F20 [get_ports {fan3_pin3}]

set_property IOSTANDARD LVCMOS33 [get_ports {fan*}]

##############################################################################
# Hashboard Expansion Headers J1-J9 (3.3V Bank 34 & 35)
##############################################################################
# Each header is an 18-pin dual-row 2.54mm header:
# Pin 1, 2: GND
# Remaining pins carry differential pair signals from Banks 34 and 35.

## J8 Header (Bank 34)
set_property PACKAGE_PIN W19 [get_ports {j8_pin3_l22n}]
set_property PACKAGE_PIN W18 [get_ports {j8_pin4_l22p}]
set_property PACKAGE_PIN Y18 [get_ports {j8_pin5_l17p}]
set_property PACKAGE_PIN V16 [get_ports {j8_pin11_l18p}]
set_property PACKAGE_PIN W16 [get_ports {j8_pin12_l18n}]
set_property PACKAGE_PIN Y19 [get_ports {j8_pin15_l17n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j8_*}]

## J7 Header (Bank 34)
set_property PACKAGE_PIN W19 [get_ports {j7_pin3_l22n}]
set_property PACKAGE_PIN W18 [get_ports {j7_pin4_l22p}]
set_property PACKAGE_PIN T20 [get_ports {j7_pin5_l15p}]
set_property PACKAGE_PIN V20 [get_ports {j7_pin11_l16p}]
set_property PACKAGE_PIN W20 [get_ports {j7_pin12_l16n}]
set_property PACKAGE_PIN U20 [get_ports {j7_pin15_l15n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j7_*}]

## J6 Header (Bank 34)
set_property PACKAGE_PIN W19 [get_ports {j6_pin3_l22n}]
set_property PACKAGE_PIN W18 [get_ports {j6_pin4_l22p}]
set_property PACKAGE_PIN U14 [get_ports {j6_pin5_l11p}]
set_property PACKAGE_PIN U18 [get_ports {j6_pin11_l12p}]
set_property PACKAGE_PIN U19 [get_ports {j6_pin12_l12n}]
set_property PACKAGE_PIN U15 [get_ports {j6_pin15_l11n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j6_*}]

## J4 Header (Bank 34 + 35)
set_property PACKAGE_PIN V18 [get_ports {j4_pin3_l21n}]
set_property PACKAGE_PIN V17 [get_ports {j4_pin4_l21p}]
set_property PACKAGE_PIN Y16 [get_ports {j4_pin5_l7p}]
set_property PACKAGE_PIN T16 [get_ports {j4_pin6_l9p}]
set_property PACKAGE_PIN E17 [get_ports {j4_pin7_l3p}]
set_property PACKAGE_PIN D18 [get_ports {j4_pin8_l3n}]
set_property PACKAGE_PIN W14 [get_ports {j4_pin11_l8p}]
set_property PACKAGE_PIN Y14 [get_ports {j4_pin12_l8n}]
set_property PACKAGE_PIN Y17 [get_ports {j4_pin15_l7n}]
set_property PACKAGE_PIN B19 [get_ports {j4_pin17_l2p}]
set_property PACKAGE_PIN B20 [get_ports {j4_pin18_l2n}]
set_property PACKAGE_PIN K16 [get_ports {j4_pin21_l24p}]
set_property PACKAGE_PIN J16 [get_ports {j4_pin22_l24n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j4_*}]

## J3 Header (Bank 34 + 35)
set_property PACKAGE_PIN P16 [get_ports {j3_pin3_l24n}]
set_property PACKAGE_PIN P15 [get_ports {j3_pin4_l24p}]
set_property PACKAGE_PIN T14 [get_ports {j3_pin5_l5p}]
set_property PACKAGE_PIN T10 [get_ports {j3_pin6_l1n}]
set_property PACKAGE_PIN C20 [get_ports {j3_pin7_l1p}]
set_property PACKAGE_PIN B20 [get_ports {j3_pin8_l1n}]
set_property PACKAGE_PIN P14 [get_ports {j3_pin11_l6p}]
set_property PACKAGE_PIN R14 [get_ports {j3_pin12_l6n}]
set_property PACKAGE_PIN T15 [get_ports {j3_pin15_l5n}]
set_property PACKAGE_PIN T17 [get_ports {j3_pin17_l20p}]
set_property PACKAGE_PIN R18 [get_ports {j3_pin18_l20n}]
set_property PACKAGE_PIN M14 [get_ports {j3_pin21_l23p}]
set_property PACKAGE_PIN M15 [get_ports {j3_pin22_l23n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j3_*}]

## J2 Header (Bank 34 + 35)
set_property PACKAGE_PIN P18 [get_ports {j2_pin3_l23n}]
set_property PACKAGE_PIN N17 [get_ports {j2_pin4_l23p}]
set_property PACKAGE_PIN U17 [get_ports {j2_pin5_l9p}]
set_property PACKAGE_PIN T11 [get_ports {j2_pin6_l1p}]
set_property PACKAGE_PIN V15 [get_ports {j2_pin7_l10p}]
set_property PACKAGE_PIN W15 [get_ports {j2_pin8_l10n}]
set_property PACKAGE_PIN V12 [get_ports {j2_pin11_l4p}]
set_property PACKAGE_PIN W13 [get_ports {j2_pin12_l4n}]
set_property PACKAGE_PIN V13 [get_ports {j2_pin15_l3n}]
set_property PACKAGE_PIN T12 [get_ports {j2_pin17_l2p}]
set_property PACKAGE_PIN U12 [get_ports {j2_pin18_l2n}]
set_property PACKAGE_PIN L14 [get_ports {j2_pin21_l22p}]
set_property PACKAGE_PIN L15 [get_ports {j2_pin22_l22n}]
set_property IOSTANDARD LVCMOS33 [get_ports {j2_*}]

##############################################################################
# Vivado Bitstream Configuration
##############################################################################
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 50 [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
