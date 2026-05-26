#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/wuffs_glue.h"

static uint8_t* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(void) {
    size_t in_len = 0;
    uint8_t* in_bytes = slurp("tests/bricks-color.png", &in_len);
    if (!in_bytes) { fprintf(stderr, "open input failed\n"); return 99; }

    printf("sniff: %s\n", tcw_sniff(in_bytes, in_len));

    uint32_t w, h;
    uint8_t* pix = NULL;
    tcw_err err = {0};
    int rc = tcw_decode(in_bytes, in_len, &w, &h, &pix, &err);
    free(in_bytes);
    if (rc != TCW_OK) {
        fprintf(stderr, "decode failed rc=%d: %s\n", rc, err.msg);
        return 1;
    }
    printf("decoded %ux%u rgba[0..3]=%02x %02x %02x %02x\n",
           w, h, pix[0], pix[1], pix[2], pix[3]);

    uint8_t* png_out = NULL;
    size_t png_out_len = 0;
    rc = tcw_encode_png(w, h, pix, &png_out, &png_out_len, &err);
    if (rc != TCW_OK) {
        fprintf(stderr, "encode failed rc=%d: %s\n", rc, err.msg);
        free(pix);
        return 2;
    }
    printf("encoded PNG: %zu bytes, first 8: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           png_out_len, png_out[0], png_out[1], png_out[2], png_out[3],
           png_out[4], png_out[5], png_out[6], png_out[7]);

    uint8_t* jpg_out = NULL;
    size_t jpg_out_len = 0;
    rc = tcw_encode_jpeg(w, h, pix, 85, &jpg_out, &jpg_out_len, &err);
    if (rc != TCW_OK) {
        fprintf(stderr, "encode_jpeg failed rc=%d: %s\n", rc, err.msg);
        free(pix); free(png_out);
        return 4;
    }
    printf("encoded JPEG: %zu bytes, SOI=%02x %02x sniff=%s\n",
           jpg_out_len, jpg_out[0], jpg_out[1],
           tcw_sniff(jpg_out, jpg_out_len));
    free(jpg_out);

    /* Resize 1x1 to 4x4 */
    uint8_t* resized = NULL;
    rc = tcw_resize(w, h, pix, 4, 4, NULL, &resized, &err);
    if (rc != TCW_OK) {
        fprintf(stderr, "resize failed rc=%d: %s\n", rc, err.msg);
        free(pix); free(png_out);
        return 3;
    }
    printf("resized 1x1->4x4 ok, top-left rgba=%02x %02x %02x %02x\n",
           resized[0], resized[1], resized[2], resized[3]);

    free(resized);
    free(pix);
    free(png_out);
    return 0;
}
