/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Avatar v1: expand a Telegram "stripped thumb" (the ~30-40 byte inline JPEG
 * skeleton in userProfilePhoto/chatPhoto) into a real baseline JPEG, decode it
 * with the vendored TJpgDec, and scale to a small RGB square. The 623-byte
 * header template and the [164]=h/[166]=w patch rule are byte-exact from
 * tdesktop (via Telethon utils.stripped_photo_to_jpg); invariants are asserted
 * in tg_avatar_self_test.
 */

#include <stdlib.h>
#include <string.h>
#include "tg_avatar.h"
#include "tg_file.h"
#include "../third_party/tjpgd/tjpgd.h"

static const unsigned char tg_avatar_jpeg_header[623] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x28, 0x1c, 0x1e, 0x23, 0x1e, 0x19, 0x28, 0x23, 0x21, 0x23, 0x2d,
    0x2b, 0x28, 0x30, 0x3c, 0x64, 0x41, 0x3c, 0x37, 0x37, 0x3c, 0x7b, 0x58,
    0x5d, 0x49, 0x64, 0x91, 0x80, 0x99, 0x96, 0x8f, 0x80, 0x8c, 0x8a, 0xa0,
    0xb4, 0xe6, 0xc3, 0xa0, 0xaa, 0xda, 0xad, 0x8a, 0x8c, 0xc8, 0xff, 0xcb,
    0xda, 0xee, 0xf5, 0xff, 0xff, 0xff, 0x9b, 0xc1, 0xff, 0xff, 0xff, 0xfa,
    0xff, 0xe6, 0xfd, 0xff, 0xf8, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x2b, 0x2d,
    0x2d, 0x3c, 0x35, 0x3c, 0x76, 0x41, 0x41, 0x76, 0xf8, 0xa5, 0x8c, 0xa5,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8,
    0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xf8,
    0xf8, 0xf8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
    0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
    0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
    0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
    0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
    0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00,
    0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00
};

int tg_avatar_expand_stripped(const unsigned char *stripped,
                              unsigned long stripped_len,
                              unsigned char *out, unsigned long out_cap,
                              unsigned long *out_len)
{
    unsigned long need;

    if (stripped == 0 || out == 0 || out_len == 0 || stripped_len < 3UL ||
        stripped[0] != 0x01U) {
        return 1;
    }
    need = sizeof(tg_avatar_jpeg_header) + (stripped_len - 3UL) + 2UL;
    if (need > out_cap) {
        return 1;
    }
    memcpy(out, tg_avatar_jpeg_header, sizeof(tg_avatar_jpeg_header));
    out[164] = stripped[1]; /* height low byte */
    out[166] = stripped[2]; /* width  low byte */
    memcpy(out + sizeof(tg_avatar_jpeg_header), stripped + 3,
           stripped_len - 3UL);
    out[need - 2UL] = 0xffU;
    out[need - 1UL] = 0xd9U;
    *out_len = need;
    return 0;
}

/* tjpgd input/output plumbing: decode FROM a memory buffer INTO a fixed RGB888
   frame capped at TG_AVATAR_SRC_MAX^2 (stripped thumbs are ~40 px). */
typedef struct tg_avatar_io {
    const unsigned char *data;
    unsigned long size;
    unsigned long pos;
    unsigned char *rgb;   /* TG_AVATAR_SRC_MAX * TG_AVATAR_SRC_MAX * 3 */
    unsigned int w;
    unsigned int h;
    unsigned int stride;
} tg_avatar_io;

static size_t tg_avatar_in(JDEC *jd, uint8_t *buf, size_t len)
{
    tg_avatar_io *io = (tg_avatar_io *)jd->device;

    if (io->pos + len > io->size) {
        len = (size_t)(io->size - io->pos);
    }
    if (buf != 0 && len > 0) {
        memcpy(buf, io->data + io->pos, len);
    }
    io->pos += len;
    return len;
}

static int tg_avatar_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    tg_avatar_io *io = (tg_avatar_io *)jd->device;
    const unsigned char *src = (const unsigned char *)bitmap;
    unsigned int y;
    unsigned int x;

    for (y = rect->top; y <= rect->bottom; ++y) {
        for (x = rect->left; x <= rect->right; ++x) {
            if (x < io->w && y < io->h) {
                unsigned char *d = io->rgb +
                    (((unsigned long)y * io->stride + x) * 3UL);
                d[0] = src[0];
                d[1] = src[1];
                d[2] = src[2];
            }
            src += 3;
        }
    }
    return 1;
}

static void tg_image_scale_axis(unsigned int pos,
                                unsigned int dst_size,
                                unsigned int src_size,
                                int *index0,
                                int *index1,
                                unsigned long *fraction)
{
    unsigned long numerator;
    unsigned long denominator;
    unsigned long whole;
    unsigned long remainder;

    numerator = 0UL;
    denominator = 1UL;
    if (dst_size > 1U && src_size > 1U) {
        numerator = (unsigned long)pos * (unsigned long)(src_size - 1U);
        denominator = (unsigned long)(dst_size - 1U);
    }
    whole = numerator / denominator;
    remainder = numerator % denominator;
    *index0 = (int)whole;
    if (index1 != 0) {
        *index1 = *index0 + 1 < (int)src_size ? *index0 + 1 : *index0;
    }
    *fraction = (remainder * 65536UL) / denominator;
}

static int tg_image_scale_rgb_bilinear_stride(
    const unsigned char *src_rgb, int sw, int sh, int src_stride,
    unsigned char *dst_rgb, int dw, int dh)
{
    int *x_index;
    unsigned short *x_fraction;
    int y;
    int x;

    if (src_rgb == 0 || dst_rgb == 0 || sw <= 0 || sh <= 0 ||
        src_stride < sw || dw <= 0 || dh <= 0 ||
        sw > 65535 || sh > 65535 || dw > 65535 || dh > 65535) {
        return 1;
    }
    x_index = (int *)malloc((size_t)dw * sizeof(*x_index));
    x_fraction =
        (unsigned short *)malloc((size_t)dw * sizeof(*x_fraction));
    if (x_index == 0 || x_fraction == 0) {
        free(x_index);
        free(x_fraction);
        return 1;
    }
    for (x = 0; x < dw; ++x) {
        unsigned long fx;

        tg_image_scale_axis((unsigned int)x, (unsigned int)dw,
                            (unsigned int)sw, &x_index[x], 0, &fx);
        x_fraction[x] = (unsigned short)fx;
    }
    for (y = 0; y < dh; ++y) {
        unsigned long fy;
        int y0;
        int y1;

        tg_image_scale_axis((unsigned int)y, (unsigned int)dh,
                            (unsigned int)sh, &y0, &y1, &fy);
        for (x = 0; x < dw; ++x) {
            int x0;
            int x1;
            unsigned long fx;
            int c;

            x0 = x_index[x];
            x1 = x0 + 1 < sw ? x0 + 1 : x0;
            fx = (unsigned long)x_fraction[x];
            for (c = 0; c < 3; ++c) {
                unsigned long top;
                unsigned long bottom;
                unsigned long value;
                const unsigned char *row0;
                const unsigned char *row1;

                row0 = src_rgb +
                    ((unsigned long)y0 * (unsigned long)src_stride * 3UL);
                row1 = src_rgb +
                    ((unsigned long)y1 * (unsigned long)src_stride * 3UL);
                top = ((unsigned long)row0[x0 * 3 + c] *
                           (65536UL - fx) +
                       (unsigned long)row0[x1 * 3 + c] * fx + 32768UL) >> 16;
                bottom = ((unsigned long)row1[x0 * 3 + c] *
                              (65536UL - fx) +
                          (unsigned long)row1[x1 * 3 + c] * fx +
                          32768UL) >> 16;
                value = (top * (65536UL - fy) + bottom * fy + 32768UL) >> 16;
                dst_rgb[(((unsigned long)y * (unsigned long)dw +
                          (unsigned long)x) * 3UL) + (unsigned long)c] =
                    (unsigned char)value;
            }
        }
    }
    free(x_index);
    free(x_fraction);
    return 0;
}

int tg_image_scale_rgb_bilinear(const unsigned char *src_rgb,
                                int sw, int sh,
                                unsigned char *dst_rgb,
                                int dw, int dh)
{
    return tg_image_scale_rgb_bilinear_stride(src_rgb, sw, sh, sw,
                                               dst_rgb, dw, dh);
}

#define TG_IMAGE_CANON_CACHE_HEADER 20U
#define TG_IMAGE_CANON_CACHE_VERSION 2U
#define TG_IMAGE_CANON_CACHE_RGB888 1U

static void tg_image_cache_put_u32(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value & 0xffUL);
    p[1] = (unsigned char)((value >> 8) & 0xffUL);
    p[2] = (unsigned char)((value >> 16) & 0xffUL);
    p[3] = (unsigned char)((value >> 24) & 0xffUL);
}

static unsigned long tg_image_cache_get_u32(const unsigned char *p)
{
    return (unsigned long)p[0] |
           ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

int tg_image_canonical_cache_prepare(FILE *file, int expected_w,
                                     int expected_h,
                                     unsigned long *payload_size)
{
    unsigned char header[TG_IMAGE_CANON_CACHE_HEADER];
    unsigned long stored_w;
    unsigned long stored_h;
    unsigned long stored_size;
    unsigned long expected_size;
    long file_size;

    if (file == 0 || expected_w <= 0 || expected_h <= 0 ||
        expected_w > 4096 || expected_h > 4096 ||
        payload_size == 0) {
        return 1;
    }
    if (fseek(file, 0L, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return 1;
    }
    stored_w = tg_image_cache_get_u32(header + 8);
    stored_h = tg_image_cache_get_u32(header + 12);
    stored_size = tg_image_cache_get_u32(header + 16);
    expected_size = (unsigned long)expected_w *
                    (unsigned long)expected_h * 3UL;
    if (header[0] != (unsigned char)'T' ||
        header[1] != (unsigned char)'G' ||
        header[2] != (unsigned char)'P' ||
        header[3] != (unsigned char)'C' ||
        header[4] != TG_IMAGE_CANON_CACHE_VERSION ||
        header[5] != TG_IMAGE_CANON_CACHE_RGB888 ||
        stored_w != (unsigned long)expected_w ||
        stored_h != (unsigned long)expected_h ||
        stored_size != expected_size) {
        return 1;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        return 1;
    }
    file_size = ftell(file);
    if (file_size < 0L ||
        (unsigned long)file_size !=
            (unsigned long)TG_IMAGE_CANON_CACHE_HEADER + stored_size ||
        fseek(file, (long)TG_IMAGE_CANON_CACHE_HEADER, SEEK_SET) != 0) {
        return 1;
    }
    *payload_size = stored_size;
    return 0;
}

int tg_image_canonical_cache_write(const char *path,
                                   const unsigned char *rgb,
                                   int w, int h)
{
    unsigned char header[TG_IMAGE_CANON_CACHE_HEADER];
    unsigned long payload_size;
    char part_path[256];
    FILE *file;
    int ok;
    int close_rc;

    if (path == 0 || rgb == 0 || w <= 0 || h <= 0 ||
        w > 4096 || h > 4096 ||
        strlen(path) + 5U >= sizeof(part_path)) {
        return 1;
    }
    payload_size = (unsigned long)w * (unsigned long)h * 3UL;
    memset(header, 0, sizeof(header));
    header[0] = (unsigned char)'T';
    header[1] = (unsigned char)'G';
    header[2] = (unsigned char)'P';
    header[3] = (unsigned char)'C';
    header[4] = TG_IMAGE_CANON_CACHE_VERSION;
    header[5] = TG_IMAGE_CANON_CACHE_RGB888;
    tg_image_cache_put_u32(header + 8, (unsigned long)w);
    tg_image_cache_put_u32(header + 12, (unsigned long)h);
    tg_image_cache_put_u32(header + 16, payload_size);
    sprintf(part_path, "%s.tmp", path);
    (void)remove(part_path);
    file = tg_file_fopen_replace(part_path, "wb");
    if (file == 0) {
        return 1;
    }
    ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
         fwrite(rgb, 1, payload_size, file) == payload_size;
    close_rc = fclose(file);
    if (close_rc != 0) {
        ok = 0;
    }
    if (ok) {
        (void)remove(path); /* AmigaDOS Rename does not replace a target. */
        ok = rename(part_path, path) == 0;
    }
    if (!ok) {
        (void)remove(part_path);
        return 1;
    }
    return 0;
}

int tg_image_canonical_cache_read(const char *path,
                                  unsigned char *rgb,
                                  unsigned long rgb_cap,
                                  int expected_w, int expected_h)
{
    FILE *file;
    unsigned long payload_size;
    int ok;

    if (path == 0 || rgb == 0) {
        return 1;
    }
    file = fopen(path, "rb");
    if (file == 0) {
        return 1;
    }
    ok = tg_image_canonical_cache_prepare(
             file, expected_w, expected_h, &payload_size) == 0 &&
         payload_size <= rgb_cap &&
         fread(rgb, 1, payload_size, file) == payload_size;
    fclose(file);
    return ok ? 0 : 1;
}

int tg_avatar_decode_jpeg(const unsigned char *jpeg, unsigned long jpeg_len,
                          unsigned char *dst_rgb, int dw, int dh)
{
    static unsigned char work[3500 + 512];
    static unsigned char src_rgb[TG_AVATAR_SRC_MAX * TG_AVATAR_SRC_MAX * 3];
    tg_avatar_io io;
    JDEC jd;
    unsigned int scale;

    if (jpeg == 0 || jpeg_len == 0UL || dst_rgb == 0 || dw <= 0 || dh <= 0) {
        return 1;
    }
    memset(&io, 0, sizeof(io));
    memset(src_rgb, 0, sizeof(src_rgb));
    io.data = jpeg;
    io.size = jpeg_len;
    io.rgb = src_rgb;
    io.stride = TG_AVATAR_SRC_MAX;
    if (jd_prepare(&jd, tg_avatar_in, work, sizeof(work), &io) != JDR_OK) {
        return 1;
    }
    /* Smallest 1/2^scale output that fits the source frame: a 160px avatar
       decodes at 1/4 (40px), a stripped thumb (~40px) at 1/1. */
    for (scale = 0; scale <= 3; ++scale) {
        if ((jd.width >> scale) <= TG_AVATAR_SRC_MAX &&
            (jd.height >> scale) <= TG_AVATAR_SRC_MAX) {
            break;
        }
    }
    if (scale > 3 || (jd.width >> scale) == 0 || (jd.height >> scale) == 0) {
        return 1;
    }
    io.w = (unsigned int)(jd.width >> scale);
    io.h = (unsigned int)(jd.height >> scale);
    if (jd_decomp(&jd, tg_avatar_out, (uint8_t)scale) != JDR_OK) {
        return 1;
    }
    return tg_image_scale_rgb_bilinear_stride(
        src_rgb, (int)io.w, (int)io.h, TG_AVATAR_SRC_MAX,
        dst_rgb, dw, dh);
}

typedef struct tg_image_jpeg_io {
    const unsigned char *data;
    unsigned long size;
    unsigned long pos;
    unsigned char *dst_rgb;
    unsigned int sw;
    unsigned int sh;
    unsigned int dw;
    unsigned int dh;
    int ready_rows;
} tg_image_jpeg_io;

struct tg_image_jpeg_decoder {
    JDEC jd;
    tg_image_jpeg_io io;
    unsigned char *work;
    unsigned char *resample_rgb;
    unsigned char *final_rgb;
    int final_w;
    int final_h;
    int bilinear_pending;
    int failed;
};

static size_t tg_image_jpeg_in(JDEC *jd, uint8_t *buf, size_t len)
{
    tg_image_jpeg_io *io;

    io = (tg_image_jpeg_io *)jd->device;
    if (io->pos + len > io->size) {
        len = (size_t)(io->size - io->pos);
    }
    if (buf != 0 && len > 0U) {
        memcpy(buf, io->data + io->pos, len);
    }
    io->pos += (unsigned long)len;
    return len;
}

static unsigned int tg_image_scale_ceil(unsigned int value,
                                        unsigned int dst,
                                        unsigned int src)
{
    return (unsigned int)(((unsigned long)value * (unsigned long)dst +
                           (unsigned long)src - 1UL) /
                          (unsigned long)src);
}

/* Scale each completed tjpgd MCU directly into the canonical destination.
   There is no full-size source frame: every destination pixel belongs to one
   MCU rectangle, so the callbacks fill disjoint regions from top to bottom. */
static int tg_image_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    tg_image_jpeg_io *io;
    const unsigned char *src;
    unsigned int rw;
    unsigned int dx0;
    unsigned int dx1;
    unsigned int dy0;
    unsigned int dy1;
    unsigned int dx;
    unsigned int dy;

    io = (tg_image_jpeg_io *)jd->device;
    src = (const unsigned char *)bitmap;
    rw = (unsigned int)rect->right - (unsigned int)rect->left + 1U;
    dx0 = tg_image_scale_ceil((unsigned int)rect->left, io->dw, io->sw);
    dx1 = tg_image_scale_ceil((unsigned int)rect->right + 1U, io->dw, io->sw);
    dy0 = tg_image_scale_ceil((unsigned int)rect->top, io->dh, io->sh);
    dy1 = tg_image_scale_ceil((unsigned int)rect->bottom + 1U, io->dh, io->sh);
    if (dx1 > io->dw) {
        dx1 = io->dw;
    }
    if (dy1 > io->dh) {
        dy1 = io->dh;
    }
    for (dy = dy0; dy < dy1; ++dy) {
        unsigned int sy;

        sy = (unsigned int)(((unsigned long)dy * io->sh) / io->dh);
        for (dx = dx0; dx < dx1; ++dx) {
            unsigned int sx;
            const unsigned char *s;
            unsigned char *d;

            sx = (unsigned int)(((unsigned long)dx * io->sw) / io->dw);
            s = src + ((((unsigned long)sy - rect->top) * rw +
                        ((unsigned long)sx - rect->left)) * 3UL);
            d = io->dst_rgb +
                ((((unsigned long)dy * io->dw) + dx) * 3UL);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    if ((unsigned int)rect->right + 1U >= io->sw &&
        (int)dy1 > io->ready_rows) {
        io->ready_rows = (int)dy1;
    }
    return 1;
}

static tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin_scale_internal(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int requested_scale, int bilinear_upscale,
    int *actual_scale, int *decode_rc)
{
    tg_image_jpeg_decoder *decoder;
    JRESULT jr;
    unsigned int scale;
    unsigned int sw;
    unsigned int sh;

    if (decode_rc != 0) {
        *decode_rc = (int)JDR_PAR;
    }
    if (actual_scale != 0) {
        *actual_scale = TG_IMAGE_JPEG_SCALE_AUTO;
    }
    if (jpeg == 0 || jpeg_len == 0UL || dst_rgb == 0 || dw <= 0 || dh <= 0 ||
        source_edge_cap <= 0 || source_edge_cap > 1024 ||
        requested_scale < TG_IMAGE_JPEG_SCALE_AUTO || requested_scale > 3) {
        return 0;
    }
    decoder = (tg_image_jpeg_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == 0) {
        if (decode_rc != 0) {
            *decode_rc = (int)JDR_MEM1;
        }
        return 0;
    }
    decoder->work = (unsigned char *)malloc(4012U);
    if (decoder->work == 0) {
        free(decoder);
        if (decode_rc != 0) {
            *decode_rc = (int)JDR_MEM1;
        }
        return 0;
    }
    decoder->io.data = jpeg;
    decoder->io.size = jpeg_len;
    decoder->io.dst_rgb = dst_rgb;
    decoder->io.dw = (unsigned int)dw;
    decoder->io.dh = (unsigned int)dh;
    jr = jd_prepare(&decoder->jd, tg_image_jpeg_in, decoder->work, 4012U,
                    &decoder->io);
    if (jr != JDR_OK) {
        tg_image_jpeg_decoder_destroy(decoder);
        if (decode_rc != 0) {
            *decode_rc = (int)jr;
        }
        return 0;
    }
    sw = sh = 0U;
    scale = requested_scale == TG_IMAGE_JPEG_SCALE_AUTO
        ? 0U : (unsigned int)requested_scale;
    for (; scale <= 3U; ++scale) {
        unsigned int div;
        unsigned int cap_w;
        unsigned int cap_h;

        div = 1U << scale;
        cap_w = ((unsigned int)decoder->jd.width + div - 1U) / div;
        cap_h = ((unsigned int)decoder->jd.height + div - 1U) / div;
        if (cap_w > 0U && cap_h > 0U &&
            cap_w <= (unsigned int)source_edge_cap &&
            cap_h <= (unsigned int)source_edge_cap) {
            sw = (unsigned int)decoder->jd.width >> scale;
            sh = (unsigned int)decoder->jd.height >> scale;
            break;
        }
        if (requested_scale != TG_IMAGE_JPEG_SCALE_AUTO) {
            scale = 4U;
            break;
        }
    }
    if (scale > 3U || sw == 0U || sh == 0U) {
        tg_image_jpeg_decoder_destroy(decoder);
        if (decode_rc != 0) {
            *decode_rc = (int)JDR_PAR;
        }
        return 0;
    }
    decoder->io.sw = sw;
    decoder->io.sh = sh;
    if (bilinear_upscale && (sw < (unsigned int)dw || sh < (unsigned int)dh)) {
        unsigned long pixels;

        pixels = (unsigned long)sw * (unsigned long)sh;
        decoder->resample_rgb =
            (unsigned char *)malloc((size_t)pixels * 3U);
        if (decoder->resample_rgb == 0) {
            tg_image_jpeg_decoder_destroy(decoder);
            if (decode_rc != 0) {
                *decode_rc = (int)JDR_MEM1;
            }
            return 0;
        }
        decoder->final_rgb = dst_rgb;
        decoder->final_w = dw;
        decoder->final_h = dh;
        decoder->bilinear_pending = 1;
        decoder->io.dst_rgb = decoder->resample_rgb;
        decoder->io.dw = sw;
        decoder->io.dh = sh;
    }
    jr = jd_decomp_begin(&decoder->jd, (uint8_t)scale);
    if (jr != JDR_OK) {
        tg_image_jpeg_decoder_destroy(decoder);
        if (decode_rc != 0) {
            *decode_rc = (int)jr;
        }
        return 0;
    }
    if (decode_rc != 0) {
        *decode_rc = (int)JDR_OK;
    }
    if (actual_scale != 0) {
        *actual_scale = (int)scale;
    }
    return decoder;
}

tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin_scale(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int requested_scale, int *actual_scale, int *decode_rc)
{
    return tg_image_jpeg_decoder_begin_scale_internal(
        jpeg, jpeg_len, dst_rgb, dw, dh, source_edge_cap,
        requested_scale, 0, actual_scale, decode_rc);
}

tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin_scale_bilinear(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int requested_scale, int *actual_scale, int *decode_rc)
{
    return tg_image_jpeg_decoder_begin_scale_internal(
        jpeg, jpeg_len, dst_rgb, dw, dh, source_edge_cap,
        requested_scale, 1, actual_scale, decode_rc);
}

tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int *decode_rc)
{
    return tg_image_jpeg_decoder_begin_scale(
        jpeg, jpeg_len, dst_rgb, dw, dh, source_edge_cap,
        TG_IMAGE_JPEG_SCALE_AUTO, 0, decode_rc);
}

int tg_image_jpeg_decoder_step(tg_image_jpeg_decoder *decoder,
                               unsigned int max_mcus,
                               int *ready_rows,
                               int *decode_rc)
{
    JRESULT jr;
    int done;

    if (ready_rows != 0) {
        *ready_rows = decoder != 0 ? decoder->io.ready_rows : 0;
    }
    if (decode_rc != 0) {
        *decode_rc = (int)JDR_PAR;
    }
    if (decoder == 0 || max_mcus == 0U || decoder->failed) {
        return -1;
    }
    done = 0;
    jr = jd_decomp_step(&decoder->jd, tg_image_jpeg_out, max_mcus, &done);
    if (jr != JDR_OK) {
        decoder->failed = 1;
        if (decode_rc != 0) {
            *decode_rc = (int)jr;
        }
        return -1;
    }
    if (done) {
        if (decoder->bilinear_pending) {
            if (tg_image_scale_rgb_bilinear(
                    decoder->resample_rgb,
                    (int)decoder->io.sw, (int)decoder->io.sh,
                    decoder->final_rgb,
                    decoder->final_w, decoder->final_h) != 0) {
                decoder->failed = 1;
                if (decode_rc != 0) {
                    *decode_rc = (int)JDR_FMT3;
                }
                return -1;
            }
            free(decoder->resample_rgb);
            decoder->resample_rgb = 0;
            decoder->bilinear_pending = 0;
            decoder->io.ready_rows = decoder->final_h;
        } else {
            decoder->io.ready_rows = (int)decoder->io.dh;
        }
    }
    if (ready_rows != 0) {
        *ready_rows = decoder->io.ready_rows;
    }
    if (decode_rc != 0) {
        *decode_rc = (int)JDR_OK;
    }
    return done ? 1 : 0;
}

void tg_image_jpeg_decoder_destroy(tg_image_jpeg_decoder *decoder)
{
    if (decoder == 0) {
        return;
    }
    free(decoder->resample_rgb);
    free(decoder->work);
    free(decoder);
}

int tg_image_decode_jpeg_scaled(const unsigned char *jpeg,
                                unsigned long jpeg_len,
                                unsigned char *dst_rgb,
                                int dw, int dh,
                                int source_edge_cap)
{
    tg_image_jpeg_decoder *decoder;
    int ready_rows;
    int decode_rc;
    int step;

    decoder = tg_image_jpeg_decoder_begin(jpeg, jpeg_len, dst_rgb, dw, dh,
                                          source_edge_cap, &decode_rc);
    if (decoder == 0) {
        return 1;
    }
    do {
        step = tg_image_jpeg_decoder_step(decoder, ~0U, &ready_rows,
                                          &decode_rc);
    } while (step == 0);
    tg_image_jpeg_decoder_destroy(decoder);
    return step == 1 ? 0 : 1;
}

int tg_image_decode_jpeg_bilinear_scaled(const unsigned char *jpeg,
                                         unsigned long jpeg_len,
                                         unsigned char *dst_rgb,
                                         int dw, int dh,
                                         int source_edge_cap)
{
    tg_image_jpeg_decoder *decoder;
    int ready_rows;
    int decode_rc;
    int step;

    decoder = tg_image_jpeg_decoder_begin_scale_bilinear(
        jpeg, jpeg_len, dst_rgb, dw, dh, source_edge_cap,
        TG_IMAGE_JPEG_SCALE_AUTO, 0, &decode_rc);
    if (decoder == 0) {
        return 1;
    }
    do {
        step = tg_image_jpeg_decoder_step(decoder, ~0U, &ready_rows,
                                          &decode_rc);
    } while (step == 0);
    tg_image_jpeg_decoder_destroy(decoder);
    return step == 1 ? 0 : 1;
}

int tg_image_canonical_size(unsigned long source_w,
                            unsigned long source_h,
                            int edge_cap,
                            int *out_w,
                            int *out_h)
{
    unsigned long w;
    unsigned long h;

    if (source_w == 0UL || source_h == 0UL || edge_cap <= 0 ||
        out_w == 0 || out_h == 0) {
        return 1;
    }
    w = source_w;
    h = source_h;
    if (w > (unsigned long)edge_cap || h > (unsigned long)edge_cap) {
        if (w >= h) {
            h = (h * (unsigned long)edge_cap) / w;
            w = (unsigned long)edge_cap;
        } else {
            w = (w * (unsigned long)edge_cap) / h;
            h = (unsigned long)edge_cap;
        }
    }
    if (w == 0UL) {
        w = 1UL;
    }
    if (h == 0UL) {
        h = 1UL;
    }
    *out_w = (int)w;
    *out_h = (int)h;
    return 0;
}

void tg_image_ordered_dither_rgb_level(const unsigned char *rgb,
                                       int x, int y, int amplitude,
                                       unsigned char *out_rgb)
{
    static const signed char bayer[16] = {
        -8,  0, -6,  2,
         4, -4,  6, -2,
        -5,  3, -7,  1,
         7, -1,  5, -3
    };
    int offset;
    int c;

    if (rgb == 0 || out_rgb == 0) {
        return;
    }
    if (amplitude < 0) {
        amplitude = 0;
    } else if (amplitude > 4) {
        amplitude = 4;
    }
    offset = (int)bayer[((y & 3) << 2) | (x & 3)] * amplitude;
    for (c = 0; c < 3; ++c) {
        int value;

        value = (int)rgb[c] + offset;
        if (value < 0) {
            value = 0;
        } else if (value > 255) {
            value = 255;
        }
        out_rgb[c] = (unsigned char)value;
    }
}

void tg_image_ordered_dither_rgb(const unsigned char *rgb,
                                 int x, int y,
                                 unsigned char *out_rgb)
{
    tg_image_ordered_dither_rgb_level(rgb, x, y, 4, out_rgb);
}

int tg_avatar_decode_stripped(const unsigned char *stripped,
                              unsigned long stripped_len,
                              unsigned char *dst_rgb, int dw, int dh)
{
    static unsigned char jpeg[900];
    unsigned long jpeg_len;

    if (tg_avatar_expand_stripped(stripped, stripped_len, jpeg, sizeof(jpeg),
                                  &jpeg_len) != 0) {
        return 1;
    }
    return tg_avatar_decode_jpeg(jpeg, jpeg_len, dst_rgb, dw, dh);
}

#if !defined(TG_NO_SELFTEST)
static unsigned long tg_avatar_self_test_hash(const unsigned char *data,
                                               unsigned long size)
{
    unsigned long hash;
    unsigned long i;

    hash = 2166136261UL;
    for (i = 0UL; i < size; ++i) {
        hash ^= (unsigned long)data[i];
        hash = (hash * 16777619UL) & 0xffffffffUL;
    }
    return hash;
}

/* Model the GUI's stripped/1:8/1:4/final frame transitions with deliberately
   unrelated source widths. Each complete RGB frame and its palette replay use
   the same published stride before the next pass replaces it. */
static int tg_avatar_photo_transition_self_test(void)
{
    static const int source_w[4] = { 37, 71, 137, 263 };
    static const int source_h[4] = { 5, 7, 9, 11 };
    static const unsigned long rgb_golden[4] = {
        0x7d063e38UL, 0x3c081814UL, 0xa69e00eaUL, 0x17b7af36UL
    };
    static const unsigned long pen_golden[4] = {
        0x20b4f0ceUL, 0xfc660737UL, 0x69e80bf5UL, 0xf72baeaaUL
    };
    static unsigned char source[263 * 11 * 3];
    static unsigned char frame[449 * 19 * 3];
    static unsigned char rgb_replay[113 * 13 * 3];
    static unsigned char pen_replay[113 * 13];
    int pass;

    for (pass = 0; pass < 4; ++pass) {
        int y;
        int replay_at;
        int pen_at;

        for (y = 0; y < source_h[pass]; ++y) {
            int x;

            for (x = 0; x < source_w[pass]; ++x) {
                unsigned long at;

                at = ((unsigned long)y * (unsigned long)source_w[pass] +
                      (unsigned long)x) * 3UL;
                source[at] = (unsigned char)(x * 17 + y * 3 + pass * 29);
                source[at + 1UL] =
                    (unsigned char)(x * 5 + y * 19 + pass * 31);
                source[at + 2UL] =
                    (unsigned char)(x * 11 + y * 7 + pass * 13);
            }
        }
        if (tg_image_scale_rgb_bilinear(
                source, source_w[pass], source_h[pass],
                frame, 449, 19) != 0) {
            return 0;
        }
        replay_at = 0;
        pen_at = 0;
        for (y = 0; y < 13; ++y) {
            int sy;
            int x;

            sy = (y * 19) / 13;
            for (x = 0; x < 113; ++x) {
                int sx;
                unsigned long at;
                unsigned char r;
                unsigned char g;
                unsigned char b;

                sx = (x * 449) / 113;
                at = ((unsigned long)sy * 449UL + (unsigned long)sx) * 3UL;
                r = frame[at];
                g = frame[at + 1UL];
                b = frame[at + 2UL];
                rgb_replay[replay_at++] = r;
                rgb_replay[replay_at++] = g;
                rgb_replay[replay_at++] = b;
                pen_replay[pen_at++] = (unsigned char)(
                    (r & 0xe0U) | ((g >> 3) & 0x1cU) | (b >> 6));
            }
        }
        if (tg_avatar_self_test_hash(
                rgb_replay, (unsigned long)replay_at) != rgb_golden[pass] ||
            tg_avatar_self_test_hash(
                pen_replay, (unsigned long)pen_at) != pen_golden[pass]) {
            return 0;
        }
    }
    return 1;
}

int tg_avatar_self_test(void)
{
#include "tg_photo_test_fixture.inc"
    static const unsigned char photo_jpeg[] = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
        0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
        0x00, 0x0e, 0x0a, 0x0b, 0x0d, 0x0b, 0x09, 0x0e, 0x0d, 0x0c, 0x0d, 0x10,
        0x0f, 0x0e, 0x11, 0x16, 0x24, 0x17, 0x16, 0x14, 0x14, 0x16, 0x2c, 0x20,
        0x21, 0x1a, 0x24, 0x34, 0x2e, 0x37, 0x36, 0x33, 0x2e, 0x32, 0x32, 0x3a,
        0x41, 0x53, 0x46, 0x3a, 0x3d, 0x4e, 0x3e, 0x32, 0x32, 0x48, 0x62, 0x49,
        0x4e, 0x56, 0x58, 0x5d, 0x5e, 0x5d, 0x38, 0x45, 0x66, 0x6d, 0x65, 0x5a,
        0x6c, 0x53, 0x5b, 0x5d, 0x59, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x0f, 0x10,
        0x10, 0x16, 0x13, 0x16, 0x2a, 0x17, 0x17, 0x2a, 0x59, 0x3b, 0x32, 0x3b,
        0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
        0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
        0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
        0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59,
        0x59, 0x59, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x04, 0x00, 0x08, 0x03,
        0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
        0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xff, 0xc4, 0x00, 0x19,
        0x10, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x05, 0x11, 0x12, 0xff,
        0xc4, 0x00, 0x15, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x06, 0xff, 0xc4,
        0x00, 0x1c, 0x11, 0x00, 0x01, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x04, 0x03,
        0x05, 0x11, 0x12, 0x31, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02,
        0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x93, 0x0b, 0x33, 0x6e, 0x76, 0x00,
        0xf9, 0x14, 0xdb, 0xbf, 0x14, 0xdd, 0xf0, 0x93, 0x31, 0xd9, 0x5f, 0xff,
        0xd9
    };
    static const unsigned char probe[] = { 0x01, 0x08, 0x08, 0xaa, 0xbb };
    unsigned char out[900];
    unsigned long out_len;

    if (!tg_avatar_photo_transition_self_test()) {
        puts("avatar self-test: photo pass stride transition failed");
        return 2;
    }

    /* template invariants (byte-exact from tdesktop/Telethon) */
    if (sizeof(tg_avatar_jpeg_header) != 623 ||
        tg_avatar_jpeg_header[0] != 0xffU ||
        tg_avatar_jpeg_header[1] != 0xd8U ||
        tg_avatar_jpeg_header[158] != 0xffU ||
        tg_avatar_jpeg_header[159] != 0xc0U ||
        tg_avatar_jpeg_header[160] != 0x00U ||
        tg_avatar_jpeg_header[161] != 0x11U ||
        tg_avatar_jpeg_header[162] != 0x08U) {
        puts("avatar self-test: header template invariants broken");
        return 2;
    }
    if (tg_avatar_expand_stripped(probe, sizeof(probe), out, sizeof(out),
                                  &out_len) != 0 ||
        out_len != 623UL + 2UL + 2UL || out[164] != 0x08U ||
        out[166] != 0x08U || out[623] != 0xaaU || out[624] != 0xbbU ||
        out[out_len - 2UL] != 0xffU || out[out_len - 1UL] != 0xd9U) {
        puts("avatar self-test: expansion failed");
        return 2;
    }
    /* reject rules: too short / wrong marker / does not fit */
    if (tg_avatar_expand_stripped(probe, 2UL, out, sizeof(out), &out_len) == 0) {
        puts("avatar self-test: short payload must be rejected");
        return 2;
    }
    {
        int w;
        int h;
        unsigned char scaled[16 * 8 * 3];
        unsigned char native_rgb[8 * 4 * 3];
        unsigned char smooth[16 * 8 * 3];
        unsigned char smooth_expected[16 * 8 * 3];
        static const unsigned char bilinear_src[12] = {
            0U, 0U, 0U,       255U, 0U, 0U,
            0U, 255U, 0U,     255U, 255U, 255U
        };
        unsigned char bilinear_dst[3 * 3 * 3];
        unsigned char cache_roundtrip[12];
        unsigned char rgb[3];
        unsigned char dithered[3];

        if (tg_image_canonical_size(4000UL, 3000UL, 256, &w, &h) != 0 ||
            w != 256 || h != 192 ||
            tg_image_canonical_size(100UL, 200UL, 256, &w, &h) != 0 ||
            w != 100 || h != 200 ||
            tg_image_canonical_size(0UL, 200UL, 256, &w, &h) == 0 ||
            tg_image_decode_jpeg_scaled(photo_jpeg, sizeof(photo_jpeg),
                                        scaled, 16, 8, 32) != 0 ||
            memcmp(scaled, scaled + ((16 * 8 - 1) * 3), 3) == 0) {
            puts("avatar self-test: canonical photo geometry failed");
            return 2;
        }
        if (tg_image_decode_jpeg_scaled(
                photo_jpeg, sizeof(photo_jpeg), native_rgb, 8, 4, 32) != 0 ||
            tg_image_scale_rgb_bilinear(
                native_rgb, 8, 4, smooth_expected, 16, 8) != 0 ||
            tg_image_decode_jpeg_bilinear_scaled(
                photo_jpeg, sizeof(photo_jpeg), smooth, 16, 8, 32) != 0 ||
            memcmp(smooth, smooth_expected, sizeof(smooth)) != 0 ||
            memcmp(smooth, scaled, sizeof(smooth)) == 0) {
            puts("avatar self-test: filtered final upscale failed");
            return 2;
        }
        if (tg_image_scale_rgb_bilinear(
                bilinear_src, 2, 2, bilinear_dst, 3, 3) != 0 ||
            bilinear_dst[(4 * 3)] != 128U ||
            bilinear_dst[(4 * 3) + 1] != 128U ||
            bilinear_dst[(4 * 3) + 2] != 64U ||
            memcmp(bilinear_dst, bilinear_src, 3U) != 0 ||
            memcmp(bilinear_dst + (8 * 3), bilinear_src + 9, 3U) != 0) {
            puts("avatar self-test: bilinear photo scaling failed");
            return 2;
        }
        (void)remove("tg-photo-cache-selftest.pgc");
        if (tg_image_canonical_cache_write(
                "tg-photo-cache-selftest.pgc", bilinear_src, 2, 2) != 0 ||
            tg_image_canonical_cache_read(
                "tg-photo-cache-selftest.pgc", cache_roundtrip,
                sizeof(cache_roundtrip), 2, 2) != 0 ||
            memcmp(cache_roundtrip, bilinear_src,
                   sizeof(cache_roundtrip)) != 0) {
            (void)remove("tg-photo-cache-selftest.pgc");
            puts("avatar self-test: canonical cache roundtrip failed");
            return 2;
        }
        {
            FILE *bad;
            int bad_ok;

            bad = tg_file_fopen_replace("tg-photo-cache-selftest.pgc", "wb");
            if (bad == 0) {
                (void)remove("tg-photo-cache-selftest.pgc");
                puts("avatar self-test: corrupt cache setup failed");
                return 2;
            }
            bad_ok = fwrite("bad", 1, 3U, bad) == 3U;
            if (fclose(bad) != 0) {
                bad_ok = 0;
            }
            if (!bad_ok || tg_image_canonical_cache_read(
                    "tg-photo-cache-selftest.pgc", cache_roundtrip,
                    sizeof(cache_roundtrip), 2, 2) == 0) {
                (void)remove("tg-photo-cache-selftest.pgc");
                puts("avatar self-test: corrupt canonical cache accepted");
                return 2;
            }
        }
        (void)remove("tg-photo-cache-selftest.pgc");
        rgb[0] = 1U;
        rgb[1] = 128U;
        rgb[2] = 254U;
        tg_image_ordered_dither_rgb(rgb, 0, 0, dithered);
        if (dithered[0] != 0U || dithered[1] != 96U ||
            dithered[2] != 222U) {
            puts("avatar self-test: ordered photo dither failed");
            return 2;
        }
        tg_image_ordered_dither_rgb(rgb, 0, 3, dithered);
        if (dithered[0] != 29U || dithered[1] != 156U ||
            dithered[2] != 255U) {
            puts("avatar self-test: ordered photo dither clamp failed");
            return 2;
        }
        tg_image_ordered_dither_rgb_level(rgb, 0, 0, 2, dithered);
        if (dithered[0] != 0U || dithered[1] != 112U ||
            dithered[2] != 238U) {
            puts("avatar self-test: light photo dither failed");
            return 2;
        }
        tg_image_ordered_dither_rgb_level(rgb, 0, 0, 0, dithered);
        if (memcmp(rgb, dithered, 3U) != 0) {
            puts("avatar self-test: disabled photo dither failed");
            return 2;
        }
    }
    {
        static unsigned char progressive[32 * 32 * 3];
        static unsigned char complete[32 * 32 * 3];
        static unsigned char quality[32 * 32 * 3];
        tg_image_jpeg_decoder *decoder;
        int ready_rows;
        int previous_rows;
        int decode_rc;
        int step_rc;
        int steps;
        int saw_partial;

        memset(progressive, 0, sizeof(progressive));
        decoder = tg_image_jpeg_decoder_begin(
            tg_progress_jpeg, sizeof(tg_progress_jpeg), progressive,
            32, 32, 64, &decode_rc);
        if (decoder == 0) {
            puts("avatar self-test: progressive JPEG begin failed");
            return 2;
        }
        ready_rows = previous_rows = steps = saw_partial = 0;
        do {
            step_rc = tg_image_jpeg_decoder_step(
                decoder, 1U, &ready_rows, &decode_rc);
            ++steps;
            if (ready_rows < previous_rows || ready_rows > 32 || steps > 64) {
                tg_image_jpeg_decoder_destroy(decoder);
                puts("avatar self-test: progressive JPEG state mismatch");
                return 2;
            }
            if (ready_rows > 0 && ready_rows < 32) {
                saw_partial = 1;
            }
            previous_rows = ready_rows;
        } while (step_rc == 0);
        tg_image_jpeg_decoder_destroy(decoder);
        if (step_rc != 1 || ready_rows != 32 || !saw_partial || steps <= 1 ||
            tg_image_decode_jpeg_scaled(
                tg_progress_jpeg, sizeof(tg_progress_jpeg), complete,
                32, 32, 64) != 0 ||
            memcmp(progressive, complete, sizeof(progressive)) != 0) {
            puts("avatar self-test: progressive JPEG result mismatch");
            return 2;
        }
        /* The GUI's browser-style quality sequence must converge byte-for-byte
           to the ordinary finest-scale decode. Every pass is still sliced. */
        {
            static const int pass_scale[3] = { 3, 2, 0 };
            int pass;

            memset(quality, 0, sizeof(quality));
            for (pass = 0; pass < 3; ++pass) {
                int actual_scale;

                decoder = tg_image_jpeg_decoder_begin_scale(
                    tg_progress_jpeg, sizeof(tg_progress_jpeg), quality,
                    32, 32, 64, pass_scale[pass], &actual_scale, &decode_rc);
                if (decoder == 0 || actual_scale != pass_scale[pass]) {
                    tg_image_jpeg_decoder_destroy(decoder);
                    puts("avatar self-test: quality pass begin failed");
                    return 2;
                }
                do {
                    step_rc = tg_image_jpeg_decoder_step(
                        decoder, 1U, &ready_rows, &decode_rc);
                } while (step_rc == 0);
                tg_image_jpeg_decoder_destroy(decoder);
                if (step_rc != 1) {
                    puts("avatar self-test: quality pass decode failed");
                    return 2;
                }
            }
            if (memcmp(quality, complete, sizeof(quality)) != 0) {
                puts("avatar self-test: quality passes do not converge");
                return 2;
            }
        }
    }
    puts("avatar self-test: ok (template 623 bytes, patch h/w, FFD9 tail)");
    return 0;
}
#endif /* !TG_NO_SELFTEST */
