# tclwuffs

Memory-safe image decode / resize / encode for Tcl/Tk. Decoders are
[google/wuffs](https://github.com/google/wuffs), resizing is
[`stb_image_resize2`](https://github.com/nothings/stb), PNG encoding is
[`stb_image_write`](https://github.com/nothings/stb). Untrusted bytes
only ever hit the wuffs parsers; everything else operates on already-decoded
RGBA buffers.

Two packages:

- **`tclwuffs`** — works in plain `tclsh`. Pure bytes-in / bytes-out.
- **`tkwuffs`** — adds a Tk `photo` image bridge. Requires Tk.

## Build

Vendored sources are committed under `vendor/`. To rebuild from clean:

```sh
make            # builds tclwuffs (+ tkwuffs if Tk is detected)
make test       # runs tclsh9.0/wish9.0 test suites
make smoke      # direct C-API smoke binary
make deps       # re-fetch vendored sources, verify SHA256
make clean
```

Build paths come from env vars (`TCL_INCLUDE`, `TCL_STUB_LIB`,
`TK_INCLUDE`, `TK_STUB_LIB`) when set — that's how
[zippy](https://github.com/pounceandmiss/zippy) drives static-archive
embedding. With nothing set, the Makefile sources `tclConfig.sh` /
`tkConfig.sh` from common system locations (`/usr/local/lib`, `/usr/lib`,
`/usr/lib64`, `/usr/lib/tcl9.0`, `/opt/homebrew/lib`); override with
`TCLCONFIG=` / `TKCONFIG=` if yours lives elsewhere.

Each tier produces both a shared library and a static archive:

| Output                      | Purpose                                  |
|-----------------------------|------------------------------------------|
| `libtclwuffs<VER>.so`       | `package require tclwuffs` (Tcl `load`)  |
| `libtclwuffs<VER>.a`        | Static link into a Tcl-embedding host    |
| `libtkwuffs<VER>.so`        | `package require tkwuffs`                |
| `libtkwuffs<VER>.a`         | Static link                              |
| `pkgIndex.tcl`              | Tcl package index for both               |

## API reference

All commands raise Tcl errors with `-errorcode {TCLWUFFS <CATEGORY> ...}`
on failure (see [Errors](#errors)).

### No-Tk tier (`package require tclwuffs`)

#### `::tclwuffs::sniff $bytes`

Inspects the first ~16 bytes of `$bytes` and returns the format name as
a string: `png`, `jpeg`, `gif`, `bmp`, `webp`, or `""` if unrecognized.
Does not raise on garbage input — empty result is the unknown signal.

#### `::tclwuffs::decode $bytes`

Decodes `$bytes` to a packed RGBA buffer (8 bits/channel, straight alpha,
top-down rows). Returns a dict:

| Key      | Type     | Notes                              |
|----------|----------|------------------------------------|
| `width`  | int      | image width in pixels              |
| `height` | int      | image height in pixels             |
| `pixels` | bytes    | `width * height * 4` bytes, RGBA   |

Supported decoders: PNG, JPEG, GIF, BMP, WebP (lossless only — lossy
VP8 will work automatically once google/wuffs PR #168 lands and the
pinned commit is bumped).

#### `::tclwuffs::encode_png $w $h $pixels`

Encodes `$pixels` (must be exactly `$w * $h * 4` bytes, packed RGBA
straight-alpha) into PNG bytes. Return value is zlib-deflated PNG via
`stb_image_write`.


#### `::tclwuffs::resize_bytes $bytes $w $h ?-filter default|bilinear|bicubic`

One-shot decode → resize → encode-PNG. `$bytes` is any supported input
format; output is PNG.


#### `::tclwuffs::crop_bytes $bytes $x $y $w $h`

One-shot decode → crop → encode-PNG. `$x`/`$y` are 0-based top-left;
`$x+$w` and `$y+$h` must fit inside the decoded image (out-of-bounds
raises `INVALID_INPUT`).


### Tk tier (`package require tkwuffs`)

Two things happen when you load `tkwuffs`:

1. The `::tkwuffs::*` commands below get registered.
2. **`Tk_PhotoImageFormatVersion3` hooks register for `jpeg`, `bmp`,
   and `webp`** so the *built-in* photo image creation routes through
   wuffs automatically:

   ```tcl
   package require tkwuffs
   image create photo p -file holiday.jpg     ;# decoded via wuffs
   image create photo p -data $webp_bytes     ;# also wuffs
   ```

   PNG and GIF are intentionally **not** shadowed — Tk handles those
   itself, and silently overriding well-known formats would be more
   surprising than helpful. To force-route a PNG through wuffs for
   memory-safety reasons, use `::tkwuffs::decode_to_photo` explicitly.


#### `::tkwuffs::decode_to_photo $bytes $photoName`

Decodes `$bytes` and writes the result into the existing Tk photo image
`$photoName`. Resizes the photo to match the decoded dimensions and
clears any prior contents.

```tcl
image create photo p
::tkwuffs::decode_to_photo $bytes p
```

#### `::tkwuffs::encode_png_from_photo $photoName`

Reads the pixels out of `$photoName` and returns PNG bytes. Handles
photos with or without an alpha channel; missing alpha is treated as
fully opaque.

```tcl
set png [::tkwuffs::encode_png_from_photo p]
```

#### `::tkwuffs::resize_photo $src $dst $w $h ?-filter F?`

Reads pixels from `$src` photo, resizes to `$w`×`$h`, writes the result
to `$dst` photo (overwriting). `$src` and `$dst` may be the same. Filter
options match `resize_bytes`.

```tcl
image create photo big
image create photo small
::tkwuffs::decode_to_photo $bytes big
::tkwuffs::resize_photo big small 64 64
```

#### `::tkwuffs::crop_photo $src $dst $x $y $w $h`

Reads pixels from `$src`, crops to the rectangle (`$x`,`$y`,`$w`,`$h`),
writes to `$dst`.

## Errors

Failures raise standard Tcl errors with a structured `-errorcode`:

```tcl
catch {::tclwuffs::decode "not an image"} msg
puts $::errorCode     ;# -> TCLWUFFS UNSUPPORTED_FORMAT
puts $msg             ;# human-readable detail (may include wuffs status)
```

| Category             | Cause                                                                |
|----------------------|----------------------------------------------------------------------|
| `INVALID_INPUT`      | bad argument (wrong pixel length, crop rect outside image, etc.)     |
| `UNSUPPORTED_FORMAT` | bytes don't match any supported decoder                              |
| `DECODE`             | wuffs decoder rejected the bytes (e.g. truncated, bad chunk)         |
| `ENCODE`             | stb encoder failed (rare; usually an OOM in disguise)                |
| `RESIZE`             | stb resize failed                                                    |
| `OOM`                | allocation failed somewhere in the pipeline                          |
| `TOO_LARGE`          | dimension exceeds the per-side cap (65535)                           |
| `INTERNAL`           | shouldn't happen; bug if you see it                                  |

## Pixel layout

Internal and Tcl-visible: **packed RGBA, 8 bits/channel, straight
(non-premultiplied) alpha, top-down rows**. Matches Tk 9's photo image,
so the Tk tier doesn't reorder anything when handing buffers to
`Tk_PhotoPutBlock`.

## Not there

- **Lossy WebP (VP8) decode** — automatic once google/wuffs PR #168 lands
- **EXIF orientation auto-rotate** 
