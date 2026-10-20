/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Driver-agnostic chat-session model. See tg_chat_engine.h. Owns the updates
 * cursor, the /diff flag and the cross-chat notification queue (collected by
 * the MTProto parsers in tg_mtproto_probe.c, consumed by the renderer). The
 * notification ops live here, generic and NULL-safe, so they can be unit-tested
 * on the host independently of the wire parsers.
 */

#include "tg_chat_engine.h"

#include <stdio.h>
#include <string.h>

void tg_chat_notify_reset(tg_chat_notify *notify, int armed)
{
    unsigned long i;

    if (notify == 0) {
        return;
    }
    notify->count = 0UL;
    notify->dropped = 0UL;
    notify->recent_pos = 0UL;
    for (i = 0UL; i < TG_CHAT_NOTIFY_RECENT; ++i) {
        notify->recent_ids[i] = 0UL;
    }
    notify->armed = armed;
}

int tg_chat_notify_seen(tg_chat_notify *notify, unsigned long message_id)
{
    unsigned long i;

    if (notify == 0) {
        return 1; /* no queue: tell the caller to skip */
    }
    if (message_id == 0UL) {
        return 0;
    }
    for (i = 0UL; i < TG_CHAT_NOTIFY_RECENT; ++i) {
        if (notify->recent_ids[i] == message_id) {
            return 1;
        }
    }
    notify->recent_ids[notify->recent_pos] = message_id;
    notify->recent_pos = (notify->recent_pos + 1UL) % TG_CHAT_NOTIFY_RECENT;
    return 0;
}

tg_chat_notify_entry *tg_chat_notify_claim(tg_chat_notify *notify)
{
    tg_chat_notify_entry *entry;

    if (notify == 0) {
        return 0;
    }
    if (notify->count >= TG_CHAT_NOTIFY_MAX) {
        ++notify->dropped;
        return 0;
    }
    entry = &notify->queue[notify->count];
    /* A reused slot must not keep the preceding message's sender. */
    entry->sender_name[0] = '\0';
    ++notify->count;
    return entry;
}

void tg_chat_engine_init(tg_chat_engine *engine)
{
    if (engine == 0) {
        return;
    }
    memset(&engine->updates_state, 0, sizeof(engine->updates_state));
    engine->diff_enabled = 1;
    tg_chat_notify_reset(&engine->notify, 0);
}

#if !defined(TG_NO_SELFTEST)
int tg_chat_engine_self_test(void)
{
    tg_chat_engine engine;
    tg_chat_notify_entry *entry;
    unsigned int i;

    /* Poison, then init must produce a clean session. */
    memset(&engine, 0xa5, sizeof(engine));
    engine.diff_enabled = 0;
    engine.updates_state.pts = 12345UL;
    engine.updates_state.qts = 6789UL;
    engine.updates_state.date = 1111UL;
    engine.updates_state.seq = 2222UL;

    tg_chat_engine_init(&engine);

    if (engine.diff_enabled != 1) {
        puts("chat engine self-test: diff flag default wrong");
        return 2;
    }
    if (engine.updates_state.pts != 0UL || engine.updates_state.qts != 0UL ||
        engine.updates_state.date != 0UL || engine.updates_state.seq != 0UL) {
        puts("chat engine self-test: cursor not zeroed");
        return 2;
    }
    if (engine.notify.count != 0UL || engine.notify.dropped != 0UL ||
        engine.notify.armed != 0) {
        puts("chat engine self-test: notify not clean after init");
        return 2;
    }

    /* Dedupe ring: first sight new, second sight a duplicate; id 0 never dedup. */
    if (tg_chat_notify_seen(&engine.notify, 100UL) != 0) {
        puts("chat engine self-test: first id reported as seen");
        return 2;
    }
    if (tg_chat_notify_seen(&engine.notify, 100UL) != 1) {
        puts("chat engine self-test: duplicate id not deduped");
        return 2;
    }
    if (tg_chat_notify_seen(&engine.notify, 0UL) != 0 ||
        tg_chat_notify_seen(&engine.notify, 0UL) != 0) {
        puts("chat engine self-test: id 0 wrongly deduped");
        return 2;
    }

    /* Overflow: claim exactly MAX entries, then the next claim drops. */
    for (i = 0U; i < TG_CHAT_NOTIFY_MAX; ++i) {
        entry = tg_chat_notify_claim(&engine.notify);
        if (entry == 0) {
            puts("chat engine self-test: claim failed before full");
            return 2;
        }
        if (entry->sender_name[0] != '\0') {
            puts("chat engine self-test: claimed slot kept its sender");
            return 2;
        }
        strcpy(entry->sender_name, "Previous sender");
        entry->is_chat = 0;
    }
    if (tg_chat_notify_claim(&engine.notify) != 0) {
        puts("chat engine self-test: claim past full did not drop");
        return 2;
    }
    if (engine.notify.count != TG_CHAT_NOTIFY_MAX || engine.notify.dropped != 1UL) {
        puts("chat engine self-test: overflow counters wrong");
        return 2;
    }
    tg_chat_notify_reset(&engine.notify, 1);
    entry = tg_chat_notify_claim(&engine.notify);
    if (entry == 0 || entry->sender_name[0] != '\0') {
        puts("chat engine self-test: reset then claim kept its sender");
        return 2;
    }

    /* NULL-safety. */
    tg_chat_notify_reset(0, 1);
    if (tg_chat_notify_seen(0, 5UL) != 1 || tg_chat_notify_claim(0) != 0) {
        puts("chat engine self-test: NULL queue not handled");
        return 2;
    }

    puts("chat engine self-test: ok (cursor, diff, notify dedupe + overflow)");
    return 0;
}
#endif /* !TG_NO_SELFTEST */
