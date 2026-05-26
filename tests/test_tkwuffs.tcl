# Smoke tests for the Tk tier. Run via `make test-tk` (needs wish9.0).

set here [file dirname [info script]]
set root [file dirname $here]
lappend auto_path $root
package require Tk
package require tkwuffs

# Headless: don't try to map any toplevel.
wm withdraw .

set fixture [file join $here bricks-color.png]
if {![file exists $fixture]} {
    puts stderr "fixture not found: $fixture"
    exit 1
}
set fp [open $fixture rb]; set bytes [read $fp]; close $fp

set fails 0
proc check {label expr} {
    upvar fails fails
    if {[uplevel 1 [list expr $expr]]} {
        puts "ok   $label"
    } else {
        puts "FAIL $label"
        incr fails
    }
}

image create photo p
::tkwuffs::decode_to_photo $bytes p
check "decode_to_photo width"  {[image width p] == 160}
check "decode_to_photo height" {[image height p] == 120}

set png [::tkwuffs::encode_png_from_photo p]
check "encode_png_from_photo signature" \
    {[string range $png 0 7] eq "\x89PNG\r\n\x1a\n"}

image create photo p2
::tkwuffs::resize_photo p p2 64 48
check "resize_photo width"  {[image width p2] == 64}
check "resize_photo height" {[image height p2] == 48}

::tkwuffs::resize_photo p p2 32 32 -filter bicubic
check "resize_photo -filter applied" {[image width p2] == 32}

image create photo p3
::tkwuffs::crop_photo p p3 10 10 50 40
check "crop_photo width"  {[image width p3] == 50}
check "crop_photo height" {[image height p3] == 40}

set rc [catch {::tkwuffs::decode_to_photo "garbage" p} msg]
check "decode garbage raises" {$rc != 0}
check "errorcode UNSUPPORTED_FORMAT" \
    {[lindex $::errorCode 1] eq "UNSUPPORTED_FORMAT"}

if {$fails == 0} {
    puts "ALL tkwuffs TESTS PASSED"
    exit 0
} else {
    puts stderr "$fails test(s) failed"
    exit 1
}
