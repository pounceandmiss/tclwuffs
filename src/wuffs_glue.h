#ifndef TCLWUFFS_WUFFS_GLUE_H
#define TCLWUFFS_WUFFS_GLUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error categories. These are *our* codes; neither wuffs nor stb expose
 * an enum we could reuse (wuffs status is a const char*; stb returns 0/1).
 * The value is chosen by the failure site, not parsed from any library
 * message: a non-OK wuffs status caught inside the decode path is
 * TCW_ERR_DECODE, an stbir_resize NULL is TCW_ERR_RESIZE, our own malloc
 * miss is TCW_ERR_OOM, and so on. Maps onto the spec's required
 * Tcl -errorcode {TCLWUFFS <CATEGORY> ...} taxonomy. */
enum {
    TCW_OK                  =  0,
    TCW_ERR_INVALID_INPUT   = -1,   /* INVALID_INPUT */
    TCW_ERR_OOM             = -2,   /* OOM */
    TCW_ERR_UNSUPPORTED_FMT = -3,   /* UNSUPPORTED_FORMAT */
    TCW_ERR_DECODE          = -4,   /* DECODE */
    TCW_ERR_ENCODE          = -5,   /* ENCODE */
    TCW_ERR_RESIZE          = -6,   /* RESIZE */
    TCW_ERR_TOO_LARGE       = -7    /* TOO_LARGE */
};

typedef struct {
    int  code;
    char msg[256];
} tcw_err;

/* Returns "png", "jpeg", "gif", "bmp", "webp", or "" if the bytes don't
 * look like any supported format. Inspects only the first ~16 bytes.
 * The returned pointer is a static string. */
const char* tcw_sniff(const uint8_t* bytes, size_t len);

/* Decode bytes to RGBA (straight alpha). On success, sets out_w/out_h and
 * writes a heap pointer to *out_pixels (caller frees with tcw_free).
 * *out_pixels is w*h*4 bytes, top-to-bottom rows. */
int tcw_decode(const uint8_t* bytes, size_t len,
               uint32_t* out_w, uint32_t* out_h, uint8_t** out_pixels,
               tcw_err* err);

/* Cheap dimension-only inspection: parses just enough of the bitstream
 * to read width/height. No frame decode, no full allocation. Used by
 * Tk's photo-image-format match procs, which must report dimensions
 * before the photo gets sized. */
int tcw_dims(const uint8_t* bytes, size_t len,
             uint32_t* out_w, uint32_t* out_h, tcw_err* err);

/* Encode RGBA pixels (straight alpha) to PNG bytes. Uses stb_image_write,
 * which emits a normally zlib-deflated PNG. */
int tcw_encode_png(uint32_t w, uint32_t h, const uint8_t* rgba,
                   uint8_t** out_bytes, size_t* out_len,
                   tcw_err* err);

/* Encode RGBA pixels to baseline JPEG bytes via stb_image_write. The alpha
 * channel is dropped (JPEG has no alpha). `quality` is 1..100; values
 * outside that range are clamped. */
int tcw_encode_jpeg(uint32_t w, uint32_t h, const uint8_t* rgba, int quality,
                    uint8_t** out_bytes, size_t* out_len,
                    tcw_err* err);

/* Resize an RGBA buffer (no parsing). `filter` is one of "default",
 * "bilinear", "bicubic", or NULL (treated as "default"). */
int tcw_resize(uint32_t in_w, uint32_t in_h, const uint8_t* in_rgba,
               uint32_t out_w, uint32_t out_h, const char* filter,
               uint8_t** out_rgba, tcw_err* err);

/* Crop an RGBA buffer to (x,y,w,h). */
int tcw_crop(uint32_t in_w, uint32_t in_h, const uint8_t* in_rgba,
             int32_t x, int32_t y, uint32_t out_w, uint32_t out_h,
             uint8_t** out_rgba, tcw_err* err);

/* Convenience: decode + resize + re-encode. See tcw_resize for filter.
 * `format` is "png" (default if NULL) or "jpeg"; `quality` is honored only
 * for JPEG (1..100, clamped). */
int tcw_resize_bytes(const uint8_t* in_bytes, size_t in_len,
                     uint32_t out_w, uint32_t out_h, const char* filter,
                     const char* format, int quality,
                     uint8_t** out_bytes, size_t* out_len,
                     tcw_err* err);

/* Convenience: decode + crop + re-encode. `format`/`quality` as above. */
int tcw_crop_bytes(const uint8_t* in_bytes, size_t in_len,
                   int32_t x, int32_t y, uint32_t out_w, uint32_t out_h,
                   const char* format, int quality,
                   uint8_t** out_bytes, size_t* out_len,
                   tcw_err* err);

/* Frees any pointer returned via an out param above. */
void tcw_free(void* p);

#ifdef __cplusplus
}
#endif

#endif
