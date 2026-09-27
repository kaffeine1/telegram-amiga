/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "tg_mtproto.h"
#include "tg_mtproto_auth.h"
#include "tg_mtproto_bigint.h"
#include "tg_mtproto_crypto.h"
#include "tg_mtproto_dc.h"
#include "tg_mtproto_encrypted.h"
#include "tg_mtproto_envelope.h"
#include "tg_mtproto_login.h"
#include "tg_mtproto_message_id.h"
#include "tg_mtproto_probe.h"
#include "tg_mtproto_rsa.h"
#include "tg_mtproto_session.h"
#include "tg_mtproto_srp.h"
#include "tg_mtproto_tl.h"
#include "tg_mtproto_transport.h"

#if !defined(TG_NO_SELFTEST)
typedef int (*tg_mtproto_self_test_fn)(void);

static int tg_mtproto_run_self_test_step(const char *name,
                                         tg_mtproto_self_test_fn fn)
{
    if (fn() != 0) {
        printf("mtproto %s self-test: failed\n", name);
        return 2;
    }
    printf("mtproto %s self-test: ok\n", name);
    return 0;
}

int tg_mtproto_self_test(void)
{
    if (tg_mtproto_run_self_test_step("dc", tg_mtproto_dc_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("message-id",
                                      tg_mtproto_message_id_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("auth", tg_mtproto_auth_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("rsa", tg_mtproto_rsa_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("bigint",
                                      tg_mtproto_bigint_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("tl", tg_mtproto_tl_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("envelope",
                                      tg_mtproto_envelope_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("encrypted",
                                      tg_mtproto_encrypted_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("transport",
                                      tg_mtproto_transport_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("login",
                                      tg_mtproto_login_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("probe",
                                      tg_mtproto_probe_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("crypto",
                                      tg_mtproto_crypto_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("aes", tg_mtproto_aes_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("srp", tg_mtproto_srp_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("session",
                                      tg_mtproto_session_self_test) != 0) {
        return 2;
    }

    puts("mtproto self-test: ok");
    return 0;
}

int tg_mtproto_self_test_fast(void)
{
    if (tg_mtproto_run_self_test_step("dc", tg_mtproto_dc_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("message-id",
                                      tg_mtproto_message_id_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("auth", tg_mtproto_auth_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("tl", tg_mtproto_tl_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("envelope",
                                      tg_mtproto_envelope_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("encrypted",
                                      tg_mtproto_encrypted_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("transport",
                                      tg_mtproto_transport_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("login",
                                      tg_mtproto_login_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("probe",
                                      tg_mtproto_probe_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("crypto",
                                      tg_mtproto_crypto_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("aes", tg_mtproto_aes_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("session",
                                      tg_mtproto_session_self_test) != 0) {
        return 2;
    }

    puts("mtproto fast self-test: ok");
    return 0;
}

int tg_mtproto_self_test_heavy(void)
{
    if (tg_mtproto_run_self_test_step("rsa", tg_mtproto_rsa_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("bigint",
                                      tg_mtproto_bigint_self_test) != 0) {
        return 2;
    }
    if (tg_mtproto_run_self_test_step("srp", tg_mtproto_srp_self_test) != 0) {
        return 2;
    }

    puts("mtproto heavy self-test: ok");
    return 0;
}
#endif /* !TG_NO_SELFTEST */
