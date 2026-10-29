/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * The GUI chat driver: the bridge that lets the chat engine feed the native
 * GUI. It implements tg_chat_driver.on_message (the seam from step 3) by
 * projecting each resolved tg_chat_message_row into a tg_gui_message appended
 * to a tg_gui_state -- instead of the console driver, which prints it. This is
 * what turns the demo GUI window into one that shows real conversation rows.
 *
 * Portable projection with local notification preferences, no network.
 * Host-testable. See tg_chat_engine.h
 * (tg_chat_driver / tg_chat_message_row) and tg_gui.h (tg_gui_state).
 */

#ifndef TG_GUI_DRIVER_H
#define TG_GUI_DRIVER_H

#include "tg_chat_engine.h"
#include "tg_gui.h"
#include "tg_mtproto_login.h"

typedef struct tg_gui_chat_driver {
    tg_gui_state *state; /* the GUI model this driver appends messages to */
    /* Insert position for the load-older paging path: -1 (default) appends the
       row at the end (newest); >= 0 inserts it at that index and advances, so an
       oldest-first batch lands in order ABOVE the existing transcript. */
    int prepend_at;
    /* When prepending into a FULL ring: 1 drops the newest tail to make room
       (safe only when the newest is off-screen, i.e. a scrollbar scrolled to the
       top); 0 refuses the insert so on-screen newest rows are never evicted (the
       fits-the-window case) -- paging then simply stops at the ring's capacity. */
    int prepend_allow_drop;
} tg_gui_chat_driver;

/* Binds chat_driver (the engine-facing vtable) so its on_message appends to
   `state` through `gui`. After this, hand chat_driver to the engine; every row
   it emits lands in state->messages (newest last, oldest dropped past the
   ring's capacity). */
void tg_gui_chat_driver_bind(tg_gui_chat_driver *gui, tg_gui_state *state,
                             tg_chat_driver *chat_driver);

/* Maps a sender name to a stable avatar tint index [0, TG_GUI_AVATAR_COLORS).
   Exposed so the chat-list projection can colour the same name identically. */
int tg_gui_driver_color_for(const char *name);

/* Appends a just-sent outgoing message to the transcript optimistically (no
   network), so it shows at once even when a confirm-poll would be slow to come
   back (notably MorphOS). `text` is already Latin-1 (typed) and copied
   verbatim; `own_label` is the sender label. */
void tg_gui_driver_append_own(tg_gui_chat_driver *gui, const char *text,
                              const char *own_label, const char *reply_snippet,
                              unsigned long sent_id);

/* The peer has read our outgoing messages up to read_outbox_max: advance the
   open-chat read cursor (monotonic) and promote shown own messages at or below
   it from "sent" to "seen". Returns 1 if any mark changed (repaint needed).
   Called by the live session after a getPeerDialogs refresh. */
int tg_gui_driver_set_read_outbox_max(tg_gui_chat_driver *gui,
                                      unsigned long read_outbox_max);

/* Reset the read cursor when switching chats (so it can drop to the new chat's
   value rather than being pinned by the monotonic advance above). */
void tg_gui_driver_reset_read_outbox(tg_gui_chat_driver *gui);

/* 1 if any shown own message is still "sent" (awaiting the peer's read receipt).
   The live poll uses this to tighten the read-cursor refresh cadence while a
   double-check is pending, so it updates near real-time instead of every Nth
   tick, then relaxes once everything is read. */
int tg_gui_driver_has_unseen_own(const tg_gui_chat_driver *gui);

/* Edit: replace the text of the shown message with this server id (Latin-1
   verbatim). Returns 1 if found (a repaint is needed). */
int tg_gui_driver_update_text(tg_gui_chat_driver *gui, unsigned long message_id,
                              const char *text);

/* Server updates carry UTF-8. Convert it to the GUI's Latin-1 display encoding,
   then replace the shown message text by id. */
int tg_gui_driver_update_text_utf8(tg_gui_chat_driver *gui,
                                   unsigned long message_id,
                                   const char *text);

int tg_gui_driver_update_reactions(tg_gui_chat_driver *gui,
                                    unsigned long message_id,
                                    const tg_reactions *reactions);

/* The messages whose reactions the periodic poll asks for, as Telegram's
   reactions page wants: shown, not the user's own, already carrying
   reactions. Newest first from the bottom of the view (rows scrolled out
   below it are skipped), at most `max`. Returns how many went into ids.
   A message whose reactions history could not read is asked for too,
   own or not, until one answer came for it. */
unsigned int tg_gui_driver_reaction_poll_ids(const tg_gui_chat_driver *gui,
                                             unsigned long *ids,
                                             unsigned int max);

/* The poll answered for these ids: their reactions are known now, even
   when Telegram had none to report. */
void tg_gui_driver_reactions_asked(tg_gui_chat_driver *gui,
                                   const unsigned long *ids, unsigned int n);

/* Attach send/history metadata, then complete every pending message with the
   matching 64-bit webpage id. Text uses the edit path; photos remain lazy. */
void tg_gui_driver_set_pending_webpage(tg_gui_chat_driver *gui,
                                       unsigned long message_id,
                                       unsigned long id_hi,
                                       unsigned long id_lo);
int tg_gui_driver_apply_webpage(tg_gui_chat_driver *gui,
                                const tg_mtproto_web_page *page,
                                int photo_ready);

/* Delete: drop the shown message with this server id, shifting the tail up.
   Returns 1 if removed (a repaint is needed). */
int tg_gui_driver_remove_by_id(tg_gui_chat_driver *gui,
                               unsigned long message_id);

/* A background photo download completed: mark every shown message carrying
   that Telegram photo id ready. Returns 1 when the model changed. */
int tg_gui_driver_mark_photo_ready(tg_gui_chat_driver *gui,
                                   unsigned long photo_id_hi,
                                   unsigned long photo_id_lo);

/* Host-CI self-test: feeds synthetic rows through the driver and asserts the
   resulting tg_gui_state (sender, text, time, is_own, colour, ring overflow).
   Returns 0 on success. */
int tg_gui_driver_self_test(void);

#endif
