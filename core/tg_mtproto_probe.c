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
#include "tg_mtproto_bigint.h"
#include "tg_mtproto_encrypted.h"
#include "tg_mtproto_envelope.h"
#include "tg_mtproto_login.h"
#include "tg_chat_engine.h"
#include "tg_mtproto_message_id.h"
#include "tg_mtproto_probe.h"
#include "tg_mtproto_rsa.h"
#include "tg_mtproto_session.h"
#include "tg_mtproto_srp.h"
#include "tg_mtproto_transport.h"
#include "tg_console.h"
#include "tg_console_ui.h"
#include "tg_console_tui.h"
#include "tg_file.h"
#include "tg_gui.h"
#include "tg_gui_driver.h"
#include "tg_gui_session.h"
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

/*
 * Consecutive failed reads/sends in the interactive chat loop before it drops
 * and reopens the connection. Recovers a wedged long-running session (stale
 * salt/seqno, or a silently dropped TCP link) instead of polling a dead session
 * forever.
 */
#define TG_MTPROTO_CHAT_STALL_LIMIT 3UL
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
#if defined(__m68k__)
#define TG_MTPROTO_GZIP_UNPACKED_MAX 65536UL
#else
#define TG_MTPROTO_GZIP_UNPACKED_MAX 131072UL
#endif
/* Receive buffer for one decrypted reply frame handed to recv_abridged_packet.
   The old 32 KiB was sized for a 5-message page; the deep-backlog open now asks
   for 60 (non-m68k) / 30 (m68k), and a busy group's frame overruns 32 KiB, so
   recv_abridged_packet rejected it ("Could not read messages now") and the chat
   loaded few/no messages. Size it to the page we actually request. */
#if defined(__m68k__)
#define TG_MTPROTO_REPLY_RECV_MAX 49152U
#else
#define TG_MTPROTO_REPLY_RECV_MAX 131072U
#endif
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
static void tg_mtproto_replay_quiet_stream(FILE *quiet, FILE *fallback);

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
    static unsigned char payload[3072];
    static unsigned char packet[3200];
    /* getHistory of a busy group can return several messages plus the referenced
       users/chats; a too-small buffer makes recv_abridged_packet reject the
       frame (payload > capacity) and the read hard-fails with "Could not read
       messages now." Sized by TG_MTPROTO_REPLY_RECV_MAX for the deep-backlog page
       we now request (60 non-m68k / 30 m68k), not the old 5-message page. */
    static unsigned char response[TG_MTPROTO_REPLY_RECV_MAX];
    unsigned long encrypted_padding_length;
    unsigned long payload_length;
    unsigned long response_length;
    unsigned long query_start_time;
    unsigned int attempt;
    unsigned int receive_attempt;
    int retry_request;
    unsigned long response_constructor;
    tg_mtproto_bad_msg_notification bad_msg;
    static tg_mtproto_encrypted_message decrypted;
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
        }
        memset(&bad_msg, 0, sizeof(bad_msg));
        if (tg_platform_break_pending()) {
            fprintf(stream, "%s: user-break\n", label);
            return 2;
        }
        for (receive_attempt = 0U; receive_attempt < max_receive_attempts;
             ++receive_attempt) {
            if ((unsigned long)time(0) - query_start_time >=
                    query_budget_seconds) {
                break;  /* time budget hit: soft-fail, connection still alive */
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
                continue;
            }
            if (tg_mtproto_is_async_update_constructor(response_constructor)) {
                continue;
            }
            if (response_constructor ==
                    TG_MTPROTO_BAD_MSG_NOTIFICATION_CONSTRUCTOR ||
                response_constructor == TG_MTPROTO_BAD_SERVER_SALT_CONSTRUCTOR) {
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
        fprintf(stream, "%s: rpc-response-not-received\n", label);
        return TG_MTPROTO_QUERY_SOFT_FAIL;
    }

    fprintf(stream, "%s: bad-msg-retry-failed\n", label);
    return TG_MTPROTO_QUERY_SOFT_FAIL;
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
static void tg_mtproto_print_login_code_hint(FILE *stream,
                                             unsigned long type_constructor)
{
    const char *hint = 0;

    if (stream == 0) {
        return;
    }
    switch (type_constructor) {
    case 0x3dbb5986UL: /* auth.sentCodeTypeApp */
        hint = "Check your other Telegram apps (mobile/desktop/web) for the"
               " code.";
        break;
    case 0xc000bba2UL: /* auth.sentCodeTypeSms */
    case 0xa416ac81UL: /* auth.sentCodeTypeSmsWord */
    case 0xb37794afUL: /* auth.sentCodeTypeSmsPhrase */
    case 0xd9565c39UL: /* auth.sentCodeTypeFragmentSms */
    case 0x009fd736UL: /* auth.sentCodeTypeFirebaseSms */
        hint = "Telegram is sending the code by SMS to your phone.";
        break;
    case 0x5353e5a7UL: /* auth.sentCodeTypeCall */
        hint = "Telegram will call your phone and speak the code.";
        break;
    case 0xab03c6d9UL: /* auth.sentCodeTypeFlashCall */
    case 0x82006484UL: /* auth.sentCodeTypeMissedCall */
        hint = "Telegram is calling: the last digits of the caller ID are the"
               " code.";
        break;
    case 0xf450f59bUL: /* auth.sentCodeTypeEmailCode */
        hint = "Check your email for the code.";
        break;
    default:
        return; /* unknown delivery type - stay silent rather than mislead */
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
        return;
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

/* updateShort#78d4dec1 update:Update date:int -- the standard single-update
   envelope that carries transient typing. */
static void tg_chat_typing_collect_short(const unsigned char *body,
                                         unsigned long body_length)
{
    tg_mtproto_tl_reader reader;
    unsigned long outer;
    unsigned long inner;

    if (tg_chat_typing_target == 0) {
        return;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &outer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &inner) != TG_MTPROTO_TL_OK) {
        return;
    }
    tg_chat_typing_parse_update(&reader, inner);
}

static void tg_chat_notify_collect_updates(const unsigned char *body,
                                           unsigned long body_length)
{
    tg_mtproto_tl_reader reader;
    static tg_mtproto_message_text message;
    tg_mtproto_dialog_peer dest;
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
    for (i = 0UL; i < count; ++i) {
        if (tg_mtproto_tl_read_u32(&reader, &item_constructor) !=
            TG_MTPROTO_TL_OK) {
            return;
        }
        if (item_constructor == TG_MTPROTO_UPDATE_USER_TYPING_CONSTRUCTOR ||
            item_constructor ==
                TG_MTPROTO_UPDATE_CHAT_USER_TYPING_CONSTRUCTOR ||
            item_constructor ==
                TG_MTPROTO_UPDATE_CHANNEL_USER_TYPING_CONSTRUCTOR) {
            /* A typing item leading the container: record it, then stop (items
               are not length-prefixed, so the walk cannot continue past it). */
            tg_chat_typing_parse_update(&reader, item_constructor);
            return;
        }
        if (item_constructor ==
            TG_MTPROTO_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR) {
            /* The peer read our messages: record (peer, max_id) for the loop to
               apply when it is the open chat. Then stop (items are not
               length-prefixed, so the walk cannot continue past it). */
            tg_mtproto_dialog_peer rpeer;
            unsigned long rmax;

            if (tg_chat_read_outbox_target != 0 &&
                tg_mtproto_read_update_read_history_outbox(&reader, &rpeer,
                                                           &rmax) ==
                    TG_MTPROTO_TL_OK) {
                tg_chat_read_outbox_target->peer_id_hi = rpeer.id_hi;
                tg_chat_read_outbox_target->peer_id_lo = rpeer.id_lo;
                tg_chat_read_outbox_target->max_id = rmax;
                tg_chat_read_outbox_target->pending = 1;
            }
            return;
        }
        if (item_constructor != TG_MTPROTO_UPDATE_NEW_MESSAGE_CONSTRUCTOR &&
            item_constructor !=
                TG_MTPROTO_UPDATE_NEW_CHANNEL_MESSAGE_CONSTRUCTOR) {
            /* Unknown update items cannot be skipped (no length prefix). */
            return;
        }
        if (tg_mtproto_read_update_message_text(&reader, &message, &dest) !=
            TG_MTPROTO_TL_OK) {
#ifdef TG_MTPROTO_DIAG
            fprintf(stderr, "notify-diag rich parse-failed\n");
#endif
            return;
        }
#ifdef TG_MTPROTO_DIAG
        fprintf(stderr,
                "notify-diag rich msg id=%lu has_text=%d out=%d dest=0x%08lx\n",
                message.id, message.has_text, message.is_out,
                dest.peer_constructor);
#endif
        tg_chat_notify_push_message(&message, &dest);
        /* The rich message reader may leave unparsed tail bytes (reply
           markup and friends), so do not trust the reader position for
           further items. */
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
        /* The single-update envelope: the live "is typing" carrier. */
        tg_chat_typing_collect_short(body, body_length);
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
    static unsigned char initialized_query[1200];
    static unsigned char layered_query[1300];
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
    unsigned char wrapped_query[1400];
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
static int tg_mtproto_display_is_invisible(unsigned long cp)
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
    if (tg_mtproto_display_is_invisible(cp)) {
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
    if (tg_mtproto_display_is_invisible(cp)) {
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

    if (path == 0 || cache == 0) {
        return 2;
    }
    memset(cache, 0, sizeof(*cache));
    file = fopen(path, "r");
    if (file == 0) {
        return 2;
    }
    public_count = 0UL;
    while (fgets(line, sizeof(line), file) != 0) {
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
    }
    return 0;
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
}

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
            messages.message_count, messages.chat_count, messages.user_count);
    if (messages.is_slice || messages.is_not_modified ||
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
    unsigned char query[512];
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
    unsigned char query[512];
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
                if (line_length + 1UL < sizeof(line)) {
                    line[line_length] = buffer[i];
                    ++line_length;
                }
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
/* After repainting the TUI input row (which leaves the hardware cursor at the end
   of the visible text), pull the cursor back to the caret so LEFT/RIGHT show where
   the next edit lands. TUI-only; one short CSI move, no SGR -> safe on MorphOS. */
static void tg_chat_tui_place_caret(FILE *stream, unsigned long line_length,
                                    unsigned long caret)
{
    unsigned long back;

    if (caret > line_length) {
        caret = line_length;
    }
    back = line_length - caret;
    if (back > 0UL) {
        fprintf(stream, TG_UI_CSI "%luD", back);
        fflush(stream);
    }
}

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

            c = tg_chat_caret;
            if (c > *line_length) {
                c = *line_length;
            }
            if (c > 0UL) {
                /* delete the char before the caret, shifting the tail left */
                memmove(&line[c - 1UL], &line[c], *line_length - c);
                --(*line_length);
                tg_chat_caret = c - 1UL;
                tg_console_tui_input(tg_chat_tui_stream,
                                     tg_console_tui_prompt(), line,
                                     *line_length);
                tg_chat_tui_place_caret(tg_chat_tui_stream, *line_length,
                                        tg_chat_caret);
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
                /* cursor RIGHT (TUI only): step the caret toward the end and
                   nudge the hardware cursor one column right (no full redraw). */
                if (tg_console_tui_active() && tg_chat_tui_stream != 0 &&
                    tg_chat_caret < *line_length) {
                    ++tg_chat_caret;
                    fputs(TG_UI_CSI "C", tg_chat_tui_stream);
                    fflush(tg_chat_tui_stream);
                }
            } else if (direction == 'D') {
                /* cursor LEFT (TUI only): step the caret toward the start. */
                if (tg_console_tui_active() && tg_chat_tui_stream != 0 &&
                    tg_chat_caret > 0UL) {
                    --tg_chat_caret;
                    fputs(TG_UI_CSI "D", tg_chat_tui_stream);
                    fflush(tg_chat_tui_stream);
                }
            } else if (direction == '|' && fkey == 12UL) {
                /* Window NEWSIZE raw event: let the chat loop repaint the
                   full-screen layout with the new geometry. */
                tg_console_tui_note_resize();
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
            tg_console_tui_input(tg_chat_tui_stream,
                                 tg_console_tui_prompt(), line, *line_length);
            tg_chat_tui_place_caret(tg_chat_tui_stream, *line_length,
                                    tg_chat_caret);
        } else {
            line[*line_length] = ch;
            ++(*line_length);
            if (raw) {
                fputc(ch, stream);
                fflush(stream);
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
                tg_console_tui_input(tg_chat_tui_stream,
                                     tg_console_tui_prompt(), out,
                                     line_length);
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

    row.text = text;
    row.has_time = (date != 0UL);
    row.local_epoch = date; /* the test uses delta 0, so local_epoch == date */
    row.is_out = is_out;
    row.is_group = is_group;
    row.peer_label = peer_label;
    row.own_label = own_label;
    row.sender = sender;
    row.reply_quote = 0; /* golden script carries no replies */
    row.id = 0UL;
    row.from_id_hi = 0UL;
    row.from_id_lo = 0UL;
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
        qrow.text = "ok";
        qrow.has_time = 0;
        qrow.local_epoch = 0UL;
        qrow.is_out = 0;
        qrow.is_group = 0;
        qrow.peer_label = "Mario";
        qrow.own_label = "Io";
        qrow.sender = "Mario";
        qrow.reply_quote = "ciao";
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
    puts("chat render self-test: ok (transcript renderer golden)");
    return 0;
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
        tg_mtproto_close_quiet_stream(quiet, stream);
        return query_rc;
    }
    if (result.result_constructor == TG_MTPROTO_RPC_ERROR_CONSTRUCTOR) {
        (void)tg_mtproto_print_rpc_error(label, &result, quiet);
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
        tg_console_tui_input(tg_chat_tui_stream, tg_console_tui_prompt(),
                             pending, pending_length);
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
    char api_id[32];
    char line[512];
#if TG_MTPROTO_DISPLAY_LATIN1
    char send_line[1024];
#endif
    const char *peer_arg;
    const char *username_arg;
    const char *search_arg;
    const char *remove_arg;
    const char *color_arg;
    unsigned long line_length;
    unsigned long last_seen_message_id;
    unsigned long printed_message_count;
    unsigned long consecutive_failures;
    unsigned long sent_message_id;
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
    tg_mtproto_chat_print_system_line(stream, "Loading chats...");
    if (tg_mtproto_peer_cache_available(peer_cache_file)) {
        rc = 0;
    } else {
        quiet = tg_mtproto_open_quiet_stream(stream);
        rc = tg_mtproto_auth_list_peers_file(host, port, api_file, auth_file,
                                             dc_id_text, peer_limit,
                                             peer_cache_file, quiet);
        /* Heavy accounts return messages.dialogsSlice and list-peers fails;
           keep its technical log quiet and fall back to /add. */
        tg_mtproto_close_quiet_stream(quiet, stream);
    }
    if (rc != 0 && !tg_mtproto_peer_cache_available(peer_cache_file)) {
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
    tg_mtproto_chat_print_system_line(stream, "Opening session...");
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
            rc = tg_mtproto_chat_read_line_edit(line, sizeof(line),
                                                &line_length, 3600UL, chat_raw,
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
            if (line_length > 0UL) {
                /* A draft is being composed: skip background polls entirely.
                   Since updates ride the session, every poll grew heavy
                   (update parsing, gunzip) and on a 68030/25 the round
                   trips wedged themselves between keystrokes -- the field
                   report read "characters take too long to appear". Polls
                   resume the moment the line is sent or cleared. */
                continue;
            }
            poll_now = time(0);
            if (poll_now != (time_t)-1 && chat_last_poll != (time_t)0 &&
                poll_now >= chat_last_poll &&
                (unsigned long)(poll_now - chat_last_poll) < watch_seconds) {
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
    unsigned char payload[64];
    unsigned char packet[80];
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

        tg_chat_nq = 0;
        tg_chat_typing_target = 0;
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
    tg_chat_typing_sink typing;     /* live "is typing" sink, armed at open */
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
        fputs(msg, f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}

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
    if (!tg_mtproto_peer_cache_available(peer_cache_file)) {
#if !(defined(__MORPHOS__) || defined(__MORPHOS))
        static tg_mtproto_peer_cache probe_cache; /* 16K -- off the stack */
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
                                                &probe_cache) != 0 ||
                probe_cache.count <= prev ||
                probe_cache.count >= TG_MTPROTO_PEER_CACHE_MAX) {
                break; /* no new dialogs this page, or the cache is full */
            }
            prev = probe_cache.count;
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
        count = tg_mtproto_chat_list_parse(peer_cache_file, 0UL, rows,
                                           TG_CHAT_LIST_MAX, &missing);
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
    /* Opening getHistory limit -- must be <= TG_MTPROTO_MESSAGE_TEXT_LIST_MAX (the
       parser only keeps that many per read, the real backlog cap). MorphOS stays
       tiny (a large reply is its documented bsdsocket freeze trigger); m68k a bit
       smaller for the 8MB budget; PPC/AROS get the deep backlog. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
    history_limit = "12";
#elif defined(__m68k__)
    history_limit = "30";
#else
    history_limit = "60";
#endif
    sprintf(tg_gui_session_state.current_peer_index, "%lu", peer_index);
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
    count = tg_mtproto_chat_list_parse(tg_gui_session_state.peer_cache_file, 0UL,
                                       rows, TG_CHAT_LIST_MAX, &missing);
    /* ALWAYS fire, even with count 0: skipping the callback on an emptied
       list left the window's chat_count stale, and the remove flow then
       opened a neighbour from rows that no longer existed (stuck UI). */
    tg_gui_session_state.driver.on_chat_list_changed(
        tg_gui_session_state.driver.ctx, rows, count);
}

/* Public: rebuild the sidebar from the cached chat list (peer-cache file, no
   network) -- used to restore the real list after cancelling the search picker. */
void tg_gui_session_refresh_chats(void)
{
    tg_gui_session_reload_chats();
}

/* Public: remove the chat at `peer_index` (the 1-based sidebar number, == the
   peer-cache public index) from telegram-peers.txt, persist it, then reproject
   the sidebar. The user can re-add the chat later via search. Returns 0 on
   success, non-zero otherwise. */
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
    rc = tg_mtproto_peer_cache_remove_public_index(
        tg_gui_session_state.peer_cache_file, index_text, 0, 0UL, quiet);
    tg_mtproto_close_quiet_stream(quiet, stream);
    if (rc == 0) {
        tg_gui_session_reload_chats(); /* reproject the sidebar from the saved file */
    }
    return rc;
}

/* Public: move the chat at sidebar row src_index to dst_index (both 1-based,
   == row + 1), persist the new order, reproject the sidebar, and re-select the
   chat that was open (reload_chats zeroes selected_chat, so re-find it by peer id).
   No network fetch -- the transcript is untouched. Returns 0 on success. */
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

/* Add `selected` to the peer cache, refresh the sidebar, then highlight + open
   it (the search path bypasses apply_selection, so set the title here too).
   Returns 0 on success. */
static int tg_gui_search_open_entry(const tg_mtproto_peer_cache_entry *selected,
                                    FILE *stream)
{
    char new_index[32];
    char new_label[TG_GUI_NAME_MAX];
    FILE *quiet;
    unsigned long prev_timeout;
    int rc;

    if (selected == 0) {
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

    tg_gui_session_reload_chats();
    {
        unsigned long idx;

        if (tg_mtproto_parse_ulong_arg(new_index, &idx) == 0 && idx != 0UL) {
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
    }
    return 0;
}

/* Run contacts.search for `query` online and keep the openable results (limit 10,
   so the reply stays MorphOS-safe where getDialogs is not). Does NOT touch the
   peer cache or open anything -- the window then either opens the only match or
   shows a picker. Returns the openable-result count (>= 0), -1 on failure. */
int tg_gui_session_search_run(const char *query, FILE *stream)
{
    unsigned char qbuf[256];
    char trimmed[128];
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
    if (trimmed[0] == '\0') {
        return -1;
    }

    quiet = tg_mtproto_open_quiet_stream(stream);
    prev_timeout = tg_net_connect_timeout_seconds();
    tg_net_set_connect_timeout_seconds(12UL);
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
    return (e->title[0] != '\0') ? e->title : e->username;
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
    char send_line[1024];
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
       TUI send path. The composer is at most TG_GUI_TEXT_MAX bytes, so the
       worst-case 2x expansion still fits send_line[1024]; on overflow fall back
       to the raw text (best effort). */
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
    if (tg_gui_session_state.dc_id_text[0] != '\0') {
        unsigned long home = 0UL;
        const char *p = tg_gui_session_state.dc_id_text;

        while (*p >= '0' && *p <= '9') {
            home = home * 10UL + (unsigned long)(*p - '0');
            ++p;
        }
        if (home != 0UL && dc != 0UL && dc != home) {
            return; /* foreign DC: keep the thumb (v3 territory) */
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
    if (tg_mtproto_send_saved_query_on_context(
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

int tg_gui_session_tick(FILE *stream)
{
    FILE *quiet;
    int dirty;

    if (!tg_gui_session_state.open || stream == 0) {
        return 0;
    }
    dirty = 0;
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
        /* Real-time read receipt (5c): an updateReadHistoryOutbox push recorded
           by the drain applies instantly when it is the open chat -- the poll
           below stays as the backstop (dropped pushes / MorphOS). */
        if (tg_gui_session_state.read_outbox_sink.pending) {
            tg_gui_session_state.read_outbox_sink.pending = 0;
            if (tg_gui_session_state.read_outbox_sink.peer_id_hi ==
                    tg_gui_session_state.open_peer_id_hi &&
                tg_gui_session_state.read_outbox_sink.peer_id_lo ==
                    tg_gui_session_state.open_peer_id_lo &&
                tg_gui_driver_set_read_outbox_max(
                    &tg_gui_session_state.gui_driver,
                    tg_gui_session_state.read_outbox_sink.max_id)) {
                dirty = 1;
            }
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

    /* Dispatch every harvested notification to the GUI driver, then clear the
       queue (same consume semantics as the console's print_notify_lines). */
    if (tg_gui_session_state.engine.notify.count > 0UL) {
        unsigned long i;

        for (i = 0UL; i < tg_gui_session_state.engine.notify.count; ++i) {
            if (tg_gui_session_state.driver.on_notification != 0) {
                tg_gui_session_state.driver.on_notification(
                    tg_gui_session_state.driver.ctx,
                    &tg_gui_session_state.engine.notify.queue[i]);
            }
        }
        tg_gui_session_state.engine.notify.count = 0UL;
        tg_gui_session_state.engine.notify.dropped = 0UL;
        dirty = 1;
        /* Live unread increments just landed -> persist so the badges are correct
           after a restart (no-op write when nothing actually changed). */
        tg_gui_session_persist_unread();
    }

    /* Reflect the live "is typing" sink into the open chat's header: shown only
       for the open peer and only while fresh (the TTL); cleared otherwise. A
       change in either direction is a repaint. */
    {
        tg_gui_state *gs;

        gs = tg_gui_session_state.gui_driver.state;
        if (gs != 0) {
            const char *want;
            char built[TG_GUI_NAME_MAX];
            int fresh;
            unsigned long now;

            now = (unsigned long)time(0);
            fresh = 0;
            if (tg_gui_session_state.typing.active &&
                now >= tg_gui_session_state.typing.seen_epoch &&
                (now - tg_gui_session_state.typing.seen_epoch) <=
                    TG_MTPROTO_TYPING_TTL_SECONDS) {
                fresh = 1;
            } else {
                /* Stale or absent: disarm so a later identical push re-fires. */
                tg_gui_session_state.typing.active = 0;
            }
            want = "";
            if (fresh && tg_gui_session_state.current_peer_index[0] != '\0' &&
                tg_gui_session_state.typing.peer_id_hi ==
                    tg_gui_session_state.open_peer_id_hi &&
                tg_gui_session_state.typing.peer_id_lo ==
                    tg_gui_session_state.open_peer_id_lo) {
                const char *name;

                /* Name who is typing: in a DM it is the open peer; in a group
                   match the typer's id against a recently shown sender. */
                name = 0;
                if (!tg_gui_session_state.open_peer_is_chat) {
                    if (tg_gui_session_state.current_peer_label[0] != '\0') {
                        name = tg_gui_session_state.current_peer_label;
                    }
                } else {
                    int mi;

                    /* (1) a recently shown sender (cheap, already loaded). */
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
                    /* (2)/(3) otherwise the lazily-fetched member list (supergroups,
                       off-MorphOS); one synchronous fetch per open group. */
                    if (name == 0) {
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
                dirty = 1;
            }
        }
    }
    return dirty;
}

void tg_gui_session_close(void)
{
    tg_mtproto_avatar_store_save(); /* keep the previews for next run */
    if (!tg_gui_session_state.open) {
        tg_chat_nq = 0;
        tg_chat_typing_target = 0;
        tg_chat_read_outbox_target = 0;
        return;
    }
    tg_gui_log("close: closing context");
    tg_mtproto_close_auth_context(&tg_gui_session_state.context);
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
