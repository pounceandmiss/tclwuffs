#include "wuffs_glue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "wuffs_config.h"
#include "../vendor/wuffs-v0.4.c"
#include "../vendor/stb_image_resize2.h"
#include "../vendor/stb_image_write.h"

/* Hard cap to refuse pathological dimensions before we allocate. 65535 is
 * large enough for any sane image; smaller multiplied out fits comfortably
 * in size_t on 32-bit platforms (65535*65535*4 ~ 17 GB but we only allocate
 * what we actually decode, so the cap is really about per-side sanity). */
#define TCW_MAX_DIMENSION 65535

/* error helpers */

static void set_err(tcw_err* err, int code, const char* fmt, ...) {
    if (!err) return;
    err->code = code;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof err->msg, fmt, ap);
        va_end(ap);
    } else {
        err->msg[0] = '\0';
    }
}

static void clear_err(tcw_err* err) {
    if (!err) return;
    err->code = TCW_OK;
    err->msg[0] = '\0';
}

/* public: format sniffing */

/* Internal helper: returns the wuffs FourCC or 0 if unrecognized. */
static int32_t sniff_fourcc(const uint8_t* bytes, size_t len) {
    if (!bytes || len == 0) return 0;
    return wuffs_base__magic_number_guess_fourcc(
        wuffs_base__make_slice_u8((uint8_t*)bytes, len), true);
}

const char* tcw_sniff(const uint8_t* bytes, size_t len) {
    switch (sniff_fourcc(bytes, len)) {
        case WUFFS_BASE__FOURCC__PNG:  return "png";
        case WUFFS_BASE__FOURCC__JPEG: return "jpeg";
        case WUFFS_BASE__FOURCC__GIF:  return "gif";
        case WUFFS_BASE__FOURCC__BMP:  return "bmp";
        case WUFFS_BASE__FOURCC__WEBP: return "webp";
        default:                       return "";
    }
}

/* internal: per-format decoder setup.
 * The concrete wuffs_<fmt>__decoder structs are opaque outside of the
 * implementation TU, so we use the heap allocators (which malloc + init
 * + upcast) and a single matching free routine. Dispatch is on wuffs's
 * FourCC directly since that's what the sniffer returns. */

static wuffs_base__image_decoder*
alloc_decoder(int32_t fourcc, tcw_err* err) {
    wuffs_base__image_decoder* dec = NULL;
    switch (fourcc) {
        case WUFFS_BASE__FOURCC__PNG:
            dec = wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
            break;
        case WUFFS_BASE__FOURCC__JPEG:
            dec = wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
            break;
        case WUFFS_BASE__FOURCC__GIF:
            dec = wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
            break;
        case WUFFS_BASE__FOURCC__BMP:
            dec = wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
            break;
        case WUFFS_BASE__FOURCC__WEBP:
            dec = wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder();
            break;
        default:
            set_err(err, TCW_ERR_UNSUPPORTED_FMT, "unsupported format");
            return NULL;
    }
    if (!dec) {
        set_err(err, TCW_ERR_OOM, "alloc decoder for fourcc 0x%08X",
                (unsigned)fourcc);
    }
    return dec;
}

/* public: decode */

int tcw_decode(const uint8_t* bytes, size_t len,
               uint32_t* out_w, uint32_t* out_h, uint8_t** out_pixels,
               tcw_err* err) {
    clear_err(err);
    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    if (!bytes || len == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "empty input");
        return TCW_ERR_INVALID_INPUT;
    }

    int32_t fourcc = sniff_fourcc(bytes, len);
    if (fourcc == 0) {
        set_err(err, TCW_ERR_UNSUPPORTED_FMT, "unrecognized image format");
        return TCW_ERR_UNSUPPORTED_FMT;
    }

    wuffs_base__image_decoder* dec = alloc_decoder(fourcc, err);
    if (!dec) return err ? err->code : TCW_ERR_DECODE;

    int rc = TCW_OK;
    uint8_t* pix = NULL;
    uint8_t* workbuf = NULL;

    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(
        (uint8_t*)bytes, len, /*closed=*/true);

    wuffs_base__image_config img_cfg = {0};
    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config(
        dec, &img_cfg, &src);
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_image_config: %s", st.repr);
        rc = TCW_ERR_DECODE;
        goto done;
    }

    uint32_t W = wuffs_base__pixel_config__width(&img_cfg.pixcfg);
    uint32_t H = wuffs_base__pixel_config__height(&img_cfg.pixcfg);
    if (W == 0 || H == 0) {
        set_err(err, TCW_ERR_DECODE, "zero-sized image");
        rc = TCW_ERR_DECODE;
        goto done;
    }
    if (W > TCW_MAX_DIMENSION || H > TCW_MAX_DIMENSION) {
        set_err(err, TCW_ERR_TOO_LARGE, "image %ux%u exceeds max dimension %d",
                W, H, TCW_MAX_DIMENSION);
        rc = TCW_ERR_TOO_LARGE;
        goto done;
    }

    wuffs_base__pixel_config__set(&img_cfg.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, W, H);

    size_t pix_len = (size_t)W * (size_t)H * 4u;
    pix = (uint8_t*)malloc(pix_len);
    if (!pix) {
        set_err(err, TCW_ERR_OOM, "alloc %zu bytes for pixels", pix_len);
        rc = TCW_ERR_OOM;
        goto done;
    }

    wuffs_base__pixel_buffer pixbuf = {0};
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pixbuf, &img_cfg.pixcfg, wuffs_base__make_slice_u8(pix, pix_len));
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "pixel_buffer__set_from_slice: %s", st.repr);
        rc = TCW_ERR_DECODE;
        goto done;
    }

    uint64_t workbuf_len =
        wuffs_base__image_decoder__workbuf_len(dec).max_incl;
    if (workbuf_len > (uint64_t)SIZE_MAX) {
        set_err(err, TCW_ERR_OOM, "workbuf too large");
        rc = TCW_ERR_OOM;
        goto done;
    }
    if (workbuf_len > 0) {
        workbuf = (uint8_t*)malloc((size_t)workbuf_len);
        if (!workbuf) {
            set_err(err, TCW_ERR_OOM, "alloc workbuf");
            rc = TCW_ERR_OOM;
            goto done;
        }
    }

    st = wuffs_base__image_decoder__decode_frame(
        dec, &pixbuf, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(workbuf, (size_t)workbuf_len), NULL);
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_frame: %s", st.repr);
        rc = TCW_ERR_DECODE;
        goto done;
    }

    /* For a packed RGBA_NONPREMUL pixel buffer constructed via
     * set_from_slice on a buffer we sized exactly to W*H*4, the plane's data
     * begins at our buffer with stride == W*4, no repacking needed. */

    *out_w = W;
    *out_h = H;
    *out_pixels = pix;
    pix = NULL;   /* ownership transferred */

done:
    free(workbuf);
    free(pix);
    free(dec);
    return rc;
}

/* public: dimension-only probe */

int tcw_dims(const uint8_t* bytes, size_t len,
             uint32_t* out_w, uint32_t* out_h, tcw_err* err) {
    clear_err(err);
    *out_w = 0;
    *out_h = 0;

    if (!bytes || len == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "empty input");
        return TCW_ERR_INVALID_INPUT;
    }
    int32_t fourcc = sniff_fourcc(bytes, len);
    if (fourcc == 0) {
        set_err(err, TCW_ERR_UNSUPPORTED_FMT, "unrecognized image format");
        return TCW_ERR_UNSUPPORTED_FMT;
    }
    wuffs_base__image_decoder* dec = alloc_decoder(fourcc, err);
    if (!dec) return err ? err->code : TCW_ERR_DECODE;

    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(
        (uint8_t*)bytes, len, /*closed=*/true);
    wuffs_base__image_config cfg = {0};
    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config(
        dec, &cfg, &src);
    int rc = TCW_OK;
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_image_config: %s", st.repr);
        rc = TCW_ERR_DECODE;
    } else {
        *out_w = wuffs_base__pixel_config__width(&cfg.pixcfg);
        *out_h = wuffs_base__pixel_config__height(&cfg.pixcfg);
        if (*out_w == 0 || *out_h == 0) {
            set_err(err, TCW_ERR_DECODE, "zero-sized image");
            rc = TCW_ERR_DECODE;
        }
    }
    free(dec);
    return rc;
}

/* public: streaming multi-frame decoder */

/* Browsers bump sub-20ms GIF delays to ~100ms (treating them as authoring
 * mistakes); we match so a 0-delay frame can't spin Tk's event loop. */
#define TCW_MIN_FRAME_DELAY_MS  20
#define TCW_BUMP_FRAME_DELAY_MS 100
#define TCW_MAX_FRAME_DELAY_MS  (UINT32_MAX / 2)

static uint32_t flicks_to_ms_clamped(uint64_t flicks) {
    uint64_t ms = flicks / WUFFS_BASE__FLICKS_PER_MILLISECOND;
    if (ms < TCW_MIN_FRAME_DELAY_MS) return TCW_BUMP_FRAME_DELAY_MS;
    if (ms > TCW_MAX_FRAME_DELAY_MS) return TCW_MAX_FRAME_DELAY_MS;
    return (uint32_t)ms;
}

/* Clear a sub-rectangle to transparent black. Clips silently. */
static void canvas_clear_rect(uint8_t* canvas, uint32_t cw, uint32_t ch,
                              wuffs_base__rect_ie_u32 r) {
    if (r.min_incl_x >= cw || r.min_incl_y >= ch) return;
    uint32_t x0 = r.min_incl_x;
    uint32_t y0 = r.min_incl_y;
    uint32_t x1 = r.max_excl_x > cw ? cw : r.max_excl_x;
    uint32_t y1 = r.max_excl_y > ch ? ch : r.max_excl_y;
    if (x1 <= x0 || y1 <= y0) return;
    size_t stride = (size_t)cw * 4u;
    size_t row_bytes = (size_t)(x1 - x0) * 4u;
    for (uint32_t y = y0; y < y1; y++) {
        memset(canvas + y * stride + x0 * 4u, 0, row_bytes);
    }
}

/* Copy a sub-rectangle from a same-sized backup. Clips silently. */
static void canvas_restore_rect(uint8_t* canvas, const uint8_t* backup,
                                uint32_t cw, uint32_t ch,
                                wuffs_base__rect_ie_u32 r) {
    if (r.min_incl_x >= cw || r.min_incl_y >= ch) return;
    uint32_t x0 = r.min_incl_x;
    uint32_t y0 = r.min_incl_y;
    uint32_t x1 = r.max_excl_x > cw ? cw : r.max_excl_x;
    uint32_t y1 = r.max_excl_y > ch ? ch : r.max_excl_y;
    if (x1 <= x0 || y1 <= y0) return;
    size_t stride = (size_t)cw * 4u;
    size_t row_bytes = (size_t)(x1 - x0) * 4u;
    for (uint32_t y = y0; y < y1; y++) {
        memcpy(canvas + y * stride + x0 * 4u,
               backup + y * stride + x0 * 4u,
               row_bytes);
    }
}

struct tcw_decoder {
    wuffs_base__image_decoder* dec;
    int32_t   fourcc;
    uint8_t*  bytes;          /* owned copy of source */
    size_t    bytes_len;
    wuffs_base__io_buffer    src;
    wuffs_base__image_config cfg;
    uint32_t  width;
    uint32_t  height;
    uint32_t  loop_count;
    uint8_t*  canvas;         /* width*height*4, composed in place */
    size_t    canvas_len;
    uint8_t*  workbuf;
    size_t    workbuf_len;
    uint8_t*  backup;         /* lazy alloc for RESTORE_PREVIOUS */
    wuffs_base__rect_ie_u32   prev_bounds;
    uint8_t   prev_disposal;
    uint8_t   ended;
};

int tcw_decoder_open(const uint8_t* bytes, size_t len,
                     tcw_decoder** out, tcw_err* err) {
    clear_err(err);
    *out = NULL;

    if (!bytes || len == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "empty input");
        return TCW_ERR_INVALID_INPUT;
    }
    int32_t fourcc = sniff_fourcc(bytes, len);
    if (fourcc == 0) {
        set_err(err, TCW_ERR_UNSUPPORTED_FMT, "unrecognized image format");
        return TCW_ERR_UNSUPPORTED_FMT;
    }

    tcw_decoder* d = (tcw_decoder*)calloc(1, sizeof(*d));
    if (!d) {
        set_err(err, TCW_ERR_OOM, "alloc decoder handle");
        return TCW_ERR_OOM;
    }

    /* Own a copy; the io_buffer references these bytes across many calls. */
    d->bytes = (uint8_t*)malloc(len);
    if (!d->bytes) {
        free(d);
        set_err(err, TCW_ERR_OOM, "alloc source-bytes copy");
        return TCW_ERR_OOM;
    }
    memcpy(d->bytes, bytes, len);
    d->bytes_len = len;
    d->fourcc = fourcc;

    d->dec = alloc_decoder(fourcc, err);
    if (!d->dec) {
        free(d->bytes); free(d);
        return err ? err->code : TCW_ERR_DECODE;
    }

    d->src = wuffs_base__ptr_u8__reader(d->bytes, d->bytes_len, /*closed=*/true);

    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config(
        d->dec, &d->cfg, &d->src);
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_image_config: %s", st.repr);
        tcw_decoder_close(d);
        return TCW_ERR_DECODE;
    }

    d->width  = wuffs_base__pixel_config__width(&d->cfg.pixcfg);
    d->height = wuffs_base__pixel_config__height(&d->cfg.pixcfg);
    if (d->width == 0 || d->height == 0) {
        set_err(err, TCW_ERR_DECODE, "zero-sized image");
        tcw_decoder_close(d);
        return TCW_ERR_DECODE;
    }
    if (d->width > TCW_MAX_DIMENSION || d->height > TCW_MAX_DIMENSION) {
        set_err(err, TCW_ERR_TOO_LARGE,
                "image %ux%u exceeds max dimension %d",
                d->width, d->height, TCW_MAX_DIMENSION);
        tcw_decoder_close(d);
        return TCW_ERR_TOO_LARGE;
    }
    wuffs_base__pixel_config__set(&d->cfg.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, d->width, d->height);

    d->canvas_len = (size_t)d->width * (size_t)d->height * 4u;
    d->canvas = (uint8_t*)calloc(1, d->canvas_len);
    if (!d->canvas) {
        set_err(err, TCW_ERR_OOM, "alloc canvas %zu bytes", d->canvas_len);
        tcw_decoder_close(d);
        return TCW_ERR_OOM;
    }

    uint64_t wbl = wuffs_base__image_decoder__workbuf_len(d->dec).max_incl;
    if (wbl > (uint64_t)SIZE_MAX) {
        set_err(err, TCW_ERR_OOM, "workbuf too large");
        tcw_decoder_close(d);
        return TCW_ERR_OOM;
    }
    d->workbuf_len = (size_t)wbl;
    if (d->workbuf_len > 0) {
        d->workbuf = (uint8_t*)malloc(d->workbuf_len);
        if (!d->workbuf) {
            set_err(err, TCW_ERR_OOM, "alloc workbuf");
            tcw_decoder_close(d);
            return TCW_ERR_OOM;
        }
    }

    d->prev_disposal     = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
    d->loop_count        = wuffs_base__image_decoder__num_animation_loops(d->dec);

    *out = d;
    return TCW_OK;
}

uint32_t tcw_decoder_width(const tcw_decoder* d)      { return d ? d->width      : 0; }
uint32_t tcw_decoder_height(const tcw_decoder* d)     { return d ? d->height     : 0; }
uint32_t tcw_decoder_loop_count(const tcw_decoder* d) { return d ? d->loop_count : 0; }

int tcw_decoder_next(tcw_decoder* d, const uint8_t** out_pixels,
                     uint32_t* out_delay_ms, tcw_err* err) {
    clear_err(err);
    if (out_pixels)   *out_pixels   = NULL;
    if (out_delay_ms) *out_delay_ms = 0;
    if (!d) {
        set_err(err, TCW_ERR_INVALID_INPUT, "null decoder");
        return TCW_ERR_INVALID_INPUT;
    }
    if (d->ended) return TCW_END;

    wuffs_base__frame_config fc = {0};
    wuffs_base__status st = wuffs_base__image_decoder__decode_frame_config(
        d->dec, &fc, &d->src);
    if (st.repr == wuffs_base__note__end_of_data) {
        d->ended = 1;
        /* GIF NETSCAPE2.0 loop count may only be known after the trailer. */
        d->loop_count = wuffs_base__image_decoder__num_animation_loops(d->dec);
        return TCW_END;
    }
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_frame_config: %s", st.repr);
        return TCW_ERR_DECODE;
    }

    /* Apply previous frame's disposal before drawing this one. */
    if (d->prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) {
        canvas_clear_rect(d->canvas, d->width, d->height, d->prev_bounds);
    } else if (d->prev_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS
               && d->backup) {
        canvas_restore_rect(d->canvas, d->backup, d->width, d->height,
                            d->prev_bounds);
    }

    wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
    uint8_t disposal = wuffs_base__frame_config__disposal(&fc);

    if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
        if (!d->backup) {
            d->backup = (uint8_t*)malloc(d->canvas_len);
            if (!d->backup) {
                set_err(err, TCW_ERR_OOM, "alloc disposal backup");
                return TCW_ERR_OOM;
            }
        }
        memcpy(d->backup, d->canvas, d->canvas_len);
    }

    wuffs_base__pixel_buffer pixbuf = {0};
    st = wuffs_base__pixel_buffer__set_from_slice(
        &pixbuf, &d->cfg.pixcfg,
        wuffs_base__make_slice_u8(d->canvas, d->canvas_len));
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "pixel_buffer__set_from_slice: %s", st.repr);
        return TCW_ERR_DECODE;
    }

    wuffs_base__pixel_blend blend =
        wuffs_base__frame_config__overwrite_instead_of_blend(&fc)
            ? WUFFS_BASE__PIXEL_BLEND__SRC
            : WUFFS_BASE__PIXEL_BLEND__SRC_OVER;

    st = wuffs_base__image_decoder__decode_frame(
        d->dec, &pixbuf, &d->src, blend,
        wuffs_base__make_slice_u8(d->workbuf, d->workbuf_len), NULL);
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "decode_frame: %s", st.repr);
        return TCW_ERR_DECODE;
    }

    d->prev_disposal = disposal;
    d->prev_bounds   = bounds;

    if (out_pixels)   *out_pixels   = d->canvas;
    if (out_delay_ms) *out_delay_ms = flicks_to_ms_clamped(
        wuffs_base__frame_config__duration(&fc));
    return TCW_OK;
}

int tcw_decoder_restart(tcw_decoder* d, tcw_err* err) {
    clear_err(err);
    if (!d) {
        set_err(err, TCW_ERR_INVALID_INPUT, "null decoder");
        return TCW_ERR_INVALID_INPUT;
    }
    /* Reinit instead of restart_frame(): the latter wants an io_position
     * we'd have to capture separately. One alloc per restart is cheap. */
    free(d->dec);
    d->dec = alloc_decoder(d->fourcc, err);
    if (!d->dec) return err ? err->code : TCW_ERR_DECODE;

    d->src = wuffs_base__ptr_u8__reader(d->bytes, d->bytes_len, /*closed=*/true);
    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config(
        d->dec, &d->cfg, &d->src);
    if (st.repr) {
        set_err(err, TCW_ERR_DECODE, "restart decode_image_config: %s", st.repr);
        return TCW_ERR_DECODE;
    }
    wuffs_base__pixel_config__set(&d->cfg.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, d->width, d->height);

    memset(d->canvas, 0, d->canvas_len);
    d->prev_disposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
    memset(&d->prev_bounds, 0, sizeof(d->prev_bounds));
    d->ended = 0;
    return TCW_OK;
}

void tcw_decoder_close(tcw_decoder* d) {
    if (!d) return;
    free(d->dec);
    free(d->bytes);
    free(d->canvas);
    free(d->workbuf);
    free(d->backup);
    free(d);
}

/* public: encode PNG via stb_image_write */

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
    int      err;   /* nonzero = grow failure */
} grow_buf;

static void grow_buf_write(void* ctx, void* data, int size) {
    if (size <= 0) return;
    grow_buf* b = (grow_buf*)ctx;
    if (b->err) return;
    size_t n = (size_t)size;
    if (n > SIZE_MAX - b->len) { b->err = 1; return; }
    size_t want = b->len + n;
    if (want > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < want) {
            if (cap > SIZE_MAX / 2) { b->err = 1; return; }
            cap *= 2;
        }
        uint8_t* p = (uint8_t*)realloc(b->data, cap);
        if (!p) { b->err = 1; return; }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len = want;
}

int tcw_encode_png(uint32_t w, uint32_t h, const uint8_t* rgba,
                   uint8_t** out_bytes, size_t* out_len,
                   tcw_err* err) {
    clear_err(err);
    *out_bytes = NULL;
    *out_len = 0;

    if (!rgba || w == 0 || h == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "invalid encode args");
        return TCW_ERR_INVALID_INPUT;
    }
    if (w > TCW_MAX_DIMENSION || h > TCW_MAX_DIMENSION) {
        set_err(err, TCW_ERR_TOO_LARGE, "encode dimensions too large");
        return TCW_ERR_TOO_LARGE;
    }

    grow_buf gb = {0};
    int rc = stbi_write_png_to_func(
        grow_buf_write, &gb,
        (int)w, (int)h, 4, rgba, (int)w * 4);

    if (!rc || gb.err) {
        free(gb.data);
        set_err(err, TCW_ERR_ENCODE,
                gb.err ? "encode out of memory" : "stbi_write_png_to_func failed");
        return gb.err ? TCW_ERR_OOM : TCW_ERR_ENCODE;
    }

    *out_bytes = gb.data;
    *out_len = gb.len;
    return TCW_OK;
}

int tcw_encode_jpeg(uint32_t w, uint32_t h, const uint8_t* rgba, int quality,
                    uint8_t** out_bytes, size_t* out_len,
                    tcw_err* err) {
    clear_err(err);
    *out_bytes = NULL;
    *out_len = 0;

    if (!rgba || w == 0 || h == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "invalid encode args");
        return TCW_ERR_INVALID_INPUT;
    }
    if (w > TCW_MAX_DIMENSION || h > TCW_MAX_DIMENSION) {
        set_err(err, TCW_ERR_TOO_LARGE, "encode dimensions too large");
        return TCW_ERR_TOO_LARGE;
    }
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    /* stb_image_write's JPEG encoder accepts comp=4 and silently ignores
     * the alpha channel, no RGBA->RGB pass needed. */
    grow_buf gb = {0};
    int rc = stbi_write_jpg_to_func(
        grow_buf_write, &gb,
        (int)w, (int)h, 4, rgba, quality);

    if (!rc || gb.err) {
        free(gb.data);
        set_err(err, TCW_ERR_ENCODE,
                gb.err ? "encode out of memory" : "stbi_write_jpg_to_func failed");
        return gb.err ? TCW_ERR_OOM : TCW_ERR_ENCODE;
    }

    *out_bytes = gb.data;
    *out_len = gb.len;
    return TCW_OK;
}

/* public: resize */

/* Returns 0 on success and writes the stb filter to *out; on a bad name,
 * sets err and returns nonzero. NULL or "default" maps to
 * STBIR_FILTER_DEFAULT. */
static int map_filter(const char* name, stbir_filter* out, tcw_err* err) {
    if (!name || !strcmp(name, "default"))   { *out = STBIR_FILTER_DEFAULT;     return 0; }
    if (!strcmp(name, "bilinear"))           { *out = STBIR_FILTER_TRIANGLE;    return 0; }
    if (!strcmp(name, "bicubic"))            { *out = STBIR_FILTER_CUBICBSPLINE; return 0; }
    set_err(err, TCW_ERR_INVALID_INPUT,
            "unknown filter \"%s\" (want default|bilinear|bicubic)", name);
    return -1;
}

int tcw_resize(uint32_t in_w, uint32_t in_h, const uint8_t* in_rgba,
               uint32_t out_w, uint32_t out_h, const char* filter,
               uint8_t** out_rgba, tcw_err* err) {
    clear_err(err);
    *out_rgba = NULL;

    if (!in_rgba || in_w == 0 || in_h == 0 || out_w == 0 || out_h == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "invalid resize args");
        return TCW_ERR_INVALID_INPUT;
    }
    if (out_w > TCW_MAX_DIMENSION || out_h > TCW_MAX_DIMENSION) {
        set_err(err, TCW_ERR_TOO_LARGE, "output dimensions too large");
        return TCW_ERR_TOO_LARGE;
    }

    stbir_filter stb_filter;
    if (map_filter(filter, &stb_filter, err) != 0) return TCW_ERR_INVALID_INPUT;

    size_t out_len = (size_t)out_w * (size_t)out_h * 4u;
    uint8_t* out = (uint8_t*)malloc(out_len);
    if (!out) {
        set_err(err, TCW_ERR_OOM, "alloc resize output");
        return TCW_ERR_OOM;
    }

    void* r = stbir_resize(
        in_rgba,  (int)in_w,  (int)in_h,  0,
        out,      (int)out_w, (int)out_h, 0,
        STBIR_RGBA, STBIR_TYPE_UINT8_SRGB,
        STBIR_EDGE_CLAMP, stb_filter);
    if (!r) {
        free(out);
        set_err(err, TCW_ERR_RESIZE, "stbir_resize failed");
        return TCW_ERR_RESIZE;
    }

    *out_rgba = out;
    return TCW_OK;
}

/* public: crop */

int tcw_crop(uint32_t in_w, uint32_t in_h, const uint8_t* in_rgba,
             int32_t x, int32_t y, uint32_t out_w, uint32_t out_h,
             uint8_t** out_rgba, tcw_err* err) {
    clear_err(err);
    *out_rgba = NULL;

    if (!in_rgba || in_w == 0 || in_h == 0 || out_w == 0 || out_h == 0) {
        set_err(err, TCW_ERR_INVALID_INPUT, "invalid crop args");
        return TCW_ERR_INVALID_INPUT;
    }
    if (x < 0 || y < 0 ||
        (uint32_t)x > in_w || (uint32_t)y > in_h ||
        out_w > in_w - (uint32_t)x ||
        out_h > in_h - (uint32_t)y) {
        set_err(err, TCW_ERR_INVALID_INPUT,
                "crop rect [%d,%d %ux%u] outside %ux%u",
                x, y, out_w, out_h, in_w, in_h);
        return TCW_ERR_INVALID_INPUT;
    }

    size_t row_bytes = (size_t)out_w * 4u;
    size_t out_len = row_bytes * (size_t)out_h;
    uint8_t* out = (uint8_t*)malloc(out_len);
    if (!out) {
        set_err(err, TCW_ERR_OOM, "alloc crop output");
        return TCW_ERR_OOM;
    }

    size_t in_stride = (size_t)in_w * 4u;
    for (uint32_t row = 0; row < out_h; row++) {
        const uint8_t* src_row =
            in_rgba + ((size_t)(y + (int32_t)row) * in_stride) + ((size_t)x * 4u);
        memcpy(out + row * row_bytes, src_row, row_bytes);
    }

    *out_rgba = out;
    return TCW_OK;
}

/* public: convenience pipelines */

/* Dispatch to PNG or JPEG encoder based on `format` (NULL or "png" -> PNG,
 * "jpeg" -> JPEG). Returns TCW_ERR_INVALID_INPUT for unknown formats. */
static int encode_rgba(uint32_t w, uint32_t h, const uint8_t* rgba,
                       const char* format, int quality,
                       uint8_t** out_bytes, size_t* out_len,
                       tcw_err* err) {
    if (!format || strcmp(format, "png") == 0)
        return tcw_encode_png(w, h, rgba, out_bytes, out_len, err);
    if (strcmp(format, "jpeg") == 0)
        return tcw_encode_jpeg(w, h, rgba, quality, out_bytes, out_len, err);
    set_err(err, TCW_ERR_INVALID_INPUT, "unknown format: %s", format);
    return TCW_ERR_INVALID_INPUT;
}

/* Cheap upfront sanity check so callers can reject a bad format before
 * sinking work into a decode+resize/crop. Same semantics as encode_rgba. */
static int check_format(const char* format, tcw_err* err) {
    if (!format) return TCW_OK;
    if (strcmp(format, "png") == 0)  return TCW_OK;
    if (strcmp(format, "jpeg") == 0) return TCW_OK;
    set_err(err, TCW_ERR_INVALID_INPUT, "unknown format: %s", format);
    return TCW_ERR_INVALID_INPUT;
}

int tcw_resize_bytes(const uint8_t* in_bytes, size_t in_len,
                     uint32_t out_w, uint32_t out_h, const char* filter,
                     const char* format, int quality,
                     uint8_t** out_bytes, size_t* out_len,
                     tcw_err* err) {
    *out_bytes = NULL;
    *out_len = 0;

    int rc = check_format(format, err);
    if (rc != TCW_OK) return rc;

    uint32_t in_w, in_h;
    uint8_t* rgba = NULL;
    rc = tcw_decode(in_bytes, in_len, &in_w, &in_h, &rgba, err);
    if (rc != TCW_OK) return rc;

    uint8_t* resized = NULL;
    rc = tcw_resize(in_w, in_h, rgba, out_w, out_h, filter, &resized, err);
    free(rgba);
    if (rc != TCW_OK) return rc;

    rc = encode_rgba(out_w, out_h, resized, format, quality,
                     out_bytes, out_len, err);
    free(resized);
    return rc;
}

int tcw_crop_bytes(const uint8_t* in_bytes, size_t in_len,
                   int32_t x, int32_t y, uint32_t out_w, uint32_t out_h,
                   const char* format, int quality,
                   uint8_t** out_bytes, size_t* out_len,
                   tcw_err* err) {
    *out_bytes = NULL;
    *out_len = 0;

    int rc = check_format(format, err);
    if (rc != TCW_OK) return rc;

    uint32_t in_w, in_h;
    uint8_t* rgba = NULL;
    rc = tcw_decode(in_bytes, in_len, &in_w, &in_h, &rgba, err);
    if (rc != TCW_OK) return rc;

    uint8_t* cropped = NULL;
    rc = tcw_crop(in_w, in_h, rgba, x, y, out_w, out_h, &cropped, err);
    free(rgba);
    if (rc != TCW_OK) return rc;

    rc = encode_rgba(out_w, out_h, cropped, format, quality,
                     out_bytes, out_len, err);
    free(cropped);
    return rc;
}

/* memory management */

void tcw_free(void* p) {
    free(p);
}
