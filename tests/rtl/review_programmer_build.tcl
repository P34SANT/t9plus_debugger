# Offline post-route review. Does not connect to hardware.
set root [file normalize [lindex $argv 0]]
set checkpoint [file join $root build_programmer t9_stm32_programmer.runs impl_1 top_programmer_routed.dcp]
open_checkpoint $checkpoint
report_timing_summary -warn_on_violation -file plink_timing_summary.rpt
report_utilization -file plink_utilization.rpt
report_drc -file plink_drc.rpt
set worst [get_timing_paths -delay_type max -max_paths 1]
if {[llength $worst] == 0} { error "No internal timing paths found" }
set slack [get_property SLACK $worst]
puts "INTERNAL_WNS_NS=$slack"
if {$slack < 0} { error "Internal setup timing failed" }
set hold [get_timing_paths -delay_type min -max_paths 1]
if {[llength $hold] != 0 && [get_property SLACK $hold] < 0} {
    error "Internal hold timing failed"
}
puts "PASS: internal routed timing; external target timing still requires board/cable data"
