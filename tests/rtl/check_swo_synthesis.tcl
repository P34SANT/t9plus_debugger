set root [lindex $argv 0]
read_verilog [file join $root vivado axil_register_slave.v]
read_verilog [file join $root vivado axi_swo_engine.v]
synth_design -top axi_swo_engine -part xc7z010clg400-1
set brams [get_cells -hier -filter {REF_NAME =~ RAMB*}]
if {[llength $brams] == 0} { error "SWO FIFO did not infer BRAM" }
report_utilization -file swo_v2_utilization.rpt
puts "PASS: SWO v2 inferred [llength $brams] BRAM primitives"
