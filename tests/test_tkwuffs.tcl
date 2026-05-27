# Smoke tests for the Tk tier. Run via `make test-tk` (needs wish9.0).

set here [file dirname [info script]]
set root [file dirname $here]
set auto_path [linsert $auto_path 0 $root]
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

# put_rgba round-trips raw RGBA into a photo.
image create photo praw -width 4 -height 1
set raw [binary format c* [list \
    255   0   0 255 \
      0 255   0 255 \
      0   0 255 255 \
    128 128 128 255]]
::tkwuffs::put_rgba praw 4 1 $raw
check "put_rgba photo width"  {[image width praw] == 4}
check "put_rgba photo height" {[image height praw] == 1}
check "put_rgba red pixel"    {[praw get 0 0] eq "255 0 0"}
check "put_rgba blue pixel"   {[praw get 2 0] eq "0 0 255"}

# Animation helper: animation/play/pause/restart lifecycle.
set gif_path [file join $here Stirling_Animation.gif]
if {[file exists $gif_path]} {
    set gfp [open $gif_path rb]; set gbytes [read $gfp]; close $gfp
    set baseline_imgs [image names]

    # animation: constructor creates the photo, does not auto-start.
    ::tkwuffs::animation a1 -data $gbytes
    check "animation creates photo if missing" \
        {[image type a1] eq "photo"}
    set inf [::tkwuffs::info a1]
    check "animation defaults to lazy mode"   {[dict get $inf mode] eq "lazy"}
    check "animation not playing on creation" {![dict get $inf playing]}
    check "animation reads source dims"       {[dict get $inf width] > 0
                                               && [dict get $inf height] > 0}

    # play / pause
    ::tkwuffs::play a1
    check "play starts ticking" {[dict get [::tkwuffs::info a1] playing]}
    ::tkwuffs::pause a1
    check "pause halts ticking" {![dict get [::tkwuffs::info a1] playing]}
    ::tkwuffs::play a1
    set iter_before [dict get [::tkwuffs::info a1] iter]
    ::tkwuffs::play a1
    check "play on running is a no-op" \
        {[dict get [::tkwuffs::info a1] iter] == $iter_before}
    ::tkwuffs::pause a1

    # restart: rewind, preserve play/pause state.
    ::tkwuffs::restart a1
    check "restart from paused stays paused" \
        {![dict get [::tkwuffs::info a1] playing]}
    check "restart resets iter" {[dict get [::tkwuffs::info a1] iter] == 0}
    ::tkwuffs::play a1
    ::tkwuffs::restart a1
    check "restart from playing stays playing" \
        {[dict get [::tkwuffs::info a1] playing]}

    # re-animation replaces source.
    ::tkwuffs::animation a1 -data $gbytes -cache eager
    check "re-animation switches to eager" \
        {[dict get [::tkwuffs::info a1] mode] eq "eager"}
    check "re-animation includes frame_count" \
        {[dict exists [::tkwuffs::info a1] frame_count]}

    # -file variant.
    ::tkwuffs::animation a2 -file $gif_path
    check "animation -file loads data" {[dict get [::tkwuffs::info a2] width] > 0}

    # bad opts.
    set rc [catch {::tkwuffs::animation pX -cache bogus -data $gbytes} msg]
    check "animation -cache bogus raises" {$rc != 0}
    set rc [catch {::tkwuffs::animation pX} msg]
    check "animation without -data/-file raises" {$rc != 0}
    set rc [catch {::tkwuffs::animation pX -data $gbytes -file $gif_path} msg]
    check "animation with both -data and -file raises" {$rc != 0}

    # play/pause/restart/info on uninitialized photo all raise.
    image create photo plain
    foreach cmd {play pause restart info} {
        set rc [catch [list ::tkwuffs::$cmd plain] msg]
        check "$cmd on un-animated photo raises" {$rc != 0}
    }
    image delete plain

    # Delete trace: deleting the parent photo reclaims eager-mode frame photos.
    set imgs_with_eager [image names]
    image delete a1
    check "image delete removes animation state" \
        {[catch {::tkwuffs::info a1}]}
    # All a1's private frame photos should be gone too.
    check "image delete reclaims eager frame photos" \
        {[llength [image names]] < [llength $imgs_with_eager]}
    image delete a2
    check "no leaked photos overall" \
        {[lsort [image names]] eq [lsort $baseline_imgs]}
}

if {$fails == 0} {
    puts "ALL tkwuffs TESTS PASSED"
    exit 0
} else {
    puts stderr "$fails test(s) failed"
    exit 1
}
