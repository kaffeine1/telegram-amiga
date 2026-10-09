/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */
#ifndef TG_AVATAR_H
#define TG_AVATAR_H

#include <stdio.h>

/* Largest source thumb we decode (stripped thumbs are ~40 px). */
#define TG_AVATAR_SRC_MAX 64

/* Expands a stripped thumb into a real baseline JPEG (header template + payload
   + FFD9). 0 = ok. */
int tg_avatar_expand_stripped(const unsigned char *stripped,
                              unsigned long stripped_len,
                              unsigned char *out, unsigned long out_cap,
                              unsigned long *out_len);

/* Decode a whole baseline JPEG (e.g. the downloaded 160px avatar), picking the
   largest 1/2^k tjpgd scale that fits TG_AVATAR_SRC_MAX, or a coarser one
   that still covers dw x dh, then scale into dst_rgb (dw*dh*3, RGB888).
   0 = ok. */
int tg_avatar_decode_jpeg(const unsigned char *jpeg, unsigned long jpeg_len,
                          unsigned char *dst_rgb, int dw, int dh);

/* Index of the palette entry nearest to rgb by squared RGB distance (the
   first of equals wins) and that distance; -1 for an empty palette. pal holds
   n RGB triples. Kept here, with the decoder, so the 68k builds compile this
   per-pixel loop at -O2 too: at -O0 it was 1.6 s an avatar on a stock A1200. */
int tg_avatar_nearest(const unsigned char *pal, int n,
                      const unsigned char *rgb, long *out_d);

/* Colour choice for small palettes (the emoji on AGA/ECS): a distance that
   counts a change of tint (chroma) three times a change of brightness. */
long tg_avatar_tint_distance(const unsigned char *a, const unsigned char *b);
/* Groups the used entries of a palette (weight > 0, n <= 256) into shades,
   heaviest first: each entry joins the first group whose founding colour is
   closer than `same`, or founds one (up to max_groups <= 64; past that it
   joins the nearest). group_of[i] gets the group (-1 when unused); per group,
   in descending total weight, group_rgb its weighted mean colour and
   group_weight its weight. Returns the number of groups. */
int tg_avatar_tint_groups(const unsigned char *pal,
                          const unsigned long *weight, int n, long same,
                          int max_groups, int *group_of,
                          unsigned char *group_rgb,
                          unsigned long *group_weight);
/* Nearest entry by tint distance among those with usable[i] set (all when
   usable is 0); -1 when none. */
int tg_avatar_nearest_tint(const unsigned char *pal,
                           const unsigned char *usable, int n,
                           const unsigned char *rgb, long *out_d);
/* Among the k (<= 8) nearest usable entries, the pair whose half-and-half mix
   (a checkerboard of the two) is nearest to rgb: its distance, or -1 when
   fewer than two entries are usable. */
long tg_avatar_best_tint_pair(const unsigned char *pal,
                              const unsigned char *usable, int n,
                              const unsigned char *rgb, int k, int *pa,
                              int *pb);

/* General message-photo path: decode a baseline JPEG with tjpgd and scale it
   into caller-owned RGB888. Intermediate memory is allocated only for the
   duration of the decode and bounded by source_edge_cap. */
int tg_image_decode_jpeg_scaled(const unsigned char *jpeg,
                                unsigned long jpeg_len,
                                unsigned char *dst_rgb,
                                int dw, int dh,
                                int source_edge_cap);

/* Resumable variant used by the GUI. The input JPEG and destination RGB buffer
   must remain valid until destroy. Each step decodes at most max_mcus and
   returns 0=more, 1=done, -1=error. ready_rows grows monotonically; the GUI
   keeps this staging output hidden until a complete quality pass is ready. */
typedef struct tg_image_jpeg_decoder tg_image_jpeg_decoder;
#define TG_IMAGE_JPEG_SCALE_AUTO (-1)
tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int *decode_rc);
/* Explicit tjpgd scale variant: 0=full, 1=1/2, 2=1/4, 3=1/8, -1=the finest
   scale that fits source_edge_cap. actual_scale receives the selected value. */
tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin_scale(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int requested_scale, int *actual_scale, int *decode_rc);
/* Same state machine, with bilinear replacement only when the selected JPEG
   scale is smaller than the destination. */
tg_image_jpeg_decoder *tg_image_jpeg_decoder_begin_scale_bilinear(
    const unsigned char *jpeg, unsigned long jpeg_len,
    unsigned char *dst_rgb, int dw, int dh, int source_edge_cap,
    int requested_scale, int *actual_scale, int *decode_rc);
int tg_image_jpeg_decoder_step(tg_image_jpeg_decoder *decoder,
                               unsigned int max_mcus,
                               int *ready_rows,
                               int *decode_rc);
void tg_image_jpeg_decoder_destroy(tg_image_jpeg_decoder *decoder);

/* Fit source dimensions inside a square canonical edge without changing the
   aspect ratio or enlarging a smaller image. 0 = valid dimensions returned. */
int tg_image_canonical_size(unsigned long source_w,
                            unsigned long source_h,
                            int edge_cap,
                            int *out_w,
                            int *out_h);

/* Bilinear RGB888 resize. Source and destination are tightly packed; unlike
   the cheap replay scaler this is intended for one-time thumbnail/canonical
   preparation, never for paint. 0 = ok. */
int tg_image_scale_rgb_bilinear(const unsigned char *src_rgb,
                                int sw, int sh,
                                unsigned char *dst_rgb,
                                int dw, int dh);
int tg_image_decode_jpeg_bilinear_scaled(const unsigned char *jpeg,
                                         unsigned long jpeg_len,
                                         unsigned char *dst_rgb,
                                         int dw, int dh,
                                         int source_edge_cap);

/* Versioned RGB888 canonical-photo cache. The file helper validates magic,
   format, dimensions and exact payload size, leaving FILE positioned at the
   first pixel so retro backends can read it in bounded chunks. */
int tg_image_canonical_cache_prepare(FILE *file, int expected_w,
                                     int expected_h,
                                     unsigned long *payload_size);
int tg_image_canonical_cache_write(const char *path,
                                   const unsigned char *rgb,
                                   int w, int h);
int tg_image_canonical_cache_read(const char *path,
                                  unsigned char *rgb,
                                  unsigned long rgb_cap,
                                  int expected_w, int expected_h);

/* Apply the 4x4 ordered-dither offset used before palette matching. */
void tg_image_ordered_dither_rgb(const unsigned char *rgb,
                                 int x, int y,
                                 unsigned char *out_rgb);
/* Same Bayer matrix with an explicit amplitude: 4 = full, 2 = light,
   0 = disabled. Values outside 0..4 are clamped. */
void tg_image_ordered_dither_rgb_level(const unsigned char *rgb,
                                       int x, int y, int amplitude,
                                       unsigned char *out_rgb);

/* Expand + decode + nearest-neighbour scale into dst_rgb (dw*dh*3, RGB888).
   0 = ok; any failure leaves the caller free to fall back to initials. */
int tg_avatar_decode_stripped(const unsigned char *stripped,
                              unsigned long stripped_len,
                              unsigned char *dst_rgb, int dw, int dh);

int tg_avatar_self_test(void);

#endif
