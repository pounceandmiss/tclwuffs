#!/usr/bin/env wish9.0
# Minimal tkwuffs animation demo. Run: wish9.0 example_animate.tcl

package require Tk
package require Ttk
ttk::style theme use clam

set auto_path [linsert $auto_path 0 [file dirname [file normalize [info script]]]]
package require tkwuffs

::tkwuffs::animation p -file [file join [file dirname [info script]] tests Stirling_Animation.gif]
::tkwuffs::play p

set meta [::tkwuffs::info p]
wm title . "tkwuffs animation"

# Tall image on the left, controls stacked on the right.
pack [ttk::label .img -image p] -side left -padx 12 -pady 12

set panel [ttk::frame .panel]
ttk::label  $panel.meta    -text "[dict get $meta width]x[dict get $meta height], [dict get $meta mode]"
ttk::button $panel.play    -text "Play"    -command [list ::tkwuffs::play    p]
ttk::button $panel.pause   -text "Pause"   -command [list ::tkwuffs::pause   p]
ttk::button $panel.restart -text "Restart" -command [list ::tkwuffs::restart p]
ttk::button $panel.quit    -text "Quit"    -command exit
pack $panel.meta -anchor w -pady {0 8}
pack $panel.play $panel.pause $panel.restart $panel.quit -fill x -pady {0 8}
pack $panel -side left -anchor n -padx {0 12} -pady 12
