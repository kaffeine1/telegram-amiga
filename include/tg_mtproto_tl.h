/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_TL_H
#define TG_MTPROTO_TL_H

typedef enum tg_mtproto_tl_status {
    TG_MTPROTO_TL_OK = 0,
    TG_MTPROTO_TL_INVALID_ARGUMENT = 1,
    TG_MTPROTO_TL_BUFFER_TOO_SMALL = 2,
    TG_MTPROTO_TL_TRUNCATED = 3,
    TG_MTPROTO_TL_INVALID_DATA = 4
} tg_mtproto_tl_status;

typedef struct tg_mtproto_tl_writer {
    unsigned char *buffer;
    unsigned long capacity;
    unsigned long length;
} tg_mtproto_tl_writer;

typedef struct tg_mtproto_tl_reader {
    const unsigned char *buffer;
    unsigned long length;
    unsigned long offset;
} tg_mtproto_tl_reader;

void tg_mtproto_tl_writer_init(tg_mtproto_tl_writer *writer,
                               unsigned char *buffer,
                               unsigned long capacity);
void tg_mtproto_tl_reader_init(tg_mtproto_tl_reader *reader,
                               const unsigned char *buffer,
                               unsigned long length);

tg_mtproto_tl_status tg_mtproto_tl_write_u32(tg_mtproto_tl_writer *writer,
                                             unsigned long value);
tg_mtproto_tl_status tg_mtproto_tl_write_u8(tg_mtproto_tl_writer *writer,
                                            unsigned long value);
tg_mtproto_tl_status tg_mtproto_tl_write_u64(tg_mtproto_tl_writer *writer,
                                             unsigned long hi,
                                             unsigned long lo);
tg_mtproto_tl_status tg_mtproto_tl_write_raw(tg_mtproto_tl_writer *writer,
                                             const unsigned char *data,
                                             unsigned long data_length);
tg_mtproto_tl_status tg_mtproto_tl_write_bytes(tg_mtproto_tl_writer *writer,
                                               const unsigned char *data,
                                               unsigned long data_length);

tg_mtproto_tl_status tg_mtproto_tl_read_u32(tg_mtproto_tl_reader *reader,
                                            unsigned long *value);
tg_mtproto_tl_status tg_mtproto_tl_read_u64(tg_mtproto_tl_reader *reader,
                                            unsigned long *hi,
                                            unsigned long *lo);
tg_mtproto_tl_status tg_mtproto_tl_read_raw(tg_mtproto_tl_reader *reader,
                                            const unsigned char **data,
                                            unsigned long data_length);
tg_mtproto_tl_status tg_mtproto_tl_read_bytes(tg_mtproto_tl_reader *reader,
                                              const unsigned char **data,
                                              unsigned long *data_length);

const char *tg_mtproto_tl_status_name(tg_mtproto_tl_status status);
int tg_mtproto_tl_self_test(void);

#endif
