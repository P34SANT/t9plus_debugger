#
# create_programmer_project.tcl - Antminer T9+ STM32 SWD Programmer & Dual UART Devboard
# Target SoC: Xilinx Zynq-7000 XC7Z010-1CLG400C
#

set script_dir [file normalize [file dirname [info script]]]
set repo_root  [file normalize [file join $script_dir ".."]]
set proj_dir   [file normalize [file join $repo_root "build_programmer"]]
set proj_name  "t9_stm32_programmer"
set part_name  "xc7z010clg400-1"

puts "==> Creating Vivado Project '$proj_name' in '$proj_dir'..."
create_project -force $proj_name $proj_dir -part $part_name
set_property target_language Verilog [current_project]
set_property default_lib work [current_project]

# ----------------------------------------------------------------------------
# Create Block Design
# ----------------------------------------------------------------------------
puts "==> Creating Block Design 'design_programmer'..."
create_bd_design "design_programmer"

# Instantiate Processing System 7 (Zynq PS)
puts "==> Configuring Zynq PS7..."
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7 processing_system7_0]

set_property -dict [list \
  CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V} \
  CONFIG.PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V} \
  CONFIG.PCW_APU_PERIPHERAL_FREQMHZ {666.666666} \
  CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ {33.333333} \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.000000} \
  CONFIG.PCW_FPGA_FCLK0_ENABLE {1} \
  CONFIG.PCW_USE_M_AXI_GP0 {1} \
  CONFIG.PCW_UIPARAM_DDR_PARTNO {MT41K128M16 JT-125} \
  CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit} \
  CONFIG.PCW_UIPARAM_DDR_MEMORY_TYPE {DDR 3} \
  CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ {533.333333} \
  CONFIG.PCW_NAND_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_NAND_NAND_IO {MIO 0 2..14} \
  CONFIG.PCW_NAND_GRP_D8_ENABLE {0} \
  CONFIG.PCW_ENET0_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_ENET0_ENET0_IO {MIO 16 .. 27} \
  CONFIG.PCW_ENET0_GRP_MDIO_ENABLE {1} \
  CONFIG.PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53} \
  CONFIG.PCW_ENET_RESET_ENABLE {0} \
  CONFIG.PCW_SD0_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_SD0_SD0_IO {MIO 40 .. 45} \
  CONFIG.PCW_SD0_GRP_CD_ENABLE {0} \
  CONFIG.PCW_SD0_GRP_WP_ENABLE {0} \
  CONFIG.PCW_UART1_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_UART1_UART1_IO {MIO 48 .. 49} \
  CONFIG.PCW_GPIO_MIO_GPIO_ENABLE {1} \
  CONFIG.PCW_GPIO_MIO_GPIO_IO {MIO} \
  CONFIG.PCW_UART0_PERIPHERAL_ENABLE {1} \
  CONFIG.PCW_UART0_UART0_IO {EMIO} \
  CONFIG.PCW_GPIO_EMIO_GPIO_ENABLE {1} \
  CONFIG.PCW_GPIO_EMIO_GPIO_IO {16} \
] $ps7

# Export Fixed I/O and DDR Interfaces to Top Level
make_bd_intf_pins_external [get_bd_intf_pins processing_system7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins processing_system7_0/FIXED_IO]

# ----------------------------------------------------------------------------
# Instantiate Reset Controller & AXI Interconnect
# ----------------------------------------------------------------------------
puts "==> Instantiating Reset Controller and AXI Interconnect..."
set rst_gen [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_ps7_0_100M]
set axi_ic  [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect ps7_0_axi_periph]
set_property -dict [list CONFIG.NUM_MI {2} CONFIG.NUM_SI {1}] $axi_ic

# ----------------------------------------------------------------------------
# Instantiate AXI UARTLite for Secondary Target UART (Base 0x42C00000)
# ----------------------------------------------------------------------------
puts "==> Instantiating AXI UARTLite for Secondary Target UART..."
set axi_uart [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_uartlite axi_uartlite_0]
set_property -dict [list CONFIG.C_BAUDRATE {115200}] $axi_uart

# ----------------------------------------------------------------------------
# Instantiate AXI GPIO for Status & Diagnostic (Base 0x41200000)
# ----------------------------------------------------------------------------
puts "==> Instantiating AXI GPIO for Status & Diagnostic..."
set axi_gpio [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio axi_gpio_0]
set_property -dict [list CONFIG.C_GPIO_WIDTH {8} CONFIG.C_ALL_OUTPUTS {0} CONFIG.C_IS_DUAL {0}] $axi_gpio

# ----------------------------------------------------------------------------
# Connect Clocks and Resets
# ----------------------------------------------------------------------------
puts "==> Wiring Clocks and Resets..."
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] \
               [get_bd_pins processing_system7_0/M_AXI_GP0_ACLK] \
               [get_bd_pins rst_ps7_0_100M/slowest_sync_clk] \
               [get_bd_pins ps7_0_axi_periph/ACLK] \
               [get_bd_pins ps7_0_axi_periph/S00_ACLK] \
               [get_bd_pins ps7_0_axi_periph/M00_ACLK] \
               [get_bd_pins ps7_0_axi_periph/M01_ACLK] \
               [get_bd_pins axi_uartlite_0/s_axi_aclk] \
               [get_bd_pins axi_gpio_0/s_axi_aclk]

connect_bd_net [get_bd_pins processing_system7_0/FCLK_RESET0_N] [get_bd_pins rst_ps7_0_100M/ext_reset_in]

connect_bd_net [get_bd_pins rst_ps7_0_100M/interconnect_aresetn] [get_bd_pins ps7_0_axi_periph/ARESETN]
connect_bd_net [get_bd_pins rst_ps7_0_100M/peripheral_aresetn] \
               [get_bd_pins ps7_0_axi_periph/S00_ARESETN] \
               [get_bd_pins ps7_0_axi_periph/M00_ARESETN] \
               [get_bd_pins ps7_0_axi_periph/M01_ARESETN] \
               [get_bd_pins axi_uartlite_0/s_axi_aresetn] \
               [get_bd_pins axi_gpio_0/s_axi_aresetn]

# Connect AXI GP0 -> Interconnect -> Slaves
connect_bd_intf_net [get_bd_intf_pins processing_system7_0/M_AXI_GP0] [get_bd_intf_pins ps7_0_axi_periph/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M00_AXI] [get_bd_intf_pins axi_gpio_0/S_AXI]
connect_bd_intf_net [get_bd_intf_pins ps7_0_axi_periph/M01_AXI] [get_bd_intf_pins axi_uartlite_0/S_AXI]

# Assign Memory Addresses
assign_bd_address [get_bd_addr_segs {axi_gpio_0/S_AXI/Reg }]
set_property offset 0x41200000 [get_bd_addr_segs {processing_system7_0/Data/SEG_axi_gpio_0_Reg}]
set_property range 64K [get_bd_addr_segs {processing_system7_0/Data/SEG_axi_gpio_0_Reg}]

assign_bd_address [get_bd_addr_segs {axi_uartlite_0/S_AXI/Reg }]
set_property offset 0x42C00000 [get_bd_addr_segs {processing_system7_0/Data/SEG_axi_uartlite_0_Reg}]
set_property range 64K [get_bd_addr_segs {processing_system7_0/Data/SEG_axi_uartlite_0_Reg}]

# Make EMIO GPIO, PS UART0, AXI UART, and Clock external to be connected in top-level Verilog
create_bd_port -dir O clk_100mhz
connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_ports clk_100mhz]

create_bd_port -dir O -from 15 -to 0 emio_gpio_o
create_bd_port -dir I -from 15 -to 0 emio_gpio_i
create_bd_port -dir O -from 15 -to 0 emio_gpio_t
connect_bd_net [get_bd_pins processing_system7_0/GPIO_O] [get_bd_ports emio_gpio_o]
connect_bd_net [get_bd_pins processing_system7_0/GPIO_I] [get_bd_ports emio_gpio_i]
connect_bd_net [get_bd_pins processing_system7_0/GPIO_T] [get_bd_ports emio_gpio_t]

create_bd_port -dir O ps_uart0_tx
create_bd_port -dir I ps_uart0_rx
connect_bd_net [get_bd_pins processing_system7_0/UART0_TX] [get_bd_ports ps_uart0_tx]
connect_bd_net [get_bd_pins processing_system7_0/UART0_RX] [get_bd_ports ps_uart0_rx]

create_bd_port -dir O pl_uart1_tx
create_bd_port -dir I pl_uart1_rx
connect_bd_net [get_bd_pins axi_uartlite_0/tx] [get_bd_ports pl_uart1_tx]
connect_bd_net [get_bd_pins axi_uartlite_0/rx] [get_bd_ports pl_uart1_rx]

regenerate_bd_layout
validate_bd_design
save_bd_design

puts "==> Generating Block Design Output Products..."
generate_target all [get_files "design_programmer.bd"]
export_ip_user_files -of_objects [get_files "design_programmer.bd"] -no_script -sync -force -quiet

puts "==> Block design created successfully."
