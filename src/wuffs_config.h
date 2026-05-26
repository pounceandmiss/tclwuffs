#ifndef TCLWUFFS_WUFFS_CONFIG_H
#define TCLWUFFS_WUFFS_CONFIG_H

/* Centralizes the WUFFS_CONFIG__MODULE__* selection. Both the implementation
 * TU (vendor_wuffs.c) and consumers that include wuffs-v0.4.c as a header
 * pull this in first so the visible module set stays in sync. */

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__VP8
#define WUFFS_CONFIG__MODULE__WEBP
#define WUFFS_CONFIG__MODULE__ZLIB

/* Narrow the runtime-selectable destination pixel formats. We only ever
 * decode to straight-alpha RGBA, matching Tk 9's photo image. */
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_RGBA_NONPREMUL

#endif
