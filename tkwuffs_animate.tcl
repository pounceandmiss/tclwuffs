# Tk-photo animation helper. Loaded by pkgIndex.tcl after the .so.
# Public API: animation / play / pause / restart / info. See README.
# State lives on the photo; a command-delete trace on the photo cleans
# up when [image delete p] or [rename p ""] fires.

package require tclwuffs

namespace eval ::tkwuffs {
    # Per-photo state dict. Common keys: mode (lazy|eager), width, height,
    # loops (0 = forever), iter, onstop, after (pending id or "").
    # Lazy adds: bytes, decoder (created on first play).
    # Eager adds: frames (private photos), delays, index.
    variable _anims
    array set _anims {}

    proc _findphoto {name} {
        if {[catch {image type $name} t]} { return "" }
        if {$t ne "photo"} { return "" }
        return $name
    }

    proc animation {photoName args} {
        variable _anims

        set data ""
        set file ""
        set cache lazy
        set loops {}
        set onstop ""
        set i 0
        while {$i < [llength $args]} {
            set flag [lindex $args $i]
            if {$i + 1 >= [llength $args]} {
                return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                    "$flag requires a value"
            }
            set val [lindex $args [expr {$i + 1}]]
            switch -- $flag {
                -data   { set data $val }
                -file   { set file $val }
                -cache  { set cache $val }
                -loops  { set loops $val }
                -onstop { set onstop $val }
                default {
                    return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                        "unknown option: $flag"
                }
            }
            incr i 2
        }
        if {$data eq "" && $file eq ""} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "-data or -file is required"
        }
        if {$data ne "" && $file ne ""} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "use either -data or -file, not both"
        }
        if {$cache ni {lazy eager}} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "-cache must be lazy or eager (got \"$cache\")"
        }
        if {$file ne ""} {
            set fp [open $file rb]
            set data [read $fp]
            close $fp
        }

        set first_time 1
        if {[::info exists _anims($photoName)]} {
            _teardown $photoName
            set first_time 0
        }
        if {[_findphoto $photoName] eq ""} {
            image create photo $photoName
        }

        if {$cache eq "lazy"} {
            # Throwaway decoder for metadata; real one is built on first play.
            set probe [::tclwuffs::decoder $data]
            set meta [$probe info]
            $probe destroy
            set src_loops [dict get $meta loop_count]
            if {$loops eq ""} { set loops $src_loops }
            set _anims($photoName) [dict create \
                mode    lazy                    \
                bytes   $data                   \
                decoder ""                      \
                width   [dict get $meta width]  \
                height  [dict get $meta height] \
                loops   $loops                  \
                iter    0                       \
                onstop  $onstop                 \
                after   ""]
        } else {
            # Eager: drain the decoder into private photos now.
            set dec [::tclwuffs::decoder $data]
            set meta [$dec info]
            set src_loops [dict get $meta loop_count]
            if {$loops eq ""} { set loops $src_loops }
            set w [dict get $meta width]
            set h [dict get $meta height]
            set frames {}
            set delays {}
            while {[set f [$dec next]] ne ""} {
                set ph [image create photo -width $w -height $h]
                ::tkwuffs::put_rgba $ph $w $h [dict get $f pixels]
                lappend frames $ph
                lappend delays [dict get $f delay_ms]
            }
            $dec destroy
            if {[llength $frames] == 0} {
                return -code error -errorcode {TCLWUFFS DECODE} \
                    "no frames decoded"
            }
            set _anims($photoName) [dict create \
                mode    eager   \
                frames  $frames \
                delays  $delays \
                width   $w      \
                height  $h      \
                loops   $loops  \
                iter    0       \
                index   0       \
                onstop  $onstop \
                after   ""]
        }

        if {$first_time} {
            trace add command $photoName delete \
                [list ::tkwuffs::_photo_gone $photoName]
        }
        return $photoName
    }

    proc play {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "no animation for photo: $photoName"
        }
        set s $_anims($photoName)
        if {[dict get $s after] ne ""} return
        set cap [dict get $s loops]
        if {$cap != 0 && [dict get $s iter] >= $cap} return

        if {[dict get $s mode] eq "lazy" && [dict get $s decoder] eq ""} {
            set dec [::tclwuffs::decoder [dict get $s bytes]]
            dict set s decoder $dec
            set _anims($photoName) $s
        }
        _tick $photoName
    }

    proc pause {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "no animation for photo: $photoName"
        }
        set aid [dict get $_anims($photoName) after]
        if {$aid eq ""} return
        after cancel $aid
        dict set _anims($photoName) after ""
    }

    proc restart {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "no animation for photo: $photoName"
        }
        set s $_anims($photoName)
        set was_playing [expr {[dict get $s after] ne ""}]

        set aid [dict get $s after]
        if {$aid ne ""} { after cancel $aid }
        dict set s after ""

        if {[dict get $s mode] eq "lazy"} {
            set dec [dict get $s decoder]
            if {$dec ne ""} {
                catch {$dec destroy}
                dict set s decoder ""
            }
        } else {
            dict set s index 0
        }
        dict set s iter 0
        set _anims($photoName) $s

        if {$was_playing} {
            play $photoName
        } else {
            # Render frame 0 without leaving ticks pending.
            play $photoName
            pause $photoName
        }
    }

    proc info {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} {
            return -code error -errorcode {TCLWUFFS INVALID_INPUT} \
                "no animation for photo: $photoName"
        }
        set s $_anims($photoName)
        set d [dict create \
            target  $photoName            \
            mode    [dict get $s mode]    \
            width   [dict get $s width]   \
            height  [dict get $s height]  \
            iter    [dict get $s iter]    \
            loops   [dict get $s loops]   \
            playing [expr {[dict get $s after] ne ""}]]
        if {[dict get $s mode] eq "eager"} {
            dict set d frame_count [llength [dict get $s frames]]
            dict set d index       [dict get $s index]
        }
        return $d
    }

    proc _tick {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} return
        if {[_findphoto $photoName] eq ""} return
        set s $_anims($photoName)
        if {[dict get $s mode] eq "lazy"} {
            _tick_lazy $photoName s
        } else {
            _tick_eager $photoName s
        }
        set _anims($photoName) $s
    }

    proc _tick_lazy {photoName sVar} {
        upvar 1 $sVar s
        set dec [dict get $s decoder]
        set f [$dec next]
        if {$f eq ""} {
            dict incr s iter
            set cap [dict get $s loops]
            if {$cap != 0 && [dict get $s iter] >= $cap} {
                _finish s; return
            }
            $dec restart
            set f [$dec next]
            if {$f eq ""} { _finish s; return }
        }
        ::tkwuffs::put_rgba $photoName \
            [dict get $s width] [dict get $s height] [dict get $f pixels]
        dict set s after [after [dict get $f delay_ms] \
            [list ::tkwuffs::_tick $photoName]]
    }

    proc _tick_eager {photoName sVar} {
        upvar 1 $sVar s
        set frames [dict get $s frames]
        set delays [dict get $s delays]
        set i      [dict get $s index]
        $photoName copy [lindex $frames $i] -compositingrule set
        set n    [llength $frames]
        set next [expr {$i + 1}]
        if {$next >= $n} {
            dict incr s iter
            set cap [dict get $s loops]
            if {$cap != 0 && [dict get $s iter] >= $cap} {
                _finish s; return
            }
            set next 0
        }
        dict set s index $next
        dict set s after [after [lindex $delays $i] \
            [list ::tkwuffs::_tick $photoName]]
    }

    # Loop cap reached. State stays alive; call restart to play again.
    proc _finish {sVar} {
        upvar 1 $sVar s
        set cb [dict get $s onstop]
        dict set s after ""
        if {$cb ne ""} { uplevel #0 $cb }
    }

    proc _teardown {photoName} {
        variable _anims
        if {![::info exists _anims($photoName)]} return
        set s $_anims($photoName)
        set aid [dict get $s after]
        if {$aid ne ""} { after cancel $aid }
        if {[dict get $s mode] eq "lazy"} {
            set dec [dict get $s decoder]
            if {$dec ne ""} { catch {$dec destroy} }
        } else {
            foreach ph [dict get $s frames] {
                catch {image delete $ph}
            }
        }
        unset _anims($photoName)
    }

    proc _photo_gone {photoName args} {
        catch {_teardown $photoName}
    }
}
