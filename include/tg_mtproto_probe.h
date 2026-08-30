/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_PROBE_H
#define TG_MTPROTO_PROBE_H

#include <stdio.h>

#include "tg_chat_engine.h"
#include "tg_mtproto_tl.h"

tg_mtproto_tl_status tg_mtproto_build_req_pq_multi(
    tg_mtproto_tl_writer *writer,
    unsigned long message_id_hi,
    unsigned long message_id_lo,
    const unsigned char nonce[16]);

int tg_mtproto_req_pq_probe(const char *host, const char *port, FILE *stream);
int tg_mtproto_2fa_bench(FILE *stream);
int tg_mtproto_req_dh_probe(const char *host, const char *port,
                            const char *dc_id_text, FILE *stream);
int tg_mtproto_auth_send_code(const char *host,
                              const char *port,
                              const char *dc_id_text,
                              const char *api_id_text,
                              const char *api_hash,
                              const char *phone_number,
                              const char *auth_file,
                              const char *code_hash_file,
                              FILE *stream);
int tg_mtproto_auth_send_code_file(const char *host,
                                   const char *port,
                                   const char *dc_id_text,
                                   const char *api_file,
                                   const char *phone_number,
                                   const char *auth_file,
                                   const char *code_hash_file,
                                   FILE *stream);
int tg_mtproto_auth_sign_in(const char *host,
                            const char *port,
                            const char *api_id_text,
                            const char *auth_file,
                            const char *phone_number,
                            const char *code_hash_file,
                            const char *phone_code,
                            const char *dc_id_text,
                            FILE *stream);
int tg_mtproto_auth_sign_in_file(const char *host,
                                 const char *port,
                                 const char *api_file,
                                 const char *auth_file,
                                 const char *phone_number,
                                 const char *code_hash_file,
                                 const char *phone_code,
                                 const char *dc_id_text,
                                 FILE *stream);
int tg_mtproto_auth_sign_up(const char *host,
                            const char *port,
                            const char *api_id_text,
                            const char *auth_file,
                            const char *phone_number,
                            const char *code_hash_file,
                            const char *first_name,
                            const char *last_name,
                            const char *dc_id_text,
                            FILE *stream);
int tg_mtproto_auth_get_config(const char *host,
                               const char *port,
                               const char *api_id_text,
                               const char *auth_file,
                               const char *dc_id_text,
                               FILE *stream);
int tg_mtproto_auth_get_config_file(const char *host,
                                    const char *port,
                                    const char *api_file,
                                    const char *auth_file,
                                    const char *dc_id_text,
                                    FILE *stream);
int tg_mtproto_auth_get_password(const char *host,
                                 const char *port,
                                 const char *api_id_text,
                                 const char *auth_file,
                                 const char *dc_id_text,
                                 FILE *stream);
int tg_mtproto_auth_get_password_file(const char *host,
                                      const char *port,
                                      const char *api_file,
                                      const char *auth_file,
                                      const char *dc_id_text,
                                      FILE *stream);
int tg_mtproto_auth_check_password(const char *host,
                                   const char *port,
                                   const char *api_id_text,
                                   const char *auth_file,
                                   const char *dc_id_text,
                                   const char *password_file,
                                   FILE *stream);
int tg_mtproto_auth_check_password_file(const char *host,
                                        const char *port,
                                        const char *api_file,
                                        const char *auth_file,
                                        const char *dc_id_text,
                                        const char *password_file,
                                        FILE *stream);
int tg_mtproto_auth_login_wizard_file(const char *host,
                                      const char *port,
                                      const char *dc_id_text,
                                      const char *api_file,
                                      const char *auth_file,
                                      const char *code_hash_file,
                                      FILE *stream);
int tg_mtproto_auth_status(const char *host,
                           const char *port,
                           const char *api_id_text,
                           const char *auth_file,
                           const char *dc_id_text,
                           FILE *stream);
int tg_mtproto_auth_status_file(const char *host,
                                const char *port,
                                const char *api_file,
                                const char *auth_file,
                                const char *dc_id_text,
                                FILE *stream);
int tg_mtproto_auth_inspect(const char *auth_file, FILE *stream);
int tg_mtproto_auth_check_local_files(const char *api_file,
                                      const char *auth_file,
                                      const char *password_file,
                                      const char *code_hash_file,
                                      FILE *stream);
int tg_mtproto_auth_get_self(const char *host,
                             const char *port,
                             const char *api_id_text,
                             const char *auth_file,
                             const char *dc_id_text,
                             FILE *stream);
int tg_mtproto_auth_get_dialogs(const char *host,
                                const char *port,
                                const char *api_id_text,
                                const char *auth_file,
                                const char *dc_id_text,
                                const char *limit_text,
                                FILE *stream);
int tg_mtproto_auth_get_dialogs_file(const char *host,
                                     const char *port,
                                     const char *api_file,
                                     const char *auth_file,
                                     const char *dc_id_text,
                                     const char *limit_text,
                                     FILE *stream);
int tg_mtproto_auth_list_peers_file(const char *host,
                                    const char *port,
                                    const char *api_file,
                                    const char *auth_file,
                                    const char *dc_id_text,
                                    const char *limit_text,
                                    const char *peer_cache_file,
                                    FILE *stream);
/* One-shot live peer-cache refresh for the GUI sidebar: derives host/DC from
   the saved session and runs list-peers (with a heavy-account retry). See the
   definition in tg_mtproto_probe.c. */
int tg_mtproto_gui_refresh_peer_cache(const char *api_file,
                                      const char *auth_file,
                                      const char *peer_cache_file,
                                      FILE *stream);
int tg_mtproto_auth_resolve_username_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *username_text,
                                          const char *peer_cache_file,
                                          FILE *stream);
int tg_mtproto_auth_get_history_self(const char *host,
                                     const char *port,
                                     const char *api_id_text,
                                     const char *auth_file,
                                     const char *dc_id_text,
                                     const char *limit_text,
                                     FILE *stream);
int tg_mtproto_auth_get_history_self_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *limit_text,
                                          FILE *stream);
int tg_mtproto_auth_get_history_peer_file(const char *host,
                                          const char *port,
                                          const char *api_file,
                                          const char *auth_file,
                                          const char *dc_id_text,
                                          const char *peer_cache_file,
                                          const char *peer_index_text,
                                          const char *limit_text,
                                          FILE *stream);
int tg_mtproto_auth_send_self(const char *host,
                              const char *port,
                              const char *api_id_text,
                              const char *auth_file,
                              const char *dc_id_text,
                              const char *message,
                              FILE *stream);
int tg_mtproto_auth_send_peer_file(const char *host,
                                   const char *port,
                                   const char *api_file,
                                   const char *auth_file,
                                   const char *dc_id_text,
                                   const char *peer_cache_file,
                                   const char *peer_index_text,
                                   const char *message,
                                   FILE *stream);
int tg_mtproto_auth_chat_file(const char *host,
                              const char *port,
                              const char *api_file,
                              const char *auth_file,
                              const char *dc_id_text,
                              const char *peer_cache_file,
                              FILE *stream);
int tg_mtproto_auth_forget(const char *auth_file,
                           const char *code_hash_file,
                           FILE *stream);
int tg_mtproto_probe_self_test(void);

/* Golden parity check for the transcript renderer (the chat model/view seam):
   renders a fixed message script with colours off and asserts byte-equality
   against a recorded golden. Host/CI runnable; pins output across the
   chat-engine extraction. */
int tg_mtproto_chat_render_self_test(void);

/* Parses the peer-cache file into chat-list rows (resolving display name +
   @username flag, user/group kind, unread, current-chat marker). Returns the
   row count (<= max); sets *file_missing non-zero when the cache file is absent.
   The file format is probe.c's, hence this lives here. */
int tg_mtproto_chat_list_parse(const char *path, unsigned long current_index,
                               tg_chat_list_row *rows, int max,
                               int *file_missing);

/* F10: append the pinned "Saved Messages" row (self chat) as the last row. */
void tg_gui_saved_messages_row(tg_chat_list_row *rows, int *count, int max);

/* Golden parity check for the chat-list renderer: parses a fixed peer cache and
   asserts the grouped console output byte-for-byte. Host/CI runnable. */
int tg_mtproto_chat_list_self_test(void);

/* Interactive console diagnostic: prints the colour roles, the pen palette,
   the Latin-1 marker glyphs and sample emoji mappings so a tester can verify
   in seconds how the UI renders on a given console (--console-ui-test). */
int tg_mtproto_console_ui_test(FILE *stream);

/* Render one Unicode codepoint into `out` (cap >= 8) as ASCII/Latin-1 for the
   native GUI text path. Returns the bytes written; 0 means "omit" (no glyph).
   The mapping mirrors the console display path, but unmapped/symbol-block
   codepoints are omitted instead of drawn as '?'. */
unsigned long tg_mtproto_display_codepoint_to_latin1(unsigned long cp,
                                                     char *out,
                                                     unsigned long cap);

/* 1 when the codepoint only modifies the character before it (variation
   selector, zero-width joiner, skin tone). Callers that collapse whitespace
   around an omitted codepoint need to leave these alone: they attach to a
   neighbour rather than standing in for a word. */
int tg_mtproto_display_codepoint_is_invisible(unsigned long cp);

#endif
