/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <stdlib.h>
#include "tg_mtproto_login.h"
#include "tg_avatar.h"

#define TG_MTPROTO_CURRENT_LAYER 214UL
#define TG_INVOKE_WITH_LAYER_CONSTRUCTOR 0xda9b0d0dUL
#define TG_INVOKE_WITHOUT_UPDATES_CONSTRUCTOR 0xbf9459b7UL
#define TG_INIT_CONNECTION_CONSTRUCTOR 0xc1cd5ea9UL
#define TG_CODE_SETTINGS_CONSTRUCTOR 0xad253d78UL
#define TG_AUTH_SEND_CODE_CONSTRUCTOR 0xa677244fUL
#define TG_AUTH_SIGN_IN_CONSTRUCTOR 0x8d52a951UL
#define TG_AUTH_SIGN_UP_CONSTRUCTOR 0xaac7b717UL
#define TG_AUTH_CHECK_PASSWORD_CONSTRUCTOR 0xd18b4d16UL
#define TG_HELP_GET_CONFIG_CONSTRUCTOR 0xc4f9186bUL
#define TG_ACCOUNT_GET_PASSWORD_CONSTRUCTOR 0x548a30f5UL
#define TG_INPUT_CHECK_PASSWORD_EMPTY_CONSTRUCTOR 0x9880f658UL
#define TG_INPUT_CHECK_PASSWORD_SRP_CONSTRUCTOR 0xd27ff082UL
#define TG_USERS_GET_USERS_CONSTRUCTOR 0x0d91a548UL
#define TG_INPUT_USER_SELF_CONSTRUCTOR 0xf7c1b13fUL
#define TG_MESSAGES_GET_DIALOGS_CONSTRUCTOR 0xa0f4cb4fUL
#define TG_MESSAGES_GET_PEER_DIALOGS_CONSTRUCTOR 0xe470bcfdUL
#define TG_INPUT_DIALOG_PEER_CONSTRUCTOR 0xfcaafeb7UL
#define TG_MESSAGES_PEER_DIALOGS_CONSTRUCTOR 0x3371c354UL
#define TG_MESSAGES_GET_HISTORY_CONSTRUCTOR 0x4423e6c5UL
#define TG_MESSAGES_SEND_MESSAGE_CONSTRUCTOR 0xfe05dc9aUL
/* messages.forwardMessages#978928ca at layer 214, verified against the
   official TDLib schema revision that declares MTPROTO_LAYER = 214. */
#define TG_MESSAGES_FORWARD_MESSAGES_CONSTRUCTOR 0x978928caUL
/* inputReplyToMessage, layer 214 -- verified on core.telegram.org/constructor/
   inputReplyToMessage (current schema page self-reports Layer 214). For a plain
   reply we serialize flags=0 + reply_to_msg_id only, so the newest fields
   (monoforum/todo/quote) never hit the wire; only this 4-byte ctor id matters,
   and a wrong id surfaces as a visible RPC error (non-destructive). */
#define TG_INPUT_REPLY_TO_MESSAGE_CONSTRUCTOR 0x869fbe10UL
/* edit/delete (layer 214, hashes verified on core.telegram.org/method/...):
   messages.editMessage#dfd14005 flags:# ... peer:InputPeer id:int message:flags.11?string ...
   messages.deleteMessages#e58e95d2 flags:# revoke:flags.0?true id:Vector<int>
   channels.deleteMessages#84c1fd4e channel:InputChannel id:Vector<int> */
#define TG_MESSAGES_EDIT_MESSAGE_CONSTRUCTOR 0xdfd14005UL
#define TG_MESSAGES_DELETE_MESSAGES_CONSTRUCTOR 0xe58e95d2UL
#define TG_CHANNELS_DELETE_MESSAGES_CONSTRUCTOR 0x84c1fd4eUL
#define TG_MESSAGES_EDIT_MESSAGE_FLAG_MESSAGE 0x800UL /* flags.11 = message present */
#define TG_CONTACTS_RESOLVE_USERNAME_CONSTRUCTOR 0xf93ccba3UL
#define TG_CONTACTS_RESOLVE_USERNAME_FLAGS_CONSTRUCTOR 0x725afbbcUL
#define TG_CONTACTS_SEARCH_CONSTRUCTOR 0x11f812d8UL
#define TG_INPUT_PEER_EMPTY_CONSTRUCTOR 0x7f3b18eaUL
#define TG_INPUT_PEER_SELF_CONSTRUCTOR 0x7da07ec9UL
#define TG_INPUT_PEER_USER_CONSTRUCTOR 0xdde8a54cUL
#define TG_INPUT_PEER_SELF_CONSTRUCTOR 0x7da07ec9UL /* layer-214 verified */
#define TG_INPUT_PEER_CHAT_CONSTRUCTOR 0x35a95cb9UL
#define TG_INPUT_PEER_CHANNEL_CONSTRUCTOR 0x27bcbbfcUL
/* channels.getParticipants(channelParticipantsRecent) -- resolve a supergroup
   member id->name for the typing indicator (TL layer 214, verified on
   core.telegram.org). */
#define TG_CHANNELS_GET_PARTICIPANTS_CONSTRUCTOR 0x77ced9d0UL
#define TG_INPUT_CHANNEL_CONSTRUCTOR 0xf35aec28UL
#define TG_CHANNEL_PARTICIPANTS_RECENT_CONSTRUCTOR 0xde3f3c79UL
#define TG_PEER_USER_CONSTRUCTOR 0x59511722UL
#define TG_PEER_CHAT_CONSTRUCTOR 0x36c6019aUL
#define TG_PEER_CHANNEL_CONSTRUCTOR 0xa2a5371eUL
/* updateReadHistoryOutbox#2f2f21bf peer:Peer max_id:int pts:int pts_count:int
   (layer 214, verified on core.telegram.org/constructor/updateReadHistoryOutbox)
   -- the peer read OUR messages up to max_id; used for real-time read receipts. */
#define TG_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR 0x2f2f21bfUL
#define TG_DIALOG_CONSTRUCTOR 0xd58a08c6UL
#define TG_DIALOG_FOLDER_CONSTRUCTOR 0x71bd134cUL
#define TG_FOLDER_CONSTRUCTOR 0xff544e65UL
#define TG_CHAT_PHOTO_EMPTY_CONSTRUCTOR 0x37c1011cUL
#define TG_CHAT_PHOTO_CONSTRUCTOR 0x1c6e1c11UL
#define TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR 0x99622c0cUL
#define TG_NOTIFICATION_SOUND_DEFAULT_CONSTRUCTOR 0x97e8bebeUL
#define TG_NOTIFICATION_SOUND_NONE_CONSTRUCTOR 0x6f0c34dfUL
#define TG_NOTIFICATION_SOUND_LOCAL_CONSTRUCTOR 0x830b9ae4UL
#define TG_NOTIFICATION_SOUND_RINGTONE_CONSTRUCTOR 0xff6c8049UL
#define TG_DRAFT_MESSAGE_EMPTY_CONSTRUCTOR 0x1b0c841aUL
#define TG_MSGS_ACK_CONSTRUCTOR 0x62d6b459UL
#define TG_UPDATES_GET_STATE_CONSTRUCTOR 0xedd4882aUL
#define TG_UPDATES_STATE_CONSTRUCTOR 0xa56c2a3eUL
#define TG_UPDATES_GET_DIFFERENCE_CONSTRUCTOR 0x19c2f763UL
#define TG_UPDATES_DIFFERENCE_EMPTY_CONSTRUCTOR 0x5d75a138UL
#define TG_UPDATES_DIFFERENCE_CONSTRUCTOR 0x00f49ca0UL
#define TG_UPDATES_DIFFERENCE_SLICE_CONSTRUCTOR 0xa8fb1981UL
#define TG_UPDATES_DIFFERENCE_TOO_LONG_CONSTRUCTOR 0x4afe8f6dUL
#define TG_VECTOR_CONSTRUCTOR 0x1cb5c415UL
#define TG_RPC_RESULT_CONSTRUCTOR 0xf35c6d01UL
#define TG_RPC_ERROR_CONSTRUCTOR 0x2144ca19UL
#define TG_BAD_MSG_NOTIFICATION_CONSTRUCTOR 0xa7eff811UL
#define TG_BAD_SERVER_SALT_CONSTRUCTOR 0xedab447bUL
#define TG_AUTH_SENT_CODE_CONSTRUCTOR 0x5e002502UL
#define TG_AUTH_SENT_CODE_SUCCESS_CONSTRUCTOR 0x2390fe44UL
#define TG_AUTH_SENT_CODE_PAYMENT_REQUIRED_CONSTRUCTOR 0xd7a2fcf9UL
#define TG_AUTH_AUTHORIZATION_CONSTRUCTOR 0x2ea2c0d4UL
#define TG_AUTH_AUTHORIZATION_SIGNUP_REQUIRED_CONSTRUCTOR 0x44747e9aUL
#define TG_CONFIG_CONSTRUCTOR 0xcc1a241eUL
#define TG_ACCOUNT_PASSWORD_CONSTRUCTOR 0x957b50fbUL
#define TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR 0x3a912d4aUL
#define TG_USER_CONSTRUCTOR 0x020b1422UL
#define TG_CHAT_EMPTY_CONSTRUCTOR 0x29562865UL
#define TG_CHAT_CONSTRUCTOR 0x41cbf256UL
#define TG_CHAT_FORBIDDEN_CONSTRUCTOR 0x6592a1a7UL
#define TG_CHANNEL_CONSTRUCTOR 0xfe685355UL
#define TG_CHANNEL_FORBIDDEN_CONSTRUCTOR 0x17d493d5UL
#define TG_MESSAGE_EMPTY_CONSTRUCTOR 0x90a6ca84UL
#define TG_MESSAGE_CONSTRUCTOR 0x9815cec8UL
#define TG_MESSAGE_SERVICE_CONSTRUCTOR 0x7a800e0aUL
#define TG_MESSAGE_FWD_HEADER_CONSTRUCTOR 0x4e4df4bbUL
#define TG_MESSAGE_REPLY_HEADER_CONSTRUCTOR 0x6917560bUL
#define TG_MESSAGE_REPLIES_CONSTRUCTOR 0x83d60fc2UL
#define TG_MESSAGE_REACTIONS_CONSTRUCTOR 0x0a339f0bUL
#define TG_REACTION_COUNT_CONSTRUCTOR 0xa3d1cb80UL
#define TG_MESSAGES_DIALOGS_CONSTRUCTOR 0x15ba6c40UL
#define TG_MESSAGES_DIALOGS_SLICE_CONSTRUCTOR 0x71e094f3UL
#define TG_MESSAGES_DIALOGS_NOT_MODIFIED_CONSTRUCTOR 0xf0e3e596UL
#define TG_MESSAGES_MESSAGES_CONSTRUCTOR 0x8c718e87UL
#define TG_MESSAGES_MESSAGES_SLICE_CONSTRUCTOR 0x762b263dUL
#define TG_MESSAGES_CHANNEL_MESSAGES_CONSTRUCTOR 0xc776ba4eUL
#define TG_MESSAGES_MESSAGES_NOT_MODIFIED_CONSTRUCTOR 0x74535f21UL
#define TG_CONTACTS_RESOLVED_PEER_CONSTRUCTOR 0x7f077ad9UL
#define TG_CONTACTS_FOUND_CONSTRUCTOR 0xb3134d9dUL
#define TG_UPDATE_SHORT_SENT_MESSAGE_CONSTRUCTOR 0x9015e101UL

static unsigned long tg_read_u32_le(const unsigned char *p)
{
    return ((unsigned long)p[0]) |
           (((unsigned long)p[1]) << 8) |
           (((unsigned long)p[2]) << 16) |
           (((unsigned long)p[3]) << 24);
}

static tg_mtproto_tl_status tg_write_input_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash);

static tg_mtproto_tl_status tg_write_string(tg_mtproto_tl_writer *writer,
                                            const char *text)
{
    if (text == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    return tg_mtproto_tl_write_bytes(writer, (const unsigned char *)text,
                                     (unsigned long)strlen(text));
}

static tg_mtproto_tl_status tg_read_string_copy(tg_mtproto_tl_reader *reader,
                                                char *buffer,
                                                unsigned long buffer_size)
{
    const unsigned char *bytes;
    unsigned long length;
    unsigned long copy_length;
    tg_mtproto_tl_status status;

    if (buffer == 0 || buffer_size == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    buffer[0] = '\0';
    status = tg_mtproto_tl_read_bytes(reader, &bytes, &length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    copy_length = length;
    if (copy_length >= buffer_size) {
        copy_length = buffer_size - 1UL;
    }
    memcpy(buffer, bytes, (size_t)copy_length);
    buffer[copy_length] = '\0';
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_read_bytes_copy(
    tg_mtproto_tl_reader *reader,
    unsigned char *buffer,
    unsigned long buffer_size,
    unsigned long *decoded_length)
{
    const unsigned char *bytes;
    unsigned long length;
    tg_mtproto_tl_status status;

    if (buffer == 0 || decoded_length == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_read_bytes(reader, &bytes, &length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if (length > buffer_size) {
        return TG_MTPROTO_TL_BUFFER_TOO_SMALL;
    }
    if (length > 0UL) {
        memcpy(buffer, bytes, (size_t)length);
    }
    *decoded_length = length;
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_string(tg_mtproto_tl_reader *reader)
{
    const unsigned char *bytes;
    unsigned long length;

    return tg_mtproto_tl_read_bytes(reader, &bytes, &length);
}

static tg_mtproto_tl_status tg_skip_auth_sent_code_type(
    tg_mtproto_tl_reader *reader,
    unsigned long *type_constructor,
    unsigned long *type_length,
    int *has_type_length)
{
    unsigned long constructor;
    unsigned long flags;
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_read_u32(reader, &constructor);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if (type_constructor != 0) {
        *type_constructor = constructor;
    }
    if (type_length != 0) {
        *type_length = 0UL;
    }
    if (has_type_length != 0) {
        *has_type_length = 0;
    }

    switch (constructor) {
    case 0x3dbb5986UL: /* auth.sentCodeTypeApp */
    case 0xc000bba2UL: /* auth.sentCodeTypeSms */
    case 0x5353e5a7UL: /* auth.sentCodeTypeCall */
        status = tg_mtproto_tl_read_u32(reader, &flags);
        if (status == TG_MTPROTO_TL_OK) {
            if (type_length != 0) {
                *type_length = flags;
            }
            if (has_type_length != 0) {
                *has_type_length = 1;
            }
        }
        return status;
    case 0xab03c6d9UL: /* auth.sentCodeTypeFlashCall */
        return tg_skip_string(reader);
    case 0x82006484UL: /* auth.sentCodeTypeMissedCall */
    case 0xd9565c39UL: /* auth.sentCodeTypeFragmentSms */
        status = tg_skip_string(reader);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_read_u32(reader, &flags);
        }
        if (status == TG_MTPROTO_TL_OK) {
            if (type_length != 0) {
                *type_length = flags;
            }
            if (has_type_length != 0) {
                *has_type_length = 1;
            }
        }
        return status;
    case 0xf450f59bUL: /* auth.sentCodeTypeEmailCode */
        status = tg_mtproto_tl_read_u32(reader, &flags);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_skip_string(reader);
        }
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK) {
            if (type_length != 0) {
                *type_length = constructor;
            }
            if (has_type_length != 0) {
                *has_type_length = 1;
            }
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 8UL) != 0UL) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 16UL) != 0UL) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        return status;
    case 0xa5491deaUL: /* auth.sentCodeTypeSetUpEmailRequired */
        return tg_mtproto_tl_read_u32(reader, &flags);
    case 0x009fd736UL: /* auth.sentCodeTypeFirebaseSms */
        status = tg_mtproto_tl_read_u32(reader, &flags);
        if (status == TG_MTPROTO_TL_OK && (flags & 1UL) != 0UL) {
            status = tg_skip_string(reader);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 4UL) != 0UL) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 4UL) != 0UL) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 4UL) != 0UL) {
            status = tg_skip_string(reader);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 2UL) != 0UL) {
            status = tg_skip_string(reader);
        }
        if (status == TG_MTPROTO_TL_OK && (flags & 2UL) != 0UL) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_read_u32(reader, &constructor);
        }
        if (status == TG_MTPROTO_TL_OK) {
            if (type_length != 0) {
                *type_length = constructor;
            }
            if (has_type_length != 0) {
                *has_type_length = 1;
            }
        }
        return status;
    case 0xa416ac81UL: /* auth.sentCodeTypeSmsWord */
    case 0xb37794afUL: /* auth.sentCodeTypeSmsPhrase */
        status = tg_mtproto_tl_read_u32(reader, &flags);
        if (status == TG_MTPROTO_TL_OK && (flags & 1UL) != 0UL) {
            status = tg_skip_string(reader);
        }
        return status;
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

tg_mtproto_tl_status tg_mtproto_build_invoke_with_layer(
    tg_mtproto_tl_writer *writer,
    unsigned long layer,
    const unsigned char *query,
    unsigned long query_length)
{
    tg_mtproto_tl_status status;

    if (query == 0 && query_length > 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_INVOKE_WITH_LAYER_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, layer);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, query, query_length);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_invoke_without_updates(
    tg_mtproto_tl_writer *writer,
    const unsigned char *query,
    unsigned long query_length)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || query == 0 || query_length == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(
        writer, TG_INVOKE_WITHOUT_UPDATES_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, query, query_length);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_init_connection(
    tg_mtproto_tl_writer *writer,
    unsigned long api_id,
    const char *device_model,
    const char *system_version,
    const char *app_version,
    const char *lang_code,
    const unsigned char *query,
    unsigned long query_length)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || device_model == 0 || system_version == 0 ||
        app_version == 0 || lang_code == 0 ||
        (query == 0 && query_length > 0UL) ||
        device_model[0] == '\0' || system_version[0] == '\0' ||
        app_version[0] == '\0' || lang_code[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_INIT_CONNECTION_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, api_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, device_model);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, system_version);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, app_version);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, lang_code);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, "");
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, lang_code);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, query, query_length);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_auth_send_code(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    unsigned long api_id,
    const char *api_hash)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || phone_number == 0 || api_hash == 0 ||
        phone_number[0] == '\0' || api_hash[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_AUTH_SEND_CODE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_number);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, api_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, api_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_CODE_SETTINGS_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_auth_sign_in(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    const char *phone_code_hash,
    const char *phone_code)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || phone_number == 0 || phone_code_hash == 0 ||
        phone_code == 0 || phone_number[0] == '\0' ||
        phone_code_hash[0] == '\0' || phone_code[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_AUTH_SIGN_IN_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_number);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_code_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_code);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_auth_sign_up(
    tg_mtproto_tl_writer *writer,
    const char *phone_number,
    const char *phone_code_hash,
    const char *first_name,
    const char *last_name)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || phone_number == 0 || phone_code_hash == 0 ||
        first_name == 0 || last_name == 0 || phone_number[0] == '\0' ||
        phone_code_hash[0] == '\0' || first_name[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_AUTH_SIGN_UP_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_number);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, phone_code_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, first_name);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, last_name);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_input_check_password_empty(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u32(
        writer, TG_INPUT_CHECK_PASSWORD_EMPTY_CONSTRUCTOR);
}

tg_mtproto_tl_status tg_mtproto_build_input_check_password_srp(
    tg_mtproto_tl_writer *writer,
    unsigned long srp_id_hi,
    unsigned long srp_id_lo,
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char m1[TG_MTPROTO_SHA256_LENGTH])
{
    tg_mtproto_tl_status status;

    if (writer == 0 || a == 0 || m1 == 0 || a_length == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_INPUT_CHECK_PASSWORD_SRP_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, srp_id_hi, srp_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, a, a_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, m1,
                                           TG_MTPROTO_SHA256_LENGTH);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_auth_check_password_empty(
    tg_mtproto_tl_writer *writer)
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_AUTH_CHECK_PASSWORD_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_build_input_check_password_empty(writer);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_auth_check_password_srp(
    tg_mtproto_tl_writer *writer,
    unsigned long srp_id_hi,
    unsigned long srp_id_lo,
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char m1[TG_MTPROTO_SHA256_LENGTH])
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_AUTH_CHECK_PASSWORD_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_build_input_check_password_srp(
            writer, srp_id_hi, srp_id_lo, a, a_length, m1);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_help_get_config(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u32(writer, TG_HELP_GET_CONFIG_CONSTRUCTOR);
}

tg_mtproto_tl_status tg_mtproto_build_account_get_password(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u32(writer, TG_ACCOUNT_GET_PASSWORD_CONSTRUCTOR);
}

tg_mtproto_tl_status tg_mtproto_build_users_get_self(
    tg_mtproto_tl_writer *writer)
{
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_write_u32(writer, TG_USERS_GET_USERS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_USER_SELF_CONSTRUCTOR);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_updates_get_state(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u32(writer, TG_UPDATES_GET_STATE_CONSTRUCTOR);
}

/*
 * updates.getDifference#19c2f763 flags:# pts:int pts_limit:flags.1?int
 * pts_total_limit:flags.0?int date:int qts:int qts_limit:flags.2?int.
 * pts_total_limit caps how much backlog the server returns in one reply --
 * the knob that makes draining viable on a 1KB/s link.
 */
tg_mtproto_tl_status tg_mtproto_build_updates_get_difference(
    tg_mtproto_tl_writer *writer,
    unsigned long pts,
    unsigned long date,
    unsigned long qts,
    unsigned long pts_total_limit)
{
    tg_mtproto_tl_status status;
    unsigned long flags = pts_total_limit != 0UL ? 0x1UL : 0x0UL;

    status = tg_mtproto_tl_write_u32(writer,
                                     TG_UPDATES_GET_DIFFERENCE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, flags);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, pts);
    }
    if (status == TG_MTPROTO_TL_OK && pts_total_limit != 0UL) {
        status = tg_mtproto_tl_write_u32(writer, pts_total_limit);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, date);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, qts);
    }
    return status;
}

/* Parses updates.state#a56c2a3e pts qts date seq unread_count from an
   rpc-result body (constructor already consumed by the result parser). */
tg_mtproto_tl_status tg_mtproto_parse_updates_state(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_updates_state *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long unread;
    tg_mtproto_tl_status status;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor != TG_UPDATES_STATE_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    status = tg_mtproto_tl_read_u32(&reader, &out->pts);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u32(&reader, &out->qts);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u32(&reader, &out->date);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u32(&reader, &out->seq);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u32(&reader, &unread);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_get_dialogs(
    tg_mtproto_tl_writer *writer,
    unsigned long limit)
{
    return tg_mtproto_build_messages_get_dialogs_page(
        writer, limit, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0);
}

tg_mtproto_tl_status tg_mtproto_build_messages_get_dialogs_page(
    tg_mtproto_tl_writer *writer,
    unsigned long limit,
    unsigned long offset_id,
    unsigned long offset_peer_constructor,
    unsigned long offset_peer_id_hi,
    unsigned long offset_peer_id_lo,
    unsigned long offset_access_hash_hi,
    unsigned long offset_access_hash_lo,
    int offset_has_access_hash)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || limit == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_GET_DIALOGS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        /* flags bit1: folder_id present. folder_id 0 = the MAIN folder only,
           so archived chats stay out of the sidebar (0.0.8; archive
           management itself is a future release). Without the flag the
           server returns archived dialogs mixed in. */
        status = tg_mtproto_tl_write_u32(writer, 2UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* folder_id: main */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* offset_date */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, offset_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        if (offset_id == 0UL || offset_peer_constructor == 0UL) {
            status = tg_mtproto_tl_write_u32(writer,
                                             TG_INPUT_PEER_EMPTY_CONSTRUCTOR);
        } else {
            status = tg_write_input_peer(writer, offset_peer_constructor,
                                         offset_peer_id_hi, offset_peer_id_lo,
                                         offset_access_hash_hi,
                                         offset_access_hash_lo,
                                         offset_has_access_hash);
        }
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, 0UL);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_self(
    tg_mtproto_tl_writer *writer,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || limit == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_GET_HISTORY_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_PEER_SELF_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, 0UL);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_contacts_resolve_username(
    tg_mtproto_tl_writer *writer,
    const char *username)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || username == 0 || username[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(
        writer, TG_CONTACTS_RESOLVE_USERNAME_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, username);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_contacts_resolve_username_flags(
    tg_mtproto_tl_writer *writer,
    const char *username)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || username == 0 || username[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(
        writer, TG_CONTACTS_RESOLVE_USERNAME_FLAGS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, username);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_contacts_search(
    tg_mtproto_tl_writer *writer,
    const char *query,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || query == 0 || query[0] == '\0' || limit == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_CONTACTS_SEARCH_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, query);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    return status;
}

static tg_mtproto_tl_status tg_write_input_peer_user(
    tg_mtproto_tl_writer *writer,
    unsigned long user_id_hi,
    unsigned long user_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo)
{
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_write_u32(writer, TG_INPUT_PEER_USER_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, user_id_hi, user_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, access_hash_hi,
                                         access_hash_lo);
    }
    return status;
}

static tg_mtproto_tl_status tg_write_input_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash)
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    switch (peer_constructor) {
    case TG_MTPROTO_PEER_SELF_CONSTRUCTOR:
        /* Saved Messages: inputPeerSelf has no fields -- one constructor and
           every caller (history, send, media, files) works unchanged. */
        return tg_mtproto_tl_write_u32(writer,
                                       TG_INPUT_PEER_SELF_CONSTRUCTOR);
    case TG_PEER_USER_CONSTRUCTOR:
        if (!has_access_hash) {
            return TG_MTPROTO_TL_INVALID_ARGUMENT;
        }
        return tg_write_input_peer_user(writer, peer_id_hi, peer_id_lo,
                                        access_hash_hi, access_hash_lo);
    case TG_PEER_CHAT_CONSTRUCTOR:
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_PEER_CHAT_CONSTRUCTOR);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u64(writer, peer_id_hi, peer_id_lo);
        }
        return status;
    case TG_PEER_CHANNEL_CONSTRUCTOR:
        if (!has_access_hash) {
            return TG_MTPROTO_TL_INVALID_ARGUMENT;
        }
        status = tg_mtproto_tl_write_u32(writer,
                                         TG_INPUT_PEER_CHANNEL_CONSTRUCTOR);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u64(writer, peer_id_hi, peer_id_lo);
        }
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u64(writer, access_hash_hi,
                                             access_hash_lo);
        }
        return status;
    default:
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
}

/* messages.getPeerDialogs#e470bcfd peers:Vector<InputDialogPeer> -- one peer,
   to refresh that chat's read_outbox_max_id (the "seen" cursor) without the
   heavy full getDialogs. Reply is messages.peerDialogs (dialogs vector first). */
tg_mtproto_tl_status tg_mtproto_build_messages_get_peer_dialogs(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash)
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_GET_PEER_DIALOGS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL); /* one InputDialogPeer */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer,
                                         TG_INPUT_DIALOG_PEER_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_peer(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long offset_id,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || limit == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_GET_HISTORY_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* offset_id: with 0 the server pins the newest; the load-older paging
           passes the oldest message currently shown to fetch the page below it. */
        status = tg_mtproto_tl_write_u32(writer, offset_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, 0UL);
    }
    return status;
}

/* channels.getParticipants#77ced9d0 channel:InputChannel
   filter:ChannelParticipantsFilter offset:int limit:int hash:long. Used to resolve
   a supergroup member's id->name for the typing indicator; the reply's
   users:Vector<User> is scanned by the generic tg_mtproto_parse_message_peers. */
tg_mtproto_tl_status tg_mtproto_build_channels_get_participants_recent(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || limit == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_CHANNELS_GET_PARTICIPANTS_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        /* channel: inputChannel#f35aec28 channel_id:long access_hash:long */
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_CHANNEL_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, channel_id_hi, channel_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, access_hash_hi, access_hash_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* filter: channelParticipantsRecent#de3f3c79 (no fields) */
        status = tg_mtproto_tl_write_u32(
            writer, TG_CHANNEL_PARTICIPANTS_RECENT_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* offset:int */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit); /* limit:int */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, 0UL); /* hash:long = 0 */
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_get_history_user(
    tg_mtproto_tl_writer *writer,
    unsigned long user_id_hi,
    unsigned long user_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long limit)
{
    return tg_mtproto_build_messages_get_history_peer(
        writer, TG_PEER_USER_CONSTRUCTOR, user_id_hi, user_id_lo,
        access_hash_hi, access_hash_lo, 1, 0UL, limit);
}

/* inputReplyToMessage#869fbe10 flags:# reply_to_msg_id:int ... (layer 214).
   Plain reply: flags=0, so no top_msg_id/quote/monoforum/todo are serialized. */
static tg_mtproto_tl_status tg_write_input_reply_to_message(
    tg_mtproto_tl_writer *writer, unsigned long reply_to_msg_id)
{
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_write_u32(writer,
                                     TG_INPUT_REPLY_TO_MESSAGE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags = 0 */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, reply_to_msg_id);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_send_self(
    tg_mtproto_tl_writer *writer,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || message == 0 || message[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_SEND_MESSAGE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer,
                                         (reply_to_msg_id != 0UL) ? 1UL : 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_PEER_SELF_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK && reply_to_msg_id != 0UL) {
        status = tg_write_input_reply_to_message(writer, reply_to_msg_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, message);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_messages_send_user(
    tg_mtproto_tl_writer *writer,
    unsigned long user_id_hi,
    unsigned long user_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    const char *message,
    unsigned long reply_to_msg_id,
    unsigned long random_id_hi,
    unsigned long random_id_lo)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || message == 0 || message[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_SEND_MESSAGE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer,
                                         (reply_to_msg_id != 0UL) ? 1UL : 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer_user(writer, user_id_hi, user_id_lo,
                                          access_hash_hi, access_hash_lo);
    }
    if (status == TG_MTPROTO_TL_OK && reply_to_msg_id != 0UL) {
        status = tg_write_input_reply_to_message(writer, reply_to_msg_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, message);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    return status;
}

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
    unsigned long random_id_lo)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || message == 0 || message[0] == '\0') {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_SEND_MESSAGE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer,
                                         (reply_to_msg_id != 0UL) ? 1UL : 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK && reply_to_msg_id != 0UL) {
        status = tg_write_input_reply_to_message(writer, reply_to_msg_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, message);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    return status;
}

/* messages.forwardMessages#978928ca flags:# from_peer:InputPeer
   id:Vector<int> random_id:Vector<long> to_peer:InputPeer ... = Updates.
   The first implementation forwards one message with flags=0; keeping both
   peers generic lets Saved Messages and the destination picker share the same
   wire builder. */
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
    int to_has_access_hash)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || message_id == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(
        writer, TG_MESSAGES_FORWARD_MESSAGES_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(
            writer, from_peer_constructor, from_peer_id_hi, from_peer_id_lo,
            from_access_hash_hi, from_access_hash_lo, from_has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, message_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(
            writer, to_peer_constructor, to_peer_id_hi, to_peer_id_lo,
            to_access_hash_hi, to_access_hash_lo, to_has_access_hash);
    }
    return status;
}

/* messages.editMessage#dfd14005 -- edit an OWN message's text. Minimal text
   edit: flags = message-present (flags.11), peer, id, message. */
tg_mtproto_tl_status tg_mtproto_build_messages_edit_message(
    tg_mtproto_tl_writer *writer,
    unsigned long peer_constructor,
    unsigned long peer_id_hi,
    unsigned long peer_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    int has_access_hash,
    unsigned long message_id,
    const char *message)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || message == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_EDIT_MESSAGE_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer,
                                         TG_MESSAGES_EDIT_MESSAGE_FLAG_MESSAGE);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi, access_hash_lo,
                                     has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, message_id);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, message);
    }
    return status;
}

/* messages.deleteMessages#e58e95d2 -- delete ONE message. revoke = delete for
   everyone (flags.0); 0 = delete only for me. id is a Vector<int> of one. */
tg_mtproto_tl_status tg_mtproto_build_messages_delete_messages(
    tg_mtproto_tl_writer *writer,
    int revoke,
    unsigned long message_id)
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_MESSAGES_DELETE_MESSAGES_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, revoke ? 1UL : 0UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL); /* one id */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, message_id);
    }
    return status;
}

/* channels.deleteMessages#84c1fd4e -- delete ONE message in a channel/supergroup
   (always for everyone). channel:InputChannel id:Vector<int> of one. */
tg_mtproto_tl_status tg_mtproto_build_channels_delete_messages(
    tg_mtproto_tl_writer *writer,
    unsigned long channel_id_hi,
    unsigned long channel_id_lo,
    unsigned long access_hash_hi,
    unsigned long access_hash_lo,
    unsigned long message_id)
{
    tg_mtproto_tl_status status;

    if (writer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer,
                                     TG_CHANNELS_DELETE_MESSAGES_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_INPUT_CHANNEL_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, channel_id_hi, channel_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, access_hash_hi, access_hash_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, message_id);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_msgs_ack(
    tg_mtproto_tl_writer *writer,
    const unsigned long *msg_id_hi,
    const unsigned long *msg_id_lo,
    unsigned long msg_id_count)
{
    unsigned long i;
    tg_mtproto_tl_status status;

    if (writer == 0 || msg_id_count == 0UL || msg_id_hi == 0 ||
        msg_id_lo == 0 || msg_id_count > 8192UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, TG_MSGS_ACK_CONSTRUCTOR);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, TG_VECTOR_CONSTRUCTOR);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, msg_id_count);
    }
    for (i = 0UL; status == TG_MTPROTO_TL_OK && i < msg_id_count; ++i) {
        status = tg_mtproto_tl_write_u64(writer, msg_id_hi[i], msg_id_lo[i]);
    }
    return status;
}

/* upload.getFile#be5335be for a peer's SMALL profile photo, addressed via the
   inputPeerPhotoFileLocation#37257e99 shortcut (NO file_reference field; `big`
   unset = the 160x160 crop). flags=0 on both: no `precise`, no `cdn_supported`
   -- the server then serves the bytes directly and upload.fileCdnRedirect is a
   bail-out, not a requirement. `offset` is a TL LONG (the #1 wire trap: 4 bytes
   here shifts `limit` and yields cryptic failures); `limit` must be a power-of-
   two multiple of 4096 dividing 1 MB. Hashes verified on core.telegram.org and
   cross-checked against TDLib telegram_api.tl; unchanged from layer 214 through
   the live layer. */
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
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || limit == 0UL || (limit % 4096UL) != 0UL ||
        (1048576UL % limit) != 0UL || (offset % 4096UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xbe5335beUL); /* upload.getFile */
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags: plain fetch */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0x37257e99UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags: big unset */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, photo_id_hi, photo_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* offset:long -- avatars are far below 4 GB, so the high word is 0 */
        status = tg_mtproto_tl_write_u64(writer, 0UL, offset);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    return status;
}

/* upload.file#096a18d5 type:storage.FileType mtime:int bytes:bytes. The
   storage.FileType members are bare constructors (no fields). Recognizes
   upload.fileCdnRedirect#f18cda44 explicitly (sets *cdn_redirect, returns OK
   with no bytes) so the caller bails to a fallback instead of misparsing. */
tg_mtproto_tl_status tg_mtproto_parse_upload_file(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    const unsigned char **bytes,
    unsigned long *bytes_length,
    int *cdn_redirect)
{
    tg_mtproto_tl_reader reader;
    unsigned long scratch;

    if (body == 0 || bytes == 0 || bytes_length == 0 || cdn_redirect == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    *bytes = 0;
    *bytes_length = 0UL;
    *cdn_redirect = 0;
    if (constructor == 0xf18cda44UL) { /* upload.fileCdnRedirect */
        *cdn_redirect = 1;
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != 0x096a18d5UL) { /* upload.file */
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA; /* type constructor + mtime */
    }
    return tg_mtproto_tl_read_bytes(&reader, bytes, bytes_length);
}

static tg_mtproto_tl_status tg_read_vector_count(tg_mtproto_tl_reader *reader,
                                                 unsigned long *count);

/* --- F9 file sharing chunk 1: document TL (layer 214, verified) ---------
   Wire shapes cross-checked ON the layer-214 scheme (TDLib @ MTPROTO_LAYER
   214) -- notably messages.sendMedia is #ac55d9c1 there, not the live
   schema's #0330e77f. The Document skips cover every PhotoSize/VideoSize/
   DocumentAttribute variant of that layer so the reader stays wire-exact. */

static tg_mtproto_tl_status tg_skip_bytes_field(tg_mtproto_tl_reader *reader)
{
    const unsigned char *p;
    unsigned long n;

    return tg_mtproto_tl_read_bytes(reader, &p, &n);
}

static tg_mtproto_tl_status tg_skip_u32s(tg_mtproto_tl_reader *reader,
                                         unsigned long count)
{
    unsigned long scratch;

    while (count-- > 0UL) {
        if (tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_int_vector(tg_mtproto_tl_reader *reader)
{
    unsigned long n;

    if (tg_read_vector_count(reader, &n) != TG_MTPROTO_TL_OK || n > 4096UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return tg_skip_u32s(reader, n);
}

static tg_mtproto_tl_status tg_skip_input_sticker_set(
    tg_mtproto_tl_reader *reader)
{
    unsigned long ctor;

    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (ctor) {
    case 0xffb62b95UL: /* inputStickerSetEmpty */
    case 0x028703c8UL: /* inputStickerSetAnimatedEmoji */
        return TG_MTPROTO_TL_OK;
    case 0x9de7a269UL: /* inputStickerSetID: id + access_hash */
        return tg_skip_u32s(reader, 4UL);
    case 0x861cc8a0UL: /* inputStickerSetShortName */
        return tg_skip_string(reader);
    case 0xe67f520eUL: /* inputStickerSetDice: emoticon */
        return tg_skip_string(reader);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_photo_size(tg_mtproto_tl_reader *reader)
{
    unsigned long ctor;

    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK ||
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) { /* every variant: type */
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (ctor) {
    case 0x0e17e23cUL: /* photoSizeEmpty */
        return TG_MTPROTO_TL_OK;
    case 0x75c78e60UL: /* photoSize: w h size */
        return tg_skip_u32s(reader, 3UL);
    case 0x021e1ad6UL: /* photoCachedSize: w h bytes */
        if (tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_bytes_field(reader);
    case 0xe0b0bc2eUL: /* photoStrippedSize: bytes */
    case 0xd8214d41UL: /* photoPathSize: bytes */
        return tg_skip_bytes_field(reader);
    case 0xfa3efb95UL: /* photoSizeProgressive: w h Vector<int> */
        if (tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_int_vector(reader);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_video_size(tg_mtproto_tl_reader *reader);

#ifndef TG_MTPROTO_PHOTO_TARGET_EDGE
#if defined(__m68k__)
#define TG_MTPROTO_PHOTO_TARGET_EDGE 256UL
#else
#define TG_MTPROTO_PHOTO_TARGET_EDGE 800UL
#endif
#endif

#ifndef TG_MTPROTO_PHOTO_VIEWER_TARGET_EDGE
#if defined(__m68k__)
#define TG_MTPROTO_PHOTO_VIEWER_TARGET_EDGE 640UL
#else
#define TG_MTPROTO_PHOTO_VIEWER_TARGET_EDGE 1024UL
#endif
#endif
#ifndef TG_MTPROTO_PHOTO_VIEWER_BYTES_MAX
#if defined(__m68k__)
#define TG_MTPROTO_PHOTO_VIEWER_BYTES_MAX (768UL * 1024UL)
#else
#define TG_MTPROTO_PHOTO_VIEWER_BYTES_MAX (2UL * 1024UL * 1024UL)
#endif
#endif
#ifndef TG_MTPROTO_PHOTO_BYTES_MAX
#if defined(__m68k__)
#define TG_MTPROTO_PHOTO_BYTES_MAX (160UL * 1024UL)
#else
#define TG_MTPROTO_PHOTO_BYTES_MAX (1024UL * 1024UL)
#endif
#endif

/* Reads one PhotoSize and, when it names a downloadable server-side thumb,
   records its dimensions/bytes. Cached/stripped/path variants are consumed but
   deliberately not selected: they are inline helper payloads rather than the
   JPEG file requested through inputPhotoFileLocation. */
static tg_mtproto_tl_status tg_read_photo_size_candidate(
    tg_mtproto_tl_reader *reader, char *type, unsigned long type_size,
    unsigned long *width, unsigned long *height, unsigned long *size,
    int *downloadable, int *progressive, unsigned char *stripped,
    unsigned long stripped_cap, unsigned long *stripped_len)
{
    unsigned long ctor;
    unsigned long count;
    unsigned long i;
    unsigned long value;

    if (type == 0 || type_size == 0UL || width == 0 || height == 0 ||
        size == 0 || downloadable == 0 || progressive == 0 || stripped == 0 ||
        stripped_len == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    type[0] = '\0';
    *width = 0UL;
    *height = 0UL;
    *size = 0UL;
    *downloadable = 0;
    *progressive = 0;
    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK ||
        tg_read_string_copy(reader, type, type_size) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (ctor) {
    case 0x0e17e23cUL: /* photoSizeEmpty */
        return TG_MTPROTO_TL_OK;
    case 0x75c78e60UL: /* photoSize: w h size */
        if (tg_mtproto_tl_read_u32(reader, width) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, height) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, size) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        *downloadable = 1;
        return TG_MTPROTO_TL_OK;
    case 0x021e1ad6UL: /* photoCachedSize: w h bytes */
        if (tg_mtproto_tl_read_u32(reader, width) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, height) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_bytes_field(reader);
    case 0xe0b0bc2eUL: /* photoStrippedSize: bytes */
    {
        const unsigned char *bytes;
        unsigned long bytes_len;

        if (tg_mtproto_tl_read_bytes(reader, &bytes, &bytes_len) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if (*stripped_len == 0UL && bytes_len >= 3UL &&
            bytes_len <= stripped_cap && bytes[0] == 0x01U) {
            memcpy(stripped, bytes, bytes_len);
            *stripped_len = bytes_len;
        }
        return TG_MTPROTO_TL_OK;
    }
    case 0xd8214d41UL: /* photoPathSize: bytes */
        return tg_skip_bytes_field(reader);
    case 0xfa3efb95UL: /* photoSizeProgressive: w h Vector<int> */
        if (tg_mtproto_tl_read_u32(reader, width) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, height) != TG_MTPROTO_TL_OK ||
            tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
            count == 0UL || count > 64UL) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        value = 0UL;
        for (i = 0UL; i < count; ++i) {
            if (tg_mtproto_tl_read_u32(reader, &value) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        }
        *size = value; /* final progressive cut is the complete thumb */
        *downloadable = 1;
        *progressive = 1;
        return TG_MTPROTO_TL_OK;
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static int tg_photo_candidate_better(unsigned long edge, unsigned long best,
                                     unsigned long target)
{
    unsigned long distance;
    unsigned long best_distance;

    distance = edge > target ? edge - target : target - edge;
    best_distance = best > target ? best - target : target - best;
    if (distance != best_distance) {
        return distance < best_distance;
    }
    /* Equal distance: prefer the smaller representation on retro hardware. */
    return edge < best;
}

static int tg_photo_candidate_preferred(unsigned long edge,
                                        unsigned long best,
                                        unsigned long target,
                                        int progressive,
                                        int best_progressive)
{
    if (progressive != best_progressive) {
        return !progressive;
    }
    return tg_photo_candidate_better(edge, best, target);
}

tg_mtproto_tl_status tg_mtproto_read_photo(tg_mtproto_tl_reader *reader,
                                           tg_mtproto_photo_meta *out)
{
    unsigned long ctor;
    unsigned long flags;
    unsigned long scratch;
    const unsigned char *ref;
    unsigned long ref_len;
    unsigned long count;
    unsigned long i;
    unsigned long best_edge;
    unsigned long best_large_edge;
    int best_progressive;
    int best_large_progressive;

    if (reader == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (ctor == 0x2331b22dUL) { /* photoEmpty: id */
        return tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo);
    }
    if (ctor != 0xfb197a65UL ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->access_hash_hi,
                               &out->access_hash_lo) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_bytes(reader, &ref, &ref_len) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
        count > 64UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (ref_len > 0UL && ref_len <= TG_MTPROTO_FILE_REF_MAX) {
        memcpy(out->file_reference, ref, ref_len);
        out->file_reference_len = ref_len;
    }
    best_edge = 0UL;
    best_large_edge = 0UL;
    best_progressive = 0;
    best_large_progressive = 0;
    for (i = 0UL; i < count; ++i) {
        char type[TG_MTPROTO_PHOTO_TYPE_MAX];
        unsigned long width;
        unsigned long height;
        unsigned long size;
        unsigned long edge;
        int downloadable;
        int progressive;

        if (tg_read_photo_size_candidate(reader, type, sizeof(type), &width,
                                         &height, &size, &downloadable,
                                         &progressive,
                                         out->stripped,
                                         sizeof(out->stripped),
                                         &out->stripped_len) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        edge = width > height ? width : height;
        if (!downloadable || type[0] == '\0' || width == 0UL || height == 0UL ||
            width > 8192UL || height > 8192UL || size == 0UL) {
            continue;
        }
        if (out->variant_count < TG_MTPROTO_PHOTO_VARIANT_MAX) {
            tg_mtproto_photo_variant *variant;

            variant = &out->variants[out->variant_count++];
            strcpy(variant->type, type);
            variant->width = width;
            variant->height = height;
            variant->size = size;
            variant->progressive = (unsigned char)(progressive ? 1 : 0);
        }
        if (size <= TG_MTPROTO_PHOTO_BYTES_MAX &&
            (best_edge == 0UL ||
             tg_photo_candidate_preferred(edge, best_edge,
                                          TG_MTPROTO_PHOTO_TARGET_EDGE,
                                          progressive, best_progressive))) {
            strcpy(out->thumb_type, type);
            out->width = width;
            out->height = height;
            out->size = size;
            best_edge = edge;
            best_progressive = progressive;
        }
        if (size <= TG_MTPROTO_PHOTO_VIEWER_BYTES_MAX &&
            (best_large_edge == 0UL ||
             tg_photo_candidate_preferred(
                 edge, best_large_edge, TG_MTPROTO_PHOTO_VIEWER_TARGET_EDGE,
                 progressive, best_large_progressive))) {
            strcpy(out->large_thumb_type, type);
            out->large_width = width;
            out->large_height = height;
            out->large_size = size;
            best_large_edge = edge;
            best_large_progressive = progressive;
        }
    }
    if ((flags & 2UL) != 0UL) { /* video_sizes:Vector<VideoSize> */
        if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
            count > 64UL) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        for (i = 0UL; i < count; ++i) {
            if (tg_skip_video_size(reader) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        }
    }
    if (tg_mtproto_tl_read_u32(reader, &out->dc_id) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->has_photo = out->file_reference_len != 0UL &&
                     out->thumb_type[0] != '\0' && out->size != 0UL;
    out->has_large = out->file_reference_len != 0UL &&
                     out->large_thumb_type[0] != '\0' &&
                     out->large_size != 0UL;
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_video_size(tg_mtproto_tl_reader *reader)
{
    unsigned long ctor;
    unsigned long flags;

    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (ctor) {
    case 0xde33b094UL: /* videoSize: flags type w h size [flags.0 double] */
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_skip_string(reader) != TG_MTPROTO_TL_OK ||
            tg_skip_u32s(reader, 3UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 1UL) != 0UL &&
            tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    case 0xf85c413cUL: /* videoSizeEmojiMarkup: emoji_id + Vector<int> */
        if (tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_int_vector(reader);
    case 0x0da082feUL: /* videoSizeStickerMarkup */
        if (tg_skip_input_sticker_set(reader) != TG_MTPROTO_TL_OK ||
            tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_int_vector(reader);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

/* Priority, never last-one-wins: a GIF arrives as video plus animated, a voice
   note as audio with the voice flag, a sticker as sticker plus imageSize. */
static unsigned char tg_mtproto_document_kind_of(unsigned long attrs)
{
    if ((attrs & TG_MTPROTO_DOC_ATTR_STICKER) != 0UL) {
        return (unsigned char)TG_MTPROTO_DOC_KIND_STICKER;
    }
    if ((attrs & TG_MTPROTO_DOC_ATTR_ANIMATED) != 0UL) {
        return (unsigned char)TG_MTPROTO_DOC_KIND_GIF;
    }
    if ((attrs & TG_MTPROTO_DOC_ATTR_VOICE) != 0UL) {
        return (unsigned char)TG_MTPROTO_DOC_KIND_VOICE;
    }
    if ((attrs & TG_MTPROTO_DOC_ATTR_VIDEO) != 0UL) {
        return (unsigned char)TG_MTPROTO_DOC_KIND_VIDEO;
    }
    if ((attrs & TG_MTPROTO_DOC_ATTR_AUDIO) != 0UL) {
        return (unsigned char)TG_MTPROTO_DOC_KIND_AUDIO;
    }
    return (unsigned char)TG_MTPROTO_DOC_KIND_FILE;
}

/* Whole seconds out of a TL double, without touching the FPU: these lanes run
   from a 68000 with software floats to PPC, and a duration only ever needs the
   integer part. IEEE 754 binary64 is sign, 11 bit exponent, 52 bit mantissa,
   the top 20 bits of which sit in the high word. Anything past 2^21 seconds
   (24 days) is not a real duration, so the shift never leaves that word and
   the low word carries only the fraction we are dropping. */
static unsigned long tg_tl_double_seconds(unsigned long hi, unsigned long lo)
{
    unsigned long exponent = (hi >> 20) & 0x7ffUL;
    unsigned long shift;

    (void)lo;
    if (exponent < 1023UL) {
        return 0UL; /* zero, or a clip shorter than a second */
    }
    shift = exponent - 1023UL;
    if (shift > 20UL) {
        return 0UL; /* absurd: say unknown rather than print a wrapped number */
    }
    return (1UL << shift) | ((hi & 0xfffffUL) >> (20UL - shift));
}

/* Trim a copied UTF-8 string back to a whole codepoint. tg_read_string_copy
   cuts on a byte boundary, and half an emoji is worse than a shorter one. */
static void tg_trim_utf8_tail(char *text)
{
    unsigned long n = (unsigned long)strlen(text);
    unsigned long lead = n;
    unsigned char b;
    unsigned long need;

    while (lead > 0UL &&
           ((unsigned char)text[lead - 1UL] & 0xc0U) == 0x80U) {
        --lead; /* back over continuation bytes */
    }
    if (lead == 0UL) {
        text[0] = '\0'; /* continuations only: nothing whole to keep */
        return;
    }
    --lead;
    b = (unsigned char)text[lead];
    if (b < 0x80U) {
        need = 1UL;
    } else if ((b & 0xe0U) == 0xc0U) {
        need = 2UL;
    } else if ((b & 0xf0U) == 0xe0U) {
        need = 3UL;
    } else if ((b & 0xf8U) == 0xf0U) {
        need = 4UL;
    } else {
        need = 0UL; /* stray byte: not a lead at all */
    }
    if (need == 0UL || lead + need > n) {
        text[lead] = '\0';
    }
}

static tg_mtproto_tl_status tg_read_document_attribute(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_document_meta *out)
{
    unsigned long ctor;
    unsigned long flags;
    unsigned long dur_hi;
    unsigned long dur_lo;

    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (ctor) {
    case 0x15590068UL: /* documentAttributeFilename: THE one we want */
        return tg_read_string_copy(reader, out->file_name,
                                   sizeof(out->file_name));
    case 0x11b58939UL: /* animated: a GIF, paired with a video attribute */
        out->attr_seen |= TG_MTPROTO_DOC_ATTR_ANIMATED;
        return TG_MTPROTO_TL_OK;
    case 0x9801d2f7UL: /* hasStickers */
        return TG_MTPROTO_TL_OK;
    case 0x6c37c15cUL: /* imageSize: w h */
        out->attr_seen |= TG_MTPROTO_DOC_ATTR_IMAGE;
        if (tg_mtproto_tl_read_u32(reader, &out->width) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &out->height) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    case 0x6319d612UL: /* sticker: flags alt stickerset [flags.0 MaskCoords] */
        out->attr_seen |= TG_MTPROTO_DOC_ATTR_STICKER;
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_read_string_copy(reader, out->alt, sizeof(out->alt)) !=
                TG_MTPROTO_TL_OK ||
            tg_skip_input_sticker_set(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        tg_trim_utf8_tail(out->alt);
        if ((flags & 1UL) != 0UL) { /* maskCoords#aed6dbb2: ctor n + 3 double */
            return tg_skip_u32s(reader, 8UL);
        }
        return TG_MTPROTO_TL_OK;
    case 0x43c57c48UL: /* video: flags duration(double) w h [opts] */
        out->attr_seen |= TG_MTPROTO_DOC_ATTR_VIDEO;
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &dur_hi, &dur_lo) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &out->width) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &out->height) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->duration = tg_tl_double_seconds(dur_hi, dur_lo);
        if ((flags & 4UL) != 0UL &&
            tg_skip_u32s(reader, 1UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 16UL) != 0UL &&
            tg_skip_u32s(reader, 2UL) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 32UL) != 0UL &&
            tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    case 0x9852f9c6UL: /* audio: flags duration [title][performer][waveform] */
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &out->duration) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->attr_seen |= ((flags & 1024UL) != 0UL) /* voice:flags.10 */
                              ? TG_MTPROTO_DOC_ATTR_VOICE
                              : TG_MTPROTO_DOC_ATTR_AUDIO;
        if ((flags & 1UL) != 0UL &&
            tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 2UL) != 0UL &&
            tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 4UL) != 0UL &&
            tg_skip_bytes_field(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    case 0xfd149899UL: /* customEmoji: flags alt stickerset */
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return tg_skip_input_sticker_set(reader);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

tg_mtproto_tl_status tg_mtproto_read_document(tg_mtproto_tl_reader *reader,
                                              tg_mtproto_document_meta *out)
{
    unsigned long ctor;
    unsigned long flags;
    unsigned long scratch;
    const unsigned char *ref;
    unsigned long ref_len;
    unsigned long i;
    unsigned long count;

    if (reader == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (ctor == 0x36f8c871UL) { /* documentEmpty: id */
        return tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo);
    }
    if (ctor != 0x8fd4c4d8UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->access_hash_hi,
                               &out->access_hash_lo) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_bytes(reader, &ref, &ref_len) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_read_string_copy(reader, out->mime, sizeof(out->mime)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->size_hi, &out->size_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (ref_len > 0UL && ref_len <= TG_MTPROTO_FILE_REF_MAX) {
        memcpy(out->file_reference, ref, ref_len);
        out->file_reference_len = ref_len; /* truncated ref = useless: keep 0 */
    }
    if ((flags & 1UL) != 0UL) { /* thumbs:Vector<PhotoSize> */
        if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
            count > 64UL) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        for (i = 0UL; i < count; ++i) {
            if (tg_skip_photo_size(reader) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        }
    }
    if ((flags & 2UL) != 0UL) { /* video_thumbs:Vector<VideoSize> */
        if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
            count > 64UL) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        for (i = 0UL; i < count; ++i) {
            if (tg_skip_video_size(reader) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        }
    }
    if (tg_mtproto_tl_read_u32(reader, &out->dc_id) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK ||
        count > 64UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < count; ++i) {
        if (tg_read_document_attribute(reader, out) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    out->kind = tg_mtproto_document_kind_of(out->attr_seen);
    out->has_document = 1;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_build_upload_get_document(
    tg_mtproto_tl_writer *writer,
    const tg_mtproto_document_meta *doc,
    unsigned long offset,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || doc == 0 || !doc->has_document ||
        doc->file_reference_len == 0UL || limit == 0UL ||
        (limit % 4096UL) != 0UL || (1048576UL % limit) != 0UL ||
        (offset % 4096UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xbe5335beUL); /* upload.getFile */
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags */
    }
    if (status == TG_MTPROTO_TL_OK) { /* inputDocumentFileLocation */
        status = tg_mtproto_tl_write_u32(writer, 0xbad07584UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, doc->id_hi, doc->id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, doc->access_hash_hi,
                                         doc->access_hash_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, doc->file_reference,
                                           doc->file_reference_len);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, ""); /* thumb_size: full file */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, offset); /* long */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_upload_get_photo(
    tg_mtproto_tl_writer *writer,
    const tg_mtproto_photo_meta *photo,
    unsigned long offset,
    unsigned long limit)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || photo == 0 || !photo->has_photo ||
        photo->file_reference_len == 0UL || photo->thumb_type[0] == '\0' ||
        limit == 0UL || (limit % 4096UL) != 0UL ||
        (1048576UL % limit) != 0UL || (offset % 4096UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xbe5335beUL); /* upload.getFile */
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0x40181ffeUL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, photo->id_hi, photo->id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, photo->access_hash_hi,
                                         photo->access_hash_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, photo->file_reference,
                                           photo->file_reference_len);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, photo->thumb_type);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, 0UL, offset);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, limit);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_upload_save_file_part(
    tg_mtproto_tl_writer *writer,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long part_index,
    const unsigned char *data,
    unsigned long data_length)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || data == 0 || data_length == 0UL ||
        data_length > 524288UL || part_index > 2999UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xb304a621UL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, file_id_hi, file_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, part_index);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, data, data_length);
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_build_upload_save_big_file_part(
    tg_mtproto_tl_writer *writer,
    unsigned long file_id_hi,
    unsigned long file_id_lo,
    unsigned long part_index,
    unsigned long total_parts,
    const unsigned char *data,
    unsigned long data_length)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || data == 0 || data_length == 0UL ||
        data_length > 524288UL || total_parts == 0UL ||
        total_parts > 8000UL || part_index >= total_parts) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xde7b673dUL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, file_id_hi, file_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, part_index);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, total_parts);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_bytes(writer, data, data_length);
    }
    return status;
}

static tg_mtproto_tl_status tg_build_messages_send_media_document_common(
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
    unsigned long random_id_lo,
    int big_file)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || file_name == 0 || file_name[0] == '\0' ||
        mime_type == 0 || mime_type[0] == '\0' || file_parts == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    /* messages.sendMedia#ac55d9c1 -- the LAYER-214 id (not the live one). */
    status = tg_mtproto_tl_write_u32(writer, 0xac55d9c1UL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* flags */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) { /* inputMediaUploadedDocument */
        status = tg_mtproto_tl_write_u32(writer, 0x037c9330UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 16UL); /* force_file */
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* Files above 10 MB, uploaded with saveBigFilePart, must use
           inputFileBig#fa4f0bb5. Small files keep inputFile#f52ff27f. */
        status = tg_mtproto_tl_write_u32(
            writer, big_file ? 0xfa4f0bb5UL : 0xf52ff27fUL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, file_id_hi, file_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, file_parts);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, file_name);
    }
    if (status == TG_MTPROTO_TL_OK && !big_file) {
        status = tg_write_string(writer, ""); /* md5: server-side optional */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, mime_type);
    }
    if (status == TG_MTPROTO_TL_OK) { /* attributes: Vector(1) filename */
        status = tg_mtproto_tl_write_u32(writer, 0x1cb5c415UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 1UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0x15590068UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, file_name);
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* message: the caption (UTF-8), shown under the attachment. Empty
           when the sender typed none, exactly as before. */
        status = tg_write_string(writer, caption != 0 ? caption : "");
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    return status;
}

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
    unsigned long random_id_lo)
{
    return tg_build_messages_send_media_document_common(
        writer, peer_constructor, peer_id_hi, peer_id_lo, access_hash_hi,
        access_hash_lo, has_access_hash, file_id_hi, file_id_lo, file_parts,
        file_name, mime_type, caption, random_id_hi, random_id_lo, 0);
}

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
    unsigned long random_id_lo)
{
    tg_mtproto_tl_status status;

    if (writer == 0 || file_name == 0 || file_name[0] == '\0' ||
        file_parts == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_write_u32(writer, 0xac55d9c1UL);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* sendMedia flags */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_input_peer(writer, peer_constructor, peer_id_hi,
                                     peer_id_lo, access_hash_hi,
                                     access_hash_lo, has_access_hash);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(
            writer, 0x1e287d04UL); /* inputMediaUploadedPhoto, layer 214 */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0UL); /* media flags */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, 0xf52ff27fUL); /* inputFile */
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, file_id_hi, file_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(writer, file_parts);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, file_name);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_write_string(writer, ""); /* md5: server-side optional */
    }
    if (status == TG_MTPROTO_TL_OK) {
        /* message: the photo caption (UTF-8); empty when none was typed. */
        status = tg_write_string(writer, caption != 0 ? caption : "");
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(writer, random_id_hi, random_id_lo);
    }
    return status;
}

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
    unsigned long random_id_lo)
{
    return tg_build_messages_send_media_document_common(
        writer, peer_constructor, peer_id_hi, peer_id_lo, access_hash_hi,
        access_hash_lo, has_access_hash, file_id_hi, file_id_lo, file_parts,
        file_name, mime_type, caption, random_id_hi, random_id_lo, 1);
}

tg_mtproto_tl_status tg_mtproto_parse_rpc_result(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_rpc_result *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long constructor;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_RPC_RESULT_CONSTRUCTOR ||
        tg_mtproto_tl_read_u64(&reader, &out->request_msg_id_hi,
                               &out->request_msg_id_lo) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->result_constructor) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->result_body = body + reader.offset;
    out->result_body_length = body_length - reader.offset;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_rpc_error(
    const unsigned char *body,
    unsigned long body_length,
    long *error_code,
    char *error_message,
    unsigned long error_message_size)
{
    tg_mtproto_tl_reader reader;
    const unsigned char *message;
    unsigned long constructor;
    unsigned long code;
    unsigned long message_length;
    unsigned long copy_length;

    if (body == 0 || error_code == 0 || error_message == 0 ||
        error_message_size == 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    error_message[0] = '\0';
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_RPC_ERROR_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(&reader, &code) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_bytes(&reader, &message, &message_length) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    *error_code = (long)code;
    copy_length = message_length;
    if (copy_length >= error_message_size) {
        copy_length = error_message_size - 1UL;
    }
    memcpy(error_message, message, (size_t)copy_length);
    error_message[copy_length] = '\0';
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_bad_msg_notification(
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_bad_msg_notification *out)
{
    tg_mtproto_tl_reader reader;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &out->constructor) !=
            TG_MTPROTO_TL_OK ||
        (out->constructor != TG_BAD_MSG_NOTIFICATION_CONSTRUCTOR &&
         out->constructor != TG_BAD_SERVER_SALT_CONSTRUCTOR) ||
        tg_mtproto_tl_read_u64(&reader, &out->bad_msg_id_hi,
                               &out->bad_msg_id_lo) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->bad_msg_seqno) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->error_code) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (out->constructor == TG_BAD_SERVER_SALT_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u64(&reader, &out->new_server_salt_hi,
                                   &out->new_server_salt_lo) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->has_new_server_salt = 1;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_auth_sent_code(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_sent_code *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long flags;
    unsigned long unused;
    tg_mtproto_tl_status status;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->constructor = constructor;
    tg_mtproto_tl_reader_init(&reader, body, body_length);

    if (constructor == TG_AUTH_SENT_CODE_PAYMENT_REQUIRED_CONSTRUCTOR) {
        status = tg_skip_string(&reader);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_read_string_copy(&reader, out->phone_code_hash,
                                         sizeof(out->phone_code_hash));
        }
        return status;
    }
    if (constructor == TG_AUTH_SENT_CODE_SUCCESS_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != TG_AUTH_SENT_CODE_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    status = tg_mtproto_tl_read_u32(&reader, &flags);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_skip_auth_sent_code_type(&reader, &out->type_constructor,
                                             &out->type_length,
                                             &out->has_type_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_read_string_copy(&reader, out->phone_code_hash,
                                     sizeof(out->phone_code_hash));
    }
    if (status == TG_MTPROTO_TL_OK && (flags & 2UL) != 0UL) {
        status = tg_mtproto_tl_read_u32(&reader, &unused);
    }
    if (status == TG_MTPROTO_TL_OK && (flags & 4UL) != 0UL) {
        status = tg_mtproto_tl_read_u32(&reader, &out->timeout);
        out->has_timeout = 1;
    }
    return status;
}

tg_mtproto_tl_status tg_mtproto_parse_config_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_config_summary *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long flags;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (constructor != TG_CONFIG_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->date) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->expires) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->test_mode_constructor) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->this_dc) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_account_password_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_password_summary *out)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_tl_status status;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (constructor != TG_ACCOUNT_PASSWORD_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &out->flags) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->has_recovery = (out->flags & 1UL) != 0UL;
    out->has_secure_values = (out->flags & 2UL) != 0UL;
    out->has_password = (out->flags & 4UL) != 0UL;
    if (!out->has_password) {
        return TG_MTPROTO_TL_OK;
    }
    if (tg_mtproto_tl_read_u32(&reader, &out->current_algo_constructor) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->has_current_algo = 1;
    if (out->current_algo_constructor != TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    status = tg_read_bytes_copy(&reader, out->current_salt1,
                                sizeof(out->current_salt1),
                                &out->current_salt1_length);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_read_bytes_copy(&reader, out->current_salt2,
                                    sizeof(out->current_salt2),
                                    &out->current_salt2_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u32(&reader, &out->current_g);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_read_bytes_copy(&reader, out->current_p,
                                    sizeof(out->current_p),
                                    &out->current_p_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_read_bytes_copy(&reader, out->srp_b, sizeof(out->srp_b),
                                    &out->srp_b_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_read_u64(&reader, &out->srp_id_hi,
                                        &out->srp_id_lo);
    }
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_user_vector_first(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_user_summary *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long count;
    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (constructor != TG_VECTOR_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &count) != TG_MTPROTO_TL_OK ||
        count == 0UL ||
        tg_mtproto_tl_read_u32(&reader, &out->constructor) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (out->constructor != TG_USER_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(&reader, &out->flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->flags2) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(&reader, &out->id_hi, &out->id_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->is_self = (out->flags & (1UL << 10)) != 0UL;
    out->is_bot = (out->flags & (1UL << 14)) != 0UL;
    if ((out->flags & 1UL) != 0UL &&
        tg_mtproto_tl_read_u64(&reader, &out->access_hash_hi,
                               &out->access_hash_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 1UL) != 0UL) {
        out->has_access_hash = 1;
    }
    if ((out->flags & 2UL) != 0UL &&
        tg_read_string_copy(&reader, out->first_name,
                            sizeof(out->first_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 4UL) != 0UL &&
        tg_read_string_copy(&reader, out->last_name,
                            sizeof(out->last_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 8UL) != 0UL &&
        tg_read_string_copy(&reader, out->username,
                            sizeof(out->username)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 16UL) != 0UL &&
        tg_read_string_copy(&reader, out->phone,
                            sizeof(out->phone)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_read_vector_count(tg_mtproto_tl_reader *reader,
                                                 unsigned long *count)
{
    unsigned long vector_constructor;

    if (tg_mtproto_tl_read_u32(reader, &vector_constructor) !=
            TG_MTPROTO_TL_OK ||
        vector_constructor != TG_VECTOR_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_bool(tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor != 0xbc799737UL && constructor != 0x997275b5UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_notification_sound(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long scratch_hi;
    unsigned long scratch_lo;
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_read_u32(reader, &constructor);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    switch (constructor) {
    case TG_NOTIFICATION_SOUND_DEFAULT_CONSTRUCTOR:
    case TG_NOTIFICATION_SOUND_NONE_CONSTRUCTOR:
        return TG_MTPROTO_TL_OK;
    case TG_NOTIFICATION_SOUND_LOCAL_CONSTRUCTOR:
        status = tg_skip_string(reader);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_skip_string(reader);
        }
        return status;
    case TG_NOTIFICATION_SOUND_RINGTONE_CONSTRUCTOR:
        return tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_peer_notify_settings(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;
    unsigned long bit;
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_read_u32(reader, &constructor);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if (constructor != TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    status = tg_mtproto_tl_read_u32(reader, &flags);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if ((flags & 1UL) != 0UL && tg_skip_bool(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL && tg_skip_bool(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (bit = 8UL; bit <= 32UL; bit <<= 1) {
        if ((flags & bit) != 0UL &&
            tg_skip_notification_sound(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    if ((flags & 64UL) != 0UL && tg_skip_bool(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 128UL) != 0UL && tg_skip_bool(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (bit = 256UL; bit <= 1024UL; bit <<= 1) {
        if ((flags & bit) != 0UL &&
            tg_skip_notification_sound(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_draft_message(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor != TG_DRAFT_MESSAGE_EMPTY_CONSTRUCTOR) {
        /* A real draftMessage#3fccf7ef carries reply_to/entities/media that are
           expensive (and risky) to skip field-by-field. Signal the caller so it
           can stop walking the dialog vector gracefully and fall back to the
           whole-body peer scan instead of corrupting the read offset. */
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    /* draftMessageEmpty#1b0c841a flags:# date:flags.0?int. Telegram now sends
       the "empty" draft with a flags word and an optional clear date, so the
       old zero-field skip desynced every dialog that had ever held a draft. */
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 1UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_read_peer_ref(tg_mtproto_tl_reader *reader,
                                             tg_mtproto_dialog_peer *peer)
{
    if (peer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (tg_mtproto_tl_read_u32(reader, &peer->peer_constructor) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (peer->peer_constructor) {
    case TG_PEER_USER_CONSTRUCTOR:
    case TG_PEER_CHAT_CONSTRUCTOR:
    case TG_PEER_CHANNEL_CONSTRUCTOR:
        return tg_mtproto_tl_read_u64(reader, &peer->id_hi, &peer->id_lo);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_chat_photo(tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch_hi;
    unsigned long scratch_lo;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_CHAT_PHOTO_EMPTY_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != TG_CHAT_PHOTO_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    /* chatPhoto#1c6e1c11 flags:# has_video:flags.0?true photo_id:long
       stripped_thumb:flags.1?bytes dc_id:int */
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL && tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return tg_mtproto_tl_read_u32(reader, &scratch_lo);
}

static tg_mtproto_tl_status tg_skip_folder(tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;

    /* folder#ff544e65 flags:# autofill_new_broadcasts:flags.0?true
       exclude_pinned:flags.1?true emoticon:flags.3?string id:int title:string
       photo:flags.2?ChatPhoto */
    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_FOLDER_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 8UL) != 0UL && tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_skip_chat_photo(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_read_dialog_peer(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_dialog_peer *peer)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_DIALOG_FOLDER_CONSTRUCTOR) {
        /* dialogFolder#71bd134c folder:Folder peer:Peer top_message:int
           unread_muted_peers_count:int unread_unmuted_peers_count:int
           unread_muted_messages_count:int unread_unmuted_messages_count:int.
           This is the Archived-chats container, not a selectable chat: consume
           its bytes and leave peer->peer_constructor 0 so callers skip it while
           the real dialogs that follow it still parse. */
        tg_mtproto_dialog_peer folder_peer;
        memset(&folder_peer, 0, sizeof(folder_peer));
        if (tg_skip_folder(reader) != TG_MTPROTO_TL_OK ||
            tg_read_peer_ref(reader, &folder_peer) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        peer->peer_constructor = 0UL;
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != TG_DIALOG_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_read_peer_ref(reader, peer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &peer->top_message) !=
            TG_MTPROTO_TL_OK ||
        /* read_inbox_max_id (skipped) then read_outbox_max_id (captured). */
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &peer->read_outbox_max_id) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &peer->unread_count) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_skip_peer_notify_settings(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 1UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL &&
        tg_skip_draft_message(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 16UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 32UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

const char *tg_mtproto_peer_constructor_name(unsigned long constructor)
{
    switch (constructor) {
    case TG_PEER_USER_CONSTRUCTOR:
        return "user";
    case TG_PEER_CHAT_CONSTRUCTOR:
        return "chat";
    case TG_PEER_CHANNEL_CONSTRUCTOR:
        return "channel";
    default:
        return "unknown";
    }
}

tg_mtproto_tl_status tg_mtproto_parse_dialogs_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_dialogs_summary *out)
{
    tg_mtproto_tl_reader reader;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->constructor = constructor;
    tg_mtproto_tl_reader_init(&reader, body, body_length);

    if (constructor == TG_MESSAGES_DIALOGS_NOT_MODIFIED_CONSTRUCTOR) {
        out->is_not_modified = 1;
        return tg_mtproto_tl_read_u32(&reader, &out->count);
    }
    if (constructor == TG_MESSAGES_DIALOGS_SLICE_CONSTRUCTOR) {
        out->is_slice = 1;
        if (tg_mtproto_tl_read_u32(&reader, &out->count) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor != TG_MESSAGES_DIALOGS_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(&reader, &out->dialog_count) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (out->dialog_count != 0UL &&
        (reader.length - reader.offset < 4UL ||
         tg_read_u32_le(reader.buffer + reader.offset) != TG_VECTOR_CONSTRUCTOR)) {
        return TG_MTPROTO_TL_OK;
    }
    if (tg_read_vector_count(&reader, &out->message_count) !=
            TG_MTPROTO_TL_OK ||
        tg_read_vector_count(&reader, &out->chat_count) != TG_MTPROTO_TL_OK ||
        tg_read_vector_count(&reader, &out->user_count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_dialog_peer_list(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_dialog_peer_list *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long i;
    unsigned long count;
    tg_mtproto_dialog_peer peer;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (constructor == TG_MESSAGES_DIALOGS_NOT_MODIFIED_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (constructor == TG_MESSAGES_DIALOGS_SLICE_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(&reader, &out->total_dialog_count) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor != TG_MESSAGES_DIALOGS_CONSTRUCTOR &&
               constructor != TG_MESSAGES_PEER_DIALOGS_CONSTRUCTOR) {
        /* messages.peerDialogs#3371c354 leads with the same dialogs:Vector
           (no slice count), so it parses through this path; the trailing
           messages/chats/users/state are left unread. */
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor != TG_MESSAGES_DIALOGS_SLICE_CONSTRUCTOR) {
        out->total_dialog_count = count;
    }
    for (i = 0UL; i < count; ++i) {
        memset(&peer, 0, sizeof(peer));
        if (tg_read_dialog_peer(&reader, &peer) != TG_MTPROTO_TL_OK) {
            break;  /* unparseable dialog: keep what we parsed so far */
        }
        if (peer.peer_constructor == 0UL) {
            continue;  /* dialogFolder container, not a real peer */
        }
        if (out->count < TG_MTPROTO_DIALOG_PEER_MAX) {
            out->peers[out->count] = peer;
            ++out->count;
        } else {
            out->truncated = 1;
        }
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_peer_cache_entry *tg_peer_cache_find(
    tg_mtproto_peer_cache *cache,
    unsigned long peer_constructor,
    unsigned long id_hi,
    unsigned long id_lo)
{
    unsigned long i;

    for (i = 0UL; i < cache->count; ++i) {
        if (cache->entries[i].peer_constructor == peer_constructor &&
            cache->entries[i].id_hi == id_hi &&
            cache->entries[i].id_lo == id_lo) {
            return &cache->entries[i];
        }
    }
    return 0;
}

static tg_mtproto_peer_cache_entry *tg_peer_cache_add(
    tg_mtproto_peer_cache *cache,
    unsigned long peer_constructor,
    unsigned long id_hi,
    unsigned long id_lo)
{
    tg_mtproto_peer_cache_entry *entry;

    entry = tg_peer_cache_find(cache, peer_constructor, id_hi, id_lo);
    if (entry != 0) {
        return entry;
    }
    if (cache->count >= TG_MTPROTO_PEER_CACHE_MAX) {
        cache->truncated = 1;
        return 0;
    }
    entry = &cache->entries[cache->count++];
    memset(entry, 0, sizeof(*entry));
    entry->peer_constructor = peer_constructor;
    entry->id_hi = id_hi;
    entry->id_lo = id_lo;
    return entry;
}

static void tg_peer_cache_copy_text(char *dst,
                                    unsigned long dst_size,
                                    const char *src)
{
    unsigned long i;

    if (dst == 0 || dst_size == 0UL) {
        return;
    }
    dst[0] = '\0';
    if (src == 0) {
        return;
    }
    for (i = 0UL; i + 1UL < dst_size && src[i] != '\0'; ++i) {
        if (src[i] == '\r' || src[i] == '\n' || src[i] == '\t') {
            dst[i] = ' ';
        } else {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static void tg_peer_cache_copy_user_title(tg_mtproto_peer_cache_entry *entry,
                                          const tg_mtproto_user_summary *user)
{
    unsigned long pos;
    unsigned long i;

    if (entry == 0 || user == 0) {
        return;
    }
    entry->title[0] = '\0';
    pos = 0UL;
    for (i = 0UL; user->first_name[i] != '\0' &&
         pos + 1UL < sizeof(entry->title); ++i) {
        entry->title[pos++] = user->first_name[i];
    }
    if (pos > 0UL && user->last_name[0] != '\0' &&
        pos + 1UL < sizeof(entry->title)) {
        entry->title[pos++] = ' ';
    }
    for (i = 0UL; user->last_name[i] != '\0' &&
         pos + 1UL < sizeof(entry->title); ++i) {
        entry->title[pos++] = user->last_name[i];
    }
    entry->title[pos] = '\0';
    if (entry->title[0] == '\0') {
        tg_peer_cache_copy_text(entry->title, sizeof(entry->title),
                                user->username);
    }
}

/* userProfilePhoto#82d1f706 flags:# has_video:flags.0?true personal:flags.2?true
   photo_id:long stripped_thumb:flags.1?bytes dc_id:int (empty variant
   userProfilePhotoEmpty#4f11bae1). Was a pure skip; now CAPTURES photo_id/dc_id
   and the inline stripped thumb into the user summary (avatar v1: the thumb
   alone is enough to draw a small offline avatar; hashes re-verified on
   core.telegram.org, unchanged through the live layer). */
static tg_mtproto_tl_status tg_read_user_profile_photo(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_user_summary *out)
{
    unsigned long constructor;
    unsigned long flags;
    const unsigned char *thumb;
    unsigned long thumb_len;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == 0x4f11bae1UL) {
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != 0x82d1f706UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->photo_id_hi,
                               &out->photo_id_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL) {
        if (tg_mtproto_tl_read_bytes(reader, &thumb, &thumb_len) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        /* Keep only thumbs that fit whole: truncated JPEG data is useless. */
        if (thumb_len > 0UL && thumb_len <= TG_MTPROTO_STRIPPED_MAX) {
            memcpy(out->stripped, thumb, thumb_len);
            out->stripped_len = thumb_len;
        }
    }
    return tg_mtproto_tl_read_u32(reader, &out->photo_dc_id);
}

static tg_mtproto_tl_status tg_skip_user_status(tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long scratch;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (constructor) {
    case 0x09d05049UL:
    case 0xe26f42f1UL:
    case 0x07bf09fcUL:
    case 0x77ebc742UL:
        return TG_MTPROTO_TL_OK;
    case 0xedb93949UL:
    case 0x008c703fUL:
        return tg_mtproto_tl_read_u32(reader, &scratch);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_restriction_reason_vector(
    tg_mtproto_tl_reader *reader)
{
    unsigned long count;
    unsigned long i;
    unsigned long constructor;

    if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < count; ++i) {
        if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
            constructor != 0xd072acb4UL ||
            tg_skip_string(reader) != TG_MTPROTO_TL_OK ||
            tg_skip_string(reader) != TG_MTPROTO_TL_OK ||
            tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    return TG_MTPROTO_TL_OK;
}

static void tg_avatar_store_put(unsigned long id_hi, unsigned long id_lo,
                                const unsigned char *thumb, unsigned long len,
                                unsigned long photo_id_hi,
                                unsigned long photo_id_lo,
                                unsigned long dc_id);

static tg_mtproto_tl_status tg_read_user_summary_from_reader(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_user_summary *out)
{
    memset(out, 0, sizeof(*out));
    if (tg_mtproto_tl_read_u32(reader, &out->constructor) !=
            TG_MTPROTO_TL_OK ||
        out->constructor != TG_USER_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &out->flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &out->flags2) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->is_self = (out->flags & (1UL << 10)) != 0UL;
    out->is_bot = (out->flags & (1UL << 14)) != 0UL;
    if ((out->flags & 1UL) != 0UL) {
        if (tg_mtproto_tl_read_u64(reader, &out->access_hash_hi,
                                   &out->access_hash_lo) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->has_access_hash = 1;
    }
    if ((out->flags & 2UL) != 0UL &&
        tg_read_string_copy(reader, out->first_name,
                            sizeof(out->first_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 4UL) != 0UL &&
        tg_read_string_copy(reader, out->last_name,
                            sizeof(out->last_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 8UL) != 0UL &&
        tg_read_string_copy(reader, out->username,
                            sizeof(out->username)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 16UL) != 0UL &&
        tg_read_string_copy(reader, out->phone,
                            sizeof(out->phone)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 32UL) != 0UL &&
        tg_read_user_profile_photo(reader, out) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 64UL) != 0UL &&
        tg_skip_user_status(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & (1UL << 18)) != 0UL &&
        tg_skip_restriction_reason_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & (1UL << 19)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & (1UL << 22)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    /* Feed the avatar store at READ level so EVERY user scan captures --
       the dialogs parser goes through tg_peer_cache_apply_user, not
       update_user, and the harvest was silently missing all DMs. */
    if (out->stripped_len > 0UL) {
        tg_avatar_store_put(out->id_hi, out->id_lo, out->stripped,
                            out->stripped_len, out->photo_id_hi,
                            out->photo_id_lo, out->photo_dc_id);
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_read_user_leading_from_reader(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_user_summary *out)
{
    memset(out, 0, sizeof(*out));
    if (tg_mtproto_tl_read_u32(reader, &out->constructor) !=
            TG_MTPROTO_TL_OK ||
        out->constructor != TG_USER_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &out->flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &out->flags2) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &out->id_hi, &out->id_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->is_self = (out->flags & (1UL << 10)) != 0UL;
    out->is_bot = (out->flags & (1UL << 14)) != 0UL;
    if ((out->flags & 1UL) != 0UL) {
        if (tg_mtproto_tl_read_u64(reader, &out->access_hash_hi,
                                   &out->access_hash_lo) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->has_access_hash = 1;
    }
    if ((out->flags & 2UL) != 0UL &&
        tg_read_string_copy(reader, out->first_name,
                            sizeof(out->first_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 4UL) != 0UL &&
        tg_read_string_copy(reader, out->last_name,
                            sizeof(out->last_name)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 8UL) != 0UL &&
        tg_read_string_copy(reader, out->username,
                            sizeof(out->username)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((out->flags & 16UL) != 0UL &&
        tg_read_string_copy(reader, out->phone,
                            sizeof(out->phone)) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    /* photo:flags.5?UserProfilePhoto comes right after phone. Capture it
       BEST-EFFORT: this is the reader every production user scan goes
       through (dialogs, history, search -- via tg_peer_cache_scan_users),
       and it used to stop here, which is why user avatars never captured
       on the real wire while chats did. The scan resyncs on the next
       marker regardless, so a photo parse hiccup must not drop the user
       whose leading fields we already read. */
    if ((out->flags & 32UL) != 0UL) {
        (void)tg_read_user_profile_photo(reader, out);
    }
    return TG_MTPROTO_TL_OK;
}

static int tg_peer_cache_apply_user(tg_mtproto_peer_cache *cache,
                                    const tg_mtproto_user_summary *user)
{
    tg_mtproto_peer_cache_entry *entry;

    entry = tg_peer_cache_find(cache, TG_PEER_USER_CONSTRUCTOR,
                               user->id_hi, user->id_lo);
    if (entry == 0) {
        if (!cache->collect_all) {
            return 0;
        }
        entry = tg_peer_cache_add(cache, TG_PEER_USER_CONSTRUCTOR,
                                  user->id_hi, user->id_lo);
        if (entry == 0) {
            return 0;
        }
        entry->from_dialog = 1;
    } else if (!entry->from_dialog && !cache->collect_all) {
        return 0;
    }
    entry->has_access_hash = user->has_access_hash;
    entry->access_hash_hi = user->access_hash_hi;
    entry->access_hash_lo = user->access_hash_lo;
    entry->is_self = user->is_self;
    entry->is_bot = user->is_bot;
    tg_peer_cache_copy_user_title(entry, user);
    tg_peer_cache_copy_text(entry->username, sizeof(entry->username),
                            user->username);
    /* Avatar v1: adopt the freshly parsed photo id/dc + stripped thumb; only
       overwrite when this parse actually carried a photo, so a partial user
       object does not wipe a thumb captured moments earlier. */
    if (user->photo_id_hi != 0UL || user->photo_id_lo != 0UL) {
        entry->photo_id_hi = user->photo_id_hi;
        entry->photo_id_lo = user->photo_id_lo;
        entry->photo_dc_id = user->photo_dc_id;
        if (user->stripped_len > 0UL) {
            memcpy(entry->stripped, user->stripped, user->stripped_len);
            entry->stripped_len = user->stripped_len;
            tg_avatar_store_put(user->id_hi, user->id_lo, user->stripped,
                                user->stripped_len, user->photo_id_hi,
                                user->photo_id_lo, user->photo_dc_id);
        }
    }
    return 1;
}

static unsigned long tg_peer_cache_scan_users(const unsigned char *body,
                                              unsigned long body_length,
                                              tg_mtproto_peer_cache *cache)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_user_summary user;
    unsigned long offset;
    unsigned long updated;

    if (body == 0 || cache == 0 || body_length < 20UL) {
        return 0UL;
    }
    updated = 0UL;
    for (offset = 0UL; offset + 20UL <= body_length; offset += 4UL) {
        if (tg_read_u32_le(body + offset) != TG_USER_CONSTRUCTOR) {
            continue;
        }
        tg_mtproto_tl_reader_init(&reader, body + offset,
                                  body_length - offset);
        if (tg_read_user_leading_from_reader(&reader, &user) ==
                TG_MTPROTO_TL_OK &&
            tg_peer_cache_apply_user(cache, &user)) {
            ++updated;
        }
    }
    return updated;
}

static int tg_peer_cache_apply_chat(tg_mtproto_peer_cache *cache,
                                    unsigned long peer_constructor,
                                    unsigned long id_hi,
                                    unsigned long id_lo,
                                    unsigned long access_hash_hi,
                                    unsigned long access_hash_lo,
                                    int has_access_hash,
                                    const char *title,
                                    const char *username)
{
    tg_mtproto_peer_cache_entry *entry;

    entry = tg_peer_cache_find(cache, peer_constructor, id_hi, id_lo);
    if (entry == 0) {
        if (!cache->collect_all) {
            return 0;
        }
        entry = tg_peer_cache_add(cache, peer_constructor, id_hi, id_lo);
        if (entry == 0) {
            return 0;
        }
        entry->from_dialog = 1;
    } else if (!entry->from_dialog && !cache->collect_all) {
        return 0;
    }
    entry->has_access_hash = has_access_hash;
    entry->access_hash_hi = access_hash_hi;
    entry->access_hash_lo = access_hash_lo;
    tg_peer_cache_copy_text(entry->title, sizeof(entry->title), title);
    tg_peer_cache_copy_text(entry->username, sizeof(entry->username),
                            username);
    return 1;
}

/* Global avatar-thumb store: the dialog scanners run on function-local static
   caches the GUI cannot reach, so captured stripped thumbs ALSO land here,
   keyed by peer id -- the stable key the sidebar rows already carry
   (chat->peer_id_hi/lo). Fixed table, same-id updates reuse their slot,
   round-robin eviction when full. ~8 KB static. */
/* Sized to hold the WHOLE sidebar plus the transient captures that share the
   store (group members, message senders): with only 64 slots the round-robin
   eviction let a busy group's members push the sidebar chats' thumbs out
   before the at-close save, so previews went missing across restarts.
   ~152 bytes per slot. */
#if defined(__m68k__)
#define TG_AVATAR_STORE_MAX 128 /* ~19 KB static */
#else
#define TG_AVATAR_STORE_MAX 256 /* ~38 KB static */
#endif
typedef struct tg_avatar_slot {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long photo_id_hi; /* for the v2 getFile + disk-cache key */
    unsigned long photo_id_lo;
    unsigned long dc_id;
    unsigned long len; /* 0 = free slot */
    unsigned char thumb[TG_MTPROTO_STRIPPED_MAX];
} tg_avatar_slot;
static tg_avatar_slot tg_avatar_store[TG_AVATAR_STORE_MAX];
static unsigned long tg_avatar_store_next = 0UL;
/* Bumped on every store_put: lets the renderer's negative cache know when a
   retry is worth it (new thumbs arrived) instead of re-probing disk+store on
   every repaint -- that per-repaint fopen() was crushing OS3. */
static unsigned long tg_avatar_store_gen = 1UL;

unsigned long tg_mtproto_avatar_store_generation(void)
{
    return tg_avatar_store_gen;
}

static void tg_avatar_store_put(unsigned long id_hi, unsigned long id_lo,
                                const unsigned char *thumb, unsigned long len,
                                unsigned long photo_id_hi,
                                unsigned long photo_id_lo,
                                unsigned long dc_id)
{
    unsigned long i;
    tg_avatar_slot *slot = 0;

    if (thumb == 0 || len == 0UL || len > TG_MTPROTO_STRIPPED_MAX) {
        return;
    }
    for (i = 0UL; i < TG_AVATAR_STORE_MAX; ++i) {
        if (tg_avatar_store[i].len != 0UL &&
            tg_avatar_store[i].id_hi == id_hi &&
            tg_avatar_store[i].id_lo == id_lo) {
            slot = &tg_avatar_store[i];
            break;
        }
        if (slot == 0 && tg_avatar_store[i].len == 0UL) {
            slot = &tg_avatar_store[i];
        }
    }
    if (slot == 0) {
        slot = &tg_avatar_store[tg_avatar_store_next % TG_AVATAR_STORE_MAX];
        ++tg_avatar_store_next;
    }
    ++tg_avatar_store_gen;
    slot->id_hi = id_hi;
    slot->id_lo = id_lo;
    slot->photo_id_hi = photo_id_hi;
    slot->photo_id_lo = photo_id_lo;
    slot->dc_id = dc_id;
    memcpy(slot->thumb, thumb, len);
    slot->len = len;
}

int tg_mtproto_avatar_meta_lookup(unsigned long id_hi, unsigned long id_lo,
                                  unsigned long *photo_id_hi,
                                  unsigned long *photo_id_lo,
                                  unsigned long *dc_id)
{
    unsigned long i;

    for (i = 0UL; i < TG_AVATAR_STORE_MAX; ++i) {
        if (tg_avatar_store[i].len != 0UL &&
            tg_avatar_store[i].id_hi == id_hi &&
            tg_avatar_store[i].id_lo == id_lo) {
            *photo_id_hi = tg_avatar_store[i].photo_id_hi;
            *photo_id_lo = tg_avatar_store[i].photo_id_lo;
            *dc_id = tg_avatar_store[i].dc_id;
            return 1;
        }
    }
    return 0;
}

/* Persistence for the thumb store (data/telegram-thumbs.bin): a raw dump of
   the slot array, machine-local by nature (never travels between systems, so
   layout/endianness stay the compiler's own). The version byte invalidates
   the cache when the slot struct changes -- worst case the previews simply
   re-capture. Blurred previews thus survive client restarts, matching the
   crisp on-disk photos. */
#define TG_AVATAR_STORE_FILE "data/telegram-thumbs.bin"
#define TG_AVATAR_STORE_VERSION 1UL

void tg_mtproto_avatar_store_save(void)
{
    FILE *f;
    unsigned long header[2];

    f = fopen(TG_AVATAR_STORE_FILE, "wb");
    if (f == 0) {
        return;
    }
    header[0] = TG_AVATAR_STORE_VERSION;
    header[1] = sizeof(tg_avatar_slot);
    if (fwrite(header, sizeof(header), 1, f) == 1) {
        (void)fwrite(tg_avatar_store, sizeof(tg_avatar_store), 1, f);
    }
    fclose(f);
}

void tg_mtproto_avatar_store_load(void)
{
    FILE *f;
    unsigned long header[2];

    f = fopen(TG_AVATAR_STORE_FILE, "rb");
    if (f == 0) {
        return;
    }
    if (fread(header, sizeof(header), 1, f) == 1 &&
        header[0] == TG_AVATAR_STORE_VERSION &&
        header[1] == sizeof(tg_avatar_slot)) {
        if (fread(tg_avatar_store, sizeof(tg_avatar_store), 1, f) != 1) {
            memset(tg_avatar_store, 0, sizeof(tg_avatar_store));
        }
    }
    fclose(f);
}

int tg_mtproto_avatar_thumb_lookup(unsigned long id_hi, unsigned long id_lo,
                                   const unsigned char **thumb,
                                   unsigned long *len)
{
    unsigned long i;

    for (i = 0UL; i < TG_AVATAR_STORE_MAX; ++i) {
        if (tg_avatar_store[i].len != 0UL &&
            tg_avatar_store[i].id_hi == id_hi &&
            tg_avatar_store[i].id_lo == id_lo) {
            *thumb = tg_avatar_store[i].thumb;
            *len = tg_avatar_store[i].len;
            return 1;
        }
    }
    return 0;
}

/* chat#/channel#: the photo:ChatPhoto field follows right after the leading
   fields the scanner reads (title / optional username), so capture it into the
   just-applied cache entry. BEST-EFFORT by design: any parse hiccup here simply
   stops the capture -- the chat itself is already applied and the scanner's
   resync finds the next object exactly as it did when this field was skipped.
   chatPhoto#1c6e1c11 flags:# has_video:flags.0?true photo_id:long
   stripped_thumb:flags.1?bytes dc_id:int. */
static void tg_chat_photo_capture(tg_mtproto_tl_reader *reader,
                                  tg_mtproto_peer_cache *cache,
                                  unsigned long peer_constructor,
                                  unsigned long id_hi,
                                  unsigned long id_lo)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long ph_hi;
    unsigned long ph_lo;
    unsigned long dc;
    const unsigned char *thumb;
    unsigned long thumb_len;
    tg_mtproto_peer_cache_entry *entry;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_CHAT_PHOTO_CONSTRUCTOR) {
        return; /* empty/absent photo: keep the initials */
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &ph_hi, &ph_lo) != TG_MTPROTO_TL_OK) {
        return;
    }
    thumb = 0;
    thumb_len = 0UL;
    if ((flags & 2UL) != 0UL &&
        tg_mtproto_tl_read_bytes(reader, &thumb, &thumb_len) !=
            TG_MTPROTO_TL_OK) {
        return;
    }
    if (tg_mtproto_tl_read_u32(reader, &dc) != TG_MTPROTO_TL_OK) {
        return;
    }
    entry = tg_peer_cache_find(cache, peer_constructor, id_hi, id_lo);
    if (entry == 0) {
        return;
    }
    entry->photo_id_hi = ph_hi;
    entry->photo_id_lo = ph_lo;
    entry->photo_dc_id = dc;
    if (thumb_len > 0UL && thumb_len <= TG_MTPROTO_STRIPPED_MAX) {
        memcpy(entry->stripped, thumb, thumb_len);
        entry->stripped_len = thumb_len;
        tg_avatar_store_put(id_hi, id_lo, thumb, thumb_len,
                            ph_hi, ph_lo, dc);
    }
}

static tg_mtproto_tl_status tg_read_chat_leading_from_reader(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_peer_cache *cache)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long flags2;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    char title[128];
    char username[96];
    int has_access_hash;

    title[0] = '\0';
    username[0] = '\0';
    access_hash_hi = 0UL;
    access_hash_lo = 0UL;
    has_access_hash = 0;
    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (constructor) {
    case TG_CHAT_CONSTRUCTOR:
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
                TG_MTPROTO_TL_OK ||
            tg_read_string_copy(reader, title, sizeof(title)) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        (void)flags;
        if (!tg_peer_cache_apply_chat(cache, TG_PEER_CHAT_CONSTRUCTOR,
                                      id_hi, id_lo, 0UL, 0UL, 0, title,
                                      username)) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        tg_chat_photo_capture(reader, cache, TG_PEER_CHAT_CONSTRUCTOR,
                              id_hi, id_lo);
        return TG_MTPROTO_TL_OK;
    case TG_CHANNEL_CONSTRUCTOR:
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &flags2) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & (1UL << 13)) != 0UL) {
            if (tg_mtproto_tl_read_u64(reader, &access_hash_hi,
                                       &access_hash_lo) !=
                TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
            has_access_hash = 1;
        }
        if (tg_read_string_copy(reader, title, sizeof(title)) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & (1UL << 6)) != 0UL &&
            tg_read_string_copy(reader, username, sizeof(username)) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        (void)flags2;
        if (!tg_peer_cache_apply_chat(cache, TG_PEER_CHANNEL_CONSTRUCTOR,
                                      id_hi, id_lo, access_hash_hi,
                                      access_hash_lo, has_access_hash,
                                      title, username)) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        tg_chat_photo_capture(reader, cache, TG_PEER_CHANNEL_CONSTRUCTOR,
                              id_hi, id_lo);
        return TG_MTPROTO_TL_OK;
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static unsigned long tg_peer_cache_scan_chats(const unsigned char *body,
                                              unsigned long body_length,
                                              tg_mtproto_peer_cache *cache)
{
    tg_mtproto_tl_reader reader;
    unsigned long offset;
    unsigned long constructor;
    unsigned long updated;

    if (body == 0 || cache == 0 || body_length < 20UL) {
        return 0UL;
    }
    updated = 0UL;
    for (offset = 0UL; offset + 20UL <= body_length; offset += 4UL) {
        constructor = tg_read_u32_le(body + offset);
        if (constructor != TG_CHAT_CONSTRUCTOR &&
            constructor != TG_CHANNEL_CONSTRUCTOR) {
            continue;
        }
        tg_mtproto_tl_reader_init(&reader, body + offset,
                                  body_length - offset);
        if (tg_read_chat_leading_from_reader(&reader, cache) ==
            TG_MTPROTO_TL_OK) {
            ++updated;
        }
    }
    return updated;
}

static tg_mtproto_tl_status tg_read_peer_cache_chat(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_peer_cache *cache)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long access_hash_hi;
    unsigned long access_hash_lo;
    tg_mtproto_peer_cache_entry *entry;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (constructor) {
    case TG_CHAT_EMPTY_CONSTRUCTOR:
        if (tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        (void)tg_peer_cache_add(cache, TG_PEER_CHAT_CONSTRUCTOR, id_hi, id_lo);
        return TG_MTPROTO_TL_OK;
    case TG_CHAT_FORBIDDEN_CONSTRUCTOR:
        if (tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        entry = tg_peer_cache_add(cache, TG_PEER_CHAT_CONSTRUCTOR, id_hi,
                                  id_lo);
        if (entry == 0) {
            return tg_skip_string(reader);
        }
        return tg_read_string_copy(reader, entry->title,
                                   sizeof(entry->title));
    case TG_CHANNEL_FORBIDDEN_CONSTRUCTOR:
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u64(reader, &access_hash_hi,
                                   &access_hash_lo) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        entry = tg_peer_cache_add(cache, TG_PEER_CHANNEL_CONSTRUCTOR, id_hi,
                                  id_lo);
        if (entry == 0) {
            return tg_skip_string(reader);
        }
        entry->has_access_hash = 1;
        entry->access_hash_hi = access_hash_hi;
        entry->access_hash_lo = access_hash_lo;
        if (tg_read_string_copy(reader, entry->title, sizeof(entry->title)) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & (1UL << 16)) != 0UL &&
            tg_mtproto_tl_read_u32(reader, &id_lo) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

/*
 * Message text entities (bold/italic/code/strike) are surfaced as inline ASCII
 * markers baked into the text so every front-end -- console and the native GUI
 * on all five targets -- shows formatting with no per-layer plumbing. Telegram
 * entity offsets/lengths are UTF-16 code units over the UTF-8 body, so the
 * marker pass maps them to byte positions. Real GUI font styling (SetSoftStyle)
 * can later read structured entities; this is the smallest visible step.
 */
#define TG_MSG_ENT_NONE   0UL
#define TG_MSG_ENT_BOLD   1UL
#define TG_MSG_ENT_ITALIC 2UL
#define TG_MSG_ENT_CODE   3UL /* inline code + pre block */
#define TG_MSG_ENT_STRIKE 4UL
#define TG_MSG_ENTITY_MAX 24

typedef struct tg_msg_entity {
    unsigned long type; /* TG_MSG_ENT_* */
    unsigned long off;  /* UTF-16 code units from the start of the body */
    unsigned long len;  /* UTF-16 code units */
} tg_msg_entity;

/* The 1-char marker for a styled entity, or 0 for entities we leave as plain
   text (mentions, links, hashtags ... already readable as their own text). */
static const char *tg_msg_entity_marker(unsigned long type)
{
    switch (type) {
    case TG_MSG_ENT_BOLD:
        return "*";
    case TG_MSG_ENT_ITALIC:
        return "_";
    case TG_MSG_ENT_CODE:
        return "`";
    case TG_MSG_ENT_STRIKE:
        return "~";
    default:
        return 0;
    }
}

/* Length in bytes of the UTF-8 sequence starting at s[0], and how many UTF-16
   code units it represents (2 for astral planes via a surrogate pair, else 1).
   Malformed lead/continuation bytes are treated as a single 1-unit byte. */
static unsigned long tg_utf8_step(const unsigned char *s,
                                  unsigned long *utf16_units)
{
    unsigned char c = s[0];

    if (c < 0x80U) {
        *utf16_units = 1UL;
        return 1UL;
    }
    if ((c & 0xE0U) == 0xC0U) {
        *utf16_units = 1UL;
        return 2UL;
    }
    if ((c & 0xF0U) == 0xE0U) {
        *utf16_units = 1UL;
        return 3UL;
    }
    if ((c & 0xF8U) == 0xF0U) {
        *utf16_units = 2UL;
        return 4UL;
    }
    *utf16_units = 1UL;
    return 1UL;
}

/* Rewrite text in place, inserting open/close markers around styled spans. The
   "opened" bookkeeping guarantees balanced markers even if a malformed entity
   reaches past the end of the body. */
static void tg_apply_entity_markers(char *text, unsigned long size,
                                    const tg_msg_entity *ents, int count)
{
    char buf[TG_MTPROTO_MESSAGE_TEXT_MAX];
    char opened[TG_MSG_ENTITY_MAX];
    unsigned long bi = 0UL; /* output byte index */
    unsigned long ti = 0UL; /* input byte index */
    unsigned long u16 = 0UL; /* UTF-16 position in the body */
    int e;

    for (e = 0; e < count && e < TG_MSG_ENTITY_MAX; ++e) {
        opened[e] = 0;
    }
    for (;;) {
        /* Closes before opens at the same position so adjacent spans abut. */
        for (e = 0; e < count && e < TG_MSG_ENTITY_MAX; ++e) {
            const char *m;
            if (opened[e] && ents[e].off + ents[e].len == u16 &&
                (m = tg_msg_entity_marker(ents[e].type)) != 0) {
                while (*m != '\0' && bi + 1UL < sizeof(buf)) {
                    buf[bi++] = *m++;
                }
                opened[e] = 0;
            }
        }
        for (e = 0; e < count && e < TG_MSG_ENTITY_MAX; ++e) {
            const char *m;
            if (!opened[e] && ents[e].off == u16 && ents[e].len > 0UL &&
                (m = tg_msg_entity_marker(ents[e].type)) != 0) {
                while (*m != '\0' && bi + 1UL < sizeof(buf)) {
                    buf[bi++] = *m++;
                }
                opened[e] = 1;
            }
        }
        if (text[ti] == '\0') {
            break;
        }
        {
            unsigned long units;
            unsigned long n = tg_utf8_step((const unsigned char *)text + ti,
                                           &units);
            unsigned long k;
            for (k = 0UL; k < n && text[ti] != '\0'; ++k) {
                if (bi + 1UL < sizeof(buf)) {
                    buf[bi++] = text[ti];
                }
                ++ti;
            }
            u16 += units;
        }
    }
    /* Close any span the body ended inside of. */
    for (e = 0; e < count && e < TG_MSG_ENTITY_MAX; ++e) {
        const char *m;
        if (opened[e] && (m = tg_msg_entity_marker(ents[e].type)) != 0) {
            while (*m != '\0' && bi + 1UL < sizeof(buf)) {
                buf[bi++] = *m++;
            }
        }
    }
    buf[bi] = '\0';
    for (ti = 0UL; ti + 1UL < size && buf[ti] != '\0'; ++ti) {
        text[ti] = buf[ti];
    }
    text[ti] = '\0';
}

/* Read a vector<MessageEntity>. When ents/count are non-NULL the styled spans
   (bold/italic/code/strike) are captured for tg_apply_entity_markers; passing
   NULL skips the vector (consuming its bytes). */
static tg_mtproto_tl_status tg_read_message_entity_vector(
    tg_mtproto_tl_reader *reader, tg_msg_entity *ents, int max, int *count)
{
    unsigned long n;
    unsigned long constructor;
    unsigned long off;
    unsigned long len;
    unsigned long scratch_hi;
    unsigned long scratch_lo;
    unsigned long i;

    if (count != 0) {
        *count = 0;
    }
    if (tg_read_vector_count(reader, &n) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < n; ++i) {
        unsigned long type = TG_MSG_ENT_NONE;

        if (tg_mtproto_tl_read_u32(reader, &constructor) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &off) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &len) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        switch (constructor) {
        case 0x73924be0UL: /* messageEntityPre (offset,length,language) */
            type = TG_MSG_ENT_CODE;
            if (tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
            break;
        case 0x76a6d327UL: /* messageEntityTextUrl (offset,length,url) */
            if (tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
            break;
        case 0xdc7b1140UL: /* messageEntityMentionName */
        case 0xc8cf05f8UL: /* messageEntityCustomEmoji */
            if (tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
            break;
        case 0xbd610bc9UL: /* messageEntityBold */
            type = TG_MSG_ENT_BOLD;
            break;
        case 0x826f8b60UL: /* messageEntityItalic */
            type = TG_MSG_ENT_ITALIC;
            break;
        case 0x28a20571UL: /* messageEntityCode */
            type = TG_MSG_ENT_CODE;
            break;
        case 0xbf0693d4UL: /* messageEntityStrike */
            type = TG_MSG_ENT_STRIKE;
            break;
        case 0xbb92ba95UL: /* messageEntityUnknown */
        case 0xfa04579dUL: /* messageEntityMention */
        case 0x6f635b0dUL: /* messageEntityHashtag */
        case 0x6cef8ac7UL: /* messageEntityBotCommand */
        case 0x6ed02538UL: /* messageEntityUrl */
        case 0x64e475c2UL: /* messageEntityEmail */
        case 0x9b69e34bUL: /* messageEntityPhone */
        case 0x4c4e743fUL: /* messageEntityCashtag */
        case 0x9c4e7e8bUL: /* messageEntityUnderline */
        case 0x20df5d0UL:  /* messageEntityBlockquote */
        case 0x32ca960fUL: /* messageEntitySpoiler */
        case 0x761e6af4UL: /* messageEntityBankCard */
            break;
        default:
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if (type != TG_MSG_ENT_NONE && ents != 0 && count != 0 &&
            *count < max) {
            ents[*count].type = type;
            ents[*count].off = off;
            ents[*count].len = len;
            ++(*count);
        }
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_message_entity_vector(
    tg_mtproto_tl_reader *reader)
{
    return tg_read_message_entity_vector(reader, 0, 0, 0);
}

/* messageReplyHeader#6917560b (layer 214). Field order on the wire:
   reply_to_msg_id(flags.4), reply_to_peer_id(flags.0), reply_from(flags.5),
   reply_media(flags.8), reply_to_top_id(flags.1), quote_text(flags.6),
   quote_entities(flags.7), quote_offset(flags.10). When out_reply_to_msg_id /
   out_quote are non-NULL the reply target and the inline quote are captured;
   passing NULL just skips the header (consuming its bytes). reply_from /
   reply_media (flags 5/8) are still rejected -- this small reader cannot walk
   a nested fwd-header / media there. */
static tg_mtproto_tl_status tg_read_message_reply_header(
    tg_mtproto_tl_reader *reader, unsigned long *out_reply_to_msg_id,
    char *out_quote, unsigned long quote_size)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;
    tg_mtproto_dialog_peer peer;

    if (out_reply_to_msg_id != 0) {
        *out_reply_to_msg_id = 0UL;
    }
    if (out_quote != 0 && quote_size > 0UL) {
        out_quote[0] = '\0';
    }
    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MESSAGE_REPLY_HEADER_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 4)) != 0UL &&
        tg_mtproto_tl_read_u32(
            reader, out_reply_to_msg_id != 0 ? out_reply_to_msg_id : &scratch) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 1UL) != 0UL &&
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 5)) != 0UL || (flags & (1UL << 8)) != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 6)) != 0UL) {
        if (out_quote != 0 && quote_size > 0UL) {
            if (tg_read_string_copy(reader, out_quote, quote_size) !=
                TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        } else if (tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    if ((flags & (1UL << 7)) != 0UL &&
        tg_skip_message_entity_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 10)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 11)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_message_reply_header(
    tg_mtproto_tl_reader *reader)
{
    return tg_read_message_reply_header(reader, 0, 0, 0UL);
}

static tg_mtproto_tl_status tg_skip_message_fwd_header(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;
    tg_mtproto_dialog_peer peer;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MESSAGE_FWD_HEADER_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 1UL) != 0UL &&
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 5)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 8UL) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 4)) != 0UL) {
        if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    if ((flags & (1UL << 8)) != 0UL &&
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 9)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 10)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 6)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_reaction(tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long scratch_hi;
    unsigned long scratch_lo;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    switch (constructor) {
    case 0x79f5d419UL: /* reactionEmpty */
    case 0x523da4ebUL: /* reactionPaid */
        return TG_MTPROTO_TL_OK;
    case 0x1b2286b8UL: /* reactionEmoji */
        return tg_skip_string(reader);
    case 0x8935fc73UL: /* reactionCustomEmoji */
        return tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo);
    default:
        return TG_MTPROTO_TL_INVALID_DATA;
    }
}

static tg_mtproto_tl_status tg_skip_reaction_count_vector(
    tg_mtproto_tl_reader *reader)
{
    unsigned long count;
    unsigned long constructor;
    unsigned long flags;
    unsigned long scratch;
    unsigned long i;

    if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < count; ++i) {
        if (tg_mtproto_tl_read_u32(reader, &constructor) !=
                TG_MTPROTO_TL_OK ||
            constructor != TG_REACTION_COUNT_CONSTRUCTOR ||
            tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 1UL) != 0UL &&
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if (tg_skip_reaction(reader) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_message_replies(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long count;
    unsigned long scratch_hi;
    unsigned long scratch_lo;
    unsigned long i;
    tg_mtproto_dialog_peer peer;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MESSAGE_REPLIES_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL) {
        if (tg_read_vector_count(reader, &count) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        for (i = 0UL; i < count; ++i) {
            if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
                return TG_MTPROTO_TL_INVALID_DATA;
            }
        }
    }
    if ((flags & 1UL) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 8UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_message_reactions(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MESSAGE_REACTIONS_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_skip_reaction_count_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 2UL) != 0UL || (flags & (1UL << 4)) != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_skip_common_message(
    tg_mtproto_tl_reader *reader)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long flags2;
    unsigned long scratch_hi;
    unsigned long scratch_lo;
    tg_mtproto_dialog_peer peer;

    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_MESSAGE_EMPTY_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 1UL) != 0UL &&
            tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    }
    if (constructor == TG_MESSAGE_SERVICE_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & (1UL << 8)) != 0UL &&
            tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & (1UL << 28)) != 0UL &&
            tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 8UL) != 0UL) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if (tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor != TG_MESSAGE_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &flags2) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 8)) != 0UL &&
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 29)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 28)) != 0UL && /* saved_peer_id (Saved Messages);
           was mis-gated on flags2.2 and desynced the whole self-chat walk */
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_skip_message_fwd_header(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 64UL) != 0UL || (flags & 512UL) != 0UL ||
        (flags2 & 8UL) != 0UL || (flags2 & 128UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 11)) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags2 & 1UL) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 8UL) != 0UL &&
        tg_skip_message_reply_header(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK ||
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 128UL) != 0UL &&
        tg_skip_message_entity_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 10)) != 0UL) {
        if (tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    }
    if ((flags & (1UL << 23)) != 0UL &&
        tg_skip_message_replies(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 15)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 16)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 17)) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 20)) != 0UL &&
        tg_skip_message_reactions(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 22)) != 0UL &&
        tg_skip_restriction_reason_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 25)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 30)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags2 & 32UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags2 & 64UL) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

/* A short bracketed placeholder for a media-only message (no caption), so the
   transcript shows "[Photo]" instead of an empty bubble. The client does not
   download media -- this is awareness only. Constructor ids verified against the
   Telegram TL schema (core.telegram.org); anything else falls back to "[Media]". */
static const char *tg_mtproto_media_label(unsigned long constructor)
{
    switch (constructor) {
    case 0x695150d7UL: /* messageMediaPhoto */
        return "[Photo]";
    case 0x52d8ccd9UL: /* messageMediaDocument (file/video/voice/sticker/gif) */
        return "[File]";
    case 0x56e0d474UL: /* messageMediaGeo */
        return "[Location]";
    case 0xddf10c3bUL: /* messageMediaWebPage: a link the server previewed */
        return "[Link]";
    default:
        return "[Media]";
    }
}

/* Bounded append for the label builders: stops at the cap and keeps the string
   terminated, which is exactly what a fixed size bubble label wants. */
static void tg_label_append(char *text, unsigned long text_size,
                            unsigned long *n, const char *s)
{
    while (*s != '\0' && *n + 1UL < text_size) {
        text[(*n)++] = *s++;
    }
    text[*n] = '\0';
}

/* m:ss, or h:mm:ss once past the hour. `buf` needs 16 bytes. */
static void tg_format_duration(unsigned long seconds, char *buf)
{
    unsigned long h = seconds / 3600UL;
    unsigned long m = (seconds / 60UL) % 60UL;
    unsigned long s = seconds % 60UL;

    if (h != 0UL) {
        sprintf(buf, "%lu:%02lu:%02lu", h, m, s);
    } else {
        sprintf(buf, "%lu:%02lu", m, s);
    }
}

/* Builds the transcript label for an attached document into `text`. A plain
   file gets its name and a compact human size (B / KB / MB from the 64-bit
   size); the kinds people actually recognise get told apart, because
   "[File: sticker.webp (14 KB)]" says nothing a reader wants to know and
   "[Video 1:32 640x360]" says everything. Media-only messages (no caption)
   show this instead of an empty bubble; the document meta itself is captured
   separately for the download, so a shorter label costs no function. */
static void tg_mtproto_format_document_label(
    const tg_mtproto_document_meta *doc, char *text, unsigned long text_size)
{
    char size_buf[24];
    char dur_buf[16];
    char geo_buf[24];
    const char *name;
    unsigned long n = 0UL;
    const char *p;

    if (text_size < 8UL) {
        if (text_size > 0UL) {
            text[0] = '\0';
        }
        return;
    }
    /* A sticker stands for an emoji, and that emoji is the whole message. The
       .webp behind it has a name nobody wants to read and a size nobody cares
       about, so neither appears. */
    if (doc->kind == (unsigned char)TG_MTPROTO_DOC_KIND_STICKER) {
        tg_label_append(text, text_size, &n, "[Sticker");
        if (doc->alt[0] != '\0') {
            tg_label_append(text, text_size, &n, " ");
            tg_label_append(text, text_size, &n, doc->alt);
        }
        tg_label_append(text, text_size, &n, "]");
        return;
    }
    /* Human size: exact enough for a label; treats >4GB as MB (avatars/docs on
       these systems are far smaller, and 64-bit division is costly on 68k). */
    if (doc->size_hi != 0UL || doc->size_lo >= 1048576UL) {
        unsigned long mb = (doc->size_hi != 0UL)
                               ? (4096UL + doc->size_lo / 1048576UL)
                               : (doc->size_lo / 1048576UL);
        sprintf(size_buf, "%lu MB", mb);
    } else if (doc->size_lo >= 1024UL) {
        sprintf(size_buf, "%lu KB", doc->size_lo / 1024UL);
    } else {
        sprintf(size_buf, "%lu B", doc->size_lo);
    }
    dur_buf[0] = '\0';
    geo_buf[0] = '\0';
    if (doc->duration != 0UL) {
        tg_format_duration(doc->duration, dur_buf);
    }
    if (doc->width != 0UL && doc->height != 0UL) {
        sprintf(geo_buf, "%lux%lu", doc->width, doc->height);
    }
    switch (doc->kind) {
    case TG_MTPROTO_DOC_KIND_VIDEO:
    case TG_MTPROTO_DOC_KIND_GIF:
        /* A clip is its length and its shape, not its camera filename. The
           GIF keeps the size because it is the one people forward blind. */
        tg_label_append(text, text_size, &n,
                        (doc->kind == TG_MTPROTO_DOC_KIND_GIF)
                            ? "[GIF" : "[Video");
        if (dur_buf[0] != '\0') {
            tg_label_append(text, text_size, &n, " ");
            tg_label_append(text, text_size, &n, dur_buf);
        }
        if (geo_buf[0] != '\0') {
            tg_label_append(text, text_size, &n, " ");
            tg_label_append(text, text_size, &n, geo_buf);
        }
        tg_label_append(text, text_size, &n, " (");
        tg_label_append(text, text_size, &n, size_buf);
        tg_label_append(text, text_size, &n, ")]");
        return;
    case TG_MTPROTO_DOC_KIND_VOICE:
        /* A voice note has no name worth showing: it is a length. */
        tg_label_append(text, text_size, &n, "[Voice");
        if (dur_buf[0] != '\0') {
            tg_label_append(text, text_size, &n, " ");
            tg_label_append(text, text_size, &n, dur_buf);
        }
        tg_label_append(text, text_size, &n, " (");
        tg_label_append(text, text_size, &n, size_buf);
        tg_label_append(text, text_size, &n, ")]");
        return;
    case TG_MTPROTO_DOC_KIND_AUDIO:
        /* Music keeps its filename: it is how you know which track it is. */
        name = (doc->file_name[0] != '\0') ? doc->file_name : "Audio";
        tg_label_append(text, text_size, &n, "[Audio: ");
        tg_label_append(text, text_size, &n, name);
        if (dur_buf[0] != '\0') {
            tg_label_append(text, text_size, &n, " ");
            tg_label_append(text, text_size, &n, dur_buf);
        }
        tg_label_append(text, text_size, &n, " (");
        tg_label_append(text, text_size, &n, size_buf);
        tg_label_append(text, text_size, &n, ")]");
        return;
    default:
        break;
    }
    name = (doc->file_name[0] != '\0') ? doc->file_name : "File";
    for (p = "[File: "; *p != '\0' && n + 1UL < text_size; ++p) {
        text[n++] = *p;
    }
    for (p = name; *p != '\0' && n + 1UL < text_size; ++p) {
        text[n++] = *p;
    }
    for (p = " ("; *p != '\0' && n + 1UL < text_size; ++p) {
        text[n++] = *p;
    }
    for (p = size_buf; *p != '\0' && n + 1UL < text_size; ++p) {
        text[n++] = *p;
    }
    for (p = ")]"; *p != '\0' && n + 1UL < text_size; ++p) {
        text[n++] = *p;
    }
    text[n] = '\0';
}

/* A link preview is a page the server fetched on our behalf, so the fields are
   short by construction. These caps are the bubble's, not the protocol's. */
#define TG_WEBPAGE_SITE_MAX 40U
#define TG_WEBPAGE_TITLE_MAX 96U
#define TG_WEBPAGE_DESC_MAX 128U

/* Reads the WebPage of a messageMediaWebPage and appends the two preview lines
   under the message text: the bracketed site and title, then the first line of
   the description. A bare pasted link used to show the URL and nothing else.

   Only the fields a text bubble can show are read, in TL declaration order,
   and the walk stops right after the photo. It never needs to go further:
   cached_page:flags.10 is an Instant View article, a recursive tree of page
   blocks and rich text that would cost more code than the whole feature, and
   the caller abandons the reader after the media in any case.

   Constructors verified at core.telegram.org: messageMediaWebPage ddf10c3b,
   webPage e89c45b2, and the three that carry nothing to show, webPageEmpty
   211a1788, webPagePending b0d13e47, webPageNotModified 7311ca11. A pending
   preview is the server still fetching the page; it arrives later through
   updateWebPage, which this client does not follow yet. */
static void tg_mtproto_append_web_page(tg_mtproto_tl_reader *reader,
                                       tg_mtproto_message_text *out)
{
    char site[TG_WEBPAGE_SITE_MAX];
    char title[TG_WEBPAGE_TITLE_MAX];
    char desc[TG_WEBPAGE_DESC_MAX];
    unsigned long ctor;
    unsigned long flags;
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long scratch;
    unsigned long n;
    unsigned long i;

    site[0] = '\0';
    title[0] = '\0';
    desc[0] = '\0';
    if (tg_mtproto_tl_read_u32(reader, &ctor) != TG_MTPROTO_TL_OK ||
        ctor != 0xe89c45b2UL) {
        return; /* empty, pending or not modified: nothing to draw yet */
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u64(reader, &id_hi, &id_lo) != TG_MTPROTO_TL_OK ||
        tg_skip_string(reader) != TG_MTPROTO_TL_OK ||  /* url */
        tg_skip_string(reader) != TG_MTPROTO_TL_OK ||  /* display_url */
        tg_mtproto_tl_read_u32(reader, &scratch) != TG_MTPROTO_TL_OK) {
        return;
    }
    if ((flags & 1UL) != 0UL && /* type: "article", "photo", "video", ... */
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return;
    }
    if ((flags & 2UL) != 0UL &&
        tg_read_string_copy(reader, site, sizeof(site)) != TG_MTPROTO_TL_OK) {
        return;
    }
    if ((flags & 4UL) != 0UL &&
        tg_read_string_copy(reader, title, sizeof(title)) !=
            TG_MTPROTO_TL_OK) {
        return;
    }
    if ((flags & 8UL) != 0UL &&
        tg_read_string_copy(reader, desc, sizeof(desc)) != TG_MTPROTO_TL_OK) {
        return;
    }
    tg_trim_utf8_tail(site);
    tg_trim_utf8_tail(title);
    /* The description is a paragraph; the bubble gets its first line. */
    for (i = 0UL; desc[i] != '\0'; ++i) {
        if (desc[i] == '\n' || desc[i] == '\r') {
            desc[i] = '\0';
            break;
        }
    }
    tg_trim_utf8_tail(desc);
    /* The preview photo is a plain Photo, so it goes through the same bounded
       inline pipeline as any other and obeys the same Inline photos setting.
       The message keeps its own text, so this is never a photo-only bubble. */
    if ((flags & 16UL) != 0UL) {
        (void)tg_mtproto_read_photo(reader, &out->photo);
    }
    if (site[0] == '\0' && title[0] == '\0' && desc[0] == '\0') {
        return; /* a preview with nothing in it is not worth a line */
    }
    n = (unsigned long)strlen(out->text);
    if (site[0] != '\0' || title[0] != '\0') {
        if (n != 0UL && n + 1UL < sizeof(out->text)) {
            out->text[n++] = '\n';
            out->text[n] = '\0';
        }
        tg_label_append(out->text, sizeof(out->text), &n, "[Link: ");
        if (site[0] != '\0') {
            tg_label_append(out->text, sizeof(out->text), &n, site);
            if (title[0] != '\0') {
                tg_label_append(out->text, sizeof(out->text), &n, " - ");
            }
        }
        tg_label_append(out->text, sizeof(out->text), &n, title);
        tg_label_append(out->text, sizeof(out->text), &n, "]");
    }
    if (desc[0] != '\0') {
        if (n != 0UL && n + 1UL < sizeof(out->text)) {
            out->text[n++] = '\n';
            out->text[n] = '\0';
        }
        tg_label_append(out->text, sizeof(out->text), &n, desc);
    }
    out->has_text = out->text[0] != '\0';
}

/* Keep a real caption first (entity offsets refer to it), then place the file
   affordance on its own line. A truncated suffix is still preferable to losing
   either the caption or the fact that the attachment is downloadable. */
static void tg_mtproto_append_document_label(
    const tg_mtproto_document_meta *doc, char *text, unsigned long text_size)
{
    char label[TG_MTPROTO_DOC_NAME_MAX + 40U];
    unsigned long n;
    unsigned long i;
    unsigned long label_len;

    if (text == 0 || text_size == 0UL) {
        return;
    }
    tg_mtproto_format_document_label(doc, label, sizeof(label));
    n = (unsigned long)strlen(text);
    label_len = (unsigned long)strlen(label);
    if (n != 0UL && n + 1UL + label_len + 1UL > text_size) {
        n = text_size > label_len + 2UL
                ? text_size - label_len - 2UL : 0UL;
        /* The caption is UTF-8 at this point. Never leave a partial sequence
           before the newline when the fixed message buffer needs trimming. */
        while (n > 0UL &&
               (((unsigned char)text[n] & 0xc0U) == 0x80U)) {
            --n;
        }
        text[n] = '\0';
    }
    if (n != 0UL && n + 1UL < text_size) {
        text[n++] = '\n';
    }
    i = 0UL;
    while (label[i] != '\0' && n + 1UL < text_size) {
        text[n++] = label[i++];
    }
    text[n] = '\0';
}

static tg_mtproto_tl_status tg_read_common_message_text(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_message_text *out,
    tg_mtproto_dialog_peer *out_dest)
{
    unsigned long constructor;
    unsigned long flags;
    unsigned long flags2;
    unsigned long scratch_hi;
    unsigned long scratch_lo;
    tg_mtproto_dialog_peer peer;

    if (out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (out_dest != 0) {
        memset(out_dest, 0, sizeof(*out_dest));
    }
    if (tg_mtproto_tl_read_u32(reader, &constructor) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_MESSAGE_EMPTY_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 1UL) != 0UL &&
            tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        return TG_MTPROTO_TL_OK;
    }
    if (constructor != TG_MESSAGE_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_u32(reader, &flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &flags2) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(reader, &out->id) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->flags = flags;
    out->is_out = (flags & 2UL) != 0UL;
    if ((flags & (1UL << 8)) != 0UL) {
        if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->from_constructor = peer.peer_constructor;
        out->from_id_hi = peer.id_hi;
        out->from_id_lo = peer.id_lo;
    }
    if ((flags & (1UL << 29)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (out_dest != 0) {
        /* The message's peer_id: the chat this message belongs to. */
        *out_dest = peer;
    }
    if ((flags & (1UL << 28)) != 0UL && /* saved_peer_id (Saved Messages);
           was mis-gated on flags2.2 and desynced the whole self-chat walk */
        tg_read_peer_ref(reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 4UL) != 0UL &&
        tg_skip_message_fwd_header(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & (1UL << 11)) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags2 & 1UL) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if ((flags & 8UL) != 0UL) {
        if (tg_read_message_reply_header(reader, &out->reply_to_msg_id,
                                         out->reply_quote,
                                         sizeof(out->reply_quote)) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        out->has_reply = 1;
    }
    if (tg_mtproto_tl_read_u32(reader, &out->date) != TG_MTPROTO_TL_OK ||
        tg_read_string_copy(reader, out->text, sizeof(out->text)) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->has_text = out->text[0] != '\0';
    /*
     * Bot replies often carry reply markup or other post-text fields. Keep the
     * message text even when this small reader cannot skip the remaining tail.
     */
    if ((flags & 64UL) != 0UL || (flags & 512UL) != 0UL ||
        (flags2 & 8UL) != 0UL || (flags2 & 128UL) != 0UL) {
        /* media (flag.9) is the first tail field after the text in the TL. For a
           media-only message (no caption) read JUST its constructor for a
           "[Photo]"/"[File]"/... placeholder, then bail on the rest of the tail
           (we do not walk the full MessageMedia). Reading one u32 here cannot
           corrupt the parse -- we return immediately after, exactly as before. */
        if ((flags & 512UL) != 0UL) {
            unsigned long media_ctor;

            if (tg_mtproto_tl_read_u32(reader, &media_ctor) == TG_MTPROTO_TL_OK) {
                /* messageMediaDocument#52d8ccd9: flags:# document:flags.0?Document.
                   Capture the Document (name/size + the download meta) BEST-
                   EFFORT; the reader is abandoned right after either way, so a
                   parse hiccup just falls back to the plain placeholder. */
                if (media_ctor == 0x52d8ccd9UL) {
                    unsigned long mflags;

                    if (tg_mtproto_tl_read_u32(reader, &mflags) ==
                            TG_MTPROTO_TL_OK &&
                        (mflags & 1UL) != 0UL &&
                        tg_mtproto_read_document(reader, &out->document) ==
                            TG_MTPROTO_TL_OK &&
                        out->document.has_document) {
                        tg_mtproto_append_document_label(&out->document,
                                                         out->text,
                                                         sizeof(out->text));
                        out->has_text = out->text[0] != '\0';
                    }
                } else if (media_ctor == 0x695150d7UL) {
                    unsigned long mflags;

                    out->photo_only = !out->has_text;
                    if (tg_mtproto_tl_read_u32(reader, &mflags) ==
                            TG_MTPROTO_TL_OK &&
                        (mflags & 1UL) != 0UL) {
                        (void)tg_mtproto_read_photo(reader, &out->photo);
                    }
                } else if (media_ctor == 0xddf10c3bUL) {
                    /* messageMediaWebPage: flags then a non-optional WebPage.
                       A pasted link used to show as the bare URL, because the
                       preview the server built for it was skipped whole. */
                    unsigned long mflags;

                    if (tg_mtproto_tl_read_u32(reader, &mflags) ==
                            TG_MTPROTO_TL_OK) {
                        tg_mtproto_append_web_page(reader, out);
                    }
                }
                if (!out->has_text) { /* non-document media, or no caption */
                    const char *label = tg_mtproto_media_label(media_ctor);
                    unsigned long li = 0UL;

                    while (label[li] != '\0' && li + 1UL < sizeof(out->text)) {
                        out->text[li] = label[li];
                        ++li;
                    }
                    out->text[li] = '\0';
                    out->has_text = 1;
                }
            }
        }
        return TG_MTPROTO_TL_OK;
    }

    if ((flags & 128UL) != 0UL) {
        tg_msg_entity ents[TG_MSG_ENTITY_MAX];
        int ent_count = 0;

        if (tg_read_message_entity_vector(reader, ents, TG_MSG_ENTITY_MAX,
                                          &ent_count) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_OK;
        }
        if (ent_count > 0 && out->has_text) {
            tg_apply_entity_markers(out->text, sizeof(out->text), ents,
                                    ent_count);
            out->has_text = out->text[0] != '\0';
        }
    }
    if ((flags & (1UL << 10)) != 0UL) {
        if (tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(reader, &scratch_lo) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_OK;
        }
    }
    if ((flags & (1UL << 23)) != 0UL &&
        tg_skip_message_replies(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 15)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 16)) != 0UL &&
        tg_skip_string(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 17)) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 20)) != 0UL &&
        tg_skip_message_reactions(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 22)) != 0UL &&
        tg_skip_restriction_reason_vector(reader) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 25)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags & (1UL << 30)) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags2 & 32UL) != 0UL &&
        tg_mtproto_tl_read_u32(reader, &scratch_lo) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    if ((flags2 & 64UL) != 0UL &&
        tg_mtproto_tl_read_u64(reader, &scratch_hi, &scratch_lo) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    return TG_MTPROTO_TL_OK;
}

static int tg_message_text_resync(tg_mtproto_tl_reader *reader,
                                  unsigned long start_offset,
                                  unsigned long max_id)
{
    unsigned long pos;
    unsigned long constructor;

    if (reader == 0 || start_offset >= reader->length) {
        return 0;
    }
    pos = (start_offset + 3UL) & ~3UL;
    while (pos + 4UL <= reader->length) {
        constructor = tg_read_u32_le(reader->buffer + pos);
        if (constructor == TG_MESSAGE_CONSTRUCTOR) {
            /* A bare 4-byte constructor match is not enough: the same bytes occur
               by chance inside a skipped photo/document/keyboard payload, and the
               id-only check still let garbage through (Amici showed "60/60 r=17/17"
               but rendered blank rows). Trial-PARSE the candidate on a throwaway
               reader: only a real Message parses to a valid, strictly-descending id
               (0 < id < the last one read) -- the parse walks flags/from_id/peer_id
               and bails out on garbage. With no bound yet (max_id 0) accept the
               first structural match. */
            if (max_id == 0UL) {
                reader->offset = pos;
                return 1;
            }
            {
                tg_mtproto_tl_reader trial;
                tg_mtproto_message_text probe;

                trial = *reader;
                trial.offset = pos;
                if (tg_read_common_message_text(&trial, &probe, 0) ==
                        TG_MTPROTO_TL_OK &&
                    probe.id != 0UL && probe.id < max_id) {
                    reader->offset = pos;
                    return 1;
                }
            }
        }
        pos += 4UL;
    }
    return 0;
}

void tg_mtproto_parse_message_peers(const unsigned char *body,
                                    unsigned long body_length,
                                    tg_mtproto_peer_cache *out)
{
    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (body == 0) {
        return;
    }
    /* No dialog peers are pre-seeded here, so allow the scanners to add a new
       entry for every user/chat referenced in the response. Otherwise the
       enrich-only path would leave the cache empty and group messages would
       fall back to the chat title instead of the actual sender name. */
    out->collect_all = 1;
    (void)tg_peer_cache_scan_chats(body, body_length, out);
    (void)tg_peer_cache_scan_users(body, body_length, out);
}

tg_mtproto_tl_status tg_mtproto_parse_dialog_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_dialog_peer peer;
    tg_mtproto_user_summary user;
    tg_mtproto_peer_cache_entry *entry;
    unsigned long count;
    unsigned long i;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (constructor == TG_MESSAGES_DIALOGS_NOT_MODIFIED_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (constructor == TG_MESSAGES_DIALOGS_SLICE_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(&reader, &out->total_dialog_count) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor != TG_MESSAGES_DIALOGS_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_MESSAGES_DIALOGS_CONSTRUCTOR) {
        out->total_dialog_count = count;
    }
    for (i = 0UL; i < count; ++i) {
        memset(&peer, 0, sizeof(peer));
        if (tg_read_dialog_peer(&reader, &peer) != TG_MTPROTO_TL_OK) {
            /* An unparseable dialog (e.g. a real draftMessage with media) would
               desync the rest of the vector. Stop walking dialogs but keep what
               we have; the whole-body chat/user scans below still recover the
               remaining peers. */
            break;
        }
        if (peer.peer_constructor == 0UL) {
            continue;  /* dialogFolder container, not a real peer */
        }
        entry = tg_peer_cache_add(out, peer.peer_constructor, peer.id_hi,
                                  peer.id_lo);
        if (entry != 0) {
            entry->top_message = peer.top_message;
            entry->unread_count = peer.unread_count;
            entry->from_dialog = 1;
        }
    }
    out->user_count = tg_peer_cache_scan_users(body, body_length, out);
    (void)tg_peer_cache_scan_chats(body, body_length, out);
    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    for (i = 0UL; i < count; ++i) {
        if (tg_skip_common_message(&reader) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_OK;
        }
    }
    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    out->chat_count = count;
    for (i = 0UL; i < count; ++i) {
        if (tg_read_peer_cache_chat(&reader, out) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_OK;
        }
    }
    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_OK;
    }
    out->user_count = count;
    for (i = 0UL; i < count; ++i) {
        if (tg_read_user_summary_from_reader(&reader, &user) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_OK;
        }
        entry = tg_peer_cache_add(out, TG_PEER_USER_CONSTRUCTOR, user.id_hi,
                                  user.id_lo);
        if (entry != 0) {
            (void)tg_peer_cache_apply_user(out, &user);
        }
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_resolved_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_dialog_peer peer;
    tg_mtproto_peer_cache_entry *entry;
    unsigned long i;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (constructor != TG_CONTACTS_RESOLVED_PEER_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    memset(&peer, 0, sizeof(peer));
    if (tg_read_peer_ref(&reader, &peer) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    entry = tg_peer_cache_add(out, peer.peer_constructor, peer.id_hi,
                              peer.id_lo);
    if (entry == 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    entry->from_dialog = 1;
    entry->top_message = 0UL;
    entry->unread_count = 0UL;

    (void)tg_peer_cache_scan_chats(body, body_length, out);
    (void)tg_peer_cache_scan_users(body, body_length, out);

    out->user_count = 0UL;
    out->chat_count = 0UL;
    for (i = 0UL; i < out->count; ++i) {
        if (out->entries[i].peer_constructor ==
            TG_PEER_USER_CONSTRUCTOR) {
            ++out->user_count;
        } else if (out->entries[i].peer_constructor ==
                       TG_PEER_CHAT_CONSTRUCTOR ||
                   out->entries[i].peer_constructor ==
                       TG_PEER_CHANNEL_CONSTRUCTOR) {
            ++out->chat_count;
        }
    }
    out->total_dialog_count = out->count;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_contacts_search_peer_cache(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_peer_cache *out)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_dialog_peer peer;
    tg_mtproto_peer_cache_entry *entry;
    unsigned long count;
    unsigned long i;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (constructor != TG_CONTACTS_FOUND_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);

    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < count; ++i) {
        memset(&peer, 0, sizeof(peer));
        if (tg_read_peer_ref(&reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        entry = tg_peer_cache_add(out, peer.peer_constructor, peer.id_hi,
                                  peer.id_lo);
        if (entry != 0) {
            entry->from_dialog = 1;
        }
    }

    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    for (i = 0UL; i < count; ++i) {
        memset(&peer, 0, sizeof(peer));
        if (tg_read_peer_ref(&reader, &peer) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        entry = tg_peer_cache_add(out, peer.peer_constructor, peer.id_hi,
                                  peer.id_lo);
        if (entry != 0) {
            entry->from_dialog = 1;
        }
    }

    (void)tg_peer_cache_scan_chats(body, body_length, out);
    (void)tg_peer_cache_scan_users(body, body_length, out);

    out->user_count = 0UL;
    out->chat_count = 0UL;
    for (i = 0UL; i < out->count; ++i) {
        if (out->entries[i].peer_constructor == TG_PEER_USER_CONSTRUCTOR) {
            ++out->user_count;
        } else if (out->entries[i].peer_constructor ==
                       TG_PEER_CHAT_CONSTRUCTOR ||
                   out->entries[i].peer_constructor ==
                       TG_PEER_CHANNEL_CONSTRUCTOR) {
            ++out->chat_count;
        }
    }
    out->total_dialog_count = out->count;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_messages_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_messages_summary *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long scratch;

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->constructor = constructor;
    tg_mtproto_tl_reader_init(&reader, body, body_length);

    if (constructor == TG_MESSAGES_MESSAGES_NOT_MODIFIED_CONSTRUCTOR) {
        out->is_not_modified = 1;
        return tg_mtproto_tl_read_u32(&reader, &out->count);
    }
    if (constructor == TG_MESSAGES_MESSAGES_SLICE_CONSTRUCTOR) {
        out->is_slice = 1;
        if (tg_mtproto_tl_read_u32(&reader, &out->flags) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &out->count) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((out->flags & 1UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((out->flags & 4UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((out->flags & 8UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor == TG_MESSAGES_CHANNEL_MESSAGES_CONSTRUCTOR) {
        out->is_channel_messages = 1;
        if (tg_mtproto_tl_read_u32(&reader, &out->flags) !=
                TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &out->count) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((out->flags & 4UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor != TG_MESSAGES_MESSAGES_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(&reader, &out->message_count) !=
            TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (out->message_count != 0UL &&
        (reader.length - reader.offset < 4UL ||
         tg_read_u32_le(reader.buffer + reader.offset) !=
             TG_VECTOR_CONSTRUCTOR)) {
        return TG_MTPROTO_TL_OK;
    }
    if (constructor == TG_MESSAGES_CHANNEL_MESSAGES_CONSTRUCTOR &&
        tg_read_vector_count(&reader, &scratch) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_read_vector_count(&reader, &out->chat_count) != TG_MTPROTO_TL_OK ||
        tg_read_vector_count(&reader, &out->user_count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_message_text_list(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_message_text_list *out)
{
    tg_mtproto_tl_reader reader;
    tg_mtproto_message_text message;
    unsigned long flags;
    unsigned long count;
    unsigned long scratch;
    unsigned long i;
    unsigned long message_start;
    unsigned long last_seen_id = 0UL; /* id of the last message read; resync bound */

    if (body == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, body, body_length);

    if (constructor == TG_MESSAGES_MESSAGES_NOT_MODIFIED_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    if (constructor == TG_MESSAGES_MESSAGES_SLICE_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(&reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &out->total_message_count) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 1UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 4UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 8UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor == TG_MESSAGES_CHANNEL_MESSAGES_CONSTRUCTOR) {
        if (tg_mtproto_tl_read_u32(&reader, &flags) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK ||
            tg_mtproto_tl_read_u32(&reader, &out->total_message_count) !=
                TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
        if ((flags & 4UL) != 0UL &&
            tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_INVALID_DATA;
        }
    } else if (constructor != TG_MESSAGES_MESSAGES_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (tg_read_vector_count(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (constructor == TG_MESSAGES_MESSAGES_CONSTRUCTOR) {
        out->total_message_count = count;
    }
    out->page_count = count; /* diag: messages actually in the fetched vector */
    i = 0UL;
    while (i < count && reader.offset < reader.length) {
        unsigned long peek_constructor =
            (reader.offset + 4UL <= reader.length) ?
                tg_read_u32_le(reader.buffer + reader.offset) : 0UL;
        message_start = reader.offset;
        if (tg_read_common_message_text(&reader, &message, 0) !=
            TG_MTPROTO_TL_OK) {
            /*
             * Older parser paths can stop on media tails such as
             * messageMediaDocument. Try to resync to the next TL Message
             * constructor so /history can keep showing the surrounding text.
             */
            if (out->abort_constructor == 0UL) {
                out->abort_constructor = peek_constructor;
            }
            ++out->resync_attempts;
            if (tg_message_text_resync(&reader, message_start + 4UL,
                                       last_seen_id)) {
                ++out->resync_ok;
                continue;
            }
            return TG_MTPROTO_TL_OK;
        }
        if (message.id != 0UL) {
            last_seen_id = message.id; /* descending; bounds the next resync */
        }
        if (message.has_text) {
            if (out->count < TG_MTPROTO_MESSAGE_TEXT_LIST_MAX) {
                out->messages[out->count] = message;
                ++out->count;
            } else {
                out->truncated = 1;
            }
        }
        ++i;
    }
    if (count > TG_MTPROTO_MESSAGE_TEXT_LIST_MAX) {
        out->truncated = 1;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_parse_updates_summary(
    unsigned long constructor,
    const unsigned char *body,
    unsigned long body_length,
    tg_mtproto_updates_summary *out)
{
    tg_mtproto_tl_reader reader;
    unsigned long scratch;

    if (out == 0 || (body == 0 && body_length > 0UL)) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->constructor = constructor;
    if (constructor != TG_UPDATE_SHORT_SENT_MESSAGE_CONSTRUCTOR) {
        return TG_MTPROTO_TL_OK;
    }
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &out->flags) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->id) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &scratch) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_read_u32(&reader, &out->date) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    out->has_sent_message = 1;
    return TG_MTPROTO_TL_OK;
}

/* Reads peer + max_id from a reader positioned JUST AFTER the
   updateReadHistoryOutbox constructor; pts/pts_count are left unread. Shared by
   the body parser (below) and the live update-stream dispatch (probe.c), which
   reaches this point with the item constructor already consumed. */
tg_mtproto_tl_status tg_mtproto_read_update_read_history_outbox(
    tg_mtproto_tl_reader *reader, tg_mtproto_dialog_peer *out_peer,
    unsigned long *out_max_id)
{
    tg_mtproto_tl_status status;

    if (reader == 0 || out_peer == 0 || out_max_id == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    *out_max_id = 0UL;
    status = tg_read_peer_ref(reader, out_peer);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_read_u32(reader, out_max_id);
}

/* Parses a bare updateReadHistoryOutbox#2f2f21bf body into the peer + max_id (the
   read cursor for OUR sent messages); pts/pts_count are ignored. Pure + testable;
   the live push path uses the parsed peer to flip own messages to "seen" at once
   when it matches the open chat. */
tg_mtproto_tl_status tg_mtproto_parse_update_read_history_outbox(
    const unsigned char *body, unsigned long body_length,
    tg_mtproto_dialog_peer *out_peer, unsigned long *out_max_id)
{
    tg_mtproto_tl_reader reader;
    unsigned long constructor;

    if (out_peer == 0 || out_max_id == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    *out_max_id = 0UL;
    tg_mtproto_tl_reader_init(&reader, body, body_length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    return tg_mtproto_read_update_read_history_outbox(&reader, out_peer,
                                                      out_max_id);
}

int tg_mtproto_is_auth_authorization_constructor(unsigned long constructor)
{
    return constructor == TG_AUTH_AUTHORIZATION_CONSTRUCTOR ||
           constructor == TG_AUTH_AUTHORIZATION_SIGNUP_REQUIRED_CONSTRUCTOR;
}

/* Directly exercises tg_apply_entity_markers, including the UTF-16 -> UTF-8
   offset mapping that the TL-level "*group* reply" case does not reach. */
#if !defined(TG_NO_SELFTEST)
static int tg_entity_marker_case(const char *in, const tg_msg_entity *ents,
                                 int count, const char *expect)
{
    char buf[TG_MTPROTO_MESSAGE_TEXT_MAX];
    unsigned long i;

    for (i = 0UL; i + 1UL < sizeof(buf) && in[i] != '\0'; ++i) {
        buf[i] = in[i];
    }
    buf[i] = '\0';
    tg_apply_entity_markers(buf, sizeof(buf), ents, count);
    return strcmp(buf, expect) == 0;
}

static int tg_entity_marker_self_test(void)
{
    tg_msg_entity e[2];

    /* Two non-overlapping spans, plain ASCII. */
    e[0].type = TG_MSG_ENT_BOLD;
    e[0].off = 0UL;
    e[0].len = 5UL;
    e[1].type = TG_MSG_ENT_ITALIC;
    e[1].off = 6UL;
    e[1].len = 5UL;
    if (!tg_entity_marker_case("group reply", e, 2, "*group* _reply_")) {
        return 2;
    }
    /* Inline code marker. */
    e[0].type = TG_MSG_ENT_CODE;
    e[0].off = 0UL;
    e[0].len = 4UL;
    if (!tg_entity_marker_case("code here", e, 1, "`code` here")) {
        return 2;
    }
    /* UTF-16 offset 1 sits AFTER a 2-byte UTF-8 char (U+00E9 'e-acute'); the
       bold span must land on 'b', not be byte-mapped to mid-sequence. */
    e[0].type = TG_MSG_ENT_BOLD;
    e[0].off = 1UL;
    e[0].len = 1UL;
    if (!tg_entity_marker_case("\xC3\xA9" "b", e, 1, "\xC3\xA9" "*b*")) {
        return 2;
    }
    /* An astral emoji (U+1F600) is 2 UTF-16 units; the span on the next char
       must start at offset 2, not 1. */
    e[0].type = TG_MSG_ENT_BOLD;
    e[0].off = 2UL;
    e[0].len = 1UL;
    if (!tg_entity_marker_case("\xF0\x9F\x98\x80" "X", e, 1,
                               "\xF0\x9F\x98\x80" "*X*")) {
        return 2;
    }
    return 0;
}

int tg_mtproto_login_self_test(void)
{
    static const unsigned char expected_send_code[] = {
        0x4fU, 0x24U, 0x77U, 0xa6U,
        0x0bU, '+', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        0x2aU, 0x00U, 0x00U, 0x00U,
        0x08U, 'a', 'p', 'i', 'h', 'a', 's', 'h', '1',
        0x00U, 0x00U, 0x00U,
        0x78U, 0x3dU, 0x25U, 0xadU,
        0x00U, 0x00U, 0x00U, 0x00U
    };
    static const unsigned char expected_sign_in[] = {
        0x51U, 0xa9U, 0x52U, 0x8dU,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x02U, '+', '1', 0x00U,
        0x04U, 'h', 'a', 's', 'h', 0x00U, 0x00U, 0x00U,
        0x05U, '1', '2', '3', '4', '5', 0x00U, 0x00U
    };
    static const unsigned char srp_salt1[] = { 0x11U, 0x12U };
    static const unsigned char srp_salt2[] = { 0x21U, 0x22U, 0x23U };
    static const unsigned char srp_p[] = { 0x31U, 0x32U, 0x33U, 0x34U };
    static const unsigned char srp_b[] = { 0x41U, 0x42U, 0x43U };
    static const unsigned char srp_a[] = { 0x51U, 0x52U, 0x53U, 0x54U };
    unsigned char query[128];
    unsigned char initialized[192];
    unsigned char wrapped[160];
    unsigned char rpc[64];
    unsigned char peer_rpc[512];
    unsigned char m1[TG_MTPROTO_SHA256_LENGTH];
    char error_text[32];
    long error_code;
    tg_mtproto_bad_msg_notification bad_msg;
    tg_mtproto_config_summary config;
    tg_mtproto_dialog_peer_list peer_list;
    tg_mtproto_dialogs_summary dialogs;
    static tg_mtproto_message_text_list text_list;
    tg_mtproto_peer_cache peer_cache;
    tg_mtproto_messages_summary messages;
    tg_mtproto_password_summary password;
    tg_mtproto_rpc_result result;
    tg_mtproto_sent_code sent_code;
    tg_mtproto_updates_summary updates;
    tg_mtproto_user_summary user;
    tg_mtproto_tl_writer writer;

    if (tg_entity_marker_self_test() != 0) {
        return 2;
    }
#if !defined(TG_NO_GUI)
    if (tg_avatar_self_test() != 0) {
        return 2;
    }
#endif
    /* F9 chunk 2: the document label formatter (name + human size). */
    {
        tg_mtproto_document_meta d;
        char lbl[64];

        memset(&d, 0, sizeof(d));
        d.has_document = 1;
        d.size_lo = 2048UL; /* 2 KB */
        strcpy(d.file_name, "notes.txt");
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[File: notes.txt (2 KB)]") != 0) {
            puts("f9 self-test: document label (KB) mismatch");
            return 2;
        }
        d.size_lo = 5UL; /* 5 B */
        d.file_name[0] = '\0'; /* no name -> "File" */
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[File: File (5 B)]") != 0) {
            puts("f9 self-test: document label (no-name/B) mismatch");
            return 2;
        }
        d.size_hi = 0UL;
        d.size_lo = 3UL * 1048576UL; /* 3 MB */
        strcpy(d.file_name, "clip.mp4");
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[File: clip.mp4 (3 MB)]") != 0) {
            puts("f9 self-test: document label (MB) mismatch");
            return 2;
        }
        strcpy(lbl, "commento");
        tg_mtproto_append_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "commento\n[File: clip.mp4 (3 MB)]") != 0) {
            puts("f9 self-test: caption plus document label mismatch");
            return 2;
        }
    }

    /* 0.0.92: the kinds people recognise. The parser keeps the sticker emoji,
       the clip length and the geometry it used to walk past, and the label
       says what the thing is instead of naming a .webp nobody asked about. */
    {
        tg_mtproto_document_meta d;
        char lbl[80];

        /* A sticker is its emoji, with no name and no size. */
        memset(&d, 0, sizeof(d));
        d.has_document = 1;
        d.size_lo = 14336UL;
        strcpy(d.file_name, "sticker.webp");
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_STICKER |
                                             TG_MTPROTO_DOC_ATTR_IMAGE);
        strcpy(d.alt, "\xf0\x9f\x98\x80"); /* grinning face */
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Sticker \xf0\x9f\x98\x80]") != 0) {
            puts("0.0.92 self-test: sticker label mismatch");
            return 2;
        }
        d.alt[0] = '\0'; /* a pack with no alt still reads as a sticker */
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Sticker]") != 0) {
            puts("0.0.92 self-test: sticker label without alt mismatch");
            return 2;
        }

        /* Video and GIF differ only by the animated attribute. */
        memset(&d, 0, sizeof(d));
        d.has_document = 1;
        d.size_lo = 3UL * 1048576UL;
        strcpy(d.file_name, "VID_0001.mp4");
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_VIDEO);
        d.duration = 92UL;
        d.width = 640UL;
        d.height = 360UL;
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Video 1:32 640x360 (3 MB)]") != 0) {
            puts("0.0.92 self-test: video label mismatch");
            return 2;
        }
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_VIDEO |
                                             TG_MTPROTO_DOC_ATTR_ANIMATED);
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[GIF 1:32 640x360 (3 MB)]") != 0) {
            puts("0.0.92 self-test: gif label mismatch");
            return 2;
        }
        d.duration = 3725UL; /* past the hour: h:mm:ss */
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_VIDEO);
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Video 1:02:05 640x360 (3 MB)]") != 0) {
            puts("0.0.92 self-test: long video label mismatch");
            return 2;
        }

        /* Voice keeps no name; music keeps its own. */
        memset(&d, 0, sizeof(d));
        d.has_document = 1;
        d.size_lo = 30UL * 1024UL;
        strcpy(d.file_name, "audio.ogg");
        d.duration = 7UL;
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_VOICE);
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Voice 0:07 (30 KB)]") != 0) {
            puts("0.0.92 self-test: voice label mismatch");
            return 2;
        }
        strcpy(d.file_name, "song.mp3");
        d.duration = 225UL;
        d.size_lo = 5UL * 1048576UL;
        d.kind = tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_AUDIO);
        tg_mtproto_format_document_label(&d, lbl, sizeof(lbl));
        if (strcmp(lbl, "[Audio: song.mp3 3:45 (5 MB)]") != 0) {
            puts("0.0.92 self-test: audio label mismatch");
            return 2;
        }

        /* A GIF arrives as video AND animated, a voice note as audio with the
           voice flag: the kind is a priority, not the attribute seen last. */
        if (tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_VIDEO |
                                        TG_MTPROTO_DOC_ATTR_ANIMATED) !=
                (unsigned char)TG_MTPROTO_DOC_KIND_GIF ||
            tg_mtproto_document_kind_of(TG_MTPROTO_DOC_ATTR_AUDIO |
                                        TG_MTPROTO_DOC_ATTR_VOICE) !=
                (unsigned char)TG_MTPROTO_DOC_KIND_VOICE ||
            tg_mtproto_document_kind_of(0UL) !=
                (unsigned char)TG_MTPROTO_DOC_KIND_FILE) {
            puts("0.0.92 self-test: document kind priority mismatch");
            return 2;
        }

        /* Durations arrive as IEEE 754 doubles and these lanes have no FPU to
           spare: 92.5 -> 92, 0.4 -> 0, and the high word carries it all. */
        if (tg_tl_double_seconds(0x40572000UL, 0UL) != 92UL ||
            tg_tl_double_seconds(0x3fd99999UL, 0x9999999aUL) != 0UL ||
            tg_tl_double_seconds(0x3ff00000UL, 0UL) != 1UL ||
            tg_tl_double_seconds(0UL, 0UL) != 0UL) {
            puts("0.0.92 self-test: TL double to seconds mismatch");
            return 2;
        }

        /* An alt cut mid-emoji by the fixed buffer must lose the whole
           codepoint, never leave half of one behind. */
        {
            char alt[8];

            strcpy(alt, "ab\xf0\x9f\x98");  /* 4 byte emoji, 3 bytes of it */
            tg_trim_utf8_tail(alt);
            if (strcmp(alt, "ab") != 0) {
                puts("0.0.92 self-test: utf-8 tail trim mismatch");
                return 2;
            }
            strcpy(alt, "a\xc3\xa8");        /* whole 2 byte sequence: kept */
            tg_trim_utf8_tail(alt);
            if (strcmp(alt, "a\xc3\xa8") != 0) {
                puts("0.0.92 self-test: utf-8 tail trim ate a whole codepoint");
                return 2;
            }
        }
    }

    /* 0.0.92 link previews: a pasted link used to draw as the bare URL, with
       the preview the server built for it skipped whole. Walk a synthetic
       webPage off the wire and check the two lines it adds, the flag order it
       depends on, and that the three empty forms stay silent. */
    {
        unsigned char wire[256];
        tg_mtproto_tl_writer ww;
        tg_mtproto_tl_reader wr;
        static tg_mtproto_message_text msg;
        tg_mtproto_tl_status ws;

        tg_mtproto_tl_writer_init(&ww, wire, sizeof(wire));
        ws = tg_mtproto_tl_write_u32(&ww, 0xe89c45b2UL); /* webPage */
        /* flags: type(0) site_name(1) title(2) description(3), no photo */
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 15UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u64(&ww, 0UL, 7UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww,
            "https://www.morphos-team.net/");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww,
            "morphos-team.net");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 0UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww, "article");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww, "MorphOS Team");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww,
            "MorphOS 3.19 released");
        /* Two paragraphs: the bubble takes the first line only. */
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww,
            "The team is pleased to announce.\nSecond paragraph.");
        if (ws != TG_MTPROTO_TL_OK) {
            puts("0.0.92 self-test: could not build the webPage wire");
            return 2;
        }
        memset(&msg, 0, sizeof(msg));
        strcpy(msg.text, "https://www.morphos-team.net/");
        msg.has_text = 1;
        tg_mtproto_tl_reader_init(&wr, wire, ww.length);
        tg_mtproto_append_web_page(&wr, &msg);
        if (strcmp(msg.text,
                   "https://www.morphos-team.net/\n"
                   "[Link: MorphOS Team - MorphOS 3.19 released]\n"
                   "The team is pleased to announce.") != 0) {
            printf("0.0.92 self-test: web page preview is \"%s\"\n", msg.text);
            return 2;
        }
        if (msg.photo.has_photo) {
            puts("0.0.92 self-test: web page invented a photo");
            return 2;
        }

        /* Title but no site name, and no description. */
        tg_mtproto_tl_writer_init(&ww, wire, sizeof(wire));
        ws = tg_mtproto_tl_write_u32(&ww, 0xe89c45b2UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 4UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u64(&ww, 0UL, 8UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww, "http://a.b/");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww, "a.b");
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 0UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_write_string(&ww, "Solo titolo");
        if (ws != TG_MTPROTO_TL_OK) {
            puts("0.0.92 self-test: could not build the title-only wire");
            return 2;
        }
        memset(&msg, 0, sizeof(msg));
        strcpy(msg.text, "http://a.b/");
        msg.has_text = 1;
        tg_mtproto_tl_reader_init(&wr, wire, ww.length);
        tg_mtproto_append_web_page(&wr, &msg);
        if (strcmp(msg.text, "http://a.b/\n[Link: Solo titolo]") != 0) {
            printf("0.0.92 self-test: title-only preview is \"%s\"\n",
                   msg.text);
            return 2;
        }

        /* The server is still fetching it: say nothing rather than guess. */
        tg_mtproto_tl_writer_init(&ww, wire, sizeof(wire));
        ws = tg_mtproto_tl_write_u32(&ww, 0xb0d13e47UL); /* webPagePending */
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 0UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u64(&ww, 0UL, 9UL);
        if (ws == TG_MTPROTO_TL_OK) ws = tg_mtproto_tl_write_u32(&ww, 0UL);
        if (ws != TG_MTPROTO_TL_OK) {
            puts("0.0.92 self-test: could not build the pending wire");
            return 2;
        }
        memset(&msg, 0, sizeof(msg));
        strcpy(msg.text, "http://a.b/");
        msg.has_text = 1;
        tg_mtproto_tl_reader_init(&wr, wire, ww.length);
        tg_mtproto_append_web_page(&wr, &msg);
        if (strcmp(msg.text, "http://a.b/") != 0) {
            printf("0.0.92 self-test: pending preview wrote \"%s\"\n",
                   msg.text);
            return 2;
        }
    }

    /* 0.0.9 photo metadata: parse a full layer-214 Photo, select the nearest
       bounded server thumb (including a progressive size), then pin the
       inputPhotoFileLocation byte layout. */
    {
        unsigned char photo_wire[256];
        unsigned char query[80];
        unsigned char ref_byte[1];
        static const unsigned char stripped_thumb[5] = {
            0x01U, 0x08U, 0x0cU, 0xaaU, 0xbbU
        };
        tg_mtproto_tl_writer pw;
        tg_mtproto_tl_writer qw;
        tg_mtproto_tl_reader pr;
        tg_mtproto_photo_meta photo;
        tg_mtproto_tl_status ps;
        const char *expected_inline_type;
        unsigned long expected_inline_width;
        unsigned long expected_inline_height;
        unsigned long expected_inline_size;
        const char *expected_large_type;
        unsigned long expected_large_width;
        unsigned long expected_large_height;
        unsigned long expected_large_size;
        int progressive_found;
        unsigned long variant_at;

        ref_byte[0] = 0xfeU;
#if defined(__m68k__)
        expected_inline_type = "m";
        expected_inline_width = 160UL;
        expected_inline_height = 100UL;
        expected_inline_size = 50000UL;
        expected_large_type = "m";
        expected_large_width = 160UL;
        expected_large_height = 100UL;
        expected_large_size = 50000UL;
#else
        expected_inline_type = "y";
        expected_inline_width = 1280UL;
        expected_inline_height = 800UL;
        expected_inline_size = 900000UL;
        expected_large_type = "y";
        expected_large_width = 1280UL;
        expected_large_height = 800UL;
        expected_large_size = 900000UL;
#endif
        tg_mtproto_tl_writer_init(&pw, photo_wire, sizeof(photo_wire));
        ps = tg_mtproto_tl_write_u32(&pw, 0xfb197a65UL); /* photo */
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 0UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u64(
            &pw, 0x01020304UL, 0x05060708UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u64(
            &pw, 0x0a0b0c0dUL, 0x0e0f1011UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_bytes(
            &pw, ref_byte, sizeof(ref_byte));
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 123UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, TG_VECTOR_CONSTRUCTOR);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 5UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, 0xe0b0bc2eUL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_write_string(&pw, "i");
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_bytes(
            &pw, stripped_thumb, sizeof(stripped_thumb));
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, 0x75c78e60UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_write_string(&pw, "s");
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 80UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 50UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 10000UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, 0x75c78e60UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_write_string(&pw, "m");
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 160UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 100UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 50000UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, 0xfa3efb95UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_write_string(&pw, "x");
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 800UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 500UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, TG_VECTOR_CONSTRUCTOR);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 2UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 120000UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 500000UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(
            &pw, 0x75c78e60UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_write_string(&pw, "y");
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 1280UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 800UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 900000UL);
        if (ps == TG_MTPROTO_TL_OK) ps = tg_mtproto_tl_write_u32(&pw, 4UL);
        if (ps != TG_MTPROTO_TL_OK) {
            puts("photo self-test: fixture build failed");
            return 2;
        }
        tg_mtproto_tl_reader_init(&pr, photo_wire, pw.length);
        progressive_found = 0;
        if (tg_mtproto_read_photo(&pr, &photo) == TG_MTPROTO_TL_OK) {
            for (variant_at = 0UL; variant_at < photo.variant_count;
                 ++variant_at) {
                if (strcmp(photo.variants[variant_at].type, "x") == 0 &&
                    photo.variants[variant_at].progressive) {
                    progressive_found = 1;
                }
            }
        } else {
            puts("photo self-test: Photo parse failed");
            return 2;
        }
        if (
            pr.offset != pw.length || !photo.has_photo ||
            strcmp(photo.thumb_type, expected_inline_type) != 0 ||
            photo.width != expected_inline_width ||
            photo.height != expected_inline_height ||
            photo.size != expected_inline_size ||
            !photo.has_large ||
            strcmp(photo.large_thumb_type, expected_large_type) != 0 ||
            photo.large_width != expected_large_width ||
            photo.large_height != expected_large_height ||
            photo.large_size != expected_large_size ||
            photo.variant_count != 4UL || !progressive_found ||
            photo.dc_id != 4UL || photo.file_reference_len != 1UL ||
            photo.file_reference[0] != 0xfeU ||
            photo.stripped_len != sizeof(stripped_thumb) ||
            memcmp(photo.stripped, stripped_thumb,
                   sizeof(stripped_thumb)) != 0) {
            puts("photo self-test: Photo parse/selection mismatch");
            return 2;
        }
        tg_mtproto_tl_writer_init(&qw, query, sizeof(query));
        if (tg_mtproto_build_upload_get_photo(&qw, &photo, 0UL, 65536UL) !=
                TG_MTPROTO_TL_OK ||
            qw.length != 48UL || query[0] != 0xbeU || query[3] != 0xbeU ||
            query[8] != 0xfeU || query[9] != 0x1fU ||
            query[10] != 0x18U || query[11] != 0x40U ||
            query[28] != 0x01U || query[29] != 0xfeU ||
            query[32] != 0x01U ||
            query[33] != (unsigned char)expected_inline_type[0] ||
            query[44] != 0x00U || query[46] != 0x01U) {
            puts("photo self-test: getFile(photo) layout mismatch");
            return 2;
        }
    }

    /* F9 chunk 1: byte-verify the document TL (layer-214 shapes). */    /* F9 chunk 1: byte-verify the document TL (layer-214 shapes). */
    {
        unsigned char q[160];
        tg_mtproto_tl_writer w;
        static const unsigned char part_expected[20] = {
            0x21, 0xa6, 0x04, 0xb3,                         /* saveFilePart */
            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* file_id */
            0x02, 0x00, 0x00, 0x00,                         /* part 2 */
            0x03, 0xaa, 0xbb, 0xcc                          /* bytes {3} */
        };
        static const unsigned char part_data[3] = { 0xaa, 0xbb, 0xcc };
        static const unsigned char big_part_expected[24] = {
            0x3d, 0x67, 0x7b, 0xde,                         /* saveBigFilePart */
            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* file_id */
            0x02, 0x00, 0x00, 0x00,                         /* part 2 */
            0x09, 0x00, 0x00, 0x00,                         /* total 9 */
            0x03, 0xaa, 0xbb, 0xcc                          /* bytes {3} */
        };
        tg_mtproto_document_meta doc;

        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_save_file_part(&w, 0x11223344UL,
                                                   0x55667788UL, 2UL,
                                                   part_data, 3UL) !=
                TG_MTPROTO_TL_OK ||
            w.length != sizeof(part_expected) ||
            memcmp(q, part_expected, sizeof(part_expected)) != 0) {
            puts("f9 self-test: saveFilePart bytes mismatch");
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_save_big_file_part(
                &w, 0x11223344UL, 0x55667788UL, 2UL, 9UL,
                part_data, 3UL) != TG_MTPROTO_TL_OK ||
            w.length != sizeof(big_part_expected) ||
            memcmp(q, big_part_expected, sizeof(big_part_expected)) != 0) {
            puts("f9 self-test: saveBigFilePart bytes mismatch");
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_save_big_file_part(
                &w, 0x11223344UL, 0x55667788UL, 9UL, 9UL,
                part_data, 3UL) != TG_MTPROTO_TL_INVALID_ARGUMENT) {
            puts("f9 self-test: saveBigFilePart accepted part == total");
            return 2;
        }
        memset(&doc, 0, sizeof(doc));
        doc.has_document = 1;
        doc.id_hi = 0x01020304UL;
        doc.id_lo = 0x05060708UL;
        doc.access_hash_hi = 0x0a0b0c0dUL;
        doc.access_hash_lo = 0x0e0f1011UL;
        doc.file_reference[0] = 0xfe;
        doc.file_reference_len = 1UL;
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_get_document(&w, &doc, 0UL, 65536UL) !=
                TG_MTPROTO_TL_OK ||
            w.length != 48UL ||
            q[0] != 0xbeU || q[3] != 0xbeU ||        /* upload.getFile */
            q[8] != 0x84U || q[9] != 0x75U ||        /* inputDocFileLoc LE */
            q[10] != 0xd0U || q[11] != 0xbaU ||
            q[28] != 0x01U || q[29] != 0xfeU ||      /* ref: len 1 + 0xfe */
            q[44] != 0x00U || q[46] != 0x01U) {      /* limit 65536 LE */
            puts("f9 self-test: getFile(document) layout mismatch");
            return 2;
        }
        /* no file_reference -> must refuse before the wire */
        doc.file_reference_len = 0UL;
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_get_document(&w, &doc, 0UL, 65536UL) !=
            TG_MTPROTO_TL_INVALID_ARGUMENT) {
            puts("f9 self-test: missing file_reference must be rejected");
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_messages_send_media_document(
                &w, TG_PEER_USER_CONSTRUCTOR, 0UL, 1UL, 0UL, 2UL, 1,
                0x0000deadUL, 0x0000beefUL, 3UL, "b.txt", "text/plain", 0,
                0x11UL, 0x22UL) != TG_MTPROTO_TL_OK ||
            q[0] != 0xc1U || q[1] != 0xd9U ||        /* sendMedia LAYER 214 */
            q[2] != 0x55U || q[3] != 0xacU ||
            q[28] != 0x30U || q[29] != 0x93U ||      /* inputMediaUploadedDoc */
            q[30] != 0x7cU || q[31] != 0x03U ||
            q[32] != 0x10U ||                        /* force_file flag */
            q[36] != 0x7fU || q[37] != 0xf2U) {      /* inputFile LE */
            puts("f9 self-test: sendMedia(document) layout mismatch");
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_messages_send_media_photo(
                &w, TG_PEER_USER_CONSTRUCTOR, 0UL, 1UL, 0UL, 2UL, 1,
                0x0000deadUL, 0x0000beefUL, 3UL, "p.jpg", 0,
                0x11UL, 0x22UL) != TG_MTPROTO_TL_OK ||
            w.length != 76UL ||
            q[0] != 0xc1U || q[1] != 0xd9U || q[2] != 0x55U ||
            q[3] != 0xacU ||
            q[28] != 0x04U || q[29] != 0x7dU ||
            q[30] != 0x28U || q[31] != 0x1eU ||
            q[32] != 0x00U ||
            q[36] != 0x7fU || q[37] != 0xf2U ||
            q[38] != 0x2fU || q[39] != 0xf5U ||
            q[48] != 0x03U || q[52] != 0x05U ||
            q[53] != (unsigned char)'p' ||
            q[60] != 0x00U || q[64] != 0x00U ||
            q[68] != 0x22U || q[72] != 0x11U) {
            puts("photo send self-test: sendMedia(photo) layout mismatch");
            return 2;
        }
        /* Caption run (0.0.91): "Hi!" lands in the message field. A 3-byte
           TL string is 1 length byte + 3 bytes, already a multiple of four,
           so it fills exactly the four bytes the empty string used and the
           random_id stays put. */
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_messages_send_media_photo(
                &w, TG_PEER_USER_CONSTRUCTOR, 0UL, 1UL, 0UL, 2UL, 1,
                0x0000deadUL, 0x0000beefUL, 3UL, "p.jpg", "Hi!",
                0x11UL, 0x22UL) != TG_MTPROTO_TL_OK ||
            w.length != 76UL ||
            q[64] != 0x03U ||
            q[65] != (unsigned char)'H' ||
            q[66] != (unsigned char)'i' ||
            q[67] != (unsigned char)'!' ||
            q[68] != 0x22U || q[72] != 0x11U) {
            puts("photo caption self-test: caption bytes mismatch");
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_messages_send_media_big_document(
                &w, TG_PEER_USER_CONSTRUCTOR, 0UL, 1UL, 0UL, 2UL, 1,
                0x0000deadUL, 0x0000beefUL, 9UL, "b.bin",
                "application/octet-stream", "Hi!", 0x11UL, 0x22UL) !=
                TG_MTPROTO_TL_OK ||
            q[36] != 0xb5U || q[37] != 0x0bU ||
            q[38] != 0x4fU || q[39] != 0xfaU) {
            puts("f9 self-test: inputFileBig layout mismatch");
            return 2;
        }
    }

    /* Avatar v2: byte-verify upload.getFile(inputPeerPhotoFileLocation) --
       especially the 8-byte offset (the wire trap) and the tail layout. */
    {
        unsigned char q[64];
        tg_mtproto_tl_writer w;

        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_get_peer_photo(
                &w, TG_PEER_USER_CONSTRUCTOR, 0x00000001UL, 0x02030405UL,
                0x0a0b0c0dUL, 0x0e0f1011UL, 1, 0x55667788UL, 0x11223344UL,
                0UL, 65536UL) != TG_MTPROTO_TL_OK ||
            w.length != 56UL ||
            q[0] != 0xbeU || q[1] != 0x35U || q[2] != 0x53U || q[3] != 0xbeU ||
            q[8] != 0x99U || q[9] != 0x7eU || q[10] != 0x25U ||
            q[11] != 0x37U ||
            q[36] != 0x44U || q[37] != 0x33U || q[38] != 0x22U ||
            q[44] != 0x00U || q[48] != 0x00U || q[51] != 0x00U ||
            q[52] != 0x00U || q[53] != 0x00U || q[54] != 0x01U ||
            q[55] != 0x00U) {
            return 2;
        }
        tg_mtproto_tl_writer_init(&w, q, sizeof(q));
        if (tg_mtproto_build_upload_get_peer_photo(
                &w, TG_PEER_USER_CONSTRUCTOR, 0UL, 1UL, 0UL, 0UL, 1, 0UL, 1UL,
                0UL, 12345UL) != TG_MTPROTO_TL_INVALID_ARGUMENT) {
            return 2;
        }
    }

    /* Avatar v1: byte-verified userProfilePhoto capture (photo_id lo/hi wire
       order, stripped thumb via TL bytes short-form + padding, dc_id). */
    {
        static const unsigned char photo_blob[] = {
            0x06, 0xf7, 0xd1, 0x82,             /* userProfilePhoto (LE) */
            0x02, 0x00, 0x00, 0x00,             /* flags: stripped_thumb */
            0x44, 0x33, 0x22, 0x11,             /* photo_id lo */
            0x88, 0x77, 0x66, 0x55,             /* photo_id hi */
            0x03, 0x01, 0x08, 0x08,             /* bytes len 3: 01 08 08 */
            0x04, 0x00, 0x00, 0x00              /* dc_id = 4 */
        };
        tg_mtproto_tl_reader reader;
        tg_mtproto_user_summary u;

        memset(&u, 0, sizeof(u));
        tg_mtproto_tl_reader_init(&reader, photo_blob, sizeof(photo_blob));
        if (tg_read_user_profile_photo(&reader, &u) != TG_MTPROTO_TL_OK ||
            u.photo_id_hi != 0x55667788UL || u.photo_id_lo != 0x11223344UL ||
            u.photo_dc_id != 4UL || u.stripped_len != 3UL ||
            u.stripped[0] != 0x01U || u.stripped[1] != 0x08U ||
            u.stripped[2] != 0x08U || reader.offset != sizeof(photo_blob)) {
            return 2;
        }
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_send_code(&writer, "+1234567890", 42UL,
                                        "apihash1") != TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_send_code) ||
        memcmp(query, expected_send_code, sizeof(expected_send_code)) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, wrapped, sizeof(wrapped));
    if (tg_mtproto_build_invoke_with_layer(&writer, TG_MTPROTO_CURRENT_LAYER,
                                           query,
                                           sizeof(expected_send_code)) !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_send_code) + 8UL ||
        wrapped[0] != 0x0dU || wrapped[1] != 0x0dU ||
        wrapped[2] != 0x9bU || wrapped[3] != 0xdaU ||
        wrapped[4] != 214U) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, initialized, sizeof(initialized));
    if (tg_mtproto_build_init_connection(&writer, 42UL, "Amiga",
                                         "portable", "0.1", "en", query,
                                         sizeof(expected_send_code)) !=
            TG_MTPROTO_TL_OK ||
        initialized[0] != 0xa9U || initialized[1] != 0x5eU ||
        initialized[2] != 0xcdU || initialized[3] != 0xc1U) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_sign_in(&writer, "+1", "hash", "12345") !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_sign_in) ||
        memcmp(query, expected_sign_in, sizeof(expected_sign_in)) != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_RPC_RESULT_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x11223344UL, 0x55667788UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0x5e002502UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_rpc_result(rpc, writer.length, &result) !=
            TG_MTPROTO_TL_OK ||
        result.request_msg_id_hi != 0x11223344UL ||
        result.request_msg_id_lo != 0x55667788UL ||
        result.result_constructor != 0x5e002502UL ||
        result.result_body_length != 4UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_RPC_ERROR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 400UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "PHONE_NUMBER_INVALID") !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_rpc_error(rpc, writer.length, &error_code,
                                   error_text, sizeof(error_text)) !=
            TG_MTPROTO_TL_OK ||
        error_code != 400L ||
        strcmp(error_text, "PHONE_NUMBER_INVALID") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_BAD_SERVER_SALT_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x01020304UL, 0x05060708UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 48UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x99aabbccUL, 0xddeeff00UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_bad_msg_notification(rpc, writer.length, &bad_msg) !=
            TG_MTPROTO_TL_OK ||
        bad_msg.error_code != 48UL ||
        bad_msg.new_server_salt_hi != 0x99aabbccUL ||
        bad_msg.new_server_salt_lo != 0xddeeff00UL ||
        !bad_msg.has_new_server_salt) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0x3dbb5986UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 5UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "hash") != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_auth_sent_code(TG_AUTH_SENT_CODE_CONSTRUCTOR, rpc,
                                        writer.length, &sent_code) !=
            TG_MTPROTO_TL_OK ||
        sent_code.type_constructor != 0x3dbb5986UL ||
        sent_code.type_length != 5UL ||
        !sent_code.has_type_length ||
        strcmp(sent_code.phone_code_hash, "hash") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_help_get_config(&writer) != TG_MTPROTO_TL_OK ||
        writer.length != 4UL ||
        query[0] != 0x6bU || query[1] != 0x18U ||
        query[2] != 0xf9U || query[3] != 0xc4U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_account_get_password(&writer) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 4UL ||
        query[0] != 0xf5U || query[1] != 0x30U ||
        query[2] != 0x8aU || query[3] != 0x54U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_users_get_self(&writer) != TG_MTPROTO_TL_OK ||
        writer.length != 16UL ||
        query[0] != 0x48U || query[1] != 0xa5U ||
        query[2] != 0x91U || query[3] != 0x0dU ||
        query[12] != 0x3fU || query[13] != 0xb1U ||
        query[14] != 0xc1U || query[15] != 0xf7U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_contacts_search(&writer, "mario", 10UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 16UL ||
        query[0] != 0xd8U || query[1] != 0x12U ||
        query[2] != 0xf8U || query[3] != 0x11U ||
        query[4] != 5U || memcmp(query + 5U, "mario", 5U) != 0 ||
        query[12] != 10U || query[13] != 0U ||
        query[14] != 0U || query[15] != 0U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_sign_up(&writer, "+1234567890", "hash",
                                      "Amiga", "") != TG_MTPROTO_TL_OK ||
        writer.length != 40UL ||
        query[0] != 0x17U || query[1] != 0xb7U ||
        query[2] != 0xc7U || query[3] != 0xaaU ||
        query[4] != 0x01U || query[5] != 0x00U ||
        query[6] != 0x00U || query[7] != 0x00U) {
        return 2;
    }

    memset(m1, 0x61, sizeof(m1));
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_check_password_empty(&writer) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 8UL ||
        query[0] != 0x16U || query[1] != 0x4dU ||
        query[2] != 0x8bU || query[3] != 0xd1U ||
        query[4] != 0x58U || query[5] != 0xf6U ||
        query[6] != 0x80U || query[7] != 0x98U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_auth_check_password_srp(
            &writer, 0x01020304UL, 0x05060708UL, srp_a, sizeof(srp_a),
            m1) != TG_MTPROTO_TL_OK ||
        writer.length != 60UL ||
        query[0] != 0x16U || query[1] != 0x4dU ||
        query[2] != 0x8bU || query[3] != 0xd1U ||
        query[4] != 0x82U || query[5] != 0xf0U ||
        query[6] != 0x7fU || query[7] != 0xd2U ||
        query[8] != 0x08U || query[9] != 0x07U ||
        query[10] != 0x06U || query[11] != 0x05U ||
        query[12] != 0x04U || query[13] != 0x03U ||
        query[14] != 0x02U || query[15] != 0x01U ||
        query[16] != sizeof(srp_a) ||
        memcmp(query + 17U, srp_a, sizeof(srp_a)) != 0 ||
        query[24] != TG_MTPROTO_SHA256_LENGTH ||
        memcmp(query + 25U, m1, sizeof(m1)) != 0) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_dialogs(&writer, 20UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 36UL || /* +4: flags bit1 + folder_id 0 (main) */
        query[0] != 0x4fU || query[1] != 0xcbU ||
        query[2] != 0xf4U || query[3] != 0xa0U ||
        query[4] != 0x02U || query[5] != 0x00U || /* flags: folder_id set */
        query[8] != 0x00U ||                      /* folder_id: main */
        query[20] != 0xeaU || query[21] != 0x18U ||
        query[22] != 0x3bU || query[23] != 0x7fU ||
        query[24] != 20U) {
        return 2;
    }
    /* messages.getPeerDialogs builder: one user InputDialogPeer, read back to
       confirm the wire layout (constructor / vector / inputDialogPeer / peer). */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_peer_dialogs(
            &writer, TG_PEER_USER_CONSTRUCTOR, 0UL, 0x00ABCDEFUL, 0x11223344UL,
            0x55667788UL, 1) != TG_MTPROTO_TL_OK ||
        writer.length != 36UL) {
        return 2;
    }
    {
        tg_mtproto_tl_reader reader;
        unsigned long w0, w1, w2, w3, w4;
        unsigned long id_hi, id_lo, ah_hi, ah_lo;

        tg_mtproto_tl_reader_init(&reader, query, writer.length);
        if (tg_mtproto_tl_read_u32(&reader, &w0) != TG_MTPROTO_TL_OK ||
            w0 != TG_MESSAGES_GET_PEER_DIALOGS_CONSTRUCTOR ||
            tg_mtproto_tl_read_u32(&reader, &w1) != TG_MTPROTO_TL_OK ||
            w1 != TG_VECTOR_CONSTRUCTOR ||
            tg_mtproto_tl_read_u32(&reader, &w2) != TG_MTPROTO_TL_OK ||
            w2 != 1UL ||
            tg_mtproto_tl_read_u32(&reader, &w3) != TG_MTPROTO_TL_OK ||
            w3 != TG_INPUT_DIALOG_PEER_CONSTRUCTOR ||
            tg_mtproto_tl_read_u32(&reader, &w4) != TG_MTPROTO_TL_OK ||
            w4 != TG_INPUT_PEER_USER_CONSTRUCTOR ||
            tg_mtproto_tl_read_u64(&reader, &id_hi, &id_lo) != TG_MTPROTO_TL_OK ||
            id_hi != 0UL || id_lo != 0x00ABCDEFUL ||
            tg_mtproto_tl_read_u64(&reader, &ah_hi, &ah_lo) !=
                TG_MTPROTO_TL_OK ||
            ah_hi != 0x11223344UL || ah_lo != 0x55667788UL) {
            return 2;
        }
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_self(&writer, 10UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 40UL ||
        query[0] != 0xc5U || query[1] != 0xe6U ||
        query[2] != 0x23U || query[3] != 0x44U ||
        query[4] != 0xc9U || query[5] != 0x7eU ||
        query[6] != 0xa0U || query[7] != 0x7dU ||
        query[20] != 10U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_user(
            &writer, 0x01020304UL, 0x05060708UL, 0x11121314UL,
            0x15161718UL, 10UL) != TG_MTPROTO_TL_OK ||
        writer.length != 56UL ||
        query[0] != 0xc5U || query[1] != 0xe6U ||
        query[2] != 0x23U || query[3] != 0x44U ||
        query[4] != 0x4cU || query[5] != 0xa5U ||
        query[6] != 0xe8U || query[7] != 0xddU ||
        query[8] != 0x08U || query[9] != 0x07U ||
        query[10] != 0x06U || query[11] != 0x05U ||
        query[12] != 0x04U || query[13] != 0x03U ||
        query[14] != 0x02U || query[15] != 0x01U ||
        query[16] != 0x18U || query[17] != 0x17U ||
        query[18] != 0x16U || query[19] != 0x15U ||
        query[20] != 0x14U || query[21] != 0x13U ||
        query[22] != 0x12U || query[23] != 0x11U ||
        query[36] != 10U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_peer(
            &writer, TG_PEER_CHAT_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0UL, 0UL, 0, 0UL, 10UL) != TG_MTPROTO_TL_OK ||
        writer.length != 48UL ||
        query[0] != 0xc5U || query[1] != 0xe6U ||
        query[2] != 0x23U || query[3] != 0x44U ||
        query[4] != 0xb9U || query[5] != 0x5cU ||
        query[6] != 0xa9U || query[7] != 0x35U ||
        query[8] != 0x08U || query[9] != 0x07U ||
        query[10] != 0x06U || query[11] != 0x05U ||
        query[12] != 0x04U || query[13] != 0x03U ||
        query[14] != 0x02U || query[15] != 0x01U ||
        query[28] != 10U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_get_history_peer(
            &writer, TG_PEER_CHANNEL_CONSTRUCTOR, 0x01020304UL,
            0x05060708UL, 0x11121314UL, 0x15161718UL, 1, 0UL, 10UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 56UL ||
        query[4] != 0xfcU || query[5] != 0xbbU ||
        query[6] != 0xbcU || query[7] != 0x27U ||
        query[36] != 10U) {
        return 2;
    }
    /* channels.getParticipants(recent): lock the verified layer-214 hashes + the
       LE int64 (lo-then-hi) layout. channel_id=0x1122334455667788,
       access_hash=0x99AABBCCDDEEFF00, limit=32 -> exactly 44 bytes. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_channels_get_participants_recent(
            &writer, 0x11223344UL, 0x55667788UL, 0x99AABBCCUL, 0xDDEEFF00UL,
            32UL) != TG_MTPROTO_TL_OK ||
        writer.length != 44UL ||
        query[0] != 0xd0U || query[1] != 0xd9U || query[2] != 0xceU ||
        query[3] != 0x77U ||                                  /* method */
        query[4] != 0x28U || query[5] != 0xecU || query[6] != 0x5aU ||
        query[7] != 0xf3U ||                                  /* inputChannel */
        query[8] != 0x88U || query[11] != 0x55U ||            /* channel_id lo */
        query[12] != 0x44U || query[15] != 0x11U ||           /* channel_id hi */
        query[16] != 0x00U || query[19] != 0xddU ||           /* access_hash lo */
        query[20] != 0xccU || query[23] != 0x99U ||           /* access_hash hi */
        query[24] != 0x79U || query[25] != 0x3cU || query[26] != 0x3fU ||
        query[27] != 0xdeU ||                                 /* filter */
        query[28] != 0x00U || query[32] != 0x20U) {           /* offset 0, limit 32 */
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_self(&writer, "hi", 0UL, 0x11223344UL,
                                            0x55667788UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 24UL ||
        query[0] != 0x9aU || query[1] != 0xdcU ||
        query[2] != 0x05U || query[3] != 0xfeU ||
        query[8] != 0xc9U || query[9] != 0x7eU ||
        query[10] != 0xa0U || query[11] != 0x7dU ||
        query[16] != 0x88U || query[17] != 0x77U ||
        query[18] != 0x66U || query[19] != 0x55U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_user(
            &writer, 0x01020304UL, 0x05060708UL, 0x11121314UL,
            0x15161718UL, "hi", 0UL, 0x11223344UL, 0x55667788UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 40UL ||
        query[0] != 0x9aU || query[1] != 0xdcU ||
        query[2] != 0x05U || query[3] != 0xfeU ||
        query[8] != 0x4cU || query[9] != 0xa5U ||
        query[10] != 0xe8U || query[11] != 0xddU ||
        query[28] != 0x02U || query[29] != 'h' ||
        query[30] != 'i' ||
        query[32] != 0x88U || query[33] != 0x77U ||
        query[34] != 0x66U || query[35] != 0x55U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_peer(
            &writer, TG_PEER_CHAT_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0UL, 0UL, 0, "hi", 0UL, 0x11223344UL, 0x55667788UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 32UL ||
        query[8] != 0xb9U || query[9] != 0x5cU ||
        query[10] != 0xa9U || query[11] != 0x35U ||
        query[20] != 0x02U || query[21] != 'h' ||
        query[22] != 'i' ||
        query[24] != 0x88U || query[25] != 0x77U ||
        query[26] != 0x66U || query[27] != 0x55U) {
        return 2;
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_peer(
            &writer, TG_PEER_CHANNEL_CONSTRUCTOR, 0x01020304UL,
            0x05060708UL, 0x11121314UL, 0x15161718UL, 1, "hi",
            0UL, 0x11223344UL, 0x55667788UL) != TG_MTPROTO_TL_OK ||
        writer.length != 40UL ||
        query[8] != 0xfcU || query[9] != 0xbbU ||
        query[10] != 0xbcU || query[11] != 0x27U ||
        query[28] != 0x02U || query[29] != 'h' ||
        query[30] != 'i' ||
        query[32] != 0x88U || query[33] != 0x77U ||
        query[34] != 0x66U || query[35] != 0x55U) {
        return 2;
    }
    /* FORWARD: messages.forwardMessages#978928ca flags=0, chat source,
       id=[0x0A0B0C0D], random_id=[0x1122334455667788], self destination.
       The exact 52-byte shape pins the official layer-214 hash and field order. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_forward_message(
            &writer, TG_PEER_CHAT_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0UL, 0UL, 0, 0x0A0B0C0DUL, 0x11223344UL, 0x55667788UL,
            TG_MTPROTO_PEER_SELF_CONSTRUCTOR, 0UL, 0UL, 0UL, 0UL, 0) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 52UL ||
        query[0] != 0xcaU || query[1] != 0x28U ||
        query[2] != 0x89U || query[3] != 0x97U ||            /* method */
        query[4] != 0x00U ||                                 /* flags */
        query[8] != 0xb9U || query[11] != 0x35U ||           /* source */
        query[12] != 0x08U || query[19] != 0x01U ||          /* source id */
        query[20] != 0x15U || query[23] != 0x1cU ||          /* id vector */
        query[24] != 0x01U || query[28] != 0x0dU ||
        query[31] != 0x0aU ||                                /* one message id */
        query[32] != 0x15U || query[35] != 0x1cU ||          /* random vector */
        query[36] != 0x01U || query[40] != 0x88U ||
        query[47] != 0x11U ||                                /* one random id */
        query[48] != 0xc9U || query[49] != 0x7eU ||
        query[50] != 0xa0U || query[51] != 0x7dU) {          /* inputPeerSelf */
        return 2;
    }
    /* Generic picker path: user source -> channel destination. Both peers carry
       access hashes; pin their constructors and offsets independently from the
       shorter chat -> self shape above. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_forward_message(
            &writer, TG_PEER_USER_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0x11121314UL, 0x15161718UL, 1, 0x0A0B0C0DUL,
            0x41424344UL, 0x45464748UL, TG_PEER_CHANNEL_CONSTRUCTOR,
            0x21222324UL, 0x25262728UL, 0x31323334UL, 0x35363738UL, 1) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 76UL ||
        query[8] != 0x4cU || query[9] != 0xa5U ||
        query[10] != 0xe8U || query[11] != 0xddU ||          /* inputPeerUser */
        query[12] != 0x08U || query[19] != 0x01U ||         /* source id */
        query[20] != 0x18U || query[27] != 0x11U ||         /* source hash */
        query[28] != 0x15U || query[31] != 0x1cU ||         /* id vector */
        query[36] != 0x0dU || query[39] != 0x0aU ||         /* message id */
        query[40] != 0x15U || query[43] != 0x1cU ||         /* random vector */
        query[48] != 0x48U || query[55] != 0x41U ||         /* random id */
        query[56] != 0xfcU || query[57] != 0xbbU ||
        query[58] != 0xbcU || query[59] != 0x27U ||         /* inputPeerChannel */
        query[60] != 0x28U || query[67] != 0x21U ||         /* destination id */
        query[68] != 0x38U || query[75] != 0x31U) {         /* destination hash */
        return 2;
    }
    /* REPLY: flags bit 0 set + inputReplyToMessage#869fbe10 (flags=0,
       reply_to_msg_id=0x0A0B0C0D) inserted between peer and message -> 44 bytes.
       Pins the layer-214 ctor id + the wire position of the reply. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_send_peer(
            &writer, TG_PEER_CHAT_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0UL, 0UL, 0, "hi", 0x0A0B0C0DUL, 0x11223344UL, 0x55667788UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 44UL ||
        query[4] != 0x01U || query[5] != 0x00U ||            /* flags bit 0 */
        query[8] != 0xb9U || query[11] != 0x35U ||           /* inputPeerChat */
        query[20] != 0x10U || query[21] != 0xbeU ||
        query[22] != 0x9fU || query[23] != 0x86U ||          /* inputReplyToMessage */
        query[24] != 0x00U ||                                /* reply flags 0 */
        query[28] != 0x0dU || query[29] != 0x0cU ||
        query[30] != 0x0bU || query[31] != 0x0aU ||          /* reply_to_msg_id LE */
        query[32] != 0x02U || query[33] != 'h' || query[34] != 'i' ||
        query[36] != 0x88U || query[39] != 0x55U) {          /* random_id lo */
        return 2;
    }
    /* EDIT: messages.editMessage#dfd14005 flags=0x800(message) peer=chat id=hi
       message="hi" -> 28 bytes. Pins the ctor id + flags + wire order. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_edit_message(
            &writer, TG_PEER_CHAT_CONSTRUCTOR, 0x01020304UL, 0x05060708UL,
            0UL, 0UL, 0, 0x0A0B0C0DUL, "hi") != TG_MTPROTO_TL_OK ||
        writer.length != 28UL ||
        query[0] != 0x05U || query[1] != 0x40U ||
        query[2] != 0xd1U || query[3] != 0xdfU ||            /* editMessage ctor */
        query[4] != 0x00U || query[5] != 0x08U ||            /* flags 0x800 */
        query[8] != 0xb9U || query[11] != 0x35U ||           /* inputPeerChat */
        query[20] != 0x0dU || query[23] != 0x0aU ||          /* id LE */
        query[24] != 0x02U || query[25] != 'h' || query[26] != 'i') {
        return 2;
    }
    /* DELETE (peer): messages.deleteMessages#e58e95d2 revoke=1 id=[hi] ->
       ctor + flags + vector + count + id = 20 bytes. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_messages_delete_messages(&writer, 1, 0x0A0B0C0DUL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 20UL ||
        query[0] != 0xd2U || query[1] != 0x95U ||
        query[2] != 0x8eU || query[3] != 0xe5U ||            /* deleteMessages ctor */
        query[4] != 0x01U ||                                 /* revoke flag */
        query[8] != 0x15U || query[11] != 0x1cU ||           /* vector ctor */
        query[12] != 0x01U ||                                /* count 1 */
        query[16] != 0x0dU || query[19] != 0x0aU) {          /* id LE */
        return 2;
    }
    /* DELETE (channel): channels.deleteMessages#84c1fd4e inputChannel id=[hi] ->
       ctor + inputChannel(ctor+id8+hash8) + vector + count + id = 36 bytes. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_channels_delete_messages(
            &writer, 0x01020304UL, 0x05060708UL, 0x11121314UL, 0x15161718UL,
            0x0A0B0C0DUL) != TG_MTPROTO_TL_OK ||
        writer.length != 36UL ||
        query[0] != 0x4eU || query[1] != 0xfdU ||
        query[2] != 0xc1U || query[3] != 0x84U ||            /* channels.deleteMessages ctor */
        query[4] != 0x28U || query[7] != 0xf3U ||            /* inputChannel ctor */
        query[8] != 0x08U || query[15] != 0x01U ||           /* channel_id lo..hi */
        query[16] != 0x18U || query[23] != 0x11U ||          /* access_hash lo..hi */
        query[24] != 0x15U || query[27] != 0x1cU ||          /* vector ctor */
        query[28] != 0x01U ||                                /* count 1 */
        query[32] != 0x0dU || query[35] != 0x0aU) {          /* id LE */
        return 2;
    }
    /* updateReadHistoryOutbox#2f2f21bf parser (5c): build a body (peerUser
       id=0x0102030405060708, max_id=0x0A0B0C0D) and read it back. */
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_tl_write_u32(&writer,
            TG_UPDATE_READ_HISTORY_OUTBOX_CONSTRUCTOR) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x01020304UL, 0x05060708UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0x0A0B0C0DUL) != TG_MTPROTO_TL_OK) {
        return 2;
    }
    {
        tg_mtproto_dialog_peer rpeer;
        unsigned long rmax;

        if (tg_mtproto_parse_update_read_history_outbox(query, writer.length,
                                                        &rpeer, &rmax) !=
                TG_MTPROTO_TL_OK ||
            rpeer.peer_constructor != TG_PEER_USER_CONSTRUCTOR ||
            rpeer.id_hi != 0x01020304UL || rpeer.id_lo != 0x05060708UL ||
            rmax != 0x0A0B0C0DUL) {
            return 2;
        }
    }
    tg_mtproto_tl_writer_init(&writer, query, sizeof(query));
    if (tg_mtproto_build_msgs_ack(&writer, &bad_msg.bad_msg_id_hi,
                                  &bad_msg.bad_msg_id_lo, 1UL) !=
            TG_MTPROTO_TL_OK ||
        writer.length != 20UL ||
        query[0] != 0x59U || query[1] != 0xb4U ||
        query[2] != 0xd6U || query[3] != 0x62U ||
        query[4] != 0x15U || query[5] != 0xc4U ||
        query[6] != 0xb5U || query[7] != 0x1cU) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 100UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 200UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0xbc799737UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_config_summary(TG_CONFIG_CONSTRUCTOR, rpc,
                                        writer.length, &config) !=
            TG_MTPROTO_TL_OK ||
        config.date != 100UL || config.expires != 200UL ||
        config.this_dc != 2UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, 7UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, srp_salt1, sizeof(srp_salt1)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, srp_salt2, sizeof(srp_salt2)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, srp_p, sizeof(srp_p)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_bytes(&writer, srp_b, sizeof(srp_b)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x01020304UL, 0x05060708UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_account_password_summary(
            TG_ACCOUNT_PASSWORD_CONSTRUCTOR, rpc, writer.length,
            &password) != TG_MTPROTO_TL_OK ||
        !password.has_recovery || !password.has_secure_values ||
        !password.has_password ||
        !password.has_current_algo ||
        password.current_algo_constructor != TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR ||
        password.current_salt1_length != sizeof(srp_salt1) ||
        password.current_salt2_length != sizeof(srp_salt2) ||
        password.current_p_length != sizeof(srp_p) ||
        password.srp_b_length != sizeof(srp_b) ||
        password.current_g != 3UL ||
        password.srp_id_hi != 0x01020304UL ||
        password.srp_id_lo != 0x05060708UL ||
        memcmp(password.current_salt1, srp_salt1, sizeof(srp_salt1)) != 0 ||
        memcmp(password.current_salt2, srp_salt2, sizeof(srp_salt2)) != 0 ||
        memcmp(password.current_p, srp_p, sizeof(srp_p)) != 0 ||
        memcmp(password.srp_b, srp_b, sizeof(srp_b)) != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 4UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 5UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialogs_summary(TG_MESSAGES_DIALOGS_CONSTRUCTOR,
                                         rpc, writer.length, &dialogs) !=
            TG_MTPROTO_TL_OK ||
        dialogs.dialog_count != 2UL || dialogs.message_count != 3UL ||
        dialogs.chat_count != 4UL || dialogs.user_count != 5UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_DIALOG_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 11UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 9UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 10UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_DIALOG_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_CHAT_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x87654321UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 21UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 19UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 20UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 4UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialog_peer_list(TG_MESSAGES_DIALOGS_CONSTRUCTOR,
                                          peer_rpc, writer.length,
                                          &peer_list) != TG_MTPROTO_TL_OK ||
        peer_list.count != 2UL || peer_list.total_dialog_count != 2UL ||
        peer_list.peers[0].peer_constructor != TG_PEER_USER_CONSTRUCTOR ||
        peer_list.peers[0].id_lo != 0x12345678UL ||
        peer_list.peers[0].top_message != 11UL ||
        peer_list.peers[0].read_outbox_max_id != 10UL ||
        peer_list.peers[0].unread_count != 3UL ||
        peer_list.peers[1].peer_constructor != TG_PEER_CHAT_CONSTRUCTOR ||
        peer_list.peers[1].id_lo != 0x87654321UL ||
        peer_list.peers[1].top_message != 21UL ||
        peer_list.peers[1].read_outbox_max_id != 20UL ||
        peer_list.peers[1].unread_count != 4UL) {
        return 2;
    }
    /* The same body parses under messages.peerDialogs too -- the getPeerDialogs
       reply leads with the dialogs vector, so the read_outbox cursor (the "seen"
       source) is reachable without a dedicated peerDialogs parser. */
    if (tg_mtproto_parse_dialog_peer_list(TG_MESSAGES_PEER_DIALOGS_CONSTRUCTOR,
                                          peer_rpc, writer.length,
                                          &peer_list) != TG_MTPROTO_TL_OK ||
        peer_list.count != 2UL ||
        peer_list.peers[0].read_outbox_max_id != 10UL ||
        peer_list.peers[1].read_outbox_max_id != 20UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_DIALOG_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 11UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 9UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 10UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_CHAT_FORBIDDEN_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x87654321UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Test Group") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL | 2UL | 4UL | 8UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x01020304UL, 0x05060708UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Ada") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Lovelace") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "ada") != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialog_peer_cache(TG_MESSAGES_DIALOGS_CONSTRUCTOR,
                                           peer_rpc, writer.length,
                                           &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 2UL || peer_cache.total_dialog_count != 1UL ||
        peer_cache.user_count != 1UL || peer_cache.chat_count != 1UL ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x01020304UL ||
        peer_cache.entries[0].access_hash_lo != 0x05060708UL ||
        strcmp(peer_cache.entries[0].title, "Ada Lovelace") != 0 ||
        strcmp(peer_cache.entries[0].username, "ada") != 0 ||
        peer_cache.entries[1].peer_constructor != TG_PEER_CHAT_CONSTRUCTOR ||
        strcmp(peer_cache.entries[1].title, "Test Group") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_DIALOG_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 31UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 29UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 30UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 5UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL << 13) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x11121314UL, 0x15161718UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Test Channel") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialog_peer_cache(TG_MESSAGES_DIALOGS_CONSTRUCTOR,
                                           peer_rpc, writer.length,
                                           &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 1UL || peer_cache.chat_count != 1UL ||
        peer_cache.entries[0].peer_constructor != TG_PEER_CHANNEL_CONSTRUCTOR ||
        peer_cache.entries[0].id_lo != 0x2468ace0UL ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x11121314UL ||
        peer_cache.entries[0].access_hash_lo != 0x15161718UL ||
        strcmp(peer_cache.entries[0].title, "Test Channel") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_DIALOG_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 11UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 9UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 10UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 3UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_NOTIFY_SETTINGS_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0xdeadbeefUL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL | 2UL | 8UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x11111111UL, 0x22222222UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Grace") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "grace") != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_dialog_peer_cache(TG_MESSAGES_DIALOGS_CONSTRUCTOR,
                                           peer_rpc, writer.length,
                                           &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 1UL || peer_cache.user_count != 1UL ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x11111111UL ||
        peer_cache.entries[0].access_hash_lo != 0x22222222UL ||
        strcmp(peer_cache.entries[0].title, "Grace") != 0 ||
        strcmp(peer_cache.entries[0].username, "grace") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL | 2UL | 8UL | 32UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x33333333UL, 0x44444444UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Kaffaine") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "kaffobot") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0xabcdef01UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_resolved_peer_cache(
            TG_CONTACTS_RESOLVED_PEER_CONSTRUCTOR, peer_rpc, writer.length,
            &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 1UL || peer_cache.user_count != 1UL ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x33333333UL ||
        peer_cache.entries[0].access_hash_lo != 0x44444444UL ||
        strcmp(peer_cache.entries[0].title, "Kaffaine") != 0 ||
        strcmp(peer_cache.entries[0].username, "kaffobot") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_PEER_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL << 13) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x11121314UL, 0x15161718UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Resolved Channel") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_resolved_peer_cache(
            TG_CONTACTS_RESOLVED_PEER_CONSTRUCTOR, peer_rpc, writer.length,
            &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 1UL || peer_cache.chat_count != 1UL ||
        peer_cache.entries[0].peer_constructor != TG_PEER_CHANNEL_CONSTRUCTOR ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x11121314UL ||
        peer_cache.entries[0].access_hash_lo != 0x15161718UL ||
        strcmp(peer_cache.entries[0].title, "Resolved Channel") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_CHANNEL_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, (1UL << 6) | (1UL << 13)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x2468ace0UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x11121314UL, 0x15161718UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Amiga Group") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "amigagroup") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL | 2UL | 8UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0x33333333UL, 0x44444444UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Mario") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "mrossi") != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_contacts_search_peer_cache(
            TG_CONTACTS_FOUND_CONSTRUCTOR, peer_rpc, writer.length,
            &peer_cache) != TG_MTPROTO_TL_OK ||
        peer_cache.count != 2UL || peer_cache.user_count != 1UL ||
        peer_cache.chat_count != 1UL ||
        !peer_cache.entries[0].has_access_hash ||
        peer_cache.entries[0].access_hash_hi != 0x33333333UL ||
        peer_cache.entries[0].access_hash_lo != 0x44444444UL ||
        strcmp(peer_cache.entries[0].title, "Mario") != 0 ||
        strcmp(peer_cache.entries[0].username, "mrossi") != 0 ||
        peer_cache.entries[1].peer_constructor != TG_PEER_CHANNEL_CONSTRUCTOR ||
        !peer_cache.entries[1].has_access_hash ||
        peer_cache.entries[1].access_hash_hi != 0x11121314UL ||
        peer_cache.entries[1].access_hash_lo != 0x15161718UL ||
        strcmp(peer_cache.entries[1].title, "Amiga Group") != 0 ||
        strcmp(peer_cache.entries[1].username, "amigagroup") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 6UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 7UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 8UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_messages_summary(TG_MESSAGES_MESSAGES_CONSTRUCTOR,
                                          rpc, writer.length, &messages) !=
            TG_MTPROTO_TL_OK ||
        messages.message_count != 6UL || messages.chat_count != 7UL ||
        messages.user_count != 8UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, peer_rpc, sizeof(peer_rpc));
    if (tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 4UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_MESSAGE_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 4UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1000UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer,
                                TG_MESSAGE_FWD_HEADER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1111UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2221UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "forward reply") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_MESSAGE_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1001UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2222UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "reply text") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_MESSAGE_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1002UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x12345678UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2223UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "my text") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_MESSAGE_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 8UL | 128UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1003UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_PEER_CHAT_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 0x87654321UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer,
                                TG_MESSAGE_REPLY_HEADER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, (1UL << 4) | (1UL << 6)) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1002UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "reply quote") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 2224UL) != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "group reply") != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0xbd610bc9UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 5UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_VECTOR_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_message_text_list(TG_MESSAGES_MESSAGES_CONSTRUCTOR,
                                           peer_rpc, writer.length,
                                           &text_list) != TG_MTPROTO_TL_OK ||
        text_list.count != 4UL || text_list.total_message_count != 4UL ||
        text_list.messages[0].id != 1000UL ||
        text_list.messages[0].date != 2221UL ||
        text_list.messages[0].is_out ||
        strcmp(text_list.messages[0].text, "forward reply") != 0 ||
        text_list.messages[1].id != 1001UL ||
        text_list.messages[1].date != 2222UL ||
        text_list.messages[1].is_out ||
        strcmp(text_list.messages[1].text, "reply text") != 0 ||
        text_list.messages[2].id != 1002UL ||
        text_list.messages[2].date != 2223UL ||
        !text_list.messages[2].is_out ||
        strcmp(text_list.messages[2].text, "my text") != 0 ||
        text_list.messages[3].id != 1003UL ||
        text_list.messages[3].date != 2224UL ||
        text_list.messages[3].is_out ||
        /* messageEntityBold over "group" (offset 0, len 5) -> inline markers. */
        strcmp(text_list.messages[3].text, "*group* reply") != 0 ||
        /* reply header: reply_to_msg_id (flags.4) + quote_text (flags.6). */
        !text_list.messages[3].has_reply ||
        text_list.messages[3].reply_to_msg_id != 1002UL ||
        strcmp(text_list.messages[3].reply_quote, "reply quote") != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 44UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 222UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_updates_summary(
            TG_UPDATE_SHORT_SENT_MESSAGE_CONSTRUCTOR, rpc, writer.length,
            &updates) != TG_MTPROTO_TL_OK ||
        !updates.has_sent_message || updates.id != 44UL ||
        updates.date != 222UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, rpc, sizeof(rpc));
    if (tg_mtproto_tl_write_u32(&writer, 1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, TG_USER_CONSTRUCTOR) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, (1UL << 10) | 2UL | 8UL) !=
            TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u32(&writer, 0UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_u64(&writer, 0UL, 12345UL) !=
            TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "Test") != TG_MTPROTO_TL_OK ||
        tg_write_string(&writer, "tester") != TG_MTPROTO_TL_OK ||
        tg_mtproto_parse_user_vector_first(TG_VECTOR_CONSTRUCTOR, rpc,
                                           writer.length, &user) !=
            TG_MTPROTO_TL_OK ||
        !user.is_self || user.is_bot || user.id_lo != 12345UL ||
        strcmp(user.first_name, "Test") != 0 ||
        strcmp(user.username, "tester") != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */

tg_mtproto_tl_status tg_mtproto_read_update_message_text(
    tg_mtproto_tl_reader *reader,
    tg_mtproto_message_text *out,
    tg_mtproto_dialog_peer *out_dest)
{
    if (reader == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    return tg_read_common_message_text(reader, out, out_dest);
}

int tg_mtproto_resync_message_text(tg_mtproto_tl_reader *reader,
                                   unsigned long fallback_offset)
{
    if (reader == 0) {
        return 0;
    }
    return tg_message_text_resync(reader, fallback_offset, 0UL);
}
