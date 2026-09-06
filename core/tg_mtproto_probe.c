/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#if TG_ENABLE_GZIP || TG_ENABLE_GZIP_PUFF
#include <limits.h>
#endif

#if TG_ENABLE_GZIP
#include <zlib.h>
#endif

#if TG_ENABLE_GZIP_PUFF
#include "puff.h"
#endif

#include "tg_mtproto_auth.h"
#include "tg_mtproto_dc.h"
#include "tg_mtproto_bigint.h"
#include "tg_mtproto_encrypted.h"
#include "tg_mtproto_envelope.h"
#include "tg_mtproto_login.h"
#include "tg_chat_engine.h"
#include "tg_mtproto_message_id.h"
#include "tg_mtproto_probe.h"
#include "tg_emoji_sheet.h"
#include "tg_mtproto_rsa.h"
#include "tg_mtproto_session.h"
#include "tg_mtproto_srp.h"
#include "tg_mtproto_transport.h"
#include "tg_console.h"
#include "tg_console_ui.h"
#include "tg_console_tui.h"
#include "tg_avatar.h"
#include "tg_file.h"
#include "tg_gui.h"
#include "tg_gui_driver.h"
#include "tg_gui_session.h"

#if defined(TG_NO_GUI)
/* TUI-only build (plain-68000 package): the window, the renderer, the chat
   driver and the JPEG decoder are not linked in, so the handful of calls
   this file makes into them become inert. Everything they drive -- inline
   photos, avatars, the sidebar model -- is GUI-only anyway. */
#define tg_gui_window_avatar_invalidate(id_hi, id_lo) ((void)0)
#define tg_gui_window_photo_cache_file_changed(path) ((void)0)
#define tg_gui_window_photo_cache_file_removed(path) ((void)0)
#define tg_avatar_expand_stripped(src, len, out, cap, out_len) (1)
#define tg_gui_chat_driver_bind(gui, state, chat_driver) ((void)0)
#define tg_gui_driver_append_own(gui, text, own, snippet, sent_id) ((void)0)
#define tg_gui_driver_set_read_outbox_max(gui, read_max) (0)
#define tg_gui_driver_reset_read_outbox(gui) ((void)0)
#define tg_gui_driver_has_unseen_own(gui) (0)
#define tg_gui_driver_update_text(gui, message_id, text) (0)
#define tg_gui_driver_update_text_utf8(gui, message_id, text) (0)
#define tg_gui_driver_remove_by_id(gui, message_id) (0)
#define tg_gui_driver_mark_photo_ready(gui, id_hi, id_lo) (0)
#endif
#include "tg_net.h"
#include "tg_platform.h"

#define TG_MTPROTO_RPC_RESULT_CONSTRUCTOR 0xf35c6d01UL
#define TG_MTPROTO_RPC_ERROR_CONSTRUCTOR 0x2144ca19UL
#define TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR 0x73f1f8dcUL
/*
 * tg_mtproto_send_encrypted_query_limited soft-failure code: no matching
 * rpc_result within the bound (or bad_msg retries exhausted). The TCP
 * connection may still be alive, but a late rpc_result can poison the next
 * query on a persistent chat context, so context callers close and reopen after
 * a soft failure.
 */
#define TG_MTPROTO_QUERY_SOFT_FAIL 3
/*
 * Total wall-clock budget for one encrypted query's receive phase. On a heavy
 * account the persistent connection carries a dense update stream and the
 * matching rpc_result is interleaved far back; a plain count bound cuts off too
 * early. We keep reading/ACKing past updates until the result OR this budget
 * elapses, with the per-call attempt count as a high safety ceiling.
 */
/* MorphOS bsdsocket streams large replies at ~1KB/s on the field machine:
   a 20s budget made nearly every chat poll soft-fail into a reconnect
   storm. Give slow links room to finish instead. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
#define TG_MTPROTO_QUERY_BUDGET_SECONDS 45UL
#else
#define TG_MTPROTO_QUERY_BUDGET_SECONDS 20UL
#endif

/* Why the last encrypted query gave up (budget vs transport). The reason is
   otherwise printed only to the QUIET stream, so a GUI download could only say
   a bare "no reply" -- useless when diagnosing a slow-link failure. */
static char tg_mtproto_query_fail[64];
/* Defined by the upload engine further down; the text client and the GUI
   session accessor both put a server refusal into words. */
static const char *tg_mtproto_upload_failure_text(const char *raw);
#if TG_MTPROTO_DISPLAY_LATIN1
static int tg_mtproto_latin1_to_utf8(const char *src, char *dst,
                                     unsigned long dst_size);
#endif

/*
 * Consecutive failed reads/sends in the interactive chat loop before it drops
 * and reopens the connection. Recovers a wedged long-running session (stale
 * salt/seqno, or a silently dropped TCP link) instead of polling a dead session
 * forever.
 */
#define TG_MTPROTO_CHAT_STALL_LIMIT 3UL
/* Seconds of keyboard quiet before a parked draft lets background polls
   resume (they stay fully suspended while keys are actually flowing). */
#define TG_MTPROTO_CHAT_DRAFT_QUIET_SECONDS 5UL
#define TG_MTPROTO_CHAT_OPEN_HISTORY_ATTEMPTS 3U
/* First login on OS3 can be slow, but a blocking recv() must not leave the
   user staring at progress dots for minutes. This is deliberately wider than
   the failed 5s experiment and only wraps the phone-code request. */
#define TG_MTPROTO_LOGIN_NETWORK_TIMEOUT_SECONDS 20UL
#define TG_MTPROTO_LOGIN_QUERY_BUDGET_SECONDS 45UL

/*
 * auth.signIn outcome codes so the login wizard can tell apart the cases that
 * otherwise all collapse to a generic non-zero: a correct code on a 2FA account
 * (server asks for the password) versus a rejected code (user should retry the
 * code, NOT jump into the 2FA flow -- doing so previously led to a confusing
 * "auth-key-unregistered" on checkPassword). Naive callers still see non-zero.
 */
#define TG_MTPROTO_SIGN_IN_PASSWORD_NEEDED 4
#define TG_MTPROTO_SIGN_IN_CODE_INVALID 5
/*
 * auth.checkPassword outcome: the 2FA password was wrong (PASSWORD_HASH_INVALID).
 * The login wizard re-prompts for the password instead of aborting the whole
 * login. Naive callers still see this as a plain non-zero failure.
 */
#define TG_MTPROTO_CHECK_PASSWORD_INVALID 6

/*
 * Raw chat input gives Up/Down command-history recall. It requires every
 * interactive prompt -- the chat picker, Search, Remove and /add selection, not
 * just the main input loop -- to echo and line-edit in raw mode, otherwise those
 * prompts look silent (typing produces nothing) and the keyboard appears dead.
 *
 * That is now wired up: tg_mtproto_chat_prompt_line drives the same raw-aware
 * editor as the main loop (via the tg_chat_input_raw flag), and the editor fully
 * consumes any unrecognised CSI sequence (Amiga CON: window close/resize events
 * arrive as ESC '[' ... or single-byte CSI 0x9B ...) so they never leak into the
 * typed line. So raw mode is on by default; it falls back to cooked input
 * automatically if tg_platform_stdin_set_raw is unsupported on the target.
 */
#ifndef TG_ENABLE_CHAT_RAW_INPUT
#define TG_ENABLE_CHAT_RAW_INPUT 1
#endif
#define TG_MTPROTO_GZIP_PACKED_CONSTRUCTOR 0x3072cfa1UL
#define TG_MTPROTO_BAD_MSG_NOTIFICATION_CONSTRUCTOR 0xa7eff811UL
#define TG_MTPROTO_BAD_SERVER_SALT_CONSTRUCTOR 0xedab447bUL
#define TG_MTPROTO_UPDATES_CONSTRUCTOR 0x74ae4240UL
#define TG_MTPROTO_UPDATES_COMBINED_CONSTRUCTOR 0x725b04c3UL
#define TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR 0x78d4dec1UL
#define TG_MTPROTO_UPDATE_SHORT_MESSAGE_CONSTRUCTOR 0x914fbf11UL
#define TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_CONSTRUCTOR 0x16812688UL
/* Layer-214 variants of the same updates (identical leading field layout:
   flags, id:int, sender ids:long, message:string, ...). */
#define TG_MTPROTO_UPDATE_SHORT_MESSAGE_L214_CONSTRUCTOR 0x313bc7f8UL
#define TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_L214_CONSTRUCTOR 0x4d6deea5UL
#define TG_MTPROTO_UPDATE_SHORT_SENT_MESSAGE_CONSTRUCTOR 0x9015e101UL
/* Items inside the rich updates#74ae4240 container (observed live, layer
   214 still uses the classic ids). */
#define TG_MTPROTO_UPDATE_NEW_MESSAGE_CONSTRUCTOR 0x1f2b0afdUL
#define TG_MTPROTO_UPDATE_NEW_CHANNEL_MESSAGE_CONSTRUCTOR 0x62ba04d9UL
#define TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR 0xe40370a3UL
#define TG_MTPROTO_UPDATE_EDIT_CHANNEL_MESSAGE_CONSTRUCTOR 0x1b3f4df7UL
#define TG_MTPROTO_MESSAGE_CONSTRUCTOR 0x9815cec8UL
#define TG_MTPROTO_TL_VECTOR_CONSTRUCTOR 0x1cb5c415UL
#define TG_MTPROTO_UPDATES_TOO_LONG_CONSTRUCTOR 0xe317af7eUL
#define TG_MTPROTO_AUTH_SENT_CODE_CONSTRUCTOR 0x5e002502UL
#define TG_MTPROTO_AUTH_SENT_CODE_SUCCESS_CONSTRUCTOR 0x2390fe44UL
#define TG_MTPROTO_AUTH_SENT_CODE_PAYMENT_REQUIRED_CONSTRUCTOR 0xd7a2fcf9UL
#define TG_MTPROTO_CONFIG_CONSTRUCTOR 0xcc1a241eUL
#define TG_MTPROTO_ACCOUNT_PASSWORD_CONSTRUCTOR 0x957b50fbUL
/* Unpacked-reply buffer for a gzip_packed container. A getHistory page of 60
   messages (non-m68k) with the referenced users/chats can unpack past 64 KiB on
   a busy group; size it to 128 KiB there. m68k fetches 30 and keeps 64 KiB to
   respect its tighter box. Pairs with TG_MTPROTO_REPLY_RECV_MAX below. */
#ifndef TG_MTPROTO_GZIP_UNPACKED_MAX /* overridable: LOWMEM halves it */
#if defined(__m68k__)
#define TG_MTPROTO_GZIP_UNPACKED_MAX 65536UL
#else
#define TG_MTPROTO_GZIP_UNPACKED_MAX 131072UL
#endif
#endif
/* Receive buffer for one decrypted reply frame handed to recv_abridged_packet.
   The old 32 KiB was sized for a 5-message page; the deep-backlog open now asks
   for 60 (non-m68k) / 30 (m68k), and a busy group's frame overruns 32 KiB, so
   recv_abridged_packet rejected it ("Could not read messages now") and the chat
   loaded few/no messages. Size it to the page we actually request. */
#if defined(__m68k__)
#ifndef TG_MTPROTO_REPLY_RECV_MAX /* overridable: LOWMEM shrinks the reply box */
#define TG_MTPROTO_REPLY_RECV_MAX 49152U
#endif
#else
#define TG_MTPROTO_REPLY_RECV_MAX 131072U
#endif
/* Send-side buffers that must hold a whole upload chunk. The saveFilePart query
   IS a file chunk (unlike getFile, whose query is a tiny ~40-byte request), so
   the initConnection wrap buffers and the encrypted send buffers have to be
   chunk-sized or build_init_connection overflows and the upload fails with
   "Upload failed". Reuse the per-platform message-body bound (12 KB m68k / 72 KB
   others), which comfortably holds an 8 KB / 64 KB chunk plus the wrap+envelope.
   This is the send-direction twin of the download PACKET_MAX/body fix. */
#define TG_MTPROTO_QUERY_SEND_MAX TG_MTPROTO_ENCRYPTED_BODY_MAX
#define TG_MTPROTO_PEER_USER_CONSTRUCTOR 0x59511722UL
#define TG_MTPROTO_PEER_CHAT_CONSTRUCTOR 0x36c6019aUL
#define TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR 0xa2a5371eUL
/* Transient "is typing" updates (layer 214; verified hashes). These never ride
   getDifference, only the live push stream -- so the indicator is best-effort
   and surfaces only where pushes flow (not the suppressed MorphOS path). */
#define TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR 0xc01e857fUL
#define TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR 0x83487af0UL
#define TG_MTPROTO_UPDATE_CHANNEL_USER_TYPING_CONSTRUCTOR 0x8c88c923UL
/* updateReadHistoryOutbox#2f2f21bf: the peer read OUR messages up to max_id --
   real-time read receipts (5c). Parser/reader live in tg_mtproto_login.c. */
#define TG_MTPROTO_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR 0x2f2f21bfUL
#define TG_MTPROTO_SEND_MESSAGE_TYPING_ACTION_CONSTRUCTOR 0x16bf744eUL
/* A typing action is refreshed by the server every ~5s while active; clear the
   indicator a hair later so it stays lit during real typing and drops soon
   after. */
#define TG_MTPROTO_TYPING_TTL_SECONDS 6UL
#define TG_MTPROTO_PHONE_MIGRATE_RC_BASE 40

/* A600 field logs (DIAG4, 2026-08-09): with no usable peers cache the
   start-up walks the full network chat-list bootstrap, and the machine dies
   MID-conversation at a different point every run while the PCMCIA wifi
   leds run hot -- every call of ours returns rc 0 right up to the cut. On
   the plain-68000 lane, breathe for a second between the bootstrap's
   network rounds: it spaces the bursts the card and its driver must absorb,
   costs nothing anywhere else, and doubles as the tester's requested "slow
   down the initial chat download". No-op on every other build. */
#if defined(TG_LOWMEM)
#define TG_MTPROTO_BOOTSTRAP_BREATHER() tg_platform_sleep_seconds(1UL)
#else
#define TG_MTPROTO_BOOTSTRAP_BREATHER() ((void)0)
#endif

typedef struct tg_mtproto_auth_context {
    tg_net_connection connection;
    tg_mtproto_session session;
    tg_mtproto_message_id last_msg_id;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    long server_time_delta_seconds;
    int connection_open;
} tg_mtproto_auth_context;

#if TG_ENABLE_GZIP || TG_ENABLE_GZIP_PUFF
static unsigned char tg_mtproto_gzip_unpacked[TG_MTPROTO_GZIP_UNPACKED_MAX];
#endif

static int tg_mtproto_auth_check_password_text(const char *host,
                                               const char *port,
                                               const char *api_id_text,
                                               const char *auth_file,
                                               const char *dc_id_text,
                                               const char *password_input,
                                               FILE *stream);

static FILE *tg_mtproto_open_quiet_stream(FILE *fallback);
static void tg_mtproto_close_quiet_stream(FILE *quiet, FILE *fallback);
static void tg_mtproto_quiet_tmp_sweep(void);
static void tg_mtproto_replay_quiet_stream(FILE *quiet, FILE *fallback);
static int tg_chat_notify_gunzip(const unsigned char *body,
                                 unsigned long body_length,
                                 const unsigned char **out,
                                 unsigned long *out_length);

static int tg_mtproto_production_endpoint_for_dc(unsigned long dc_id,
                                                 const char **host,
                                                 const char **dc_id_text)
{
    if (host == 0 || dc_id_text == 0) {
        return 1;
    }
    switch (dc_id) {
    case 1UL:
        *host = "149.154.175.50";
        *dc_id_text = "1";
        return 0;
    case 2UL:
        *host = "149.154.167.50";
        *dc_id_text = "2";
        return 0;
    case 3UL:
        *host = "149.154.175.100";
        *dc_id_text = "3";
        return 0;
    case 4UL:
        *host = "149.154.167.91";
        *dc_id_text = "4";
        return 0;
    case 5UL:
        *host = "91.108.56.130";
        *dc_id_text = "5";
        return 0;
    default:
        return 1;
    }
}

static int tg_mtproto_parse_phone_migrate_dc(const char *message,
                                             unsigned long *dc_id)
{
    unsigned long value;
    const char *digits;

    if (message == 0 || dc_id == 0) {
        return 0;
    }
    if (strncmp(message, "PHONE_MIGRATE_", 14) != 0) {
        return 0;
    }
    digits = message + 14;
    if (*digits < '1' || *digits > '9') {
        return 0;
    }
    value = 0UL;
    while (*digits >= '0' && *digits <= '9') {
        value = (value * 10UL) + (unsigned long)(*digits - '0');
        ++digits;
    }
    if (*digits != '\0' || value == 0UL || value > 255UL) {
        return 0;
    }
    *dc_id = value;
    return 1;
}

static int tg_mtproto_is_async_update_constructor(unsigned long constructor)
{
    return constructor == TG_MTPROTO_UPDATES_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATES_COMBINED_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATE_SHORT_MESSAGE_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATE_SHORT_MESSAGE_L214_CONSTRUCTOR ||
           constructor ==
               TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_L214_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATE_SHORT_SENT_MESSAGE_CONSTRUCTOR ||
           constructor == TG_MTPROTO_UPDATES_TOO_LONG_CONSTRUCTOR;
}

/*
 * Cross-chat notifications. Telegram pushes updates for EVERY chat over the
 * open session socket; the receive loop used to ack and discard them. The
 * collector below extracts new-message updates (updateShortMessage for DMs,
 * updateShortChatMessage for basic groups -- both the legacy and the
 * layer-214 constructors, whose leading fields match) into a small queue;
 * the interactive chat drains it and shows "who wrote where" lines for
 * chats other than the open one. Channel posts ride the richer 'updates'
 * container and are not collected yet.
 *
 * Collection is armed only while the interactive chat runs, so one-shot
 * commands (list-peers, login) never accumulate entries. Zero extra network
 * traffic: everything here was already on the wire.
 */
static unsigned long tg_mtproto_read_u32_le(const unsigned char *data);

/* The cross-chat notification queue, dedupe ring and constants now live in the
   chat engine (tg_chat_engine.notify, see tg_chat_engine.h). The MTProto
   parsers below reach the active chat session's queue through this back-pointer,
   bound at session start; it is NULL outside a chat and the notify ops are
   NULL-safe, so collection simply no-ops there. */
static tg_chat_notify *tg_chat_nq = 0;
/* Live "is typing" sink. The push collector records the most recent typing peer
   here; the GUI session reads it each tick and lights the header for the open
   chat. NULL outside a GUI session (the console path leaves it untouched). */
typedef struct tg_chat_typing_sink {
    int active;
    int is_chat;             /* group/channel vs DM */
    unsigned long peer_id_hi; /* DM user id, or group/channel id */
    unsigned long peer_id_lo;
    unsigned long from_id_hi; /* who is typing (groups); = peer for a DM */
    unsigned long from_id_lo;
    unsigned long seen_epoch; /* time() when last seen, for the TTL */
} tg_chat_typing_sink;
static tg_chat_typing_sink *tg_chat_typing_target = 0;

/* Explicit context bundle for the file workers below, so the SAME machinery
   serves both front-ends: the GUI wrappers fill it from the session
   singleton, the console chat loop from its own locals (F9 parity for the
   TUI). `context` is the caller's authenticated connection; `peer_index` is
   the peer-cache index TEXT ("self" opens Saved Messages). */
typedef struct tg_mtproto_file_ctx {
    const char *host;
    const char *port;
    const char *api_id;
    const char *auth_file;
    const char *dc_id_text;
    tg_mtproto_auth_context *context;
    const char *peer_cache_file;
    const char *peer_index;
} tg_mtproto_file_ctx;

/* File workers (defined with the GUI session code below; the console chat
   loop calls them with its own locals). */
static int tg_mtproto_file_download(const struct tg_mtproto_file_ctx *fc,
                                    unsigned long msg_id, char *out_path,
                                    unsigned long out_path_size, FILE *stream,
                                    tg_gui_download_progress_fn progress,
                                    void *progress_data);
static int tg_mtproto_file_send(const struct tg_mtproto_file_ctx *fc,
                                const char *path, FILE *stream,
                                tg_gui_upload_progress_fn progress,
                                void *progress_data,
                                int as_photo, const char *caption);
/* Live read-receipt sink (5c): the push collector records the most recent
   updateReadHistoryOutbox (which peer read up to which id); the GUI loop applies
   it when the peer matches the open chat. NULL outside a GUI session. */
typedef struct tg_chat_read_outbox_sink {
    unsigned long peer_id_hi; /* peer of the latest read-outbox push */
    unsigned long peer_id_lo;
    unsigned long max_id;     /* the read cursor it reported */
    int pending;              /* 1 = a push not yet applied by the loop */
} tg_chat_read_outbox_sink;
static tg_chat_read_outbox_sink *tg_chat_read_outbox_target = 0;
/* Remote edits arrive as updateEditMessage/updateEditChannelMessage pushes.
   Keep a short queue because one encrypted-query receive loop may consume
   several updates before the GUI gets control back. */
#define TG_CHAT_EDIT_QUEUE_MAX 8U
typedef struct tg_chat_edit_entry {
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long message_id;
    char text[TG_GUI_MSG_TEXT_MAX];
} tg_chat_edit_entry;
typedef struct tg_chat_edit_sink {
    tg_chat_edit_entry queue[TG_CHAT_EDIT_QUEUE_MAX];
    unsigned long count;
} tg_chat_edit_sink;
static tg_chat_edit_sink *tg_chat_edit_target = 0;
/* The real console stream while the chat runs, for TUI components that must
   bypass capture streams (input-row redraws from the editor and the
   sub-prompts). 0 outside the chat. */
static FILE *tg_chat_tui_stream = 0;
static void tg_mtproto_chat_show_prompt(FILE *stream,
                                        const char *own_label,
                                        const char *peer_label,
                                        const char *pending,
                                        unsigned long pending_length,
                                        int raw);
/* Console bell (BEL -> screen flash on Amiga consoles) on notifications. */
static int tg_chat_bell_enabled = 1;
/* Gap-handling cursor (updates.getState / getDifference) and the /diff toggle
   now live in the chat engine (tg_chat_engine), a stack-local of
   tg_mtproto_auth_chat_file -- the first slice of the engine extraction. */
/* Day (local-frame epoch/86400) of the last transcript line, for the
   "--- 10 Jun ---" separators; 0 = nothing printed yet this chat. */
static unsigned long tg_chat_day_shown = 0UL;
/* When set, the history renderer routes resolved rows here instead of building
   the console driver -- the GUI session points this at its own driver around a
   history fetch, then clears it. NULL (the default) keeps the console path
   byte-identical. */
static const tg_chat_driver *tg_chat_message_driver_override = 0;

/* getHistory offset_id for the next history fetch. 0 (default) = newest page;
   tg_gui_session_load_older sets it to the oldest message currently shown so the
   server returns the page BELOW it, then restores 0. The print-history path reads
   it when building the query; every other caller leaves it 0 (newest-pinned). */
static unsigned long tg_mtproto_history_offset_id_override = 0UL;

/* Server-side total message count for the last getHistory, used by the open path
   to arm the forced "pull older" scrollbar when the chat has more history than the
   loaded page (see tg_gui_state.more_above). */
static unsigned long tg_gui_last_hist_total = 0UL;

/* tg_chat_notify_reset / tg_chat_notify_seen now live in tg_chat_engine.c and
   operate on the engine's notify queue (reached here via tg_chat_nq). */

/* Parses one bare updateShortMessage/updateShortChatMessage payload. */
static void tg_chat_notify_collect_one(const unsigned char *body,
                                       unsigned long body_length)
{
    tg_mtproto_tl_reader reader;
    tg_chat_notify_entry *entry;
    unsigned long constructor;
    unsigned long flags;
    unsigned long message_id;
    unsigned long sender_hi;
    unsigned long sender_lo;
    unsigned long chat_hi;
    unsigned long chat_lo;
    const unsigned char *text;
    unsigned long text_length;
    unsigned long copy_length;
    int is_chat;

    if (body == 0 || body_length < 4UL) {
        return;
    }
    constructor = tg_mtproto_read_u32_le(body);
    if (constructor == TG_MTPROTO_UPDATE_SHORT_MESSAGE_CONSTRUCTOR ||
        constructor == TG_MTPROTO_UPDATE_SHORT_MESSAGE_L214_CONSTRUCTOR) {
        is_chat = 0;
    } else if (constructor ==
                   TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_CONSTRUCTOR ||
               constructor ==
                   TG_MTPROTO_UPDATE_SHORT_CHAT_MESSAGE_L214_CONSTRUCTOR) {
        is_chat = 1;
    } else {
        return;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &message_id) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(&reader, &sender_hi, &sender_lo) !=
            TG_MTPROTO_TL_OK) {
        return;
    }
    chat_hi = sender_hi;
    chat_lo = sender_lo;
    if (is_chat &&
        tg_mtproto_tl_read_u64(&reader, &chat_hi, &chat_lo) !=
            TG_MTPROTO_TL_OK) {
        return;
    }
    if (tg_mtproto_tl_read_bytes(&reader, &text, &text_length) !=
        TG_MTPROTO_TL_OK) {
        return;
    }
    /* flags bit 1 = outgoing (sent from this account on another device):
       still useful to see, but skip for now to keep the stream calm. */
    if ((flags & 0x2UL) != 0UL) {
        return;
    }
    if (tg_chat_notify_seen(tg_chat_nq, message_id)) {
        return;
    }
    entry = tg_chat_notify_claim(tg_chat_nq);
    if (entry == 0) {
        return;
    }
    entry->is_chat = is_chat;
    entry->peer_id_hi = chat_hi;
    entry->peer_id_lo = chat_lo;
    entry->from_id_hi = sender_hi;
    entry->from_id_lo = sender_lo;
    copy_length = text_length;
    if (copy_length >= TG_CHAT_NOTIFY_TEXT) {
        copy_length = TG_CHAT_NOTIFY_TEXT - 1UL;
    }
    memcpy(entry->text, text, copy_length);
    entry->text[copy_length] = '\0';
}

static void tg_chat_notify_collect(const unsigned char *body,
                                   unsigned long body_length);
static unsigned long tg_mtproto_chat_local_epoch(unsigned long message_date,
                                                 long server_delta);
static void tg_mtproto_chat_print_message_time(FILE *stream,
                                               unsigned long local_epoch);
static void tg_mtproto_chat_print_day_separator(FILE *stream,
                                                unsigned long local_epoch);

static void tg_mtproto_probe_random(unsigned char *bytes, unsigned long length)
{
    static unsigned long seed = 0;
    unsigned long i;

    if (tg_platform_random_bytes(bytes, length)) {
        return;
    }
    if (seed == 0UL) {
        seed = (unsigned long)time(0);
    }
    for (i = 0; i < length; ++i) {
        seed = (seed * 1103515245UL) + 12345UL;
        bytes[i] = (unsigned char)((seed >> 16) & 0xffUL);
    }
}

static int tg_mtproto_secure_random(unsigned char *bytes, unsigned long length)
{
    return tg_platform_random_bytes(bytes, length);
}

static void tg_mtproto_saved_session_random(unsigned char *bytes,
                                            unsigned long length)
{
    if (!tg_platform_random_bytes(bytes, length)) {
        tg_mtproto_probe_random(bytes, length);
    }
}

static unsigned long tg_mtproto_context_time(
    const tg_mtproto_auth_context *context)
{
    unsigned long now;
    unsigned long delta;

    now = (unsigned long)time(0);
    if (context == 0 || context->server_time_delta_seconds == 0L) {
        return now;
    }
    if (context->server_time_delta_seconds < 0L) {
        delta = (unsigned long)(0L - context->server_time_delta_seconds);
        return now > delta ? now - delta : 0UL;
    }
    return now + (unsigned long)context->server_time_delta_seconds;
}

/*
 * Seconds by which the raw local Amiga clock (tg_platform_local_epoch, read from
 * DateStamp -- the value the Workbench clock shows) leads the C library time().
 * Zero on toolchains where time() already returns the raw local clock, but on
 * AmigaOS3/clib2 time() adds the locale GMT offset to synthesize UTC and never
 * applies DST, so time() is NOT the wall clock. The display path corrects the
 * server delta by this skew so message times match the system clock regardless
 * of how Locale/DST are configured; protocol timing keeps using time()/the UTC
 * delta untouched. On hosts tg_platform_local_epoch falls back to time(), so the
 * skew is zero and behaviour is unchanged.
 */
static long tg_mtproto_local_clock_skew(void)
{
    unsigned long local_now;
    unsigned long lib_now;

    local_now = tg_platform_local_epoch();
    lib_now = (unsigned long)time(0);
    if (local_now >= lib_now) {
        return (long)(local_now - lib_now);
    }
    return -(long)(lib_now - local_now);
}

static void tg_mtproto_sync_time_from_server(
    tg_mtproto_auth_context *context,
    const tg_mtproto_encrypted_message *message)
{
    unsigned long now;
    unsigned long server_time;
    unsigned long delta;

    if (context == 0 || message == 0 || message->message_id_hi == 0UL) {
        return;
    }

    now = (unsigned long)time(0);
    server_time = message->message_id_hi;
    if (server_time >= now) {
        delta = server_time - now;
        context->server_time_delta_seconds = (long)delta;
    } else {
        delta = now - server_time;
        context->server_time_delta_seconds = -((long)delta);
    }

    context->last_msg_id.hi = message->message_id_hi;
    context->last_msg_id.lo = message->message_id_lo;
    context->session.last_msg_id_hi = context->last_msg_id.hi;
    context->session.last_msg_id_lo = context->last_msg_id.lo;
}

static void tg_mtproto_refresh_saved_session(
    tg_mtproto_auth_context *context)
{
    if (context == 0) {
        return;
    }
    tg_mtproto_saved_session_random(context->session.session_id,
                                    sizeof(context->session.session_id));
    context->session.seq_no = 1UL;
    tg_mtproto_client_message_id(tg_mtproto_context_time(context), 4UL, 0,
                                 &context->last_msg_id);
    context->session.last_msg_id_hi = context->last_msg_id.hi;
    context->session.last_msg_id_lo = context->last_msg_id.lo;
}

static void tg_mtproto_probe_nonce(unsigned char nonce[16])
{
    tg_mtproto_probe_random(nonce, 16UL);
}

static tg_net_status tg_mtproto_send_all(tg_net_connection *connection,
                                         const unsigned char *data,
                                         unsigned long length,
                                         char *error_buffer,
                                         unsigned long error_buffer_size)
{
    unsigned long sent;
    unsigned long offset;
    tg_net_status status;

    offset = 0;
    while (offset < length) {
        status = tg_net_send(connection, data + offset, length - offset, &sent,
                             error_buffer, error_buffer_size);
        if (status != TG_NET_OK) {
            return status;
        }
        if (sent == 0) {
            return TG_NET_SEND_FAILED;
        }
        offset += sent;
    }

    return TG_NET_OK;
}

/* Bumped by every chunk of bytes the socket actually delivers. The encrypted
   query loop watches it to tell "slow but progressing" from "wedged": its
   budget must expire on IDLE time, never on total time, or a big reply on a
   slow link is killed while it is still streaming in fine. */
static unsigned long tg_mtproto_rx_progress;

static tg_net_status tg_mtproto_recv_exact(tg_net_connection *connection,
                                           unsigned char *data,
                                           unsigned long length,
                                           char *error_buffer,
                                           unsigned long error_buffer_size)
{
    unsigned long received;
    unsigned long offset;
    tg_net_status status;

    offset = 0;
    while (offset < length) {
        status = tg_net_recv(connection, data + offset, length - offset,
                             &received, error_buffer, error_buffer_size);
        if (status != TG_NET_OK) {
            /* A timeout after part of this chunk was already read cannot be
               retried without losing the consumed bytes -> hard failure. Only a
               timeout with nothing consumed (offset == 0) stays retryable. */
            if (status == TG_NET_TIMEOUT && offset != 0UL) {
                return TG_NET_RECV_FAILED;
            }
            return status;
        }
        if (received == 0) {
            return TG_NET_CLOSED;
        }
        tg_mtproto_rx_progress += received; /* data moved: query is alive */
        offset += received;
    }

    return TG_NET_OK;
}

static tg_net_status tg_mtproto_recv_abridged_packet(
    tg_net_connection *connection,
    unsigned char *payload,
    unsigned long payload_capacity,
    unsigned long *payload_length,
    char *error_buffer,
    unsigned long error_buffer_size)
{
    unsigned char length_header[4];
    unsigned long length_words;
    tg_net_status status;

    if (payload_length != 0) {
        *payload_length = 0;
    }
    if (payload == 0 || payload_length == 0) {
        return TG_NET_INVALID_ARGUMENT;
    }

    status = tg_mtproto_recv_exact(connection, length_header, 1,
                                   error_buffer, error_buffer_size);
    if (status != TG_NET_OK) {
        return status;
    }

    if (length_header[0] < 0x7fU) {
        length_words = length_header[0];
    } else {
        status = tg_mtproto_recv_exact(connection, length_header + 1, 3,
                                       error_buffer, error_buffer_size);
        if (status != TG_NET_OK) {
            /* The length byte was already consumed, so a timeout here leaves the
               packet half-read and is not safely retryable -> hard failure. */
            return status == TG_NET_TIMEOUT ? TG_NET_RECV_FAILED : status;
        }
        length_words = ((unsigned long)length_header[1]) |
                       (((unsigned long)length_header[2]) << 8) |
                       (((unsigned long)length_header[3]) << 16);
    }

    *payload_length = length_words * 4UL;
    if (*payload_length > payload_capacity) {
        return TG_NET_RECV_FAILED;
    }

    status = tg_mtproto_recv_exact(connection, payload, *payload_length,
                                   error_buffer, error_buffer_size);
    return status == TG_NET_TIMEOUT ? TG_NET_RECV_FAILED : status;
}

static unsigned long tg_mtproto_read_u32_le(const unsigned char *data)
{
    return ((unsigned long)data[0]) |
           (((unsigned long)data[1]) << 8) |
           (((unsigned long)data[2]) << 16) |
           (((unsigned long)data[3]) << 24);
}

static void tg_mtproto_u32_be(unsigned long value, unsigned char bytes[4])
{
    bytes[0] = (unsigned char)((value >> 24) & 0xffUL);
    bytes[1] = (unsigned char)((value >> 16) & 0xffUL);
    bytes[2] = (unsigned char)((value >> 8) & 0xffUL);
    bytes[3] = (unsigned char)(value & 0xffUL);
}

static int tg_mtproto_body_is_expected_pong(const unsigned char *body,
                                            unsigned long body_length,
                                            unsigned long ping_id_hi,
                                            unsigned long ping_id_lo)
{
    return body != 0 && body_length >= 20UL &&
           tg_mtproto_read_u32_le(body) == 0x347773c5UL &&
           tg_mtproto_read_u32_le(body + 12U) == ping_id_lo &&
           tg_mtproto_read_u32_le(body + 16U) == ping_id_hi;
}

static int tg_mtproto_container_has_expected_pong(
    const unsigned char *body,
    unsigned long body_length,
    unsigned long ping_id_hi,
    unsigned long ping_id_lo)
{
    unsigned long count;
    unsigned long index;
    unsigned long offset;
    unsigned long nested_length;

    if (body == 0 || body_length < 8UL ||
        tg_mtproto_read_u32_le(body) != 0x73f1f8dcUL) {
        return 0;
    }
    count = tg_mtproto_read_u32_le(body + 4U);
    offset = 8UL;
    for (index = 0UL; index < count; ++index) {
        if (body_length - offset < 16UL) {
            return 0;
        }
        nested_length = tg_mtproto_read_u32_le(body + offset + 12U);
        offset += 16UL;
        if (nested_length > body_length - offset) {
            return 0;
        }
        if (tg_mtproto_body_is_expected_pong(body + offset, nested_length,
                                             ping_id_hi, ping_id_lo)) {
            return 1;
        }
        offset += nested_length;
    }
    return 0;
}

static int tg_mtproto_parse_dc_id(const char *text, long *dc_id)
{
    char *endptr;
    long value;
    const char *number_text;
    int is_test_dc;

    if (text == 0 || text[0] == '\0' || dc_id == 0) {
        return 1;
    }
    is_test_dc = 0;
    number_text = text;
    if (strncmp(text, "test:", 5U) == 0) {
        is_test_dc = 1;
        number_text = text + 5U;
    }
    value = strtol(number_text, &endptr, 10);
    if (endptr == number_text || *endptr != '\0' || value < -100000L ||
        value > 100000L) {
        return 1;
    }
    if (is_test_dc) {
        if (value < 1L || value > 9999L) {
            return 1;
        }
        value += 10000L;
    }
    *dc_id = value;
    return 0;
}

static int tg_mtproto_parse_ulong_arg(const char *text, unsigned long *out)
{
    char *endptr;
    unsigned long value;

    if (text == 0 || text[0] == '\0' || out == 0) {
        return 1;
    }
    value = strtoul(text, &endptr, 10);
    if (endptr == text || *endptr != '\0') {
        return 1;
    }
    *out = value;
    return 0;
}

static void tg_mtproto_close_auth_context(tg_mtproto_auth_context *context)
{
    if (context != 0 && context->connection_open) {
#ifdef TG_MTPROTO_DIAG
        fprintf(stderr, "notify-diag ctx-real-close\n");
#endif
        tg_net_close(&context->connection);
        context->connection_open = 0;
    }
}

static void tg_mtproto_skip_auth_context_close(tg_mtproto_auth_context *context,
                                               FILE *stream,
                                               const char *label)
{
#ifdef TG_MTPROTO_DIAG
    fprintf(stderr, "notify-diag ctx-skip-close label=%s\n",
            label != 0 ? label : "?");
    (void)stream;
#else
    (void)stream;
    (void)label;
#endif
    if (context != 0) {
        /* Historically this left the socket OPEN (it only re-inited the struct),
           a workaround from when shutdown()-before-close() froze on slow links.
           That abandoned a socket per one-shot auth op (sendCode/signIn/
           checkPassword/saved-query). Harmless on the console (process exit
           reaps the fds), but in the long-lived GUI those orphaned sockets pile
           up and CloseLibrary(bsdsocket) stalls the task at exit (notably OS4).
           The freeze is now handled in the platform close (no shutdown before
           CloseSocket), and the struct is re-inited right below so nothing can
           reuse the connection -- so do a real, non-blocking close here. */
        if (context->connection_open) {
            tg_net_close(&context->connection);
        }
        context->connection_open = 0;
        tg_net_connection_init(&context->connection);
    }
}

static int tg_mtproto_validate_saved_auth_dc(
    const tg_mtproto_auth_context *context,
    unsigned long requested_dc,
    FILE *stream,
    const char *label)
{
    if (context == 0 || stream == 0 || label == 0 || requested_dc == 0UL) {
        return 2;
    }
    if (context->session.dc_id != 0UL &&
        context->session.dc_id != requested_dc) {
        fprintf(stream,
                "%s: auth-dc-mismatch auth-file-dc %lu requested-dc %lu\n",
                label, context->session.dc_id, requested_dc);
        return 2;
    }
    return 0;
}

static int tg_mtproto_find_rpc_result_direct(
    const unsigned char *body,
    unsigned long body_length,
    unsigned long request_msg_id_hi,
    unsigned long request_msg_id_lo,
    tg_mtproto_rpc_result *out)
{
    tg_mtproto_rpc_result result;

    if (tg_mtproto_parse_rpc_result(body, body_length, &result) !=
            TG_MTPROTO_TL_OK) {
        return 0;
    }
    if (result.request_msg_id_hi != request_msg_id_hi ||
        result.request_msg_id_lo != request_msg_id_lo) {
        return 0;
    }
    if (out != 0) {
        *out = result;
    }
    return 1;
}

static int tg_mtproto_find_rpc_result(
    const unsigned char *body,
    unsigned long body_length,
    unsigned long request_msg_id_hi,
    unsigned long request_msg_id_lo,
    tg_mtproto_rpc_result *out)
{
    unsigned long count;
    unsigned long index;
    unsigned long offset;
    unsigned long nested_length;

    if (tg_mtproto_find_rpc_result_direct(body, body_length, request_msg_id_hi,
                                          request_msg_id_lo, out)) {
        return 1;
    }
    if (body == 0 || body_length < 8UL ||
        tg_mtproto_read_u32_le(body) != TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR) {
        return 0;
    }
    count = tg_mtproto_read_u32_le(body + 4U);
    offset = 8UL;
    for (index = 0UL; index < count; ++index) {
        if (body_length - offset < 16UL) {
            return 0;
        }
        nested_length = tg_mtproto_read_u32_le(body + offset + 12U);
        offset += 16UL;
        if (nested_length > body_length - offset) {
            return 0;
        }
        if (tg_mtproto_find_rpc_result_direct(body + offset, nested_length,
                                              request_msg_id_hi,
                                              request_msg_id_lo, out)) {
            return 1;
        }
        offset += nested_length;
    }
    return 0;
}

static int tg_mtproto_find_bad_msg_direct(
    const unsigned char *body,
    unsigned long body_length,
    unsigned long request_msg_id_hi,
    unsigned long request_msg_id_lo,
    tg_mtproto_bad_msg_notification *out)
{
    tg_mtproto_bad_msg_notification notification;

    if (tg_mtproto_parse_bad_msg_notification(body, body_length,
                                              &notification) !=
            TG_MTPROTO_TL_OK) {
        return 0;
    }
    if (notification.bad_msg_id_hi != request_msg_id_hi ||
        notification.bad_msg_id_lo != request_msg_id_lo) {
        return 0;
    }
    if (out != 0) {
        *out = notification;
    }
    return 1;
}

static int tg_mtproto_find_bad_msg(
    const unsigned char *body,
    unsigned long body_length,
    unsigned long request_msg_id_hi,
    unsigned long request_msg_id_lo,
    tg_mtproto_bad_msg_notification *out)
{
    unsigned long count;
    unsigned long index;
    unsigned long offset;
    unsigned long nested_length;

    if (tg_mtproto_find_bad_msg_direct(body, body_length, request_msg_id_hi,
                                       request_msg_id_lo, out)) {
        return 1;
    }
    if (body == 0 || body_length < 8UL ||
        tg_mtproto_read_u32_le(body) != TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR) {
        return 0;
    }
    count = tg_mtproto_read_u32_le(body + 4U);
    offset = 8UL;
    for (index = 0UL; index < count; ++index) {
        if (body_length - offset < 16UL) {
            return 0;
        }
        nested_length = tg_mtproto_read_u32_le(body + offset + 12U);
        offset += 16UL;
        if (nested_length > body_length - offset) {
            return 0;
        }
        if (tg_mtproto_find_bad_msg_direct(body + offset, nested_length,
                                           request_msg_id_hi,
                                           request_msg_id_lo, out)) {
            return 1;
        }
        offset += nested_length;
    }
    return 0;
}

static void tg_mtproto_collect_ack_ids(
    const tg_mtproto_encrypted_message *message,
    unsigned long *ack_hi,
    unsigned long *ack_lo,
    unsigned long ack_capacity,
    unsigned long *ack_count)
{
    unsigned long count;
    unsigned long index;
    unsigned long offset;
    unsigned long nested_length;

    if (ack_count != 0) {
        *ack_count = 0;
    }
    if (message == 0 || ack_hi == 0 || ack_lo == 0 || ack_count == 0 ||
        ack_capacity == 0UL) {
        return;
    }

    ack_hi[0] = message->message_id_hi;
    ack_lo[0] = message->message_id_lo;
    *ack_count = 1UL;

    if (message->body_length < 8UL ||
        tg_mtproto_read_u32_le(message->body) !=
            TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR) {
        return;
    }

    count = tg_mtproto_read_u32_le(message->body + 4U);
    offset = 8UL;
    for (index = 0UL; index < count && *ack_count < ack_capacity; ++index) {
        if (message->body_length - offset < 16UL) {
            return;
        }
        ack_lo[*ack_count] = tg_mtproto_read_u32_le(message->body + offset);
        ack_hi[*ack_count] = tg_mtproto_read_u32_le(message->body + offset + 4U);
        nested_length = tg_mtproto_read_u32_le(message->body + offset + 12U);
        offset += 16UL;
        if (nested_length > message->body_length - offset) {
            return;
        }
        ++(*ack_count);
        offset += nested_length;
    }
}

static int tg_mtproto_send_encrypted_service(
    tg_mtproto_auth_context *context,
    const unsigned char *body,
    unsigned long body_length,
    FILE *stream,
    const char *label)
{
    unsigned char encrypted_padding[64];
    static unsigned char payload[1024];
    static unsigned char packet[1100];
    unsigned long encrypted_padding_length;
    unsigned long payload_length;
    tg_mtproto_message_id msg_id;
    tg_mtproto_tl_writer writer;
    tg_net_status net_status;
    char error_buffer[160];

    if (context == 0 || !context->connection_open || body == 0 ||
        body_length == 0UL || stream == 0 || label == 0) {
        return 2;
    }

    encrypted_padding_length = 12UL;
    while (((32UL + body_length + encrypted_padding_length) % 16UL) != 0UL) {
        ++encrypted_padding_length;
    }
    tg_mtproto_saved_session_random(encrypted_padding,
                                    encrypted_padding_length);
    tg_mtproto_client_message_id(tg_mtproto_context_time(context), 20UL,
                                 &context->last_msg_id, &msg_id);
    context->last_msg_id = msg_id;
    context->session.last_msg_id_hi = msg_id.hi;
    context->session.last_msg_id_lo = msg_id.lo;
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_encrypted_message(
            &writer, context->auth_key, context->session.server_salt_hi,
            context->session.server_salt_lo, context->session.session_id,
            msg_id.hi, msg_id.lo, context->session.seq_no - 1UL,
            body, body_length, encrypted_padding,
            encrypted_padding_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: service-build-failed\n", label);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: service-transport-build-failed\n", label);
        return 2;
    }
    error_buffer[0] = '\0';
    net_status = tg_mtproto_send_all(&context->connection, packet,
                                     writer.length, error_buffer,
                                     sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: service-send-failed (%s)\n", label,
                tg_net_status_name(net_status));
        return 2;
    }
    return 0;
}

static void tg_mtproto_ack_server_messages(
    tg_mtproto_auth_context *context,
    const unsigned long *ack_hi,
    const unsigned long *ack_lo,
    unsigned long ack_count,
    FILE *stream,
    const char *label)
{
    unsigned char body[256];
    tg_mtproto_tl_writer writer;

    if (ack_count == 0UL) {
        return;
    }
    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_build_msgs_ack(&writer, ack_hi, ack_lo, ack_count) !=
        TG_MTPROTO_TL_OK) {
        return;
    }
    (void)tg_mtproto_send_encrypted_service(context, body, writer.length,
                                            stream, label);
}

static int tg_mtproto_send_query_acks_enabled(void)
{
    return 1;
}

static void tg_mtproto_ack_encrypted_message(
    tg_mtproto_auth_context *context,
    const tg_mtproto_encrypted_message *message,
    FILE *stream,
    const char *label)
{
    unsigned long ack_hi[16];
    unsigned long ack_lo[16];
    unsigned long ack_count;

    if (!tg_mtproto_send_query_acks_enabled()) {
        return;
    }
    tg_mtproto_collect_ack_ids(message, ack_hi, ack_lo, 16UL, &ack_count);
    tg_mtproto_ack_server_messages(context, ack_hi, ack_lo, ack_count, stream,
                                   label);
}

/* Shared query-loop buffers (one query in flight per task, callers are
   strictly serial): the classic loop below and the 1d pipelined pair reuse
   the SAME storage, so the split adds no BSS. payload/packet carry the
   outgoing frame; response/decrypted the incoming one. */
static unsigned char tg_mtproto_q_payload[TG_MTPROTO_QUERY_SEND_MAX];
static unsigned char tg_mtproto_q_packet[TG_MTPROTO_QUERY_SEND_MAX + 64U];
/* getHistory of a busy group can return several messages plus the referenced
   users/chats; a too-small buffer makes recv_abridged_packet reject the
   frame (payload > capacity) and the read hard-fails. Sized by
   TG_MTPROTO_REPLY_RECV_MAX for the deep-backlog page. */
static unsigned char tg_mtproto_q_response[TG_MTPROTO_REPLY_RECV_MAX];
static tg_mtproto_encrypted_message tg_mtproto_q_decrypted;
#define payload tg_mtproto_q_payload
#define packet tg_mtproto_q_packet
#define response tg_mtproto_q_response
#define decrypted tg_mtproto_q_decrypted

static int tg_mtproto_send_encrypted_query_limited(
    tg_mtproto_auth_context *context,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_rpc_result *rpc_result,
    FILE *stream,
    const char *label,
    unsigned int max_receive_attempts,
    unsigned long query_budget_seconds)
{
    unsigned char encrypted_padding[64];
    unsigned long encrypted_padding_length;
    unsigned long payload_length;
    unsigned long response_length;
    unsigned long query_start_time;
    unsigned long query_now;
    unsigned long rx_seen;      /* rx_progress snapshot: detects streaming */
    unsigned int attempt;
    unsigned int receive_attempt;
    int retry_request;
    unsigned long response_constructor;
    tg_mtproto_bad_msg_notification bad_msg;
    tg_mtproto_message_id request_msg_id;
    tg_mtproto_tl_writer writer;
    tg_net_status net_status;
    char error_buffer[160];

    if (context == 0 || !context->connection_open || body == 0 ||
        body_length == 0UL || rpc_result == 0 || stream == 0 ||
        label == 0) {
        return 2;
    }
    if (max_receive_attempts == 0U) {
        max_receive_attempts = 32U;
    }
    if (query_budget_seconds == 0UL) {
        query_budget_seconds = TG_MTPROTO_QUERY_BUDGET_SECONDS;
    }
    query_start_time = (unsigned long)time(0);
    rx_seen = tg_mtproto_rx_progress;
    /* Cleared ONCE per query, not per attempt: a retry used to wipe the reason
       recorded by the attempt that actually failed, leaving a bare "no reply". */
    tg_mtproto_query_fail[0] = '\0';

#ifdef TG_MTPROTO_DIAG
    fprintf(stream, "%s: encrypted query phase enter.\n", label);
    fflush(stream);
#endif
    for (attempt = 0U; attempt < 6U; ++attempt) {
        retry_request = 0;
        encrypted_padding_length = 12UL;
        while (((32UL + body_length + encrypted_padding_length) % 16UL) !=
               0UL) {
            ++encrypted_padding_length;
        }
        tg_mtproto_saved_session_random(encrypted_padding,
                                        encrypted_padding_length);
        tg_mtproto_client_message_id(tg_mtproto_context_time(context), 16UL,
                                     &context->last_msg_id, &request_msg_id);
        context->last_msg_id = request_msg_id;
        context->session.last_msg_id_hi = request_msg_id.hi;
        context->session.last_msg_id_lo = request_msg_id.lo;

#ifdef TG_MTPROTO_DIAG
        fprintf(stream, "%s: encrypted query phase build attempt %u.\n",
                label, (unsigned int)(attempt + 1U));
        fflush(stream);
#endif
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        if (tg_mtproto_write_encrypted_message(
                &writer, context->auth_key, context->session.server_salt_hi,
                context->session.server_salt_lo, context->session.session_id,
                request_msg_id.hi, request_msg_id.lo,
                context->session.seq_no, body, body_length,
                encrypted_padding, encrypted_padding_length) !=
            TG_MTPROTO_TL_OK) {
            fprintf(stream, "%s: encrypted-query-build-failed\n", label);
            return 2;
        }
        payload_length = writer.length;

#ifdef TG_MTPROTO_DIAG
        fprintf(stream, "%s: encrypted query phase packet attempt %u.\n",
                label, (unsigned int)(attempt + 1U));
        fflush(stream);
#endif
        tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
        if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
            TG_MTPROTO_TL_OK) {
            fprintf(stream, "%s: transport-build-failed\n", label);
            return 2;
        }

        error_buffer[0] = '\0';
#ifdef TG_MTPROTO_DIAG
        fprintf(stream, "%s: encrypted query phase send attempt %u.\n",
                label, (unsigned int)(attempt + 1U));
        fflush(stream);
#endif
        net_status = tg_mtproto_send_all(&context->connection, packet,
                                         writer.length, error_buffer,
                                         sizeof(error_buffer));
        if (net_status == TG_NET_OK) {
            /* MTProto seq_no is consumed by sending the content-related
               request, not by receiving its rpc_result. Advance it immediately
               so a soft timeout followed by a retry does not resend another
               content message with the same session_id/seq_no pair. */
            context->session.seq_no += 2UL;
        } else {
            /* The request never left (send failed or timed out on a wedged
               socket): there is nothing to wait for -- failing now beats
               burning the whole receive budget polling for a reply to an
               unsent query. The caller's retry can reconnect. */
            sprintf(tg_mtproto_query_fail, "send %.40s",
                    tg_net_status_name(net_status));
            fprintf(stream, "%s: send-failed (%s)\n", label,
                    tg_net_status_name(net_status));
            return 2;
        }
        memset(&bad_msg, 0, sizeof(bad_msg));
        if (tg_platform_break_pending()) {
            fprintf(stream, "%s: user-break\n", label);
            return 2;
        }
        for (receive_attempt = 0U; receive_attempt < max_receive_attempts;
             ++receive_attempt) {
            if (tg_mtproto_rx_progress != rx_seen) {
                /* Bytes arrived since the last check: the reply is streaming,
                   just slowly (a 64 KB file chunk over a phone hotspot easily
                   outlasts a 20s TOTAL budget). Re-arm the deadline so the
                   budget measures IDLE time -- a genuinely wedged session still
                   trips it, a progressing transfer no longer dies mid-file. */
                rx_seen = tg_mtproto_rx_progress;
                query_start_time = (unsigned long)time(0);
            }
            query_now = (unsigned long)time(0);
            if (query_now < query_start_time) {
                /* AmiKit may correct its wall clock backwards shortly after
                   networking comes up. Treat that as a fresh budget origin;
                   unsigned subtraction would otherwise look like an instant
                   multi-year timeout. */
                query_start_time = query_now;
            } else if (query_now - query_start_time >=
                           query_budget_seconds) {
                sprintf(tg_mtproto_query_fail, "no data for %lus",
                        query_budget_seconds);
                break;  /* IDLE budget hit: soft-fail, connection still alive */
            }
            if (net_status == TG_NET_OK) {
#ifdef TG_MTPROTO_DIAG
                fprintf(stream,
                        "%s: encrypted query phase recv attempt %u.%u.\n",
                        label, (unsigned int)(attempt + 1U),
                        (unsigned int)(receive_attempt + 1U));
                fflush(stream);
#endif
                net_status = tg_mtproto_recv_abridged_packet(
                    &context->connection, response, sizeof(response),
                    &response_length, error_buffer, sizeof(error_buffer));
            }
            if (net_status == TG_NET_TIMEOUT) {
                /* No data within the per-recv timeout, and nothing was consumed
                   from the stream: keep polling within the wall-clock budget
                   (checked at the top of this loop) instead of failing the whole
                   query on the first quiet interval. This is the common case
                   while waiting for an rpc_result on a slow link; without it a
                   single quiet recv surfaced as "Could not send message". A
                   genuinely wedged session still soft-fails once the budget is
                   spent, and the chat loop's reconnect-on-stall recovers it. */
                net_status = TG_NET_OK;
                continue;
            }
            if (net_status != TG_NET_OK) {
                sprintf(tg_mtproto_query_fail, "transport %.40s",
                        tg_net_status_name(net_status));
                fprintf(stream, "%s: transport-failed (%s)\n", label,
                        tg_net_status_name(net_status));
                return 2;
            }
#ifdef TG_MTPROTO_DIAG
            fprintf(stream, "%s: encrypted query phase decrypt bytes %lu.\n",
                    label, response_length);
            fflush(stream);
#endif
#ifdef TG_MTPROTO_DIAG
            fprintf(stream,
                    "%s: diag frame len=%lu mod16=%lu key_id_lo=0x%08lx\n",
                    label, response_length,
                    response_length >= 24UL ?
                        (response_length - 24UL) % 16UL : 0UL,
                    response_length >= 4UL ?
                        tg_mtproto_read_u32_le(response) : 0UL);
            fflush(stream);
#endif
            if (tg_mtproto_decrypt_encrypted_message(response, response_length,
                                                     context->auth_key,
                                                     &decrypted) !=
                TG_MTPROTO_TL_OK) {
                fprintf(stream, "%s: encrypted-response-decrypt-failed\n",
                        label);
                return 2;
            }
            /* Keep the server-time delta current on every decrypted server
               message, not just on bad_msg 16/17. A mid-session reconnect zeroes
               the context (delta=0), so without this an OS3 box with a drifting
               clock would keep emitting msg_ids outside the +/-300s window and
               the first post-reconnect query would fail. */
            tg_mtproto_sync_time_from_server(context, &decrypted);
#ifdef TG_MTPROTO_DIAG
            fprintf(stream, "%s: encrypted query phase parse body %lu.\n",
                    label, decrypted.body_length);
            fflush(stream);
#endif
#ifdef TG_MTPROTO_DIAG
            fprintf(stream, "%s: diag body constructor 0x%08lx\n", label,
                    decrypted.body_length >= 4UL ?
                        tg_mtproto_read_u32_le(decrypted.body) : 0UL);
            fflush(stream);
#endif
            if (tg_mtproto_find_bad_msg(decrypted.body, decrypted.body_length,
                                        request_msg_id.hi, request_msg_id.lo,
                                        &bad_msg)) {
#ifdef TG_MTPROTO_DIAG
                fprintf(stream,
                        "%s: diag bad-msg code=%lu seqno=%lu salt=%d\n",
                        label, bad_msg.error_code, bad_msg.bad_msg_seqno,
                        bad_msg.has_new_server_salt);
                fflush(stream);
#endif
                if (bad_msg.has_new_server_salt &&
                    bad_msg.error_code == 48UL) {
                    context->session.server_salt_hi =
                        bad_msg.new_server_salt_hi;
                    context->session.server_salt_lo =
                        bad_msg.new_server_salt_lo;
                    retry_request = 1;
                    break;
                }
                if (bad_msg.error_code == 32UL) {
                    context->session.seq_no += 2UL;
                    retry_request = 1;
                    break;
                }
                if (bad_msg.error_code == 33UL &&
                    context->session.seq_no >= 2UL) {
                    context->session.seq_no -= 2UL;
                    retry_request = 1;
                    break;
                }
                if (bad_msg.error_code == 16UL ||
                    bad_msg.error_code == 17UL) {
                    tg_mtproto_sync_time_from_server(context, &decrypted);
                    retry_request = 1;
                    break;
                }
                fprintf(stream, "%s: bad-msg error-code %lu\n", label,
                        bad_msg.error_code);
                return 2;
            }
            if (tg_platform_break_pending()) {
                fprintf(stream, "%s: user-break\n", label);
                return 2;
            }
            tg_mtproto_ack_encrypted_message(context, &decrypted, stream,
                                             label);
            /* Harvest cross-chat new-message updates that used to be
               discarded right below (no-op unless the chat armed it). */
            tg_chat_notify_collect(decrypted.body, decrypted.body_length);
            if (tg_mtproto_find_rpc_result(decrypted.body,
                                           decrypted.body_length,
                                           request_msg_id.hi,
                                           request_msg_id.lo,
                                           rpc_result)) {
#ifdef TG_MTPROTO_DIAG
                fprintf(stream,
                        "%s: encrypted query phase rpc result found.\n",
                        label);
                fflush(stream);
#endif
                return 0;
            }
            response_constructor = decrypted.body_length >= 4UL ?
                tg_mtproto_read_u32_le(decrypted.body) : 0UL;
            if (response_constructor == TG_MTPROTO_RPC_RESULT_CONSTRUCTOR) {
                /* This is a late result for an older request. Continuing on the
                   same persistent stream can block forever on fragile/slow
                   bsdsocket stacks while the current result sits behind stale
                   traffic. Surface a soft failure: the context caller already
                   closes the socket, discarding both the stale result and the
                   in-flight query before retrying cleanly. */
                sprintf(tg_mtproto_query_fail, "stale rpc result");
                return TG_MTPROTO_QUERY_SOFT_FAIL;
            }
            if (tg_mtproto_is_async_update_constructor(response_constructor)) {
                continue;
            }
            if (response_constructor ==
                    TG_MTPROTO_BAD_MSG_NOTIFICATION_CONSTRUCTOR ||
                response_constructor == TG_MTPROTO_BAD_SERVER_SALT_CONSTRUCTOR) {
                continue;
            }
            if (response_constructor ==
                    TG_MTPROTO_GZIP_PACKED_CONSTRUCTOR) {
                /* A standalone push -- typically `updates` carrying a LONG
                   incoming message -- arrives gzip-compressed at the top
                   level of the decrypted body. The notify harvest above
                   already unpacks it; here it only has to be recognised as
                   an async update instead of being rejected, which turned
                   any send that raced such a push into "Could not send
                   message" (field report 2026-08-06, ctor 0x3072cfa1). */
                const unsigned char *unpacked;
                unsigned long unpacked_length;

                if (tg_chat_notify_gunzip(decrypted.body,
                                          decrypted.body_length,
                                          &unpacked, &unpacked_length) &&
                    unpacked_length >= 4UL &&
                    tg_mtproto_read_u32_le(unpacked) ==
                        TG_MTPROTO_RPC_RESULT_CONSTRUCTOR) {
                    /* Our own result, compressed whole: unpacking it here
                       would alias the shared scratch buffer, so fail softly
                       and let the caller reopen and re-ask cleanly. */
                    sprintf(tg_mtproto_query_fail, "gzipped rpc result");
                    return TG_MTPROTO_QUERY_SOFT_FAIL;
                }
                continue;
            }
            if (response_constructor != TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR &&
                response_constructor != 0x9ec20908UL) {
                fprintf(stream,
                        "%s: encrypted-response-unexpected constructor 0x%08lx\n",
                        label, response_constructor);
                return 2;
            }
        }
        if (retry_request) {
            continue;
        }
        if (tg_mtproto_query_fail[0] == '\0') {
            sprintf(tg_mtproto_query_fail, "no rpc result in %u tries",
                    (unsigned int)(attempt + 1U));
        }
        fprintf(stream, "%s: rpc-response-not-received\n", label);
        return TG_MTPROTO_QUERY_SOFT_FAIL;
    }

    fprintf(stream, "%s: bad-msg-retry-failed\n", label);
    return TG_MTPROTO_QUERY_SOFT_FAIL;
}

#undef payload
#undef packet
#undef response
#undef decrypted

/* --- 0.0.8 punto 1d: the pipelined query pair (file channel only). --------
   send_query_noreply fires a query and returns; recv_rpc_result waits for
   ONE specific msg_id.
   Any anomaly (bad_msg, budget, transport, a reply for anything else)
   makes the caller close the connection -- that drains everything in
   flight and the proven 0.0.7 per-chunk retry takes over synchronously.
   Exactly ONE query is in flight at a time (the next chunk, prefetched
   while the current one lands), so there is nothing to queue or park. */

static int tg_mtproto_send_query_noreply(tg_mtproto_auth_context *context,
                                         const unsigned char *body,
                                         unsigned long body_length,
                                         tg_mtproto_message_id *out_msg_id,
                                         FILE *stream, const char *label)
{
    unsigned char encrypted_padding[64];
    unsigned long encrypted_padding_length;
    unsigned long payload_length;
    tg_mtproto_tl_writer writer;
    tg_net_status net_status;
    char error_buffer[160];

    if (context == 0 || !context->connection_open || body == 0 ||
        body_length == 0UL || out_msg_id == 0 || stream == 0 || label == 0) {
        return 2;
    }
    tg_mtproto_query_fail[0] = '\0';
    encrypted_padding_length = 12UL;
    while (((32UL + body_length + encrypted_padding_length) % 16UL) != 0UL) {
        ++encrypted_padding_length;
    }
    tg_mtproto_saved_session_random(encrypted_padding,
                                    encrypted_padding_length);
    tg_mtproto_client_message_id(tg_mtproto_context_time(context), 16UL,
                                 &context->last_msg_id, out_msg_id);
    context->last_msg_id = *out_msg_id;
    context->session.last_msg_id_hi = out_msg_id->hi;
    context->session.last_msg_id_lo = out_msg_id->lo;
    tg_mtproto_tl_writer_init(&writer, tg_mtproto_q_payload,
                              sizeof(tg_mtproto_q_payload));
    if (tg_mtproto_write_encrypted_message(
            &writer, context->auth_key, context->session.server_salt_hi,
            context->session.server_salt_lo, context->session.session_id,
            out_msg_id->hi, out_msg_id->lo, context->session.seq_no, body,
            body_length, encrypted_padding,
            encrypted_padding_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: pipe-build-failed\n", label);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, tg_mtproto_q_packet,
                              sizeof(tg_mtproto_q_packet));
    if (tg_mtproto_write_abridged_packet(&writer, tg_mtproto_q_payload,
                                         payload_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: pipe-transport-build-failed\n", label);
        return 2;
    }
    error_buffer[0] = '\0';
    net_status = tg_mtproto_send_all(&context->connection,
                                     tg_mtproto_q_packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        sprintf(tg_mtproto_query_fail, "send %.40s",
                tg_net_status_name(net_status));
        fprintf(stream, "%s: pipe-send-failed (%s)\n", label,
                tg_net_status_name(net_status));
        return 2;
    }
    context->session.seq_no += 2UL; /* consumed by SENDING, as ever */
    return 0;
}

/* 0 = wanted delivered; 1 = idle budget hit (reason set); 2 = transport or
   decrypt trouble; 3 = pipe-broken (a bad_msg needs a RESEND, which the
   split model cannot do -- the caller reconnects and retries). */
static int tg_mtproto_recv_rpc_result(tg_mtproto_auth_context *context,
                                      const tg_mtproto_message_id *wanted,
                                      tg_mtproto_rpc_result *rpc_result,
                                      FILE *stream, const char *label,
                                      unsigned long query_budget_seconds)
{
    unsigned long response_length;
    unsigned long query_start_time;
    unsigned long rx_seen;
    unsigned long response_constructor;
    tg_mtproto_bad_msg_notification bad_msg;
    tg_net_status net_status;
    char error_buffer[160];

    if (context == 0 || !context->connection_open || wanted == 0 ||
        rpc_result == 0 || stream == 0 || label == 0) {
        return 2;
    }
    if (query_budget_seconds == 0UL) {
        query_budget_seconds = TG_MTPROTO_QUERY_BUDGET_SECONDS;
    }
    query_start_time = (unsigned long)time(0);
    rx_seen = tg_mtproto_rx_progress;
    for (;;) {
        if (tg_mtproto_rx_progress != rx_seen) {
            rx_seen = tg_mtproto_rx_progress;
            query_start_time = (unsigned long)time(0); /* idle budget */
        }
        if ((unsigned long)time(0) - query_start_time >=
                query_budget_seconds) {
            sprintf(tg_mtproto_query_fail, "no data for %lus",
                    query_budget_seconds);
            return 1;
        }
        net_status = tg_mtproto_recv_abridged_packet(
            &context->connection, tg_mtproto_q_response,
            sizeof(tg_mtproto_q_response), &response_length, error_buffer,
            sizeof(error_buffer));
        if (net_status == TG_NET_TIMEOUT) {
            continue; /* quiet interval: budget above decides */
        }
        if (net_status != TG_NET_OK) {
            sprintf(tg_mtproto_query_fail, "transport %.40s",
                    tg_net_status_name(net_status));
            fprintf(stream, "%s: pipe-transport-failed (%s)\n", label,
                    tg_net_status_name(net_status));
            return 2;
        }
        if (tg_mtproto_decrypt_encrypted_message(
                tg_mtproto_q_response, response_length, context->auth_key,
                &tg_mtproto_q_decrypted) != TG_MTPROTO_TL_OK) {
            fprintf(stream, "%s: pipe-decrypt-failed\n", label);
            return 2;
        }
        tg_mtproto_sync_time_from_server(context, &tg_mtproto_q_decrypted);
        if (tg_mtproto_find_bad_msg(tg_mtproto_q_decrypted.body,
                                    tg_mtproto_q_decrypted.body_length,
                                    wanted->hi, wanted->lo, &bad_msg)) {
            /* Salt/seq/time fixes need a RESEND, which the split model
               cannot do: apply what is applicable and report pipe-broken. */
            if (bad_msg.has_new_server_salt && bad_msg.error_code == 48UL) {
                context->session.server_salt_hi = bad_msg.new_server_salt_hi;
                context->session.server_salt_lo = bad_msg.new_server_salt_lo;
            }
            sprintf(tg_mtproto_query_fail, "pipe bad-msg %lu",
                    bad_msg.error_code);
            return 3;
        }
        tg_mtproto_ack_encrypted_message(context, &tg_mtproto_q_decrypted,
                                         stream, label);
        tg_chat_notify_collect(tg_mtproto_q_decrypted.body,
                               tg_mtproto_q_decrypted.body_length);
        if (tg_mtproto_find_rpc_result(tg_mtproto_q_decrypted.body,
                                       tg_mtproto_q_decrypted.body_length,
                                       wanted->hi, wanted->lo, rpc_result)) {
            return 0;
        }
        response_constructor = tg_mtproto_q_decrypted.body_length >= 4UL ?
            tg_mtproto_read_u32_le(tg_mtproto_q_decrypted.body) : 0UL;
        if (response_constructor == TG_MTPROTO_RPC_RESULT_CONSTRUCTOR ||
            tg_mtproto_is_async_update_constructor(response_constructor) ||
            response_constructor ==
                TG_MTPROTO_BAD_MSG_NOTIFICATION_CONSTRUCTOR ||
            response_constructor == TG_MTPROTO_BAD_SERVER_SALT_CONSTRUCTOR ||
            response_constructor == TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR ||
            response_constructor == 0x9ec20908UL) {
            continue; /* push/ack/other traffic: same tolerance as classic */
        }
        fprintf(stream, "%s: pipe-unexpected constructor 0x%08lx\n", label,
                response_constructor);
        return 2;
    }
}


static int tg_mtproto_send_encrypted_query(
    tg_mtproto_auth_context *context,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_rpc_result *rpc_result,
    FILE *stream,
    const char *label)
{
    return tg_mtproto_send_encrypted_query_limited(
        context, body, body_length, rpc_result, stream, label, 32U,
        TG_MTPROTO_QUERY_BUDGET_SECONDS);
}

static int tg_mtproto_send_encrypted_query_login(
    tg_mtproto_auth_context *context,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_rpc_result *rpc_result,
    FILE *stream,
    const char *label)
{
    return tg_mtproto_send_encrypted_query_limited(
        context, body, body_length, rpc_result, stream, label, 96U,
        TG_MTPROTO_LOGIN_QUERY_BUDGET_SECONDS);
}

/*
 * 2FA/SRP progress: installed as the crypto progress hook so the heavy
 * PBKDF2 + modpow work (slow on 68080) animates a dot loader instead of looking
 * frozen. Single-threaded: the active stream is held in a file-static pointer.
 */
static FILE *tg_login_progress_stream = 0;

static void tg_login_progress_dot(void)
{
    if (tg_login_progress_stream != 0) {
        fputc('.', tg_login_progress_stream);
        fflush(tg_login_progress_stream);
    }
}

/*
 * Login/auth progress indicator. Under TG_MTPROTO_DIAG it prints the full phase
 * line (useful when debugging the handshake); in a normal build it prints a
 * single progress dot, so the login shows a quiet semi-animated loader instead
 * of raw "mtproto ... phase ..." logs.
 */
/*
 * Tell the user WHERE Telegram delivered the login code.
 *
 * On accounts that already have a Telegram session somewhere, the server
 * picks `auth.sentCodeTypeApp` and pushes the code through those clients
 * instead of an SMS. Without this hint the user keeps waiting for a text
 * message that will never arrive (the AROS / desktop-only setup hits this).
 */
/* Where Telegram put the login code, in two lengths from ONE table: the
   console can afford a sentence, the GUI status line is 48 bytes. Two
   tables would drift the first time Telegram adds a delivery type. */
static const char *tg_mtproto_sent_code_text(unsigned long type_constructor,
                                             int brief)
{
    switch (type_constructor) {
    case 0x3dbb5986UL: /* auth.sentCodeTypeApp */
        return brief ? "Code sent in Telegram on your phone"
                     : "Check your other Telegram apps (mobile/desktop/web)"
                       " for the code.";
    case 0xc000bba2UL: /* auth.sentCodeTypeSms */
    case 0xa416ac81UL: /* auth.sentCodeTypeSmsWord */
    case 0xb37794afUL: /* auth.sentCodeTypeSmsPhrase */
    case 0xd9565c39UL: /* auth.sentCodeTypeFragmentSms */
    case 0x009fd736UL: /* auth.sentCodeTypeFirebaseSms */
        return brief ? "Code sent by SMS"
                     : "Telegram is sending the code by SMS to your phone.";
    case 0x5353e5a7UL: /* auth.sentCodeTypeCall */
        return brief ? "Telegram is calling with the code"
                     : "Telegram will call your phone and speak the code.";
    case 0xab03c6d9UL: /* auth.sentCodeTypeFlashCall */
    case 0x82006484UL: /* auth.sentCodeTypeMissedCall */
        return brief ? "Read the code from the incoming call"
                     : "Telegram is calling: the last digits of the caller ID"
                       " are the code.";
    case 0xf450f59bUL: /* auth.sentCodeTypeEmailCode */
        return brief ? "Code sent to your email"
                     : "Check your email for the code.";
    default:
        /* Unknown delivery type: the GUI still needs a prompt, the console
           stays silent rather than mislead. */
        return brief ? "Enter the code you received" : 0;
    }
}

static void tg_mtproto_print_login_code_hint(FILE *stream,
                                             unsigned long type_constructor)
{
    const char *hint;

    if (stream == 0) {
        return;
    }
    hint = tg_mtproto_sent_code_text(type_constructor, 0);
    if (hint == 0) {
        return;
    }
    fprintf(stream, "%s\n", hint);
}


static void tg_mtproto_login_phase(FILE *stream, const char *phase)
{
    if (stream == 0) {
        return;
    }
#ifdef TG_MTPROTO_DIAG
    if (phase != 0) {
        fprintf(stream, "mtproto login: %s\n", phase);
    }
#else
    /* Human build: the login is fast now (WaitSelect recv + DC4 bootstrap), so
       the per-phase progress strings ("Contacting Telegram", "Preparing secure
       login key", ...) are just noise. Emit one dot per phase instead; the
       verbose per-phase names remain available in the TG_MTPROTO_DIAG build
       above for development. */
    (void)phase;
    fputc('.', stream);
#endif
    fflush(stream);
}

static int tg_mtproto_open_auth_context(const char *host,
                                        const char *port,
                                        const char *dc_id_text,
                                        tg_mtproto_auth_context *context,
                                        FILE *stream,
                                        const char *label)
{
    unsigned char nonce[16];
    unsigned char new_nonce[32];
    unsigned char padding[96];
    unsigned char temp_key[32];
    unsigned char p_bytes[4];
    unsigned char q_bytes[4];
    unsigned char inner_data[160];
    unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH];
    unsigned char client_encrypted[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned char b[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char client_padding[15];
    unsigned char session_id[8];
    unsigned char body[384];
    unsigned char payload[512];
    unsigned char packet[600];
    unsigned char response[1200];
    unsigned long body_length;
    unsigned long client_encrypted_length;
    unsigned long payload_length;
    unsigned long response_length;
    unsigned long constructor;
    unsigned long p;
    unsigned long q;
    unsigned int i;
    long dc_id;
    tg_mtproto_message_id first_msg_id;
    tg_mtproto_message_id second_msg_id;
    tg_mtproto_message_id third_msg_id;
    tg_mtproto_res_pq res_pq;
    tg_mtproto_server_dh_params_ok params_ok;
    tg_mtproto_server_dh_inner_data inner;
    tg_mtproto_set_client_dh_answer dh_answer;
    tg_mtproto_tl_writer writer;
    tg_net_status net_status;
    const tg_mtproto_public_key *public_key;
    char error_buffer[160];

    if (host == 0 || port == 0 || context == 0 || stream == 0 ||
        label == 0 || tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0) {
        fprintf(stream, "%s: invalid-arguments\n", label);
        return 2;
    }
    memset(context, 0, sizeof(*context));

    tg_mtproto_login_phase(stream, "auth-key rng");
    memset(b, 0, sizeof(b));
    if (!tg_mtproto_secure_random(nonce, sizeof(nonce)) ||
        !tg_mtproto_secure_random(new_nonce, sizeof(new_nonce)) ||
        !tg_mtproto_secure_random(padding, sizeof(padding)) ||
        !tg_mtproto_secure_random(temp_key, sizeof(temp_key)) ||
        !tg_mtproto_secure_random(
            b + TG_MTPROTO_DH_VALUE_MAX -
                TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES,
            TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES) ||
        !tg_mtproto_secure_random(client_padding, sizeof(client_padding)) ||
        !tg_mtproto_secure_random(session_id, sizeof(session_id))) {
        fprintf(stream, "%s: secure-rng-unavailable\n", label);
        return 2;
    }
    b[TG_MTPROTO_DH_VALUE_MAX - TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES] |= 0x80U;

    tg_mtproto_login_phase(stream, "auth-key req_pq");
    tg_mtproto_client_message_id((unsigned long)time(0), 4UL, 0,
                                 &first_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_build_req_pq_multi(&writer, first_msg_id.hi,
                                      first_msg_id.lo, nonce) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: req-pq-build-failed\n", label);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
            TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: req-pq-transport-build-failed\n", label);
        return 2;
    }

    error_buffer[0] = '\0';
    tg_mtproto_login_phase(stream, "auth-key connect");
    net_status = tg_net_connect(&context->connection, host, port, error_buffer,
                                sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: connect-failed (%s)\n", label,
                tg_net_status_name(net_status));
        return 2;
    }
    context->connection_open = 1;

    tg_mtproto_login_phase(stream, "auth-key res_pq");
    net_status = tg_mtproto_send_all(&context->connection, packet,
                                     writer.length, error_buffer,
                                     sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(
            &context->connection, response, sizeof(response), &response_length,
            error_buffer, sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: req-pq-failed (%s)\n", label,
                tg_net_status_name(net_status));
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    constructor = response_length >= 24UL ?
        tg_mtproto_read_u32_le(response + 20) : 0UL;
    if (constructor != 0x05162463UL ||
        tg_mtproto_parse_res_pq(response, response_length, &res_pq) !=
            TG_MTPROTO_TL_OK ||
        !tg_mtproto_res_pq_nonce_matches(&res_pq, nonce) ||
        tg_mtproto_pq_factor(res_pq.pq, res_pq.pq_length, &p, &q) != 0) {
        fprintf(stream, "%s: res-pq-parse-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    public_key = tg_mtproto_select_public_key(&res_pq);
    if (public_key == 0) {
        fprintf(stream, "%s: rsa-key-not-found\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth-key req_DH");
    tg_mtproto_u32_be(p, p_bytes);
    tg_mtproto_u32_be(q, q_bytes);
    tg_mtproto_tl_writer_init(&writer, inner_data, sizeof(inner_data));
    if (tg_mtproto_build_p_q_inner_data_dc(&writer, res_pq.pq,
                                           res_pq.pq_length, p_bytes,
                                           sizeof(p_bytes), q_bytes,
                                           sizeof(q_bytes), nonce,
                                           res_pq.server_nonce, new_nonce,
                                           dc_id) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: inner-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    for (i = 0U; i < 32U; ++i) {
        if (tg_mtproto_rsa_pad(inner_data, writer.length, padding, temp_key,
                               public_key, encrypted_data) ==
            TG_MTPROTO_TL_OK) {
            break;
        }
        if (!tg_mtproto_secure_random(temp_key, sizeof(temp_key))) {
            fprintf(stream, "%s: secure-rng-unavailable\n", label);
            tg_mtproto_close_auth_context(context);
            return 2;
        }
    }
    if (i == 32U) {
        fprintf(stream, "%s: rsa-pad-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_build_req_dh_params(&writer, nonce, res_pq.server_nonce,
                                       p_bytes, sizeof(p_bytes), q_bytes,
                                       sizeof(q_bytes),
                                       &public_key->fingerprint,
                                       encrypted_data) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: req-dh-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    body_length = writer.length;
    tg_mtproto_client_message_id((unsigned long)time(0), 8UL, &first_msg_id,
                                 &second_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_plain_message(&writer, second_msg_id.hi,
                                       second_msg_id.lo, body,
                                       body_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: req-dh-envelope-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: req-dh-transport-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth-key server_DH");
    net_status = tg_mtproto_send_all(&context->connection, packet,
                                     writer.length, error_buffer,
                                     sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(
            &context->connection, response, sizeof(response), &response_length,
            error_buffer, sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: req-dh-failed (%s)\n", label,
                tg_net_status_name(net_status));
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    constructor = response_length >= 24UL ?
        tg_mtproto_read_u32_le(response + 20) : 0UL;
    if (constructor != 0xd0e8075cUL ||
        tg_mtproto_parse_server_dh_params_ok(response, response_length,
                                             &params_ok) != TG_MTPROTO_TL_OK ||
        memcmp(params_ok.nonce, nonce, 16U) != 0 ||
        memcmp(params_ok.server_nonce, res_pq.server_nonce, 16U) != 0 ||
        tg_mtproto_decrypt_server_dh_inner_data(
            params_ok.encrypted_answer, params_ok.encrypted_answer_length,
            new_nonce, nonce, res_pq.server_nonce, &inner) !=
            TG_MTPROTO_TL_OK) {
        fprintf(stream,
                "%s: server-dh-parse-failed response-bytes %lu first-word 0x%08lx constructor 0x%08lx\n",
                label, response_length,
                response_length >= 4UL ? tg_mtproto_read_u32_le(response) : 0UL,
                constructor);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    if (!tg_mtproto_check_dh_params(&inner)) {
        fprintf(stream, "%s: dh-params-check-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth-key client_DH");
    if (tg_mtproto_build_client_dh_request(&inner, new_nonce, b,
                                           client_padding, client_encrypted,
                                           &client_encrypted_length,
                                           context->auth_key) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: client-dh-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_build_set_client_dh_params(
            &writer, nonce, res_pq.server_nonce, client_encrypted,
            client_encrypted_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: set-client-dh-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    body_length = writer.length;
    tg_mtproto_client_message_id((unsigned long)time(0), 12UL,
                                 &second_msg_id, &third_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_plain_message(&writer, third_msg_id.hi,
                                       third_msg_id.lo, body,
                                       body_length) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: set-client-envelope-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: set-client-transport-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    tg_mtproto_login_phase(stream, "auth-key set_client_DH");
    net_status = tg_mtproto_send_all(&context->connection, packet,
                                     writer.length, error_buffer,
                                     sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(
            &context->connection, response, sizeof(response), &response_length,
            error_buffer, sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: set-client-dh-failed (%s)\n", label,
                tg_net_status_name(net_status));
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    if (tg_mtproto_parse_set_client_dh_answer(response, response_length,
                                              &dh_answer) !=
        TG_MTPROTO_TL_OK ||
        !tg_mtproto_verify_dh_gen_ok(&dh_answer, nonce, res_pq.server_nonce,
                                     new_nonce, context->auth_key)) {
        fprintf(stream, "%s: dh-gen-not-ok\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth-key ready");
    tg_mtproto_session_from_auth_key(&context->session, (unsigned long)dc_id,
                                     context->auth_key, new_nonce,
                                     res_pq.server_nonce,
                                     session_id);
    context->session.seq_no = 1UL;
    context->last_msg_id = third_msg_id;
    context->session.last_msg_id_hi = third_msg_id.hi;
    context->session.last_msg_id_lo = third_msg_id.lo;
    /* Seed the server-time offset from the handshake's server_time so the very
       first encrypted query already carries a server-aligned msg_id. Without
       this, a client whose local clock is wrong (notably AmigaOS4 on emulated
       PPC, which drifts and resets to the 1978 epoch) sends the first query
       with a stale msg_id and the server answers bad_msg_notification 16/17.
       With a correct clock the delta is ~0, so other platforms are unaffected. */
    {
        unsigned long handshake_now = (unsigned long)time(0);
        if ((unsigned long)inner.server_time >= handshake_now) {
            context->server_time_delta_seconds =
                (long)((unsigned long)inner.server_time - handshake_now);
        } else {
            context->server_time_delta_seconds =
                -(long)(handshake_now - (unsigned long)inner.server_time);
        }
    }
    return 0;
}

static int tg_mtproto_load_auth_context(const char *host,
                                        const char *port,
                                        const char *auth_file,
                                        tg_mtproto_auth_context *context,
                                        FILE *stream,
                                        const char *label)
{
    tg_mtproto_session_status session_status;
    tg_net_status net_status;
    tg_mtproto_tl_writer writer;
    unsigned char init_packet[1];
    char error_buffer[160];

    if (host == 0 || port == 0 || auth_file == 0 || context == 0 ||
        stream == 0 || label == 0) {
        fprintf(stream, "%s: invalid-arguments\n", label);
        return 2;
    }
    memset(context, 0, sizeof(*context));
    session_status = tg_mtproto_session_load_authorization(
        auth_file, &context->session, context->auth_key);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-load-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }

    tg_mtproto_refresh_saved_session(context);
#ifdef TG_MTPROTO_DIAG
    fprintf(stderr, "notify-diag ctx-open label=%s\n", label);
#endif
    error_buffer[0] = '\0';
    net_status = tg_net_connect(&context->connection, host, port, error_buffer,
                                sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: connect-failed (%s)\n", label,
                tg_net_status_name(net_status));
        return 2;
    }
    context->connection_open = 1;
    tg_mtproto_tl_writer_init(&writer, init_packet, sizeof(init_packet));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: transport-init-build-failed\n", label);
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    net_status = tg_mtproto_send_all(&context->connection, init_packet,
                                     writer.length, error_buffer,
                                     sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "%s: transport-init-failed (%s)\n", label,
                tg_net_status_name(net_status));
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    return 0;
}

static void tg_mtproto_trim_line(char *text)
{
    unsigned long length;

    if (text == 0) {
        return;
    }
    length = (unsigned long)strlen(text);
    while (length > 0UL &&
           (text[length - 1U] == '\n' || text[length - 1U] == '\r' ||
            text[length - 1U] == ' ' || text[length - 1U] == '\t')) {
        text[length - 1U] = '\0';
        --length;
    }
}

static void tg_mtproto_trim_newline(char *text)
{
    unsigned long length;

    if (text == 0) {
        return;
    }
    length = (unsigned long)strlen(text);
    while (length > 0UL &&
           (text[length - 1U] == '\n' || text[length - 1U] == '\r')) {
        text[length - 1U] = '\0';
        --length;
    }
}

static void tg_mtproto_secure_zero(void *data, unsigned long length)
{
    volatile unsigned char *bytes;

    bytes = (volatile unsigned char *)data;
    while (length > 0UL) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

static int tg_mtproto_load_password_file(const char *path,
                                         char *password,
                                         unsigned long password_size,
                                         unsigned long *password_length,
                                         FILE *stream,
                                         const char *label)
{
    tg_file_status file_status;

    if (password_length != 0) {
        *password_length = 0UL;
    }
    if (path == 0 || password == 0 || password_size == 0UL ||
        password_length == 0) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: password-file-invalid\n", label);
        }
        return 2;
    }
    file_status = tg_file_read_text(path, password, password_size,
                                    password_length);
    if (file_status == TG_FILE_TOO_LARGE) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: password-file-too-large\n", label);
        }
        return 2;
    }
    if (file_status != TG_FILE_OK) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: password-file-load-failed (%s)\n", label,
                    tg_file_status_name(file_status));
        }
        return 2;
    }
    tg_mtproto_trim_newline(password);
    *password_length = (unsigned long)strlen(password);
    if (*password_length == 0UL) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: password-file-empty\n", label);
        }
        tg_mtproto_secure_zero(password, password_size);
        return 2;
    }
    return 0;
}

static int tg_mtproto_prompt_line(const char *prompt,
                                  char *out,
                                  unsigned long out_size,
                                  int required,
                                  FILE *stream,
                                  const char *label)
{
    if (out != 0 && out_size > 0UL) {
        out[0] = '\0';
    }
    if (prompt == 0 || out == 0 || out_size == 0UL || stream == 0 ||
        label == 0) {
        return 2;
    }
    fputs(prompt, stream);
    fflush(stream);
    if (fgets(out, (int)out_size, stdin) == 0) {
        fprintf(stream, "%s: input-closed\n", label);
        return 2;
    }
    tg_mtproto_trim_line(out);
    if (required && out[0] == '\0') {
        fprintf(stream, "%s: input-empty\n", label);
        return 2;
    }
    return 0;
}

/* Reads a secret (2FA password) without echoing it where the console allows it. */
static int tg_mtproto_prompt_hidden_line(const char *prompt,
                                         char *out,
                                         unsigned long out_size,
                                         FILE *stream,
                                         const char *label)
{
    if (out != 0 && out_size > 0UL) {
        out[0] = '\0';
    }
    if (prompt == 0 || out == 0 || out_size == 0UL || stream == 0 ||
        label == 0) {
        return 2;
    }
    fputs(prompt, stream);
    fflush(stream);
    if (tg_platform_stdin_read_hidden_line(out, out_size) != 0) {
        fprintf(stream, "%s: input-closed\n", label);
        return 2;
    }
    fputc('\n', stream);    /* the typed Return was not echoed */
    fflush(stream);
    tg_mtproto_trim_line(out);
    return 0;
}

/* Phone numbers arrive through console keymaps and VNC bridges that can
   inject formatting or stray glyphs; Telegram wants the international
   number as bare digits. Keep digits, drop everything else ('+' included:
   the API accepts the prefix-less form). */
static void tg_mtproto_sanitize_phone(char *phone)
{
    unsigned long read_index;
    unsigned long write_index;

    if (phone == 0) {
        return;
    }
    write_index = 0UL;
    for (read_index = 0UL; phone[read_index] != '\0'; ++read_index) {
        if (phone[read_index] >= '0' && phone[read_index] <= '9') {
            phone[write_index] = phone[read_index];
            ++write_index;
        }
    }
    phone[write_index] = '\0';
}

static void tg_mtproto_copy_trimmed_field(const char *source,
                                          unsigned long source_length,
                                          char *out,
                                          unsigned long out_size)
{
    unsigned long start;
    unsigned long end;
    unsigned long length;

    if (out == 0 || out_size == 0UL) {
        return;
    }
    out[0] = '\0';
    if (source == 0) {
        return;
    }
    start = 0UL;
    while (start < source_length &&
           (source[start] == ' ' || source[start] == '\t')) {
        ++start;
    }
    end = source_length;
    while (end > start &&
           (source[end - 1UL] == ' ' || source[end - 1UL] == '\t' ||
            source[end - 1UL] == '\r' || source[end - 1UL] == '\n')) {
        --end;
    }
    length = end - start;
    if (length >= out_size) {
        length = out_size - 1UL;
    }
    if (length > 0UL) {
        memcpy(out, source + start, (size_t)length);
    }
    out[length] = '\0';
}

static int tg_mtproto_load_api_credentials(const char *path,
                                           char *api_id,
                                           unsigned long api_id_size,
                                           char *api_hash,
                                           unsigned long api_hash_size,
                                           FILE *stream,
                                           const char *label)
{
    char text[256];
    unsigned long text_length;
    unsigned long offset;
    unsigned long line_start;
    unsigned int field;
    tg_file_status file_status;

    if (api_id != 0 && api_id_size > 0UL) {
        api_id[0] = '\0';
    }
    if (api_hash != 0 && api_hash_size > 0UL) {
        api_hash[0] = '\0';
    }
    if (path == 0 || api_id == 0 || api_hash == 0 ||
        api_id_size == 0UL || api_hash_size == 0UL) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-invalid\n", label);
        }
        return 2;
    }

    file_status = tg_file_read_text(path, text, sizeof(text), &text_length);
    if (file_status == TG_FILE_TOO_LARGE) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-too-large\n", label);
        }
        return 2;
    }
    if (file_status != TG_FILE_OK) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-load-failed (%s)\n", label,
                    tg_file_status_name(file_status));
        }
        return 2;
    }

    field = 0U;
    offset = 0UL;
    while (offset <= text_length && field < 2U) {
        line_start = offset;
        while (offset < text_length && text[offset] != '\n' &&
               text[offset] != '\r') {
            ++offset;
        }
        if (offset > line_start) {
            if (field == 0U) {
                tg_mtproto_copy_trimmed_field(text + line_start,
                                              offset - line_start,
                                              api_id, api_id_size);
                if (api_id[0] != '\0') {
                    ++field;
                }
            } else {
                tg_mtproto_copy_trimmed_field(text + line_start,
                                              offset - line_start,
                                              api_hash, api_hash_size);
                if (api_hash[0] != '\0') {
                    ++field;
                }
            }
        }
        while (offset < text_length &&
               (text[offset] == '\n' || text[offset] == '\r')) {
            ++offset;
        }
        if (offset == text_length) {
            break;
        }
    }
    tg_mtproto_secure_zero(text, sizeof(text));
    if (api_id[0] == '\0' || api_hash[0] == '\0') {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-incomplete\n", label);
        }
        tg_mtproto_secure_zero(api_hash, api_hash_size);
        return 2;
    }
    return 0;
}

static int tg_mtproto_load_api_id_file(const char *path,
                                       char *api_id,
                                       unsigned long api_id_size,
                                       FILE *stream,
                                       const char *label)
{
    char text[256];
    unsigned long text_length;
    unsigned long offset;
    unsigned long line_start;
    tg_file_status file_status;

    if (api_id != 0 && api_id_size > 0UL) {
        api_id[0] = '\0';
    }
    if (path == 0 || api_id == 0 || api_id_size == 0UL) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-invalid\n", label);
        }
        return 2;
    }

    file_status = tg_file_read_text(path, text, sizeof(text), &text_length);
    if (file_status == TG_FILE_TOO_LARGE) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-too-large\n", label);
        }
        return 2;
    }
    if (file_status != TG_FILE_OK) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-load-failed (%s)\n", label,
                    tg_file_status_name(file_status));
        }
        return 2;
    }

    offset = 0UL;
    while (offset <= text_length) {
        line_start = offset;
        while (offset < text_length && text[offset] != '\n' &&
               text[offset] != '\r') {
            ++offset;
        }
        if (offset > line_start) {
            tg_mtproto_copy_trimmed_field(text + line_start,
                                          offset - line_start,
                                          api_id, api_id_size);
            if (api_id[0] != '\0') {
                break;
            }
        }
        while (offset < text_length &&
               (text[offset] == '\n' || text[offset] == '\r')) {
            ++offset;
        }
        if (offset == text_length) {
            break;
        }
    }
    tg_mtproto_secure_zero(text, sizeof(text));
    if (api_id[0] == '\0') {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: api-file-incomplete\n", label);
        }
        return 2;
    }
    return 0;
}

static int tg_mtproto_check_secret_file_permissions(const char *label,
                                                    const char *path,
                                                    FILE *stream)
{
#if defined(S_IRWXG) && defined(S_IRWXO)
    struct stat status;

    if (label == 0 || path == 0 || path[0] == '\0' || stream == 0) {
        return 0;
    }
    if (stat(path, &status) != 0) {
        return 0;
    }
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        fprintf(stream,
                "mtproto local-files: warning %s permissions are broad\n",
                label);
        return 1;
    }
#else
    (void)label;
    (void)path;
    (void)stream;
#endif
    return 0;
}

static int tg_mtproto_check_code_hash_file(const char *path,
                                           FILE *stream,
                                           const char *label)
{
    char text[256];
    unsigned long text_length;
    tg_file_status file_status;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    file_status = tg_file_read_text(path, text, sizeof(text), &text_length);
    if (file_status == TG_FILE_TOO_LARGE) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: code-hash-file-too-large\n", label);
        }
        return 2;
    }
    if (file_status != TG_FILE_OK) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: code-hash-file-load-failed (%s)\n", label,
                    tg_file_status_name(file_status));
        }
        return 2;
    }
    tg_mtproto_trim_newline(text);
    if (text[0] == '\0') {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: code-hash-file-empty\n", label);
        }
        return 2;
    }
    return 0;
}

static int tg_mtproto_print_rpc_error(const char *label,
                                      const tg_mtproto_rpc_result *result,
                                      FILE *stream)
{
    char error_message[128];
    long error_code;

    if (result == 0 || stream == 0 || label == 0 ||
        result->result_constructor != TG_MTPROTO_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_parse_rpc_error(result->result_body - 4U,
                                   result->result_body_length + 4U,
                                   &error_code, error_message,
                                   sizeof(error_message)) !=
            TG_MTPROTO_TL_OK) {
        return 0;
    }
    /* Keep the RPC name available to non-console front-ends. The query loop
       clears this buffer before each request, so a later status line cannot
       inherit an older operation's failure. */
    sprintf(tg_mtproto_query_fail, "%.63s", error_message);
    if (strcmp(error_message, "SESSION_PASSWORD_NEEDED") == 0 ||
        strcmp(error_message, "PHONE_PASSWORD_PROTECTED") == 0) {
        /* Expected during login: the wizard prints the human "2FA password
           required." line, so keep this technical marker out of normal builds. */
#ifdef TG_MTPROTO_DIAG
        fprintf(stream, "%s: two-factor-password-required\n", label);
#endif
    } else if (strcmp(error_message, "PASSWORD_HASH_INVALID") == 0) {
        fprintf(stream, "%s: password-invalid\n", label);
    } else if (strcmp(error_message, "SRP_ID_INVALID") == 0) {
        fprintf(stream, "%s: srp-id-invalid\n", label);
    } else if (strcmp(error_message, "PASSWORD_MISSING") == 0) {
        fprintf(stream, "%s: password-missing\n", label);
    } else if (strcmp(error_message, "AUTH_KEY_UNREGISTERED") == 0) {
        fprintf(stream, "%s: auth-key-unregistered\n", label);
    } else if (strcmp(error_message, "PEER_FLOOD") == 0) {
        fprintf(stream, "%s: telegram-refused-send (PEER_FLOOD)\n",
                label);
    } else {
        fprintf(stream, "%s: rpc-error %ld %s\n", label, error_code,
                error_message);
    }
    return 1;
}

static int tg_mtproto_rpc_phone_migrate_dc(
    const tg_mtproto_rpc_result *result,
    unsigned long *dc_id)
{
    char error_message[128];
    long error_code;

    if (result == 0 || dc_id == 0 ||
        result->result_constructor != TG_MTPROTO_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_parse_rpc_error(result->result_body - 4U,
                                   result->result_body_length + 4U,
                                   &error_code, error_message,
                                   sizeof(error_message)) !=
            TG_MTPROTO_TL_OK) {
        return 0;
    }
    (void)error_code;
    return tg_mtproto_parse_phone_migrate_dc(error_message, dc_id);
}

#if TG_ENABLE_GZIP_PUFF
static int tg_mtproto_gzip_skip_zero_string(const unsigned char *data,
                                            unsigned long length,
                                            unsigned long *offset)
{
    if (data == 0 || offset == 0 || *offset >= length) {
        return 2;
    }
    while (*offset < length && data[*offset] != 0U) {
        ++(*offset);
    }
    if (*offset >= length) {
        return 2;
    }
    ++(*offset);
    return 0;
}

static int tg_mtproto_gzip_unpack_puff(const unsigned char *packed_data,
                                       unsigned long packed_length,
                                       unsigned long *unpacked_length)
{
    unsigned long offset;
    unsigned long extra_length;
    unsigned long source_length;
    unsigned long dest_length;
    unsigned int flags;
    int rc;

    if (unpacked_length != 0) {
        *unpacked_length = 0UL;
    }
    if (packed_data == 0 || unpacked_length == 0 || packed_length < 18UL ||
        packed_data[0] != 0x1fU || packed_data[1] != 0x8bU ||
        packed_data[2] != 8U) {
        return 2;
    }

    flags = (unsigned int)packed_data[3];
    if ((flags & 0xe0U) != 0U) {
        return 2;
    }
    offset = 10UL;

    if ((flags & 4U) != 0U) {
        if (packed_length - offset < 2UL) {
            return 2;
        }
        extra_length = ((unsigned long)packed_data[offset]) |
                       (((unsigned long)packed_data[offset + 1U]) << 8);
        offset += 2UL;
        if (extra_length > packed_length - offset) {
            return 2;
        }
        offset += extra_length;
    }
    if ((flags & 8U) != 0U &&
        tg_mtproto_gzip_skip_zero_string(packed_data, packed_length,
                                         &offset) != 0) {
        return 2;
    }
    if ((flags & 16U) != 0U &&
        tg_mtproto_gzip_skip_zero_string(packed_data, packed_length,
                                         &offset) != 0) {
        return 2;
    }
    if ((flags & 2U) != 0U) {
        if (packed_length - offset < 2UL) {
            return 2;
        }
        offset += 2UL;
    }
    if (packed_length - offset < 8UL) {
        return 2;
    }

    source_length = packed_length - offset - 8UL;
    dest_length = TG_MTPROTO_GZIP_UNPACKED_MAX;
    rc = puff(tg_mtproto_gzip_unpacked, &dest_length,
              packed_data + offset, &source_length);
    if (rc != 0 || dest_length < 4UL) {
        return 2;
    }
    *unpacked_length = dest_length;
    return 0;
}
#endif

static int tg_mtproto_unpack_gzip_result(tg_mtproto_rpc_result *result,
                                         FILE *stream,
                                         const char *label)
{
    const unsigned char *packed_data;
    unsigned long packed_length;
    tg_mtproto_tl_reader reader;

    if (result == 0 || stream == 0 || label == 0 ||
        result->result_constructor != TG_MTPROTO_GZIP_PACKED_CONSTRUCTOR) {
        return 0;
    }

    tg_mtproto_tl_reader_init(&reader, result->result_body,
                              result->result_body_length);
    if (tg_mtproto_tl_read_bytes(&reader, &packed_data, &packed_length) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: gzip-packed-parse-failed\n", label);
        return 2;
    }

#if TG_ENABLE_GZIP
    {
        int zrc;
        z_stream zs;

        if (packed_length > (unsigned long)UINT_MAX ||
            TG_MTPROTO_GZIP_UNPACKED_MAX > (unsigned long)UINT_MAX) {
            fprintf(stream, "%s: gzip-packed-too-large\n", label);
            return 2;
        }

        memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef *)packed_data;
        zs.avail_in = (uInt)packed_length;
        zs.next_out = (Bytef *)tg_mtproto_gzip_unpacked;
        zs.avail_out = (uInt)TG_MTPROTO_GZIP_UNPACKED_MAX;

        zrc = inflateInit2(&zs, 16 + MAX_WBITS);
        if (zrc == Z_OK) {
            zrc = inflate(&zs, Z_FINISH);
            (void)inflateEnd(&zs);
        }
        if (zrc != Z_STREAM_END || zs.total_out < 4UL) {
            fprintf(stream, "%s: gzip-unpack-failed\n", label);
            return 2;
        }

        result->result_constructor =
            tg_mtproto_read_u32_le(tg_mtproto_gzip_unpacked);
        result->result_body = tg_mtproto_gzip_unpacked + 4;
        result->result_body_length = (unsigned long)zs.total_out - 4UL;
        return 0;
    }
#elif TG_ENABLE_GZIP_PUFF
    {
        unsigned long unpacked_length;

        if (tg_mtproto_gzip_unpack_puff(packed_data, packed_length,
                                        &unpacked_length) != 0) {
            fprintf(stream, "%s: gzip-unpack-failed\n", label);
            return 2;
        }

        result->result_constructor =
            tg_mtproto_read_u32_le(tg_mtproto_gzip_unpacked);
        result->result_body = tg_mtproto_gzip_unpacked + 4;
        result->result_body_length = unpacked_length - 4UL;
        return 0;
    }
#else
    (void)packed_data;
    (void)packed_length;
    fprintf(stream, "%s: gzip-packed-response-unsupported\n", label);
    return 2;
#endif
}

/* Best-effort, silent gunzip for update pushes (they arrive gzip-packed on
   busy accounts). Returns 1 and points out/out_length at the static unpack
   buffer on success. Failure (no inflater compiled in, oversize, corrupt)
   just drops the update: notifications are opportunistic. */
static int tg_chat_notify_gunzip(const unsigned char *body,
                                 unsigned long body_length,
                                 const unsigned char **out,
                                 unsigned long *out_length)
{
    const unsigned char *packed_data;
    unsigned long packed_length;
    tg_mtproto_tl_reader reader;

    tg_mtproto_tl_reader_init(&reader, body + 4UL, body_length - 4UL);
    if (tg_mtproto_tl_read_bytes(&reader, &packed_data, &packed_length) !=
        TG_MTPROTO_TL_OK) {
        return 0;
    }
#if TG_ENABLE_GZIP
    {
        int zrc;
        z_stream zs;

        if (packed_length > (unsigned long)UINT_MAX) {
            return 0;
        }
        memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef *)packed_data;
        zs.avail_in = (uInt)packed_length;
        zs.next_out = (Bytef *)tg_mtproto_gzip_unpacked;
        zs.avail_out = (uInt)TG_MTPROTO_GZIP_UNPACKED_MAX;
        zrc = inflateInit2(&zs, 16 + MAX_WBITS);
        if (zrc == Z_OK) {
            zrc = inflate(&zs, Z_FINISH);
            (void)inflateEnd(&zs);
        }
        if (zrc != Z_STREAM_END || zs.total_out < 4UL) {
            return 0;
        }
        *out = tg_mtproto_gzip_unpacked;
        *out_length = (unsigned long)zs.total_out;
        return 1;
    }
#elif TG_ENABLE_GZIP_PUFF
    {
        unsigned long unpacked_length;

        if (tg_mtproto_gzip_unpack_puff(packed_data, packed_length,
                                        &unpacked_length) != 0 ||
            unpacked_length < 4UL) {
            return 0;
        }
        *out = tg_mtproto_gzip_unpacked;
        *out_length = unpacked_length;
        return 1;
    }
#else
    (void)packed_data;
    (void)packed_length;
    (void)out;
    (void)out_length;
    return 0;
#endif
}

/* Extracts the first new-message update from a rich updates container
   (updates#74ae4240 / updatesCombined#725b04c3): channel posts and bot
   replies with entities arrive here instead of updateShortMessage. Update
   items are not length-prefixed, so the walk stops at the first item it
   cannot parse; in practice the new-message update leads the vector. The
   message body is parsed by the same reader the /history transcript uses,
   which also yields the destination peer (the chat the message belongs to). */
/* Filters one parsed Message and, when it is a fresh inbound text, fills a
   notify-queue slot. Shared by the live-push collector and the
   updates.getDifference drain. */
static void tg_chat_notify_push_message(const tg_mtproto_message_text *message,
                                        const tg_mtproto_dialog_peer *dest)
{
    tg_chat_notify_entry *entry;
    unsigned long copy_length;

    if (message == 0 || !message->has_text || message->is_out ||
        tg_chat_notify_seen(tg_chat_nq, message->id)) {
        return;
    }
    entry = tg_chat_notify_claim(tg_chat_nq);
    if (entry == 0) {
        return;
    }
    if (dest != 0 && dest->peer_constructor != 0UL) {
        entry->is_chat =
            dest->peer_constructor != TG_MTPROTO_PEER_USER_CONSTRUCTOR;
        entry->peer_id_hi = dest->id_hi;
        entry->peer_id_lo = dest->id_lo;
    } else {
        entry->is_chat = 0;
        entry->peer_id_hi = message->from_id_hi;
        entry->peer_id_lo = message->from_id_lo;
    }
    entry->from_id_hi = message->from_id_hi;
    entry->from_id_lo = message->from_id_lo;
    copy_length = (unsigned long)strlen(message->text);
    if (copy_length >= TG_CHAT_NOTIFY_TEXT) {
        copy_length = TG_CHAT_NOTIFY_TEXT - 1UL;
    }
    memcpy(entry->text, message->text, copy_length);
    entry->text[copy_length] = '\0';
}

/* Records the most recent typing peer into the live sink (if a GUI session
   armed it). `from_id` is who is typing (the sender, for groups; the peer
   itself for a DM). The GUI tick decides whether it matches the open chat and
   resolves the name. */
static void tg_chat_typing_record(int is_chat, unsigned long peer_id_hi,
                                  unsigned long peer_id_lo,
                                  unsigned long from_id_hi,
                                  unsigned long from_id_lo)
{
    if (tg_chat_typing_target == 0) {
        tg_gui_log("typing: push seen but sink unarmed");
        return;
    }
    {
        char d[96];

        sprintf(d, "typing: record chat=%d peer=%08lx%08lx from=%08lx%08lx",
                is_chat, peer_id_hi, peer_id_lo, from_id_hi, from_id_lo);
        tg_gui_log(d);
    }
    tg_chat_typing_target->active = 1;
    tg_chat_typing_target->is_chat = is_chat;
    tg_chat_typing_target->peer_id_hi = peer_id_hi;
    tg_chat_typing_target->peer_id_lo = peer_id_lo;
    tg_chat_typing_target->from_id_hi = from_id_hi;
    tg_chat_typing_target->from_id_lo = from_id_lo;
    tg_chat_typing_target->seen_epoch = (unsigned long)time(0);
}

/* Reads the SendMessageAction at the reader and returns 1 only for the plain
   "typing" action (uploading/recording/etc. are ignored -- we show "is
   typing", not a generic activity). */
static int tg_chat_typing_action_is_typing(tg_mtproto_tl_reader *reader)
{
    unsigned long action;

    if (tg_mtproto_tl_read_u32(reader, &action) != TG_MTPROTO_TL_OK) {
        return 0;
    }
    return action == TG_MTPROTO_SEND_MESSAGE_TYPING_ACTION_CONSTRUCTOR;
}

/* Given a reader positioned just after an Update constructor, parses the three
   *UserTyping variants and records the typing peer. Other updates are ignored.
   The reader is left in an indeterminate position (the caller stops walking). */
static void tg_chat_typing_parse_update(tg_mtproto_tl_reader *reader,
                                        unsigned long update_ctor)
{
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long peer_ctor;
    unsigned long from_hi;
    unsigned long from_lo;
    unsigned long flags;
    unsigned long top;

    if (update_ctor == TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR) {
        /* DM: the typing user IS the peer. */
        if (tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) == TG_MTPROTO_TL_OK &&
            tg_chat_typing_action_is_typing(reader)) {
            tg_chat_typing_record(0, id_hi, id_lo, id_hi, id_lo);
        }
        return;
    }
    if (update_ctor == TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) == TG_MTPROTO_TL_OK &&
            tg_mtproto_tl_read_u32(reader, &peer_ctor) == TG_MTPROTO_TL_OK &&
            tg_mtproto_tl_read_u64(reader, &from_hi, &from_lo) ==
                TG_MTPROTO_TL_OK &&
            tg_chat_typing_action_is_typing(reader)) {
            tg_chat_typing_record(1, id_hi, id_lo, from_hi, from_lo);
        }
        return;
    }
    if (update_ctor == TG_MTPROTO_UPDATE_CHANNEL_USER_TYPING_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
                TG_MTPROTO_TL_OK) {
            return;
        }
        if ((flags & 0x1UL) != 0UL &&
            tg_mtproto_tl_read_u32(reader, &top) != TG_MTPROTO_TL_OK) {
            return;
        }
        if (tg_mtproto_tl_read_u32(reader, &peer_ctor) == TG_MTPROTO_TL_OK &&
            tg_mtproto_tl_read_u64(reader, &from_hi, &from_lo) ==
                TG_MTPROTO_TL_OK &&
            tg_chat_typing_action_is_typing(reader)) {
            tg_chat_typing_record(1, id_hi, id_lo, from_hi, from_lo);
        }
        return;
    }
}

static void tg_chat_edit_parse_update(tg_mtproto_tl_reader *reader)
{
    static tg_mtproto_message_text message;
    tg_mtproto_dialog_peer dest;
    tg_chat_edit_entry *entry;
    unsigned long copy_length;
    unsigned long i;

    if (tg_chat_edit_target == 0 ||
        tg_mtproto_read_update_message_text(reader, &message, &dest) !=
            TG_MTPROTO_TL_OK ||
        message.id == 0UL || dest.peer_constructor == 0UL) {
        return;
    }
    entry = 0;
    for (i = 0UL; i < tg_chat_edit_target->count; ++i) {
        tg_chat_edit_entry *queued;

        queued = &tg_chat_edit_target->queue[i];
        if (queued->peer_constructor == dest.peer_constructor &&
            queued->peer_id_hi == dest.id_hi &&
            queued->peer_id_lo == dest.id_lo &&
            queued->message_id == message.id) {
            entry = queued;
            break;
        }
    }
    if (entry == 0 &&
        tg_chat_edit_target->count >= TG_CHAT_EDIT_QUEUE_MAX) {
        for (i = 1UL; i < TG_CHAT_EDIT_QUEUE_MAX; ++i) {
            tg_chat_edit_target->queue[i - 1UL] =
                tg_chat_edit_target->queue[i];
        }
        tg_chat_edit_target->count = TG_CHAT_EDIT_QUEUE_MAX - 1UL;
    }
    if (entry == 0) {
        entry = &tg_chat_edit_target->queue[tg_chat_edit_target->count++];
        entry->peer_constructor = dest.peer_constructor;
        entry->peer_id_hi = dest.id_hi;
        entry->peer_id_lo = dest.id_lo;
        entry->message_id = message.id;
    }
    copy_length = (unsigned long)strlen(message.text);
    if (copy_length >= sizeof(entry->text)) {
        copy_length = sizeof(entry->text) - 1UL;
    }
    memcpy(entry->text, message.text, copy_length);
    entry->text[copy_length] = '\0';
}

static int tg_chat_edit_peer_matches_open(const tg_chat_edit_entry *entry,
                                          unsigned long peer_constructor,
                                          unsigned long peer_id_hi,
                                          unsigned long peer_id_lo)
{
    if (entry == 0) {
        return 0;
    }
    if (entry->peer_constructor == peer_constructor &&
        entry->peer_id_hi == peer_id_hi &&
        entry->peer_id_lo == peer_id_lo) {
        return 1;
    }
    /* Saved Messages is addressed locally as inputPeerSelf (ids 0/0), while
       message.peer_id in the edit update is PeerUser(our id). The subsequent
       transcript lookup by message id is the final guard. */
    return peer_constructor == TG_MTPROTO_PEER_SELF_CONSTRUCTOR &&
           entry->peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR;
}

/* A Vector<Update> is not length-prefixed per item. The normal collector can
   therefore consume only its first known item safely: a Message parser may
   intentionally stop after text when optional media/reply fields follow.
   Remote edits still have a strong aligned signature
   updateEdit*(message#9815cec8 ...), so scan the complete updates envelope and
   validate every candidate with the real Message parser. This catches an edit
   preceded by read/draft/status updates, as observed with Saved Messages. */
static void tg_chat_edit_scan_updates(const unsigned char *body,
                                      unsigned long body_length,
                                      unsigned long start_offset)
{
    tg_mtproto_tl_reader reader;
    unsigned long constructor;
    unsigned long offset;

    if (tg_chat_edit_target == 0 || body == 0 ||
        start_offset >= body_length) {
        return;
    }
    offset = (start_offset + 3UL) & ~3UL;
    while (offset + 8UL <= body_length) {
        constructor = tg_mtproto_read_u32_le(body + offset);
        if (constructor == TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR ||
            constructor ==
                TG_MTPROTO_UPDATE_EDIT_CHANNEL_MESSAGE_CONSTRUCTOR) {
            tg_mtproto_tl_reader_init(&reader, body, body_length);
            reader.offset = offset + 4UL;
            tg_chat_edit_parse_update(&reader);
        }
        offset += 4UL;
    }
}

/* Parse one Update with the reader positioned immediately after its
   constructor. Update items are not length-prefixed, so callers consume one
   known item and stop rather than guessing where an unknown tail ends. */
static void tg_chat_collect_update_item(tg_mtproto_tl_reader *reader,
                                        unsigned long update_ctor)
{
    if (update_ctor == TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR ||
        update_ctor == TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR ||
        update_ctor == TG_MTPROTO_UPDATE_CHANNEL_USER_TYPING_CONSTRUCTOR) {
        tg_chat_typing_parse_update(reader, update_ctor);
        return;
    }
    if (update_ctor == TG_MTPROTO_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR) {
        tg_mtproto_dialog_peer peer;
        unsigned long max_id;

        if (tg_chat_read_outbox_target != 0 &&
            tg_mtproto_read_update_read_history_outbox(reader, &peer, &max_id) ==
                TG_MTPROTO_TL_OK) {
            tg_chat_read_outbox_target->peer_id_hi = peer.id_hi;
            tg_chat_read_outbox_target->peer_id_lo = peer.id_lo;
            tg_chat_read_outbox_target->max_id = max_id;
            tg_chat_read_outbox_target->pending = 1;
        }
        return;
    }
    if (update_ctor == TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR ||
        update_ctor == TG_MTPROTO_UPDATE_EDIT_CHANNEL_MESSAGE_CONSTRUCTOR) {
        tg_chat_edit_parse_update(reader);
        return;
    }
    if (update_ctor == TG_MTPROTO_UPDATE_NEW_MESSAGE_CONSTRUCTOR ||
        update_ctor == TG_MTPROTO_UPDATE_NEW_CHANNEL_MESSAGE_CONSTRUCTOR) {
        static tg_mtproto_message_text message;
        tg_mtproto_dialog_peer dest;

        if (tg_mtproto_read_update_message_text(reader, &message, &dest) ==
            TG_MTPROTO_TL_OK) {
            tg_chat_notify_push_message(&message, &dest);
        }
    }
}

/* updateShort#78d4dec1 update:Update date:int -- the standard single-update
   envelope used by typing, read receipts, new messages and remote edits. */
static void tg_chat_update_collect_short(const unsigned char *body,
                                         unsigned long body_length)
{
    tg_mtproto_tl_reader reader;
    unsigned long outer;
    unsigned long inner;

    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &outer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &inner) != TG_MTPROTO_TL_OK) {
        return;
    }
    tg_chat_collect_update_item(&reader, inner);
}

static void tg_chat_notify_collect_updates(const unsigned char *body,
                                           unsigned long body_length)
{
    tg_mtproto_tl_reader reader;
    unsigned long constructor;
    unsigned long item_constructor;
    unsigned long count;
    unsigned long i;

    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MTPROTO_TL_VECTOR_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(&reader, &count) != TG_MTPROTO_TL_OK) {
        return;
    }
    if (count > 0UL) {
        tg_chat_edit_scan_updates(body, body_length, reader.offset);
    }
    for (i = 0UL; i < count; ++i) {
        if (tg_mtproto_tl_read_u32(&reader, &item_constructor) !=
            TG_MTPROTO_TL_OK) {
            return;
        }
        if (item_constructor != TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_CHANNEL_USER_TYPING_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR &&
            item_constructor != TG_MTPROTO_UPDATE_NEW_MESSAGE_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_NEW_CHANNEL_MESSAGE_CONSTRUCTOR &&
            item_constructor != TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_EDIT_CHANNEL_MESSAGE_CONSTRUCTOR) {
            /* Unknown update items cannot be skipped (no length prefix). */
            return;
        }
        tg_chat_collect_update_item(&reader, item_constructor);
#ifdef TG_MTPROTO_DIAG
        if (item_constructor == TG_MTPROTO_UPDATE_NEW_MESSAGE_CONSTRUCTOR ||
            item_constructor ==
                TG_MTPROTO_UPDATE_NEW_CHANNEL_MESSAGE_CONSTRUCTOR) {
            fprintf(stderr, "notify-diag rich message/update consumed\n");
        }
#endif
        /* Known parsers may leave optional tail bytes, so do not trust the
           reader position for further vector items. */
        return;
    }
}

/* Sees every decrypted MTProto payload once: collects bare new-message
   updates, walks msg_container parts and unwraps gzip-packed pushes. */
static void tg_chat_notify_collect(const unsigned char *body,
                                   unsigned long body_length)
{
    unsigned long constructor;
    unsigned long count;
    unsigned long part_length;
    unsigned long offset;
    unsigned long i;
    const unsigned char *unpacked;
    unsigned long unpacked_length;

    if (tg_chat_nq == 0 || !tg_chat_nq->armed || body == 0 ||
        body_length < 4UL) {
        return;
    }
    constructor = tg_mtproto_read_u32_le(body);
#ifdef TG_MTPROTO_DIAG
    fprintf(stderr, "notify-diag body ctor 0x%08lx len %lu\n", constructor,
            body_length);
#endif
    if (constructor == TG_MTPROTO_GZIP_PACKED_CONSTRUCTOR) {
        if (tg_chat_notify_gunzip(body, body_length, &unpacked,
                                  &unpacked_length)) {
            tg_chat_notify_collect(unpacked, unpacked_length);
        }
        return;
    }
#ifdef TG_MTPROTO_DIAG
    if ((constructor == TG_MTPROTO_UPDATES_CONSTRUCTOR ||
         constructor == TG_MTPROTO_UPDATES_COMBINED_CONSTRUCTOR) &&
        body_length >= 16UL) {
        fprintf(stderr, "notify-diag updates vec 0x%08lx n=%lu item0=0x%08lx\n",
                tg_mtproto_read_u32_le(body + 4UL),
                tg_mtproto_read_u32_le(body + 8UL),
                tg_mtproto_read_u32_le(body + 12UL));
    }
#endif
    if (constructor == TG_MTPROTO_UPDATES_CONSTRUCTOR ||
        constructor == TG_MTPROTO_UPDATES_COMBINED_CONSTRUCTOR) {
        tg_chat_notify_collect_updates(body, body_length);
        return;
    }
    if (constructor == TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR) {
        tg_chat_update_collect_short(body, body_length);
        return;
    }
    if (constructor != TG_MTPROTO_MSG_CONTAINER_CONSTRUCTOR) {
        tg_chat_notify_collect_one(body, body_length);
        return;
    }
    if (body_length < 8UL) {
        return;
    }
    count = tg_mtproto_read_u32_le(body + 4UL);
    offset = 8UL;
    for (i = 0UL; i < count; ++i) {
        /* part: msg_id(8) seqno(4) bytes(4) body[bytes] */
        if (offset + 16UL > body_length) {
            return;
        }
        part_length = tg_mtproto_read_u32_le(body + offset + 12UL);
        offset += 16UL;
        if (part_length > body_length - offset) {
            return;
        }
#ifdef TG_MTPROTO_DIAG
        if (part_length >= 4UL) {
            fprintf(stderr, "notify-diag part ctor 0x%08lx len %lu\n",
                    tg_mtproto_read_u32_le(body + offset), part_length);
        }
#endif
        if (part_length >= 4UL &&
            tg_mtproto_read_u32_le(body + offset) ==
                TG_MTPROTO_GZIP_PACKED_CONSTRUCTOR) {
            if (tg_chat_notify_gunzip(body + offset, part_length, &unpacked,
                                      &unpacked_length)) {
                tg_chat_notify_collect(unpacked, unpacked_length);
            }
        } else {
            tg_chat_notify_collect_one(body + offset, part_length);
        }
        offset += part_length;
    }
}

/*
 * One-shot commands wrap their connection bootstrap in invokeWithoutUpdates:
 * a busy account's update stream is pure overhead for them, especially on
 * slow links. The interactive chat flips this on so its persistent
 * connection DOES receive the update pushes that feed the cross-chat
 * notifications (the receive loop already drains and ACKs them within the
 * query budget).
 */
static int tg_mtproto_session_updates_wanted = 0;

static void tg_mtproto_set_session_updates(int enabled)
{
    tg_mtproto_session_updates_wanted = enabled ? 1 : 0;
}

static int tg_mtproto_build_initialized_query(tg_mtproto_tl_writer *writer,
                                              unsigned char *wrapped_query,
                                              unsigned long wrapped_capacity,
                                              unsigned long api_id,
                                              const unsigned char *query,
                                              unsigned long query_length)
{
    static unsigned char initialized_query[TG_MTPROTO_QUERY_SEND_MAX];
    static unsigned char layered_query[TG_MTPROTO_QUERY_SEND_MAX];
    unsigned long initialized_length;
    unsigned long layered_length;
    tg_mtproto_tl_status status;

    tg_mtproto_tl_writer_init(writer, initialized_query,
                              sizeof(initialized_query));
    status = tg_mtproto_build_init_connection(writer, api_id, "Amiga",
                                              "portable", "0.1", "en",
                                              query, query_length);
    if (status != TG_MTPROTO_TL_OK) {
        return 1;
    }
    initialized_length = writer->length;

    tg_mtproto_tl_writer_init(writer, layered_query, sizeof(layered_query));
    status = tg_mtproto_build_invoke_with_layer(writer, 214UL,
                                                initialized_query,
                                                initialized_length);
    if (status != TG_MTPROTO_TL_OK) {
        return 1;
    }
    layered_length = writer->length;

    tg_mtproto_tl_writer_init(writer, wrapped_query, wrapped_capacity);
    if (tg_mtproto_session_updates_wanted) {
        status = tg_mtproto_tl_write_raw(writer, layered_query,
                                         layered_length);
    } else {
        status = tg_mtproto_build_invoke_without_updates(writer,
                                                         layered_query,
                                                         layered_length);
    }
    return status == TG_MTPROTO_TL_OK ? 0 : 1;
}

static int tg_mtproto_send_saved_query_limited(const char *host,
                                               const char *port,
                                               const char *api_id_text,
                                               const char *auth_file,
                                               const char *dc_id_text,
                                               const unsigned char *query,
                                               unsigned long query_length,
                                               tg_mtproto_rpc_result *result,
                                               FILE *stream,
                                               const char *label,
                                               unsigned int
                                                   max_receive_attempts,
                                               int skip_close_on_failure)
{
    unsigned char wrapped_query[1400];
    unsigned long api_id;
    tg_mtproto_auth_context context;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    long dc_id;

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || query == 0 || query_length == 0UL ||
        result == 0 || label == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }
    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    if (tg_mtproto_validate_saved_auth_dc(&context, (unsigned long)dc_id,
                                          stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }

    if (tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, query_length) != 0) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_encrypted_query_limited(
            &context, wrapped_query, writer.length, result, stream, label,
            max_receive_attempts, TG_MTPROTO_QUERY_BUDGET_SECONDS) != 0) {
        if (skip_close_on_failure) {
            tg_mtproto_skip_auth_context_close(&context, stream, label);
        } else {
            tg_mtproto_close_auth_context(&context);
        }
        return 2;
    }
    tg_mtproto_skip_auth_context_close(&context, stream, label);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    return 0;
}

static int tg_mtproto_ensure_saved_auth_context(
    const char *host,
    const char *port,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    FILE *stream,
    const char *label)
{
    long dc_id;

    if (context == 0 || stream == 0 || label == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }
    if (context->connection_open) {
        return 0;
    }
    if (tg_mtproto_load_auth_context(host, port, auth_file, context, stream,
                                     label) != 0) {
        return 2;
    }
    if (tg_mtproto_validate_saved_auth_dc(context, (unsigned long)dc_id,
                                          stream, label) != 0) {
        tg_mtproto_close_auth_context(context);
        return 2;
    }
    return 0;
}

static int tg_mtproto_send_saved_query_on_context(
    const char *host,
    const char *port,
    const char *api_id_text,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const unsigned char *query,
    unsigned long query_length,
    tg_mtproto_rpc_result *result,
    FILE *stream,
    const char *label,
    unsigned int max_receive_attempts)
{
    /* static + chunk-sized: the saveFilePart body is a whole file chunk. */
    static unsigned char wrapped_query[TG_MTPROTO_QUERY_SEND_MAX];
    unsigned long api_id;
    int qrc;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;

    if (stream == 0 || api_id_text == 0 || auth_file == 0 || query == 0 ||
        query_length == 0UL || result == 0 || label == 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0 && label != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }
    if (tg_mtproto_ensure_saved_auth_context(host, port, auth_file,
                                             dc_id_text, context, stream,
                                             label) != 0) {
        return 2;
    }
    if (tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, query_length) != 0) {
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    qrc = tg_mtproto_send_encrypted_query_limited(
            context, wrapped_query, writer.length, result, stream, label,
            max_receive_attempts, TG_MTPROTO_QUERY_BUDGET_SECONDS);
    if (qrc != 0) {
#ifdef TG_MTPROTO_DIAG
        fprintf(stderr, "notify-diag ctx-close qrc=%d label=%s\n", qrc,
                label);
#endif
        /* A soft failure means the matching rpc_result was not seen within the
           receive budget. Keep no stale stream around: the late reply may still
           arrive and be mistaken for the next command's response. */
        tg_mtproto_close_auth_context(context);
        return qrc == TG_MTPROTO_QUERY_SOFT_FAIL ?
            TG_MTPROTO_QUERY_SOFT_FAIL : 2;
    }
    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context->session, context->auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    return 0;
}

static int tg_mtproto_send_saved_query(const char *host,
                                       const char *port,
                                       const char *api_id_text,
                                       const char *auth_file,
                                       const char *dc_id_text,
                                       const unsigned char *query,
                                       unsigned long query_length,
                                       tg_mtproto_rpc_result *result,
                                       FILE *stream,
                                       const char *label)
{
    return tg_mtproto_send_saved_query_limited(
        host, port, api_id_text, auth_file, dc_id_text, query, query_length,
        result, stream, label, 32U, 0);
}

/* Where the last auth.sendCode put the code, and how long it is: parsed
   already, and until now thrown away. */
static unsigned long tg_mtproto_sent_code_type;
static unsigned long tg_mtproto_sent_code_len;

static void tg_mtproto_remember_sent_code(const tg_mtproto_sent_code *sc)
{
    tg_mtproto_sent_code_type = (sc != 0) ? sc->type_constructor : 0UL;
    tg_mtproto_sent_code_len =
        (sc != 0 && sc->has_type_length) ? sc->type_length : 0UL;
}

const char *tg_mtproto_sent_code_hint(void)
{
    return tg_mtproto_sent_code_text(tg_mtproto_sent_code_type, 1);
}

unsigned long tg_mtproto_sent_code_length(void)
{
    return tg_mtproto_sent_code_len;
}

int tg_mtproto_auth_send_code(const char *host,
                              const char *port,
                              const char *dc_id_text,
                              const char *api_id_text,
                              const char *api_hash,
                              const char *phone_number,
                              const char *auth_file,
                              const char *code_hash_file,
                              FILE *stream)
{
    unsigned char query[512];
    unsigned char initialized_query[640];
    unsigned char wrapped_query[760];
    unsigned long api_id;
    unsigned long query_length;
    tg_file_status file_status;
    tg_mtproto_auth_context context;
    tg_mtproto_rpc_result result;
    tg_mtproto_sent_code sent_code;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto auth.sendCode";

    if (stream == 0 || host == 0 || port == 0 || dc_id_text == 0 ||
        api_id_text == 0 || api_hash == 0 || phone_number == 0 ||
        auth_file == 0 || code_hash_file == 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto auth.sendCode: invalid-arguments\n", stream);
        }
        return 2;
    }

    if (tg_mtproto_open_auth_context(host, port, dc_id_text, &context, stream,
                                     label) != 0) {
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth.sendCode build");
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_send_code(&writer, phone_number, api_id,
                                        api_hash) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, initialized_query,
                              sizeof(initialized_query));
    if (tg_mtproto_build_init_connection(&writer, api_id, "Amiga",
                                         "portable", "0.1", "en", query,
                                         query_length) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, wrapped_query, sizeof(wrapped_query));
    if (tg_mtproto_build_invoke_with_layer(&writer, 214UL, initialized_query,
                                           query_length) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: invoke-layer-build-failed\n", label);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth.sendCode send");
    if (tg_mtproto_send_encrypted_query_login(
            &context, wrapped_query, writer.length, &result, stream,
            label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_login_phase(stream, "auth.sendCode response");
    tg_mtproto_skip_auth_context_close(&context, stream, label);

    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        unsigned long migrate_dc;
        if (tg_mtproto_rpc_phone_migrate_dc(&result, &migrate_dc)) {
            if (migrate_dc <= 215UL) {
                return TG_MTPROTO_PHONE_MIGRATE_RC_BASE + (int)migrate_dc;
            }
        }
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (result.result_constructor != TG_MTPROTO_AUTH_SENT_CODE_CONSTRUCTOR &&
        result.result_constructor !=
            TG_MTPROTO_AUTH_SENT_CODE_PAYMENT_REQUIRED_CONSTRUCTOR &&
        result.result_constructor !=
            TG_MTPROTO_AUTH_SENT_CODE_SUCCESS_CONSTRUCTOR) {
        fprintf(stream, "%s: unexpected-result 0x%08lx\n", label,
                result.result_constructor);
        return 2;
    }
    if (tg_mtproto_parse_auth_sent_code(result.result_constructor,
                                        result.result_body,
                                        result.result_body_length,
                                        &sent_code) != TG_MTPROTO_TL_OK ||
        sent_code.phone_code_hash[0] == '\0') {
        fprintf(stream, "%s: sent-code-parse-failed\n", label);
        return 2;
    }
    /* Telegram says WHERE it put the code, and it is rarely an SMS: with
       another device signed in it delivers the code inside Telegram itself.
       A user watching an empty inbox concludes this client is broken (field
       question, 2026-08), so keep the answer and let the login screen say
       it. */
    tg_mtproto_remember_sent_code(&sent_code);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    file_status = tg_file_write_text(code_hash_file, sent_code.phone_code_hash,
                                     (unsigned long)strlen(
                                         sent_code.phone_code_hash));
    if (file_status == TG_FILE_OK) {
        file_status = tg_file_append_text(code_hash_file, "\n", 1UL);
    }
    if (file_status != TG_FILE_OK) {
        fprintf(stream, "%s: code-hash-save-failed (%s)\n", label,
                tg_file_status_name(file_status));
        return 2;
    }

    fprintf(stream, "Login code sent.\n");
    tg_mtproto_print_login_code_hint(stream, sent_code.type_constructor);
    fflush(stream);
    return 0;
}

int tg_mtproto_auth_send_code_file(const char *host,
                                   const char *port,
                                   const char *dc_id_text,
                                   const char *api_file,
                                   const char *phone_number,
                                   const char *auth_file,
                                   const char *code_hash_file,
                                   FILE *stream)
{
    char api_id[32];
    char api_hash[96];
    int rc;
    static const char label[] = "mtproto auth.sendCode";

    if (tg_mtproto_load_api_credentials(api_file, api_id, sizeof(api_id),
                                        api_hash, sizeof(api_hash),
                                        stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_send_code(host, port, dc_id_text, api_id, api_hash,
                                   phone_number, auth_file, code_hash_file,
                                   stream);
    tg_mtproto_secure_zero(api_hash, sizeof(api_hash));
    return rc;
}

int tg_mtproto_auth_sign_in(const char *host,
                            const char *port,
                            const char *api_id_text,
                            const char *auth_file,
                            const char *phone_number,
                            const char *code_hash_file,
                            const char *phone_code,
                            const char *dc_id_text,
                            FILE *stream)
{
    unsigned char query[512];
    unsigned char initialized_query[640];
    unsigned char wrapped_query[760];
    char code_hash[160];
    unsigned long code_hash_length;
    unsigned long api_id;
    unsigned long query_length;
    tg_file_status file_status;
    tg_mtproto_auth_context context;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    long dc_id;
    int qrc;
    static const char label[] = "mtproto auth.signIn";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 ||
        phone_number == 0 || code_hash_file == 0 || phone_code == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto auth.signIn: invalid-arguments\n", stream);
        }
        return 2;
    }

    file_status = tg_file_read_text(code_hash_file, code_hash,
                                    sizeof(code_hash), &code_hash_length);
    if (file_status != TG_FILE_OK) {
        fprintf(stream, "%s: code-hash-load-failed (%s)\n", label,
                tg_file_status_name(file_status));
        return 2;
    }
    tg_mtproto_trim_line(code_hash);
    if (code_hash[0] == '\0') {
        fprintf(stream, "%s: code-hash-empty\n", label);
        return 2;
    }

    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_login_phase(stream, "auth.signIn build");
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_sign_in(&writer, phone_number, code_hash,
                                      phone_code) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, initialized_query,
                              sizeof(initialized_query));
    if (tg_mtproto_build_init_connection(&writer, api_id, "Amiga",
                                         "portable", "0.1", "en", query,
                                         query_length) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, wrapped_query, sizeof(wrapped_query));
    if (tg_mtproto_build_invoke_with_layer(&writer, 214UL, initialized_query,
                                           query_length) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: invoke-layer-build-failed\n", label);
        return 2;
    }

    tg_mtproto_login_phase(stream, "auth.signIn send");
    qrc = tg_mtproto_send_encrypted_query_login(
        &context, wrapped_query, writer.length, &result, stream, label);
    if (qrc != 0) {
        if (qrc == TG_MTPROTO_QUERY_SOFT_FAIL) {
            session_status = tg_mtproto_session_save_authorization(
                auth_file, &context.session, context.auth_key, 1);
            if (session_status != TG_MTPROTO_SESSION_OK) {
                fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                        tg_mtproto_session_status_name(session_status));
            }
        }
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_login_phase(stream, "auth.signIn response");
    tg_mtproto_skip_auth_context_close(&context, stream, label);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }

    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        char sign_in_error[128];
        long sign_in_error_code;

        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        if (tg_mtproto_parse_rpc_error(result.result_body - 4U,
                                       result.result_body_length + 4U,
                                       &sign_in_error_code, sign_in_error,
                                       sizeof(sign_in_error)) ==
            TG_MTPROTO_TL_OK) {
            if (strcmp(sign_in_error, "SESSION_PASSWORD_NEEDED") == 0 ||
                strcmp(sign_in_error, "PHONE_PASSWORD_PROTECTED") == 0) {
                return TG_MTPROTO_SIGN_IN_PASSWORD_NEEDED;
            }
            if (strcmp(sign_in_error, "PHONE_CODE_INVALID") == 0 ||
                strcmp(sign_in_error, "PHONE_CODE_EMPTY") == 0 ||
                strcmp(sign_in_error, "PHONE_CODE_EXPIRED") == 0) {
                return TG_MTPROTO_SIGN_IN_CODE_INVALID;
            }
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (!tg_mtproto_is_auth_authorization_constructor(
            result.result_constructor)) {
        fprintf(stream, "%s: unexpected-result 0x%08lx\n", label,
                result.result_constructor);
        return 2;
    }
    if (result.result_constructor ==
            0x44747e9aUL) {
        fprintf(stream, "%s: signup-required; run --mtproto-auth-sign-up\n",
                label);
        return 2;
    }

#ifdef TG_MTPROTO_DIAG
    fprintf(stream, "%s: signed in\n", label);
    fprintf(stream, "%s: auth state updated\n", label);
#endif
    return 0;
}

int tg_mtproto_auth_sign_in_file(const char *host,
                                 const char *port,
                                 const char *api_file,
                                 const char *auth_file,
                                 const char *phone_number,
                                 const char *code_hash_file,
                                 const char *phone_code,
                                 const char *dc_id_text,
                                 FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto auth.signIn";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_sign_in(host, port, api_id, auth_file, phone_number,
                                 code_hash_file, phone_code, dc_id_text,
                                 stream);
    return rc;
}

int tg_mtproto_auth_sign_up(const char *host,
                            const char *port,
                            const char *api_id_text,
                            const char *auth_file,
                            const char *phone_number,
                            const char *code_hash_file,
                            const char *first_name,
                            const char *last_name,
                            const char *dc_id_text,
                            FILE *stream)
{
    unsigned char query[512];
    unsigned char initialized_query[640];
    unsigned char wrapped_query[760];
    char code_hash[160];
    unsigned long code_hash_length;
    unsigned long api_id;
    unsigned long query_length;
    tg_file_status file_status;
    tg_mtproto_auth_context context;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    long dc_id;
    static const char label[] = "mtproto auth.signUp";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || phone_number == 0 || code_hash_file == 0 ||
        first_name == 0 || last_name == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto auth.signUp: invalid-arguments\n", stream);
        }
        return 2;
    }

    file_status = tg_file_read_text(code_hash_file, code_hash,
                                    sizeof(code_hash), &code_hash_length);
    if (file_status != TG_FILE_OK) {
        fprintf(stream, "%s: code-hash-load-failed (%s)\n", label,
                tg_file_status_name(file_status));
        return 2;
    }
    tg_mtproto_trim_line(code_hash);
    if (code_hash[0] == '\0') {
        fprintf(stream, "%s: code-hash-empty\n", label);
        return 2;
    }

    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_sign_up(&writer, phone_number, code_hash,
                                      first_name, last_name) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, initialized_query,
                              sizeof(initialized_query));
    if (tg_mtproto_build_init_connection(&writer, api_id, "Amiga",
                                         "portable", "0.1", "en", query,
                                         query_length) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, wrapped_query, sizeof(wrapped_query));
    if (tg_mtproto_build_invoke_with_layer(&writer, 214UL, initialized_query,
                                           query_length) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: invoke-layer-build-failed\n", label);
        return 2;
    }

    if (tg_mtproto_send_encrypted_query(&context, wrapped_query, writer.length,
                                        &result, stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_close_auth_context(&context);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }

    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (!tg_mtproto_is_auth_authorization_constructor(
            result.result_constructor)) {
        fprintf(stream, "%s: unexpected-result 0x%08lx\n", label,
                result.result_constructor);
        return 2;
    }
    if (result.result_constructor == 0x44747e9aUL) {
        fprintf(stream, "%s: signup-still-required\n", label);
        return 2;
    }

    fprintf(stream, "%s: signed up\n", label);
    fprintf(stream, "%s: auth state updated\n", label);
    return 0;
}

int tg_mtproto_auth_get_config(const char *host,
                               const char *port,
                               const char *api_id_text,
                               const char *auth_file,
                               const char *dc_id_text,
                               FILE *stream)
{
    unsigned char query[32];
    unsigned char wrapped_query[760];
    unsigned long api_id;
    tg_mtproto_auth_context context;
    tg_mtproto_config_summary config;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    long dc_id;
    static const char label[] = "mtproto help.getConfig";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto help.getConfig: invalid-arguments\n", stream);
        }
        return 2;
    }
    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_help_get_config(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, 4UL) != 0) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_encrypted_query(&context, wrapped_query, writer.length,
                                        &result, stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_close_auth_context(&context);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_config_summary(result.result_constructor,
                                        result.result_body,
                                        result.result_body_length,
                                        &config) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: config-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: this_dc %lu\n", label, config.this_dc);
    fprintf(stream, "%s: date %lu expires %lu\n", label, config.date,
            config.expires);
    return 0;
}

int tg_mtproto_auth_get_config_file(const char *host,
                                    const char *port,
                                    const char *api_file,
                                    const char *auth_file,
                                    const char *dc_id_text,
                                    FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto help.getConfig";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_get_config(host, port, api_id, auth_file,
                                    dc_id_text, stream);
    return rc;
}

int tg_mtproto_auth_get_password(const char *host,
                                 const char *port,
                                 const char *api_id_text,
                                 const char *auth_file,
                                 const char *dc_id_text,
                                 FILE *stream)
{
    unsigned char query[32];
    unsigned char wrapped_query[760];
    unsigned long api_id;
    tg_mtproto_auth_context context;
    tg_mtproto_password_summary password;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    long dc_id;
    static const char label[] = "mtproto account.getPassword";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto account.getPassword: invalid-arguments\n", stream);
        }
        return 2;
    }
    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_account_get_password(&writer) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, 4UL) != 0) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_encrypted_query(&context, wrapped_query, writer.length,
                                        &result, stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_close_auth_context(&context);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_account_password_summary(result.result_constructor,
                                                  result.result_body,
                                                  result.result_body_length,
                                                  &password) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: password-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: has_password %s\n", label,
            password.has_password ? "yes" : "no");
    if (password.has_current_algo) {
        fprintf(stream, "%s: current_algo 0x%08lx\n", label,
                password.current_algo_constructor);
    }
    if (password.current_algo_constructor == 0x3a912d4aUL) {
        fprintf(stream,
                "%s: srp params salt1 %lu salt2 %lu p %lu B %lu g %lu srp_id 0x%08lx%08lx\n",
                label, password.current_salt1_length,
                password.current_salt2_length, password.current_p_length,
                password.srp_b_length, password.current_g,
                password.srp_id_hi, password.srp_id_lo);
    }
    fprintf(stream, "%s: srp_check pending\n", label);
    return 0;
}

int tg_mtproto_auth_get_password_file(const char *host,
                                      const char *port,
                                      const char *api_file,
                                      const char *auth_file,
                                      const char *dc_id_text,
                                      FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto account.getPassword";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_get_password(host, port, api_id, auth_file,
                                      dc_id_text, stream);
    return rc;
}

static int tg_mtproto_auth_check_password_text(const char *host,
                                               const char *port,
                                               const char *api_id_text,
                                               const char *auth_file,
                                               const char *dc_id_text,
                                               const char *password_input,
                                               FILE *stream)
{
    unsigned char query[512];
    unsigned char wrapped_query[760];
    unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH];
    char password_text[512];
    unsigned long password_length;
    unsigned long api_id;
    tg_mtproto_auth_context context;
    tg_mtproto_password_summary password;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_srp_proof proof;
    tg_mtproto_tl_writer writer;
    long dc_id;
    int qrc;
    static const char label[] = "mtproto auth.checkPassword";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || password_input == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto auth.checkPassword: invalid-arguments\n", stream);
        }
        return 2;
    }

    password_length = (unsigned long)strlen(password_input);
    if (password_length == 0UL || password_length >= sizeof(password_text)) {
        fprintf(stream, "%s: password-invalid\n", label);
        return 2;
    }
    memcpy(password_text, password_input, password_length + 1UL);

    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_account_get_password(&writer) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, writer.length) != 0) {
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        fprintf(stream, "%s: get-password-build-failed\n", label);
        return 2;
    }
    qrc = tg_mtproto_send_encrypted_query_login(
        &context, wrapped_query, writer.length, &result, stream, label);
    if (qrc != 0) {
        if (qrc == TG_MTPROTO_QUERY_SOFT_FAIL) {
            session_status = tg_mtproto_session_save_authorization(
                auth_file, &context.session, context.auth_key, 1);
            if (session_status != TG_MTPROTO_SESSION_OK) {
                fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                        tg_mtproto_session_status_name(session_status));
            }
        }
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        tg_mtproto_close_auth_context(&context);
        session_status = tg_mtproto_session_save_authorization(
            auth_file, &context.session, context.auth_key, 1);
        if (session_status != TG_MTPROTO_SESSION_OK) {
            fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                    tg_mtproto_session_status_name(session_status));
        } else if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        return 2;
    }
    if (tg_mtproto_parse_account_password_summary(result.result_constructor,
                                                  result.result_body,
                                                  result.result_body_length,
                                                  &password) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        fprintf(stream, "%s: password-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    if (!password.has_password || !password.has_current_algo) {
        tg_mtproto_close_auth_context(&context);
        session_status = tg_mtproto_session_save_authorization(
            auth_file, &context.session, context.auth_key, 1);
        if (session_status != TG_MTPROTO_SESSION_OK) {
            fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                    tg_mtproto_session_status_name(session_status));
            tg_mtproto_secure_zero(password_text, sizeof(password_text));
            return 2;
        }
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        fprintf(stream, "%s: no password required\n", label);
        fprintf(stream, "%s: auth state updated\n", label);
        return 0;
    }
    /* Use a 256-bit private SRP exponent 'a' (low bytes only; big-endian) with
       the top bit forced, instead of a full 2048-bit one. Only g^a leaves the
       device, so a's bit-length is the client's choice; 256 bits keeps standard
       SRP security while shrinking two of the three 2048-bit modexps (g^a and
       base^(a+u*x)) to ~256-/~512-bit exponents -- a big 2FA speed-up for AmiSSL
       BN_mod_exp on m68k. Same lever as TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES. */
    memset(random_a, 0, sizeof(random_a));
    if (!tg_mtproto_secure_random(
            random_a + TG_MTPROTO_SRP_VALUE_LENGTH -
                TG_MTPROTO_SRP_PRIVATE_EXPONENT_BYTES,
            TG_MTPROTO_SRP_PRIVATE_EXPONENT_BYTES)) {
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        fprintf(stream, "%s: secure-rng-unavailable\n", label);
        return 2;
    }
    random_a[TG_MTPROTO_SRP_VALUE_LENGTH -
             TG_MTPROTO_SRP_PRIVATE_EXPONENT_BYTES] |= 0x80U;
    fprintf(stream, "Verifying password");
    fflush(stream);
    tg_login_progress_stream = stream;
    tg_mtproto_set_progress_hook(tg_login_progress_dot);
    if (tg_mtproto_srp_make_proof(&password,
                                  (const unsigned char *)password_text,
                                  password_length, random_a, &proof) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_set_progress_hook(0);
        tg_login_progress_stream = 0;
        fputc('\n', stream);
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(random_a, sizeof(random_a));
        tg_mtproto_secure_zero(password_text, sizeof(password_text));
        fprintf(stream, "%s: srp-proof-build-failed\n", label);
        return 2;
    }
    tg_mtproto_set_progress_hook(0);
    tg_login_progress_stream = 0;
    fputc('\n', stream);
    fflush(stream);
    tg_mtproto_secure_zero(random_a, sizeof(random_a));
    tg_mtproto_secure_zero(password_text, sizeof(password_text));

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_check_password_srp(
            &writer, password.srp_id_hi, password.srp_id_lo,
            proof.a, proof.a_length, proof.m1) != TG_MTPROTO_TL_OK ||
        tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, writer.length) != 0) {
        tg_mtproto_close_auth_context(&context);
        tg_mtproto_secure_zero(&proof, sizeof(proof));
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    tg_mtproto_secure_zero(&proof, sizeof(proof));
    qrc = tg_mtproto_send_encrypted_query_login(
        &context, wrapped_query, writer.length, &result, stream, label);
    if (qrc != 0) {
        if (qrc == TG_MTPROTO_QUERY_SOFT_FAIL) {
            session_status = tg_mtproto_session_save_authorization(
                auth_file, &context.session, context.auth_key, 1);
            if (session_status != TG_MTPROTO_SESSION_OK) {
                fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                        tg_mtproto_session_status_name(session_status));
            }
        }
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_close_auth_context(&context);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        char check_password_error[128];
        long check_password_error_code;

        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        if (tg_mtproto_parse_rpc_error(result.result_body - 4U,
                                       result.result_body_length + 4U,
                                       &check_password_error_code,
                                       check_password_error,
                                       sizeof(check_password_error)) ==
                TG_MTPROTO_TL_OK &&
            strcmp(check_password_error, "PASSWORD_HASH_INVALID") == 0) {
            return TG_MTPROTO_CHECK_PASSWORD_INVALID;
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (!tg_mtproto_is_auth_authorization_constructor(
            result.result_constructor)) {
        fprintf(stream, "%s: unexpected-result 0x%08lx\n", label,
                result.result_constructor);
        return 2;
    }
    if (result.result_constructor == 0x44747e9aUL) {
        fprintf(stream, "%s: signup-required\n", label);
        return 2;
    }

#ifdef TG_MTPROTO_DIAG
    fprintf(stream, "%s: signed in\n", label);
    fprintf(stream, "%s: auth state updated\n", label);
#endif
    return 0;
}

int tg_mtproto_auth_check_password(const char *host,
                                   const char *port,
                                   const char *api_id_text,
                                   const char *auth_file,
                                   const char *dc_id_text,
                                   const char *password_file,
                                   FILE *stream)
{
    char password_text[512];
    unsigned long password_length;
    int rc;
    static const char label[] = "mtproto auth.checkPassword";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || password_file == 0) {
        if (stream != 0) {
            fputs("mtproto auth.checkPassword: invalid-arguments\n", stream);
        }
        return 2;
    }

    if (tg_mtproto_load_password_file(password_file, password_text,
                                      sizeof(password_text),
                                      &password_length, stream,
                                      label) != 0) {
        return 2;
    }
    (void)password_length;
    rc = tg_mtproto_auth_check_password_text(host, port, api_id_text,
                                             auth_file, dc_id_text,
                                             password_text, stream);
    tg_mtproto_secure_zero(password_text, sizeof(password_text));
    return rc;
}

int tg_mtproto_auth_check_password_file(const char *host,
                                        const char *port,
                                        const char *api_file,
                                        const char *auth_file,
                                        const char *dc_id_text,
                                        const char *password_file,
                                        FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto auth.checkPassword";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_check_password(host, port, api_id, auth_file,
                                        dc_id_text, password_file, stream);
    return rc;
}

int tg_mtproto_auth_login_wizard_file(const char *host,
                                      const char *port,
                                      const char *dc_id_text,
                                      const char *api_file,
                                      const char *auth_file,
                                      const char *code_hash_file,
                                      FILE *stream)
{
    char api_id[32];
    char phone[64];
    char code[64];
    char password[512];
    const char *current_host;
    const char *current_dc_id_text;
    unsigned long saved_timeout;
    int restore_timeout;
    int rc;
    static const char label[] = "mtproto login wizard";

    if (stream == 0 || host == 0 || port == 0 || dc_id_text == 0 ||
        api_file == 0 || auth_file == 0 || code_hash_file == 0) {
        if (stream != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    fprintf(stream, "Connecting to Telegram.\n");
    fflush(stream);

    saved_timeout = tg_net_connect_timeout_seconds();
    restore_timeout = 0;
    if (saved_timeout == 0UL ||
        saved_timeout > TG_MTPROTO_LOGIN_NETWORK_TIMEOUT_SECONDS) {
        tg_net_set_connect_timeout_seconds(
            TG_MTPROTO_LOGIN_NETWORK_TIMEOUT_SECONDS);
        restore_timeout = 1;
    }
    /* Telegram rejecting the number (format slip, stray glyph from an odd
       console path) re-prompts like a wrong code does, instead of dumping
       the user back to the shell to start over. */
    {
        int phone_attempt;

        rc = 2;
        for (phone_attempt = 0; phone_attempt < 3; ++phone_attempt) {
            current_host = host;
            current_dc_id_text = dc_id_text;
            if (tg_mtproto_prompt_line("Phone number: ", phone, sizeof(phone),
                                       1, stream, label) != 0) {
                if (restore_timeout) {
                    tg_net_set_connect_timeout_seconds(saved_timeout);
                }
                return 2;
            }
            tg_mtproto_sanitize_phone(phone);
            if (phone[0] == '\0') {
                fprintf(stream, "%s: input-empty\n", label);
                if (restore_timeout) {
                    tg_net_set_connect_timeout_seconds(saved_timeout);
                }
                return 2;
            }
            /* Echo the digits actually sent: the one honest way to spot a
               console keymap mangling the input. */
            fprintf(stream, "Using phone number %s.\n", phone);
            fprintf(stream, "Sending login code request.\n");
            fflush(stream);
            rc = tg_mtproto_auth_send_code_file(current_host, port,
                                                current_dc_id_text, api_file,
                                                phone, auth_file,
                                                code_hash_file, stream);
            if (rc > TG_MTPROTO_PHONE_MIGRATE_RC_BASE) {
                unsigned long migrate_dc;
                const char *migrate_host;
                const char *migrate_dc_text;

                migrate_dc =
                    (unsigned long)(rc - TG_MTPROTO_PHONE_MIGRATE_RC_BASE);
                if (tg_mtproto_production_endpoint_for_dc(
                        migrate_dc, &migrate_host, &migrate_dc_text) == 0) {
                    /* Show the DC switch and re-announce sendCode so the user
                       has a visible midway marker during the long DH
                       handshake. (We tried making this silent but the
                       resulting one-long-gap-before-dots looked like a freeze
                       on slow CPUs / flaky links.) */
                    fprintf(stream, "Using Telegram DC %s.\n", migrate_dc_text);
                    current_host = migrate_host;
                    current_dc_id_text = migrate_dc_text;
                    fprintf(stream, "Sending login code request.\n");
                    fflush(stream);
                    rc = tg_mtproto_auth_send_code_file(current_host, port,
                                                        current_dc_id_text,
                                                        api_file, phone,
                                                        auth_file,
                                                        code_hash_file,
                                                        stream);
                }
            }
            if (rc == 0) {
                break;
            }
            if (phone_attempt < 2) {
                fprintf(stream,
                        "Telegram did not accept that number. International "
                        "format, digits only, country code first (Italy: "
                        "39 then the mobile number). Empty input aborts.\n");
            }
        }
    }
    if (restore_timeout) {
        tg_net_set_connect_timeout_seconds(saved_timeout);
    }
    if (rc != 0) {
        tg_mtproto_secure_zero(phone, sizeof(phone));
        return rc;
    }

    fprintf(stream, "\nTelegram login code received.\n");
    fflush(stream);
    /* Re-prompt when Telegram rejects the code instead of mistaking it for a
       2FA challenge (which then failed confusingly on checkPassword). */
    rc = TG_MTPROTO_SIGN_IN_CODE_INVALID;
    while (rc == TG_MTPROTO_SIGN_IN_CODE_INVALID) {
        fprintf(stream, "Type the Telegram code and press Return.\n");
        fflush(stream);
        if (tg_mtproto_prompt_line("Telegram code (empty to abort): ", code,
                                   sizeof(code), 0, stream, label) != 0) {
            tg_mtproto_secure_zero(phone, sizeof(phone));
            return 2;
        }
        if (code[0] == '\0') {
            tg_mtproto_secure_zero(phone, sizeof(phone));
            fprintf(stream, "%s: aborted\n", label);
            return 2;
        }
        fprintf(stream, "Checking Telegram code.\n");
        fflush(stream);
        rc = tg_mtproto_auth_sign_in_file(current_host, port, api_file,
                                          auth_file, phone, code_hash_file,
                                          code, current_dc_id_text, stream);
        tg_mtproto_secure_zero(code, sizeof(code));
        if (rc == TG_MTPROTO_SIGN_IN_CODE_INVALID) {
            fprintf(stream,
                    "That code was not accepted. Check the latest Telegram "
                    "message and try again.\n");
        }
    }
    if (rc == TG_MTPROTO_SIGN_IN_PASSWORD_NEEDED) {
        fprintf(stream,
                "2FA password required.\n"
                "WARNING: two-step verification derives the key with PBKDF2 "
                "(100000 iterations of SHA-512). On a slow 68k -- e.g. a stock "
                "14 MHz 68020 -- this takes about 40 minutes, long enough for "
                "Telegram to drop the login before it finishes. If this machine "
                "is slow, disable Two-Step Verification on your account "
                "(Telegram app: Settings > Privacy and Security > Two-Step "
                "Verification) and sign in again. To try anyway, enter the "
                "password (empty to abort).\n");
        for (;;) {
            if (tg_mtproto_prompt_hidden_line("2FA password, empty to abort: ",
                                              password, sizeof(password),
                                              stream, label) != 0) {
                tg_mtproto_secure_zero(phone, sizeof(phone));
                return 2;
            }
            if (password[0] == '\0') {
                tg_mtproto_secure_zero(phone, sizeof(phone));
                tg_mtproto_secure_zero(password, sizeof(password));
                fprintf(stream, "%s: aborted\n", label);
                return 2;
            }
            rc = tg_mtproto_auth_check_password_text(current_host, port, api_id,
                                                     auth_file,
                                                     current_dc_id_text,
                                                     password, stream);
            tg_mtproto_secure_zero(password, sizeof(password));
            if (rc != TG_MTPROTO_CHECK_PASSWORD_INVALID) {
                break;
            }
            /* Wrong password: re-prompt instead of dropping out of the wizard. */
            fprintf(stream,
                    "That password was not accepted. Try again "
                    "(empty to abort).\n");
        }
        if (rc != 0) {
            tg_mtproto_secure_zero(phone, sizeof(phone));
            return rc;
        }
    } else if (rc != 0) {
        /* Any other sign-in failure was already reported by auth.signIn. */
        tg_mtproto_secure_zero(phone, sizeof(phone));
        return rc;
    }

    tg_mtproto_secure_zero(phone, sizeof(phone));
    {
        FILE *status_quiet = tg_mtproto_open_quiet_stream(stream);
        rc = tg_mtproto_auth_status_file(current_host, port, api_file,
                                         auth_file, current_dc_id_text,
                                         status_quiet);
        if (rc != 0) {
            tg_mtproto_replay_quiet_stream(status_quiet, stream);
        }
        tg_mtproto_close_quiet_stream(status_quiet, stream);
    }
    if (rc != 0) {
        return rc;
    }
    fprintf(stream, "Login complete.\n");
    return 0;
}

int tg_mtproto_auth_status(const char *host,
                           const char *port,
                           const char *api_id_text,
                           const char *auth_file,
                           const char *dc_id_text,
                           FILE *stream)
{
    unsigned char query[64];
    unsigned long query_length;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_user_summary user;
    static const char label[] = "mtproto auth.status";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || dc_id_text == 0) {
        if (stream != 0) {
            fputs("mtproto auth.status: invalid-arguments\n", stream);
        }
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_users_get_self(&writer) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    if (tg_mtproto_send_saved_query(host, port, api_id_text, auth_file,
                                    dc_id_text, query, query_length,
                                    &result, stream, label) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_is_auth_authorization_constructor(
            result.result_constructor)) {
        if (result.result_constructor == 0x44747e9aUL) {
            fprintf(stream, "%s: signup-required\n", label);
            return 2;
        }
        fprintf(stream, "%s: ok\n", label);
        return 0;
    }
    if (tg_mtproto_parse_user_vector_first(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &user) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: session-state-unknown constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    return 0;
}

int tg_mtproto_auth_status_file(const char *host,
                                const char *port,
                                const char *api_file,
                                const char *auth_file,
                                const char *dc_id_text,
                                FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto auth.status";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_status(host, port, api_id, auth_file, dc_id_text,
                                stream);
    return rc;
}

int tg_mtproto_auth_inspect(const char *auth_file, FILE *stream)
{
    tg_mtproto_session session;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    unsigned long auth_key_id_hi;
    unsigned long auth_key_id_lo;
    tg_mtproto_session_status status;
    static const char label[] = "mtproto auth.inspect";

    if (stream == 0 || auth_file == 0 || auth_file[0] == '\0') {
        if (stream != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }

    status = tg_mtproto_session_load_authorization(auth_file, &session,
                                                   auth_key);
    if (status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-invalid (%s)\n", label,
                tg_mtproto_session_status_name(status));
        return 2;
    }

    tg_mtproto_auth_key_id(auth_key, &auth_key_id_hi, &auth_key_id_lo);
    tg_mtproto_secure_zero(auth_key, sizeof(auth_key));

    fprintf(stream, "%s: file valid\n", label);
    fprintf(stream, "%s: dc_id=%lu\n", label, session.dc_id);
    fprintf(stream, "%s: auth_key=present\n", label);
    if (auth_key_id_hi == session.auth_key_id_hi &&
        auth_key_id_lo == session.auth_key_id_lo) {
        fprintf(stream, "%s: auth_key_id=matches\n", label);
    } else {
        fprintf(stream, "%s: auth_key_id=mismatch\n", label);
        return 2;
    }
    fprintf(stream, "%s: server_salt=present\n", label);
    fprintf(stream, "%s: session_id=present\n", label);
    fprintf(stream, "%s: seq_no=%lu\n", label, session.seq_no);
    if (session.last_msg_id_hi != 0UL || session.last_msg_id_lo != 0UL) {
        fprintf(stream, "%s: last_msg_id=present\n", label);
    } else {
        fprintf(stream, "%s: last_msg_id=none\n", label);
    }
    return 0;
}

int tg_mtproto_auth_check_local_files(const char *api_file,
                                      const char *auth_file,
                                      const char *password_file,
                                      const char *code_hash_file,
                                      FILE *stream)
{
    char api_id[32];
    char api_hash[96];
    char password[256];
    unsigned long password_length;
    tg_mtproto_session session;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    tg_mtproto_session_status session_status;
    int ok;
    static const char label[] = "mtproto local-files";

    if (stream == 0 || api_file == 0 || auth_file == 0) {
        if (stream != 0) {
            fprintf(stream, "%s: invalid-arguments\n", label);
        }
        return 2;
    }

    ok = 1;
    tg_mtproto_check_secret_file_permissions("api-file", api_file, stream);
    if (tg_mtproto_load_api_credentials(api_file, api_id, sizeof(api_id),
                                        api_hash, sizeof(api_hash),
                                        0, 0) != 0) {
        fprintf(stream, "%s: api-file invalid\n", label);
        ok = 0;
    } else {
        fprintf(stream, "%s: api-file ok\n", label);
    }
    tg_mtproto_secure_zero(api_hash, sizeof(api_hash));

    tg_mtproto_check_secret_file_permissions("auth-file", auth_file, stream);
    session_status = tg_mtproto_session_load_authorization(auth_file, &session,
                                                           auth_key);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file invalid (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        ok = 0;
    } else {
        fprintf(stream, "%s: auth-file ok\n", label);
        fprintf(stream, "%s: auth-file dc_id=%lu\n", label, session.dc_id);
    }
    tg_mtproto_secure_zero(auth_key, sizeof(auth_key));

    if (password_file != 0 && password_file[0] != '\0') {
        tg_mtproto_check_secret_file_permissions("password-file",
                                                 password_file, stream);
        if (tg_mtproto_load_password_file(password_file, password,
                                          sizeof(password),
                                          &password_length, 0, 0) != 0) {
            fprintf(stream, "%s: password-file invalid\n", label);
            ok = 0;
        } else {
            fprintf(stream, "%s: password-file ok\n", label);
        }
        tg_mtproto_secure_zero(password, sizeof(password));
    } else {
        fprintf(stream, "%s: password-file skipped\n", label);
    }

    if (code_hash_file != 0 && code_hash_file[0] != '\0') {
        tg_mtproto_check_secret_file_permissions("code-hash-file",
                                                 code_hash_file, stream);
        if (tg_mtproto_check_code_hash_file(code_hash_file, stream,
                                            label) != 0) {
            fprintf(stream, "%s: code-hash-file invalid\n", label);
            ok = 0;
        } else {
            fprintf(stream, "%s: code-hash-file ok\n", label);
        }
    } else {
        fprintf(stream, "%s: code-hash-file skipped\n", label);
    }

    if (!ok) {
        fprintf(stream, "%s: failed\n", label);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    return 0;
}

int tg_mtproto_auth_get_self(const char *host,
                             const char *port,
                             const char *api_id_text,
                             const char *auth_file,
                             const char *dc_id_text,
                             FILE *stream)
{
    unsigned char query[64];
    unsigned char wrapped_query[760];
    unsigned long api_id;
    unsigned long query_length;
    tg_mtproto_auth_context context;
    tg_mtproto_rpc_result result;
    tg_mtproto_session_status session_status;
    tg_mtproto_tl_writer writer;
    tg_mtproto_user_summary user;
    long dc_id;
    static const char label[] = "mtproto users.getUsers(self)";

    if (stream == 0 || host == 0 || port == 0 || api_id_text == 0 ||
        auth_file == 0 || tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0 ||
        tg_mtproto_parse_ulong_arg(api_id_text, &api_id) != 0) {
        if (stream != 0) {
            fputs("mtproto users.getUsers(self): invalid-arguments\n", stream);
        }
        return 2;
    }
    if (tg_mtproto_load_auth_context(host, port, auth_file, &context, stream,
                                     label) != 0) {
        return 2;
    }
    context.session.dc_id = (unsigned long)dc_id;

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_users_get_self(&writer) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    query_length = writer.length;
    if (tg_mtproto_build_initialized_query(&writer, wrapped_query,
                                           sizeof(wrapped_query), api_id,
                                           query, query_length) != 0) {
        tg_mtproto_close_auth_context(&context);
        fprintf(stream, "%s: init-connection-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_encrypted_query(&context, wrapped_query, writer.length,
                                        &result, stream, label) != 0) {
        tg_mtproto_close_auth_context(&context);
        return 2;
    }
    tg_mtproto_close_auth_context(&context);

    session_status = tg_mtproto_session_save_authorization(
        auth_file, &context.session, context.auth_key, 1);
    if (session_status != TG_MTPROTO_SESSION_OK) {
        fprintf(stream, "%s: auth-file-save-failed (%s)\n", label,
                tg_mtproto_session_status_name(session_status));
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_user_vector_first(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &user) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: user-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: id 0x%08lx%08lx\n", label, user.id_hi, user.id_lo);
    fprintf(stream, "%s: self %s bot %s\n", label,
            user.is_self ? "yes" : "no", user.is_bot ? "yes" : "no");
    if (user.first_name[0] != '\0' || user.last_name[0] != '\0') {
        fprintf(stream, "%s: name %s %s\n", label, user.first_name,
                user.last_name);
    }
    if (user.username[0] != '\0') {
        fprintf(stream, "%s: username %s\n", label, user.username);
    }
    return 0;
}

int tg_mtproto_auth_get_dialogs(const char *host,
                                const char *port,
                                const char *api_id_text,
                                const char *auth_file,
                                const char *dc_id_text,
                                const char *limit_text,
                                FILE *stream)
{
    unsigned char query[64];
    unsigned long limit;
    unsigned long i;
    tg_mtproto_dialogs_summary dialogs;
    tg_mtproto_dialog_peer_list peer_list;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto messages.getDialogs";

    if (stream == 0 || tg_mtproto_parse_ulong_arg(limit_text, &limit) != 0 ||
        limit == 0UL || limit > 100UL) {
        if (stream != 0) {
            fputs("mtproto messages.getDialogs: invalid-arguments\n", stream);
        }
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_dialogs(&writer, limit) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query(host, port, api_id_text, auth_file,
                                    dc_id_text, query, writer.length, &result,
                                    stream, label) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_dialogs_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &dialogs) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: dialogs-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            dialogs.constructor);
    fprintf(stream, "%s: dialogs %lu messages %lu chats %lu users %lu\n",
            label, dialogs.dialog_count, dialogs.message_count,
            dialogs.chat_count, dialogs.user_count);
    if (dialogs.is_slice || dialogs.is_not_modified) {
        fprintf(stream, "%s: count %lu\n", label, dialogs.count);
    }
    if (tg_mtproto_parse_dialog_peer_list(result.result_constructor,
                                          result.result_body,
                                          result.result_body_length,
                                          &peer_list) == TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: peer_count %lu\n", label, peer_list.count);
        for (i = 0UL; i < peer_list.count; ++i) {
            fprintf(stream,
                    "%s: peer %lu type %s id 0x%08lx%08lx top %lu unread %lu\n",
                    label, i + 1UL,
                    tg_mtproto_peer_constructor_name(
                        peer_list.peers[i].peer_constructor),
                    peer_list.peers[i].id_hi,
                    peer_list.peers[i].id_lo,
                    peer_list.peers[i].top_message,
                    peer_list.peers[i].unread_count);
        }
        if (peer_list.truncated) {
            fprintf(stream, "%s: peer_list_truncated\n", label);
        }
    } else if (dialogs.dialog_count != 0UL) {
        fprintf(stream, "%s: peer_list_parse_skipped\n", label);
    }
    return 0;
}

int tg_mtproto_auth_get_dialogs_file(const char *host,
                                     const char *port,
                                     const char *api_file,
                                     const char *auth_file,
                                     const char *dc_id_text,
                                     const char *limit_text,
                                     FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto messages.getDialogs";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_get_dialogs(host, port, api_id, auth_file,
                                     dc_id_text, limit_text, stream);
    return rc;
}

static void tg_mtproto_write_cache_text(FILE *stream, const char *text)
{
    unsigned long i;

    if (stream == 0 || text == 0) {
        return;
    }
    for (i = 0UL; text[i] != '\0'; ++i) {
        if (text[i] == '\r' || text[i] == '\n' || text[i] == '\t') {
            fputc(' ', stream);
        } else {
            fputc((unsigned char)text[i], stream);
        }
    }
}

#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__AROS__) || \
    defined(__MORPHOS__) || defined(__MORPHOS)
#define TG_MTPROTO_DISPLAY_LATIN1 1
#else
#define TG_MTPROTO_DISPLAY_LATIN1 0
#endif

/* 1 when message text/markers should be emitted as raw UTF-8: always on
   targets without the Latin-1 display layer, and on Amiga targets when the
   user picked --ui-charset utf8 (e.g. running over ssh from a modern
   terminal). */
static int tg_mtproto_display_utf8(void)
{
#if TG_MTPROTO_DISPLAY_LATIN1
    return tg_console_ui_charset() == TG_UI_CHARSET_UTF8;
#else
    return 1;
#endif
}

/* ASCII/Latin-1 emoticon for the emoji people actually send, or 0 when the
   codepoint has no readable rendition. A retro text client drawing ":)" is
   both honest and period-correct; everything else falls through to the '¤'
   placeholder rather than a bare '?', which reads as lost text. These helpers
   are pure (no Amiga deps) so they live outside the Latin-1 display guard and
   feed both the console path and the GUI text path. */
static const char *tg_mtproto_display_emoticon(unsigned long cp)
{
    switch (cp) {
    case 0x263aUL: /* white smiling face */
    case 0x1f600UL:
    case 0x1f642UL:
    case 0x1f60aUL:
    case 0x1f60cUL:
        return ":)";
    case 0x1f601UL:
    case 0x1f603UL:
    case 0x1f604UL:
    case 0x1f605UL:
    case 0x1f606UL:
        return ":D";
    case 0x1f602UL: /* tears of joy */
    case 0x1f923UL: /* rofl */
        return ":'D";
    case 0x1f609UL:
        return ";)";
    case 0x1f61bUL:
    case 0x1f61cUL:
    case 0x1f61dUL:
        return ":P";
    case 0x2639UL:
    case 0x1f641UL:
    case 0x1f61eUL:
    case 0x1f622UL:
        return ":(";
    case 0x1f62dUL: /* loudly crying */
        return ":'(";
    case 0x1f62eUL:
    case 0x1f632UL:
        return ":O";
    case 0x1f617UL:
    case 0x1f618UL:
    case 0x1f619UL:
    case 0x1f61aUL:
        return ":*";
    case 0x2764UL: /* heavy black heart */
    case 0x2665UL:
    case 0x1f499UL:
    case 0x1f49aUL:
    case 0x1f49bUL:
    case 0x1f49cUL:
    case 0x1f5a4UL:
    case 0x1f90dUL:
    case 0x1f90eUL:
    case 0x1f9e1UL:
    case 0x1f493UL: /* beating heart */
    case 0x1f495UL: /* two hearts */
    case 0x1f496UL: /* sparkling heart */
    case 0x1f497UL: /* growing heart */
    case 0x1f498UL: /* heart with arrow */
    case 0x1f49dUL: /* heart with ribbon */
    case 0x1f49eUL: /* revolving hearts */
    case 0x1f49fUL: /* heart decoration */
    case 0x1f60dUL: /* heart eyes */
    case 0x1f970UL: /* smiling with hearts */
        return "<3";
    case 0x1f494UL:
        return "</3";
    case 0x1f44dUL:
        return "(y)";
    case 0x1f44eUL:
        return "(n)";
    case 0x2705UL:
    case 0x2713UL:
    case 0x2714UL:
        return "v";
    case 0x274cUL:
    case 0x2716UL:
        return "x";
    case 0x2b50UL:
    case 0x1f31fUL:
        return "*";
    case 0x2192UL:
        return "->";
    case 0x2190UL:
        return "<-";
    case 0x1f60eUL: /* sunglasses */
        return "8)";
    case 0x1f607UL: /* halo */
        return "O:)";
    case 0x1f620UL: /* angry */
    case 0x1f621UL: /* pouting */
    case 0x1f624UL: /* triumph / huffing */
        return ">:(";
    case 0x1f614UL: /* pensive */
    case 0x1f615UL: /* confused */
    case 0x1f910UL: /* zipper-mouth */
    case 0x1f914UL: /* thinking */
    case 0x1f62cUL: /* grimace */
        return ":/";
    case 0x1f628UL: /* fearful */
    case 0x1f631UL: /* face screaming */
    case 0x1f62fUL: /* hushed */
    case 0x1f635UL: /* dizzy */
        return ":O";
    case 0x1f44cUL: /* OK hand */
        return "(ok)";
    case 0x1f634UL: /* sleeping */
    case 0x1f971UL: /* yawning */
        return "(zzz)";
    case 0x1f62aUL: /* sleepy */
    case 0x1f613UL: /* downcast w/ sweat */
    case 0x1f629UL: /* weary */
    case 0x1f62bUL: /* tired */
    case 0x1f97aUL: /* pleading */
        return ":(";
    case 0x1f60fUL: /* smirk */
        return ";)";
    case 0x1f60bUL: /* yum */
    case 0x1f924UL: /* drooling */
        return ":P";
    case 0x1f644UL: /* rolling eyes */
    case 0x1f612UL: /* unamused */
        return ":/";
    case 0x1f633UL: /* flushed */
        return ":O";
    case 0x1f389UL: /* party popper */
    case 0x1f38aUL: /* confetti ball */
    case 0x1f973UL: /* partying face */
    case 0x1f64cUL: /* raising hands */
    case 0x1f64bUL: /* happy person raising hand */
        return "\\o/";
    case 0x1f525UL: /* fire */
        return "(fire)";
    case 0x1f4aaUL: /* flexed biceps */
        return "(flex)";
    case 0x1f44fUL: /* clapping hands */
        return "(clap)";
    case 0x1f917UL: /* hugging face */
        return "(hug)";
    case 0x1f4afUL: /* hundred points */
        return "(100)";
    case 0x2728UL: /* sparkles */
        return "*";
    case 0x1f937UL: /* shrug */
        return "(shrug)";
    case 0x1f440UL: /* eyes */
        return "(eyes)";
    case 0x1f44bUL: /* waving hand */
        return "(wave)";
    case 0x1f91dUL: /* handshake */
        return "(shake)";
    case 0x270cUL: /* victory hand */
    case 0x1f91eUL: /* crossed fingers */
        return "v";
    case 0x1f64fUL: /* folded hands / thanks */
        return "(pray)";
    case 0x1f4a9UL: /* pile of poo */
        return "(poo)";
    case 0x2757UL: /* exclamation mark */
    case 0x2755UL: /* white exclamation */
        return "!";
    case 0x2753UL: /* question mark */
    case 0x2754UL: /* white question */
        return "?";
    case 0x1f339UL: /* rose */
    case 0x1f490UL: /* bouquet */
        return "(rose)";
    default:
        return 0;
    }
}

/* Codepoints that only modify a neighbouring emoji print as nothing at all.
   Without this, "<heart><variation-selector>" rendered as two '?'. */
int tg_mtproto_display_codepoint_is_invisible(unsigned long cp)
{
    return (cp >= 0xfe00UL && cp <= 0xfe0fUL) || /* variation selectors */
           (cp >= 0x200bUL && cp <= 0x200fUL) || /* ZW space/joiner/marks */
           (cp >= 0x1f3fbUL && cp <= 0x1f3ffUL) || /* skin tones */
           cp == 0x2060UL || cp == 0xfeffUL;       /* word joiner / BOM */
}

/* Symbol/emoji blocks with no Latin-1 shape: one neutral placeholder. */
static int tg_mtproto_display_is_symbol_block(unsigned long cp)
{
    return (cp >= 0x1f000UL && cp <= 0x1faffUL) ||
           (cp >= 0x2600UL && cp <= 0x27bfUL) ||
           (cp >= 0x2b00UL && cp <= 0x2bffUL) ||
           (cp >= 0x2190UL && cp <= 0x21ffUL) ||
           (cp >= 0x2300UL && cp <= 0x23ffUL);
}

/* GUI counterpart of tg_mtproto_print_display_codepoint: render one codepoint
   into `out` (cap >= 8) as ASCII/Latin-1 and return the bytes written. The
   mapping order mirrors the console path exactly, but symbol-block and unknown
   codepoints return 0 (OMIT) so the GUI skips them instead of drawing a box or
   a '?'. */
unsigned long tg_mtproto_display_codepoint_to_latin1(unsigned long cp,
                                                     char *out,
                                                     unsigned long cap)
{
    const char *emoticon;

    if (out == 0 || cap == 0UL) {
        return 0UL;
    }
    if (cp == '\r' || cp == '\n' || cp == '\t') {
        out[0] = ' ';
        return 1UL;
    }
    if (cp < 0x100UL) {
        out[0] = (char)(unsigned char)cp;
        return 1UL;
    }
    switch (cp) {
    case 0x2018UL:
    case 0x2019UL:
    case 0x02bcUL:
        out[0] = '\'';
        return 1UL;
    case 0x201cUL:
    case 0x201dUL:
        out[0] = '"';
        return 1UL;
    case 0x2013UL:
    case 0x2014UL:
    case 0x2212UL:
        out[0] = '-';
        return 1UL;
    case 0x2026UL:
        if (cap < 3UL) {
            return 0UL;
        }
        out[0] = '.';
        out[1] = '.';
        out[2] = '.';
        return 3UL;
    default:
        break;
    }
    if (tg_mtproto_display_codepoint_is_invisible(cp)) {
        return 0UL;
    }
    /* Flag emoji are pairs of regional indicators: render each as its country
       letter ("IT", "DE"), which is exactly the information. */
    if (cp >= 0x1f1e6UL && cp <= 0x1f1ffUL) {
        out[0] = (char)('A' + (int)(cp - 0x1f1e6UL));
        return 1UL;
    }
    emoticon = tg_mtproto_display_emoticon(cp);
    if (emoticon != 0) {
        unsigned long n;

        n = 0UL;
        while (emoticon[n] != '\0') {
            if (n >= cap) {
                return 0UL;
            }
            out[n] = emoticon[n];
            ++n;
        }
        return n;
    }
    /* Symbol-block placeholder and the final fallback both OMIT in the GUI. */
    if (tg_mtproto_display_is_symbol_block(cp)) {
        return 0UL;
    }
    return 0UL;
}

#if TG_MTPROTO_DISPLAY_LATIN1
static unsigned long tg_mtproto_utf8_read_codepoint(const char *text,
                                                    unsigned long *index)
{
    const unsigned char *bytes;
    unsigned long i;
    unsigned long cp;

    bytes = (const unsigned char *)text;
    i = *index;
    if (bytes[i] < 0x80U) {
        *index = i + 1UL;
        return bytes[i];
    }
    if ((bytes[i] & 0xe0U) == 0xc0U && bytes[i + 1UL] != '\0' &&
        (bytes[i + 1UL] & 0xc0U) == 0x80U) {
        cp = ((unsigned long)(bytes[i] & 0x1fU) << 6) |
             (unsigned long)(bytes[i + 1UL] & 0x3fU);
        *index = i + 2UL;
        return cp;
    }
    if ((bytes[i] & 0xf0U) == 0xe0U && bytes[i + 1UL] != '\0' &&
        bytes[i + 2UL] != '\0' &&
        (bytes[i + 1UL] & 0xc0U) == 0x80U &&
        (bytes[i + 2UL] & 0xc0U) == 0x80U) {
        cp = ((unsigned long)(bytes[i] & 0x0fU) << 12) |
             ((unsigned long)(bytes[i + 1UL] & 0x3fU) << 6) |
             (unsigned long)(bytes[i + 2UL] & 0x3fU);
        *index = i + 3UL;
        return cp;
    }
    if ((bytes[i] & 0xf8U) == 0xf0U && bytes[i + 1UL] != '\0' &&
        bytes[i + 2UL] != '\0' && bytes[i + 3UL] != '\0' &&
        (bytes[i + 1UL] & 0xc0U) == 0x80U &&
        (bytes[i + 2UL] & 0xc0U) == 0x80U &&
        (bytes[i + 3UL] & 0xc0U) == 0x80U) {
        cp = ((unsigned long)(bytes[i] & 0x07U) << 18) |
             ((unsigned long)(bytes[i + 1UL] & 0x3fU) << 12) |
             ((unsigned long)(bytes[i + 2UL] & 0x3fU) << 6) |
             (unsigned long)(bytes[i + 3UL] & 0x3fU);
        *index = i + 4UL;
        return cp;
    }
    *index = i + 1UL;
    return bytes[i];
}

static void tg_mtproto_print_display_codepoint(FILE *stream, unsigned long cp)
{
    const char *emoticon;

    if (cp == '\r' || cp == '\n' || cp == '\t') {
        fputc(' ', stream);
        return;
    }
    if (cp < 0x100UL) {
        fputc((unsigned char)cp, stream);
        return;
    }
    switch (cp) {
    case 0x2018UL:
    case 0x2019UL:
    case 0x02bcUL:
        fputc('\'', stream);
        return;
    case 0x201cUL:
    case 0x201dUL:
        fputc('"', stream);
        return;
    case 0x2013UL:
    case 0x2014UL:
    case 0x2212UL:
        fputc('-', stream);
        return;
    case 0x2026UL:
        fputs("...", stream);
        return;
    default:
        break;
    }
    if (tg_mtproto_display_codepoint_is_invisible(cp)) {
        return;
    }
    /* Flag emoji are pairs of regional indicators: print them as the two
       country letters ("IT", "DE"), which is exactly the information. */
    if (cp >= 0x1f1e6UL && cp <= 0x1f1ffUL) {
        fputc((int)('A' + (int)(cp - 0x1f1e6UL)), stream);
        return;
    }
    emoticon = tg_mtproto_display_emoticon(cp);
    if (emoticon != 0) {
        fputs(emoticon, stream);
        return;
    }
    if (tg_mtproto_display_is_symbol_block(cp)) {
        fputc(0xa4, stream); /* generic-symbol placeholder ('¤') */
        return;
    }
    fputc('?', stream);
}

/* Encode an ISO-8859-1 (Amiga console/keymap) line as UTF-8 for the MTProto
   wire. 0x00-0x7F pass through; 0x80-0xFF -> two-byte UTF-8. Output can be up to
   twice the input length. Returns 1 on success, 0 if it would overflow dst (dst
   is then left empty). Without this, an accented character typed on the Amiga
   (e.g. 'a-grave' = 0xE0) is sent as a lone 0xE0 byte, which is invalid UTF-8
   and Telegram replaces it with U+FFFD. */
static int tg_mtproto_latin1_to_utf8(const char *src, char *dst,
                                     unsigned long dst_size)
{
    const unsigned char *s;
    unsigned long i;
    unsigned long o;

    if (src == 0 || dst == 0 || dst_size == 0UL) {
        return 0;
    }
    s = (const unsigned char *)src;
    o = 0UL;
    for (i = 0UL; s[i] != '\0'; ++i) {
        unsigned char c = s[i];
        unsigned long emoji;

        /* An emoji pair from the composer (see tg_gui.h): two bytes in, the
           codepoint's UTF-8 out, three or four bytes, still within the
           "twice the input" bound this buffer is sized for. */
        if ((c == TG_GUI_EMOJI_PREFIX0 || c == TG_GUI_EMOJI_PREFIX1) &&
            s[i + 1U] != '\0' &&
            tg_gui_emoji_pair_at(src, i + 2UL, i, &emoji) &&
            emoji < tg_emoji_sheet_count) {
            unsigned long cp = tg_emoji_sheet_codepoints[emoji];
            unsigned long need = cp >= 0x10000UL ? 4UL : 3UL;

            if (o + need >= dst_size) {
                dst[0] = '\0';
                return 0;
            }
            if (need == 4UL) {
                dst[o++] = (char)(0xf0U | (cp >> 18));
                dst[o++] = (char)(0x80U | ((cp >> 12) & 0x3fU));
            } else {
                dst[o++] = (char)(0xe0U | (cp >> 12));
            }
            dst[o++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
            dst[o++] = (char)(0x80U | (cp & 0x3fU));
            ++i; /* the index byte */
            continue;
        }
        if (c < 0x80U) {
            if (o + 1UL >= dst_size) {
                dst[0] = '\0';
                return 0;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 2UL >= dst_size) {
                dst[0] = '\0';
                return 0;
            }
            dst[o++] = (char)(0xc0U | (c >> 6));
            dst[o++] = (char)(0x80U | (c & 0x3fU));
        }
    }
    dst[o] = '\0';
    return 1;
}
#endif

static void tg_mtproto_print_cache_text(FILE *stream, const char *text)
{
#if TG_MTPROTO_DISPLAY_LATIN1
    unsigned long i;
    unsigned long cp;

    if (stream == 0 || text == 0) {
        return;
    }
    if (tg_mtproto_display_utf8()) {
        /* --ui-charset utf8: the console understands UTF-8, skip transcoding. */
        tg_mtproto_write_cache_text(stream, text);
        return;
    }
    i = 0UL;
    while (text[i] != '\0') {
        cp = tg_mtproto_utf8_read_codepoint(text, &i);
        tg_mtproto_print_display_codepoint(stream, cp);
    }
#else
    tg_mtproto_write_cache_text(stream, text);
#endif
}

/*
 * Message bodies, unlike names/labels, keep their real line breaks: a
 * multi-line message prints on multiple console lines, with continuation
 * lines slightly indented so the message stays visually grouped under its
 * sender. Names keep using tg_mtproto_print_cache_text, which flattens
 * whitespace into spaces.
 */
static void tg_mtproto_print_message_text(FILE *stream, const char *text)
{
    unsigned long i;
    unsigned long cp;

    if (stream == 0 || text == 0) {
        return;
    }
    i = 0UL;
#if TG_MTPROTO_DISPLAY_LATIN1
    if (!tg_mtproto_display_utf8()) {
        while (text[i] != '\0') {
            cp = tg_mtproto_utf8_read_codepoint(text, &i);
            if (cp == '\r') {
                continue;
            }
            if (cp == '\n') {
                tg_console_ui_end_line(stream);
                fputs("  ", stream);
                continue;
            }
            tg_mtproto_print_display_codepoint(stream, cp);
        }
        return;
    }
#endif
    while (text[i] != '\0') {
        cp = (unsigned long)(unsigned char)text[i];
        ++i;
        if (cp == '\r') {
            continue;
        }
        if (cp == '\n') {
            tg_console_ui_end_line(stream);
            fputs("  ", stream);
            continue;
        }
        if (cp == '\t') {
            fputc(' ', stream);
            continue;
        }
        fputc((int)cp, stream);
    }
}

/* Maximum number of UTF-8 characters of a group/channel title shown as the
   per-line "[group]" prefix before it is truncated with "..". */
#define TG_MTPROTO_GROUP_LABEL_MAX 16UL

/* Print at most max_chars UTF-8 characters of label, appending ".." when the
   label was longer. Counting whole UTF-8 sequences (not raw bytes) keeps
   accented/multibyte titles from being cut in the middle of a character. */
static void tg_mtproto_print_label_truncated(FILE *stream, const char *label,
                                             unsigned long max_chars)
{
    char buf[128];
    unsigned long i;
    unsigned long out;
    unsigned long chars;
    int truncated;

    if (stream == 0 || label == 0) {
        return;
    }
    i = 0UL;
    out = 0UL;
    chars = 0UL;
    truncated = 0;
    while (label[i] != '\0') {
        unsigned char lead;
        unsigned long seq;
        unsigned long k;

        if (chars >= max_chars) {
            truncated = 1;
            break;
        }
        lead = (unsigned char)label[i];
        if (lead < 0x80U) {
            seq = 1UL;
        } else if ((lead & 0xE0U) == 0xC0U) {
            seq = 2UL;
        } else if ((lead & 0xF0U) == 0xE0U) {
            seq = 3UL;
        } else if ((lead & 0xF8U) == 0xF0U) {
            seq = 4UL;
        } else {
            seq = 1UL;
        }
        if (out + seq >= sizeof(buf)) {
            truncated = 1;
            break;
        }
        for (k = 0UL; k < seq && label[i] != '\0'; ++k) {
            buf[out++] = label[i++];
        }
        ++chars;
    }
    if (truncated) {
        while (out > 0UL && buf[out - 1UL] == ' ') {
            --out;
        }
    }
    buf[out] = '\0';
    tg_mtproto_print_cache_text(stream, buf);
    if (truncated) {
        fputs("..", stream);
    }
}

static void tg_mtproto_copy_cache_field(char *dest,
                                        unsigned long dest_size,
                                        const char *begin,
                                        const char *end)
{
    unsigned long length;

    if (dest == 0 || dest_size == 0UL) {
        return;
    }
    dest[0] = '\0';
    if (begin == 0) {
        return;
    }
    while (*begin == ' ' || *begin == '\t') {
        ++begin;
    }
    if (end == 0) {
        end = begin + strlen(begin);
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    if (end <= begin || (begin[0] == '-' && begin + 1 == end)) {
        return;
    }
    length = (unsigned long)(end - begin);
    if (length >= dest_size) {
        length = dest_size - 1UL;
    }
    memcpy(dest, begin, (size_t)length);
    dest[length] = '\0';
}

static unsigned long tg_mtproto_peer_constructor_from_name(const char *name)
{
    if (name == 0) {
        return 0UL;
    }
    if (strcmp(name, "user") == 0) {
        return TG_MTPROTO_PEER_USER_CONSTRUCTOR;
    }
    if (strcmp(name, "chat") == 0) {
        return TG_MTPROTO_PEER_CHAT_CONSTRUCTOR;
    }
    if (strcmp(name, "channel") == 0) {
        return TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR;
    }
    return 0UL;
}

static tg_mtproto_peer_cache_entry *tg_mtproto_peer_cache_find_local(
    tg_mtproto_peer_cache *cache,
    unsigned long peer_constructor,
    unsigned long id_hi,
    unsigned long id_lo)
{
    unsigned long i;

    if (cache == 0) {
        return 0;
    }
    for (i = 0UL; i < cache->count; ++i) {
        if (cache->entries[i].peer_constructor == peer_constructor &&
            cache->entries[i].id_hi == id_hi &&
            cache->entries[i].id_lo == id_lo) {
            return &cache->entries[i];
        }
    }
    return 0;
}

static unsigned long tg_mtproto_peer_cache_public_count(
    const tg_mtproto_peer_cache *cache)
{
    unsigned long i;
    unsigned long count;

    if (cache == 0) {
        return 0UL;
    }
    count = 0UL;
    for (i = 0UL; i < cache->count; ++i) {
        if (!cache->entries[i].is_self) {
            ++count;
        }
    }
    return count;
}

static void tg_mtproto_recount_peer_cache(tg_mtproto_peer_cache *cache)
{
    unsigned long i;

    if (cache == 0) {
        return;
    }
    cache->user_count = 0UL;
    cache->chat_count = 0UL;
    for (i = 0UL; i < cache->count; ++i) {
        if (cache->entries[i].peer_constructor ==
            TG_MTPROTO_PEER_USER_CONSTRUCTOR) {
            ++cache->user_count;
        } else if (cache->entries[i].peer_constructor ==
                       TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ||
                   cache->entries[i].peer_constructor ==
                       TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) {
            ++cache->chat_count;
        }
    }
}

static void tg_mtproto_copy_plain_cache_text(char *dest,
                                             unsigned long dest_size,
                                             const char *src)
{
    unsigned long i;

    if (dest == 0 || dest_size == 0UL) {
        return;
    }
    dest[0] = '\0';
    if (src == 0) {
        return;
    }
    for (i = 0UL; i + 1UL < dest_size && src[i] != '\0'; ++i) {
        if (src[i] == '\r' || src[i] == '\n' || src[i] == '\t') {
            dest[i] = ' ';
        } else {
            dest[i] = src[i];
        }
    }
    dest[i] = '\0';
}

static void tg_mtproto_copy_self_display_title(
    char *dest,
    unsigned long dest_size,
    const tg_mtproto_user_summary *user)
{
    unsigned long pos;
    unsigned long i;

    if (dest == 0 || dest_size == 0UL) {
        return;
    }
    dest[0] = '\0';
    if (user == 0) {
        return;
    }
    pos = 0UL;
    for (i = 0UL; user->first_name[i] != '\0' &&
         pos + 1UL < dest_size; ++i) {
        dest[pos++] = user->first_name[i];
    }
    if (pos > 0UL && user->last_name[0] != '\0' &&
        pos + 1UL < dest_size) {
        dest[pos++] = ' ';
    }
    for (i = 0UL; user->last_name[i] != '\0' &&
         pos + 1UL < dest_size; ++i) {
        dest[pos++] = user->last_name[i];
    }
    dest[pos] = '\0';
    if (dest[0] == '\0') {
        tg_mtproto_copy_plain_cache_text(dest, dest_size, user->username);
    }
}

static int tg_mtproto_peer_cache_set_self(
    tg_mtproto_peer_cache *cache,
    const tg_mtproto_user_summary *user)
{
    tg_mtproto_peer_cache_entry *entry;

    if (cache == 0 || user == 0) {
        return 2;
    }
    entry = tg_mtproto_peer_cache_find_local(
        cache, TG_MTPROTO_PEER_USER_CONSTRUCTOR, user->id_hi, user->id_lo);
    if (entry == 0) {
        if (cache->count >= TG_MTPROTO_PEER_CACHE_MAX) {
            cache->truncated = 1;
            return 2;
        }
        entry = &cache->entries[cache->count++];
        memset(entry, 0, sizeof(*entry));
        entry->peer_constructor = TG_MTPROTO_PEER_USER_CONSTRUCTOR;
        entry->id_hi = user->id_hi;
        entry->id_lo = user->id_lo;
    }
    entry->has_access_hash = user->has_access_hash;
    entry->access_hash_hi = user->access_hash_hi;
    entry->access_hash_lo = user->access_hash_lo;
    entry->is_self = 1;
    entry->is_bot = user->is_bot;
    tg_mtproto_copy_plain_cache_text(entry->username, sizeof(entry->username),
                                     user->username);
    tg_mtproto_copy_self_display_title(entry->title, sizeof(entry->title),
                                       user);
    tg_mtproto_recount_peer_cache(cache);
    return entry->title[0] != '\0' || entry->username[0] != '\0' ? 0 : 2;
}

static void tg_mtproto_merge_peer_cache_entry(
    tg_mtproto_peer_cache_entry *dest,
    const tg_mtproto_peer_cache_entry *src)
{
    char old_title[sizeof(dest->title)];
    char old_username[sizeof(dest->username)];
    unsigned long old_hash_hi;
    unsigned long old_hash_lo;
    int old_has_access_hash;

    if (dest == 0 || src == 0) {
        return;
    }
    strcpy(old_title, dest->title);
    strcpy(old_username, dest->username);
    old_hash_hi = dest->access_hash_hi;
    old_hash_lo = dest->access_hash_lo;
    old_has_access_hash = dest->has_access_hash;
    *dest = *src;
    if (!dest->has_access_hash && old_has_access_hash) {
        dest->has_access_hash = 1;
        dest->access_hash_hi = old_hash_hi;
        dest->access_hash_lo = old_hash_lo;
    }
    if (dest->title[0] == '\0') {
        strcpy(dest->title, old_title);
    }
    if (dest->username[0] == '\0') {
        strcpy(dest->username, old_username);
    }
}

static int tg_mtproto_load_peer_cache_file(const char *path,
                                           tg_mtproto_peer_cache *cache)
{
    FILE *file;
    char line[512];
    char type[24];
    char hash_text[32];
    char self_text[8];
    char bot_text[8];
    char *title;
    char *username;
    tg_mtproto_peer_cache_entry *entry;
    unsigned long peer_index;
    unsigned long public_count;
    unsigned long peer_constructor;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long top_message;
    unsigned long unread_count;
#ifdef TG_DIAG_TRACE
    unsigned long diag_lines;
#endif

    if (path == 0 || cache == 0) {
        return 2;
    }
    memset(cache, 0, sizeof(*cache));
    file = fopen(path, "r");
    if (file == 0) {
        return 2;
    }
    public_count = 0UL;
#ifdef TG_DIAG_TRACE
    /* A600 hunt: this parse is the hottest suspect in the crash window. A
       tick every 8 lines costs little on a diag build and, if the run dies
       mid-file, the last tick says HOW FAR it got -- which line of the
       tester's own peers file to look at. */
    diag_lines = 0UL;
    tg_gui_log("diag: peers file open");
#define TG_PEERS_DIAG_TICK() \
    do { \
        ++diag_lines; \
        if ((diag_lines & 7UL) == 0UL) { \
            char diag_msg[40]; \
            sprintf(diag_msg, "diag: peers line %lu", diag_lines); \
            tg_gui_log(diag_msg); \
        } \
    } while (0)
#else
#define TG_PEERS_DIAG_TICK() ((void)0)
#endif
    while (fgets(line, sizeof(line), file) != 0) {
        TG_PEERS_DIAG_TICK();
        peer_index = 0UL;
        id_hi = id_lo = top_message = unread_count = 0UL;
        type[0] = hash_text[0] = self_text[0] = bot_text[0] = '\0';
        if (strncmp(line, "self ", 5) == 0) {
            if (cache->count >= TG_MTPROTO_PEER_CACHE_MAX) {
                cache->truncated = 1;
                continue;
            }
            entry = &cache->entries[cache->count++];
            memset(entry, 0, sizeof(*entry));
            entry->peer_constructor = TG_MTPROTO_PEER_USER_CONSTRUCTOR;
            entry->is_self = 1;
            title = strstr(line, " title ");
            username = strstr(line, " username ");
            if (username != 0) {
                tg_mtproto_copy_cache_field(entry->username,
                                            sizeof(entry->username),
                                            username + 10, title);
            }
            if (title != 0) {
                tg_mtproto_copy_cache_field(entry->title,
                                            sizeof(entry->title),
                                            title + 7, 0);
            }
            continue;
        }
        if (sscanf(line,
                   "peer %lu type %23s id 0x%8lx%8lx access_hash %31s top %lu unread %lu self %7s bot %7s",
                   &peer_index, type, &id_hi, &id_lo, hash_text,
                   &top_message, &unread_count, self_text, bot_text) != 9) {
            continue;
        }
        peer_constructor = tg_mtproto_peer_constructor_from_name(type);
        if (peer_constructor == 0UL) {
            continue;
        }
        if (cache->count >= TG_MTPROTO_PEER_CACHE_MAX) {
            cache->truncated = 1;
            continue;
        }
        entry = &cache->entries[cache->count++];
        memset(entry, 0, sizeof(*entry));
        entry->peer_constructor = peer_constructor;
        entry->id_hi = id_hi;
        entry->id_lo = id_lo;
        entry->top_message = top_message;
        entry->unread_count = unread_count;
        entry->is_self = strcmp(self_text, "yes") == 0;
        entry->is_bot = strcmp(bot_text, "yes") == 0;
        if (hash_text[0] == '0' && hash_text[1] == 'x' &&
            sscanf(hash_text, "0x%8lx%8lx", &entry->access_hash_hi,
                   &entry->access_hash_lo) == 2) {
            entry->has_access_hash = 1;
        }
        title = strstr(line, " title ");
        username = strstr(line, " username ");
        if (username != 0) {
            tg_mtproto_copy_cache_field(entry->username,
                                        sizeof(entry->username),
                                        username + 10, title);
        }
        if (title != 0) {
            tg_mtproto_copy_cache_field(entry->title, sizeof(entry->title),
                                        title + 7, 0);
        }
        if (!entry->is_self) {
            ++public_count;
        }
    }
    fclose(file);
#ifdef TG_DIAG_TRACE
    {
        char diag_msg[48];

        sprintf(diag_msg, "diag: peers file done, %lu entries", cache->count);
        tg_gui_log(diag_msg);
    }
#endif
#undef TG_PEERS_DIAG_TICK
    tg_mtproto_recount_peer_cache(cache);
    return cache->count > 0UL || public_count > 0UL ? 0 : 2;
}

static int tg_mtproto_peer_cache_available(const char *path)
{
    /* static, not on the stack: a peer_cache is ~32 KiB at the raised cap and this
       runs at startup -- a stack copy here helped overflow the TUI's stack. */
    static tg_mtproto_peer_cache cache;

    return tg_mtproto_load_peer_cache_file(path, &cache) == 0 &&
           tg_mtproto_peer_cache_public_count(&cache) > 0UL;
}

static int tg_mtproto_peer_cache_next_offset(
    const tg_mtproto_peer_cache *cache,
    unsigned long *offset_id,
    unsigned long *peer_constructor,
    unsigned long *id_hi,
    unsigned long *id_lo,
    unsigned long *access_hash_hi,
    unsigned long *access_hash_lo,
    int *has_access_hash)
{
    const tg_mtproto_peer_cache_entry *entry;
    unsigned long i;

    if (offset_id != 0) {
        *offset_id = 0UL;
    }
    if (peer_constructor != 0) {
        *peer_constructor = 0UL;
    }
    if (id_hi != 0) {
        *id_hi = 0UL;
    }
    if (id_lo != 0) {
        *id_lo = 0UL;
    }
    if (access_hash_hi != 0) {
        *access_hash_hi = 0UL;
    }
    if (access_hash_lo != 0) {
        *access_hash_lo = 0UL;
    }
    if (has_access_hash != 0) {
        *has_access_hash = 0;
    }
    if (cache == 0 || cache->count == 0UL || offset_id == 0 ||
        peer_constructor == 0 || id_hi == 0 || id_lo == 0 ||
        access_hash_hi == 0 || access_hash_lo == 0 || has_access_hash == 0) {
        return 1;
    }
    i = cache->count;
    while (i > 0UL) {
        --i;
        entry = &cache->entries[i];
        if (entry->top_message == 0UL || entry->is_self) {
            continue;
        }
        if ((entry->peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR ||
             entry->peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) &&
            !entry->has_access_hash) {
            continue;
        }
        *offset_id = entry->top_message;
        *peer_constructor = entry->peer_constructor;
        *id_hi = entry->id_hi;
        *id_lo = entry->id_lo;
        *access_hash_hi = entry->access_hash_hi;
        *access_hash_lo = entry->access_hash_lo;
        *has_access_hash = entry->has_access_hash;
        return 0;
    }
    return 1;
}

static void tg_mtproto_merge_peer_cache(tg_mtproto_peer_cache *dest,
                                        const tg_mtproto_peer_cache *fresh)
{
    unsigned long i;
    tg_mtproto_peer_cache_entry *entry;

    if (dest == 0 || fresh == 0) {
        return;
    }
    if (fresh->total_dialog_count > dest->total_dialog_count) {
        dest->total_dialog_count = fresh->total_dialog_count;
    }
    for (i = 0UL; i < fresh->count; ++i) {
        entry = tg_mtproto_peer_cache_find_local(
            dest, fresh->entries[i].peer_constructor,
            fresh->entries[i].id_hi, fresh->entries[i].id_lo);
        if (entry != 0) {
            tg_mtproto_merge_peer_cache_entry(entry, &fresh->entries[i]);
            continue;
        }
        if (dest->count >= TG_MTPROTO_PEER_CACHE_MAX) {
            dest->truncated = 1;
            continue;
        }
        dest->entries[dest->count++] = fresh->entries[i];
    }
    if (fresh->truncated) {
        dest->truncated = 1;
    }
    tg_mtproto_recount_peer_cache(dest);
}

static int tg_mtproto_save_peer_cache_file(
    const char *path,
    const tg_mtproto_peer_cache *cache,
    FILE *stream,
    const char *label)
{
    FILE *file;
    unsigned long i;
    unsigned long public_index;
    unsigned long public_count;
    unsigned long public_user_count;
    const tg_mtproto_peer_cache_entry *self_entry;
    const tg_mtproto_peer_cache_entry *entry;

    if (path == 0 || cache == 0) {
        return 2;
    }
    file = fopen(path, "w");
    if (file == 0) {
        if (stream != 0) {
            fprintf(stream, "%s: peer-cache-open-failed\n", label);
        }
        return 2;
    }
    public_count = 0UL;
    public_user_count = 0UL;
    self_entry = 0;
    for (i = 0UL; i < cache->count; ++i) {
        entry = &cache->entries[i];
        if (entry->is_self) {
            self_entry = entry;
            continue;
        }
        ++public_count;
        if (entry->peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR) {
            ++public_user_count;
        }
    }
    fprintf(file, "mtproto-peer-cache-v1\n");
    fprintf(file, "count %lu total_dialogs %lu users %lu chats %lu\n",
            public_count, cache->total_dialog_count, public_user_count,
            cache->chat_count);
    if (self_entry != 0) {
        fprintf(file, "self username ");
        if (self_entry->username[0] != '\0') {
            tg_mtproto_write_cache_text(file, self_entry->username);
        } else {
            fputc('-', file);
        }
        fprintf(file, " title ");
        if (self_entry->title[0] != '\0') {
            tg_mtproto_write_cache_text(file, self_entry->title);
        } else {
            fputc('-', file);
        }
        fputc('\n', file);
    }
    public_index = 1UL;
    for (i = 0UL; i < cache->count; ++i) {
        entry = &cache->entries[i];
        if (entry->is_self) {
            continue;
        }
        fprintf(file,
                "peer %lu type %s id 0x%08lx%08lx access_hash ",
                public_index,
                tg_mtproto_peer_constructor_name(entry->peer_constructor),
                entry->id_hi, entry->id_lo);
        ++public_index;
        if (entry->has_access_hash) {
            fprintf(file, "0x%08lx%08lx", entry->access_hash_hi,
                    entry->access_hash_lo);
        } else {
            fprintf(file, "-");
        }
        fprintf(file, " top %lu unread %lu self %s bot %s username ",
                entry->top_message, entry->unread_count,
                entry->is_self ? "yes" : "no",
                entry->is_bot ? "yes" : "no");
        if (entry->username[0] != '\0') {
            tg_mtproto_write_cache_text(file, entry->username);
        } else {
            fputc('-', file);
        }
        fprintf(file, " title ");
        if (entry->title[0] != '\0') {
            tg_mtproto_write_cache_text(file, entry->title);
        } else {
            fputc('-', file);
        }
        fputc('\n', file);
    }
    if (cache->truncated) {
        fprintf(file, "truncated yes\n");
    }
    if (fclose(file) != 0) {
        if (stream != 0) {
            fprintf(stream, "%s: peer-cache-close-failed\n", label);
        }
        return 2;
    }
    return 0;
}

static FILE *tg_mtproto_open_quiet_stream(FILE *fallback);
static void tg_mtproto_close_quiet_stream(FILE *quiet, FILE *fallback);

static int tg_mtproto_auth_refresh_self_cache_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    FILE *stream)
{
    unsigned char query[64];
    int has_cache;
    static tg_mtproto_peer_cache cache;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_user_summary user;
    static const char label[] = "mtproto users.getSelf";

    if (stream == 0 || host == 0 || port == 0 || api_id == 0 ||
        auth_file == 0 || dc_id_text == 0 || context == 0 ||
        peer_cache_file == 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_users_get_self(&writer) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 4U) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_user_vector_first(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &user) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: user-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    has_cache = tg_mtproto_load_peer_cache_file(peer_cache_file, &cache) == 0;
    if (!has_cache) {
        memset(&cache, 0, sizeof(cache));
    }
    if (tg_mtproto_peer_cache_set_self(&cache, &user) != 0) {
        return 2;
    }
    return tg_mtproto_save_peer_cache_file(peer_cache_file, &cache, stream,
                                           label);
}

/* updates.getState: primes (or refreshes) the gap-handling cursor. */
static int tg_mtproto_chat_get_updates_state_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    tg_mtproto_updates_state *state,
    FILE *stream)
{
    unsigned char query[32];
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto updates.getState";

    if (stream == 0 || host == 0 || port == 0 || api_id == 0 ||
        auth_file == 0 || dc_id_text == 0 || context == 0 || state == 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_updates_get_state(&writer) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 4U) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_updates_state(result.result_constructor,
                                       result.result_body,
                                       result.result_body_length,
                                       state) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: state-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    return 0;
}

/*
 * One paced updates.getDifference call: harvests new inbound messages into
 * the notify queue (printed by the usual cross-chat printer) and advances
 * the cursor. pts_total_limit keeps each reply small enough for a slow
 * link -- this is the MorphOS notification path, where live pushes stay
 * suppressed because the full backlog drowns the connection.
 * Returns 0 when the cursor advanced, 1 on soft trouble (cursor kept).
 */
static int tg_mtproto_chat_drain_difference_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    tg_mtproto_updates_state *state,
    FILE *stream)
{
    unsigned char query[64];
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_tl_reader reader;
    static tg_mtproto_message_text message;
    tg_mtproto_dialog_peer dest;
    unsigned long vector_constructor;
    unsigned long count;
    unsigned long message_start;
    unsigned long i;
    static const char label[] = "mtproto updates.getDifference";

    if (stream == 0 || host == 0 || port == 0 || api_id == 0 ||
        auth_file == 0 || dc_id_text == 0 || context == 0 || state == 0 ||
        state->pts == 0UL) {
        return 1;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    /* Tiny batch on purpose: full Message objects are heavy, and large
       replies are the payload class that froze MorphOS machines. The
       backlog drains a few messages per pass instead. */
    if (tg_mtproto_build_updates_get_difference(&writer, state->pts,
                                                state->date, state->qts,
                                                6UL) != TG_MTPROTO_TL_OK) {
        return 1;
    }
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 4U) != 0) {
        return 1;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        (void)tg_mtproto_print_rpc_error(label, &result, stream);
        return 1;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 1;
    }
    if (result.result_constructor ==
        TG_MTPROTO_UPDATES_DIFFERENCE_EMPTY_CONSTRUCTOR) {
        tg_mtproto_tl_reader_init(&reader, result.result_body,
                                  result.result_body_length);
        if (tg_mtproto_tl_read_u32(&reader, &state->date) ==
                TG_MTPROTO_TL_OK) {
            (void)tg_mtproto_tl_read_u32(&reader, &state->seq);
        }
        return 0;
    }
    if (result.result_constructor ==
        TG_MTPROTO_UPDATES_DIFFERENCE_TOO_LONG_CONSTRUCTOR) {
        tg_mtproto_tl_reader_init(&reader, result.result_body,
                                  result.result_body_length);
        (void)tg_mtproto_tl_read_u32(&reader, &state->pts);
        return 0;
    }
    if (result.result_constructor !=
            TG_MTPROTO_UPDATES_DIFFERENCE_CONSTRUCTOR &&
        result.result_constructor !=
            TG_MTPROTO_UPDATES_DIFFERENCE_SLICE_CONSTRUCTOR) {
        fprintf(stream, "%s: unexpected constructor 0x%08lx\n", label,
                result.result_constructor);
        return 1;
    }
    /* difference / differenceSlice: new_messages:Vector<Message> first.
       Walk it with the history parser's read+resync pattern; the trailing
       vectors and state are not parsed -- the cursor is refreshed with a
       cheap updates.getState instead (messages skipped in between are
       caught on the next pass; the dedupe ring absorbs any overlap). */
    tg_mtproto_tl_reader_init(&reader, result.result_body,
                              result.result_body_length);
    if (tg_mtproto_tl_read_u32(&reader, &vector_constructor) !=
            TG_MTPROTO_TL_OK ||
        vector_constructor != TG_MTPROTO_TL_VECTOR_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(&reader, &count) != TG_MTPROTO_TL_OK) {
        return 1;
    }
    for (i = 0UL; i < count && reader.offset < reader.length; ++i) {
        message_start = reader.offset;
        if (tg_mtproto_read_update_message_text(&reader, &message, &dest) !=
            TG_MTPROTO_TL_OK) {
            if (!tg_mtproto_resync_message_text(&reader,
                                                message_start + 4UL)) {
                break;
            }
            continue;
        }
        tg_chat_notify_push_message(&message, &dest);
    }
    (void)tg_mtproto_chat_get_updates_state_on_context(
        host, port, api_id, auth_file, dc_id_text, context, state, stream);
    return 0;
}

int tg_mtproto_auth_list_peers_file(const char *host,
                                    const char *port,
                                    const char *api_file,
                                    const char *auth_file,
                                    const char *dc_id_text,
                                    const char *limit_text,
                                    const char *peer_cache_file,
                                    FILE *stream)
{
    unsigned char query[128];
    unsigned long limit;
    unsigned long i;
    unsigned long offset_id;
    unsigned long offset_peer_constructor;
    unsigned long offset_id_hi;
    unsigned long offset_id_lo;
    unsigned long offset_access_hash_hi;
    unsigned long offset_access_hash_lo;
    int offset_has_access_hash;
    int has_existing_cache;
    char api_id[32];
    tg_mtproto_dialogs_summary dialogs;
    static tg_mtproto_peer_cache cache;
    static tg_mtproto_peer_cache existing_cache;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto list-peers";

    if (stream == 0 || tg_mtproto_parse_ulong_arg(limit_text, &limit) != 0 ||
        limit == 0UL || limit > 100UL || peer_cache_file == 0) {
        if (stream != 0) {
            fputs("mtproto list-peers: invalid-arguments\n", stream);
        }
        return 2;
    }
    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    has_existing_cache =
        tg_mtproto_load_peer_cache_file(peer_cache_file, &existing_cache) == 0;
    if (has_existing_cache &&
        tg_mtproto_peer_cache_next_offset(&existing_cache, &offset_id,
                                          &offset_peer_constructor,
                                          &offset_id_hi, &offset_id_lo,
                                          &offset_access_hash_hi,
                                          &offset_access_hash_lo,
                                          &offset_has_access_hash) == 0) {
        fprintf(stream, "%s: page offset top %lu from cached peers %lu\n",
                label, offset_id, existing_cache.count);
    } else {
        offset_id = 0UL;
        offset_peer_constructor = 0UL;
        offset_id_hi = 0UL;
        offset_id_lo = 0UL;
        offset_access_hash_hi = 0UL;
        offset_access_hash_lo = 0UL;
        offset_has_access_hash = 0;
        fprintf(stream, "%s: page offset first\n", label);
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_dialogs_page(
            &writer, limit, offset_id, offset_peer_constructor, offset_id_hi,
            offset_id_lo, offset_access_hash_hi, offset_access_hash_lo,
            offset_has_access_hash) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_limited(
            host, port, api_id, auth_file, dc_id_text, query, writer.length,
            &result, stream, label, 2U, 0) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_dialogs_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &dialogs) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialog_peer_cache(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &cache) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: dialogs-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    if (has_existing_cache) {
        tg_mtproto_merge_peer_cache(&existing_cache, &cache);
        cache = existing_cache;
    }
    if (tg_mtproto_save_peer_cache_file(peer_cache_file, &cache, stream,
                                        label) != 0) {
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            dialogs.constructor);
    fprintf(stream, "%s: peers %lu total_dialogs %lu users %lu chats %lu\n",
            label, cache.count, cache.total_dialog_count, cache.user_count,
            cache.chat_count);
    for (i = 0UL; i < cache.count; ++i) {
        fprintf(stream, "%s: peer %lu type %s id 0x%08lx%08lx",
                label, i + 1UL,
                tg_mtproto_peer_constructor_name(
                    cache.entries[i].peer_constructor),
                cache.entries[i].id_hi, cache.entries[i].id_lo);
        if (cache.entries[i].title[0] != '\0') {
            fprintf(stream, " title ");
            tg_mtproto_print_cache_text(stream, cache.entries[i].title);
        }
        if (cache.entries[i].username[0] != '\0') {
            fprintf(stream, " username ");
            tg_mtproto_print_cache_text(stream, cache.entries[i].username);
        }
        fprintf(stream, " unread %lu\n", cache.entries[i].unread_count);
    }
    if (cache.truncated) {
        fprintf(stream, "%s: peer_cache_truncated\n", label);
    }
    if (cache.total_dialog_count > cache.count) {
        fprintf(stream, "%s: more_peers_available cached %lu total %lu\n",
                label, cache.count, cache.total_dialog_count);
    }
    fprintf(stream, "%s: peer_cache_saved %s\n", label, peer_cache_file);
    return 0;
}

static void tg_mtproto_normalize_username(const char *input,
                                          char *output,
                                          unsigned long output_size)
{
    unsigned long i;
    unsigned long pos;

    if (output == 0 || output_size == 0UL) {
        return;
    }
    output[0] = '\0';
    if (input == 0) {
        return;
    }
    while (*input == ' ' || *input == '\t' || *input == '@') {
        ++input;
    }
    if (strncmp(input, "https://t.me/", 13) == 0) {
        input += 13;
    } else if (strncmp(input, "http://t.me/", 12) == 0) {
        input += 12;
    } else if (strncmp(input, "t.me/", 5) == 0) {
        input += 5;
    }
    pos = 0UL;
    for (i = 0UL; input[i] != '\0' && pos + 1UL < output_size; ++i) {
        if (input[i] == ' ' || input[i] == '\t' || input[i] == '\r' ||
            input[i] == '\n' || input[i] == '/' || input[i] == '?') {
            break;
        }
        output[pos++] = input[i];
    }
    output[pos] = '\0';
}

int tg_mtproto_auth_resolve_username_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *username_text,
                                          const char *peer_cache_file,
                                          FILE *stream)
{
    unsigned char query[256];
    char api_id[32];
    char username[128];
    unsigned long i;
    int has_existing_cache;
    static tg_mtproto_peer_cache cache;
    static tg_mtproto_peer_cache existing_cache;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto resolve-username";

    if (stream == 0 || host == 0 || port == 0 || api_file == 0 ||
        auth_file == 0 || dc_id_text == 0 || username_text == 0 ||
        peer_cache_file == 0) {
        if (stream != 0) {
            fputs("mtproto resolve-username: invalid-arguments\n", stream);
        }
        return 2;
    }
    tg_mtproto_normalize_username(username_text, username, sizeof(username));
    if (username[0] == '\0') {
        fprintf(stream, "%s: empty-username\n", label);
        return 2;
    }
    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_contacts_resolve_username(&writer, username) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_limited(
            host, port, api_id, auth_file, dc_id_text, query, writer.length,
            &result, stream, label, 12U, 1) != 0) {
        fprintf(stream, "%s: modern-method-no-result, trying fallback\n",
                label);
        tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
        if (tg_mtproto_build_contacts_resolve_username_flags(&writer,
                                                             username) !=
            TG_MTPROTO_TL_OK) {
            fprintf(stream, "%s: fallback-query-build-failed\n", label);
            return 2;
        }
        if (tg_mtproto_send_saved_query_limited(
                host, port, api_id, auth_file, dc_id_text, query,
                writer.length, &result, stream, label, 12U, 1) != 0) {
            return 2;
        }
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    fprintf(stream, "%s: result constructor 0x%08lx body %lu\n", label,
            result.result_constructor, result.result_body_length);
    fflush(stream);
    if (tg_mtproto_parse_resolved_peer_cache(result.result_constructor,
                                            result.result_body,
                                            result.result_body_length,
                                            &cache) != TG_MTPROTO_TL_OK ||
        cache.count == 0UL) {
        fprintf(stream, "%s: peer-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    has_existing_cache =
        tg_mtproto_load_peer_cache_file(peer_cache_file, &existing_cache) == 0;
    if (has_existing_cache) {
        tg_mtproto_merge_peer_cache(&existing_cache, &cache);
        cache = existing_cache;
    }
    if (tg_mtproto_save_peer_cache_file(peer_cache_file, &cache, stream,
                                        label) != 0) {
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: username %s\n", label, username);
    for (i = 0UL; i < cache.count; ++i) {
        fprintf(stream, "%s: peer %lu type %s id 0x%08lx%08lx",
                label, i + 1UL,
                tg_mtproto_peer_constructor_name(
                    cache.entries[i].peer_constructor),
                cache.entries[i].id_hi, cache.entries[i].id_lo);
        if (cache.entries[i].title[0] != '\0') {
            fprintf(stream, " title ");
            tg_mtproto_print_cache_text(stream, cache.entries[i].title);
        }
        if (cache.entries[i].username[0] != '\0') {
            fprintf(stream, " username ");
            tg_mtproto_print_cache_text(stream, cache.entries[i].username);
        }
        fprintf(stream, "\n");
    }
    fprintf(stream, "%s: peer_cache_saved %s\n", label, peer_cache_file);
    return 0;
}

int tg_mtproto_auth_get_history_self(const char *host,
                                     const char *port,
                                     const char *api_id_text,
                                     const char *auth_file,
                                     const char *dc_id_text,
                                     const char *limit_text,
                                     FILE *stream)
{
    unsigned char query[64];
    unsigned long limit;
    tg_mtproto_messages_summary messages;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto messages.getHistory(self)";

    if (stream == 0 || tg_mtproto_parse_ulong_arg(limit_text, &limit) != 0 ||
        limit == 0UL || limit > 100UL) {
        if (stream != 0) {
            fputs("mtproto messages.getHistory(self): invalid-arguments\n",
                  stream);
        }
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_self(&writer, limit) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query(host, port, api_id_text, auth_file,
                                    dc_id_text, query, writer.length, &result,
                                    stream, label) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_messages_summary(result.result_constructor,
                                          result.result_body,
                                          result.result_body_length,
                                          &messages) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: messages-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            messages.constructor);
    fprintf(stream, "%s: messages %lu chats %lu users %lu\n", label,
            messages.message_count, messages.chat_count, messages.user_count);
    if (messages.is_slice || messages.is_not_modified ||
        messages.is_channel_messages) {
        fprintf(stream, "%s: count %lu\n", label, messages.count);
    }    return 0;
}

int tg_mtproto_auth_get_history_self_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *limit_text,
                                          FILE *stream)
{
    char api_id[32];
    int rc;
    static const char label[] = "mtproto messages.getHistory(self)";

    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0) {
        return 2;
    }
    rc = tg_mtproto_auth_get_history_self(host, port, api_id, auth_file,
                                          dc_id_text, limit_text, stream);
    return rc;
}

static int tg_mtproto_load_peer_cache_peer(const char *path,
                                           const char *peer_index_text,
                                           unsigned long *peer_constructor,
                                           unsigned long *peer_id_hi,
                                           unsigned long *peer_id_lo,
                                           unsigned long *access_hash_hi,
                                           unsigned long *access_hash_lo,
                                           int *has_access_hash,
                                           FILE *stream,
                                           const char *label)
{
    /* Saved Messages: the self chat has no cache row -- synthesize the
       sentinel peer; tg_write_input_peer turns it into inputPeerSelf. */
    if (peer_index_text != 0 && strcmp(peer_index_text, "self") == 0) {
        *peer_constructor = TG_MTPROTO_PEER_SELF_CONSTRUCTOR;
        *peer_id_hi = 0UL;
        *peer_id_lo = 0UL;
        *access_hash_hi = 0UL;
        *access_hash_lo = 0UL;
        *has_access_hash = 0;
        return 0;
    }
    FILE *file;
    char line[512];
    char type[24];
    unsigned long wanted_index;
    unsigned long index;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long hash_hi;
    unsigned long hash_lo;
    unsigned long constructor;
    int matched;

    if (path == 0 || peer_constructor == 0 || peer_id_hi == 0 ||
        peer_id_lo == 0 || access_hash_hi == 0 || access_hash_lo == 0 ||
        has_access_hash == 0 ||
        tg_mtproto_parse_ulong_arg(peer_index_text, &wanted_index) != 0 ||
        wanted_index == 0UL) {
        fprintf(stream, "%s: invalid-peer-cache-arguments\n", label);
        return 2;
    }
    file = fopen(path, "r");
    if (file == 0) {
        fprintf(stream, "%s: peer-cache-open-failed\n", label);
        return 2;
    }
    while (fgets(line, sizeof(line), file) != 0) {
        type[0] = '\0';
        index = 0UL;
        id_hi = id_lo = hash_hi = hash_lo = 0UL;
        matched = sscanf(line,
                         "peer %lu type %23s id 0x%8lx%8lx access_hash 0x%8lx%8lx",
                         &index, type, &id_hi, &id_lo, &hash_hi,
                         &hash_lo);
        if (index == wanted_index) {
            fclose(file);
            if (matched < 4) {
                fprintf(stream, "%s: peer-cache-parse-failed\n", label);
                return 2;
            }
            constructor = tg_mtproto_peer_constructor_from_name(type);
            if (constructor == 0UL) {
                fprintf(stream, "%s: peer-cache-type-unsupported\n", label);
                return 2;
            }
            *peer_constructor = constructor;
            *peer_id_hi = id_hi;
            *peer_id_lo = id_lo;
            *access_hash_hi = hash_hi;
            *access_hash_lo = hash_lo;
            *has_access_hash = matched == 6;
            if ((constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR ||
                 constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) &&
                !*has_access_hash) {
                fprintf(stream, "%s: peer-cache-access-hash-missing\n",
                        label);
                return 2;
            }
            return 0;
        }
    }
    fclose(file);
    fprintf(stream, "%s: peer-cache-index-not-found\n", label);
    return 2;
}

/* Prints the cached chat list. When current_index_text names a chat index,
   that entry is rendered in the prompt (bold) colour with a trailing marker
   so the active chat is visible at a glance. */
static void tg_chat_list_copy_name(char *dest, const char *src)
{
    strncpy(dest, src, TG_CHAT_LIST_NAME_MAX - 1U);
    dest[TG_CHAT_LIST_NAME_MAX - 1U] = '\0';
}

/* One-shot live refresh of the peer cache for the GUI sidebar: load the saved
   session to learn its DC, resolve the production endpoint, and run the same
   list-peers fetch the console chat session uses (limit 5, then a minimal
   retry for heavy dialogsSlice accounts). No interactive prompt, no held
   context -- it opens and closes its own connection inside list_peers_file.
   The caller then re-parses the cache through tg_mtproto_chat_list_parse.
   Returns 0 when the cache was (re)written or already usable, non-zero when no
   chats could be obtained (the caller falls back to whatever cache exists). */
int tg_mtproto_gui_refresh_peer_cache(const char *api_file,
                                      const char *auth_file,
                                      const char *peer_cache_file, FILE *stream)
{
    tg_mtproto_session session;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    tg_mtproto_session_status status;
    const char *host;
    const char *dc_id_text;

    if (api_file == 0 || auth_file == 0 || peer_cache_file == 0) {
        return 2;
    }
    status = tg_mtproto_session_load_authorization(auth_file, &session,
                                                   auth_key);
    tg_mtproto_secure_zero(auth_key, sizeof(auth_key));
    if (status != TG_MTPROTO_SESSION_OK) {
        return 2;
    }
    if (tg_mtproto_production_endpoint_for_dc(session.dc_id, &host,
                                              &dc_id_text) != 0) {
        return 2;
    }
    /* An existing peer cache is authoritative: never re-fetch getDialogs on every
       launch (wasteful, and on MorphOS freeze-prone). The TUI path already guards
       on cache presence the same way. */
    if (tg_mtproto_peer_cache_available(peer_cache_file)) {
        return 0;
    }
#if defined(__MORPHOS__) || defined(__MORPHOS)
    /* MorphOS: messages.getDialogs hard-freezes the machine on a real (heavy)
       account even at limit 1 -- the same dialogs payload class as the read-
       receipt getPeerDialogs already suppressed here (f14daa1). With no cache yet
       we open with an empty sidebar instead of freezing; the chat list is
       populated from the staged telegram-peers.txt cache. */
    (void)host;
    (void)dc_id_text;
    return 2;
#else
    {
        FILE *quiet;
        int rc;

        quiet = tg_mtproto_open_quiet_stream(stream);
        rc = tg_mtproto_auth_list_peers_file(host, "443", api_file, auth_file,
                                             dc_id_text, "5", peer_cache_file,
                                             quiet);
        tg_mtproto_close_quiet_stream(quiet, stream);
        if (rc != 0 && !tg_mtproto_peer_cache_available(peer_cache_file)) {
            quiet = tg_mtproto_open_quiet_stream(stream);
            rc = tg_mtproto_auth_list_peers_file(host, "443", api_file, auth_file,
                                                 dc_id_text, "1", peer_cache_file,
                                                 quiet);
            tg_mtproto_close_quiet_stream(quiet, stream);
        }
        return rc;
    }
#endif
}

int tg_mtproto_chat_list_parse(const char *path, unsigned long current_index,
                               tg_chat_list_row *rows, int max, int *file_missing)
{
    FILE *file;
    char line[512];
    char type[24];
    int count;

    if (file_missing != 0) {
        *file_missing = 0;
    }
    if (path == 0 || rows == 0 || max <= 0) {
        return 0;
    }
    file = fopen(path, "r");
    if (file == 0) {
        if (file_missing != 0) {
            *file_missing = 1;
        }
        return 0;
    }
    count = 0;
    while (count < max && fgets(line, sizeof(line), file) != 0) {
        tg_chat_list_row *row;
        unsigned long index;
        unsigned long unread;
        char *title;
        char *username;
        char *unread_text;

        index = 0UL;
        type[0] = '\0';
        if (sscanf(line, "peer %lu type %23s", &index, type) < 2) {
            continue;
        }
        row = &rows[count];
        row->index = index;
        row->is_user = strcmp(type, "user") == 0;
        unread = 0UL;
        unread_text = strstr(line, " unread ");
        if (unread_text != 0) {
            (void)sscanf(unread_text + 8, "%lu", &unread);
        }
        row->unread = unread;
        row->is_current = (current_index != 0UL && index == current_index);
        /* The peer id (id 0x<hi8><lo8>) lets a driver match a notification to
           this row; written by the cache, ignored by the console renderer. */
        row->peer_id_hi = 0UL;
        row->peer_id_lo = 0UL;
        {
            char *id_text;

            id_text = strstr(line, " id 0x");
            if (id_text != 0) {
                (void)sscanf(id_text + 6, "%8lx%8lx", &row->peer_id_hi,
                             &row->peer_id_lo);
            }
        }
        row->name[0] = '\0';
        row->name_is_username = 0;
        /* Name resolution, byte-for-byte as the old inline printer: title
           (unless "-"), else @username (unless "-"), else the type string. The
           file always writes a title field, so the @username fallback only
           fires for malformed lines; a present-but-"-" title yields a blank
           name (preserved quirk). */
        title = strstr(line, " title ");
        username = strstr(line, " username ");
        if (title != 0) {
            title += 7;
            tg_mtproto_trim_line(title);
            if (title[0] != '-' || title[1] != '\0') {
                tg_chat_list_copy_name(row->name, title);
            }
        } else if (username != 0) {
            char *cut;

            username += 10;
            cut = strstr(username, " title ");
            if (cut != 0) {
                *cut = '\0';
            }
            tg_mtproto_trim_line(username);
            if (username[0] != '-' || username[1] != '\0') {
                tg_chat_list_copy_name(row->name, username);
                row->name_is_username = 1;
            }
        } else {
            tg_chat_list_copy_name(row->name, type);
        }
        ++count;
    }
    fclose(file);
    return count;
}

/* Console rendering of the chat list: two passes (single chats, then groups and
   channels), each with its header, the per-row "N. name[ U new][ *]". The
   driver-agnostic rows come from tg_mtproto_chat_list_parse; the GUI driver
   fills tg_gui_state.chats from the same rows instead. */
static void tg_mtproto_chat_list_render_console(FILE *stream,
                                                const tg_chat_list_row *rows,
                                                int count)
{
    int printed_single;
    int printed_group;
    int pass;

    if (stream == 0) {
        return;
    }
    if (count <= 0) {
        fprintf(stream, "No chats available.\n");
        return;
    }
    printed_single = 0;
    printed_group = 0;
    for (pass = 0; pass < 2; ++pass) {
        int want_user;
        int i;

        want_user = pass == 0;
        for (i = 0; i < count; ++i) {
            const tg_chat_list_row *row;

            row = &rows[i];
            if ((row->is_user != 0) != (want_user != 0)) {
                continue;
            }
            if (row->is_user) {
                if (!printed_single) {
                    fputs("Single chats:", stream);
                    tg_console_ui_end_line(stream);
                    printed_single = 1;
                }
            } else {
                if (!printed_group) {
                    if (printed_single) {
                        tg_console_ui_end_line(stream);
                    }
                    fputs("Groups and channels:", stream);
                    tg_console_ui_end_line(stream);
                    printed_group = 1;
                }
            }
            if (row->is_current) {
                tg_console_ui_role(stream, TG_UI_ROLE_PROMPT);
            }
            fprintf(stream, "%lu. ", row->index);
            if (row->name[0] != '\0') {
                if (row->name_is_username) {
                    fprintf(stream, "@");
                }
                tg_mtproto_print_cache_text(stream, row->name);
            }
            if (row->unread > 0UL) {
                tg_console_ui_role(stream, TG_UI_ROLE_NOTIFY);
                fprintf(stream, " %lu new", row->unread);
                tg_console_ui_reset(stream);
            }
            if (row->is_current) {
                fputs(" *", stream);
                tg_console_ui_reset(stream);
            }
            tg_console_ui_end_line(stream);
        }
    }
}

static void tg_mtproto_print_peer_cache_public(const char *path, FILE *stream,
                                               const char *current_index_text)
{
    tg_chat_list_row rows[TG_CHAT_LIST_MAX];
    unsigned long current_index;
    const char *digits;
    int count;
    int file_missing;

    current_index = 0UL;
    if (current_index_text != 0) {
        digits = current_index_text;
        while (*digits >= '0' && *digits <= '9') {
            current_index = (current_index * 10UL) +
                            (unsigned long)(*digits - '0');
            ++digits;
        }
        if (*digits != '\0') {
            current_index = 0UL;
        }
    }
    count = tg_mtproto_chat_list_parse(path, current_index, rows,
                                       TG_CHAT_LIST_MAX, &file_missing);
    if (file_missing) {
        fprintf(stream, "No cached chats yet.\n");
        return;
    }
    tg_mtproto_chat_list_render_console(stream, rows, count);
    /* The GUI pins Saved Messages as the last sidebar row; the console pins
       it as row 0 (cache indexes are 1-based, so 0 is forever free): it reads
       and picks like every other row, at startup and in-chat alike. */
    fprintf(stream, "0. Saved Messages (your cloud drawer)\n");
}

#if !defined(TG_NO_SELFTEST)
static const char tg_chat_list_golden[] =
    "Single chats:\n"
    "1. Mario Rossi\n"
    "2. \n"
    "3. @carla\n"
    "\n"
    "Groups and channels:\n"
    "4. Dev Group 9 new\n"
    "5. News 11 new *\n";

int tg_mtproto_chat_list_self_test(void)
{
    static const char path[] = "tg-chatlist-selftest.tmp";
    tg_chat_list_row rows[TG_CHAT_LIST_MAX];
    FILE *file;
    FILE *cap;
    char buf[1024];
    size_t n;
    int count;
    int file_missing;
    int saved_color;
    int saved_charset;
    int saved_theme;

    file = fopen(path, "w");
    if (file == 0) {
        puts("chat list self-test: cannot write temp cache");
        return 2;
    }
    /* user w/ title; user w/ title "-" (blank name quirk); user w/o title
       (@username fallback); group w/ unread; channel w/ unread + current. */
    fputs("peer 1 type user id 0x0 access_hash - top 0 unread 0 self no bot no "
          "username mario title Mario Rossi\n",
          file);
    fputs("peer 2 type user id 0x0 access_hash - top 0 unread 0 self no bot no "
          "username pippo title -\n",
          file);
    fputs("peer 3 type user id 0x0 access_hash - top 0 unread 0 self no bot no "
          "username carla\n",
          file);
    fputs("peer 4 type group id 0x0 access_hash - top 0 unread 9 self no bot no "
          "username - title Dev Group\n",
          file);
    fputs("peer 5 type channel id 0x0 access_hash - top 0 unread 11 self no bot "
          "no username news title News\n",
          file);
    fclose(file);

    saved_color = tg_console_ui_color_mode();
    saved_charset = tg_console_ui_charset();
    saved_theme = tg_console_ui_theme();
    tg_console_ui_set_color_mode(TG_UI_COLOR_OFF);
    tg_console_ui_set_charset(TG_UI_CHARSET_LATIN1);
    tg_console_ui_set_theme(TG_UI_THEME_PLAIN);

    count = tg_mtproto_chat_list_parse(path, 5UL, rows, TG_CHAT_LIST_MAX,
                                       &file_missing);
    remove(path);

    cap = tmpfile();
    if (cap == 0) {
        tg_console_ui_set_color_mode(saved_color);
        tg_console_ui_set_charset(saved_charset);
        tg_console_ui_set_theme(saved_theme);
        puts("chat list self-test: cannot open temp file");
        return 2;
    }
    tg_mtproto_chat_list_render_console(cap, rows, count);
    rewind(cap);
    n = fread(buf, 1, sizeof(buf) - 1U, cap);
    buf[n] = '\0';
    fclose(cap);

    tg_console_ui_set_color_mode(saved_color);
    tg_console_ui_set_charset(saved_charset);
    tg_console_ui_set_theme(saved_theme);

    if (file_missing) {
        puts("chat list self-test: temp cache reported missing");
        return 2;
    }
    if (strcmp(buf, tg_chat_list_golden) != 0) {
        printf("chat list self-test: MISMATCH\n---actual(%lu)---\n%s---end---\n",
               (unsigned long)n, buf);
        return 2;
    }
    puts("chat list self-test: ok (grouped chat-list golden)");
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_mtproto_load_self_cache_label(const char *path,
                                            char *label_buffer,
                                            unsigned long label_buffer_size)
{
    FILE *file;
    char line[512];
    char *title;
    char *username;

    if (label_buffer == 0 || label_buffer_size == 0UL) {
        return 2;
    }
    label_buffer[0] = '\0';
    if (path == 0) {
        return 2;
    }
    file = fopen(path, "r");
    if (file == 0) {
        return 2;
    }
    while (fgets(line, sizeof(line), file) != 0) {
        if (strncmp(line, "self ", 5) != 0) {
            continue;
        }
        title = strstr(line, " title ");
        username = strstr(line, " username ");
        if (title != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        title + 7, 0);
        }
        if (label_buffer[0] == '\0' && username != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        username + 10, title);
        }
        fclose(file);
        return label_buffer[0] != '\0' ? 0 : 2;
    }
    fclose(file);
    return 2;
}

static int tg_mtproto_load_peer_cache_label(const char *path,
                                            const char *peer_index_text,
                                            char *label_buffer,
                                            unsigned long label_buffer_size)
{
    if (peer_index_text != 0 && strcmp(peer_index_text, "self") == 0) {
        if (label_buffer_size > 0UL) {
            strncpy(label_buffer, "Saved Messages", label_buffer_size - 1UL);
            label_buffer[label_buffer_size - 1UL] = '\0';
        }
        return 0;
    }
    FILE *file;
    char line[512];
    char type[24];
    unsigned long wanted_index;
    unsigned long index;
    char *title;
    char *username;

    if (label_buffer == 0 || label_buffer_size == 0UL) {
        return 2;
    }
    label_buffer[0] = '\0';
    if (path == 0 || peer_index_text == 0 ||
        tg_mtproto_parse_ulong_arg(peer_index_text, &wanted_index) != 0 ||
        wanted_index == 0UL) {
        return 2;
    }
    file = fopen(path, "r");
    if (file == 0) {
        return 2;
    }
    while (fgets(line, sizeof(line), file) != 0) {
        index = 0UL;
        type[0] = '\0';
        if (sscanf(line, "peer %lu type %23s", &index, type) < 2 ||
            index != wanted_index) {
            continue;
        }
        title = strstr(line, " title ");
        username = strstr(line, " username ");
        if (title != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        title + 7, 0);
        }
        if (label_buffer[0] == '\0' && username != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        username + 10, title);
        }
        fclose(file);
        return label_buffer[0] != '\0' ? 0 : 2;
    }
    fclose(file);
    return 2;
}

static int tg_mtproto_ascii_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

static int tg_mtproto_ascii_contains_ci(const char *text, const char *needle)
{
    unsigned long i;
    unsigned long j;

    if (text == 0 || needle == 0 || needle[0] == '\0') {
        return 0;
    }
    for (i = 0UL; text[i] != '\0'; ++i) {
        j = 0UL;
        while (needle[j] != '\0' && text[i + j] != '\0' &&
               tg_mtproto_ascii_lower((unsigned char)text[i + j]) ==
                   tg_mtproto_ascii_lower((unsigned char)needle[j])) {
            ++j;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void tg_mtproto_peer_cache_entry_label(
    const tg_mtproto_peer_cache_entry *entry,
    char *label,
    unsigned long label_size)
{
    if (label == 0 || label_size == 0UL) {
        return;
    }
    label[0] = '\0';
    if (entry == 0) {
        return;
    }
    if (entry->title[0] != '\0') {
        tg_mtproto_copy_plain_cache_text(label, label_size, entry->title);
    } else if (entry->username[0] != '\0') {
        tg_mtproto_copy_plain_cache_text(label, label_size, entry->username);
    }
}

static void tg_mtproto_print_peer_cache_entry_line(
    FILE *stream,
    unsigned long public_index,
    const tg_mtproto_peer_cache_entry *entry)
{
    if (stream == 0 || entry == 0) {
        return;
    }
    fprintf(stream, "%lu. ", public_index);
    if (entry->title[0] != '\0') {
        tg_mtproto_print_cache_text(stream, entry->title);
        if (entry->username[0] != '\0') {
            fprintf(stream, " (@");
            tg_mtproto_print_cache_text(stream, entry->username);
            fprintf(stream, ")");
        }
    } else if (entry->username[0] != '\0') {
        fprintf(stream, "@");
        tg_mtproto_print_cache_text(stream, entry->username);
    } else {
        fprintf(stream, "%s",
                tg_mtproto_peer_constructor_name(entry->peer_constructor));
    }
    fprintf(stream, "\n");
}

static unsigned long tg_mtproto_peer_cache_search_public(
    const char *path,
    const char *query,
    FILE *stream)
{
    tg_mtproto_peer_cache cache;
    char trimmed_query[128];
    unsigned long i;
    unsigned long public_index;
    unsigned long match_count;
    const tg_mtproto_peer_cache_entry *entry;

    if (stream == 0 || path == 0 || query == 0) {
        return 0UL;
    }
    tg_mtproto_copy_plain_cache_text(trimmed_query, sizeof(trimmed_query),
                                     query);
    tg_mtproto_trim_line(trimmed_query);
    if (trimmed_query[0] == '\0') {
        fprintf(stream, "Type something to search for.\n");
        return 0UL;
    }
    if (tg_mtproto_load_peer_cache_file(path, &cache) != 0 ||
        tg_mtproto_peer_cache_public_count(&cache) == 0UL) {
        fprintf(stream, "No cached chats yet. Add one with /add @username.\n");
        return 0UL;
    }
    public_index = 0UL;
    match_count = 0UL;
    for (i = 0UL; i < cache.count; ++i) {
        entry = &cache.entries[i];
        if (entry->is_self) {
            continue;
        }
        ++public_index;
        if (!tg_mtproto_ascii_contains_ci(entry->title, trimmed_query) &&
            !tg_mtproto_ascii_contains_ci(entry->username, trimmed_query)) {
            continue;
        }
        if (match_count == 0UL) {
            fprintf(stream, "Matches:\n");
        }
        tg_mtproto_print_peer_cache_entry_line(stream, public_index, entry);
        ++match_count;
    }
    if (match_count == 0UL) {
        fprintf(stream, "No cached chat matches that name.\n");
    } else {
        fprintf(stream, "Type the number to switch chat.\n");
    }
    return match_count;
}

static int tg_mtproto_peer_cache_remove_public_index(
    const char *path,
    const char *peer_index_text,
    char *removed_label,
    unsigned long removed_label_size,
    FILE *stream)
{
    tg_mtproto_peer_cache cache;
    unsigned long wanted_index;
    unsigned long public_index;
    unsigned long i;
    unsigned long remove_index;

    if (removed_label != 0 && removed_label_size > 0UL) {
        removed_label[0] = '\0';
    }
    if (path == 0 || peer_index_text == 0 ||
        tg_mtproto_parse_ulong_arg(peer_index_text, &wanted_index) != 0 ||
        wanted_index == 0UL) {
        fprintf(stream, "Use /remove <number>.\n");
        return 2;
    }
    if (tg_mtproto_load_peer_cache_file(path, &cache) != 0) {
        fprintf(stream, "No cached chats to remove.\n");
        return 2;
    }
    public_index = 0UL;
    remove_index = cache.count;
    for (i = 0UL; i < cache.count; ++i) {
        if (cache.entries[i].is_self) {
            continue;
        }
        ++public_index;
        if (public_index == wanted_index) {
            remove_index = i;
            break;
        }
    }
    if (remove_index >= cache.count) {
        fprintf(stream, "Chat number not found.\n");
        return 2;
    }
    tg_mtproto_peer_cache_entry_label(&cache.entries[remove_index],
                                      removed_label, removed_label_size);
    for (i = remove_index; i + 1UL < cache.count; ++i) {
        cache.entries[i] = cache.entries[i + 1UL];
    }
    --cache.count;
    tg_mtproto_recount_peer_cache(&cache);
    if (tg_mtproto_save_peer_cache_file(path, &cache, 0,
                                        "chat cache") != 0) {
        fprintf(stream, "Could not update cached chats.\n");
        return 2;
    }
    return 0;
}

/* Move the chat at public index src_public to public index dst_public (both
   1-based, == sidebar row + 1, skipping the is_self entry) and persist. The new
   array order of the non-self entries IS the new on-disk/sidebar order (save emits
   them in array order, renumbered 1..N). Mirrors remove_public_index's
   load/mutate/save shape. Returns 0 on success. */
static int tg_mtproto_peer_cache_reorder_public_index(
    const char *path,
    unsigned long src_public,
    unsigned long dst_public,
    FILE *stream)
{
    tg_mtproto_peer_cache cache;
    unsigned long pub[TG_MTPROTO_PEER_CACHE_MAX];
    unsigned long n;
    unsigned long i;
    unsigned long src_arr;
    unsigned long dst_arr;
    tg_mtproto_peer_cache_entry moved;

    if (path == 0 || src_public == 0UL || dst_public == 0UL) {
        return 2;
    }
    if (src_public == dst_public) {
        return 0; /* no-op */
    }
    if (tg_mtproto_load_peer_cache_file(path, &cache) != 0) {
        if (stream != 0) {
            fprintf(stream, "No cached chats to reorder.\n");
        }
        return 2;
    }
    /* Array indices of the non-self entries, in public order (self is at index 0
       after a save/load round-trip and is skipped on save, so it keeps its slot). */
    n = 0UL;
    for (i = 0UL; i < cache.count; ++i) {
        if (!cache.entries[i].is_self) {
            if (n < TG_MTPROTO_PEER_CACHE_MAX) {
                pub[n] = i;
            }
            ++n;
        }
    }
    if (n == 0UL || n > TG_MTPROTO_PEER_CACHE_MAX ||
        src_public > n || dst_public > n) {
        return 2;
    }
    src_arr = pub[src_public - 1UL];
    dst_arr = pub[dst_public - 1UL];
    moved = cache.entries[src_arr];
    if (src_arr < dst_arr) {
        for (i = src_arr; i < dst_arr; ++i) {
            cache.entries[i] = cache.entries[i + 1UL];
        }
    } else {
        for (i = src_arr; i > dst_arr; --i) {
            cache.entries[i] = cache.entries[i - 1UL];
        }
    }
    cache.entries[dst_arr] = moved;
    tg_mtproto_recount_peer_cache(&cache);
    if (tg_mtproto_save_peer_cache_file(path, &cache, 0, "chat cache") != 0) {
        if (stream != 0) {
            fprintf(stream, "Could not save chat order.\n");
        }
        return 2;
    }
    return 0;
}

static const char *tg_mtproto_peer_cache_entry_kind(
    const tg_mtproto_peer_cache_entry *entry)
{
    if (entry == 0) {
        return "chat";
    }
    if (entry->peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR) {
        return entry->is_bot ? "bot" : "user";
    }
    if (entry->peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR) {
        return "group";
    }
    if (entry->peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) {
        return "group/channel";
    }
    return "chat";
}

static int tg_mtproto_peer_cache_entry_is_openable(
    const tg_mtproto_peer_cache_entry *entry)
{
    if (entry == 0 || entry->is_self) {
        return 0;
    }
    if (entry->peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR ||
        entry->peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) {
        return entry->has_access_hash;
    }
    return entry->peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR;
}

static void tg_mtproto_print_search_result_line(
    FILE *stream,
    unsigned long index,
    const tg_mtproto_peer_cache_entry *entry)
{
    if (stream == 0 || entry == 0) {
        return;
    }
    fprintf(stream, "%lu. ", index);
    if (entry->title[0] != '\0') {
        tg_mtproto_print_cache_text(stream, entry->title);
        if (entry->username[0] != '\0') {
            fprintf(stream, " (@");
            tg_mtproto_print_cache_text(stream, entry->username);
            fprintf(stream, ")");
        }
    } else if (entry->username[0] != '\0') {
        fprintf(stream, "@");
        tg_mtproto_print_cache_text(stream, entry->username);
    } else {
        fprintf(stream, "%s",
                tg_mtproto_peer_constructor_name(entry->peer_constructor));
    }
    fprintf(stream, " [%s", tg_mtproto_peer_cache_entry_kind(entry));
    if (!tg_mtproto_peer_cache_entry_is_openable(entry)) {
        fprintf(stream, ", cannot open");
    }
    fprintf(stream, "]\n");
}

static int tg_mtproto_peer_cache_find_public_index(
    const tg_mtproto_peer_cache *cache,
    const tg_mtproto_peer_cache_entry *wanted,
    unsigned long *public_index_out)
{
    unsigned long i;
    unsigned long public_index;

    if (public_index_out != 0) {
        *public_index_out = 0UL;
    }
    if (cache == 0 || wanted == 0) {
        return 2;
    }
    public_index = 0UL;
    for (i = 0UL; i < cache->count; ++i) {
        if (cache->entries[i].is_self) {
            continue;
        }
        ++public_index;
        if (cache->entries[i].peer_constructor == wanted->peer_constructor &&
            cache->entries[i].id_hi == wanted->id_hi &&
            cache->entries[i].id_lo == wanted->id_lo) {
            if (public_index_out != 0) {
                *public_index_out = public_index;
            }
            return 0;
        }
    }
    return 2;
}

static int tg_mtproto_peer_cache_find_username_public_index(
    const char *path,
    const char *username_text,
    char *index_text,
    unsigned long index_text_size,
    char *label,
    unsigned long label_size)
{
    tg_mtproto_peer_cache cache;
    char username[128];
    unsigned long i;
    unsigned long public_index;

    if (index_text != 0 && index_text_size > 0UL) {
        index_text[0] = '\0';
    }
    if (label != 0 && label_size > 0UL) {
        label[0] = '\0';
    }
    if (path == 0 || username_text == 0 || index_text == 0 ||
        index_text_size == 0UL) {
        return 2;
    }
    tg_mtproto_normalize_username(username_text, username, sizeof(username));
    if (username[0] == '\0' ||
        tg_mtproto_load_peer_cache_file(path, &cache) != 0) {
        return 2;
    }
    public_index = 0UL;
    for (i = 0UL; i < cache.count; ++i) {
        if (cache.entries[i].is_self) {
            continue;
        }
        ++public_index;
        if (cache.entries[i].username[0] == '\0' ||
            strlen(cache.entries[i].username) != strlen(username) ||
            !tg_mtproto_ascii_contains_ci(cache.entries[i].username,
                                          username)) {
            continue;
        }
        sprintf(index_text, "%lu", public_index);
        tg_mtproto_peer_cache_entry_label(&cache.entries[i], label,
                                          label_size);
        return 0;
    }
    return 2;
}

static int tg_mtproto_peer_cache_text_looks_username(const char *text)
{
    unsigned long i;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (i = 0UL; text[i] != '\0'; ++i) {
        if (!((text[i] >= 'A' && text[i] <= 'Z') ||
              (text[i] >= 'a' && text[i] <= 'z') ||
              (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

static int tg_mtproto_chat_arg_is_exact_username(const char *text)
{
    if (text == 0) {
        return 0;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    return text[0] == '@' || strncmp(text, "t.me/", 5) == 0 ||
           strncmp(text, "https://t.me/", 13) == 0 ||
           strncmp(text, "http://t.me/", 12) == 0;
}

static int tg_mtproto_peer_cache_add_selected(
    const char *path,
    const tg_mtproto_peer_cache_entry *selected,
    char *index_text,
    unsigned long index_text_size,
    char *label,
    unsigned long label_size,
    FILE *stream)
{
    static tg_mtproto_peer_cache cache;
    static tg_mtproto_peer_cache fresh;
    unsigned long public_index;
    int has_cache;

    if (index_text != 0 && index_text_size > 0UL) {
        index_text[0] = '\0';
    }
    if (label != 0 && label_size > 0UL) {
        label[0] = '\0';
    }
    if (path == 0 || selected == 0 ||
        !tg_mtproto_peer_cache_entry_is_openable(selected)) {
        return 2;
    }
    has_cache = tg_mtproto_load_peer_cache_file(path, &cache) == 0;
    if (!has_cache) {
        memset(&cache, 0, sizeof(cache));
    }
    memset(&fresh, 0, sizeof(fresh));
    fresh.entries[0] = *selected;
    fresh.entries[0].from_dialog = 1;
    fresh.count = 1UL;
    fresh.total_dialog_count = 1UL;
    tg_mtproto_recount_peer_cache(&fresh);
    tg_mtproto_merge_peer_cache(&cache, &fresh);
    if (tg_mtproto_peer_cache_find_public_index(&cache, selected,
                                                &public_index) != 0) {
        return 2;
    }
    if (tg_mtproto_save_peer_cache_file(path, &cache, stream,
                                        "chat cache") != 0) {
        return 2;
    }
    if (index_text != 0 && index_text_size > 0UL) {
        sprintf(index_text, "%lu", public_index);
    }
    tg_mtproto_peer_cache_entry_label(selected, label, label_size);
    return 0;
}

static int tg_mtproto_chat_prompt_line(const char *prompt,
                                       char *out,
                                       unsigned long out_size,
                                       int required,
                                       FILE *stream,
                                       const char *label);

static int tg_mtproto_auth_search_global_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    const char *query_text,
    char *selected_peer_index,
    unsigned long selected_peer_index_size,
    char *selected_peer_label,
    unsigned long selected_peer_label_size,
    FILE *stream)
{
    unsigned char query[256];
    char trimmed_query[128];
    char choice[32];
    unsigned long public_index;
    unsigned long chosen_index;
    unsigned long i;
    const tg_mtproto_peer_cache_entry *selected;
    FILE *quiet;
    static tg_mtproto_peer_cache search_cache;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto contacts.search";

    if (selected_peer_index != 0 && selected_peer_index_size > 0UL) {
        selected_peer_index[0] = '\0';
    }
    if (selected_peer_label != 0 && selected_peer_label_size > 0UL) {
        selected_peer_label[0] = '\0';
    }
    if (stream == 0 || query_text == 0 || peer_cache_file == 0) {
        return 2;
    }
    tg_mtproto_copy_plain_cache_text(trimmed_query, sizeof(trimmed_query),
                                     query_text);
    tg_mtproto_trim_line(trimmed_query);
    if (trimmed_query[0] == '\0') {
        fprintf(stream, "Type a name, username, or group to add.\n");
        return 2;
    }
#if TG_MTPROTO_DISPLAY_LATIN1
    /* Latin-1 keymap -> UTF-8 for the wire (same as message send); without it
       a query with accented letters is rejected by the server. */
    {
        static char search_utf8[256];

        if (tg_mtproto_latin1_to_utf8(trimmed_query, search_utf8,
                                      sizeof(search_utf8))) {
            tg_mtproto_copy_plain_cache_text(trimmed_query,
                                             sizeof(trimmed_query),
                                             search_utf8);
        }
    }
#endif

    quiet = tg_mtproto_open_quiet_stream(stream);
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_contacts_search(&writer, trimmed_query, 10UL) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        fprintf(stream, "Search text is too long.\n");
        return 2;
    }
    /* Use the same generous receive budget as send/history (200 attempts,
       still capped by TG_MTPROTO_QUERY_BUDGET_SECONDS). On a busy account the
       contacts.found reply arrives behind a backlog of update messages on the
       persistent chat connection; a small cap (was 8) exhausted before the
       reply was seen and surfaced as "Could not search Telegram now.", which
       in turn forced the exact-@username resolveUsername fallback. */
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, quiet, label, 200U) != 0) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        fprintf(stream, "Could not search Telegram now.\n");
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        (void)tg_mtproto_print_rpc_error(label, &result, quiet);
        tg_mtproto_close_quiet_stream(quiet, stream);
        fprintf(stream, "No Telegram results for that search.\n");
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, quiet, label) != 0 ||
        tg_mtproto_parse_contacts_search_peer_cache(
            result.result_constructor, result.result_body,
            result.result_body_length, &search_cache) !=
            TG_MTPROTO_TL_OK) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        fprintf(stream, "Could not read Telegram search results.\n");
        return 2;
    }
    tg_mtproto_close_quiet_stream(quiet, stream);

    public_index = 0UL;
    for (i = 0UL; i < search_cache.count; ++i) {
        if (search_cache.entries[i].is_self ||
            !tg_mtproto_peer_cache_entry_is_openable(
                &search_cache.entries[i])) {
            continue;
        }
        ++public_index;
        if (public_index == 1UL) {
            fprintf(stream, "Search results:\n");
        }
        tg_mtproto_print_search_result_line(stream, public_index,
                                            &search_cache.entries[i]);
    }
    if (public_index == 0UL) {
        fprintf(stream,
                "No openable Telegram results. Try @username or a t.me link.\n");
        return 2;
    }
    if (tg_mtproto_chat_prompt_line(
            "Choose result number (empty to cancel): ", choice,
            sizeof(choice), 0, stream, label) != 0) {
        return 2;
    }
    if (choice[0] == '\0') {
        fprintf(stream, "Search cancelled.\n");
        return 1;
    }
    if (tg_mtproto_parse_ulong_arg(choice, &chosen_index) != 0 ||
        chosen_index == 0UL || chosen_index > public_index) {
        fprintf(stream, "Result number not found.\n");
        return 2;
    }
    selected = 0;
    public_index = 0UL;
    for (i = 0UL; i < search_cache.count; ++i) {
        if (search_cache.entries[i].is_self ||
            !tg_mtproto_peer_cache_entry_is_openable(
                &search_cache.entries[i])) {
            continue;
        }
        ++public_index;
        if (public_index == chosen_index) {
            selected = &search_cache.entries[i];
            break;
        }
    }
    if (selected == 0 ||
        tg_mtproto_peer_cache_add_selected(
            peer_cache_file, selected, selected_peer_index,
            selected_peer_index_size, selected_peer_label,
            selected_peer_label_size, stream) != 0) {
        fprintf(stream, "Could not add that chat.\n");
        return 2;
    }
    fprintf(stream, "Chat added: ");
    if (selected_peer_label != 0 && selected_peer_label[0] != '\0') {
        tg_mtproto_print_cache_text(stream, selected_peer_label);
    } else {
        fprintf(stream, "chat");
    }
    fprintf(stream, ".\n");
    return 0;
}

int tg_mtproto_auth_get_history_peer_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *peer_cache_file,
                                          const char *peer_index_text,
                                          const char *limit_text,
                                          FILE *stream)
{
    unsigned char query[64];
    unsigned long limit;
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    char api_id[32];
    tg_mtproto_messages_summary messages;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto messages.getHistory(peer)";

    if (stream == 0 || tg_mtproto_parse_ulong_arg(limit_text, &limit) != 0 ||
        limit == 0UL || limit > 100UL) {
        if (stream != 0) {
            fputs("mtproto messages.getHistory(peer): invalid-arguments\n",
                  stream);
        }
        return 2;
    }
    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0 ||
        tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_peer(
            &writer, peer_constructor, peer_id_hi, peer_id_lo,
            access_hash_hi, access_hash_lo, has_access_hash, 0UL, limit) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_limited(host, port, api_id, auth_file,
                                            dc_id_text, query, writer.length,
                                            &result, stream, label, 2U,
                                            0) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_messages_summary(result.result_constructor,
                                          result.result_body,
                                          result.result_body_length,
                                          &messages) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: messages-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            messages.constructor);
    fprintf(stream, "%s: messages %lu chats %lu users %lu\n", label,
            messages.message_count, messages.chat_count, messages.user_count);    if (messages.is_slice || messages.is_not_modified ||
        messages.is_channel_messages) {
        fprintf(stream, "%s: count %lu\n", label, messages.count);
    }
    return 0;
}

int tg_mtproto_auth_send_self(const char *host,
                              const char *port,
                              const char *api_id_text,
                              const char *auth_file,
                              const char *dc_id_text,
                              const char *message,
                              FILE *stream)
{
    unsigned char query[512];
    unsigned char random_id[8];
    unsigned long random_id_hi;
    unsigned long random_id_lo;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_updates_summary updates;
    static const char label[] = "mtproto messages.sendMessage(self)";

    if (stream == 0 || message == 0 || message[0] == '\0') {
        if (stream != 0) {
            fputs("mtproto messages.sendMessage(self): invalid-arguments\n",
                  stream);
        }
        return 2;
    }
    tg_mtproto_saved_session_random(random_id, sizeof(random_id));
    random_id_lo = tg_mtproto_read_u32_le(random_id);
    random_id_hi = tg_mtproto_read_u32_le(random_id + 4U);

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_self(&writer, message, 0UL, random_id_hi,
                                            random_id_lo) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query(host, port, api_id_text, auth_file,
                                    dc_id_text, query, writer.length, &result,
                                    stream, label) != 0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_updates_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &updates) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: updates-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            updates.constructor);
    if (updates.has_sent_message) {
        fprintf(stream, "%s: message_id %lu date %lu\n", label, updates.id,
                updates.date);
    }
    return 0;
}

int tg_mtproto_auth_send_peer_file(const char *host,
                                   const char *port,
                                   const char *api_file,
                                   const char *auth_file,
                                   const char *dc_id_text,
                                   const char *peer_cache_file,
                                   const char *peer_index_text,
                                   const char *message,
                                   FILE *stream)
{
    /* The whole sendMessage query, text included, has to fit here. It was
       512 bytes, which quietly capped a message at roughly 460 characters:
       anything longer failed to build and the client refused to send it,
       which is what a pasted text file runs into (issue #14). Size it from
       the composer instead, doubled for the Latin-1 to UTF-8 growth, plus
       room for the envelope. Static, not on the stack: the 68000 profile
       runs on a fraction of the usual stack. */
    static unsigned char query[(TG_GUI_MSG_TEXT_MAX * 2) + 128];
    unsigned char random_id[8];
    unsigned long random_id_hi;
    unsigned long random_id_lo;
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    char api_id[32];
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_updates_summary updates;
    static const char label[] = "mtproto messages.sendMessage(peer)";

    if (stream == 0 || message == 0 || message[0] == '\0') {
        if (stream != 0) {
            fputs("mtproto messages.sendMessage(peer): invalid-arguments\n",
                  stream);
        }
        return 2;
    }
    if (tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                    stream, label) != 0 ||
        tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    /* Diagnostic (surfaced via the quiet-stream replay on failure): which peer
       kind the cache resolved and whether it carries an access_hash. A channel
       (supergroup) with access_hash=NO cannot be addressed -- the immediate
       send failure we are chasing on the stock A1200. */
    fprintf(stream, "%s: peer=%s id=0x%08lx%08lx access_hash=%s\n", label,
            peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR ? "channel" :
            peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ? "chat" :
            peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR ? "user" : "?",
            peer_id_hi, peer_id_lo, has_access_hash ? "yes" : "NO");
    tg_mtproto_saved_session_random(random_id, sizeof(random_id));
    random_id_lo = tg_mtproto_read_u32_le(random_id);
    random_id_hi = tg_mtproto_read_u32_le(random_id + 4U);

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_peer(
            &writer, peer_constructor, peer_id_hi, peer_id_lo,
            access_hash_hi, access_hash_lo, has_access_hash, message,
            0UL, random_id_hi, random_id_lo) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    if (tg_mtproto_send_saved_query_limited(host, port, api_id, auth_file,
                                            dc_id_text, query, writer.length,
                                            &result, stream, label, 6U, 1) !=
        0) {
        return 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_updates_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &updates) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: updates-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    fprintf(stream, "%s: ok\n", label);
    fprintf(stream, "%s: constructor 0x%08lx\n", label,
            updates.constructor);
    if (updates.has_sent_message) {
        fprintf(stream, "%s: message_id %lu date %lu\n", label, updates.id,
                updates.date);
    }
    return 0;
}

static int tg_mtproto_auth_send_peer_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    const char *peer_index_text,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long *sent_message_id,
    FILE *stream)
{
    /* The whole sendMessage query, text included, has to fit here. It was
       512 bytes, which quietly capped a message at roughly 460 characters:
       anything longer failed to build and the client refused to send it,
       which is what a pasted text file runs into (issue #14). Size it from
       the composer instead, doubled for the Latin-1 to UTF-8 growth, plus
       room for the envelope. Static, not on the stack: the 68000 profile
       runs on a fraction of the usual stack. */
    static unsigned char query[(TG_GUI_MSG_TEXT_MAX * 2) + 128];
    unsigned char random_id[8];
    unsigned long random_id_hi;
    unsigned long random_id_lo;
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_updates_summary updates;
    int qrc;
    static const char label[] = "mtproto messages.sendMessage(peer)";

    if (sent_message_id != 0) {
        *sent_message_id = 0UL;
    }
    if (stream == 0 || message == 0 || message[0] == '\0') {
        if (stream != 0) {
            fputs("mtproto messages.sendMessage(peer): invalid-arguments\n",
                  stream);
        }
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    /* Diagnostic (surfaced via the quiet-stream replay on failure): which peer
       kind the cache resolved and whether it carries an access_hash. A channel
       (supergroup) with access_hash=NO cannot be addressed -- the immediate
       send failure we are chasing on the stock A1200. */
    fprintf(stream, "%s: peer=%s id=0x%08lx%08lx access_hash=%s\n", label,
            peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR ? "channel" :
            peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ? "chat" :
            peer_constructor == TG_MTPROTO_PEER_USER_CONSTRUCTOR ? "user" : "?",
            peer_id_hi, peer_id_lo, has_access_hash ? "yes" : "NO");
    tg_mtproto_saved_session_random(random_id, sizeof(random_id));
    random_id_lo = tg_mtproto_read_u32_le(random_id);
    random_id_hi = tg_mtproto_read_u32_le(random_id + 4U);

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_peer(
            &writer, peer_constructor, peer_id_hi, peer_id_lo,
            access_hash_hi, access_hash_lo, has_access_hash, message,
            reply_to_msg_id, random_id_hi, random_id_lo) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    memset(&result, 0, sizeof(result));
    qrc = tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 600U);
    if (qrc != 0) {
        return qrc == TG_MTPROTO_QUERY_SOFT_FAIL ?
            TG_MTPROTO_QUERY_SOFT_FAIL : 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    if (tg_mtproto_parse_updates_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &updates) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: updates-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    if (sent_message_id != 0 && updates.has_sent_message) {
        *sent_message_id = updates.id;
    }
    return 0;
}

/* Send a pre-built query on the open context, surfacing only success/failure
   (edit/delete do not need the reply body). 0 ok, SOFT_FAIL on timeout, 2 else. */
static int tg_mtproto_auth_simple_query_on_context(
    const char *host, const char *port, const char *api_id,
    const char *auth_file, const char *dc_id_text,
    tg_mtproto_auth_context *context, const unsigned char *query,
    unsigned long query_len, FILE *stream, const char *label)
{
    tg_mtproto_rpc_result result;
    int qrc;

    memset(&result, 0, sizeof(result));
    qrc = tg_mtproto_send_saved_query_on_context(host, port, api_id, auth_file,
                                                 dc_id_text, context, query,
                                                 query_len, &result, stream,
                                                 label, 600U);
    if (qrc != 0) {
        return qrc == TG_MTPROTO_QUERY_SOFT_FAIL ? TG_MTPROTO_QUERY_SOFT_FAIL : 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 2;
    }
    return 0;
}

/* Forward one message between two cached peers on the open chat connection.
   "self" is accepted at either endpoint and serializes as inputPeerSelf. */
static int tg_mtproto_auth_forward_peer_on_context(
    const char *host, const char *port, const char *api_id,
    const char *auth_file, const char *dc_id_text,
    tg_mtproto_auth_context *context, const char *peer_cache_file,
    const char *from_peer_index_text, const char *to_peer_index_text,
    unsigned long message_id, FILE *stream)
{
    unsigned char query[256];
    unsigned char random_id[8];
    unsigned long from_constructor;
    unsigned long from_id_hi;
    unsigned long from_id_lo;
    unsigned long from_hash_hi;
    unsigned long from_hash_lo;
    unsigned long to_constructor;
    unsigned long to_id_hi;
    unsigned long to_id_lo;
    unsigned long to_hash_hi;
    unsigned long to_hash_lo;
    unsigned long random_id_hi;
    unsigned long random_id_lo;
    int from_has_hash;
    int to_has_hash;
    int qrc;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    tg_mtproto_updates_summary updates;
    static const char label[] = "mtproto messages.forwardMessages";

    if (stream == 0 || message_id == 0UL || from_peer_index_text == 0 ||
        to_peer_index_text == 0) {
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(
            peer_cache_file, from_peer_index_text, &from_constructor,
            &from_id_hi, &from_id_lo, &from_hash_hi, &from_hash_lo,
            &from_has_hash, stream, label) != 0 ||
        tg_mtproto_load_peer_cache_peer(
            peer_cache_file, to_peer_index_text, &to_constructor, &to_id_hi,
            &to_id_lo, &to_hash_hi, &to_hash_lo, &to_has_hash, stream,
            label) != 0) {
        return 2;
    }
    tg_mtproto_saved_session_random(random_id, sizeof(random_id));
    random_id_lo = tg_mtproto_read_u32_le(random_id);
    random_id_hi = tg_mtproto_read_u32_le(random_id + 4U);
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_forward_message(
            &writer, from_constructor, from_id_hi, from_id_lo, from_hash_hi,
            from_hash_lo, from_has_hash, message_id, random_id_hi,
            random_id_lo, to_constructor, to_id_hi, to_id_lo, to_hash_hi,
            to_hash_lo, to_has_hash) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    memset(&result, 0, sizeof(result));
    qrc = tg_mtproto_send_saved_query_on_context(
        host, port, api_id, auth_file, dc_id_text, context, query,
        writer.length, &result, stream, label, 600U);
    if (qrc != 0) {
        return qrc == TG_MTPROTO_QUERY_SOFT_FAIL ?
            TG_MTPROTO_QUERY_SOFT_FAIL : 2;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        if (!tg_mtproto_print_rpc_error(label, &result, stream)) {
            fprintf(stream, "%s: rpc-error-parse-failed\n", label);
        }
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0 ||
        tg_mtproto_parse_updates_summary(result.result_constructor,
                                         result.result_body,
                                         result.result_body_length,
                                         &updates) != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: updates-parse-failed constructor 0x%08lx\n",
                label, result.result_constructor);
        return 2;
    }
    return 0;
}

/* messages.editMessage on the open context: replace an OWN message's text. */
static int tg_mtproto_auth_edit_peer_on_context(
    const char *host, const char *port, const char *api_id,
    const char *auth_file, const char *dc_id_text,
    tg_mtproto_auth_context *context, const char *peer_cache_file,
    const char *peer_index_text, unsigned long message_id, const char *message,
    FILE *stream)
{
    unsigned char query[512];
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto messages.editMessage";

    if (stream == 0 || message == 0 || message[0] == '\0' || message_id == 0UL) {
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_edit_message(
            &writer, peer_constructor, peer_id_hi, peer_id_lo, access_hash_hi,
            access_hash_lo, has_access_hash, message_id, message) !=
        TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    return tg_mtproto_auth_simple_query_on_context(host, port, api_id, auth_file,
                                                   dc_id_text, context, query,
                                                   writer.length, stream, label);
}

/* deleteMessages on the open context: messages.* for users/basic chats,
   channels.* for channels/supergroups. revoke = delete for everyone. */
static int tg_mtproto_auth_delete_peer_on_context(
    const char *host, const char *port, const char *api_id,
    const char *auth_file, const char *dc_id_text,
    tg_mtproto_auth_context *context, const char *peer_cache_file,
    const char *peer_index_text, unsigned long message_id, int revoke,
    FILE *stream)
{
    unsigned char query[256];
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    tg_mtproto_tl_writer writer;
    tg_mtproto_tl_status bst;
    static const char label[] = "mtproto deleteMessages";

    if (stream == 0 || message_id == 0UL) {
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR) {
        if (!has_access_hash) {
            fprintf(stream, "%s: channel without access_hash\n", label);
            return 2;
        }
        bst = tg_mtproto_build_channels_delete_messages(
            &writer, peer_id_hi, peer_id_lo, access_hash_hi, access_hash_lo,
            message_id);
    } else {
        bst = tg_mtproto_build_messages_delete_messages(&writer, revoke,
                                                        message_id);
    }
    if (bst != TG_MTPROTO_TL_OK) {
        fprintf(stream, "%s: query-build-failed\n", label);
        return 2;
    }
    return tg_mtproto_auth_simple_query_on_context(host, port, api_id, auth_file,
                                                   dc_id_text, context, query,
                                                   writer.length, stream, label);
}

/* Refresh the open chat's read_outbox_max_id (how far the peer has read OUR
   messages -- the read-receipt cursor) with a one-peer messages.getPeerDialogs.
   Sets *out to the cursor (0 if the chat is absent / unread); returns 0 on
   success, non-zero. Re-enabled on MorphOS 2026-06-20: the "window opens then
   freezes on the first tick" symptom this was disabled for was the unserialized
   first-tick repaint racing the layer build, now cured by the LockLayerRom
   bracket in tg_gui_window.c -- not the (tiny one-peer) getPeerDialogs reply. */
static int tg_mtproto_chat_fetch_read_outbox_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    const char *peer_index_text,
    unsigned long *out_read_outbox_max,
    FILE *stream)
{
    unsigned char query[64];
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static tg_mtproto_dialog_peer_list dialogs;
    unsigned long i;
    static const char label[] = "mtproto messages.getPeerDialogs";

    if (out_read_outbox_max != 0) {
        *out_read_outbox_max = 0UL;
    }
    if (stream == 0 || out_read_outbox_max == 0) {
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_peer_dialogs(
            &writer, peer_constructor, peer_id_hi, peer_id_lo, access_hash_hi,
            access_hash_lo, has_access_hash) != TG_MTPROTO_TL_OK) {
        return 2;
    }
    memset(&result, 0, sizeof(result));
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 4U) != 0) {
        return 1;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        (void)tg_mtproto_print_rpc_error(label, &result, stream);
        return 1;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 1;
    }
    /* peerDialogs leads with the dialogs vector; the list parser accepts it. */
    if (tg_mtproto_parse_dialog_peer_list(result.result_constructor,
                                          result.result_body,
                                          result.result_body_length,
                                          &dialogs) != TG_MTPROTO_TL_OK) {
        return 1;
    }
    for (i = 0UL; i < dialogs.count; ++i) {
        if (dialogs.peers[i].peer_constructor == peer_constructor &&
            dialogs.peers[i].id_hi == peer_id_hi &&
            dialogs.peers[i].id_lo == peer_id_lo) {
            *out_read_outbox_max = dialogs.peers[i].read_outbox_max_id;
            break;
        }
    }
    return 0;
}

/* Fetch a supergroup's recent members and parse their id->name into out_members,
   for the typing indicator. v1: peerChannel only (basic groups have no access_hash
   and would need messages.getFullChat). MorphOS SKIPS entirely -- a large
   participants reply is the documented bsdsocket freeze trigger. Returns 0 on a
   parsed reply, non-zero otherwise (caller falls back to "someone"). */
static int tg_mtproto_gui_fetch_group_members(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    const char *peer_index_text,
    tg_mtproto_peer_cache *out_members,
    FILE *stream)
{
#if defined(__MORPHOS__) || defined(__MORPHOS)
    (void)host; (void)port; (void)api_id; (void)auth_file; (void)dc_id_text;
    (void)context; (void)peer_cache_file; (void)peer_index_text; (void)stream;
    if (out_members != 0) {
        memset(out_members, 0, sizeof(*out_members));
    }
    return 2; /* never fetch on MorphOS (freeze guard) */
#else
    unsigned char query[64];
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto channels.getParticipants";
#if defined(__m68k__)
    const unsigned long member_limit = 24UL; /* tighter box */
#else
    const unsigned long member_limit = 64UL; /* == peer-cache capacity */
#endif

    if (out_members != 0) {
        memset(out_members, 0, sizeof(*out_members));
    }
    if (stream == 0 || out_members == 0) {
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        stream, label) != 0) {
        return 2;
    }
    /* Supergroups/channels only: basic groups (peerChat) carry no access_hash and
       need messages.getFullChat (deferred). */
    if (peer_constructor != TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR ||
        !has_access_hash) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_channels_get_participants_recent(
            &writer, peer_id_hi, peer_id_lo, access_hash_hi, access_hash_lo,
            member_limit) != TG_MTPROTO_TL_OK) {
        return 2;
    }
    memset(&result, 0, sizeof(result));
    if (tg_mtproto_send_saved_query_on_context(
            host, port, api_id, auth_file, dc_id_text, context, query,
            writer.length, &result, stream, label, 4U) != 0) {
        return 1;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        (void)tg_mtproto_print_rpc_error(label, &result, stream);
        return 1;
    }
    if (tg_mtproto_unpack_gzip_result(&result, stream, label) != 0) {
        return 1;
    }
    /* channels.channelParticipants carries users:Vector<User>; the generic scanner
       extracts each user's id + name (title) into out_members. */
    tg_mtproto_parse_message_peers(result.result_body, result.result_body_length,
                                   out_members);
    return 0;
#endif
}

/* On the Amiga lanes tmpfile() is NOT anonymous: clib2/newlib create a real
   "tmpNNNNNN" file in the CURRENT DIRECTORY and AmigaDOS cannot delete an
   open file, so every quiet query left a dropping in the program drawer
   (field report 2026-08-04: a drawer flooded with tmp* files). Quiet streams
   now live in T: (the standard RAM-backed temp assign, same home as the
   search cache), one slot per nesting level, swept at first use and when the
   context closes. The host keeps the POSIX tmpfile(), which really is
   anonymous. */
#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__MORPHOS) || defined(__AROS__)
#define TG_MTPROTO_QUIET_TMP_SLOTS 8
static int tg_mtproto_quiet_depth;

static void tg_mtproto_quiet_tmp_name(char *out, int slot)
{
    sprintf(out, "T:tg-quiet-%d.tmp", slot);
}

static void tg_mtproto_quiet_tmp_sweep(void)
{
    char name[32];
    int i;

    for (i = 0; i < TG_MTPROTO_QUIET_TMP_SLOTS; ++i) {
        tg_mtproto_quiet_tmp_name(name, i);
        (void)remove(name);
    }
}

static FILE *tg_mtproto_open_quiet_stream(FILE *fallback)
{
    static int swept;
    char name[32];
    FILE *quiet;

    if (!swept) {
        swept = 1;
        tg_mtproto_quiet_tmp_sweep();
    }
    if (tg_mtproto_quiet_depth >= TG_MTPROTO_QUIET_TMP_SLOTS) {
        return fallback;
    }
    tg_mtproto_quiet_tmp_name(name, tg_mtproto_quiet_depth);
    quiet = fopen(name, "w+b"); /* w+ truncates any stale slot content */
    if (quiet == 0) {
        return fallback;
    }
    ++tg_mtproto_quiet_depth;
    return quiet;
}

static void tg_mtproto_close_quiet_stream(FILE *quiet, FILE *fallback)
{
    char name[32];

    if (quiet != 0 && quiet != fallback) {
        fclose(quiet);
        if (tg_mtproto_quiet_depth > 0) {
            --tg_mtproto_quiet_depth;
            tg_mtproto_quiet_tmp_name(name, tg_mtproto_quiet_depth);
            (void)remove(name);
        }
    }
}
#else
static FILE *tg_mtproto_open_quiet_stream(FILE *fallback)
{
    FILE *quiet;

    quiet = tmpfile();
    if (quiet == 0) {
        return fallback;
    }
    return quiet;
}

static void tg_mtproto_close_quiet_stream(FILE *quiet, FILE *fallback)
{
    if (quiet != 0 && quiet != fallback) {
        fclose(quiet);
    }
}

static void tg_mtproto_quiet_tmp_sweep(void)
{
}
#endif

static void tg_mtproto_replay_quiet_stream(FILE *quiet, FILE *fallback)
{
    char line[256];

    if (quiet == 0 || fallback == 0 || quiet == fallback) {
        return;
    }
    rewind(quiet);
    while (fgets(line, sizeof(line), quiet) != 0) {
        fputs(line, fallback);
    }
}

/* Pull the most useful error line out of the quiet stream into `out`, so the GUI
   (which has no console -- stdout is NIL: on a Workbench launch) can show WHY a
   login step failed instead of a generic message. Prefer a line that names an
   error; otherwise keep the last substantive line (the DH progress is just dots). */
static void tg_mtproto_capture_quiet_error(FILE *quiet, FILE *fallback,
                                           char *out, unsigned long out_size)
{
    char line[256];
    char last[256];

    if (out == 0 || out_size == 0UL) {
        return;
    }
    out[0] = '\0';
    last[0] = '\0';
    if (quiet == 0 || quiet == fallback) {
        return;
    }
    rewind(quiet);
    while (fgets(line, sizeof(line), quiet) != 0) {
        unsigned long n;
        unsigned long i;
        int substantive;

        n = (unsigned long)strlen(line);
        while (n > 0UL && (line[n - 1UL] == '\n' || line[n - 1UL] == '\r')) {
            line[--n] = '\0';
        }
        substantive = 0;
        for (i = 0UL; i < n; ++i) {
            if (line[i] != '.' && line[i] != ' ' && line[i] != '\t') {
                substantive = 1;
                break;
            }
        }
        if (!substantive) {
            continue;
        }
        if (strstr(line, "rror") != 0 || strstr(line, "ail") != 0 ||
            strstr(line, "nvalid") != 0 || strstr(line, "FLOOD") != 0 ||
            strstr(line, "MIGRATE") != 0 || strstr(line, "BANNED") != 0 ||
            strstr(line, "imeout") != 0 || strstr(line, "onnect") != 0) {
            unsigned long copy_len;

            copy_len = n;
            if (copy_len >= out_size) {
                copy_len = out_size - 1UL;
            }
            memcpy(out, line, (size_t)copy_len);
            out[copy_len] = '\0';
        }
        {
            unsigned long copy_len;

            copy_len = n;
            if (copy_len >= (unsigned long)sizeof(last)) {
                copy_len = (unsigned long)sizeof(last) - 1UL;
            }
            memcpy(last, line, (size_t)copy_len);
            last[copy_len] = '\0';
        }
    }
    if (out[0] == '\0' && last[0] != '\0') {
        unsigned long copy_len;

        copy_len = (unsigned long)strlen(last);
        if (copy_len >= out_size) {
            copy_len = out_size - 1UL;
        }
        memcpy(out, last, (size_t)copy_len);
        out[copy_len] = '\0';
    }
}

static void tg_mtproto_reset_quiet_stream(FILE *quiet, FILE *fallback)
{
    if (quiet != 0 && quiet != fallback) {
        rewind(quiet);
    }
}

static long tg_mtproto_quiet_stream_length(FILE *quiet, FILE *fallback)
{
    if (quiet == 0 || quiet == fallback) {
        return 0L;
    }
    return ftell(quiet);
}

static void tg_mtproto_replay_quiet_stream_length(FILE *quiet,
                                                  FILE *fallback,
                                                  long length)
{
    char buffer[256];
    char line[512];
    unsigned long line_length;
    long remaining;
    unsigned long chunk;
    unsigned long i;

    if (quiet == 0 || fallback == 0 || quiet == fallback || length <= 0L) {
        return;
    }
    rewind(quiet);
    remaining = length;
    if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
        /* Full-screen mode: feed the captured bytes line by line into the
           transcript region instead of streaming them at the cursor. */
        line_length = 0UL;
        while (remaining > 0L) {
            chunk = remaining > (long)sizeof(buffer) ?
                (unsigned long)sizeof(buffer) : (unsigned long)remaining;
            chunk = (unsigned long)fread(buffer, 1U, (size_t)chunk, quiet);
            if (chunk == 0UL) {
                break;
            }
            for (i = 0UL; i < chunk; ++i) {
                if (buffer[i] == '\n') {
                    line[line_length] = '\0';
                    tg_console_tui_line(tg_chat_tui_stream, line);
                    line_length = 0UL;
                    continue;
                }
                line_length = tg_console_tui_line_push(
                    tg_chat_tui_stream, line, sizeof(line), line_length,
                    buffer[i]);
            }
            remaining -= (long)chunk;
        }
        if (line_length > 0UL) {
            line[line_length] = '\0';
            tg_console_tui_line(tg_chat_tui_stream, line);
        }
        return;
    }
    while (remaining > 0L) {
        chunk = remaining > (long)sizeof(buffer) ?
            (unsigned long)sizeof(buffer) : (unsigned long)remaining;
        chunk = (unsigned long)fread(buffer, 1U, (size_t)chunk, quiet);
        if (chunk == 0UL) {
            break;
        }
        (void)fwrite(buffer, 1U, (size_t)chunk, fallback);
        remaining -= (long)chunk;
    }
}

/* Command/message history for the interactive chat (Up/Down recall). */
#define TG_CHAT_HISTORY_MAX 16U
#define TG_CHAT_HISTORY_LEN 512U
static char tg_chat_history[TG_CHAT_HISTORY_MAX][TG_CHAT_HISTORY_LEN];
static unsigned long tg_chat_history_count = 0UL;
static long tg_chat_history_recall = -1L;

/*
 * Whether the interactive chat currently has stdin in raw console mode. Set at
 * the top of the chat loop once tg_platform_stdin_set_raw(1) succeeds, and read
 * by tg_mtproto_chat_prompt_line so the sub-prompts (Peer index, Search, Remove,
 * /add name, search result number) echo and line-edit the same way as the main
 * input loop. Without this, raw mode left those cooked prompts silent -- typing
 * produced nothing on screen, which looked like a dead keyboard. Every caller of
 * tg_mtproto_chat_prompt_line runs inside the chat loop, so setting this once per
 * chat entry is sufficient.
 */
static int tg_chat_input_raw = 0;
/* TUI message-input caret: byte offset into the line buffer (0..line_length).
   File-static like the history because the line editor is called once per key and
   the caret must persist across calls. Used only on the full-screen TUI. */
static unsigned long tg_chat_caret = 0UL;

static void tg_chat_history_reset(void)
{
    tg_chat_history_count = 0UL;
    tg_chat_history_recall = -1L;
}

static void tg_chat_history_add(const char *text)
{
    unsigned long i;

    if (text == 0 || text[0] == '\0') {
        tg_chat_history_recall = -1L;
        return;
    }
    if (tg_chat_history_count > 0UL &&
        strcmp(tg_chat_history[tg_chat_history_count - 1UL], text) == 0) {
        tg_chat_history_recall = -1L;
        return;
    }
    if (tg_chat_history_count >= TG_CHAT_HISTORY_MAX) {
        for (i = 1UL; i < TG_CHAT_HISTORY_MAX; ++i) {
            strcpy(tg_chat_history[i - 1UL], tg_chat_history[i]);
        }
        tg_chat_history_count = TG_CHAT_HISTORY_MAX - 1UL;
    }
    strncpy(tg_chat_history[tg_chat_history_count], text,
            TG_CHAT_HISTORY_LEN - 1UL);
    tg_chat_history[tg_chat_history_count][TG_CHAT_HISTORY_LEN - 1UL] = '\0';
    ++tg_chat_history_count;
    tg_chat_history_recall = -1L;
}

/* One-line command cheat-sheet shown when a lone '/' starts the input row
   (both the TUI-transcript and the linear-raw printers use it). */
#define TG_CHAT_SLASH_HINT_TEXT \
    "Commands: /peers /saved /forward /forwardto /getfile /sendfile /photo\n" \
    "          /search /add /remove /history /swap /watch /diff /color /bell\n" \
    "          /help /quit\n"

/*
 * Chat input reader. In raw mode it echoes characters itself and supports
 * backspace plus, when use_history is set, Up/Down command-history recall
 * (cursor keys arrive as ESC '[' 'A'/'B' or the single-byte CSI 0x9B 'A'/'B').
 * Unrecognised CSI sequences (window close/resize events) are always consumed so
 * they never leak into the typed line. When use_history is 0 the reader still
 * echoes and line-edits but does not record or recall history -- used by the
 * sub-prompts (Peer index, /add name, ...) so answering them does not pollute
 * the message history and Up/Down there is simply swallowed. In cooked fallback
 * mode (raw == 0) it just accumulates the line as before, without echo.
 */
static int tg_mtproto_chat_read_line_edit(char *line,
                                          unsigned long line_size,
                                          unsigned long *line_length,
                                          unsigned long timeout_seconds,
                                          int raw,
                                          int use_history,
                                          FILE *stream)
{
    char ch;
    int rc;
    unsigned long i;
    long idx;
    int direction;
    unsigned long fkey;

    if (line == 0 || line_size == 0UL || line_length == 0 || stream == 0) {
        return -1;
    }
    if (tg_chat_caret > *line_length) {
        tg_chat_caret = *line_length;
    }
    /* C:Break (or a break signal set while a query ran) should quit prompts
       too, not just network waits. */
    if (tg_platform_break_pending()) {
        return -1;
    }
    rc = tg_platform_stdin_read_char(timeout_seconds, &ch);
    if (rc <= 0) {
        /* Read timeout: check for a file dropped on the console window and
           inject its path at the caret, so "/sendfile " + drop just works.
           Only in the interactive chat editor with the TUI live; a no-op on
           every build but an armed OS4 console. */
        if (rc == 0 && use_history && tg_console_tui_active() &&
            tg_chat_tui_stream != 0) {
            char drop[256];

            if (tg_platform_console_drop_poll(drop, sizeof(drop))) {
                unsigned long di;
                char drop_note[300];

                /* Visible proof in the transcript that the drop ARRIVED (a
                   field report can then separate "never delivered" from
                   "injection failed"). */
                sprintf(drop_note, "Dropped: %.280s", drop);
                tg_console_tui_line(tg_chat_tui_stream, drop_note);

                for (di = 0UL; drop[di] != '\0'; ++di) {
                    unsigned long c;

                    if (*line_length + 1UL >= line_size) {
                        break;
                    }
                    c = tg_chat_caret;
                    if (c > *line_length) {
                        c = *line_length;
                    }
                    memmove(&line[c + 1UL], &line[c], *line_length - c);
                    line[c] = drop[di];
                    ++(*line_length);
                    tg_chat_caret = c + 1UL;
                }
                tg_console_tui_input_caret(
                    tg_chat_tui_stream, tg_console_tui_prompt(), line,
                    *line_length, tg_chat_caret);
            }
        }
        return rc;
    }
    if (raw && ch == (char)0x03) {
        /* RAW-mode consoles deliver Ctrl+C as a plain 0x03 byte instead of a
           break signal: treat it as end-of-input so every prompt can quit. */
        return -1;
    }
    if (ch == '\t' && raw && use_history) {
        /* Tab on an empty line jumps back to the previous chat; mid-line it
           is ignored (a literal tab inside a message helps nobody). */
        if (*line_length == 0UL && line_size > 6UL) {
            strcpy(line, "/swap");
            fputc('\n', stream);
            fflush(stream);
            return 1;
        }
        return 0;
    }
    if (ch == '\r' || ch == '\n') {
        if (*line_length >= line_size) {
            *line_length = line_size - 1UL;
        }
        line[*line_length] = '\0';
        if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
            /* Push the submitted line into the transcript so the dialogue
               keeps reading naturally, then clear the input row. */
            char echo_line[640];

            sprintf(echo_line, "%.90s%.500s", tg_console_tui_prompt(), line);
            tg_console_tui_line(tg_chat_tui_stream, echo_line);
            tg_console_tui_input(tg_chat_tui_stream,
                                 tg_console_tui_prompt(), 0, 0UL);
        } else if (raw) {
            fputc('\n', stream);
            fflush(stream);
        }
        if (use_history) {
            tg_chat_history_add(line);
        }
        *line_length = 0UL;
        tg_chat_caret = 0UL;
        return 1;
    }
    if (ch == '\b' || ch == 127) {
        if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
            unsigned long c;
            int changed = 0;

            c = tg_chat_caret;
            if (c > *line_length) {
                c = *line_length;
            }
            if (ch == 127) {
                /* Canc/Del: forward-delete AT the caret (the GUI fix, TUI
                   edition -- 0x7F is not a second backspace). */
                if (c < *line_length) {
                    memmove(&line[c], &line[c + 1UL],
                            *line_length - c - 1UL);
                    --(*line_length);
                    changed = 1;
                }
            } else if (c > 0UL) {
                /* Backspace: delete the char BEFORE the caret. */
                memmove(&line[c - 1UL], &line[c], *line_length - c);
                --(*line_length);
                tg_chat_caret = c - 1UL;
                changed = 1;
            }
            if (changed) {
                if (ch != 127 && tg_chat_caret == *line_length &&
                    tg_console_tui_input_backspace(tg_chat_tui_stream,
                                                   tg_console_tui_prompt(),
                                                   line,
                                                   *line_length)) {
                    /* caret-at-end rubout: no repaint, no flicker */
                } else {
                    tg_console_tui_input_caret(
                        tg_chat_tui_stream, tg_console_tui_prompt(), line,
                        *line_length, tg_chat_caret);
                }
            }
        } else if (*line_length > 0UL) {
            --(*line_length);
            if (raw) {
                fputs("\b \b", stream);
                fflush(stream);
            }
        }
        return 0;
    }
    if (raw && (ch == 0x1B || ch == (char)0x9BU)) {
        if (ch == 0x1B) {
            if (tg_platform_stdin_read_char(0UL, &ch) <= 0 || ch != '[') {
                return 0;
            }
        }
        if (tg_platform_stdin_read_char(0UL, &ch) <= 0) {
            return 0;
        }
        direction = (int)(unsigned char)ch;
        fkey = 0UL;
        if (direction >= '0' && direction <= '9') {
            /* Leading digits: either a function key (CSI <n> '~': F1..F10 =
               0~..9~, shifted = 10~..19~) or a raw input event report
               (CSI <class>;...| -- class 12 is the window NEWSIZE event the
               TUI subscribes to). Collect the number, then decide. */
            while (direction >= '0' && direction <= '9') {
                fkey = (fkey * 10UL) + (unsigned long)(direction - '0');
                if (tg_platform_stdin_read_char(0UL, &ch) <= 0) {
                    return 0;
                }
                direction = (int)(unsigned char)ch;
            }
            if (use_history && direction == '~' && fkey <= 19UL &&
                line_size >= 16UL) {
                /* Function key: jump straight to that chat by completing the
                   line as a synthesized /peer command. Any half-typed text
                   is discarded -- Fn is an explicit "go there now". */
                if (!tg_console_tui_active()) {
                    for (i = 0UL; i < *line_length; ++i) {
                        fputs("\b \b", stream);
                    }
                }
                sprintf(line, "/peer %lu", fkey + 1UL);
                *line_length = 0UL;
                tg_chat_caret = 0UL;
                if (!tg_console_tui_active()) {
                    fputc('\n', stream);
                }
                fflush(stream);
                return 1;
            }
            /* Not a function key: fall through to the CSI consumption. */
        }
        if (direction != 'A' && direction != 'B') {
            /* Not Up/Down. Consume the rest of an unrecognised CSI sequence
               so its trailing bytes do not leak into the typed line. CSI
               param and intermediate bytes are 0x20-0x3F; the final byte is
               0x40-0x7E. */
            while (direction >= 0x20 && direction < 0x40) {
                if (tg_platform_stdin_read_char(0UL, &ch) <= 0) {
                    break;
                }
                direction = (int)(unsigned char)ch;
            }
            if (direction == 'C') {
                /* A wrapped composer needs an absolute row/column repaint when
                   the caret crosses a visual-line boundary. */
                if (tg_console_tui_active() && tg_chat_tui_stream != 0 &&
                    tg_chat_caret < *line_length) {
                    ++tg_chat_caret;
                    tg_console_tui_input_caret(
                        tg_chat_tui_stream, tg_console_tui_prompt(), line,
                        *line_length, tg_chat_caret);
                }
            } else if (direction == 'D') {
                /* LEFT follows the same wrapped-row layout as RIGHT. */
                if (tg_console_tui_active() && tg_chat_tui_stream != 0 &&
                    tg_chat_caret > 0UL) {
                    --tg_chat_caret;
                    tg_console_tui_input_caret(
                        tg_chat_tui_stream, tg_console_tui_prompt(), line,
                        *line_length, tg_chat_caret);
                }
            } else if (direction == '|' && fkey == 12UL) {
                /* Window NEWSIZE raw event: let the chat loop repaint the
                   full-screen layout with the new geometry. */
                tg_console_tui_note_resize();
            } else if (direction == '|' && fkey == 11UL) {
                /* CLOSEWINDOW raw event: the user clicked the console's close
                   gadget. Same clean quit as Ctrl+C/EOF -- the chat loop
                   prints "Input closed.", leaves the TUI and returns. */
                return -1;
            } else if (direction == 'T') {
                /* Shift+Up (CSI T on every Amiga console; PgUp on most
                   modern keymaps): page back through the transcript. */
                tg_console_tui_scroll(stream, 1);
            } else if (direction == 'S') {
                /* Shift+Down: toward live. */
                tg_console_tui_scroll(stream, -1);
            }
            return 0;
        }
        if (!use_history || tg_chat_history_count == 0UL) {
            return 0;
        }
        if (direction == 'A') {
            if (tg_chat_history_recall < 0L) {
                idx = (long)tg_chat_history_count - 1L;
            } else if (tg_chat_history_recall > 0L) {
                idx = tg_chat_history_recall - 1L;
            } else {
                idx = 0L;
            }
        } else {
            if (tg_chat_history_recall < 0L) {
                return 0;
            }
            idx = tg_chat_history_recall + 1L;
        }
        if (!tg_console_tui_active()) {
            for (i = 0UL; i < *line_length; ++i) {
                fputs("\b \b", stream);
            }
        }
        if (direction == 'B' && idx >= (long)tg_chat_history_count) {
            *line_length = 0UL;
            line[0] = '\0';
            tg_chat_caret = 0UL;
            tg_chat_history_recall = -1L;
            if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
                tg_console_tui_input(tg_chat_tui_stream,
                                     tg_console_tui_prompt(), 0, 0UL);
            }
            fflush(stream);
            return 0;
        }
        tg_chat_history_recall = idx;
        strncpy(line, tg_chat_history[idx], line_size - 1UL);
        line[line_size - 1UL] = '\0';
        *line_length = (unsigned long)strlen(line);
        tg_chat_caret = *line_length;
        if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
            tg_console_tui_input(tg_chat_tui_stream,
                                 tg_console_tui_prompt(), line,
                                 *line_length);
        } else {
            fputs(line, stream);
        }
        fflush(stream);
        return 0;
    }
    if (*line_length + 1UL < line_size) {
        if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
            unsigned long c;

            c = tg_chat_caret;
            if (c > *line_length) {
                c = *line_length;
            }
            /* insert at the caret, shifting the tail right (the buffer is not
               NUL-terminated mid-edit, so move exactly *line_length - c bytes) */
            memmove(&line[c + 1UL], &line[c], *line_length - c);
            line[c] = ch;
            ++(*line_length);
            tg_chat_caret = c + 1UL;
            if (use_history && ch == '/' && *line_length == 1UL) {
                /* Command hint: a lone leading '/' lists what can follow (the
                   full stories live in /help). Printed to the transcript, then
                   the input row is redrawn below with the '/' kept. */
                FILE *hint_cap = tg_console_tui_capture_begin(stream);

                fputs(TG_CHAT_SLASH_HINT_TEXT, hint_cap);
                tg_console_tui_capture_end(hint_cap, stream);
            }
            if (tg_chat_caret == *line_length &&
                (!use_history || ch != '/' || *line_length != 1UL) &&
                tg_console_tui_input_append(tg_chat_tui_stream,
                                            tg_console_tui_prompt(),
                                            line,
                                            *line_length, ch)) {
                /* caret-at-end echo: no repaint, no flicker */
            } else {
                tg_console_tui_input_caret(
                    tg_chat_tui_stream, tg_console_tui_prompt(), line,
                    *line_length, tg_chat_caret);
            }
        } else {
            line[*line_length] = ch;
            ++(*line_length);
            if (raw) {
                fputc(ch, stream);
                fflush(stream);
                if (use_history && ch == '/' && *line_length == 1UL) {
                    /* Command hint, linear-raw flavour (consoles with no
                       full-screen layout -- AROS, plain pipes): print the
                       list on its own lines, then re-echo the typed '/' so
                       the user keeps typing on a fresh line. The TUI branch
                       above prints the same hint into the transcript. */
                    fputs("\n" TG_CHAT_SLASH_HINT_TEXT, stream);
                    fwrite(line, 1, (size_t)*line_length, stream);
                    fflush(stream);
                }
            }
        }
    }
    return 0;
}

static int tg_mtproto_chat_prompt_line(const char *prompt,
                                       char *out,
                                       unsigned long out_size,
                                       int required,
                                       FILE *stream,
                                       const char *label)
{
    unsigned long line_length;
    int rc;

    if (out != 0 && out_size > 0UL) {
        out[0] = '\0';
    }
    if (prompt == 0 || out == 0 || out_size == 0UL || stream == 0 ||
        label == 0) {
        return 2;
    }
    if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
        tg_console_tui_set_prompt(prompt);
        tg_console_tui_input(tg_chat_tui_stream, prompt, 0, 0UL);
    } else {
        fputs(prompt, stream);
        fflush(stream);
    }
    line_length = 0UL;
    for (;;) {
        if (tg_console_tui_resize_pending() && tg_chat_tui_stream != 0) {
            if (tg_console_tui_resize(tg_chat_tui_stream,
                                      " Telegram Amiga ")) {
                tg_console_tui_input_caret(
                    tg_chat_tui_stream, tg_console_tui_prompt(), out,
                    line_length, tg_chat_caret);
            }
        }
        /* Drive the same editor as the main loop so the prompt echoes and
           line-edits in raw mode (use_history = 0: do not record/recall the
           message history for these one-shot answers). In cooked fallback mode
           tg_chat_input_raw is 0 and it behaves like the old reader. */
        rc = tg_mtproto_chat_read_line_edit(out, out_size, &line_length,
                                            3600UL, tg_chat_input_raw, 0,
                                            stream);
        if (rc < 0) {
            fprintf(stream, "%s: input-closed\n", label);
            return 2;
        }
        if (rc == 0) {
            continue;
        }
        tg_mtproto_trim_line(out);
        if (required && out[0] == '\0') {
            fprintf(stream, "%s: input-empty\n", label);
            return 2;
        }
        return 0;
    }
}

static int tg_mtproto_chat_peer_command_arg(const char *line,
                                            const char **arg)
{
    const char *p;

    if (line == 0 || arg == 0) {
        return 0;
    }
    if (strncmp(line, "/peer", 5) == 0 &&
        (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
        p = line + 5;
    } else if (strncmp(line, "peer", 4) == 0 &&
               (line[4] == '\0' || line[4] == ' ' || line[4] == '\t')) {
        p = line + 4;
    } else {
        return 0;
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    *arg = p;
    return 1;
}

static int tg_mtproto_chat_named_command_arg(const char *line,
                                             const char *command,
                                             const char **arg)
{
    unsigned long length;

    if (arg != 0) {
        *arg = "";
    }
    if (line == 0 || command == 0 || arg == 0) {
        return 0;
    }
    length = (unsigned long)strlen(command);
    if (strncmp(line, command, (size_t)length) != 0) {
        return 0;
    }
    if (line[length] != '\0' && line[length] != ' ' &&
        line[length] != '\t') {
        return 0;
    }
    *arg = line + length;
    while (**arg == ' ' || **arg == '\t') {
        ++*arg;
    }
    return 1;
}

static int tg_mtproto_chat_copy_peer_index(char *dest,
                                           unsigned long dest_size,
                                           const char *src)
{
    const char *peer_arg;
    unsigned long length;

    if (dest == 0 || dest_size == 0UL || src == 0 || src[0] == '\0') {
        return 2;
    }
    if (tg_mtproto_chat_peer_command_arg(src, &peer_arg) &&
        peer_arg[0] != '\0') {
        src = peer_arg;
    }
    length = (unsigned long)strlen(src);
    if (length >= dest_size) {
        return 2;
    }
    strcpy(dest, src);
    tg_mtproto_trim_line(dest);
    return dest[0] != '\0' ? 0 : 2;
}

static int tg_mtproto_chat_is_number_line(const char *line)
{
    const char *p;

    if (line == 0 || line[0] == '\0') {
        return 0;
    }
    p = line;
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        ++p;
    }
    return 1;
}

/* Console rendering of one resolved transcript row: the day separator (when the
   day changes) + [HH:MM] time, the optional [group] prefix, the sender prefix
   (own / resolved sender / 1:1 peer / unknown group author / "them:") and the
   message text. `day_shown` carries the last printed day across calls. This is
   the console driver's realisation of a tg_chat_message_row; the GUI driver
   projects the same row into a tg_gui_message instead. Pinned byte-for-byte by
   the --chat-render-self-test golden. */
static void tg_mtproto_chat_render_message(FILE *stream,
                                           const tg_chat_message_row *row,
                                           unsigned long *day_shown)
{
    if (stream == 0 || row == 0 || day_shown == 0) {
        return;
    }
    if (row->has_time) {
        if (*day_shown != row->local_epoch / 86400UL) {
            if (*day_shown != 0UL) {
                tg_mtproto_chat_print_day_separator(stream, row->local_epoch);
            }
            *day_shown = row->local_epoch / 86400UL;
        }
        tg_mtproto_chat_print_message_time(stream, row->local_epoch);
    }
    if (row->is_group && row->peer_label != 0 && row->peer_label[0] != '\0') {
        tg_console_ui_role(stream, TG_UI_ROLE_GROUP);
        fputc('[', stream);
        tg_mtproto_print_label_truncated(stream, row->peer_label,
                                         TG_MTPROTO_GROUP_LABEL_MAX);
        fputc(']', stream);
        tg_console_ui_reset(stream);
        fputc(' ', stream);
    }
    if (row->is_out) {
        tg_console_ui_role(stream, TG_UI_ROLE_OWN);
        if (row->own_label != 0 && row->own_label[0] != '\0') {
            tg_mtproto_print_cache_text(stream, row->own_label);
            fprintf(stream, ":");
        } else {
            fprintf(stream, "me:");
        }
        tg_console_ui_reset(stream);
        fputc(' ', stream);
    } else {
        tg_console_ui_role(stream, TG_UI_ROLE_PEER);
        if (row->sender != 0) {
            tg_mtproto_print_cache_text(stream, row->sender);
            fprintf(stream, ":");
        } else if (!row->is_group && row->peer_label != 0 &&
                   row->peer_label[0] != '\0') {
            /* 1:1 chat: the peer is the sender. */
            tg_mtproto_print_cache_text(stream, row->peer_label);
            fprintf(stream, ":");
        } else if (row->is_group) {
            /* Group sender we could not resolve; the [group] prefix is already
               shown, so just mark the unknown author. */
            fprintf(stream, "?:");
        } else {
            fprintf(stream, "them:");
        }
        tg_console_ui_reset(stream);
        fputc(' ', stream);
    }
    if (row->reply_quote != 0 && row->reply_quote[0] != '\0') {
        /* Quoted reference, ASCII-bracketed before the body (plain -- no SGR
           role so MorphOS stays safe even with colour on). */
        fputs("[> ", stream);
        tg_mtproto_print_message_text(stream, row->reply_quote);
        fputs("] ", stream);
    }
    tg_mtproto_print_message_text(stream, row->text);
    tg_console_ui_end_line(stream);
}

/* The console driver: realises tg_chat_driver.on_message by printing the row
   with tg_mtproto_chat_render_message. Its ctx carries the output stream and
   the persistent day-separator cursor. */
typedef struct tg_chat_console_driver {
    FILE *stream;
    unsigned long *day_shown;
} tg_chat_console_driver;

static void tg_chat_console_on_message(void *ctx,
                                       const tg_chat_message_row *row)
{
    tg_chat_console_driver *console = (tg_chat_console_driver *)ctx;

    tg_mtproto_chat_render_message(console->stream, row, console->day_shown);
}

/* Golden parity harness for the transcript renderer. Drives
   tg_mtproto_chat_render_message over a fixed message script (colours off,
   latin1, fixed epochs so gmtime output is deterministic and host/CI-portable)
   and asserts the captured bytes match a recorded golden. This pins the
   model/view seam byte-for-byte across the chat-engine extraction steps that
   reroute it (history split, tick body, command dispatch). */
#if !defined(TG_NO_SELFTEST)
static const char tg_chat_render_golden[] =
    "[22:13] Mario: Ciao\n"
    "[22:18] Io: Tutto bene?\n"
    "[23:13] [Sviluppo] Alice: Salve a tutti\n"
    "[23:18] [Sviluppo] ?: boh\n"
    "--- 15 Nov ---\n"
    "[23:13] Mario: Nuovo giorno\n"
    "--- 16 Nov ---\n"
    "[00:13] [Sviluppo Server..] Bob: ok\n"
    "Mario: senza data\n"
    "them: anon\n"
    "me: io anon\n"
    "Mario: riga1\n"
    "  riga2\n";

static void tg_chat_render_emit(tg_chat_driver *driver, unsigned long date,
                                int is_out, int is_group, const char *peer_label,
                                const char *own_label, const char *sender,
                                const char *text)
{
    tg_chat_message_row row;

    memset(&row, 0, sizeof(row));
    row.text = text;
    row.has_time = (date != 0UL);
    row.local_epoch = date; /* the test uses delta 0, so local_epoch == date */
    row.is_out = is_out;
    row.is_group = is_group;
    row.peer_label = peer_label;
    row.own_label = own_label;
    row.sender = sender;
    row.reply_quote = 0; /* golden script carries no replies */
    driver->on_message(driver->ctx, &row);
}

int tg_mtproto_chat_render_self_test(void)
{
    unsigned long day_shown;
    tg_chat_console_driver console_drv;
    tg_chat_driver driver;
    FILE *cap;
    char buf[1024];
    size_t n;
    int saved_color;
    int saved_charset;
    int saved_theme;

    saved_color = tg_console_ui_color_mode();
    saved_charset = tg_console_ui_charset();
    saved_theme = tg_console_ui_theme();
    tg_console_ui_set_color_mode(TG_UI_COLOR_OFF);
    tg_console_ui_set_charset(TG_UI_CHARSET_LATIN1);
    tg_console_ui_set_theme(TG_UI_THEME_PLAIN);

    cap = tmpfile();
    if (cap == 0) {
        puts("chat render self-test: cannot open temp file");
        return 2;
    }
    day_shown = 0UL;
    /* Route every row through the console driver's on_message so the test pins
       the whole step-3 path (driver vtable -> render_message), not just the
       renderer in isolation. */
    console_drv.stream = cap;
    console_drv.day_shown = &day_shown;
    driver.ctx = &console_drv;
    driver.on_message = tg_chat_console_on_message;
    driver.on_chat_list_changed = 0;
    driver.on_notification = 0;

    tg_chat_render_emit(&driver, 1700000000UL, 0, 0, "Mario", "Io", 0, "Ciao");
    tg_chat_render_emit(&driver, 1700000300UL, 1, 0, "Mario", "Io", 0,
                        "Tutto bene?");
    tg_chat_render_emit(&driver, 1700003600UL, 0, 1, "Sviluppo", "Io", "Alice",
                        "Salve a tutti");
    tg_chat_render_emit(&driver, 1700003900UL, 0, 1, "Sviluppo", "Io", 0, "boh");
    tg_chat_render_emit(&driver, 1700090000UL, 0, 0, "Mario", "Io", 0,
                        "Nuovo giorno");
    /* Long group label: exercises tg_mtproto_print_label_truncated (>=16 chars
       -> trailing-space trim + ".." ellipsis). */
    tg_chat_render_emit(&driver, 1700093600UL, 0, 1, "Sviluppo Server Group",
                        "Io", "Bob", "ok");
    tg_chat_render_emit(&driver, 0UL, 0, 0, "Mario", "Io", 0, "senza data");
    tg_chat_render_emit(&driver, 0UL, 0, 0, "", "Io", 0, "anon");
    tg_chat_render_emit(&driver, 0UL, 1, 0, "", "", 0, "io anon");
    tg_chat_render_emit(&driver, 0UL, 0, 0, "Mario", "Io", 0, "riga1\nriga2");

    rewind(cap);
    n = fread(buf, 1, sizeof(buf) - 1U, cap);
    buf[n] = '\0';
    fclose(cap);

    tg_console_ui_set_color_mode(saved_color);
    tg_console_ui_set_charset(saved_charset);
    tg_console_ui_set_theme(saved_theme);

    if (strcmp(buf, tg_chat_render_golden) != 0) {
        printf("chat render self-test: MISMATCH\n---actual(%lu)---\n%s"
               "---end---\n",
               (unsigned long)n, buf);
        return 2;
    }
    /* A reply row renders its quoted reference bracketed before the body. */
    {
        tg_chat_message_row qrow;
        unsigned long qday = 0UL;
        FILE *qcap;
        char qbuf[256];
        size_t qn;

        qcap = tmpfile();
        if (qcap == 0) {
            puts("chat render self-test: cannot open temp file");
            return 2;
        }
        memset(&qrow, 0, sizeof(qrow));
        qrow.text = "ok";
        qrow.has_time = 0;
        qrow.local_epoch = 0UL;
        qrow.is_out = 0;
        qrow.is_group = 0;
        qrow.peer_label = "Mario";
        qrow.own_label = "Io";
        qrow.sender = "Mario";
        qrow.reply_quote = "ciao";
        qrow.has_document = 0;
        tg_mtproto_chat_render_message(qcap, &qrow, &qday);
        rewind(qcap);
        qn = fread(qbuf, 1, sizeof(qbuf) - 1U, qcap);
        qbuf[qn] = '\0';
        fclose(qcap);
        if (strstr(qbuf, "[> ciao]") == 0) {
            printf("chat render self-test: reply quote MISMATCH: %s\n", qbuf);
            return 2;
        }
    }
    if (tg_console_tui_layout_self_test() != 0) {
        return 2;
    }
    puts("chat render self-test: ok (transcript renderer golden)");
    return 0;
}
#endif /* !TG_NO_SELFTEST */

/* --- 0.0.9 inline-photo cache queue ------------------------------------
   History parsing offers compact Photo metadata here while the response is
   still alive. The GUI event loop later downloads at most one chunk per turn;
   paints only ever read a completed photos/tgph*.jpg. Newest visible photos
   win if the bounded queue fills. */
#if defined(TG_NO_GUI)
/* Text-only build: nothing ever displays a photo, so the catalogue and the
   queues shrink to the smallest legal size instead of holding 48 entries of
   photo metadata for a client that cannot show them. */
#define TG_GUI_PHOTO_QUEUE_MAX 1
#define TG_GUI_PHOTO_TRIED_MAX 1
#define TG_GUI_PHOTO_CATALOG_MAX 1
#elif defined(__m68k__)
#define TG_GUI_PHOTO_QUEUE_MAX 8
#define TG_GUI_PHOTO_TRIED_MAX 24
#define TG_GUI_PHOTO_CATALOG_MAX 48
#else
#define TG_GUI_PHOTO_QUEUE_MAX 16
#define TG_GUI_PHOTO_TRIED_MAX 48
#define TG_GUI_PHOTO_CATALOG_MAX 96
#endif

typedef struct tg_gui_photo_queue_entry {
    tg_mtproto_photo_meta photo;
    int large;
    int progressive_skipped;
    int require_jpeg; /* Save-as must bypass an already-decoded .pgc cache. */
} tg_gui_photo_queue_entry;

typedef struct tg_gui_photo_cache_variant {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned char large;
    char type[TG_MTPROTO_PHOTO_TYPE_MAX];
} tg_gui_photo_cache_variant;

typedef struct tg_gui_photo_fetch_state {
    int active;
    int large;
    tg_mtproto_photo_meta photo;
    tg_mtproto_file_ctx home;
    tg_mtproto_file_ctx file;
    FILE *out;
    FILE *quiet;
    FILE *stream;
    unsigned long offset;
    unsigned long need_dc;
    int retries;
    char path[64];
    char part_path[72];
} tg_gui_photo_fetch_state;

static tg_gui_photo_queue_entry tg_gui_photo_queue[TG_GUI_PHOTO_QUEUE_MAX];
static int tg_gui_photo_queue_count;
static unsigned long tg_gui_photo_tried_hi[TG_GUI_PHOTO_TRIED_MAX];
static unsigned long tg_gui_photo_tried_lo[TG_GUI_PHOTO_TRIED_MAX];
static unsigned char tg_gui_photo_tried_large[TG_GUI_PHOTO_TRIED_MAX];
static char tg_gui_photo_tried_type[TG_GUI_PHOTO_TRIED_MAX]
                                     [TG_MTPROTO_PHOTO_TYPE_MAX];
static int tg_gui_photo_tried_count;
static tg_gui_photo_cache_variant
    tg_gui_photo_cache_variants[TG_GUI_PHOTO_TRIED_MAX];
static int tg_gui_photo_cache_variant_count;
static tg_mtproto_photo_meta tg_gui_photo_catalog[TG_GUI_PHOTO_CATALOG_MAX];
static int tg_gui_photo_catalog_count;
static int tg_gui_photo_inline_enabled = 1;
static int tg_gui_photo_cache_paused;
static tg_gui_photo_fetch_state tg_gui_photo_fetch;
static int tg_gui_photo_queue_pop(tg_gui_photo_queue_entry *entry);
static tg_mtproto_photo_meta *tg_gui_photo_catalog_find(
    unsigned long id_hi, unsigned long id_lo);

static void tg_gui_photo_log(const char *message)
{
    if (tg_gui_log_is_enabled()) {
        tg_gui_log(message);
    }
}

static void tg_gui_photo_log_progress(const char *stage,
                                      unsigned long current,
                                      unsigned long total)
{
    char line[96];

    if (!tg_gui_log_is_enabled()) {
        return;
    }
    sprintf(line, "photo: %s %lu/%lu", stage, current, total);
    tg_gui_log(line);
}

int tg_gui_session_photo_cache_path(char *path, unsigned long path_size,
                                    unsigned long id_hi, unsigned long id_lo,
                                    int large)
{
    if (path == 0 || path_size < 40UL || (id_hi == 0UL && id_lo == 0UL)) {
        return 1;
    }
    sprintf(path, large ? "photos/tgph%08lx%08lx-l.jpg"
                        : "photos/tgph%08lx%08lx.jpg",
            id_hi, id_lo);
    return 0;
}

int tg_gui_session_photo_thumb_cache_path(char *path,
                                          unsigned long path_size,
                                          unsigned long id_hi,
                                          unsigned long id_lo)
{
    if (path == 0 || path_size < 42UL || (id_hi == 0UL && id_lo == 0UL)) {
        return 1;
    }
    sprintf(path, "photos/tgph%08lx%08lx-t.jpg", id_hi, id_lo);
    return 0;
}

int tg_gui_session_photo_canonical_cache_path(
    char *path, unsigned long path_size,
    unsigned long id_hi, unsigned long id_lo, int large)
{
    if (path == 0 || path_size < 42UL || (id_hi == 0UL && id_lo == 0UL)) {
        return 1;
    }
    sprintf(path, large ? "photos/tgph%08lx%08lx-l.pgc"
                        : "photos/tgph%08lx%08lx.pgc",
            id_hi, id_lo);
    return 0;
}

static int tg_gui_photo_canonical_cache_exists(unsigned long id_hi,
                                               unsigned long id_lo,
                                               int large)
{
    char path[64];
    FILE *probe;

    if (tg_gui_session_photo_canonical_cache_path(
            path, sizeof(path), id_hi, id_lo, large) != 0) {
        return 0;
    }
    probe = fopen(path, "rb");
    if (probe == 0) {
        return 0;
    }
    fclose(probe);
    return 1;
}

static int tg_gui_photo_thumb_cache_exists(unsigned long id_hi,
                                           unsigned long id_lo)
{
    char path[64];
    FILE *probe;

    if (tg_gui_session_photo_thumb_cache_path(
            path, sizeof(path), id_hi, id_lo) != 0) {
        return 0;
    }
    probe = fopen(path, "rb");
    if (probe == 0) {
        return 0;
    }
    fclose(probe);
    return 1;
}

static void tg_gui_photo_store_stripped(const tg_mtproto_photo_meta *photo)
{
    unsigned char jpeg[900];
    unsigned long jpeg_len;
    char path[64];
    char part_path[72];
    FILE *out;
    int ok;
    int close_rc;

    if (photo == 0 || photo->stripped_len == 0UL ||
        tg_gui_photo_thumb_cache_exists(photo->id_hi, photo->id_lo) ||
        tg_gui_session_photo_thumb_cache_path(
            path, sizeof(path), photo->id_hi, photo->id_lo) != 0 ||
        tg_avatar_expand_stripped(photo->stripped, photo->stripped_len,
                                  jpeg, sizeof(jpeg), &jpeg_len) != 0) {
        return;
    }
    (void)mkdir("photos", 0777);
    sprintf(part_path, "%s.part", path);
    (void)remove(part_path);
    out = fopen(part_path, "wb");
    if (out == 0) {
        return;
    }
    ok = fwrite(jpeg, 1, jpeg_len, out) == jpeg_len;
    close_rc = fclose(out);
    if (close_rc != 0) {
        ok = 0;
    }
    if (ok) {
        (void)remove(path); /* AmigaDOS Rename does not replace a target. */
        ok = rename(part_path, path) == 0;
        if (ok) {
            tg_gui_window_photo_cache_file_changed(path);
        }
    }
    if (!ok) {
        (void)remove(part_path);
        tg_gui_window_photo_cache_file_changed(path);
        tg_gui_photo_log("photo: stripped preview cache write failed");
    }
}

static int tg_gui_photo_cache_exists(unsigned long id_hi, unsigned long id_lo,
                                     int large)
{
    char path[64];
    FILE *probe;

    if (tg_gui_session_photo_cache_path(path, sizeof(path), id_hi, id_lo,
                                        large) != 0) {
        return 0;
    }
    probe = fopen(path, "rb");
    if (probe == 0) {
        return 0;
    }
    fclose(probe);
    return 1;
}

static int tg_gui_photo_was_tried(unsigned long id_hi, unsigned long id_lo,
                                  int large, const char *type)
{
    int i;

    if (type == 0 || type[0] == '\0') {
        return 0;
    }
    for (i = 0; i < tg_gui_photo_tried_count; ++i) {
        if (tg_gui_photo_tried_hi[i] == id_hi &&
            tg_gui_photo_tried_lo[i] == id_lo &&
            tg_gui_photo_tried_large[i] == (unsigned char)(large ? 1 : 0) &&
            strcmp(tg_gui_photo_tried_type[i], type) == 0) {
            return 1;
        }
    }
    return 0;
}

static void tg_gui_photo_remember_tried(unsigned long id_hi,
                                        unsigned long id_lo, int large,
                                        const char *type)
{
    int at;

    if (type == 0 || type[0] == '\0' ||
        tg_gui_photo_was_tried(id_hi, id_lo, large, type)) {
        return;
    }
    at = tg_gui_photo_tried_count;
    if (at >= TG_GUI_PHOTO_TRIED_MAX) {
        int i;

        for (i = 1; i < TG_GUI_PHOTO_TRIED_MAX; ++i) {
            tg_gui_photo_tried_hi[i - 1] = tg_gui_photo_tried_hi[i];
            tg_gui_photo_tried_lo[i - 1] = tg_gui_photo_tried_lo[i];
            tg_gui_photo_tried_large[i - 1] = tg_gui_photo_tried_large[i];
            strcpy(tg_gui_photo_tried_type[i - 1],
                   tg_gui_photo_tried_type[i]);
        }
        at = TG_GUI_PHOTO_TRIED_MAX - 1;
    } else {
        ++tg_gui_photo_tried_count;
    }
    tg_gui_photo_tried_hi[at] = id_hi;
    tg_gui_photo_tried_lo[at] = id_lo;
    tg_gui_photo_tried_large[at] = (unsigned char)(large ? 1 : 0);
    strcpy(tg_gui_photo_tried_type[at], type);
}

static void tg_gui_photo_cache_variant_remember(unsigned long id_hi,
                                                unsigned long id_lo,
                                                int large,
                                                const char *type)
{
    int i;
    int at;

    if (type == 0 || type[0] == '\0') {
        return;
    }
    for (i = 0; i < tg_gui_photo_cache_variant_count; ++i) {
        if (tg_gui_photo_cache_variants[i].id_hi == id_hi &&
            tg_gui_photo_cache_variants[i].id_lo == id_lo &&
            tg_gui_photo_cache_variants[i].large ==
                (unsigned char)(large ? 1 : 0)) {
            strcpy(tg_gui_photo_cache_variants[i].type, type);
            return;
        }
    }
    at = tg_gui_photo_cache_variant_count;
    if (at >= TG_GUI_PHOTO_TRIED_MAX) {
        for (i = 1; i < TG_GUI_PHOTO_TRIED_MAX; ++i) {
            tg_gui_photo_cache_variants[i - 1] =
                tg_gui_photo_cache_variants[i];
        }
        at = TG_GUI_PHOTO_TRIED_MAX - 1;
    } else {
        ++tg_gui_photo_cache_variant_count;
    }
    tg_gui_photo_cache_variants[at].id_hi = id_hi;
    tg_gui_photo_cache_variants[at].id_lo = id_lo;
    tg_gui_photo_cache_variants[at].large =
        (unsigned char)(large ? 1 : 0);
    strcpy(tg_gui_photo_cache_variants[at].type, type);
}

static int tg_gui_photo_cache_variant_forget(unsigned long id_hi,
                                             unsigned long id_lo,
                                             int large, char *type,
                                             unsigned long type_size)
{
    int i;
    int j;

    if (type != 0 && type_size != 0UL) {
        type[0] = '\0';
    }
    for (i = 0; i < tg_gui_photo_cache_variant_count; ++i) {
        if (tg_gui_photo_cache_variants[i].id_hi == id_hi &&
            tg_gui_photo_cache_variants[i].id_lo == id_lo &&
            tg_gui_photo_cache_variants[i].large ==
                (unsigned char)(large ? 1 : 0)) {
            if (type != 0 && type_size != 0UL) {
                strncpy(type, tg_gui_photo_cache_variants[i].type,
                        type_size - 1UL);
                type[type_size - 1UL] = '\0';
            }
            for (j = i + 1; j < tg_gui_photo_cache_variant_count; ++j) {
                tg_gui_photo_cache_variants[j - 1] =
                    tg_gui_photo_cache_variants[j];
            }
            --tg_gui_photo_cache_variant_count;
            return 1;
        }
    }
    return 0;
}

static int tg_gui_photo_prepare_queue_entry(
    const tg_mtproto_photo_meta *source, int large,
    tg_gui_photo_queue_entry *entry)
{
    unsigned long current_edge;
    unsigned long current_size;
    unsigned long best_edge;
    unsigned long i;
    const tg_mtproto_photo_variant *best;
    int selected_progressive;

    if (source == 0 || entry == 0 || !source->has_photo) {
        return 0;
    }
    memset(entry, 0, sizeof(*entry));
    entry->photo = *source;
    entry->large = large ? 1 : 0;
    if (entry->large && source->has_large) {
        strcpy(entry->photo.thumb_type, source->large_thumb_type);
        entry->photo.width = source->large_width;
        entry->photo.height = source->large_height;
        entry->photo.size = source->large_size;
    }
    if (entry->photo.thumb_type[0] == '\0' || entry->photo.width == 0UL ||
        entry->photo.height == 0UL || entry->photo.size == 0UL) {
        return 0;
    }
    selected_progressive = 0;
    for (i = 0UL; i < source->variant_count; ++i) {
        if (strcmp(source->variants[i].type, entry->photo.thumb_type) == 0) {
            selected_progressive = source->variants[i].progressive != 0U;
        } else if (source->variants[i].progressive) {
            entry->progressive_skipped = 1;
        }
    }
    if (!tg_gui_photo_was_tried(entry->photo.id_hi, entry->photo.id_lo,
                                entry->large, entry->photo.thumb_type)) {
        return 1;
    }

    /* A rejected source falls back only toward smaller baseline files. This
       preserves the platform byte bound of the parser's primary choice while
       avoiding another format that the baseline-only decoder cannot read. */
    current_edge = entry->photo.width > entry->photo.height
                       ? entry->photo.width : entry->photo.height;
    current_size = entry->photo.size;
    best = 0;
    best_edge = 0UL;
    for (i = 0UL; i < source->variant_count; ++i) {
        const tg_mtproto_photo_variant *variant;
        unsigned long edge;

        variant = &source->variants[i];
        edge = variant->width > variant->height
                   ? variant->width : variant->height;
        if (variant->progressive || variant->type[0] == '\0' ||
            edge == 0UL || edge >= current_edge ||
            variant->size == 0UL || variant->size > current_size ||
            tg_gui_photo_was_tried(source->id_hi, source->id_lo,
                                   entry->large, variant->type)) {
            continue;
        }
        if (best == 0 || edge > best_edge ||
            (edge == best_edge && variant->size < best->size)) {
            best = variant;
            best_edge = edge;
        }
    }
    if (best == 0) {
        return 0;
    }
    strcpy(entry->photo.thumb_type, best->type);
    entry->photo.width = best->width;
    entry->photo.height = best->height;
    entry->photo.size = best->size;
    entry->progressive_skipped = selected_progressive ||
                                 entry->progressive_skipped;
    return 1;
}

static int tg_gui_photo_queue_insert(const tg_gui_photo_queue_entry *source)
{
    tg_gui_photo_queue_entry entry;
    int i;

    if (source == 0) {
        return 0;
    }
    entry = *source;
    if (tg_gui_photo_cache_exists(entry.photo.id_hi, entry.photo.id_lo,
                                  entry.large) ||
        (!entry.require_jpeg &&
         tg_gui_photo_canonical_cache_exists(
             entry.photo.id_hi, entry.photo.id_lo, entry.large))) {
        return 0;
    }
    if (tg_gui_photo_fetch.active &&
        tg_gui_photo_fetch.large == entry.large &&
        tg_gui_photo_fetch.photo.id_hi == entry.photo.id_hi &&
        tg_gui_photo_fetch.photo.id_lo == entry.photo.id_lo) {
        return 1;
    }
    for (i = 0; i < tg_gui_photo_queue_count; ++i) {
        if (tg_gui_photo_queue[i].large == entry.large &&
            tg_gui_photo_queue[i].photo.id_hi == entry.photo.id_hi &&
            tg_gui_photo_queue[i].photo.id_lo == entry.photo.id_lo) {
            int j;

            for (j = i + 1; j < tg_gui_photo_queue_count; ++j) {
                tg_gui_photo_queue[j - 1] = tg_gui_photo_queue[j];
            }
            tg_gui_photo_queue[tg_gui_photo_queue_count - 1] = entry;
            return 1;
        }
    }
    if (tg_gui_photo_queue_count >= TG_GUI_PHOTO_QUEUE_MAX) {
        int drop;

        drop = 0;
        for (i = 0; i < TG_GUI_PHOTO_QUEUE_MAX; ++i) {
            if (!tg_gui_photo_queue[i].large) {
                drop = i;
                break;
            }
        }
        for (i = drop + 1; i < TG_GUI_PHOTO_QUEUE_MAX; ++i) {
            tg_gui_photo_queue[i - 1] = tg_gui_photo_queue[i];
        }
        tg_gui_photo_queue_count = TG_GUI_PHOTO_QUEUE_MAX - 1;
    }
    tg_gui_photo_queue[tg_gui_photo_queue_count++] = entry;
    return 1;
}

void tg_gui_session_photo_decode_failed_variant(unsigned long id_hi,
                                                unsigned long id_lo,
                                                int large)
{
    char path[64];
    char failed_type[TG_MTPROTO_PHOTO_TYPE_MAX];
    tg_mtproto_photo_meta *photo;
    tg_gui_photo_queue_entry fallback;
    int known_type;

    if (id_hi == 0UL && id_lo == 0UL) {
        return;
    }
    if (tg_gui_session_photo_cache_path(path, sizeof(path), id_hi, id_lo,
                                        large) != 0) {
        return;
    }
    (void)remove(path);
    tg_gui_window_photo_cache_file_removed(path);
    known_type = tg_gui_photo_cache_variant_forget(
        id_hi, id_lo, large, failed_type, sizeof(failed_type));
    if (known_type) {
        tg_gui_photo_remember_tried(id_hi, id_lo, large, failed_type);
    }
    photo = tg_gui_photo_catalog_find(id_hi, id_lo);
    if (photo != 0 &&
        tg_gui_photo_prepare_queue_entry(photo, large, &fallback) &&
        tg_gui_photo_queue_insert(&fallback)) {
        char line[80];

        sprintf(line, known_type ? "photo: decode reject; fallback %s"
                                 : "photo: old cache rejected; retry %s",
                fallback.photo.thumb_type);
        tg_gui_photo_log(line);
    } else {
        tg_gui_photo_log("photo: decode reject; baseline sizes exhausted");
    }
}

void tg_gui_session_photo_decode_failed(unsigned long id_hi,
                                        unsigned long id_lo)
{
    tg_gui_session_photo_decode_failed_variant(id_hi, id_lo, 0);
}

static tg_mtproto_photo_meta *tg_gui_photo_catalog_find(unsigned long id_hi,
                                                       unsigned long id_lo)
{
    int i;

    for (i = 0; i < tg_gui_photo_catalog_count; ++i) {
        if (tg_gui_photo_catalog[i].id_hi == id_hi &&
            tg_gui_photo_catalog[i].id_lo == id_lo) {
            return &tg_gui_photo_catalog[i];
        }
    }
    return 0;
}

static void tg_gui_photo_catalog_offer(const tg_mtproto_photo_meta *photo)
{
    tg_mtproto_photo_meta *old;
    int i;

    if (photo == 0 || !photo->has_photo) {
        return;
    }
    tg_gui_photo_store_stripped(photo);
    old = tg_gui_photo_catalog_find(photo->id_hi, photo->id_lo);
    if (old != 0) {
        *old = *photo;
        return;
    }
    if (tg_gui_photo_catalog_count >= TG_GUI_PHOTO_CATALOG_MAX) {
        for (i = 1; i < TG_GUI_PHOTO_CATALOG_MAX; ++i) {
            tg_gui_photo_catalog[i - 1] = tg_gui_photo_catalog[i];
        }
        tg_gui_photo_catalog_count = TG_GUI_PHOTO_CATALOG_MAX - 1;
    }
    tg_gui_photo_catalog[tg_gui_photo_catalog_count++] = *photo;
}

static int tg_gui_photo_queue_offer(const tg_mtproto_photo_meta *source,
                                    int large)
{
    tg_gui_photo_queue_entry entry;

    if (source == 0 || !source->has_photo || tg_gui_photo_cache_paused ||
        (!large && !tg_gui_photo_inline_enabled)) {
        return 0;
    }
    if (!tg_gui_photo_prepare_queue_entry(source, large, &entry)) {
        return 0;
    }
    return tg_gui_photo_queue_insert(&entry);
}

/* 0.0.92: is there still any chance of drawing this image? False once every
   variant has been fetched and refused, which is what an undecodable format
   looks like from here. A bubble that keeps reserving space for a picture the
   client has given up on shows the user an empty rectangle for ever, and no
   later paint will ever fill it. The scratch entry is static because it is a
   whole photo record and this runs inside the per-message loop. */
static int tg_gui_photo_still_possible(const tg_mtproto_photo_meta *photo)
{
    static tg_gui_photo_queue_entry probe;

    if (photo == 0 || !photo->has_photo) {
        return 0;
    }
    return tg_gui_photo_prepare_queue_entry(photo, 0, &probe);
}

int tg_gui_session_photo_pending(void)
{
    return !tg_gui_photo_cache_paused &&
           (tg_gui_photo_fetch.active || tg_gui_photo_queue_count > 0);
}

int tg_gui_session_request_inline_photo(unsigned long id_hi,
                                        unsigned long id_lo)
{
    tg_mtproto_photo_meta *photo;
    int full_ready;
    int preview_ready;

    if (!tg_gui_photo_inline_enabled) {
        return 0;
    }
    full_ready = tg_gui_photo_cache_exists(id_hi, id_lo, 0) ||
                 tg_gui_photo_canonical_cache_exists(id_hi, id_lo, 0);
    preview_ready = tg_gui_photo_thumb_cache_exists(id_hi, id_lo);
    photo = tg_gui_photo_catalog_find(id_hi, id_lo);
    if (!full_ready && photo != 0) {
        tg_gui_photo_queue_offer(photo, 0);
    }
    return full_ready ? 2 : (preview_ready ? 1 : 0);
}

int tg_gui_session_request_viewer_photo(unsigned long id_hi,
                                        unsigned long id_lo,
                                        unsigned long *source_w,
                                        unsigned long *source_h)
{
    tg_mtproto_photo_meta *photo;

    photo = tg_gui_photo_catalog_find(id_hi, id_lo);
    if (photo == 0) {
        /* A cached large JPEG remains useful after the bounded per-chat
           metadata catalog has rotated. Keep the caller's dimensions in that
           case: the viewer can still decode and fit the cached image. */
        return (tg_gui_photo_cache_exists(id_hi, id_lo, 1) ||
                tg_gui_photo_canonical_cache_exists(id_hi, id_lo, 1))
                   ? 0 : 1;
    }
    if (source_w != 0) {
        *source_w = photo->has_large ? photo->large_width : photo->width;
    }
    if (source_h != 0) {
        *source_h = photo->has_large ? photo->large_height : photo->height;
    }
    if (!tg_gui_photo_cache_exists(id_hi, id_lo, 1) &&
        !tg_gui_photo_canonical_cache_exists(id_hi, id_lo, 1)) {
        tg_gui_photo_queue_offer(photo, 1);
    }
    return 0;
}

int tg_gui_session_request_photo_jpeg(unsigned long id_hi,
                                      unsigned long id_lo, int large)
{
    tg_mtproto_photo_meta *photo;
    tg_gui_photo_queue_entry entry;
    int i;

    large = large ? 1 : 0;
    if (tg_gui_photo_cache_exists(id_hi, id_lo, large)) {
        return 2;
    }
    if (tg_gui_photo_fetch.active && tg_gui_photo_fetch.large == large &&
        tg_gui_photo_fetch.photo.id_hi == id_hi &&
        tg_gui_photo_fetch.photo.id_lo == id_lo) {
        return 1;
    }
    for (i = 0; i < tg_gui_photo_queue_count; ++i) {
        if (tg_gui_photo_queue[i].large == large &&
            tg_gui_photo_queue[i].photo.id_hi == id_hi &&
            tg_gui_photo_queue[i].photo.id_lo == id_lo) {
            tg_gui_photo_queue[i].require_jpeg = 1;
            return 1;
        }
    }
    photo = tg_gui_photo_catalog_find(id_hi, id_lo);
    if (photo == 0 || tg_gui_photo_cache_paused ||
        !tg_gui_photo_prepare_queue_entry(photo, large, &entry)) {
        return 0;
    }
    entry.require_jpeg = 1;
    return tg_gui_photo_queue_insert(&entry) ? 1 : 0;
}

int tg_gui_session_photo_fetch_progress(unsigned long id_hi,
                                        unsigned long id_lo, int large,
                                        unsigned long *done,
                                        unsigned long *total)
{
    int i;

    if (done != 0) {
        *done = 0UL;
    }
    if (total != 0) {
        *total = 0UL;
    }
    large = large ? 1 : 0;
    if (tg_gui_photo_fetch.active && tg_gui_photo_fetch.large == large &&
        tg_gui_photo_fetch.photo.id_hi == id_hi &&
        tg_gui_photo_fetch.photo.id_lo == id_lo) {
        if (done != 0) {
            *done = tg_gui_photo_fetch.offset;
        }
        if (total != 0) {
            *total = tg_gui_photo_fetch.photo.size;
        }
        return 1;
    }
    for (i = 0; i < tg_gui_photo_queue_count; ++i) {
        if (tg_gui_photo_queue[i].large == large &&
            tg_gui_photo_queue[i].photo.id_hi == id_hi &&
            tg_gui_photo_queue[i].photo.id_lo == id_lo) {
            if (total != 0) {
                *total = tg_gui_photo_queue[i].photo.size;
            }
            return 1;
        }
    }
    return 0;
}

void tg_gui_session_set_inline_photos(int enabled)
{
    int i;
    int kept;

    tg_gui_photo_inline_enabled = enabled ? 1 : 0;
    if (tg_gui_photo_inline_enabled) {
        return;
    }
    kept = 0;
    for (i = 0; i < tg_gui_photo_queue_count; ++i) {
        if (tg_gui_photo_queue[i].large) {
            tg_gui_photo_queue[kept++] = tg_gui_photo_queue[i];
        }
    }
    tg_gui_photo_queue_count = kept;
    if (tg_gui_photo_fetch.active && !tg_gui_photo_fetch.large) {
        if (tg_gui_photo_fetch.out != 0) {
            fclose(tg_gui_photo_fetch.out);
        }
        if (tg_gui_photo_fetch.part_path[0] != '\0') {
            (void)remove(tg_gui_photo_fetch.part_path);
            tg_gui_window_photo_cache_file_removed(
                tg_gui_photo_fetch.part_path);
        }
        if (tg_gui_photo_fetch.quiet != 0) {
            tg_mtproto_close_quiet_stream(tg_gui_photo_fetch.quiet,
                                          tg_gui_photo_fetch.stream);
        }
        memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
    }
}

void tg_gui_session_photo_cache_clear_prepare(void)
{
    tg_gui_photo_cache_paused = 1;
    if (tg_gui_photo_fetch.out != 0) {
        fclose(tg_gui_photo_fetch.out);
        tg_gui_photo_fetch.out = 0;
    }
    if (tg_gui_photo_fetch.part_path[0] != '\0') {
        (void)remove(tg_gui_photo_fetch.part_path);
        tg_gui_window_photo_cache_file_removed(tg_gui_photo_fetch.part_path);
    }
    if (tg_gui_photo_fetch.quiet != 0) {
        tg_mtproto_close_quiet_stream(tg_gui_photo_fetch.quiet,
                                      tg_gui_photo_fetch.stream);
    }
    memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
    tg_gui_photo_queue_count = 0;
    tg_gui_photo_tried_count = 0;
    tg_gui_photo_cache_variant_count = 0;
}

void tg_gui_session_photo_cache_clear_finish(void)
{
    tg_gui_photo_cache_paused = 0;
}

static void tg_gui_photo_queue_reset(void)
{
    if (tg_gui_photo_fetch.out != 0) {
        fclose(tg_gui_photo_fetch.out);
        tg_gui_photo_fetch.out = 0;
    }
    if (tg_gui_photo_fetch.part_path[0] != '\0') {
        (void)remove(tg_gui_photo_fetch.part_path);
        tg_gui_window_photo_cache_file_removed(tg_gui_photo_fetch.part_path);
    }
    if (tg_gui_photo_fetch.quiet != 0) {
        tg_mtproto_close_quiet_stream(tg_gui_photo_fetch.quiet,
                                      tg_gui_photo_fetch.stream);
    }
    memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
    tg_gui_photo_queue_count = 0;
    tg_gui_photo_tried_count = 0;
    tg_gui_photo_cache_variant_count = 0;
    tg_gui_photo_catalog_count = 0;
    tg_gui_photo_cache_paused = 0;
}

static int tg_mtproto_auth_print_history_text_peer_on_context(
    const char *host,
    const char *port,
    const char *api_id,
    const char *auth_file,
    const char *dc_id_text,
    tg_mtproto_auth_context *context,
    const char *peer_cache_file,
    const char *peer_index_text,
    const char *limit_text,
    FILE *stream,
    unsigned long *last_seen_message_id,
    unsigned long *printed_message_count,
    int only_new,
    int include_outgoing,
    int print_empty_status,
    const char *peer_label,
    const char *own_label)
{
    unsigned char query[64];
    unsigned long limit;
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    int is_group;
    int query_rc;
    unsigned long i;
    unsigned long max_seen_message_id;
    unsigned long printed;
    unsigned long k;
    FILE *quiet;
    static tg_mtproto_message_text_list texts;
    static tg_mtproto_peer_cache sender_cache;
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    static const char label[] = "mtproto messages.getHistory(peer)";

    if (printed_message_count != 0) {
        *printed_message_count = 0UL;
    }
    if (stream == 0 || tg_mtproto_parse_ulong_arg(limit_text, &limit) != 0 ||
        limit == 0UL || limit > 100UL) {
        return 2;
    }

    quiet = tg_mtproto_open_quiet_stream(stream);
    if (tg_mtproto_load_peer_cache_peer(peer_cache_file, peer_index_text,
                                        &peer_constructor, &peer_id_hi,
                                        &peer_id_lo, &access_hash_hi,
                                        &access_hash_lo, &has_access_hash,
                                        quiet, label) != 0) {
        /* These fail lines otherwise die inside this function's own quiet
           stream: name them in the crash-safe log so a field report says WHY
           a chat opened empty (--gui-live-debug only, like every probe). */
        tg_gui_log("hist: peer load fail");
        tg_mtproto_close_quiet_stream(quiet, stream);
        return 2;
    }
    is_group = (peer_constructor == TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ||
                peer_constructor == TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR);
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_peer(
            &writer, peer_constructor, peer_id_hi, peer_id_lo,
            access_hash_hi, access_hash_lo, has_access_hash,
            tg_mtproto_history_offset_id_override, limit) !=
        TG_MTPROTO_TL_OK) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return 2;
    }
    /* Propagate the real query result (SOFT_FAIL vs hard 2) instead of
       collapsing to 2, so the chat shows "slow link" (timeout) vs "error N".
       A higher receive cap lets one read drain more of a heavy account's
       pending-update backlog before the time budget is spent. */
    query_rc = tg_mtproto_send_saved_query_on_context(
        host, port, api_id, auth_file, dc_id_text, context, query,
        writer.length, &result, quiet, label, 600U);
    if (query_rc != 0) {
        char qline[40];

        sprintf(qline, "hist: query fail rc=%d", query_rc);
        tg_gui_log(qline);
        tg_mtproto_close_quiet_stream(quiet, stream);
        return query_rc;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        long ecode;
        char emsg[48];
        char eline[96];

        (void)tg_mtproto_print_rpc_error(label, &result, quiet);
        ecode = 0L;
        emsg[0] = '\0';
        (void)tg_mtproto_parse_rpc_error(result.result_body - 4U,
                                         result.result_body_length + 4U,
                                         &ecode, emsg, sizeof(emsg));
        sprintf(eline, "hist: rpc error %ld %.48s", ecode, emsg);
        tg_gui_log(eline);
        tg_mtproto_close_quiet_stream(quiet, stream);
        return 2;
    }
    if (tg_mtproto_unpack_gzip_result(&result, quiet, label) != 0) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return 2;
    }
    if (tg_mtproto_parse_message_text_list(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &texts) != TG_MTPROTO_TL_OK) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return 2;
    }
#ifdef TG_MTPROTO_DIAG
    fprintf(stream,
            "%s: diag result=0x%08lx body=%lu count=%lu total=%lu abort=0x%08lx\n",
            label, result.result_constructor, result.result_body_length,
            texts.count, texts.total_message_count, texts.abort_constructor);
    fflush(stream);
#endif
    /* Diagnostic for the "only 2-3 messages at open" reports: kept = text rows
       displayed, total = messages in the fetched page, abort = the TL constructor
       that stopped the parser (0 = clean; 0x7a800e0a = messageService;
       0x9815cec8 = a message whose media/reply field could not be read). Reaches
       the disk log only under --gui-live-debug, plus the kernel-debug channel. */
    {
        char histdiag[208];
        sprintf(histdiag,
                "hist[%s]: kept=%lu page=%lu total=%lu trunc=%d abort=0x%08lx "
                "resync=%lu/%lu body=%lu",
                label, texts.count, texts.page_count, texts.total_message_count,
                texts.truncated, texts.abort_constructor, texts.resync_ok,
                texts.resync_attempts, result.result_body_length);
        tg_gui_log(histdiag);
    }
    tg_gui_last_hist_total = texts.total_message_count;
    tg_mtproto_close_quiet_stream(quiet, stream);

    /* Resolve group-message senders from the response's users/chats. */
    tg_mtproto_parse_message_peers(result.result_body,
                                   result.result_body_length, &sender_cache);

    max_seen_message_id = last_seen_message_id != 0 ?
        *last_seen_message_id : 0UL;
    printed = 0UL;
    if (texts.count == 0UL) {
        if (print_empty_status) {
            fprintf(stream, "history refreshed\n");
        }
        return 0;
    }
    i = texts.count;
    {
        tg_chat_console_driver console_drv;
        tg_chat_driver driver;
        long display_delta;

        /* The history renderer hands each resolved row to a driver: the console
           one (wrapping this stream + the day-separator cursor) by default, or
           the GUI driver when a session has installed an override. */
        if (tg_chat_message_driver_override != 0) {
            driver = *tg_chat_message_driver_override;
        } else {
            console_drv.stream = stream;
            console_drv.day_shown = &tg_chat_day_shown;
            driver.ctx = &console_drv;
            driver.on_message = tg_chat_console_on_message;
            driver.on_chat_list_changed = 0; /* console list via render_console */
            driver.on_notification = 0;
        }
        /* Anchor message times on the real local wall clock instead of time():
           the server delta is server-UTC minus time(), but on clib2 time() is
           already (locale-offset) UTC, so that delta loses the timezone and any
           DST gap leaks through as an "off by an hour" display. Subtracting the
           wall-clock-vs-time() skew rebases the delta on the Workbench clock. */
        display_delta = context->server_time_delta_seconds
                      - tg_mtproto_local_clock_skew();
        while (i > 0UL) {
            --i;
            if (texts.messages[i].id > max_seen_message_id) {
                max_seen_message_id = texts.messages[i].id;
            }
            if (last_seen_message_id != 0 && only_new &&
                texts.messages[i].id <= *last_seen_message_id) {
                continue;
            }
            if (!include_outgoing && texts.messages[i].is_out) {
                continue;
            }
            /* In a group/channel prefix every line with the (truncated) chat
               title so the user keeps both the group and the sender in view.
               1:1 chats skip the prefix: there the peer already is the sender.
               The sender name is resolved here (fetch/parse stays in the loop);
               the resolved row is then handed to the driver. */
            {
                const char *sender = 0;
                tg_chat_message_row row;

                memset(&row, 0, sizeof(row));

                if (!texts.messages[i].is_out &&
                    texts.messages[i].from_constructor != 0UL) {
                    for (k = 0UL; k < sender_cache.count; ++k) {
                        if (sender_cache.entries[k].peer_constructor ==
                                texts.messages[i].from_constructor &&
                            sender_cache.entries[k].id_hi ==
                                texts.messages[i].from_id_hi &&
                            sender_cache.entries[k].id_lo ==
                                texts.messages[i].from_id_lo) {
                            if (sender_cache.entries[k].title[0] != '\0') {
                                sender = sender_cache.entries[k].title;
                            } else if (sender_cache.entries[k].username[0] !=
                                       '\0') {
                                sender = sender_cache.entries[k].username;
                            }
                            break;
                        }
                    }
                }
                row.text = texts.messages[i].text;
                row.has_time = (texts.messages[i].date != 0UL);
                row.local_epoch =
                    row.has_time
                        ? tg_mtproto_chat_local_epoch(
                              texts.messages[i].date,
                              display_delta)
                        : 0UL;
                row.is_out = texts.messages[i].is_out;
                row.is_group = is_group;
                row.peer_label = peer_label;
                row.own_label = own_label;
                row.sender = sender;
                /* Show the server's inline quote when the reply carries one (no
                   extra round-trip); else no reference line. */
                row.reply_quote =
                    (texts.messages[i].has_reply &&
                     texts.messages[i].reply_quote[0] != '\0')
                        ? texts.messages[i].reply_quote
                        : 0;
                row.id = texts.messages[i].id;
                row.from_id_hi = texts.messages[i].from_id_hi;
                row.from_id_lo = texts.messages[i].from_id_lo;
                row.has_document = texts.messages[i].document.has_document;
                row.has_photo = texts.messages[i].photo.has_photo;
                row.photo_only = texts.messages[i].photo_only;
                row.photo_id_hi = texts.messages[i].photo.id_hi;
                row.photo_id_lo = texts.messages[i].photo.id_lo;
                row.photo_width = texts.messages[i].photo.width;
                row.photo_height = texts.messages[i].photo.height;
                /* 0.0.92 field diagnostic: a document that carries no
                   usable thumbnail and one whose thumbnail simply never
                   arrives look identical on screen (label, empty space), so
                   say here what the parser actually found. One line per
                   document, only with the debug log on. */
                if (row.has_document && tg_gui_log_is_enabled()) {
                    char dline[128];

                    sprintf(dline,
                            "doc: kind=%u attrs=%lu mime=%.24s thumb=%s "
                            "%lux%lu %lu b stripped=%lu has_photo=%d "
                            "from_doc=%d",
                            (unsigned)texts.messages[i].document.kind,
                            texts.messages[i].document.attr_seen,
                            texts.messages[i].document.mime[0] != '\0'
                                ? texts.messages[i].document.mime : "-",
                            texts.messages[i].photo.thumb_type[0] != '\0'
                                ? texts.messages[i].photo.thumb_type : "-",
                            texts.messages[i].photo.width,
                            texts.messages[i].photo.height,
                            texts.messages[i].photo.size,
                            texts.messages[i].photo.stripped_len,
                            texts.messages[i].photo.has_photo,
                            texts.messages[i].photo.from_document);
                    tg_gui_log(dline);
                }
                if (row.has_photo) {
                    row.photo_ready = tg_gui_photo_cache_exists(
                        row.photo_id_hi, row.photo_id_lo, 0);
                    /* Nothing cached and nothing left to try: the bubble
                       stops holding space open for it and falls back to its
                       text, rather than showing an empty frame for ever. */
                    if (!row.photo_ready &&
                        !tg_gui_photo_still_possible(
                            &texts.messages[i].photo)) {
                        row.has_photo = 0;
                        row.photo_only = 0;
                    }
                }
                if (row.has_photo) {
                    if (tg_chat_message_driver_override != 0) {
                        /* Keep credentials for a later visible-only inline
                           request or an explicit viewer click. Parsing alone
                           never starts background photo work. */
                        tg_gui_photo_catalog_offer(&texts.messages[i].photo);
                    }
                }
                driver.on_message(driver.ctx, &row);
            }
            ++printed;
        }
    }
    if (last_seen_message_id != 0) {
        *last_seen_message_id = max_seen_message_id;
    }
    if (printed_message_count != 0) {
        *printed_message_count = printed;
    }
    if (printed == 0UL && print_empty_status) {
        fprintf(stream, "history refreshed\n");
    }
    return 0;
}

static void tg_mtproto_chat_print_input_prompt(FILE *stream,
                                               const char *own_label,
                                               const char *peer_label)
{
    if (stream == 0) {
        return;
    }
    tg_console_ui_role(stream, TG_UI_ROLE_PROMPT);
    if (peer_label != 0 && peer_label[0] != '\0') {
        /* Truncate the bracketed chat name the same way the message lines do,
           so a long group title does not push the typing position far right. */
        fprintf(stream, "[");
        tg_mtproto_print_label_truncated(stream, peer_label,
                                         TG_MTPROTO_GROUP_LABEL_MAX);
        fprintf(stream, "] ");
    }
    if (own_label != 0 && own_label[0] != '\0') {
        tg_mtproto_print_cache_text(stream, own_label);
    } else {
        fprintf(stream, "me");
    }
    fprintf(stream, ":");
    tg_console_ui_reset(stream);
    fputc(' ', stream);
    fflush(stream);
}

/* One status/info line in the system colour. */
static void tg_mtproto_chat_print_system_line(FILE *stream, const char *text)
{
    char line[256];

    if (stream == 0 || text == 0) {
        return;
    }
    if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
        sprintf(line, "%.16s%.200s%.16s",
                tg_console_ui_role_string(TG_UI_ROLE_SYSTEM), text,
                tg_console_ui_role_string(TG_UI_ROLE_RESET));
        tg_console_tui_line(tg_chat_tui_stream, line);
        return;
    }
    tg_console_ui_role(stream, TG_UI_ROLE_SYSTEM);
    fputs(text, stream);
    tg_console_ui_reset(stream);
    tg_console_ui_end_line(stream);
    fflush(stream);
}

/*
 * Maps a server-side (UTC) message date into the machine's wall-clock frame by
 * subtracting a delta. Callers pass the DISPLAY delta = (server UTC minus the
 * local wall clock), i.e. the protocol's server-UTC-minus-time() delta rebased
 * onto the real Amiga clock via tg_mtproto_local_clock_skew(); see the render
 * loop. Rendering then uses gmtime so no host timezone is applied a second time.
 * (Anchoring on the raw wall clock rather than time() is what keeps clib2/OS3 --
 * whose time() returns locale-offset UTC with no DST -- on the system clock.)
 */
static unsigned long tg_mtproto_chat_local_epoch(unsigned long message_date,
                                                 long server_delta)
{
    if (server_delta > 0L && (unsigned long)server_delta < message_date) {
        return message_date - (unsigned long)server_delta;
    }
    if (server_delta < 0L) {
        return message_date + (unsigned long)(0L - server_delta);
    }
    return message_date;
}

/* "[HH:MM] " prefix for one transcript line, in the quiet context colour. */
static void tg_mtproto_chat_print_message_time(FILE *stream,
                                               unsigned long local_epoch)
{
    time_t when;
    struct tm *parts;

    when = (time_t)local_epoch;
    parts = gmtime(&when);
    if (parts == 0) {
        return;
    }
    tg_console_ui_role(stream, TG_UI_ROLE_GROUP);
    fprintf(stream, "[%02d:%02d]", parts->tm_hour, parts->tm_min);
    tg_console_ui_reset(stream);
    fputc(' ', stream);
}

/* "--- 10 Jun ---" separator when the transcript crosses a day boundary. */
static void tg_mtproto_chat_print_day_separator(FILE *stream,
                                                unsigned long local_epoch)
{
    static const char *month_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    time_t when;
    struct tm *parts;

    when = (time_t)local_epoch;
    parts = gmtime(&when);
    if (parts == 0 || parts->tm_mon < 0 || parts->tm_mon > 11) {
        return;
    }
    tg_console_ui_role(stream, TG_UI_ROLE_SYSTEM);
    fprintf(stream, "--- %d %s ---", parts->tm_mday,
            month_names[parts->tm_mon]);
    tg_console_ui_reset(stream);
    tg_console_ui_end_line(stream);
}

/* Clear the prompt line before printing asynchronous output (auto-read
   results, later cross-chat notifications). In raw mode the cursor sits on
   the prompt line, possibly mid-word: return to column 0 and erase to end of
   line so the transcript prints where the prompt was, with no stale prompt
   stacking up. Cooked consoles cannot redraw, so just move to a fresh line. */
static void tg_mtproto_chat_clear_input_line(FILE *stream, int raw)
{
    if (stream == 0 || tg_console_tui_active()) {
        return;
    }
    /* The CSI-deaf console (one AROS icon-console type) would print the
       erase sequence as stray glyphs before every async line; ask the
       mini-termcap instead of guessing from colour state. */
    if (raw &&
        tg_console_caps_get()->csi_output != TG_UI_CSI_OUTPUT_DEAF) {
        fputs("\r" TG_UI_CSI "K", stream);
    } else {
        fputc('\n', stream);
    }
}

/* Reprint the prompt plus whatever the user had already typed. Raw mode does
   its own echo, so the half-typed line can be restored after async output;
   in cooked mode the console owns the echo and only the prompt is printed. */
static void tg_mtproto_chat_redraw_input(FILE *stream,
                                         const char *own_label,
                                         const char *peer_label,
                                         const char *line,
                                         unsigned long line_length,
                                         int raw)
{
    /* Direct printer here: tg_mtproto_chat_show_prompt delegates to this
       function in linear mode, so calling it back would recurse forever
       (field guru on AROS: stack out of range at show_prompt). */
    tg_mtproto_chat_print_input_prompt(stream, own_label, peer_label);
    if (raw && line != 0 && line_length > 0UL) {
        fwrite(line, 1, (size_t)line_length, stream);
        fflush(stream);
    }
}

/* Shows the input prompt in the current mode: in the TUI it caches the
   rendered prompt and redraws the fixed input row (with any pending text);
   in linear mode it prints the prompt (and re-echoes the pending text when
   raw, matching the old redraw behaviour). */
static void tg_mtproto_chat_show_prompt(FILE *stream,
                                        const char *own_label,
                                        const char *peer_label,
                                        const char *pending,
                                        unsigned long pending_length,
                                        int raw)
{
    char prompt_text[96];
    FILE *capture;
    unsigned long length;
    int ch;

    if (tg_console_tui_active() && tg_chat_tui_stream != 0) {
        capture = tmpfile();
        if (capture != 0) {
            tg_mtproto_chat_print_input_prompt(capture, own_label,
                                               peer_label);
            rewind(capture);
            length = 0UL;
            for (;;) {
                ch = fgetc(capture);
                if (ch == EOF || ch == '\n') {
                    break;
                }
                if (length + 1UL < sizeof(prompt_text)) {
                    prompt_text[length] = (char)ch;
                    ++length;
                }
            }
            prompt_text[length] = '\0';
            fclose(capture);
            tg_console_tui_set_prompt(prompt_text);
        }
        tg_console_tui_input_caret(
            tg_chat_tui_stream, tg_console_tui_prompt(), pending,
            pending_length, tg_chat_caret);
        return;
    }
    tg_mtproto_chat_redraw_input(stream, own_label, peer_label, pending,
                                 pending_length, raw);
}

/* Updates the TUI status bar with the open chat's name. */
/* Renders cached UTF-8 text into the display charset (same mapping as
   tg_mtproto_print_cache_text) for callers that need a string, not a
   stream -- e.g. the status bar, which printed accents as mojibake. */
static void tg_mtproto_cache_text_to_display(const char *text,
                                             char *out,
                                             unsigned long out_size)
{
    FILE *tmp;
    unsigned long n;
    int ch;

    if (out == 0 || out_size == 0UL) {
        return;
    }
    out[0] = '\0';
    if (text == 0) {
        return;
    }
    tmp = tmpfile();
    if (tmp == 0) {
        /* Best effort: the raw text (worst case shows mojibake again). */
        n = 0UL;
        while (text[n] != '\0' && n + 1UL < out_size) {
            out[n] = text[n];
            ++n;
        }
        out[n] = '\0';
        return;
    }
    tg_mtproto_print_cache_text(tmp, text);
    rewind(tmp);
    n = 0UL;
    while (n + 1UL < out_size && (ch = fgetc(tmp)) != EOF) {
        out[n++] = (char)ch;
    }
    out[n] = '\0';
    fclose(tmp);
}

static void tg_mtproto_chat_tui_status(const char *peer_label)
{
    char shown[64];
    char status[96];

    if (!tg_console_tui_active() || tg_chat_tui_stream == 0) {
        return;
    }
    if (peer_label != 0 && peer_label[0] != '\0') {
        tg_mtproto_cache_text_to_display(peer_label, shown, sizeof(shown));
        sprintf(status, " Telegram Amiga - %.60s ", shown);
    } else {
        sprintf(status, " Telegram Amiga ");
    }
    tg_console_tui_status(tg_chat_tui_stream, status);
}

/* Finds the public chat-list index (and label) of a cached peer id. */
static int tg_mtproto_peer_cache_find_by_id(const char *path,
                                            unsigned long id_hi,
                                            unsigned long id_lo,
                                            unsigned long *out_index,
                                            char *label_buffer,
                                            unsigned long label_buffer_size)
{
    FILE *file;
    char line[512];
    char type[24];
    unsigned long index;
    unsigned long hi;
    unsigned long lo;
    char *title;
    char *username;

    if (out_index != 0) {
        *out_index = 0UL;
    }
    if (label_buffer != 0 && label_buffer_size > 0UL) {
        label_buffer[0] = '\0';
    }
    if (path == 0 || out_index == 0 || label_buffer == 0 ||
        label_buffer_size == 0UL) {
        return 2;
    }
    file = fopen(path, "r");
    if (file == 0) {
        return 2;
    }
    while (fgets(line, sizeof(line), file) != 0) {
        index = 0UL;
        type[0] = '\0';
        hi = 0UL;
        lo = 0UL;
        if (sscanf(line, "peer %lu type %23s id 0x%8lx%8lx", &index, type,
                   &hi, &lo) < 4) {
            continue;
        }
        if (hi != id_hi || lo != id_lo) {
            continue;
        }
        title = strstr(line, " title ");
        username = strstr(line, " username ");
        if (title != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        title + 7, 0);
        }
        if (label_buffer[0] == '\0' && username != 0) {
            tg_mtproto_copy_cache_field(label_buffer, label_buffer_size,
                                        username + 10, title);
        }
        *out_index = index;
        fclose(file);
        return 0;
    }
    fclose(file);
    return 2;
}

/* Drains the cross-chat notification queue: one inverse-video line per
   message from a chat other than the open one, prefixed with the chat-list
   number so an F-key or a bare number jumps straight there. */
static void tg_mtproto_chat_print_notify_lines(FILE *stream,
                                               const char *peer_cache_file,
                                               const char *current_index_text)
{
    char label[128];
    unsigned long current_index;
    unsigned long index;
    unsigned long i;
    const char *digits;
    const tg_chat_notify_entry *entry;

    if (tg_chat_nq == 0) {
        return;
    }
    if (stream == 0 || tg_chat_nq->count == 0UL) {
        tg_chat_nq->count = 0UL;
        tg_chat_nq->dropped = 0UL;
        return;
    }
    current_index = 0UL;
    if (current_index_text != 0) {
        digits = current_index_text;
        while (*digits >= '0' && *digits <= '9') {
            current_index = (current_index * 10UL) +
                            (unsigned long)(*digits - '0');
            ++digits;
        }
        if (*digits != '\0') {
            current_index = 0UL;
        }
    }
    for (i = 0UL; i < tg_chat_nq->count; ++i) {
        entry = &tg_chat_nq->queue[i];
        index = 0UL;
        label[0] = '\0';
        (void)tg_mtproto_peer_cache_find_by_id(peer_cache_file,
                                               entry->peer_id_hi,
                                               entry->peer_id_lo, &index,
                                               label, sizeof(label));
        if (current_index != 0UL && index == current_index) {
            /* The open chat: the normal auto-read already shows it. */
            continue;
        }
        if (label[0] == '\0' &&
            (entry->from_id_hi != 0UL || entry->from_id_lo != 0UL) &&
            (entry->from_id_hi != entry->peer_id_hi ||
             entry->from_id_lo != entry->peer_id_lo)) {
            /* Chat not cached yet (e.g. stale peer list): fall back to the
               sender's name, which often is. */
            unsigned long sender_index;

            (void)tg_mtproto_peer_cache_find_by_id(peer_cache_file,
                                                   entry->from_id_hi,
                                                   entry->from_id_lo,
                                                   &sender_index, label,
                                                   sizeof(label));
        }
        tg_console_ui_role(stream, TG_UI_ROLE_NOTIFY);
        if (index != 0UL) {
            fprintf(stream, "[%lu] ", index);
        } else {
            fputs("[+] ", stream);
        }
        if (label[0] != '\0') {
            tg_mtproto_print_cache_text(stream, label);
            if (entry->is_chat) {
                fputs(" (group)", stream);
            }
        } else {
            fputs(entry->is_chat ? "group message" : "new message", stream);
        }
        fputs(": ", stream);
        tg_mtproto_print_cache_text(stream, entry->text);
        tg_console_ui_reset(stream);
        tg_console_ui_end_line(stream);
    }
    if (tg_chat_nq->dropped > 0UL) {
        tg_console_ui_role(stream, TG_UI_ROLE_NOTIFY);
        fprintf(stream, "(+%lu more)", tg_chat_nq->dropped);
        tg_console_ui_reset(stream);
        tg_console_ui_end_line(stream);
    }
    if (tg_chat_bell_enabled) {
        /* Intuition DisplayBeep, not a BEL byte: console handlers improvise
           on BEL (AmiKit's replacement console clears the window, one AROS
           icon console draws a stray glyph). The flash never touches the
           console stream. */
        tg_platform_display_beep();
    }
    tg_chat_nq->count = 0UL;
    tg_chat_nq->dropped = 0UL;
    fflush(stream);
}

static void tg_mtproto_chat_print_help(FILE *stream)
{
    static const char *help_lines[] = {
        "",
        "Commands:",
        "  text          send a message",
        "  Enter         read new messages now",
        "  number        switch to chat number",
        "  F1..F10       switch to chat 1..10 (shift: 11..20)",
        "  Tab           back to the previous chat",
        "  Shift+Up/Down page through older messages (and back to live)",
        "  /swap         back to the previous chat",
        "  /peers        show cached chats",
        "  /search text  find cached chats by name or username",
        "  /add name     search Telegram and add a chat",
        "  /remove n     remove cached chat n",
        "  /history      show recent messages without new-message filtering",
        "  /forward [id] forward latest (or given id) to Saved Messages",
        "  /forwardto n [id] forward latest (or given id) to chat n",
        "  /getfile      download the newest file in this chat to downloads/",
        "  /sendfile p   send a file (large files supported; build limit applies)",
        "  /photo p      send a JPEG as a photo (over 10 MiB becomes a file)",
        "  /saved        open Saved Messages (your cloud transfer drawer)",
        "  /watch sec    set auto-read interval",
        "  /watch off    disable auto-read",
        "  /resize       redraw the layout after a window resize",
        "  /diff         toggle background catch-up (or /diff on|off)",
        "  /color        toggle colours (or /color on|off)",
        "  /bell         toggle the notification flash/bell",
        "  /help         show this help",
        "  /quit         exit",
        0
    };
    unsigned long i;

    if (stream == 0) {
        return;
    }
    tg_console_ui_role(stream, TG_UI_ROLE_SYSTEM);
    for (i = 0UL; help_lines[i] != 0; ++i) {
        fputs(help_lines[i], stream);
        tg_console_ui_end_line(stream);
    }
    /* Live status of the console drag-and-drop (the startup print is wiped by
       the TUI screen clear, so /help is where a field report can read WHY a
       drop does nothing). */
    fprintf(stream, "  file drag-and-drop: %s",
            tg_platform_console_drop_diag());
    tg_console_ui_end_line(stream);
    tg_console_ui_reset(stream);
    tg_console_ui_end_line(stream);
}

static int tg_mtproto_chat_open_history(FILE *stream,
                                        FILE *quiet,
                                        const char *host,
                                        const char *port,
                                        const char *api_id,
                                        const char *auth_file,
                                        const char *dc_id_text,
                                        tg_mtproto_auth_context *context,
                                        const char *peer_cache_file,
                                        const char *peer_index,
                                        const char *peer_label,
                                        const char *own_label,
                                        unsigned long *last_seen_message_id)
{
    unsigned int attempt;
    unsigned long requested_last_seen_message_id;
    unsigned long printed_message_count;
    long quiet_length;
    int rc;
    FILE *note;

    if (stream == 0 || quiet == 0 || peer_index == 0 ||
        peer_index[0] == '\0' || last_seen_message_id == 0) {
        return 0;
    }

    /* In the full-screen layout the progress note and the failure notices
       must travel through the transcript region; a direct print would land
       on the pinned input row and scroll the whole window. */
    note = tg_console_tui_capture_begin(stream);
    fprintf(note, "Opening chat");
    fflush(note);
    for (attempt = 0; attempt < TG_MTPROTO_CHAT_OPEN_HISTORY_ATTEMPTS;
         ++attempt) {
        if (attempt > 0U) {
            fprintf(note, ".");
            fflush(note);
        }
        requested_last_seen_message_id = 0UL;
        printed_message_count = 0UL;
        tg_mtproto_reset_quiet_stream(quiet, stream);
        rc = tg_mtproto_auth_print_history_text_peer_on_context(
            host, port, api_id, auth_file, dc_id_text, context,
            peer_cache_file, peer_index, "5", quiet,
            &requested_last_seen_message_id, &printed_message_count,
            0, 1, 0, peer_label, own_label);
        if (rc == 0) {
            *last_seen_message_id = requested_last_seen_message_id;
            quiet_length = tg_mtproto_quiet_stream_length(quiet, stream);
            fprintf(note, "\n");
            tg_console_tui_capture_end(note, stream);
            if (printed_message_count > 0UL &&
                (quiet == stream || quiet_length > 0L)) {
                tg_mtproto_replay_quiet_stream_length(
                    quiet, stream, quiet_length);
            }
            return 0;
        }
        tg_mtproto_close_auth_context(context);
    }
    fprintf(note, "\n");
    if (rc == TG_MTPROTO_QUERY_SOFT_FAIL) {
        fprintf(note,
                "No reply yet (slow link). Press Enter to retry reading.\n");
    } else {
        fprintf(note, "Could not load recent messages now (error %d).\n", rc);
    }
    tg_console_tui_capture_end(note, stream);
    return rc;
}

static void tg_mtproto_chat_load_own_label(const char *host,
                                           const char *port,
                                           const char *api_id,
                                           const char *auth_file,
                                           const char *dc_id_text,
                                           tg_mtproto_auth_context *context,
                                           const char *peer_cache_file,
                                           char *own_label,
                                           unsigned long own_label_size,
                                           FILE *stream)
{
    FILE *quiet;

    if (own_label == 0 || own_label_size == 0UL) {
        return;
    }
    if (tg_mtproto_load_self_cache_label(peer_cache_file, own_label,
                                         own_label_size) == 0) {
        return;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    (void)tg_mtproto_auth_refresh_self_cache_on_context(
        host, port, api_id, auth_file, dc_id_text, context, peer_cache_file,
        quiet);
    tg_mtproto_close_quiet_stream(quiet, stream);
    if (tg_mtproto_load_self_cache_label(peer_cache_file, own_label,
                                         own_label_size) != 0) {
        tg_mtproto_copy_plain_cache_text(own_label, own_label_size, "me");
    }
}

int tg_mtproto_auth_chat_file(const char *host,
                              const char *port,
                              const char *api_file,
                              const char *auth_file,
                              const char *dc_id_text,
                              const char *peer_cache_file,
                              FILE *stream)
{
    char peer_index[32];
    char peer_label[128];
    char prev_peer_index[32];
    char prev_peer_label[128];
    char swap_peer_index[32];
    char swap_peer_label[128];
    char requested_peer_text[32];
    char requested_peer_index[32];
    char requested_peer_label[128];
    char own_label[128];
    char removed_label[128];
    char forward_destination[32];
    char forward_id_text[32];
    char forward_extra[2];
    char api_id[32];
    char line[512];
#if TG_MTPROTO_DISPLAY_LATIN1
    char send_line[1024];
#endif
    const char *peer_arg;
    const char *username_arg;
    const char *search_arg;
    const char *cmd_arg;
    const char *remove_arg;
    const char *color_arg;
    unsigned long line_length;
    unsigned long last_seen_message_id;
    unsigned long printed_message_count;
    unsigned long consecutive_failures;
    unsigned long sent_message_id;
    unsigned long forward_message_id;
    unsigned long watch_seconds;
    unsigned long parsed_watch_seconds;
    unsigned long saved_timeout;
    FILE *quiet;
    FILE *chat_quiet;
    long chat_quiet_length;
    tg_mtproto_auth_context chat_context;
    tg_chat_engine chat_engine;
    int rc;
    int peer_command;
    int peer_history_ready;
    int chat_raw;
    int have_replay;
    time_t chat_last_poll;
    time_t chat_draft_key;          /* wall clock of the last draft keystroke */
    unsigned long chat_draft_len;   /* draft state at the previous editor exit */
    unsigned long chat_draft_caret;
    int chat_line_consumed;         /* a dispatched line awaits its reset */
    static const char label[] = "chat";
    static const char peer_limit[] = "5";

    if (stream == 0 || host == 0 || port == 0 || api_file == 0 ||
        auth_file == 0 || dc_id_text == 0 || peer_cache_file == 0) {
        if (stream != 0) {
            fputs("chat: invalid-arguments\n", stream);
        }
        return 2;
    }
    memset(&chat_context, 0, sizeof(chat_context));
    /* Bring up the chat engine and bind the notification back-pointer before
       anything can collect (the recv path arms only after the reset below).
       init zeroes the updates cursor + notify queue and enables /diff. */
    tg_chat_engine_init(&chat_engine);
    tg_chat_nq = &chat_engine.notify;
    chat_quiet = 0;
    api_id[0] = '\0';
    saved_timeout = tg_net_connect_timeout_seconds();
    /* A busy account's first getDialogs page can be slow to stream in on some
       stacks (notably MorphOS bsdsocket), so allow a generous per-recv window;
       a too-short timeout aborts mid-frame, desyncs the link and leaves the
       client stuck at "Loading chats...". */
    tg_net_set_connect_timeout_seconds(20UL);
    /*
     * Raw console input enables Up/Down command-history recall. On real Amiga
     * CON: the close gadget and other window events arrive as CSI-style escape
     * sequences; the line editor now consumes any unrecognised CSI fully (see
     * tg_mtproto_chat_read_line_edit) so those events no longer leak stray bytes
     * into the typed line. Falls back to cooked input if set_raw is unsupported.
     */
#if TG_ENABLE_CHAT_RAW_INPUT
    chat_raw = (tg_platform_stdin_set_raw(1) == 0);
#else
    chat_raw = 0;
#endif
    /* Let tg_mtproto_chat_prompt_line (Peer index / Search / /add prompts) echo
       and line-edit in the same mode as the main loop. */
    tg_chat_input_raw = chat_raw;
    /* Colour AUTO mode keys off the same signal: a real interactive console. */
    tg_console_ui_set_interactive(chat_raw);
    /* Full-screen layout when the console cooperates (needs raw mode for the
       window-size report); falls back to the linear flow otherwise. */
    tg_chat_tui_stream = stream;
    if (!chat_raw || !tg_console_tui_enter(stream, " Telegram Amiga ")) {
        /* No full-screen layout: when the mini-termcap marks the console
           CSI-deaf (the probe sequence went to the screen as glyphs),
           AUTO colours stay off in the linear flow too -- their SGR bytes
           would be drawn as garbage. --ui-color on still forces them. */
        if (chat_raw &&
            tg_console_ui_color_mode() == TG_UI_COLOR_AUTO &&
            tg_console_caps_get()->csi_output == TG_UI_CSI_OUTPUT_DEAF) {
            tg_console_ui_set_interactive(0);
        }
        /* Dark theme: paint the window black before the first output. */
        tg_console_ui_enter_screen(stream);
    }
    tg_chat_history_reset();
    /* Arm the cross-chat notification collector for this chat run, and ask
       the server to actually push updates on the chat's connection (one-shot
       commands keep them suppressed via invokeWithoutUpdates). MorphOS stays
       suppressed: a busy account's pending-update backlog swamps its slow
       bsdsocket link (the very scenario the wrapper exists for) and stalled
       the chat at session open on real hardware -- so no cross-chat
       notifications there until a backlog-draining strategy lands. */
    tg_chat_notify_reset(&chat_engine.notify, 1);
    tg_chat_day_shown = 0UL;
#if defined(__MORPHOS__) || defined(__MORPHOS)
    tg_mtproto_set_session_updates(0);
#else
    tg_mtproto_set_session_updates(1);
#endif
#ifdef TG_DIAG_TRACE
    /* A600 hunt, DIAG4: the crashed run's trail ended at "loading saved
       session" while the screen reached "Loading chats..." -- this stretch
       had no probes at all. One line per step turns the next field log into
       the name of the call that never returned. */
    tg_gui_log("diag: chat console ready");
#endif
    tg_mtproto_chat_print_system_line(stream, "Loading chats...");
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: loading-chats printed, peers parse begin");
#endif
    if (tg_mtproto_peer_cache_available(peer_cache_file)) {
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: peers parse done, cache usable");
#endif
        rc = 0;
    } else {
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: peers parse done, cache NOT usable -> network list");
#endif
        TG_MTPROTO_BOOTSTRAP_BREATHER();
        quiet = tg_mtproto_open_quiet_stream(stream);
        rc = tg_mtproto_auth_list_peers_file(host, port, api_file, auth_file,
                                             dc_id_text, peer_limit,
                                             peer_cache_file, quiet);
        /* Heavy accounts return messages.dialogsSlice and list-peers fails;
           keep its technical log quiet and fall back to /add. */
        tg_mtproto_close_quiet_stream(quiet, stream);
    }
    if (rc != 0 && !tg_mtproto_peer_cache_available(peer_cache_file)) {
        TG_MTPROTO_BOOTSTRAP_BREATHER();
        quiet = tg_mtproto_open_quiet_stream(stream);
        rc = tg_mtproto_auth_list_peers_file(host, port, api_file, auth_file,
                                             dc_id_text, "1",
                                             peer_cache_file, quiet);
        tg_mtproto_close_quiet_stream(quiet, stream);
    }
    if (rc != 0 && !tg_mtproto_peer_cache_available(peer_cache_file)) {
        tg_mtproto_chat_print_system_line(stream, "No cached chats yet.");
        tg_mtproto_chat_print_system_line(
            stream, "Use /add name to search users or groups.");
        rc = 0;
    }
    if (rc != 0) {
        tg_mtproto_chat_print_system_line(stream, "Using cached chats.");
    }
    TG_MTPROTO_BOOTSTRAP_BREATHER();
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: printing opening-session");
#endif
    tg_mtproto_chat_print_system_line(stream, "Opening session...");
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: opening-session printed");
#endif
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = tg_mtproto_load_api_id_file(api_file, api_id, sizeof(api_id),
                                     quiet, label);
    if (rc == 0) {
        rc = tg_mtproto_ensure_saved_auth_context(host, port, auth_file,
                                                  dc_id_text, &chat_context,
                                                  quiet, "chat session");
        if (rc != 0) {
            /* The first session open on a slow link often stalls (field
               MorphOS report: stuck on "Opening session...", relaunch went
               straight through). A fresh connection usually succeeds:
               retry once ourselves instead of making the user relaunch. */
            tg_mtproto_close_auth_context(&chat_context);
            tg_mtproto_reset_quiet_stream(quiet, stream);
            tg_mtproto_chat_print_system_line(stream,
                                              "Slow start, retrying once...");
            rc = tg_mtproto_ensure_saved_auth_context(host, port, auth_file,
                                                      dc_id_text,
                                                      &chat_context, quiet,
                                                      "chat session");
        }
    }
    if (rc != 0) {
        tg_mtproto_replay_quiet_stream(quiet, stream);
        tg_mtproto_close_quiet_stream(quiet, stream);
        tg_net_set_connect_timeout_seconds(saved_timeout);
        tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
        if (chat_raw) { tg_platform_stdin_set_raw(0); }
        return 2;
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_mtproto_chat_print_system_line(stream, "Loading profile...");
    tg_mtproto_chat_load_own_label(host, port, api_id, auth_file, dc_id_text,
                                   &chat_context, peer_cache_file,
                                   own_label, sizeof(own_label), stream);
    /* The gap-handling cursor starts unprimed (zeroed by tg_chat_engine_init
       at session start); the drain tick primes it lazily (one query) so the
       fragile session-open phase on slow links stays as light as before. */
    if (tg_mtproto_peer_cache_available(peer_cache_file)) {
        tg_mtproto_chat_print_system_line(stream, "Choose a chat:");
        fputc('\n', stream);
        {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            tg_mtproto_print_peer_cache_public(peer_cache_file, tui_cap,
                                               peer_index);
            tg_console_tui_capture_end(tui_cap, stream);
        }
        if (tg_mtproto_chat_prompt_line("\nPeer index: ", peer_index,
                                        sizeof(peer_index), 1, stream,
                                        label) != 0) {
            tg_mtproto_close_auth_context(&chat_context);
            tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
            return 2;
        }
        /* The picker footer advertises /saved: honour it (and its plain
           forms) here too by normalising to the "self" sentinel index. */
        if (strcmp(peer_index, "/saved") == 0 ||
            strcmp(peer_index, "saved") == 0 ||
            strcmp(peer_index, "0") == 0) {
            strcpy(peer_index, "self");
        }
        if (tg_mtproto_load_peer_cache_label(peer_cache_file, peer_index,
                                             peer_label,
                                             sizeof(peer_label)) != 0) {
            peer_label[0] = '\0';
        }
        tg_mtproto_chat_tui_status(peer_label);
    } else {
        peer_index[0] = '\0';
        peer_label[0] = '\0';
        tg_mtproto_chat_print_system_line(stream,
                                          "Type /add name to find a chat.");
    }
    last_seen_message_id = 0UL;
    /* Auto-read cadence: every poll costs a full slow-link round trip on
       MorphOS, so pace it down there; /watch can still change it. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
    watch_seconds = 12UL;
#else
    watch_seconds = 2UL;
#endif
    /* Heavy accounts must reach the prompt before any blocking history read. */
    peer_history_ready = 0;
    chat_last_poll = (time_t)0;
    chat_draft_key = (time_t)0;
    chat_draft_len = 0UL;
    chat_draft_caret = 0UL;
    chat_line_consumed = 0;
    prev_peer_index[0] = '\0';
    prev_peer_label[0] = '\0';
    line_length = 0UL;
    consecutive_failures = 0UL;
    chat_quiet = tg_mtproto_open_quiet_stream(stream);
    {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            tg_mtproto_chat_print_help(tui_cap);
            tg_console_tui_capture_end(tui_cap, stream);
        }
    {
        char watch_note[64];
        sprintf(watch_note, "Auto-read every %lu second(s).", watch_seconds);
        tg_mtproto_chat_print_system_line(stream, watch_note);
    }
    if (peer_index[0] == '\0') {
        tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
    }
    for (;;) {
        if (chat_line_consumed) {
            /* The previous iteration dispatched this line (message sent or
               command run). The dispatcher re-measured line_length to do its
               work, so without this reset the next keystroke would edit the
               GHOST of the sent message (68000 field report 2026-08-05), and
               the stale length kept the draft guard suppressing polls. */
            chat_line_consumed = 0;
            line[0] = '\0';
            line_length = 0UL;
            tg_chat_caret = 0UL;
        }
        if (tg_console_tui_resize_pending()) {
            if (tg_console_tui_resize(stream, " Telegram Amiga ")) {
                tg_mtproto_chat_tui_status(peer_label);
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label,
                                            line, line_length,
                                            tg_chat_input_raw);
            }
        }
        if (peer_index[0] != '\0' && !peer_history_ready) {
            peer_history_ready = 1;
            (void)tg_mtproto_chat_open_history(
                stream, chat_quiet, host, port, api_id, auth_file, dc_id_text,
                &chat_context, peer_cache_file, peer_index, peer_label,
                own_label, &last_seen_message_id);
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
        }
        if (watch_seconds == 0UL) {
            /* Auto-read is off, so we would otherwise block ~forever. Wake the
               editor every few seconds anyway so a file dropped on the console
               window (polled on the read timeout) is noticed promptly; rc==0
               just re-loops without any network poll, keeping /watch off. */
            rc = tg_mtproto_chat_read_line_edit(line, sizeof(line),
                                                &line_length, 3UL, chat_raw,
                                                1, stream);
            if (rc == 0) {
                continue;
            }
        } else {
            rc = tg_mtproto_chat_read_line_edit(line, sizeof(line),
                                                &line_length, watch_seconds,
                                                chat_raw, 1, stream);
        }
        if (rc == 0) {
            time_t poll_now;

            if (line_length > 0UL && !chat_raw) {
                /* Cooked console: the half-typed line cannot be redrawn after
                   async output, so hold polling until Enter (old behavior). */
                continue;
            }
            if (peer_index[0] == '\0') {
                continue;
            }
            /* In raw mode rc==0 also fires after every keystroke, not just on
               the watch timeout. Throttle on wall-clock so fast typing does
               not turn into a poll per keypress. */
            /* Draft keystroke tracker: the editor also returns on its wake
               timeout, so only a CHANGE in the pending line counts as
               keyboard activity. An empty line clears the tracker. */
            if (line_length == 0UL) {
                chat_draft_key = (time_t)0;
                chat_draft_len = 0UL;
                chat_draft_caret = 0UL;
            } else if (line_length != chat_draft_len ||
                       tg_chat_caret != chat_draft_caret) {
                chat_draft_len = line_length;
                chat_draft_caret = tg_chat_caret;
                chat_draft_key = time(0);
            }
            if (line_length > 0UL) {
                /* A draft is being composed: hold background polls while the
                   user is actually typing (a 68030/25 field report read
                   "characters take too long to appear" when round trips
                   wedged between keystrokes). But not FOREVER: a parked
                   draft used to suppress replies entirely (68000 field
                   report 2026-08-05), so once the keyboard has been quiet
                   for a few seconds the normal poll cadence resumes. */
                poll_now = time(0);
                if (poll_now == (time_t)-1 ||
                    chat_draft_key == (time_t)0 ||
                    poll_now < chat_draft_key ||
                    (unsigned long)(poll_now - chat_draft_key) <
                        TG_MTPROTO_CHAT_DRAFT_QUIET_SECONDS) {
                    continue;
                }
            }
            poll_now = time(0);
            if (poll_now != (time_t)-1 && chat_last_poll != (time_t)0 &&
                poll_now >= chat_last_poll &&
                (unsigned long)(poll_now - chat_last_poll) < watch_seconds) {
                continue;
            }
            if (tg_platform_stdin_readable(0UL)) {
                /* A keystroke is already queued: starting a poll now would
                   freeze the editor for the whole round trip (seconds on a
                   7 MHz 68000) and then spit the buffered keys out at once.
                   Serve the keyboard first; the poll runs on a quiet pass. */
                continue;
            }
            chat_last_poll = poll_now;
            quiet = chat_quiet;
            tg_mtproto_reset_quiet_stream(quiet, stream);
            printed_message_count = 0UL;
            rc = tg_mtproto_auth_print_history_text_peer_on_context(
                host, port, api_id, auth_file, dc_id_text, &chat_context,
                peer_cache_file, peer_index, "5", quiet,
                &last_seen_message_id, &printed_message_count, 1, 0, 0,
                peer_label, own_label);
            if (rc == 0) {
                consecutive_failures = 0UL;
            } else if (++consecutive_failures >= TG_MTPROTO_CHAT_STALL_LIMIT) {
                /* The shared session looks wedged (e.g. stale salt/seqno after a
                   long idle, or repeated soft timeouts that keep the connection
                   open). Drop it so the next poll reopens a fresh connection and
                   resumes, instead of polling a dead session forever. */
                tg_mtproto_close_auth_context(&chat_context);
                consecutive_failures = 0UL;
            }
            chat_quiet_length = tg_mtproto_quiet_stream_length(quiet, stream);
            /* The paced getDifference drain, after the replay length is
               fixed so its quiet noise never leaks into the transcript.
               On MorphOS pushes are suppressed (the full backlog drowns
               its link) and the drain IS the notification path: every
               tick (~12s). On push platforms it is a reconciliation sweep
               (~60s) that catches whatever a dropped push missed; the
               dedupe ring absorbs the overlap with live pushes. /diff off
               turns it off everywhere. */
            if (rc == 0 && chat_engine.diff_enabled) {
                static unsigned long diff_tick = 0UL;
                unsigned long diff_cadence;
#if defined(__MORPHOS__) || defined(__MORPHOS)
                diff_cadence = 1UL;
#else
                diff_cadence = 30UL;
#endif
                ++diff_tick;
                if ((diff_tick % diff_cadence) == 0UL) {
                    if (chat_engine.updates_state.pts == 0UL) {
                        (void)tg_mtproto_chat_get_updates_state_on_context(
                            host, port, api_id, auth_file, dc_id_text,
                            &chat_context, &chat_engine.updates_state, quiet);
                    } else {
                        (void)tg_mtproto_chat_drain_difference_on_context(
                            host, port, api_id, auth_file, dc_id_text,
                            &chat_context, &chat_engine.updates_state, quiet);
                    }
                }
            }
            have_replay = rc == 0 && printed_message_count > 0UL &&
                          (quiet == stream || chat_quiet_length > 0L);
            if (have_replay || chat_engine.notify.count > 0UL) {
                tg_mtproto_chat_clear_input_line(stream, chat_raw);
                if (have_replay) {
                    tg_mtproto_replay_quiet_stream_length(
                        quiet, stream, chat_quiet_length);
                }
                {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                tg_mtproto_chat_print_notify_lines(tui_cap, peer_cache_file,
                                                   peer_index);
                tg_console_tui_capture_end(tui_cap, stream);
            }
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label,
                                            line, line_length, chat_raw);
            }
            continue;
        }
        if (rc < 0) {
            tg_mtproto_chat_print_system_line(stream, "Input closed.");
            tg_mtproto_close_quiet_stream(chat_quiet, stream);
            tg_mtproto_close_auth_context(&chat_context);
            tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
            return 0;
        }
        tg_mtproto_trim_line(line);
        if (line[0] == '\0') {
            if (peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(
                    stream, "Choose a chat first with /add name.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                rc = tg_mtproto_auth_print_history_text_peer_on_context(
                    host, port, api_id, auth_file, dc_id_text, &chat_context,
                    peer_cache_file, peer_index, "5", tui_cap,
                    &last_seen_message_id, 0, 1, 0, 0, peer_label, own_label);
                if (rc == TG_MTPROTO_QUERY_SOFT_FAIL) {
                    fprintf(tui_cap,
                            "No reply yet (slow link). Press Enter to retry.\n");
                } else if (rc != 0) {
                    fprintf(tui_cap,
                            "Could not read messages now (error %d).\n", rc);
                }
                tg_console_tui_capture_end(tui_cap, stream);
            }
            {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            tg_mtproto_chat_print_notify_lines(tui_cap, peer_cache_file,
                                               peer_index);
            tg_console_tui_capture_end(tui_cap, stream);
        }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        /* The chat loop never trimmed its line (only the startup picker did):
           a habitual trailing space after an exact-match command ("/getfile ")
           fell through the whole chain and was SENT as a message. Trim before
           dispatch so every strcmp command tolerates trailing whitespace;
           message text is unaffected beyond the trailing blanks nobody wants. */
        tg_mtproto_trim_line(line);
        line_length = (unsigned long)strlen(line);
        chat_line_consumed = 1; /* reset at the next loop top, after dispatch */
        if (strcmp(line, "/quit") == 0 || strcmp(line, "quit") == 0) {
            tg_mtproto_chat_print_system_line(stream, "Bye.");
            tg_mtproto_close_quiet_stream(chat_quiet, stream);
            tg_mtproto_close_auth_context(&chat_context);
            tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
            return 0;
        }
        if (strcmp(line, "/help") == 0 || strcmp(line, "help") == 0) {
            {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            tg_mtproto_chat_print_help(tui_cap);
            tg_console_tui_capture_end(tui_cap, stream);
        }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        color_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/color", &color_arg) ||
            tg_mtproto_chat_named_command_arg(line, "color", &color_arg)) {
            if (color_arg != 0 && strcmp(color_arg, "on") == 0) {
                tg_console_ui_set_color_mode(TG_UI_COLOR_ON);
            } else if (color_arg != 0 && strcmp(color_arg, "off") == 0) {
                tg_console_ui_set_color_mode(TG_UI_COLOR_OFF);
            } else {
                tg_console_ui_set_color_mode(tg_console_ui_color_active() ?
                                             TG_UI_COLOR_OFF : TG_UI_COLOR_ON);
            }
            tg_mtproto_chat_print_system_line(
                stream,
                tg_console_ui_color_active() ? "Colors on." : "Colors off.");
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        color_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/bell", &color_arg) ||
            tg_mtproto_chat_named_command_arg(line, "bell", &color_arg)) {
            if (color_arg != 0 && strcmp(color_arg, "on") == 0) {
                tg_chat_bell_enabled = 1;
            } else if (color_arg != 0 && strcmp(color_arg, "off") == 0) {
                tg_chat_bell_enabled = 0;
            } else {
                tg_chat_bell_enabled = !tg_chat_bell_enabled;
            }
            tg_mtproto_chat_print_system_line(
                stream, tg_chat_bell_enabled ? "Bell on." : "Bell off.");
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (strcmp(line, "/swap") == 0 || strcmp(line, "swap") == 0) {
            if (prev_peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(stream,
                                                  "No previous chat yet.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            strcpy(swap_peer_index, peer_index);
            strcpy(swap_peer_label, peer_label);
            strcpy(peer_index, prev_peer_index);
            strcpy(peer_label, prev_peer_label);
            strcpy(prev_peer_index, swap_peer_index);
            strcpy(prev_peer_label, swap_peer_label);
            last_seen_message_id = 0UL;
            peer_history_ready = 0;
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                fprintf(tui_cap, "Current chat: ");
                tg_mtproto_print_cache_text(tui_cap, peer_label);
                fprintf(tui_cap, "\n");
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_tui_status(peer_label);
            continue;
        }
        if (strcmp(line, "/peers") == 0) {
            tg_mtproto_chat_load_own_label(host, port, api_id, auth_file,
                                           dc_id_text, &chat_context,
                                           peer_cache_file,
                                           own_label, sizeof(own_label),
                                           stream);
            {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            fprintf(tui_cap, "\nChoose a chat:\n\n");
            tg_mtproto_print_peer_cache_public(peer_cache_file, tui_cap,
                                               peer_index);
            fprintf(tui_cap,
                    "Type a number, /search text, or /add name.\n");
            tg_console_tui_capture_end(tui_cap, stream);
        }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (strcmp(line, "/history") == 0) {
            if (peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(
                    stream, "Choose a chat first with /peers or /add name.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                rc = tg_mtproto_auth_print_history_text_peer_on_context(
                    host, port, api_id, auth_file, dc_id_text, &chat_context,
                    peer_cache_file, peer_index, "10", tui_cap,
                    0, 0, 0, 1, 1, peer_label, own_label);
                if (rc != 0) {
                    fprintf(tui_cap, "Could not read message history now.\n");
                }
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (tg_mtproto_chat_named_command_arg(line, "/getfile", &cmd_arg)) {
            /* F9 parity for the console: download the MOST RECENT document in
               the open chat into downloads/ (same machinery as the GUI). */
            if (peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(
                    stream, "Choose a chat first with /peers or /add name.");
            } else {
                tg_mtproto_file_ctx fc;
                char saved_path[160];
                int frc;
                FILE *tui_cap;

                fc.host = host; fc.port = port; fc.api_id = api_id;
                fc.auth_file = auth_file; fc.dc_id_text = dc_id_text;
                fc.context = &chat_context;
                fc.peer_cache_file = peer_cache_file;
                fc.peer_index = peer_index;
                tg_mtproto_chat_print_system_line(stream, "Downloading...");
                frc = tg_mtproto_file_download(&fc, 0UL, saved_path,
                                               sizeof(saved_path), stream,
                                               0, 0); /* TUI: no % hook (yet) */
                tui_cap = tg_console_tui_capture_begin(stream);
                if (frc == 0) {
                    fprintf(tui_cap, "Saved to %s\n", saved_path);
                } else if (frc == 2) {
                    fprintf(tui_cap,
                            "File is on another server - not supported yet.\n");
                } else if (frc == 3) {
                    fprintf(tui_cap, "Could not write to downloads/.\n");
                } else if (frc == 4) {
                    if (saved_path[0] != '\0') {
                        fprintf(tui_cap, "Transfer failed: %.110s\n",
                                saved_path);
                    } else {
                        fprintf(tui_cap, "Transfer failed (server error).\n");
                    }
                } else if (saved_path[0] != '\0') {
                    fprintf(tui_cap, "No file: %.120s\n", saved_path);
                } else {
                    fprintf(tui_cap, "Download failed.\n");
                }
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        {
            int send_as_photo;

            send_as_photo = -1;
            if (tg_mtproto_chat_named_command_arg(line, "/sendfile",
                                                   &cmd_arg)) {
                send_as_photo = 0;
            } else if (tg_mtproto_chat_named_command_arg(line, "/photo",
                                                          &cmd_arg)) {
                send_as_photo = 1;
            }
            if (send_as_photo >= 0) {
                /* Upload to the open chat. Both helpers match their bare and
                   tab forms, so a syntax probe can never fall through and be
                   SENT as a literal message. */
                const char *fpath = cmd_arg;
                const char *fcaption = 0;
                char fpath_buf[256];

                /* An icon dropped on the console injects the path QUOTED (the
                   con-handler quotes paths with spaces, Shell-style): accept
                   "path" by stripping the surrounding double quotes. Whatever
                   follows the closing quote (or, for /photo, the first space
                   of an unquoted path) is the caption. */
                if (fpath[0] == '"') {
                    unsigned long fl = 0UL;

                    ++fpath;
                    while (fpath[fl] != '\0' && fpath[fl] != '"' &&
                           fl + 1UL < sizeof(fpath_buf)) {
                        fpath_buf[fl] = fpath[fl];
                        ++fl;
                    }
                    if (send_as_photo && fpath[fl] == '"') {
                        const char *ct = fpath + fl + 1UL;

                        while (*ct == ' ') {
                            ++ct;
                        }
                        if (*ct != '\0') {
                            fcaption = ct;
                        }
                    }
                    fpath_buf[fl] = '\0';
                    fpath = fpath_buf;
                } else if (send_as_photo) {
                    unsigned long fl = 0UL;

                    while (fpath[fl] != '\0' && fpath[fl] != ' ' &&
                           fl + 1UL < sizeof(fpath_buf)) {
                        fpath_buf[fl] = fpath[fl];
                        ++fl;
                    }
                    if (fpath[fl] == ' ') {
                        const char *ct = fpath + fl;

                        while (*ct == ' ') {
                            ++ct;
                        }
                        if (*ct != '\0') {
                            fcaption = ct;
                        }
                        fpath_buf[fl] = '\0';
                        fpath = fpath_buf;
                    }
                }
                if (peer_index[0] == '\0') {
                    tg_mtproto_chat_print_system_line(
                        stream, "Choose a chat first with /peers or /add name.");
                } else if (*fpath == '\0') {
                    tg_mtproto_chat_print_system_line(
                        stream, send_as_photo
                            ? "Usage: /photo <jpeg-or-png-path> [caption]"
                            : "Usage: /sendfile <path>");
                } else {
                    tg_mtproto_file_ctx fc;
                    int frc;
                    FILE *tui_cap;

                    fc.host = host; fc.port = port; fc.api_id = api_id;
                    fc.auth_file = auth_file; fc.dc_id_text = dc_id_text;
                    fc.context = &chat_context;
                    fc.peer_cache_file = peer_cache_file;
                    fc.peer_index = peer_index;
                    tg_mtproto_chat_print_system_line(
                        stream, send_as_photo ? "Uploading photo..."
                                              : "Uploading...");
                    frc = tg_mtproto_file_send(&fc, fpath, stream, 0, 0,
                                               send_as_photo,
                                               send_as_photo ? fcaption : 0);
                    tui_cap = tg_console_tui_capture_begin(stream);
                    if (frc == 0) {
                        if (send_as_photo &&
                            tg_gui_session_transfer_photo_fallback()) {
                            fprintf(tui_cap,
                                    "File sent (photo was over 10 MiB).\n");
                        } else {
                            fprintf(tui_cap, send_as_photo ? "Photo sent.\n"
                                                          : "File sent.\n");
                        }
                    } else if (frc == 2) {
                        fprintf(tui_cap,
                                "File too big (%lu MiB limit on this build).\n",
                                tg_gui_session_upload_limit_mib());
                    } else if (frc == 3) {
                        fprintf(tui_cap, send_as_photo
                                             ? "Could not read that photo.\n"
                                             : "Could not read that file.\n");
                    } else if (frc == 5) {
                        fprintf(tui_cap, "That file is empty (0 bytes).\n");
                    } else if (frc == 7) {
                        fprintf(tui_cap, "Not sent as a photo: %.100s.\n",
                                tg_mtproto_query_fail[0] != '\0'
                                    ? tg_mtproto_query_fail
                                    : "not a valid JPEG or PNG");
                    } else if (tg_mtproto_query_fail[0] != '\0') {
                        fprintf(tui_cap, "Upload failed: %.100s\n",
                                tg_mtproto_upload_failure_text(
                                    tg_mtproto_query_fail));
                    } else {
                        fprintf(tui_cap, "Upload failed.\n");
                    }
                    tg_console_tui_capture_end(tui_cap, stream);
                }
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
        }
        if (strcmp(line, "/saved") == 0 || strcmp(line, "0") == 0) {
            /* F10 parity: jump to Saved Messages (the self chat, peer index
               "self" in the cache loaders) as the cloud transfer drawer. */
            if (peer_index[0] != '\0' && strcmp(peer_index, "self") != 0) {
                strcpy(prev_peer_index, peer_index);
                strcpy(prev_peer_label, peer_label);
            }
            strcpy(peer_index, "self");
            strcpy(peer_label, "Saved Messages");
            last_seen_message_id = 0UL;
            peer_history_ready = 0;
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);

                fprintf(tui_cap, "Current chat: Saved Messages\n");
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_tui_status(peer_label);
            continue;
        }
        if (strcmp(line, "/diff") == 0 || strcmp(line, "/diff on") == 0 ||
            strcmp(line, "/diff off") == 0) {
            if (strcmp(line, "/diff on") == 0) {
                chat_engine.diff_enabled = 1;
            } else if (strcmp(line, "/diff off") == 0) {
                chat_engine.diff_enabled = 0;
            } else {
                chat_engine.diff_enabled = !chat_engine.diff_enabled;
            }
            tg_mtproto_chat_print_system_line(
                stream, chat_engine.diff_enabled
                            ? "Background catch-up on."
                            : "Background catch-up off.");
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (strcmp(line, "/difftest") == 0) {
            /* Undocumented diagnostic: prime + one paced getDifference,
               reporting the cursor and what got queued. */
            char diff_note[96];
            int diff_rc;
            FILE *diff_quiet = tg_mtproto_open_quiet_stream(stream);
            if (chat_engine.updates_state.pts == 0UL) {
                (void)tg_mtproto_chat_get_updates_state_on_context(
                    host, port, api_id, auth_file, dc_id_text, &chat_context,
                    &chat_engine.updates_state, diff_quiet);
            }
            sprintf(diff_note, "diff: pts=%lu seq=%lu date=%lu",
                    chat_engine.updates_state.pts, chat_engine.updates_state.seq,
                    chat_engine.updates_state.date);
            tg_mtproto_chat_print_system_line(stream, diff_note);
            diff_rc = tg_mtproto_chat_drain_difference_on_context(
                host, port, api_id, auth_file, dc_id_text, &chat_context,
                &chat_engine.updates_state, diff_quiet);
            tg_mtproto_close_quiet_stream(diff_quiet, stream);
            sprintf(diff_note, "diff: rc=%d queued=%lu new-pts=%lu", diff_rc,
                    chat_engine.notify.count, chat_engine.updates_state.pts);
            tg_mtproto_chat_print_system_line(stream, diff_note);
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                tg_mtproto_chat_print_notify_lines(tui_cap, peer_cache_file,
                                                   peer_index);
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (strcmp(line, "/resize") == 0) {
            /* Manual re-fit for consoles that do not deliver NEWSIZE raw
               events (the MorphOS tabbed terminal): re-query the window and
               repaint. The same-geometry guard makes it a clean no-op when
               nothing changed. */
            if (tg_console_tui_active()) {
                if (tg_console_tui_resize(stream, " Telegram Amiga ")) {
                    tg_mtproto_chat_tui_status(peer_label);
                } else {
                    tg_mtproto_chat_print_system_line(
                        stream, "Window size unchanged.");
                }
            } else {
                tg_mtproto_chat_print_system_line(
                    stream, "Full-screen mode is not active.");
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        search_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/search", &search_arg) ||
            tg_mtproto_chat_named_command_arg(line, "search", &search_arg) ||
            tg_mtproto_chat_named_command_arg(line, "/find", &search_arg) ||
            tg_mtproto_chat_named_command_arg(line, "find", &search_arg)) {
            if (search_arg == 0 || search_arg[0] == '\0') {
                if (tg_mtproto_chat_prompt_line("Search: ", line,
                                                sizeof(line), 1, stream,
                                                label) != 0) {
                    tg_mtproto_close_quiet_stream(chat_quiet, stream);
                    tg_mtproto_close_auth_context(&chat_context);
                    tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
                    return 2;
                }
                search_arg = line;
            }
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                tg_mtproto_peer_cache_search_public(peer_cache_file,
                                                    search_arg, tui_cap);
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        remove_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/remove", &remove_arg) ||
            tg_mtproto_chat_named_command_arg(line, "remove", &remove_arg) ||
            tg_mtproto_chat_named_command_arg(line, "/delete", &remove_arg) ||
            tg_mtproto_chat_named_command_arg(line, "delete", &remove_arg)) {
            if (remove_arg == 0 || remove_arg[0] == '\0') {
                if (tg_mtproto_chat_prompt_line("Remove chat number: ", line,
                                                sizeof(line), 1, stream,
                                                label) != 0) {
                    tg_mtproto_close_quiet_stream(chat_quiet, stream);
                    tg_mtproto_close_auth_context(&chat_context);
                    tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
                    return 2;
                }
                remove_arg = line;
            }
            if (tg_mtproto_peer_cache_remove_public_index(
                    peer_cache_file, remove_arg, removed_label,
                    sizeof(removed_label), stream) != 0) {
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                fprintf(tui_cap, "Removed ");
                if (removed_label[0] != '\0') {
                    tg_mtproto_print_cache_text(tui_cap, removed_label);
                } else {
                    fprintf(tui_cap, "chat");
                }
                fprintf(tui_cap, ".\n");
                tg_console_tui_capture_end(tui_cap, stream);
            }
            if (!tg_mtproto_peer_cache_available(peer_cache_file)) {
                peer_index[0] = '\0';
                peer_label[0] = '\0';
                peer_history_ready = 0;
                tg_mtproto_chat_print_system_line(
                    stream, "No cached chats. Add one with /add name.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            fprintf(tui_cap, "\nChoose a chat:\n\n");
            tg_mtproto_print_peer_cache_public(peer_cache_file, tui_cap,
                                               peer_index);
            tg_console_tui_capture_end(tui_cap, stream);
        }
            if (tg_mtproto_chat_prompt_line("Peer index: ",
                                            requested_peer_text,
                                            sizeof(requested_peer_text), 1,
                                            stream, label) != 0) {
                tg_mtproto_close_quiet_stream(chat_quiet, stream);
                tg_mtproto_close_auth_context(&chat_context);
                tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
                return 2;
            }
            if (tg_mtproto_chat_copy_peer_index(
                    requested_peer_index, sizeof(requested_peer_index),
                    requested_peer_text) == 0 &&
                tg_mtproto_load_peer_cache_label(peer_cache_file,
                                                 requested_peer_index,
                                                 requested_peer_label,
                                                 sizeof(requested_peer_label)) ==
                    0) {
                if (peer_index[0] != '\0' &&
                    strcmp(peer_index, requested_peer_index) != 0) {
                    strcpy(prev_peer_index, peer_index);
                    strcpy(prev_peer_label, peer_label);
                }
                strcpy(peer_index, requested_peer_index);
                strcpy(peer_label, requested_peer_label);
                last_seen_message_id = 0UL;
                peer_history_ready = 0;
                {
                    FILE *tui_cap = tg_console_tui_capture_begin(stream);
                    fprintf(tui_cap, "Current chat: ");
                    tg_mtproto_print_cache_text(tui_cap, peer_label);
                    fprintf(tui_cap, "\n");
                    tg_console_tui_capture_end(tui_cap, stream);
                }
                tg_mtproto_chat_tui_status(peer_label);
                continue;
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        username_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/add", &username_arg) ||
            tg_mtproto_chat_named_command_arg(line, "add", &username_arg)) {
            if (username_arg == 0 || username_arg[0] == '\0') {
                if (tg_mtproto_chat_prompt_line("Name, username or t.me link: ",
                                                line, sizeof(line), 1,
                                                stream, label) != 0) {
                    tg_mtproto_close_quiet_stream(chat_quiet, stream);
                    tg_mtproto_close_auth_context(&chat_context);
                    tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
                    return 2;
                }
                username_arg = line;
            }
            requested_peer_index[0] = '\0';
            requested_peer_label[0] = '\0';
            if (tg_mtproto_chat_arg_is_exact_username(username_arg)) {
                quiet = tg_mtproto_open_quiet_stream(stream);
                rc = tg_mtproto_auth_resolve_username_file(
                    host, port, api_file, auth_file, dc_id_text, username_arg,
                    peer_cache_file, quiet);
                tg_mtproto_close_quiet_stream(quiet, stream);
            } else {
                /* The global-search picker interleaves its result list with
                   its own prompt: simplest correct behaviour in full-screen
                   mode is to drop to the linear flow for its duration. */
                if (tg_console_tui_active()) {
                    tg_console_tui_leave(stream);
                    /* leave() resets attributes: repaint the dark theme so
                       the picker list does not sit on the bare window
                       colour until the full-screen layout returns. */
                    tg_console_ui_enter_screen(stream);
                }
                rc = tg_mtproto_auth_search_global_on_context(
                    host, port, api_id, auth_file, dc_id_text, &chat_context,
                    peer_cache_file, username_arg, requested_peer_index,
                    sizeof(requested_peer_index), requested_peer_label,
                    sizeof(requested_peer_label), stream);
                if (tg_chat_tui_stream != 0 && tg_chat_input_raw) {
                    (void)tg_console_tui_enter(stream, " Telegram Amiga ");
                }
                if (rc != 0 &&
                    tg_mtproto_peer_cache_text_looks_username(username_arg)) {
                    quiet = tg_mtproto_open_quiet_stream(stream);
                    rc = tg_mtproto_auth_resolve_username_file(
                        host, port, api_file, auth_file, dc_id_text,
                        username_arg, peer_cache_file, quiet);
                    tg_mtproto_close_quiet_stream(quiet, stream);
                }
            }
            if (rc != 0) {
                tg_mtproto_chat_print_system_line(
                    stream,
                    "Could not add that chat. Try @username or a t.me link.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            if (requested_peer_index[0] == '\0' &&
                tg_mtproto_peer_cache_find_username_public_index(
                    peer_cache_file, username_arg, requested_peer_index,
                    sizeof(requested_peer_index), requested_peer_label,
                    sizeof(requested_peer_label)) != 0) {
                requested_peer_index[0] = '\0';
                requested_peer_label[0] = '\0';
            }
            if (requested_peer_index[0] != '\0') {
                if (peer_index[0] != '\0' &&
                    strcmp(peer_index, requested_peer_index) != 0) {
                    strcpy(prev_peer_index, peer_index);
                    strcpy(prev_peer_label, peer_label);
                }
                strcpy(peer_index, requested_peer_index);
                strcpy(peer_label, requested_peer_label);
                last_seen_message_id = 0UL;
                peer_history_ready = 0;
                {
                    FILE *tui_cap = tg_console_tui_capture_begin(stream);
                    fprintf(tui_cap, "Current chat: ");
                    if (peer_label[0] != '\0') {
                        tg_mtproto_print_cache_text(tui_cap, peer_label);
                    } else {
                        fprintf(tui_cap, "%s", peer_index);
                    }
                    fprintf(tui_cap, "\n");
                    tg_console_tui_capture_end(tui_cap, stream);
                }
                tg_mtproto_chat_tui_status(peer_label);
                continue;
            } else {
                {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            fprintf(tui_cap, "\nChat added. Cached chats:\n\n");
            tg_mtproto_print_peer_cache_public(peer_cache_file, tui_cap,
                                               peer_index);
            fprintf(tui_cap, "Type a number to switch.\n");
            tg_console_tui_capture_end(tui_cap, stream);
        }
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        peer_arg = 0;
        peer_command = tg_mtproto_chat_peer_command_arg(line, &peer_arg);
        if (peer_command || tg_mtproto_chat_is_number_line(line)) {
            if (!peer_command) {
                if (tg_mtproto_chat_copy_peer_index(
                        requested_peer_index,
                        sizeof(requested_peer_index), line) != 0) {
                    {
                        char peer_note[160];
                        sprintf(peer_note, "%.64s: use /peer <number>", label);
                        tg_mtproto_chat_print_system_line(stream, peer_note);
                    }
                    tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                    continue;
                }
            } else if (peer_arg[0] == '\0') {
                {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            fprintf(tui_cap, "\nChoose a chat:\n\n");
            tg_mtproto_print_peer_cache_public(peer_cache_file, tui_cap,
                                               peer_index);
            tg_console_tui_capture_end(tui_cap, stream);
        }
                if (tg_mtproto_chat_prompt_line("Peer index: ",
                                                requested_peer_text,
                                                sizeof(requested_peer_text),
                                                1, stream, label) != 0) {
                    tg_mtproto_close_quiet_stream(chat_quiet, stream);
                    tg_mtproto_close_auth_context(&chat_context);
                    tg_net_set_connect_timeout_seconds(saved_timeout);
            tg_console_tui_leave(stream);
            tg_chat_tui_stream = 0;
            tg_console_ui_leave_screen(stream);
            if (chat_raw) { tg_platform_stdin_set_raw(0); }
                    return 2;
                }
                if (tg_mtproto_chat_copy_peer_index(
                        requested_peer_index, sizeof(requested_peer_index),
                        requested_peer_text) != 0) {
                    {
                        char peer_note[160];
                        sprintf(peer_note, "%.64s: use /peer <number>", label);
                        tg_mtproto_chat_print_system_line(stream, peer_note);
                    }
                    tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                    continue;
                }
            } else if (tg_mtproto_chat_copy_peer_index(
                           requested_peer_index,
                           sizeof(requested_peer_index), peer_arg) != 0) {
                {
                    char peer_note[160];
                    sprintf(peer_note, "%.64s: use /peer <number>", label);
                    tg_mtproto_chat_print_system_line(stream, peer_note);
                }
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            if (tg_mtproto_load_peer_cache_label(peer_cache_file,
                                                 requested_peer_index,
                                                 requested_peer_label,
                                                 sizeof(requested_peer_label))
                != 0) {
                {
                    char peer_note[160];
                    sprintf(peer_note, "%.64s: peer-not-found", label);
                    tg_mtproto_chat_print_system_line(stream, peer_note);
                }
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
                continue;
            }
            if (peer_index[0] != '\0' &&
                strcmp(peer_index, requested_peer_index) != 0) {
                strcpy(prev_peer_index, peer_index);
                strcpy(prev_peer_label, peer_label);
            }
            strcpy(peer_index, requested_peer_index);
            strcpy(peer_label, requested_peer_label);
            last_seen_message_id = 0UL;
            peer_history_ready = 0;
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                fprintf(tui_cap, "Current chat: ");
                tg_mtproto_print_cache_text(tui_cap, peer_label);
                fprintf(tui_cap, "\n");
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_tui_status(peer_label);
            continue;
        }
        if (strcmp(line, "/read") == 0) {
            {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);
                rc = tg_mtproto_auth_print_history_text_peer_on_context(
                    host, port, api_id, auth_file, dc_id_text, &chat_context,
                    peer_cache_file, peer_index, "5", tui_cap,
                    &last_seen_message_id, 0, 0, 1, 1, peer_label, own_label);
                if (rc == TG_MTPROTO_QUERY_SOFT_FAIL) {
                    fprintf(tui_cap,
                            "No reply yet (slow link). Press Enter to retry.\n");
                } else if (rc != 0) {
                    fprintf(tui_cap,
                            "Could not read messages now (error %d).\n", rc);
                }
                tg_console_tui_capture_end(tui_cap, stream);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        cmd_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/forwardto", &cmd_arg)) {
            char forward_note[160];
            int forward_args;

            if (peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(
                    stream, "Choose a chat before forwarding a message.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            forward_destination[0] = '\0';
            forward_id_text[0] = '\0';
            forward_extra[0] = '\0';
            forward_args = cmd_arg != 0
                               ? sscanf(cmd_arg, "%31s %31s %1s",
                                        forward_destination, forward_id_text,
                                        forward_extra)
                               : 0;
            if (forward_args < 1 || forward_args > 2 ||
                tg_mtproto_chat_copy_peer_index(
                    requested_peer_index, sizeof(requested_peer_index),
                    forward_destination) != 0 ||
                tg_mtproto_load_peer_cache_label(
                    peer_cache_file, requested_peer_index,
                    requested_peer_label, sizeof(requested_peer_label)) != 0) {
                tg_mtproto_chat_print_system_line(
                    stream,
                    "Use /forwardto <chat-number> [message-id].");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            forward_message_id = last_seen_message_id;
            if (forward_args == 2 &&
                (tg_mtproto_parse_ulong_arg(forward_id_text,
                                            &forward_message_id) != 0 ||
                 forward_message_id == 0UL)) {
                tg_mtproto_chat_print_system_line(
                    stream,
                    "Use /forwardto <chat-number> [message-id].");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            if (forward_message_id == 0UL) {
                tg_mtproto_chat_print_system_line(
                    stream, "No recent message is available to forward.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            quiet = tg_mtproto_open_quiet_stream(stream);
            rc = tg_mtproto_auth_forward_peer_on_context(
                host, port, api_id, auth_file, dc_id_text, &chat_context,
                peer_cache_file, peer_index, requested_peer_index,
                forward_message_id, quiet);
            tg_mtproto_close_quiet_stream(quiet, stream);
            if (rc == 0) {
                FILE *tui_cap = tg_console_tui_capture_begin(stream);

                fprintf(tui_cap, "Forwarded to ");
                tg_mtproto_print_cache_text(tui_cap, requested_peer_label);
                fprintf(tui_cap, ".\n");
                tg_console_tui_capture_end(tui_cap, stream);
            } else {
                sprintf(forward_note, "Could not forward message: %.80s",
                        tg_mtproto_query_fail[0] != '\0'
                            ? tg_mtproto_query_fail : "no reply");
                tg_mtproto_chat_print_system_line(stream, forward_note);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        cmd_arg = 0;
        if (tg_mtproto_chat_named_command_arg(line, "/forward", &cmd_arg)) {
            char forward_note[128];

            if (peer_index[0] == '\0') {
                tg_mtproto_chat_print_system_line(
                    stream, "Choose a chat before forwarding a message.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            forward_message_id = last_seen_message_id;
            if (cmd_arg != 0 && cmd_arg[0] != '\0' &&
                (tg_mtproto_parse_ulong_arg(cmd_arg, &forward_message_id) != 0 ||
                 forward_message_id == 0UL)) {
                tg_mtproto_chat_print_system_line(
                    stream, "Use /forward or /forward <message-id>.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            if (forward_message_id == 0UL) {
                tg_mtproto_chat_print_system_line(
                    stream, "No recent message is available to forward.");
                tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                            0UL, tg_chat_input_raw);
                continue;
            }
            quiet = tg_mtproto_open_quiet_stream(stream);
            rc = tg_mtproto_auth_forward_peer_on_context(
                host, port, api_id, auth_file, dc_id_text, &chat_context,
                peer_cache_file, peer_index, "self", forward_message_id,
                quiet);
            tg_mtproto_close_quiet_stream(quiet, stream);
            if (rc == 0) {
                tg_mtproto_chat_print_system_line(
                    stream, "Forwarded to Saved Messages.");
            } else {
                sprintf(forward_note, "Could not forward message: %.80s",
                        tg_mtproto_query_fail[0] != '\0'
                            ? tg_mtproto_query_fail : "no reply");
                tg_mtproto_chat_print_system_line(stream, forward_note);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if ((strncmp(line, "/watch", 6) == 0 &&
             (line[6] == '\0' || line[6] == ' ' || line[6] == '\t')) ||
            (strncmp(line, "watch", 5) == 0 &&
             (line[5] == '\0' || line[5] == ' ' || line[5] == '\t'))) {
            if (tg_console_parse_watch_command(line, &parsed_watch_seconds) !=
                0) {
                tg_mtproto_chat_print_system_line(
                    stream, "Use /watch <seconds <= 3600> or /watch off.");
                continue;
            }
            watch_seconds = parsed_watch_seconds;
            if (watch_seconds == 0UL) {
                tg_mtproto_chat_print_system_line(stream,
                                                  "Auto-read disabled.");
            } else {
                char watch_note[64];
                sprintf(watch_note, "Auto-read every %lu second(s).",
                        watch_seconds);
                tg_mtproto_chat_print_system_line(stream, watch_note);
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        if (peer_index[0] == '\0') {
            tg_mtproto_chat_print_system_line(
                stream, "Choose a chat first with /peers or /add name.");
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        quiet = tg_mtproto_open_quiet_stream(stream);
        {
            const char *send_text = line;
#if TG_MTPROTO_DISPLAY_LATIN1
            /* The typed line is ISO-8859-1 (Amiga keymap); convert to UTF-8 so
               accented characters reach Telegram intact. On overflow fall back
               to the raw line (best effort). */
            if (tg_mtproto_latin1_to_utf8(line, send_line, sizeof(send_line))) {
                send_text = send_line;
            }
#endif
            rc = tg_mtproto_auth_send_peer_on_context(
                host, port, api_id, auth_file, dc_id_text, &chat_context,
                peer_cache_file, peer_index, send_text, 0UL, &sent_message_id,
                quiet);
        }
        if (rc == 0) {
            consecutive_failures = 0UL;
        } else if (++consecutive_failures >= TG_MTPROTO_CHAT_STALL_LIMIT) {
            tg_mtproto_close_auth_context(&chat_context);
            consecutive_failures = 0UL;
        }
        if (rc != 0) {
            /* Surface the captured send diagnostics (peer kind/access_hash,
               build or RPC error) so a failed send is not just an opaque
               "Could not send message." -- needed to diagnose the supergroup
               send failure on the stock A1200. */
            tg_mtproto_replay_quiet_stream(quiet, stream);
            tg_mtproto_close_quiet_stream(quiet, stream);
            if (rc == TG_MTPROTO_QUERY_SOFT_FAIL) {
                tg_mtproto_chat_print_system_line(
                    stream, "Message not confirmed. Press Enter to refresh.");
            } else {
                tg_mtproto_chat_print_system_line(stream,
                                                  "Could not send message.");
            }
            tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
            continue;
        }
        tg_mtproto_close_quiet_stream(quiet, stream);
        if (sent_message_id > last_seen_message_id) {
            last_seen_message_id = sent_message_id;
        }
        /* Confirm delivery with a compact check marker on its own line instead
           of re-printing the whole message. Re-printing the text (plus the
           auto-read poll echoing our own outgoing message) made it look like
           the typed line was repeated several times. The console already
           echoed what was typed, so a small marker is enough. ISO-8859-1 has
           no real check glyph, so Latin-1 consoles get a "sent" guillemet;
           UTF-8 displays get the true checkmark. */
        {
            FILE *tui_cap = tg_console_tui_capture_begin(stream);
            tg_console_ui_role(tui_cap, TG_UI_ROLE_MARKER);
            if (tg_mtproto_display_utf8()) {
                fputs("[\xe2\x9c\x93]", tui_cap); /* [(U+2713)] */
            } else {
                fputs("[ok]", tui_cap); /* friendliest Latin-1 marker */
            }
            tg_console_ui_reset(tui_cap);
            fputc('\n', tui_cap);
            tg_console_tui_capture_end(tui_cap, stream);
        }
        tg_mtproto_chat_show_prompt(stream, own_label, peer_label, 0,
                                        0UL, tg_chat_input_raw);
    }
}

int tg_mtproto_auth_forget(const char *auth_file,
                           const char *code_hash_file,
                           FILE *stream)
{
    int removed;

    if (stream == 0 || auth_file == 0 || auth_file[0] == '\0') {
        if (stream != 0) {
            fputs("mtproto auth.forget: invalid-arguments\n", stream);
        }
        return 2;
    }
    removed = 0;
    if (remove(auth_file) == 0) {
        ++removed;
    }
    if (code_hash_file != 0 && code_hash_file[0] != '\0' &&
        remove(code_hash_file) == 0) {
        ++removed;
    }
    fprintf(stream, "mtproto auth.forget: removed %d file(s)\n", removed);
    return 0;
}

tg_mtproto_tl_status tg_mtproto_build_req_pq_multi(
    tg_mtproto_tl_writer *writer,
    unsigned long message_id_hi,
    unsigned long message_id_lo,
    const unsigned char nonce[16])
{
    unsigned char body[20];
    tg_mtproto_tl_writer body_writer;
    tg_mtproto_tl_status status;

    if (nonce == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    tg_mtproto_tl_writer_init(&body_writer, body, sizeof(body));
    status = tg_mtproto_tl_write_u32(&body_writer, 0xbe7e8ef1UL);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    status = tg_mtproto_tl_write_raw(&body_writer, nonce, 16);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }

    return tg_mtproto_write_plain_message(writer, message_id_hi, message_id_lo,
                                          body, body_writer.length);
}

/* Non-interactive 2FA cost breakdown for slow CPUs. Times the two parts of an
   SRP password check in isolation: the fixed PBKDF2-HMAC-SHA512 100000-iteration
   x-derivation, and a 2048-bit modexp with a full vs a 256-bit exponent (the
   g^a / base^(a+u*x) lever). No network, no auth files. */
int tg_mtproto_2fa_bench(FILE *stream)
{
    unsigned char password[16];
    unsigned char salt[8];
    unsigned char derived[TG_MTPROTO_SHA512_LENGTH];
    unsigned char base[TG_MTPROTO_BIGINT_SIZE];
    unsigned char modulus[TG_MTPROTO_BIGINT_SIZE];
    unsigned char exp_full[TG_MTPROTO_BIGINT_SIZE];
    unsigned char exp_short[TG_MTPROTO_BIGINT_SIZE];
    unsigned char result[TG_MTPROTO_BIGINT_SIZE];
    unsigned long t0;
    unsigned long t1;
    unsigned int i;

    if (stream == 0) {
        return 2;
    }

    for (i = 0U; i < sizeof(password); ++i) {
        password[i] = (unsigned char)(i + 1U);
    }
    for (i = 0U; i < sizeof(salt); ++i) {
        salt[i] = (unsigned char)(0x10U + i);
    }
    for (i = 0U; i < TG_MTPROTO_BIGINT_SIZE; ++i) {
        base[i] = (unsigned char)((i * 7U) + 1U);
        modulus[i] = (unsigned char)((i * 5U) + 3U);
        exp_full[i] = (unsigned char)((i * 3U) + 9U);
    }
    base[0] = 0x00U;                                  /* base < modulus */
    modulus[0] |= 0x80U;                              /* full 2048-bit */
    modulus[TG_MTPROTO_BIGINT_SIZE - 1U] |= 0x01U;    /* odd modulus */
    memset(exp_short, 0, sizeof(exp_short));
    for (i = 0U; i < 32U; ++i) {
        exp_short[TG_MTPROTO_BIGINT_SIZE - 32U + i] =
            (unsigned char)((i * 3U) + 9U);
    }
    exp_short[TG_MTPROTO_BIGINT_SIZE - 32U] |= 0x80U; /* 256-bit value */

    fprintf(stream, "2fa bench: PBKDF2-HMAC-SHA512 100000 iters...\n");
    fflush(stream);
    t0 = (unsigned long)time(0);
    tg_mtproto_pbkdf2_hmac_sha512(password, sizeof(password), salt,
                                  sizeof(salt), 100000UL, derived,
                                  sizeof(derived));
    t1 = (unsigned long)time(0);
    fprintf(stream, "2fa bench: PBKDF2 100000 = %lus\n",
            (unsigned long)(t1 - t0));
    fflush(stream);

    fprintf(stream, "2fa bench: modexp 2048-bit exponent (2 ops)...\n");
    fflush(stream);
    t0 = (unsigned long)time(0);
    for (i = 0U; i < 2U; ++i) {
        tg_mtproto_bigint_mod_exp(base, exp_full, TG_MTPROTO_BIGINT_SIZE,
                                  modulus, result);
    }
    t1 = (unsigned long)time(0);
    fprintf(stream, "2fa bench: modexp 2048-bit exp = %lus / 2 ops\n",
            (unsigned long)(t1 - t0));
    fflush(stream);

    fprintf(stream, "2fa bench: modexp 256-bit exponent (2 ops)...\n");
    fflush(stream);
    t0 = (unsigned long)time(0);
    for (i = 0U; i < 2U; ++i) {
        tg_mtproto_bigint_mod_exp(base, exp_short, TG_MTPROTO_BIGINT_SIZE,
                                  modulus, result);
    }
    t1 = (unsigned long)time(0);
    fprintf(stream, "2fa bench: modexp 256-bit exp = %lus / 2 ops\n",
            (unsigned long)(t1 - t0));
    fflush(stream);

    fputs("2fa bench: done\n", stream);
    fflush(stream);
    return 0;
}

int tg_mtproto_req_pq_probe(const char *host, const char *port, FILE *stream)
{
    unsigned char nonce[16];
    unsigned char payload[64];
    unsigned char packet[80];
    unsigned char response[1024];
    unsigned long payload_length;
    unsigned long response_length;
    unsigned long constructor;
    unsigned long p;
    unsigned long q;
    unsigned int i;
    tg_mtproto_message_id msg_id;
    tg_mtproto_res_pq res_pq;
    tg_mtproto_tl_writer writer;
    tg_net_connection connection;
    tg_net_status net_status;
    char error_buffer[160];

    if (host == 0 || port == 0 || stream == 0) {
        return 2;
    }

    tg_mtproto_probe_nonce(nonce);
    tg_mtproto_client_message_id((unsigned long)time(0), 4UL, 0, &msg_id);

    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_build_req_pq_multi(&writer, msg_id.hi, msg_id.lo, nonce) !=
        TG_MTPROTO_TL_OK) {
        fputs("mtproto req_pq probe: packet-build-failed\n", stream);
        return 2;
    }
    payload_length = writer.length;

    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
            TG_MTPROTO_TL_OK) {
        fputs("mtproto req_pq probe: transport-build-failed\n", stream);
        return 2;
    }

    error_buffer[0] = '\0';
    net_status = tg_net_connect(&connection, host, port, error_buffer,
                                sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "mtproto req_pq probe: connect-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    net_status = tg_mtproto_send_all(&connection, packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(&connection, response,
                                                     sizeof(response),
                                                     &response_length,
                                                     error_buffer,
                                                     sizeof(error_buffer));
    }
    tg_net_close(&connection);

    if (net_status != TG_NET_OK) {
        fprintf(stream, "mtproto req_pq probe: transport-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    constructor = 0;
    if (response_length >= 24UL) {
        constructor = tg_mtproto_read_u32_le(response + 20);
    }

    fprintf(stream,
            "mtproto req_pq probe: received %lu bytes, constructor 0x%08lx\n",
            response_length, constructor);

    if (constructor != 0x05162463UL) {
        return 2;
    }
    if (tg_mtproto_parse_res_pq(response, response_length, &res_pq) !=
            TG_MTPROTO_TL_OK ||
        !tg_mtproto_res_pq_nonce_matches(&res_pq, nonce)) {
        fputs("mtproto req_pq probe: resPQ-parse-failed\n", stream);
        return 2;
    }

    fprintf(stream,
            "mtproto req_pq probe: pq-bytes %lu, fingerprints %u\n",
            res_pq.pq_length, res_pq.fingerprint_count);
    for (i = 0U; i < res_pq.fingerprint_count; ++i) {
        fprintf(stream, "mtproto req_pq probe: fingerprint[%u] 0x%08lx%08lx\n",
                i, res_pq.fingerprints[i].hi, res_pq.fingerprints[i].lo);
    }
    if (tg_mtproto_pq_factor(res_pq.pq, res_pq.pq_length, &p, &q) != 0) {
        fputs("mtproto req_pq probe: pq-factor-failed\n", stream);
        return 2;
    }
    fprintf(stream, "mtproto req_pq probe: p %lu q %lu\n", p, q);

    return 0;
}

int tg_mtproto_req_dh_probe(const char *host, const char *port,
                            const char *dc_id_text, FILE *stream)
{
    unsigned char nonce[16];
    unsigned char new_nonce[32];
    unsigned char padding[96];
    unsigned char temp_key[32];
    unsigned char p_bytes[4];
    unsigned char q_bytes[4];
    unsigned char inner_data[160];
    unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH];
    unsigned char client_encrypted[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    unsigned char b[TG_MTPROTO_DH_VALUE_MAX];
    unsigned char client_padding[15];
    unsigned char session_id[8];
    unsigned char ping_id_bytes[8];
    unsigned char encrypted_padding[64];
    unsigned char body[384];
    unsigned char payload[512];
    unsigned char packet[600];
    unsigned char response[1200];
    unsigned long body_length;
    unsigned long client_encrypted_length;
    unsigned long payload_length;
    unsigned long encrypted_padding_length;
    unsigned long response_length;
    unsigned long constructor;
    unsigned long ping_id_hi;
    unsigned long ping_id_lo;
    unsigned long p;
    unsigned long q;
    unsigned int i;
    long dc_id;
    tg_mtproto_message_id first_msg_id;
    tg_mtproto_message_id second_msg_id;
    tg_mtproto_message_id third_msg_id;
    tg_mtproto_res_pq res_pq;
    tg_mtproto_server_dh_params_ok params_ok;
    tg_mtproto_server_dh_inner_data inner;
    tg_mtproto_set_client_dh_answer dh_answer;
    static tg_mtproto_encrypted_message decrypted;
    tg_mtproto_session session;
    tg_mtproto_tl_writer writer;
    tg_net_connection connection;
    tg_net_status net_status;
    const tg_mtproto_public_key *public_key;
    char error_buffer[160];

    if (host == 0 || port == 0 || stream == 0 ||
        tg_mtproto_parse_dc_id(dc_id_text, &dc_id) != 0) {
        fputs("mtproto req_DH_params probe: invalid-arguments\n", stream);
        return 2;
    }

    tg_mtproto_probe_nonce(nonce);
    tg_mtproto_client_message_id((unsigned long)time(0), 4UL, 0,
                                 &first_msg_id);

    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_build_req_pq_multi(&writer, first_msg_id.hi,
                                      first_msg_id.lo, nonce) !=
        TG_MTPROTO_TL_OK) {
        fputs("mtproto req_DH_params probe: req_pq-build-failed\n", stream);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
            TG_MTPROTO_TL_OK) {
        fputs("mtproto req_DH_params probe: req_pq-transport-build-failed\n",
              stream);
        return 2;
    }

    error_buffer[0] = '\0';
    net_status = tg_net_connect(&connection, host, port, error_buffer,
                                sizeof(error_buffer));
    if (net_status != TG_NET_OK) {
        fprintf(stream, "mtproto req_DH_params probe: connect-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    net_status = tg_mtproto_send_all(&connection, packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(&connection, response,
                                                     sizeof(response),
                                                     &response_length,
                                                     error_buffer,
                                                     sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        tg_net_close(&connection);
        fprintf(stream, "mtproto req_DH_params probe: req_pq-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    constructor = response_length >= 24UL ?
        tg_mtproto_read_u32_le(response + 20) : 0UL;
    if (constructor != 0x05162463UL ||
        tg_mtproto_parse_res_pq(response, response_length, &res_pq) !=
            TG_MTPROTO_TL_OK ||
        !tg_mtproto_res_pq_nonce_matches(&res_pq, nonce) ||
        tg_mtproto_pq_factor(res_pq.pq, res_pq.pq_length, &p, &q) != 0) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: resPQ-parse-failed\n", stream);
        return 2;
    }

    public_key = tg_mtproto_select_public_key(&res_pq);
    if (public_key == 0) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: rsa-key-not-found\n", stream);
        return 2;
    }

    tg_mtproto_u32_be(p, p_bytes);
    tg_mtproto_u32_be(q, q_bytes);
    tg_mtproto_probe_random(new_nonce, sizeof(new_nonce));
    tg_mtproto_probe_random(padding, sizeof(padding));
    tg_mtproto_probe_random(temp_key, sizeof(temp_key));

    tg_mtproto_tl_writer_init(&writer, inner_data, sizeof(inner_data));
    if (tg_mtproto_build_p_q_inner_data_dc(&writer, res_pq.pq,
                                           res_pq.pq_length, p_bytes,
                                           sizeof(p_bytes), q_bytes,
                                           sizeof(q_bytes), nonce,
                                           res_pq.server_nonce, new_nonce,
                                           dc_id) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: inner-build-failed\n", stream);
        return 2;
    }

    for (i = 0U; i < 32U; ++i) {
        if (tg_mtproto_rsa_pad(inner_data, writer.length, padding, temp_key,
                               public_key, encrypted_data) ==
            TG_MTPROTO_TL_OK) {
            break;
        }
        tg_mtproto_probe_random(temp_key, sizeof(temp_key));
    }
    if (i == 32U) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: rsa-pad-failed\n", stream);
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_build_req_dh_params(&writer, nonce, res_pq.server_nonce,
                                       p_bytes, sizeof(p_bytes), q_bytes,
                                       sizeof(q_bytes),
                                       &public_key->fingerprint,
                                       encrypted_data) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: req-dh-build-failed\n", stream);
        return 2;
    }
    body_length = writer.length;
    tg_mtproto_client_message_id((unsigned long)time(0), 8UL, &first_msg_id,
                                 &second_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_plain_message(&writer, second_msg_id.hi,
                                       second_msg_id.lo, body,
                                       body_length) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: envelope-build-failed\n", stream);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: transport-build-failed\n", stream);
        return 2;
    }

    net_status = tg_mtproto_send_all(&connection, packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(&connection, response,
                                                     sizeof(response),
                                                     &response_length,
                                                     error_buffer,
                                                     sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        tg_net_close(&connection);
        fprintf(stream, "mtproto req_DH_params probe: req-dh-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    constructor = response_length >= 24UL ?
        tg_mtproto_read_u32_le(response + 20) : 0UL;
    fprintf(stream,
            "mtproto req_DH_params probe: received %lu bytes, constructor 0x%08lx\n",
            response_length, constructor);
    if (constructor != 0xd0e8075cUL ||
        tg_mtproto_parse_server_dh_params_ok(response, response_length,
                                             &params_ok) != TG_MTPROTO_TL_OK ||
        memcmp(params_ok.nonce, nonce, 16U) != 0 ||
        memcmp(params_ok.server_nonce, res_pq.server_nonce, 16U) != 0 ||
        tg_mtproto_decrypt_server_dh_inner_data(
            params_ok.encrypted_answer, params_ok.encrypted_answer_length,
            new_nonce, nonce, res_pq.server_nonce, &inner) !=
            TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: server-dh-parse-failed\n", stream);
        return 2;
    }

    fprintf(stream,
            "mtproto req_DH_params probe: g %lu, dh_prime %lu bytes, g_a %lu bytes, server_time %lu\n",
            inner.g, inner.dh_prime_length, inner.g_a_length,
            inner.server_time);

    if (!tg_mtproto_check_dh_params(&inner)) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: dh-params-check-failed\n",
              stream);
        return 2;
    }
    tg_mtproto_probe_random(b, sizeof(b));
    tg_mtproto_probe_random(client_padding, sizeof(client_padding));
    if (tg_mtproto_build_client_dh_request(&inner, new_nonce, b,
                                           client_padding, client_encrypted,
                                           &client_encrypted_length,
                                           auth_key) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: client-dh-build-failed\n",
              stream);
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_build_set_client_dh_params(
            &writer, nonce, res_pq.server_nonce, client_encrypted,
            client_encrypted_length) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: set-client-dh-build-failed\n",
              stream);
        return 2;
    }
    body_length = writer.length;
    tg_mtproto_client_message_id((unsigned long)time(0), 12UL,
                                 &second_msg_id, &third_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_plain_message(&writer, third_msg_id.hi,
                                       third_msg_id.lo, body,
                                       body_length) != TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: set-client-envelope-failed\n",
              stream);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: set-client-transport-build-failed\n",
              stream);
        return 2;
    }
    net_status = tg_mtproto_send_all(&connection, packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(&connection, response,
                                                     sizeof(response),
                                                     &response_length,
                                                     error_buffer,
                                                     sizeof(error_buffer));
    }
    if (net_status != TG_NET_OK) {
        tg_net_close(&connection);
        fprintf(stream, "mtproto req_DH_params probe: set-client-dh-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }

    constructor = response_length >= 24UL ?
        tg_mtproto_read_u32_le(response + 20) : 0UL;
    fprintf(stream,
            "mtproto req_DH_params probe: final received %lu bytes, constructor 0x%08lx\n",
            response_length, constructor);
    if (tg_mtproto_parse_set_client_dh_answer(response, response_length,
                                              &dh_answer) !=
        TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: set-client-dh-parse-failed\n",
              stream);
        return 2;
    }
    if (!tg_mtproto_verify_dh_gen_ok(&dh_answer, nonce, res_pq.server_nonce,
                                     new_nonce, auth_key)) {
        fprintf(stream,
                "mtproto req_DH_params probe: dh-gen-not-ok constructor 0x%08lx\n",
                dh_answer.constructor);
        tg_net_close(&connection);
        return 2;
    }
    fputs("mtproto req_DH_params probe: dh_gen_ok, auth_key derived in memory only\n",
          stream);

    tg_mtproto_probe_random(session_id, sizeof(session_id));
    tg_mtproto_session_from_auth_key(&session, (unsigned long)dc_id, auth_key,
                                     new_nonce, res_pq.server_nonce,
                                     session_id);
    tg_mtproto_probe_random(ping_id_bytes, sizeof(ping_id_bytes));
    ping_id_lo = tg_mtproto_read_u32_le(ping_id_bytes);
    ping_id_hi = tg_mtproto_read_u32_le(ping_id_bytes + 4U);
    tg_mtproto_tl_writer_init(&writer, body, sizeof(body));
    if (tg_mtproto_tl_write_u32(&writer, 0x7abe77ecUL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, ping_id_hi, ping_id_lo) !=
            TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: ping-build-failed\n", stream);
        return 2;
    }
    body_length = writer.length;
    encrypted_padding_length = 12UL;
    while (((32UL + body_length + encrypted_padding_length) % 16UL) != 0UL) {
        ++encrypted_padding_length;
    }
    tg_mtproto_probe_random(encrypted_padding, encrypted_padding_length);
    tg_mtproto_client_message_id((unsigned long)time(0), 16UL,
                                 &third_msg_id, &third_msg_id);
    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_write_encrypted_message(
            &writer, auth_key, session.server_salt_hi,
            session.server_salt_lo, session.session_id, third_msg_id.hi,
            third_msg_id.lo, 1UL, body, body_length,
            encrypted_padding, encrypted_padding_length) !=
        TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: encrypted-ping-build-failed\n",
              stream);
        return 2;
    }
    payload_length = writer.length;
    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_packet(&writer, payload, payload_length) !=
        TG_MTPROTO_TL_OK) {
        tg_net_close(&connection);
        fputs("mtproto req_DH_params probe: encrypted-ping-transport-build-failed\n",
              stream);
        return 2;
    }
    net_status = tg_mtproto_send_all(&connection, packet, writer.length,
                                     error_buffer, sizeof(error_buffer));
    if (net_status == TG_NET_OK) {
        net_status = tg_mtproto_recv_abridged_packet(&connection, response,
                                                     sizeof(response),
                                                     &response_length,
                                                     error_buffer,
                                                     sizeof(error_buffer));
    }
    tg_net_close(&connection);
    if (net_status != TG_NET_OK) {
        fprintf(stream, "mtproto req_DH_params probe: encrypted-ping-failed (%s)\n",
                tg_net_status_name(net_status));
        return 2;
    }
    if (tg_mtproto_decrypt_encrypted_message(response, response_length,
                                             auth_key, &decrypted) !=
        TG_MTPROTO_TL_OK) {
        fputs("mtproto req_DH_params probe: encrypted-response-decrypt-failed\n",
              stream);
        return 2;
    }
    constructor = decrypted.body_length >= 4UL ?
        tg_mtproto_read_u32_le(decrypted.body) : 0UL;
    fprintf(stream,
            "mtproto req_DH_params probe: encrypted response %lu bytes, constructor 0x%08lx\n",
            decrypted.body_length, constructor);
    if (tg_mtproto_body_is_expected_pong(decrypted.body,
                                         decrypted.body_length, ping_id_hi,
                                         ping_id_lo) ||
        tg_mtproto_container_has_expected_pong(decrypted.body,
                                               decrypted.body_length,
                                               ping_id_hi, ping_id_lo)) {
        fputs("mtproto req_DH_params probe: encrypted ping pong ok\n",
              stream);
        return 0;
    }
    if (constructor == 0xedab447bUL) {
        fputs("mtproto req_DH_params probe: bad_server_salt received\n",
              stream);
        return 2;
    }
    fputs("mtproto req_DH_params probe: encrypted-ping-unexpected-response\n",
          stream);
    return 2;
}

#if !defined(TG_NO_SELFTEST)
static int tg_gui_hidden_projection_self_test(void);

#if !defined(TG_NO_SELFTEST)
static int tg_mtproto_executable_sniff_self_test(void); /* defined by the download engine */
static int tg_mtproto_photo_gate_self_test(void);       /* defined by the upload engine */
#endif

int tg_mtproto_probe_self_test(void)
{
    static const unsigned char nonce[16] = {
        0x79U, 0xf0U, 0xafU, 0xb5U, 0x02U, 0x52U, 0xe5U, 0xfcU,
        0x96U, 0x92U, 0x4bU, 0xfcU, 0xecU, 0xdaU, 0x4fU, 0x05U
    };
    static const unsigned char expected[] = {
        0xefU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x60U, 0x97U, 0x05U, 0x00U, 0xebU, 0xe5U, 0x77U, 0x67U,
        0x14U, 0x00U, 0x00U, 0x00U,
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x79U, 0xf0U, 0xafU, 0xb5U, 0x02U, 0x52U, 0xe5U, 0xfcU,
        0x96U, 0x92U, 0x4bU, 0xfcU, 0xecU, 0xdaU, 0x4fU, 0x05U
    };
    static const char password_path[] = "telegram-mtproto-password-self-test.tmp";
    static const char missing_password_path[] =
        "telegram-mtproto-password-missing-self-test.tmp";
    static const char password_text[] = "secret\r\n";
    static const char api_path[] = "telegram-mtproto-api-self-test.tmp";
    static const char missing_api_path[] =
        "telegram-mtproto-api-missing-self-test.tmp";
    static const char api_text[] = "\n 12345 \r\n abcdef0123456789 \n";
    static const char peer_path[] = "telegram-mtproto-peer-self-test.tmp";
    static const char peer_text[] =
        "mtproto-peer-cache-v1\n"
        "count 3 total_dialogs 3 users 1 chats 2\n"
        "peer 1 type user id 0x0000000000000001 access_hash 0x0000000000000002 top 0 unread 0 self no bot no username ada title Ada\n"
        "peer 2 type chat id 0x0000000000000003 access_hash - top 0 unread 0 self no bot no username - title Test Group\n"
        "peer 3 type channel id 0x0000000000000004 access_hash 0x0000000000000005 top 0 unread 0 self no bot no username - title Test Channel\n";
    unsigned char payload[128];
    unsigned char packet[160];
    char api_id[32];
    char api_hash[96];
    char password[16];
    unsigned long password_length;
    unsigned long peer_constructor;
    unsigned long peer_id_hi;
    unsigned long peer_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    FILE *quiet;
    tg_mtproto_tl_writer writer;

    tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
    if (tg_mtproto_build_req_pq_multi(&writer, 0x6777e5ebUL, 0x00059760UL,
                                      nonce) != TG_MTPROTO_TL_OK ||
        writer.length != 40UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_abridged_packet(&writer, payload, 40UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected) ||
        memcmp(packet, expected, sizeof(expected)) != 0) {
        return 2;
    }

    (void)remove(password_path);
    (void)remove(missing_password_path);
    if (tg_mtproto_load_password_file(missing_password_path, password,
                                      sizeof(password), &password_length,
                                      0, 0) == 0) {
        return 2;
    }
    if (tg_file_write_text(password_path, "", 0UL) != TG_FILE_OK ||
        tg_mtproto_load_password_file(password_path, password,
                                      sizeof(password), &password_length,
                                      0, 0) == 0) {
        (void)remove(password_path);
        return 2;
    }
    if (tg_file_write_text(password_path, password_text,
                           (unsigned long)strlen(password_text)) !=
            TG_FILE_OK ||
        tg_mtproto_load_password_file(password_path, password,
                                      sizeof(password), &password_length,
                                      0, 0) != 0 ||
        password_length != 6UL ||
        strcmp(password, "secret") != 0) {
        (void)remove(password_path);
        return 2;
    }
    tg_mtproto_secure_zero(password, sizeof(password));
    if (tg_mtproto_load_password_file(password_path, password, 4UL,
                                      &password_length, 0, 0) == 0) {
        (void)remove(password_path);
        return 2;
    }
    (void)remove(password_path);

    (void)remove(api_path);
    (void)remove(missing_api_path);
    if (tg_mtproto_load_api_credentials(missing_api_path, api_id,
                                        sizeof(api_id), api_hash,
                                        sizeof(api_hash), 0, 0) == 0) {
        return 2;
    }
    if (tg_file_write_text(api_path, "12345\n", 6UL) != TG_FILE_OK ||
        tg_mtproto_load_api_credentials(api_path, api_id, sizeof(api_id),
                                        api_hash, sizeof(api_hash),
                                        0, 0) == 0) {
        (void)remove(api_path);
        return 2;
    }
    if (tg_file_write_text(api_path, api_text,
                           (unsigned long)strlen(api_text)) != TG_FILE_OK ||
        tg_mtproto_load_api_credentials(api_path, api_id, sizeof(api_id),
                                        api_hash, sizeof(api_hash),
                                        0, 0) != 0 ||
        strcmp(api_id, "12345") != 0 ||
        strcmp(api_hash, "abcdef0123456789") != 0) {
        (void)remove(api_path);
        return 2;
    }
    if (tg_mtproto_load_api_id_file(api_path, api_id, sizeof(api_id),
                                    0, 0) != 0 ||
        strcmp(api_id, "12345") != 0) {
        (void)remove(api_path);
        return 2;
    }
    tg_mtproto_secure_zero(api_hash, sizeof(api_hash));
    (void)remove(api_path);

    quiet = tmpfile();
    if (quiet == 0) {
        return 2;
    }
    (void)remove(peer_path);
    if (tg_file_write_text(peer_path, peer_text,
                           (unsigned long)strlen(peer_text)) != TG_FILE_OK) {
        fclose(quiet);
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_path, "2", &peer_constructor,
                                        &peer_id_hi, &peer_id_lo,
                                        &access_hash_hi, &access_hash_lo,
                                        &has_access_hash, quiet,
                                        "peer-cache-self-test") != 0 ||
        peer_constructor != TG_MTPROTO_PEER_CHAT_CONSTRUCTOR ||
        peer_id_hi != 0UL || peer_id_lo != 3UL || has_access_hash) {
        fclose(quiet);
        (void)remove(peer_path);
        return 2;
    }
    if (tg_mtproto_load_peer_cache_peer(peer_path, "3", &peer_constructor,
                                        &peer_id_hi, &peer_id_lo,
                                        &access_hash_hi, &access_hash_lo,
                                        &has_access_hash, quiet,
                                        "peer-cache-self-test") != 0 ||
        peer_constructor != TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR ||
        peer_id_hi != 0UL || peer_id_lo != 4UL ||
        access_hash_hi != 0UL || access_hash_lo != 5UL ||
        !has_access_hash) {
        fclose(quiet);
        (void)remove(peer_path);
        return 2;
    }
    fclose(quiet);
    (void)remove(peer_path);

    /* Live "is typing" parse (updateShort -> *UserTyping -> typing action). The
       collector writes the sink; here we drive synthetic pushes through it. */
    {
        tg_chat_notify typing_nq;
        tg_chat_typing_sink typing_sink;
        tg_chat_edit_sink edit_sink;
        int ok;

        memset(&typing_nq, 0, sizeof(typing_nq));
        tg_chat_notify_reset(&typing_nq, 1);
        tg_chat_nq = &typing_nq;
        tg_chat_typing_target = &typing_sink;

        /* DM: updateShort{ updateUserTyping{ user=0xABCD, typing }, date }. */
        memset(&typing_sink, 0, sizeof(typing_sink));
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        ok = tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0xABCDUL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_SEND_MESSAGE_TYPING_ACTION_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000000UL) == TG_MTPROTO_TL_OK;
        if (ok) {
            tg_chat_notify_collect(payload, writer.length);
        }
        if (!ok || !typing_sink.active || typing_sink.is_chat != 0 ||
            typing_sink.peer_id_hi != 0UL ||
            typing_sink.peer_id_lo != 0xABCDUL ||
            typing_sink.from_id_lo != 0xABCDUL) { /* DM: typer == peer */
            tg_chat_nq = 0;
            tg_chat_typing_target = 0;
            puts("probe self-test: typing DM parse wrong");
            return 2;
        }

        /* A non-typing action (bogus action ctor) must NOT light typing. */
        memset(&typing_sink, 0, sizeof(typing_sink));
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        ok = tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x11UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 0xdeadbeefUL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000000UL) == TG_MTPROTO_TL_OK;
        if (ok) {
            tg_chat_notify_collect(payload, writer.length);
        }
        if (!ok || typing_sink.active) {
            tg_chat_nq = 0;
            tg_chat_typing_target = 0;
            puts("probe self-test: non-typing action must not light");
            return 2;
        }

        /* Basic group: updateChatUserTyping{ chat=0x900, from user, typing }. */
        memset(&typing_sink, 0, sizeof(typing_sink));
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        ok = tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x900UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_PEER_USER_CONSTRUCTOR) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x55UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer,
                 TG_MTPROTO_SEND_MESSAGE_TYPING_ACTION_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000000UL) == TG_MTPROTO_TL_OK;
        if (ok) {
            tg_chat_notify_collect(payload, writer.length);
        }
        if (!ok || !typing_sink.active || typing_sink.is_chat != 1 ||
            typing_sink.peer_id_lo != 0x900UL ||
            typing_sink.from_id_lo != 0x55UL) { /* group: typer = from user */
            tg_chat_nq = 0;
            tg_chat_typing_target = 0;
            puts("probe self-test: group typing parse wrong");
            return 2;
        }

        /* A remote edit in updateShort must preserve the destination peer and
           UTF-8 text for the GUI-side in-place replacement. */
        memset(&edit_sink, 0, sizeof(edit_sink));
        tg_chat_edit_target = &edit_sink;
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        ok = tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_UPDATE_SHORT_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, TG_MTPROTO_MESSAGE_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1UL << 8) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 0UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 77UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_PEER_USER_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x44UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_PEER_USER_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x99UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000000UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_bytes(
                 &writer, (const unsigned char *)"pi\xc3\xb9 recente", 12UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 10UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000001UL) ==
                 TG_MTPROTO_TL_OK;
        if (ok) {
            tg_chat_notify_collect(payload, writer.length);
        }
        if (!ok || edit_sink.count != 1UL ||
            edit_sink.queue[0].peer_constructor !=
                TG_MTPROTO_PEER_USER_CONSTRUCTOR ||
            edit_sink.queue[0].peer_id_lo != 0x99UL ||
            edit_sink.queue[0].message_id != 77UL ||
            strcmp(edit_sink.queue[0].text, "pi\xc3\xb9 recente") != 0) {
            tg_chat_nq = 0;
            tg_chat_typing_target = 0;
            tg_chat_edit_target = 0;
            printf("probe self-test: remote edit parse wrong "
                   "(ok=%d count=%lu peer=0x%08lx id=%lu text=%s)\n",
                   ok, edit_sink.count,
                   edit_sink.queue[0].peer_constructor,
                   edit_sink.queue[0].message_id,
                   edit_sink.queue[0].text);
            return 2;
        }

        /* Saved Messages commonly bundles another update before the edit.
           Verify that the aligned scan finds the second vector item and that
           PeerUser(own id) matches the open inputPeerSelf sentinel. */
        memset(&edit_sink, 0, sizeof(edit_sink));
        tg_mtproto_tl_writer_init(&writer, payload, sizeof(payload));
        ok = tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_UPDATES_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_TL_VECTOR_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 2UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer,
                 TG_MTPROTO_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_PEER_USER_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x44UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 70UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 10UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_UPDATE_EDIT_MESSAGE_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, TG_MTPROTO_MESSAGE_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1UL << 8) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 0UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 78UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_PEER_USER_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x44UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(
                 &writer, TG_MTPROTO_PEER_USER_CONSTRUCTOR) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u64(&writer, 0UL, 0x44UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1700000002UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_bytes(
                 &writer, (const unsigned char *)"saved newer", 11UL) ==
                 TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 11UL) == TG_MTPROTO_TL_OK &&
             tg_mtproto_tl_write_u32(&writer, 1UL) == TG_MTPROTO_TL_OK;
        if (ok) {
            tg_chat_notify_collect(payload, writer.length);
        }
        if (!ok || edit_sink.count != 1UL ||
            edit_sink.queue[0].message_id != 78UL ||
            strcmp(edit_sink.queue[0].text, "saved newer") != 0 ||
            !tg_chat_edit_peer_matches_open(
                &edit_sink.queue[0], TG_MTPROTO_PEER_SELF_CONSTRUCTOR,
                0UL, 0UL)) {
            tg_chat_nq = 0;
            tg_chat_typing_target = 0;
            tg_chat_edit_target = 0;
            puts("probe self-test: Saved Messages bundled edit wrong");
            return 2;
        }

        tg_chat_nq = 0;
        tg_chat_typing_target = 0;
        tg_chat_edit_target = 0;
    }

    /* Upload send-path guard (the twin of the download body/PACKET_MAX fix): a
       saveFilePart query IS a whole file chunk, so build_initialized_query must
       wrap a chunk-sized query without overflowing its buffers. A query at the
       per-platform send bound stands in for a full upload chunk; the pre-fix
       1.2 KB wrap buffers failed this and the upload reported "Upload failed". */
    {
        static unsigned char big_q[TG_MTPROTO_QUERY_SEND_MAX];
        static unsigned char big_out[TG_MTPROTO_QUERY_SEND_MAX];
        unsigned long big_len = TG_MTPROTO_QUERY_SEND_MAX - 2048U;
        tg_mtproto_tl_writer bw;
        unsigned long k;

        for (k = 0UL; k < big_len; ++k) {
            big_q[k] = (unsigned char)((k * 5U + 1U) & 0xffU);
        }
        if (tg_mtproto_build_initialized_query(&bw, big_out, sizeof(big_out),
                                               12345UL, big_q, big_len) != 0) {
            puts("probe self-test: chunk-sized query wrap overflowed");
            return 2;
        }
        /* ...and its ENCRYPTED form must fit a buffer the size of payload[] /
           packet[] in send_encrypted_query_limited (the rest of the send-path
           fix). Reuse big_q as the output; a dummy key/session is enough since
           we only check the writer does not overflow. */
        {
            static unsigned char akey[TG_MTPROTO_AUTH_KEY_LENGTH];
            unsigned char sid[8];
            unsigned char pad[64];
            unsigned long plen = 12UL;
            tg_mtproto_tl_writer ew;

            for (k = 0UL; k < sizeof(akey); ++k) {
                akey[k] = (unsigned char)k;
            }
            for (k = 0UL; k < sizeof(sid); ++k) {
                sid[k] = (unsigned char)(0x30U + k);
            }
            while (((32UL + bw.length + plen) % 16UL) != 0UL) {
                ++plen;
            }
            for (k = 0UL; k < plen; ++k) {
                pad[k] = (unsigned char)(0x60U + k);
            }
            tg_mtproto_tl_writer_init(&ew, big_q, sizeof(big_q));
            if (tg_mtproto_write_encrypted_message(
                    &ew, akey, 0x11111111UL, 0x22222222UL, sid, 0x33333333UL,
                    0x44444444UL, 1UL, big_out, bw.length, pad, plen) !=
                TG_MTPROTO_TL_OK) {
                puts("probe self-test: encrypted chunk exceeds send buffer");
                return 2;
            }
        }
    }

    if (tg_mtproto_executable_sniff_self_test() != 0) {
        return 2;
    }
    if (tg_mtproto_photo_gate_self_test() != 0) {
        return 2;
    }
    if (tg_gui_hidden_projection_self_test() != 0) {
        return 2;
    }

    /* Foreground viewer demand survives a full inline queue and is selected
       first. This pins the scheduler rule without opening a network context. */
    {
        tg_mtproto_photo_meta photo;
        tg_gui_photo_queue_entry next;
        int i;
        int large_found;

        tg_gui_photo_queue_reset();
        tg_gui_photo_inline_enabled = 1;
        for (i = 0; i < TG_GUI_PHOTO_QUEUE_MAX; ++i) {
            memset(&photo, 0, sizeof(photo));
            photo.has_photo = 1;
            photo.id_hi = 0xf0090000UL;
            photo.id_lo = (unsigned long)i + 1UL;
            strcpy(photo.thumb_type, "m");
            photo.width = photo.height = 64UL;
            photo.size = 1024UL;
            tg_gui_photo_queue_offer(&photo, 0);
        }
        memset(&photo, 0, sizeof(photo));
        photo.has_photo = 1;
        photo.id_hi = 0xf0090001UL;
        photo.id_lo = 1UL;
        strcpy(photo.thumb_type, "m");
        photo.width = photo.height = 64UL;
        photo.size = 1024UL;
        photo.has_large = 1;
        strcpy(photo.large_thumb_type, "x");
        photo.large_width = photo.large_height = 256UL;
        photo.large_size = 4096UL;
        tg_gui_photo_queue_offer(&photo, 1);
        photo.id_hi = 0xf0090002UL;
        photo.id_lo = 1UL;
        photo.has_large = 0;
        tg_gui_photo_queue_offer(&photo, 0);
        large_found = 0;
        for (i = 0; i < tg_gui_photo_queue_count; ++i) {
            if (tg_gui_photo_queue[i].large) {
                large_found = 1;
            }
        }
        if (!large_found || !tg_gui_session_photo_pending() ||
            !tg_gui_photo_queue_pop(&next) || !next.large) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: viewer photo queue priority wrong");
            return 2;
        }
        tg_gui_photo_queue_reset();
    }

    /* Save-as needs the original JPEG, not only the decoded RGB canonical.
       A .pgc that satisfies painting must therefore not suppress this demand. */
    {
        tg_mtproto_photo_meta photo;
        tg_gui_photo_queue_entry next;
        char canonical[64];
        FILE *file;
        unsigned long done;
        unsigned long total;
        int fixture_failed;

        tg_gui_photo_queue_reset();
        memset(&photo, 0, sizeof(photo));
        photo.has_photo = 1;
        photo.id_hi = 0xf00900a5UL;
        photo.id_lo = 0x5aUL;
        strcpy(photo.thumb_type, "m");
        photo.width = photo.height = 64UL;
        photo.size = 1024UL;
        photo.has_large = 1;
        strcpy(photo.large_thumb_type, "x");
        photo.large_width = photo.large_height = 256UL;
        photo.large_size = 4096UL;
        tg_gui_photo_catalog_offer(&photo);
        (void)mkdir("photos", 0777);
        if (tg_gui_session_photo_canonical_cache_path(
                canonical, sizeof(canonical), photo.id_hi, photo.id_lo, 1) !=
            0) {
            puts("probe self-test: photo save canonical path failed");
            return 2;
        }
        (void)remove(canonical);
        file = fopen(canonical, "wb");
        fixture_failed = file == 0;
        if (file != 0) {
            if (fputc(0, file) == EOF) {
                fixture_failed = 1;
            }
            if (fclose(file) != 0) {
                fixture_failed = 1;
            }
        }
        if (fixture_failed) {
            (void)remove(canonical);
            puts("probe self-test: photo save canonical fixture failed");
            return 2;
        }
        done = total = 0UL;
        if (tg_gui_session_request_photo_jpeg(
                photo.id_hi, photo.id_lo, 1) != 1 ||
            !tg_gui_session_photo_fetch_progress(
                photo.id_hi, photo.id_lo, 1, &done, &total) ||
            done != 0UL || total != photo.large_size ||
            !tg_gui_photo_queue_pop(&next) || !next.large ||
            !next.require_jpeg) {
            (void)remove(canonical);
            tg_gui_photo_queue_reset();
            puts("probe self-test: photo save JPEG demand was suppressed");
            return 2;
        }
        (void)remove(canonical);
        tg_gui_photo_queue_reset();
    }

    /* Unsupported/corrupt sources are remembered by exact size type. A mixed
       catalog keeps the baseline primary, then walks smaller baseline sizes;
       a progressive-only photo exhausts cleanly without poisoning another id. */
    {
        tg_mtproto_photo_meta photo;
        tg_gui_photo_queue_entry next;

        tg_gui_photo_queue_reset();
        tg_gui_photo_inline_enabled = 1;
        memset(&photo, 0, sizeof(photo));
        photo.has_photo = 1;
        photo.id_hi = 0xf0091000UL;
        photo.id_lo = 1UL;
        strcpy(photo.thumb_type, "y");
        photo.width = 1280UL;
        photo.height = 800UL;
        photo.size = 900000UL;
        photo.has_large = 1;
        strcpy(photo.large_thumb_type, "y");
        photo.large_width = photo.width;
        photo.large_height = photo.height;
        photo.large_size = photo.size;
        photo.variant_count = 4UL;
        strcpy(photo.variants[0].type, "s");
        photo.variants[0].width = 80UL;
        photo.variants[0].height = 50UL;
        photo.variants[0].size = 10000UL;
        strcpy(photo.variants[1].type, "m");
        photo.variants[1].width = 320UL;
        photo.variants[1].height = 200UL;
        photo.variants[1].size = 50000UL;
        strcpy(photo.variants[2].type, "x");
        photo.variants[2].width = 800UL;
        photo.variants[2].height = 500UL;
        photo.variants[2].size = 500000UL;
        photo.variants[2].progressive = 1U;
        strcpy(photo.variants[3].type, "y");
        photo.variants[3].width = photo.width;
        photo.variants[3].height = photo.height;
        photo.variants[3].size = photo.size;
        tg_gui_photo_catalog_offer(&photo);
        if (!tg_gui_photo_queue_offer(&photo, 0) ||
            !tg_gui_photo_queue_pop(&next) ||
            strcmp(next.photo.thumb_type, "y") != 0 ||
            !next.progressive_skipped) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: baseline photo primary wrong");
            return 2;
        }
        tg_gui_photo_cache_variant_remember(photo.id_hi, photo.id_lo, 0,
                                            next.photo.thumb_type);
        tg_gui_session_photo_decode_failed(photo.id_hi, photo.id_lo);
        if (!tg_gui_photo_queue_pop(&next) ||
            strcmp(next.photo.thumb_type, "m") != 0) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: baseline photo first fallback wrong");
            return 2;
        }
        tg_gui_photo_cache_variant_remember(photo.id_hi, photo.id_lo, 0,
                                            next.photo.thumb_type);
        tg_gui_session_photo_decode_failed(photo.id_hi, photo.id_lo);
        if (!tg_gui_photo_queue_pop(&next) ||
            strcmp(next.photo.thumb_type, "s") != 0) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: baseline photo second fallback wrong");
            return 2;
        }
        tg_gui_photo_cache_variant_remember(photo.id_hi, photo.id_lo, 0,
                                            next.photo.thumb_type);
        tg_gui_session_photo_decode_failed(photo.id_hi, photo.id_lo);
        if (tg_gui_session_photo_pending()) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: exhausted baseline photo was requeued");
            return 2;
        }
        if (!tg_gui_photo_queue_offer(&photo, 1) ||
            !tg_gui_photo_queue_pop(&next) || !next.large ||
            strcmp(next.photo.thumb_type, "y") != 0) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: viewer baseline primary wrong");
            return 2;
        }
        tg_gui_photo_cache_variant_remember(photo.id_hi, photo.id_lo, 1,
                                            next.photo.thumb_type);
        tg_gui_session_photo_decode_failed_variant(photo.id_hi, photo.id_lo,
                                                   1);
        if (!tg_gui_photo_queue_pop(&next) || !next.large ||
            strcmp(next.photo.thumb_type, "m") != 0) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: viewer baseline fallback wrong");
            return 2;
        }

        memset(&photo, 0, sizeof(photo));
        photo.has_photo = 1;
        photo.id_hi = 0xf0091001UL;
        photo.id_lo = 1UL;
        strcpy(photo.thumb_type, "x");
        photo.width = 800UL;
        photo.height = 500UL;
        photo.size = 500000UL;
        photo.variant_count = 1UL;
        strcpy(photo.variants[0].type, "x");
        photo.variants[0].width = photo.width;
        photo.variants[0].height = photo.height;
        photo.variants[0].size = photo.size;
        photo.variants[0].progressive = 1U;
        tg_gui_photo_catalog_offer(&photo);
        if (!tg_gui_photo_queue_offer(&photo, 0) ||
            !tg_gui_photo_queue_pop(&next) ||
            strcmp(next.photo.thumb_type, "x") != 0) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: progressive-only primary missing");
            return 2;
        }
        tg_gui_photo_cache_variant_remember(photo.id_hi, photo.id_lo, 0,
                                            next.photo.thumb_type);
        tg_gui_session_photo_decode_failed(photo.id_hi, photo.id_lo);
        if (tg_gui_session_photo_pending()) {
            tg_gui_photo_queue_reset();
            puts("probe self-test: progressive-only photo did not exhaust");
            return 2;
        }
        tg_gui_photo_queue_reset();
    }

    return 0;
}

int tg_mtproto_console_ui_test(FILE *stream)
{
    /* UTF-8 literals kept as escapes so the source stays pure ASCII:
       heart+VS16, tears of joy, thumbs up, flag IT (two regional
       indicators), check mark, right arrow, fire, ellipsis. */
    static const char sample_emoji[] =
        "\xe2\x9d\xa4\xef\xb8\x8f \xf0\x9f\x98\x82 \xf0\x9f\x91\x8d "
        "\xf0\x9f\x87\xae\xf0\x9f\x87\xb9 \xe2\x9c\x93 \xe2\x86\x92 "
        "\xf0\x9f\x94\xa5 \xe2\x80\xa6";
    static const char sample_accents[] =
        "perch\xc3\xa9 c'\xc3\xa8 gi\xc3\xa0 l\xc3\xac";
    int pen;

    if (stream == 0) {
        return 2;
    }
    /* The whole point is to look at the output, so force colours on. */
    tg_console_ui_set_interactive(1);

    fprintf(stream, "console ui test\n");
    fprintf(stream, "display layer: %s, charset: %s\n\n",
            TG_MTPROTO_DISPLAY_LATIN1 ? "latin1-transcode" : "raw",
            tg_mtproto_display_utf8() ? "utf8" : "latin1");

    fputs("fg pens: ", stream);
    for (pen = 0; pen < 8; ++pen) {
        fprintf(stream, TG_UI_CSI "3%dm%d" TG_UI_CSI "0m ", pen, pen);
    }
    fputs("\nbg pens: ", stream);
    for (pen = 0; pen < 8; ++pen) {
        /* Each background pen with both white and black text on top, so the
           tester can tell which pen is the solid black (expected: pen 1). */
        fprintf(stream, TG_UI_CSI "32;4%dm%d" TG_UI_CSI "30;4%dm%d" TG_UI_CSI "0m ", pen, pen, pen,
                pen);
    }
    fputs("\nattrs: ", stream);
    fputs(TG_UI_CSI "1mbold" TG_UI_CSI "0m ", stream);
    fputs(TG_UI_CSI "4munderline" TG_UI_CSI "0m ", stream);
    fputs(TG_UI_CSI "7minverse" TG_UI_CSI "0m\n\n", stream);

    fputs("dark theme preview (chat default):\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_RESET);
    fputs("plain message text" TG_UI_CSI "K\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_PEER);
    fputs("Mario Rossi:", stream);
    tg_console_ui_reset(stream);
    fputs(" testo dal contatto" TG_UI_CSI "K\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_OWN);
    fputs("me:", stream);
    tg_console_ui_reset(stream);
    fputs(" testo mio" TG_UI_CSI "K\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_SYSTEM);
    fputs("Loading chats... (system)", stream);
    tg_console_ui_reset(stream);
    fputs(TG_UI_CSI "K\n", stream);
    fputs(TG_UI_CSI "0m\n", stream);

    fputs("alerts:\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_NOTIFY);
    fputs("[2] Mario: nuovo messaggio (notify)", stream);
    fputs(TG_UI_CSI "0m\n", stream);
    tg_console_ui_role(stream, TG_UI_ROLE_MARKER);
    if (tg_mtproto_display_utf8()) {
        fputs("[\xe2\x9c\x93]", stream);
    } else {
        fputs("[ok]", stream);
    }
    tg_console_ui_reset(stream);
    fputs(TG_UI_CSI "0m (send marker)\n\n", stream);

    fputs("glyphs: ", stream);
    if (tg_mtproto_display_utf8()) {
        /* Same glyphs, UTF-8 encoded for raw displays. */
        fputs("\xc2\xbb \xc2\xab \xc2\xa4 \xc2\xb7 \xc2\xb1 \xc3\x97 "
              "\xc3\xb7\n", stream);
    } else {
        fputs("\xbb \xab \xa4 \xb7 \xb1 \xd7 \xf7\n", stream);
    }

    fputs("emoji mapping: ", stream);
    tg_mtproto_print_cache_text(stream, sample_emoji);
    fputc('\n', stream);
    fputs("accents: ", stream);
    tg_mtproto_print_cache_text(stream, sample_accents);
    fputc('\n', stream);
    fflush(stream);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

/* --- Live GUI session bridge (slice 3) --------------------------------------
 *
 * A non-interactive front-end over the same chat core the console uses, kept in
 * this translation unit so it reaches the static network helpers directly. It
 * holds an authenticated connection for the window's lifetime and drains
 * getDifference on demand, dispatching harvested notifications to the GUI
 * driver (which bumps + flashes the matching sidebar badge). The console chat
 * path is untouched; this is all new, additive code. Per-chat live history and
 * sending land in later slices. One session per process (the window owns it),
 * so the state is a file-static singleton.
 */
static struct {
    tg_mtproto_auth_context context;
    tg_chat_engine engine;
    tg_gui_chat_driver gui_driver;
    tg_chat_driver driver;
    const char *host;
    const char *port;
    const char *dc_id_text;
    const char *auth_file;
    const char *api_file;      /* for the on-demand dialog reload */
    const char *peer_cache_file;
    char api_id[32];
    char own_label[128];
    char current_peer_index[32];   /* the open chat (1-based number); "" = none */
    char current_peer_label[128];
    unsigned long last_seen_message_id;
    unsigned long saved_timeout;
    unsigned long diff_tick;
    unsigned long read_outbox_tick; /* cadence for the read-receipt refresh */
    /* Real-time read receipt (5c): an updateReadHistoryOutbox push records its
       (peer, max_id) here; the loop applies it next tick when the peer matches
       the open chat (the poll backstop still runs). Armed at open. */
    tg_chat_read_outbox_sink read_outbox_sink;
    tg_chat_edit_sink edit_sink;   /* remote message edits, armed at open */
    tg_chat_typing_sink typing;     /* live "is typing" sink, armed at open */
    unsigned long open_peer_constructor;
    unsigned long open_peer_id_hi;  /* the open chat's peer id, to match typing */
    unsigned long open_peer_id_lo;
    int open_peer_is_chat;
    /* Group member id->name cache for the typing indicator: lazily filled from
       channels.getParticipants(recent) when an open supergroup's typer cannot be
       resolved from the loaded transcript. member_fetch_done = attempted for THIS
       open group (one shot, reset on chat switch); member_for_* = the group it
       holds, so a re-open of a different group re-fetches. */
    tg_mtproto_peer_cache member_cache;
    int member_fetch_done;
    unsigned long member_for_id_hi;
    unsigned long member_for_id_lo;
    int open;
    /* First-login flow state (no saved session). Threaded across the phone ->
       code -> 2FA steps the window drives. */
    struct {
        int active;
        const char *api_file;       /* paths the eventual session reopens with */
        const char *auth_file;
        const char *cache_file;
        const char *host;           /* resolved endpoint (may change on migrate) */
        const char *dc_id_text;
        char api_id[32];            /* loaded once; api_hash stays transient */
        char phone[64];             /* sanitized, set by send_code, used by sign_in */
        char code_hash_file[64];    /* where send_code drops the phone_code_hash */
        char last_error[192];       /* real send_code/sign_in failure, for the GUI
                                       (no console: stdout is NIL: on a WB launch) */
    } login;
} tg_gui_session_state;

/* Crash-safe lifecycle log (--gui-live-debug): each line is opened, written,
   flushed and closed so a hard crash/reboot still leaves the trail up to the
   last completed step. Relative path -> the launcher's CWD (a disk dir like
   Work:TGh, which survives a MorphOS reboot, not RAM:). Off by default. */
static int tg_gui_log_on = 0;

void tg_gui_log_enable(void)
{
    tg_gui_log_on = 1;
}

int tg_gui_log_is_enabled(void)
{
    return tg_gui_log_on;
}

void tg_gui_log(const char *msg)
{
    FILE *f;

    if (msg == 0) {
        return;
    }
    /* Always emit on the unbuffered kernel-debug channel (a logtool reads it
       live, and it survives a hard freeze). On MorphOS this also paces the
       startup just enough to dodge a timing-sensitive freeze the bare run hit
       -- a no-op channel elsewhere, so it costs nothing there. The disk copy is
       opt-in (--gui-live-debug) since a write-back FS may not commit it. */
    tg_platform_debug(msg);
    if (!tg_gui_log_on) {
        return;
    }
    /* Absolute path via PROGDIR: (the binary's dir, e.g. Work:TGh) so the log
       lands next to the program regardless of the launcher's current dir -- a
       relative name followed the CWD, which IconX/Ambient does not set to the
       program's drawer, so the file ended up unreachable. */
#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
    f = fopen("PROGDIR:tg-gui-debug.log", "a");
#else
    f = fopen("tg-gui-debug.log", "a");
#endif
    if (f != 0) {
        unsigned long t = (unsigned long)time(0) % 100000UL;

        fprintf(f, "[%05lu] ", t); /* gaps between lines = where time went */
        fputs(msg, f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}

/* --- Hidden chats ---------------------------------------------------------
   A removed chat stays in the peer cache so the instant local filter can find
   it without a network round trip. The sidebar projection skips ids listed in
   data/telegram-hidden.txt; the filter projection includes and marks them.
   Opening one forgets the mark. Plain linear scans: the file is tiny. */
#define TG_GUI_HIDDEN_FILE "data/telegram-hidden.txt"
#define TG_GUI_HIDDEN_TMP "data/telegram-hidden.tmp"

static int tg_gui_hidden_contains_file(const char *path, unsigned long id_hi,
                                       unsigned long id_lo)
{
    FILE *f;
    unsigned long hi;
    unsigned long lo;
    int found = 0;

    if (path == 0) {
        return 0;
    }
    f = fopen(path, "r");
    if (f == 0) {
        return 0;
    }
    while (fscanf(f, "%lx %lx", &hi, &lo) == 2) {
        if (hi == id_hi && lo == id_lo) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int tg_gui_hidden_contains(unsigned long id_hi, unsigned long id_lo)
{
    return tg_gui_hidden_contains_file(TG_GUI_HIDDEN_FILE, id_hi, id_lo);
}

static int tg_gui_hidden_add(unsigned long id_hi, unsigned long id_lo)
{
    FILE *f;

    if (id_hi == 0UL && id_lo == 0UL) {
        return 2;
    }
    if (tg_gui_hidden_contains(id_hi, id_lo)) {
        return 0;
    }
    (void)mkdir("data", 0777);
    f = fopen(TG_GUI_HIDDEN_FILE, "a");
    if (f == 0) {
        return 2;
    }
    fprintf(f, "%08lx %08lx\n", id_hi, id_lo);
    fclose(f);
    return 0;
}

static void tg_gui_hidden_forget(unsigned long id_hi, unsigned long id_lo)
{
    /* Copy every OTHER entry to a temp file and swap it in. Streaming, not
       buffered: the list is appended to without a cap, so an earlier
       fixed-array rewrite silently dropped everything past its 128th entry
       and those chats came back on the next reload. */
    FILE *src;
    FILE *dst;
    unsigned long hi;
    unsigned long lo;
    int changed = 0;

    src = fopen(TG_GUI_HIDDEN_FILE, "r");
    if (src == 0) {
        return;
    }
    dst = fopen(TG_GUI_HIDDEN_TMP, "w");
    if (dst == 0) {
        fclose(src);
        return;
    }
    while (fscanf(src, "%lx %lx", &hi, &lo) == 2) {
        if (hi == id_hi && lo == id_lo) {
            changed = 1;
            continue; /* drop it */
        }
        fprintf(dst, "%08lx %08lx\n", hi, lo);
    }
    fclose(src);
    fclose(dst);
    if (!changed) {
        remove(TG_GUI_HIDDEN_TMP);
        return;
    }
    remove(TG_GUI_HIDDEN_FILE); /* AmigaDOS rename does not overwrite */
    if (rename(TG_GUI_HIDDEN_TMP, TG_GUI_HIDDEN_FILE) != 0) {
        remove(TG_GUI_HIDDEN_TMP);
    }
}

/* Shared 16K scratch for peer-cache probes (off the stack: m68k). */
static tg_mtproto_peer_cache tg_gui_probe_cache;

static void tg_gui_hidden_mark_name(char *name)
{
    static const char suffix[] = " (hidden)";
    unsigned long display_max;
    unsigned long name_len;
    unsigned long suffix_len;
    unsigned long keep;

    if (name == 0) {
        return;
    }
    /* The GUI driver may prepend '@' and stores the result in TG_GUI_NAME_MAX.
       Keep the suffix visible after that final projection too. */
    display_max = TG_GUI_NAME_MAX - 2UL;
    if (display_max >= TG_CHAT_LIST_NAME_MAX) {
        display_max = TG_CHAT_LIST_NAME_MAX - 1UL;
    }
    name_len = (unsigned long)strlen(name);
    suffix_len = (unsigned long)strlen(suffix);
    if (name_len + suffix_len > display_max) {
        keep = display_max - suffix_len;
        /* Do not leave a partial UTF-8 character before the ASCII suffix. */
        while (keep > 0UL &&
               (((unsigned char)name[keep] & 0xc0U) == 0x80U)) {
            --keep;
        }
        name[keep] = '\0';
    }
    strcat(name, suffix);
}

/* Parse once and keep the cache's public indexes intact. Normal sidebar
   projection omits hidden rows; local search includes them with a marker. */
static int tg_gui_chat_list_project(const char *cache_path,
                                    const char *hidden_path,
                                    unsigned long current_index,
                                    tg_chat_list_row *rows, int max,
                                    int *file_missing, int include_hidden,
                                    int mark_hidden)
{
    int count;
    int i;
    int out;

    count = tg_mtproto_chat_list_parse(cache_path, current_index, rows, max,
                                       file_missing);
    out = 0;
    for (i = 0; i < count; ++i) {
        int hidden;

        hidden = tg_gui_hidden_contains_file(hidden_path, rows[i].peer_id_hi,
                                             rows[i].peer_id_lo);
        if (hidden && !include_hidden) {
            continue;
        }
        if (out != i) {
            rows[out] = rows[i];
        }
        if (hidden && mark_hidden) {
            tg_gui_hidden_mark_name(rows[out].name);
        }
        ++out;
    }
    return out;
}

#if !defined(TG_NO_SELFTEST)
static int tg_gui_hidden_projection_self_test(void)
{
    static const char cache_path[] = "tg-hidden-cache-selftest.tmp";
    static const char hidden_path[] = "tg-hidden-list-selftest.tmp";
    tg_chat_list_row rows[4];
    FILE *f;
    int missing;
    int count;

    f = fopen(cache_path, "w");
    if (f == 0) {
        puts("probe self-test: cannot write hidden cache");
        return 2;
    }
    fputs("peer 1 type user id 0x0000000000000001 access_hash - top 0 "
          "unread 0 self no bot no username one title Visible One\n", f);
    fputs("peer 2 type user id 0x0000000000000002 access_hash - top 0 "
          "unread 0 self no bot no username two title Hidden Peer\n", f);
    fputs("peer 3 type group id 0x0000000000000003 access_hash - top 0 "
          "unread 0 self no bot no username three title Visible Three\n", f);
    fclose(f);
    f = fopen(hidden_path, "w");
    if (f == 0) {
        remove(cache_path);
        puts("probe self-test: cannot write hidden list");
        return 2;
    }
    fputs("00000000 00000002\n", f);
    fclose(f);

    missing = 0;
    count = tg_gui_chat_list_project(cache_path, hidden_path, 0UL, rows, 4,
                                     &missing, 0, 0);
    if (missing || count != 2 || rows[0].index != 1UL ||
        rows[1].index != 3UL) {
        remove(cache_path);
        remove(hidden_path);
        puts("probe self-test: hidden sidebar projection wrong");
        return 2;
    }
    count = tg_gui_chat_list_project(cache_path, hidden_path, 0UL, rows, 4,
                                     &missing, 1, 1);
    remove(cache_path);
    remove(hidden_path);
    if (missing || count != 3 || rows[1].index != 2UL ||
        strcmp(rows[1].name, "Hidden Peer (hidden)") != 0) {
        puts("probe self-test: hidden filter projection wrong");
        return 2;
    }
    return 0;
}
#endif /* !TG_NO_SELFTEST */

int tg_gui_session_open(const char *api_file, const char *auth_file,
                        const char *peer_cache_file, tg_gui_state *state,
                        FILE *stream)
{
    tg_mtproto_session session;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    const char *host;
    const char *dc_id_text;
    FILE *quiet;
    int rc;
    static const char label[] = "gui session";

    if (api_file == 0 || auth_file == 0 || peer_cache_file == 0 || state == 0 ||
        stream == 0) {
        return 2;
    }
    memset(&tg_gui_session_state, 0, sizeof(tg_gui_session_state));
    tg_gui_photo_inline_enabled = state->inline_photos ? 1 : 0;
    /* Derive the production endpoint from the saved session's DC. */
    if (tg_mtproto_session_load_authorization(auth_file, &session, auth_key) !=
        TG_MTPROTO_SESSION_OK) {
        return 2;
    }
    tg_mtproto_secure_zero(auth_key, sizeof(auth_key));
    if (tg_mtproto_production_endpoint_for_dc(session.dc_id, &host,
                                              &dc_id_text) != 0) {
        return 2;
    }
    tg_gui_session_state.host = host;
    tg_gui_session_state.port = "443";
    tg_gui_session_state.dc_id_text = dc_id_text;
    tg_gui_session_state.auth_file = auth_file;
    tg_gui_session_state.api_file = api_file;
    tg_gui_session_state.peer_cache_file = peer_cache_file;

    /* Engine + notify queue + GUI driver, mirroring the console prelude: bind
       the notify back-pointer and arm collection before anything can recv. */
    tg_chat_engine_init(&tg_gui_session_state.engine);
    tg_chat_nq = &tg_gui_session_state.engine.notify;
    tg_chat_notify_reset(&tg_gui_session_state.engine.notify, 1);
    /* Arm the live typing sink (the push collector writes it; the tick reads it)
       and turn ON the update push stream so "<name> is typing" arrives -- typing
       is a transient updateShort that the getDifference drain never carries, so
       pushes are the ONLY path. Now enabled on MorphOS too (2026-06-20): the deep
       freeze that justified keeping pushes off there was the unserialized repaint
       racing the layer build (now cured by the LockLayerRom bracket). CAVEAT: a
       busy account's initial update backlog still floods the slow bsdsocket link
       at session open -- with the layer lock that degrades to SLOW, no longer a
       freeze, but if it is too slow on a packed account this is the first knob to
       reconsider (a backlog-draining strategy, or pushes off again). */
    memset(&tg_gui_session_state.typing, 0, sizeof(tg_gui_session_state.typing));
    tg_chat_typing_target = &tg_gui_session_state.typing;
    memset(&tg_gui_session_state.read_outbox_sink, 0,
           sizeof(tg_gui_session_state.read_outbox_sink));
    tg_chat_read_outbox_target = &tg_gui_session_state.read_outbox_sink;
    memset(&tg_gui_session_state.edit_sink, 0,
           sizeof(tg_gui_session_state.edit_sink));
    tg_chat_edit_target = &tg_gui_session_state.edit_sink;
    tg_mtproto_set_session_updates(1);
    tg_gui_chat_driver_bind(&tg_gui_session_state.gui_driver, state,
                            &tg_gui_session_state.driver);

    /* A busy account's first connect can be slow to stream in (notably on a
       MorphOS bsdsocket link), so allow a generous per-recv window for the
       open; the per-tick drain later runs on a much shorter leash so the
       window stays responsive. The original timeout is restored on close. */
    tg_gui_session_state.saved_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(20UL);
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = tg_mtproto_load_api_id_file(api_file, tg_gui_session_state.api_id,
                                     sizeof(tg_gui_session_state.api_id), quiet,
                                     label);
    if (rc == 0) {
        rc = tg_mtproto_ensure_saved_auth_context(host, "443", auth_file,
                                                  dc_id_text,
                                                  &tg_gui_session_state.context,
                                                  quiet, label);
        if (rc != 0) {
            /* Slow links often stall the very first open; one fresh retry, as
               the console session does. */
            tg_mtproto_close_auth_context(&tg_gui_session_state.context);
            rc = tg_mtproto_ensure_saved_auth_context(
                host, "443", auth_file, dc_id_text,
                &tg_gui_session_state.context, quiet, label);
        }
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(tg_gui_session_state.saved_timeout);
    if (rc != 0) {
        tg_mtproto_close_auth_context(&tg_gui_session_state.context);
        tg_chat_nq = 0;
        tg_chat_typing_target = 0;
        tg_chat_read_outbox_target = 0;
        tg_chat_edit_target = 0;
        return 2;
    }
    tg_gui_log("open: connected");

    /* This account's display name (for is_out transcript rows); start with no
       chat open. */
    {
        FILE *q;

        q = tg_mtproto_open_quiet_stream(stream);
        tg_mtproto_chat_load_own_label(host, "443", tg_gui_session_state.api_id,
                                       auth_file, dc_id_text,
                                       &tg_gui_session_state.context,
                                       peer_cache_file,
                                       tg_gui_session_state.own_label,
                                       sizeof(tg_gui_session_state.own_label),
                                       q);
        tg_mtproto_close_quiet_stream(q, stream);
    }
    tg_gui_session_state.current_peer_index[0] = '\0';
    tg_gui_session_state.current_peer_label[0] = '\0';
    tg_gui_session_state.last_seen_message_id = 0UL;

    /* First launch after a fresh login: no curated cache file yet -> fetch the
       FULL dialog list once (paged + merged) so the sidebar starts populated.
       Every later launch reuses the persisted, user-curated cache (the drag-drop
       order + the removals), so we never refetch and never clobber that curation.
       MorphOS SKIPS getDialogs (the documented bsdsocket freeze on the reply) --
       there the list starts empty and Search adds chats. */
    /* FRESH login only (field decision 2026-07-26): a start-up refetch on
       every run made a busy account feel heavy, and resurrected removals.
       Existing installs enrich the list on demand via the Reload menu. */
    if (!tg_mtproto_peer_cache_available(peer_cache_file)) {
#if !(defined(__MORPHOS__) || defined(__MORPHOS))
        FILE *q;
        int iter;
        unsigned long prev;

        q = tg_mtproto_open_quiet_stream(stream);
        prev = 0UL;
        for (iter = 0; iter < 8; ++iter) { /* 8 x 30 covers the 128-entry cache */
            if (tg_mtproto_auth_list_peers_file(host, "443", api_file, auth_file,
                                                dc_id_text, "30", peer_cache_file,
                                                q) != 0) {
                break; /* a page failed -> keep whatever pages already landed */
            }
            if (tg_mtproto_load_peer_cache_file(peer_cache_file,
                                                &tg_gui_probe_cache) != 0 ||
                tg_gui_probe_cache.count <= prev ||
                tg_gui_probe_cache.count >= TG_MTPROTO_PEER_CACHE_MAX) {
                break; /* no new dialogs this page, or the cache is full */
            }
            prev = tg_gui_probe_cache.count;
        }
        tg_mtproto_close_quiet_stream(q, stream);
#endif
    }

    /* Project the current cache into the sidebar (the caller may have just
       refreshed it from the network). */
    {
        tg_chat_list_row rows[TG_CHAT_LIST_MAX];
        int count;
        int missing;

        missing = 0;
        count = tg_gui_chat_list_project(peer_cache_file, TG_GUI_HIDDEN_FILE,
                                         0UL, rows, TG_CHAT_LIST_MAX, &missing,
                                         0, 0);
        tg_gui_saved_messages_row(rows, &count, TG_CHAT_LIST_MAX);
        if (tg_gui_session_state.driver.on_chat_list_changed != 0 &&
            count > 0) {
            tg_gui_session_state.driver.on_chat_list_changed(
                tg_gui_session_state.driver.ctx, rows, count);
        }
    }
    tg_gui_session_state.open = 1;
    tg_mtproto_avatar_store_load(); /* blurred previews from the last run */
    tg_gui_log("open: ready");
    return 0;
}

static void tg_gui_session_fetch_open_avatar(FILE *stream);

int tg_gui_session_open_chat(unsigned long peer_index, FILE *stream)
{
    FILE *quiet;
    unsigned long printed;
    unsigned long prev_timeout;
    const char *history_limit;

    if (!tg_gui_session_state.open || stream == 0 || peer_index == 0UL) {
        return 0;
    }
    tg_gui_log("open_chat: start");
    /* Do not finish an old chat's thumbnails after the user switched. Fresh
       history below repopulates the bounded queue with the visible chat. */
    tg_gui_photo_queue_reset();
    /* Opening getHistory limit -- must be <= TG_MTPROTO_MESSAGE_TEXT_LIST_MAX (the
       parser only keeps that many per read, the real backlog cap). MorphOS stays
       tiny (a large reply is its documented bsdsocket freeze trigger); m68k a bit
       smaller for the 8MB budget; PPC/AROS get the deep backlog. Overridable so
       a diagnostic build can shrink the reply (the TUI pages by 5 and survives
       setups where the GUI's bigger read dies -- AmiKit/PiStorm hunt). */
#ifndef TG_GUI_OPEN_HISTORY_LIMIT
#if defined(__MORPHOS__) || defined(__MORPHOS)
#define TG_GUI_OPEN_HISTORY_LIMIT "12"
#elif defined(__m68k__)
#define TG_GUI_OPEN_HISTORY_LIMIT "30"
#else
#define TG_GUI_OPEN_HISTORY_LIMIT "60"
#endif
#endif
    history_limit = TG_GUI_OPEN_HISTORY_LIMIT;
    if (peer_index == TG_GUI_SAVED_PEER_INDEX) {
        strcpy(tg_gui_session_state.current_peer_index, "self");
    } else {
        sprintf(tg_gui_session_state.current_peer_index, "%lu", peer_index);
    }
    if (tg_mtproto_load_peer_cache_label(
            tg_gui_session_state.peer_cache_file,
            tg_gui_session_state.current_peer_index,
            tg_gui_session_state.current_peer_label,
            sizeof(tg_gui_session_state.current_peer_label)) != 0) {
        tg_gui_session_state.current_peer_label[0] = '\0';
    }
    /* Fresh transcript for the newly opened chat: reset the ring AND the
       scroll-to-bottom state. Every newest-reload path (open_selection, search
       open, notification open) funnels through here, so clearing the flags in
       this one spot guarantees no path leaves a stale newest_dropped or a phantom
       unread badge. */
    if (tg_gui_session_state.gui_driver.state != 0) {
        tg_gui_session_state.gui_driver.state->message_count = 0;
        tg_gui_session_state.gui_driver.state->msg_gen++;
        tg_gui_session_state.gui_driver.state->newest_dropped = 0;
        tg_gui_session_state.gui_driver.state->unread_below = 0;
        /* A reply target points into the OLD chat -- never carry it across. */
        tg_gui_session_state.gui_driver.state->reply_to_id = 0UL;
        tg_gui_session_state.gui_driver.state->reply_sender[0] = '\0';
        tg_gui_session_state.gui_driver.state->reply_snippet[0] = '\0';
    }
    tg_gui_session_state.last_seen_message_id = 0UL;
    /* Restart the read-receipt cursor: the loaded history starts as "sent",
       then the getPeerDialogs read below promotes the read ones to "seen". */
    tg_gui_driver_reset_read_outbox(&tg_gui_session_state.gui_driver);

    quiet = tg_mtproto_open_quiet_stream(stream);
    /* Remember the open peer's id so the tick can match inbound typing updates
       to this chat, and clear any stale "is typing" indicator on switch. */
    tg_gui_session_state.open_peer_constructor = 0UL;
    tg_gui_session_state.open_peer_id_hi = 0UL;
    tg_gui_session_state.open_peer_id_lo = 0UL;
    tg_gui_session_state.open_peer_is_chat = 0;
    tg_gui_session_state.typing.active = 0;
    /* Fresh group -> re-attempt the member-name fetch once for the typing names. */
    tg_gui_session_state.member_fetch_done = 0;
    tg_gui_session_state.member_cache.count = 0UL;
    if (tg_gui_session_state.gui_driver.state != 0) {
        tg_gui_session_state.gui_driver.state->typing[0] = '\0';
    }
    {
        unsigned long pc;
        unsigned long ph;
        unsigned long pl;
        unsigned long ahh;
        unsigned long ahl;
        int hah;

        if (tg_mtproto_load_peer_cache_peer(
                tg_gui_session_state.peer_cache_file,
                tg_gui_session_state.current_peer_index, &pc, &ph, &pl, &ahh,
                &ahl, &hah, quiet, "gui open peer") == 0) {
            tg_gui_session_state.open_peer_constructor = pc;
            tg_gui_session_state.open_peer_id_hi = ph;
            tg_gui_session_state.open_peer_id_lo = pl;
            tg_gui_session_state.open_peer_is_chat =
                (pc != TG_MTPROTO_PEER_USER_CONSTRUCTOR) ? 1 : 0;
        }
    }
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL); /* the recent history can stream */
    printed = 0UL;
    /* Route the resolved rows into the GUI transcript (in + out), then restore
       the console default. */
    tg_chat_message_driver_override = &tg_gui_session_state.driver;
    (void)tg_mtproto_auth_print_history_text_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, history_limit, quiet,
        &tg_gui_session_state.last_seen_message_id, &printed, 0, 1, 0,
        tg_gui_session_state.current_peer_label,
        tg_gui_session_state.own_label);
    tg_chat_message_driver_override = 0;
    if (printed == 0UL) {
        /* Zero rows means the history call failed or filtered everything --
           and its reason is buried in the quiet stream. Surface it: one line
           in the crash-safe log, the full protocol chatter on the console
           stream (visible when launched from a Shell / on the host build). */
        tg_gui_log("open_chat: EMPTY transcript, replaying quiet stream");
        tg_mtproto_replay_quiet_stream(quiet, stream);
    }
    /* Older history exists beyond the loaded rows when the server's total for this
       peer exceeds what we show -- arm the forced "pull older" scrollbar so the
       user can fetch more even when the loaded page fits the window. The subtitle
       is cleared (the per-chat header status line is unused for now). */
    if (tg_gui_session_state.gui_driver.state != 0) {
        tg_gui_session_state.gui_driver.state->subtitle[0] = '\0';
        tg_gui_session_state.gui_driver.state->more_above =
            (tg_gui_last_hist_total >
             (unsigned long)tg_gui_session_state.gui_driver.state->message_count)
                ? 1 : 0;
    }
    /* One getPeerDialogs read so own messages already read by the peer show the
       double-check at open (the tick refreshes it live thereafter). Now ALSO on
       MorphOS: the freeze this was disabled for was the first-tick repaint racing
       the layer build (cured by the LockLayerRom bracket), not the tiny reply. */
    {
        unsigned long read_max;

        if (tg_mtproto_chat_fetch_read_outbox_on_context(
                tg_gui_session_state.host, tg_gui_session_state.port,
                tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
                tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
                tg_gui_session_state.peer_cache_file,
                tg_gui_session_state.current_peer_index, &read_max, quiet) == 0) {
            (void)tg_gui_driver_set_read_outbox_max(
                &tg_gui_session_state.gui_driver, read_max);
        }
    }
    /* Avatar v2: one bounded photo download for THIS chat (no-op on MorphOS,
       once per peer per session, silent on any failure). */
    tg_gui_session_fetch_open_avatar(quiet);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_gui_log("open_chat: done");
    return 1; /* the selection changed -> always repaint */
}

/* Public: page OLDER history at the top of the open chat. Fetches the getHistory
   page just below the oldest message currently shown and PREPENDS it to the
   transcript, so the user can scroll back beyond the open backlog. The window
   loop calls this once when a scroll-up lands at the top. allow_drop_newest = 1
   lets the full ring evict its newest tail to make room (safe only when those
   rows are off-screen); 0 keeps them. Tri-state return: > 0 = that many older
   messages prepended; 0 = the server confirmed NO older message (real chat
   start); < 0 = could not page right now (fetch failed, or nothing pageable yet)
   -- the caller must NOT treat < 0 as the chat start. */
int tg_gui_session_load_older(FILE *stream, int allow_drop_newest)
{
    FILE *quiet;
    unsigned long prev_timeout;
    unsigned long offset_id;
    unsigned long dummy_last_seen;
    unsigned long dummy_printed;
    const char *older_limit;
    tg_gui_state *gst;
    int loaded;
    int rc;

    if (!tg_gui_session_state.open || stream == 0) {
        return -1;
    }
    gst = tg_gui_session_state.gui_driver.state;
    if (gst == 0 || gst->message_count <= 0) {
        return -1;
    }
    if (tg_gui_session_state.current_peer_index[0] == '\0') {
        return -1; /* no chat opened in this session yet */
    }
    /* The oldest row currently shown is the paging cursor; an optimistic echo
       (id 0) at the top is transient (a just-sent message awaiting its server
       id) -- report 'try later' (< 0), never the chat start. */
    offset_id = gst->messages[0].id;
    if (offset_id == 0UL) {
        return -1;
    }
    /* Smaller page than the open: MorphOS stays tiny (bsdsocket freeze guard),
       m68k modest, PPC/AROS a touch more. All ride the resized reply buffer. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
    older_limit = "10";
#elif defined(__m68k__)
    older_limit = "20";
#else
    older_limit = "30";
#endif
    tg_gui_log("load_older: start");
    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL);

    dummy_last_seen = 0UL;
    dummy_printed = 0UL;
    /* Insert the (oldest-first) batch above the transcript; tell the renderer to
       fetch the page below offset_id. Both overrides are restored right after. */
    tg_gui_session_state.gui_driver.prepend_at = 0;
    tg_gui_session_state.gui_driver.prepend_allow_drop = allow_drop_newest ? 1 : 0;
    tg_mtproto_history_offset_id_override = offset_id;
    tg_chat_message_driver_override = &tg_gui_session_state.driver;
    rc = tg_mtproto_auth_print_history_text_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, older_limit, quiet,
        &dummy_last_seen, &dummy_printed, 0, 1, 0,
        tg_gui_session_state.current_peer_label,
        tg_gui_session_state.own_label);
    tg_chat_message_driver_override = 0;
    tg_mtproto_history_offset_id_override = 0UL;
    loaded = tg_gui_session_state.gui_driver.prepend_at; /* rows inserted above */
    tg_gui_session_state.gui_driver.prepend_at = -1;     /* back to append mode */
    tg_gui_session_state.gui_driver.prepend_allow_drop = 0;

    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_gui_log("load_older: done");
    /* > 0 rows actually prepended -> report them (even a ring-full partial fill).
       0 rows: distinguish a clean 'no older messages' (rc 0 = chat start) from a
       failed fetch (rc != 0 -> < 0 so the caller does not latch the chat start). */
    if (loaded > 0) {
        return loaded;
    }
    return (rc == 0) ? 0 : -1;
}

/* Re-project the peer cache into the GUI sidebar (after it changed, e.g. a search
   result was added). Mirrors the projection in tg_gui_session_open. */
static void tg_gui_session_reload_chats(void)
{
    tg_chat_list_row rows[TG_CHAT_LIST_MAX];
    int count;
    int missing;

    if (tg_gui_session_state.driver.on_chat_list_changed == 0) {
        return;
    }
    missing = 0;
    count = tg_gui_chat_list_project(tg_gui_session_state.peer_cache_file,
                                     TG_GUI_HIDDEN_FILE, 0UL, rows,
                                     TG_CHAT_LIST_MAX, &missing, 0, 0);
    tg_gui_saved_messages_row(rows, &count, TG_CHAT_LIST_MAX);
    /* ALWAYS fire, even with count 0: skipping the callback on an emptied
       list left the window's chat_count stale, and the remove flow then
       opened a neighbour from rows that no longer existed (stuck UI). */
    tg_gui_session_state.driver.on_chat_list_changed(
        tg_gui_session_state.driver.ctx, rows, count);
}

/* F10: appends the pinned "Saved Messages" row (the self chat / cloud
   archive) as the LAST sidebar row. Bottom placement is load-bearing: the
   drag-reorder maps row positions straight to the file's public indexes, so
   anything above the synthetic row keeps that mapping intact. */
void tg_gui_saved_messages_row(tg_chat_list_row *rows, int *count, int max)
{
    tg_chat_list_row *row;

    if (rows == 0 || count == 0 || *count < 0 || max <= 0) {
        return;
    }
    if (*count >= max) {
        /* Full list (a heavy account at the sidebar cap): the pinned row must
           STILL exist, so the last chat row yields its slot -- losing chat
           #max beats silently losing Saved Messages. */
        *count = max - 1;
    }
    row = &rows[*count];
    memset(row, 0, sizeof(*row));
    row->index = TG_GUI_SAVED_PEER_INDEX;
    row->is_user = 1;
    strcpy(row->name, "Saved Messages");
    *count += 1;
}

/* Public: rebuild the sidebar from the cached chat list (peer-cache file, no
   network) -- used to restore the real list after cancelling the search picker. */
void tg_gui_session_refresh_chats(void)
{
    tg_gui_session_reload_chats();
}

/* Project every cached row for the instant local filter. Hidden rows are kept
   and marked here; the normal sidebar projection above omits them. */
void tg_gui_session_show_filterable_chats(void)
{
    tg_chat_list_row rows[TG_CHAT_LIST_MAX];
    int count;
    int missing;

    if (tg_gui_session_state.driver.on_chat_list_changed == 0) {
        return;
    }
    missing = 0;
    count = tg_gui_chat_list_project(tg_gui_session_state.peer_cache_file,
                                     TG_GUI_HIDDEN_FILE, 0UL, rows,
                                     TG_CHAT_LIST_MAX, &missing, 1, 1);
    tg_gui_saved_messages_row(rows, &count, TG_CHAT_LIST_MAX);
    tg_gui_session_state.driver.on_chat_list_changed(
        tg_gui_session_state.driver.ctx, rows, count);
}

/* Opening a local-filter row is an explicit request to restore that chat to
   the sidebar. Non-hidden rows are harmless no-ops. */
int tg_gui_session_unhide_chat(unsigned long peer_index, FILE *stream)
{
    char index_text[24];
    FILE *quiet;
    unsigned long pc;
    unsigned long ph;
    unsigned long pl;
    unsigned long ahh;
    unsigned long ahl;
    int hah;
    int rc;

    if (!tg_gui_session_state.open || stream == 0 || peer_index == 0UL ||
        peer_index == TG_GUI_SAVED_PEER_INDEX) {
        return 0;
    }
    sprintf(index_text, "%lu", peer_index);
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = tg_mtproto_load_peer_cache_peer(
        tg_gui_session_state.peer_cache_file, index_text, &pc, &ph, &pl, &ahh,
        &ahl, &hah, quiet, "unhide chat");
    tg_mtproto_close_quiet_stream(quiet, stream);
    if (rc != 0) {
        return rc;
    }
    tg_gui_hidden_forget(ph, pl);
    return 0;
}

/* Public: hide the chat at `peer_index` while retaining its peer-cache row.
   That stable row makes local search instant and preserves its access hash. */
int tg_gui_session_remove_chat(unsigned long peer_index, FILE *stream)
{
    char index_text[24]; /* fits %lu of a 64-bit unsigned long (20 digits + NUL) */
    FILE *quiet;
    int rc;

    if (!tg_gui_session_state.open || stream == 0 || peer_index == 0UL) {
        return 2;
    }
    sprintf(index_text, "%lu", peer_index);
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = 2;
    {
        unsigned long pc;
        unsigned long ph;
        unsigned long pl;
        unsigned long ahh;
        unsigned long ahl;
        int hah;

        if (tg_mtproto_load_peer_cache_peer(
                tg_gui_session_state.peer_cache_file, index_text, &pc, &ph,
                &pl, &ahh, &ahl, &hah, quiet, "hide chat") == 0) {
            rc = tg_gui_hidden_add(ph, pl);
        }
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    if (rc == 0) {
        tg_gui_session_reload_chats(); /* reproject the sidebar from the saved file */
    }
    return rc;
}

/* Menu "Reload chat list" (0.0.8): re-page the dialog list from the server
   on demand -- start-up no longer refetches (a busy account felt heavy and
   removals resurrected). Additive like the first-login bootstrap, honours
   the hidden list, then reprojects the sidebar.

   MorphOS: this used to report "not available". The getDialogs guard dates
   from 2026-06-18, three days BEFORE the real cause of the MorphOS freezes
   was found and fixed -- a 32 KB PPC task stack (__stack, 62df870), which
   the deep dialogs parse overflowed. Small getHistory replies always worked
   there, which fits that reading exactly. So the reload runs on MorphOS
   too, with the smallest pages of any lane to keep each reply modest. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
#define TG_GUI_RELOAD_PAGE "10"
#define TG_GUI_RELOAD_PAGES 12
#else
#define TG_GUI_RELOAD_PAGE "30"
#define TG_GUI_RELOAD_PAGES 8
#endif

int tg_gui_session_reload_chat_list(FILE *stream)
{
    if (!tg_gui_session_state.open || stream == 0) {
        return 2;
    }
    {
        FILE *q;
        int iter;
        unsigned long prev = 0UL;

        q = tg_mtproto_open_quiet_stream(stream);
        for (iter = 0; iter < TG_GUI_RELOAD_PAGES; ++iter) {
            if (tg_mtproto_auth_list_peers_file(
                    tg_gui_session_state.host, "443",
                    tg_gui_session_state.api_file,
                    tg_gui_session_state.auth_file,
                    tg_gui_session_state.dc_id_text, TG_GUI_RELOAD_PAGE,
                    tg_gui_session_state.peer_cache_file, q) != 0) {
                break; /* keep whatever pages landed */
            }
            if (tg_mtproto_load_peer_cache_file(
                    tg_gui_session_state.peer_cache_file,
                    &tg_gui_probe_cache) != 0 ||
                tg_gui_probe_cache.count <= prev ||
                tg_gui_probe_cache.count >= TG_MTPROTO_PEER_CACHE_MAX) {
                break;
            }
            prev = tg_gui_probe_cache.count;
        }
        tg_mtproto_close_quiet_stream(q, stream);
        tg_gui_session_reload_chats();
        return 0;
    }
}

/* Public: move a chat between two peer-cache public indexes, persist the new
   order, reproject the sidebar, and re-select the chat that was open
   (reload_chats zeroes selected_chat, so re-find it by peer id). Hidden rows
   may sit between the visible ones, which is why callers pass the row indexes
   rather than sidebar positions. No network fetch; the transcript is untouched. */
int tg_gui_session_reorder_chat(unsigned long src_index, unsigned long dst_index,
                                FILE *stream)
{
    FILE *quiet;
    int rc;
    tg_gui_state *gst;

    if (!tg_gui_session_state.open || stream == 0 ||
        src_index == 0UL || dst_index == 0UL) {
        return 2;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = tg_mtproto_peer_cache_reorder_public_index(
        tg_gui_session_state.peer_cache_file, src_index, dst_index, quiet);
    tg_mtproto_close_quiet_stream(quiet, stream);
    if (rc != 0) {
        return rc;
    }
    tg_gui_session_reload_chats(); /* reprojects sidebar; resets selected_chat to 0 */
    /* Re-find the open chat by its (immutable) peer id and reselect it, since the
       reproject lost the selection and the row indices were renumbered. */
    gst = tg_gui_session_state.gui_driver.state;
    if (gst != 0 && (tg_gui_session_state.open_peer_id_hi != 0UL ||
                     tg_gui_session_state.open_peer_id_lo != 0UL)) {
        int k;
        for (k = 0; k < gst->chat_count; ++k) {
            if (gst->chats[k].peer_id_hi ==
                    tg_gui_session_state.open_peer_id_hi &&
                gst->chats[k].peer_id_lo ==
                    tg_gui_session_state.open_peer_id_lo) {
                gst->selected_chat = k;
                break;
            }
        }
    }
    return 0;
}

/* Public: sync the in-memory sidebar unread badges to the persisted chat cache, so
   a read (badge cleared on open) and live increments survive a restart instead of
   snapping back to the getDialogs snapshot. Matches GUI chats to cache entries by
   (immutable) peer id and writes ONLY when something actually changed -- cheap to
   call after an open or after a notification batch. */
void tg_gui_session_persist_unread(void)
{
    tg_mtproto_peer_cache cache;
    tg_gui_state *gst;
    unsigned long i;
    int changed;

    if (!tg_gui_session_state.open) {
        return;
    }
    gst = tg_gui_session_state.gui_driver.state;
    if (gst == 0) {
        return;
    }
    if (tg_mtproto_load_peer_cache_file(tg_gui_session_state.peer_cache_file,
                                        &cache) != 0) {
        return;
    }
    changed = 0;
    for (i = 0UL; i < cache.count; ++i) {
        int k;

        if (cache.entries[i].is_self) {
            continue;
        }
        for (k = 0; k < gst->chat_count; ++k) {
            if (gst->chats[k].peer_id_hi == cache.entries[i].id_hi &&
                gst->chats[k].peer_id_lo == cache.entries[i].id_lo) {
                unsigned long u = (gst->chats[k].unread > 0)
                                      ? (unsigned long)gst->chats[k].unread
                                      : 0UL;

                if (cache.entries[i].unread_count != u) {
                    cache.entries[i].unread_count = u;
                    changed = 1;
                }
                break;
            }
        }
    }
    if (changed) {
        (void)tg_mtproto_save_peer_cache_file(
            tg_gui_session_state.peer_cache_file, &cache, 0, "chat cache");
    }
}

/* Results of the last GUI contacts.search, kept so the window can show a picker
   (choose among several matches) instead of always opening the top one. */
static tg_mtproto_peer_cache tg_gui_search_results;
static int tg_gui_search_openable_idx[TG_MTPROTO_PEER_CACHE_MAX];
static int tg_gui_search_openable_n;

/* Ensure `selected` is in the real peer cache and return its public index.
   Opening a search result unhides it; choosing it only as a forward destination
   deliberately does not. The caller decides when to reload/open the sidebar. */
static int tg_gui_search_cache_entry(
    const tg_mtproto_peer_cache_entry *selected, FILE *stream, int unhide,
    unsigned long *out_index)
{
    char new_index[32];
    char new_label[TG_GUI_NAME_MAX];
    FILE *quiet;
    unsigned long prev_timeout;
    unsigned long idx;
    int rc;

    if (out_index != 0) {
        *out_index = 0UL;
    }
    if (selected == 0 || stream == 0 || out_index == 0) {
        return 1;
    }
    new_index[0] = '\0';
    new_label[0] = '\0';
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(12UL);
    quiet = tg_mtproto_open_quiet_stream(stream);
    rc = tg_mtproto_peer_cache_add_selected(
        tg_gui_session_state.peer_cache_file, selected, new_index,
        sizeof(new_index), new_label, sizeof(new_label), quiet);
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    if (rc != 0 || new_index[0] == '\0') {
        return 1;
    }
    if (tg_mtproto_parse_ulong_arg(new_index, &idx) != 0 || idx == 0UL) {
        return 1;
    }
    if (unhide) {
        /* Deliberately reopened from the search: no longer hidden (0.0.8). */
        tg_gui_hidden_forget(selected->id_hi, selected->id_lo);
    }
    *out_index = idx;
    return 0;
}

/* Add `selected` to the peer cache, refresh the sidebar, then highlight + open
   it (the search path bypasses apply_selection, so set the title here too). */
static int tg_gui_search_open_entry(const tg_mtproto_peer_cache_entry *selected,
                                    FILE *stream)
{
    unsigned long idx;

    if (tg_gui_search_cache_entry(selected, stream, 1, &idx) != 0) {
        return 1;
    }

    tg_gui_session_reload_chats();
    {
        tg_gui_state *gs = tg_gui_session_state.gui_driver.state;

        if (gs != 0) {
            int r;
            int found = 0;

            for (r = 0; r < gs->chat_count; ++r) {
                if (gs->chats[r].index == idx) {
                    gs->selected_chat = r;
                    found = 1;
                    break;
                }
            }
            if (found) {
                const char *nm = gs->chats[gs->selected_chat].name;
                unsigned long ti;

                gs->chat_scroll_to_sel = 1; /* scroll the sidebar to it */
                for (ti = 0UL; ti + 1UL < sizeof(gs->title) &&
                               nm[ti] != '\0'; ++ti) {
                    gs->title[ti] = nm[ti];
                }
                gs->title[ti] = '\0';
            }
        }
        (void)tg_gui_session_open_chat(idx, stream);
    }
    return 0;
}

/* Case-insensitive substring match, ASCII folding only -- the twin of the
   sidebar's tg_gui_window_name_matches (kept separate: that one lives in the
   window backend, this one must link on every build of the probe). */
static int tg_gui_search_name_matches(const char *name, const char *q)
{
    unsigned long nl = (unsigned long)strlen(name);
    unsigned long ql = (unsigned long)strlen(q);
    unsigned long i;
    unsigned long j;

    if (ql == 0UL || ql > nl) {
        return 0;
    }
    for (i = 0UL; i + ql <= nl; ++i) {
        for (j = 0UL; j < ql; ++j) {
            char a = name[i + j];
            char b = q[j];

            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (j == ql) {
            return 1;
        }
    }
    return 0;
}

/* Throwaway cache file for the own-dialogs search stage: T: (RAM) on Amiga
   systems, data/ on the host build. Never the real peer cache. */
#if defined(__m68k__) || defined(__MORPHOS__) || defined(__amigaos4__) || \
    defined(__AROS__)
#define TG_GUI_SEARCH_TMP_CACHE "T:tg-search-dialogs.tmp"
#else
#define TG_GUI_SEARCH_TMP_CACHE "data/tg-search-dialogs.tmp"
#endif

/* Online search for `query`, two stages (0.0.8). Stage 1 pages the account's
   OWN dialogs (same paging as Reload chat list) into a throwaway cache file
   and keeps the name/username matches -- a hidden chat or a private group has
   no public username, so the global search can never find it, yet it IS in
   the dialog list. Only when stage 1 matches nothing does stage 2 run the
   classic contacts.search (limit 10, MorphOS-safe reply). Neither stage
   touches the real peer cache or opens anything -- the window then either
   opens the only match or shows a picker. Returns the openable-result count
   (>= 0), -1 on failure. */
int tg_gui_session_search_run(const char *query, FILE *stream)
{
    unsigned char qbuf[256];
    char trimmed[128];
    char match_query[128];
    tg_mtproto_rpc_result result;
    tg_mtproto_tl_writer writer;
    FILE *quiet;
    unsigned long i;
    unsigned long prev_timeout;
    int rc;
    static const char label[] = "gui contacts.search";

    tg_gui_search_openable_n = 0;
    tg_gui_search_results.count = 0;
    if (!tg_gui_session_state.open || stream == 0 || query == 0) {
        return -1;
    }
    tg_mtproto_copy_plain_cache_text(trimmed, sizeof(trimmed), query);
    tg_mtproto_trim_line(trimmed);
    /* An EMPTY query is the browse mode ("Browse all chats...", 0.0.8): stage
       1 keeps every dialog instead of the name matches -- the way to find a
       removed chat when the exact name escapes you -- and stage 2 never runs
       (a global search needs a term). */
    /* Kept for the dialog-name match BEFORE the UTF-8 conversion below: cached
       titles are matched byte-wise ASCII-folded, same semantics as the sidebar
       filter. */
    tg_mtproto_copy_plain_cache_text(match_query, sizeof(match_query), trimmed);
#if TG_MTPROTO_DISPLAY_LATIN1
    /* The query is Latin-1 (Amiga keymap); Telegram wants UTF-8. Without this
       a name with a-ring/a-uml/o-uml went out as invalid UTF-8 and the server
       rejected the whole search (the user saw "network problem"). */
    {
        static char query_utf8[256];

        if (tg_mtproto_latin1_to_utf8(trimmed, query_utf8,
                                      sizeof(query_utf8))) {
            tg_mtproto_copy_plain_cache_text(trimmed, sizeof(trimmed),
                                             query_utf8);
        }
    }
#endif

    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(12UL);

    /* --- Stage 1: the account's own dialogs (hidden chats included) ------- */
    remove(TG_GUI_SEARCH_TMP_CACHE);
    {
        int iter;
        unsigned long prev = 0UL;

        for (iter = 0; iter < TG_GUI_RELOAD_PAGES; ++iter) {
            if (tg_mtproto_auth_list_peers_file(
                    tg_gui_session_state.host, "443",
                    tg_gui_session_state.api_file,
                    tg_gui_session_state.auth_file,
                    tg_gui_session_state.dc_id_text, TG_GUI_RELOAD_PAGE,
                    TG_GUI_SEARCH_TMP_CACHE, quiet) != 0) {
                break; /* match against whatever pages landed */
            }
            if (tg_mtproto_load_peer_cache_file(TG_GUI_SEARCH_TMP_CACHE,
                                                &tg_gui_probe_cache) != 0 ||
                tg_gui_probe_cache.count <= prev ||
                tg_gui_probe_cache.count >= TG_MTPROTO_PEER_CACHE_MAX) {
                break;
            }
            prev = tg_gui_probe_cache.count;
        }
        if (tg_mtproto_load_peer_cache_file(TG_GUI_SEARCH_TMP_CACHE,
                                            &tg_gui_probe_cache) == 0) {
            unsigned long cap = (match_query[0] != '\0')
                                    ? 10UL
                                    : (unsigned long)TG_GUI_MAX_CHATS;

            for (i = 0UL; i < tg_gui_probe_cache.count &&
                          tg_gui_search_results.count < cap; ++i) {
                const tg_mtproto_peer_cache_entry *e =
                    &tg_gui_probe_cache.entries[i];

                if (e->is_self) {
                    continue;
                }
                if (match_query[0] == '\0' ||
                    tg_gui_search_name_matches(e->title, match_query) ||
                    tg_gui_search_name_matches(e->username, match_query)) {
                    tg_gui_search_results
                        .entries[tg_gui_search_results.count] = *e;
                    tg_gui_search_results.count += 1UL;
                }
            }
        }
        remove(TG_GUI_SEARCH_TMP_CACHE);
        /* The probe cache was scratch space: reload it from the REAL file so
           later cache users see the true sidebar list again. */
        (void)tg_mtproto_load_peer_cache_file(
            tg_gui_session_state.peer_cache_file, &tg_gui_probe_cache);
    }
    if (tg_gui_search_results.count > 0UL || match_query[0] == '\0') {
        /* Matches found -- or browse mode, where the global search would be
           meaningless without a term (an empty browse just reports 0). */
        tg_mtproto_close_quiet_stream(quiet, stream);
        tg_net_set_connect_timeout_seconds(prev_timeout);
        goto openable;
    }

    /* --- Stage 2: the classic global contacts.search ----------------------- */
    tg_mtproto_tl_writer_init(&writer, qbuf, sizeof(qbuf));
    if (tg_mtproto_build_contacts_search(&writer, trimmed, 10UL) !=
        TG_MTPROTO_TL_OK) {
        tg_net_set_connect_timeout_seconds(prev_timeout);
        tg_mtproto_close_quiet_stream(quiet, stream);
        return -1;
    }
    memset(&result, 0, sizeof(result));
    rc = tg_mtproto_send_saved_query_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context, qbuf,
        writer.length, &result, quiet, label, 200U);
    if (rc != 0 ||
        result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_unpack_gzip_result(&result, quiet, label) != 0 ||
        tg_mtproto_parse_contacts_search_peer_cache(
            result.result_constructor, result.result_body,
            result.result_body_length, &tg_gui_search_results) !=
            TG_MTPROTO_TL_OK) {
        tg_net_set_connect_timeout_seconds(prev_timeout);
        tg_mtproto_close_quiet_stream(quiet, stream);
        return -1;
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(prev_timeout);

openable:
    for (i = 0UL; i < tg_gui_search_results.count &&
                  tg_gui_search_openable_n <
                      (int)(sizeof(tg_gui_search_openable_idx) /
                            sizeof(tg_gui_search_openable_idx[0]));
         ++i) {
        if (!tg_gui_search_results.entries[i].is_self &&
            tg_mtproto_peer_cache_entry_is_openable(
                &tg_gui_search_results.entries[i])) {
            tg_gui_search_openable_idx[tg_gui_search_openable_n] = (int)i;
            ++tg_gui_search_openable_n;
        }
    }
    return tg_gui_search_openable_n;
}

/* UTF-8 (wire) -> Latin-1 (GUI display), for search-result titles that reach
   the sidebar directly rather than through the driver's copy_latin1. Without
   it a name like "Bjorn" with o-umlaut showed as the raw two-byte "A~ 3/4".
   No-op copy when the build is not Latin-1 (host/utf8). */
static void tg_mtproto_utf8_to_latin1_name(char *dst, unsigned long dst_size,
                                           const char *src)
{
#if TG_MTPROTO_DISPLAY_LATIN1
    unsigned long i;
    unsigned long o;

    if (dst == 0 || dst_size == 0UL) {
        return;
    }
    i = 0UL;
    o = 0UL;
    while (src != 0 && src[i] != '\0' && o + 1UL < dst_size) {
        unsigned long cp;
        char one[8];
        unsigned long got;

        cp = tg_mtproto_utf8_read_codepoint(src, &i);
        got = tg_mtproto_display_codepoint_to_latin1(cp, one, sizeof(one));
        if (got == 0UL || o + got >= dst_size) {
            break;
        }
        memcpy(dst + o, one, got);
        o += got;
    }
    dst[o] = '\0';
#else
    if (dst != 0 && dst_size > 0UL) {
        unsigned long i;

        for (i = 0UL; src != 0 && src[i] != '\0' && i + 1UL < dst_size; ++i) {
            dst[i] = src[i];
        }
        dst[(src != 0) ? i : 0UL] = '\0';
    }
#endif
}

int tg_gui_session_search_count(void)
{
    return tg_gui_search_openable_n;
}

const char *tg_gui_session_search_name(int i)
{
    const tg_mtproto_peer_cache_entry *e;

    if (i < 0 || i >= tg_gui_search_openable_n) {
        return "";
    }
    /* Username-only contacts (no title) are openable too -- fall back to the
       @username so the picker row is not blank, matching the console printer. */
    e = &tg_gui_search_results.entries[tg_gui_search_openable_idx[i]];
    {
        static char latin1_name[TG_GUI_NAME_MAX];
        const char *raw = (e->title[0] != '\0') ? e->title : e->username;

        /* the wire title is UTF-8; the sidebar renders Latin-1 (bug: "o-uml"
           showed as two chars). Convert like the chat-list path does. */
        tg_mtproto_utf8_to_latin1_name(latin1_name, sizeof(latin1_name), raw);
        return latin1_name;
    }
}

/* Open the i-th openable result of the last search (add it to the cache + open). */
int tg_gui_session_search_open_result(int i, FILE *stream)
{
    if (i < 0 || i >= tg_gui_search_openable_n) {
        return 1;
    }
    return tg_gui_search_open_entry(
        &tg_gui_search_results.entries[tg_gui_search_openable_idx[i]], stream);
}

/* Add an online-picker result to the cache without opening or unhiding it, and
   return the public cache index suitable for tg_gui_session_forward(). */
int tg_gui_session_search_cache_result(int i, unsigned long *peer_index,
                                       FILE *stream)
{
    if (i < 0 || i >= tg_gui_search_openable_n) {
        return 1;
    }
    return tg_gui_search_cache_entry(
        &tg_gui_search_results.entries[tg_gui_search_openable_idx[i]], stream,
        0, peer_index);
}

/* Back-compat: search + open the top match (no picker). 0 = opened, 1 = no
   result / network, 2 = bad args. */
int tg_gui_session_search_open(const char *query, FILE *stream)
{
    int cnt;

    if (!tg_gui_session_state.open || stream == 0 || query == 0) {
        return 2;
    }
    cnt = tg_gui_session_search_run(query, stream);
    if (cnt <= 0) {
        return 1;
    }
    return tg_gui_session_search_open_result(0, stream) == 0 ? 0 : 1;
}

int tg_gui_session_send(const char *text, unsigned long reply_to_msg_id,
                        FILE *stream)
{
    FILE *quiet;
    unsigned long sent_id;
    unsigned long prev_timeout;
    int rc;
    const char *send_text;
#if TG_MTPROTO_DISPLAY_LATIN1
    /* Worst case 2x the composer buffer (every byte a high Latin-1 char),
       and static so a 4 KB composer does not land on the stack. It used to
       be 1024 with a comment claiming "the composer is at most 256 bytes":
       that went stale when the composer grew, so a long PASTED text quietly
       fell back to raw Latin-1 and its accents reached other clients as
       replacement characters. */
    static char send_line[(TG_GUI_MSG_TEXT_MAX * 2) + 1];
#endif

    if (!tg_gui_session_state.open || stream == 0 || text == 0 ||
        text[0] == '\0' ||
        tg_gui_session_state.current_peer_index[0] == '\0') {
        return 2;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL);
    sent_id = 0UL;
    send_text = text;
#if TG_MTPROTO_DISPLAY_LATIN1
    /* The composer text is ISO-8859-1 (Amiga keymap); convert to UTF-8 so
       accented characters (a-grave = 0xE0, etc.) reach Telegram intact instead
       of being sent as a lone high byte (invalid UTF-8 -> U+FFFD). Mirrors the
       TUI send path. Newlines pass through untouched, so a pasted text file
       keeps its line breaks on every client. */
    if (tg_mtproto_latin1_to_utf8(text, send_line, sizeof(send_line))) {
        send_text = send_line;
    }
#endif
    rc = tg_mtproto_auth_send_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, send_text, reply_to_msg_id,
        &sent_id, quiet);
    if (rc == 0) {
        /* Show the sent message at once, optimistically, with no extra network
           round-trip -- a confirm-poll is slow and unreliable on MorphOS (it
           was the "sent but not shown" symptom). Echo the ORIGINAL Latin-1
           `text` (NOT send_text): the GUI renderer is Latin-1, so passing the
           UTF-8 copy would double-encode into mojibake. The open-chat poll now
           includes outgoing (for multi-device), and the driver dedups/reconciles
           this echo by server id (or by text until the id is known), so it is
           never re-fetched into a duplicate. */
        tg_gui_driver_append_own(&tg_gui_session_state.gui_driver, text,
                                 tg_gui_session_state.own_label,
                                 (reply_to_msg_id != 0UL &&
                                  tg_gui_session_state.gui_driver.state != 0)
                                     ? tg_gui_session_state.gui_driver.state
                                           ->reply_snippet
                                     : 0,
                                 sent_id);
    }
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    return rc;
}

const char *tg_gui_session_last_action_error(void)
{
    return tg_mtproto_query_fail;
}

int tg_gui_session_forward(unsigned long message_id,
                           unsigned long destination_peer_index,
                           FILE *stream)
{
    char destination[32];
    FILE *quiet;
    unsigned long prev_timeout;
    int rc;

    if (!tg_gui_session_state.open || stream == 0 || message_id == 0UL ||
        destination_peer_index == 0UL ||
        tg_gui_session_state.current_peer_index[0] == '\0') {
        return 2;
    }
    if (destination_peer_index == TG_GUI_SAVED_PEER_INDEX) {
        strcpy(destination, "self");
    } else {
        sprintf(destination, "%lu", destination_peer_index);
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL);
    rc = tg_mtproto_auth_forward_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, destination, message_id,
        quiet);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    return rc;
}

unsigned long tg_gui_session_current_peer_index(void)
{
    unsigned long index;

    if (!tg_gui_session_state.open ||
        tg_gui_session_state.current_peer_index[0] == '\0') {
        return 0UL;
    }
    if (strcmp(tg_gui_session_state.current_peer_index, "self") == 0) {
        return TG_GUI_SAVED_PEER_INDEX;
    }
    if (tg_mtproto_parse_ulong_arg(tg_gui_session_state.current_peer_index,
                                   &index) != 0) {
        return 0UL;
    }
    return index;
}

int tg_gui_session_edit(const char *text, unsigned long message_id, FILE *stream)
{
    FILE *quiet;
    unsigned long prev_timeout;
    int rc;
    const char *edit_text;
#if TG_MTPROTO_DISPLAY_LATIN1
    char edit_line[1024];
#endif

    if (!tg_gui_session_state.open || stream == 0 || text == 0 ||
        text[0] == '\0' || message_id == 0UL ||
        tg_gui_session_state.current_peer_index[0] == '\0') {
        return 2;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL);
    edit_text = text;
#if TG_MTPROTO_DISPLAY_LATIN1
    if (tg_mtproto_latin1_to_utf8(text, edit_line, sizeof(edit_line))) {
        edit_text = edit_line; /* send UTF-8; the local echo keeps Latin-1 */
    }
#endif
    rc = tg_mtproto_auth_edit_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, message_id, edit_text, quiet);
    if (rc == 0) {
        /* Update the on-screen bubble at once with the ORIGINAL Latin-1 text. */
        (void)tg_gui_driver_update_text(&tg_gui_session_state.gui_driver,
                                        message_id, text);
    }
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    return rc;
}

int tg_gui_session_delete(unsigned long message_id, FILE *stream)
{
    FILE *quiet;
    unsigned long prev_timeout;
    int rc;

    if (!tg_gui_session_state.open || stream == 0 || message_id == 0UL ||
        tg_gui_session_state.current_peer_index[0] == '\0') {
        return 2;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(10UL);
    /* revoke = 1: delete for everyone (the usual intent; the server ignores
       revoke when it is past the allowed window). */
    rc = tg_mtproto_auth_delete_peer_on_context(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index, message_id, 1, quiet);
    if (rc == 0) {
        (void)tg_gui_driver_remove_by_id(&tg_gui_session_state.gui_driver,
                                         message_id);
    }
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_close_quiet_stream(quiet, stream);
    return rc;
}

/* One-shot member fetch for the OPEN group (same lazily-triggered fetch the
   typing-name resolver uses; member_fetch_done is marked BEFORE the call so a
   miss never re-fetches this open). MorphOS returns empty by design (the
   freeze guard lives inside tg_mtproto_gui_fetch_group_members). */
static void tg_gui_session_ensure_members(FILE *stream)
{
    FILE *fq;

    if (tg_gui_session_state.member_fetch_done) {
        return;
    }
    tg_gui_session_state.member_fetch_done = 1;
    tg_gui_session_state.member_for_id_hi =
        tg_gui_session_state.open_peer_id_hi;
    tg_gui_session_state.member_for_id_lo =
        tg_gui_session_state.open_peer_id_lo;
    fq = tg_mtproto_open_quiet_stream(stream);
    (void)tg_mtproto_gui_fetch_group_members(
        tg_gui_session_state.host, tg_gui_session_state.port,
        tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
        tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
        tg_gui_session_state.peer_cache_file,
        tg_gui_session_state.current_peer_index,
        &tg_gui_session_state.member_cache, fq);
    tg_mtproto_close_quiet_stream(fq, stream);
}

/* Case-insensitive ASCII prefix test; a shorter `s` fails via the NUL byte. */
static int tg_gui_session_prefix_ci(const char *s, const char *prefix,
                                    unsigned long n)
{
    unsigned long i;

    for (i = 0UL; i < n; ++i) {
        unsigned char a = (unsigned char)s[i];
        unsigned char b = (unsigned char)prefix[i];

        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + 32U);
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + 32U);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

int tg_gui_session_mention_candidates(const char *prefix, char *items,
                                      unsigned long item_size, int max,
                                      FILE *stream)
{
    unsigned long i;
    unsigned long plen;
    int n;

    if (items == 0 || item_size < 2UL || max <= 0 || stream == 0) {
        return 0;
    }
    if (!tg_gui_session_state.open ||
        !tg_gui_session_state.open_peer_is_chat) {
        return 0; /* mentions only make sense in an open group */
    }
    tg_gui_session_ensure_members(stream);
    plen = (prefix != 0) ? (unsigned long)strlen(prefix) : 0UL;
    n = 0;
    for (i = 0UL;
         i < tg_gui_session_state.member_cache.count && n < max; ++i) {
        const tg_mtproto_peer_cache_entry *e =
            &tg_gui_session_state.member_cache.entries[i];
        char *dst;
        unsigned long k;

        if (e->username[0] == '\0' || e->is_self) {
            /* Only usernames can be inserted as a plain-text mention; the
               sender's own name would be noise. */
            continue;
        }
        if (plen != 0UL &&
            !tg_gui_session_prefix_ci(e->username, prefix, plen) &&
            !tg_gui_session_prefix_ci(e->title, prefix, plen)) {
            continue;
        }
        dst = items + ((unsigned long)n * item_size);
        k = 0UL;
        while (e->username[k] != '\0' && k + 1UL < item_size) {
            dst[k] = e->username[k];
            ++k;
        }
        dst[k] = '\0';
        ++n;
    }
    return n;
}

/* Avatar v2 (safe scope): ONE bounded download of the OPEN chat's small
   profile photo, at open_chat time only -- never from the live tick, and never
   on MorphOS (same discipline as the member fetch). Cached as a flat
   tgav<peerid>.jpg next to the binary; re-fetched at most once per peer per
   session (freshness) and skipped when the photo lives on another DC (the v3
   export/import path is deliberately out of scope). All failures are silent:
   the renderer keeps the stripped thumb / initials. */
#define TG_GUI_AVFETCH_MAX 64
static unsigned long tg_gui_avfetch_hi[TG_GUI_AVFETCH_MAX];
static unsigned long tg_gui_avfetch_lo[TG_GUI_AVFETCH_MAX];
static int tg_gui_avfetch_n = 0;

/* F9 chunk 3: download the document attached to message `msg_id` in the OPEN
   chat, on the session context, into downloads/<name>. Re-fetches the chat
   history to obtain a FRESH file_reference (references expire; a stale one from
   open time would fail mid-transfer), then loops upload.getFile writing each
   chunk straight to disk (a multi-MB file must never be buffered whole on OS3).
   Same-DC only for now (a foreign-DC document needs the export/import dance,
   deferred with the foreign-DC avatars). Bounded, silent-failing, GUI-tick-free
   -- called only from the explicit "Download" action. */
/* MUST stay a valid getFile limit (4096*2^k dividing 1 MB) AND leave the
   getFile RESPONSE -- chunk bytes + upload.file wrapper + MTProto envelope --
   comfortably inside TG_MTPROTO_REPLY_RECV_MAX, or the reply overruns the
   receive buffer and looks like "no reply". m68k recv is 48 KB -> 32 KB
   chunk; the other lanes recv 128 KB -> 64 KB. (This was the file-download
   failure on files big enough to need a FULL chunk; small files returned a
   short reply and slipped under the limit.) */
/* The getFile RESPONSE (chunk bytes + upload.file wrapper + envelope) must fit
   inside TG_MTPROTO_ENCRYPTED_BODY_MAX after decryption -- file bytes are not
   gzip-compressed, so the raw chunk lands in that buffer whole. m68k body is
   40 KB -> 32 KB chunk; the others hold 72 KB -> 64 KB chunk. Both are valid
   getFile limits (4096*2^k dividing 1 MB). A bigger chunk = fewer synchronous
   round-trips = faster (each getFile waits for its whole reply before the next
   is sent, so throughput is round-trip-bound, not bandwidth-bound). MorphOS
   was briefly halved to 32 KB to fit a TOTAL-time query budget; that budget is
   now idle-based, so it is back to 64 KB with the rest -- twice the speed on a
   fast link, and its 72 KB buffers already hold it. */
#ifndef TG_GUI_DL_CHUNK /* overridable: LOWMEM shrinks body and chunk together */
#if defined(__m68k__)
#define TG_GUI_DL_CHUNK 32768UL   /* 32 KB, within the 40 KB decrypted body */
#else
#define TG_GUI_DL_CHUNK 65536UL   /* 64 KB, within the 72 KB decrypted body */
#endif
#endif
/* Per-chunk retries before a download gives up. A long transfer over a flaky
   link (phone hotspot, PLIP, a busy DC) loses the odd chunk; re-asking for the
   same offset costs one round-trip and saves the whole file. */
#define TG_GUI_DL_CHUNK_RETRIES 4
/* Write buffer for the file being downloaded: a few chunks' worth, so the
   drive is touched in big blocks. m68k keeps it modest (its whole BSS is
   the tight budget), the others can afford more. */
#ifndef TG_GUI_DL_WBUF /* overridable: LOWMEM pairs it with the small chunk */
#if defined(__m68k__)
#define TG_GUI_DL_WBUF (64UL * 1024UL)
#else
#define TG_GUI_DL_WBUF (128UL * 1024UL)
#endif
#endif

/* Where downloads land (0.0.8, tester request: an 030 owner wanted them in
   RAM: for speed). Default "downloads" next to the program, as always;
   data/telegram-downloads.txt overrides it with one line, e.g. "RAM:TGdl"
   or "Work:Incoming". Read once per run -- a transfer must not change
   destination halfway. Trailing separator optional, we add one if needed. */
static char tg_gui_dl_dir[96];

const char *tg_gui_session_download_dir(void)
{
    if (tg_gui_dl_dir[0] == '\0') {
        FILE *f = fopen("data/telegram-downloads.txt", "r");

        if (f != 0) {
            if (fgets(tg_gui_dl_dir, (int)sizeof(tg_gui_dl_dir), f) != 0) {
                unsigned long n = (unsigned long)strlen(tg_gui_dl_dir);

                while (n > 0UL && (tg_gui_dl_dir[n - 1UL] == '\n' ||
                                   tg_gui_dl_dir[n - 1UL] == '\r' ||
                                   tg_gui_dl_dir[n - 1UL] == ' ' ||
                                   tg_gui_dl_dir[n - 1UL] == '\t')) {
                    tg_gui_dl_dir[--n] = '\0';
                }
                /* A trailing '/' would double up when we join the name;
                   a trailing ':' is a volume root and must stay. */
                if (n > 0UL && tg_gui_dl_dir[n - 1UL] == '/') {
                    tg_gui_dl_dir[n - 1UL] = '\0';
                }
            }
            fclose(f);
        }
        if (tg_gui_dl_dir[0] == '\0') {
            strcpy(tg_gui_dl_dir, "downloads");
        }
    }
    return tg_gui_dl_dir;
}

/* Point downloads at `dir` from now on and remember it for the next run.
   A transfer already running keeps the path it built at start, so this is
   safe at any time. 0 = stored (and persisted); non-zero = rejected. */
int tg_gui_session_set_download_dir(const char *dir)
{
    FILE *f;
    const char *path = "data/telegram-downloads.txt";
    char tmp[64];
    unsigned long n;
    int failed;

    if (dir == 0 || dir[0] == '\0' ||
        strlen(dir) + 1UL > sizeof(tg_gui_dl_dir)) {
        return 1;
    }
    strcpy(tg_gui_dl_dir, dir);
    n = (unsigned long)strlen(tg_gui_dl_dir);
    while (n > 0UL && (tg_gui_dl_dir[n - 1UL] == ' ' ||
                       tg_gui_dl_dir[n - 1UL] == '\t')) {
        tg_gui_dl_dir[--n] = '\0';
    }
    if (n > 0UL && tg_gui_dl_dir[n - 1UL] == '/') {
        tg_gui_dl_dir[n - 1UL] = '\0'; /* the join adds it back */
    }
    if (tg_gui_dl_dir[0] == '\0') {
        strcpy(tg_gui_dl_dir, "downloads");
        return 1;
    }
    (void)mkdir("data", 0777);
    if (strlen(path) + 5UL > sizeof(tmp)) {
        return 2;
    }
    strcpy(tmp, path);
    strcat(tmp, ".tmp");
    f = fopen(tmp, "w");
    if (f == 0) {
        return 2; /* in force for this run, but not remembered */
    }
    failed = fprintf(f, "%s\n", tg_gui_dl_dir) < 0;
    if (fclose(f) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(tmp);
        return 2;
    }
    (void)remove(path);
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return 2;
    }
    return 0;
}

static void tg_gui_dl_sanitize_name(const char *in, char *out,
                                    unsigned long out_size)
{
    unsigned long n = 0UL;

    if (out_size == 0UL) {
        return;
    }
    if (in != 0) {
        const char *p;

        for (p = in; *p != '\0' && n + 1UL < out_size; ++p) {
            unsigned char c = (unsigned char)*p;

            /* drop path separators and control bytes -- keep it a bare name */
            if (c == '/' || c == ':' || c == '\\' || c < 0x20U) {
                continue;
            }
            out[n++] = (char)c;
        }
    }
    if (n == 0UL) { /* no usable name: a stable fallback */
        const char *f = "download.bin";

        while (*f != '\0' && n + 1UL < out_size) {
            out[n++] = *f++;
        }
    }
    out[n] = '\0';
}

/* Join the download drawer and the file name into dst WITHOUT overflowing it.
   Both inputs are bounded but their sum is not: the drawer holds up to 95
   chars (the menu's ASL requester writes it) and a Telegram file_name up to
   127, against a 144-byte destination -- a deep enough drawer plus a long
   attachment name used to write past the end of tg_gui_dl.path and into the
   FILE pointers right behind it. The name is shortened, never the drawer (the
   user picked that), and the extension survives the cut: a shortened name is
   still openable, one that lost its ".lha" is not. */
static void tg_gui_dl_join_path(char *dst, unsigned long dst_size,
                                const char *dir, const char *name)
{
    const char *sep;
    unsigned long dn;
    unsigned long nn;
    unsigned long room;
    unsigned long ext;   /* index of the last '.' in name, nn = none */
    unsigned long o;
    unsigned long i;

    if (dst == 0 || dst_size == 0UL) {
        return;
    }
    dst[0] = '\0';
    if (dir == 0) {
        dir = "";
    }
    if (name == 0) {
        name = "";
    }
    dn = (unsigned long)strlen(dir);
    sep = (dn > 0UL && dir[dn - 1UL] == ':') ? "" : "/";
    o = 0UL;
    for (i = 0UL; i < dn && o + 1UL < dst_size; ++i) {
        dst[o++] = dir[i];
    }
    for (i = 0UL; sep[i] != '\0' && o + 1UL < dst_size; ++i) {
        dst[o++] = sep[i];
    }
    room = (o + 1UL < dst_size) ? (dst_size - 1UL - o) : 0UL;
    nn = (unsigned long)strlen(name);
    if (nn <= room) {
        for (i = 0UL; i < nn; ++i) {
            dst[o++] = name[i];
        }
        dst[o] = '\0';
        return;
    }
    ext = nn;
    for (i = nn; i > 0UL; --i) {
        if (name[i - 1UL] == '.') {
            ext = i - 1UL;
            break;
        }
    }
    /* Keep the extension only when it is short enough to be worth the room. */
    if (ext < nn && (nn - ext) < 8UL && (nn - ext) < room) {
        unsigned long head = room - (nn - ext);

        for (i = 0UL; i < head; ++i) {
            dst[o++] = name[i];
        }
        for (i = ext; i < nn; ++i) {
            dst[o++] = name[i];
        }
    } else {
        for (i = 0UL; i < room; ++i) {
            dst[o++] = name[i];
        }
    }
    dst[o] = '\0';
}

/* Re-fetch the open chat's newest history and copy the document meta of the
   message with id == msg_id (fresh file_reference). 0 = found + has document. */
static char tg_dl_diag[96]; /* find_open_document -> download status */

static int tg_mtproto_file_find_document(const tg_mtproto_file_ctx *fc,
                                         unsigned long msg_id, FILE *quiet,
                                         tg_mtproto_document_meta *out,
                                         unsigned long *resolved_id)
{
    unsigned char query[64];
    unsigned long pc, ph, pl, ahh, ahl;
    int hah;
    tg_mtproto_tl_writer writer;
    tg_mtproto_rpc_result result;
    static tg_mtproto_message_text_list texts;
    unsigned long i;
    static const char label[] = "mtproto getHistory(download)";

    if (tg_mtproto_load_peer_cache_peer(
            fc->peer_cache_file,
            fc->peer_index, &pc, &ph, &pl, &ahh,
            &ahl, &hah, quiet, label) != 0) {
        return 1;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    /* Anchor the fetch AT the target: offset_id = msg_id + 1 makes the server
       return messages with id <= msg_id, newest first, so the wanted message
       is the first result no matter how far back it was scrolled. msg_id == 0
       means "the most recent document" (console /getfile): fetch the newest
       page and take the first message that carries one. */
    if (tg_mtproto_build_messages_get_history_peer(&writer, pc, ph, pl, ahh,
                                                   ahl, hah,
                                                   msg_id != 0UL ? msg_id + 1UL
                                                                 : 0UL,
                                                   msg_id != 0UL ? 5UL : 20UL)
        != TG_MTPROTO_TL_OK) {
        return 1;
    }
    memset(&result, 0, sizeof(result));
    if (tg_mtproto_send_saved_query_on_context(
            fc->host, fc->port,
            fc->api_id, fc->auth_file,
            fc->dc_id_text, fc->context,
            query, writer.length, &result, quiet, label, 600U) != 0 ||
        result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_unpack_gzip_result(&result, quiet, label) != 0 ||
        tg_mtproto_parse_message_text_list(result.result_constructor,
                                           result.result_body,
                                           result.result_body_length,
                                           &texts) != TG_MTPROTO_TL_OK) {
        return 1;
    }
    for (i = 0UL; i < texts.count; ++i) {
        if (msg_id != 0UL && texts.messages[i].id != msg_id) {
            continue;
        }
        if (!texts.messages[i].document.has_document) {
            if (msg_id == 0UL) {
                continue; /* latest-mode: keep scanning the page */
            }
            strcpy(tg_dl_diag, "no document on message");
            return 1;
        }
        if (texts.messages[i].document.file_reference_len == 0UL) {
            sprintf(tg_dl_diag, "reference too long (dc=%lu size=%lu:%lu)",
                    texts.messages[i].document.dc_id,
                    texts.messages[i].document.size_hi,
                    texts.messages[i].document.size_lo);
            return 1;
        }
        *out = texts.messages[i].document;
        if (resolved_id != 0) {
            /* Latest-mode callers pin the retry to THIS message: a mid-transfer
               re-resolve of "the newest document" could bind a newer file and
               write its bytes under the original name. */
            *resolved_id = texts.messages[i].id;
        }
        sprintf(tg_dl_diag, "dc=%lu size=%lu:%lu ref=%lu",
                out->dc_id, out->size_hi, out->size_lo,
                out->file_reference_len);
        return 0;
    }
    strcpy(tg_dl_diag, msg_id != 0UL ? "message not in history page"
                                     : "no recent file in this chat");
    return 1;
}

/* F9 upload: stream from disk to the OPEN chat without buffering the whole
   file. At <=10 MiB use saveFilePart + inputFile; above that use
   saveBigFilePart + inputFileBig. Telegram's current non-Premium config allows
   4000 parts. Keeping the already-proven per-platform chunks avoids adding
   several 512 KiB send buffers to a low-memory Amiga: this yields a conservative
   ceiling of 31.25 MiB on m68k (8 KiB parts) and 250 MiB elsewhere (64 KiB).
   Returns 0 ok, 1 transfer failure, 2 over this build's limit, 3 unreadable.
   Blocking on-context call from the explicit user action, never the tick. */
#define TG_GUI_UL_BIG_THRESHOLD (10UL * 1048576UL)
#define TG_GUI_UL_MAX_PARTS 4000UL
#define TG_GUI_UL_LIMIT (TG_GUI_DL_CHUNK * TG_GUI_UL_MAX_PARTS)

/* The ceiling in MiB, for the "file too big" message. Derived, never spelled
   out again: the GUI used to hardcode "31 MiB"/"250 MiB", which silently went
   WRONG the moment the per-platform chunk changed (m68k really became 125 MiB
   and MorphOS dropped from 250 to 125). */
unsigned long tg_gui_session_upload_limit_mib(void)
{
    return (unsigned long)(TG_GUI_UL_LIMIT / 1048576UL);
}

/* Why the last upload/download gave up, for the GUI status line (empty when
   there was none). Points at the shared query-failure reason. */
const char *tg_gui_session_last_transfer_error(void)
{
    return tg_mtproto_upload_failure_text(tg_mtproto_query_fail);
}

/* --- 0.0.8 punto 1b: the upload is a state machine too (one engine). ------
   begin() opens the file and resolves the peer, step() sends ONE part (or,
   after the last part, the sendMedia), end() closes up. Same design as the
   download engine above; one transfer at a time overall. */
typedef struct tg_gui_ul_state {
    int active;
    tg_mtproto_file_ctx fc;   /* peer_index repointed at the copy below */
    char peer_index_copy[64];
    char name[TG_MTPROTO_DOC_NAME_MAX]; /* bare filename for sendMedia */
    FILE *f;
    FILE *quiet;
    FILE *stream;
    unsigned long file_id_hi, file_id_lo;
    unsigned long pc, ph, pl, ahh, ahl;
    int hah;
    unsigned long parts, part;
    int big_file;
    int requested_photo;
    int as_photo;
    int photo_fallback;
    int part_retry;
    unsigned long got;  /* bytes of the CURRENT part held in part_buf */
    int part_loaded;    /* part_buf holds the current (unacknowledged) part */
    int rc;
    /* UTF-8 caption for the sendMedia, converted at begin(); empty for none.
       1024 bytes tracks the server's own caption limit for normal accounts.
       The photo-over-10-MiB document fallback carries it too. */
    char caption[1024];
} tg_gui_ul_state;

static tg_gui_ul_state tg_gui_ul;

static int tg_mtproto_jpeg_sof_marker(int marker)
{
    return (marker >= 0xc0 && marker <= 0xc3) ||
           (marker >= 0xc5 && marker <= 0xc7) ||
           (marker >= 0xc9 && marker <= 0xcb) ||
           (marker >= 0xcd && marker <= 0xcf);
}

/* Validate the JPEG marker stream through its first scan and EOI. A SOI plus a
   plausible SOF alone is not sufficient: a truncated upload would otherwise be
   accepted and rejected only after every part reached Telegram. The cursor is
   restored for the upload on every outcome. */
static int tg_mtproto_jpeg_file_valid(FILE *f)
{
    unsigned long segments;
    int c;
    int marker;
    int have_sof;
    int valid;

    have_sof = 0;
    valid = 0;
    if (f == 0 || fseek(f, 0L, SEEK_SET) != 0 ||
        fgetc(f) != 0xff || fgetc(f) != 0xd8) {
        if (f != 0) {
            (void)fseek(f, 0L, SEEK_SET);
        }
        return 0;
    }
    for (segments = 0UL; segments < 512UL; ++segments) {
        int len_hi;
        int len_lo;
        unsigned long segment_len;

        do {
            c = fgetc(f);
        } while (c != EOF && c != 0xff);
        if (c == EOF) {
            break;
        }
        do {
            marker = fgetc(f);
        } while (marker == 0xff);
        if (marker == EOF || marker == 0xd9) {
            break;
        }
        if (marker == 0x00 || marker == 0xd8 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        len_hi = fgetc(f);
        len_lo = fgetc(f);
        if (len_hi == EOF || len_lo == EOF) {
            break;
        }
        segment_len = ((unsigned long)(unsigned char)len_hi << 8) |
                      (unsigned long)(unsigned char)len_lo;
        if (segment_len < 2UL) {
            break;
        }
        if (marker == 0xda) {
            /* Skip the scan header, then walk entropy-coded bytes until EOI.
               A literal 0xff in entropy data is stuffed as 0xff00, so 0xffd9
               is unambiguous here. Progressive JPEGs may contain more marker
               segments/scans; scanning through them remains safe and bounded
               by EOF and the 10 MiB photo cap. */
            if (!have_sof ||
                fseek(f, (long)(segment_len - 2UL), SEEK_CUR) != 0) {
                break;
            }
            for (;;) {
                c = fgetc(f);
                if (c == EOF) {
                    break;
                }
                if (c != 0xff) {
                    continue;
                }
                do {
                    marker = fgetc(f);
                } while (marker == 0xff);
                if (marker == EOF) {
                    break;
                }
                if (marker == 0xd9) {
                    valid = 1;
                    break;
                }
            }
            break;
        }
        if (tg_mtproto_jpeg_sof_marker(marker)) {
            int precision;
            int h_hi;
            int h_lo;
            int w_hi;
            int w_lo;
            unsigned long width;
            unsigned long height;

            if (segment_len < 7UL) {
                break;
            }
            precision = fgetc(f);
            h_hi = fgetc(f);
            h_lo = fgetc(f);
            w_hi = fgetc(f);
            w_lo = fgetc(f);
            if (precision == EOF || h_hi == EOF || h_lo == EOF ||
                w_hi == EOF || w_lo == EOF) {
                break;
            }
            height = ((unsigned long)(unsigned char)h_hi << 8) |
                     (unsigned long)(unsigned char)h_lo;
            width = ((unsigned long)(unsigned char)w_hi << 8) |
                    (unsigned long)(unsigned char)w_lo;
            if (precision == 0 || width == 0UL || height == 0UL) {
                break;
            }
            have_sof = 1;
            if (fseek(f, (long)(segment_len - 7UL), SEEK_CUR) != 0) {
                break;
            }
            continue;
        }
        if (fseek(f, (long)(segment_len - 2UL), SEEK_CUR) != 0) {
            break;
        }
    }
    (void)fseek(f, 0L, SEEK_SET);
    return valid;
}

/* Telegram's own limits for a photo, applied before a single byte goes up:
   width plus height at most 10000, and neither side more than 20 times the
   other. The server answers PHOTO_INVALID_DIMENSIONS otherwise, after the
   whole upload; saying it here costs nothing and saves the transfer. */
static int tg_mtproto_photo_dims_ok(unsigned long w, unsigned long h,
                                    const char **why)
{
    if (w == 0UL || h == 0UL) {
        *why = "image has no size";
        return 0;
    }
    if (w + h > 10000UL) {
        *why = "image too large for a photo (width + height over 10000)";
        return 0;
    }
    if (w > h * 20UL || h > w * 20UL) {
        *why = "image too elongated for a photo (over 20:1)";
        return 0;
    }
    return 1;
}

/* Validate a PNG the way the JPEG walk does: the 8 byte signature, an IHDR
   as the first chunk with a size Telegram will take, then every chunk up to
   IEND, so a truncated file is refused here and not after the last part
   reached the server. CRCs are not checked; the server decodes the file and
   will say if it is broken inside. The cursor is restored on every outcome. */
static int tg_mtproto_png_file_valid(FILE *f, unsigned long *out_w,
                                     unsigned long *out_h)
{
    static const unsigned char sig[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU
    };
    unsigned char head[8];
    unsigned char ihdr[13];
    unsigned long chunks;
    int valid = 0;

    *out_w = 0UL;
    *out_h = 0UL;
    if (f == 0 || fseek(f, 0L, SEEK_SET) != 0 ||
        fread(head, 1, 8, f) != 8 || memcmp(head, sig, 8) != 0) {
        if (f != 0) {
            (void)fseek(f, 0L, SEEK_SET);
        }
        return 0;
    }
    for (chunks = 0UL; chunks < 65536UL; ++chunks) {
        unsigned char ch[8];
        unsigned long len;

        if (fread(ch, 1, 8, f) != 8) {
            break; /* ran out before IEND: truncated */
        }
        len = ((unsigned long)ch[0] << 24) | ((unsigned long)ch[1] << 16) |
              ((unsigned long)ch[2] << 8) | (unsigned long)ch[3];
        if (len > 0x7fffffffUL) {
            break;
        }
        if (chunks == 0UL) {
            if (memcmp(ch + 4, "IHDR", 4) != 0 || len != 13UL ||
                fread(ihdr, 1, 13, f) != 13) {
                break;
            }
            *out_w = ((unsigned long)ihdr[0] << 24) |
                     ((unsigned long)ihdr[1] << 16) |
                     ((unsigned long)ihdr[2] << 8) | (unsigned long)ihdr[3];
            *out_h = ((unsigned long)ihdr[4] << 24) |
                     ((unsigned long)ihdr[5] << 16) |
                     ((unsigned long)ihdr[6] << 8) | (unsigned long)ihdr[7];
            if (fseek(f, 4L, SEEK_CUR) != 0) { /* CRC */
                break;
            }
            continue;
        }
        if (memcmp(ch + 4, "IEND", 4) == 0) {
            valid = 1;
            break;
        }
        if (fseek(f, (long)len + 4L, SEEK_CUR) != 0) { /* data + CRC */
            break;
        }
    }
    if (valid && (fgetc(f) == EOF) == 0) {
        /* bytes after IEND are tolerated by decoders; keep it valid */
    }
    (void)fseek(f, 0L, SEEK_SET);
    return valid;
}

/* The photo gate: the first bytes say what the file is, the matching walk
   says whether it is whole, and Telegram's size rule says whether it will be
   taken as a photo. `why` gets a sentence a status line can show. Extension
   is not consulted: a JPEG called .png is still a JPEG. */
static int tg_mtproto_photo_file_valid(FILE *f, const char **why)
{
    unsigned char magic[4];
    unsigned long w = 0UL;
    unsigned long h = 0UL;

    *why = "not a valid JPEG or PNG";
    if (f == 0 || fseek(f, 0L, SEEK_SET) != 0 ||
        fread(magic, 1, 4, f) != 4) {
        if (f != 0) {
            (void)fseek(f, 0L, SEEK_SET);
        }
        return 0;
    }
    if (magic[0] == 0xffU && magic[1] == 0xd8U) {
        if (!tg_mtproto_jpeg_file_valid(f)) {
            *why = "not a valid JPEG (truncated or damaged)";
            return 0;
        }
        return 1; /* the JPEG walk already refused an empty frame */
    }
    if (magic[0] == 0x89U && magic[1] == 'P' && magic[2] == 'N' &&
        magic[3] == 'G') {
        if (!tg_mtproto_png_file_valid(f, &w, &h)) {
            *why = "not a valid PNG (truncated or damaged)";
            return 0;
        }
        return tg_mtproto_photo_dims_ok(w, h, why);
    }
    (void)fseek(f, 0L, SEEK_SET);
    return 0;
}

/* The server's own refusals of a photo, said in words. Anything else keeps
   the RPC name, which is what a bug report needs. */
static const char *tg_mtproto_upload_failure_text(const char *raw)
{
    if (raw == 0) {
        return 0;
    }
    if (strcmp(raw, "PHOTO_INVALID_DIMENSIONS") == 0) {
        return "Telegram refused the photo's size (width + height over "
               "10000, or over 20:1)";
    }
    if (strcmp(raw, "PHOTO_EXT_INVALID") == 0) {
        return "Telegram does not take this image format as a photo";
    }
    if (strcmp(raw, "IMAGE_PROCESS_FAILED") == 0) {
        return "Telegram could not decode the image";
    }
    if (strcmp(raw, "PHOTO_SAVE_FILE_INVALID") == 0) {
        return "Telegram could not save the photo";
    }
    return raw;
}

#if !defined(TG_NO_SELFTEST)
/* Host-runnable: the gate says yes to a whole JPEG and a whole PNG, no to a
   truncated one of each, no to a PNG Telegram would refuse for its size, and
   no to plain text, each with the sentence a status line will show. */
static int tg_mtproto_photo_gate_self_test(void)
{
    static const unsigned char jpeg_ok[] = {
        0xff,0xd8, 0xff,0xc0,0x00,0x0b,0x08,0x00,0x08,0x00,0x08,0x01,0x01,0x11,0x00,
        0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x00,0x3f,0x00, 0x12,0x34, 0xff,0xd9 };
    static const unsigned char png_head[] = {
        0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
        0x00,0x00,0x00,0x0d,'I','H','D','R', 0x00,0x00,0x00,0x08, 0x00,0x00,0x00,0x08,
        0x08,0x02,0x00,0x00,0x00, 0x00,0x00,0x00,0x00 };
    static const unsigned char png_tail[] = {
        0x00,0x00,0x00,0x01,'I','D','A','T',0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,'I','E','N','D', 0xae,0x42,0x60,0x82 };
    static const unsigned char txt[] = "hello, not an image\n";
    const char *path = "tg-photo-gate-selftest.bin";
    struct { const unsigned char *a; unsigned long an; const unsigned char *b;
             unsigned long bn; int want; const char *why_has; } c[6];
    unsigned char png_big[sizeof(png_head)];
    int i;

    memcpy(png_big, png_head, sizeof(png_head));
    png_big[16] = 0x00; png_big[17] = 0x00; png_big[18] = 0x27; png_big[19] = 0x10; /* w 10000 */
    png_big[20] = 0x00; png_big[21] = 0x00; png_big[22] = 0x00; png_big[23] = 0x08; /* h 8 */
    c[0].a = jpeg_ok; c[0].an = sizeof(jpeg_ok); c[0].b = 0; c[0].bn = 0UL; c[0].want = 1; c[0].why_has = 0;
    c[1].a = jpeg_ok; c[1].an = sizeof(jpeg_ok) - 4UL; c[1].b = 0; c[1].bn = 0UL; c[1].want = 0; c[1].why_has = "JPEG";
    c[2].a = png_head; c[2].an = sizeof(png_head); c[2].b = png_tail; c[2].bn = sizeof(png_tail); c[2].want = 1; c[2].why_has = 0;
    c[3].a = png_head; c[3].an = sizeof(png_head); c[3].b = png_tail; c[3].bn = 9UL; c[3].want = 0; c[3].why_has = "PNG";
    c[4].a = png_big; c[4].an = sizeof(png_big); c[4].b = png_tail; c[4].bn = sizeof(png_tail); c[4].want = 0; c[4].why_has = "10000";
    c[5].a = txt; c[5].an = sizeof(txt) - 1UL; c[5].b = 0; c[5].bn = 0UL; c[5].want = 0; c[5].why_has = "JPEG or PNG";
    for (i = 0; i < 6; ++i) {
        FILE *f = fopen(path, "wb");
        const char *why = 0;
        int got;

        if (f == 0) {
            return 2;
        }
        fwrite(c[i].a, 1, (size_t)c[i].an, f);
        if (c[i].b != 0) {
            fwrite(c[i].b, 1, (size_t)c[i].bn, f);
        }
        fclose(f);
        f = fopen(path, "rb");
        got = f != 0 ? tg_mtproto_photo_file_valid(f, &why) : -1;
        if (f != 0) {
            fclose(f);
        }
        (void)remove(path);
        if (got != c[i].want ||
            (c[i].why_has != 0 && (why == 0 || strstr(why, c[i].why_has) == 0))) {
            printf("photo gate self-test: case %d gave %d (%s)\n", i, got,
                   why != 0 ? why : "-");
            return 2;
        }
    }
    if (strcmp(tg_mtproto_upload_failure_text("PHOTO_INVALID_DIMENSIONS"),
               "PHOTO_INVALID_DIMENSIONS") == 0 ||
        strcmp(tg_mtproto_upload_failure_text("FLOOD_WAIT_3"), "FLOOD_WAIT_3") != 0) {
        puts("photo gate self-test: server refusal wording");
        return 2;
    }
#if TG_MTPROTO_DISPLAY_LATIN1
    /* 0.0.93: an emoji pair in composer text goes out as the codepoint's
       UTF-8; a lone prefix byte stays the two byte Latin-1 form as before. */
    {
        char src[8];
        char out[32];
        unsigned long cp;
        unsigned long need;

        if (!tg_gui_emoji_encode(0UL, src)) {
            return 2;
        }
        src[2] = '!'; src[3] = '\0';
        cp = tg_emoji_sheet_codepoints[0];
        need = cp >= 0x10000UL ? 4UL : 3UL;
        if (!tg_mtproto_latin1_to_utf8(src, out, sizeof(out)) ||
            strlen(out) != need + 1UL || out[need] != '!' ||
            (need == 4UL && (unsigned char)out[0] != (0xf0U | (cp >> 18))) ||
            (need == 3UL && (unsigned char)out[0] != (0xe0U | (cp >> 12))) ||
            (unsigned char)out[need - 1UL] != (0x80U | (cp & 0x3fU))) {
            puts("photo gate self-test: emoji pair did not expand to UTF-8");
            return 2;
        }
        src[0] = (char)0x80; src[1] = '\0';
        if (!tg_mtproto_latin1_to_utf8(src, out, sizeof(out)) ||
            strlen(out) != 2UL || (unsigned char)out[0] != 0xc2U) {
            puts("photo gate self-test: lone prefix changed meaning");
            return 2;
        }
    }
#endif
    return 0;
}
#endif

/* Open the file, size/limit checks, peer resolution, file_id. 0 = armed;
   != 0 = failed fast with the usual rc codes (1 generic, 2 too big, 3 file,
   5 empty). */
static int tg_mtproto_upload_begin(const tg_mtproto_file_ctx *fc,
                                   const char *caption,
                                   const char *path, FILE *stream,
                                   int as_photo)
{
    const char *why = 0;
    unsigned char rnd[8];
    long file_size;
    const char *name;
    const char *p;
    unsigned long n;

    if (fc == 0 || stream == 0 || path == 0 || path[0] == '\0' ||
        fc->peer_index == 0 || fc->peer_index[0] == '\0' ||
        tg_gui_ul.active) {
        return 1;
    }
    memset(&tg_gui_ul, 0, sizeof(tg_gui_ul));
    tg_gui_ul.requested_photo = as_photo != 0;
    tg_gui_ul.fc = *fc;
    for (n = 0UL; fc->peer_index[n] != '\0' &&
                  n + 1UL < sizeof(tg_gui_ul.peer_index_copy); ++n) {
        tg_gui_ul.peer_index_copy[n] = fc->peer_index[n];
    }
    tg_gui_ul.peer_index_copy[n] = '\0';
    tg_gui_ul.fc.peer_index = tg_gui_ul.peer_index_copy;
    if (caption != 0 && caption[0] != '\0') {
#if TG_MTPROTO_DISPLAY_LATIN1
        /* The composer/console text is ISO-8859-1 (Amiga keymap): encode it
           for the wire. On overflow keep the raw bytes, the same best-effort
           the message send path uses. */
        if (!tg_mtproto_latin1_to_utf8(caption, tg_gui_ul.caption,
                                       sizeof(tg_gui_ul.caption))) {
            unsigned long ci;

            for (ci = 0UL; caption[ci] != '\0' &&
                           ci + 1UL < sizeof(tg_gui_ul.caption); ++ci) {
                tg_gui_ul.caption[ci] = caption[ci];
            }
            tg_gui_ul.caption[ci] = '\0';
        }
#else
        {
            unsigned long ci;

            for (ci = 0UL; caption[ci] != '\0' &&
                           ci + 1UL < sizeof(tg_gui_ul.caption); ++ci) {
                tg_gui_ul.caption[ci] = caption[ci];
            }
            tg_gui_ul.caption[ci] = '\0';
        }
#endif
    }
    tg_gui_ul.stream = stream;
    tg_mtproto_query_fail[0] = '\0'; /* fresh reason for this upload */
    tg_gui_ul.f = fopen(path, "rb");
    if (tg_gui_ul.f == 0) {
        return 3;
    }
    if (fseek(tg_gui_ul.f, 0L, SEEK_END) == 0 && ftell(tg_gui_ul.f) == 0L) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        return 5; /* empty file: nothing to upload (0-byte markers, etc.) */
    }
    if (fseek(tg_gui_ul.f, 0L, SEEK_END) != 0 ||
        (file_size = ftell(tg_gui_ul.f)) <= 0L ||
        fseek(tg_gui_ul.f, 0L, SEEK_SET) != 0) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        return 3;
    }
    if ((unsigned long)file_size > TG_GUI_UL_LIMIT) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        return 2;
    }
    tg_gui_ul.big_file = (unsigned long)file_size > TG_GUI_UL_BIG_THRESHOLD;
    if (tg_gui_ul.requested_photo) {
        if (tg_gui_ul.big_file) {
            tg_gui_ul.photo_fallback = 1;
        } else if (!tg_mtproto_photo_file_valid(tg_gui_ul.f, &why)) {
            sprintf(tg_mtproto_query_fail, "%.63s", why); /* 64 byte buffer */
            fclose(tg_gui_ul.f);
            tg_gui_ul.f = 0;
            return 7;
        } else {
            tg_gui_ul.as_photo = 1;
        }
    }
    /* Bare filename: whatever follows the last '/' or ':' of the path.
       COPIED: the sendMedia goes out long after begin() returns and the
       caller's path buffer may be reused by then. */
    name = path;
    for (p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == ':') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        return 3;
    }
    for (n = 0UL; name[n] != '\0' && n + 1UL < sizeof(tg_gui_ul.name); ++n) {
        tg_gui_ul.name[n] = name[n];
    }
    tg_gui_ul.name[n] = '\0';
    tg_gui_ul.quiet = tg_mtproto_open_quiet_stream(stream);
    if (tg_mtproto_load_peer_cache_peer(
            tg_gui_ul.fc.peer_cache_file,
            tg_gui_ul.fc.peer_index, &tg_gui_ul.pc, &tg_gui_ul.ph,
            &tg_gui_ul.pl, &tg_gui_ul.ahh,
            &tg_gui_ul.ahl, &tg_gui_ul.hah, tg_gui_ul.quiet,
            tg_gui_ul.as_photo ? "mtproto sendMedia(photo)"
                               : "mtproto sendMedia(document)") != 0) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        tg_mtproto_close_quiet_stream(tg_gui_ul.quiet, stream);
        tg_gui_ul.quiet = 0;
        return 1;
    }
    tg_mtproto_saved_session_random(rnd, sizeof(rnd));
    tg_gui_ul.file_id_lo = tg_mtproto_read_u32_le(rnd);
    tg_gui_ul.file_id_hi = tg_mtproto_read_u32_le(rnd + 4U);
    tg_gui_ul.parts = ((unsigned long)file_size + TG_GUI_DL_CHUNK - 1UL) /
                      TG_GUI_DL_CHUNK;
    if (tg_gui_ul.parts == 0UL || tg_gui_ul.parts > TG_GUI_UL_MAX_PARTS) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
        tg_mtproto_close_quiet_stream(tg_gui_ul.quiet, stream);
        tg_gui_ul.quiet = 0;
        return 2;
    }
    tg_gui_ul.rc = 1; /* any early break below is a generic failure */
    tg_gui_ul.active = 1;
    return 0;
}

/* Send ONE part per call (a failed part retries on the NEXT call, same
   buffer: saveFilePart is idempotent for (file_id, part)); once every part
   is in, the same call sends the sendMedia. Returns 1 while running, 0 once
   finished (rc set); then call tg_mtproto_upload_end(). */
static int tg_mtproto_upload_step(void)
{
    static unsigned char part_buf[TG_GUI_DL_CHUNK];
    static unsigned char part_query[TG_GUI_DL_CHUNK + 64UL];
    unsigned char query[512];
    unsigned char rnd[8];
    tg_mtproto_tl_writer writer;
    tg_mtproto_rpc_result result;

    if (!tg_gui_ul.active || tg_gui_ul.rc == 6) {
        return 0; /* idle, or cancelled: don't send more parts / the media */
    }
    if (tg_gui_ul.part >= tg_gui_ul.parts) {
        /* Every part is up: attach them to the chat with the sendMedia. */
        unsigned long rand_hi, rand_lo;
        tg_mtproto_tl_status build_status;
        const char *query_name;

        tg_mtproto_saved_session_random(rnd, sizeof(rnd));
        rand_lo = tg_mtproto_read_u32_le(rnd);
        rand_hi = tg_mtproto_read_u32_le(rnd + 4U);
        tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
        if (tg_gui_ul.as_photo) {
            build_status = tg_mtproto_build_messages_send_media_photo(
                &writer, tg_gui_ul.pc, tg_gui_ul.ph, tg_gui_ul.pl,
                tg_gui_ul.ahh, tg_gui_ul.ahl, tg_gui_ul.hah,
                tg_gui_ul.file_id_hi, tg_gui_ul.file_id_lo,
                tg_gui_ul.parts, tg_gui_ul.name, tg_gui_ul.caption,
                rand_hi, rand_lo);
            query_name = "mtproto sendMedia(photo)";
        } else if (tg_gui_ul.big_file) {
            build_status = tg_mtproto_build_messages_send_media_big_document(
                &writer, tg_gui_ul.pc, tg_gui_ul.ph, tg_gui_ul.pl,
                tg_gui_ul.ahh, tg_gui_ul.ahl, tg_gui_ul.hah,
                tg_gui_ul.file_id_hi, tg_gui_ul.file_id_lo,
                tg_gui_ul.parts, tg_gui_ul.name,
                "application/octet-stream", tg_gui_ul.caption,
                rand_hi, rand_lo);
            query_name = "mtproto sendMedia(document)";
        } else {
            build_status = tg_mtproto_build_messages_send_media_document(
                &writer, tg_gui_ul.pc, tg_gui_ul.ph, tg_gui_ul.pl,
                tg_gui_ul.ahh, tg_gui_ul.ahl, tg_gui_ul.hah,
                tg_gui_ul.file_id_hi, tg_gui_ul.file_id_lo,
                tg_gui_ul.parts, tg_gui_ul.name,
                "application/octet-stream", tg_gui_ul.caption,
                rand_hi, rand_lo);
            query_name = "mtproto sendMedia(document)";
        }
        if (build_status != TG_MTPROTO_TL_OK) {
            return 0; /* rc 1 */
        }
        memset(&result, 0, sizeof(result));
        if (tg_mtproto_send_saved_query_on_context(
                tg_gui_ul.fc.host, tg_gui_ul.fc.port,
                tg_gui_ul.fc.api_id, tg_gui_ul.fc.auth_file,
                tg_gui_ul.fc.dc_id_text, tg_gui_ul.fc.context,
                query, writer.length, &result, tg_gui_ul.quiet,
                query_name, 600U) != 0 ||
            result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
            return 0; /* rc 1 */
        }
        tg_gui_ul.rc = 0; /* the tick's history poll shows the sent row */
        return 0;
    }
    if (!tg_gui_ul.part_loaded) {
        tg_gui_ul.got = (unsigned long)fread(part_buf, 1, TG_GUI_DL_CHUNK,
                                             tg_gui_ul.f);
        if (tg_gui_ul.got == 0UL) {
            sprintf(tg_mtproto_query_fail, "disk read failed at part %lu",
                    tg_gui_ul.part + 1UL);
            return 0; /* short read = disk trouble (rc 1) */
        }
        tg_gui_ul.part_loaded = 1;
        tg_gui_ul.part_retry = 0;
    }
    tg_mtproto_tl_writer_init(&writer, part_query, sizeof(part_query));
    if ((tg_gui_ul.big_file
             ? tg_mtproto_build_upload_save_big_file_part(
                   &writer, tg_gui_ul.file_id_hi, tg_gui_ul.file_id_lo,
                   tg_gui_ul.part, tg_gui_ul.parts, part_buf, tg_gui_ul.got)
             : tg_mtproto_build_upload_save_file_part(
                   &writer, tg_gui_ul.file_id_hi, tg_gui_ul.file_id_lo,
                   tg_gui_ul.part, part_buf, tg_gui_ul.got)) !=
        TG_MTPROTO_TL_OK) {
        sprintf(tg_mtproto_query_fail, "build failed at part %lu",
                tg_gui_ul.part + 1UL);
        return 0; /* local build error: not retryable (rc 1) */
    }
    memset(&result, 0, sizeof(result));
    if (tg_mtproto_send_saved_query_on_context(
            tg_gui_ul.fc.host, tg_gui_ul.fc.port,
            tg_gui_ul.fc.api_id, tg_gui_ul.fc.auth_file,
            tg_gui_ul.fc.dc_id_text,
            tg_gui_ul.fc.context, part_query, writer.length,
            &result, tg_gui_ul.quiet,
            tg_gui_ul.big_file ? "mtproto saveBigFilePart"
                               : "mtproto saveFilePart",
            600U) == 0 &&
        result.result_constructor == 0x997275b5UL /* boolTrue */) {
        ++tg_gui_ul.part;
        tg_gui_ul.part_loaded = 0;
        return 1; /* next call: next part (or the sendMedia) */
    }
    /* Part failed (a lost chunk / slow-link timeout / non-boolTrue).
       Re-send the SAME part next call: saveFilePart is idempotent for
       (file_id, part) and part_buf still holds the data, so no re-read. */
    if (++tg_gui_ul.part_retry > TG_GUI_DL_CHUNK_RETRIES) {
        char pf[80];

        sprintf(pf, "part %lu/%lu: %.48s", tg_gui_ul.part + 1UL,
                tg_gui_ul.parts,
                tg_mtproto_query_fail[0] != '\0'
                    ? tg_mtproto_query_fail : "no reply");
        strcpy(tg_mtproto_query_fail, pf);
        return 0; /* rc 1 */
    }
    {
        char rl[96];
        sprintf(rl, "upload: retry %d part %lu/%lu (%.32s)",
                tg_gui_ul.part_retry, tg_gui_ul.part + 1UL, tg_gui_ul.parts,
                tg_mtproto_query_fail);
        tg_gui_log(rl);
        if (strncmp(tg_mtproto_query_fail, "send", 4) == 0 ||
            strncmp(tg_mtproto_query_fail, "transport", 9) == 0) {
            /* A wedged socket never recovers by itself: reconnect. */
            tg_mtproto_close_auth_context(tg_gui_ul.fc.context);
        }
    }
    return 1;
}

/* Cancel a running upload: no sendMedia is sent, the uploaded parts are
   left for Telegram to expire. */
static void tg_mtproto_upload_cancel(void)
{
    if (tg_gui_ul.active) {
        tg_gui_ul.rc = 6; /* cancelled (5 already means empty file) */
        tg_gui_log("upload: cancelled by user");
    }
}

/* Close the engine and return the final rc. */
static int tg_mtproto_upload_end(void)
{
    int rc = tg_gui_ul.rc;

    if (tg_gui_ul.f != 0) {
        fclose(tg_gui_ul.f);
        tg_gui_ul.f = 0;
    }
    if (tg_gui_ul.quiet != 0) {
        tg_mtproto_close_quiet_stream(tg_gui_ul.quiet, tg_gui_ul.stream);
        tg_gui_ul.quiet = 0;
    }
    tg_gui_ul.active = 0;
    return rc;
}

/* Blocking wrapper over the state machine (TUI and legacy callers). */
static int tg_mtproto_file_send(const tg_mtproto_file_ctx *fc,
                                const char *path, FILE *stream,
                                tg_gui_upload_progress_fn progress,
                                void *progress_data,
                                int as_photo, const char *caption)
{
    int brc;

    brc = tg_mtproto_upload_begin(fc, caption, path, stream, as_photo);
    if (brc != 0) {
        return brc;
    }
    while (tg_mtproto_upload_step()) {
        if (progress != 0 &&
            progress(tg_gui_ul.part, tg_gui_ul.parts, progress_data) != 0) {
            tg_mtproto_upload_cancel();
            break;
        }
    }
    return tg_mtproto_upload_end();
}

/* Returns: 0 ok, 1 generic failure, 2 foreign DC (needs multi-DC, deferred),
   3 could not create/write the file. Writes the saved path into out_path. */
/* --- 0.0.8 punto 1c: multi-DC file channel. --------------------------------
   A foreign document (doc.dc_id != home, or a FILE_MIGRATE_X reply) downloads
   from ITS datacenter: the file channel gets a per-DC auth key (full DH on
   first contact, cached in data/telegram-auth-dc<N>.bin) and the account
   authority travels once per run with auth.exportAuthorization (home DC) +
   auth.importAuthorization (target DC). One foreign channel at a time, kept
   open for the session. TL hashes checked against core.telegram.org/schema
   on 2026-07-25:
     auth.exportAuthorization#e5bfffcd dc_id:int = auth.ExportedAuthorization;
     auth.exportedAuthorization#b434e2b8 id:long bytes:bytes;
     auth.importAuthorization#a57a7dad id:long bytes:bytes = auth.Authorization; */
static tg_mtproto_auth_context tg_gui_foreign_context;
static unsigned long tg_gui_foreign_dc; /* DC the channel points at; 0 none */
/* Sized for a full %lu (dc is 1..5 in practice, the DC table validates it
   first, but gcc's format-overflow check reasons about the whole range). */
static char tg_gui_foreign_dc_text[24];
static char tg_gui_foreign_auth_file[56];
static int tg_gui_foreign_imported; /* account authority imported this run */

static int tg_gui_session_setup_foreign_channel(
    const tg_mtproto_file_ctx *home, unsigned long dc,
    tg_mtproto_file_ctx *out, FILE *stream)
{
    const tg_mtproto_dc_option *opt;

    opt = tg_mtproto_dc_by_id((int)dc);
    if (home == 0 || out == 0 || stream == 0 || opt == 0) {
        return 1;
    }
    if (tg_gui_foreign_dc != dc) {
        if (tg_gui_foreign_context.connection_open) {
            tg_mtproto_close_auth_context(&tg_gui_foreign_context);
        }
        sprintf(tg_gui_foreign_dc_text, "%lu", dc);
        sprintf(tg_gui_foreign_auth_file, "data/telegram-auth-dc%lu.bin", dc);
        tg_gui_foreign_dc = dc;
        tg_gui_foreign_imported = 0;
    }
    if (!tg_gui_foreign_context.connection_open) {
        if (tg_mtproto_load_auth_context(opt->mt_ip, "443",
                                         tg_gui_foreign_auth_file,
                                         &tg_gui_foreign_context, stream,
                                         "mtproto file-dc") != 0) {
            /* No cached key for this DC yet: full DH handshake, then keep it.
               The slow part (seconds, stack-heavy on m68k like the login DH)
               happens once per DC, ever -- the key file is reused after. */
            tg_gui_log("file-dc: no cached key, DH handshake");
            if (tg_mtproto_open_auth_context(opt->mt_ip, "443",
                                             tg_gui_foreign_dc_text,
                                             &tg_gui_foreign_context, stream,
                                             "mtproto file-dc") != 0) {
                return 1;
            }
            if (tg_mtproto_session_save_authorization(
                    tg_gui_foreign_auth_file,
                    &tg_gui_foreign_context.session,
                    tg_gui_foreign_context.auth_key, 1) !=
                TG_MTPROTO_SESSION_OK) {
                /* Not fatal: the channel works, only the reuse cache is
                   lost (next run redoes the DH). */
                tg_gui_log("file-dc: auth cache save failed");
            }
            tg_gui_foreign_imported = 0;
        }
        tg_gui_log("file-dc: channel open");
    }
    if (!tg_gui_foreign_imported) {
        unsigned char q[640];
        tg_mtproto_tl_writer writer;
        tg_mtproto_rpc_result result;
        unsigned long id_lo;
        unsigned long id_hi;
        const unsigned char *abytes;
        unsigned long abytes_len;

        /* Export the account authority from the HOME DC (on the file
           channel: same class of bounded RPC as a chunk)... */
        tg_mtproto_tl_writer_init(&writer, q, sizeof(q));
        if (tg_mtproto_tl_write_u32(&writer, 0xe5bfffcdUL) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_write_u32(&writer, dc) != TG_MTPROTO_TL_OK) {
            return 1;
        }
        memset(&result, 0, sizeof(result));
        if (tg_mtproto_send_saved_query_on_context(
                home->host, home->port, home->api_id, home->auth_file,
                home->dc_id_text, home->context, q, writer.length, &result,
                stream, "mtproto exportAuthorization", 600U) != 0 ||
            tg_mtproto_unpack_gzip_result(&result, stream,
                                          "mtproto exportAuthorization") !=
                0 ||
            result.result_constructor != 0xb434e2b8UL ||
            result.result_body_length < 9UL) {
            tg_gui_log("file-dc: export failed");
            return 1;
        }
        id_lo = tg_mtproto_read_u32_le(result.result_body);
        id_hi = tg_mtproto_read_u32_le(result.result_body + 4U);
        {
            tg_mtproto_tl_reader reader;

            tg_mtproto_tl_reader_init(&reader, result.result_body + 8U,
                                      result.result_body_length - 8UL);
            if (tg_mtproto_tl_read_bytes(&reader, &abytes, &abytes_len) !=
                    TG_MTPROTO_TL_OK ||
                abytes_len == 0UL || abytes_len > 512UL) {
                tg_gui_log("file-dc: export parse failed");
                return 1;
            }
        }
        /* ...and import it on the target DC. First query on that channel,
           so send_saved_query wraps it in initConnection+invokeWithLayer --
           exactly the shape official clients use for the import. */
        tg_mtproto_tl_writer_init(&writer, q, sizeof(q));
        if (tg_mtproto_tl_write_u32(&writer, 0xa57a7dadUL) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_write_u32(&writer, id_lo) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_write_u32(&writer, id_hi) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_write_bytes(&writer, abytes, abytes_len) !=
                TG_MTPROTO_TL_OK) {
            return 1;
        }
        memset(&result, 0, sizeof(result));
        if (tg_mtproto_send_saved_query_on_context(
                opt->mt_ip, "443", home->api_id, tg_gui_foreign_auth_file,
                tg_gui_foreign_dc_text, &tg_gui_foreign_context, q,
                writer.length, &result, stream,
                "mtproto importAuthorization", 600U) != 0 ||
            result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
            tg_gui_log("file-dc: import failed");
            return 1;
        }
        tg_gui_foreign_imported = 1;
        tg_gui_log("file-dc: account imported");
    }
    *out = *home;
    out->host = opt->mt_ip;
    out->port = "443";
    out->dc_id_text = tg_gui_foreign_dc_text;
    out->auth_file = tg_gui_foreign_auth_file;
    out->context = &tg_gui_foreign_context;
    return 0;
}

/* --- 0.0.8 punto 1b: the download is a resumable STATE MACHINE. -----------
   begin() arms it, step() moves ONE chunk, end() closes it. The GUI pumps
   step() from the event loop (window stays alive during a transfer); the TUI
   and any legacy caller run the same engine to completion through the
   blocking wrapper below, so there is exactly one download engine. One
   transfer at a time by design (static state). */
typedef struct tg_gui_dl_state {
    int active;
    tg_mtproto_file_ctx fc;   /* peer_index repointed at the copy below */
    tg_mtproto_file_ctx fc_file; /* where the CHUNKS flow: home, or the
                                    foreign DC channel (0.0.8 punto 1c);
                                    find/refetch always stay on fc (home) */
    unsigned long need_dc;    /* != 0: open the file channel on this DC
                                 before the next chunk */
    /* 0.0.8 punto 1d: ONE prefetched chunk in flight. When armed, the
       getFile for `pre_offset` is already on the wire and its reply is
       matched by `pre_id`; the RTT of that request hides behind the
       current chunk's own wait. Any anomaly drops the pipeline (the
       connection is closed, which drains it) and the proven synchronous
       retry takes the chunk. */
    int pre_armed;
    tg_mtproto_message_id pre_id;
    unsigned long pre_offset;
    int migrations;           /* FILE_MIGRATE hops, bounded */
    char peer_index_copy[64]; /* the live one moves when the user changes chat */
    unsigned long msg_id;
    unsigned long resolved_msg_id;
    tg_mtproto_document_meta doc;
    char path[TG_MTPROTO_DOC_NAME_MAX + 16];
    FILE *f;
    FILE *quiet;
    FILE *stream;
    unsigned long offset;
    int chunk_retry;
    int refetched;
    int rc;       /* final outcome, meaningful once a step returned 0 */
    char fail[96]; /* failure reason for the caller's status line */
} tg_gui_dl_state;

static tg_gui_dl_state tg_gui_dl;

/* Resolve the document, guard the DC, create the local file: everything up
   to the first chunk. 0 = armed (active=1); != 0 = failed fast with the same
   rc codes the blocking call always used (reason in tg_gui_dl.fail). */
static int tg_mtproto_download_begin(const tg_mtproto_file_ctx *fc,
                                     unsigned long msg_id, FILE *stream)
{
    char safe[TG_MTPROTO_DOC_NAME_MAX];
    unsigned long home;
    unsigned long n;

    if (tg_gui_dl.active) {
        return 1; /* one transfer at a time; fail[] belongs to the running one */
    }
    memset(&tg_gui_dl, 0, sizeof(tg_gui_dl)); /* fail[] cleared for the caller */
    if (fc == 0 || stream == 0 ||
        fc->peer_index == 0 || fc->peer_index[0] == '\0') {
        return 1;
    }
    tg_gui_dl.fc = *fc;
    /* Copy the peer index: the caller's points at the session's CURRENT chat
       and would silently retarget this transfer on a chat switch. */
    for (n = 0UL; fc->peer_index[n] != '\0' &&
                  n + 1UL < sizeof(tg_gui_dl.peer_index_copy); ++n) {
        tg_gui_dl.peer_index_copy[n] = fc->peer_index[n];
    }
    tg_gui_dl.peer_index_copy[n] = '\0';
    tg_gui_dl.fc.peer_index = tg_gui_dl.peer_index_copy;
    tg_gui_dl.msg_id = msg_id;
    tg_gui_dl.stream = stream;
    tg_gui_dl.quiet = tg_mtproto_open_quiet_stream(stream);
    tg_dl_diag[0] = '\0';
    if (tg_mtproto_file_find_document(&tg_gui_dl.fc, msg_id, tg_gui_dl.quiet,
                                      &tg_gui_dl.doc,
                                      &tg_gui_dl.resolved_msg_id) != 0) {
        tg_gui_log("download: not found");
        sprintf(tg_gui_dl.fail, "%.90s", tg_dl_diag);
        tg_mtproto_close_quiet_stream(tg_gui_dl.quiet, stream);
        tg_gui_dl.quiet = 0;
        return 1;
    }
    {
        char d[128];
        sprintf(d, "download: doc dc=%lu size=%lu:%lu reflen=%lu",
                tg_gui_dl.doc.dc_id, tg_gui_dl.doc.size_hi,
                tg_gui_dl.doc.size_lo, tg_gui_dl.doc.file_reference_len);
        tg_gui_log(d);
    }
    /* Same-DC guard (foreign documents need export/importAuthorization). */
    home = 0UL;
    if (tg_gui_dl.fc.dc_id_text[0] != '\0') {
        const char *p = tg_gui_dl.fc.dc_id_text;

        while (*p >= '0' && *p <= '9') {
            home = home * 10UL + (unsigned long)(*p - '0');
            ++p;
        }
    }
    tg_gui_dl.fc_file = tg_gui_dl.fc; /* same-DC default: chunks flow home */
    if (home != 0UL && tg_gui_dl.doc.dc_id != 0UL &&
        tg_gui_dl.doc.dc_id != home) {
        /* 0.0.8 punto 1c: the document lives on ANOTHER datacenter. The
           first step opens the file channel there (cached key or one-off
           DH + import) instead of failing with "not supported yet". */
        tg_gui_dl.need_dc = tg_gui_dl.doc.dc_id;
        tg_gui_log("download: foreign DC, using its file channel");
    }
    tg_gui_dl_sanitize_name(tg_gui_dl.doc.file_name, safe, sizeof(safe));
    {
        const char *dir = tg_gui_session_download_dir();

        (void)mkdir(dir, 0777); /* EEXIST is the norm; volume roots fail
                                   harmlessly and are used as they are */
        tg_platform_ensure_drawer_icon(dir); /* visible on Workbench */
        tg_gui_dl_join_path(tg_gui_dl.path, sizeof(tg_gui_dl.path), dir, safe);
    }
    tg_gui_dl.f = fopen(tg_gui_dl.path, "wb");
    if (tg_gui_dl.f != 0) {
        /* One big write buffer instead of the runtime's default: a tester on
           an 030 could HEAR the drive working through a download, which is
           what many small writes sound like (we write the real file as it
           arrives -- there is no temporary to move elsewhere). Best effort:
           if the buffer cannot be set the transfer just runs as before. */
        static char dl_wbuf[TG_GUI_DL_WBUF];

        (void)setvbuf(tg_gui_dl.f, dl_wbuf, _IOFBF, sizeof(dl_wbuf));
    }
    if (tg_gui_dl.f == 0) {
        tg_mtproto_close_quiet_stream(tg_gui_dl.quiet, stream);
        tg_gui_dl.quiet = 0;
        return 3;
    }
    tg_gui_dl.rc = 4; /* file open: any further failure is a transfer error */
    tg_gui_dl.active = 1;
    return 0;
}

/* Move ONE chunk. Returns 1 while the transfer is running, 0 once finished
   (rc set); the caller must then call tg_mtproto_download_end(). */
/* Fire a getFile WITHOUT waiting (0.0.8 punto 1d): same initConnection +
   invokeWithLayer wrapper the synchronous path uses, then the raw send.
   Returns 0 with the msg_id to match the reply against. Only used for the
   PREFETCH of the next chunk: the current chunk always goes through the
   proven synchronous call, so any failure here just means "no prefetch". */
static int tg_mtproto_pipe_send_getfile(const tg_mtproto_file_ctx *fc,
                                        const unsigned char *query,
                                        unsigned long query_length,
                                        tg_mtproto_message_id *out_id,
                                        FILE *stream)
{
    static unsigned char wrapped[768]; /* a getFile is small; not a part */
    unsigned long api_id;
    tg_mtproto_tl_writer writer;

    if (fc == 0 || fc->context == 0 || !fc->context->connection_open ||
        tg_mtproto_parse_ulong_arg(fc->api_id, &api_id) != 0) {
        return 1;
    }
    if (tg_mtproto_build_initialized_query(&writer, wrapped, sizeof(wrapped),
                                           api_id, query, query_length) != 0) {
        return 1;
    }
    return tg_mtproto_send_query_noreply(fc->context, wrapped, writer.length,
                                         out_id, stream,
                                         "mtproto getFile(prefetch)");
}

/* Drop any in-flight prefetch: the connection is closed (which is what
   actually drains the wire) and the parking slot is emptied. */
static void tg_mtproto_download_pipe_reset(void)
{
    if (tg_gui_dl.pre_armed) {
        tg_mtproto_close_auth_context(tg_gui_dl.fc_file.context);
        tg_gui_dl.pre_armed = 0;
    }
}

static int tg_mtproto_download_step(void)
{
    unsigned char query[384]; /* holds a getFile with a long file_reference */
    tg_mtproto_tl_writer writer;
    tg_mtproto_rpc_result result;
    const unsigned char *bytes;
    unsigned long bytes_len;
    int cdn = 0; /* stays 0 when unpack fails before the parse fills it */

    if (!tg_gui_dl.active || tg_gui_dl.rc == 5) {
        return 0; /* idle, or cancelled: don't overwrite rc with a late finish */
    }
    if (tg_gui_dl.need_dc != 0UL) {
        /* Open (or switch) the foreign file channel before the next chunk.
           Blocking, but the slow path (DH) happens once per DC ever. */
        if (tg_gui_session_setup_foreign_channel(&tg_gui_dl.fc,
                                                 tg_gui_dl.need_dc,
                                                 &tg_gui_dl.fc_file,
                                                 tg_gui_dl.quiet) != 0) {
            sprintf(tg_gui_dl.fail, "file DC %lu: channel setup failed",
                    tg_gui_dl.need_dc);
            return 0; /* rc 4: the status line says why */
        }
        tg_gui_dl.need_dc = 0UL;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_upload_get_document(&writer, &tg_gui_dl.doc,
                                             tg_gui_dl.offset,
                                             TG_GUI_DL_CHUNK) !=
        TG_MTPROTO_TL_OK) {
        return 0; /* rc stays 4 */
    }
    memset(&result, 0, sizeof(result));
    {
        int gfrc;
        char gl[128];

        if (tg_gui_dl.pre_armed &&
            tg_gui_dl.pre_offset == tg_gui_dl.offset) {
            /* 1d: this chunk was asked for one step ago -- its round trip
               already happened while the previous chunk was landing. Just
               collect the reply. Anything unusual drops the pipeline and
               falls through to the synchronous request below. */
            gfrc = tg_mtproto_recv_rpc_result(
                tg_gui_dl.fc_file.context, &tg_gui_dl.pre_id, &result,
                tg_gui_dl.quiet, "mtproto getFile(pipelined)", 600U);
            tg_gui_dl.pre_armed = 0;
            if (gfrc != 0) {
                sprintf(gl, "download: pipe off=%lu rc=%d, falling back",
                        tg_gui_dl.offset, gfrc);
                tg_gui_log(gl);
                tg_mtproto_download_pipe_reset(); /* closes: drains the wire */
                memset(&result, 0, sizeof(result));
                gfrc = tg_mtproto_send_saved_query_on_context(
                    tg_gui_dl.fc_file.host, tg_gui_dl.fc_file.port,
                    tg_gui_dl.fc_file.api_id, tg_gui_dl.fc_file.auth_file,
                    tg_gui_dl.fc_file.dc_id_text,
                    tg_gui_dl.fc_file.context, query, writer.length, &result,
                    tg_gui_dl.quiet, "mtproto getFile(document)", 600U);
            }
        } else {
            tg_mtproto_download_pipe_reset(); /* stale prefetch, if any */
            gfrc = tg_mtproto_send_saved_query_on_context(
                tg_gui_dl.fc_file.host, tg_gui_dl.fc_file.port,
                tg_gui_dl.fc_file.api_id, tg_gui_dl.fc_file.auth_file,
                tg_gui_dl.fc_file.dc_id_text,
                tg_gui_dl.fc_file.context, query, writer.length, &result,
                tg_gui_dl.quiet, "mtproto getFile(document)", 600U);
        }
        sprintf(gl, "download: getFile off=%lu qlen=%lu rc=%d ctor=0x%08lx",
                tg_gui_dl.offset, writer.length, gfrc,
                result.result_constructor);
        tg_gui_log(gl);
        if (gfrc != 0) {
            /* One flaky chunk must not throw away a transfer that is already
               megabytes in. Retry the SAME offset a few times -- nothing has
               been written for it yet -- and only give up once a chunk fails
               repeatedly. */
            if (++tg_gui_dl.chunk_retry <= TG_GUI_DL_CHUNK_RETRIES) {
                char rl[96];

                sprintf(rl, "download: retry %d @off %lu (%.40s)",
                        tg_gui_dl.chunk_retry, tg_gui_dl.offset,
                        tg_mtproto_query_fail);
                tg_gui_log(rl);
                tg_mtproto_download_pipe_reset(); /* nothing may be in flight */
                if (strncmp(tg_mtproto_query_fail, "send", 4) == 0 ||
                    strncmp(tg_mtproto_query_fail, "transport", 9) == 0) {
                    /* Dead/wedged socket: reconnect before re-asking. */
                    tg_mtproto_close_auth_context(tg_gui_dl.fc_file.context);
                }
                return 1; /* same offset, fresh getFile next step */
            }
            sprintf(tg_gui_dl.fail, "@off %lu after %d tries: %.48s",
                    tg_gui_dl.offset, tg_gui_dl.chunk_retry - 1,
                    tg_mtproto_query_fail[0] != '\0'
                        ? tg_mtproto_query_fail : "no reply");
            return 0; /* rc 4 */
        }
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        long ecode = 0L;
        char emsg[96];

        tg_mtproto_download_pipe_reset(); /* the prefetch is void now */
        emsg[0] = '\0';
        (void)tg_mtproto_parse_rpc_error(result.result_body - 4U,
                                         result.result_body_length + 4U,
                                         &ecode, emsg, sizeof(emsg));
        {
            char d[160];
            sprintf(d, "download: getFile rpc-error %ld %s", ecode, emsg);
            tg_gui_log(d);
        }
        if (emsg[0] != '\0') {
            sprintf(tg_gui_dl.fail, "%.90s", emsg);
        }
        /* FILE_MIGRATE_X mid-transfer: the bytes live on DC X. Hop the
           file channel there and re-ask the SAME offset (nothing written
           for it yet). Bounded: a migrate ping-pong means server trouble. */
        if (strncmp(emsg, "FILE_MIGRATE_", 13) == 0) {
            unsigned long mdc = 0UL;
            const char *p = emsg + 13;

            while (*p >= '0' && *p <= '9') {
                mdc = mdc * 10UL + (unsigned long)(*p - '0');
                ++p;
            }
            if (mdc != 0UL && ++tg_gui_dl.migrations <= 2 &&
                tg_mtproto_dc_by_id((int)mdc) != 0) {
                tg_gui_dl.need_dc = mdc;
                return 1; /* next step opens the channel on that DC */
            }
            tg_gui_dl.rc = 2;
            return 0;
        }
        /* FILE_REFERENCE_EXPIRED mid-transfer: re-fetch once, restart. */
        if (!tg_gui_dl.refetched &&
            tg_mtproto_file_find_document(
                &tg_gui_dl.fc,
                tg_gui_dl.resolved_msg_id != 0UL ? tg_gui_dl.resolved_msg_id
                                                 : tg_gui_dl.msg_id,
                tg_gui_dl.quiet, &tg_gui_dl.doc, 0) == 0) {
            tg_gui_dl.refetched = 1;
            tg_gui_dl.offset = 0UL;
            if (fseek(tg_gui_dl.f, 0L, SEEK_SET) == 0) {
                return 1;
            }
        }
        return 0; /* rc 4 */
    }
    if (tg_mtproto_unpack_gzip_result(&result, tg_gui_dl.quiet,
                                      "mtproto getFile(document)") != 0 ||
        tg_mtproto_parse_upload_file(result.result_constructor,
                                     result.result_body,
                                     result.result_body_length, &bytes,
                                     &bytes_len, &cdn) !=
            TG_MTPROTO_TL_OK ||
        cdn) {
        tg_gui_log("download: cdn/parse fail (unsupported)");
        strcpy(tg_gui_dl.fail, cdn ? "CDN file" : "bad reply");
        return 0; /* CDN redirect (large public file) not handled yet */
    }
    if (bytes_len > 0UL &&
        fwrite(bytes, 1, bytes_len, tg_gui_dl.f) != bytes_len) {
        tg_gui_dl.rc = 3;
        return 0;
    }
    tg_gui_dl.offset += bytes_len;
    tg_gui_dl.chunk_retry = 0; /* this chunk landed */
    if (bytes_len < TG_GUI_DL_CHUNK) {
        tg_mtproto_download_pipe_reset(); /* short chunk = last: drop any prefetch */
        tg_gui_dl.rc = 0;
        return 0;
    }
    if (tg_gui_dl.offset >= (0x00100000UL * 512UL)) {
        tg_mtproto_download_pipe_reset();
        return 0; /* 512 MB hard stop: no runaway on a bad size (rc 4) */
    }
    /* 1d: ask for the NEXT chunk now, so its round trip overlaps this
       step's write and the caller's paint. Only when the file is known to
       have more bytes; a failure just means the next step goes synchronous. */
    if (!tg_gui_dl.pre_armed &&
        tg_gui_dl.offset + TG_GUI_DL_CHUNK <= tg_gui_dl.doc.size_lo) {
        tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
        if (tg_mtproto_build_upload_get_document(&writer, &tg_gui_dl.doc,
                                                 tg_gui_dl.offset,
                                                 TG_GUI_DL_CHUNK) ==
                TG_MTPROTO_TL_OK &&
            tg_mtproto_pipe_send_getfile(&tg_gui_dl.fc_file, query,
                                         writer.length, &tg_gui_dl.pre_id,
                                         tg_gui_dl.quiet) == 0) {
            tg_gui_dl.pre_armed = 1;
            tg_gui_dl.pre_offset = tg_gui_dl.offset;
        }
    }
    return 1;
}

/* Cancel a running transfer (close gadget / ESC): end() removes the partial. */
static void tg_mtproto_download_cancel(void)
{
    if (tg_gui_dl.active) {
        tg_mtproto_download_pipe_reset();
        tg_gui_dl.rc = 5;
        tg_gui_log("download: cancelled by user");
    }
}

/* Close the engine: file (partial removed on failure), quiet stream, state.
   Returns the final rc; out_path gets the saved path (ok) or the reason. */
/* Two families of loadable file exist on these systems, and the name is no
   help for either: people pass programs around with a .exe suffix that means
   nothing here (issue #15), so the magic bytes decide.

   A classic AmigaDOS file starts with HUNK_HEADER, 0x000003F3: plain 68k
   executables, libraries, devices, handlers and datatypes all do. Object
   files and link libraries start with 0x000003E7 instead and must NOT be
   marked runnable.

   AmigaOS 4, MorphOS and AROS programs are ELF, 0x7F "ELF", which the first
   cut of this check did not know: a downloaded OS4 build of this very client
   came out without its bit, found in the field on 2026-09-03. The four byte
   magic is all we look at on purpose. Reading e_type to exclude object files
   would be the HUNK rule's twin, but AROS executables are relocatable ELF,
   ET_REL, so that rule would strip the bit from every AROS program. A foreign
   ELF marked runnable costs nothing: LoadSeg refuses it cleanly. */
static int tg_mtproto_file_is_amiga_executable(const char *path)
{
    FILE *f;
    unsigned char magic[4];
    unsigned long got;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    f = fopen(path, "rb");
    if (f == 0) {
        return 0;
    }
    got = (unsigned long)fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (got != 4UL) {
        return 0;
    }
    if (magic[0] == 0x00U && magic[1] == 0x00U &&
        magic[2] == 0x03U && magic[3] == 0xf3U) {
        return 1; /* HUNK_HEADER */
    }
    return magic[0] == 0x7fU && magic[1] == 'E' && magic[2] == 'L' &&
           magic[3] == 'F';
}

#if !defined(TG_NO_SELFTEST)
/* Host-runnable: the sniff must say yes to both loadable families, and no
   to a HUNK object file, a text file and a short file. */
static int tg_mtproto_executable_sniff_self_test(void)
{
    static const unsigned char hunk[8] = {0x00,0x00,0x03,0xf3,0,0,0,0};
    static const unsigned char obj[8] = {0x00,0x00,0x03,0xe7,0,0,0,0};
    static const unsigned char elf[8] = {0x7f,'E','L','F',1,1,1,0};
    static const unsigned char txt[8] = {'R','E','A','D','M','E','\n',0};
    static const unsigned char tiny[2] = {0x00,0x00};
    const char *path = "tg-exe-sniff-selftest.bin";
    struct { const unsigned char *b; unsigned long n; int want; } c[5];
    int i;

    c[0].b = hunk; c[0].n = 8UL; c[0].want = 1;
    c[1].b = elf;  c[1].n = 8UL; c[1].want = 1;
    c[2].b = obj;  c[2].n = 8UL; c[2].want = 0;
    c[3].b = txt;  c[3].n = 8UL; c[3].want = 0;
    c[4].b = tiny; c[4].n = 2UL; c[4].want = 0;
    for (i = 0; i < 5; ++i) {
        FILE *f = fopen(path, "wb");
        int got;

        if (f == 0) {
            return 2;
        }
        fwrite(c[i].b, 1, (size_t)c[i].n, f);
        fclose(f);
        got = tg_mtproto_file_is_amiga_executable(path);
        (void)remove(path);
        if (got != c[i].want) {
            printf("executable sniff self-test: case %d gave %d\n", i, got);
            return 2;
        }
    }
    return 0;
}
#endif

/* A saved attachment that IS a program gets its "e" bit cleared, so it runs
   straight away instead of needing a manual protect (issue #15). Shared by
   the download engine and any other path that lands a file on disk. */
static void tg_mtproto_mark_downloaded_file(const char *path)
{
    if (tg_mtproto_file_is_amiga_executable(path)) {
        tg_platform_set_executable(path);
    }
}

static int tg_mtproto_download_end(char *out_path,
                                   unsigned long out_path_size)
{
    int rc = tg_gui_dl.rc;

    tg_mtproto_download_pipe_reset(); /* never leave a request in flight */
    if (out_path != 0 && out_path_size > 0UL) {
        out_path[0] = '\0';
    }
    if (tg_gui_dl.f != 0 && fclose(tg_gui_dl.f) != 0 && rc == 0) {
        rc = 3;
    }
    tg_gui_dl.f = 0;
    if (rc != 0) {
        (void)remove(tg_gui_dl.path); /* a half file is worse than none */
    } else {
        tg_mtproto_mark_downloaded_file(tg_gui_dl.path);
    }
    if (out_path != 0 && out_path_size > 0UL) {
        const char *src = (rc == 0) ? tg_gui_dl.path : tg_gui_dl.fail;
        unsigned long n = 0UL;

        while (src[n] != '\0' && n + 1UL < out_path_size) {
            out_path[n] = src[n]; ++n;
        }
        out_path[n] = '\0';
    }
    if (tg_gui_dl.quiet != 0) {
        tg_mtproto_close_quiet_stream(tg_gui_dl.quiet, tg_gui_dl.stream);
        tg_gui_dl.quiet = 0;
    }
    tg_gui_dl.active = 0;
    return rc;
}

/* Blocking wrapper over the state machine: the TUI (and any legacy caller)
   still gets the old single-call download, byte-identical outcomes. */
static int tg_mtproto_file_download(const tg_mtproto_file_ctx *fc,
                                    unsigned long msg_id, char *out_path,
                                    unsigned long out_path_size, FILE *stream,
                                    tg_gui_download_progress_fn progress,
                                    void *progress_data)
{
    int brc;

    brc = tg_mtproto_download_begin(fc, msg_id, stream);
    if (brc != 0) {
        if (out_path != 0 && out_path_size > 0UL) {
            unsigned long n = 0UL;

            /* When begin() bounced off a RUNNING transfer, fail[] is that
               transfer's -- report nothing rather than someone else's reason. */
            if (!tg_gui_dl.active) {
                while (tg_gui_dl.fail[n] != '\0' && n + 1UL < out_path_size) {
                    out_path[n] = tg_gui_dl.fail[n]; ++n;
                }
            }
            out_path[n] = '\0';
        }
        return brc;
    }
    while (tg_mtproto_download_step()) {
        if (progress != 0 &&
            progress(tg_gui_dl.offset, tg_gui_dl.doc.size_lo,
                     progress_data) != 0) {
            tg_mtproto_download_cancel();
            break;
        }
    }
    return tg_mtproto_download_end(out_path, out_path_size);
}

/* Fills the bundle from the GUI session singleton. */
/* 0.0.8 punto 1a: file transfers get their OWN MTProto context (second
   socket, same DC and auth key). MTProto explicitly supports multiple
   sessions per key, and load_auth_context already assigns a fresh random
   session_id on every open, so the two sessions never collide on seq_no.
   With this, an upload/download no longer interleaves queries with the live
   chat session -- the foundation for the non-blocking transfers (1b) and the
   file-channel multi-DC (1c). Lazily opened on the first transfer; on open
   failure we fall back to the live context (the pre-1a behaviour), so a
   transfer still works when the second connection cannot come up. */
static tg_mtproto_auth_context tg_gui_file_context;

static void tg_gui_session_file_ctx(tg_mtproto_file_ctx *fc)
{
    fc->host = tg_gui_session_state.host;
    fc->port = tg_gui_session_state.port;
    fc->api_id = tg_gui_session_state.api_id;
    fc->auth_file = tg_gui_session_state.auth_file;
    fc->dc_id_text = tg_gui_session_state.dc_id_text;
    fc->peer_cache_file = tg_gui_session_state.peer_cache_file;
    fc->peer_index = tg_gui_session_state.current_peer_index;
    if (!tg_gui_file_context.connection_open) {
        FILE *quiet = tg_mtproto_open_quiet_stream(stdout);

        if (tg_mtproto_load_auth_context(fc->host, fc->port, fc->auth_file,
                                         &tg_gui_file_context, quiet,
                                         "mtproto file-channel") != 0) {
            tg_mtproto_close_quiet_stream(quiet, stdout);
            tg_gui_log("file-ctx: open failed, falling back to live session");
            fc->context = &tg_gui_session_state.context;
            return;
        }
        tg_mtproto_close_quiet_stream(quiet, stdout);
        tg_gui_log("file-ctx: own socket open");
    }
    fc->context = &tg_gui_file_context;
}

static unsigned long tg_gui_session_home_dc(void)
{
    unsigned long dc;
    const char *p;

    dc = 0UL;
    p = tg_gui_session_state.dc_id_text;
    while (p != 0 && *p >= '0' && *p <= '9') {
        dc = dc * 10UL + (unsigned long)(*p - '0');
        ++p;
    }
    return dc;
}

static int tg_gui_photo_finish(int success, const char *reason)
{
    unsigned long id_hi;
    unsigned long id_lo;
    int dirty;

    id_hi = tg_gui_photo_fetch.photo.id_hi;
    id_lo = tg_gui_photo_fetch.photo.id_lo;
    dirty = 0;
    if (tg_gui_photo_fetch.out != 0) {
        if (fclose(tg_gui_photo_fetch.out) != 0) {
            success = 0;
        }
        tg_gui_photo_fetch.out = 0;
    }
    if (success) {
        (void)remove(tg_gui_photo_fetch.path);
        if (rename(tg_gui_photo_fetch.part_path,
                   tg_gui_photo_fetch.path) != 0) {
            success = 0;
            reason = "cache rename";
        } else {
            /* Read the cache entry back before calling it done. A filesystem
               that accepts write, close and rename and then loses the file
               (a tired card, a full or damaged volume) used to send the
               client into an endless re-fetch: every repaint asked again,
               every turn went to the network and nothing ever decoded, with
               no line in the log saying why. Now the miss is named once and
               the fetch is a clean failure. */
            FILE *probe = fopen(tg_gui_photo_fetch.path, "rb");

            if (probe == 0) {
                success = 0;
                reason = "cache vanished after write";
            } else {
                fclose(probe);
                tg_gui_window_photo_cache_file_changed(
                    tg_gui_photo_fetch.path);
            }
        }
    }
    if (!success) {
        (void)remove(tg_gui_photo_fetch.part_path);
        tg_gui_window_photo_cache_file_removed(
            tg_gui_photo_fetch.part_path);
        if (tg_gui_photo_fetch.path[0] != '\0') {
            tg_gui_window_photo_cache_file_changed(tg_gui_photo_fetch.path);
        }
        if (reason != 0) {
            char line[96];

            sprintf(line, "photo: fetch fail %s", reason);
            tg_gui_photo_log(line);
        } else {
            tg_gui_photo_log("photo: fetch fail");
        }
    } else {
        tg_gui_photo_cache_variant_remember(
            id_hi, id_lo, tg_gui_photo_fetch.large,
            tg_gui_photo_fetch.photo.thumb_type);
        if (tg_gui_photo_fetch.large) {
            dirty = 1;
            tg_gui_photo_log("photo: viewer fetch done");
        } else {
            dirty = tg_gui_driver_mark_photo_ready(
                &tg_gui_session_state.gui_driver, id_hi, id_lo);
            tg_gui_photo_log("photo: fetch done");
        }
    }
    if (tg_gui_photo_fetch.quiet != 0) {
        tg_mtproto_close_quiet_stream(tg_gui_photo_fetch.quiet,
                                      tg_gui_photo_fetch.stream);
    }
    memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
    return dirty;
}

static int tg_gui_photo_queue_pop(tg_gui_photo_queue_entry *entry)
{
    int at;
    int i;

    if (entry == 0 || tg_gui_photo_queue_count <= 0) {
        return 0;
    }
    at = tg_gui_photo_queue_count - 1;
    /* A clicked viewer is foreground work. Pick the newest large request
       before inline thumbnails even when later paints refreshed the latter. */
    for (i = tg_gui_photo_queue_count - 1; i >= 0; --i) {
        if (tg_gui_photo_queue[i].large) {
            at = i;
            break;
        }
    }
    *entry = tg_gui_photo_queue[at];
    for (i = at + 1; i < tg_gui_photo_queue_count; ++i) {
        tg_gui_photo_queue[i - 1] = tg_gui_photo_queue[i];
    }
    --tg_gui_photo_queue_count;
    return 1;
}

static int tg_gui_photo_begin(FILE *stream)
{
    tg_gui_photo_queue_entry entry;
    unsigned long home_dc;
    int found;

    found = 0;
    while (tg_gui_photo_queue_pop(&entry)) {
        /* Viewer first, otherwise newest visible. Stale/cached entries are
           discarded here without poisoning retry of a future visible paint. */
        if ((entry.large || tg_gui_photo_inline_enabled) &&
            !tg_gui_photo_cache_exists(entry.photo.id_hi, entry.photo.id_lo,
                                       entry.large) &&
            !tg_gui_photo_was_tried(entry.photo.id_hi, entry.photo.id_lo,
                                    entry.large,
                                    entry.photo.thumb_type)) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return 0;
    }
    memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
    tg_gui_photo_fetch.photo = entry.photo;
    tg_gui_photo_fetch.large = entry.large;
    if (tg_gui_session_photo_cache_path(
            tg_gui_photo_fetch.path, sizeof(tg_gui_photo_fetch.path),
            entry.photo.id_hi, entry.photo.id_lo, entry.large) != 0) {
        memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
        return 0;
    }
    sprintf(tg_gui_photo_fetch.part_path, "%s.part", tg_gui_photo_fetch.path);
    (void)mkdir("photos", 0777);
    (void)remove(tg_gui_photo_fetch.part_path);
    tg_gui_photo_fetch.out = fopen(tg_gui_photo_fetch.part_path, "wb");
    if (tg_gui_photo_fetch.out == 0) {
        tg_gui_photo_log("photo: fetch fail cache open");
        memset(&tg_gui_photo_fetch, 0, sizeof(tg_gui_photo_fetch));
        return 0;
    }
    tg_gui_photo_fetch.stream = stream;
    tg_gui_photo_fetch.quiet = tg_mtproto_open_quiet_stream(stream);
    tg_gui_session_file_ctx(&tg_gui_photo_fetch.home);
    tg_gui_photo_fetch.file = tg_gui_photo_fetch.home;
    home_dc = tg_gui_session_home_dc();
    if (home_dc != 0UL && entry.photo.dc_id != 0UL &&
        entry.photo.dc_id != home_dc) {
        tg_gui_photo_fetch.need_dc = entry.photo.dc_id;
    }
    tg_gui_photo_fetch.active = 1;
    if (entry.progressive_skipped && tg_gui_log_is_enabled()) {
        char line[80];

        sprintf(line, "photo: progressive size skipped, fallback %s",
                entry.photo.thumb_type);
        tg_gui_log(line);
    }
    tg_gui_photo_log_progress(entry.large ? "viewer fetch begin" : "fetch begin",
                              0UL, entry.photo.size);
    return 1;
}

int tg_gui_session_photo_step(FILE *stream)
{
    unsigned char query[384];
    tg_mtproto_tl_writer writer;
    tg_mtproto_rpc_result result;
    const unsigned char *bytes;
    unsigned long bytes_len;
    unsigned long previous_timeout;
    char previous_fail[64];
    int cdn;
    int rc;
    int dirty;

    if (tg_gui_photo_cache_paused || !tg_gui_session_state.open || stream == 0 ||
        tg_gui_session_transfer_busy()) {
        return 0;
    }
    if (!tg_gui_photo_fetch.active && !tg_gui_photo_begin(stream)) {
        return 0;
    }
    if (tg_gui_photo_fetch.need_dc != 0UL ||
        (tg_gui_photo_fetch.photo.dc_id != 0UL &&
         tg_gui_photo_fetch.photo.dc_id != tg_gui_session_home_dc() &&
         tg_gui_foreign_dc != tg_gui_photo_fetch.photo.dc_id)) {
        if (tg_gui_session_setup_foreign_channel(
                &tg_gui_photo_fetch.home, tg_gui_photo_fetch.photo.dc_id,
                &tg_gui_photo_fetch.file, tg_gui_photo_fetch.quiet) != 0) {
            return tg_gui_photo_finish(0, "datacenter channel");
        }
        tg_gui_photo_fetch.need_dc = 0UL;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_upload_get_photo(
            &writer, &tg_gui_photo_fetch.photo, tg_gui_photo_fetch.offset,
            TG_GUI_DL_CHUNK) != TG_MTPROTO_TL_OK) {
        return tg_gui_photo_finish(0, "request build");
    }
    strcpy(previous_fail, tg_mtproto_query_fail);
    previous_timeout = tg_net_connect_timeout_seconds();
    /* Background thumbnails yield quickly on a slow link; the foreground chat
       and explicit downloads retain their normal, longer timeout. */
    tg_net_set_connect_timeout_seconds(3UL);
    memset(&result, 0, sizeof(result));
    rc = tg_mtproto_send_saved_query_on_context(
        tg_gui_photo_fetch.file.host, tg_gui_photo_fetch.file.port,
        tg_gui_photo_fetch.file.api_id, tg_gui_photo_fetch.file.auth_file,
        tg_gui_photo_fetch.file.dc_id_text, tg_gui_photo_fetch.file.context,
        query, writer.length, &result, tg_gui_photo_fetch.quiet,
        "mtproto getFile(photo)", 600U);
    tg_net_set_connect_timeout_seconds(previous_timeout);
    if (rc != 0) {
        if (++tg_gui_photo_fetch.retries <= 2) {
            if (strncmp(tg_mtproto_query_fail, "send", 4) == 0 ||
                strncmp(tg_mtproto_query_fail, "transport", 9) == 0) {
                tg_mtproto_close_auth_context(
                    tg_gui_photo_fetch.file.context);
            }
            strcpy(tg_mtproto_query_fail, previous_fail);
            return 0;
        }
        strcpy(tg_mtproto_query_fail, previous_fail);
        return tg_gui_photo_finish(0, "query timeout/transport");
    }
    tg_gui_photo_fetch.retries = 0;
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        long ecode;
        char emsg[64];

        ecode = 0L;
        emsg[0] = '\0';
        (void)tg_mtproto_parse_rpc_error(result.result_body - 4U,
                                         result.result_body_length + 4U,
                                         &ecode, emsg, sizeof(emsg));
        if (strncmp(emsg, "FILE_MIGRATE_", 13) == 0) {
            unsigned long dc;
            const char *p;

            dc = 0UL;
            p = emsg + 13;
            while (*p >= '0' && *p <= '9') {
                dc = dc * 10UL + (unsigned long)(*p - '0');
                ++p;
            }
            if (dc != 0UL && tg_mtproto_dc_by_id((int)dc) != 0) {
                tg_gui_photo_fetch.need_dc = dc;
                tg_gui_photo_fetch.photo.dc_id = dc;
                strcpy(tg_mtproto_query_fail, previous_fail);
                return 0;
            }
        }
        if (tg_gui_log_is_enabled()) {
            char line[96];

            sprintf(line, "photo: fetch rpc %.63s", emsg[0] != '\0'
                                                   ? emsg : "unknown");
            tg_gui_log(line);
        }
        strcpy(tg_mtproto_query_fail, previous_fail);
        return tg_gui_photo_finish(0, "rpc error");
    }
    cdn = 0;
    if (tg_mtproto_unpack_gzip_result(&result, tg_gui_photo_fetch.quiet,
                                      "mtproto getFile(photo)") != 0) {
        strcpy(tg_mtproto_query_fail, previous_fail);
        return tg_gui_photo_finish(0, "gzip response");
    }
    if (tg_mtproto_parse_upload_file(result.result_constructor,
                                     result.result_body,
                                     result.result_body_length, &bytes,
                                     &bytes_len, &cdn) != TG_MTPROTO_TL_OK ||
        cdn || bytes_len == 0UL ||
        tg_gui_photo_fetch.offset + bytes_len >
            tg_gui_photo_fetch.photo.size) {
        strcpy(tg_mtproto_query_fail, previous_fail);
        return tg_gui_photo_finish(0, cdn ? "cdn response" : "file response");
    }
    if (fwrite(bytes, 1, bytes_len, tg_gui_photo_fetch.out) != bytes_len) {
        strcpy(tg_mtproto_query_fail, previous_fail);
        return tg_gui_photo_finish(0, "cache write");
    }
    tg_gui_photo_fetch.offset += bytes_len;
    tg_gui_photo_log_progress("fetch step", tg_gui_photo_fetch.offset,
                              tg_gui_photo_fetch.photo.size);
    dirty = 0;
    if (bytes_len < TG_GUI_DL_CHUNK ||
        tg_gui_photo_fetch.offset >= tg_gui_photo_fetch.photo.size) {
        dirty = tg_gui_photo_finish(1, 0);
    }
    strcpy(tg_mtproto_query_fail, previous_fail);
    return dirty;
}

int tg_gui_session_download_document(unsigned long msg_id, char *out_path,
                                     unsigned long out_path_size, FILE *stream,
                                     tg_gui_download_progress_fn progress,
                                     void *progress_data)
{
    tg_mtproto_file_ctx fc;

    if (!tg_gui_session_state.open || msg_id == 0UL) {
        if (out_path != 0 && out_path_size > 0UL) {
            out_path[0] = '\0';
        }
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    return tg_mtproto_file_download(&fc, msg_id, out_path, out_path_size,
                                    stream, progress, progress_data);
}

int tg_gui_session_send_document(const char *path, FILE *stream,
                                 tg_gui_upload_progress_fn progress,
                                 void *progress_data)
{
    tg_mtproto_file_ctx fc;

    if (!tg_gui_session_state.open) {
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    return tg_mtproto_file_send(&fc, path, stream, progress, progress_data, 0,
                                0);
}

int tg_gui_session_send_photo(const char *path, FILE *stream,
                              tg_gui_upload_progress_fn progress,
                              void *progress_data)
{
    tg_mtproto_file_ctx fc;

    if (!tg_gui_session_state.open) {
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    return tg_mtproto_file_send(&fc, path, stream, progress, progress_data, 1,
                                0);
}

/* --- 0.0.8 punto 1b: non-blocking transfer API for the GUI event loop. ----
   The window arms a transfer and keeps processing events; each pump call
   moves ONE chunk/part on the file channel. Direction is remembered here so
   the window only ever talks to this one four-call API. */
static int tg_gui_transfer_dir; /* 0 idle, 1 download, 2 upload */

int tg_gui_session_transfer_busy(void)
{
    return tg_gui_transfer_dir;
}

int tg_gui_session_transfer_start_download(unsigned long msg_id, FILE *stream)
{
    tg_mtproto_file_ctx fc;
    int rc;

    if (!tg_gui_session_state.open || msg_id == 0UL ||
        tg_gui_transfer_dir != 0) {
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    rc = tg_mtproto_download_begin(&fc, msg_id, stream);
    if (rc != 0) {
        /* Surface the begin() reason (e.g. "document not in cache") where
           the status bar already looks: tg_gui_session_last_transfer_error. */
        if (!tg_gui_dl.active && tg_gui_dl.fail[0] != '\0') {
            sprintf(tg_mtproto_query_fail, "%.60s", tg_gui_dl.fail);
        }
        return rc;
    }
    tg_gui_transfer_dir = 1;
    return 0;
}

int tg_gui_session_transfer_start_upload(const char *path,
                                         const char *caption, FILE *stream)
{
    tg_mtproto_file_ctx fc;
    int rc;

    if (!tg_gui_session_state.open || tg_gui_transfer_dir != 0) {
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    rc = tg_mtproto_upload_begin(&fc, caption, path, stream, 0);
    if (rc != 0) {
        return rc;
    }
    tg_gui_transfer_dir = 2;
    return 0;
}

int tg_gui_session_transfer_start_photo(const char *path,
                                        const char *caption, FILE *stream)
{
    tg_mtproto_file_ctx fc;
    int rc;

    if (!tg_gui_session_state.open || tg_gui_transfer_dir != 0) {
        return 1;
    }
    tg_gui_session_file_ctx(&fc);
    rc = tg_mtproto_upload_begin(&fc, caption, path, stream, 1);
    if (rc != 0) {
        return rc;
    }
    tg_gui_transfer_dir = 2;
    return 0;
}

int tg_gui_session_transfer_requested_photo(void)
{
    return tg_gui_ul.requested_photo;
}

int tg_gui_session_transfer_photo_fallback(void)
{
    return tg_gui_ul.photo_fallback;
}

unsigned long tg_gui_session_transfer_bytes(void)
{
    if (tg_gui_transfer_dir == 1) {
        return tg_gui_dl.offset;
    }
    if (tg_gui_transfer_dir == 2) {
        /* Parts confirmed so far; the last one may be short, which only
           makes the rate a hair conservative at the very end. */
        return tg_gui_ul.part * TG_GUI_DL_CHUNK;
    }
    return 0UL;
}

int tg_gui_session_transfer_step(unsigned long *done, unsigned long *total)
{
    int running = 0;

    if (tg_gui_transfer_dir == 1) {
        running = tg_mtproto_download_step();
        if (done != 0) {
            *done = tg_gui_dl.offset;
        }
        if (total != 0) {
            *total = tg_gui_dl.doc.size_lo;
        }
    } else if (tg_gui_transfer_dir == 2) {
        running = tg_mtproto_upload_step();
        if (done != 0) {
            *done = tg_gui_ul.part;
        }
        if (total != 0) {
            *total = tg_gui_ul.parts;
        }
    }
    return running;
}

void tg_gui_session_transfer_cancel(void)
{
    if (tg_gui_transfer_dir == 1) {
        tg_mtproto_download_cancel();
    } else if (tg_gui_transfer_dir == 2) {
        tg_mtproto_upload_cancel();
    }
}

int tg_gui_session_transfer_end(char *out_path, unsigned long out_path_size)
{
    int rc = 1;

    if (out_path != 0 && out_path_size > 0UL) {
        out_path[0] = '\0';
    }
    if (tg_gui_transfer_dir == 1) {
        rc = tg_mtproto_download_end(out_path, out_path_size);
    } else if (tg_gui_transfer_dir == 2) {
        rc = tg_mtproto_upload_end();
    }
    tg_gui_transfer_dir = 0;
    return rc;
}


static void tg_gui_session_fetch_open_avatar(FILE *stream)
{
    /* Also on MorphOS: open_chat already runs the same class of bounded
       on-context RPCs there (getHistory, getPeerDialogs) without issue -- the
       historic freezes were getDialogs/tick races, not one-shot queries. */
    unsigned long id_hi = tg_gui_session_state.open_peer_id_hi;
    unsigned long id_lo = tg_gui_session_state.open_peer_id_lo;
    unsigned long photo_hi;
    unsigned long photo_lo;
    unsigned long dc;
    unsigned long pc;
    unsigned long ph;
    unsigned long pl;
    unsigned long ahh;
    unsigned long ahl;
    int hah;
    int i;
    unsigned char query[96];
    tg_mtproto_tl_writer writer;
    tg_mtproto_rpc_result result;
    const unsigned char *bytes;
    unsigned long bytes_len;
    int cdn;
    int av_foreign = 0; /* photo lives on another DC: use the 1c channel */
    FILE *quiet;
    static const char label[] = "mtproto upload.getFile(avatar)";

    if (!tg_gui_session_state.open || stream == 0 ||
        (id_hi == 0UL && id_lo == 0UL)) {
        return;
    }
    for (i = 0; i < tg_gui_avfetch_n; ++i) {
        if (tg_gui_avfetch_hi[i] == id_hi && tg_gui_avfetch_lo[i] == id_lo) {
            return; /* already tried this session */
        }
    }
    /* A cached photo on disk is good enough: re-downloading on every chat
       open was a needless RPC on a slow link and made opening feel sluggish.
       Refresh path: delete the avatars/ drawer (or the one file). */
    {
        char cached[48];
        FILE *probe;

        sprintf(cached, "avatars/tgav%08lx%08lx.jpg", id_hi, id_lo);
        probe = fopen(cached, "rb");
        if (probe != 0) {
            fclose(probe);
            if (tg_gui_avfetch_n < TG_GUI_AVFETCH_MAX) {
                tg_gui_avfetch_hi[tg_gui_avfetch_n] = id_hi;
                tg_gui_avfetch_lo[tg_gui_avfetch_n] = id_lo;
                ++tg_gui_avfetch_n;
            }
            return;
        }
    }
    if (!tg_mtproto_avatar_meta_lookup(id_hi, id_lo, &photo_hi, &photo_lo,
                                       &dc)) {
        return; /* no capture yet (peer straight from the file cache) */
    }
    if (photo_hi == 0UL && photo_lo == 0UL) {
        return; /* no profile photo */
    }
    /* 0.0.8 1c-bis: a foreign-DC avatar now downloads through the same
       foreign file channel the document downloads use (the old guard just
       kept the blurred thumb forever). Only yield when a running transfer
       holds that channel pointed at a DIFFERENT datacenter. */
    {
        unsigned long home = 0UL;
        const char *p = tg_gui_session_state.dc_id_text;

        while (*p >= '0' && *p <= '9') {
            home = home * 10UL + (unsigned long)(*p - '0');
            ++p;
        }
        av_foreign = (home != 0UL && dc != 0UL && dc != home);
        if (av_foreign && tg_gui_session_transfer_busy() &&
            tg_gui_foreign_dc != dc) {
            return; /* do not retarget the channel under a live transfer;
                       unmarked, so a later chat open retries */
        }
    }
    /* Mark BEFORE the attempt: any failure must not retry this session. */
    if (tg_gui_avfetch_n < TG_GUI_AVFETCH_MAX) {
        tg_gui_avfetch_hi[tg_gui_avfetch_n] = id_hi;
        tg_gui_avfetch_lo[tg_gui_avfetch_n] = id_lo;
        ++tg_gui_avfetch_n;
    }
    if (tg_mtproto_load_peer_cache_peer(
            tg_gui_session_state.peer_cache_file,
            tg_gui_session_state.current_peer_index, &pc, &ph, &pl, &ahh,
            &ahl, &hah, stream, label) != 0) {
        return;
    }
    if (ph != id_hi || pl != id_lo) {
        return; /* stale open state: never fetch the wrong peer */
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_upload_get_peer_photo(&writer, pc, ph, pl, ahh, ahl,
                                               hah, photo_hi, photo_lo, 0UL,
                                               65536UL) != TG_MTPROTO_TL_OK) {
        return;
    }
    quiet = tg_mtproto_open_quiet_stream(stream);
    memset(&result, 0, sizeof(result));
    if (av_foreign) {
        /* Foreign avatar: open (or reuse) the 1c channel on ITS datacenter
           and fetch the bytes there. First contact costs the one-off DH. */
        tg_mtproto_file_ctx home_fc;
        tg_mtproto_file_ctx av_fc;

        tg_gui_session_file_ctx(&home_fc);
        if (tg_gui_session_setup_foreign_channel(&home_fc, dc, &av_fc,
                                                 quiet) != 0 ||
            tg_mtproto_send_saved_query_on_context(
                av_fc.host, av_fc.port, av_fc.api_id, av_fc.auth_file,
                av_fc.dc_id_text, av_fc.context, query, writer.length,
                &result, quiet, label, 600U) != 0) {
            tg_mtproto_close_quiet_stream(quiet, stream);
            return; /* channel or query failed: the thumb stays */
        }
    } else if (tg_mtproto_send_saved_query_on_context(
            tg_gui_session_state.host, tg_gui_session_state.port,
            tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
            tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
            query, writer.length, &result, quiet, label, 600U) != 0) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_unpack_gzip_result(&result, quiet, label) != 0) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return; /* FILE_MIGRATE / FLOOD / reference expired: thumb stays */
    }
    if (tg_mtproto_parse_upload_file(result.result_constructor,
                                     result.result_body,
                                     result.result_body_length, &bytes,
                                     &bytes_len, &cdn) != TG_MTPROTO_TL_OK ||
        cdn || bytes_len == 0UL || bytes_len >= 65536UL) {
        tg_mtproto_close_quiet_stream(quiet, stream);
        return; /* cdn redirect / empty / suspiciously full chunk: skip */
    }
    {
        char name[48];
        FILE *f;

        sprintf(name, "avatars/tgav%08lx%08lx.jpg", id_hi, id_lo);
        (void)mkdir("avatars", 0777); /* best-effort; EEXIST is the norm */
        f = fopen(name, "wb");
        if (f != 0) {
            if (fwrite(bytes, 1, bytes_len, f) == bytes_len) {
                fclose(f);
                tg_gui_window_avatar_invalidate(id_hi, id_lo);
            } else {
                fclose(f);
                remove(name); /* half-written cache is worse than none */
            }
        }
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
}

int tg_gui_session_is_open(void)
{
    return tg_gui_session_state.open;
}

/* Resolve the open group's typing member id->name. Tries the lazily-fetched
   member cache first; if empty and not yet attempted for this group, fetches the
   recent members ONCE (peerChannel + off-MorphOS) and re-scans. Returns a pointer
   into the member cache (valid until the next open/fetch) or 0 for "someone". */
static const char *tg_gui_session_resolve_typing_member(FILE *stream)
{
    unsigned long fh;
    unsigned long fl;
    unsigned long ci;

    fh = tg_gui_session_state.typing.from_id_hi;
    fl = tg_gui_session_state.typing.from_id_lo;
    for (ci = 0UL; ci < tg_gui_session_state.member_cache.count; ++ci) {
        if (tg_gui_session_state.member_cache.entries[ci].id_hi == fh &&
            tg_gui_session_state.member_cache.entries[ci].id_lo == fl &&
            tg_gui_session_state.member_cache.entries[ci].title[0] != '\0') {
            return tg_gui_session_state.member_cache.entries[ci].title;
        }
    }
    if (!tg_gui_session_state.member_fetch_done) {
        FILE *fq;

        /* Mark BEFORE the call so a miss/failure never re-fetches this open. */
        tg_gui_session_state.member_fetch_done = 1;
        tg_gui_session_state.member_for_id_hi =
            tg_gui_session_state.open_peer_id_hi;
        tg_gui_session_state.member_for_id_lo =
            tg_gui_session_state.open_peer_id_lo;
        fq = tg_mtproto_open_quiet_stream(stream);
        if (tg_mtproto_gui_fetch_group_members(
                tg_gui_session_state.host, tg_gui_session_state.port,
                tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
                tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
                tg_gui_session_state.peer_cache_file,
                tg_gui_session_state.current_peer_index,
                &tg_gui_session_state.member_cache, fq) == 0) {
            tg_mtproto_close_quiet_stream(fq, stream);
            for (ci = 0UL; ci < tg_gui_session_state.member_cache.count; ++ci) {
                if (tg_gui_session_state.member_cache.entries[ci].id_hi == fh &&
                    tg_gui_session_state.member_cache.entries[ci].id_lo == fl &&
                    tg_gui_session_state.member_cache.entries[ci].title[0] !=
                        '\0') {
                    return tg_gui_session_state.member_cache.entries[ci].title;
                }
            }
        } else {
            tg_mtproto_close_quiet_stream(fq, stream);
        }
    }
    return 0;
}

static int tg_gui_session_apply_edit_updates(void)
{
    unsigned long i;
    int dirty;

    dirty = 0;
    for (i = 0UL; i < tg_gui_session_state.edit_sink.count; ++i) {
        tg_chat_edit_entry *entry;

        int peer_match;

        entry = &tg_gui_session_state.edit_sink.queue[i];
        peer_match = tg_chat_edit_peer_matches_open(
            entry, tg_gui_session_state.open_peer_constructor,
            tg_gui_session_state.open_peer_id_hi,
            tg_gui_session_state.open_peer_id_lo);
        if (tg_gui_session_state.current_peer_index[0] != '\0' &&
            peer_match &&
            tg_gui_driver_update_text_utf8(
                &tg_gui_session_state.gui_driver, entry->message_id,
                entry->text)) {
            dirty = 1;
        }
    }
    tg_gui_session_state.edit_sink.count = 0UL;
    return dirty;
}

static int tg_gui_session_apply_read_update(void)
{
    int dirty;

    dirty = 0;
    if (tg_gui_session_state.read_outbox_sink.pending) {
        tg_gui_session_state.read_outbox_sink.pending = 0;
        if (tg_gui_session_state.current_peer_index[0] != '\0' &&
            tg_gui_session_state.read_outbox_sink.peer_id_hi ==
                tg_gui_session_state.open_peer_id_hi &&
            tg_gui_session_state.read_outbox_sink.peer_id_lo ==
                tg_gui_session_state.open_peer_id_lo &&
            tg_gui_driver_set_read_outbox_max(
                &tg_gui_session_state.gui_driver,
                tg_gui_session_state.read_outbox_sink.max_id)) {
            dirty = 1;
        }
    }
    return dirty;
}

static int tg_gui_session_dispatch_notifications(void)
{
    unsigned long i;

    if (tg_gui_session_state.engine.notify.count == 0UL) {
        return 0;
    }
    for (i = 0UL; i < tg_gui_session_state.engine.notify.count; ++i) {
        if (tg_gui_session_state.driver.on_notification != 0) {
            tg_gui_session_state.driver.on_notification(
                tg_gui_session_state.driver.ctx,
                &tg_gui_session_state.engine.notify.queue[i]);
        }
    }
    tg_gui_session_state.engine.notify.count = 0UL;
    tg_gui_session_state.engine.notify.dropped = 0UL;
    tg_gui_session_persist_unread();
    return 1;
}

static int tg_gui_session_apply_typing(FILE *stream, int allow_member_fetch)
{
    tg_gui_state *gs;
    const char *want;
    char built[TG_GUI_NAME_MAX];
    int fresh;
    unsigned long now;

    gs = tg_gui_session_state.gui_driver.state;
    if (gs == 0) {
        return 0;
    }
    now = (unsigned long)time(0);
    fresh = 0;
    if (tg_gui_session_state.typing.active &&
        now >= tg_gui_session_state.typing.seen_epoch &&
        (now - tg_gui_session_state.typing.seen_epoch) <=
            TG_MTPROTO_TYPING_TTL_SECONDS) {
        fresh = 1;
    } else {
        tg_gui_session_state.typing.active = 0;
    }
    if (fresh) {
        char td[112];

        sprintf(td, "typing: fresh sink=%08lx%08lx open=%08lx%08lx idx=%s",
                tg_gui_session_state.typing.peer_id_hi,
                tg_gui_session_state.typing.peer_id_lo,
                tg_gui_session_state.open_peer_id_hi,
                tg_gui_session_state.open_peer_id_lo,
                tg_gui_session_state.current_peer_index);
        tg_gui_log(td);
    }
    want = "";
    if (fresh && tg_gui_session_state.current_peer_index[0] != '\0' &&
        tg_gui_session_state.typing.peer_id_hi ==
            tg_gui_session_state.open_peer_id_hi &&
        tg_gui_session_state.typing.peer_id_lo ==
            tg_gui_session_state.open_peer_id_lo) {
        const char *name;

        name = 0;
        if (!tg_gui_session_state.open_peer_is_chat) {
            if (tg_gui_session_state.current_peer_label[0] != '\0') {
                name = tg_gui_session_state.current_peer_label;
            }
        } else {
            int mi;

            for (mi = 0; mi < gs->message_count; ++mi) {
                if (!gs->messages[mi].is_own &&
                    gs->messages[mi].from_id_hi ==
                        tg_gui_session_state.typing.from_id_hi &&
                    gs->messages[mi].from_id_lo ==
                        tg_gui_session_state.typing.from_id_lo &&
                    gs->messages[mi].sender[0] != '\0') {
                    name = gs->messages[mi].sender;
                    break;
                }
            }
            /* The receive-only composing path must stay network-free. It may
               consult an already-fetched cache, but never trigger the member
               query itself. */
            if (name == 0 &&
                (allow_member_fetch ||
                 tg_gui_session_state.member_fetch_done)) {
                name = tg_gui_session_resolve_typing_member(stream);
            }
        }
        if (name != 0) {
            static const char suffix[] = " is typing...";
            unsigned long di;
            unsigned long si;

            di = 0UL;
            while (di + sizeof(suffix) < sizeof(built) &&
                   name[di] != '\0') {
                built[di] = name[di];
                ++di;
            }
            for (si = 0UL; si + 1UL < sizeof(suffix) &&
                           di + 1UL < sizeof(built); ++si) {
                built[di++] = suffix[si];
            }
            built[di] = '\0';
            want = built;
        } else {
            want = "someone is typing...";
        }
    }
    if (strcmp(gs->typing, want) != 0) {
        unsigned long n;

        n = (unsigned long)strlen(want);
        if (n >= sizeof(gs->typing)) {
            n = sizeof(gs->typing) - 1UL;
        }
        memcpy(gs->typing, want, n);
        gs->typing[n] = '\0';
        return 1;
    }
    return 0;
}

static int tg_gui_session_apply_pushes(FILE *stream, int allow_member_fetch)
{
    int dirty;

    dirty = 0;
    if (tg_gui_session_apply_edit_updates()) {
        dirty = 1;
    }
    if (tg_gui_session_apply_read_update()) {
        dirty = 1;
    }
    if (tg_gui_session_dispatch_notifications()) {
        dirty = 1;
    }
    if (tg_gui_session_apply_typing(stream, allow_member_fetch)) {
        dirty = 1;
    }
    return dirty;
}

int tg_gui_session_receive_pending(FILE *stream)
{
    static unsigned char response[TG_MTPROTO_REPLY_RECV_MAX];
    /* static like every other decrypt site in this file: the struct embeds a
       ~72 KB body on PPC and this runs once a second inside the GUI event
       loop -- a stack frame that size is a Guru on sub-1MB stacks. */
    static tg_mtproto_encrypted_message decrypted;
    unsigned long response_length;
    unsigned long prev_timeout;
    tg_net_status status;
    char error_buffer[128];
    FILE *quiet;
    int ready;
    int dirty;

    if (!tg_gui_session_state.open || stream == 0) {
        return 0;
    }
    dirty = 0;
    /* Even a quiet socket may require the typing TTL to clear. */
    if (!tg_gui_session_state.context.connection.is_open) {
        return tg_gui_session_apply_pushes(stream, 0);
    }
    ready = tg_net_poll_readable(&tg_gui_session_state.context.connection,
                                 error_buffer, sizeof(error_buffer));
    if (ready <= 0) {
        if (ready < 0) {
            tg_gui_log("receive_pending: poll failed; reconnect on full tick");
            tg_mtproto_close_auth_context(&tg_gui_session_state.context);
        }
        return tg_gui_session_apply_pushes(stream, 0);
    }

    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    /* Readability only guarantees that the first byte is ready. Bound the rest
       of the frame; a partial frame is not recoverable, so close and let the
       next regular tick reconnect instead of leaving a desynchronised stream.
       KNOWN TRADE-OFF: a frame trickling in can hold the GUI up to ~1s per
       recv chunk while composing -- bounded and rare (pushes are small), and
       the idle-tick poll already blocks comparably; async reassembly is not
       worth its complexity here. */
    tg_net_set_connect_timeout_seconds(1UL);
    error_buffer[0] = '\0';
    response_length = 0UL;
    status = tg_mtproto_recv_abridged_packet(
        &tg_gui_session_state.context.connection, response, sizeof(response),
        &response_length, error_buffer, sizeof(error_buffer));
    tg_net_set_connect_timeout_seconds(prev_timeout);
    if (status != TG_NET_OK ||
        tg_mtproto_decrypt_encrypted_message(
            response, response_length, tg_gui_session_state.context.auth_key,
            &decrypted) != TG_MTPROTO_TL_OK) {
        tg_gui_log("receive_pending: frame failed; reconnect on full tick");
        tg_mtproto_close_auth_context(&tg_gui_session_state.context);
        tg_mtproto_close_quiet_stream(quiet, stream);
        return tg_gui_session_apply_pushes(stream, 0);
    }
    tg_mtproto_sync_time_from_server(&tg_gui_session_state.context, &decrypted);
    tg_mtproto_ack_encrypted_message(&tg_gui_session_state.context, &decrypted,
                                     quiet, "gui receive");
    tg_chat_notify_collect(decrypted.body, decrypted.body_length);
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_gui_log("receive_pending: one frame");
    if (tg_gui_session_apply_pushes(stream, 0)) {
        dirty = 1;
    }
    return dirty;
}

int tg_gui_session_tick(FILE *stream)
{
    FILE *quiet;
    int dirty;

    if (!tg_gui_session_state.open || stream == 0) {
        return 0;
    }
    dirty = 0;
    tg_gui_log("tick: begin");
    quiet = tg_mtproto_open_quiet_stream(stream);
    /* Run the poll on a short leash: a stalled recv must cost a brief hiccup,
       not freeze the window. Restored right after. */
    {
        unsigned long prev_timeout;

        prev_timeout = tg_net_connect_timeout_seconds();
        /* On MorphOS this value is ALSO the per-recv and per-connect select()
           timeout. 3s is too short to RECONNECT the dropped idle MTProto link,
           so the inbound poll never re-established and nothing was received (send
           worked because it uses 10s). Give MorphOS the same headroom the console
           steady-state poll (20s) and GUI send (10s) already use. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
        tg_net_set_connect_timeout_seconds(12UL);
#else
        tg_net_set_connect_timeout_seconds(3UL);
#endif
    /* New messages in the open chat stream straight into the transcript. */
    if (tg_gui_session_state.current_peer_index[0] != '\0') {
        unsigned long printed;

        printed = 0UL;
        tg_chat_message_driver_override = &tg_gui_session_state.driver;
        (void)tg_mtproto_auth_print_history_text_peer_on_context(
            tg_gui_session_state.host, tg_gui_session_state.port,
            tg_gui_session_state.api_id, tg_gui_session_state.auth_file,
            tg_gui_session_state.dc_id_text, &tg_gui_session_state.context,
            tg_gui_session_state.peer_cache_file,
            tg_gui_session_state.current_peer_index, "5", quiet,
            &tg_gui_session_state.last_seen_message_id, &printed, 1, 1, 0,
            tg_gui_session_state.current_peer_label,
            tg_gui_session_state.own_label);
        tg_chat_message_driver_override = 0;
        if (printed > 0UL) {
            dirty = 1;
        }
        /* Refresh the peer's read cursor so own messages flip to the double-
           check once the peer reads them. Throttled: read state changes slowly
           and getPeerDialogs is a needless round-trip on every short tick. Now
           runs on MorphOS too: the "window opens then freezes on the first tick"
           regression (a1aad83) this was disabled for was the unserialized repaint
           racing the layer build, cured by the LockLayerRom bracket -- the reply
           is one peer. Cadence kept (the round-trip still costs on the slow link). */
        {
            unsigned long read_cadence;

            /* Adaptive: while an own message is still "sent" (we are waiting for
               the peer to read it), refresh the read cursor EVERY tick so the
               single->double check flips near real-time; once everything is read,
               relax to every 5th tick (read state then changes rarely and the
               one-peer getPeerDialogs is a needless round-trip on the slow link). */
            read_cadence = tg_gui_driver_has_unseen_own(
                               &tg_gui_session_state.gui_driver)
                               ? 1UL
                               : 5UL;
            ++tg_gui_session_state.read_outbox_tick;
            if ((tg_gui_session_state.read_outbox_tick % read_cadence) == 0UL) {
                unsigned long read_max;

                if (tg_mtproto_chat_fetch_read_outbox_on_context(
                        tg_gui_session_state.host, tg_gui_session_state.port,
                        tg_gui_session_state.api_id,
                        tg_gui_session_state.auth_file,
                        tg_gui_session_state.dc_id_text,
                        &tg_gui_session_state.context,
                        tg_gui_session_state.peer_cache_file,
                        tg_gui_session_state.current_peer_index, &read_max,
                        quiet) == 0) {
                    if (tg_gui_driver_set_read_outbox_max(
                            &tg_gui_session_state.gui_driver, read_max)) {
                        dirty = 1;
                    }
                }
            }
        }
    }
    /* The cadence-gated getDifference drain harvests inbound messages into the
       notify queue. As on the console: every tick on MorphOS (where pushes are
       suppressed and the drain IS the path), a slower reconciliation sweep
       elsewhere. The cursor is primed lazily on the first eligible tick. */
    if (tg_gui_session_state.engine.diff_enabled) {
        unsigned long diff_cadence;

        /* MorphOS GUI: with pushes now ON (typing/read-receipts re-enabled) the
           live cross-chat stream arrives via pushes, so the getDifference drain
           reverts from every-tick to a periodic backstop (every 4th tick) that
           recovers anything a dropped push missed. This is what lets watch_seconds
           drop to 6 without each tick getting heavier. The open chat's own new
           messages do not depend on this drain (the history poll carries them). */
#if defined(__MORPHOS__) || defined(__MORPHOS)
        diff_cadence = 4UL;
#else
        diff_cadence = 30UL;
#endif
        ++tg_gui_session_state.diff_tick;
        if ((tg_gui_session_state.diff_tick % diff_cadence) == 0UL) {
            /* The drain can sit in a quiet recv; on MorphOS the per-recv select()
               timeout IS the connect timeout, so cap it short here (the history
               poll above keeps the 12s it needs to reconnect). This stops a window
               close from waiting ~12s on an idle drain recv. Restored after. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
            tg_net_set_connect_timeout_seconds(4UL);
#endif
            if (tg_gui_session_state.engine.updates_state.pts == 0UL) {
                (void)tg_mtproto_chat_get_updates_state_on_context(
                    tg_gui_session_state.host, tg_gui_session_state.port,
                    tg_gui_session_state.api_id,
                    tg_gui_session_state.auth_file,
                    tg_gui_session_state.dc_id_text,
                    &tg_gui_session_state.context,
                    &tg_gui_session_state.engine.updates_state, quiet);
            } else {
                (void)tg_mtproto_chat_drain_difference_on_context(
                    tg_gui_session_state.host, tg_gui_session_state.port,
                    tg_gui_session_state.api_id,
                    tg_gui_session_state.auth_file,
                    tg_gui_session_state.dc_id_text,
                    &tg_gui_session_state.context,
                    &tg_gui_session_state.engine.updates_state, quiet);
            }
#if defined(__MORPHOS__) || defined(__MORPHOS)
            tg_net_set_connect_timeout_seconds(12UL);
#endif
        }
    }
        tg_net_set_connect_timeout_seconds(prev_timeout);
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_gui_log("tick: end");
    if (tg_gui_session_apply_pushes(stream, 1)) {
        dirty = 1;
    }
    return dirty;
}

void tg_gui_session_close(void)
{
    tg_mtproto_avatar_store_save(); /* keep the previews for next run */
    tg_gui_photo_queue_reset();
    if (!tg_gui_session_state.open) {
        tg_chat_nq = 0;
        tg_chat_typing_target = 0;
        tg_chat_read_outbox_target = 0;
        tg_chat_edit_target = 0;
        return;
    }
    tg_gui_log("close: closing context");
    tg_mtproto_close_auth_context(&tg_gui_session_state.context);
    if (tg_gui_file_context.connection_open) {
        /* The dedicated file channel (0.0.8 1a): closed with the session so
           the MorphOS settle below covers both connections. */
        tg_mtproto_close_auth_context(&tg_gui_file_context);
        tg_gui_log("close: file channel closed");
    }
    if (tg_gui_foreign_context.connection_open) {
        /* The foreign-DC file channel (0.0.8 1c): same treatment. The
           per-DC auth key file stays for reuse; the import is redone on
           the next run (harmless). */
        tg_mtproto_close_auth_context(&tg_gui_foreign_context);
        tg_gui_log("close: foreign file channel closed");
    }
    tg_gui_foreign_dc = 0UL;
    tg_gui_foreign_imported = 0;
    tg_mtproto_quiet_tmp_sweep(); /* leave no quiet-stream slot behind in T: */
    tg_gui_log("close: context closed");
#if defined(__MORPHOS__) || defined(__MORPHOS)
    /* Let the bsdsocket stack drive the just-closed connection from FIN-WAIT to
       CLOSED before we exit. On this -noixemul stack the runtime tears bsdsocket
       down after main(), and doing that while the held connection is still in
       transit on the slow link HARD-FREEZES the machine. The old 3s build never
       hit this because the idle link had already died long before close. A short
       settle reproduces that "already dead" state deterministically. */
    tg_platform_sleep_seconds(3UL);
    tg_gui_log("close: settle done");
#endif
    tg_net_set_connect_timeout_seconds(tg_gui_session_state.saved_timeout);
    tg_chat_nq = 0;
    tg_chat_typing_target = 0;
    tg_chat_read_outbox_target = 0;
    tg_chat_edit_target = 0;
    tg_gui_session_state.open = 0;
}

/* --- first-login flow (no saved session) -------------------------------- *
 * The window drives phone -> code -> 2FA; each call is one blocking network
 * round-trip over the console wizard's headless backend. Lives here so it can
 * reach the static auth helpers (send_code / sign_in / check_password) and the
 * session singleton directly. The login DH handshake is slow on m68k, so each
 * step bumps the connect timeout while it runs. */

static void tg_gui_login_copy(char *dest, unsigned long size, const char *src)
{
    unsigned long i;

    if (dest == 0 || size == 0UL) {
        return;
    }
    for (i = 0UL; i + 1UL < size && src != 0 && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

void tg_gui_session_login_begin(const char *api_file, const char *auth_file,
                                const char *peer_cache_file)
{
    memset(&tg_gui_session_state.login, 0, sizeof(tg_gui_session_state.login));
    tg_gui_session_state.login.active = 1;
    tg_gui_session_state.login.api_file = api_file;
    tg_gui_session_state.login.auth_file = auth_file;
    tg_gui_session_state.login.cache_file = peer_cache_file;
    /* European numbers usually live on DC4; a wrong guess just costs one
       PHONE_MIGRATE round-trip, handled in send_code below. */
    tg_gui_session_state.login.host = "149.154.167.91";
    tg_gui_session_state.login.dc_id_text = "4";
    tg_gui_login_copy(tg_gui_session_state.login.code_hash_file,
                      sizeof(tg_gui_session_state.login.code_hash_file),
                      "data/phone-code-hash.txt");
}

int tg_gui_session_login_send_code(const char *phone, FILE *stream)
{
    char api_id[32];
    char api_hash[96];
    unsigned long prev_timeout;
    FILE *quiet;
    int rc;
    static const char label[] = "gui auth.sendCode";

    if (!tg_gui_session_state.login.active || phone == 0 || stream == 0) {
        return TG_GUI_LOGIN_ERROR;
    }
    tg_gui_session_state.login.last_error[0] = '\0';
    if (tg_mtproto_load_api_credentials(tg_gui_session_state.login.api_file,
                                        api_id, sizeof(api_id), api_hash,
                                        sizeof(api_hash), stream, label) != 0) {
        strncpy(tg_gui_session_state.login.last_error,
                "Cannot read telegram-api.txt",
                sizeof(tg_gui_session_state.login.last_error) - 1UL);
        return TG_GUI_LOGIN_ERROR;
    }
    tg_gui_login_copy(tg_gui_session_state.login.phone,
                      sizeof(tg_gui_session_state.login.phone), phone);
    tg_mtproto_sanitize_phone(tg_gui_session_state.login.phone);
    tg_gui_login_copy(tg_gui_session_state.login.api_id,
                      sizeof(tg_gui_session_state.login.api_id), api_id);
    if (tg_gui_session_state.login.phone[0] == '\0') {
        strncpy(tg_gui_session_state.login.last_error,
                "Empty phone number (enter digits with country code)",
                sizeof(tg_gui_session_state.login.last_error) - 1UL);
        tg_mtproto_secure_zero(api_hash, sizeof(api_hash));
        return TG_GUI_LOGIN_BAD_PHONE;
    }
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(45UL); /* DH handshake is slow on m68k */
    /* Run the DH handshake on a QUIET stream (tmpfile) + KPutStr pacing: on
       MorphOS raw console output during network I/O hard-freezes the machine, and
       open_auth_context fputc('.')/fprintf's the DH progress. The chat path is
       safe precisely because it never lets net output touch CON:; the login must
       do the same. */
    quiet = tg_mtproto_open_quiet_stream(stream);
    tg_gui_log("login: send_code start");
    rc = tg_mtproto_auth_send_code(tg_gui_session_state.login.host, "443",
                                   tg_gui_session_state.login.dc_id_text, api_id,
                                   api_hash, tg_gui_session_state.login.phone,
                                   tg_gui_session_state.login.auth_file,
                                   tg_gui_session_state.login.code_hash_file,
                                   quiet);
    if (rc > TG_MTPROTO_PHONE_MIGRATE_RC_BASE) {
        unsigned long migrate_dc;
        const char *migrate_host;
        const char *migrate_dc_text;

        migrate_dc = (unsigned long)(rc - TG_MTPROTO_PHONE_MIGRATE_RC_BASE);
        if (tg_mtproto_production_endpoint_for_dc(migrate_dc, &migrate_host,
                                                  &migrate_dc_text) == 0) {
            /* The migrated endpoint is what sign_in / checkPassword must use. */
            tg_gui_session_state.login.host = migrate_host;
            tg_gui_session_state.login.dc_id_text = migrate_dc_text;
            rc = tg_mtproto_auth_send_code(
                migrate_host, "443", migrate_dc_text, api_id, api_hash,
                tg_gui_session_state.login.phone,
                tg_gui_session_state.login.auth_file,
                tg_gui_session_state.login.code_hash_file, quiet);
        }
    }
    tg_gui_log("login: send_code done");
    if (rc != 0) {
        tg_mtproto_capture_quiet_error(
            quiet, stream, tg_gui_session_state.login.last_error,
            sizeof(tg_gui_session_state.login.last_error));
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    tg_mtproto_secure_zero(api_hash, sizeof(api_hash));
    return (rc == 0) ? TG_GUI_LOGIN_OK : TG_GUI_LOGIN_BAD_PHONE;
}

int tg_gui_session_login_sign_in(const char *code, FILE *stream)
{
    unsigned long prev_timeout;
    FILE *quiet;
    int rc;

    if (!tg_gui_session_state.login.active || code == 0 || stream == 0) {
        return TG_GUI_LOGIN_ERROR;
    }
    tg_gui_session_state.login.last_error[0] = '\0';
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(45UL);
    /* Quiet stream + pacing, as in send_code: no raw CON: output on MorphOS. */
    quiet = tg_mtproto_open_quiet_stream(stream);
    tg_gui_log("login: sign_in start");
    rc = tg_mtproto_auth_sign_in(tg_gui_session_state.login.host, "443",
                                 tg_gui_session_state.login.api_id,
                                 tg_gui_session_state.login.auth_file,
                                 tg_gui_session_state.login.phone,
                                 tg_gui_session_state.login.code_hash_file, code,
                                 tg_gui_session_state.login.dc_id_text, quiet);
    tg_gui_log("login: sign_in done");
    if (rc != 0 && rc != TG_MTPROTO_SIGN_IN_PASSWORD_NEEDED &&
        rc != TG_MTPROTO_SIGN_IN_CODE_INVALID) {
        tg_mtproto_capture_quiet_error(
            quiet, stream, tg_gui_session_state.login.last_error,
            sizeof(tg_gui_session_state.login.last_error));
    }
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    if (rc == 0) {
        return TG_GUI_LOGIN_OK;
    }
    if (rc == TG_MTPROTO_SIGN_IN_PASSWORD_NEEDED) {
        return TG_GUI_LOGIN_NEED_2FA;
    }
    if (rc == TG_MTPROTO_SIGN_IN_CODE_INVALID) {
        return TG_GUI_LOGIN_BAD_CODE;
    }
    return TG_GUI_LOGIN_ERROR;
}

const char *tg_gui_session_login_last_error(void)
{
    return tg_gui_session_state.login.last_error;
}

int tg_gui_session_login_check_password(const char *password, FILE *stream)
{
    unsigned long prev_timeout;
    FILE *quiet;
    int rc;

    if (!tg_gui_session_state.login.active || password == 0 || stream == 0) {
        return TG_GUI_LOGIN_ERROR;
    }
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(45UL);
    /* Quiet stream + pacing, as in send_code: no raw CON: output on MorphOS. */
    quiet = tg_mtproto_open_quiet_stream(stream);
    tg_gui_log("login: check_password start");
    rc = tg_mtproto_auth_check_password_text(
        tg_gui_session_state.login.host, "443",
        tg_gui_session_state.login.api_id, tg_gui_session_state.login.auth_file,
        tg_gui_session_state.login.dc_id_text, password, quiet);
    tg_gui_log("login: check_password done");
    tg_mtproto_close_quiet_stream(quiet, stream);
    tg_net_set_connect_timeout_seconds(prev_timeout);
    if (rc == 0) {
        return TG_GUI_LOGIN_OK;
    }
    if (rc == TG_MTPROTO_CHECK_PASSWORD_INVALID) {
        return TG_GUI_LOGIN_BAD_PASSWORD;
    }
    return TG_GUI_LOGIN_ERROR;
}

int tg_gui_session_login_activate(tg_gui_state *state, FILE *stream)
{
    const char *api_file;
    const char *auth_file;
    const char *cache_file;
    int rc;

    if (!tg_gui_session_state.login.active || state == 0 || stream == 0) {
        return 2;
    }
    /* tg_gui_session_open memsets the singleton (clearing login), so snapshot
       the paths first. */
    api_file = tg_gui_session_state.login.api_file;
    auth_file = tg_gui_session_state.login.auth_file;
    cache_file = tg_gui_session_state.login.cache_file;
    /* Pull the fresh peer list, then open the live session over the auth.bin the
       login just wrote (session_open reloads everything from the file). */
    (void)tg_mtproto_gui_refresh_peer_cache(api_file, auth_file, cache_file,
                                            stream);
    rc = tg_gui_session_open(api_file, auth_file, cache_file, state, stream);
    if (rc != 0) {
        return rc;
    }
    state->mode = TG_GUI_MODE_CHAT;
    state->composing = 0;
    state->input[0] = '\0';
    state->input_masked = 0;
    if (state->chat_count > 0) {
        tg_gui_login_copy(state->title, sizeof(state->title),
                          state->chats[state->selected_chat].name);
        (void)tg_gui_session_open_chat(
            state->chats[state->selected_chat].index, stream);
    } else {
        tg_gui_login_copy(state->title, sizeof(state->title), "Telegram Amiga");
    }
    tg_gui_login_copy(state->status, sizeof(state->status),
                      "Live - F1-F10 chats, Q quits");
    return 0;
}
