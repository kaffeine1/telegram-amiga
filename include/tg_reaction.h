/* Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT */
#ifndef TG_REACTION_H
#define TG_REACTION_H

#define TG_REACTION_MAX 4
#define TG_REACTION_DEFAULTS 10

/* Byte fields avoid long alignment in the replicated message arrays. Only
   the displayed scalar survives; custom, paid and compound emoji use '*'. */
typedef struct tg_reaction {
    unsigned char emoji[3];
    unsigned char count[4];
    unsigned char chosen;
} tg_reaction;

typedef struct tg_reactions {
    unsigned char count;
    unsigned char min;
    tg_reaction items[TG_REACTION_MAX];
} tg_reactions;

unsigned long tg_reaction_codepoint(const tg_reaction *reaction);
unsigned long tg_reaction_count(const tg_reaction *reaction);
void tg_reaction_set(tg_reaction *reaction, unsigned long cp,
                     unsigned long count, int chosen);
unsigned long tg_reaction_parse_emoji(const char *text);
unsigned long tg_reaction_default(int index);
const char *tg_reaction_name(int index);
const char *tg_reaction_utf8(int index);
int tg_reaction_choice(const tg_reactions *reactions, int index);
/* The codepoint to send for `cp` on a message: 0 (take it back) when it is
   already the user's own reaction there. */
unsigned long tg_reaction_choice_cp(const tg_reactions *reactions,
                                    unsigned long cp);
/* UTF-8 of a reaction codepoint into out (at least 5 bytes), as Telegram
   lists them: bare, no variation selector. 0 when cp is not encodable. */
int tg_reaction_encode(unsigned long cp, char *out);

/* The reactions a chat allows (its full chat's available_reactions). */
#define TG_REACTIONS_ALLOW_ALL 0
#define TG_REACTIONS_ALLOW_NONE 1
#define TG_REACTIONS_ALLOW_SOME 2
#define TG_REACTION_ALLOWED_MAX 24
typedef struct tg_reactions_allowed {
    int mode;
    int count;
    unsigned long cps[TG_REACTION_ALLOWED_MAX];
} tg_reactions_allowed;

#endif
