# Changelog

Telegram Amiga, a from-scratch native MTProto Telegram client for
AmigaOS 3.x, AmigaOS 4.x, MorphOS and AROS (i386/x86_64).
Dates use YYYY-MM-DD. Each release ships on all five platform lanes
unless noted.

## [Unreleased]

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
- A bubble no longer holds space open for a picture the client has given up
  on. Once every size of an image has been fetched and refused, which is what
  an undecodable format looks like from here, the message falls back to its
  text instead of showing an empty frame that no later paint will ever fill.
- A downloaded Amiga program comes out runnable. The executable bit is set
  from the file's own magic longword rather than its name, on the completed
  download only, and the other protection bits survive. Reported with the
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
