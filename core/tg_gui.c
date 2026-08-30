/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Portable model + renderer for the native GUI line. The per-platform backend
 * supplies only metrics and draw primitives (see tg_gui.h); everything about
 * layout lives here so it can be unit-tested on the host and shared by every
 * Amiga backend. Redraw discipline (only changed rows) comes later with the
 * real window; this file establishes the full-paint geometry first.
 */

#include "tg_gui.h"
#include "tg_gui_session.h" /* tg_gui_log: crash-safe first-paint trail */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Enough wrapped lines to render a full TG_GUI_MSG_TEXT_MAX message at a narrow
   bubble width. starts[]/lengths[] are stack arrays (~lines * 16 B per paint),
   fine under the GUI launcher's 1 MB stack. Tiered to match the message buffer. */
#if defined(__m68k__)
#define TG_GUI_WRAP_MAX_LINES 128
#else
#define TG_GUI_WRAP_MAX_LINES 256
#endif

/* Air between the "Replying to..." strip and the composer below it (field
   request: the strip read as glued to the box). The reserved band grows by the
   same amount, so the strip rises without reaching into the transcript, and
   every consumer of the geometry -- paint, hit test, mention popup -- reads it
   from here. */
#define TG_GUI_REPLY_LIFT 3

/* Custom-drawn vertical scrollbar geometry (no GadTools propgadget, so it is
   identical on every backend). TG_GUI_SCROLLBAR_W lives in tg_gui.h, shared
   with the event loop's knob hit-test. */
#define TG_GUI_SCROLLBAR_MIN_KNOB 14

/* When 0, tg_gui_paint skips the leading full-window clear so a repeated repaint
   of unchanged opaque content does not flash the window (used by the redraw-time
   measurement). Default on. */
static int tg_gui_clear_background = 1;

void tg_gui_set_background_clear(int enabled)
{
    tg_gui_clear_background = enabled ? 1 : 0;
}

static int tg_gui_photo_dither_parse(const char *value)
{
    if (value != 0 &&
        (value[0] == 'l' || value[0] == 'L') &&
        (value[1] == 'i' || value[1] == 'I')) {
        return TG_GUI_PHOTO_DITHER_LIGHT;
    }
    if (value != 0 &&
        (value[0] == 'o' || value[0] == 'O') &&
        (value[1] == 'f' || value[1] == 'F')) {
        return TG_GUI_PHOTO_DITHER_OFF;
    }
    return TG_GUI_PHOTO_DITHER_FULL;
}

static unsigned long tg_gui_photo_cache_limit_parse(const char *value)
{
    unsigned long parsed;
    const char *tail;

    if (value != 0 && strncmp(value, "unlimited", 9UL) == 0 &&
        (value[9] == '\0' || value[9] == '\r' || value[9] == '\n')) {
        return TG_GUI_PHOTO_CACHE_UNLIMITED_MB;
    }
    parsed = 0UL;
    tail = value;
    while (tail != 0 && *tail >= '0' && *tail <= '9') {
        parsed = parsed * 10UL + (unsigned long)(*tail - '0');
        ++tail;
    }
    if (tail == value ||
        (tail != 0 && *tail != '\0' && *tail != '\r' && *tail != '\n')) {
        return TG_GUI_PHOTO_CACHE_DEFAULT_MB;
    }
    if (parsed == 10UL || parsed == 50UL || parsed == 200UL) {
        return parsed;
    }
    return TG_GUI_PHOTO_CACHE_DEFAULT_MB;
}

static int tg_gui_photo_pref_word(const char *value, const char *word)
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

void tg_gui_photo_preferences_load(const char *path, int *inline_photos,
                                   int *inline_photos_explicit,
                                   int *photo_dither,
                                   unsigned long *photo_cache_limit_mb)
{
    FILE *file;
    char value[64];
    int enabled;
    int explicit_choice;
    int dither;
    unsigned long cache_limit;
    int first_line;

    enabled = 1;
    explicit_choice = 0;
    dither = TG_GUI_PHOTO_DITHER_FULL;
    cache_limit = TG_GUI_PHOTO_CACHE_DEFAULT_MB;
    file = (path != 0 && path[0] != '\0') ? fopen(path, "rb") : 0;
    if (file != 0) {
        first_line = 1;
        while (fgets(value, sizeof(value), file) != 0) {
            if (first_line && tg_gui_photo_pref_word(value, "on")) {
                enabled = 1;
                explicit_choice = 1;
            } else if (first_line && tg_gui_photo_pref_word(value, "off")) {
                enabled = 0;
                explicit_choice = 1;
            } else if (strncmp(value, "dither=", 7UL) == 0) {
                dither = tg_gui_photo_dither_parse(value + 7);
            } else if (strncmp(value, "cache_limit=", 12UL) == 0) {
                cache_limit = tg_gui_photo_cache_limit_parse(value + 12);
            }
            first_line = 0;
        }
        fclose(file);
    }
    if (inline_photos != 0) {
        *inline_photos = enabled;
    }
    if (inline_photos_explicit != 0) {
        *inline_photos_explicit = explicit_choice;
    }
    if (photo_dither != 0) {
        *photo_dither = dither;
    }
    if (photo_cache_limit_mb != 0) {
        *photo_cache_limit_mb = cache_limit;
    }
}

int tg_gui_photo_preferences_save(const char *path, int inline_photos,
                                  int inline_photos_explicit,
                                  int photo_dither,
                                  unsigned long photo_cache_limit_mb)
{
    FILE *file;
    char tmp[288];
    unsigned long length;
    const char *dither;
    char cache_limit[32];
    int failed;

    if (path == 0 || path[0] == '\0') {
        return 1;
    }
    length = (unsigned long)strlen(path);
    if (length + 5UL >= sizeof(tmp)) {
        return 1;
    }
    memcpy(tmp, path, length);
    memcpy(tmp + length, ".tmp", 5UL);
    file = fopen(tmp, "wb");
    if (file == 0) {
        return 1;
    }
    dither = photo_dither == TG_GUI_PHOTO_DITHER_LIGHT ? "light" :
              photo_dither == TG_GUI_PHOTO_DITHER_OFF ? "off" : "full";
    if (photo_cache_limit_mb == TG_GUI_PHOTO_CACHE_UNLIMITED_MB) {
        strcpy(cache_limit, "unlimited");
    } else {
        sprintf(cache_limit, "%lu", photo_cache_limit_mb);
    }
    failed = fputs(inline_photos_explicit
                       ? (inline_photos ? "on\n" : "off\n")
                       : "auto\n",
                   file) == EOF ||
             fputs("dither=", file) == EOF || fputs(dither, file) == EOF ||
             fputc('\n', file) == EOF ||
             fputs("cache_limit=", file) == EOF ||
             fputs(cache_limit, file) == EOF || fputc('\n', file) == EOF;
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

int tg_gui_inline_photos_load(const char *path)
{
    int enabled;

    tg_gui_photo_preferences_load(path, &enabled, 0, 0, 0);
    return enabled;
}

int tg_gui_inline_photos_save(const char *path, int enabled)
{
    int dither;
    unsigned long cache_limit;

    tg_gui_photo_preferences_load(path, 0, 0, &dither, &cache_limit);
    return tg_gui_photo_preferences_save(path, enabled, 1, dither,
                                         cache_limit);
}

int tg_gui_inline_photos_resolve(int explicit_choice, int explicit_value,
                                 int classic_os3, int cpu_at_least_040,
                                 int has_rtg)
{
    if (explicit_choice) {
        return explicit_value ? 1 : 0;
    }
    if (classic_os3 && (!cpu_at_least_040 || !has_rtg)) {
        return 0;
    }
    return 1;
}

static int tg_gui_photo_cache_older(const tg_gui_photo_cache_item *a,
                                    const tg_gui_photo_cache_item *b)
{
    if (a->days != b->days) {
        return a->days < b->days;
    }
    if (a->minutes != b->minutes) {
        return a->minutes < b->minutes;
    }
    return a->ticks < b->ticks;
}

unsigned long long tg_gui_photo_cache_prune_plan(
    tg_gui_photo_cache_item *items, int count,
    unsigned long long limit_bytes,
    unsigned long max_remove,
    unsigned long *remove_count)
{
    unsigned long long total;
    unsigned long planned;
    int i;

    total = 0ULL;
    planned = 0UL;
    if (remove_count != 0) {
        *remove_count = 0UL;
    }
    if (items == 0 || count <= 0) {
        return 0ULL;
    }
    for (i = 0; i < count; ++i) {
        items[i].remove = 0;
        total += (unsigned long long)items[i].bytes;
    }
    if (limit_bytes == 0ULL) {
        return total;
    }
    while (total > limit_bytes &&
           (max_remove == 0UL || planned < max_remove)) {
        int oldest;

        oldest = -1;
        for (i = 0; i < count; ++i) {
            if (items[i].remove || items[i].protected_entry) {
                continue;
            }
            if (oldest < 0 ||
                tg_gui_photo_cache_older(&items[i], &items[oldest])) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            break;
        }
        items[oldest].remove = 1;
        total = (unsigned long long)items[oldest].bytes > total
                    ? 0ULL : total - (unsigned long long)items[oldest].bytes;
        ++planned;
    }
    if (remove_count != 0) {
        *remove_count = planned;
    }
    return total;
}

int tg_gui_photo_cache_clear_files(const char *const *paths, int count,
                                   unsigned long *removed_files,
                                   unsigned long *removed_bytes)
{
    unsigned long files;
    unsigned long bytes;
    int failures;
    int i;

    files = 0UL;
    bytes = 0UL;
    failures = 0;
    if (paths == 0 || count < 0) {
        return 1;
    }
    for (i = 0; i < count; ++i) {
        struct stat info;
        unsigned long size;

        if (paths[i] == 0 || paths[i][0] == '\0') {
            ++failures;
            continue;
        }
        size = stat(paths[i], &info) == 0 && info.st_size > 0
                   ? (unsigned long)info.st_size : 0UL;
        if (remove(paths[i]) == 0) {
            ++files;
            bytes += size;
        } else {
            ++failures;
        }
    }
    if (removed_files != 0) {
        *removed_files = files;
    }
    if (removed_bytes != 0) {
        *removed_bytes = bytes;
    }
    return failures;
}

int tg_gui_photo_cache_choose_slot(const int *states,
                                   const unsigned char *visible,
                                   const unsigned long *last_use,
                                   int count)
{
    int i;
    int best;
    unsigned long oldest;

    if (states == 0 || visible == 0 || last_use == 0 || count <= 0) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (states[i] == 0) {
            return i;
        }
    }
    best = -1;
    oldest = 0UL;
    for (i = 0; i < count; ++i) {
        if (states[i] == 2 || visible[i]) {
            continue;
        }
        if (best < 0 || last_use[i] < oldest) {
            best = i;
            oldest = last_use[i];
        }
    }
    return best;
}

void tg_gui_photo_decode_gate_reset(tg_gui_photo_decode_gate *gate)
{
    if (gate != 0) {
        memset(gate, 0, sizeof(*gate));
    }
}

int tg_gui_photo_decode_gate_acquire(tg_gui_photo_decode_gate *gate,
                                     const void *owner,
                                     unsigned long id_hi,
                                     unsigned long id_lo,
                                     int scope)
{
    if (gate == 0 || owner == 0) {
        return 0;
    }
    if (gate->owner != 0) {
        return tg_gui_photo_decode_gate_owns(
            gate, owner, id_hi, id_lo, scope);
    }
    gate->owner = owner;
    gate->id_hi = id_hi;
    gate->id_lo = id_lo;
    gate->scope = scope;
    return 1;
}

int tg_gui_photo_decode_gate_owns(const tg_gui_photo_decode_gate *gate,
                                  const void *owner,
                                  unsigned long id_hi,
                                  unsigned long id_lo,
                                  int scope)
{
    return gate != 0 && owner != 0 && gate->owner == owner &&
           gate->id_hi == id_hi && gate->id_lo == id_lo &&
           gate->scope == scope;
}

void tg_gui_photo_decode_gate_release(tg_gui_photo_decode_gate *gate,
                                      const void *owner)
{
    if (gate != 0 && owner != 0 && gate->owner == owner) {
        tg_gui_photo_decode_gate_reset(gate);
    }
}

void tg_gui_photo_pace_init(tg_gui_photo_pace *pace,
                            unsigned long minimum,
                            unsigned long initial,
                            unsigned long maximum,
                            unsigned long target_ms)
{
    if (pace == 0) {
        return;
    }
    if (minimum == 0UL) {
        minimum = 1UL;
    }
    if (maximum < minimum) {
        maximum = minimum;
    }
    if (initial < minimum) {
        initial = minimum;
    } else if (initial > maximum) {
        initial = maximum;
    }
    pace->minimum = minimum;
    pace->maximum = maximum;
    pace->budget = initial;
    pace->target_ms = target_ms != 0UL ? target_ms : 1UL;
}

int tg_gui_photo_pace_observe(tg_gui_photo_pace *pace,
                              unsigned long elapsed_ms)
{
    unsigned long next;

    if (pace == 0 || pace->minimum == 0UL ||
        pace->maximum < pace->minimum || pace->target_ms == 0UL) {
        return 0;
    }
    next = pace->budget;
    if (elapsed_ms < (pace->target_ms + 1UL) / 2UL &&
        next < pace->maximum) {
        if (next > pace->maximum / 2UL) {
            next = pace->maximum;
        } else {
            next *= 2UL;
        }
    } else if (elapsed_ms > pace->target_ms && next > pace->minimum) {
        next /= 2UL;
        if (next < pace->minimum) {
            next = pace->minimum;
        }
    }
    if (next == pace->budget) {
        return 0;
    }
    pace->budget = next;
    return 1;
}

int tg_gui_photo_preview_prepare_all(int count,
                                     tg_gui_photo_preview_prepare_fn prepare,
                                     void *context)
{
    int i;
    int ready;

    if (count <= 0 || prepare == 0) {
        return 0;
    }
    ready = 0;
    for (i = 0; i < count; ++i) {
        if (prepare(context, i)) {
            ++ready;
        }
    }
    return ready;
}

typedef struct tg_gui_photo_preview_test {
    unsigned char preview_only[6];
    int quality_commits;
} tg_gui_photo_preview_test;

#if !defined(TG_NO_SELFTEST)
static int tg_gui_photo_preview_test_prepare(void *context, int index)
{
    tg_gui_photo_preview_test *test;

    test = (tg_gui_photo_preview_test *)context;
    if (test == 0 || index < 0 || index >= 6 || test->quality_commits != 0) {
        return 0;
    }
    test->preview_only[index] = 1U;
    return 1;
}
#endif /* !TG_NO_SELFTEST */

static void tg_gui_copy(char *dest, unsigned long size, const char *src)
{
    unsigned long i;

    if (dest == 0 || size == 0UL) {
        return;
    }
    if (src == 0) {
        dest[0] = '\0';
        return;
    }
    for (i = 0UL; i + 1UL < size && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static void tg_gui_set_chat(tg_gui_chat *chat, const char *name,
                            const char *preview, const char *time,
                            const char *initials, int avatar_color, int unread)
{
    tg_gui_copy(chat->name, sizeof(chat->name), name);
    tg_gui_copy(chat->preview, sizeof(chat->preview), preview);
    tg_gui_copy(chat->time, sizeof(chat->time), time);
    tg_gui_copy(chat->initials, sizeof(chat->initials), initials);
    chat->avatar_color = avatar_color;
    chat->unread = unread;
}

static void tg_gui_set_message(tg_gui_message *message, const char *sender,
                               const char *text, const char *time,
                               int sender_color, int is_own, int is_system)
{
    tg_gui_copy(message->sender, sizeof(message->sender), sender);
    tg_gui_copy(message->text, sizeof(message->text), text);
    tg_gui_copy(message->time, sizeof(message->time), time);
    message->sender_color = sender_color;
    message->is_own = is_own;
    message->is_system = is_system;
    message->reply_text[0] = '\0'; /* demo carries no replies */
}

void tg_gui_demo_state(tg_gui_state *state)
{
    state->selected_msg = -1;
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->theme = TG_GUI_THEME_DARK;
    state->inline_photos = 1;
    state->photo_dither = TG_GUI_PHOTO_DITHER_FULL;
    state->photo_cache_limit_mb = TG_GUI_PHOTO_CACHE_DEFAULT_MB;

    tg_gui_set_chat(&state->chats[0], "Sviluppo AmigaIta",
                    "Tu: ottimo, la pausa-bozza ha fatto il suo", "09:20",
                    "SA", 0, 0);
    tg_gui_set_chat(&state->chats[1], "Mario Rossi", "ci vediamo domani allora",
                    "13:10", "MR", 1, 2);
    tg_gui_set_chat(&state->chats[2], "MorphOS Team",
                    "Giulia: build pronta da provare", "12:48", "MT", 2, 7);
    tg_gui_set_chat(&state->chats[3], "Anna", "perfetto, grazie mille", "11:20",
                    "A", 3, 0);
    tg_gui_set_chat(&state->chats[4], "Amiga News",
                    "Nuovo update di sistema disponibile", "ieri", "AN", 4, 0);
    state->chat_count = 5;
    state->selected_chat = 0;

    tg_gui_copy(state->title, sizeof(state->title), "Sviluppo AmigaIta");
    tg_gui_copy(state->subtitle, sizeof(state->subtitle), "group - 128 members");

    tg_gui_set_message(&state->messages[0], "Henry Out",
                       "Sul 030 a 25 MHz ora si scrive fluido, niente piu' "
                       "scatti tra un tasto e l'altro.",
                       "09:14", 1, 0, 0);
    tg_gui_set_message(&state->messages[1], "Lallo", "[Photo]", "09:16", 2,
                       0, 0);
    state->messages[1].has_photo = 1;
    state->messages[1].photo_ready = 1;
    state->messages[1].photo_only = 1;
    state->messages[1].photo_id_lo = 0x1234UL;
    state->messages[1].photo_width = 320UL;
    state->messages[1].photo_height = 180UL;
    tg_gui_set_message(&state->messages[2], "",
                       "Ottimo, allora la pausa-bozza ha fatto il suo lavoro.",
                       "09:20", 0, 1, 0);
    tg_gui_set_message(&state->messages[3], "", "Marco e' entrato nel gruppo",
                       "", 0, 0, 1);
    /* The own message carries a read receipt so the demo shows the double-check
       (the peer has read it); incoming/system rows keep no mark. */
    state->messages[2].read_state = TG_GUI_READ_SEEN;
    state->message_count = 4;

    tg_gui_copy(state->input, sizeof(state->input), "");
    tg_gui_copy(state->status, sizeof(state->status), "Connected - DC4");
}

/* Word-wraps text to max_width using the backend's font metrics, filling the
   starts/lengths arrays with up to max_lines segments. Returns the line count.
   Always makes progress (at least one character per line) so a glyph wider than
   max_width cannot loop forever. */
static int tg_gui_wrap(tg_gui_backend *backend, const char *text, int max_width,
                       unsigned long *starts, unsigned long *lengths,
                       int max_lines)
{
    unsigned long total;
    unsigned long i;
    int line;

    total = (unsigned long)strlen(text);
    i = 0UL;
    line = 0;
    while (line < max_lines) {
        unsigned long line_start;
        unsigned long last_space;
        int have_space;
        unsigned long j;
        int hard_break;

        line_start = i;
        last_space = 0UL;
        have_space = 0;
        hard_break = 0;
        j = i;
        while (j < total) {
            unsigned long segment;

            if (text[j] == '\n') { /* an explicit line break in the message */
                hard_break = 1;
                break;
            }
            segment = j - line_start + 1UL;
            if (j > line_start &&
                backend->text_width(backend, text + line_start, segment) >
                    max_width) {
                break;
            }
            if (text[j] == ' ') {
                last_space = j;
                have_space = 1;
            }
            ++j;
        }
        if (hard_break) {
            /* Emit up to (not including) the newline, then resume AFTER it --
               so real newlines and bullet lists keep their shape, and a blank
               line between paragraphs stays a blank line. */
            starts[line] = line_start;
            lengths[line] = j - line_start;
            ++line;
            i = j + 1UL; /* skip the '\n' */
            if (i >= total) {
                break; /* text ended on the newline: no trailing empty line */
            }
            continue;
        }
        if (j >= total) {
            starts[line] = line_start;
            lengths[line] = total - line_start;
            ++line;
            break;
        }
        if (have_space && last_space > line_start) {
            starts[line] = line_start;
            lengths[line] = last_space - line_start;
            i = last_space + 1UL;
        } else {
            starts[line] = line_start;
            lengths[line] = j - line_start;
            i = j;
        }
        ++line;
    }
    return line;
}

static tg_gui_rect tg_gui_make_rect(int x, int y, int w, int h)
{
    tg_gui_rect rect;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return rect;
}

/* Draws text but never wider than max_w, appending ".." when it has to cut.
   Columns (the chat list especially) must clip horizontally: the backends
   draw to a single RastPort with no per-column clip, so an unclipped long
   preview would bleed straight across the sidebar boundary into the
   conversation pane. */
/* Baseline that centres one line of text vertically inside a box `box_h` tall
   starting at `box_y`. draw_text places text by BASELINE, so centring has to
   work from the font's real ascent: doing it from the line height alone leaves
   the text low by the descender depth, which grows with the font (unread
   badges and the reply strip, field reports on 0.0.9). Backends without
   metrics reproduce the previous approximation. */
static int tg_gui_centred_baseline(tg_gui_backend *backend, int box_y,
                                   int box_h)
{
    int lh = backend->line_height(backend);
    int glyph_h;
    int ascent;

    if (lh <= 0) {
        return box_y;
    }
    /* line_height carries the leading; the glyph cell is what we centre. */
    glyph_h = (lh > 2) ? (lh - 2) : lh;
    ascent = (backend->font_ascent != 0) ? backend->font_ascent(backend)
                                         : glyph_h;
    if (ascent <= 0 || ascent > lh) {
        ascent = glyph_h;
    }
    return box_y + ((box_h - glyph_h) / 2) + ascent;
}

/* Left inset of a disc of diameter h at pixel row y. Doubled coordinates
   keep the arithmetic integral: the row's centre sits at 2*y+1 of 2*h, and
   the loop is a tiny integer square root (h is a couple of text lines at
   most). Shared by every piece of round chrome below. */
static int tg_gui_round_inset(int y, int h)
{
    int dy = (2 * y + 1) - h;
    int rr = (h * h) - (dy * dy);
    int dx = 0;

    if (rr <= 0) {
        return h / 2;
    }
    while ((dx + 1) * (dx + 1) <= rr) {
        ++dx;
    }
    return (h - dx) / 2;
}

/* A pill: fully rounded left and right caps (radius h/2), one fill_rect run
   per pixel row; with w == h it is a disc. The unread pills, the jump button
   and its badge read as the desktop client's round chrome without any new
   backend primitive. A pill narrower than tall keeps the plain box. */
static void tg_gui_fill_pill(tg_gui_backend *backend, int pen, tg_gui_rect rect)
{
    int y;

    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    if (backend->fill_pill != 0) { /* backend smooths the edge where it can */
        backend->fill_pill(backend, pen, rect);
        return;
    }
    if (rect.w < rect.h) {
        backend->fill_rect(backend, pen, rect);
        return;
    }
    for (y = 0; y < rect.h; ++y) {
        int inset = tg_gui_round_inset(y, rect.h);
        int w = rect.w - (2 * inset);

        if (w > 0) {
            backend->fill_rect(backend, pen,
                               tg_gui_make_rect(rect.x + inset, rect.y + y,
                                                w, 1));
        }
    }
}

/* The blinking caret, aligned to the glyph cell of the line it sits on.
   Text is drawn from its BASELINE, so a caret positioned from the line box
   alone floats above the letters by the font's descender depth: invisible with
   topaz 8, obvious with a tall default font (MorphOS field report, 0.0.9).
   Backends that expose the font ascent get an exact fit; the others keep the
   previous approximation. */
static void tg_gui_draw_caret(tg_gui_backend *backend, int pen, int x,
                              int baseline)
{
    int lh = backend->line_height(backend);
    int ascent;
    int h;

    if (lh <= 0) {
        return;
    }
    /* line_height carries the leading; the glyph cell itself is what the
       caret should cover. */
    h = (lh > 2) ? (lh - 2) : lh;
    ascent = (backend->font_ascent != 0) ? backend->font_ascent(backend) : h;
    if (ascent <= 0 || ascent > lh) {
        ascent = h;
    }
    backend->fill_rect(backend, pen, tg_gui_make_rect(x, baseline - ascent, 2, h));
}

static void tg_gui_draw_clipped(tg_gui_backend *backend, int pen, int x,
                                int baseline, const char *text, int max_w)
{
    unsigned long n;
    int dots_w;
    int budget;
    unsigned long fit;

    if (text == 0 || max_w <= 0) {
        return;
    }
    n = (unsigned long)strlen(text);
    if (n == 0UL) {
        return;
    }
    if (backend->text_width(backend, text, n) <= max_w) {
        backend->draw_text(backend, pen, x, baseline, text, n);
        return;
    }
    dots_w = backend->text_width(backend, "..", 2UL);
    budget = max_w - dots_w;
    if (budget <= 0) {
        return; /* not even room for the ellipsis */
    }
    fit = n;
    while (fit > 0UL &&
           backend->text_width(backend, text, fit) > budget) {
        --fit;
    }
    if (fit > 0UL) {
        backend->draw_text(backend, pen, x, baseline, text, fit);
        backend->draw_text(backend, pen,
                           x + backend->text_width(backend, text, fit), baseline,
                           "..", 2UL);
    }
}

/* Draws a vertical scrollbar: a full-height track with a proportional knob.
   total/view/offset are in the panel's own units (chat rows or messages);
   offset is measured from the top (0..total-view). No knob when all fits. */
static void tg_gui_paint_scrollbar(tg_gui_backend *backend, int x, int track_y,
                                   int track_h, int total, int view, int offset,
                                   int *out_knob_y, int *out_knob_h)
{
    int knob_h;
    int knob_y;
    int span;
    int max_off;

    knob_y = track_y;
    knob_h = (track_h > 0) ? track_h : 0;
    if (track_h <= 0) {
        if (out_knob_y) {
            *out_knob_y = knob_y;
        }
        if (out_knob_h) {
            *out_knob_h = knob_h;
        }
        return;
    }
    backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                       tg_gui_make_rect(x, track_y, TG_GUI_SCROLLBAR_W, track_h));
    if (total <= view || view <= 0) {
        /* Everything fits: inert full track, no draggable knob. */
        if (out_knob_y) {
            *out_knob_y = track_y;
        }
        if (out_knob_h) {
            *out_knob_h = track_h;
        }
        return;
    }
    knob_h = (track_h * view) / total;
    if (knob_h < TG_GUI_SCROLLBAR_MIN_KNOB) {
        knob_h = TG_GUI_SCROLLBAR_MIN_KNOB;
    }
    if (knob_h > track_h) {
        knob_h = track_h;
    }
    span = track_h - knob_h;
    max_off = total - view;
    if (offset < 0) {
        offset = 0;
    }
    if (offset > max_off) {
        offset = max_off;
    }
    knob_y = track_y + (max_off > 0 ? (span * offset) / max_off : 0);
    backend->fill_rect(backend, TG_GUI_PEN_TEXT_DIM,
                       tg_gui_make_rect(x + 2, knob_y, TG_GUI_SCROLLBAR_W - 4,
                                        knob_h));
    if (out_knob_y) {
        *out_knob_y = knob_y;
    }
    if (out_knob_h) {
        *out_knob_h = knob_h;
    }
}

/* The sidebar search box (top strip): its own background + the query/placeholder
   + a caret that blinks when focused. Standalone so the caret blink can repaint
   just this strip (via tg_gui_paint_caret) instead of the whole window. */
static int tg_gui_input_text_w(int width, int sidebar_w);
static int tg_gui_input_rows(const tg_gui_state *state, tg_gui_backend *backend,
                             int width, int sidebar_w);

/* Byte offset in text[0..len] whose glyph boundary sits closest to dx --
   the shared half of click-to-place-the-caret (F8). Latin-1: 1 byte = 1
   glyph, so a linear boundary scan is exact. O(len) text_width calls, only
   on a click. */
static int tg_gui_text_click_offset(tg_gui_backend *backend, const char *text,
                                    unsigned long len, int dx)
{
    unsigned long i;
    int prev = 0;

    if (dx <= 0) {
        return 0;
    }
    for (i = 1UL; i <= len; ++i) {
        int w = backend->text_width(backend, text, i);

        if (dx < (prev + w + 1) / 2) {
            return (int)(i - 1UL); /* closer to the previous boundary */
        }
        if (dx < w) {
            return (int)i;
        }
        prev = w;
    }
    return (int)len;
}

/* Click over the '@' mention popup -> the 0-based candidate index under the
   pointer, or -1 when the pointer is outside it. Recomputes the exact
   geometry tg_gui_paint_input_row draws the popup with, so a click lands on
   the same row the eye sees. Returns -1 unless the popup is actually up. */
int tg_gui_mention_click(const tg_gui_state *state, tg_gui_backend *backend,
                         int x, int y)
{
    int width;
    int height;
    int lh;
    int sidebar_w;
    int status_h;
    int content_h;
    int rows;
    int input_h;
    int box_top;
    int ih;
    int n;
    int bw;
    int bh;
    int bx;
    int by;
    int idx;

    if (state == 0 || backend == 0 || state->mode != TG_GUI_MODE_CHAT ||
        !state->composing || !state->mention_active ||
        state->mention_count <= 0) {
        return -1;
    }
    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return -1;
    }
    sidebar_w = tg_gui_sidebar_w(width);
    status_h = lh + 6;
    content_h = height - status_h;
    rows = tg_gui_input_rows(state, backend, width, sidebar_w);
    input_h = (rows * lh) + 14;
    box_top = content_h - input_h;
    ih = lh + 4;
    n = state->mention_count;
    bw = 220;
    bh = (n * ih) + 4;
    bx = sidebar_w + 8;
    by = box_top - bh - 2;
    if (state->reply_to_id != 0UL) {
        by -= (lh + 4 + TG_GUI_REPLY_LIFT);
    }
    if (bx + bw > width - 8) {
        bw = width - 8 - bx;
    }
    if (by < 0) {
        by = 0;
    }
    if (x < bx || x >= bx + bw || y < by + 2 || y >= by + 2 + (n * ih)) {
        return -1;
    }
    idx = (y - (by + 2)) / ih;
    if (idx < 0 || idx >= n) {
        return -1;
    }
    return idx;
}

/* Click in the sidebar search box -> caret byte offset in search_query, or -1
   when the click is outside the box. Geometry mirrors the search painter. */
int tg_gui_search_click_caret(const tg_gui_state *state,
                              tg_gui_backend *backend, int x, int y)
{
    int width;
    int lh;
    int sidebar_w;

    if (state == 0 || backend == 0) {
        return -1;
    }
    width = backend->width(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || lh <= 0) {
        return -1;
    }
    sidebar_w = tg_gui_sidebar_w(width);
    if (x < 0 || x >= sidebar_w || y < 0 || y >= lh + 10) {
        return -1;
    }
    return tg_gui_text_click_offset(backend, state->search_query,
                                    (unsigned long)strlen(state->search_query),
                                    x - 10);
}

/* Click in the composer -> caret byte offset in input[], or -1 outside the
   text band. Mirrors tg_gui_paint_input_row's geometry exactly: same wrap,
   same last-`rows`-lines window, same per-line baselines. */
int tg_gui_input_click_caret(const tg_gui_state *state,
                             tg_gui_backend *backend, int x, int y)
{
    unsigned long starts[TG_GUI_WRAP_MAX_LINES];
    unsigned long lengths[TG_GUI_WRAP_MAX_LINES];
    int width;
    int height;
    int lh;
    int sidebar_w;
    int area_x;
    int rows;
    int input_h;
    int box_top;
    int n;
    int first;
    int line;

    if (state == 0 || backend == 0 || state->mode != TG_GUI_MODE_CHAT) {
        return -1;
    }
    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return -1;
    }
    sidebar_w = tg_gui_sidebar_w(width);
    area_x = sidebar_w + 12;
    rows = tg_gui_input_rows(state, backend, width, sidebar_w);
    input_h = (rows * lh) + 14;
    box_top = (height - (lh + 6)) - input_h;
    if (state->input[0] == '\0') {
        return 0; /* empty input: anywhere in the box is offset 0 */
    }
    n = tg_gui_wrap(backend, state->input,
                    tg_gui_input_text_w(width, sidebar_w), starts, lengths,
                    TG_GUI_WRAP_MAX_LINES);
    if (n < 1) {
        n = 1;
    }
    first = (n > rows) ? (n - rows) : 0;
    line = first + ((y - (box_top + 2)) / lh);
    if (line < first) {
        line = first;
    }
    if (line >= n) {
        line = n - 1;
    }
    return (int)starts[line] +
           tg_gui_text_click_offset(backend, state->input + starts[line],
                                    lengths[line], x - area_x);
}

static void tg_gui_paint_search_box(const tg_gui_state *state,
                                    tg_gui_backend *backend)
{
    int width;
    int lh;
    int sidebar_w;
    int search_h;
    int sbase;

    width = backend->width(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || lh <= 0) {
        return;
    }
    sidebar_w = tg_gui_sidebar_w(width);
    search_h = lh + 10;
    sbase = (search_h / 2) + 4;
    backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                       tg_gui_make_rect(0, 0, sidebar_w, search_h));
    if (state->search_query[0] != '\0') {
        backend->draw_text(backend, TG_GUI_PEN_TEXT, 10, sbase,
                           state->search_query,
                           (unsigned long)strlen(state->search_query));
    } else {
        const char *placeholder;
        unsigned long placeholder_len;

        if (state->forward_pick_active) {
            placeholder = "Choose destination...";
            placeholder_len = 21UL;
        } else if (state->search_active) {
            placeholder = "Type a name, ENTER...";
            placeholder_len = 20UL;
        } else {
            placeholder = "Search chats...";
            placeholder_len = 15UL;
        }
        backend->draw_text(backend, TG_GUI_PEN_TEXT_DIM, 10, sbase,
                           placeholder, placeholder_len);
    }
    if (state->search_active && state->cursor_on) {
        int cx;
        unsigned long qlen = (unsigned long)strlen(state->search_query);
        unsigned long sc = (unsigned long)((state->search_caret >= 0)
                                               ? state->search_caret
                                               : 0);

        if (sc > qlen) {
            sc = qlen;
        }
        cx = 10 + backend->text_width(backend, state->search_query, sc) + 1;
        tg_gui_draw_caret(backend, TG_GUI_PEN_TEXT, cx, sbase);
    }
}

static void tg_gui_paint_sidebar(const tg_gui_state *state,
                                 tg_gui_backend *backend, int sidebar_w,
                                 int content_h, int lh)
{
    int search_h;
    int row_h;
    int y;
    int i;
    int view_rows;
    int need_bar;
    int list_w;
    tg_gui_state *st;

    backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                       tg_gui_make_rect(0, 0, sidebar_w, content_h));

    search_h = lh + 10;
    tg_gui_paint_search_box(state, backend);

    row_h = (2 * lh) + 12;
    view_rows = (row_h > 0) ? ((content_h - search_h) / row_h) : 0;
    if (view_rows < 1) {
        view_rows = 1;
    }
    need_bar = state->chat_count > view_rows;
    list_w = need_bar ? (sidebar_w - TG_GUI_SCROLLBAR_W) : sidebar_w;
    /* The painter owns the geometry, so it clamps the scroll offset the event
       loop advanced freely (cast away const to write the model's own field). */
    st = (tg_gui_state *)state;
    if (st->chat_scroll > state->chat_count - view_rows) {
        st->chat_scroll = state->chat_count - view_rows;
    }
    if (st->chat_scroll < 0) {
        st->chat_scroll = 0;
    }
    /* One-shot: a fresh selection (chat opened / search result) scrolls the list
       so the selected chat is visible -- without snapping back when you scroll the
       list manually on later frames. */
    if (st->chat_scroll_to_sel) {
        int target = (state->nav_chat >= 0) ? state->nav_chat
                                            : state->selected_chat;

        st->chat_scroll_to_sel = 0;
        if (target < st->chat_scroll) {
            st->chat_scroll = target;
        } else if (target >= st->chat_scroll + view_rows) {
            st->chat_scroll = target - view_rows + 1;
        }
        if (st->chat_scroll > state->chat_count - view_rows) {
            st->chat_scroll = state->chat_count - view_rows;
        }
        if (st->chat_scroll < 0) {
            st->chat_scroll = 0;
        }
    }
    y = search_h;
    for (i = state->chat_scroll;
         i < state->chat_count && y + row_h <= content_h; ++i) {
        const tg_gui_chat *chat;
        int avatar;
        int text_x;
        int name_pen;
        int preview_pen;

        chat = &state->chats[i];
        /* The selected row is set apart by its tint and accent bar; the text
           pens are the same as an unselected row. */
        name_pen = TG_GUI_PEN_TEXT;
        preview_pen = TG_GUI_PEN_TEXT_DIM;
        if (i == state->selected_chat) {
            backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                               tg_gui_make_rect(0, y, sidebar_w, row_h));
            backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                               tg_gui_make_rect(0, y, 3, row_h));
        } else if (i == state->nav_chat) {
            /* Arrow-key focus: tint only (the OPEN chat keeps its accent
               bar), so "where ENTER will land" reads at a glance. */
            backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                               tg_gui_make_rect(0, y, sidebar_w, row_h));
        }
        /* The row being dragged for reorder gets a tint so it reads as "lifted". */
        if (state->drag_active && i == state->drag_src) {
            backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                               tg_gui_make_rect(0, y, sidebar_w, row_h));
        }

        avatar = (2 * lh);
        /* The background this row painted under its round chrome: the edge
           smoothing must blend toward it, or the ring reads as a halo. */
        backend->round_bg =
            (i == state->selected_chat || i == state->nav_chat ||
             (state->drag_active && i == state->drag_src))
                ? TG_GUI_PEN_SELECT
                : TG_GUI_PEN_WINDOW;
        /* Real avatar first (decoded stripped thumb, when the backend and the
           store have one); the classic colored-initials square is the fallback
           and the only path on backends without image support. */
        if (backend->avatar_image == 0 ||
            !backend->avatar_image(backend, chat->peer_id_hi,
                                   chat->peer_id_lo,
                                   tg_gui_make_rect(8, y + 6, avatar,
                                                    avatar))) {
            backend->avatar_fill(backend, chat->avatar_color,
                                 tg_gui_make_rect(8, y + 6, avatar, avatar));
            /* Centre the initials in the circle: measured width across, and
               the shared centred baseline down. The old hand-tuned formula
               (half a cell below the middle, minus a guessed descent) was
               calibrated on topaz 8 and rode high on the taller fonts other
               systems default to, MorphOS among them. */
            {
                unsigned long ilen = (unsigned long)strlen(chat->initials);
                int iw = backend->text_width(backend, chat->initials, ilen);
                int ix = 8 + ((avatar - iw) / 2);
                int iy = tg_gui_centred_baseline(backend, y + 6, avatar);

                if (ix < 8) {
                    ix = 8;
                }
                backend->draw_text(backend, TG_GUI_PEN_TEXT, ix, iy,
                                   chat->initials, ilen);
            }
        }

        text_x = 8 + avatar + 8;
        {
            char badge[8];
            int badge_w;
            int badge_x;
            int has_preview;
            int row_mid;
            int name_baseline;
            int name_limit;

            /* The peer cache carries no last-message preview, so a row is a
               single line: the name and the unread badge sit on the avatar's
               vertical centre (level with the initials), instead of name-on-top
               with the badge a line below. When a preview is present (a future
               per-chat fill) the row keeps the two-line name-over-preview
               layout with the time top-right. */
            has_preview = chat->preview[0] != '\0';
            row_mid = y + 6 + lh; /* avatar centre == initials baseline */
            /* Single line: drop the name a couple of pixels below the initials
               baseline so it sits at the optical centre between the avatar and
               badge bubbles (the bitmap-font descent makes a baseline-aligned
               name read high). */
            name_baseline = has_preview ? (y + 4 + lh) : (row_mid + 2);

            badge[0] = '\0';
            badge_w = 0;
            badge_x = list_w - 10;
            if (chat->unread > 0) {
                int num_w;

                sprintf(badge, "%d", chat->unread > 999 ? 999 : chat->unread);
                num_w = backend->text_width(backend, badge,
                                            (unsigned long)strlen(badge));
                badge_w = num_w + 10; /* horizontal padding inside the pill */
                if (badge_w < lh + 6) {
                    badge_w = lh + 6; /* keep it pill-shaped for one digit */
                }
                badge_x = list_w - 8 - badge_w;
            }
            /* Name clips before the time column (two-line) or before the badge
               / row edge (single-line). */
            if (has_preview) {
                name_limit = list_w - 38;
            } else {
                name_limit = (chat->unread > 0) ? (badge_x - 6)
                                                : (list_w - 10);
            }
            tg_gui_draw_clipped(backend, name_pen, text_x, name_baseline,
                                chat->name, name_limit - text_x);
            if (has_preview) {
                int preview_limit;

                backend->draw_text(backend, TG_GUI_PEN_TEXT_DIM, sidebar_w - 34,
                                   y + 4 + lh, chat->time,
                                   (unsigned long)strlen(chat->time));
                preview_limit = (chat->unread > 0) ? (badge_x - 4)
                                                   : (sidebar_w - 10);
                tg_gui_draw_clipped(backend, preview_pen, text_x,
                                    y + 8 + (2 * lh), chat->preview,
                                    preview_limit - text_x);
            }
            if (chat->unread > 0) {
                int badge_top;
                int badge_h;
                int num_x;

                badge_h = lh + 4;
                /* Two-line: pill on the preview line. Single-line: pill centred
                   on the row middle, level with the name and avatar. The count
                   stays inside the fill either way. */
                badge_top = has_preview ? ((y + 8 + (2 * lh)) - lh)
                                        : (row_mid - (badge_h / 2));
                /* A chat that just got a notification draws its badge in the
                   accent pen to stand out; the live event loop toggles
                   chat->flash for a true blink. */
                tg_gui_fill_pill(backend,
                                 chat->flash ? TG_GUI_PEN_ACCENT
                                             : TG_GUI_PEN_BADGE,
                                 tg_gui_make_rect(badge_x, badge_top, badge_w,
                                                  badge_h));
                num_x = badge_x +
                        (badge_w - backend->text_width(
                                       backend, badge,
                                       (unsigned long)strlen(badge))) /
                            2;
                /* Centred in the pill both ways: horizontally above, and
                   vertically from the font's own ascent. */
                backend->draw_text(backend, TG_GUI_PEN_BADGE_TEXT, num_x,
                                   tg_gui_centred_baseline(backend, badge_top,
                                                           badge_h),
                                   badge, (unsigned long)strlen(badge));
            }
        }
        y += row_h;
    }
    if (need_bar) {
        int ky;
        int kh;

        tg_gui_paint_scrollbar(backend, sidebar_w - TG_GUI_SCROLLBAR_W, search_h,
                               content_h - search_h, state->chat_count,
                               view_rows, state->chat_scroll, &ky, &kh);
        st->sb_list_x = sidebar_w - TG_GUI_SCROLLBAR_W;
        st->sb_list_ty = search_h;
        st->sb_list_th = content_h - search_h;
        st->sb_list_ky = ky;
        st->sb_list_kh = kh;
        st->sb_list_max = state->chat_count - view_rows;
    } else {
        st->sb_list_max = 0;
    }
    /* Drag-drop insertion line: a 2px accent bar at the gap the drop would land in
       (same target math as tg_gui_chat_drop_target), clamped to the visible band. */
    if (state->drag_active) {
        int target = tg_gui_chat_drop_target(state, lh, state->drag_cur_y);
        int vis = target - state->chat_scroll;
        int bar_y;

        if (vis < 0) {
            vis = 0;
        }
        if (vis > view_rows) {
            vis = view_rows;
        }
        bar_y = search_h + (vis * row_h);
        backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                           tg_gui_make_rect(0, bar_y - 1, list_w, 2));
    }
}

/* Draws one wrapped line of message text, interpreting the inline markup
   markers (* _ ` ~) as styling: each marker toggles a TG_GUI_STYLE_* bit, is
   not drawn itself, and the run between markers is drawn in the current style
   via the backend's set_style hook. *style carries across wrapped lines so a
   span that breaks over two lines stays styled. Backends without set_style get
   clean text (markers skipped), just unstyled. */
/* Length of the web link starting at text[i], or 0 when there is none.
   Word-anchored (the caller only asks at word starts) and stops at the
   first blank; trailing punctuation is left out so a link at the end of a
   sentence does not swallow the full stop -- the same trimming the click
   handler does, so what is underlined is what opens. */
static unsigned long tg_gui_link_span(const char *text, unsigned long i,
                                      unsigned long length)
{
    unsigned long n = length - i;
    unsigned long end;

    if ((n > 7UL && strncmp(text + i, "http://", 7) == 0) ||
        (n > 8UL && strncmp(text + i, "https://", 8) == 0) ||
        (n > 4UL && strncmp(text + i, "www.", 4) == 0)) {
        end = i;
        while (end < length && text[end] != ' ' && text[end] != '\t') {
            ++end;
        }
        while (end > i && (text[end - 1UL] == '.' || text[end - 1UL] == ',' ||
                           text[end - 1UL] == ';' || text[end - 1UL] == ':' ||
                           text[end - 1UL] == '!' || text[end - 1UL] == '?' ||
                           text[end - 1UL] == ')' || text[end - 1UL] == ']' ||
                           text[end - 1UL] == '>' || text[end - 1UL] == '"')) {
            --end;
        }
        return end - i;
    }
    return 0UL;
}

/* Draws one wrapped line of message text, interpreting the inline markup
   markers (* _ ` ~) as styling: each marker toggles a TG_GUI_STYLE_* bit, is
   not drawn itself, and the run between markers is drawn in the current style
   via the backend's set_style hook. *style carries across wrapped lines so a
   span that breaks over two lines stays styled. Web links are drawn in
   link_pen and underlined (0.0.8) so a clickable URL looks clickable.
   Backends without set_style get clean text (markers skipped), just
   unstyled. */
static void tg_gui_draw_markup(tg_gui_backend *backend, int pen, int link_pen,
                               int x, int baseline, const char *text,
                               unsigned long length, int *style)
{
    unsigned long run_start = 0UL;
    unsigned long link_left = 0UL; /* chars of the current link still to draw */
    unsigned long i;

    for (i = 0UL; i <= length; ++i) {
        int toggle = 0;
        int boundary = 0;

        if (i < length) {
            /* Markup markers are text INSIDE a link: an underscore in
               en.wikipedia.org/wiki/Amiga_500 is part of the address, not an
               italic switch. Reading it as markup dropped the byte from the
               drawn URL and left the rest of the message italic. */
            switch (link_left > 0UL ? '\0' : text[i]) {
            case '*':
                toggle = TG_GUI_STYLE_BOLD;
                break;
            case '_':
                toggle = TG_GUI_STYLE_ITALIC;
                break;
            case '`':
                toggle = TG_GUI_STYLE_CODE;
                break;
            case '~':
                toggle = TG_GUI_STYLE_STRIKE;
                break;
            default:
                break;
            }
            if (link_left == 0UL &&
                (i == 0UL || text[i - 1UL] == ' ' || text[i - 1UL] == '\t')) {
                unsigned long span = tg_gui_link_span(text, i, length);

                if (span > 0UL) {
                    link_left = span;
                    boundary = 1; /* flush the plain run before the link */
                }
            }
        }
        if (toggle != 0 || boundary || i == length) {
            if (i > run_start) {
                if (backend->set_style != 0) {
                    backend->set_style(backend, link_left > 0UL && !boundary
                                                    ? (*style |
                                                       TG_GUI_STYLE_UNDERLINE)
                                                    : *style);
                }
                backend->draw_text(backend,
                                   (link_left > 0UL && !boundary) ? link_pen
                                                                  : pen,
                                   x, baseline, text + run_start,
                                   i - run_start);
                x += backend->text_width(backend, text + run_start,
                                         i - run_start);
            }
            if (toggle != 0) {
                *style ^= toggle;
                run_start = i + 1UL;
            } else {
                run_start = i; /* link boundary: the run resumes here */
            }
        }
        if (link_left > 0UL && i < length) {
            --link_left;
            if (link_left == 0UL) {
                /* Link ends AFTER this char: flush it in link style now. */
                unsigned long end = i + 1UL;

                if (end > run_start) {
                    if (backend->set_style != 0) {
                        backend->set_style(backend,
                                           *style | TG_GUI_STYLE_UNDERLINE);
                    }
                    backend->draw_text(backend, link_pen, x, baseline,
                                       text + run_start, end - run_start);
                    x += backend->text_width(backend, text + run_start,
                                             end - run_start);
                }
                run_start = end;
            }
        }
    }
}

/* The read-receipt mark drawn after an own message's timestamp. The bitmap fonts
   carry no check glyph, so we draw a real tick by hand (like the jump button's
   triangle): 1 check = sent (in the cloud), 2 = read by the peer. Telegram has no
   distinct "delivered to device" state for cloud chats, so there is no third
   mark. Returns 0 (none) / 1 (sent) / 2 (read) -- 0 for incoming + unsent echoes. */
static int tg_gui_check_count(const tg_gui_message *message)
{
    if (message->is_own) {
        if (message->read_state == TG_GUI_READ_SEEN) {
            return 2;
        }
        if (message->read_state == TG_GUI_READ_SENT) {
            return 1;
        }
    }
    return 0;
}

/* One check's side length for a given line height (a tick is ~square). Kept to
   ~half the line so it sits on the timestamp baseline without out-growing the
   digits and bleeding into the text line above (was lh-3, too tall on big OS4
   fonts). */
static int tg_gui_check_size(int lh)
{
    int ch = lh / 2;

    return (ch < 5) ? 5 : ch;
}

/* Pixel width the status line must reserve for `count` checks: the second check
   overlaps the first by half (Telegram-style), so two are 1.5 ticks wide. */
static int tg_gui_check_width(int count, int lh)
{
    int ch = tg_gui_check_size(lh);

    if (count <= 0) {
        return 0;
    }
    return (count >= 2) ? (ch + ((ch + 1) / 2)) : ch;
}

/* A short axis-aligned-stepped line (the backend has only fill_rect / draw_text),
   `t` px thick -- enough to render a clean tick at status-line sizes. */
static void tg_gui_draw_seg(tg_gui_backend *backend, int pen, int x0, int y0,
                            int x1, int y1, int t)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int steps = (dx > dy) ? dx : dy;
    int i;

    if (steps < 1) {
        steps = 1;
    }
    for (i = 0; i <= steps; ++i) {
        int x = x0 + (((x1 - x0) * i) / steps);
        int y = y0 + (((y1 - y0) * i) / steps);

        backend->fill_rect(backend, pen, tg_gui_make_rect(x, y, t, t));
    }
}

/* One tick with its top-left at (x, top), side `ch`: a short down-right arm into
   the bottom vertex, then a long up-right arm. */
static void tg_gui_draw_one_check(tg_gui_backend *backend, int pen, int x,
                                  int top, int ch, int t)
{
    int vx = x + ((ch * 2) / 5); /* the bottom vertex, ~0.4 from the left */
    int vy = top + ch - 1;

    tg_gui_draw_seg(backend, pen, x, top + ((ch + 1) / 2), vx, vy, t);
    tg_gui_draw_seg(backend, pen, vx, vy, x + ch - 1, top, t);
}

/* The receipt mark sitting on the status baseline: one tick for sent, two for
   read. The read pair is drawn in the azure read pen so it changes colour when
   the peer reads the message (the sent tick uses the dim timestamp pen). */
static void tg_gui_draw_checks(tg_gui_backend *backend, int x, int baseline,
                               int lh, int count, int sent_pen)
{
    int ch = tg_gui_check_size(lh);
    int t = (ch >= 9) ? 2 : 1;
    int top = baseline - ch;
    int pen = (count >= 2) ? TG_GUI_PEN_READ : sent_pen;

    if (count <= 0) {
        return;
    }
    tg_gui_draw_one_check(backend, pen, x, top, ch, t);
    if (count >= 2) {
        tg_gui_draw_one_check(backend, pen, x + ((ch + 1) / 2), top, ch, t);
    }
}

/* Shared bubble geometry: the wrap, the bubble box and the header/reply
   bands, EXACTLY as the painter lays them out -- used by the painter, the
   char-level hit test and nothing else, so they can never drift apart. */
typedef struct tg_gui_bubble_geom {
    int pad;
    int bubble_x;
    int bubble_w;
    int header_h;
    int reply_h;
    int photo_w;
    int photo_h;
    int photo_gap;
    int line_count;
    unsigned long starts[TG_GUI_WRAP_MAX_LINES];
    unsigned long lengths[TG_GUI_WRAP_MAX_LINES];
} tg_gui_bubble_geom;

#if defined(__m68k__)
#define TG_GUI_INLINE_PHOTO_MAX_W 256
#define TG_GUI_INLINE_PHOTO_MAX_H 256
#else
#define TG_GUI_INLINE_PHOTO_MAX_W 448
#define TG_GUI_INLINE_PHOTO_MAX_H 448
#endif

static void tg_gui_photo_geometry(const tg_gui_message *message,
                                  int content_w, int *out_w, int *out_h)
{
    int max_w;
    int w;
    int h;

    *out_w = 0;
    *out_h = 0;
    if (message == 0 || !message->has_photo ||
        message->photo_width == 0UL || message->photo_height == 0UL ||
        content_w <= 0) {
        return;
    }
    /* Photos use roughly 60% of the transcript column. This leaves visible
       chat context around them while allowing a substantially larger source
       than the first 0.0.9 candidate. */
    max_w = (content_w * 3) / 4;
    if (max_w > TG_GUI_INLINE_PHOTO_MAX_W) {
        max_w = TG_GUI_INLINE_PHOTO_MAX_W;
    }
    if (max_w < 1) {
        max_w = 1;
    }
    if (max_w > content_w) {
        max_w = content_w;
    }
    w = (message->photo_width < (unsigned long)max_w)
            ? (int)message->photo_width : max_w;
    h = (int)((message->photo_height * (unsigned long)w) /
              message->photo_width);
    if (h < 1) {
        h = 1;
    }
    if (h > TG_GUI_INLINE_PHOTO_MAX_H) {
        w = (int)(((unsigned long)w * TG_GUI_INLINE_PHOTO_MAX_H) /
                  (unsigned long)h);
        h = TG_GUI_INLINE_PHOTO_MAX_H;
    }
    if (w < 1) {
        w = 1;
    }
    *out_w = w;
    *out_h = h;
}

/* Width of text[0..len) AS RENDERED by tg_gui_draw_markup: the style marker
   chars (* _ ` ~) are elided there and never advance x, so they must not be
   measured either -- or the selection tint and the char-under-pointer mapping
   drift right of the glyphs by one marker width each. Amiga font widths are
   additive (no kerning), so summing the runs equals the drawn advance. */
static int tg_gui_marked_width(tg_gui_backend *backend, const char *text,
                               unsigned long from, unsigned long to)
{
    unsigned long i;
    unsigned long run;
    int w = 0;

    run = from;
    for (i = from; i <= to; ++i) {
        int marker = 0;

        if (i < to) {
            marker = (text[i] == '*' || text[i] == '_' || text[i] == '`' ||
                      text[i] == '~');
        }
        if (marker || i == to) {
            if (i > run) {
                w += backend->text_width(backend, text + run, i - run);
            }
            run = i + 1UL;
        }
    }
    return w;
}

/* A message joins the run above it when both are incoming, from the same
   sender, with nothing else (an own message, a day separator, a system line)
   between them: the sender name then shows only on the first of the run, the
   way the desktop client groups a busy conversation. Pure derivation from
   the two neighbours, so every geometry consumer computes the same answer. */
static int tg_gui_message_grouped(const tg_gui_state *state, int index)
{
    const tg_gui_message *m;
    const tg_gui_message *prev;

    if (state == 0 || index <= 0 || index >= state->message_count) {
        return 0;
    }
    m = &state->messages[index];
    prev = &state->messages[index - 1];
    if (m->is_own || m->is_system || prev->is_own || prev->is_system) {
        return 0;
    }
    if (m->sender[0] == '\0' || strcmp(m->sender, prev->sender) != 0) {
        return 0;
    }
    return 1;
}

/* What stands in for an image that is not being drawn: the marker with the
   click target that opens the viewer on demand. It says "[Photo]" for a photo,
   but a sticker's own label ("[Sticker :)]") says more for the same space, and
   for a photo_only message that label IS the whole message text. Anything
   longer than a marker falls back, so the box never grows into a paragraph. */
#define TG_GUI_PHOTO_PLACEHOLDER_MAX 24U
static const char *tg_gui_photo_placeholder(const tg_gui_message *message)
{
    if (message->photo_only && message->text[0] == '[' &&
        strlen(message->text) < TG_GUI_PHOTO_PLACEHOLDER_MAX) {
        return message->text;
    }
    return "[Photo]";
}

static void tg_gui_bubble_geometry(tg_gui_backend *backend,
                                   const tg_gui_message *message, int area_x,
                                   int area_w, int lh, int inline_photos,
                                   int grouped, tg_gui_bubble_geom *geo)
{
    int max_bubble_w;
    int widest;
    int k;
    int has_time;
    int time_w;
    int check_count;
    int has_mark;
    int read_mark_w;
    int status_w;

    geo->pad = 8;
    max_bubble_w = (area_w * 78) / 100;
    if (max_bubble_w < 40) {
        /* Narrow window: keep a right gutter rather than touching the edge. */
        max_bubble_w = (area_w > 2 * geo->pad) ? (area_w - 2 * geo->pad)
                                               : area_w;
    }
    if (message->has_photo && !inline_photos) {
        const char *ph = tg_gui_photo_placeholder(message);

        geo->photo_w = backend->text_width(backend, ph,
                                           (unsigned long)strlen(ph));
        geo->photo_h = lh;
    } else {
        tg_gui_photo_geometry(message, max_bubble_w - (2 * geo->pad),
                              &geo->photo_w, &geo->photo_h);
    }
    geo->photo_gap = geo->photo_h > 0 ? 4 : 0;
    if (message->photo_only && geo->photo_h > 0) {
        geo->line_count = 0; /* the downloaded image replaces "[Photo]" */
    } else {
        geo->line_count = tg_gui_wrap(
            backend, message->text, max_bubble_w - (2 * geo->pad),
            geo->starts, geo->lengths, TG_GUI_WRAP_MAX_LINES);
        if (geo->line_count <= 0) {
            geo->line_count = 1;
            geo->starts[0] = 0UL;
            geo->lengths[0] = 0UL;
        }
    }
    widest = geo->photo_w;
    for (k = 0; k < geo->line_count; ++k) {
        int w;

        w = backend->text_width(backend, message->text + geo->starts[k],
                                geo->lengths[k]);
        if (w > widest) {
            widest = w;
        }
    }
    has_time = (message->time[0] != '\0');
    time_w = has_time ? backend->text_width(
                            backend, message->time,
                            (unsigned long)strlen(message->time))
                      : 0;
    check_count = tg_gui_check_count(message);
    has_mark = (check_count > 0);
    read_mark_w = has_mark ? tg_gui_check_width(check_count, lh) : 0;
    status_w = time_w + read_mark_w + ((has_time && has_mark) ? 4 : 0);
    if (status_w > widest) {
        widest = status_w; /* the bubble must hold the timestamp/receipt line */
    }
    if (message->reply_text[0] != '\0') {
        int reply_w;

        reply_w = backend->text_width(backend, "> ", 2UL) +
                  backend->text_width(
                      backend, message->reply_text,
                      (unsigned long)strlen(message->reply_text));
        if (reply_w > widest) {
            widest = reply_w; /* the quoted reference line must fit too */
        }
    }
    geo->bubble_w = widest + (2 * geo->pad);
    if (geo->bubble_w > max_bubble_w) {
        geo->bubble_w = max_bubble_w;
    }
    geo->bubble_x = message->is_own ? (area_x + area_w - geo->bubble_w)
                                    : area_x;
    /* lh + lh/2: gap below the sender-name baseline wider than the font
       descent (see the painter). A grouped message drops the name entirely
       and keeps a hair of air so the run still reads as separate bubbles. */
    geo->header_h = (message->is_own || grouped) ? 0 : (lh + (lh / 2));
    geo->reply_h = (message->reply_text[0] != '\0') ? lh : 0;
}

/* The bubble fill with its corners clipped: a three-row step (3,1,1) above
   and below a full-width body, which reads as a rounded rectangle at every
   size the transcript produces. Costs four extra fills, no new pens. Each
   rounded edge is skipped when the viewport clip cut that edge, so a bubble
   crossing the top or bottom of the transcript stays flush there. */
static void tg_gui_fill_bubble(tg_gui_backend *backend, int pen, int x,
                               int top, int w, int h, int round_top,
                               int round_bottom)
{
    int r = 3;
    int body_top = top;
    int body_h = h;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (w < (2 * r) + 2 || h < (2 * r) + ((round_top && round_bottom) ? r : 1)) {
        round_top = 0;
        round_bottom = 0;
    }
    if (round_top) {
        backend->fill_rect(backend, pen, tg_gui_make_rect(x + r, top, w - (2 * r), 1));
        backend->fill_rect(backend, pen, tg_gui_make_rect(x + 1, top + 1, w - 2, 2));
        body_top += r;
        body_h -= r;
    }
    if (round_bottom) {
        backend->fill_rect(backend, pen,
                           tg_gui_make_rect(x + 1, top + h - r, w - 2, 2));
        backend->fill_rect(backend, pen,
                           tg_gui_make_rect(x + r, top + h - 1, w - (2 * r), 1));
        body_h -= r;
    }
    backend->fill_rect(backend, pen, tg_gui_make_rect(x, body_top, w, body_h));
}

static int tg_gui_paint_bubble(tg_gui_backend *backend,
                               const tg_gui_message *message, int area_x,
                               int area_w, int y, int lh, int top, int bottom,
                               long sel_lo, long sel_hi, int inline_photos,
                               int grouped, tg_gui_rect *out_photo)
{
    int style;
    unsigned long starts[TG_GUI_WRAP_MAX_LINES];
    unsigned long lengths[TG_GUI_WRAP_MAX_LINES];
    int pad;
    int line_count;
    int k;
    int bubble_w;
    int bubble_x;
    int bubble_h;
    int header_h;
    int fill_pen;
    int text_pen;
    int time_pen;
    int has_time;
    int time_w;
    int has_reply;
    int reply_h;
    int photo_w;
    int photo_h;
    int photo_gap;
    int check_count;
    int read_mark_w;
    int has_mark;
    int has_status;
    int status_w;

    {
        tg_gui_bubble_geom geo;

        tg_gui_bubble_geometry(backend, message, area_x, area_w, lh,
                               inline_photos, grouped, &geo);
        pad = geo.pad;
        line_count = geo.line_count;
        for (k = 0; k < line_count; ++k) {
            starts[k] = geo.starts[k];
            lengths[k] = geo.lengths[k];
        }
        bubble_w = geo.bubble_w;
        header_h = geo.header_h;
        reply_h = geo.reply_h;
        photo_w = geo.photo_w;
        photo_h = geo.photo_h;
        photo_gap = geo.photo_gap;
        bubble_x = geo.bubble_x;
    }
    has_time = (message->time[0] != '\0');
    time_w = has_time ? backend->text_width(backend, message->time,
                                            (unsigned long)strlen(message->time))
                      : 0;
    check_count = tg_gui_check_count(message);
    has_mark = (check_count > 0);
    read_mark_w = has_mark ? tg_gui_check_width(check_count, lh)
                           : 0;
    has_status = has_time || has_mark;
    status_w = time_w + read_mark_w + ((has_time && has_mark) ? 4 : 0);
    (void)status_w;
    has_reply = (message->reply_text[0] != '\0');
    /* Reserve a line inside the bubble for the timestamp / read-receipt mark so
       it stays on the coloured background instead of spilling out below it, plus
       one for the quoted-reply reference line when present. */
    bubble_h = header_h + reply_h + photo_h + photo_gap +
               (line_count * lh) + (has_status ? lh : 0) + 6;

    if (message->is_own) {
        fill_pen = TG_GUI_PEN_ACCENT;
        text_pen = TG_GUI_PEN_ACCENT_TEXT;
        time_pen = TG_GUI_PEN_ACCENT_TEXT;
    } else {
        fill_pen = TG_GUI_PEN_SURFACE;
        text_pen = TG_GUI_PEN_TEXT;
        time_pen = TG_GUI_PEN_TEXT_DIM;
        {
            int sender_pen;

            /* Enforce the avatar-tint band here rather than leaning on the
               backend to clamp an out-of-range sender_color. */
            sender_pen = message->sender_color;
            if (sender_pen < 0 || sender_pen >= TG_GUI_AVATAR_COLORS) {
                sender_pen = 0;
            }
            if (!grouped && (y + lh) <= bottom && y >= top) {
                backend->draw_text(backend, sender_pen + TG_GUI_PEN_COUNT,
                                   bubble_x + 2, y + lh, message->sender,
                                   (unsigned long)strlen(message->sender));
            }
        }
    }

    /* Clip every drawn part to the transcript bottom so a tall bubble near the
       edge cannot bleed into the input row / status bar on a resized window. */
    {
        int fill_top;
        int fill_bottom;

        fill_top = y + header_h;
        fill_bottom = y + bubble_h;
        if (fill_top < top) {
            fill_top = top;
        }
        if (fill_bottom > bottom) {
            fill_bottom = bottom;
        }
        if (fill_bottom > fill_top) {
            tg_gui_fill_bubble(backend, fill_pen, bubble_x, fill_top,
                               bubble_w, fill_bottom - fill_top,
                               fill_top == y + header_h,
                               fill_bottom == y + bubble_h);
        }
    }
    if (has_reply) {
        int reply_baseline;

        reply_baseline = y + header_h + lh;
        if (reply_baseline <= bottom && reply_baseline - lh >= top) {
            char line[TG_GUI_REPLY_MAX + 4];

            line[0] = '>';
            line[1] = ' ';
            tg_gui_copy(line + 2, sizeof(line) - 2, message->reply_text);
            /* Dimmed, plain (no markup), clipped to the bubble width with "..". */
            tg_gui_draw_clipped(backend, time_pen, bubble_x + pad, reply_baseline,
                                line, bubble_w - (2 * pad));
        }
    }
    if (out_photo != 0) {
        *out_photo = tg_gui_make_rect(0, 0, 0, 0);
    }
    if (photo_h > 0) {
        tg_gui_rect photo_rect;
        tg_gui_rect clip;
        int drawn;

        photo_rect = tg_gui_make_rect(bubble_x + pad,
                                      y + header_h + reply_h + 2,
                                      photo_w, photo_h);
        if (out_photo != 0) {
            *out_photo = photo_rect;
        }
        clip = tg_gui_make_rect(area_x, top, area_w, bottom - top);
        drawn = 0;
        if (inline_photos && backend->photo_image != 0) {
            drawn = backend->photo_image(backend, message->photo_id_hi,
                                         message->photo_id_lo,
                                         message->photo_width,
                                         message->photo_height,
                                         photo_rect, clip);
        }
        if (!drawn && photo_rect.y + lh >= top && photo_rect.y < bottom) {
            /* A deferred first decode (notably during NEWSIZE) must not leave
               an unexplained empty bubble. The next stable paint replaces
               this marker with the cached canonical image. */
            const char *ph = tg_gui_photo_placeholder(message);

            backend->draw_text(backend, time_pen, photo_rect.x,
                               photo_rect.y + lh, ph,
                               (unsigned long)strlen(ph));
        }
    }
    style = 0;
    for (k = 0; k < line_count; ++k) {
        int baseline;

        baseline = y + header_h + reply_h + photo_h + photo_gap +
                   (k * lh) + lh;
        if (baseline <= bottom && baseline - lh >= top) {
            /* Mouse selection: tint the selected char range of THIS visual
               line before the glyphs go down, so the text stays crisp on the
               SELECT band (same language as the selected sidebar row). */
            if (sel_hi > sel_lo) {
                long ls = (long)starts[k];
                long le = ls + (long)lengths[k];
                long lo = sel_lo > ls ? sel_lo : ls;
                long hi = sel_hi < le ? sel_hi : le;

                if (hi > lo) {
                    int x0 = bubble_x + pad +
                             tg_gui_marked_width(backend, message->text,
                                                 (unsigned long)ls,
                                                 (unsigned long)lo);
                    int sw = tg_gui_marked_width(backend, message->text,
                                                 (unsigned long)lo,
                                                 (unsigned long)hi);
                    int sy = baseline - lh + 3;
                    int sh = lh;

                    if (sy < top) { /* clip to the transcript viewport */
                        sh -= top - sy;
                        sy = top;
                    }
                    if (sy + sh > bottom) {
                        sh = bottom - sy;
                    }
                    if (sw > 0 && sh > 0) {
                        backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                                           tg_gui_make_rect(x0, sy, sw, sh));
                    }
                }
            }
            /* On an OWN bubble the background is the accent blue, where a
               blue link would vanish: use the azure that the read-receipt
               ticks already prove readable there. */
            tg_gui_draw_markup(backend, text_pen,
                               message->is_own ? TG_GUI_PEN_READ
                                               : TG_GUI_PEN_LINK,
                               bubble_x + pad, baseline,
                               message->text + starts[k], lengths[k], &style);
        }
    }
    if (backend->set_style != 0) {
        backend->set_style(backend, 0); /* reset before time / next bubble */
    }
    if (has_status) {
        int status_baseline;
        int status_x;

        /* One line below the last text line, right-aligned, inside the fill:
           timestamp then (for own messages) the read-receipt mark. */
        status_baseline = y + header_h + reply_h + photo_h + photo_gap +
                          (line_count * lh) + lh;
        status_x = bubble_x + bubble_w - pad - status_w;
        if (status_x < bubble_x + pad) {
            status_x = bubble_x + pad;
        }
        if (status_baseline <= bottom && status_baseline - lh >= top) {
            if (has_time) {
                backend->draw_text(backend, time_pen, status_x, status_baseline,
                                   message->time,
                                   (unsigned long)strlen(message->time));
            }
            if (has_mark) {
                int mark_x;

                mark_x = status_x + time_w + ((has_time) ? 4 : 0);
                tg_gui_draw_checks(backend, mark_x, status_baseline, lh,
                                   check_count, time_pen);
            }
        }
    }
    return bubble_h + 6;
}

/* The vertical space a message occupies (matching what tg_gui_paint_bubble
   advances y by) without drawing -- used to anchor the newest messages to the
   bottom of the transcript so a fresh send/receive is always visible. */
static int tg_gui_message_height(tg_gui_backend *backend,
                                 const tg_gui_message *message, int area_w,
                                 int lh, int inline_photos, int grouped)
{
    tg_gui_bubble_geom geo;
    int has_time;
    int has_status;

    if (message->is_system) {
        return lh + 6;
    }
    tg_gui_bubble_geometry(backend, message, 0, area_w, lh, inline_photos,
                           grouped, &geo);
    has_time = (message->time[0] != '\0');
    /* The status line also shows for an own message's read-receipt mark even
       when it has no timestamp (the optimistic echo). */
    has_status = has_time || (tg_gui_check_count(message) > 0);
    /* bubble_h + 6, mirroring tg_gui_paint_bubble's return (incl. reply line). */
    return geo.header_h + geo.reply_h + geo.photo_h + geo.photo_gap +
           (geo.line_count * lh) + (has_status ? lh : 0) + 6 + 6;
}

/* Draws just the bottom composer row: the input box, the typed text (or the
   placeholder / idle text) with the blinking caret, and the Send button.
   Factored out of tg_gui_paint_main so the caret blink (tg_gui_paint_caret) can
   repaint ONLY this strip instead of the whole window -- a full repaint twice a
   second was visible as a constant refresh on slow planar displays (OS3). The
   geometry is recomputed from the backend so it matches tg_gui_paint_main. */
/* Composer box geometry. The input box grows to WRAP a long message instead of
   running it off the right edge, capped so it never eats the transcript. Shared
   by the input-row painter, tg_gui_paint_main (transcript_bottom) and -- via the
   painter-cached state->input_h -- the hit-test, so all three agree. */
#define TG_GUI_INPUT_MAX_ROWS 4

/* Width available for the typed text: from the text origin to just before the
   Send button at width-64. */
static int tg_gui_input_text_w(int width, int sidebar_w)
{
    int w;

    w = (width - 64) - (sidebar_w + 12) - 8;
    if (w < 20) {
        w = 20;
    }
    return w;
}

/* Number of wrapped input rows to show (1..TG_GUI_INPUT_MAX_ROWS). */
static int tg_gui_input_rows(const tg_gui_state *state, tg_gui_backend *backend,
                             int width, int sidebar_w)
{
    unsigned long starts[TG_GUI_WRAP_MAX_LINES];
    unsigned long lengths[TG_GUI_WRAP_MAX_LINES];
    int n;

    if (state->mode != TG_GUI_MODE_CHAT || state->input[0] == '\0') {
        return 1;
    }
    n = tg_gui_wrap(backend, state->input,
                    tg_gui_input_text_w(width, sidebar_w), starts, lengths,
                    TG_GUI_WRAP_MAX_LINES);
    if (n < 1) {
        n = 1;
    }
    if (n > TG_GUI_INPUT_MAX_ROWS) {
        n = TG_GUI_INPUT_MAX_ROWS;
    }
    return n;
}

static int tg_gui_input_h(const tg_gui_state *state, tg_gui_backend *backend,
                          int width, int sidebar_w, int lh)
{
    int h = (tg_gui_input_rows(state, backend, width, sidebar_w) * lh) + 14;
    if (state->reply_to_id != 0UL) {
        /* room for the "Replying to ..." header strip above the box, plus the
           air that lifts it clear of the composer */
        h += lh + 4 + TG_GUI_REPLY_LIFT;
    }
    return h;
}

int tg_gui_input_layout_height(const tg_gui_state *state,
                               tg_gui_backend *backend)
{
    int width;
    int lh;
    int sidebar_w;

    if (state == 0 || backend == 0 || backend->width == 0 ||
        backend->line_height == 0) {
        return 0;
    }
    width = backend->width(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || lh <= 0) {
        return 0;
    }
    sidebar_w = tg_gui_sidebar_w(width);
    return tg_gui_input_h(state, backend, width, sidebar_w, lh);
}

/* Draws just the bottom composer row: the input box (now wrapped to multiple
   lines for a long message, with the caret on the right line), the placeholder /
   idle text, and the Send button. Factored out so the caret blink can repaint
   ONLY this strip. Geometry is recomputed from the backend to match
   tg_gui_paint_main, and the box height comes from the shared helper above. */
static void tg_gui_paint_input_row(const tg_gui_state *state,
                                   tg_gui_backend *backend)
{
    int width;
    int height;
    int lh;
    int sidebar_w;
    int status_h;
    int content_h;
    int area_x;
    int input_h;
    int box_top;
    int rows;

    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return;
    }
    status_h = lh + 6;
    content_h = height - status_h;
    sidebar_w = tg_gui_sidebar_w(width);
    area_x = sidebar_w + 12;
    rows = tg_gui_input_rows(state, backend, width, sidebar_w);
    input_h = (rows * lh) + 14;
    box_top = content_h - input_h;

    /* When replying, a dim header strip sits ABOVE the box (the box itself does
       not move -- tg_gui_input_h reserved the extra line so the transcript
       shrank instead). It shows "<sender>: <snippet>" with an accent quote bar
       on the left and an "X" cancel hot-spot on the right. */
    if (state->reply_to_id != 0UL) {
        int strip_y = box_top - (lh + 4) - TG_GUI_REPLY_LIFT;
        int strip_h = lh + 2;
        /* Centred in the strip instead of hanging off a line-height guess,
           which left the text on the strip's bottom edge at every font size. */
        int strip_base = tg_gui_centred_baseline(backend, strip_y, strip_h);
        char head[TG_GUI_NAME_MAX + TG_GUI_REPLY_MAX + 4];
        unsigned long hp = 0UL;
        const char *s;

        backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                           tg_gui_make_rect(sidebar_w + 8, strip_y,
                                            width - sidebar_w - 16, strip_h));
        backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                           tg_gui_make_rect(sidebar_w + 8, strip_y, 3, strip_h));
        s = state->reply_sender;
        while (*s != '\0' && hp + 2UL < sizeof(head)) {
            head[hp++] = *s++;
        }
        if (hp + 2UL < sizeof(head)) {
            head[hp++] = ':';
            head[hp++] = ' ';
        }
        s = state->reply_snippet;
        while (*s != '\0' && hp + 1UL < sizeof(head)) {
            head[hp++] = *s++;
        }
        head[hp] = '\0';
        tg_gui_draw_clipped(backend, TG_GUI_PEN_TEXT_DIM, sidebar_w + 16,
                            strip_base, head, width - sidebar_w - 16 - 26);
        backend->draw_text(backend, TG_GUI_PEN_TEXT, width - 22, strip_base,
                           "X", 1UL);
    }

    backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                       tg_gui_make_rect(sidebar_w + 8, box_top,
                                        width - sidebar_w - 16, input_h - 4));
    if (state->input[0] != '\0') {
        unsigned long starts[TG_GUI_WRAP_MAX_LINES];
        unsigned long lengths[TG_GUI_WRAP_MAX_LINES];
        int n;
        int first;
        int k;

        n = tg_gui_wrap(backend, state->input,
                        tg_gui_input_text_w(width, sidebar_w), starts, lengths,
                        TG_GUI_WRAP_MAX_LINES);
        if (n < 1) {
            n = 1;
            starts[0] = 0UL;
            lengths[0] = (unsigned long)strlen(state->input);
        }
        /* If the message wraps past the box, show its LAST `rows` lines so the
           caret (normally at the end while typing) stays in view. */
        first = (n > rows) ? (n - rows) : 0;
        /* Composer selection tint: behind the glyphs, per visual line. */
        if (state->composing && state->in_sel_active) {
            long tl = (long)strlen(state->input);
            long a = (long)state->in_sel_anchor;
            long b = (long)state->input_caret;
            long lo = a < b ? a : b;
            long hi = a > b ? a : b;

            if (lo < 0) {
                lo = 0;
            }
            if (hi > tl) {
                hi = tl;
            }
            for (k = first; k < n && (k - first) < rows && hi > lo; ++k) {
                long ls = (long)starts[k];
                long le = ls + (long)lengths[k];
                long slo = lo > ls ? lo : ls;
                long shi = hi < le ? hi : le;

                if (shi > slo) {
                    int x0 = area_x +
                             backend->text_width(
                                 backend, state->input + ls,
                                 (unsigned long)(slo - ls));
                    int sw = backend->text_width(
                        backend, state->input + slo,
                        (unsigned long)(shi - slo));

                    backend->fill_rect(
                        backend, TG_GUI_PEN_SELECT,
                        tg_gui_make_rect(x0,
                                         box_top + ((k - first) * lh) + 4,
                                         sw, lh));
                }
            }
        }
        for (k = first; k < n && (k - first) < rows; ++k) {
            backend->draw_text(backend, TG_GUI_PEN_TEXT, area_x,
                               box_top + ((k - first) * lh) + lh + 2,
                               state->input + starts[k], lengths[k]);
        }
        if (state->composing && state->cursor_on) {
            unsigned long caret_off;
            int line;

            caret_off = (unsigned long)state->input_caret;
            if (caret_off > (unsigned long)strlen(state->input)) {
                caret_off = (unsigned long)strlen(state->input);
            }
            /* The wrapped line the caret falls on (first line it fits within). */
            line = n - 1;
            for (k = 0; k < n; ++k) {
                if (caret_off >= starts[k] &&
                    caret_off <= starts[k] + lengths[k]) {
                    line = k;
                    break;
                }
            }
            if (line >= first && (line - first) < rows) {
                int caret_x;

                caret_x = area_x +
                          backend->text_width(backend,
                                              state->input + starts[line],
                                              caret_off - starts[line]) +
                          1;
                /* Same baseline the line's text was drawn from, just above. */
                tg_gui_draw_caret(backend, TG_GUI_PEN_TEXT, caret_x,
                                  box_top + ((line - first) * lh) + lh + 2);
            }
        }
    } else if (state->composing) {
        if (state->cursor_on) {
            /* Empty composer: the baseline the first typed line will use. */
            tg_gui_draw_caret(backend, TG_GUI_PEN_TEXT, area_x,
                              box_top + lh + 2);
        }
    } else {
        backend->draw_text(backend, TG_GUI_PEN_TEXT_DIM, area_x,
                           box_top + lh + 2, "Write a message...", 18UL);
    }
    backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                       tg_gui_make_rect(width - 64, box_top, 56, input_h - 4));
    backend->draw_text(backend, TG_GUI_PEN_ACCENT_TEXT, width - 56,
                       box_top + lh + 2, "Send", 4UL);

    /* '@' mention popup: the candidate usernames above the composer (over the
       reply strip when one is shown); the highlighted row is what ENTER/TAB
       will insert. Same accent-frame + surface look as the context menu. */
    if (state->composing && state->mention_active && state->mention_count > 0) {
        int ih = lh + 4;
        int n = state->mention_count;
        int bw = 220;
        int bh = (n * ih) + 4;
        int bx = sidebar_w + 8;
        int by = box_top - bh - 2;
        int mi;

        if (state->reply_to_id != 0UL) {
            by -= (lh + 4 + TG_GUI_REPLY_LIFT); /* sit above the reply strip */
        }
        if (bx + bw > width - 8) {
            bw = width - 8 - bx;
        }
        if (by < 0) {
            by = 0;
        }
        backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                           tg_gui_make_rect(bx - 1, by - 1, bw + 2, bh + 2));
        backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                           tg_gui_make_rect(bx, by, bw, bh));
        for (mi = 0; mi < n; ++mi) {
            int pen = TG_GUI_PEN_TEXT;
            char row[TG_GUI_MENTION_LEN + 2];
            unsigned long rl = 0UL;
            const char *u = state->mention_items[mi];

            if (mi == state->mention_sel) {
                backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                                   tg_gui_make_rect(bx, by + 2 + (mi * ih),
                                                    bw, ih));
                pen = TG_GUI_PEN_ACCENT_TEXT;
            }
            row[rl++] = '@';
            while (*u != '\0' && rl < sizeof(row) - 1UL) {
                row[rl++] = *u++;
            }
            row[rl] = '\0';
            backend->draw_text(backend, pen, bx + 8, by + 2 + (mi * ih) + lh,
                               row, rl);
        }
    }
}

/* The floating "scroll to newest" button: an accent square with a filled
   down-triangle (the bitmap fonts carry no arrow glyph and the backend has only
   fill_rect/draw_text). When `unread` > 0 a small badge with the count (clamped
   to "9+") sits at the top-right. */
static void tg_gui_paint_jump_button(tg_gui_backend *backend, int x, int y,
                                     int w, int h, int unread)
{
    int cx = x + (w / 2);
    int arm = w / 4;             /* half-width of the triangle's top edge */
    int ty = y + (h / 2) - (arm / 2);
    int r;

    backend->round_bg = TG_GUI_PEN_WINDOW;
    tg_gui_fill_pill(backend, TG_GUI_PEN_ACCENT, tg_gui_make_rect(x, y, w, h));
    for (r = 0; r <= arm; ++r) {
        int half = arm - r;      /* wide at top, narrowing to a point downward */
        backend->fill_rect(backend, TG_GUI_PEN_ACCENT_TEXT,
                           tg_gui_make_rect(cx - half, ty + r, (2 * half) + 1, 1));
    }
    if (unread > 0) {
        char num[4];
        int bw = backend->line_height(backend);
        int bx = x + w - bw + 2;
        int by = y - 2;

        if (unread > 9) {
            num[0] = '9';
            num[1] = '+';
            num[2] = '\0';
        } else {
            num[0] = (char)('0' + unread);
            num[1] = '\0';
        }
        tg_gui_fill_pill(backend, TG_GUI_PEN_BADGE,
                         tg_gui_make_rect(bx, by, bw, bw));
        backend->draw_text(backend, TG_GUI_PEN_BADGE_TEXT, bx + 2,
                           tg_gui_centred_baseline(backend, by, bw), num,
                           (unsigned long)strlen(num));
    }
}

static void tg_gui_paint_main(const tg_gui_state *state,
                              tg_gui_backend *backend, int sidebar_w,
                              int width, int content_h, int lh)
{
    int area_x;
    int area_w;
    int header_h;
    int input_h;
    int y;
    int i;
    int transcript_bottom;
    int transcript_top;
    tg_gui_state *st;

    /* Clear this panel's own background (the sidebar already does the same), so
       tg_gui_paint no longer needs a leading full-window clear that flashed the
       entire window on every repaint -- very visible on OS3 planar displays. The
       sidebar + this main panel + the status bar tile the whole window. */
    backend->fill_rect(backend, TG_GUI_PEN_WINDOW,
                       tg_gui_make_rect(sidebar_w, 0, width - sidebar_w,
                                        content_h));

    area_x = sidebar_w + 12;
    area_w = width - sidebar_w - 24 - TG_GUI_SCROLLBAR_W;
    if (area_w < 40) {
        area_w = 40;
    }
    ((tg_gui_state *)state)->tr_area_x = area_x;
    ((tg_gui_state *)state)->tr_area_w = area_w;
    /* ANY transcript mutation (generation bump) invalidates a char-range
       selection AND a latched-but-unreleased press: at a full ring the count
       stays constant while every index shifts, so a count snapshot lies. */
    if (state->sel_active &&
        (state->msg_gen != state->sel_gen_snap || state->sel_msg < 0 ||
         state->sel_msg >= state->message_count)) {
        ((tg_gui_state *)state)->sel_active = 0;
    }
    if (state->sel_press_armed && state->msg_gen != state->sel_press_gen) {
        ((tg_gui_state *)state)->sel_press_armed = 0;
        ((tg_gui_state *)state)->sel_press_char = -1;
    }

    header_h = lh + 10;
    /* The open chat's avatar sits before the title, same drawing as its
       sidebar row (real image first, initials square as the fallback), so
       the header answers "which chat am I in" the way the desktop client
       does (field request). Text shifts right only when a chat is open, so
       the login/cached states keep their plain layout. */
    {
        int text_x = area_x;

        if (state->selected_chat >= 0 &&
            state->selected_chat < state->chat_count) {
            const tg_gui_chat *open_chat = &state->chats[state->selected_chat];
            int av = (2 * lh) - 2;

            backend->round_bg = TG_GUI_PEN_WINDOW;
            if (backend->avatar_image == 0 ||
                !backend->avatar_image(backend, open_chat->peer_id_hi,
                                       open_chat->peer_id_lo,
                                       tg_gui_make_rect(area_x, 4, av, av))) {
                backend->avatar_fill(backend, open_chat->avatar_color,
                                     tg_gui_make_rect(area_x, 4, av, av));
                {
                    unsigned long ilen =
                        (unsigned long)strlen(open_chat->initials);
                    int iw = backend->text_width(backend, open_chat->initials,
                                                 ilen);
                    int ix = area_x + ((av - iw) / 2);

                    if (ix < area_x) {
                        ix = area_x;
                    }
                    backend->draw_text(backend, TG_GUI_PEN_TEXT, ix,
                                       tg_gui_centred_baseline(backend, 4, av),
                                       open_chat->initials, ilen);
                }
            }
            text_x = area_x + av + 8;
        }
        tg_gui_draw_clipped(backend, TG_GUI_PEN_TEXT, text_x, lh + 2,
                            state->title, area_w - (text_x - area_x));
        /* While the peer is typing, the second header line shows "X is
           typing..." in the accent colour instead of the static subtitle
           (Telegram's cue). */
        if (state->typing[0] != '\0') {
            tg_gui_draw_clipped(backend, TG_GUI_PEN_ACCENT, text_x,
                                header_h + lh - 2, state->typing,
                                area_w - (text_x - area_x));
        } else {
            tg_gui_draw_clipped(backend, TG_GUI_PEN_TEXT_DIM, text_x,
                                header_h + lh - 2, state->subtitle,
                                area_w - (text_x - area_x));
        }
    }

    input_h = tg_gui_input_h(state, backend, width, sidebar_w, lh);
    ((tg_gui_state *)state)->input_h = input_h; /* cache for the hit-test */
    transcript_bottom = content_h - input_h - 4;

    /* One full line below the subtitle baseline so the first incoming bubble's
       sender name clears the header at any font size. */
    y = header_h + (2 * lh) + 4;
    transcript_top = y;
    /* Pixel-granular transcript scroll: transcript_scroll is a PIXEL offset up
       from the newest-pinned position (0 = newest pinned to the bottom). This
       lets a single message taller than the viewport be scrolled through fully --
       the old message-granular scroll left such a bubble's tail clipped and
       unreachable. */
    {
        int avail;
        int total;
        int max_scroll;
        int j;

        avail = transcript_bottom - transcript_top;
        total = 0;
        for (j = 0; j < state->message_count; ++j) {
            total += tg_gui_message_height(backend, &state->messages[j], area_w,
                                           lh, state->inline_photos,
                                           tg_gui_message_grouped(state, j));
        }
        {
            int real_max = (total > avail) ? (total - avail) : 0;
            /* Forced "pull older" range: when the loaded rows fit (no real scroll)
               but the chat has older history, add a small phantom range so a
               scrollbar is drawn and a drag-up / wheel-up can trigger load_older. */
            int forced = (real_max == 0 && state->more_above &&
                          state->message_count > 0) ? lh : 0;
            int content_scroll;

            max_scroll = real_max + forced;
            /* The painter owns the geometry: clamp the offset the event loop
               advanced freely (cast away const to write the model's own field). */
            st = (tg_gui_state *)state;
            if (st->transcript_scroll > max_scroll) {
                st->transcript_scroll = max_scroll;
            }
            if (st->transcript_scroll < 0) {
                st->transcript_scroll = 0;
            }
            /* Content scroll uses only the REAL range, so a phantom bar keeps the
               newest pinned to the bottom -- the phantom is a load-older handle,
               not an actual scroll into blank space. */
            content_scroll = (st->transcript_scroll < real_max) ?
                             st->transcript_scroll : real_max;
            /* Oldest message's top y; scroll == 0 pins the newest to the bottom. */
            y = transcript_bottom - total + content_scroll;
            if (max_scroll > 0) {
                int ky;
                int kh;

                /* Knob sized against avail+max_scroll: a real overflow gives
                   `total` (unchanged); a phantom range leaves a small gap at the
                   top so there is a knob to grab and drag up. */
                tg_gui_paint_scrollbar(backend, width - TG_GUI_SCROLLBAR_W,
                                       transcript_top, avail, avail + max_scroll,
                                       avail, max_scroll - st->transcript_scroll,
                                       &ky, &kh);
                st->sb_tr_x = width - TG_GUI_SCROLLBAR_W;
                st->sb_tr_ty = transcript_top;
                st->sb_tr_th = avail;
                st->sb_tr_ky = ky;
                st->sb_tr_kh = kh;
                st->sb_tr_max = max_scroll;
            } else {
                st->sb_tr_max = 0;
            }
            /* Scroll-to-bottom button: draw only when NOT at the true newest AND
               a REAL scroll exists. at_true_bottom = (transcript_scroll==0 &&
               !newest_dropped). The phantom pull-older range (real_max==0,
               forced=lh) does NOT count -- there the newest is already pinned to
               the visual bottom, so a jump button would be noise. When the newest
               was evicted by paging, real_max>0 (the stale ring overflows), so the
               button correctly appears even at transcript_scroll==0. */
            {
                int at_true_bottom = (st->transcript_scroll == 0 &&
                                      !st->newest_dropped);
                if (!at_true_bottom && real_max > 0) {
                    int jbw = lh + 8;
                    int jbh = lh + 8;
                    int gap = 4;
                    int jbx = width - TG_GUI_SCROLLBAR_W - jbw - gap;
                    int jby = transcript_bottom - jbh - gap;

                    if (jbx < area_x) {
                        jbx = area_x;
                    }
                    tg_gui_paint_jump_button(backend, jbx, jby, jbw, jbh,
                                             st->unread_below);
                    st->jb_x = jbx;
                    st->jb_y = jby;
                    st->jb_w = jbw;
                    st->jb_h = jbh;
                } else {
                    st->jb_w = 0;
                }
            }
        }
    }
    ((tg_gui_state *)state)->msg_cached = state->message_count;
    for (i = 0; i < TG_GUI_MAX_MESSAGES; ++i) {
        ((tg_gui_state *)state)->photo_w[i] = 0;
        ((tg_gui_state *)state)->photo_h[i] = 0;
    }
    for (i = 0; i < state->message_count; ++i) {
        const tg_gui_message *message;
        int h;

        message = &state->messages[i];
        /* Cache this row's top (renderer space, scroll already applied) for the
           click-to-reply hit-test; the bottom is the next row's top. */
        ((tg_gui_state *)state)->msg_top[i] = y;
        h = tg_gui_message_height(backend, message, area_w, lh,
                                  state->inline_photos,
                                  tg_gui_message_grouped(state, i));
        /* Draw only messages intersecting the viewport; each part is clipped to
           [transcript_top, transcript_bottom] inside the bubble. */
        if (y + h > transcript_top && y < transcript_bottom) {
            if (i == state->selected_msg && !message->is_system) {
                /* Clicked-row highlight, same language as the selected chat
                   row in the sidebar: the darker SELECT tint across the row
                   plus the left accent bar; the bubble then paints on top. */
                int by = (y > transcript_top) ? y : transcript_top;
                int bb = (y + h < transcript_bottom) ? (y + h)
                                                     : transcript_bottom;

                if (bb > by) {
                    backend->fill_rect(backend, TG_GUI_PEN_SELECT,
                                       tg_gui_make_rect(area_x - 6, by,
                                                        area_w + 6, bb - by));
                    backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                                       tg_gui_make_rect(area_x - 6, by, 3,
                                                        bb - by));
                }
            }
            if (message->is_system) {
                if (y + lh > transcript_top && y + lh <= transcript_bottom) {
                    backend->draw_text(backend, TG_GUI_PEN_TEXT_DIM,
                                       area_x + (area_w / 4), y + lh,
                                       message->text,
                                       (unsigned long)strlen(message->text));
                }
            } else {
                long sel_lo = -1;
                long sel_hi = -1;
                tg_gui_rect photo_rect;

                if (state->sel_active && i == state->sel_msg) {
                    sel_lo = state->sel_a < state->sel_b ? state->sel_a
                                                         : state->sel_b;
                    sel_hi = (state->sel_a > state->sel_b ? state->sel_a
                                                          : state->sel_b) +
                             1L; /* the char under the pointer is included */
                    {
                        long tl = (long)strlen(message->text);

                        if (sel_hi > tl) {
                            sel_hi = tl;
                        }
                    }
                }
                (void)tg_gui_paint_bubble(
                    backend, message, area_x, area_w, y, lh, transcript_top,
                    transcript_bottom, sel_lo, sel_hi, state->inline_photos,
                    tg_gui_message_grouped(state, i), &photo_rect);
                if (photo_rect.w > 0 && photo_rect.h > 0) {
                    st->photo_x[i] = photo_rect.x;
                    st->photo_y[i] = photo_rect.y;
                    st->photo_w[i] = photo_rect.w;
                    st->photo_h[i] = photo_rect.h;
                }
            }
        }
        y += h;
    }

    tg_gui_paint_input_row(state, backend);
}

/* Cursor Y (inner-relative) -> drag-drop insert-before target in [0, chat_count].
   Mirrors tg_gui_paint_sidebar's search_h/row_h/chat_scroll, rounding to the
   nearest inter-row gap so the drop lands where the insertion line is drawn. */
int tg_gui_chat_drop_target(const tg_gui_state *state, int lh, int y)
{
    int search_h;
    int row_h;
    int rel;
    int slot;
    int target;

    if (state == 0) {
        return 0;
    }
    search_h = lh + 10;       /* keep in sync with tg_gui_paint_sidebar */
    row_h = (2 * lh) + 12;
    if (row_h < 1) {
        row_h = 1;
    }
    rel = y - search_h;
    slot = (rel + (row_h / 2)) / row_h;
    if (slot < 0) {
        slot = 0;
    }
    target = state->chat_scroll + slot;
    if (target < 0) {
        target = 0;
    }
    if (target > state->chat_count) {
        target = state->chat_count;
    }
    return target;
}

/* Sidebar width for a given window width -- shared by the painter and the
   hit-test so a click maps to exactly what was drawn. */
int tg_gui_sidebar_w(int width)
{
    int sidebar_w;

    if (width < 280) {
        sidebar_w = width / 3;
    } else {
        sidebar_w = (width * 36) / 100;
        if (sidebar_w < 120) {
            sidebar_w = 120;
        }
        if (sidebar_w > width - 160) {
            sidebar_w = width - 160;
        }
    }
    if (sidebar_w < 1) {
        sidebar_w = 1;
    }
    return sidebar_w;
}


long tg_gui_transcript_char_at(const tg_gui_state *state,
                               tg_gui_backend *backend, int lh, int msg_index,
                               int x, int y)
{
    tg_gui_bubble_geom geo;
    const tg_gui_message *message;
    int ty;
    int k;
    long ls;
    long ll;
    int lx;
    long c;

    if (state == 0 || backend == 0 || msg_index < 0 ||
        msg_index >= state->message_count ||
        state->msg_cached != state->message_count) {
        return -1;
    }
    message = &state->messages[msg_index];
    if (message->is_system || message->text[0] == '\0' ||
        (message->photo_only && message->has_photo)) {
        return -1;
    }
    tg_gui_bubble_geometry(backend, message, state->tr_area_x,
                           state->tr_area_w, lh, state->inline_photos,
                           tg_gui_message_grouped(state, msg_index), &geo);
    ty = y - (state->msg_top[msg_index] + geo.header_h + geo.reply_h +
              geo.photo_h + geo.photo_gap);
    if (ty < 0) {
        return 0; /* header band or above: clamp to the start */
    }
    k = ty / lh;
    if (k >= geo.line_count) {
        return (long)strlen(message->text); /* below the text: clamp to end */
    }
    ls = (long)geo.starts[k];
    ll = (long)geo.lengths[k];
    lx = x - (geo.bubble_x + geo.pad);
    if (lx <= 0) {
        return ls;
    }
    for (c = 1; c <= ll; ++c) {
        if (tg_gui_marked_width(backend, message->text, (unsigned long)ls,
                                (unsigned long)(ls + c)) > lx) {
            return ls + c - 1;
        }
    }
    return ls + ll;
}

/* If the character at offset `ch` of message `m` sits inside a web link,
   copy the link into out (prefixing bare www. with http://) and return 1.
   A "link" is the whitespace-delimited word around ch, stripped of the
   style markers and wrapping punctuation, starting with http(s):// or www.
   Pure text scan: entities already put the URL text in the message body. */
int tg_gui_url_at(const tg_gui_message *m, long ch, char *out,
                  unsigned long out_size)
{
    long len;
    long a;
    long b;
    unsigned long n;
    unsigned long need;
    const char *w;
    long wl;
    int www;

    if (m == 0 || out == 0 || out_size < 12UL || m->is_system ||
        m->text[0] == '\0' || ch < 0) {
        return 0;
    }
    len = (long)strlen(m->text);
    if (ch >= len) {
        ch = len - 1;
    }
    if (m->text[ch] == ' ' || m->text[ch] == '\n' || m->text[ch] == '\t') {
        return 0; /* clicked the gap between words */
    }
    a = ch;
    while (a > 0 && m->text[a - 1] != ' ' && m->text[a - 1] != '\n' &&
           m->text[a - 1] != '\t') {
        --a;
    }
    b = ch;
    while (b + 1 < len && m->text[b + 1] != ' ' && m->text[b + 1] != '\n' &&
           m->text[b + 1] != '\t') {
        ++b;
    }
    /* Strip style markers and wrapping punctuation from both ends. */
    while (a <= b && (m->text[a] == '*' || m->text[a] == '_' ||
                      m->text[a] == '`' || m->text[a] == '~' ||
                      m->text[a] == '(' || m->text[a] == '<' ||
                      m->text[a] == '[' || m->text[a] == '"' ||
                      m->text[a] == '\'')) {
        ++a;
    }
    while (b >= a && (m->text[b] == '*' || m->text[b] == '_' ||
                      m->text[b] == '`' || m->text[b] == '~' ||
                      m->text[b] == ')' || m->text[b] == '>' ||
                      m->text[b] == ']' || m->text[b] == '"' ||
                      m->text[b] == '\'' || m->text[b] == '.' ||
                      m->text[b] == ',' || m->text[b] == ';' ||
                      m->text[b] == ':' || m->text[b] == '!' ||
                      m->text[b] == '?')) {
        --b;
    }
    if (b < a) {
        return 0;
    }
    w = m->text + a;
    wl = b - a + 1;
    www = 0;
    /* The minimum lengths MUST match tg_gui_link_span's, or the renderer
       underlines addresses this refuses to open: scheme plus at least one
       character of host (http://x, https://x, www.a). */
    if ((wl > 7 && strncmp(w, "http://", 7) == 0) ||
        (wl > 8 && strncmp(w, "https://", 8) == 0)) {
        ;
    } else if (wl > 4 && strncmp(w, "www.", 4) == 0) {
        www = 1; /* bare www.: prefix a scheme for the opener */
    } else {
        return 0;
    }
    need = (unsigned long)wl + (www ? 7UL : 0UL);
    if (need + 1UL > out_size) {
        return 0; /* longer than the caller can take: not clickable */
    }
    n = 0UL;
    if (www) {
        strcpy(out, "http://");
        n = 7UL;
    }
    memcpy(out + n, w, (unsigned long)wl);
    out[n + (unsigned long)wl] = '\0';
    return 1;
}

int tg_gui_selection_get(const tg_gui_state *state, char *out,
                         unsigned long out_size)
{
    long lo;
    long hi;
    long tl;
    const char *text;

    if (state == 0 || out == 0 || out_size == 0UL || !state->sel_active ||
        state->sel_msg < 0 || state->sel_msg >= state->message_count) {
        return 0;
    }
    text = state->messages[state->sel_msg].text;
    tl = (long)strlen(text);
    lo = state->sel_a < state->sel_b ? state->sel_a : state->sel_b;
    hi = (state->sel_a > state->sel_b ? state->sel_a : state->sel_b) + 1L;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > tl) {
        hi = tl;
    }
    if (hi <= lo) {
        return 0;
    }
    if ((unsigned long)(hi - lo) >= out_size) {
        hi = lo + (long)out_size - 1L;
    }
    memcpy(out, text + lo, (unsigned long)(hi - lo));
    out[hi - lo] = '\0';
    return 1;
}

int tg_gui_hit_test(const tg_gui_state *state, int width, int height, int lh,
                    int x, int y)
{
    int sidebar_w;
    int status_h;
    int content_h;
    int input_h;

    if (state == 0 || lh <= 0 || width <= 0 || height <= 0) {
        return TG_GUI_HIT_NONE;
    }
    status_h = lh + 6;
    content_h = height - status_h;
    sidebar_w = tg_gui_sidebar_w(width);
    input_h = (state->input_h > 0) ? state->input_h : (lh + 14);
    /* The input row / Send button live along the bottom of the right pane. */
    if (y >= content_h - input_h && y < content_h - 4 && x >= sidebar_w) {
        /* When replying, the top line of the region is the "<sender>: <snippet>"
           header; its far-right "X" cancels the reply, the rest just focuses. */
        if (state->reply_to_id != 0UL &&
            y < content_h - input_h + lh + 4 + TG_GUI_REPLY_LIFT) {
            if (x >= width - 26) {
                return TG_GUI_HIT_REPLY_CANCEL;
            }
            return TG_GUI_HIT_INPUT;
        }
        if (x >= width - 64) {
            return TG_GUI_HIT_SEND;
        }
        return TG_GUI_HIT_INPUT;
    }
    /* Sidebar chat rows (search box on top, then fixed-height rows). */
    if (x >= 0 && x < sidebar_w && y >= 0 && y < content_h) {
        int search_h;
        int row_h;

        search_h = lh + 10;
        row_h = (2 * lh) + 12;
        if (y >= search_h) {
            int row;

            row = (y - search_h) / row_h + state->chat_scroll;
            if (row >= 0 && row < state->chat_count) {
                return row;
            }
        } else {
            return TG_GUI_HIT_SEARCH; /* the search box strip at the top */
        }
    }
    /* A click on a transcript bubble picks it as the reply target. Use the row
       tops cached by the last paint (msg_top[]/msg_cached); scan newest-first so
       the topmost row wins when the cached bounds touch. System lines and the
       not-yet-acked optimistic echo (id == 0) cannot be replied to. */
    if (x >= sidebar_w && y >= 0 && y < content_h - input_h
        && state->msg_cached > 0) {
        int last = state->msg_cached;
        int mi;

        if (last > state->message_count) {
            last = state->message_count;
        }
        for (mi = last - 1; mi >= 0; --mi) {
            if (state->messages[mi].has_photo && state->photo_w[mi] > 0 &&
                state->photo_h[mi] > 0 && x >= state->photo_x[mi] &&
                x < state->photo_x[mi] + state->photo_w[mi] &&
                y >= state->photo_y[mi] &&
                y < state->photo_y[mi] + state->photo_h[mi]) {
                return TG_GUI_HIT_PHOTO_BASE - mi;
            }
        }
        for (mi = last - 1; mi >= 0; --mi) {
            int top = state->msg_top[mi];
            int bot = (mi + 1 < last) ? state->msg_top[mi + 1]
                                      : (content_h - input_h);

            if (y >= top && y < bot) {
                const tg_gui_message *m = &state->messages[mi];

                if (m->is_system || m->id == 0UL) {
                    return TG_GUI_HIT_NONE;
                }
                return TG_GUI_HIT_MESSAGE_BASE - mi;
            }
        }
    }
    return TG_GUI_HIT_NONE;
}

/* Draws one centered line, clamped into the window so it never overflows. */
static void tg_gui_draw_centered(tg_gui_backend *backend, int pen, int width,
                                 int baseline, const char *text)
{
    int tw;
    int x;

    if (text == 0 || text[0] == '\0' || baseline < 0) {
        return;
    }
    tw = backend->text_width(backend, text, (unsigned long)strlen(text));
    x = (width - tw) / 2;
    if (x < 2) {
        x = 2;
    }
    tg_gui_draw_clipped(backend, pen, x, baseline, text, width - x - 2);
}

/* Draws just the login input box: the SURFACE field, the typed text (masked for
   the 2FA password) centred in the box, and the blinking caret. Factored out of
   tg_gui_paint_login so the caret blink (tg_gui_paint_caret) repaints ONLY this
   box, not the whole login screen. Geometry is recomputed to match
   tg_gui_paint_login. */
static void tg_gui_paint_login_input(const tg_gui_state *state,
                                     tg_gui_backend *backend)
{
    int width;
    int height;
    int lh;
    int cx;
    int mid;
    int box_w;
    int box_x;
    int box_y;
    int box_h;
    const char *field;
    char masked[TG_GUI_TEXT_MAX];

    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return;
    }
    cx = width / 2;
    mid = height / 2;

    box_w = (width - 40 < 280) ? (width - 40) : 280;
    if (box_w < 40) {
        box_w = (width > 8) ? (width - 8) : width;
    }
    box_x = cx - (box_w / 2);
    box_y = mid + 4;
    box_h = lh + 8;
    backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                       tg_gui_make_rect(box_x, box_y, box_w, box_h));

    if (state->input_masked) {
        unsigned long n;
        unsigned long i;

        n = (unsigned long)strlen(state->input);
        if (n >= sizeof(masked)) {
            n = sizeof(masked) - 1UL;
        }
        for (i = 0UL; i < n; ++i) {
            masked[i] = '*';
        }
        masked[n] = '\0';
        field = masked;
    } else {
        field = state->input;
    }
    {
        int tw;
        int text_x;

        /* Centre the typed text in the box so it lines up under the centred
           title/status above and the hint below. Long input falls back to
           left-aligned + clipped to the box. */
        tw = backend->text_width(backend, field, (unsigned long)strlen(field));
        text_x = box_x + (box_w - tw) / 2;
        if (text_x < box_x + 6) {
            text_x = box_x + 6;
        }
        tg_gui_draw_clipped(backend, TG_GUI_PEN_TEXT, text_x, box_y + lh + 1,
                            field, box_x + box_w - 6 - text_x);
        if (state->cursor_on) {
            int caret_x;

            caret_x = text_x + tw;
            if (caret_x < box_x + box_w - 2) {
                backend->fill_rect(backend, TG_GUI_PEN_ACCENT,
                                   tg_gui_make_rect(caret_x, box_y + 3, 2,
                                                    box_h - 6));
            }
        }
    }
}

/* The first-login screen: a centered panel with the title, the current prompt
   (state->status), the input field (masked for the 2FA password) with caret,
   and a key hint. Shown while state->mode is a TG_GUI_MODE_LOGIN_* value. */
/* One-shot trail for the very first paint (--gui-live-debug): a field crash
   inside the initial render leaves the LAST primitive named in the log (an
   AmiKit 12/13 setup dies exactly there, both on PiStorm and under WinUAE,
   while the TUI works). Later repaints skip the trail. */
static int tg_gui_first_paint_logged;

/* The DIRECT render path runs the whole painter under LockLayerRom, where the
   trail's DOS I/O must never happen (a filesystem requester under a layer lock
   deadlocks Intuition): the window backend calls this before locking, so the
   trail stays exclusive to the off-screen (unlocked) render. */
void tg_gui_paint_trail_off(void)
{
    tg_gui_first_paint_logged = 1;
}

static void tg_gui_paint_login(const tg_gui_state *state,
                               tg_gui_backend *backend)
{
    int width;
    int height;
    int lh;
    int mid;

    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return;
    }
    if (!tg_gui_first_paint_logged) {
        tg_gui_log("login paint: clear");
    }
    if (tg_gui_clear_background) {
        backend->fill_rect(backend, TG_GUI_PEN_WINDOW,
                           tg_gui_make_rect(0, 0, width, height));
    }
    mid = height / 2;

    if (!tg_gui_first_paint_logged) {
        tg_gui_log("login paint: title+status");
    }
    tg_gui_draw_centered(backend, TG_GUI_PEN_ACCENT, width, mid - (3 * lh),
                         "Telegram Amiga");
    tg_gui_draw_centered(backend, TG_GUI_PEN_TEXT, width, mid - lh,
                         state->status);

    if (!tg_gui_first_paint_logged) {
        tg_gui_log("login paint: input box");
    }
    tg_gui_paint_login_input(state, backend);

    if (!tg_gui_first_paint_logged) {
        tg_gui_log("login paint: footer");
    }
    tg_gui_draw_centered(backend, TG_GUI_PEN_TEXT_DIM, width, mid + (3 * lh),
                         "ENTER confirms   ESC quits");
    if (!tg_gui_first_paint_logged) {
        tg_gui_log("login paint: done");
        tg_gui_first_paint_logged = 1;
    }
}

static void tg_gui_paint_context_menu(const tg_gui_state *state,
                                      tg_gui_backend *backend);

/* Repaints ONLY the active caret region -- the composer input row in chat mode,
   or the login input box otherwise -- so the ~2 Hz caret blink no longer
   repaints the whole window (a visible, constant refresh on slow OS3 displays).
   Geometry is recomputed by the panel helpers to match a full tg_gui_paint. */
void tg_gui_paint_caret(const tg_gui_state *state, tg_gui_backend *backend)
{
    if (state == 0 || backend == 0) {
        return;
    }
    if (state->mode != TG_GUI_MODE_CHAT) {
        tg_gui_paint_login_input(state, backend);
    } else if (state->search_active) {
        tg_gui_paint_search_box(state, backend);
    } else {
        tg_gui_paint_input_row(state, backend);
        /* An open right-click menu can overlap the input row (opening it on
           the LAST message pops it up right above the composer), and this
           partial repaint would erase that slice of it a blink later -- the
           full repaint draws the popup last for exactly that reason. Repaint
           it on top; no-op while the menu is closed. (Sam460/OS4 field
           report, issue #5 follow-up.) */
        tg_gui_paint_context_menu(state, backend);
    }
}

/* --- Right-click context menu (popup at the pointer) ------------------- */

/* Items shown for the open menu, by target message: Reply always; Edit + Delete
   only on an OWN message with a server id. Fills labels[]/ids[] (each sized
   TG_GUI_CTX_ITEMS_MAX) and returns the count. */
int tg_gui_open_chat_is_self(const tg_gui_state *state)
{
    return state != 0 && state->selected_chat >= 0 &&
           state->selected_chat < state->chat_count &&
           state->chats[state->selected_chat].index ==
               TG_GUI_SAVED_PEER_INDEX;
}

static int tg_gui_context_items(const tg_gui_state *state, const char **labels,
                                int *ids)
{
    int n = 0;

    labels[n] = "Reply";
    ids[n] = TG_GUI_CTX_REPLY;
    ++n;
    if (state->ctx_msg >= 0 && state->ctx_msg < state->message_count &&
        state->messages[state->ctx_msg].id != 0UL) {
        const tg_gui_message *m = &state->messages[state->ctx_msg];

        /* Saved Messages: the server clears the out flag there (nothing in
           the self chat is "outgoing"), yet every message is the user's own
           -- so the self chat offers Edit/Delete on all of them. */
        if (m->is_own || tg_gui_open_chat_is_self(state)) {
            labels[n] = "Edit";
            ids[n] = TG_GUI_CTX_EDIT;
            ++n;
            labels[n] = "Delete";
            ids[n] = TG_GUI_CTX_DELETE;
            ++n;
        }
        if (m->has_document) { /* incoming OR own: you can save either */
            labels[n] = "Download";
            ids[n] = TG_GUI_CTX_DOWNLOAD;
            ++n;
        }
        if (m->has_photo) {
            labels[n] = "Save photo as...";
            ids[n] = TG_GUI_CTX_SAVE_PHOTO;
            ++n;
        }
        if (m->text[0] != '\0') {
            labels[n] = "Copy text";
            ids[n] = TG_GUI_CTX_COPY;
            ++n;
        }
        if (!m->is_system) {
            labels[n] = "Forward to Saved";
            ids[n] = TG_GUI_CTX_FORWARD_SAVED;
            ++n;
            labels[n] = "Forward to...";
            ids[n] = TG_GUI_CTX_FORWARD_TO;
            ++n;
        }
    }
    /* Chat-level: send a file to the open chat. Always offered (the popup only
       appears over a conversation) so it need not be reached via the menubar.
       "Download drawer..." is deliberately NOT here (issue #11): it is a
       preference, not a message action, so it lives in the menus only. */
    labels[n] = "Send file...";
    ids[n] = TG_GUI_CTX_SENDFILE;
    ++n;
    labels[n] = "Send photo...";
    ids[n] = TG_GUI_CTX_SENDPHOTO;
    ++n;
    return n;
}

int tg_gui_photo_default_filename(char *out, unsigned long out_size,
                                  unsigned long photo_id_hi,
                                  unsigned long photo_id_lo)
{
    unsigned long short_id;
    char name[40];
    unsigned long length;

    if (out == 0 || out_size == 0UL ||
        (photo_id_hi == 0UL && photo_id_lo == 0UL)) {
        return 1;
    }
    short_id = photo_id_lo != 0UL ? photo_id_lo : photo_id_hi;
    sprintf(name, "photo-%08lx.jpg", short_id);
    length = (unsigned long)strlen(name);
    if (length + 1UL > out_size) {
        out[0] = '\0';
        return 1;
    }
    memcpy(out, name, length + 1UL);
    return 0;
}

int tg_gui_photo_build_destination(char *out, unsigned long out_size,
                                   const char *drawer, const char *name)
{
    unsigned long drawer_len;
    unsigned long name_len;
    int slash;

    if (out == 0 || out_size == 0UL || drawer == 0 || name == 0 ||
        drawer[0] == '\0' || name[0] == '\0') {
        return 1;
    }
    drawer_len = (unsigned long)strlen(drawer);
    name_len = (unsigned long)strlen(name);
    slash = drawer[drawer_len - 1UL] != ':' &&
            drawer[drawer_len - 1UL] != '/';
    if (drawer_len + (slash ? 1UL : 0UL) + name_len + 1UL > out_size) {
        out[0] = '\0';
        return 1;
    }
    memcpy(out, drawer, drawer_len);
    if (slash) {
        out[drawer_len++] = '/';
    }
    memcpy(out + drawer_len, name, name_len + 1UL);
    return 0;
}

int tg_gui_photo_save_allowed(int destination_exists,
                              int overwrite_confirmed)
{
    return !destination_exists || overwrite_confirmed;
}

/* Measures the popup width from the labels the CURRENT state would show: the
   widest label plus the 8px text inset on each side, never below the
   TG_GUI_CTX_W minimum. The result is stored in state->ctx_w by the opener, so
   the paint and the backend-free hit-test share one width (issue #11: the
   fixed width truncated "Send file..." / "Download drawer..." on wider
   fonts). */
int tg_gui_context_menu_measure(const tg_gui_state *state,
                                tg_gui_backend *backend)
{
    const char *labels[TG_GUI_CTX_ITEMS_MAX];
    int ids[TG_GUI_CTX_ITEMS_MAX];
    int n;
    int i;
    int w = TG_GUI_CTX_W;

    if (state == 0 || backend == 0 || backend->text_width == 0) {
        return w;
    }
    n = tg_gui_context_items(state, labels, ids);
    for (i = 0; i < n; ++i) {
        int tw = backend->text_width(backend, labels[i],
                                     (unsigned long)strlen(labels[i])) + 16;

        if (tw > w) {
            w = tw;
        }
    }
    return w;
}

/* Shared geometry for the popup: box rect + per-item height for `count` items,
   clamped so the box stays fully inside the window. Backend-free: the width
   was measured (and stored) when the menu opened; 0 = the fixed minimum. */
static void tg_gui_context_box(const tg_gui_state *state, int count, int width,
                               int height, int lh, int *bx, int *by, int *bw,
                               int *bh, int *item_h)
{
    int ih = lh + 4;
    int w = (state->ctx_w > 0) ? state->ctx_w : TG_GUI_CTX_W;
    int h = (count * ih) + 4;
    int x = state->ctx_x;
    int y = state->ctx_y;

    if (x + w > width) {
        x = width - w;
    }
    if (x < 0) {
        x = 0;
    }
    if (y + h > height) {
        y = height - h;
    }
    if (y < 0) {
        y = 0;
    }
    *bx = x;
    *by = y;
    *bw = w;
    *bh = h;
    *item_h = ih;
}

/* Draws the context menu on top of everything when open: an accent frame, a
   surface fill, and each item's label. */
static void tg_gui_paint_context_menu(const tg_gui_state *state,
                                      tg_gui_backend *backend)
{
    int width;
    int height;
    int lh;
    int bx, by, bw, bh, ih, n, i;
    const char *labels[TG_GUI_CTX_ITEMS_MAX];
    int ids[TG_GUI_CTX_ITEMS_MAX];

    if (!state->ctx_visible) {
        return;
    }
    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return;
    }
    n = tg_gui_context_items(state, labels, ids);
    tg_gui_context_box(state, n, width, height, lh, &bx, &by, &bw, &bh, &ih);
    /* System menu colours (0.0.8): the popup now matches the new-look
       menubar instead of the dark chat theme -- outline, background, text
       and the hover highlight all come from the screen's own pens. */
    backend->fill_rect(backend, TG_GUI_PEN_MENU_FRAME,
                       tg_gui_make_rect(bx - 1, by - 1, bw + 2, bh + 2));
    backend->fill_rect(backend, TG_GUI_PEN_MENU_BACK,
                       tg_gui_make_rect(bx, by, bw, bh));
    for (i = 0; i < n; ++i) {
        int text_pen = TG_GUI_PEN_MENU_TEXT;

        if (i == state->ctx_hover) {
            /* Highlight the entry under the pointer so the user sees which of
               Reply/Edit/Delete the click will pick (system fill colours). */
            backend->fill_rect(backend, TG_GUI_PEN_MENU_FILL,
                               tg_gui_make_rect(bx, by + 2 + (i * ih), bw, ih));
            text_pen = TG_GUI_PEN_MENU_FILLTEXT;
        }
        backend->draw_text(backend, text_pen, bx + 8,
                           by + 2 + (i * ih) + lh, labels[i],
                           (unsigned long)strlen(labels[i]));
    }
}

int tg_gui_context_menu_hit(const tg_gui_state *state, int width, int height,
                            int lh, int x, int y)
{
    int bx, by, bw, bh, ih, n, i;
    const char *labels[TG_GUI_CTX_ITEMS_MAX];
    int ids[TG_GUI_CTX_ITEMS_MAX];

    if (state == 0 || !state->ctx_visible || lh <= 0 || width <= 0 ||
        height <= 0) {
        return -1;
    }
    n = tg_gui_context_items(state, labels, ids);
    tg_gui_context_box(state, n, width, height, lh, &bx, &by, &bw, &bh, &ih);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) {
        return -1; /* outside the box -> dismiss */
    }
    i = (y - (by + 2)) / ih;
    if (i < 0) {
        i = 0;
    }
    if (i >= n) {
        i = n - 1;
    }
    return ids[i]; /* item id: TG_GUI_CTX_REPLY / EDIT / DELETE */
}

int tg_gui_context_menu_index(const tg_gui_state *state, int width, int height,
                              int lh, int x, int y)
{
    int bx, by, bw, bh, ih, n, i;
    const char *labels[TG_GUI_CTX_ITEMS_MAX];
    int ids[TG_GUI_CTX_ITEMS_MAX];

    if (state == 0 || !state->ctx_visible || lh <= 0 || width <= 0 ||
        height <= 0) {
        return -1;
    }
    n = tg_gui_context_items(state, labels, ids);
    tg_gui_context_box(state, n, width, height, lh, &bx, &by, &bw, &bh, &ih);
    if (x < bx || x >= bx + bw || y < by || y >= by + bh) {
        return -1; /* outside the box -> no item highlighted */
    }
    i = (y - (by + 2)) / ih;
    if (i < 0) {
        i = 0;
    }
    if (i >= n) {
        i = n - 1;
    }
    return i; /* 0-based item index, for ctx_hover highlighting */
}

int tg_gui_mention_token(const char *input, int caret, int *start)
{
    int i;

    if (input == 0 || start == 0 || caret < 0) {
        return -1;
    }
    i = caret - 1;
    while (i >= 0) {
        char c = input[i];

        if (c == '@') {
            if (i == 0 || input[i - 1] == ' ' || input[i - 1] == '\n' ||
                input[i - 1] == '\t') {
                *start = i;
                return caret - i - 1; /* prefix length, 0 for a bare '@' */
            }
            return -1; /* '@' glued to a word (email-style): not a mention */
        }
        if (c == ' ' || c == '\n' || c == '\t') {
            return -1; /* whitespace between '@' and the caret: no token */
        }
        --i;
    }
    return -1;
}

void tg_gui_paint(const tg_gui_state *state, tg_gui_backend *backend)
{
    int width;
    int height;
    int lh;
    int sidebar_w;
    int status_h;
    int content_h;

    if (state == 0 || backend == 0) {
        return;
    }
    if (state->mode != TG_GUI_MODE_CHAT) {
        tg_gui_paint_login(state, backend);
        return;
    }
    width = backend->width(backend);
    height = backend->height(backend);
    lh = backend->line_height(backend);
    if (width <= 0 || height <= 0 || lh <= 0) {
        return;
    }

    status_h = lh + 6;
    content_h = height - status_h;
    /* Below ~280px there is no room for both a 120px list and a usable
       conversation, so just split a third; above it, 36% clamped to [120,
       width-160]. */
    sidebar_w = tg_gui_sidebar_w(width);

    /* No leading full-window clear: tg_gui_paint_sidebar, tg_gui_paint_main and
       the status bar below each fill their own region, so they tile the whole
       window. This avoids the full-window flash that was visible as a constant
       refresh on slow OS3 planar displays. */
    if (!tg_gui_first_paint_logged) {
        tg_gui_log("chat paint: sidebar");
    }
    tg_gui_paint_sidebar(state, backend, sidebar_w, content_h, lh);
    if (!tg_gui_first_paint_logged) {
        tg_gui_log("chat paint: main");
    }
    tg_gui_paint_main(state, backend, sidebar_w, width, content_h, lh);

    if (!tg_gui_first_paint_logged) {
        tg_gui_log("chat paint: status");
    }
    backend->fill_rect(backend, TG_GUI_PEN_SURFACE,
                       tg_gui_make_rect(0, content_h, width, status_h));
    backend->draw_text(backend, TG_GUI_PEN_TEXT_DIM, 10, content_h + lh,
                       state->status, (unsigned long)strlen(state->status));
    /* Drawn last so it overlays the transcript/status; part of the off-screen
       render, so the double-buffer blit carries it and a repaint with
       ctx_visible==0 cleanly removes it. */
    tg_gui_paint_context_menu(state, backend);
    if (!tg_gui_first_paint_logged) {
        tg_gui_log("chat paint: done");
        tg_gui_first_paint_logged = 1;
    }
}

/* --- Recording backend for the self-test ------------------------------- */

typedef struct tg_gui_record {
    int width;
    int height;
    int fills;
    int avatars;
    int photos;
    int photo_decodes;
    int photo_size_changed;
    unsigned long photo_cache_hi;
    unsigned long photo_cache_lo;
    int photo_last_w;
    int photo_last_h;
    int texts;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int read_marks; /* set when a fill in the READ pen (the read double-check) drew */
    const char *forbidden; /* a string that must NEVER be drawn (e.g. a password) */
    int forbidden_hits;    /* how many draw_text calls contained `forbidden` */
} tg_gui_record;

#if !defined(TG_NO_SELFTEST)
static int tg_gui_rec_width(tg_gui_backend *backend)
{
    return ((tg_gui_record *)backend->context)->width;
}

static int tg_gui_rec_height(tg_gui_backend *backend)
{
    return ((tg_gui_record *)backend->context)->height;
}

static int tg_gui_rec_line_height(tg_gui_backend *backend)
{
    (void)backend;
    return 10;
}

static int tg_gui_rec_text_width(tg_gui_backend *backend, const char *text,
                                 unsigned long length)
{
    (void)backend;
    (void)text;
    return (int)(length * 6UL);
}

static void tg_gui_rec_track(tg_gui_record *record, int x, int y)
{
    if (x < record->min_x) {
        record->min_x = x;
    }
    if (y < record->min_y) {
        record->min_y = y;
    }
    if (x > record->max_x) {
        record->max_x = x;
    }
    if (y > record->max_y) {
        record->max_y = y;
    }
}

static void tg_gui_rec_fill(tg_gui_backend *backend, int pen, tg_gui_rect rect)
{
    tg_gui_record *record;

    record = (tg_gui_record *)backend->context;
    record->fills += 1;
    /* The read double-check is the only thing drawn in the azure READ pen, so a
       fill in that pen proves the receipt mark rendered (its many tick segments
       just set the flag idempotently). */
    if (pen == TG_GUI_PEN_READ) {
        record->read_marks = 1;
    }
    tg_gui_rec_track(record, rect.x, rect.y);
    tg_gui_rec_track(record, rect.x + rect.w, rect.y + rect.h);
}

static void tg_gui_rec_avatar(tg_gui_backend *backend, int color_index,
                              tg_gui_rect rect)
{
    tg_gui_record *record;

    (void)color_index;
    record = (tg_gui_record *)backend->context;
    record->avatars += 1;
    tg_gui_rec_track(record, rect.x, rect.y);
    tg_gui_rec_track(record, rect.x + rect.w, rect.y + rect.h);
}

static int tg_gui_rec_photo(tg_gui_backend *backend,
                            unsigned long photo_id_hi,
                            unsigned long photo_id_lo,
                            unsigned long source_w,
                            unsigned long source_h,
                            tg_gui_rect rect,
                            tg_gui_rect clip)
{
    tg_gui_record *record;

    (void)photo_id_hi;
    (void)photo_id_lo;
    (void)source_w;
    (void)source_h;
    (void)clip;
    record = (tg_gui_record *)backend->context;
    record->photos += 1;
    if (record->photo_decodes == 0 ||
        record->photo_cache_hi != photo_id_hi ||
        record->photo_cache_lo != photo_id_lo) {
        record->photo_decodes += 1;
        record->photo_cache_hi = photo_id_hi;
        record->photo_cache_lo = photo_id_lo;
    } else if (record->photo_last_w != rect.w ||
               record->photo_last_h != rect.h) {
        record->photo_size_changed = 1;
    }
    record->photo_last_w = rect.w;
    record->photo_last_h = rect.h;
    tg_gui_rec_track(record, rect.x, rect.y);
    tg_gui_rec_track(record, rect.x + rect.w, rect.y + rect.h);
    return 1;
}

static void tg_gui_rec_text(tg_gui_backend *backend, int pen, int x,
                            int baseline, const char *text,
                            unsigned long length)
{
    tg_gui_record *record;

    (void)pen;
    record = (tg_gui_record *)backend->context;
    record->texts += 1;
    /* The read receipt is no longer text -- it is drawn as ticks in the READ pen
       and counted in tg_gui_rec_fill. */
    /* Guard that a forbidden string (a masked password) never reaches draw. */
    if (record->forbidden != 0 && record->forbidden[0] != '\0' && text != 0) {
        unsigned long flen;

        flen = (unsigned long)strlen(record->forbidden);
        if (length >= flen) {
            unsigned long i;

            for (i = 0UL; i + flen <= length; ++i) {
                unsigned long j;

                for (j = 0UL; j < flen && text[i + j] == record->forbidden[j];
                     ++j) {
                }
                if (j == flen) {
                    record->forbidden_hits += 1;
                    break;
                }
            }
        }
    }
    tg_gui_rec_track(record, x, baseline);
    tg_gui_rec_track(record, x + (int)(length * 6UL), baseline);
}

int tg_gui_self_test(void)
{
    tg_gui_state state;
    tg_gui_backend backend;
    tg_gui_record record;
    int ok;

    tg_gui_demo_state(&state);
    if (state.chat_count <= 0 || state.message_count <= 0) {
        puts("gui self-test: demo state empty");
        return 2;
    }

    memset(&record, 0, sizeof(record));
    record.width = 480;
    record.height = 320;
    record.min_x = record.width;
    record.min_y = record.height;
    record.max_x = 0;
    record.max_y = 0;
    record.forbidden = "[Photo]"; /* ready media replaces its fallback label */

    backend.context = &record;
    backend.width = tg_gui_rec_width;
    backend.height = tg_gui_rec_height;
    backend.line_height = tg_gui_rec_line_height;
    backend.font_ascent = 0; /* recorder: exercise the renderer's fallback */
    backend.text_width = tg_gui_rec_text_width;
    backend.fill_rect = tg_gui_rec_fill;
    backend.avatar_image = 0;
    backend.photo_image = tg_gui_rec_photo;
    backend.fill_pill = 0; /* recorder: exercise the renderer's row fallback */
    backend.round_bg = TG_GUI_PEN_WINDOW;
    /* The oldest slot is visible and the next one is active: neither may be
       evicted. Once every resident slot is protected, the cache must wait. */
    {
        int states[4] = { 1, 1, 2, 1 };
        unsigned char visible[4] = { 1U, 0U, 0U, 1U };
        unsigned long used[4] = { 1UL, 2UL, 3UL, 4UL };

        if (tg_gui_photo_cache_choose_slot(states, visible, used, 4) != 1) {
            puts("gui self-test: photo cache evicted a visible/active slot");
            return 2;
        }
        visible[1] = 1U;
        if (tg_gui_photo_cache_choose_slot(states, visible, used, 4) != -1) {
            puts("gui self-test: photo cache ignored protected slots");
            return 2;
        }
        states[2] = 0;
        if (tg_gui_photo_cache_choose_slot(states, visible, used, 4) != 2) {
            puts("gui self-test: photo cache did not prefer a free slot");
            return 2;
        }
    }
    /* Pass zero is independent from the one-at-a-time quality decoder: every
       requested slot must become a stripped preview before the first quality
       commit is allowed to run. */
    {
        tg_gui_photo_preview_test previews;
        int ready;
        int i;

        memset(&previews, 0, sizeof(previews));
        ready = tg_gui_photo_preview_prepare_all(
            6, tg_gui_photo_preview_test_prepare, &previews);
        previews.quality_commits = 1;
        if (ready != 6) {
            puts("gui self-test: stripped previews remained serialized");
            return 2;
        }
        for (i = 0; i < 6; ++i) {
            if (!previews.preview_only[i]) {
                puts("gui self-test: stripped preview tier incomplete");
                return 2;
            }
        }
    }
    /* Two cache fetches may complete back-to-back, but only the current owner
       may decode and publish. Different patterns make an accidental cross-slot
       commit visible instead of merely checking a busy flag. */
    {
        tg_gui_photo_decode_gate gate;
        unsigned char source_a[7] = { 1U, 3U, 5U, 7U, 9U, 11U, 13U };
        unsigned char source_b[7] = { 2U, 4U, 6U, 8U, 10U, 12U, 14U };
        unsigned char slot_a[7];
        unsigned char slot_b[7];
        int token_a = 1;
        int token_b = 2;

        memset(slot_a, 0, sizeof(slot_a));
        memset(slot_b, 0, sizeof(slot_b));
        tg_gui_photo_decode_gate_reset(&gate);
        if (!tg_gui_photo_decode_gate_acquire(
                &gate, &token_a, 0x11111111UL, 0x01010101UL, 0) ||
            tg_gui_photo_decode_gate_acquire(
                &gate, &token_b, 0x22222222UL, 0x02020202UL, 0)) {
            puts("gui self-test: photo decoders overlapped");
            return 2;
        }
        memcpy(slot_a, source_a, sizeof(slot_a));
        if (memcmp(slot_b, source_b, sizeof(slot_b)) == 0) {
            puts("gui self-test: queued photo committed early");
            return 2;
        }
        tg_gui_photo_decode_gate_release(&gate, &token_a);
        if (!tg_gui_photo_decode_gate_acquire(
                &gate, &token_b, 0x22222222UL, 0x02020202UL, 0)) {
            puts("gui self-test: queued photo did not acquire decoder");
            return 2;
        }
        memcpy(slot_b, source_b, sizeof(slot_b));
        if (memcmp(slot_a, source_a, sizeof(slot_a)) != 0 ||
            memcmp(slot_b, source_b, sizeof(slot_b)) != 0) {
            puts("gui self-test: photo decoder committed to wrong slot");
            return 2;
        }
        tg_gui_photo_decode_gate_release(&gate, &token_b);
    }
    /* The real scheduler measures DateStamp ticks. Drive the same controller
       with deterministic samples here: a fast machine reaches the cap, a slow
       one returns to the historical minimum, and changing slice widths cannot
       change the produced bytes. */
    {
        tg_gui_photo_pace fast;
        tg_gui_photo_pace slow;
        tg_gui_photo_pace decode;
        tg_gui_photo_pace replay;
        unsigned char source[257];
        unsigned char adaptive[257];
        unsigned char direct[257];
        unsigned long at;
        unsigned long i;

        tg_gui_photo_pace_init(&fast, 4UL, 12UL, 256UL, 120UL);
        for (i = 0UL; i < 8UL; ++i) {
            (void)tg_gui_photo_pace_observe(&fast, 20UL);
        }
        tg_gui_photo_pace_init(&slow, 4UL, 12UL, 256UL, 120UL);
        for (i = 0UL; i < 4UL; ++i) {
            (void)tg_gui_photo_pace_observe(&slow, 180UL);
        }
        if (fast.budget != fast.maximum || slow.budget != slow.minimum) {
            puts("gui self-test: adaptive photo pace did not converge");
            return 2;
        }
        /* A slow pen replay used to be charged to the decoder and pinned its
           budget to the floor. Independent clocks must let fast entropy work
           accelerate even while replay remains expensive. */
        tg_gui_photo_pace_init(&decode, 4UL, 12UL, 256UL, 120UL);
        tg_gui_photo_pace_init(&replay, 4UL, 12UL, 256UL, 120UL);
        for (i = 0UL; i < 5UL; ++i) {
            (void)tg_gui_photo_pace_observe(&decode, 20UL);
            (void)tg_gui_photo_pace_observe(&replay, 240UL);
        }
        if (decode.budget <= 12UL || replay.budget != replay.minimum) {
            puts("gui self-test: photo cost centres contaminated pacing");
            return 2;
        }
        for (i = 0UL; i < sizeof(source); ++i) {
            source[i] = (unsigned char)((i * 37UL + 11UL) & 255UL);
            direct[i] = (unsigned char)(source[i] ^ 0x5aU);
        }
        memset(adaptive, 0, sizeof(adaptive));
        tg_gui_photo_pace_init(&fast, 4UL, 4UL, 64UL, 120UL);
        at = 0UL;
        while (at < sizeof(source)) {
            unsigned long count = fast.budget;

            if (count > sizeof(source) - at) {
                count = sizeof(source) - at;
            }
            for (i = 0UL; i < count; ++i) {
                adaptive[at + i] = (unsigned char)(source[at + i] ^ 0x5aU);
            }
            at += count;
            (void)tg_gui_photo_pace_observe(&fast, 20UL);
        }
        if (memcmp(adaptive, direct, sizeof(direct)) != 0) {
            puts("gui self-test: adaptive photo slices changed output");
            return 2;
        }
    }
    /* Newlines split into real lines (recording backend, wide max_width so
       only the '\n' breaks apply): "a\nbc\n\nd" -> a / bc / (blank) / d. */
    {
        unsigned long ws[8];
        unsigned long wl[8];
        int nl = tg_gui_wrap(&backend, "a\nbc\n\nd", 10000, ws, wl, 8);

        if (nl != 4 || wl[0] != 1UL || wl[1] != 2UL || wl[2] != 0UL ||
            wl[3] != 1UL) {
            puts("gui self-test: newline wrap mismatch");
            return 2;
        }
    }

    /* F8 click-to-caret mapping (recording backend: 6 px per char). */
    {
        int off;

        strcpy(state.search_query, "abc");
        state.search_caret = 0;
        /* box text starts at x=10; boundaries at 10,16,22,28 */
        off = tg_gui_search_click_caret(&state, &backend, 18, 4);
        if (off != 1) { /* 18-10=8: nearest boundary after 'a' (9.5 midpoint) */
            puts("gui self-test: search click caret mismatch");
            return 2;
        }
        off = tg_gui_search_click_caret(&state, &backend, 40, 4);
        if (off != 3) { /* beyond the text: caret at the end */
            puts("gui self-test: search click end mismatch");
            return 2;
        }
        off = tg_gui_search_click_caret(&state, &backend, 5000, 4);
        if (off != -1) { /* outside the sidebar box */
            puts("gui self-test: search click outside mismatch");
            return 2;
        }
        state.search_query[0] = '\0';
    }
    backend.avatar_fill = tg_gui_rec_avatar;
    backend.draw_text = tg_gui_rec_text;
    backend.set_style = 0; /* recorder renders plain; markers are just skipped */

    /* Every server-backed message exposes both forwarding actions. Pin the
       context-menu capacity and IDs so adding another conditional item cannot
       silently overrun the fixed arrays or drop the destination picker. */
    {
        const char *labels[TG_GUI_CTX_ITEMS_MAX];
        int ids[TG_GUI_CTX_ITEMS_MAX];
        int count;
        int i;
        int saw_saved;
        int saw_picker;
        int saw_photo_save;

        state.ctx_msg = 0;
        state.messages[0].id = 123UL;
        state.messages[0].is_own = 1;
        state.messages[0].has_document = 1;
        state.messages[0].has_photo = 1;
        count = tg_gui_context_items(&state, labels, ids);
        saw_saved = 0;
        saw_picker = 0;
        saw_photo_save = 0;
        for (i = 0; i < count; ++i) {
            if (ids[i] == TG_GUI_CTX_FORWARD_SAVED) {
                saw_saved = 1;
            } else if (ids[i] == TG_GUI_CTX_FORWARD_TO) {
                saw_picker = 1;
            } else if (ids[i] == TG_GUI_CTX_SAVE_PHOTO) {
                saw_photo_save = 1;
            }
        }
        state.messages[0].id = 0UL;
        state.messages[0].is_own = 0;
        state.messages[0].has_document = 0;
        state.messages[0].has_photo = 0;
        if (count != TG_GUI_CTX_ITEMS_MAX || !saw_saved || !saw_picker ||
            !saw_photo_save) {
            puts("gui self-test: forwarding context items missing");
            return 2;
        }
    }

    /* Sender grouping: a run of incoming messages from one sender shows the
       name once. Own, system and sender-change neighbours all break the run,
       and the first message never groups. */
    {
        tg_gui_message saved0 = state.messages[0];
        tg_gui_message saved1 = state.messages[1];

        tg_gui_copy(state.messages[0].sender, sizeof(state.messages[0].sender),
                    "Mario");
        tg_gui_copy(state.messages[1].sender, sizeof(state.messages[1].sender),
                    "Mario");
        state.messages[0].is_own = 0;
        state.messages[0].is_system = 0;
        state.messages[1].is_own = 0;
        state.messages[1].is_system = 0;
        if (tg_gui_message_grouped(&state, 0) ||
            !tg_gui_message_grouped(&state, 1)) {
            puts("gui self-test: sender grouping run mismatch");
            return 2;
        }
        tg_gui_copy(state.messages[1].sender, sizeof(state.messages[1].sender),
                    "Luigi");
        if (tg_gui_message_grouped(&state, 1)) {
            puts("gui self-test: sender change must break the group");
            return 2;
        }
        tg_gui_copy(state.messages[1].sender, sizeof(state.messages[1].sender),
                    "Mario");
        state.messages[0].is_own = 1;
        if (tg_gui_message_grouped(&state, 1)) {
            puts("gui self-test: own message above must break the group");
            return 2;
        }
        state.messages[0] = saved0;
        state.messages[1] = saved1;
    }

    /* Save-as naming/path joining is platform-neutral. Existing destinations
       are rejected until the requester explicitly confirms replacement. */
    {
        char name[40];
        char path[80];

        if (tg_gui_photo_default_filename(name, sizeof(name), 0x11UL,
                                          0x1234UL) != 0 ||
            strcmp(name, "photo-00001234.jpg") != 0 ||
            tg_gui_photo_build_destination(path, sizeof(path),
                                           "RAM:Downloads", name) != 0 ||
            strcmp(path, "RAM:Downloads/photo-00001234.jpg") != 0 ||
            tg_gui_photo_build_destination(path, sizeof(path),
                                           "RAM:", name) != 0 ||
            strcmp(path, "RAM:photo-00001234.jpg") != 0 ||
            tg_gui_photo_save_allowed(1, 0) ||
            !tg_gui_photo_save_allowed(1, 1) ||
            !tg_gui_photo_save_allowed(0, 0)) {
            puts("gui self-test: photo save-as path policy mismatch");
            return 2;
        }
    }

    tg_gui_paint(&state, &backend);

    ok = 1;
    if (record.fills <= 0 || record.texts <= 0 || record.avatars <= 0 ||
        record.photos != 1) {
        ok = 0;
    }
    if (record.min_x < 0 || record.min_y < 0) {
        ok = 0;
    }
    if (record.max_x > record.width || record.max_y > record.height) {
        ok = 0;
    }
    /* The demo's one "seen" own message must emit exactly one receipt mark. */
    if (record.read_marks != 1) {
        printf("gui self-test: expected 1 read mark, drew %d\n",
               record.read_marks);
        return 2;
    }
    if (record.forbidden_hits != 0) {
        puts("gui self-test: ready photo must hide the [Photo] fallback");
        return 2;
    }
    if (!ok) {
        printf("gui self-test: failed (%d fills, %d avatars, %d photos, %d texts, "
               "bounds x[%d..%d] y[%d..%d] in %dx%d)\n",
               record.fills, record.avatars, record.photos, record.texts, record.min_x,
               record.max_x, record.min_y, record.max_y, record.width,
               record.height);
        return 2;
    }

    /* Repaint at another window width: the bubble geometry changes, but the
       backend's photo identity remains the same canonical cache entry. */
    record.width = 390;
    tg_gui_paint(&state, &backend);
    if (record.photos != 2 || record.photo_decodes != 1 ||
        !record.photo_size_changed) {
        puts("gui self-test: photo cache followed bubble geometry");
        return 2;
    }
    record.width = 480;

    /* A photo is a first-class click target. With inline media disabled the
       backend must receive no photo primitive at all, while the [Photo] label
       keeps the same on-demand viewer hit target. */
    {
        tg_gui_record off_record;
        int hit;

        hit = tg_gui_hit_test(&state, 390, 320, 10,
                              state.photo_x[1] + state.photo_w[1] / 2,
                              state.photo_y[1] + state.photo_h[1] / 2);
        if (hit != TG_GUI_HIT_PHOTO_BASE - 1) {
            puts("gui self-test: inline photo hit-test mismatch");
            return 2;
        }
        state.inline_photos = 0;
        memset(&off_record, 0, sizeof(off_record));
        off_record.width = 480;
        off_record.height = 320;
        off_record.min_x = off_record.width;
        off_record.min_y = off_record.height;
        backend.context = &off_record;
        tg_gui_paint(&state, &backend);
        hit = tg_gui_hit_test(&state, 480, 320, 10,
                              state.photo_x[1] + state.photo_w[1] / 2,
                              state.photo_y[1] + state.photo_h[1] / 2);
        state.inline_photos = 1;
        backend.context = &record;
        if (off_record.photos != 0 || state.photo_w[1] <= 0 ||
            state.photo_h[1] <= 0 || hit != TG_GUI_HIT_PHOTO_BASE - 1) {
            puts("gui self-test: disabled inline photo did background work");
            return 2;
        }

        /* 0.0.92: with inline photos off, a sticker's marker says what the
           sticker is instead of "[Photo]", and it keeps the click target that
           opens the viewer. A photo's marker is unchanged. */
        {
            char kept[TG_GUI_MSG_TEXT_MAX];
            tg_gui_record st_record;

            strcpy(kept, state.messages[1].text);
            strcpy(state.messages[1].text, "[Sticker :)]");
            state.inline_photos = 0;
            memset(&st_record, 0, sizeof(st_record));
            st_record.width = 480;
            st_record.height = 320;
            st_record.min_x = st_record.width;
            st_record.min_y = st_record.height;
            st_record.forbidden = "[Photo]";
            backend.context = &st_record;
            tg_gui_paint(&state, &backend);
            strcpy(state.messages[1].text, kept);
            state.inline_photos = 1;
            backend.context = &record;
            if (st_record.forbidden_hits != 0 || state.photo_w[1] <= 0) {
                puts("gui self-test: sticker marker fell back to [Photo]");
                return 2;
            }
        }
    }

    /* The old first-line format remains readable. "auto" preserves dither and
       cache choices without turning the hardware default into a user choice. */
    {
        const char *pref = "tg-gui-photos-selftest.txt";
        FILE *file;
        int enabled;
        int explicit_choice;
        int dither;
        unsigned long cache_limit;

        (void)remove(pref);
        tg_gui_photo_preferences_load(pref, &enabled, &explicit_choice,
                                      &dither, &cache_limit);
        if (!enabled || explicit_choice ||
            dither != TG_GUI_PHOTO_DITHER_FULL ||
            cache_limit != TG_GUI_PHOTO_CACHE_DEFAULT_MB ||
            tg_gui_photo_preferences_save(
                pref, 1, 0, TG_GUI_PHOTO_DITHER_LIGHT, 200UL) != 0) {
            (void)remove(pref);
            puts("gui self-test: photo preference default/save mismatch");
            return 2;
        }
        tg_gui_photo_preferences_load(pref, &enabled, &explicit_choice,
                                      &dither, &cache_limit);
        if (!enabled || explicit_choice ||
            dither != TG_GUI_PHOTO_DITHER_LIGHT ||
            cache_limit != 200UL) {
            (void)remove(pref);
            puts("gui self-test: photo preference round-trip mismatch");
            return 2;
        }
        if (tg_gui_photo_preferences_save(
                pref, 0, 1, TG_GUI_PHOTO_DITHER_OFF, 10UL) != 0) {
            (void)remove(pref);
            puts("gui self-test: explicit photo preference save failed");
            return 2;
        }
        tg_gui_photo_preferences_load(pref, &enabled, &explicit_choice,
                                      &dither, &cache_limit);
        if (enabled || !explicit_choice ||
            dither != TG_GUI_PHOTO_DITHER_OFF || cache_limit != 10UL) {
            (void)remove(pref);
            puts("gui self-test: explicit photo preference mismatch");
            return 2;
        }
        file = fopen(pref, "wb");
        if (file == 0) {
            (void)remove(pref);
            puts("gui self-test: old photo preference fixture failed");
            return 2;
        }
        enabled = fputs("On\n", file) != EOF;
        if (fclose(file) != 0) {
            enabled = 0;
        }
        if (!enabled) {
            (void)remove(pref);
            puts("gui self-test: old photo preference fixture failed");
            return 2;
        }
        tg_gui_photo_preferences_load(pref, &enabled, &explicit_choice,
                                      &dither, &cache_limit);
        if (!enabled || !explicit_choice ||
            dither != TG_GUI_PHOTO_DITHER_FULL ||
            cache_limit != TG_GUI_PHOTO_CACHE_DEFAULT_MB) {
            (void)remove(pref);
            puts("gui self-test: old photo preference compatibility failed");
            return 2;
        }
        (void)remove(pref);
    }

    /* Hardware default matrix. Explicit OFF and ON win everywhere; without a
       choice, only classic OS3 needs both a 040-class CPU and RTG. */
    {
        int explicit_choice;
        int value;
        int classic_os3;
        int cpu_040;
        int has_rtg;

        for (explicit_choice = 0; explicit_choice <= 1; ++explicit_choice) {
            for (value = 0; value <= 1; ++value) {
                for (classic_os3 = 0; classic_os3 <= 1; ++classic_os3) {
                    for (cpu_040 = 0; cpu_040 <= 1; ++cpu_040) {
                        for (has_rtg = 0; has_rtg <= 1; ++has_rtg) {
                            int expected;
                            int actual;

                            expected = explicit_choice
                                           ? value
                                           : !(classic_os3 &&
                                               (!cpu_040 || !has_rtg));
                            actual = tg_gui_inline_photos_resolve(
                                explicit_choice, value, classic_os3, cpu_040,
                                has_rtg);
                            if (actual != expected) {
                                puts("gui self-test: inline default matrix mismatch");
                                return 2;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Disk-cache policy is portable: protected visible photos survive even
       when older, and the oldest unprotected entries are selected first. */
    {
        tg_gui_photo_cache_item items[4];
        unsigned long remove_count;
        unsigned long long remain;

        memset(items, 0, sizeof(items));
        items[0].bytes = 6UL;
        items[0].days = 1UL;
        items[0].protected_entry = 1;
        items[1].bytes = 7UL;
        items[1].days = 2UL;
        items[2].bytes = 8UL;
        items[2].days = 3UL;
        items[3].bytes = 9UL;
        items[3].days = 4UL;
        remain = tg_gui_photo_cache_prune_plan(
            items, 4, 17UL, 0UL, &remove_count);
        if (remain != 15ULL || remove_count != 2UL || items[0].remove ||
            !items[1].remove || !items[2].remove || items[3].remove) {
            puts("gui self-test: photo cache prune policy mismatch");
            return 2;
        }

        memset(items, 0, sizeof(items));
        items[0].bytes = 3000000000UL;
        items[0].days = 1UL;
        items[1].bytes = 3000000000UL;
        items[1].days = 2UL;
        remain = tg_gui_photo_cache_prune_plan(
            items, 2, 4000000000ULL, 0UL, &remove_count);
        if (remain != 3000000000ULL || remove_count != 1UL ||
            !items[0].remove || items[1].remove) {
            puts("gui self-test: large photo cache total overflow");
            return 2;
        }

        memset(items, 0, sizeof(items));
        items[0].bytes = 6UL;
        items[0].days = 1UL;
        items[1].bytes = 7UL;
        items[1].days = 2UL;
        items[2].bytes = 8UL;
        items[2].days = 3UL;
        remain = tg_gui_photo_cache_prune_plan(
            items, 3, 5ULL, 1UL, &remove_count);
        if (remain != 15ULL || remove_count != 1UL ||
            !items[0].remove || items[1].remove || items[2].remove) {
            puts("gui self-test: photo cache prune tick was not bounded");
            return 2;
        }
    }

    /* Clear uses real files in an isolated host-side fixture and reports the
       bytes it actually removed, not an optimistic catalog count. */
    {
        const char *dir = "tg-gui-photo-cache-selftest";
        const char *paths[2];
        FILE *file;
        unsigned long removed_files;
        unsigned long removed_bytes;
        int i;

        paths[0] = "tg-gui-photo-cache-selftest/a.jpg";
        paths[1] = "tg-gui-photo-cache-selftest/b.pgc";
        (void)mkdir(dir, 0777);
        for (i = 0; i < 2; ++i) {
            int n;
            int size = i == 0 ? 11 : 23;

            file = fopen(paths[i], "wb");
            if (file == 0) {
                (void)remove(paths[0]);
                (void)remove(paths[1]);
                (void)remove(dir);
                puts("gui self-test: photo cache fixture open failed");
                return 2;
            }
            for (n = 0; n < size; ++n) {
                (void)fputc(n, file);
            }
            if (fclose(file) != 0) {
                (void)remove(paths[0]);
                (void)remove(paths[1]);
                (void)remove(dir);
                puts("gui self-test: photo cache fixture write failed");
                return 2;
            }
        }
        file = 0;
        if (tg_gui_photo_cache_clear_files(
                paths, 2, &removed_files, &removed_bytes) != 0 ||
            removed_files != 2UL || removed_bytes != 34UL ||
            (file = fopen(paths[0], "rb")) != 0) {
            if (file != 0) {
                fclose(file);
            }
            (void)remove(paths[0]);
            (void)remove(paths[1]);
            (void)remove(dir);
            puts("gui self-test: photo cache clear mismatch");
            return 2;
        }
        (void)remove(dir);
    }

    /* The "is typing" header line is a transient overlay (live-only, not in the
       demo); paint it once into a fresh recorder to keep that branch in bounds. */
    {
        tg_gui_record trec;

        tg_gui_copy(state.typing, sizeof(state.typing), "Mario is typing...");
        memset(&trec, 0, sizeof(trec));
        trec.width = 480;
        trec.height = 320;
        trec.min_x = trec.width;
        trec.min_y = trec.height;
        backend.context = &trec;
        tg_gui_paint(&state, &backend);
        state.typing[0] = '\0';
        if (trec.texts <= 0 || trec.min_x < 0 || trec.min_y < 0 ||
            trec.max_x > trec.width || trec.max_y > trec.height) {
            puts("gui self-test: typing header overlay out of bounds");
            return 2;
        }
    }

    /* The first-login 2FA screen must paint in bounds AND mask the password:
       the raw secret must never reach the backend's draw_text. */
    {
        tg_gui_state ls;
        tg_gui_record lrec;

        memset(&ls, 0, sizeof(ls));
        ls.theme = TG_GUI_THEME_DARK;
        ls.mode = TG_GUI_MODE_LOGIN_2FA;
        ls.input_masked = 1;
        ls.cursor_on = 1;
        tg_gui_copy(ls.input, sizeof(ls.input), "zzqp7secret");
        tg_gui_copy(ls.status, sizeof(ls.status), "2FA password");

        memset(&lrec, 0, sizeof(lrec));
        lrec.width = 480;
        lrec.height = 320;
        lrec.min_x = lrec.width;
        lrec.min_y = lrec.height;
        lrec.forbidden = "zzqp7secret";
        backend.context = &lrec;
        tg_gui_paint(&ls, &backend);
        if (lrec.texts <= 0 || lrec.min_x < 0 || lrec.min_y < 0 ||
            lrec.max_x > lrec.width || lrec.max_y > lrec.height) {
            puts("gui self-test: login screen out of bounds");
            return 2;
        }
        if (lrec.forbidden_hits != 0) {
            puts("gui self-test: 2FA password must be masked, not drawn");
            return 2;
        }
    }

    /* '@' mention token finder: the pure text rules the composer popup rides on. */
    {
        int st = -1;

        if (tg_gui_mention_token("@ma", 3, &st) != 2 || st != 0) {
            puts("gui self-test: mention token at line start failed");
            return 2;
        }
        if (tg_gui_mention_token("ciao @lu", 8, &st) != 2 || st != 5) {
            puts("gui self-test: mention token after space failed");
            return 2;
        }
        if (tg_gui_mention_token("ciao @", 6, &st) != 0 || st != 5) {
            puts("gui self-test: bare '@' must yield an empty prefix");
            return 2;
        }
        if (tg_gui_mention_token("mail@host", 9, &st) != -1) {
            puts("gui self-test: email-style '@' must not be a mention");
            return 2;
        }
        if (tg_gui_mention_token("@ma poi", 7, &st) != -1) {
            puts("gui self-test: space between '@' and caret must end the token");
            return 2;
        }
        if (tg_gui_mention_token("ciao", 4, &st) != -1 ||
            tg_gui_mention_token("", 0, &st) != -1) {
            puts("gui self-test: no-'@' input must yield no token");
            return 2;
        }
    }

    printf("gui self-test: ok (%d chats, %d msgs, %d fills, %d avatars, "
           "%d photos, %d texts, within %dx%d)\n",
           state.chat_count, state.message_count, record.fills, record.avatars,
           record.photos, record.texts, record.width, record.height);
    return 0;
}
#endif /* !TG_NO_SELFTEST */
