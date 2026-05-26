#ifndef TCLWUFFS_TCL_ERR_H
#define TCLWUFFS_TCL_ERR_H

#include <stdarg.h>
#include <stdio.h>
#include <tcl.h>
#include "wuffs_glue.h"

/* Shared between tcl_bindings.c and tk_bindings.c: maps a tcw_err onto a
 * Tcl error with -errorcode {TCLWUFFS <CATEGORY> ...}. */

static inline const char* tcw_err_category(int code) {
    switch (code) {
        case TCW_ERR_INVALID_INPUT:   return "INVALID_INPUT";
        case TCW_ERR_OOM:             return "OOM";
        case TCW_ERR_UNSUPPORTED_FMT: return "UNSUPPORTED_FORMAT";
        case TCW_ERR_DECODE:          return "DECODE";
        case TCW_ERR_ENCODE:          return "ENCODE";
        case TCW_ERR_RESIZE:          return "RESIZE";
        case TCW_ERR_TOO_LARGE:       return "TOO_LARGE";
        default:                      return "INTERNAL";
    }
}

static inline int tcw_raise(Tcl_Interp* interp, const tcw_err* err) {
    const char* cat = tcw_err_category(err->code);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(err->msg[0] ? err->msg : cat, -1));
    Tcl_SetErrorCode(interp, "TCLWUFFS", cat, NULL);
    return TCL_ERROR;
}

/* Convenience for argument-validation failures inside binding commands.
 * Caller passes printf-style fmt + args; we set -errorcode INVALID_INPUT. */
static inline int tcw_raise_invalid(Tcl_Interp* interp, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
    Tcl_SetErrorCode(interp, "TCLWUFFS", "INVALID_INPUT", NULL);
    return TCL_ERROR;
}

#endif
