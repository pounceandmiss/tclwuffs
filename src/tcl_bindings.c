#include <tcl.h>
#include <string.h>
#include "wuffs_glue.h"
#include "tcl_err.h"

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.1"
#endif

/* ::tclwuffs::sniff $bytes */

static int sniff_cmd(void* cd, Tcl_Interp* interp,
                     int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "bytes");
        return TCL_ERROR;
    }
    Tcl_Size len = 0;
    const unsigned char* bytes = Tcl_GetByteArrayFromObj(objv[1], &len);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(tcw_sniff(bytes, (size_t)len), -1));
    return TCL_OK;
}

/* ::tclwuffs::decode $bytes  ->  {width N height M pixels <bytes>} */

static int decode_cmd(void* cd, Tcl_Interp* interp,
                      int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "bytes");
        return TCL_ERROR;
    }
    Tcl_Size len = 0;
    const unsigned char* bytes = Tcl_GetByteArrayFromObj(objv[1], &len);

    uint32_t w = 0, h = 0;
    uint8_t* pix = NULL;
    tcw_err err = {0};
    int rc = tcw_decode(bytes, (size_t)len, &w, &h, &pix, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_Obj* dict = Tcl_NewDictObj();
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("width", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)w));
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("height", -1),
                   Tcl_NewWideIntObj((Tcl_WideInt)h));
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("pixels", -1),
                   Tcl_NewByteArrayObj(pix, (Tcl_Size)((size_t)w * h * 4u)));
    tcw_free(pix);

    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

/* ::tclwuffs::decoder $bytes: returns a handle command supporting
 * info / next / restart / destroy. See README. */

static void dec_delete_proc(void* cd) {
    tcw_decoder* dec = (tcw_decoder*)cd;
    tcw_decoder_close(dec);
}

static int dec_handle_cmd(void* cd, Tcl_Interp* interp,
                          int objc, Tcl_Obj* const objv[]) {
    tcw_decoder* dec = (tcw_decoder*)cd;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?args?");
        return TCL_ERROR;
    }
    const char* sub = Tcl_GetString(objv[1]);

    if (strcmp(sub, "info") == 0) {
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "");
            return TCL_ERROR;
        }
        Tcl_Obj* d = Tcl_NewDictObj();
        Tcl_DictObjPut(NULL, d, Tcl_NewStringObj("width", -1),
                       Tcl_NewWideIntObj(tcw_decoder_width(dec)));
        Tcl_DictObjPut(NULL, d, Tcl_NewStringObj("height", -1),
                       Tcl_NewWideIntObj(tcw_decoder_height(dec)));
        Tcl_DictObjPut(NULL, d, Tcl_NewStringObj("loop_count", -1),
                       Tcl_NewWideIntObj(tcw_decoder_loop_count(dec)));
        Tcl_SetObjResult(interp, d);
        return TCL_OK;
    }

    if (strcmp(sub, "next") == 0) {
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "");
            return TCL_ERROR;
        }
        const uint8_t* pix = NULL;
        uint32_t delay_ms = 0;
        tcw_err err = {0};
        int rc = tcw_decoder_next(dec, &pix, &delay_ms, &err);
        if (rc == TCW_END) {
            Tcl_ResetResult(interp);
            return TCL_OK;
        }
        if (rc != TCW_OK) return tcw_raise(interp, &err);
        Tcl_Obj* d = Tcl_NewDictObj();
        Tcl_DictObjPut(NULL, d, Tcl_NewStringObj("pixels", -1),
                       Tcl_NewByteArrayObj(pix,
                           (Tcl_Size)((size_t)tcw_decoder_width(dec)
                                      * tcw_decoder_height(dec) * 4u)));
        Tcl_DictObjPut(NULL, d, Tcl_NewStringObj("delay_ms", -1),
                       Tcl_NewWideIntObj(delay_ms));
        Tcl_SetObjResult(interp, d);
        return TCL_OK;
    }

    if (strcmp(sub, "restart") == 0) {
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "");
            return TCL_ERROR;
        }
        tcw_err err = {0};
        if (tcw_decoder_restart(dec, &err) != TCW_OK)
            return tcw_raise(interp, &err);
        return TCL_OK;
    }

    if (strcmp(sub, "destroy") == 0) {
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "");
            return TCL_ERROR;
        }
        /* Fires dec_delete_proc which closes the C decoder. */
        Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
        return TCL_OK;
    }

    return tcw_raise_invalid(interp,
        "unknown subcommand \"%s\": must be info, next, restart, or destroy", sub);
}

static int decoder_cmd(void* cd, Tcl_Interp* interp,
                       int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "bytes");
        return TCL_ERROR;
    }
    Tcl_Size len = 0;
    const unsigned char* bytes = Tcl_GetByteArrayFromObj(objv[1], &len);

    tcw_decoder* dec = NULL;
    tcw_err err = {0};
    int rc = tcw_decoder_open(bytes, (size_t)len, &dec, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    static int seq = 0;
    char name[64];
    snprintf(name, sizeof name, "::tclwuffs::dec%d", seq++);
    Tcl_CreateObjCommand(interp, name, dec_handle_cmd, dec, dec_delete_proc);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(name, -1));
    return TCL_OK;
}

/* ::tclwuffs::encode_png $w $h $pixels */

static int encode_png_cmd(void* cd, Tcl_Interp* interp,
                          int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "w h pixels");
        return TCL_ERROR;
    }
    int w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[1], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "width and height must be positive");
    Tcl_Size pix_len = 0;
    const unsigned char* pix = Tcl_GetByteArrayFromObj(objv[3], &pix_len);
    size_t need = (size_t)w_i * (size_t)h_i * 4u;
    if ((size_t)pix_len != need)
        return tcw_raise_invalid(interp,
            "pixels length %lld does not match %dx%d*4 = %zu",
            (long long)pix_len, w_i, h_i, need);

    uint8_t* out = NULL;
    size_t   out_len = 0;
    tcw_err err = {0};
    int rc = tcw_encode_png((uint32_t)w_i, (uint32_t)h_i, pix,
                            &out, &out_len, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    tcw_free(out);
    return TCL_OK;
}

/* Parse trailing ?-flag value? pairs starting at objv[start]. A non-NULL
 * out param means that flag is accepted; NULL means "raise on this flag".
 * Unknown flags and odd argument counts raise INVALID_INPUT. */
static int parse_encode_opts(Tcl_Interp* interp, int objc, Tcl_Obj* const objv[],
                             int start, const char** filter_out,
                             const char** format_out, int* quality_out) {
    if (((objc - start) % 2) != 0)
        return tcw_raise_invalid(interp, "options must come in -flag value pairs");
    for (int i = start; i < objc; i += 2) {
        const char* flag = Tcl_GetString(objv[i]);
        if (filter_out && strcmp(flag, "-filter") == 0) {
            *filter_out = Tcl_GetString(objv[i + 1]);
        } else if (format_out && strcmp(flag, "-format") == 0) {
            *format_out = Tcl_GetString(objv[i + 1]);
        } else if (quality_out && strcmp(flag, "-quality") == 0) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], quality_out) != TCL_OK)
                return TCL_ERROR;
            if (*quality_out < 1 || *quality_out > 100)
                return tcw_raise_invalid(interp, "quality must be in 1..100");
        } else {
            return tcw_raise_invalid(interp, "unknown option: %s", flag);
        }
    }
    return TCL_OK;
}

/* ::tclwuffs::encode_jpeg $w $h $pixels ?-quality N? */

static int encode_jpeg_cmd(void* cd, Tcl_Interp* interp,
                           int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "w h pixels ?-quality N?");
        return TCL_ERROR;
    }
    int w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[1], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[2], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "width and height must be positive");
    Tcl_Size pix_len = 0;
    const unsigned char* pix = Tcl_GetByteArrayFromObj(objv[3], &pix_len);
    size_t need = (size_t)w_i * (size_t)h_i * 4u;
    if ((size_t)pix_len != need)
        return tcw_raise_invalid(interp,
            "pixels length %lld does not match %dx%d*4 = %zu",
            (long long)pix_len, w_i, h_i, need);

    int quality = 90;
    if (parse_encode_opts(interp, objc, objv, 4, NULL, NULL, &quality) != TCL_OK)
        return TCL_ERROR;

    uint8_t* out = NULL;
    size_t   out_len = 0;
    tcw_err err = {0};
    int rc = tcw_encode_jpeg((uint32_t)w_i, (uint32_t)h_i, pix, quality,
                             &out, &out_len, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    tcw_free(out);
    return TCL_OK;
}

/* ::tclwuffs::resize_bytes $bytes $w $h ?opts? */

static int resize_bytes_cmd(void* cd, Tcl_Interp* interp,
                            int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "bytes w h ?-filter default|bilinear|bicubic? "
            "?-format png|jpeg? ?-quality N?");
        return TCL_ERROR;
    }
    Tcl_Size in_len = 0;
    const unsigned char* in_bytes = Tcl_GetByteArrayFromObj(objv[1], &in_len);
    int w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[2], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[3], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "target width and height must be positive");

    const char* filter = NULL;
    const char* format = NULL;
    int quality = 90;
    if (parse_encode_opts(interp, objc, objv, 4,
                          &filter, &format, &quality) != TCL_OK)
        return TCL_ERROR;

    uint8_t* out = NULL;
    size_t out_len = 0;
    tcw_err err = {0};
    int rc = tcw_resize_bytes(in_bytes, (size_t)in_len,
                              (uint32_t)w_i, (uint32_t)h_i, filter,
                              format, quality,
                              &out, &out_len, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    tcw_free(out);
    return TCL_OK;
}

/* ::tclwuffs::crop_bytes $bytes $x $y $w $h ?opts? */

static int crop_bytes_cmd(void* cd, Tcl_Interp* interp,
                          int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc < 6) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "bytes x y w h ?-format png|jpeg? ?-quality N?");
        return TCL_ERROR;
    }
    Tcl_Size in_len = 0;
    const unsigned char* in_bytes = Tcl_GetByteArrayFromObj(objv[1], &in_len);
    int x_i, y_i, w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[2], &x_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[3], &y_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[4], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[5], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "crop width and height must be positive");

    const char* format = NULL;
    int quality = 90;
    if (parse_encode_opts(interp, objc, objv, 6, NULL, &format, &quality) != TCL_OK)
        return TCL_ERROR;

    uint8_t* out = NULL;
    size_t out_len = 0;
    tcw_err err = {0};
    int rc = tcw_crop_bytes(in_bytes, (size_t)in_len,
                            (int32_t)x_i, (int32_t)y_i,
                            (uint32_t)w_i, (uint32_t)h_i,
                            format, quality,
                            &out, &out_len, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    tcw_free(out);
    return TCL_OK;
}

/* package init */

#ifndef DLLEXPORT
#  ifdef _WIN32
#    define DLLEXPORT __declspec(dllexport)
#  else
#    define DLLEXPORT
#  endif
#endif

DLLEXPORT int Tclwuffs_Init(Tcl_Interp* interp) {
    if (Tcl_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
    Tcl_CreateNamespace(interp, "::tclwuffs", NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::sniff",        sniff_cmd,        NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::decode",       decode_cmd,       NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::decoder",      decoder_cmd,      NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::encode_png",   encode_png_cmd,   NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::encode_jpeg",  encode_jpeg_cmd,  NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::resize_bytes", resize_bytes_cmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tclwuffs::crop_bytes",   crop_bytes_cmd,   NULL, NULL);
    if (Tcl_PkgProvide(interp, "tclwuffs", PACKAGE_VERSION) != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}
