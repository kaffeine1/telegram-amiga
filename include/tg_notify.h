/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_NOTIFY_H
#define TG_NOTIFY_H

/* Shared by the window and the text client; no window state is needed. */
#define TG_NOTIFY_OFF 0
#define TG_NOTIFY_TITLE 1
#define TG_NOTIFY_POPUP 2
#define TG_NOTIFY_POPUP_TEXT 3
#define TG_NOTIFY_DEFAULT 0
#define TG_NOTIFY_MUTE 1
#define TG_NOTIFY_RING 2
#define TG_NOTIFY_SOUND_MAX 256 /* 255 bytes of path plus the terminator */
#define TG_NOTIFY_QUIET_SECONDS 30UL
#define TG_NOTIFY_FILE "data/telegram-notify.txt"
#define TG_NOTIFY_OVERRIDE_FILE "data/telegram-chat-notify.txt"

typedef struct tg_notify_override {
    int choice;
    char sound[TG_NOTIFY_SOUND_MAX];
} tg_notify_override;

typedef struct tg_notify_ring_peer {
    unsigned long peer_constructor;
    unsigned long peer_hi;
    unsigned long peer_lo;
    unsigned long last_ring;
    int pending;
} tg_notify_ring_peer;

void tg_notify_preferences_load(const char *path, int *mode, int *beep,
                                int *mode_explicit, char *sound,
                                unsigned long sound_size);
int tg_notify_preferences_save(const char *path, int mode, int beep,
                               int mode_explicit, const char *sound);
void tg_notify_override_get(const char *path, unsigned long peer_hi,
                            unsigned long peer_lo, tg_notify_override *out);
int tg_notify_override_set(const char *path, unsigned long peer_hi,
                           unsigned long peer_lo,
                           const tg_notify_override *value);
/* Telegram's mute uses server time. The quiet spell uses local time, so an
   updated server offset cannot make a conversation ring again too soon. */
int tg_notify_should_ring(unsigned long peer_constructor,
                          unsigned long mute_until, int peer_silent,
                          int choice, int silent, int is_out,
                          int open_in_front, unsigned long server_now,
                          unsigned long local_now, unsigned long last_ring);
/* NULL means silence; an empty string means the built-in chime. */
const char *tg_notify_sound_choice(int ring, const char *chat_sound,
                                   const char *global_sound);

#define TG_NOTIFY_CHIME_RATE 22050UL
#define TG_NOTIFY_CHIME_NOTE 2420UL
#define TG_NOTIFY_CHIME_LENGTH (2UL * TG_NOTIFY_CHIME_NOTE)
void tg_notify_chime_render(signed char *out);

/* A sound file is loaded in the caller's task (at most 1 MB, no requesters)
   and plays on its own through the sound.datatype; tg_notify_sound_poll,
   called from the event loops, lets it go at its end or after three
   seconds. LOWMEM uses only DisplayBeep. A failed file or unavailable
   datatype falls back to the chime. Shutdown stops any sound still held.
   Play returns what was heard, and tg_notify_sound_why says why a file
   was not, for the debug log. */
#define TG_NOTIFY_HEARD_NOTHING 0
#define TG_NOTIFY_HEARD_FILE 1
#define TG_NOTIFY_HEARD_CHIME 2
#define TG_NOTIFY_HEARD_BEEP 3
int tg_notify_sound_play(const char *path);
const char *tg_notify_sound_why(void);
void tg_notify_sound_poll(void);
void tg_notify_sound_shutdown(void);

#endif
