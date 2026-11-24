# OOC timing for the individual RS selector and 8x8 elastic return slot.
# vivado -mode batch -source sme_za_ab.tcl -tclargs <reports-dir> ?period-ns?

if {$argc < 1 || $argc > 2} {
    error "Provide a report directory and an optional clock period in ns"
}
set output [file normalize [lindex $argv 0]]
set period 4.0
if {$argc == 2} {
    set period [lindex $argv 1]
    if {![string is double -strict $period] || $period <= 0} {
        error "Clock period must be positive"
    }
}
file mkdir $output
set rtl [file normalize [file join [file dirname [info script]] .. rtl]]
set part xczu7ev-ffvc1156-2-e

foreach {name top segments} {
    issue-s2    sme_za_rs_issue     2
    issue-s3    sme_za_rs_issue     3
    return-slot sme_za_return_slot  3
} {
    create_project -in_memory -part $part
    read_verilog -sv [file join $rtl ${top}.sv]
    synth_design -top $top -mode out_of_context -generic SEGMENTS=$segments
    if {$top eq "sme_za_rs_issue"} {
        # Use a virtual launch/capture clock for the combinational selector.
        create_clock -name control -period $period
        set_input_delay  -clock control 0 [all_inputs]
        set_output_delay -clock control 0 [all_outputs]
    } else {
        create_clock -name control -period $period [get_ports clk]
        set_input_delay  -clock control 0 [remove_from_collection [all_inputs] [get_ports clk]]
        set_output_delay -clock control 0 [all_outputs]
    }
    opt_design
    place_design
    route_design
    report_utilization -file [file join $output ${name}-utilization.rpt]
    report_timing_summary -delay_type min_max -report_unconstrained \
        -file [file join $output ${name}-timing.rpt]
    close_project
}
