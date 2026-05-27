# Smoke tests for the no-Tk tier. Run via `make test-tcl`.

set here [file dirname [info script]]
set root [file dirname $here]
set auto_path [linsert $auto_path 0 $root]
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

# decoder handle on a still PNG: info, one frame, then end-of-stream.
set dec [::tclwuffs::decoder $bytes]
set meta [$dec info]
check "decoder info has width"       {[dict get $meta width] == $w}
check "decoder info has loop_count"  {[dict exists $meta loop_count]}
set f0 [$dec next]
check "decoder first frame returned" {$f0 ne ""}
check "decoder pixels match decode"  {[dict get $f0 pixels] eq $pix}
check "decoder has delay_ms"         {[dict exists $f0 delay_ms]}
check "decoder end after one frame"  {[$dec next] eq ""}
# Idempotent end: subsequent calls keep returning empty.
check "decoder end is sticky"        {[$dec next] eq ""}
$dec restart
set f0b [$dec next]
check "decoder restart returns frame 0 again" \
    {$f0b ne "" && [dict get $f0b pixels] eq $pix}
$dec destroy
check "destroy removes the command"  {[llength [info commands $dec]] == 0}

# decoder handle on an animated GIF: many frames, plausible delays.
set gif_path [file join $here Stirling_Animation.gif]
if {[file exists $gif_path]} {
    set gfp [open $gif_path rb]; set gbytes [read $gfp]; close $gfp
    set dec [::tclwuffs::decoder $gbytes]
    set meta [$dec info]
    set gw [dict get $meta width]
    set gh [dict get $meta height]
    set count   0
    set bad_size  0
    set bad_delay 0
    while {[set f [$dec next]] ne ""} {
        incr count
        if {[string length [dict get $f pixels]] != $gw*$gh*4} {
            incr bad_size
        }
        if {[dict get $f delay_ms] < 20} { incr bad_delay }
    }
    check "GIF decoder yields multiple frames" {$count > 1}
    check "GIF frame pixels sized to W*H*4"    {$bad_size == 0}
    check "GIF delays all >= clamp floor"      {$bad_delay == 0}
    # Restart and confirm the same count.
    $dec restart
    set count2 0
    while {[$dec next] ne ""} { incr count2 }
    check "GIF restart yields same frame count" {$count2 == $count}
    $dec destroy
}

# Bad input: invalid bytes raise.
set rc [catch {::tclwuffs::decoder "not an image"} msg]
check "decoder on garbage raises" {$rc != 0}
check "decoder error is UNSUPPORTED_FORMAT" \
    {[lindex $::errorCode 1] eq "UNSUPPORTED_FORMAT"}

# Subcommand sanity.
set dec [::tclwuffs::decoder $bytes]
set rc [catch {$dec bogus} msg]
check "decoder bogus subcommand raises" {$rc != 0}
$dec destroy

if {$fails == 0} {
    puts "ALL tclwuffs TESTS PASSED"
    exit 0
} else {
    puts stderr "$fails test(s) failed"
    exit 1
}
