/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "tg_file.h"
#include <sys/stat.h>

tg_file_status tg_file_read_text(const char *path, char *buffer,
                                 unsigned long buffer_size,
                                 unsigned long *text_length)
{
    FILE *file;
    unsigned long used;
    int ch;

    if (text_length != 0) {
        *text_length = 0;
    }
    if (path == 0 || path[0] == '\0' || buffer == 0 ||
        buffer_size == 0 || text_length == 0) {
        return TG_FILE_INVALID_ARGUMENT;
    }

    file = fopen(path, "rb");
    if (file == 0) {
        buffer[0] = '\0';
        return TG_FILE_OPEN_FAILED;
    }

    used = 0;
    for (;;) {
        ch = fgetc(file);
        if (ch == EOF) {
            break;
        }
        if (used + 1 >= buffer_size) {
            fclose(file);
            buffer[0] = '\0';
            return TG_FILE_TOO_LARGE;
        }
        buffer[used] = (char)ch;
        ++used;
    }

    if (ferror(file)) {
        fclose(file);
        buffer[0] = '\0';
        return TG_FILE_READ_FAILED;
    }

    fclose(file);
    buffer[used] = '\0';
    *text_length = used;
    return TG_FILE_OK;
}

tg_file_status tg_file_write_text(const char *path, const char *text,
                                  unsigned long text_length)
{
    FILE *file;
    size_t written;

    if (path == 0 || path[0] == '\0' || (text == 0 && text_length > 0)) {
        return TG_FILE_INVALID_ARGUMENT;
    }

    /* Delete first, then create. Rewriting a file that ALREADY EXISTS on a FAT
       volume under AROS reports success from fwrite and from fclose and leaves
       zero bytes behind: our saved login vanished on every restart and the
       client could only say the file was empty. It is not our bug and not
       specific to the Raspberry Pi where it reached us: AROS carries it open
       as deadwood2/AROS issue 161, "Issues writting to existing file on FAT
       volumes", with a reproducer of nine lines and no client of ours in
       sight, and issue 43 says rename on that handler is unreliable too, which
       is why this is a delete and not a write-to-temp-then-rename. A brand new
       file commits reliably, and every platform truncates the old contents at
       open anyway, so the window where the data is gone is the one we already
       had. Reached us from ARM, diagnosed by bohunamiga. */
    (void)remove(path);

    file = fopen(path, "wb");
    if (file == 0) {
        return TG_FILE_OPEN_FAILED;
    }

    written = fwrite(text, 1, (size_t)text_length, file);
    if (written != (size_t)text_length || ferror(file)) {
        fclose(file);
        return TG_FILE_WRITE_FAILED;
    }
    /* Flush before close so a full buffer that cannot be written is reported
       here, where the file is still open and the error is unambiguous. */
    if (fflush(file) != 0) {
        fclose(file);
        return TG_FILE_WRITE_FAILED;
    }
    if (fclose(file) != 0) {
        return TG_FILE_WRITE_FAILED;
    }

    return TG_FILE_OK;
}

FILE *tg_file_fopen_replace(const char *path, const char *mode)
{
    if (path == 0 || path[0] == '\0' || mode == 0) {
        return 0;
    }
    /* See tg_file_write_text: delete, then create. Proven again on the AROS
       ARM image in QEMU (2026-09-21): an in-place rewrite on the FAT volume
       reads back as zero bytes, a delete and a fresh file read back whole. */
    (void)remove(path);
    return fopen(path, mode);
}

tg_file_status tg_file_append_text(const char *path, const char *text,
                                   unsigned long text_length)
{
    FILE *file;
    size_t written;

    if (path == 0 || path[0] == '\0' || (text == 0 && text_length > 0)) {
        return TG_FILE_INVALID_ARGUMENT;
    }

    file = fopen(path, "ab");
    if (file == 0) {
        return TG_FILE_OPEN_FAILED;
    }

    written = fwrite(text, 1, (size_t)text_length, file);
    if (written != (size_t)text_length || ferror(file)) {
        fclose(file);
        return TG_FILE_WRITE_FAILED;
    }
    if (fflush(file) != 0) {
        fclose(file);
        return TG_FILE_WRITE_FAILED;
    }
    if (fclose(file) != 0) {
        return TG_FILE_WRITE_FAILED;
    }

    return TG_FILE_OK;
}

const char *tg_file_status_name(tg_file_status status)
{
    switch (status) {
    case TG_FILE_OK:
        return "ok";
    case TG_FILE_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_FILE_OPEN_FAILED:
        return "open-failed";
    case TG_FILE_READ_FAILED:
        return "read-failed";
    case TG_FILE_TOO_LARGE:
        return "too-large";
    case TG_FILE_WRITE_FAILED:
        return "write-failed";
    default:
        return "unknown";
    }
}

/* The persistent RNG seed, PROGDIR:data/telegram-seed.bin, opened for reading
   or for saving. One copy for the four Amiga platform layers, which used to
   carry it verbatim each (a review of the 0.0.94 candidate counted them):
   the migration of a root-era seed into data/, the PROGDIR: path with the
   current-directory fallback, and the save through tg_file_fopen_replace. */
FILE *tg_file_open_seed(const char *mode)
{
    /* PROGDIR: keeps the seed next to the binary; some C libraries do not
       grok Amiga-style paths, so fall back to the current directory (the
       icon launcher CDs into the drawer anyway). */
    FILE *f;

    /* Tidy layout: the seed lives in data/ with the other auxiliary files.
       One-time migration of a root-era seed, with the data/ copy winning
       (never overwrite live state with a stale root leftover). */
    f = fopen("PROGDIR:data/telegram-seed.bin", "rb");
    if (f == 0) {
        f = fopen("data/telegram-seed.bin", "rb");
    }
    if (f != 0) {
        fclose(f);
        (void)remove("PROGDIR:telegram-seed.bin");
        (void)remove("telegram-seed.bin");
    } else {
        (void)mkdir("data", 0777);
        if (rename("PROGDIR:telegram-seed.bin",
                   "PROGDIR:data/telegram-seed.bin") != 0) {
            (void)rename("telegram-seed.bin", "data/telegram-seed.bin");
        }
    }
    if (mode[0] == 'w') {
        /* Saving replaces the file: through the one door every file the
           client rewrites uses, since a file rewritten in place on the AROS
           FAT handler reads back empty (its issue 161; a Raspberry Pi 400
           card came back with a zero-byte seed). */
        f = tg_file_fopen_replace("PROGDIR:data/telegram-seed.bin", mode);
        if (f == 0) {
            f = tg_file_fopen_replace("data/telegram-seed.bin", mode);
        }
        return f;
    }
    f = fopen("PROGDIR:data/telegram-seed.bin", mode);
    if (f == 0) {
        f = fopen("data/telegram-seed.bin", mode);
    }
    return f;
}
