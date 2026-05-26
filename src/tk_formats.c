#include <tcl.h>
#include <tk.h>
#include <string.h>
#include <stdlib.h>

#include "wuffs_glue.h"
#include "tk_formats.h"

/* Tk's V3 photo-image-format API is the NUL-safe variant introduced in
 * Tk 8.7+. The V1 API hands you a C string for -data, which is what
 * caused the long-standing Img base64/binary-data confusion; V3 passes
 * a Tcl_Obj so we can pull bytes via Tcl_GetByteArrayFromObj cleanly.
 *
 * We register PNG/JPEG/GIF/BMP/WEBP, but for PNG/GIF Tk already has
 * built-in handlers and ours would shadow them. Less surprising to skip
 * those and only fill in what Tk lacks. See README. */

/* shared helpers */

static int put_rgba_block(Tcl_Interp* interp, Tk_PhotoHandle h,
                          uint32_t img_w, uint32_t img_h, uint8_t* rgba,
                          int destX, int destY,
                          int reqW, int reqH,
                          int srcX, int srcY) {
    /* Tk passes srcX/srcY/width/height to let callers crop on read.
     * Non-positive width/height mean "rest of image from srcX/srcY". */
    int sx = srcX < 0 ? 0 : srcX;
    int sy = srcY < 0 ? 0 : srcY;
    int sw = (reqW > 0) ? reqW : (int)img_w - sx;
    int sh = (reqH > 0) ? reqH : (int)img_h - sy;
    if (sx >= (int)img_w || sy >= (int)img_h || sw <= 0 || sh <= 0 ||
        sx + sw > (int)img_w || sy + sh > (int)img_h) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj("source rectangle outside image", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "INVALID_INPUT", NULL);
        return TCL_ERROR;
    }
    Tk_PhotoImageBlock blk;
    blk.pixelPtr = rgba + ((size_t)sy * img_w + sx) * 4u;
    blk.width = sw;
    blk.height = sh;
    blk.pitch = (int)img_w * 4;
    blk.pixelSize = 4;
    blk.offset[0] = 0;
    blk.offset[1] = 1;
    blk.offset[2] = 2;
    blk.offset[3] = 3;
    return Tk_PhotoPutBlock(interp, h, &blk, destX, destY, sw, sh,
                            TK_PHOTO_COMPOSITE_SET);
}

static int decode_obj_to_photo(Tcl_Interp* interp, Tcl_Obj* dataObj,
                               Tk_PhotoHandle h,
                               int destX, int destY,
                               int reqW, int reqH,
                               int srcX, int srcY) {
    Tcl_Size len = 0;
    const unsigned char* bytes = Tcl_GetByteArrayFromObj(dataObj, &len);
    uint32_t w = 0, hh = 0;
    uint8_t* rgba = NULL;
    tcw_err err = {0};
    if (tcw_decode(bytes, (size_t)len, &w, &hh, &rgba, &err) != TCW_OK) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(err.msg[0] ? err.msg : "decode failed", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "DECODE", NULL);
        return TCL_ERROR;
    }
    int rc = put_rgba_block(interp, h, w, hh, rgba,
                            destX, destY, reqW, reqH, srcX, srcY);
    tcw_free(rgba);
    return rc;
}

/* Slurps an open Tcl_Channel into a fresh byte-array Tcl_Obj.
 * Caller owns one ref; release with Tcl_DecrRefCount. */
static Tcl_Obj* slurp_channel(Tcl_Interp* interp, Tcl_Channel chan) {
    Tcl_SetChannelOption(interp, chan, "-translation", "binary");
    Tcl_SetChannelOption(interp, chan, "-encoding", "binary");
    Tcl_Obj* obj = Tcl_NewByteArrayObj(NULL, 0);
    Tcl_IncrRefCount(obj);
    if (Tcl_ReadChars(chan, obj, -1, 0) < 0) {
        Tcl_DecrRefCount(obj);
        return NULL;
    }
    return obj;
}

/* per-format match / read procs.
 * Each format gets a tiny match wrapper that diffs against tcw_sniff's
 * string; the read path is shared (tcw_decode sniffs again internally,
 * so we don't need to pass the format name down). */

#define DEFINE_FORMAT(NAME, SNIFF_NAME)                                       \
                                                                              \
static int match_string_##NAME(Tcl_Interp* interp, Tcl_Obj* dataObj,          \
                               Tcl_Obj* formatObj, Tcl_Obj* metaIn,           \
                               int* wp, int* hp, Tcl_Obj* metaOut) {          \
    (void)interp; (void)formatObj; (void)metaIn; (void)metaOut;               \
    Tcl_Size len = 0;                                                         \
    const unsigned char* b = Tcl_GetByteArrayFromObj(dataObj, &len);          \
    if (strcmp(tcw_sniff(b, (size_t)len), SNIFF_NAME) != 0) return 0;         \
    uint32_t w = 0, h = 0;                                                    \
    tcw_err err = {0};                                                        \
    if (tcw_dims(b, (size_t)len, &w, &h, &err) != TCW_OK) return 0;           \
    *wp = (int)w; *hp = (int)h;                                               \
    return 1;                                                                 \
}                                                                             \
                                                                              \
static int read_string_##NAME(Tcl_Interp* interp, Tcl_Obj* dataObj,           \
                              Tcl_Obj* formatObj, Tcl_Obj* metaIn,            \
                              Tk_PhotoHandle h,                               \
                              int destX, int destY, int width, int height,   \
                              int srcX, int srcY, Tcl_Obj* metaOut) {        \
    (void)formatObj; (void)metaIn; (void)metaOut;                             \
    return decode_obj_to_photo(interp, dataObj, h,                            \
                                destX, destY, width, height, srcX, srcY);     \
}                                                                             \
                                                                              \
static int match_file_##NAME(Tcl_Interp* interp, Tcl_Channel chan,            \
                             const char* path, Tcl_Obj* formatObj,            \
                             Tcl_Obj* metaIn,                                 \
                             int* wp, int* hp, Tcl_Obj* metaOut) {            \
    (void)path; (void)formatObj; (void)metaIn; (void)metaOut;                 \
    /* We need full bytes to read dimensions reliably; slurping cheap   */    \
    /* file headers isn't enough for some formats (e.g. embedded JFIF). */    \
    Tcl_Obj* obj = slurp_channel(interp, chan);                               \
    Tcl_Seek(chan, 0, SEEK_SET);                                              \
    if (!obj) return 0;                                                       \
    Tcl_Size len = 0;                                                         \
    const unsigned char* b = Tcl_GetByteArrayFromObj(obj, &len);              \
    int rc = 0;                                                               \
    if (strcmp(tcw_sniff(b, (size_t)len), SNIFF_NAME) == 0) {                 \
        uint32_t w = 0, h = 0;                                                \
        tcw_err err = {0};                                                    \
        if (tcw_dims(b, (size_t)len, &w, &h, &err) == TCW_OK) {               \
            *wp = (int)w; *hp = (int)h;                                       \
            rc = 1;                                                           \
        }                                                                     \
    }                                                                         \
    Tcl_DecrRefCount(obj);                                                    \
    return rc;                                                                \
}                                                                             \
                                                                              \
static int read_file_##NAME(Tcl_Interp* interp, Tcl_Channel chan,             \
                            const char* path, Tcl_Obj* formatObj,             \
                            Tcl_Obj* metaIn, Tk_PhotoHandle h,                \
                            int destX, int destY, int width, int height,     \
                            int srcX, int srcY, Tcl_Obj* metaOut) {          \
    (void)path;                                                               \
    Tcl_Obj* obj = slurp_channel(interp, chan);                               \
    if (!obj) return TCL_ERROR;                                               \
    int rc = read_string_##NAME(interp, obj, formatObj, metaIn, h,            \
                                 destX, destY, width, height,                  \
                                 srcX, srcY, metaOut);                         \
    Tcl_DecrRefCount(obj);                                                    \
    return rc;                                                                \
}

DEFINE_FORMAT(jpeg, "jpeg")
DEFINE_FORMAT(bmp,  "bmp")
DEFINE_FORMAT(webp, "webp")

#undef DEFINE_FORMAT

/* Tk photo block to packed RGBA.
 * Tk's pixel buffer is a Tk_PhotoImageBlock whose channel offsets are
 * arbitrary (BGRA on some platforms, RGB-only when alpha is unused, etc.)
 * so we have to repack to dense RGBA before handing bytes to stb encoders
 * or tcw_* helpers. Public via tk_formats.h. */

uint8_t* tcw_block_to_rgba(const Tk_PhotoImageBlock* blk) {
    if (blk->width <= 0 || blk->height <= 0) return NULL;
    size_t w = (size_t)blk->width, h = (size_t)blk->height;
    uint8_t* dst = (uint8_t*)malloc(w * h * 4u);
    if (!dst) return NULL;
    int r = blk->offset[0], g = blk->offset[1], b = blk->offset[2];
    int a = blk->offset[3];
    int has_alpha = (a != blk->offset[0]);  /* Tk sets a==r when no alpha */
    int ps = blk->pixelSize, pitch = blk->pitch;
    for (size_t y = 0; y < h; y++) {
        const unsigned char* src_row = blk->pixelPtr + (size_t)pitch * y;
        uint8_t* dst_row = dst + w * 4u * y;
        for (size_t x = 0; x < w; x++) {
            const unsigned char* sp = src_row + (size_t)ps * x;
            dst_row[x*4 + 0] = sp[r];
            dst_row[x*4 + 1] = sp[g];
            dst_row[x*4 + 2] = sp[b];
            dst_row[x*4 + 3] = has_alpha ? sp[a] : 255;
        }
    }
    return dst;
}

/* JPEG write procs */

/* Sets a Tcl error result with -errorcode TCLWUFFS INVALID_INPUT. The
 * tcl_err.h helpers aren't usable here because they live in a
 * Tcl-bindings-only header; this is the same pattern, locally. */
static int raise_jpeg_invalid(Tcl_Interp* interp, const char* msg) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(msg, -1));
    Tcl_SetErrorCode(interp, "TCLWUFFS", "INVALID_INPUT", NULL);
    return TCL_ERROR;
}

/* Pull `-quality N` out of the format list (e.g. {jpeg -quality 75}).
 * Returns TCL_OK and writes through quality_out (default 90) on success;
 * raises INVALID_INPUT on odd arg counts, unknown flags, or quality
 * outside 1..100. */
static int parse_jpeg_quality(Tcl_Interp* interp, Tcl_Obj* formatObj,
                              int* quality_out) {
    *quality_out = 90;
    if (!formatObj) return TCL_OK;
    Tcl_Size n = 0;
    Tcl_Obj** elts = NULL;
    if (Tcl_ListObjGetElements(interp, formatObj, &n, &elts) != TCL_OK)
        return TCL_ERROR;
    /* elts[0] is the format name itself ("jpeg"); options start at 1. */
    if (((n - 1) % 2) != 0)
        return raise_jpeg_invalid(interp,
            "jpeg format options must come in -flag value pairs");
    for (Tcl_Size i = 1; i < n; i += 2) {
        const char* flag = Tcl_GetString(elts[i]);
        if (strcmp(flag, "-quality") == 0) {
            int q;
            if (Tcl_GetIntFromObj(interp, elts[i + 1], &q) != TCL_OK)
                return TCL_ERROR;
            if (q < 1 || q > 100)
                return raise_jpeg_invalid(interp, "quality must be in 1..100");
            *quality_out = q;
        } else {
            char buf[128];
            snprintf(buf, sizeof buf, "unknown jpeg option: %s", flag);
            return raise_jpeg_invalid(interp, buf);
        }
    }
    return TCL_OK;
}

static int encode_block_jpeg(Tcl_Interp* interp, Tk_PhotoImageBlock* blk,
                             int quality, uint8_t** out, size_t* out_len) {
    uint8_t* rgba = tcw_block_to_rgba(blk);
    if (!rgba) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("photo is empty or oom", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "OOM", NULL);
        return TCL_ERROR;
    }
    tcw_err err = {0};
    int rc = tcw_encode_jpeg((uint32_t)blk->width, (uint32_t)blk->height,
                             rgba, quality, out, out_len, &err);
    free(rgba);
    if (rc != TCW_OK) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj(err.msg[0] ? err.msg : "encode failed", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "ENCODE", NULL);
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int string_write_jpeg(Tcl_Interp* interp, Tcl_Obj* formatObj,
                             Tcl_Obj* metaIn, Tk_PhotoImageBlock* blk) {
    (void)metaIn;
    int quality;
    if (parse_jpeg_quality(interp, formatObj, &quality) != TCL_OK) return TCL_ERROR;
    uint8_t* out = NULL;
    size_t out_len = 0;
    if (encode_block_jpeg(interp, blk, quality, &out, &out_len) != TCL_OK)
        return TCL_ERROR;
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    free(out);
    return TCL_OK;
}

static int file_write_jpeg(Tcl_Interp* interp, const char* fileName,
                           Tcl_Obj* formatObj, Tcl_Obj* metaIn,
                           Tk_PhotoImageBlock* blk) {
    (void)metaIn;
    int quality;
    if (parse_jpeg_quality(interp, formatObj, &quality) != TCL_OK) return TCL_ERROR;
    uint8_t* out = NULL;
    size_t out_len = 0;
    if (encode_block_jpeg(interp, blk, quality, &out, &out_len) != TCL_OK)
        return TCL_ERROR;

    Tcl_Channel chan = Tcl_OpenFileChannel(interp, fileName, "w", 0644);
    if (!chan) { free(out); return TCL_ERROR; }
    /* Tcl 9 dropped "binary" as an encoding name; -translation binary on
     * its own is enough to keep the channel byte-clean for our writes. */
    if (Tcl_SetChannelOption(interp, chan, "-translation", "binary") != TCL_OK) {
        Tcl_Close(interp, chan); free(out); return TCL_ERROR;
    }
    int written = Tcl_WriteRaw(chan, (const char*)out, (Tcl_Size)out_len);
    free(out);
    if (written < 0 || (size_t)written != out_len) {
        Tcl_Close(interp, chan);
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj("short write to JPEG output file", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "ENCODE", NULL);
        return TCL_ERROR;
    }
    return Tcl_Close(interp, chan);
}

/* registration */

static Tk_PhotoImageFormatVersion3 g_fmt_jpeg;
static Tk_PhotoImageFormatVersion3 g_fmt_bmp;
static Tk_PhotoImageFormatVersion3 g_fmt_webp;

static void fill_fmt(Tk_PhotoImageFormatVersion3* f, const char* name,
                     Tk_ImageFileMatchProcVersion3* fm,
                     Tk_ImageStringMatchProcVersion3* sm,
                     Tk_ImageFileReadProcVersion3* fr,
                     Tk_ImageStringReadProcVersion3* sr) {
    memset(f, 0, sizeof *f);
    f->name = name;
    f->fileMatchProc = fm;
    f->stringMatchProc = sm;
    f->fileReadProc = fr;
    f->stringReadProc = sr;
    /* Write procs are left NULL here; per-format setup in
     * tcw_register_tk_formats() may wire them in (only jpeg does today).
     * BMP/WEBP write isn't wired up and PNG output goes through Tk's
     * built-in writer or ::tkwuffs::encode_png_from_photo. */
}

void tcw_register_tk_formats(void) {
    fill_fmt(&g_fmt_jpeg, "jpeg",
        match_file_jpeg, match_string_jpeg,
        read_file_jpeg,  read_string_jpeg);
    g_fmt_jpeg.fileWriteProc   = file_write_jpeg;
    g_fmt_jpeg.stringWriteProc = string_write_jpeg;
    fill_fmt(&g_fmt_bmp, "bmp",
        match_file_bmp,  match_string_bmp,
        read_file_bmp,   read_string_bmp);
    fill_fmt(&g_fmt_webp, "webp",
        match_file_webp, match_string_webp,
        read_file_webp,  read_string_webp);
    Tk_CreatePhotoImageFormatVersion3(&g_fmt_jpeg);
    Tk_CreatePhotoImageFormatVersion3(&g_fmt_bmp);
    Tk_CreatePhotoImageFormatVersion3(&g_fmt_webp);
}
