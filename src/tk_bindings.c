#include <tcl.h>
#include <tk.h>
#include <string.h>
#include <stdlib.h>
#include "wuffs_glue.h"
#include "tcl_err.h"
#include "tk_formats.h"

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.1"
#endif

/* Resolve a Tcl_Obj photo name to a Tk_PhotoHandle, or set a Tcl error. */
static Tk_PhotoHandle resolve_photo(Tcl_Interp* interp, Tcl_Obj* name) {
    Tk_PhotoHandle h = Tk_FindPhoto(interp, Tcl_GetString(name));
    if (!h)
        tcw_raise_invalid(interp,
            "image \"%s\" doesn't exist or is not a photo image",
            Tcl_GetString(name));
    return h;
}

/* Push RGBA pixels into a photo, replacing its contents. */
static int put_rgba_into_photo(Tcl_Interp* interp, Tk_PhotoHandle h,
                               uint32_t w, uint32_t h_, const uint8_t* rgba) {
    if (Tk_PhotoSetSize(interp, h, (int)w, (int)h_) != TCL_OK) return TCL_ERROR;
    Tk_PhotoBlank(h);

    Tk_PhotoImageBlock blk;
    blk.pixelPtr = (unsigned char*)rgba;
    blk.width = (int)w;
    blk.height = (int)h_;
    blk.pitch = (int)w * 4;
    blk.pixelSize = 4;
    blk.offset[0] = 0;
    blk.offset[1] = 1;
    blk.offset[2] = 2;
    blk.offset[3] = 3;

    if (Tk_PhotoPutBlock(interp, h, &blk, 0, 0, (int)w, (int)h_,
                         TK_PHOTO_COMPOSITE_SET) != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}

/* Copy a photo's pixels out to a fresh packed RGBA buffer (straight alpha).
 * Returns malloc'd buffer or NULL on alloc failure; sets *out_w and *out_h. */
static uint8_t* photo_to_rgba(Tk_PhotoHandle h, uint32_t* out_w, uint32_t* out_h) {
    Tk_PhotoImageBlock blk;
    Tk_PhotoGetImage(h, &blk);
    uint8_t* dst = tcw_block_to_rgba(&blk);
    if (!dst) return NULL;
    *out_w = (uint32_t)blk.width;
    *out_h = (uint32_t)blk.height;
    return dst;
}

/* ::tkwuffs::decode_to_photo $bytes $photoName */

static int decode_to_photo_cmd(void* cd, Tcl_Interp* interp,
                               int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "bytes photoName");
        return TCL_ERROR;
    }
    Tk_PhotoHandle h = resolve_photo(interp, objv[2]);
    if (!h) return TCL_ERROR;

    Tcl_Size len = 0;
    const unsigned char* bytes = Tcl_GetByteArrayFromObj(objv[1], &len);

    uint32_t w, hh;
    uint8_t* rgba = NULL;
    tcw_err err = {0};
    int rc = tcw_decode(bytes, (size_t)len, &w, &hh, &rgba, &err);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    int tk_rc = put_rgba_into_photo(interp, h, w, hh, rgba);
    tcw_free(rgba);
    return tk_rc;
}

/* ::tkwuffs::encode_png_from_photo $photoName */

static int encode_png_from_photo_cmd(void* cd, Tcl_Interp* interp,
                                     int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "photoName");
        return TCL_ERROR;
    }
    Tk_PhotoHandle h = resolve_photo(interp, objv[1]);
    if (!h) return TCL_ERROR;

    uint32_t w, hh;
    uint8_t* rgba = photo_to_rgba(h, &w, &hh);
    if (!rgba) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("photo is empty or oom", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "OOM", NULL);
        return TCL_ERROR;
    }

    uint8_t* out = NULL;
    size_t out_len = 0;
    tcw_err err = {0};
    int rc = tcw_encode_png(w, hh, rgba, &out, &out_len, &err);
    tcw_free(rgba);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(out, (Tcl_Size)out_len));
    tcw_free(out);
    return TCL_OK;
}

/* ::tkwuffs::resize_photo $src $dst $w $h ?-filter F? */

static int resize_photo_cmd(void* cd, Tcl_Interp* interp,
                            int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc < 5 || objc > 7) {
        Tcl_WrongNumArgs(interp, 1, objv,
            "src dst w h ?-filter default|bilinear|bicubic?");
        return TCL_ERROR;
    }
    Tk_PhotoHandle src = resolve_photo(interp, objv[1]);
    if (!src) return TCL_ERROR;
    Tk_PhotoHandle dst = resolve_photo(interp, objv[2]);
    if (!dst) return TCL_ERROR;

    int w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[3], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[4], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "target width and height must be positive");

    const char* filter = NULL;
    if (objc >= 7) {
        const char* flag = Tcl_GetString(objv[5]);
        if (strcmp(flag, "-filter") != 0)
            return tcw_raise_invalid(interp, "unknown option: %s", flag);
        filter = Tcl_GetString(objv[6]);
    }

    uint32_t in_w, in_h;
    uint8_t* in_rgba = photo_to_rgba(src, &in_w, &in_h);
    if (!in_rgba) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("src photo is empty or oom", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "OOM", NULL);
        return TCL_ERROR;
    }

    uint8_t* out = NULL;
    tcw_err err = {0};
    int rc = tcw_resize(in_w, in_h, in_rgba,
                        (uint32_t)w_i, (uint32_t)h_i, filter, &out, &err);
    tcw_free(in_rgba);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    int tk_rc = put_rgba_into_photo(interp, dst,
                                    (uint32_t)w_i, (uint32_t)h_i, out);
    tcw_free(out);
    return tk_rc;
}

/* ::tkwuffs::crop_photo $src $dst $x $y $w $h */

static int crop_photo_cmd(void* cd, Tcl_Interp* interp,
                          int objc, Tcl_Obj* const objv[]) {
    (void)cd;
    if (objc != 7) {
        Tcl_WrongNumArgs(interp, 1, objv, "src dst x y w h");
        return TCL_ERROR;
    }
    Tk_PhotoHandle src = resolve_photo(interp, objv[1]);
    if (!src) return TCL_ERROR;
    Tk_PhotoHandle dst = resolve_photo(interp, objv[2]);
    if (!dst) return TCL_ERROR;

    int x_i, y_i, w_i, h_i;
    if (Tcl_GetIntFromObj(interp, objv[3], &x_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[4], &y_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[5], &w_i) != TCL_OK) return TCL_ERROR;
    if (Tcl_GetIntFromObj(interp, objv[6], &h_i) != TCL_OK) return TCL_ERROR;
    if (w_i <= 0 || h_i <= 0)
        return tcw_raise_invalid(interp, "crop width and height must be positive");

    uint32_t in_w, in_h;
    uint8_t* in_rgba = photo_to_rgba(src, &in_w, &in_h);
    if (!in_rgba) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("src photo is empty or oom", -1));
        Tcl_SetErrorCode(interp, "TCLWUFFS", "OOM", NULL);
        return TCL_ERROR;
    }

    uint8_t* out = NULL;
    tcw_err err = {0};
    int rc = tcw_crop(in_w, in_h, in_rgba,
                      (int32_t)x_i, (int32_t)y_i,
                      (uint32_t)w_i, (uint32_t)h_i, &out, &err);
    tcw_free(in_rgba);
    if (rc != TCW_OK) return tcw_raise(interp, &err);

    int tk_rc = put_rgba_into_photo(interp, dst,
                                    (uint32_t)w_i, (uint32_t)h_i, out);
    tcw_free(out);
    return tk_rc;
}

/* package init */

#ifndef DLLEXPORT
#  ifdef _WIN32
#    define DLLEXPORT __declspec(dllexport)
#  else
#    define DLLEXPORT
#  endif
#endif

DLLEXPORT int Tkwuffs_Init(Tcl_Interp* interp) {
    if (Tcl_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
    if (Tk_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
    tcw_register_tk_formats();
    Tcl_CreateNamespace(interp, "::tkwuffs", NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tkwuffs::decode_to_photo",
                         decode_to_photo_cmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tkwuffs::encode_png_from_photo",
                         encode_png_from_photo_cmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tkwuffs::resize_photo",
                         resize_photo_cmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::tkwuffs::crop_photo",
                         crop_photo_cmd, NULL, NULL);
    if (Tcl_PkgProvide(interp, "tkwuffs", PACKAGE_VERSION) != TCL_OK)
        return TCL_ERROR;
    return TCL_OK;
}
