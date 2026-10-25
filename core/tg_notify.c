/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "tg_notify.h"
#include "tg_file.h"
#include "tg_mtproto_login.h"

static int tg_notify_pref_word(const char *value, const char *word)
{
    unsigned long i;

    if (value == 0 || word == 0) {
        return 0;
    }
    i = 0UL;
    while (word[i] != '\0' && value[i] != '\0') {
        char actual;
        char expected;

        actual = value[i];
        expected = word[i];
        if (actual >= 'A' && actual <= 'Z') {
            actual = (char)(actual - 'A' + 'a');
        }
        if (expected >= 'A' && expected <= 'Z') {
            expected = (char)(expected - 'A' + 'a');
        }
        if (actual != expected) {
            return 0;
        }
        ++i;
    }
    return word[i] == '\0' &&
           (value[i] == '\0' || value[i] == '\r' || value[i] == '\n');
}

/* Consume whole lines, including ones a newer build may have written. A
   continuation of an unknown or overlong line must never become a setting. */
static int tg_notify_read_line(FILE *file, char *line, unsigned long size)
{
    unsigned long n;
    int c;

    if (fgets(line, (int)size, file) == 0) {
        return 0;
    }
    n = (unsigned long)strlen(line);
    if (n != 0UL && line[n - 1UL] != '\n' && !feof(file)) {
        c = fgetc(file);
        if (c != EOF) {
            while (c != '\n' && c != EOF) {
                c = fgetc(file);
            }
            line[0] = '\0';
        }
    }
    return 1;
}

/* One sine cycle, 256 steps of 127. */
static const signed char tg_notify_sine[256] = {
    0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
    49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
    90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
    127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118,
    117, 116, 115, 113, 112, 111, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92,
    90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51,
    49, 46, 43, 40, 37, 34, 31, 28, 25, 22, 19, 16, 12, 9, 6, 3,
    0, -3, -6, -9, -12, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
    -49, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
    -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92,
    -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51,
    -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -12, -9, -6, -3
};

void tg_notify_chime_render(signed char *out)
{
    static const unsigned long notes[2] = { 880UL, 1319UL }; /* A5, E6 */
    unsigned long note;
    unsigned long i;
    unsigned long phase;
    unsigned long step;
    unsigned long fade;
    unsigned long env;

    if (out == 0) {
        return;
    }
    for (note = 0UL; note < 2UL; ++note) {
        /* 32-bit phase, its top byte the table step; integer only, which
           a 68000 without an FPU renders in a blink. */
        phase = 0UL;
        step = ((notes[note] << 16) / TG_NOTIFY_CHIME_RATE) << 16;
        for (i = 0UL; i < TG_NOTIFY_CHIME_NOTE; ++i) {
            fade = ((TG_NOTIFY_CHIME_NOTE - i) << 8) / TG_NOTIFY_CHIME_NOTE;
            env = (fade * fade) >> 8;    /* 256 down to 0, a bell's tail */
            if (i < 64UL) {
                env = (env * i) / 64UL;  /* 3 ms rise: no click */
            }
            out[note * TG_NOTIFY_CHIME_NOTE + i] = (signed char)(
                ((long)tg_notify_sine[(phase >> 24) & 0xFFUL] * (long)env) /
                320L);
            phase = (phase + step) & 0xFFFFFFFFUL;
        }
    }
}

/* An absent sound line means the chime. Old readers ignore sound=, as they
   ignore other unknown lines. Spaces in a path are data, not separators. */
void tg_notify_preferences_load(const char *path, int *mode, int *beep,
                                 int *mode_explicit, char *sound,
                                 unsigned long sound_size)
{
    FILE *file;
    char value[320];
    unsigned long length;
    int loaded_mode;
    int loaded_beep;
    int loaded_explicit;

    loaded_mode = TG_NOTIFY_TITLE; /* until the machine is known */
    loaded_beep = 0;
    loaded_explicit = 0;
    if (sound != 0 && sound_size != 0UL) {
        sound[0] = '\0';
    }
    file = (path != 0 && path[0] != '\0') ? fopen(path, "rb") : 0;
    if (file != 0) {
        while (tg_notify_read_line(file, value, sizeof(value))) {
            if (strncmp(value, "mode=", 5UL) == 0) {
                loaded_explicit = 1;
                if (tg_notify_pref_word(value + 5, "off")) {
                    loaded_mode = TG_NOTIFY_OFF;
                } else if (tg_notify_pref_word(value + 5, "title")) {
                    loaded_mode = TG_NOTIFY_TITLE;
                } else if (tg_notify_pref_word(value + 5, "popup")) {
                    loaded_mode = TG_NOTIFY_POPUP;
                } else if (tg_notify_pref_word(value + 5, "text")) {
                    loaded_mode = TG_NOTIFY_POPUP_TEXT;
                } else {
                    loaded_explicit = 0; /* "auto", or a word we lost */
                }
            } else if (strncmp(value, "beep=", 5UL) == 0) {
                loaded_beep = tg_notify_pref_word(value + 5, "on");
            } else if (strncmp(value, "sound=", 6UL) == 0 &&
                       sound != 0 && sound_size != 0UL) {
                length = (unsigned long)strcspn(value + 6, "\r\n");
                sound[0] = '\0';
                if (length < TG_NOTIFY_SOUND_MAX && length < sound_size) {
                    memcpy(sound, value + 6, length);
                    sound[length] = '\0';
                }
            }
        }
        fclose(file);
    }
    if (mode != 0) {
        *mode = loaded_mode;
    }
    if (beep != 0) {
        *beep = loaded_beep;
    }
    if (mode_explicit != 0) {
        *mode_explicit = loaded_explicit;
    }
}

int tg_notify_preferences_save(const char *path, int mode, int beep,
                                int mode_explicit, const char *sound)
{
    FILE *file;
    char tmp[288];
    unsigned long length;
    const char *word;
    int failed;

    if (path == 0 || path[0] == '\0' ||
        (sound != 0 && (strlen(sound) >= TG_NOTIFY_SOUND_MAX ||
                        strpbrk(sound, "\r\n") != 0))) {
        return 1;
    }
    length = (unsigned long)strlen(path);
    if (length + 5UL >= sizeof(tmp)) {
        return 1;
    }
    memcpy(tmp, path, length);
    memcpy(tmp + length, ".tmp", 5UL);
    file = tg_file_fopen_replace(tmp, "wb");
    if (file == 0) {
        return 1;
    }
    /* A beep toggled on a machine default keeps the mode "auto". */
    word = !mode_explicit ? "auto" :
           mode == TG_NOTIFY_OFF ? "off" :
           mode == TG_NOTIFY_POPUP ? "popup" :
           mode == TG_NOTIFY_POPUP_TEXT ? "text" : "title";
    failed = fputs("mode=", file) == EOF || fputs(word, file) == EOF ||
             fputc('\n', file) == EOF ||
             fputs(beep ? "beep=on\n" : "beep=off\n", file) == EOF;
    if (sound != 0 && sound[0] != '\0' &&
        (fputs("sound=", file) == EOF || fputs(sound, file) == EOF ||
         fputc('\n', file) == EOF)) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(tmp);
        return 1;
    }
    (void)remove(path);
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return 1;
    }
    return 0;
}

/* Phase 1 reads mute/ring plus the optional filename, including spaces. A
   sound with the default policy uses "sound hi lo path", which phase 1 skips
   because it expects a hex id first. No ring permission is implied by it. */
static int tg_notify_override_line(const char *line, unsigned long *hi,
                                       unsigned long *lo,
                                       tg_notify_override *value)
{
    char word[8];
    int used;
    unsigned long length;

    used = 0;
    memset(value, 0, sizeof(*value));
    if (strncmp(line, "sound ", 6UL) == 0) {
        if (sscanf(line + 6, "%8lx %8lx %n", hi, lo, &used) != 2 ||
            used == 0) {
            return 0;
        }
        used += 6;
    } else {
        if (sscanf(line, "%8lx %8lx %7s %n", hi, lo, word, &used) != 3 ||
            used == 0) {
            return 0;
        }
        value->choice = strcmp(word, "mute") == 0 ? TG_NOTIFY_MUTE :
                        strcmp(word, "ring") == 0 ? TG_NOTIFY_RING :
                        TG_NOTIFY_DEFAULT;
        if (value->choice == TG_NOTIFY_DEFAULT) {
            return 0;
        }
    }
    length = (unsigned long)strcspn(line + used, "\r\n");
    if (length >= sizeof(value->sound)) {
        return 0;
    }
    memcpy(value->sound, line + used, length);
    value->sound[length] = '\0';
    return 1;
}

void tg_notify_override_get(const char *path, unsigned long peer_hi,
                                unsigned long peer_lo,
                                tg_notify_override *out)
{
    FILE *file;
    char line[320];
    unsigned long hi;
    unsigned long lo;
    tg_notify_override value;

    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    file = path != 0 && path[0] != '\0' ? fopen(path, "rb") : 0;
    if (file == 0) {
        return;
    }
    while (tg_notify_read_line(file, line, sizeof(line))) {
        if (tg_notify_override_line(line, &hi, &lo, &value) &&
            hi == peer_hi && lo == peer_lo) {
            *out = value;
        }
    }
    fclose(file);
}

int tg_notify_override_set(const char *path, unsigned long peer_hi,
                               unsigned long peer_lo,
                               const tg_notify_override *value)
{
    FILE *src;
    FILE *dst;
    char tmp[288];
    char line[320];
    unsigned long length;
    unsigned long hi;
    unsigned long lo;
    tg_notify_override old;
    int failed;
    int needs_newline;
    int continuation;

    if (path == 0 || path[0] == '\0' || value == 0 ||
        value->choice < TG_NOTIFY_DEFAULT ||
        value->choice > TG_NOTIFY_RING ||
        memchr(value->sound, '\0', sizeof(value->sound)) == 0 ||
        strpbrk(value->sound, "\r\n") != 0) {
        return 1;
    }
    length = (unsigned long)strlen(path);
    if (length + 5UL >= sizeof(tmp)) {
        return 1;
    }
    memcpy(tmp, path, length);
    memcpy(tmp + length, ".tmp", 5UL);
    /* Some Amiga libraries leave errno unchanged on an open failure. An
       unreadable or missing source is empty; the replacement must still save. */
    src = fopen(path, "rb");
    dst = tg_file_fopen_replace(tmp, "wb");
    if (dst == 0) {
        if (src != 0) {
            fclose(src);
        }
        return 1;
    }
    failed = 0;
    needs_newline = 0;
    continuation = 0;
    while (src != 0 && fgets(line, sizeof(line), src) != 0) {
        length = (unsigned long)strlen(line);
        /* Preserve unknown long lines verbatim, including their chunks. */
        if (!continuation && tg_notify_override_line(line, &hi, &lo, &old) &&
            hi == peer_hi && lo == peer_lo) {
            continue;
        }
        continuation = length > 0UL && line[length - 1UL] != '\n';
        if (fputs(line, dst) == EOF) {
            failed = 1;
        }
        needs_newline = length > 0UL && line[length - 1UL] != '\n';
    }
    if (src != 0) {
        if (ferror(src)) {
            failed = 1;
        }
        fclose(src);
    }
    if (value->choice != TG_NOTIFY_DEFAULT || value->sound[0] != '\0') {
        if (needs_newline && fputc('\n', dst) == EOF) {
            failed = 1;
        }
        if (value->choice == TG_NOTIFY_DEFAULT) {
            if (fprintf(dst, "sound %08lx %08lx ", peer_hi & 0xffffffffUL,
                        peer_lo & 0xffffffffUL) < 0 ||
                fputs(value->sound, dst) == EOF || fputc('\n', dst) == EOF) {
                failed = 1;
            }
        } else if (fprintf(dst, "%08lx %08lx %.4s", peer_hi & 0xffffffffUL,
                    peer_lo & 0xffffffffUL,
                    value->choice == TG_NOTIFY_MUTE ? "mute" : "ring") < 0 ||
            (value->sound[0] != '\0' &&
             (fputc(' ', dst) == EOF || fputs(value->sound, dst) == EOF)) ||
            fputc('\n', dst) == EOF) {
            failed = 1;
        }
    }
    if (fclose(dst) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(tmp);
        return 1;
    }
    (void)remove(path); /* AmigaDOS rename does not replace a file. */
    if (rename(tmp, path) != 0) {
        return 1; /* keep the completed temporary file for recovery */
    }
    return 0;
}

int tg_notify_should_ring(unsigned long peer_constructor,
                               unsigned long mute_until, int peer_silent,
                               int choice, int silent, int is_out,
                               int open_in_front, unsigned long server_now,
                               unsigned long local_now,
                               unsigned long last_ring)
{
    if (silent || is_out || open_in_front || peer_silent ||
        choice == TG_NOTIFY_MUTE || mute_until > server_now ||
        local_now == 0UL) {
        return 0;
    }
    if (peer_constructor != TG_MTPROTO_PEER_USER_CONSTRUCTOR &&
        !((peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ||
           peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) &&
          choice == TG_NOTIFY_RING)) {
        return 0;
    }
    /* A clock moved backwards must not underflow into an immediate ring. */
    if (last_ring != 0UL && (local_now < last_ring ||
        local_now - last_ring < TG_NOTIFY_QUIET_SECONDS)) {
        return 0;
    }
    return 1;
}

const char *tg_notify_sound_choice(int ring, const char *chat_sound,
                                   const char *global_sound)
{
    if (!ring) {
        return 0;
    }
    if (chat_sound != 0 && chat_sound[0] != '\0') {
        return chat_sound;
    }
    return global_sound != 0 ? global_sound : "";
}

