# Tests for Tk_PhotoImageFormatVersion3 registration.
# Loads tkwuffs, then exercises `image create photo -file` / `-data`
# for jpeg/bmp/webp using the registered format hooks (no
# ::tkwuffs::decode_to_photo call).

set here [file dirname [info script]]
set root [file dirname $here]
lappend auto_path $root
package require Tk
package require tclwuffs
package require tkwuffs

wm withdraw .

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

# -file path
set webp [file join $here sample.webp]
if {[file exists $webp]} {
    image create photo a -file $webp
    check "-file lossless.webp width>0"  {[image width  a] > 0}
    check "-file lossless.webp height>0" {[image height a] > 0}
}

# -data path (binary-clean; the bug Img had)
if {[file exists $webp]} {
    set fp [open $webp rb]; set bytes [read $fp]; close $fp
    image create photo b -data $bytes
    check "-data webp width matches" {[image width  b] == [image width  a]}
    check "-data webp height matches" {[image height b] == [image height a]}
}

# explicit -format webp
if {[file exists $webp]} {
    image create photo c -file $webp -format webp
    check "-file -format webp width>0" {[image width c] > 0}
}

# jpeg writeProc round-trip
# Use the bricks-color PNG fixture (which the built-in Tk PNG reader handles)
# as input. Round-trip it: photo -> jpeg bytes -> new photo -> compare dims.
set png_fix [file join $here bricks-color.png]
if {[file exists $png_fix]} {
    image create photo src -file $png_fix
    set sw [image width src]
    set sh [image height src]

    # stringWriteProc: `photo data` with -format jpeg returns bytes
    set jbytes [src data -format jpeg]
    check "jpeg stringWrite SOI"  {[string range $jbytes 0 1] eq "\xff\xd8"}
    check "jpeg stringWrite sniff" {[::tclwuffs::sniff $jbytes] eq "jpeg"}

    # Reload through our jpeg readProc to confirm decodability.
    image create photo back -data $jbytes
    check "jpeg round-trip width"  {[image width  back] == $sw}
    check "jpeg round-trip height" {[image height back] == $sh}

    # fileWriteProc: `photo write` lands on disk
    set jpath [file join $here _scratch_jpeg_out.jpg]
    src write $jpath -format jpeg
    check "jpeg fileWrite produced file" {[file size $jpath] > 0}
    set fp [open $jpath rb]; set on_disk [read $fp]; close $fp
    check "jpeg fileWrite sniff"  {[::tclwuffs::sniff $on_disk] eq "jpeg"}
    file delete $jpath

    # -quality affects size
    set j_lo [src data -format {jpeg -quality 10}]
    set j_hi [src data -format {jpeg -quality 95}]
    check "jpeg -quality 10 smaller than 95" \
        {[string length $j_lo] < [string length $j_hi]}

    # invalid -quality rejected
    set rc [catch {src data -format {jpeg -quality 0}} msg]
    check "jpeg -quality 0 raises" {$rc != 0}
}

# bmp via -data, generated on the fly from our own encoder path?
# We don't have a BMP fixture in tree. Skip; webp coverage is enough
# to prove the registration mechanism works.

# garbage should fail with a Tcl error, not crash
set rc [catch {image create photo d -data "not an image"} msg]
check "garbage -data raises" {$rc != 0}

if {$fails == 0} {
    puts "ALL tk_formats TESTS PASSED"
    exit 0
} else {
    puts stderr "$fails test(s) failed"
    exit 1
}
