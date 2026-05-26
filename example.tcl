#!/usr/bin/env wish9.0
# Usage: wish9.0 example.tcl <path-to-image>
#
# Loads the same image three ways to show all paths land in tkwuffs:
#   1. -file  $path                                       (file channel to wuffs)
#   2. -data  $raw_bytes                                  (binary bytes to wuffs)
#   3. -data  [binary decode base64 $b64]                 (caller decodes b64)
#
# Unlike img, we deliberately do NOT auto-decode base64 

package require Tk
lappend auto_path [file dirname [file normalize [info script]]]
package require tkwuffs

if {$argc != 1} {
    puts stderr "usage: [info script] <image-file>"
    exit 1
}

set path [lindex $argv 0]

image create photo p_file -file $path

set fp [open $path rb]; set bytes [read $fp]; close $fp
image create photo p_bin -data $bytes

set b64 [binary encode base64 $bytes]
image create photo p_b64 -data [binary decode base64 $b64]

wm title . "[file tail $path] — three paths"
foreach {p label} {
    p_file "-file"
    p_bin  "-data (binary)"
    p_b64  "-data (base64)"
} {
    set w [image width $p]
    set h [image height $p]
    pack [label .l_$p -image $p -text "$label : ${w}x${h}" \
            -compound top -padx 8 -pady 8] -side left
}
