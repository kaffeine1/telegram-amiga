/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_TEXT_STYLE_H
#define TG_TEXT_STYLE_H

/* Internal entity toggles. Reserve C0 bytes away from tab, newline and the
   console's ESC (the spoiler's is the one below it); GUI emoji pairs use
   the separate 0x80/0x81 prefixes. Incoming text must replace these bytes
   before adding entity markers. */
#define TG_TEXT_MARKER_SPOILER '\032'
#define TG_TEXT_MARKER_BOLD   '\034'
#define TG_TEXT_MARKER_ITALIC '\035'
#define TG_TEXT_MARKER_CODE   '\036'
#define TG_TEXT_MARKER_STRIKE '\037'
#define TG_TEXT_IS_MARKER(c) (((unsigned long)(c) >= \
                              (unsigned long)TG_TEXT_MARKER_BOLD && \
                              (unsigned long)(c) <= \
                              (unsigned long)TG_TEXT_MARKER_STRIKE) || \
                             (unsigned long)(c) == \
                             (unsigned long)TG_TEXT_MARKER_SPOILER)
/* Only call for a marker. The console keeps its historical visible output;
   a spoiler stands between two bars there. */
#define TG_TEXT_MARKER_ASCII(c) \
    ((unsigned long)(c) == (unsigned long)TG_TEXT_MARKER_SPOILER \
         ? '|' : "*_`~"[(c) - TG_TEXT_MARKER_BOLD])

#endif
