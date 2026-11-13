#
# build_programmer_bitstream.tcl - Antminer T9+ STM32 Programmer Bitstream Builder
#

set script_dir [file normalize [file dirname [info script]]]
set repo_root  [file normalize [file join $script_dir ".."]]
set proj_file  [file normalize [file join $repo_root "build_programmer" "t9_stm32_programmer.xpr"]]

if {[file exists $proj_file]} {
    puts "==> Opening existing project '$proj_file'..."
    open_project $proj_file
} else {
    source [file join $script_dir "create_programmer_project.tcl"]
}

# Add top-level Verilog wrapper
puts "==> Adding top-level Verilog wrapper..."
set top_file [file normalize [file join $script_dir "top_programmer.v"]]
add_files -norecurse $top_file
set_property top top_programmer [current_fileset]
update_compile_order -fileset sources_1

# Add constraints file
puts "==> Adding programmer constraints..."
set xdc_file [file normalize [file join $repo_root "constraints" "t9_programmer.xdc"]]
add_files -fileset constrs_1 -norecurse $xdc_file
set_property target_constrs_file $xdc_file [current_fileset -constrset]

# Launch Synthesis
puts "==> Launching Synthesis (synth_1)..."
reset_run synth_1 -quiet
launch_runs synth_1 -jobs 4
wait_on_run synth_1

if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "ERROR: Synthesis failed. Check synthesis log."
    exit 1
}

# Launch Implementation & Generate Bitstream
puts "==> Launching Implementation & Bitstream Generation (impl_1)..."
reset_run impl_1 -quiet
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "ERROR: Implementation failed. Check implementation log."
    exit 1
}

# Locate output bitstream
set impl_dir [get_property DIRECTORY [get_runs impl_1]]
set bit_file [file normalize [file join $impl_dir "top_programmer.bit"]]
set hwh_file [file normalize [file join $repo_root "build_programmer" "t9_stm32_programmer.gen" "sources_1" "bd" "design_programmer" "hw_handoff" "design_programmer.hwh"]]

set dest_bit [file normalize [file join $repo_root "t9_programmer.bit"]]
set dest_hwh [file normalize [file join $repo_root "t9_programmer.hwh"]]

if {[file exists $bit_file]} {
    file copy -force $bit_file $dest_bit
    if {[file exists $hwh_file]} {
        file copy -force $hwh_file $dest_hwh
    }
    puts ""
    puts "=========================================================================="
    puts " SUCCESS: t9_programmer.bit created successfully! ([file size $dest_bit] bytes)"
    puts "=========================================================================="
} else {
    puts "ERROR: Bitstream file was not created."
    exit 1
}
