/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * GUI chat driver: projects chat-engine rows into tg_gui_state. See
 * tg_gui_driver.h. The console driver prints a row; this one appends it to the
 * GUI message model.
 */

#include "tg_gui_driver.h"
#include "tg_mtproto_probe.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void tg_gui_driver_copy(char *dest, unsigned long size, const char *src)
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

/* Engine rows carry raw UTF-8 (the console transcodes at print time). The GUI
   draws with the Amiga font (Latin-1), so copy text with a UTF-8 -> Latin-1
   transcode: codepoints < 256 map 1:1 (Italian accents render correctly),
   control bytes flatten to a space, and anything higher (emoji, dashes, ...)
   routes through tg_mtproto_display_codepoint_to_latin1, which renders the
   common emoji as their ASCII emoticons (":)" etc.) and omits anything with no
   readable rendition rather than drawing a '?'. Invalid UTF-8 bytes are
   skipped. */
static void tg_gui_driver_copy_latin1(char *dest, unsigned long size,
                                      const char *src)
{
    unsigned long i;
    unsigned long o;

    if (dest == 0 || size == 0UL) {
        return;
    }
    i = 0UL;
    o = 0UL;
    while (src != 0 && src[i] != '\0' && o + 1UL < size) {
        unsigned char c;
        unsigned long cp;
        int extra;
        int skip;

        skip = 0;
        c = (unsigned char)src[i++];
        if (c < 0x80U) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0U) == 0xC0U) {
            cp = (unsigned long)(c & 0x1FU);
            extra = 1;
        } else if ((c & 0xF0U) == 0xE0U) {
            cp = (unsigned long)(c & 0x0FU);
            extra = 2;
        } else if ((c & 0xF8U) == 0xF0U) {
            cp = (unsigned long)(c & 0x07U);
            extra = 3;
        } else {
            cp = 0UL; /* stray continuation / invalid lead byte -> omit */
            extra = 0;
            skip = 1;
        }
        while (extra > 0) {
            unsigned char cc;

            cc = (unsigned char)src[i];
            if ((cc & 0xC0U) != 0x80U) {
                skip = 1; /* truncated sequence -> omit */
                break;
            }
            cp = (cp << 6) | (unsigned long)(cc & 0x3FU);
            ++i;
            --extra;
        }
        if (skip) {
            continue;
        }
        if (cp == 0x0AUL) {
            dest[o++] = '\n'; /* keep real line breaks (lists, paragraphs) */
        } else if (cp == 0x0DUL) {
            continue; /* drop CR: a CRLF becomes a single '\n' */
        } else if (cp < 0x20UL) {
            dest[o++] = ' '; /* other control bytes -> space */
        } else if (cp < 0x100UL) {
            dest[o++] = (char)cp;
        } else {
            char tmp[8];
            unsigned long n;
            unsigned long k;

            /* A codepoint the glyph sheet knows becomes an emoji pair, the
               same two bytes the picker inserts, so a received face and a
               sent one are one thing on screen; the backend decides at draw
               time whether the cell is big enough for the picture or draws
               the text emoticon instead. Everything else keeps the text path. */
            {
                int gi = tg_gui_emoji_index_of(cp);

                if (gi >= 0 && tg_gui_emoji_encode((unsigned long)gi, tmp)) {
                    n = 2UL;
                    for (k = 0UL; k < n && o + 1UL < size; ++k) {
                        dest[o++] = tmp[k];
                    }
                    continue;
                }
            }
            n = tg_mtproto_display_codepoint_to_latin1(cp, tmp, sizeof(tmp));
            if (n == 0UL && o > 0UL && dest[o - 1UL] == ' ' &&
                !tg_mtproto_display_codepoint_is_invisible(cp)) {
                /* A symbol with no Latin-1 shape leaves nothing behind, and
                   the space that introduced it would read as a hole: "ciao
                   <brick> mondo" became "ciao  mondo", "[Sticker <brick>]"
                   became "[Sticker ]". Take the space with it. Modifiers are
                   exempt: they attach to the character before them. Plenty of
                   emoji DO have a rendition (112 are mapped by hand, and the
                   party popper is one of them); those keep their space. */
                --o;
            }
            for (k = 0UL; k < n && o + 1UL < size; ++k) {
                dest[o++] = tmp[k];
            }
        }
    }
    dest[o] = '\0';
}

int tg_gui_driver_color_for(const char *name)
{
    unsigned long hash;
    unsigned long i;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    hash = 5381UL;
    for (i = 0UL; name[i] != '\0'; ++i) {
        hash = ((hash << 5) + hash) + (unsigned long)(unsigned char)name[i];
    }
    return (int)(hash % (unsigned long)TG_GUI_AVATAR_COLORS);
}

static void tg_gui_driver_on_message(void *ctx, const tg_chat_message_row *row)
{
    /* any outcome below mutates or may mutate the transcript layout */

    tg_gui_chat_driver *gui;
    tg_gui_state *state;
    tg_gui_message *message;
    const char *who;

    gui = (tg_gui_chat_driver *)ctx;
    if (gui == 0 || gui->state == 0 || row == 0) {
        return;
    }
    state = gui->state;

    /* Multi-device dedup/reconcile: the open-chat poll now includes outgoing, so
       a message sent from ANOTHER session/device of this account shows up here.
       But our own just-sent message is already on screen as the optimistic echo,
       so never let it double:
         - server-id already present  -> it's a re-fetch, skip;
         - outgoing row matching an own echo that has no server id yet (sent_id
           was 0, e.g. a group/channel send whose response carried no
           updateShortSentMessage) -> adopt the id onto that echo;
         - otherwise fall through and append (a genuinely new message, e.g. one
           sent from the other device).
       Text is compared in the stored Latin-1 form (both the echo and the row end
       up Latin-1), so it matches regardless of the wire UTF-8. */
    if (row->id != 0UL) {
        char row_latin1[TG_GUI_MSG_TEXT_MAX];
        int have_l1 = 0;
        int di;

        for (di = 0; di < state->message_count; ++di) {
            tg_gui_message *m = &state->messages[di];

            if (m->id == row->id) {
                return; /* already shown (server id match) */
            }
            if (row->is_out && m->is_own && m->id == 0UL) {
                if (!have_l1) {
                    tg_gui_driver_copy_latin1(row_latin1, sizeof(row_latin1),
                                              row->text);
                    have_l1 = 1;
                }
                if (strcmp(m->text, row_latin1) == 0) {
                    m->id = row->id; /* the echo finally learns its server id */
                    if (m->id <= state->open_read_outbox_max) {
                        m->read_state = TG_GUI_READ_SEEN;
                    }
                    return;
                }
            }
        }
    }

    if (gui->prepend_at >= 0) {
        /* Load-older paging: insert this (older) row at prepend_at, shifting the
           existing block down one, then advance. The batch is routed oldest-first
           so successive inserts at 0,1,2,... land in order above the transcript.
           When the ring is full, drop the NEWEST (tail) to make room for older
           content -- the user is scrolling up, and reopening re-pins the newest. */
        int at = gui->prepend_at;
        int i;

        if (state->message_count >= TG_GUI_MAX_MESSAGES) {
            if (!gui->prepend_allow_drop) {
                /* Ring full and the newest rows are on-screen: refuse rather than
                   evict them. prepend_at does not advance, so load_older sees the
                   batch stop here and reports how many actually fit. */
                return;
            }
            state->message_count = TG_GUI_MAX_MESSAGES - 1; /* drop newest tail */
            state->newest_dropped = 1; /* ring-bottom now stale -> jump must reload */
            state->msg_gen++;
        }
        if (at > state->message_count) {
            at = state->message_count;
        }
        state->msg_gen++;
        for (i = state->message_count; i > at; --i) {
            state->messages[i] = state->messages[i - 1];
        }
        message = &state->messages[at];
        gui->prepend_at = at + 1;
        state->message_count += 1;
        state->msg_gen++;
    } else {
        /* Keep the most recent TG_GUI_MAX_MESSAGES: drop the oldest when full. */
        if (state->message_count >= TG_GUI_MAX_MESSAGES) {
            int i;

            for (i = 1; i < TG_GUI_MAX_MESSAGES; ++i) {
                state->messages[i - 1] = state->messages[i];
            }
            state->message_count = TG_GUI_MAX_MESSAGES - 1;
        }
        message = &state->messages[state->message_count];
        state->message_count += 1;
        state->msg_gen++;
        /* A live message arrived while the user is NOT at the true newest: count
           it for the scroll-to-bottom button's badge. (Own sends jump to the
           bottom, so they are never "unread"; the painter resets this on a jump
           and tg_gui_session_open_chat clears it on any reload.) */
        if (state->transcript_scroll > 0 || state->newest_dropped) {
            state->unread_below += 1;
        }
    }
    memset(message, 0, sizeof(*message));

    message->is_own = row->is_out ? 1 : 0;
    message->is_system = 0;
    message->has_document = row->has_document; /* F9: gates the Download item */
    message->has_photo = row->has_photo;
    message->photo_ready = row->photo_ready;
    message->photo_only = row->photo_only;
    message->photo_id_hi = row->photo_id_hi;
    message->photo_id_lo = row->photo_id_lo;
    message->photo_width = row->photo_width;
    message->photo_height = row->photo_height;

    /* Sender label: own name when outgoing; the resolved sender otherwise; the
       1:1 peer name as the fallback; empty for an unresolved group author (the
       GUI shows the bubble without a name rather than a "?:" marker). */
    if (row->is_out) {
        who = (row->own_label != 0 && row->own_label[0] != '\0') ? row->own_label
                                                                 : "";
    } else if (row->sender != 0) {
        who = row->sender;
    } else if (!row->is_group && row->peer_label != 0 &&
               row->peer_label[0] != '\0') {
        who = row->peer_label;
    } else {
        who = "";
    }
    tg_gui_driver_copy_latin1(message->sender, sizeof(message->sender), who);
    message->sender_color = tg_gui_driver_color_for(who);
    tg_gui_driver_copy_latin1(message->text, sizeof(message->text), row->text);
    if (row->reply_quote != 0 && row->reply_quote[0] != '\0') {
        tg_gui_driver_copy_latin1(message->reply_text,
                                  sizeof(message->reply_text), row->reply_quote);
    }
    message->id = row->id;
    message->from_id_hi = row->from_id_hi;
    message->from_id_lo = row->from_id_lo;
    if (message->is_own) {
        message->read_state =
            (message->id != 0UL &&
             message->id <= state->open_read_outbox_max)
                ? TG_GUI_READ_SEEN
                : TG_GUI_READ_SENT;
    }

    if (row->has_time) {
        time_t when;
        struct tm *parts;

        when = (time_t)row->local_epoch;
        parts = gmtime(&when);
        if (parts != 0) {
            sprintf(message->time, "%02d:%02d", parts->tm_hour, parts->tm_min);
        }
    }
    /* message_count was advanced by the prepend/append prologue above. */
}

void tg_gui_driver_append_own(tg_gui_chat_driver *gui, const char *text,
                              const char *own_label, const char *reply_snippet,
                              unsigned long sent_id)
{
    tg_gui_state *state;
    tg_gui_message *message;

    if (gui == 0 || gui->state == 0 || text == 0) {
        return;
    }
    state = gui->state;
    state->msg_gen++;
    if (state->message_count >= TG_GUI_MAX_MESSAGES) {
        int i;

        for (i = 1; i < TG_GUI_MAX_MESSAGES; ++i) {
            state->messages[i - 1] = state->messages[i];
        }
        state->message_count = TG_GUI_MAX_MESSAGES - 1;
    }
    message = &state->messages[state->message_count];
    memset(message, 0, sizeof(*message));
    message->is_own = 1;
    /* The composed text was typed via VANILLAKEY -- already Latin-1, so copy it
       verbatim (no UTF-8 transcode). The own label comes from the cache. */
    tg_gui_driver_copy(message->text, sizeof(message->text), text);
    tg_gui_driver_copy_latin1(message->sender, sizeof(message->sender),
                              (own_label != 0) ? own_label : "");
    message->sender_color =
        tg_gui_driver_color_for((own_label != 0) ? own_label : "");
    /* Optimistic echo: delivered ("sent"). messages.sendMessage returned the
       server message id (sent_id), so stamp it here -- that lets the read-cursor
       poll promote this echo to "seen" (the azure double-check) IN PLACE the
       moment the peer reads it, instead of only after a chat re-open re-fetches
       the row. 0 only if the send response carried no id (older fallback). */
    message->read_state = TG_GUI_READ_SENT;
    message->id = sent_id;
    /* Mirror the reply quote locally so the sent message shows its "> ..." line
       (own messages are never re-fetched, so the echo is the only local copy). */
    if (reply_snippet != 0 && reply_snippet[0] != '\0') {
        tg_gui_driver_copy(message->reply_text, sizeof(message->reply_text),
                           reply_snippet);
    }
    /* No server timestamp on the optimistic echo (memset left time empty). */
    state->message_count += 1;
}

int tg_gui_driver_set_read_outbox_max(tg_gui_chat_driver *gui,
                                      unsigned long read_outbox_max)
{
    tg_gui_state *state;
    int changed;
    int i;

    if (gui == 0 || gui->state == 0) {
        return 0;
    }
    state = gui->state;
    if (read_outbox_max <= state->open_read_outbox_max) {
        return 0; /* the read cursor only advances within a chat */
    }
    state->open_read_outbox_max = read_outbox_max;
    /* Promote already-shown own messages the peer has now read. */
    changed = 0;
    for (i = 0; i < state->message_count; ++i) {
        tg_gui_message *message;

        message = &state->messages[i];
        if (message->is_own && message->id != 0UL &&
            message->id <= read_outbox_max &&
            message->read_state == TG_GUI_READ_SENT) {
            message->read_state = TG_GUI_READ_SEEN;
            changed = 1;
        }
    }
    return changed;
}

void tg_gui_driver_reset_read_outbox(tg_gui_chat_driver *gui)
{
    if (gui != 0 && gui->state != 0) {
        gui->state->open_read_outbox_max = 0UL; /* new chat: cursor restarts */
    }
}

int tg_gui_driver_has_unseen_own(const tg_gui_chat_driver *gui)
{
    int i;

    if (gui == 0 || gui->state == 0) {
        return 0;
    }
    for (i = 0; i < gui->state->message_count; ++i) {
        const tg_gui_message *message = &gui->state->messages[i];

        if (message->is_own && message->id != 0UL &&
            message->read_state == TG_GUI_READ_SENT) {
            return 1; /* an own message still awaiting the peer's read receipt */
        }
    }
    return 0;
}

int tg_gui_driver_update_text(tg_gui_chat_driver *gui, unsigned long message_id,
                              const char *text)
{
    int i;

    if (gui == 0 || gui->state == 0 || text == 0 || message_id == 0UL) {
        return 0;
    }
    for (i = 0; i < gui->state->message_count; ++i) {
        if (gui->state->messages[i].id == message_id) {
            /* Latin-1 verbatim (the composer text is already Latin-1, like the
               optimistic echo); the bubble re-wraps on the next paint. */
            if (strcmp(gui->state->messages[i].text, text) != 0) {
                tg_gui_driver_copy(gui->state->messages[i].text,
                                   sizeof(gui->state->messages[i].text), text);
                gui->state->msg_gen++;
            }
            return 1;
        }
    }
    return 0;
}

int tg_gui_driver_update_text_utf8(tg_gui_chat_driver *gui,
                                   unsigned long message_id,
                                   const char *text)
{
    static char latin1[TG_GUI_MSG_TEXT_MAX];

    if (text == 0) {
        return 0;
    }
    tg_gui_driver_copy_latin1(latin1, sizeof(latin1), text);
    return tg_gui_driver_update_text(gui, message_id, latin1);
}

int tg_gui_driver_remove_by_id(tg_gui_chat_driver *gui, unsigned long message_id)
{
    int i;
    int j;

    if (gui == 0 || gui->state == 0 || message_id == 0UL) {
        return 0;
    }
    for (i = 0; i < gui->state->message_count; ++i) {
        if (gui->state->messages[i].id == message_id) {
            for (j = i + 1; j < gui->state->message_count; ++j) {
                gui->state->messages[j - 1] = gui->state->messages[j];
            }
            gui->state->message_count -= 1;
            gui->state->msg_gen++;
            return 1;
        }
    }
    return 0;
}

int tg_gui_driver_mark_photo_ready(tg_gui_chat_driver *gui,
                                   unsigned long photo_id_hi,
                                   unsigned long photo_id_lo)
{
    int i;
    int changed;

    if (gui == 0 || gui->state == 0 ||
        (photo_id_hi == 0UL && photo_id_lo == 0UL)) {
        return 0;
    }
    changed = 0;
    for (i = 0; i < gui->state->message_count; ++i) {
        tg_gui_message *message;

        message = &gui->state->messages[i];
        if (message->has_photo && !message->photo_ready &&
            message->photo_id_hi == photo_id_hi &&
            message->photo_id_lo == photo_id_lo) {
            message->photo_ready = 1;
            changed = 1;
        }
    }
    if (changed) {
        gui->state->msg_gen++;
    }
    return changed;
}

/* Derives 1-2 uppercase initials from a display name (skipping a leading '@'):
   first letter of the first word, plus the first letter of the second word if
   present. "Mario Rossi" -> "MR", "Anna" -> "A", "" -> "?". */
static void tg_gui_driver_initials(char *dest, unsigned long size,
                                   const char *name)
{
    unsigned long out;
    unsigned long i;
    int in_word;
    int words;

    if (dest == 0 || size == 0UL) {
        return;
    }
    if (name != 0 && name[0] == '@') {
        ++name;
    }
    out = 0UL;
    in_word = 0;
    words = 0;
    for (i = 0UL; name != 0 && name[i] != '\0' && out + 1UL < size; ++i) {
        char c;

        c = name[i];
        if (c == ' ' || c == '\t') {
            in_word = 0;
            continue;
        }
        if (!in_word) {
            in_word = 1;
            ++words;
            if (words > 2) {
                break;
            }
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            dest[out++] = c;
        }
    }
    if (out == 0UL && size > 1UL) {
        dest[out++] = '?';
    }
    dest[out] = '\0';
}

static void tg_gui_driver_on_chat_list_changed(void *ctx,
                                               const tg_chat_list_row *rows,
                                               int count)
{
    tg_gui_chat_driver *gui;
    tg_gui_state *state;
    int i;
    int n;

    gui = (tg_gui_chat_driver *)ctx;
    if (gui == 0 || gui->state == 0 || rows == 0) {
        return;
    }
    state = gui->state;
    n = count;
    if (n > TG_GUI_MAX_CHATS) {
        n = TG_GUI_MAX_CHATS;
    }
    state->chat_count = 0;
    state->selected_chat = 0;
    for (i = 0; i < n; ++i) {
        tg_gui_chat *chat;
        char display[TG_GUI_NAME_MAX];

        chat = &state->chats[state->chat_count];
        memset(chat, 0, sizeof(*chat));
        /* The display name keeps the "@" for username-only chats; the peer
           cache has no last-message preview, so preview/time stay empty. */
        if (rows[i].name_is_username && rows[i].name[0] != '\0') {
            display[0] = '@';
            tg_gui_driver_copy(display + 1, sizeof(display) - 1U, rows[i].name);
        } else {
            tg_gui_driver_copy(display, sizeof(display), rows[i].name);
        }
        tg_gui_driver_copy_latin1(chat->name, sizeof(chat->name), display);
        tg_gui_driver_initials(chat->initials, sizeof(chat->initials),
                               chat->name);
        chat->avatar_color = tg_gui_driver_color_for(chat->name);
        chat->unread = (rows[i].unread > 0UL) ? (int)rows[i].unread : 0;
        chat->index = rows[i].index;
        chat->peer_id_hi = rows[i].peer_id_hi;
        chat->peer_id_lo = rows[i].peer_id_lo;
        chat->flash = 0;
        if (rows[i].is_current) {
            state->selected_chat = state->chat_count;
        }
        state->chat_count += 1;
    }
}

/* A cross-chat notification: find the sidebar row whose peer id matches and
   bump its unread badge + flag it to blink. Peer ids in one cache are unique
   per chat, so the id alone disambiguates. The currently open chat does not
   flash (you are already reading it). Unmatched peers (not in the sidebar) are
   ignored -- there is no row to badge. */
static void tg_gui_driver_on_notification(void *ctx,
                                          const tg_chat_notify_entry *entry)
{
    tg_gui_chat_driver *gui;
    tg_gui_state *state;
    int i;

    gui = (tg_gui_chat_driver *)ctx;
    if (gui == 0 || gui->state == 0 || entry == 0) {
        return;
    }
    state = gui->state;
    /* While the search picker is up, state->chats[] holds transient search
       results (peer id 0), not the real sidebar -- nothing to badge here. */
    if (state->in_search) {
        return;
    }
    for (i = 0; i < state->chat_count; ++i) {
        tg_gui_chat *chat;

        chat = &state->chats[i];
        if (chat->peer_id_hi == entry->peer_id_hi &&
            chat->peer_id_lo == entry->peer_id_lo) {
            /* Only count/flash what you are NOT reading: the open chat's incoming
               messages are already shown in the transcript, so its badge must not
               grow while you look at it. */
            if (i != state->selected_chat) {
                chat->unread += 1;
                chat->flash = 1;
            }
            return;
        }
    }
}

void tg_gui_chat_driver_bind(tg_gui_chat_driver *gui, tg_gui_state *state,
                             tg_chat_driver *chat_driver)
{
    if (gui == 0 || chat_driver == 0) {
        return;
    }
    gui->state = state;
    gui->prepend_at = -1; /* append by default; load-older flips it transiently */
    gui->prepend_allow_drop = 0;
    chat_driver->ctx = gui;
    chat_driver->on_message = tg_gui_driver_on_message;
    chat_driver->on_chat_list_changed = tg_gui_driver_on_chat_list_changed;
    chat_driver->on_notification = tg_gui_driver_on_notification;
}

/* --- self-test ---------------------------------------------------------- */

#if !defined(TG_NO_SELFTEST)
static void tg_gui_driver_emit(tg_chat_driver *driver, unsigned long epoch,
                               int has_time, int is_out, int is_group,
                               const char *peer_label, const char *own_label,
                               const char *sender, const char *text)
{
    tg_chat_message_row row;

    memset(&row, 0, sizeof(row)); /* id / reply_quote default to none */
    row.text = text;
    row.local_epoch = epoch;
    row.has_time = has_time;
    row.is_out = is_out;
    row.is_group = is_group;
    row.peer_label = peer_label;
    row.own_label = own_label;
    row.sender = sender;
    driver->on_message(driver->ctx, &row);
}

int tg_gui_driver_self_test(void)
{
    tg_gui_state state;
    tg_gui_chat_driver gui;
    tg_chat_driver driver;
    int i;

    memset(&state, 0, sizeof(state));
    tg_gui_chat_driver_bind(&gui, &state, &driver);

    /* 0.0.92: a symbol with no Latin-1 shape takes its leading space with it,
       so it leaves a sentence rather than a hole. Modifiers do not: they hang
       off the character before them, which is not a space. */
    {
        char out[64];

        tg_gui_driver_copy_latin1(out, sizeof(out),
                                  "ciao \xf0\x9f\xa7\xb1 mondo");
        if (strcmp(out, "ciao mondo") != 0) {
            printf("gui driver self-test: omitted symbol left \"%s\"\n", out);
            return 2;
        }
        tg_gui_driver_copy_latin1(out, sizeof(out),
                                  "[Sticker \xf0\x9f\xa7\xb1]");
        if (strcmp(out, "[Sticker]") != 0) {
            printf("gui driver self-test: sticker fallback left \"%s\"\n", out);
            return 2;
        }
        /* One the glyph sheet knows keeps its space and becomes the two byte
           pair the picker inserts (0.0.93), so a received face and a sent
           one are one thing on screen; the backend draws it as a picture or
           as the emoticon depending on the cell size. */
        tg_gui_driver_copy_latin1(out, sizeof(out),
                                  "[Sticker \xf0\x9f\x98\x80]");
        {
            unsigned long idx = 0UL;
            int want = tg_gui_emoji_index_of(0x1f600UL);

            if (want < 0 || strlen(out) != 12UL || out[8] != ' ' ||
                !tg_gui_emoji_pair_at(out, 12UL, 9UL, &idx) ||
                idx != (unsigned long)want || out[11] != ']') {
                printf("gui driver self-test: sheet emoji left \"%s\"\n", out);
                return 2;
            }
        }
        /* A variation selector after a space must not eat it. */
        tg_gui_driver_copy_latin1(out, sizeof(out), "a \xef\xb8\x8f" "b");
        if (strcmp(out, "a b") != 0) {
            printf("gui driver self-test: modifier ate a space (\"%s\")\n", out);
            return 2;
        }
    }

    /* Incoming 1:1 (peer fallback), outgoing (own label), group (resolved
       sender), group unknown (no name), and a no-time row. */
    tg_gui_driver_emit(&driver, 1700000000UL, 1, 0, 0, "Mario", "Io", 0, "Ciao");
    tg_gui_driver_emit(&driver, 1700000300UL, 1, 1, 0, "Mario", "Io", 0,
                       "Tutto bene?");
    tg_gui_driver_emit(&driver, 1700003600UL, 1, 0, 1, "Sviluppo", "Io", "Alice",
                       "Salve");
    tg_gui_driver_emit(&driver, 1700003900UL, 1, 0, 1, "Sviluppo", "Io", 0,
                       "boh");
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "Mario", "Io", 0, "senza data");

    if (state.message_count != 5) {
        printf("gui driver self-test: count %d != 5\n", state.message_count);
        return 2;
    }
    if (state.messages[0].is_own != 0 ||
        strcmp(state.messages[0].sender, "Mario") != 0 ||
        strcmp(state.messages[0].text, "Ciao") != 0 ||
        strcmp(state.messages[0].time, "22:13") != 0) {
        puts("gui driver self-test: incoming 1:1 row wrong");
        return 2;
    }
    if (state.messages[1].is_own != 1 ||
        strcmp(state.messages[1].sender, "Io") != 0 ||
        strcmp(state.messages[1].text, "Tutto bene?") != 0) {
        puts("gui driver self-test: outgoing row wrong");
        return 2;
    }
    if (state.messages[2].is_own != 0 ||
        strcmp(state.messages[2].sender, "Alice") != 0 ||
        strcmp(state.messages[2].time, "23:13") != 0) {
        puts("gui driver self-test: group resolved-sender row wrong");
        return 2;
    }

    /* Inline-photo metadata stays compact across the engine/GUI boundary and
       completion flips every matching bubble exactly once. */
    {
        tg_gui_state ps;
        tg_gui_chat_driver pg;
        tg_chat_driver pd;
        tg_chat_message_row prow;

        memset(&ps, 0, sizeof(ps));
        tg_gui_chat_driver_bind(&pg, &ps, &pd);
        memset(&prow, 0, sizeof(prow));
        prow.text = "[Photo]";
        prow.sender = "Mario";
        prow.has_photo = 1;
        prow.photo_only = 1;
        prow.photo_id_hi = 0x12UL;
        prow.photo_id_lo = 0x34UL;
        prow.photo_width = 320UL;
        prow.photo_height = 180UL;
        pd.on_message(pd.ctx, &prow);
        if (ps.message_count != 1 || !ps.messages[0].has_photo ||
            ps.messages[0].photo_ready || !ps.messages[0].photo_only ||
            ps.messages[0].photo_width != 320UL ||
            tg_gui_driver_mark_photo_ready(&pg, 0x12UL, 0x34UL) != 1 ||
            !ps.messages[0].photo_ready ||
            tg_gui_driver_mark_photo_ready(&pg, 0x12UL, 0x34UL) != 0) {
            puts("gui driver self-test: inline photo projection mismatch");
            return 2;
        }
    }
    if (state.messages[3].sender[0] != '\0') {
        puts("gui driver self-test: unknown group author should have no name");
        return 2;
    }
    if (state.messages[4].time[0] != '\0') {
        puts("gui driver self-test: no-date row should have empty time");
        return 2;
    }
    /* Stable, in-range avatar colours; same name -> same colour. */
    if (state.messages[0].sender_color < 0 ||
        state.messages[0].sender_color >= TG_GUI_AVATAR_COLORS) {
        puts("gui driver self-test: avatar colour out of range");
        return 2;
    }
    if (tg_gui_driver_color_for("Alice") != tg_gui_driver_color_for("Alice")) {
        puts("gui driver self-test: colour mapping not stable");
        return 2;
    }

    /* UTF-8 -> Latin-1 transcode: Italian accents map 1:1; an emoji the
       glyph sheet knows becomes the two byte pair (0.0.93), which the
       backend draws as a picture or as the ":)" emoticon by cell size.
       "cia\xC3\xB2 \xF0\x9F\x99\x82" = "cia(o-grave) (slight-smile)". */
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "Mario", "Io", 0,
                       "cia\xC3\xB2 \xF0\x9F\x99\x82");
    {
        char want[8];
        int gi = tg_gui_emoji_index_of(0x1f642UL);

        strcpy(want, "cia\xF2 ");
        if (gi < 0 || !tg_gui_emoji_encode((unsigned long)gi, want + 5)) {
            puts("gui driver self-test: slight smile missing from the sheet");
            return 2;
        }
        want[7] = '\0';
        if (strcmp(state.messages[state.message_count - 1].text, want) != 0) {
        printf("gui driver self-test: utf8->latin1 wrong (%s)\n",
               state.messages[state.message_count - 1].text);
        return 2;
        }
    }

    /* Unmapped symbol (U+1F4E6 package, no readable rendition) is OMITTED, not
       '?'. (U+1F4A9 now maps to "(poo)", so it is no longer an unmapped case.) */
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "Mario", "Io", 0,
                       "\xF0\x9F\x93\xA6");
    if (state.messages[state.message_count - 1].text[0] != '\0') {
        printf("gui driver self-test: unmapped symbol not omitted (%s)\n",
               state.messages[state.message_count - 1].text);
        return 2;
    }

    /* Ring overflow: push well past capacity, count caps and the oldest drops. */
    for (i = 0; i < TG_GUI_MAX_MESSAGES + 10; ++i) {
        tg_gui_driver_emit(&driver, 1700000000UL, 1, 0, 0, "Mario", "Io", 0,
                           "flood");
    }
    if (state.message_count != TG_GUI_MAX_MESSAGES) {
        printf("gui driver self-test: ring not capped (%d)\n",
               state.message_count);
        return 2;
    }
    if (strcmp(state.messages[TG_GUI_MAX_MESSAGES - 1].text, "flood") != 0) {
        puts("gui driver self-test: newest not retained after overflow");
        return 2;
    }

    /* Load-older PREPEND path. (1) Non-full ring: a batch routed oldest-first
       (O1 then O2) lands in order ABOVE the existing rows. */
    memset(&state, 0, sizeof(state));
    tg_gui_chat_driver_bind(&gui, &state, &driver);
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "A");
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "B");
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "C");
    gui.prepend_at = 0;
    gui.prepend_allow_drop = 0;
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "O1");
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "O2");
    gui.prepend_at = -1;
    if (state.message_count != 5 ||
        strcmp(state.messages[0].text, "O1") != 0 ||
        strcmp(state.messages[1].text, "O2") != 0 ||
        strcmp(state.messages[2].text, "A") != 0 ||
        strcmp(state.messages[4].text, "C") != 0) {
        puts("gui driver self-test: prepend order wrong");
        return 2;
    }
    /* (2) FULL ring, allow_drop=0: the insert is REFUSED so the on-screen newest
       is never evicted, and prepend_at does not advance. */
    memset(&state, 0, sizeof(state));
    tg_gui_chat_driver_bind(&gui, &state, &driver);
    for (i = 0; i < TG_GUI_MAX_MESSAGES - 1; ++i) {
        tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "old");
    }
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "NEWEST");
    gui.prepend_at = 0;
    gui.prepend_allow_drop = 0;
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "refused");
    if (state.message_count != TG_GUI_MAX_MESSAGES || gui.prepend_at != 0 ||
        strcmp(state.messages[TG_GUI_MAX_MESSAGES - 1].text, "NEWEST") != 0) {
        puts("gui driver self-test: full-ring prepend must refuse (keep newest)");
        return 2;
    }
    /* (3) Same FULL ring, allow_drop=1: the older row leads the buffer and the
       newest tail is evicted to make room. */
    gui.prepend_at = 0;
    gui.prepend_allow_drop = 1;
    tg_gui_driver_emit(&driver, 0UL, 0, 0, 0, "P", "Io", 0, "OLDEST");
    if (state.message_count != TG_GUI_MAX_MESSAGES || gui.prepend_at != 1 ||
        strcmp(state.messages[0].text, "OLDEST") != 0 ||
        strcmp(state.messages[TG_GUI_MAX_MESSAGES - 1].text, "NEWEST") == 0) {
        puts("gui driver self-test: full-ring prepend (drop newest) wrong");
        return 2;
    }
    gui.prepend_at = -1;
    gui.prepend_allow_drop = 0;

    /* Chat-list projection: rows -> tg_gui_state.chats (the sidebar). */
    {
        tg_chat_list_row list[4];

        memset(list, 0, sizeof(list));
        list[0].index = 1UL;
        strcpy(list[0].name, "Sviluppo AmigaIta");
        list[0].is_user = 0;
        list[0].is_current = 1;
        list[0].peer_id_lo = 0x100UL;
        list[1].index = 2UL;
        strcpy(list[1].name, "Mario Rossi");
        list[1].is_user = 1;
        list[1].unread = 2UL;
        list[1].peer_id_lo = 0x200UL;
        list[2].index = 3UL;
        strcpy(list[2].name, "Anna");
        list[2].is_user = 1;
        list[2].peer_id_lo = 0x300UL;
        list[3].index = 4UL;
        strcpy(list[3].name, "carla");
        list[3].name_is_username = 1;
        list[3].is_user = 1;
        list[3].peer_id_lo = 0x400UL;

        driver.on_chat_list_changed(driver.ctx, list, 4);

        if (state.chat_count != 4) {
            printf("gui driver self-test: chat_count %d != 4\n",
                   state.chat_count);
            return 2;
        }
        if (strcmp(state.chats[0].name, "Sviluppo AmigaIta") != 0 ||
            strcmp(state.chats[0].initials, "SA") != 0 ||
            state.selected_chat != 0) {
            puts("gui driver self-test: chat[0] (group/current) wrong");
            return 2;
        }
        if (state.chats[1].unread != 2 ||
            strcmp(state.chats[1].initials, "MR") != 0) {
            puts("gui driver self-test: chat[1] (unread/initials) wrong");
            return 2;
        }
        if (strcmp(state.chats[2].initials, "A") != 0) {
            puts("gui driver self-test: chat[2] single-word initials wrong");
            return 2;
        }
        if (strcmp(state.chats[3].name, "@carla") != 0 ||
            strcmp(state.chats[3].initials, "C") != 0) {
            puts("gui driver self-test: chat[3] @username projection wrong");
            return 2;
        }
        if (state.chats[0].avatar_color < 0 ||
            state.chats[0].avatar_color >= TG_GUI_AVATAR_COLORS) {
            puts("gui driver self-test: chat avatar colour out of range");
            return 2;
        }

        /* Notifications bump the matching row's badge and flash it (unless it
           is the open chat); unknown peers are ignored. */
        {
            tg_chat_notify_entry note;

            memset(&note, 0, sizeof(note));
            note.peer_id_lo = 0x200UL; /* Mario Rossi, not selected */
            driver.on_notification(driver.ctx, &note);
            if (state.chats[1].unread != 3 || state.chats[1].flash != 1) {
                puts("gui driver self-test: notification did not flash row");
                return 2;
            }
            note.peer_id_lo = 0x100UL; /* the selected/open chat: no bump, no flash */
            driver.on_notification(driver.ctx, &note);
            if (state.chats[0].unread != 0 || state.chats[0].flash != 0) {
                puts("gui driver self-test: open chat must not bump unread/flash");
                return 2;
            }
            note.peer_id_lo = 0x999UL; /* not in the sidebar: ignored */
            driver.on_notification(driver.ctx, &note);
            if (state.chats[2].unread != 0 || state.chats[2].flash != 0 ||
                state.chats[3].unread != 0 || state.chats[3].flash != 0) {
                puts("gui driver self-test: unknown peer must not touch rows");
                return 2;
            }
        }
    }

    /* Read receipts: own messages flip "sent" -> "seen" as the peer's read
       cursor advances (monotonically); incoming and unsent rows carry no mark. */
    {
        tg_gui_state rs;
        tg_gui_chat_driver rg;
        tg_chat_driver rd;
        tg_chat_message_row row;

        memset(&rs, 0, sizeof(rs));
        tg_gui_chat_driver_bind(&rg, &rs, &rd);

        memset(&row, 0, sizeof(row));
        row.has_time = 1;
        row.local_epoch = 1700000000UL;
        row.own_label = "Io";
        row.is_out = 1;
        row.id = 100UL;
        row.text = "primo";
        rd.on_message(rd.ctx, &row);
        row.id = 200UL;
        row.text = "secondo";
        rd.on_message(rd.ctx, &row);
        row.is_out = 0;
        row.id = 150UL;
        row.sender = "Mario";
        row.text = "in arrivo";
        rd.on_message(rd.ctx, &row);

        if (rs.messages[0].read_state != TG_GUI_READ_SENT ||
            rs.messages[1].read_state != TG_GUI_READ_SENT ||
            rs.messages[2].read_state != TG_GUI_READ_NONE) {
            puts("gui driver self-test: initial read_state wrong");
            return 2;
        }
        /* Peer reads up to #100: only own #100 becomes "seen". */
        if (tg_gui_driver_set_read_outbox_max(&rg, 100UL) == 0 ||
            rs.messages[0].read_state != TG_GUI_READ_SEEN ||
            rs.messages[1].read_state != TG_GUI_READ_SENT ||
            rs.messages[2].read_state != TG_GUI_READ_NONE) {
            puts("gui driver self-test: read cursor 100 wrong");
            return 2;
        }
        /* The cursor only advances: a stale lower value changes nothing. */
        if (tg_gui_driver_set_read_outbox_max(&rg, 50UL) != 0) {
            puts("gui driver self-test: read cursor must not regress");
            return 2;
        }
        /* Peer reads up to #250: own #200 also becomes "seen". */
        if (tg_gui_driver_set_read_outbox_max(&rg, 250UL) == 0 ||
            rs.messages[1].read_state != TG_GUI_READ_SEEN) {
            puts("gui driver self-test: read cursor 250 wrong");
            return 2;
        }
        /* Re-applying the same cursor reports no change (no spurious repaint). */
        if (tg_gui_driver_set_read_outbox_max(&rg, 250UL) != 0) {
            puts("gui driver self-test: idempotent cursor must report no change");
            return 2;
        }
        /* Switching chats resets the cursor so a smaller value can take hold. */
        tg_gui_driver_reset_read_outbox(&rg);
        if (rs.open_read_outbox_max != 0UL) {
            puts("gui driver self-test: reset must clear the read cursor");
            return 2;
        }
        /* The optimistic echo now carries the server id returned by
           messages.sendMessage, so it starts "sent" with a real id. */
        tg_gui_driver_append_own(&rg, "eco", "Io", 0, 400UL);
        if (rs.messages[rs.message_count - 1].read_state != TG_GUI_READ_SENT ||
            rs.messages[rs.message_count - 1].id != 400UL) {
            puts("gui driver self-test: optimistic echo state wrong");
            return 2;
        }
        if (!tg_gui_driver_has_unseen_own(&rg)) {
            puts("gui driver self-test: echo must count as unseen-own");
            return 2;
        }
        /* Peer reads up to the echo's id -> it promotes to "seen" IN PLACE (the
           0.0.4 real-time double-check; previously id==0 blocked this). */
        if (tg_gui_driver_set_read_outbox_max(&rg, 400UL) == 0 ||
            rs.messages[rs.message_count - 1].read_state != TG_GUI_READ_SEEN) {
            puts("gui driver self-test: echo must promote to seen via sent_id");
            return 2;
        }
        if (tg_gui_driver_has_unseen_own(&rg)) {
            puts("gui driver self-test: no unseen-own after read");
            return 2;
        }
    }

    /* Multi-device dedup/reconcile: with include_outgoing the open-chat poll
       re-delivers our own messages; the driver must not double them. */
    {
        tg_gui_state ds;
        tg_gui_chat_driver dg;
        tg_chat_driver dd;
        tg_chat_message_row row;

        memset(&ds, 0, sizeof(ds));
        tg_gui_chat_driver_bind(&dg, &ds, &dd);

        /* Echo of a 1:1 send (server id known) + a re-fetch of it via the poll. */
        tg_gui_driver_append_own(&dg, "ciao", "Io", 0, 500UL);
        memset(&row, 0, sizeof(row));
        row.own_label = "Io";
        row.is_out = 1;
        row.id = 500UL;
        row.text = "ciao";
        dd.on_message(dd.ctx, &row); /* same id -> deduped */
        if (ds.message_count != 1) {
            puts("gui driver self-test: re-fetched own echo must dedup by id");
            return 2;
        }
        /* A message sent from ANOTHER session (unknown id) -> appended. */
        row.id = 600UL;
        row.text = "da altro device";
        dd.on_message(dd.ctx, &row);
        if (ds.message_count != 2 || ds.messages[1].id != 600UL) {
            puts("gui driver self-test: other-session outgoing must append");
            return 2;
        }
        /* Group-send fallback: echo with no server id yet (sent_id 0), then the
           poll brings the real id -> reconcile onto the echo, no duplicate. */
        tg_gui_driver_append_own(&dg, "gruppo", "Io", 0, 0UL);
        if (ds.message_count != 3 || ds.messages[2].id != 0UL) {
            puts("gui driver self-test: id-less echo setup wrong");
            return 2;
        }
        row.id = 700UL;
        row.text = "gruppo";
        dd.on_message(dd.ctx, &row);
        if (ds.message_count != 3 || ds.messages[2].id != 700UL) {
            puts("gui driver self-test: id-less echo must reconcile by text");
            return 2;
        }
        /* Edit: update a shown message's text by id (ds has ids 500,600,700).
           Transcript mutations must invalidate any drag-selection snapshot. */
        {
            unsigned long gen;

            gen = ds.msg_gen;
            if (tg_gui_driver_update_text(&dg, 600UL, "modificato") != 1 ||
                strcmp(ds.messages[1].text, "modificato") != 0 ||
                ds.msg_gen != gen + 1UL ||
                tg_gui_driver_update_text(&dg, 999UL, "x") != 0) {
                puts("gui driver self-test: edit update_text wrong");
                return 2;
            }
            gen = ds.msg_gen;
            if (tg_gui_driver_update_text_utf8(
                    &dg, 600UL, "pi\xc3\xb9 recente") != 1 ||
                strcmp(ds.messages[1].text, "pi\xf9 recente") != 0 ||
                ds.msg_gen != gen + 1UL) {
                puts("gui driver self-test: UTF-8 edit conversion wrong");
                return 2;
            }
        }
        /* Delete: drop by id, shifting the tail up. */
        {
            unsigned long gen;

            gen = ds.msg_gen;
            if (tg_gui_driver_remove_by_id(&dg, 500UL) != 1 ||
                ds.message_count != 2 || ds.messages[0].id != 600UL ||
                ds.msg_gen != gen + 1UL ||
                tg_gui_driver_remove_by_id(&dg, 999UL) != 0) {
                puts("gui driver self-test: delete remove_by_id wrong");
                return 2;
            }
        }
    }

    puts("gui driver self-test: ok (messages + chat list + receipts)");
    return 0;
}
#endif /* !TG_NO_SELFTEST */
