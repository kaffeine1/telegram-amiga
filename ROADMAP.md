<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# Roadmap

Telegram Amiga is a non-commercial community project. The roadmap is deliberately
pragmatic: each phase should produce something that can be compiled and verified
on at least one real Amiga-like platform.

## Phase 1: Portable Base

- Cross-platform project structure
- Separate builds for MorphOS, AmigaOS 3.x, AmigaOS 4.x and AROS
- Common logging and configuration
- Portable TCP API
- Command-line network tests

## Phase 2: HTTP and TLS

- Minimal HTTP over TCP
- Optional TLS/HTTPS backend
- Certificate validation
- OpenSSL/AmiSSL stabilization on MorphOS
- Selection of the most suitable TLS library for AmigaOS 3.x

## Phase 3: Telegram API

- HTTPS calls to the Bot API or another Telegram API suitable for the target
- Minimal JSON parsing
- Account/token configuration handling
- First message receive tests
- Inbox-format receive-only polling with persistent offsets

## Current Status

The project has moved past the Bot-API diagnostic tester to a real MTProto
client:

- MTProto human releases ship for AmigaOS 3.x, AmigaOS 4.x, MorphOS,
  AROS i386 and AROS x86_64: login wizard, 2FA/SRP, saved chat list,
  text chat and file transfer.
- Release 0.0.7 adds: robust big-file transfers (live %, chunk retry,
  cancellable, send timeouts on stalled links), GUI text selection and
  Copy/Cut/Paste in an Edit menu, reply on double-click, TUI file
  transfer/drop support, live remote edits and receive-only updates while
  composing, accented-name search, and a TUI_MODE icon tooltype.
- Release 0.0.8 (transfers 2.0): non-blocking transfers (use the client
  while a file moves) with pipelined downloads and drag-and-drop upload,
  multi-DC downloads and avatars, local-first search with a two-stage
  online search and a browse of the full dialog list, clickable URLs,
  system-coloured menus, chat list reload with a memory of the chats you
  removed, a configurable download drawer, and AfA_OS (AmiKit)
  compatibility. First release cycle with an adversarial review pass.
- Release 0.0.9 (photos): inline photo bubbles fed by an instant blurred
  preview from Telegram's stripped thumbnail, a progressive viewer, a
  canonical pixel cache on disk with a size limit and a clear action,
  pacing measured per cost centre, and JPEG upload as a native photo.
  Persistent Settings, forwarding to Saved Messages or a chosen chat,
  hidden chats in local search, and a text client that word-wraps and
  grows its composer. System datatypes remain an optional OS4-side
  optimization, while the zero-install in-binary decoder stays the
  portable base path.
- Next come the 0.0.9x releases, then 0.1.0, the first BETA: same
  program, a different promise. It ships once no known freeze remains on
  any of the five platforms, an adversarial review pass has run, and a
  full cycle has gone by without field regressions. See "The road to
  0.1.0" below for what each step carries.
- Later: per-chat file browser, multi-message selection, archive management.
- A Bot-API text path stays available as a fallback for tokens/bots.
- TLS certificate validation has passed a live CA-bundle smoke test on all four
  platforms (see `docs/TLS_CERTIFICATES.md`).
- Ongoing: broader community testing, reliability work on slow links and
  packaging polish for future releases.

## Phase 4: User Interface

- Initial text interface for debugging
- Common UI abstraction
- Platform-specific UI backends
- Native experience for MorphOS and AmigaOS where possible

## Phase 5: Usable Client

- Supported chat or conversation list
- Reading and sending messages
- Minimal local persistence
- Packaging for the supported platforms

## The road to 0.1.0

0.0.9 shipped, and 0.1.0 is the first beta. In between come the 0.0.9x
releases, whose job is to put every new feature in front of real
machines before the promise changes. The numbering counts up to the
beta on purpose, so anyone watching can see it coming.

Few large steps rather than many small ones, and for a reason that is
easy to miss: the beta gate asks for a full cycle with no field
regressions, and every release restarts that clock. Each one also costs
five fresh builds, four publishing channels and a validation pass over
five platforms, so the cost lives in the ritual, not in the code.
Features are therefore grouped by the work they share.

- **0.0.91, the visible polish.** Already half done on `main`: the
  self-tests compiled out of every lane, so the binaries are lighter,
  and the caret, reply strip and unread counts lined up with their
  text. To come: the repository link in About, grouping consecutive
  messages from one sender, rounded bubble corners, the avatar in the
  chat header, and captions on outgoing photos. All low risk, all
  immediately visible.
- **0.0.92, media coming in.** Link previews, stickers with their own
  emoji and still frame, videos with details and a frame. They share
  the same document parsing and the same photo pipeline, so they are
  cheaper together than apart.
- **0.0.93, sending.** The emoji picker with its glyph sheet, and
  sending images that are not JPEGs, PNG first.
- **0.0.94, speed and open defects.** Whatever the transfer-ceiling
  measurement turns up, the two MorphOS popup glitches, photo speed
  under AfA_OS and two-step verification on a slow 68k.
- **Room to spare.** The numbering leaves several slots between 0.0.94
  and the beta on purpose. Field reports arrive faster than plans, and
  an intermediate release is a normal thing to need, not a sign that
  something went wrong.
- **0.1.0, the beta.** No new features: a quiet cycle, an adversarial
  review pass, and the texts brought up to date.

Two things run outside this list. The plain-68000 crash hunt has its
own rhythm, since it depends on field logs from one machine. And emoji
drawn inside the transcript, the part that has to split text runs in
the renderer, stays late or waits for after the beta: that is the code
that has hurt us under AfA_OS, and it is the last thing to introduce
while trying to prove a quiet cycle.

## Planned: two-step verification on slow 68k

Signing in with Two-Step Verification derives the key with PBKDF2 (100000
iterations of SHA-512). On a stock 14 MHz 68020 that is roughly forty
minutes, and Telegram expires the SRP challenge long before it ends, so the
password can never be checked (a field report from a 68020 saw exactly that:
no error, just an expired session at the end of the wait).

The wait itself cannot shrink much -- the HMAC midstates are already
precomputed, so each iteration is down to two block transforms. What can
change is the ORDER. The slow derivation depends only on the password and
the account salts, both stable; only srp_id and srp_B expire. So:

1. fetch `account.getPassword`, keep the salts;
2. run the PBKDF2 derivation;
3. fetch `account.getPassword` again for a fresh srp_id and srp_B;
4. compute the SRP proof (short exponents, comparatively quick) and send
   `auth.checkPassword` straight away.

That turns a guaranteed failure into a long but completable login. It needs
`tg_mtproto_srp_make_proof` split into a derivation step and a proof step.

## Planned: link previews (0.0.92)

First field feedback on 0.0.9: link previews pasted into chats show up
for some links and not for others. The ones that appear are not link
previews at all; they are real attached photos with a caption (the usual
news-channel format), which the inline-photo pipeline renders. A bare
pasted link arrives as `messageMediaWebPage`, which the client currently
skips entirely, by design; the bubble shows only the clickable URL.

Plan, in order:

1. Parse `messageMediaWebPage` (TL constructors verified against the
   layer-214 schema first): show the page title or site name plus the
   first description line under the message text. The TUI shows the
   title line only.
2. When the preview carries a photo, feed it to the existing bounded
   photo pipeline (stripped preview, incremental fetch, disk cache), so
   the image costs nothing new and obeys the same inline-photos setting.
3. Optional, only if the cycle has room: handle `updateWebPage`, so a
   preview the server generates late still reaches an open chat.

Even complete, previews will stay per-link: the server builds them from
the target page's metadata, so pages without usable metadata show none
(`webPageEmpty`, same as official clients), pending ones arrive later
(`webPagePending`), and a sender can disable the preview per message.
This moves link previews out of the Tier 4 "degrade to a link line"
non-goal on the strength of the field reports.

## Planned: GitHub link in About

The About requester credits the author, the contributors and the
testers, but never says where the project lives. Add the repository URL
(https://github.com/kaffeine1/telegram-amiga) so a user who wants to
report something or fetch a newer build reads the address right there;
it is the same place the manuals and the Aminet readme already point to.

## Planned: captions when sending photos

Requested alongside the 0.0.9 feedback: let the user attach a caption to
a photo they send. The wire side is already there; both media writers
(photo and document) call `messages.sendMedia` with an empty caption
string today, so the protocol work is filling one field the client
already sends, plus the composer's existing UTF-8 conversion. Received
captions already render under the inline photo. The real work is the
input surface:

- GUI: whatever sits in the composer when the photo send is confirmed
  becomes the caption, after a one-line confirm requester when the
  composer is not empty (so an unrelated draft is never swallowed).
  Applies to Send photo..., the context-menu entry and the Workbench
  drop alike; the status line says when a caption went along.
- TUI: `/photo <path> [caption words...]`, everything after the path is
  the caption.
- The over-10-MiB fallback that sends a photo as a document carries the
  same caption.

## Planned: video messages, details and hand off to a player

A video arrives today as a plain `[File]` with its name and size, which
tells the user nothing about what a slow line is about to spend minutes
on. Three things make it a real message, and most of the machinery is
already in the tree:

- The still frame, inline. `tg_mtproto_read_document` already walks the
  document's `thumbs` vector and skips each entry; picking the best one
  instead hands a normal photo to the 0.0.9 pipeline, stripped preview
  included, under the existing Inline photos preference.
- A details line: duration and resolution from `documentAttributeVideo`
  (only the filename attribute is parsed today) next to the size and the
  format, which is free because the parser already reads the mime type
  and nobody uses it.
- Download and play: fetch with the same bounded, cancellable engine as
  any other file, then hand the result to a player command the user sets
  once, through the same SystemTags path that opens links since 0.0.8.

The split is not 68k against PPC, it is what the player on that machine
can decode. AmigaOS 4, MorphOS and AROS play what Telegram sends, so
there the feature is complete. Fast 68k systems (an accelerated 040 or
060, Vampire, PiStorm) play the formats they were always good at, which
is what RiVA exists for; H.264 is out of their reach, so they get the
frame, the details and the download, and play whatever their own player
handles. One implementation, each machine takes what it can.

No codec and no transcoding goes into the client: that is a different
program, several times this one's size, against the zero-dependency
rule. A hosted transcoder was considered and dropped on purpose. It
would cost real money in proportion to how popular it got, it would make
the project an intermediary for whatever users pushed through it, it
would end the claim that nothing of ours ever sees your media, and it
would make the headline feature die the day the bill went unpaid. The
conversion recipe belongs with the user instead: the download drawer is
configurable onto a network share, and Saved Messages already works as a
two-way drawer between the Amiga and a machine that has the CPU.

## Planned: update notice, and later self-update

A user asked for automatic update download and install, the way another
actively developed Amiga program does it. Worth doing, but staged, and
not through HTTPS: the TLS path is compiled out on every lane
(`ENABLE_TLS ?= 0`), and switching it on would mean a CA bundle and a
new attack surface on the slowest machines.

The natural channel is the one the client already speaks. Releases get
announced on a Telegram channel; resolving a public channel by username
and reading its history is existing code, and fetching the attachment is
the same bounded, cancellable, multi-DC download used for any file.

1. Notice only: read the latest published version, compare it with
   `TG_VERSION`, and say that a newer one exists and where to get it.
   Nothing is downloaded and nothing is executed, so this carries no new
   risk and already covers most of the convenience.
2. Fetch to a drawer: download the new executable next to the current
   one and let the user put it in place. Still no automatic swap.
3. Verified self-install, opt-in and off by default.

Step 3 is a remote code path and needs a real trust anchor, not just
"the right channel": an RSA signature over the binary, verified against
a public key built into the client. The primitives are already in tree
(SHA-256 in `tg_mtproto_crypto.c`, the public-exponent modular
exponentiation in `tg_mtproto_rsa.c`). The cost is operational rather
than technical: a signing key that has to stay safe for as long as the
project ships updates.

Amiga specifics for steps 2 and 3: an executable can be replaced while
it runs, since LoadSeg reads it into memory, but AmigaDOS `Rename` does
not overwrite, so the sequence is download, verify, keep a backup of the
current binary, then swap. Updates carry the executable alone, not the
`.lha`, because the client has no archive extractor and shipping one
costs size and licensing. On a slow link half a megabyte is minutes, so
the transfer stays cancellable and off the event loop, like every other
transfer since 0.0.8.

Timing: step 1 fits a 0.0.9x cycle. Steps 2 and 3 belong after the
0.1.0 beta, whose gate asks for no known freeze and a quiet cycle, which
is the worst moment to add a path that installs code.

## Planned: send other image formats, PNG first

Only a JPEG can be sent as a photo today. The GUI decides from the file
extension, and the upload path then checks the magic bytes and the SOF
segment, refusing anything else with "not a valid JPEG"; every other
image still goes out as a document, which works but arrives as a file
rather than a picture.

The upload itself is format agnostic, it is bytes plus
`inputMediaUploadedPhoto`, so the work splits in two very different
halves.

PNG is the cheap half and comes first. Telegram accepts it as a photo
and re-encodes it server side, so the client only has to recognise it
(the eight-byte signature, then IHDR for the dimensions) and let the
existing upload run. That means teaching the extension check and the
validator about a second format instead of hardcoding one, and
reporting the server's own refusals (`PHOTO_EXT_INVALID`,
`PHOTO_INVALID_DIMENSIONS`, `IMAGE_PROCESS_FAILED`) by name, the way
other RPC errors already are.

IFF ILBM is the interesting half, and the one that matters on this
platform: it is what an Amiga actually produces, and Telegram will
never accept it. That needs an encoder of our own, which sounds worse
than it is. A PNG can be written with stored, uncompressed deflate
blocks, so the whole writer is a CRC32 table, an Adler-32 sum and three
chunks, with no compressor at all. A palette ILBM maps onto an indexed
PNG with a PLTE chunk, which keeps a 320x256 screen near 80 KB; the
truecolour path costs roughly 245 KB for the same size. Both are fine
to upload, and it would let people send their own screens and artwork
as pictures rather than as attachments.

Anything we cannot turn into a picture keeps going as a document,
unchanged.

## Planned: find out what caps transfer speed

A field measurement on MorphOS: a speed test running on that same
Amiga, over that same stack and that same line, reports about 90 Mbit/s
with a 6.71 ms round trip, while our transfers sit around 320 KB/s,
which is 2.6 Mbit/s, under 3 per cent of it. Another Amiga program
reaching 90 Mbit/s on the machine settles the question of whether
bsdsocket, the interface or the line can go faster: they can. The same
ceiling shows on every architecture, which points the same way. What
holds us back is ours.

What the code does today. A part is 64 KB (32 KB on m68k). Uploads are
strictly serial: build the part, send `saveFilePart`, wait for the
`boolTrue`, then the next one. Downloads are one step better, since
0.0.8 keeps exactly one chunk prefetched while the current one lands.
Telegram itself allows parts up to 512 KB and several requests in
flight.

The arithmetic is the interesting part. 320 KB/s in 64 KB parts is five
parts a second, so 200 ms per part, against a round trip of under 7 ms.
The network wait explains about three per cent of that time. So the
first move is NOT to make the parts bigger or to pipeline harder: it is
to find out where those 200 ms actually go, the same way the photo
pacing work did it, by timing each cost centre of one part separately.
Building the query, our own AES-IGE and SHA-256 over the payload, the
socket write, the wait for the reply, the read back.

There is already one suspect worth timing first, because it fits every
piece of the evidence. `tg_mtproto_recv_exact` loops until it has the
whole chunk, and each turn of that loop calls the platform receive,
which on every lane does a `WaitSelect` and then a `recv`. The stack
hands over what has arrived, typically around an MTU, so a 64 KB chunk
costs roughly forty-five `WaitSelect` calls, each one a trip through
the scheduler and back. A few milliseconds apiece is all it takes to
account for the whole 200 ms, it would scale with bytes rather than
round trips exactly as observed, and being in shared code it would cap
every architecture at the same place, which is precisely what the field
reports say. If that is confirmed, the fix is small: try the receive
first and only wait when the socket is actually dry, and give the
socket a bigger receive buffer so each wake brings more.

Whatever the measurement says, the fixes follow the answer. If the wait
dominates, bigger parts and more requests in flight are the lever, and
the upload side has no pipelining at all to lose. If our crypto
dominates, that is a completely different job, and on PPC or a Vampire
it eventually means using what those chips actually have. Either way
the transfer has to stay inside the non-blocking pump, because the
window must keep breathing while a file moves.

No promise about the number that comes out: a retro machine will not
saturate a 90 Mbit line, and it does not need to. But under three per
cent of it is not a hardware limit, it is something we are doing.

## Design direction: closer to the desktop client

The GUI should read as Telegram to someone who already uses Telegram,
so the desktop client is the reference for layout, hierarchy and where
things live. As a reference, not as a source: the note at the end of
the emoji section applies to pixels as much as to code.

Aiming for the same visual vocabulary is not the same as aiming for the
same pixels, and on this hardware the difference matters. A paletted
screen has a pen budget, there is no alpha blending to lean on, the
font is fixed width, and an AmigaOS 3 window can be 640 by 256. So the
rule is that the layout and the hierarchy travel, while the chrome gets
translated: what the desktop does with a gradient and a shadow, we do
with a pen role and a border, and the low end drops the decoration
without ever losing the arrangement.

Much of it already lines up: the dark theme, the sidebar of avatars
with names and unread pills, our own messages right aligned in the
accent colour against incoming ones on the surface colour, day
separators, delivery ticks, the typing line, the reply strip over the
composer and the scroll-to-bottom button carrying its unread count.

The gaps worth closing, cheapest and most visible first:

- Consecutive messages from the same sender still repeat the name.
  Grouping them the way the desktop does is a small change to the
  bubble geometry, it is the single most recognisable difference in a
  busy group, and it buys back vertical space, which on a 640 by 256
  screen is worth having on its own.
- The chat header carries no avatar yet. That has its own entry above.
- Bubbles are plain rectangles. Clipping the four corners costs a
  handful of pixels and no new pens, and it is most of what makes a
  bubble read as a bubble.
- The composer has no attach or emoji buttons, so the actions that the
  desktop puts in reach live only in menus and the right-click popup.
- The pen roles were chosen by us. Mapping them onto the desktop's dark
  palette would cost nothing at runtime and would settle a lot of small
  inconsistencies at once.

Optional and only where there is room: the desktop puts the sender's
avatar beside incoming bubbles in groups. That eats horizontal space a
small screen does not have, so it belongs behind a width check if it
lands at all.

## Planned: stickers inline, and emoji that look like emoji

Two field requests that share a renderer.

A sticker arrives as a document, so today it shows as a plain file
label with a filename, which tells the reader nothing about what was
sent. Three steps, in order of what they cost:

1. Almost free: `documentAttributeSticker` is already parsed, but its
   `alt` field, the emoji the sticker stands for, is thrown away by a
   skip. Reading it instead turns the bubble into the sticker's own
   emoji rather than `Sticker.webp`.
2. The picture: a sticker document carries the same `thumbs` vector the
   inline-photo work already walks, so picking the best entry and
   handing it to the photo pipeline shows the sticker as a still, under
   the existing Inline photos preference. Worth checking per sticker
   type first, since our decoder reads JPEG and not every sticker thumb
   is one.
3. Not planned: WEBP, the gzipped Lottie of animated stickers and WEBM
   video stickers. We have no decoder for any of them, and animation is
   not what this client is for.

Emoji already do more than they appear to: 112 codepoints are mapped by
hand to text emoticons, so a grinning face reads as `:D` and a heart as
`<3`, and symbols with no sensible shape become a neutral placeholder
instead of a question mark, which would read as lost text. Two separate
jobs remain, and the less obvious one matters more.

Sending them is impossible today. An Amiga keyboard produces Latin-1,
so there is no way to type a codepoint the composer can send, and a
user who receives `:D` cannot answer in kind. That is the gap to close
first, with a picker in the spirit of the desktop client: a panel above
the composer, categories along one edge, a grid to walk with the arrow
keys, ENTER to insert, ESC to leave, and a row of recently used ones
that survives between runs like the other preferences. The rest of the
road already exists, since the send path has converted the composer to
UTF-8 from the start.

The picker also settles the order of the drawing work, because a grid
is the easy half. Painting a glyph into a cell of a panel needs no text
layout at all, so a small built-in glyph sheet pays off immediately
there, and the curated list already says which emoji are worth having.
Only afterwards comes the hard half, showing them inside the
transcript, where the renderer has to break a line into runs to place
an image between them. That is the code that has hurt us before under
AfA_OS, so it gets its own hardware pass before anyone calls it done.

On borrowing: the desktop client is worth studying for how a feature
should behave, and that is how it will be used here, as a reference for
behaviour and layout. Not for code. Telegram Desktop is GPLv3 and this
project is MIT, so lifting source from it, or porting it line by line,
is not something we can do. Where facts are needed rather than ideas,
the safer wells are the published TL schema and TDLib, which carries a
permissive licence, and for the emoji list and its categories the
Unicode data files, which is where everyone else gets them anyway.

## Planned: split long pastes into protocol-sized messages

A 6 KB pasted text is refused today (issue #14). That ceiling is
Telegram's own: the protocol caps a single message at 4096 characters,
and the composer buffer matches it (2048 on classic 68k, where RAM is
the budget). The desktop client handles the same paste by quietly
splitting it into consecutive messages, and that is the behaviour to
copy: when the text to send exceeds the cap, split at the last line
break or space before the limit, send the pieces one after another
through the existing send path, and stop cleanly at the first failure
so nothing is lost silently. Same rule in the TUI. Until then the
manuals' answer stands: a long text travels better as a document.

## Planned: new-message notification without stealing focus

A user asked for the screen to come to the front when a message arrives,
optional, for the times the client sits in the background while they do
something else. The need is real; the mechanism they suggested is not
the one to build. On the Amiga, pulling a screen or a window to the
front while someone is typing elsewhere is a system-wide interruption,
and it is exactly what the desktop client avoids: it signals, it never
grabs. So the plan is a notification, and it has to work in both states
the client can be in.

Window open, other things in front. The desktop client changes its
title and taskbar badge; the Amiga equivalent is the window title. When
a message lands in a chat other than the open one, the title becomes
"Telegram Amiga (3 new)" through SetWindowTitles, which repaints only
the title bar and stays visible on the screen's depth-arranged windows,
and it reverts when the chat is opened. Optional, off by default: one
DisplayBeep on the first unread of a run, which on Amiga is the polite
"look at me" (a screen flash, and a sound where the user configured
one), never a raise. Both are two lines each on top of the notification
collector the sidebar flash already uses.

Parked on the AppIcon. Today the iconified wait sleeps in Wait() on the
AppIcon's port alone, so a parked client neither reads the network nor
could ever notice a message. That changes to a Wait() on the port plus
the session's own network signal with a timer tick, so the update poll
keeps running while parked, and unread arrivals redraw the AppIcon's
label with the count, "TelegramAmiga (3)", where AddAppIcon allows a
text change or through a remove and re-add otherwise. Double-clicking
the icon, as now, brings the window back on the chat with the news.

Kept out on purpose: raising the screen or the window, and any hover
popup or ARexx or commodities integration for now. If people ask for a
hook, a simple one exists later, running a user command such as a sound
player on the first unread; it is a preference away, not a design.

Settings entry: "Notify: Title | Title + beep | Off". Fits the 0.0.9x
polish line and touches nothing in the protocol.

## Planned: the send-photo dialog's preview, done right

The dialog shipped with a pixel preview and lost it: three MorphOS
validation rounds judged it ugly, and the culprit turned out to be
found only after the decision to pull it. The preview decoded through
`tg_avatar_decode_jpeg`, which is built for avatars and passes through
a 64 by 64 intermediate before nearest-scaling up, so a 320-pixel
preview was a thumbnail blown up five times; no pen path could look
good downstream of that. The dialog now shows the file's name and size
where the picture was, which at least is never wrong.

Bringing the picture back is small and precise: decode with
`tg_image_decode_jpeg_scaled`, the message-photo path, with a source
edge cap sized to the preview box, then hand the pixels to the same
two renderers the dialog already had, direct RGB888 where the photo
pipeline's check passes and the inline photos' pen mapping elsewhere.
The 6 MiB whole-file read cap and the aspect-fit window sizing from
the pulled version are worth keeping. Fits any 0.0.9x cycle, and it is
the kind of change one MorphOS screenshot can judge in a minute.

## Planned: two MorphOS popup glitches

Both reported from the field on 0.0.9 and both about the right-click popup
living next to inline photos.

1. Opening the popup beside a photo draws it BEHIND the picture. The window
   paint composes the popup last for exactly this reason, but the photo
   replay is a separate pass that writes straight to the window RastPort
   (the CyberGraphX path) after the blit, so it lands on top. The popup
   needs to be excluded from the replay's dirty rectangles, or repainted
   after them.
2. On an own screen, right-clicking in the conversation to pick an entry
   makes the chat flicker. The popup path repaints more than it needs to
   there; the transcript should keep its pixels while only the popup area
   is composed.

Neither affects the other lanes, where the replay goes through the friend
bitmap before the blit rather than to the window.

## Planned: photo speed under AfA_OS

Rows are handed to cybergraphics in blocks of 8 on the 68k line, which took
the per-slice cost from 220-820 ms down to 0-20 ms on a Vampire. AmiKit still
shows a tail of slow slices (180-620 ms), so the per-call overhead there is
higher than elsewhere. First move, cheap and mechanical: raise
TG_GUI_PHOTO_REPLAY_ROWS from 8 to 16 on m68k (about 12 KB more of staging
buffer) and measure again from the log's "pace replay budget" lines. If the
tail survives that, the cost is not the call count and the investigation
below is the real answer.


Same PiStorm board, two operating systems, two very different speeds: photo
loading under AmiKit (which runs AfA_OS) is noticeably slower than on a
Vampire, while CaffeineOS on that same PiStorm is quick. The hardware is
therefore innocent; something in the AfA_OS graphics path is costing us.

First step is cheap and decisive: run both with `--gui-live-debug` and
compare which replay path each one reports. If AmiKit logs the pen path
where CaffeineOS logs the CyberGraphX one, the RGB888 self-check is failing
under AfA_OS and the fix belongs there (why it fails, and whether the window
RastPort is usable when the off-screen buffer is not). If both report the
same path, the cost is elsewhere -- most likely in the AfA bitmap-text
compatibility work, which already replaces Text() with BltTemplate glyph
runs and is the one thing this configuration does differently.

## Initial Non-Goals

- End-to-end encryption for secret chats
- Full support for heavy media
- Complete compatibility with every feature of modern Telegram clients
