#ifndef TCLWUFFS_TK_FORMATS_H
#define TCLWUFFS_TK_FORMATS_H

#include <tk.h>
#include <stdint.h>

/* Register the Tk_PhotoImageFormatVersion3 hooks so that
 *   image create photo -file foo.jpg
 *   image create photo -data $bytes
 * route through wuffs+stb for jpeg/bmp/webp. PNG and GIF are left to
 * Tk's built-in handlers; we don't shadow what Tk already does. */
void tcw_register_tk_formats(void);

/* Repack a Tk photo block (whose channel offsets and pixel size are
 * arbitrary) into a freshly malloc'd, densely-packed RGBA buffer with
 * straight alpha. Returns NULL on empty block or alloc failure; caller
 * frees with free(). Shared between the Tk write procs and the tkwuffs
 * binding commands so we don't carry two copies of the same loop. */
uint8_t* tcw_block_to_rgba(const Tk_PhotoImageBlock* blk);

#endif
