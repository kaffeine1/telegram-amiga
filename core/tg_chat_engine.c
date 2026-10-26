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

unsigned long tg_reaction_codepoint(const tg_reaction *reaction)
{
    return (unsigned long)reaction->emoji[0] |
           ((unsigned long)reaction->emoji[1] << 8) |
           ((unsigned long)reaction->emoji[2] << 16);
}

unsigned long tg_reaction_count(const tg_reaction *reaction)
{
    return (unsigned long)reaction->count[0] |
           ((unsigned long)reaction->count[1] << 8) |
           ((unsigned long)reaction->count[2] << 16) |
           ((unsigned long)reaction->count[3] << 24);
}

void tg_reaction_set(tg_reaction *reaction, unsigned long cp,
                     unsigned long count, int chosen)
{
    int i;

    if (cp > 0x10ffffUL) cp = 0UL;
    if (count > 2147483647UL) count = 2147483647UL;
    for (i = 0; i < 4; ++i) {
        reaction->count[i] = (unsigned char)(count & 255UL);
        count >>= 8;
        if (i < 3) {
            reaction->emoji[i] = (unsigned char)(cp & 255UL);
            cp >>= 8;
        }
    }
    reaction->chosen = (unsigned char)(chosen != 0);
}

/* The sheet has single scalars. Accept their optional presentation selector,
   but never turn half of a compound emoji into a different reaction. */
unsigned long tg_reaction_parse_emoji(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    unsigned long cp;
    int n;
    int i;

    if (s[0] < 0x80U) { cp = s[0]; n = 1; }
    else if (s[0] >= 0xc2U && s[0] < 0xe0U) { cp = s[0] & 31U; n = 2; }
    else if (s[0] < 0xf0U && s[0] >= 0xe0U) { cp = s[0] & 15U; n = 3; }
    else if (s[0] >= 0xf0U && s[0] <= 0xf4U) { cp = s[0] & 7U; n = 4; }
    else return 0UL;
    if (cp == 0UL && n == 1) return 0UL;
    for (i = 1; i < n; ++i) {
        if ((s[i] & 0xc0U) != 0x80U) return 0UL;
        cp = (cp << 6) | (s[i] & 63U);
    }
    if ((n == 2 && cp < 0x80UL) || (n == 3 && cp < 0x800UL) ||
        (n == 4 && cp < 0x10000UL) || cp > 0x10ffffUL ||
        (cp >= 0xd800UL && cp <= 0xdfffUL)) return 0UL;
    if (s[n] != 0U && strcmp((const char *)s + n, "\xef\xb8\x8f") != 0)
        return 0UL;
    return cp;
}

unsigned long tg_reaction_default(int index)
{
    static const unsigned long cps[TG_REACTION_DEFAULTS] = {
        0x1f44dUL, 0x2764UL, 0x1f525UL, 0x1f923UL, 0x1f631UL,
        0x1f622UL, 0x1f389UL, 0x1f64fUL, 0x1f44fUL, 0x1f44cUL
    };
    return index >= 0 && index < TG_REACTION_DEFAULTS ? cps[index] : 0UL;
}

const char *tg_reaction_name(int index)
{
    static const char *const names[TG_REACTION_DEFAULTS] = {
        "Thumbs up", "Heart", "Fire", "Laughing", "Shocked",
        "Sad", "Party", "Pray", "Clap", "OK"
    };
    return index >= 0 && index < TG_REACTION_DEFAULTS ? names[index] : "";
}

const char *tg_reaction_utf8(int index)
{
    static const char *const emoji[TG_REACTION_DEFAULTS] = {
        "\xf0\x9f\x91\x8d", "\xe2\x9d\xa4", "\xf0\x9f\x94\xa5",
        "\xf0\x9f\xa4\xa3", "\xf0\x9f\x98\xb1", "\xf0\x9f\x98\xa2",
        "\xf0\x9f\x8e\x89", "\xf0\x9f\x99\x8f", "\xf0\x9f\x91\x8f",
        "\xf0\x9f\x91\x8c"
    };
    return index >= 0 && index < TG_REACTION_DEFAULTS ? emoji[index] : "";
}

int tg_reaction_choice(const tg_reactions *reactions, int index)
{
    int i;

    if (index < 0 || index >= TG_REACTION_DEFAULTS) return -2;
    for (i = 0; i < reactions->count && i < TG_REACTION_MAX; ++i) {
        if (reactions->items[i].chosen &&
            tg_reaction_codepoint(&reactions->items[i]) == tg_reaction_default(index))
            return -1;
    }
    return index;
}

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
    tg_reactions reactions;

    memset(&reactions, 0, sizeof(reactions));
    reactions.count = 1;
    tg_reaction_set(&reactions.items[0], 0x1f44dUL, 2147483647UL, 1);
    if (sizeof(reactions) != 34U ||
        tg_reaction_count(&reactions.items[0]) != 2147483647UL ||
        tg_reaction_choice(&reactions, 0) != -1 ||
        tg_reaction_choice(&reactions, 1) != 1) {
        puts("chat engine self-test: reaction count/toggle failed");
        return 2;
    }
    reactions.items[0].chosen = 0;
    if (tg_reaction_choice(&reactions, 0) != 0 ||
        tg_reaction_parse_emoji("\xe2\x9d\xa4\xef\xb8\x8f") != 0x2764UL ||
        tg_reaction_parse_emoji("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbb") != 0UL ||
        tg_reaction_parse_emoji("\xe2") != 0UL ||
        tg_reaction_parse_emoji("\xe0\x80\x80") != 0UL) {
        puts("chat engine self-test: reaction emoji/toggle failed");
        return 2;
    }
    for (i = 0; i < TG_REACTION_DEFAULTS; ++i) {
        /* The single-codepoint standard reactions, as the Bot API lists
           them under ReactionTypeEmoji (core.telegram.org/bots/api,
           2026-10-26). Anything else is REACTION_INVALID in every chat:
           tears of joy and an open mouth were, before this list. */
        static const unsigned long standard[] = {
            0x2764UL, 0x1f44dUL, 0x1f44eUL, 0x1f525UL, 0x1f970UL, 0x1f44fUL,
            0x1f601UL, 0x1f914UL, 0x1f92fUL, 0x1f631UL, 0x1f92cUL, 0x1f622UL,
            0x1f389UL, 0x1f929UL, 0x1f92eUL, 0x1f4a9UL, 0x1f64fUL, 0x1f44cUL,
            0x1f54aUL, 0x1f921UL, 0x1f971UL, 0x1f974UL, 0x1f60dUL, 0x1f433UL,
            0x1f31aUL, 0x1f32dUL, 0x1f4afUL, 0x1f923UL, 0x26a1UL, 0x1f34cUL,
            0x1f3c6UL, 0x1f494UL, 0x1f928UL, 0x1f610UL, 0x1f353UL, 0x1f37eUL,
            0x1f48bUL, 0x1f595UL, 0x1f608UL, 0x1f634UL, 0x1f62dUL, 0x1f913UL,
            0x1f47bUL, 0x1f440UL, 0x1f383UL, 0x1f648UL, 0x1f607UL, 0x1f628UL,
            0x1f91dUL, 0x270dUL, 0x1f917UL, 0x1fae1UL, 0x1f385UL, 0x1f384UL,
            0x2603UL, 0x1f485UL, 0x1f92aUL, 0x1f5ffUL, 0x1f192UL, 0x1f498UL,
            0x1f649UL, 0x1f984UL, 0x1f618UL, 0x1f48aUL, 0x1f64aUL, 0x1f60eUL,
            0x1f47eUL, 0x1f937UL, 0x1f621UL
        };
        unsigned long k;
        int listed = 0;

        for (k = 0UL; k < sizeof(standard) / sizeof(standard[0]); ++k) {
            if (standard[k] == tg_reaction_default((int)i)) {
                listed = 1;
            }
        }
        if (!listed ||
            tg_reaction_parse_emoji(tg_reaction_utf8((int)i)) !=
            tg_reaction_default((int)i) || tg_reaction_name((int)i)[0] == '\0') {
            puts("chat engine self-test: reaction defaults failed");
            return 2;
        }
    }

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
