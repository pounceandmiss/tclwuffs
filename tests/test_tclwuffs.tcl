# Smoke tests for the no-Tk tier. Run via `make test-tcl`.

set here [file dirname [info script]]
set root [file dirname $here]
lappend auto_path $root
package require tclwuffs

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

# sniff
set fmt [::tclwuffs::sniff $bytes]
check "sniff PNG"          {$fmt eq "png"}
check "sniff empty=\"\""  {[::tclwuffs::sniff ""] eq ""}
set garbage [string repeat \x00 32]
check "sniff garbage=\"\"" {[::tclwuffs::sniff $garbage] eq ""}

# decode
set d [::tclwuffs::decode $bytes]
set w [dict get $d width]
set h [dict get $d height]
set pix [dict get $d pixels]
check "decode width=160"   {$w == 160}
check "decode height=120"  {$h == 120}
check "decode pixels size" {[string length $pix] == $w*$h*4}

# encode_png round-trip
set png [::tclwuffs::encode_png $w $h $pix]
check "encoded has PNG signature" \
    {[string range $png 0 7] eq "\x89PNG\r\n\x1a\n"}
set d2 [::tclwuffs::decode $png]
check "round-trip width matches"  {[dict get $d2 width] == $w}
check "round-trip height matches" {[dict get $d2 height] == $h}
check "round-trip pixels identical" {[dict get $d2 pixels] eq $pix}

# encode_jpeg
set jpg [::tclwuffs::encode_jpeg $w $h $pix]
check "encoded has JPEG SOI"     {[string range $jpg 0 1] eq "\xff\xd8"}
check "sniff JPEG"               {[::tclwuffs::sniff $jpg] eq "jpeg"}
set dj [::tclwuffs::decode $jpg]
check "JPEG round-trip width"    {[dict get $dj width] == $w}
check "JPEG round-trip height"   {[dict get $dj height] == $h}

set jpg_lo [::tclwuffs::encode_jpeg $w $h $pix -quality 20]
set jpg_hi [::tclwuffs::encode_jpeg $w $h $pix -quality 95]
check "lower quality is smaller" {[string length $jpg_lo] < [string length $jpg_hi]}

set rc [catch {::tclwuffs::encode_jpeg $w $h $pix -quality 0} msg]
check "encode_jpeg rejects quality=0" {$rc != 0}
check "encode_jpeg quality error category INVALID_INPUT" \
    {[lindex $::errorCode 1] eq "INVALID_INPUT"}

# resize_bytes
set r [::tclwuffs::resize_bytes $bytes 64 48]
check "resize_bytes is PNG" {[::tclwuffs::sniff $r] eq "png"}
set dr [::tclwuffs::decode $r]
check "resize_bytes new w" {[dict get $dr width] == 64}
check "resize_bytes new h" {[dict get $dr height] == 48}

set r2 [::tclwuffs::resize_bytes $bytes 32 32 -filter bicubic]
check "resize_bytes -filter accepted" {[::tclwuffs::sniff $r2] eq "png"}

set rj [::tclwuffs::resize_bytes $bytes 64 48 -format jpeg]
check "resize_bytes -format jpeg sniffs as jpeg" \
    {[::tclwuffs::sniff $rj] eq "jpeg"}
set drj [::tclwuffs::decode $rj]
check "resize+jpeg width"  {[dict get $drj width]  == 64}
check "resize+jpeg height" {[dict get $drj height] == 48}

set rj_lo [::tclwuffs::resize_bytes $bytes 64 48 -format jpeg -quality 20]
check "resize -quality 20 < default 90" \
    {[string length $rj_lo] < [string length $rj]}

set rc [catch {::tclwuffs::resize_bytes $bytes 32 32 -format bogus} msg]
check "resize_bytes -format bogus raises" {$rc != 0}

# crop_bytes
set c [::tclwuffs::crop_bytes $bytes 10 10 50 40]
set dc [::tclwuffs::decode $c]
check "crop_bytes new w" {[dict get $dc width] == 50}
check "crop_bytes new h" {[dict get $dc height] == 40}

set cj [::tclwuffs::crop_bytes $bytes 10 10 50 40 -format jpeg -quality 80]
check "crop_bytes -format jpeg sniffs as jpeg" \
    {[::tclwuffs::sniff $cj] eq "jpeg"}

# error paths
set rc [catch {::tclwuffs::decode "not an image"} msg]
check "decode garbage raises" {$rc != 0}
check "errorcode is TCLWUFFS UNSUPPORTED_FORMAT" \
    {[lrange $::errorCode 0 1] eq {TCLWUFFS UNSUPPORTED_FORMAT}}

set rc [catch {::tclwuffs::encode_png 10 10 [string repeat X 7]} msg]
check "encode_png with wrong pixel length raises" {$rc != 0}
check "encode_png error category INVALID_INPUT" \
    {[lindex $::errorCode 1] eq "INVALID_INPUT"}

set rc [catch {::tclwuffs::crop_bytes $bytes 0 0 10000 10000} msg]
check "crop out-of-bounds raises" {$rc != 0}

if {$fails == 0} {
    puts "ALL tclwuffs TESTS PASSED"
    exit 0
} else {
    puts stderr "$fails test(s) failed"
    exit 1
}
