/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_LOGIN_H
#define TG_MTPROTO_LOGIN_H

#include "tg_mtproto_crypto.h"
#include "tg_mtproto_tl.h"
#include "tg_reaction.h"

#define TG_MTPROTO_PASSWORD_BYTES_MAX 256U

/* Sentinel peer "constructor" for Saved Messages: tg_write_input_peer emits a
   bare inputPeerSelf#7da07ec9 for it, so every RPC path (history, send,
   media, file transfer) reaches the self chat with zero special-casing. */
#define TG_MTPROTO_PEER_SELF_CONSTRUCTOR 0x7da07ec9UL

typedef struct tg_mtproto_rpc_result {
    unsigned long request_msg_id_hi;
    unsigned long request_msg_id_lo;
    unsigned long result_constructor;
    const unsigned char *result_body;
    unsigned long result_body_length;
} tg_mtproto_rpc_result;

typedef struct tg_mtproto_bad_msg_notification {
    unsigned long constructor;
    unsigned long bad_msg_id_hi;
    unsigned long bad_msg_id_lo;
    unsigned long bad_msg_seqno;
    unsigned long error_code;
    unsigned long new_server_salt_hi;
    unsigned long new_server_salt_lo;
    int has_new_server_salt;
} tg_mtproto_bad_msg_notification;

typedef struct tg_mtproto_sent_code {
    unsigned long constructor;
    unsigned long type_constructor;
    unsigned long type_length;
    unsigned long timeout;
    int has_timeout;
    int has_type_length;
    unsigned long next_type; /* auth.CodeType of the next route, 0 if none */
    int has_next_type;
    char phone_code_hash[128];
} tg_mtproto_sent_code;

typedef struct tg_mtproto_config_summary {
    unsigned long date;
    unsigned long expires;
    unsigned long test_mode_constructor;
    unsigned long this_dc;
} tg_mtproto_config_summary;

typedef struct tg_mtproto_password_summary {
    unsigned long flags;
    unsigned long current_algo_constructor;
    unsigned long current_g;
    unsigned long current_salt1_length;
    unsigned long current_salt2_length;
    unsigned long current_p_length;
    unsigned long srp_b_length;
    unsigned long srp_id_hi;
    unsigned long srp_id_lo;
    int has_recovery;
    int has_secure_values;
    int has_password;
    int has_current_algo;
    unsigned char current_salt1[TG_MTPROTO_PASSWORD_BYTES_MAX];
    unsigned char current_salt2[TG_MTPROTO_PASSWORD_BYTES_MAX];
    unsigned char current_p[TG_MTPROTO_PASSWORD_BYTES_MAX];
    unsigned char srp_b[TG_MTPROTO_PASSWORD_BYTES_MAX];
} tg_mtproto_password_summary;

/* Inline profile-photo thumb (userProfilePhoto/chatPhoto stripped_thumb): a
   tiny JPEG skeleton (~30-40 bytes typical) that expands to a real baseline
   JPEG offline -- the avatar v1 source. Thumbs that do not fit whole are
   dropped (a truncated JPEG payload is useless), so the cap only trades
   coverage of rare outliers for RAM. */
#define TG_MTPROTO_STRIPPED_MAX 128U

/* Looks up the captured stripped thumb for a peer id (the avatar store filled
   by the users/chats scanners). Returns 1 and points into the store (valid
   until the slot is evicted -- copy or decode immediately), 0 = none. */
/* --- F9 file sharing: document TL (layer 214, verified at-layer on TDLib
   a8db5023 + core.telegram.org; sendMedia is #ac55d9c1 AT 214, NOT the live
   #0330e77f -- the method changed after our layer) ------------------------ */
#define TG_MTPROTO_FILE_REF_MAX 256U /* forwarded/saved files (e.g. a PDF in
        Saved Messages) carry references well past 64 bytes; dropping them left
        the file undownloadable */
#define TG_MTPROTO_DOC_NAME_MAX 128U
#define TG_MTPROTO_DOC_MIME_MAX 64U

/* The kind is derived once, after the whole attribute vector is read: a GIF
   carries video and animated together, a voice note carries audio with the
   voice flag set, and a sticker carries an imageSize as well. Deciding as the
   attributes arrive would let the last one win and misread all three. */
#define TG_MTPROTO_DOC_KIND_FILE    0U
#define TG_MTPROTO_DOC_KIND_STICKER 1U
#define TG_MTPROTO_DOC_KIND_GIF     2U
#define TG_MTPROTO_DOC_KIND_VIDEO   3U
#define TG_MTPROTO_DOC_KIND_VOICE   4U
#define TG_MTPROTO_DOC_KIND_AUDIO   5U
#define TG_MTPROTO_DOC_ATTR_STICKER  1UL
#define TG_MTPROTO_DOC_ATTR_ANIMATED 2UL
#define TG_MTPROTO_DOC_ATTR_VIDEO    4UL
#define TG_MTPROTO_DOC_ATTR_AUDIO    8UL
#define TG_MTPROTO_DOC_ATTR_VOICE    16UL
#define TG_MTPROTO_DOC_ATTR_IMAGE    32UL
/* Room for the emoji a sticker stands for. One codepoint is at most 4 bytes;
   a joined sequence (family, flag, skin tone) runs longer and gets trimmed
   back to a whole codepoint rather than kept as a broken one. */
#define TG_MTPROTO_DOC_ALT_MAX 20U

typedef struct tg_mtproto_document_meta {
    int has_document; /* 0 = documentEmpty or absent */
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    unsigned long size_hi;
    unsigned long size_lo;
    unsigned long dc_id;
    unsigned long file_reference_len; /* 0 = unusable (too big/absent) */
    unsigned char file_reference[TG_MTPROTO_FILE_REF_MAX];
    char mime[TG_MTPROTO_DOC_MIME_MAX];
    char file_name[TG_MTPROTO_DOC_NAME_MAX]; /* "" when no filename attr */
    /* What the document is, and the few attribute fields worth putting in a
       bubble. The parser already walked the whole attribute vector to reach
       the filename; from 0.0.92 it keeps what it walked past. */
    unsigned char kind;               /* TG_MTPROTO_DOC_KIND_* */
    unsigned long attr_seen;          /* parser scratch: TG_MTPROTO_DOC_ATTR_* */
    char alt[TG_MTPROTO_DOC_ALT_MAX]; /* sticker emoji, "" when none */
    unsigned long duration;           /* seconds, 0 = absent or unknown */
    unsigned long width;              /* pixels, 0 = absent */
    unsigned long height;
} tg_mtproto_document_meta;

/* Message photo (layer 214 Photo + selected PhotoSize). The parser keeps one
   bounded inline representation and one larger viewer representation. They
   share the Photo identity/reference; a truncated reference is never kept. */
#define TG_MTPROTO_PHOTO_TYPE_MAX 8U
#define TG_MTPROTO_PHOTO_VARIANT_MAX 8U
typedef struct tg_mtproto_photo_variant {
    char type[TG_MTPROTO_PHOTO_TYPE_MAX];
    unsigned long width;
    unsigned long height;
    unsigned long size;
    unsigned char progressive;
} tg_mtproto_photo_variant;

typedef struct tg_mtproto_photo_meta {
    int has_photo; /* 0 = photoEmpty, no usable size, or absent */
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    unsigned long dc_id;
    unsigned long file_reference_len;
    unsigned char file_reference[TG_MTPROTO_FILE_REF_MAX];
    char thumb_type[TG_MTPROTO_PHOTO_TYPE_MAX];
    unsigned long width;
    unsigned long height;
    unsigned long size;
    int has_large;
    char large_thumb_type[TG_MTPROTO_PHOTO_TYPE_MAX];
    unsigned long large_width;
    unsigned long large_height;
    unsigned long large_size;
    unsigned long variant_count;
    tg_mtproto_photo_variant variants[TG_MTPROTO_PHOTO_VARIANT_MAX];
    unsigned long stripped_len;
    unsigned char stripped[TG_MTPROTO_STRIPPED_MAX];
    /* 0.0.92: this "photo" is a document's thumbnail (a sticker still, a
       video frame), so the identity above is the Document's and the fetch
       must ask for inputDocumentFileLocation. Everything downstream, the
       queue, the cache, the decoder and the pacing, is unchanged: a thumb is
       a JPEG like any other. */
    int from_document;
} tg_mtproto_photo_meta;

/* The bounded part of a WebPage that both clients can display. The parser
   stops after its photo; embed/document/instant-view tails are not consumed. */
#define TG_MTPROTO_WEBPAGE_TEXT_MAX 280U
#define TG_MTPROTO_WEBPAGE_URL_MAX 256U
typedef struct tg_mtproto_web_page {
    unsigned long id_hi;
    unsigned long id_lo;
    int pending;
    char text[TG_MTPROTO_WEBPAGE_TEXT_MAX];
    tg_mtproto_photo_meta photo;
    /* The page's address, to open it: empty when it does not fit whole (a
       cut address would open something else). */
    char url[TG_MTPROTO_WEBPAGE_URL_MAX];
    /* Its picture stands for the page (an article, a video, a site), so a
       click on it opens the address; 0 when the page is the picture itself
       (type "photo") or has no address to open. */
    int opens_link;
} tg_mtproto_web_page;

/* What goes under a message's own text for this preview: the label lines,
   then the page's address on a line of its own when the message does not
   show it already (a link hidden behind a word), so that there is always
   something to click. message_text may be UTF-8 or Latin-1: addresses are
   ASCII. room is how many bytes the message can still take, the line break
   before the suffix included: the address goes in only if it fits whole (a
   cut one would open something else). Empty when the preview has nothing
   to add. Returns whether the address is shown, by the message or by the
   suffix. */
#define TG_MTPROTO_WEBPAGE_SUFFIX_MAX \
    (TG_MTPROTO_WEBPAGE_TEXT_MAX + TG_MTPROTO_WEBPAGE_URL_MAX + 2U)
int tg_mtproto_web_page_suffix(const tg_mtproto_web_page *page,
                               const char *message_text, unsigned long room,
                               char *out, unsigned long out_size);

tg_mtproto_tl_status tg_mtproto_read_web_page(tg_mtproto_tl_reader *reader,
                                             tg_mtproto_web_page *out);

/* Parses one bare Document (document#8fd4c4d8 / documentEmpty#36f8c871),
   reader positioned ON the constructor; leaves the reader right after the
   object (every variant is walked wire-exactly).

   `thumb` is optional. When given, the thumbs vector is selected into it the
   way a Photo's sizes are, and on success it carries the Document's identity
   with from_document set, ready for the inline photo pipeline: that is a
   sticker's still and a video's frame. Pass NULL to skip the vector as
   before. */
tg_mtproto_tl_status tg_mtproto_read_document(tg_mtproto_tl_reader *reader,
                                              tg_mtproto_document_meta *out,
                                              tg_mtproto_photo_meta *thumb);

/* upload.getFile of a document (inputDocumentFileLocation, empty thumb_size);
   same offset/limit rules as the avatar download. */
tg_mtproto_tl_status tg_mtproto_build_upload_get_document(
    tg_mtproto_tl_writer *writer,
    const tg_mtproto_document_meta *doc,
    unsigned long offset,
    unsigned long limit);

/* Parses one bare Photo (photo#fb197a65 / photoEmpty#2331b22d), selecting one
   downloadable PhotoSize while consuming every size/video-size wire-exactly. */
tg_mtproto_tl_status tg_mtproto_read_photo(tg_mtproto_tl_reader *reader,
                                           tg_mtproto_photo_meta *out);

/* upload.getFile(inputPhotoFileLocation) for the selected thumbnail. */
tg_mtproto_tl_status tg_mtproto_build_upload_get_photo(
    tg_mtproto_tl_writer *writer,
    const tg_mtproto_photo_meta *photo,
    unsigned long offset,
    unsigned long limit);

/* upload.saveFilePart#b304a621 (small-file upload, <= 10 MB). */
tg_mtproto_tl_status tg_mtproto_build_upload_save_file_part(
    tg_mtproto_tl_writer *writer,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long part_index,
    const unsigned char *data,
    unsigned long data_length);

/* upload.saveBigFilePart#de7b673d (large-file upload, > 10 MB). */
tg_mtproto_tl_status tg_mtproto_build_upload_save_big_file_part(
    tg_mtproto_tl_writer *writer,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long part_index,
    unsigned long total_parts,
    const unsigned char *data,
    unsigned long data_length);

/* messages.sendMedia#ac55d9c1 with inputMediaUploadedDocument (force_file,
   filename attribute) and inputFile. caption is UTF-8, NULL/empty for none.
   random_id must be fresh. */
tg_mtproto_tl_status tg_mtproto_build_messages_send_media_document(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long file_parts,
    const char *file_name,
    const char *mime_type,
    const char *caption,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

/* messages.sendMedia#ac55d9c1 with inputMediaUploadedPhoto#1e287d04 and a
   small inputFile. Layer 214; JPEG validation happens before this writer.
   caption is UTF-8, NULL/empty for none. */
tg_mtproto_tl_status tg_mtproto_build_messages_send_media_photo(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long file_parts,
    const char *file_name,
    const char *caption,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

/* Albums: several photos in one message. A photo goes up in parts as usual,
   messages.uploadMedia#14967978 turns the parts into a photo of ours that is
   in no chat yet, and one messages.sendMultiMedia#1bf89d74 names up to
   TG_MTPROTO_ALBUM_MAX of them. Layer 214. */
#define TG_MTPROTO_ALBUM_MAX 10U

typedef struct tg_mtproto_album_photo {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    unsigned long file_reference_len;
    unsigned char file_reference[TG_MTPROTO_FILE_REF_MAX];
} tg_mtproto_album_photo;

tg_mtproto_tl_status tg_mtproto_build_messages_upload_media_photo(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long file_parts,
    const char *file_name);

/* The messageMediaPhoto#695150d7 that uploadMedia answers; `body` starts at
   the constructor. Fails on anything that cannot be named in an album. */
tg_mtproto_tl_status tg_mtproto_read_uploaded_photo(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_album_photo *out);

/* caption is UTF-8 and goes on the first photo; random_ids holds a (hi, lo)
   pair for each photo. */
tg_mtproto_tl_status tg_mtproto_build_messages_send_multi_media(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    const tg_mtproto_album_photo *photos,
    unsigned int count,
    const char *caption,
    const unsigned long *random_ids);

/* Same sendMedia envelope, but the uploaded file is referenced as inputFileBig
   after upload.saveBigFilePart. */
tg_mtproto_tl_status tg_mtproto_build_messages_send_media_big_document(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long file_parts,
    const char *file_name,
    const char *mime_type,
    const char *caption,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

/* Thumb-store persistence (data/telegram-thumbs.bin): blurred previews
   survive client restarts. Load at session open, save at session close. */
unsigned long tg_mtproto_avatar_store_generation(void);
void tg_mtproto_avatar_store_save(void);
void tg_mtproto_avatar_store_load(void);

int tg_mtproto_avatar_thumb_lookup(unsigned long id_hi, unsigned long id_lo,
                                   const unsigned char **thumb,
                                   unsigned long *len);

typedef struct tg_mtproto_user_summary {
    unsigned long constructor;
    unsigned long flags;
    unsigned long flags2;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    int has_access_hash;
    int is_self;
    int is_bot;
    char first_name[96];
    char last_name[96];
    char username[96];
    char phone[64];
    /* userProfilePhoto#82d1f706 capture (all zero when photo empty/absent). */
    unsigned long photo_id_hi;
    unsigned long photo_id_lo;
    unsigned long photo_dc_id;
    unsigned long stripped_len;
    unsigned char stripped[TG_MTPROTO_STRIPPED_MAX];
} tg_mtproto_user_summary;

typedef struct tg_mtproto_dialogs_summary {
    unsigned long constructor;
    unsigned long count;
    unsigned long dialog_count;
    unsigned long message_count;
    unsigned long chat_count;
    unsigned long user_count;
    int is_slice;
    int is_not_modified;
} tg_mtproto_dialogs_summary;

#define TG_MTPROTO_DIALOG_PEER_MAX 32U

/* Transient dialog settings, never part of telegram-peers.txt. */
#define TG_MTPROTO_PEER_USER_CONSTRUCTOR 0x59511722UL
#define TG_MTPROTO_PEER_CHAT_CONSTRUCTOR 0x36c6019aUL
#define TG_MTPROTO_PEER_CHANNEL_CONSTRUCTOR 0xa2a5371eUL
typedef struct tg_mtproto_peer_notify_settings {
    unsigned long mute_until;
    unsigned char silent;
    unsigned char valid;
} tg_mtproto_peer_notify_settings;

void tg_mtproto_peer_notify_get(unsigned long peer_constructor,
                                 unsigned long id_hi, unsigned long id_lo,
                                 tg_mtproto_peer_notify_settings *out);
/* A count that goes up whenever a chat's mute time is stored different from
   what it was (the dialog list, a change on another device, a Mute sent
   from here): whoever draws the mutes compares it with the one it saw. */
unsigned long tg_mtproto_peer_notify_mute_changes(void);
/* The chat is muted until that Unix time (0 = not muted), as this run
   knows it: what a Mute sent from here leaves behind. */
void tg_mtproto_peer_notify_set_mute(unsigned long peer_constructor,
                                     unsigned long id_hi, unsigned long id_lo,
                                     unsigned long mute_until);

typedef struct tg_mtproto_dialog_peer {
    unsigned long peer_constructor;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long top_message;
    unsigned long unread_count;
    unsigned long read_outbox_max_id; /* peer has read our msgs up to this id */
    unsigned long unread_mentions_count; /* unread messages that mention us */
    int pinned;                       /* dialog flags.2: pinned to the top */
    unsigned long folder_id;          /* dialog flags.4: 1 = in the Archive */
} tg_mtproto_dialog_peer;

typedef struct tg_mtproto_dialog_peer_list {
    unsigned long count;
    unsigned long total_dialog_count;
    int truncated;
    tg_mtproto_dialog_peer peers[TG_MTPROTO_DIALOG_PEER_MAX];
} tg_mtproto_dialog_peer_list;

/* Persisted chat-cache capacity (telegram-peers.txt). Raised so the first-login
   dialog sweep can keep more of a congested account; m68k stays lower for RAM
   (each entry ~248 bytes, and several static caches are sized by this). */
#if defined(__m68k__)
#define TG_MTPROTO_PEER_CACHE_MAX 64U
#else
#define TG_MTPROTO_PEER_CACHE_MAX 128U
#endif

typedef struct tg_mtproto_peer_cache_entry {
    unsigned long peer_constructor;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    unsigned long top_message;
    unsigned long unread_count;
    int has_access_hash;
    int is_self;
    int is_bot;
    int from_dialog;
    /* The folder its dialog came in (1 = the Archive), as the last dialog
       list read said. In memory only: the peers file does not keep it. */
    unsigned long folder_id;
    char title[128];
    char username[96];
    /* Avatar v1: profile-photo id/dc + inline stripped thumb, captured from
       the users/chats vectors (in-memory only; the peers FILE format is
       unchanged -- thumbs re-arrive with the first dialogs/search fetch). */
    unsigned long photo_id_hi;
    unsigned long photo_id_lo;
    unsigned long photo_dc_id;
    unsigned long stripped_len;
    unsigned char stripped[TG_MTPROTO_STRIPPED_MAX];
} tg_mtproto_peer_cache_entry;

typedef struct tg_mtproto_peer_cache {
    unsigned long count;
    unsigned long total_dialog_count;
    unsigned long user_count;
    unsigned long chat_count;
    int truncated;
    /* When non-zero, the user/chat scanners add new entries for every peer
       found in the response instead of only enriching existing dialog peers.
       Used to collect message senders (group/channel members) for display. */
    int collect_all;
    tg_mtproto_peer_cache_entry entries[TG_MTPROTO_PEER_CACHE_MAX];
} tg_mtproto_peer_cache;

typedef struct tg_mtproto_messages_summary {
    unsigned long constructor;
    unsigned long flags;
    unsigned long count;
    unsigned long message_count;
    unsigned long chat_count;
    unsigned long user_count;
    int is_slice;
    int is_not_modified;
    int is_channel_messages;
} tg_mtproto_messages_summary;

/* A message's text as it arrives, in UTF-8. Telegram's 4096 characters take
   up to 8 KB when each is two bytes (accented letters, Cyrillic, Greek,
   emoji), so every lane keeps 8 KB and shows such a message whole; a longer
   text ends with " [...]", cut on a character boundary. */
#ifndef TG_MTPROTO_MESSAGE_TEXT_MAX /* overridable: the 68000 LOWMEM build
                                       shrinks the per-message text buffer */
#define TG_MTPROTO_MESSAGE_TEXT_MAX 8192U
#endif
/* How many messages one getHistory read parses into the shared static list.
   This caps the GUI open-backlog -- it was 8, far too few. Each entry is
   ~4KB, so the list is sized per-platform to respect the 8MB OS3 budget while
   giving the PPC/AROS lanes a deeper backlog. */
#ifndef TG_MTPROTO_MESSAGE_TEXT_LIST_MAX /* overridable: the host build can
                                            force the m68k profile so ASan
                                            hunts with the small-box bounds */
#if defined(__m68k__)
#define TG_MTPROTO_MESSAGE_TEXT_LIST_MAX 32U
#else
#define TG_MTPROTO_MESSAGE_TEXT_LIST_MAX 64U
#endif
#endif
/* A reply's quoted-snippet preview: one short line, capped well below the body
   so the 8-message list stays cheap on the 8MB OS3 budget. */
#define TG_MTPROTO_MESSAGE_REPLY_QUOTE_MAX 96U

typedef struct tg_mtproto_message_text {
    unsigned long id;
    unsigned long date;
    unsigned long flags;
    unsigned long from_constructor; /* sender peer kind (PEER_*), 0 if absent */
    unsigned long from_id_hi;
    unsigned long from_id_lo;
    int is_out;
    int has_text;
    char text[TG_MTPROTO_MESSAGE_TEXT_MAX];
    int has_reply;                  /* message replies to another (reply bit) */
    unsigned long reply_to_msg_id;  /* id of the replied-to message, 0 if none */
    /* Inline quote the server includes in the reply header (raw UTF-8); shown
       as a dimmed reference line. Empty when the reply carries no quote. */
    char reply_quote[TG_MTPROTO_MESSAGE_REPLY_QUOTE_MAX];
    /* F9: attached document (file/video/voice/...) captured for the "[File:
       name (size)]" label AND the on-request download -- has_document is 0 for
       a plain-text or non-document media message. */
    tg_mtproto_document_meta document;
    /* 0.0.9: photo metadata is transiently retained long enough for the GUI
       session to queue a bounded cache download. photo_only distinguishes the
       synthetic "[Photo]" fallback from a real caption. */
    tg_mtproto_photo_meta photo;
    int photo_only;
    int photo_is_link; /* the photo is a link preview's: a click opens the link */
    unsigned long pending_webpage_hi;
    unsigned long pending_webpage_lo;
    /* A service message (someone joined, a message was pinned, a call...):
       the MessageAction constructor, 0 for an ordinary message. text then
       holds only the action's own string (a title, a custom line); the line
       shown is written with the names by tg_mtproto_service_text. */
    unsigned long service_action;
    unsigned long service_user_hi;  /* the user the action names, if any */
    unsigned long service_user_lo;
    unsigned long service_user_count;
    unsigned long service_value;    /* seconds, timer period, score, boosts */
    unsigned long service_flags;    /* the action's own flags */
    unsigned long service_reason;   /* a call's PhoneCallDiscardReason */
    tg_reactions reactions;
    /* 1 when the reader got as far as the reactions field: reactions is
       then the message's own (none when it had no field). A reader that
       stops earlier (a media or an entity it cannot walk) leaves 0. */
    unsigned char reactions_known;
    /* The poll this message carries (0, 0 = none): its text is the poll
       written out, and tg_mtproto_poll_find gives the poll itself. */
    unsigned long poll_id_hi;
    unsigned long poll_id_lo;
} tg_mtproto_message_text;

/* --- polls ---------------------------------------------------------------
   A poll as its message carries it (messageMediaPoll: the Poll and its
   PollResults), kept so that it can be voted and written out again when
   the votes change. The texts are UTF-8 as Telegram sends them, cut to
   the buffers. The results are known once this account has voted or the
   poll is closed: Telegram sends none before. */
#if defined(__m68k__)
#define TG_MTPROTO_POLL_ANSWERS_MAX 10
#define TG_MTPROTO_POLLS_MAX 3
#else
#define TG_MTPROTO_POLL_ANSWERS_MAX 12
#define TG_MTPROTO_POLLS_MAX 8
#endif
#define TG_MTPROTO_POLL_QUESTION_MAX 160
#define TG_MTPROTO_POLL_ANSWER_MAX 64
#define TG_MTPROTO_POLL_OPTION_MAX 8
typedef struct tg_mtproto_poll_answer {
    char text[TG_MTPROTO_POLL_ANSWER_MAX];
    unsigned char option[TG_MTPROTO_POLL_OPTION_MAX]; /* what a vote sends */
    unsigned char option_len;
    unsigned char chosen;  /* this account voted it */
    unsigned char correct; /* a quiz's right answer, once known */
    unsigned long voters;
} tg_mtproto_poll_answer;
typedef struct tg_mtproto_poll {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned char closed;
    unsigned char multiple; /* several answers may be chosen */
    unsigned char quiz;
    unsigned char public_voters;
    unsigned char has_results; /* the voters of each answer are known */
    unsigned char results_min; /* ...but not which ones this account chose */
    unsigned char voted;       /* this account chose at least one */
    unsigned char answer_count;
    unsigned long total_voters;
    char question[TG_MTPROTO_POLL_QUESTION_MAX];
    tg_mtproto_poll_answer answers[TG_MTPROTO_POLL_ANSWERS_MAX];
} tg_mtproto_poll;

/* The poll with this id among the latest few read, 0 when not kept. */
const tg_mtproto_poll *tg_mtproto_poll_find(unsigned long id_hi,
                                            unsigned long id_lo);

/* The poll written out as the text of its message: the question, an
   answer a line (numbered; with its share of the votes once the results
   are known, and a mark on the ones this account chose and on a quiz's
   right one), and a last line with the votes and what kind of poll it
   is. UTF-8, cut to the buffer between two characters. */
void tg_mtproto_poll_text(const tg_mtproto_poll *poll, char *out,
                          unsigned long size);

/* updateMessagePoll#aca1657b flags:# poll_id:long poll:flags.0?Poll
   results:PollResults, the reader just past the constructor: the poll
   kept takes the new votes (and the new poll, when it comes along).
   1 = a kept poll changed, and *id_hi / *id_lo say which. */
int tg_mtproto_read_update_message_poll(tg_mtproto_tl_reader *reader,
                                        unsigned long *id_hi,
                                        unsigned long *id_lo);

/* messages.sendVote#10ea6184 peer:InputPeer msg_id:int
   options:Vector<bytes>: the answers of `poll` whose bit is set in
   `mask` (bit 0 = the first); none takes the vote back. */
tg_mtproto_tl_status tg_mtproto_build_messages_send_vote(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long message_id,
    const tg_mtproto_poll *poll,
    unsigned long mask);

/* A poll made here: messages.sendMedia#ac55d9c1 with
   inputMediaPoll#f94e5f1 and a new poll#58747131, anonymous, with one
   answer or several allowed. question and answers are UTF-8; two to
   TG_MTPROTO_POLL_CREATE_MAX answers, none empty. */
#define TG_MTPROTO_POLL_CREATE_MAX 10U
tg_mtproto_tl_status tg_mtproto_build_messages_send_media_poll(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    const char *question,
    const char *const *answers,
    unsigned int count,
    int multiple,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

/* Stopping a poll: messages.editMessage#dfd14005 with nothing but the
   media, the message's poll with its closed flag set. The question and
   the answers are not sent again: Telegram keeps the poll's own. */
tg_mtproto_tl_status tg_mtproto_build_messages_edit_message_close_poll(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long message_id,
    unsigned long poll_id_hi,
    unsigned long poll_id_lo);

/* messageMediaPoll#4bd6e798 poll:Poll results:PollResults, the reader
   just past the constructor: a poll already kept takes what the message
   says of it now. 1 = it did, and *id_hi / *id_lo say which. */
int tg_mtproto_read_message_media_poll(tg_mtproto_tl_reader *reader,
                                       unsigned long *id_hi,
                                       unsigned long *id_lo);

/* The kept poll is closed from now on: Telegram has just agreed to stop
   it. 1 = it was kept and open. */
int tg_mtproto_poll_mark_closed(unsigned long id_hi, unsigned long id_lo);

tg_mtproto_tl_status tg_mtproto_read_message_reactions(
    tg_mtproto_tl_reader *reader, tg_reactions *out);
tg_mtproto_tl_status tg_mtproto_read_update_message_reactions(
    tg_mtproto_tl_reader *reader, tg_mtproto_dialog_peer *peer,
    unsigned long *message_id, tg_reactions *out);
tg_mtproto_tl_status tg_mtproto_build_messages_send_reaction(
    tg_mtproto_tl_writer *writer, unsigned long peer_constructor,
    unsigned long peer_id_hi, unsigned long peer_id_lo,
    unsigned long hash_hi, unsigned long hash_lo, int has_hash,
    unsigned long message_id, unsigned long reaction_cp);
tg_mtproto_tl_status tg_mtproto_build_get_full_chat(
    tg_mtproto_tl_writer *writer, unsigned long peer_constructor,
    unsigned long id_hi, unsigned long id_lo,
    unsigned long hash_hi, unsigned long hash_lo);
int tg_mtproto_scan_chat_reactions(const unsigned char *body,
                                   unsigned long length,
                                   tg_reactions_allowed *out);
tg_mtproto_tl_status tg_mtproto_build_messages_get_messages_reactions(
    tg_mtproto_tl_writer *writer, unsigned long peer_constructor,
    unsigned long peer_id_hi, unsigned long peer_id_lo,
    unsigned long hash_hi, unsigned long hash_lo, int has_hash,
    const unsigned long *message_ids, unsigned int count);

typedef struct tg_mtproto_message_text_list {
    unsigned long count;
    unsigned long total_message_count;
    int truncated;
    unsigned long abort_constructor; /* msg constructor that stopped the walk, else 0 */
    unsigned long page_count;     /* messages in the fetched vector (page size) -- diag */
    unsigned long resync_attempts;/* times the walk had to resync past a bad item -- diag */
    unsigned long resync_ok;      /* of those, how many re-landed on a message -- diag */
    tg_mtproto_message_text messages[TG_MTPROTO_MESSAGE_TEXT_LIST_MAX];
} tg_mtproto_message_text_list;

typedef struct tg_mtproto_updates_summary {
    unsigned long constructor;
    unsigned long flags;
    unsigned long id;
    unsigned long date;
    int has_sent_message;
    tg_mtproto_web_page webpage;
} tg_mtproto_updates_summary;

tg_mtproto_tl_status tg_mtproto_build_invoke_with_layer(
    tg_mtproto_tl_writer *writer,
    unsigned long layer,
    const unsigned char *query,
    unsigned long query_length);

tg_mtproto_tl_status tg_mtproto_build_invoke_without_updates(
    tg_mtproto_tl_writer *writer,
    const unsigned char *query,
    unsigned long query_length);

tg_mtproto_tl_status tg_mtproto_build_init_connection(
    tg_mtproto_tl_writer *writer,
    unsigned long api_id,
    const char *device_model,
    const char *system_version,
    const char *app_version,
    const char *lang_code,
    const unsigned char *query,
    unsigned long query_length);

tg_mtproto_tl_status tg_mtproto_build_auth_send_code(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    unsigned long api_id,
    const char *api_hash);

tg_mtproto_tl_status tg_mtproto_build_auth_sign_in(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    const char *phone_code_hash,
    const char *phone_code);

/* auth.resendCode: ask Telegram to send the pending code by the route it
   named as next_type in the previous auth.sentCode (usually SMS). */
tg_mtproto_tl_status tg_mtproto_build_auth_resend_code(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    const char *phone_code_hash);

tg_mtproto_tl_status tg_mtproto_build_auth_sign_up(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    const char *phone_code_hash,
    const char *first_name,
    const char *last_name);

tg_mtproto_tl_status tg_mtproto_build_input_check_password_empty(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_input_check_password_srp(
    tg_mtproto_tl_writer *writer,
    unsigned long srp_id_hi,
    unsigned long srp_id_lo,
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char m1[TG_MTPROTO_SHA256_LENGTH]);

tg_mtproto_tl_status tg_mtproto_build_auth_check_password_empty(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_auth_check_password_srp(
    tg_mtproto_tl_writer *writer,
    unsigned long srp_id_hi,
    unsigned long srp_id_lo,
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char m1[TG_MTPROTO_SHA256_LENGTH]);

tg_mtproto_tl_status tg_mtproto_build_help_get_config(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_account_get_password(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_users_get_self(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_messages_get_dialogs(
    tg_mtproto_tl_writer *writer,
    unsigned long limit);
tg_mtproto_tl_status tg_mtproto_build_messages_get_dialogs_page(
    tg_mtproto_tl_writer *writer,
    unsigned long limit,
    unsigned long offset_id,
    unsigned long offset_peer_constructor,
    unsigned long offset_peer_id_hi,
    unsigned long offset_peer_id_lo,
    unsigned long offset_access_hash_hi,
    unsigned long offset_access_hash_lo,
    int offset_has_access_hash);

tg_mtproto_tl_status tg_mtproto_build_contacts_resolve_username(
    tg_mtproto_tl_writer *writer,
    const char *username);
tg_mtproto_tl_status tg_mtproto_build_contacts_resolve_username_flags(
    tg_mtproto_tl_writer *writer,
    const char *username);
tg_mtproto_tl_status tg_mtproto_build_contacts_search(
    tg_mtproto_tl_writer *writer,
    const char *query,
    unsigned long limit);

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_self(
    tg_mtproto_tl_writer *writer,
    unsigned long limit);

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_user(
    tg_mtproto_tl_writer *writer,
    unsigned long user_id_hi,
    unsigned long user_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long limit);

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long offset_id, /* page older: server returns id < offset_id; 0 = newest */
    unsigned long limit);

/* The same with add_offset: negative = that many messages newer than
   offset_id come first, to centre a page on one message. */
tg_mtproto_tl_status tg_mtproto_build_messages_get_history_peer_at(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long offset_id,
    long add_offset,
    unsigned long limit);

/* messages.getPinnedDialogs#d6b94df2 folder_id:int -> messages.peerDialogs
   with the pinned dialogs of that folder (0 = the main list), in order. */
tg_mtproto_tl_status tg_mtproto_build_messages_get_pinned_dialogs(
    tg_mtproto_tl_writer *writer, unsigned long folder_id);

/* messages.toggleDialogPin#a731e257 flags:# pinned:flags.0?true
   peer:InputDialogPeer -> Bool. pinned = 1 pins, 0 unpins. */
tg_mtproto_tl_status tg_mtproto_build_messages_toggle_dialog_pin(
    tg_mtproto_tl_writer *writer,
    int pinned,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

/* folders.editPeerFolders for one chat: folder_id 1 archives it, 0 brings
   it back to the main list. */
tg_mtproto_tl_status tg_mtproto_build_folders_edit_peer_folder(
    tg_mtproto_tl_writer *writer,
    unsigned long folder_id,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

/* One page of a folder's dialogs (0 the main list, 1 the Archive). The page
   after a dialog is asked with the date and id of its top message and its
   peer; exclude_pinned leaves the pinned dialogs out of later pages. */
tg_mtproto_tl_status tg_mtproto_build_messages_get_dialogs_folder_page(
    tg_mtproto_tl_writer *writer,
    unsigned long folder_id,
    int exclude_pinned,
    unsigned long limit,
    unsigned long offset_date,
    unsigned long offset_id,
    unsigned long offset_peer_constructor,
    unsigned long offset_peer_id_hi,
    unsigned long offset_peer_id_lo,
    unsigned long offset_access_hash_hi,
    unsigned long offset_access_hash_lo,
    int offset_has_access_hash);

/* The date of a message of a chat inside a reply body, 0 when absent. */
unsigned long tg_mtproto_find_message_date(const unsigned char *body,
                                           unsigned long body_length,
                                           unsigned long peer_constructor,
                                           unsigned long peer_id_hi,
                                           unsigned long peer_id_lo,
                                           unsigned long message_id);

/* Marks a chat read up to max_id: messages.readHistory, or
   channels.readHistory for a channel or a supergroup. */
tg_mtproto_tl_status tg_mtproto_build_read_history_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long max_id);

/* messages.markDialogUnread: sets (1) or clears (0) the unread mark. */
tg_mtproto_tl_status tg_mtproto_build_messages_mark_dialog_unread(
    tg_mtproto_tl_writer *writer,
    int unread,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

/* messages.getDialogUnreadMarks and its Vector<DialogPeer> reply. */
tg_mtproto_tl_status tg_mtproto_build_messages_get_dialog_unread_marks(
    tg_mtproto_tl_writer *writer);
tg_mtproto_tl_status tg_mtproto_parse_dialog_unread_marks(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_dialog_peer_list *out);

/* Drafts. messages.saveDraft with plain text (an empty text deletes the
   draft), messages.getAllDrafts, and one updateDraftMessage read from the
   reader just past its constructor: the chat, the text (cut to the buffer,
   *was_cut says so), *has_text 0 when the draft was deleted. A topic's
   draft or one this client cannot read fails. */
tg_mtproto_tl_status tg_mtproto_build_messages_save_draft(
    tg_mtproto_tl_writer *writer,
    const char *message,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);
tg_mtproto_tl_status tg_mtproto_build_messages_get_all_drafts(
    tg_mtproto_tl_writer *writer);
tg_mtproto_tl_status tg_mtproto_read_update_draft(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_dialog_peer *peer,
    int *has_text,
    char *text,
    unsigned long text_size,
    int *was_cut,
    unsigned long *date);
/* account.updateNotifySettings for one chat's mute alone (mute_until: a
   Unix time, 0 unmutes, 0x7fffffff forever), and an updateNotifySettings
   read from the reader just past its constructor into the same table
   tg_mtproto_peer_notify_get reads (OK = one chat's settings, kept). */
tg_mtproto_tl_status tg_mtproto_build_account_update_notify_mute(
    tg_mtproto_tl_writer *writer,
    unsigned long mute_until,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);
tg_mtproto_tl_status tg_mtproto_read_update_notify_settings(
    tg_mtproto_tl_reader *reader);
/* account.getNotifySettings for one chat, and its PeerNotifySettings
   answer (the whole object, constructor first) kept for that chat in the
   table tg_mtproto_peer_notify_get reads. */
tg_mtproto_tl_status tg_mtproto_build_account_get_notify_settings(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);
tg_mtproto_tl_status tg_mtproto_peer_notify_keep(
    const unsigned char *settings, unsigned long length,
    unsigned long peer_constructor, unsigned long id_hi, unsigned long id_lo);

/* The next messages.sendMessage built clears the chat's draft too (on) or
   leaves it (off, the default). */
void tg_mtproto_send_set_clear_draft(int on);

/* The mentions by name the next messages.sendMessage built carries: where
   each name is in its text (UTF-16 units) and whose it is. None (0, 0)
   takes them away again. */
#define TG_MTPROTO_SEND_MENTIONS_MAX 4
typedef struct tg_mtproto_send_mention {
    unsigned long offset;
    unsigned long length;
    unsigned long user_id_hi;
    unsigned long user_id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
} tg_mtproto_send_mention;
void tg_mtproto_send_set_mentions(const tg_mtproto_send_mention *list,
                                  int count);

/* The formatting the next messages.sendMessage built carries: where each
   styled stretch is in its text (UTF-16 units), in the order of the text.
   None (0, 0) takes it away again. A block (PRE) names no language. */
#define TG_MTPROTO_SEND_STYLES_MAX 16
#define TG_MTPROTO_STYLE_BOLD 1UL    /* messageEntityBold#bd610bc9 */
#define TG_MTPROTO_STYLE_ITALIC 2UL  /* messageEntityItalic#826f8b60 */
#define TG_MTPROTO_STYLE_CODE 3UL    /* messageEntityCode#28a20571 */
#define TG_MTPROTO_STYLE_PRE 4UL     /* messageEntityPre#73924be0 */
#define TG_MTPROTO_STYLE_STRIKE 5UL  /* messageEntityStrike#bf0693d4 */
#define TG_MTPROTO_STYLE_SPOILER 6UL /* messageEntitySpoiler#32ca960f */
typedef struct tg_mtproto_send_style {
    unsigned long kind; /* TG_MTPROTO_STYLE_* */
    unsigned long offset;
    unsigned long length;
} tg_mtproto_send_style;
void tg_mtproto_send_set_styles(const tg_mtproto_send_style *list,
                                int count);

/* messages.deleteHistory (not for a channel): for this account only, or
   with revoke for the other side of a private chat too; and the offset
   its messages.affectedHistory reply leaves to delete. */
tg_mtproto_tl_status tg_mtproto_build_messages_delete_history(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    int revoke);
tg_mtproto_tl_status tg_mtproto_parse_affected_history_offset(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    unsigned long *offset);

/* Leaves a channel or supergroup (channels.leaveChannel) or a basic group
   (messages.deleteChatUser with inputUserSelf). */
tg_mtproto_tl_status tg_mtproto_build_leave_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

/* The chat's pinned messages, newest pin first (messages.search with no
   words and the pinned filter); limit 1 gives the one under the header. */
tg_mtproto_tl_status tg_mtproto_build_messages_search_pinned(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long limit);

/* Unread mentions. messages.getUnreadMentions answers like a history
   page with the unread messages that mention this account (offset_id 1
   with add_offset -limit gives the oldest ones first, as the official
   desktop client asks them); and one message's mention marked as read
   (messages.readMessageContents, or channels.readMessageContents in a
   channel or supergroup). */
tg_mtproto_tl_status tg_mtproto_build_messages_get_unread_mentions(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long offset_id,
    long add_offset,
    unsigned long limit);
tg_mtproto_tl_status tg_mtproto_build_read_message_contents(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long message_id);

/* messages.updatePinnedMessage: pin or unpin one message; one_side keeps
   a private chat's pin to this account alone, silent pins without
   notifying the members. */
tg_mtproto_tl_status tg_mtproto_build_messages_update_pinned_message(
    tg_mtproto_tl_writer *writer,
    int pin,
    int one_side,
    int silent,
    unsigned long message_id,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

/* messages.search in one chat (no filter, newest first, below offset_id
   when non-zero). query_utf8 must be non-empty. */
tg_mtproto_tl_status tg_mtproto_build_messages_search_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    const char *query_utf8,
    unsigned long offset_id,
    unsigned long limit);

/* messages.searchGlobal: the same text search across every chat of the
   account, first page, newest first. query_utf8 must be non-empty. */
tg_mtproto_tl_status tg_mtproto_build_messages_search_global(
    tg_mtproto_tl_writer *writer,
    const char *query_utf8,
    unsigned long limit);

/* channels.getParticipants(channelParticipantsRecent) for a supergroup -- resolves
   member id->name for the typing indicator. The reply carries users:Vector<User>. */
tg_mtproto_tl_status tg_mtproto_build_channels_get_participants_recent(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long limit);

/* The same with the search filter: the members whose name or username
   begins with query_utf8. */
tg_mtproto_tl_status tg_mtproto_build_channels_get_participants_search(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    const char *query_utf8,
    unsigned long limit);

tg_mtproto_tl_status tg_mtproto_build_messages_get_peer_dialogs(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

tg_mtproto_tl_status tg_mtproto_build_messages_send_self(
    tg_mtproto_tl_writer *writer,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

tg_mtproto_tl_status tg_mtproto_build_messages_send_user(
    tg_mtproto_tl_writer *writer,
    unsigned long user_id_hi,
    unsigned long user_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

tg_mtproto_tl_status tg_mtproto_build_messages_send_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo);

/* messages.forwardMessages#978928ca (layer 214), flags=0: several messages
   of one chat in one request, in the order given, a random id each. Both
   endpoints use the same peer-constructor convention as send/getHistory. */
tg_mtproto_tl_status tg_mtproto_build_messages_forward_messages(
    tg_mtproto_tl_writer *writer,
    unsigned long from_peer_constructor,
    unsigned long from_peer_id_hi,
    unsigned long from_peer_id_lo,
    unsigned long from_access_hash_hi,
    unsigned long from_access_hash_lo,
    int from_has_access_hash,
    const unsigned long *message_ids,
    const unsigned long *random_ids_hi,
    const unsigned long *random_ids_lo,
    unsigned long count,
    unsigned long to_peer_constructor,
    unsigned long to_peer_id_hi,
    unsigned long to_peer_id_lo,
    unsigned long to_access_hash_hi,
    unsigned long to_access_hash_lo,
    int to_has_access_hash);

/* The same for one message. */
tg_mtproto_tl_status tg_mtproto_build_messages_forward_message(
    tg_mtproto_tl_writer *writer,
    unsigned long from_peer_constructor,
    unsigned long from_peer_id_hi,
    unsigned long from_peer_id_lo,
    unsigned long from_access_hash_hi,
    unsigned long from_access_hash_lo,
    int from_has_access_hash,
    unsigned long message_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo,
    unsigned long to_peer_constructor,
    unsigned long to_peer_id_hi,
    unsigned long to_peer_id_lo,
    unsigned long to_access_hash_hi,
    unsigned long to_access_hash_lo,
    int to_has_access_hash);

/* messages.editMessage#dfd14005: edit an own message's text (flags.11 message,
   peer, id, message). */
/* Avatar v2: upload.getFile of a peer's small (160px) profile photo via the
   inputPeerPhotoFileLocation shortcut. offset is a TL long (hi word 0). */
tg_mtproto_tl_status tg_mtproto_build_upload_get_peer_photo(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long photo_id_hi,
    unsigned long photo_id_lo,
    unsigned long offset,
    unsigned long limit);

/* Parses upload.file (type+mtime skipped, bytes out); recognizes
   upload.fileCdnRedirect via *cdn_redirect=1 (OK, no bytes). */
tg_mtproto_tl_status tg_mtproto_parse_upload_file(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    const unsigned char **bytes,
    unsigned long *bytes_length,
    int *cdn_redirect);

/* Avatar meta for a peer id from the capture store: photo_id + dc. 1=found. */
int tg_mtproto_avatar_meta_lookup(unsigned long id_hi, unsigned long id_lo,
                                  unsigned long *photo_id_hi,
                                  unsigned long *photo_id_lo,
                                  unsigned long *dc_id);

tg_mtproto_tl_status tg_mtproto_build_messages_edit_message(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long message_id,
    const char *message);

/* messages.deleteMessages#e58e95d2: delete one message (revoke = for everyone).
   For channels/supergroups use the channels.* form below. */
/* Several messages in one request (the single forms below are these with
   one id). */
tg_mtproto_tl_status tg_mtproto_build_messages_delete_messages_list(
    tg_mtproto_tl_writer *writer,
    int revoke,
    const unsigned long *message_ids,
    unsigned long count);
tg_mtproto_tl_status tg_mtproto_build_channels_delete_messages_list(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    const unsigned long *message_ids,
    unsigned long count);

tg_mtproto_tl_status tg_mtproto_build_messages_delete_messages(
    tg_mtproto_tl_writer *writer,
    int revoke,
    unsigned long message_id);

/* channels.deleteMessages#84c1fd4e: delete one message in a channel/supergroup. */
tg_mtproto_tl_status tg_mtproto_build_channels_delete_messages(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long message_id);

tg_mtproto_tl_status tg_mtproto_build_msgs_ack(
    tg_mtproto_tl_writer *writer,
    const unsigned long *msg_id_hi,
    const unsigned long *msg_id_lo,
    unsigned long msg_id_count);

tg_mtproto_tl_status tg_mtproto_parse_rpc_result(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_rpc_result *out);

tg_mtproto_tl_status tg_mtproto_parse_rpc_error(
    const unsigned char *body,
    unsigned long body_length,
    long *error_code,
    char *error_message,
    unsigned long error_message_size);

tg_mtproto_tl_status tg_mtproto_parse_bad_msg_notification(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_bad_msg_notification *out);

tg_mtproto_tl_status tg_mtproto_parse_auth_sent_code(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_sent_code *out);

tg_mtproto_tl_status tg_mtproto_parse_config_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_config_summary *out);

tg_mtproto_tl_status tg_mtproto_parse_account_password_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_password_summary *out);

tg_mtproto_tl_status tg_mtproto_parse_user_vector_first(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_user_summary *out);

tg_mtproto_tl_status tg_mtproto_parse_dialogs_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_dialogs_summary *out);

tg_mtproto_tl_status tg_mtproto_parse_dialog_peer_list(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_dialog_peer_list *out);

tg_mtproto_tl_status tg_mtproto_parse_dialog_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out);

tg_mtproto_tl_status tg_mtproto_parse_resolved_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out);
tg_mtproto_tl_status tg_mtproto_parse_contacts_search_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out);

const char *tg_mtproto_peer_constructor_name(unsigned long constructor);

tg_mtproto_tl_status tg_mtproto_parse_messages_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_messages_summary *out);

/* Parses one TL Message from the reader (the same core the history list
   parser uses); out_dest, when non-null, receives the message's peer_id --
   the chat it belongs to. Used by the chat's update-push collector. */
tg_mtproto_tl_status tg_mtproto_read_update_message_text(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_message_text *out,
    tg_mtproto_dialog_peer *out_dest);

/* The same walk, and page_out (optional) receives the message's
   messageMediaWebPage as tg_mtproto_read_web_page leaves it: pending with its
   id, or complete with the preview lines and photo, or all zero when the
   message carries no link. A send answer is read this way, because a page
   Telegram already had cached comes back complete right there and no
   updateWebPage follows it. */
tg_mtproto_tl_status tg_mtproto_read_update_message_page(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_message_text *out,
    tg_mtproto_dialog_peer *out_dest,
    tg_mtproto_web_page *page_out);

/* Scans forward from fallback_offset for the next TL Message constructor
   (the history walker's recovery step, exported for other Vector<Message>
   walkers). Returns non-zero when the reader was repositioned. */
int tg_mtproto_resync_message_text(tg_mtproto_tl_reader *reader,
                                   unsigned long fallback_offset);

/* updates.state / updates.getDifference (gap handling, layer 214). */
typedef struct tg_mtproto_updates_state {
    unsigned long pts;
    unsigned long qts;
    unsigned long date;
    unsigned long seq;
} tg_mtproto_updates_state;

tg_mtproto_tl_status tg_mtproto_build_updates_get_state(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_build_updates_get_difference(
    tg_mtproto_tl_writer *writer,
    unsigned long pts,
    unsigned long date,
    unsigned long qts,
    unsigned long pts_total_limit);

tg_mtproto_tl_status tg_mtproto_parse_updates_state(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_updates_state *out);

#define TG_MTPROTO_UPDATES_DIFFERENCE_EMPTY_CONSTRUCTOR 0x5d75a138UL
#define TG_MTPROTO_UPDATES_DIFFERENCE_CONSTRUCTOR 0x00f49ca0UL
#define TG_MTPROTO_UPDATES_DIFFERENCE_SLICE_CONSTRUCTOR 0xa8fb1981UL
#define TG_MTPROTO_UPDATES_DIFFERENCE_TOO_LONG_CONSTRUCTOR 0x4afe8f6dUL

tg_mtproto_tl_status tg_mtproto_parse_message_text_list(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_message_text_list *out);
tg_mtproto_tl_status tg_mtproto_parse_message_text_list_dests(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_message_text_list *out,
    tg_mtproto_dialog_peer *dests);

/* The line a service message reads as, in the words of the official clients:
   "Anna added Marco", "You pinned a message", "Marco joined Telegram".
   actor is who acted (NULL or "" when unknown), actor_is_self makes it "You",
   target is the user the action names (NULL or "" when unknown). An action
   without a wording of its own still gives a short generic line. */
void tg_mtproto_service_text(const tg_mtproto_message_text *message,
                             const char *actor, int actor_is_self,
                             const char *target, char *out,
                             unsigned long out_size);

/*
 * Extracts users/chats/channels referenced by a messages.* response into a peer
 * cache, so a group history can resolve each message's from_id to a name.
 */
void tg_mtproto_parse_message_peers(const unsigned char *body,
                                    unsigned long body_length,
                                    tg_mtproto_peer_cache *out);

/* A single sender from the users carried by an updates/difference container.
   No cache is filled. Names are UTF-8 and always cut on a character boundary. */
int tg_mtproto_find_user_name(const unsigned char *body,
                              unsigned long body_length,
                              unsigned long id_hi, unsigned long id_lo,
                              char *name, unsigned long name_size);
void tg_mtproto_copy_utf8_name(char *dest, unsigned long size, const char *src);

tg_mtproto_tl_status tg_mtproto_parse_updates_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_updates_summary *out);

/* Parses a bare updateReadHistoryOutbox#2f2f21bf body into peer + max_id (the
   read cursor for our sent messages). For real-time read receipts (5c). */
tg_mtproto_tl_status tg_mtproto_parse_update_read_history_outbox(
    const unsigned char *body, unsigned long body_length,
    tg_mtproto_dialog_peer *out_peer, unsigned long *out_max_id);

/* Same, but from a reader already past the updateReadHistoryOutbox constructor
   (used by the live update-stream dispatch). */
tg_mtproto_tl_status tg_mtproto_read_update_read_history_outbox(
    tg_mtproto_tl_reader *reader, tg_mtproto_dialog_peer *out_peer,
    unsigned long *out_max_id);

int tg_mtproto_is_auth_authorization_constructor(unsigned long constructor);

int tg_mtproto_login_self_test(void);

#endif
