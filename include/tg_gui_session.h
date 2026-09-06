/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Live GUI session bridge (Phase 5b, slice 3). A thin, non-interactive driver
 * over the same MTProto chat core the console uses: it holds an authenticated
 * connection for the lifetime of the GUI window and exposes a non-blocking
 * "tick" the window event loop pumps to harvest cross-chat notifications into
 * the sidebar badges. The implementation lives at the bottom of
 * core/tg_mtproto_probe.c so it reaches that file's static network helpers
 * directly -- no de-static of the network core. See docs/GUI_ARCHITECTURE.md.
 *
 * Single session per process (one window): the state is a file-static
 * singleton; these calls are not re-entrant.
 */

#ifndef TG_GUI_SESSION_H
#define TG_GUI_SESSION_H

#include <stdio.h>

#include "tg_gui.h" /* tg_gui_state */

/* Opens a live session over a saved authorization: derives the production
   endpoint from the auth file's DC, loads the api id, holds an authenticated
   connection, binds the GUI driver to `state`, and projects the current peer
   cache into the sidebar. The caller may refresh the cache from the network
   first (tg_mtproto_gui_refresh_peer_cache). Returns 0 on success, non-zero
   when the session could not be opened (the window can still run read-only).
   `state` must outlive the session. */
int tg_gui_session_open(const char *api_file, const char *auth_file,
                        const char *peer_cache_file, tg_gui_state *state,
                        FILE *stream);

/* One non-blocking poll cycle: a cadence-gated getDifference drain harvests
   inbound messages into the notify queue, which is then dispatched to the GUI
   driver (bumping + flashing the matching sidebar badge). Returns 1 when the
   GUI state changed (the caller should repaint), 0 otherwise. Safe to call
   when no session is open (returns 0). */
int tg_gui_session_tick(FILE *stream);

/* Receive-only live drain for the composer path. It never starts an RPC: when
   the held socket already has an encrypted frame queued, consumes at most one,
   ACKs it and applies typing/read-receipt/edit/notification pushes. Returns 1
   when GUI state changed. Safe to call frequently and with no open session. */
int tg_gui_session_receive_pending(FILE *stream);

/* Move at most one bounded inline-photo cache download step. Manual file
   transfers have priority and make this a no-op. Returns 1 only when a cached
   photo became drawable and the GUI should repaint. */
int tg_gui_session_photo_step(FILE *stream);
/* True while an inline/viewer JPEG is queued or a fetch owns the file channel.
   Used only by the native scheduler to avoid counting idle ticks as stalls. */
int tg_gui_session_photo_pending(void);

/* Inline-photo policy and demand queue. The renderer calls request_inline only
   for a visible photo; 2 means the complete JPEG is cached, 1 means only its
   stripped preview is cached, and 0 means work is queued or disabled. */
void tg_gui_session_set_inline_photos(int enabled);
/* Pause automatic photo cache traffic while the native cache manager removes
   files. Existing message metadata remains so visible photos can be requeued
   when finish() resumes the pipeline. */
void tg_gui_session_photo_cache_clear_prepare(void);
void tg_gui_session_photo_cache_clear_finish(void);
int tg_gui_session_request_inline_photo(unsigned long photo_id_hi,
                                        unsigned long photo_id_lo);

/* Queue the larger representation for the reusable photo viewer. Returns 0
   when metadata exists (cached or queued), non-zero when the photo is unknown.
   The selected source dimensions are returned for the fixed viewer geometry. */
int tg_gui_session_request_viewer_photo(unsigned long photo_id_hi,
                                        unsigned long photo_id_lo,
                                        unsigned long *source_w,
                                        unsigned long *source_h);

/* Ensure an original cached JPEG exists even when a decoded canonical cache
   already satisfies normal painting. Used by Save photo as...: 2 = cached,
   1 = queued/active, 0 = no metadata. Progress also reports queued work. */
int tg_gui_session_request_photo_jpeg(unsigned long photo_id_hi,
                                      unsigned long photo_id_lo, int large);
int tg_gui_session_photo_fetch_progress(unsigned long photo_id_hi,
                                        unsigned long photo_id_lo, int large,
                                        unsigned long *done,
                                        unsigned long *total);

/* Stable on-disk cache name shared by the session and native window backend.
   `large` selects photos/tgph<id>-l.jpg instead of the inline JPEG. */
int tg_gui_session_photo_cache_path(char *path, unsigned long path_size,
                                    unsigned long photo_id_hi,
                                    unsigned long photo_id_lo, int large);
/* Expanded photoStrippedSize JPEG used as the no-network first frame. */
int tg_gui_session_photo_thumb_cache_path(char *path,
                                          unsigned long path_size,
                                          unsigned long photo_id_hi,
                                          unsigned long photo_id_lo);
/* Versioned RGB888 canonical cache; `large` selects the viewer variant. */
int tg_gui_session_photo_canonical_cache_path(
    char *path, unsigned long path_size,
    unsigned long photo_id_hi, unsigned long photo_id_lo, int large);

/* Discard a cached JPEG that the renderer proved undecodable. This is the only
   permanent per-session photo rejection: transport failures remain retryable. */
void tg_gui_session_photo_decode_failed(unsigned long photo_id_hi,
                                        unsigned long photo_id_lo);
void tg_gui_session_photo_decode_failed_variant(unsigned long photo_id_hi,
                                                unsigned long photo_id_lo,
                                                int large);

/* Upload progress + CANCEL hook: `completed`/`total` are parts; percentage is
   completed*100/total. Runs on the calling task after each confirmed part.
   Return non-zero to abort the upload (call returns 5); the parts already sent
   are left orphaned and Telegram expires them, so no local cleanup is needed. */
typedef int (*tg_gui_upload_progress_fn)(unsigned long completed_parts,
                                         unsigned long total_parts,
                                         void *user_data);

/* Download progress + CANCEL hook: called after each received chunk with
   (bytes so far, total). Return non-zero to abort the transfer -- the partial
   file is then removed and the call returns 5. Distinct from the void upload
   hook because a big download blocks the event loop, so it must be cancellable
   (a close-gadget click) or the user has to reset the machine. */
typedef int (*tg_gui_download_progress_fn)(unsigned long done_bytes,
                                           unsigned long total_bytes,
                                           void *user_data);

/* F9: download the document attached to message msg_id in the open chat into
   downloads/<name>. 0 ok (path in out_path), 1 fail, 2 foreign DC (unsupported
   yet), 3 disk error, 5 cancelled. Blocking on-context call -- never from the
   tick. `progress` is optional and runs after each received chunk. */
int tg_gui_session_download_document(unsigned long msg_id, char *out_path,
                                     unsigned long out_path_size, FILE *stream,
                                     tg_gui_download_progress_fn progress,
                                     void *progress_data);

/* Largest file this build can upload, in MiB (chunk size x 4000 parts). Ask
   instead of hardcoding: the value moves with the per-platform chunk. */
unsigned long tg_gui_session_upload_limit_mib(void);

/* One-line reason the last transfer failed (e.g. "part 42/240: no data for
   45s"), for the status bar. Empty string when none. Borrowed, do not free. */
const char *tg_gui_session_last_transfer_error(void);

/* F9: send the file at `path` to the open chat. Files over 10 MB use
   upload.saveBigFilePart/inputFileBig. Telegram's 4000-part bound sets the
   ceiling; it follows the per-platform chunk, so ask
   tg_gui_session_upload_limit_mib() rather than quoting a number here (this
   comment used to say 31/250 MiB and went stale the moment a chunk changed).
   0 ok, 1 fail, 2 too big for this build, 3 unreadable. Blocking; never from
   the tick. `progress` is optional and runs after each confirmed part. */

int tg_gui_session_send_document(const char *path, FILE *stream,
                                 tg_gui_upload_progress_fn progress,
                                 void *progress_data);

/* Send a JPEG as an uploaded Telegram photo. Files above 10 MiB fall back to
   document mode; malformed JPEGs return 7 without uploading any part. */
int tg_gui_session_send_photo(const char *path, FILE *stream,
                              tg_gui_upload_progress_fn progress,
                              void *progress_data);

/* --- Non-blocking transfers (0.0.8): the GUI pumps, the window stays alive.
   start_* arms the transfer on the file channel and returns immediately:
   0 = armed, else the same final rc codes as the blocking calls (reason via
   tg_gui_session_last_transfer_error). Then call transfer_step() once per
   event-loop turn: it moves ONE chunk/part (one bounded RPC) and returns 1
   while running, 0 once finished -- at that point call transfer_end(), which
   returns the final rc (download: saved path or reason in out_path).
   transfer_cancel() marks the transfer aborted; the NEXT step/end unwinds it
   (download: partial file removed; upload: parts left to expire, rc 6/5).
   One transfer at a time: busy() tells which direction is active (0 idle,
   1 download, 2 upload). The TUI keeps using the blocking calls above --
   both run the same engine underneath. */
int tg_gui_session_transfer_busy(void);
int tg_gui_session_transfer_start_download(unsigned long msg_id,
                                           FILE *stream);
/* caption: text in the platform charset (Latin-1 on Amiga), NULL or empty
   for none; converted to UTF-8 inside. It rides the sendMedia of the photo,
   of a document, and of the over-10-MiB photo fallback alike. */
int tg_gui_session_transfer_start_upload(const char *path,
                                         const char *caption, FILE *stream);
int tg_gui_session_transfer_start_photo(const char *path,
                                        const char *caption, FILE *stream);
/* Describe the current/last upload choice, including a failed start. */
int tg_gui_session_transfer_requested_photo(void);
int tg_gui_session_transfer_photo_fallback(void);
int tg_gui_session_transfer_step(unsigned long *done, unsigned long *total);
/* Bytes moved so far by the running transfer (0 when idle), for a rate
   display: downloads count real bytes, uploads count confirmed parts. */
unsigned long tg_gui_session_transfer_bytes(void);

/* Directory downloads are written to: "downloads" unless
   data/telegram-downloads.txt says otherwise (one line, e.g. "RAM:TGdl").
   Borrowed, do not free; stable for the whole run. */
const char *tg_gui_session_download_dir(void);

/* Change it (menu "Download drawer..."): applies at once and is written to
   data/telegram-downloads.txt for the next run. 0 ok, 1 rejected (empty or
   too long), 2 in force but could not be saved. */
int tg_gui_session_set_download_dir(const char *dir);
void tg_gui_session_transfer_cancel(void);
int tg_gui_session_transfer_end(char *out_path, unsigned long out_path_size);

/* F10 Saved Messages: the sidebar row index that opens the self chat (cloud
   archive) is TG_GUI_SAVED_PEER_INDEX, defined in tg_gui.h (the UI layer needs
   it too). Pinned as the LAST row; remove/reorder skip it. */

/* Opens the chat at the given 1-based peer-cache index: clears the transcript,
   fetches its recent history (incoming + outgoing) into tg_gui_state.messages
   through the GUI driver, and marks it the open chat so tg_gui_session_tick
   streams its new messages live. Returns 1 (always repaint) when a session is
   open, 0 otherwise. */
int tg_gui_session_open_chat(unsigned long peer_index, FILE *stream);

/* Pages OLDER history at the top of the open chat: fetches the getHistory page
   just below the oldest message currently shown and PREPENDS it to the transcript.
   allow_drop_newest = 1 lets a full ring evict its newest tail to make room (only
   safe when those rows are off-screen); 0 keeps them (paging then stops at the
   ring's capacity). Tri-state return: > 0 = older messages added; 0 = server
   confirmed the chat start (no older); < 0 = could not page now (fetch failed /
   nothing pageable) -- the caller must NOT treat < 0 as the chat start. */
int tg_gui_session_load_older(FILE *stream, int allow_drop_newest);

/* Sends `text` to the open chat and echoes it into the transcript. When
   reply_to_msg_id != 0 the message is sent as a reply to that message id.
   Returns 0 on success, non-zero on failure or when no chat is open. */
/* Where Telegram put the login code, for the code screen to say out loud:
   it only sends an SMS when no other device is signed in, otherwise the code
   arrives as a message inside Telegram, and a user watching an empty inbox
   concludes this client is broken. Short enough for the status line. Valid
   after a successful send_code. */
const char *tg_mtproto_sent_code_hint(void);

/* Digits Telegram says the code has, 0 when it did not say. */
unsigned long tg_mtproto_sent_code_length(void);

int tg_gui_session_send(const char *text, unsigned long reply_to_msg_id,
                        FILE *stream);

/* Forwards one server-backed message from the open chat to a destination peer.
   TG_GUI_SAVED_PEER_INDEX selects Saved Messages. */
int tg_gui_session_forward(unsigned long message_id,
                           unsigned long destination_peer_index,
                           FILE *stream);

/* Public index of the peer held open by the live session, including the
   TG_GUI_SAVED_PEER_INDEX sentinel for Saved Messages. Zero means none. */
unsigned long tg_gui_session_current_peer_index(void);

/* One-line reason for the last interactive action failure (RPC name or
   transport reason). Empty after a successful query. Borrowed, do not free. */
const char *tg_gui_session_last_action_error(void);

/* Edits an own message (messages.editMessage) to `text`, and on success updates
   the on-screen bubble in place. Returns 0 ok, non-zero on failure / no chat. */
int tg_gui_session_edit(const char *text, unsigned long message_id,
                        FILE *stream);

/* Deletes a message for everyone (messages./channels.deleteMessages by peer
   type), and on success removes it from the transcript. Returns 0 ok. */
int tg_gui_session_delete(unsigned long message_id, FILE *stream);

/* '@' mention autocomplete: fills up to `max` candidate usernames (no leading
   '@', NUL-terminated, `item_size` bytes apart in `items`) for the open GROUP,
   matching `prefix` case-insensitively against username or title (empty prefix
   = first members). Triggers the same one-shot member fetch the group typing
   names use (supergroups; basic groups have no fetch yet -> 0). Returns the
   count; 0 when no group is open or nothing matches. */
int tg_gui_session_mention_candidates(const char *prefix, char *items,
                                      unsigned long item_size, int max,
                                      FILE *stream);

/* Searches Telegram for `query` (contacts.search), adds the first openable
   result to the peer cache and opens it. Small reply, MorphOS-safe. Returns 0 =
   opened, 1 = no result / network issue, 2 = bad args. */
int tg_gui_session_search_open(const char *query, FILE *stream);

/* Online search, two stages: first the account's OWN dialogs (paged like
   Reload into a throwaway cache; finds hidden chats and private groups that
   have no public username), then -- only on zero matches -- the global
   contacts.search. KEEPS the openable results (does not open or touch the
   real cache) so the window can show a picker. Returns the openable count
   (>= 0), -1 on failure. */
int tg_gui_session_search_run(const char *query, FILE *stream);
/* Count / display name of the last search's openable results (0-based). */
int tg_gui_session_search_count(void);
const char *tg_gui_session_search_name(int index);
/* Open the index-th openable result of the last search. 0 = opened. */
int tg_gui_session_search_open_result(int index, FILE *stream);
/* Add an online-picker result to the real cache without opening or unhiding it.
   Returns its public peer-cache index through peer_index. */
int tg_gui_session_search_cache_result(int index, unsigned long *peer_index,
                                       FILE *stream);

/* Rebuild the sidebar from the cached chat list (no network) -- restores the
   real list after cancelling the search picker. */
void tg_gui_session_refresh_chats(void);

/* Project all cached chats for the instant local filter. Hidden rows are
   included with a visible marker; the normal sidebar projection omits them. */
void tg_gui_session_show_filterable_chats(void);

/* Explicitly opening a cached row removes its hidden marker. Non-hidden rows
   are a no-op. Returns 0 on success. */
int tg_gui_session_unhide_chat(unsigned long peer_index, FILE *stream);

/* Hide the chat at `peer_index` from the sidebar while retaining it in the
   peer cache for instant local search. Returns 0 on success. */
int tg_gui_session_remove_chat(unsigned long peer_index, FILE *stream);

/* Menu "Reload chat list": re-page the dialog list from the server (additive,
   honours hidden chats, reprojects the sidebar). 0 ok, 2 no session,
   3 unsupported on this platform (MorphOS: getDialogs freeze). */
int tg_gui_session_reload_chat_list(FILE *stream);

/* Move the chat at peer-cache public index src_index to dst_index, persist the
   new order, reproject the sidebar, and keep the open chat selected. No network
   fetch. Returns 0 on success, non-zero otherwise. */
int tg_gui_session_reorder_chat(unsigned long src_index, unsigned long dst_index,
                                FILE *stream);

/* Persist the sidebar unread badges (cleared on open, incremented live) to the
   chat cache so they survive a restart. Writes only when a count changed. */
void tg_gui_session_persist_unread(void);

/* 1 while a live session is held (so the window can enable composing). */
int tg_gui_session_is_open(void);

/* Closes the held connection and unbinds the notification queue. */
void tg_gui_session_close(void);

/* --- First-login flow (no saved session) -------------------------------- *
 * When there is no telegram-auth.bin yet, the window drives a phone -> code
 * -> (optional) 2FA login through these calls, each a single blocking network
 * round-trip the window wraps with a "Connessione..." status. They reuse the
 * console wizard's headless backend (auth.sendCode / auth.signIn / SRP
 * checkPassword) and persist telegram-auth.bin on success. */

/* Result codes for the step calls below. */
#define TG_GUI_LOGIN_OK           0 /* step accepted; advance */
#define TG_GUI_LOGIN_NEED_2FA     1 /* code accepted, a 2FA password is required */
#define TG_GUI_LOGIN_BAD_CODE     2 /* the login code was rejected -- re-prompt */
#define TG_GUI_LOGIN_BAD_PASSWORD 3 /* the 2FA password was wrong -- re-prompt */
#define TG_GUI_LOGIN_BAD_PHONE    4 /* the number was rejected -- re-prompt */
#define TG_GUI_LOGIN_ERROR        5 /* network/other error -- retry */

/* Stores the file paths the login + the eventual session need. Call once before
   entering a login screen (paths must outlive the session). */
void tg_gui_session_login_begin(const char *api_file, const char *auth_file,
                                const char *peer_cache_file);

/* Requests the login code for `phone` (auth.sendCode, handling PHONE_MIGRATE by
   re-deriving the DC). On TG_GUI_LOGIN_OK the next step is the code. */
int tg_gui_session_login_send_code(const char *phone, FILE *stream);

/* Submits the login `code` (auth.signIn). TG_GUI_LOGIN_OK = logged in (call
   activate); TG_GUI_LOGIN_NEED_2FA = ask for the password; TG_GUI_LOGIN_BAD_CODE
   = re-prompt. */
int tg_gui_session_login_sign_in(const char *code, FILE *stream);

/* Submits the 2FA `password` (SRP auth.checkPassword). TG_GUI_LOGIN_OK = logged
   in (call activate); TG_GUI_LOGIN_BAD_PASSWORD = re-prompt. */
int tg_gui_session_login_check_password(const char *password, FILE *stream);

/* The real reason the last send_code/sign_in failed (the GUI has no console:
   stdout is NIL: on a Workbench launch). Empty string if none captured. */
const char *tg_gui_session_login_last_error(void);

/* After a successful login, brings the window live: refreshes the peer cache,
   opens the session into `state`, sets the title/status and opens the first
   chat, flipping `state->mode` to TG_GUI_MODE_CHAT. Returns 0 on success. */
int tg_gui_session_login_activate(tg_gui_state *state, FILE *stream);

/* Crash-safe diagnostic log to a disk file (tg-gui-debug.log in the CWD), to
   pin down where a hard crash happens. tg_gui_log_enable() turns it on
   (--gui-live-debug); tg_gui_log() writes one flushed line when enabled. */
void tg_gui_log_enable(void);
int tg_gui_log_is_enabled(void);
void tg_gui_log(const char *msg);

/* The text emoticon standing for sheet glyph `index` (":)", "<3", ...), or
   "?" when the table has none. Backends draw it where a glyph cell would be
   too small to read, and the clipboard gets it in place of a pair. */
const char *tg_gui_session_emoji_text(unsigned long index);

#endif
