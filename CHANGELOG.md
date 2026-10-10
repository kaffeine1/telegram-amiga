# Changelog

Telegram Amiga, a from-scratch native MTProto Telegram client for
AmigaOS 3.x, AmigaOS 4.x, MorphOS and AROS (i386, x86_64 and, from 0.0.94,
aarch64). Dates use YYYY-MM-DD. Each release ships on all six platform
lanes unless noted.

## [Unreleased]

### Fixed
- Typing in the window keeps up with the keys on a stock A1200. Since 0.0.8
  only machines running AfA OS redrew just the text field after each key;
  everywhere else every key redrew the whole window, which on a 14 MHz
  68EC020 took about three seconds a letter. Every system now
  redraws the field alone while its height stays the same, the way the
  blinking cursor already did, and copies only that strip to the screen.
  While the keys are flowing, a new message, someone typing or a photo
  step waits for a two-second pause before it redraws the window, but only
  where such a redraw measured over 300 ms; fast systems keep showing them
  at once.
- A drawer copied from a fast machine no longer opens a stock A1200 with
  inline photos. Turning inline photos or emoji on or off saved that
  choice for good, even when it only matched what the machine picks by
  itself, and the choice travelled with the drawer. Such a choice is now
  saved as "auto", so each machine applies its own default; a choice
  against the default is still kept and travels as before. A file written
  by an older version keeps its choice until it is changed once.
- A download that cannot fit is refused before its first byte, with the
  sizes: "Need 4097 KB, only 2130 KB free in RAM:". A 4 MB file into RAM:
  on a stock A1200 with the window open used to stop at 2.3 MB, minutes
  in, and the part it had was removed. On the RAM disk the room is the
  free memory, since an older ram-handler always reports itself full; a
  volume that reports no room at all is left to try, as it may simply not
  know. When a write fails on a full volume, the window and the text
  client now say "RAM: is full, stopped at 2240 KB" instead of "Could not
  write to RAM:".
- The window opens at once, on the cached chat list, with "Connecting to
  Telegram..." in the status bar, and connects from there. It used to
  connect first and open only afterwards: on a stock A1200 with a stalled
  link that meant two and a half minutes of nothing, which from Workbench
  looked like a program that does not start. When the connection fails,
  the status bar says why, for example "Offline: cannot reach Telegram";
  the reason used to go to a console that a Workbench launch does not
  have.
- Chats hidden from the list stay hidden when the window cannot connect.
  The offline list read the chat cache directly and showed them again.
- Avatars no longer hold the window back on a slow machine. On a 68k below
  the 68040 the first paint shows initials and the avatars are built
  afterwards, one per idle moment, four at a time on screen; elsewhere a
  paint builds them for up to 300 ms and leaves the rest for later. Each
  processed 32x32 avatar is kept next to its JPEG (avatars/*.rgb), so the
  next start skips the decode that cost about four seconds an avatar on a
  stock A1200 and kept its window away for a minute.
- The context menu follows the pointer on a stock A1200. Opening it and
  moving to another item redrew the whole window, about three seconds each
  time, so the highlight trailed far behind and the right item was hard to
  pick. Both now redraw only the menu's own box.
- Typing and menus no longer stall while avatars arrive on a slow machine.
  An avatar is built only after two seconds without input and with no menu
  or popup open, and then only the chat list is redrawn; one full redraw
  follows when all of them are done. On a 68k below the 68040 each avatar
  is built at 20x20, the size the chat list shows, with the JPEG decoded
  at 1/8 (block averages, no IDCT) and 400 colour lookups instead of 1024.
  The nearest-colour search behind avatars, photos and emoji now runs in
  the part of the program the 68k builds compile at -O2, with a table of
  squares instead of multiplications, and picks exactly the same pens. On
  a stock A1200 an avatar took 4.5 s: 2.5 s to decode, 1.6 s for colours.
- Emoji keep their colours on a paletted screen such as a 32- or 64-colour
  AGA Workbench, where the yellow faces came out brown. The emoji artwork
  uses 228 colours, mostly anti-aliased shades, and they were requested
  one by one in palette order until the screen ran out of pens, so the main
  yellow got whatever brown was left. Now the shades are grouped by tint
  and weighed by the pixels they cover; the heaviest groups claim pens of
  their own before the avatars do; and every colour then takes the screen
  pen nearest in tint, read from the real palette, or a checkerboard of two
  pens where that mix comes much closer. With --gui-live-debug the log
  says how many pens the screen had free and what the main colours became.
  Truecolour screens are unchanged.
- Changing the Workbench screen mode or colours no longer stalls while the
  window is open. Intuition must close the Workbench screen for that, and
  could not while our window stayed on it, so the prefs program kept
  asking to close all windows. The window now closes when the system warns
  that the Workbench is about to close, and opens again on the new screen
  when it is back; the chats and the connection stay as they were. When
  another window still holds the reset back (a Shell, for one), the window
  stays closed while the prefs program names it, and opens again once the
  reset has gone through or been cancelled: coming back at once only made
  the prefs program try again and again. The window also closes as soon as
  the warning arrives, without first running a click still queued behind
  it: on a stalled link such a click kept the reset waiting for minutes.
  OS4 and AROS warn through Intuition, MorphOS through its
  screennotify.library. On AmigaOS 3 the warning needs screennotify.library
  from Aminet (util/libs/ScreenNotify10) in LIBS:; without it nothing
  changes, and the own screen (Settings > Use own screen) avoids the problem.
- A window dragged down to its smallest size is remembered too. The size
  is saved without the borders, and the floor for a saved size was the
  window's own 320x200 minimum, which the inside of the smallest window
  never reaches.
- A link that has gone dead no longer holds the window for minutes. On
  AmigaOS 3 and 4 opening a connection had no time limit of its own and
  waited for the TCP stack to give up, over a minute (one poll on a stock
  A1200 took 84 s); it now stops after 15 s, or the longer limit a step
  already sets (MorphOS and AROS had one already). Every network wait runs
  in one-second steps, and once it has lasted two seconds a click on the
  close gadget or a Workbench reset warning ends it, so the window closes
  or makes way at once (a Ctrl+C break signal ends it too). When
  polls lose the connection twice or more in a row, the next one waits
  longer, from 5 s doubling up to a minute, which leaves the window usable
  in between.
- Typing in a busy chat on a stock A1200 no longer stalls behind long
  repaints. Laying out the conversation measured each line again at every
  character added, and every measured piece went through a byte-by-byte
  search for emoji first: with one chat open, a repaint on a cycle-exact
  A1200 took 6 s and now takes 0.9 s, with the same line breaks (checked
  against the old way on generated texts). The renderer is also built -O2
  on the 68020 lane, its self-tests passing on a 68020. While typing, a
  repaint asked for by a new message or a typing notice now waits for a
  pause as long as the last repaint took, up to 10 s, instead of 2 s, so a
  short pause no longer hands the keyboard to it. With --gui-live-debug, a
  repaint of a second or more and a key that took a fifth of a second to
  show each leave a line saying how much text was measured and drawn.

### Added
- "Settings > Show avatars" turns the profile pictures in the chat list and
  the chat header on and off. Off, the coloured initials stay, and nothing
  is downloaded, decoded or matched to the palette for them: on a stock
  A1200 each picture cost over a second, and one kept on another
  datacenter could first cost a minute-long key exchange with it.
  Like the inline photos and the emoji, it starts off on native AGA/ECS/OCS
  screens and on 68k CPUs below a 68040, on elsewhere, and a choice from the
  menu is saved in data/telegram-avatars.txt (one equal to the machine's own
  default is saved as "auto").

### Changed
- The AROS ARM64 package goes to Aminet too, as TelegramAmiga-ARM64.lha in
  comm/tcp. Aminet's list of architectures has no ARM entry, so its readme
  says "other", as other ARM AROS programs there do, and the file name and
  the Requires line say what it is: AROS aarch64 on a Raspberry Pi 4, 400
  or 5. The 0.0.95 package went up on 2026-10-08 as the first one; from
  the next release on it replaces itself like the other five.

## [0.0.95] - 2026-10-07

### Added
- The text client takes its commands from a script or a redirection
  (`TelegramAmiga <commands ...`) as well as from a console, on every lane.
  WaitForChar() only answers for consoles, so on any other input it always
  said "nothing yet" and a scripted chat never read a line; such input now
  counts as ready, since a read there returns data or the end at once. NIL:
  is left as it was on AmigaOS 3.x, AmigaOS 4 and MorphOS, so a detached
  client does not take an empty input for one that has ended. AROS keeps
  its file handles private, so there a client given NIL: for input reads
  the end at once and says "Input closed." instead of waiting for keys that
  cannot come. This is what let the transfer measurements run on a real
  Vampire without anyone at the keyboard.

- A "Full-size photos" setting in the Settings menu, off by default. With
  it on, the photo viewer opens the largest copy of a picture the decoder
  can read, with no byte cap, and draws it up to the size of the screen,
  at most 1024 pixels on the 68k and 2048 elsewhere instead of 512 and
  768. Save photo as... writes the original, the largest copy Telegram
  keeps (2560 pixels for some uploads), even when it is a progressive JPEG
  the viewer cannot show; when the original is the viewer's copy, one
  download serves both. The big copy is decoded at up to twice the view's
  edge and scaled down block by block, so no full-size frame is ever held:
  on a 68k the price is the longer download and a few megabytes while the
  viewer is open, which is why it is a choice. Each kind of copy has a
  cache file of its own, so switching the setting never shows the other
  kind's picture, and the choice is kept in data/telegram-photos.txt as
  full_size=, which older versions skip. Self-tests check the picks, the
  file names, which copy a save takes, the setting's round trip (also
  through a save of another photo setting) and the decoder's new limit,
  and each fails when the code it covers is broken. Checked on a Vampire
  and on MorphOS in QEMU, where Save photo as... wrote the 2560x1920
  original of a test upload.

### Changed
- SHA-256 works on 32-bit words. Every message the client receives is
  hashed whole to check its message key, so a 32 KB download part costs
  one SHA-256 of 32 KB. The rounds now rename their eight working variables
  instead of moving them, rotate with single instructions and need no masks,
  and whole blocks are hashed straight from the input instead of being
  copied through the context first. On a Vampire a 32 KB hash takes 26 ms
  instead of 30, and a download goes from 191 KB/s to 199: a modest step,
  because the compiler had already done well with the old code. A self-test
  checks the FIPS two-block vector, the new transform against the old one
  on random states and blocks, and a digest taken in one call against the
  same data fed one byte at a time; each part fails when its code is
  broken. The benchmark is now `--mtproto-crypto-bench` and times SHA-256
  too.
- Downloads keep several requests in flight. Every getFile used to wait for
  its reply before the next one went out, and that wait was most of each
  part: 125 of the 150 ms a 64 KB part took on a desktop, and the same order
  of wait on every Amiga. A window of requests now goes out ahead (8, or 4 on
  the 68k and 2 on the low-memory 68000 build), and since Telegram answers
  them out of order about half the time, a chunk that comes early is parked
  in a buffer and written when its turn comes. On a desktop a 4 MB file now
  comes in 1.8 s instead of 8.6 with a window of 4, and in 0.85 s with 8.
  On a Vampire, from the text client, it doubles: 97 KB/s before, 191 with
  the 68k's window of 4, and the wait for Telegram fell from 147 ms of each
  32 KB part to 1. On a real MorphOS machine, from the text client, a 4 MB
  file comes in at 1.35 to 1.6 MB/s: a 64 KB part takes about 37 ms, nearly
  all of it reading the stream, where in September 88 ms of a part went on
  waiting for Telegram.
  Two things changed underneath. The wait for a reply accepts any of the
  requests out, and lets through the acknowledgements the server sends on
  their own when several are pending. And the client's message ids no
  longer step back when the reply to an older request arrives: each message
  from the server used to set the last id, so the next message could reuse
  one and Telegram refused it. A self-test checks the parking order, that a
  reset gives every buffer back, and the ids; each check fails when the code
  it covers is broken.
- Uploads keep several parts in flight too, with the same window as
  downloads. Every saveFilePart used to wait for its acknowledgement before
  the next part went out. Telegram may confirm the parts in any order and a
  part can be sent twice at no cost, so nothing is parked here: the client
  only remembers which parts are still unconfirmed. After anything unusual
  it closes the connection, goes back to the lowest of them, and sends the
  next part the old way, alone, before the window opens again; the first
  part of every upload goes that way too, to open the connection. On a
  desktop a 4 MB file goes up at 4.75 MB/s instead of 776 KB/s, and a file
  over 10 MB (saveBigFilePart) at 4.6 MB/s. On a Vampire, from the text
  client, 2 MB go up at 213 KB/s instead of 112. Every test file was
  downloaded back and compared with the original. There the time of a
  32 KB part is now the CPU's: 67 ms of encryption and about as much for
  the TCP/IP stack, which runs on the same processor. On MorphOS, from
  the text client, 4 MB go up at 530 to 590 KB/s, against 210 KB/s with
  one part at a time in September, and the file downloaded back matched
  the original. Socket buffers of 64 and 128 KB, in place of the 32 KB
  Roadshow gives, changed nothing that stood out from the network's own
  swings, and 128 KB on MorphOS (32 KB out and 64 KB in by default) did
  no better, so the stacks keep their sizes.
  A self-test checks acknowledgements taken out of order and a rewind to
  the right place in the file; each check fails when the code it covers is
  broken.
- A first start no longer waits for the key exchange with the datacenter
  that keeps the pictures. A profile picture lives on its owner's
  datacenter, and the first time the client needs one from a datacenter
  other than its own it must agree a key with it: on a 14 MHz 68030 that
  was 74 of the 89 seconds before the window appeared. The exchange no
  longer runs while a chat opens. The window comes up, or the chat just
  chosen shows, then the status line says "Setting up pictures, once: may
  take a minute" and the exchange runs; the picture appears when it is
  through. It still holds the window while it runs, as the login does,
  but only once per datacenter, since the key is kept. Under WinUAE, on
  that 68030, a cold start had its window up after 14 s instead of 89,
  and the exchange then took 41 s with the faster pq split. A self-test
  checks which datacenter is left waiting and that nothing is offered
  without a session, and fails when that check is broken.
- The text client no longer prints a placeholder for an emoji it has no
  emoticon for. Such an emoji is left out together with the space before
  it, as the GUI already did, so "ciao <emoji> mondo" reads "ciao mondo";
  a message of nothing but such emoji shows "(emoji)" rather than an empty
  line. The common emoji keep their emoticons (":)", "<3", "(y)"), letters
  of other alphabets still show as "?" so a word does not vanish, and both
  clients learn a few more symbols: the euro becomes "EUR", a bullet the
  middle dot, "TM", "!!" and "!?" their plain forms, the play and back
  triangles "> " and "<", typographic spaces a space, and the invisible
  parts of keycap digits, subdivision flags and combining accents drop
  out instead of showing as "?". The console's text path is now compiled
  in the host build too, and a self-test runs it on twelve cases; dropping
  the rule that a left-out emoji takes its space fails it.
- Neither window opens when a transfer runs on the chat's own connection,
  which happens when the separate file connection cannot open. Between two
  steps of a transfer the GUI reads that connection for new messages, and
  it would take the replies still on their way.
- The program calls itself Unofficial Telegram Amiga, and says what it is.
  Telegram's API terms let an app's title carry the word Telegram only after
  "Unofficial" (2.3), and ask every client to tell its users that it uses
  the Telegram API and is part of the Telegram ecosystem (2.2). The title
  changes in the window, on the screen, in About, on the login screen and in
  the text client, all from one definition, which is also how the platform
  code finds the console window again. The login screen, About, the manuals,
  the README and the readme of every channel carry the sentence the terms
  ask for, and the channel listings start their description with
  "Unofficial". File and package names do not change: TelegramAmiga,
  TelegramAmiga.lha and the drawers are names, not the title, and renaming
  them would break updates.
- AES works a column at a time. Every byte that crosses the connection
  goes through AES-256 in IGE mode, and the client did it one byte at a
  time: SubBytes, ShiftRows and MixColumns as three passes over the state
  in every round, which cost a Vampire 155 ms to decrypt a single 32 KB
  download part. A round is now sixteen lookups in tables of 32-bit words
  and a few XORs per block, with decryption through the equivalent inverse
  cipher; the tables (8 KB) are built from the S-box when first needed. On
  a Vampire a 32 KB part now takes 34 ms to decrypt instead of 128, and 39
  ms to encrypt instead of 183. A self-test checks the new code against
  the FIPS-197 vector and against the byte form on random keys, IVs and
  lengths in both directions, and fails when either direction is broken.
  `--mtproto-crypto-bench` reports the cost per 32 KB part on the machine
  it runs on, and a build with the self-tests also times the byte form
  for comparison.
- The text client writes a message up to Telegram's own limit, 4096
  characters. Its line stopped at 511 without a word, so a longer text or
  a paste lost its end, and the line it sent was echoed into the
  transcript cut at 500. The line now holds 4096 characters, the
  composer's three rows show the part around the cursor, the echo wraps
  onto as many lines as the message needs, and when the line is full the
  client says once that 4096 is the most one message holds. Recall with
  the arrow keys keeps lines up to 511 characters and leaves longer ones
  out, rather than recall them cut short for Enter to send as if whole.
  The sendMessage buffers are sized for the longer of the two composers,
  two bytes a character once Latin-1 becomes UTF-8. On the host, with the
  Amiga's Latin-1 text path, a 4096-character line of accented letters
  (7888 bytes of UTF-8) went to Saved Messages and Telegram kept it whole.
  A self-test lays out a 4000-character line in the composer and fails
  with the old 640-byte buffer.
- On AmigaOS 3.x the JPEG decoder, the image scaling around it and inflate
  are built at -O2. The 68k lane builds at -O0, since this compiler has
  miscompiled the program at higher levels before, and only code the
  self-tests prove correct goes faster. These three now do: on a stock
  A1200 (68EC020, cycle-exact under WinUAE) an avatar decodes in 0.93 s
  instead of 2.39, a 640x480 photo is scaled into a message in 8.9 s
  instead of 20.9, a bilinear upscale takes 3.1 s instead of 9.3, and
  inflating a 9 KB answer 146 ms instead of 366, every result identical
  byte for byte. The TL reader gained 3% and stays at -O0, being on the
  network path as well, and the plain-68000 build keeps all three at -O0
  until it is measured on a 68000. `--media-bench <drawer>` times this
  work on any machine, on three files scripts/make-media-bench.py makes,
  and prints a checksum of each result. A self-test now inflates a
  stored, a fixed and a dynamic deflate block, and fails when the branch
  of the inflater for any of them is broken; it passes, with the others,
  on the emulated 68020.
- On the 68k a photo reaches a truecolor screen 16 rows per cybergraphics
  call instead of 8, as on the other lines, halving the calls for 12 KB
  more of staging buffer. It was meant for a tail of slow slices under
  AfA_OS; measured there, the tail stayed, and the log showed its real
  causes (the viewer's cache write and the cost of a full repaint under
  AfA), now in the roadmap.

### Fixed
- The drawer icon of the AmigaOS 3.x packages looked like a cloud of stray
  pixels where the Workbench draws the four-colour image an icon carries
  besides its colour one, as AmigaOS 3.0 and 3.1 do; a stock A1200 showed
  it so. That image was made from the shaded drawer of the colour icon by
  error diffusion in the four Workbench pens, which suits the flat program icon
  but turns soft gradients into scattered dots. It is now drawn with a
  black outline, brightness levels and a regular 2x2 texture, and no
  longer keeps the faint dots of the selected state's glow. The colour
  image is Carlo's as before, and the program icons do not change.
- Two-step verification can now finish on a slow 68k. Checking the
  password derives a key with PBKDF2, 100000 rounds of SHA-512: 54 s on a
  Vampire, 27 minutes on a 14 MHz 68030 under WinUAE, and 31 on a stock
  A1200 emulated cycle-exact (68EC020, 8 MB of fast memory), where the
  challenge Telegram hands out with account.getPassword had expired long
  before the end, as had the idle connection. The password could never be
  checked; a field report saw exactly that, with no error at the end. The
  proof is now made in two steps. Everything that depends only on the
  password and the account's salts comes first, with the connection
  closed and the session saved: the derivation, g^a and g^x. Then the
  client connects again, asks for a fresh challenge and finishes with the
  one exponentiation that needs it, about a second on a Vampire and half a
  minute on that 68030, before it sends auth.checkPassword. If the salts
  changed in between, the password was changed elsewhere, and the client
  says so. The text client's warning no longer tells slow machines to
  turn Two-Step Verification off. A self-test checks the new code against
  the values the single-step code computed, that a proof prepared with one
  challenge and finished with another is the one the second alone gives,
  and that a changed salt is caught; each check fails when the code it
  covers is broken, and the test passes on the host and on a Vampire. A
  real login with Two-Step Verification then went through on a stock
  A1200, from the text client, in 35 minutes. The manuals no longer send
  such accounts to a faster machine, or tell them to turn Two-Step
  Verification off.
- A key exchange could fail on a slow 68k before it had really begun. It
  opens with pq, a product of two primes below 2^32 that the client must
  split before it can answer, and the client split it with 64-bit
  arithmetic made of shifts and additions and a division bit by bit at
  every step, in a file the 68k builds without optimisation. On a 14 MHz
  68030 that took minutes, and Telegram closed the connection first: under
  WinUAE the exchange a cold start makes with the datacenter of the
  pictures failed that way after 138 s, where the same start on the 23rd
  of September had got through in 74 s with kinder numbers. The split now
  runs on 32-bit words in Montgomery form, four 32x32 products and no
  division per multiplication, with the processor's own 64-bit multiply
  where it has one (68020, 030, 040 and the 68080), in a file built with
  -O2. It takes 6.5 s on average on that 68030, and on a Vampire 0.29 s
  instead of 7.1. The steps and the factor found are exactly those of the
  old code, which stays in builds with self-tests as the reference: the
  self-test compares the two on eight numbers of Telegram's size with
  three constants each, on a 68k a second time through the 16-bit products
  the 68060 and the 68000 use, and fails when either path is broken. A
  walk that could go on for ever after a wrong product now stops after one
  batch. The login's own key exchange runs the same code.
- When an upload gave up on a part, the reason it reported ("part N of M"
  and what went wrong) could run one byte past its 64-byte buffer, with a
  file of a thousand parts or more and a long enough reason. It is now cut
  to fit.
- A photo or file sent with a long caption no longer fails once it has
  gone up. The sendMedia that attaches the uploaded parts was built in 512
  bytes, which left a caption from about 140 to 420 bytes, depending on the
  file name; with a longer one, from the GUI's send dialog or from /photo
  in the text client, it failed to build after every part had been sent,
  and the transfer ended as failed. It now has room for the longest
  caption and file name. The caption itself held 1024 bytes, which
  Telegram's limit of 1024 characters fills only in plain ASCII; it now
  holds 1024 characters as UTF-8. The text client, whose line now reaches
  4096 characters, says before uploading when a caption is longer than
  Telegram takes. A self-test builds the three kinds of sendMedia with the
  longest caption and file name and, where the text is Latin-1, converts
  1024 accented characters into the caption; each part fails with the old
  size.
- A long message that arrives is no longer cut inside a letter, and a cut
  one says so. Its text comes in UTF-8 and was kept in 4096 bytes, which
  hold Telegram's 4096 characters only in plain ASCII: accented letters
  and emoji take two bytes or more, so a long Italian message could lose
  its last words. The cut fell wherever the 4096th byte was, often in the
  middle of a letter, which then showed as a stray A with a tilde at the
  end. The text now has 8 KB on every lane, enough for 4096 characters of
  two bytes: 528 KB more memory on the PowerPC and AROS lanes, 272 KB on
  the 68k, and the low-memory 68000 build keeps its 2 KB. Every string the
  client reads, names and the previews of pushed messages included, is
  now cut between characters, and a message that does not fit ends with
  " [...]", its bold, italic and code kept inside the part shown. On the
  host a 4096-character message of accented letters (7888 bytes) came
  back whole with 8 KB, and with 4 KB as its first 2112 characters and
  " [...]". Self-tests cut a string, a long styled message, a styled text
  that overflows and a pushed preview; each fails when the code it covers
  is taken out.
- A text of several lines pasted into the text client no longer goes out
  as one message a line. Every line break of the paste reached the client
  as the Return key. A break with more of the text already waiting behind
  it now stays in the message as a line break, shown in the composer as a
  pilcrow, and the transcript echoes each line on its own; Return itself
  still sends, and so does the break that ends a paste, with nothing
  behind it. A CR LF pair counts as one break. This holds for the message
  line of an interactive console only: the lines of a script stay lines,
  and so do the short prompts. On the host, three pasted lines went to
  Saved Messages as one message with its two line breaks; there the raw
  console now leaves Return as a CR, the way an Amiga console sends it, so
  the same path runs. A self-test checks that a break takes one cell in
  the composer and fails when it does not.
- On AROS the Shell that started the client no longer prints its colour
  codes as text afterwards ("[42m[31m9." instead of a coloured prompt).
  The client turns its output buffering off at start, and on AROS the C
  library does that on the Shell's own console handle, so the change
  outlived the program: the Shell then wrote its prompt a character at a
  time, and the console dropped each lone ESC and printed the rest. On
  the way out the client now gives the handle back the line buffering
  dos.library opens a console with. Seen on the i386 VM after the window
  closed, and gone with the fix there and on the ARM VM; the same Shell
  came back to colour.
- Photos keep their colours after the window comes back from an iconify
  or from a switch to its own screen and back. Each time the window
  closes it gives back cybergraphics.library, and a flag meant to try
  opening it once per window stayed set, so the window that opened next
  never tried again and drew every photo through palette pens, on a
  truecolor screen too. A debug log on MorphOS showed it: the first
  window replayed photos in RGB, the window after the switch used pens
  on the same 32-bit screen. The flag now goes back with the library,
  and on a real MorphOS machine the photos kept their colours through
  both. Present since true-colour photos came to AmigaOS 3.x RTG in
  0.0.9.
- Save photo as... works before anything has been downloaded. Its
  requester opens in the download drawer, and only a file download made
  that drawer, so on a fresh install the first save failed with "Could not
  save that photo". The client now makes the drawer, with its icon, before
  the requester opens, as a download does. Found under MorphOS in QEMU
  while saving the original of a 2560x1920 test photo; with the drawer in
  place the saved file was the 2560x1920 JPEG Telegram keeps. Present
  since Save photo as... came in August.

## [0.0.94] - 2026-09-25

### Added
- A sixth package: AROS on ARM64 (aarch64, ABIv1), for the Raspberry Pi 4,
  400 and 5 running the native AROS image. It is built from the same commit
  as the other five, on the same bench as the x86_64 lane, and it ships on
  GitHub and on The AROS Archives (Aminet has no ARM64 entry to give it).
  The lane started as a community port; this release is the first the
  client's own build carries, after a full round on a Raspberry Pi 400:
  login, chats, history, photos, documents and a download, with the chat
  list, the seed and the window geometry surviving the card's FAT handler.
- Our icon, optimised by Carlo Spadoni, one set per platform, plus an icon
  for the drawer itself, which had none: a standard drawer with our design
  on it. AmigaOS 4, MorphOS and AROS get his files as he made them, in the
  format each system reads natively (an ARGB colour icon, PNG icons with
  the launcher fields written into them). AmigaOS 3.x cannot read PNG
  icons, so its set becomes AmigaOS 3.5 colour icons of 256 colours: the
  drawer is his 3.x drawer, the program his 64 pixel version shrunk to the
  size of the 3.x set, which keeps its rim clean
  on a real Workbench. The antialiased rim is blended over the Workbench
  grey and the drawing is also carried with its alpha, which the
  icon.library AmiKit ships draws over any backdrop; a planar image in the
  four Workbench pens covers 3.1. Every program icon keeps what the self-launch
  needs: a project icon whose default tool is the binary, with a 1 MB stack
  (384 KB on the 68000 build). The credit is in the About box, the readme
  and the manuals, and the artwork as delivered is kept in the repository.

### Fixed
- A download no longer makes the window flash. Every new percentage used to
  repaint the whole window, and the full paint draws the inline photos
  straight onto it after copying the rest, so on a Vampire the photos in
  view vanished and came back several times a second and the window was hard
  to use while a file came in. The progress now repaints the status bar
  alone, copied from the off-screen buffer as one strip; with a menu open it
  still falls back to the full paint. It also gives a Vampire back the 260
  ms each 32 KB part spent repainting.
- AROS ARM executables load directly after `make`, without a manual relink.
  GCC 6.5 defaults to COMMON symbols for uninitialized globals, which the
  AROS relocatable ELF loader rejects. The ARM build now enforces
  `-fno-common`, and the shared emoji geometry value is explicitly initialized.
- The GUI self-tests run on AROS ARM, and the live window there starts with
  the stack it was promised. On a 64-bit build the model the window paints
  from is over half a megabyte, and the client kept a copy of it in the
  frame of the routine that runs everything, under the live window and under
  every self-test alike: with the two GUI self-tests, which held copies of
  their own, that overran the 1 MB AmigaOS stack by a hundred kilobytes, and
  each test went silent after its bootstrap lines, its first line of output
  landing in whatever memory sat below the stack. Reproduced in the AROS ARM
  machine under QEMU, with a probe that reports the task's stack bounds (a
  full megabyte, so the frames were at fault, not the Shell's Stack command).
  The four users of that model now share one object off the stack, and every
  self-test block borrows one scratch copy per file instead of keeping its
  own, so the frames fall from 584 KB to 25 KB in the runner and to under
  10 KB in the tests, and the 68k package pays nothing new for it. Both GUI
  self-tests now pass on AROS ARM with the same output as on the host.
- Chats open again on AROS ARM, and on any FAT volume under AROS. The chat
  list is saved back whenever an unread count changes, and that save
  rewrote the file in place: on the AROS FAT handler (its issue 161, the
  same bug that ate the saved login in 0.0.93) a file rewritten in place
  reads back empty, so from the first save on every chat you opened was "not
  found" in a list you were looking at. It reached us from a Raspberry Pi
  400 and was reproduced in an AROS ARM machine under QEMU with four small
  probes: the C library reads and writes correctly, appends and renames are
  fine, only the in-place rewrite is broken. Every file the client replaces
  now goes through one door that deletes and recreates it: the chat list,
  the random seed (a Raspberry Pi 400 card came back with it at zero bytes),
  the avatar store, the window geometry, the recent emoji, downloads to a
  name that exists, and every temporary the client writes before a rename.
  The chat-list line is also read by one hand-written reader instead of
  five scanf formats, so whatever the sidebar lists, the lookup finds.
- A link Telegram already knew gets its preview on the message just sent, in
  Saved Messages, groups and channels too. When the page was in Telegram's
  cache the preview came back inside the answer to the send itself, and no
  updateWebPage followed; the short answer of a private chat was read, but
  the full Updates the other chats return kept only a pending id, so the
  preview waited for the next reload of the history. Seen on AROS x86_64
  during the 0.0.93 round. The TUI prints the lines under its sent marker.
- A saved login survives a restart on a FAT volume. Rewriting a file that
  already exists there reported success from every call and still left zero
  bytes behind, so the client wrote its session, said nothing, and asked for
  the phone number again on the next run. The fault is in the system, not in
  this client: AROS carries it as an open bug of its FAT handler, with a
  reproducer that involves no Telegram at all. Until that is fixed, files are
  deleted before they are created, which commits reliably, and a write is
  flushed while the file is still open so a failure is reported where it
  happens. It reached us from a Raspberry Pi, where bohunamiga tracked it down
  and proposed the workaround; the same code runs everywhere, so a drawer kept
  on a FAT stick or card is covered on every platform.
- Letters outside Latin-1 no longer disappear. Text is one byte per character
  because that is what an Amiga font draws, and a codepoint with no Latin-1
  shape rendered as nothing, so a Polish name arrived with holes where its own
  letters were, reported from the field on AROS ARM. Latin Extended-A now
  folds to the base letter: a reader gets "Czesc" instead of "Cze", the two
  ligatures widen to two letters, and the four Romanian letters with a comma
  below come along. Accents that Latin-1 already has are untouched, and the
  wire is unchanged: what goes out has always been UTF-8. The fold is a
  fallback and not a translation. Drawing the real letters needs a second
  codepage on the systems whose font has them, which is in the roadmap.

### Changed
- The word in front of the version comes from one place. Everything that
  prints "alpha 0.0.94" reads it from the version header, the packaging and
  the release check included: three components means an alpha, two mean the
  beta numbering. The beta starts at 0.1 and not at 0.1.0,
  because an AmigaOS version cookie is two integers and every release so far
  has read as plain "0.0" to the system. This is so that the day the number
  loses a dot, nothing is left saying alpha by accident.
- The debug log says which font the window drew with, next to its size. The
  window takes whatever face the screen hands it, and which face that is
  decides what a name written in Polish or Czech can look like at all, so it
  belongs in the line rather than in a guess from a screenshot.
- The About box credits the contributor who took the build to AROS on ARM,
  next to the one already there.

## [0.0.93] - 2026-09-12

### Added
- When the code Telegram delivers inside its app does not show up, the
  login screen offers the other route Telegram proposes for it, usually
  an SMS: press S once the wait it asks for has passed. The text client
  takes S at its code prompt too. The route and the wait were already in
  Telegram's answer and were being thrown away; the debug log now records
  both next to the delivery type.
- A paperclip at the left of the composer opens the attachment requester.
  JPEG and PNG attachments offer Photo, File or Cancel, with the existing
  caption dialog; other attachments follow the file upload path. It remains
  available when emoji and inline photos are disabled.
- "Settings > Enable emoji" switches the picker, composer button and graphical
  emoji on or off. With it off, messages keep their text emoticons and the
  composer keeps any emoji already entered. The choice is saved separately
  from photos. Like inline photos, emoji start off on native AGA/ECS/OCS
  screens or a 68k CPU below 68040; both need a 68040 or faster and an
  RTG screen to start on. Explicit on/off choices override the defaults.
- A link preview that Telegram finishes later appears in the open chat by
  itself, including on a link just sent from this client. The title and
  description update the existing message; its picture uses the same Inline
  photos setting and bounded pipeline. The text client's cached transcript
  gains the preview too.
- Emoji can be sent. "Insert emoji..." in the Telegram menu (Amiga+E) opens a
  panel above the composer: the recently used ones first, then the 109 emoji
  this client already reads back as text emoticons, in a grid walked with the
  arrow keys or clicked. ENTER inserts one at the caret and keeps the panel
  open, ESC closes it, and the recent row survives between runs. Inside the
  composer an emoji is drawn as a picture at least 16 pixels high, is
  edited and stepped over as one character, and goes out as its real Unicode
  codepoint. The pictures are Noto Emoji glyphs reduced to 16 pixels, shipped
  under the SIL Open Font License; see third_party/noto-emoji. A smiley
  button between the input and Send opens the same panel with the mouse,
  centred vertically on the visible input box even with a small font.
- Received emoji show as pictures too, the same ones the picker offers. Small
  OS3 fonts such as Topaz 8 keep their size while the layout reserves a cell
  of at least 16 pixels for a readable emoji, with matching row spacing and
  caret positioning. Turning emoji off restores compact text rows and text
  emoticons. Clipboard text keeps the emoticon representation.
- A PNG goes out as a photo. The photo gate now reads the file's own bytes
  rather than its name: a JPEG is walked through its first scan as before, a
  PNG through its chunks to IEND, so a truncated file of either kind is
  refused before a single part is uploaded. Telegram's photo limits, width
  plus height at most 10000 and at most 20 to 1, are checked from the header
  for the same reason, and the server's own refusals now come back in words
  instead of an RPC name. The menu requester, the drop target and the text
  client's /photo all take .png alongside .jpg.
- A build file for AROS aarch64 (ARM64), aimed at the cross-toolchain
  used for AROS on the Raspberry Pi. The client needs no external SSL
  library, so the port is only a matter of the build recipe. That lane is
  not part of a release and is not validated yet: it builds and the login
  goes through, while the session file it leaves behind is still being
  looked at. Contributed by bohunamiga (PR #16).

### Fixed
- A new login gets its code again. Telegram had stopped delivering the
  in-app login code to this client: it accepted the request, said the code
  was on its way inside the app, and nothing ever reached the phone, on more
  than one account and with more than one api key. The connection now
  declares its system language as a full locale, en-US, instead of a bare
  en, the change other third-party clients found to bring the codes back;
  on a number where two requests in half an hour had produced nothing, the
  first one after it arrived. The same setting had worked since the first
  release and nobody had reported a problem with it: what changed was how
  Telegram treats it. Logins that were already saved were never affected.
- The login screen no longer promises a code Telegram is not sending. One
  delivery answer means "add and verify a login email first", and it used to
  read like any other, so a first user waited for a message that could never
  arrive. That case now says what the account needs, and a delivery type this
  build does not recognise says so instead of pretending, with its number in
  the console and in the debug log. The log also records the delivery route
  and the digit count next to "send_code done", so a login that receives
  nothing can be diagnosed from the log alone. The delivery line on the
  code screen no longer cuts "phone" short.
- Clicking URL text keeps the pointer aligned with literal characters such
  as underscores and tildes, including below a link preview. Wrapped URLs
  keep their complete text and link styling across lines. Clicking the
  preview picture still opens the image viewer.
- Opening a photo's context menu or the Send photo dialog no longer freezes
  AfAOS while drawing its labels. Popup areas are copied from the completed
  buffer after photo replay, so menus, mentions and emoji stay above pictures
  during full repaints, refreshes and caret updates. The photo dialog also
  uses the compatible bitmap text and matching caption/button metrics.
- Toggling emoji no longer resizes avatars in the chat list or header.
  The search field, chat rows and open-chat header keep their compact native
  font dimensions too, with centred text and avatars, including initials.
  Names and previews still fit graphical emoji; clicks, scrolling and row
  reordering follow the same compact layout.
- Composer text, caret and selection use the font's real baseline, correcting
  the low text position with taller MorphOS fonts. The Send label stays
  centred when the composer wraps onto several lines.
- "Save photo as..." suggests .png for PNG bytes and .jpg for JPEG bytes,
  regardless of the internal cache name. An uncached image is fetched before
  the requester opens, then copied without conversion. Photos re-encoded by
  Telegram keep the received format; sending as File preserves the original.
- The automatic inline-photo default checks the screen's actual bitmap.
  Having cybergraphics.library installed no longer makes an AGA/ECS/OCS
  screen count as RTG. Emoji use the same check, including on classic OS4.

## [0.0.92] - 2026-09-04

### Added
- Links get their preview. A pasted link showed as the bare URL, because the
  preview the server had already built for it was skipped whole; the bubble
  now carries the site and page title on one line and the first line of the
  description under it. When the preview comes with a picture, that picture
  goes through the same bounded photo pipeline as any other and obeys the
  same Inline photos setting. A preview the server is still fetching stays
  silent rather than guessing.
- Videos show a frame. A document carries the same thumbnail vector a photo's
  sizes come from, so the still goes through the bounded inline pipeline
  unchanged: same disk cache, same pacing, same Inline photos setting, and
  nothing new to download twice. The clip keeps its length and size beside
  the frame, because those are not in the frame. Stickers do not get a
  picture: the thumbnail Telegram serves for one is WEBP, which this client
  has no decoder for, so it is not fetched at all and the bubble keeps the
  sticker's emoji. With inline photos switched off the marker now says what
  the thing is rather than "[Photo]", and it still opens the viewer when
  clicked.
- The transcript says what an attachment is instead of what it is called. A
  sticker shows the emoji it stands for, a clip its length and its shape, a
  voice note its duration. Music keeps its filename, which is how you know
  which track it is, and a plain file is unchanged.
- The login panel says where Telegram sent the code. With another device
  signed in the code arrives inside Telegram itself, not by SMS, and a first
  user waited for a message that was never coming. The window now says which
  it was, and the digit count when the server gives one. The manuals say it
  too, on the first-start page.

### Fixed
- On MorphOS a photo no longer paints over a popup. The direct photo replay
  writes into the window after the buffer is blitted, which put pictures on
  top of the context menu, the mention list and the emoji panel; the popups
  are now painted once more after the replay, straight onto the window.
- A bubble no longer holds space open for a picture the client has given up
  on. Once every size of an image has been fetched and refused, which is what
  an undecodable format looks like from here, the message falls back to its
  text instead of showing an empty frame that no later paint will ever fill.
- A downloaded Amiga program comes out runnable. The executable bit is set
  from the file's own magic bytes rather than its name, on the completed
  download only, and the other protection bits survive. Both loadable
  families count: HUNK for classic 68k programs and ELF for AmigaOS 4,
  MorphOS and AROS ones, which the first cut had missed. Reported with the
  polarity warning that saved a round: on Amiga those bits are active low.
- A long message goes out. The send query was built into a 512 byte buffer,
  so anything past roughly 460 characters failed to build and the client
  quietly refused to send it. Both send paths now size that buffer from the
  composer. Pasting more than the composer holds also used to truncate in
  silence while still reporting success; it now says how much landed.
- A symbol with no Latin-1 shape no longer leaves a hole. It renders as
  nothing, and the space that introduced it stayed behind, so a line read
  with a gap in the middle. The space now leaves with the symbol. Modifiers
  are exempt, since they attach to the character before them.

## [0.0.91] - 2026-08-27

### Added
- Sending a photo now opens its own dialog instead of a bare requester: the
  file's name and size, a caption line already holding whatever was in the
  composer, and Photo, File and Cancel as explicit choices. ENTER sends,
  ESC cancels, and the draft only leaves the composer once the send starts.
  The same dialog serves the menu, the file requester and a Workbench drop.
- Photos can carry a caption. It rides the sendMedia of the photo and of the
  document fallback used above 10 MiB alike, converted from the platform
  charset to UTF-8, so accented text arrives intact. The text client takes
  it as `/photo <path> [caption]`.
- The chat header shows the open chat's avatar beside its name, drawn like
  its sidebar row, with the coloured initials as the fallback.
- About names where the project lives, so a user who wants to report
  something or fetch a newer build has the address in front of them.

### Changed
- Consecutive messages from the same sender no longer repeat the name: a run
  shows it once, the way the desktop client groups a busy conversation, which
  also buys back vertical space on a small screen.
- The round chrome of the desktop client: avatars and the unread badges are
  circles and pills, the jump-to-newest button is a disc, and message bubbles
  have their corners clipped. On screens that afford exact colours the edge
  carries a one-pixel blend ring, which is the anti-aliasing hard-edged
  hardware can do; paletted screens keep their crisp edges.
- Every Amiga lane now ships without the offline self-tests, which CI runs on
  the host binary instead. The binaries lose about 15 per cent: 110 KB on
  AmigaOS 3, 136 on MorphOS, 178 on AmigaOS 4, 128 on AROS. Field diagnostics
  are still compiled in everywhere, and a stripped build says so plainly when
  a self-test flag is used.
- Background photo work can no longer be starved for good by a busy event
  loop on 68k: after a bounded number of deferred turns it proceeds anyway,
  the same valve the other lanes already had.

### Fixed
- Text that sits inside something now sits in the middle of it. The caret
  covers the glyph cell instead of floating above it, the reply strip centres
  its text and clears the composer by a few pixels, the unread counts centre
  in their badge, and the avatar initials centre in their circle. All four
  were the same mistake, a baseline guessed from the line height, and all four
  grew worse the taller the font, which is why MorphOS showed them first.
- A photo whose cache entry vanishes after a successful write is now reported
  as the failure it is, instead of being fetched again on every repaint
  forever. A tired or full volume can do this, and the loop hid it.
- The plain-68000 build pauses briefly between the rounds of the initial
  chat-list download, so a PCMCIA network card gets breathing room instead of
  a continuous burst.

## [0.0.9] - 2026-08-07

### Added
- The full-screen TUI now word-wraps transcript messages on narrow consoles,
  with indented continuation lines and hard breaks for overlong words. Its
  composer grows from one to three screen rows before reverting to a bounded
  tail view, while scrollback continues to move by logical messages.
- Photo messages now offer `Save photo as...` from their context menu whether
  inline display is enabled or not. The fixed-size viewer exposes the same
  action on the `S` key; both use a save requester, prefer the best cached
  original JPEG, fetch the viewer-size JPEG on demand when necessary and ask
  before replacing an existing file.
- `Settings` now includes a persistent photo-cache limit (10, 50 or 200 MiB,
  or Unlimited; default 50 MiB) and a confirmed `Clear photo cache` action.
  The client catalogs `photos/` incrementally during idle time, prunes the
  oldest files without evicting photos currently on screen, and never touches
  the separate avatar cache.
- Photo messages now use Telegram's embedded stripped thumbnail as an instant
  blurred preview while the bounded network image is fetched and refined. The
  tiny preview is cached separately, works in the transcript and viewer, and
  does not start background work when inline photos are disabled.
- The Telegram menu now groups persistent preferences under `Settings`:
  download drawer, inline photos and `Photo dithering` with Full, Light and
  Off levels. Each change is written immediately and restored at next start.
- Clicking a photo, including the `[Photo]` label while inline photos are
  disabled, now opens one reusable fixed-size viewer window. It requests a
  larger bounded Telegram image, keeps a separate `-l.jpg` disk cache and
  reveals the JPEG progressively without evicting transcript photo slots.
- The GUI now has a persistent `Settings > Inline photos` toggle. It defaults
  to on except on AmigaOS 3 systems without RTG or with a CPU below a 68040;
  every explicit choice wins over the hardware default. Disabling it restores
  lightweight `[Photo]` bubbles without background photo fetch or decode work.
- A message can now be forwarded to Saved Messages from its GUI context menu.
  The TUI provides `/forward` for the latest message and `/forward <id>` for an
  explicit Telegram message ID. Forwarding uses the layer-214
  `messages.forwardMessages` method and reports Telegram RPC failures by name.
- The GUI's `Forward to...` action now reuses the local-first chat search as a
  destination picker, including browse and online results. The TUI provides
  `/forwardto <chat-number> [message-id]` for the same peer-to-peer operation.
- Photo messages now render inline in GUI bubbles. The client selects a bounded
  Telegram thumbnail for each platform, downloads it incrementally through the
  existing multi-DC file channel, and reuses the on-disk `photos/` cache on
  later paints and runs. Text-only and failed-download fallbacks remain usable.
- JPEG files can now be sent as Telegram photos from the GUI, Workbench drop or
  the TUI `/photo` command. The existing non-blocking upload engine is reused;
  photos above 10 MiB are sent as documents with explicit status feedback.

### Changed
- AmigaOS 3 chooses the first-run `Inline photos` default from the active
  screen and CPU: it starts disabled without RTG or below a 68040. This
  automatic value is never written to disk, so hardware upgrades are detected;
  an explicit user toggle remains persistent in either direction.
- AmigaOS 3.x now discovers `cybergraphics.library` at runtime and sends
  inline-photo and viewer RGB888 rows directly to compatible true-colour RTG
  screens. AGA and systems without a validated CyberGraphX target keep the
  existing zero-dependency pen-grid renderer.
- Photo decode and canonical-cache reads now size their idle slices from
  measured execution time instead of a fixed CPU-family assumption. Slow 68k
  machines retain the conservative floor, while fast 68k accelerators ramp up
  toward a roughly 120 ms work budget and use a short wake cadence until the
  visible photo queue is drained.
- JPEG decode, canonical-cache reads and photo replay now keep independent
  measured budgets. Slow palette mapping can no longer throttle entropy decode
  on accelerated 68k systems, and diagnostics identify the cost centre for
  every pacing adjustment.
- Non-68k photo scheduling now starts bounded background work without waiting
  behind a continuous pointer-event stream, advances larger JPEG and palette
  slices, and loads normal canonical RGB frames in about two chunks. The
  conservative m68k pacing remains unchanged.
- Final canonical photo frames are now cached atomically as versioned RGB888
  files beside their JPEGs. Reopening a viewed chat can load the exact pixels
  in bounded idle chunks without decoding JPEG again; corrupt or stale cache
  entries are discarded and rebuilt automatically.
- Non-68k targets now select an approximately 800-pixel inline Telegram source
  within a 1 MiB cap. Final-pass upscales use bilinear filtering, while coarse
  preview passes and ordinary downscales retain the bounded fast path.
- MorphOS RTG screens now keep photos in RGB888 and replay them directly to the
  CyberGraphX window after the off-screen frame blit when its friend bitmap is
  not a CGX target. The runtime-checked pen-grid fallback remains available for
  paletted screens and incompatible drivers.
- Live resize now paints only the window background while intermediate sizes
  are arriving. On AfA_OS it also clears the current client area at the first
  size event, so the system's opaque resize stretches only blank background;
  the complete frame is rebuilt once after release. Crash-safe diagnostics mark
  resize begin, rebuild, repaint and end without changing the final layout.
- Inline photos now decode once into a platform-sized canonical cache and
  repaint from that cache at every bubble size. Resize paints never trigger a
  JPEG decode, modern RTG targets use optional RGB888 output, paletted screens
  use ordered dithering, and larger bounded thumbnails improve detail without
  making repaints depend on image size.
- Inline JPEG decoding now advances in bounded idle slices outside the paint
  path using browser-style quality passes: a complete coarse 1/8 image appears
  first, then 1/4 and final detail replace it atomically. Input, scrolling and
  resize events keep priority, and incomplete bands never enter a paint.
- The AfA_OS compatibility renderer now composes complete bitmap-font runs in
  memory and submits one `BltTemplate` per run instead of one per glyph. Native
  text rendering on systems without AfA_OS is unchanged.
- Inline-photo decoding now follows the visible viewport: the topmost visible
  photo is advanced first, off-screen partial decoders wait, and idle periods
  use larger bounded slices without taking priority over queued GUI events.
- The canonical photo cache now keeps four slots on 68k and six on wider
  targets. True LRU eviction skips active and currently visible photos, so a
  third visible image no longer makes an earlier one disappear.
- Hidden chats now remain in the local peer cache. They stay out of the normal
  sidebar, appear immediately in local search with a `(hidden)` marker, and
  return to the sidebar when opened, without an online search or cache reload.

### Fixed
- Temporary quiet-log files no longer litter the program drawer: they live
  in T: and are cleaned up at startup and exit.
- TUI composer threshold crossings now repaint only the separator and the
  one-to-three composer rows, restoring just the transcript rows that become
  visible again. Direct character echo and rubout remain active on wrapped
  composer rows, so narrow 68000 consoles no longer flash or pause per key.
- Local five-lane test packaging now creates AROS media only with Rock Ridge
  plus Joliet, verifies the executable inside every archive and ISO, rejects
  reused volume labels and keeps the macOS hybrid path limited to AmigaOS 4.
- Progressive transcript photos and the photo viewer now share one
  owner-checked decode pipeline. Back-to-back fetch completions remain queued
  until the current image commits, preventing one photo from being repeated or
  split across another message bubble on fast targets.
- Shell launches now carry the same 1 MiB minimum-stack contract as Workbench
  icons. AROS also swaps to a private safe stack when a launcher supplies less;
  OS3 and OS4 reject unsafe bounds instead of entering the stack-heavy GUI.
- Photo source selection now prefers baseline JPEG sizes that the bundled
  decoder supports. If a downloaded size is rejected, the client retries a
  smaller untried baseline size instead of rejecting the whole photo for the
  rest of the session; transcript and viewer use the same bounded fallback.
- Final photo-quality upscales now derive fixed-point coordinates without a
  32-bit overflow, preventing large images from repeating rows or tiles when
  the detailed frame replaces a stripped or coarse preview.
- Every visible stripped photo preview is now prepared before serialized
  network and quality work begins, so later photos and the on-demand viewer no
  longer remain grey while an earlier image is being refined.
- Background photo work can no longer wait forever behind continuous window
  events or an inactive window. The heartbeat now advances it, visible failed
  fetches are re-queued, and a clicked viewer photo has queue priority.
- The JPEG drop requester now offers Photo, File and Cancel as distinct
  actions; its Cancel button and Escape key leave the file untouched.
- Empty inline-photo preference files now fall back cleanly to the default
  setting without relying on an unchecked read result.
- Documents with a caption now keep the caption and append the downloadable
  file label on a new line instead of hiding the attachment name.
- Inline-photo cache downloads no longer become permanently suppressed after a
  transient network, datacenter or filesystem failure. Opt-in live diagnostics
  now identify each fetch and render stage without logging chat content.
- Inline photos on MorphOS now use the proven pen-grid renderer instead of an
  RGB888 path that could leave decoded photos grey. Other RTG targets validate
  the destination bitmap with a write/read self-check and fall back for the
  whole session when the driver cannot replay RGB pixels reliably.
- Photo fetch, decode and partial replay now remain suspended for the complete
  resize cycle. A stable placeholder frame is built first and cached images are
  restored on the next idle paint, avoiding buffer access during reallocation.
- `--gui-live-debug` now records a bounded set of AfA_OS full-paint metrics:
  render/blit clock ticks, primitive count, batched and fallback text blits,
  and RGB-row or pen-run photo replay work. Normal GUI runs remain unchanged.

## [0.0.8] - 2026-07-31

### Added
- Multi-DC downloads: a document stored on another Telegram datacenter now
  downloads from there (per-DC auth key with a one-time handshake, cached in
  `data/telegram-auth-dc<N>.bin`; `FILE_MIGRATE` mid-transfer hops too).
- Local-first search: typing in the sidebar box filters YOUR chats instantly
  from the local cache; the final "Search Telegram..." row (or ENTER with no
  local match) runs the online search. The online search itself has two
  stages: it first looks through YOUR OWN dialogs on the server (which finds
  hidden chats and private groups that have no public username -- the way
  back after removing a chat from the list), and only when nothing matches
  does it ask the global Telegram search. With an EMPTY search box, the top
  row becomes "Browse all chats...": ENTER lists every dialog of the account
  from the server, hidden chats included -- the way back when the exact name
  escapes you.
- Arrow-key navigation: up/down act on the panel under the pointer, like the
  wheel: over the sidebar they walk the chat list (ENTER opens), over the
  transcript they scroll the messages. In the search box they walk the
  result list.
- Experimental plain-68000 build option (`M68K_CPU=68000`), not part of the
  released packages yet.
- Clickable links: a http(s):// or www. URL inside a message is drawn blue
  and underlined, and clicking it opens the system browser via the
  OpenURL/URLOpen command; without one the URL is copied to the clipboard
  instead.
- Foreign-DC avatars: a profile photo stored on another datacenter now
  downloads through the same multi-DC file channel instead of staying a
  blurred thumbnail forever.
- Drag-and-drop upload: drop a file icon from the Workbench onto the chat
  window and it uploads to the open chat (the status names the file as soon
  as the drop lands), with the same non-blocking pump, progress and cancel
  as Send file...
- Reload chat list menu item: re-page the dialog list from the server on
  demand, on every platform including MorphOS. Start-up no longer refetches
  the list on every run (a busy account felt heavy); the full fetch happens
  on the first login only.
- Hidden chats memory: a chat removed from the list now STAYS removed across
  reloads and restarts; reopening it from the online search makes it visible
  again.
- Archived chats are filtered out of the dialog list (main folder only);
  archive management is on the roadmap.
- Configurable download drawer, picked from the menu ("Download drawer..."
  in the Telegram menu): a standard drawer
  requester, remembered for the next run. Downloading to a RAM: drawer is
  much quicker on a floppy or a slow disk. Defaults to `downloads` as
  before, and the file it writes (`data/telegram-downloads.txt`) can still
  be edited by hand.

### Changed
- AmigaOS 4: message ports and IO requests are now allocated through the
  OS4-native AllocSysObject/FreeSysObject family instead of the classic
  CreateMsgPort/CreateIORequest calls (community contribution, PR #10).
- The transfer status line shows the rate next to the percentage
  (e.g. "Downloading 42% 38 KB/s"), averaged over a rolling window.
- Downloads pipeline their chunks: the request for the next chunk goes out
  while the current one is still arriving, so its round trip stops costing
  wall-clock. Worth the most on slow or distant routes. Any hiccup drops the
  pipeline and the proven synchronous retry takes the chunk.
- File transfers no longer freeze the window: one chunk moves per event-loop
  turn, so you can keep chatting, switch chats and receive messages while a
  file uploads or downloads. Close gadget or ESC cancels the transfer (a
  second close quits).
- File transfers run on their own dedicated connection (second MTProto
  session), no longer interleaved with the live chat session.
- Menus follow the system colours: new-look menubar and the context popup now
  drawn with the screen's own pens (dark stays dark on OS4.1, classic grey
  stays grey elsewhere).
- Downloads write through a large buffer, so the drive is touched in big
  blocks instead of many small ones (a tester could hear the difference on
  an 030).
- The transfer status says "ESC cancels" instead of "close or ESC cancels":
  the close gadget still works, but the hint now names the obvious key.
- While a transfer is running the heavy live poll is throttled (the light
  push drain keeps messages flowing), which also speeds the transfer up.

### Fixed
- An adversarial review pass before the release found five defects, now
  fixed: a download into a deep drawer with a long attachment name could
  write past the end of the path buffer (the file name is now shortened,
  extension kept, and the drawer is never touched); an underscore or
  backtick inside a URL was swallowed from the drawn address and left the
  rest of the message in italic; the shortest addresses were underlined
  but not clickable; Cut/Paste in the sidebar search box did not refresh
  the filtered list; and unhiding a chat rewrote the hidden-chats file
  through a 128-entry buffer, resurfacing older hidden chats on accounts
  past that many.
- Repeated GUI resize events could free, rebuild and repaint the double buffer
  for every intermediate size, freezing some AmiKit/RTG systems. Resize events
  are now coalesced before one rebuild, buffer release waits for the blitter,
  and Intuition redraws the final window frame.
- AmiKit setups: the GUI froze the machine inside its very first paint on
  systems running AfA_OS 4.8, whose Text() cannot render into a layerless
  off-screen RastPort (our flicker-free double buffer). When AfA is loaded
  the client now draws bitmap text into the buffer itself via BltTemplate;
  layout and caret placement use those same bitmap-font metrics, and ordinary
  typing copies only the input strip instead of the complete window. Every
  other system keeps the native Text() path. (While hunting this, an
  AmiKit system-killer NOT caused by the client was also isolated: its bundled
  icon.library 51.4.533 can corrupt SysBase under
  Directory Opus; updating that library fixes crashes that happen with or
  without Telegram running.)
- A wall clock stepped BACKWARDS while a query waited (AmiKit syncs time
  right after networking comes up) made the reply budget expire instantly:
  the budget now re-origins instead.
- A stale rpc result left on the stream by an aborted query now surfaces as
  a clean soft-fail and reconnect, instead of ambiguous stream state on
  slow bsdsocket stacks.
- The right-click popup was too narrow for its widest labels: the width is
  now measured from the actual items with the platform's own font (issue
  #11). Same report, same conclusion: "Download drawer..." moved out of the
  popup into the menu bar only -- it is a preference, not a message action.
- Right-clicking OUTSIDE the window while it still had focus could leave you
  with no menu at all: the pointer tracking never checked whether the pointer
  had left the window, so the right-button trap stayed armed and suppressed
  the classic menu bar.
- Double-clicking a search result opened the chat BELOW it: the first click
  already opens the result and replaces the sidebar, so the second landed on
  a different list. The second half of the double click is now ignored.
- Pasting a text file kept its layout here but arrived as one paragraph on
  other clients: line breaks are now preserved (CR and CRLF normalised),
  instead of being flattened into spaces. The sidebar search box still takes
  one line.
- Long pasted text with accented characters reached other clients as
  replacement characters (the UTF-8 conversion buffer had stayed at 1 KB
  while the composer grew).
- Transfer percentage on files over ~41 MB: it wrapped back to 0 mid-file
  and climbed again (a 32-bit overflow in the percentage itself; the
  transfer was always fine).
- Accounts whose first login predates the paged dialog bootstrap were stuck
  with a handful of chats in the sidebar: the Reload chat list menu item
  fetches the full (paged) list on demand.
- Aminet only: the 0.0.7 AmigaOS 3.x archive shipped the wrong (AmigaOS 4)
  binary due to a case-insensitive filename collision in the packaging and
  was republished as 0.0.7a (same program, correct 68k binary). The GitHub
  zips were never affected. The packaging now checks the architecture of the
  binary inside every archive.

## [0.0.7] - 2026-07-24

### Added
- Live transfer percentage in the status bar; downloads and uploads are
  cancellable from the close gadget or ESC.
- Reply on double-click of a message bubble.
- Clipboard support: Copy/Cut/Paste in a proper Edit menu (Amiga+C/X/V),
  with mouse or Shift+arrow text selection in the composer.
- Live updates for messages edited on another device, even while typing.
- TUI: file send/download, including Workbench drag-and-drop.
- `TUI_MODE`/`GUI_MODE` icon tooltype to force the client flavour.

### Changed
- Big-file transfers hardened: lost chunks and parts are retried
  automatically at the same offset, stalled links hit a send timeout instead
  of hanging, wedged sockets reconnect (152 MB tested on PPC lanes).
- Upload limit raised (chunked saveBigFilePart): 250 MiB on PPC/AROS,
  125 MiB on m68k.

### Fixed
- Search with accented names.
- AROS x86_64: crash on relaunch after closing the GUI (shared socket
  library was closed per-connection).
- MorphOS: closing the GUI while the link was busy could freeze the machine
  (connection settle before bsdsocket teardown).

## [0.0.6] - 2026-07-14

### Added
- File sharing: download any received file (right-click, Download) and send
  files to the open chat (right-click, Send file...), up to 10 MB.
- Saved Messages pinned self-chat: Telegram cloud as a transfer drawer
  between the Amiga and your phone/PC.
- Iconify (menu item or OS4 titlebar gadget) parks the client on a
  Workbench AppIcon.
- Click places the text caret in the composer and search box; Del
  forward-deletes.

### Changed
- Script-free launch: two icons start the program directly (TelegramAmiga =
  GUI, TelegramAmiga-TUI = console); the IconX launcher scripts are gone.
- The binary is now called TelegramAmiga (was telegram-test).
- Truer avatar colours, rich on RTG screens.

## [0.0.5] - 2026-07-07

### Added
- Real profile-picture avatars in the chat list (instant blurred previews,
  crisp after opening a chat, cached on disk).
- @username autocomplete in groups (type @ in the composer).
- The window remembers its position and size across restarts.
- Own-screen mode (opt-in via `data/telegram-gui-win.txt`).
- `$VER` version tag in every binary.

### Changed
- Tidy program drawer: auxiliary files in `data/`, avatar photos in
  `avatars/`; old installs migrate automatically.
- Stronger first-login randomness (keyboard and mouse feed the RNG).
- Message line breaks and bullet lists render properly.

### Fixed
- A right-click while the client was busy could freeze the whole system
  (IDCMP_MENUVERIFY removed in favour of a dynamic RMBTRAP).
- More robust chat removal.

## [0.0.4] - 2026-07-02

### Added
- Edit and delete your own messages from the right-click context menu
  (with hover highlighting).
- Multi-device sync: messages sent from another device appear live in the
  open chat.

### Changed
- Live read receipts: the two blue ticks flip in real time.
- Message times follow the Amiga system clock, DST included.
- Clearer 2FA login: no cloud password, just press Enter.

## [0.0.3] - 2026-06-27

### Added
- Reply to a message (tap a bubble or right-click, Reply); the quoted line
  shows above your message.
- Real drawn delivery checkmarks: one tick sent, two blue ticks read.
- Floating scroll-to-newest button.

### Changed
- Flicker-free drawing (off-screen double buffering).

## [0.0.2] - 2026-06-24

### Added
- Scroll-to-top history paging (load older messages on demand).
- Online chat search (find and add chats not in the list).
- Persistent unread badges.
- Drag-and-drop chat reorder and removal (persistent).
- Group "is typing" indicator.
- Full chat list fetched on first login.

### Changed
- Long and multi-line messages supported end to end.
- Flashless Workbench launch (no console window flash).

## [0.0.1] - 2026-06-19

First public alphas: AmigaOS 3.x (m68k), AmigaOS 4.x, MorphOS, AROS i386
and AROS x86_64. Native Intuition GUI (chat list, conversation, live
send/receive, typing indicator, read receipts, in-window login) plus the
text-mode TUI, on a shared from-scratch MTProto core with all cryptography
built in (RSA, Diffie-Hellman, SRP/2FA, AES, SHA). In-place updates during
the 0.0.1 window added full-length messages, accented-character send,
online search, emoji-to-emoticon text, unread clearing and media
placeholders, and cured the OS3 window flicker.
