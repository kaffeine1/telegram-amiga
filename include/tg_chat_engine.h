/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Driver-agnostic chat-session model (Phase 5b), being extracted incrementally
 * from core/tg_mtproto_probe.c with the console/TUI as the behavioural oracle.
 * The goal: one chat "engine" that both the console driver and the GUI driver
 * sit on top of, instead of the chat logic printing straight to a FILE*.
 *
 * This is the first slice. It owns only the updates cursor (pts/qts/date/seq)
 * and the /diff flag -- the two file-statics that previously lived in probe.c.
 * Later slices fold in the notify queue, the chat-list model, selection/cursor,
 * the tick()/submit_line() event API and the driver callback vtable. See
 * docs/GUI_ARCHITECTURE.md.
 */

#ifndef TG_CHAT_ENGINE_H
#define TG_CHAT_ENGINE_H

#include "tg_mtproto_login.h" /* tg_mtproto_updates_state */

/* Cross-chat notification model. Entries are collected from live pushes and the
   getDifference drain, deduped by a ring of recent message ids, and consumed by
   the renderer (console today, GUI later). */
#define TG_CHAT_NOTIFY_MAX 8U
#define TG_CHAT_NOTIFY_TEXT 96U
/* Dedupe ring: must absorb the overlap between live pushes and the
   reconciliation sweep on a busy hour, not just push repeats. */
#define TG_CHAT_NOTIFY_RECENT 64U

typedef struct tg_chat_notify_entry {
    int is_chat; /* 1 = group/channel (peer ids are the chat), 0 = DM user */
    unsigned long peer_constructor; /* distinguishes groups from channels */
    unsigned char silent;
    unsigned char is_out;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    /* The sender, for groups whose chat is not in the peer cache yet: a
       stale cache then still shows WHO wrote instead of a generic line. */
    unsigned long from_id_hi;
    unsigned long from_id_lo;
    char text[TG_CHAT_NOTIFY_TEXT];
} tg_chat_notify_entry;

typedef struct tg_chat_notify {
    tg_chat_notify_entry queue[TG_CHAT_NOTIFY_MAX];
    unsigned long count;
    unsigned long dropped;
    unsigned long recent_ids[TG_CHAT_NOTIFY_RECENT];
    unsigned long recent_pos;
    int armed; /* collection only happens while a chat session is open */
} tg_chat_notify;

typedef struct tg_chat_engine {
    tg_mtproto_updates_state updates_state; /* getDifference cursor */
    int diff_enabled;                       /* background catch-up (/diff) */
    tg_chat_notify notify;                  /* cross-chat notification queue */
} tg_chat_engine;

/* Notification queue ops (all NULL-safe so callers need no guards). */
/* Clears the queue + dedupe ring and sets the armed flag. */
void tg_chat_notify_reset(tg_chat_notify *notify, int armed);
/* Returns 1 if message_id was already seen (records it otherwise). id 0 is
   never deduped. A NULL queue returns 1 (treat as seen -> caller skips). */
int tg_chat_notify_seen(tg_chat_notify *notify, unsigned long message_id);
/* Claims the next free entry, incrementing count; returns NULL (and bumps
   dropped) when the queue is full or the pointer is NULL. The caller fills the
   returned entry's fields. */
tg_chat_notify_entry *tg_chat_notify_claim(tg_chat_notify *notify);

/* One resolved transcript row handed to a driver. The engine builds it from a
   parsed message (resolving the sender, the local-frame timestamp and the
   group/peer context); a driver renders it however it likes -- the console
   driver prints today's transcript line, the GUI driver will project it into a
   tg_gui_message. Decouples "what to show" (engine) from "how to show it". */
typedef struct tg_chat_message_row {
    unsigned long id;          /* server message id (0 if unknown) */
    const char *text;          /* message body */
    unsigned long local_epoch; /* local-frame epoch; used only when has_time */
    int has_time;              /* the message carried a date */
    int is_out;                /* sent by this account */
    int is_group;              /* group/channel: emit the [title] prefix */
    const char *peer_label;    /* chat title (group prefix + 1:1 sender fallback) */
    const char *own_label;     /* this account's name, for is_out */
    const char *sender;        /* resolved incoming sender, NULL if unknown */
    const char *reply_quote;   /* quoted snippet if this is a reply, else NULL */
    unsigned long from_id_hi;  /* sender user id, to match a live typing update */
    unsigned long from_id_lo;
    int has_document;          /* F9: message carries a downloadable document */
    int has_photo;             /* 0.0.9: message carries an inline photo */
    int photo_ready;           /* its selected JPEG is present in photos/ */
    int photo_only;            /* text is the synthetic "[Photo]" fallback */
    unsigned long photo_id_hi;
    unsigned long photo_id_lo;
    unsigned long photo_width;
    unsigned long photo_height;
    unsigned long pending_webpage_hi;
    unsigned long pending_webpage_lo;
    unsigned long webpage_channel_hi;
    unsigned long webpage_channel_lo;
    int is_service;            /* a service line ("X joined"): text says it all */
} tg_chat_message_row;

/* One resolved chat-list row. The engine parses the peer cache into these
   (resolving the display name + whether it is a @username, the user-vs-group
   kind for grouping, unread and the open-chat marker); a driver renders them --
   the console driver prints the grouped list, the GUI driver fills
   tg_gui_state.chats. */
/* Intermediate chat-list row capacity (cache file -> rows -> sidebar). Kept >=
   TG_GUI_MAX_CHATS so the projection never truncates below what the sidebar shows;
   m68k stays lower for RAM. */
#if defined(__m68k__)
#define TG_CHAT_LIST_MAX 64
#else
#define TG_CHAT_LIST_MAX 128
#endif
#define TG_CHAT_LIST_NAME_MAX 128

typedef struct tg_chat_list_row {
    unsigned long index;                /* 1-based public chat number */
    char name[TG_CHAT_LIST_NAME_MAX];   /* resolved display name; "" = none */
    int name_is_username;               /* prefix "@" when rendering the name */
    int is_user;                        /* user (single chat) vs group/channel */
    unsigned long unread;
    int is_current;                     /* the currently open chat */
    unsigned long peer_id_hi;           /* peer id, to match notifications to a row */
    unsigned long peer_id_lo;
} tg_chat_list_row;

/* The driver callback surface. on_message renders one transcript row;
   on_chat_list_changed hands the whole resolved chat list; on_notification
   hands one cross-chat notification entry (the GUI driver bumps the matching
   sidebar row's unread badge; the console renders its own notify lines). ctx is
   the driver's own state. A driver may leave a callback NULL when it does not
   use that surface; callers invoke only the one they set. */
typedef struct tg_chat_driver {
    void *ctx;
    void (*on_message)(void *ctx, const tg_chat_message_row *row);
    void (*on_chat_list_changed)(void *ctx, const tg_chat_list_row *rows,
                                 int count);
    void (*on_notification)(void *ctx, const tg_chat_notify_entry *entry);
} tg_chat_driver;

/* Resets the engine for a fresh chat session: zero cursor, catch-up enabled.
   Mirrors the previous per-session reset of the probe.c file-statics. */
void tg_chat_engine_init(tg_chat_engine *engine);

/* Portable invariant self-test (host-CI runnable). Returns 0 on success. */
int tg_chat_engine_self_test(void);

#endif
