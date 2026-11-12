#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the human-facing release packages for Amigram.
# Version comes from include/tg_version.h (override with VERSION=... if needed).
#
# Each package contains ONLY the program (one binary), its self-launching icon
# (plus the Amigram-TUI console icon on the 68k line only; no scripts),
# the PUBLIC Telegram API app credentials and per-architecture IT/EN manuals.
# No user session is ever bundled: telegram-auth.bin and the peer cache are
# created locally by the user on first login (see the manual).

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PACKAGE_ROOT=${PACKAGE_ROOT:-"$ROOT_DIR/build/human-releases"}
# A relative root stopped the 0.0.96 run silently after the first zip: some
# steps run from inside other directories. Anchor it where it was given.
case $PACKAGE_ROOT in
    /*) ;;
    *) PACKAGE_ROOT="$(pwd)/$PACKAGE_ROOT" ;;
esac
DATE_STAMP=${DATE_STAMP:-$(date +%Y%m%d)}
COMMIT_ID=${COMMIT_ID:-$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)}
# Single source of truth: the version the binary itself reports (About box /
# startup banner) comes from include/tg_version.h, so the package always matches.
VERSION=${VERSION:-$(sed -n 's/.*define TG_VERSION "\([^"]*\)".*/\1/p' "$ROOT_DIR/include/tg_version.h" 2>/dev/null)}
VERSION=${VERSION:-0.0.0}
# "alpha" while the number has three components, "beta" from the two integer
# numbering the beta starts with (0.1, then 0.2, then 1.0). Derived rather than
# written down, so the day the number loses a dot nothing keeps saying alpha.
# The channel word comes from the header, the same one the About box and the
# banner print (TG_VERSION_CHANNEL), so the binary and the packaging cannot
# disagree; the dot count is only a fallback for a tree without that macro.
CHANNEL=${CHANNEL:-$(sed -n 's/.*define TG_VERSION_CHANNEL "\([^"]*\)".*/\1/p' "$ROOT_DIR/include/tg_version.h" 2>/dev/null)}
# The update notice reads a public Telegram channel named in the same header.
# While that name is empty the client never looks, its line in the settings
# is greyed, and the manuals say nothing about it.
UPDATE_CHANNEL=$(sed -n 's/.*define TG_UPDATE_CHANNEL "\([^"]*\)".*/\1/p' "$ROOT_DIR/include/tg_version.h" 2>/dev/null)
update_manual_en=""
update_manual_it=""
if [ -n "$UPDATE_CHANNEL" ]; then
    update_manual_en="
- \"Settings > Check for updates\": once a day the program reads the
  channel @$UPDATE_CHANNEL on Telegram, where new versions are
  announced. When one is newer than yours the status line says so,
  once, and About keeps saying it. Nothing is downloaded and nothing is
  installed. Switch the line off and the program never looks."
    update_manual_it="
- \"Settings > Check for updates\": una volta al giorno il programma
  legge il canale @$UPDATE_CHANNEL su Telegram, dove vengono annunciate
  le versioni nuove. Quando ce n'e' una piu' nuova della tua la riga di
  stato lo dice, una volta sola, e About continua a dirlo. Non viene
  scaricato ne' installato niente. Spegni la riga e il programma non
  guarda piu'."
fi
if [ -z "$CHANNEL" ]; then
    case $VERSION in
        *.*.*) CHANNEL=alpha ;;
        *)     CHANNEL=beta ;;
    esac
fi

md5of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | awk '{print $1}'; fi; }

AMIGAOS3_BINARY=${AMIGAOS3_BINARY:-"$ROOT_DIR/build/amigaos3-clib2/Amigram"}
# Plain-68000 variant (A500/A600/A1000/CDTV class): the text client with the
# LOWMEM caps, built by scripts/build-68000-lowmem.sh into its own folder
# (build/amigaos3-68000/ is where any other 68000 build lands). Packaged only
# when that binary exists, and refused below unless it is that recipe.
AMIGAOS3_68000_BINARY=${AMIGAOS3_68000_BINARY:-"$ROOT_DIR/build/amigaos3-68000-lowmem/Amigram"}
MORPHOS_BINARY=${MORPHOS_BINARY:-"$ROOT_DIR/build/morphos-cross/Amigram"}
AMIGAOS4_BINARY=${AMIGAOS4_BINARY:-"$ROOT_DIR/build/amigaos4/Amigram"}
AROS_I386_BINARY=${AROS_I386_BINARY:-"$ROOT_DIR/build/aros-i386-abiv0/Amigram"}
AROS_X86_64_BINARY=${AROS_X86_64_BINARY:-"$ROOT_DIR/build/aros-x86_64/Amigram"}
# aarch64 (Raspberry Pi, ABIv1): built on the bench like the x86_64 lane, from
# 6fbf9d0 straight out of make (no relink). Sixth package since 0.0.94.
AROS_AARCH64_BINARY=${AROS_AARCH64_BINARY:-"$ROOT_DIR/build/aros-aarch64/Amigram"}

# --- Aminet artifacts (.lha + .readme) --------------------------------------
# Aminet requires a real LhA ENCODER (the Mac's lhasa is extract-only); we use
# Koji Arai's lha. Set AMINET=0 to skip the Aminet pass. See the verified
# procedure in memory/aminet-publishing.md (Type comm/tcp, 6 per-arch uploads,
# version in the .readme NOT the filename, FTP upload to /new by Michele).
AMINET=${AMINET:-1}
LHA_BIN=${LHA_BIN:-"$HOME/amiga-dev/tools/lha-src/src/lha"}
AMINET_ROOT=${AMINET_ROOT:-"$PACKAGE_ROOT/aminet"}
AROSARCHIVES_ROOT=${AROSARCHIVES_ROOT:-"$PACKAGE_ROOT/arosarchives"} # set here: package_one stages the aarch64 lha there
# The program's name, and the name its archives had in the release before.
# The first Amigram release (0.1) replaces the TelegramAmiga archives on every
# channel through their Replaces fields; from the release after it, set
# PREV_NAME=Amigram (or change the default) so each release replaces the last.
APP_NAME=Amigram
PREV_NAME=${PREV_NAME:-TelegramAmiga}
prev_lower=$(printf '%s' "$PREV_NAME" | tr 'A-Z' 'a-z')
app_lower=$(printf '%s' "$APP_NAME" | tr 'A-Z' 'a-z')
# The line that follows the name wherever we publish, so that whoever knew the
# old name still recognises the program: "Amigram - Unofficial Telegram Amiga
# Client". Telegram's terms (2.3) allow the word Telegram in a title only
# after "Unofficial". Kept for the first releases after the rename; set it
# empty to drop it, and every title falls back to the name alone.
TAGLINE=${TAGLINE-Unofficial Telegram Amiga Client}
if [ -n "$TAGLINE" ]; then
    APP_TITLE="$APP_NAME - $TAGLINE"
else
    APP_TITLE="$APP_NAME"
fi
AMINET_DRAWER=${AMINET_DRAWER:-$APP_NAME}
AMINET_UPLOADER=${AMINET_UPLOADER:-"michele.dipace@kaffeine.net (Michele Dipace)"}
AMINET_AUTHOR=${AMINET_AUTHOR:-"Michele Dipace <michele.dipace@kaffeine.net>"}
DIARY_URL="https://androidlab.it/en/telegram-amiga-mtproto-client-development-diary/"
REPO_URL="https://github.com/kaffeine1/telegram-amiga"
# Voluntary support links, both named in the texts.
SUPPORT_URL="https://www.patreon.com/c/kaffeine1"
# One-time tips. OS4Depot and the AROS Archives turn their donateurl:
# header key into a "Donate" button on the file page, which takes one
# link only: a donation is a one-time thing, so the button goes to Ko-fi.
KOFI_URL="https://ko-fi.com/kaffeine1"

sha_cmd() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }

mkdir -p "$PACKAGE_ROOT"
rm -f "$PACKAGE_ROOT"/"$APP_NAME"-*-"$DATE_STAMP".zip "$PACKAGE_ROOT"/"$APP_NAME"-*-"$DATE_STAMP".info
rm -rf "$PACKAGE_ROOT"/"$APP_NAME"-*-"$DATE_STAMP"
if [ "$AMINET" = "1" ]; then rm -rf "$AMINET_ROOT"; mkdir -p "$AMINET_ROOT"; fi

# Which assets/icons/<lane>/ set a platform ships (scripts/build-icons.sh
# regenerates them from Carlo Spadoni's versions of our icon in assets/icons/carlo-spadoni).
icon_lane() {
    case "$1" in
    "AmigaOS 3.x (68000)") echo amigaos3-68000 ;;
    "AmigaOS 3.x") echo amigaos3 ;;
    "AmigaOS 4"*) echo amigaos4 ;;
    "MorphOS") echo morphos ;;
    AROS*) echo aros ;;
    *) echo "icon_lane: unknown platform $1" >&2; exit 1 ;;
    esac
}

# --- per-architecture text blocks -------------------------------------------
# requirements_en / requirements_it / notes_en / notes_it are filled per
# platform; the rest of each manual is shared.
fill_platform_text() {
    # From 0.0.9 the TUI icon ships only with the 68k package; elsewhere the
    # same binary starts the text client from the Shell, and the manuals say
    # how. tui_icon drives both the packaging and the manual wording.
    case "$1" in
    "AmigaOS 3.x (68000)") tui_icon=1; gui_icon=0 ;;
    "AmigaOS 3.x") tui_icon=1; gui_icon=1 ;;
    *) tui_icon=0; gui_icon=1 ;;
    esac
    # The plain-68000 package thanks the tester who made it possible. Empty
    # elsewhere.
    if [ "$1" = "AmigaOS 3.x (68000)" ]; then
        thanks68k_line_en="Thanks to Paolo Arus (DaRkHoRaCe): without his tenacity in testing, round
after round on an A600, the 68000 version would never have come out.
"
        thanks68k_para_en="$thanks68k_line_en
"
        thanks68k_line_it="Grazie a Paolo Arus (DaRkHoRaCe): senza la sua tenacia nei test, giro
dopo giro sul suo A600, la versione 68000 non sarebbe mai uscita.
"
    else
        thanks68k_line_en=""
        thanks68k_para_en=""
        thanks68k_line_it=""
    fi
    gui_entry_en="- Amigram: the window client (Intuition/GadTools). Double-click it.
"
    gui_entry_it="- Amigram: il client a finestra (Intuition/GadTools). Doppio click.
"
    account_en="You need a Telegram account. The client cannot create one: make it
first with the official app on a phone."
    account_it="Ti serve un account Telegram. Il client non sa crearne uno: fallo
prima con l'app ufficiale su un telefono."
    login_steps_en="2. Enter your phone number in international form (for example
   +39 333 1234567), then the code Telegram sends. If you are signed in
   to Telegram elsewhere, the code arrives there, in the Telegram service
   chat, not by SMS; the client says where it went. If nothing comes,
   wait until the code screen offers another way and press S to have it
   sent again, usually by SMS.
3. If your account has a cloud password (2FA), type it. If not, press
   Enter.
4. The client saves the login in telegram-auth.bin, in this drawer, and
   does not ask again."
    login_steps_it="2. Scrivi il numero di telefono in formato internazionale (per esempio
   +39 333 1234567), poi il codice che Telegram ti manda. Se sei
   collegato a Telegram altrove, il codice arriva li', nella chat di
   servizio di Telegram, non via SMS; il client ti dice dove l'ha
   spedito. Se non arriva niente, aspetta che la schermata del codice
   proponga un'altra via e premi S per fartelo rimandare, di solito via
   SMS.
3. Se il tuo account ha una password cloud (2FA), scrivila. Se no, premi
   Invio.
4. Il client salva l'accesso in telegram-auth.bin, in questo cassetto, e
   non te lo chiede piu'."
    shell_cmd="      Amigram --mtproto-start-file data/telegram-api.txt
        telegram-auth.bin data/phone-code-hash.txt data/telegram-peers.txt"
    if [ "$tui_icon" = "1" ]; then
        launchers_title_en="Two icons
---------"
        launchers_title_it="Due icone
---------"
        tui_entry_en="- Amigram-TUI: the text client, full screen, for slow machines
  or setups without a mouse. Both use the same login."
        tui_entry_it="- Amigram-TUI: il client testuale a schermo intero, per macchine
  lente o senza mouse. Usano lo stesso accesso."
        first_start_en="$account_en

1. Copy the drawer to a writable volume and double-click Amigram
   (or Amigram-TUI).
$login_steps_en"
        first_start_it="$account_it

1. Copia il cassetto su un volume scrivibile e fai doppio click su
   Amigram (o Amigram-TUI).
$login_steps_it"
        readme_programs="  Amigram      the window client
  Amigram-TUI  the text client"
        readme_start="Copy this drawer to a writable volume and double-click Amigram
(or Amigram-TUI). The first run signs you in: phone, code, 2FA."
        aminet_programs="Two clients share one engine and one login:

  Amigram      the window client (Intuition/GadTools): chat list
                     with avatars, bubbles, photos, emoji, menus.
  Amigram-TUI  the text client, for slow machines, a serial
                     console or an ssh session."
        aminet_tui_hint=" (or Amigram-TUI for the text client)"
        if [ "$gui_icon" = "0" ]; then
            launchers_title_en="The icon
--------"
            launchers_title_it="L'icona
-------"
            tui_entry_en="- Amigram-TUI: the text client, full screen. It is the only
  icon here: the window client needs a 68020 or better and is not in
  this build.
- From a Shell: Amigram-TUI is an empty marker file, not a
  program (running it says \"file is not executable\"). Inside the
  drawer type:
      stack 393216
$shell_cmd
  The second command is one line. The stack is needed once per Shell (a
  Shell gives 4096 bytes, and the program says so). If the Shell refuses
  to run it, type \"Protect Amigram +e\" once."
            tui_entry_it="- Amigram-TUI: il client testuale a schermo intero. E' l'unica
  icona: il client a finestra vuole un 68020 o superiore e in questa
  build non c'e'.
- Da Shell: Amigram-TUI e' un file segnaposto vuoto, non un
  programma (lanciarlo da' \"file is not executable\"). Dentro il
  cassetto scrivi:
      stack 393216
$shell_cmd
  Il secondo comando sta su una riga sola. Lo stack serve una volta per
  Shell (una Shell ne da' 4096 byte, e il programma lo dice). Se la
  Shell si rifiuta di eseguirlo, scrivi una volta
  \"Protect Amigram +e\"."
            first_start_en="$account_en

The first login is too heavy for a 68000 (a Diffie-Hellman key
exchange), so it is made elsewhere:

1. On a 68020 or better, or in an emulator (WinUAE, FS-UAE, vAmiga),
   install the standard AmigaOS 3.x package and log in there.
2. Copy telegram-auth.bin from that drawer into this one, next to
   Amigram-TUI, and data/telegram-peers.txt too if you want the
   chat list ready. Never share telegram-auth.bin.
3. Copy this drawer to a writable volume and double-click
   Amigram-TUI. The account stays usable on both machines.

With a cloud password (2FA), checking it takes about 35 minutes on a
stock A1200 and much less on faster machines. The dots show it working,
and the login then completes.

The program lets you try the login on the 68000 anyway, but the route
above is the supported one."
            first_start_it="$account_it

Il primo accesso e' troppo pesante per un 68000 (uno scambio di chiavi
Diffie-Hellman), quindi si fa altrove:

1. Su un 68020 o superiore, o in un emulatore (WinUAE, FS-UAE, vAmiga),
   installa il pacchetto AmigaOS 3.x standard e accedi li'.
2. Copia telegram-auth.bin da quel cassetto in questo, accanto a
   Amigram-TUI, e anche data/telegram-peers.txt se vuoi la lista
   chat gia' pronta. Non condividere mai telegram-auth.bin.
3. Copia questo cassetto su un volume scrivibile e fai doppio click su
   Amigram-TUI. L'account resta usabile su tutte e due le
   macchine.

Con una password cloud (2FA), il controllo richiede circa 35 minuti su
un A1200 liscio e molto meno sulle macchine piu' veloci. I puntini
mostrano che sta lavorando, poi l'accesso si completa.

Il programma ti lascia comunque provare l'accesso sul 68000, ma la via
sopra e' quella supportata."
            readme_programs="  Amigram-TUI  the text client (the window client needs a 68020)"
            readme_start="Copy this drawer to a writable volume. Log in first on a faster Amiga
or in an emulator and copy telegram-auth.bin here (see the manual), then
double-click Amigram-TUI."
            gui_entry_en=""
            gui_entry_it=""
            aminet_programs="  Amigram-TUI  the full-screen text client."
            aminet_tui_hint=""
        fi
    else
        launchers_title_en="One icon, two clients
---------------------"
        launchers_title_it="Un'icona, due client
--------------------"
        tui_entry_en="- The text client is the same program. This package has only the
  window icon; to start the text client, type in a Shell, inside the
  drawer:
$shell_cmd
  (one line). If the Shell refuses to run it, type
  \"Protect Amigram +e\" once. Both use the same login."
        tui_entry_it="- Il client testuale e' lo stesso programma. Questo pacchetto ha solo
  l'icona della finestra; per il client testuale scrivi in una Shell,
  dentro il cassetto:
$shell_cmd
  (su una riga sola). Se la Shell si rifiuta di eseguirlo, scrivi una
  volta \"Protect Amigram +e\". L'accesso e' lo stesso."
        first_start_en="$account_en

1. Copy the drawer to a writable volume and double-click Amigram.
$login_steps_en"
        first_start_it="$account_it

1. Copia il cassetto su un volume scrivibile e fai doppio click su
   Amigram.
$login_steps_it"
        readme_programs="  Amigram  the window client. The same program runs the text
                 client from a Shell (see the manual)."
        readme_start="Copy this drawer to a writable volume and double-click Amigram.
The first run signs you in: phone, code, 2FA."
        aminet_programs="  Amigram  the window client (Intuition/GadTools): chat list with
                 avatars, bubbles, photos, emoji, menus. The same
                 program runs a text client from a Shell; the manual
                 gives the command."
        aminet_tui_hint=""
    fi
    # The first Amigram release tells TelegramAmiga users what to bring along.
    if [ "$PREV_NAME" != "$APP_NAME" ]; then
        coming_en="Coming from $PREV_NAME
$(printf '%s' "Coming from $PREV_NAME" | tr -c '\n' '-')
Until 0.0.99 this program was called $PREV_NAME. To keep your session,
chats and settings, copy from the old drawer into this one:
telegram-auth.bin and the data drawer, and avatars, photos and downloads
if you want them. Do not copy the old program or its icons; the old
drawer can go afterwards.

"
        coming_it="Chi arriva da $PREV_NAME
$(printf '%s' "Chi arriva da $PREV_NAME" | tr -c '\n' '-')
Fino alla 0.0.99 questo programma si chiamava $PREV_NAME. Per tenere
sessione, chat e impostazioni, copia dal vecchio cassetto in questo:
telegram-auth.bin e il cassetto data, e avatars, photos e downloads se
li vuoi. Non copiare il vecchio programma ne' le sue icone; poi il
vecchio cassetto si puo' togliere.

"
    else
        coming_en=""
        coming_it=""
    fi
    notes_title_en="Notes for $1"
    notes_title_it="Note per $1"
    case "$1" in
    "AmigaOS 3.x (68000)")
        upload_limit="125 MiB"
        notes_title_en="Notes for the plain 68000"
        notes_title_it="Note per il 68000 liscio"
        req_en="- A plain 68000 Amiga (A500, A600, A1000, A2000, CDTV) with AmigaOS
  2.x or 3.x, a TCP/IP stack with bsdsocket.library (Roadshow, AmiTCP,
  Miami) and an internet connection.
- About 2 MB of free fast RAM.
- A first login made on a faster Amiga or in an emulator (see below)."
        req_it="- Un Amiga con 68000 liscio (A500, A600, A1000, A2000, CDTV) con
  AmigaOS 2.x o 3.x, uno stack TCP/IP con bsdsocket.library (Roadshow,
  AmiTCP, Miami) e una connessione internet.
- Circa 2 MB di fast RAM libera.
- Un primo accesso fatto su un Amiga piu' veloce o in un emulatore
  (vedi sotto)."
        notes_en="- This build leaves out the window, the renderer and the JPEG decoder,
  which is where the memory saving comes from. On a 68020 or better use
  the standard AmigaOS 3.x package.
- Limits: 12 chats in the list, 8 messages per page, message text up to
  2 KB, downloads in 16 KB pieces. The rest works as elsewhere: sending
  and receiving, files, replies, edits, read receipts.
- Type a message and press Enter to send it. The line under the top one
  shows your first chats on F1 to F10, with a star for new messages. A
  chat number opens a chat too, 0 opens Saved Messages, Tab the chat
  before. /move 5 1 puts chat 5 under F1, /list shows the chats and
  /help (or the Help key) the other commands. Messages from chats not in
  your list are counted in the top line: \"(+3 elsewhere)\"."
        notes_it="- Questa build lascia fuori la finestra, il renderer e il decoder JPEG,
  ed e' da li' che viene il risparmio di memoria. Su un 68020 o
  superiore usa il pacchetto AmigaOS 3.x standard.
- Limiti: 12 chat in lista, 8 messaggi per pagina, testo fino a 2 KB per
  messaggio, download a pezzi da 16 KB. Il resto funziona come altrove:
  invio e ricezione, file, risposte, modifiche, conferme di lettura.
- Scrivi un messaggio e premi Invio per mandarlo. La riga sotto quella
  in alto mostra le prime chat sui tasti da F1 a F10, con un asterisco
  per i messaggi nuovi. Anche il numero di una chat la apre, 0 apre i
  Messaggi salvati, Tab la chat di prima. /move 5 1 mette la chat 5
  sotto F1, /list mostra le chat e /help (o il tasto Help) gli altri
  comandi. I messaggi delle chat fuori lista si contano nella riga in
  alto: \"(+3 elsewhere)\"."
        ;;
    "AmigaOS 3.x")
        # = 32 KiB part x 4000 parts. KEEP IN SYNC with TG_GUI_DL_CHUNK
        # (core/tg_mtproto_probe.c): this said "31 MiB" long after the m68k
        # chunk grew, so the 0.0.7 OS3 package shipped manuals understating
        # the real limit (the binary itself computes and reports 125).
        upload_limit="125 MiB"
        req_en="- AmigaOS 3.1, 3.1.4 or 3.2 with a TCP/IP stack providing
  bsdsocket.library (Roadshow, AmiTCP, Miami/MiamiDx) and an internet
  connection.
- A 68020 or better. A plain 68000 has its own package.
- A few MB of free RAM. No ixemul.library, no AmiSSL."
        req_it="- AmigaOS 3.1, 3.1.4 o 3.2 con uno stack TCP/IP che fornisca
  bsdsocket.library (Roadshow, AmiTCP, Miami/MiamiDx) e una connessione
  internet.
- Un 68020 o superiore. Il 68000 liscio ha un pacchetto suo.
- Qualche MB di RAM libera. Non servono ixemul.library ne' AmiSSL."
        notes_en="- The first login (a Diffie-Hellman key exchange) takes a while on a
  68k. It happens once.
- With a cloud password (2FA), checking it takes about 35 minutes on a
  stock A1200. The dots show it working, and the login completes.
- To change the Workbench screen mode or colours with the window open,
  install screennotify.library in LIBS: (Aminet util/libs/ScreenNotify10):
  the window then closes when asked and comes back on the new screen.
  Without it, use Settings > Use own screen, or quit first.
- Text client (Amigram-TUI): Enter sends. The tabs under the top
  line show the chats on F1 to F10 (/move 5 1 puts chat 5 under F1); a
  chat number opens one, 0 Saved Messages, Tab the chat before, /list
  the chats, /help the commands."
        notes_it="- Il primo accesso (uno scambio di chiavi Diffie-Hellman) richiede un
  po' di tempo su un 68k. Succede una volta sola.
- Con una password cloud (2FA), il controllo richiede circa 35 minuti su
  un A1200 liscio. I puntini mostrano che sta lavorando, e l'accesso si
  completa.
- Per cambiare modo schermo o colori del Workbench con la finestra
  aperta, installa screennotify.library in LIBS: (Aminet
  util/libs/ScreenNotify10): la finestra si chiude quando il sistema lo
  chiede e torna sul nuovo schermo. Senza, usa Settings > Use own screen
  oppure esci prima.
- Client testuale (Amigram-TUI): Invio manda. Le schede sotto la
  riga in alto mostrano le chat sui tasti da F1 a F10 (/move 5 1 mette la
  chat 5 sotto F1); il numero di una chat la apre, 0 i Messaggi salvati,
  Tab la chat di prima, /list le chat, /help i comandi."
        ;;
    "MorphOS")
        upload_limit="250 MiB"
        req_en="- MorphOS 3.x with its TCP/IP stack and an internet connection.
- A few MB of free RAM."
        req_it="- MorphOS 3.x con il suo stack TCP/IP e una connessione internet.
- Qualche MB di RAM libera."
        notes_en="- The chat list is not fetched at the first login: use \"Reload chat
  list\" in the Amigram menu, or find chats with the search box.
- In groups the typing line says \"someone is typing\" and the @ member
  list is off: fetching a group's members freezes MorphOS.
- Own screen: MorphOS 3.16 or newer is recommended."
        notes_it="- La lista chat non viene scaricata al primo accesso: usa \"Reload chat
  list\" nel menu Amigram, oppure cerca le chat con la casella di
  ricerca.
- Nei gruppi la riga di scrittura dice \"someone is typing\" e l'elenco
  dei membri con @ e' spento: scaricare i membri di un gruppo blocca
  MorphOS.
- Schermo proprio: consigliato MorphOS 3.16 o piu' recente."
        ;;
    "AROS aarch64")
        upload_limit="250 MiB"
        req_en="- AROS for ARM64 (aarch64, ABIv1) on a Raspberry Pi 4, 400 or 5: the
  native image of 2026-08-22 or newer, with its TCP/IP stack (AROSTCP)
  running.
- A few MB of free RAM."
        req_it="- AROS per ARM64 (aarch64, ABIv1) su un Raspberry Pi 4, 400 o 5:
  l'immagine nativa del 2026-08-22 o piu' recente, con il suo stack
  TCP/IP (AROSTCP) attivo.
- Qualche MB di RAM libera."
        notes_en="- Start the network before the client: the AROSTCP start script from a
  Shell or from S:User-Startup works better on the Pi images than the
  boot switch in the network preferences.
- The stock image has no wget: copy the drawer onto the SD card from
  another computer, into any drawer on SYS:.
- The client deletes and recreates each file it saves instead of
  rewriting it: the AROS FAT handler leaves a rewritten file empty.
- Tested on a Raspberry Pi 400 and in QEMU (raspi3b)."
        notes_it="- Avvia la rete prima del client: sulle immagini per Pi lo script di
  avvio di AROSTCP, da una Shell o da S:User-Startup, funziona meglio
  dell'interruttore di avvio nelle preferenze di rete.
- L'immagine di serie non ha wget: copia il cassetto sulla scheda SD da
  un altro computer, in un cassetto qualsiasi di SYS:.
- Il client cancella e ricrea ogni file che salva invece di riscriverlo:
  il gestore FAT di AROS lascia vuoto un file riscritto.
- Provato su un Raspberry Pi 400 e in QEMU (raspi3b)."
        ;;
    *)
        upload_limit="250 MiB"
        req_en="- $1 with a TCP/IP stack (bsdsocket) and an internet connection.
- A few MB of free RAM."
        req_it="- $1 con uno stack TCP/IP (bsdsocket) e una connessione internet.
- Qualche MB di RAM libera."
        notes_en="- Nothing specific: everything in this manual applies."
        notes_it="- Niente di particolare: vale tutto quello che dice il manuale."
        ;;
    esac
    notes_en="$notes_title_en
$(printf '%s' "$notes_title_en" | tr -c '\n' '-')
$notes_en"
    notes_it="$notes_title_it
$(printf '%s' "$notes_title_it" | tr -c '\n' '-')
$notes_it"
}

write_readme() {
    cat > "$1" <<EOF
$APP_TITLE
$(printf '%s' "$APP_TITLE" | tr -c '\n' '=')
$2 - $CHANNEL $VERSION

A native Telegram client, written from scratch. No MUI, no ixemul, no
AmiSSL. It uses the Telegram API but is not made by Telegram.

$readme_programs

$readme_start

${coming_en}Manuals: Manual-EN.txt (English), Manuale-IT.txt (Italiano).
Changes: CHANGELOG.txt.

Never share telegram-auth.bin: after the first login it holds your
Telegram session.

${thanks68k_para_en}The icon is our own design. Carlo Spadoni optimised it for each system, put
it on a standard drawer for the drawer icon, and let me ship his versions.

Version: $VERSION   Build: $COMMIT_ID
Author: Michele Dipace <michele.dipace@kaffeine.net>
License: GNU GPL version 3 or later, text in LICENSE
EOF
}

# The window section of the manuals; the plain-68000 build has no window.
manual_window_en() {
    [ "$gui_icon" = "1" ] || return 0
    cat <<EOT
Using the window
----------------
Chats
- The chat list is on the left, the open chat on the right. F1 to F10
  open chats 1 to 10, Shift+F1 to F10 chats 11 to 20.
- Each chat shows its unread count until you open it.
- The search box at the top left finds a chat on Telegram when you press
  Enter, and adds it to the list.
- Del, Right-Amiga+R or "Remove chat from list" removes a chat. Search
  still finds it, marked (hidden), and opening it puts it back. Drag
  chats to reorder them.
- The right mouse button on a chat in the list opens its menu: Pin,
  Archive, Mark as read or unread, Remove chat, Mute... or Unmute,
  Enable ring (groups and channels), Sound..., Show hidden messages,
  and Leave... or Delete chat...
- Mute... asks for how long: an hour, eight hours, or until you unmute.
  The mute goes to Telegram, so your other devices follow it.
- Leave... and Delete chat... take the chat off your account on
  Telegram, after a question. For a chat with one person the question
  is whether its messages go for both of you or for you only. Neither
  can be undone.
- A chat muted on Telegram shows it in the list: its unread count is
  grey instead of blue, and with nothing unread a small crossed
  loudspeaker sits at the right of its row. The marks fill in during
  the first moments after the start.
- Saved Messages, the last chat, is your own cloud: send files or notes
  to it from a phone or PC and pick them up on the Amiga, or the other
  way round. It cannot be removed.

Messages
- Scroll with the wheel, the scrollbar or the arrow keys. At the top,
  older messages load and your place is kept.
- Click the input line to write; a click inside the text moves the
  cursor. Enter sends, Shift+Enter starts a new line. A message that
  cannot be sent stays in the input line, and the status line says why.
- Click a message to reply to it. The right mouse button on a message
  opens its menu: React..., Reply, Forward to Saved, Forward to..., and
  on your own messages Edit and Delete (for everyone).
- In a chat with one person Delete is on every message, theirs too, and
  asks how: for you and for them, or for you only.
- A chat with pinned messages shows the latest under its header,
  "Pinned:" and how it starts; a click shows the chat around it, and
  the next click the one pinned before. Pin and Unpin are in a
  message's menu. Pin asks how: in a chat with one person for both of
  you or for you only; in a group or a channel, where only those who
  may pin can, telling the members or quietly.
- In a group or a channel, someone else's message offers Hide message:
  it leaves the chat and stays out. "Show hidden messages" in the chat's
  menu brings it back. In a basic group Telegram also deletes it for
  you on every device, and that cannot be undone. The text client has
  /hide (the last message received, or /hide <id>) and /unhide.
- In a group, the menu of someone else's message has Message <name>:
  it opens a private chat with them. The chat joins the list if it was
  not there, and nothing is sent until you write.
- One tick means sent, two blue ticks read.
- In groups, "<name> is typing..." shows who is writing.
- To mention someone in a group, type @ in the input line: a list of
  people opens, those who wrote lately first; go on typing to narrow it
  by name or username (from three letters on Telegram is asked for the
  members that match). The list shows eight people at a time and
  scrolls: Up/Down or the mouse wheel move through it, Enter, Tab or a
  click inserts, Esc closes. Someone with a username is written as
  @username; someone without is written by first name, and the message
  still reaches them as a mention as long as the name is left as it was
  inserted.
- Formatting can be typed: **bold**, __italic__, \`code\`,
  ~~strike~~, ||spoiler||, and three backticks before and after a block
  of code. When the message is sent the markers leave the text and the
  other clients show the styles; here a spoiler shows as plain text. A
  marker counts only at the start and at the end of a word, and a style
  of one line ends on that line, so that 2 ** 3, a || b or __init__.py
  go as they are. "Settings > Markdown formatting" switches it off, and
  then everything goes as typed.
- Edit brings a message back into the input line with its formatting
  written as those markers, so that saving it keeps the styles. Where
  that cannot be done (bold in the middle of a word, for one) the plain
  text comes back and the status line says that saving changes the
  formatting. ENTER on a text you did not change sends nothing; if an
  edit cannot be saved the text stays in the input line.
- Drag across text to select it. Right-Amiga+C copies, Right-Amiga+X
  cuts from the input line, Right-Amiga+V pastes; the Edit menu has the
  same.
- Reactions show under a message with their count, yours highlighted.
  React... opens a grid of the reactions the chat allows: a click adds
  one, a click on yours takes it back. When the chat allows more than
  ten, the last cell is an arrow that opens them all. In the text client
  /react <name> reacts to the last message received, /reactions lists
  the names and /ids shows the message numbers that /react <id> <name>,
  /forward and /forwardto take.
- Messages edited on another device change here too.
- To act on several messages together, choose Select in the menu of
  one of them: it is marked, and a click on any other message marks it
  or takes its mark away; Shift+click marks everything from the message
  last clicked to that one. The input line gives way to a bar that says
  how many are selected, with Forward, Delete and Cancel; Esc cancels
  too, and so does opening another chat. Forward asks for the chat and
  sends them in order. Delete asks once for all of them: in a group
  your own messages go for everyone and other people's are hidden here;
  in a chat with one person it asks whether they go for both of you or
  for you only.
- A poll shows its question and its answers, numbered. To vote, open
  the menu of the poll's message (right mouse button) and choose
  Vote...: the answers take the menu's place, and a click votes. Where
  the poll allows several answers each has a mark, a click turns it,
  and Send vote sends them. Once you have voted the message shows the
  share of each answer and the number of votes, and follows the votes
  of the others while the chat is open. Take back vote, in the same
  menu, undoes it; the answer given to a quiz stays.
- "Create poll..." in the first menu makes a poll in the open chat: a
  question, two to ten answers, and a switch to allow several answers.
  TAB or the cursor keys move between the lines, ENTER goes to the next
  one and, on Create, sends. Votes are anonymous. Telegram takes polls
  in groups and channels, not in a chat with one person.
- Stop poll, in the menu of a poll you made, closes it after a
  confirmation: nobody can vote any more, the message shows the final
  results, and a stopped poll cannot be opened again.

Emoji
- The smiley next to Send, or "Insert emoji..." (Amiga+E), opens the
  panel: recent emoji first, then all of them. Arrows move, Enter
  inserts, Esc closes; a click inserts too.
- With "Settings > Emoticons become emoji", a typed :) :D ;) :P :( :'(
  :O :* <3 or xD becomes its emoji after a space or on sending.
  Backspace right after undoes it.
- With "Settings > Enable emoji" off, emoji show as text emoticons.
- Stickers show the emoji they stand for: their picture format cannot
  be decoded here.

Photos and previews
- Photos appear in the message, blurred first, then sharp. Videos show
  a frame and links a preview, the same way.
- Click a photo for a larger view. "Settings > Full-size photos" opens
  the largest copy the screen can show. "Save photo as..." (right mouse
  button, or S in the viewer) saves it in its own format.
- With "Settings > Show inline photos" off, photos stay [Photo] labels
  and nothing loads in the background; click a label to see that photo.
- With "Settings > Show avatars" off, chats keep their coloured initials
  and no profile pictures are downloaded.
- Photos, emoji and avatars start off on AGA/ECS/OCS screens (classic
  OS4 included) and on 68k CPUs below a 68040. A choice in the Settings
  menu overrides that and is kept.
- "Settings > Photo dithering" (Full, Light, Off) applies to paletted
  screens. RTG screens on MorphOS, AmigaOS 4 and AROS use true colour.
- "Settings > Photo cache limit" keeps photos/ within 10, 50 (the
  default) or 200 MB, or Unlimited. "Clear cache..." empties photos/ and
  leaves avatars/ alone.

Files
- A file shows as [File: name (size)]. Right mouse button > Download
  saves it in downloads/, or in the drawer chosen with "Settings >
  Download drawer..." (RAM: is much faster than a slow disk, and a
  reboot empties it). A file sent without a name gets one from its type,
  such as video_1a2b3c4d.mp4.
- Once a file is downloaded, its message's menu offers Open and Show in
  drawer in place of Download. Open hands the file to the Workbench, as
  a double click on its icon would; Show in drawer opens the drawer it
  is in. A program is run only after you confirm it: run one only if
  you trust who sent it. On a Workbench that cannot open files for a
  program (3.0, 3.1) the status line says where the file is. If you
  move or delete the file, the menu offers Download again.
- Send a file with "Send file..." (Amiga+F), or by dropping its icon
  on the window: up to $upload_limit on this build. "Send photo..."
  (Amiga+P) sends a JPEG or PNG as a photo, and a dropped picture asks
  Photo, File or Cancel. The paperclip left of the input line opens a
  small menu with the same two, Photo... and File..., and Poll... where
  Telegram takes polls; any key or a click elsewhere closes it.
  Telegram may re-encode photos as JPEG: send a PNG as File to keep it
  as it is. A photo over 10 MiB goes as a file.
- Several files at once: in the file requester hold Shift and click
  more than one, ten at most. Photos picked together with "Send
  photo..." leave as an album, one message: a small
  window asks for the caption, which goes on the first photo, and
  offers Album, Files or Cancel. Other files, or photos and other
  files picked together, go one message each, one after the other.
  They all go to the chat that was open when you
  picked them, even if you open another one meanwhile. The status line
  says which one is going up (2/5), and Esc stops them all; a file
  that cannot be sent is left out and the others still go.
- The status line shows progress and speed, and Esc cancels. You can
  keep chatting during a transfer.

Notifications
- "Settings > Notifications" chooses between nothing, the count in the
  window title, the count with a pop-up, and a pop-up with the text.
- With Beep on, a chat that rings plays the built-in chime or a file
  picked with "Sound file..." (IFF 8SVX, or WAV where a datatype reads
  it, up to 1 MB). The sounds/ drawer has six to start with.
- Direct chats ring; groups and channels stay quiet until you choose
  Enable ring in their menu. A chat muted in Telegram stays muted here.
  In the text client /ring on and /ring off do the same for the open
  chat.

Settings
- "Settings..." in the first menu (Amiga+T) opens the settings over the
  chat: every preference is a line there. Up and Down move, SPACE,
  ENTER or a click change the line, LEFT and RIGHT step a choice back
  and on. The Close button at the top right, ESC, or a click on the chat
  list closes it. Where this manual says "Settings > ...", it is a line
  of that panel.$update_manual_en

Window and screen
- The window remembers its size and position
  (data/telegram-gui-win.txt).
- "Settings > Use own screen" opens the client on a screen of its own;
  if that screen cannot open, it falls back to a window.
- "Iconify" (Amiga+I) leaves an icon on the Workbench; double-click it
  to come back.

ARexx
- The program has an ARexx port named AMIGRAM (AMIGRAM.1 for a second
  copy), for scripts and the Shell:
      rx "address AMIGRAM 'SHOW'"
  SHOW brings the window up, from its icon too; HIDE leaves it on its
  icon; OPEN <name> opens the chat with that name, or the first whose
  name starts with it or contains it; QUIT ends the program.
- UNREAD, CHAT and VERSION answer with a text in RESULT (the script
  needs "options results"): how many unread messages there are, the
  name of the open chat, the program's version.
- SEND <chat> <text> sends a message: SEND "Mario Rossi" build done.
  The chat's name must be the whole name, in quotes if it has spaces.
  It is switched off until you allow it: any program on your machine
  could otherwise write as you.
- The result code is 0; 5 when no chat has that name; 10 for an unknown
  command, a missing argument, or SEND while it is switched off; 20
  when Telegram could not be reached.
- The two switches are in the settings: "ARexx port" and "Scripts may
  send messages". The port is on and sending is off until you change
  them.
EOT
}

manual_window_it() {
    [ "$gui_icon" = "1" ] || return 0
    cat <<EOT
Usare la finestra
-----------------
Chat
- A sinistra la lista chat, a destra la chat aperta. I tasti da F1 a F10
  aprono le chat da 1 a 10, Shift+F1..F10 quelle da 11 a 20.
- Ogni chat mostra i messaggi non letti finche' non la apri.
- La casella di ricerca in alto a sinistra trova una chat su Telegram
  quando premi Invio, e la aggiunge alla lista.
- Del, Amiga-destro+R o "Remove chat from list" toglie una chat. La
  ricerca la trova ancora, segnata (hidden), e aprirla la rimette in
  lista. Trascina le chat per cambiarne l'ordine.
- Il tasto destro su una chat della lista apre il suo menu: Pin,
  Archive, Mark as read o unread, Remove chat, Mute... o Unmute, Enable
  ring (gruppi e canali), Sound..., Show hidden messages, e Leave... o
  Delete chat...
- Mute... chiede per quanto: un'ora, otto ore, o finche' non togli il
  silenzioso. Il silenzioso va a Telegram, quindi lo seguono anche gli
  altri tuoi dispositivi.
- Leave... e Delete chat... tolgono la chat dal tuo account su
  Telegram, dopo una domanda. Per una chat con una persona la domanda
  e' se i messaggi spariscono per tutti e due o solo per te. Nessuna
  delle due si puo' annullare.
- Una chat silenziata su Telegram lo mostra nell'elenco: il numero dei
  non letti e' grigio invece che azzurro, e se non c'e' niente da
  leggere un piccolo altoparlante barrato sta a destra della sua riga.
  I segni compaiono nei primi momenti dopo l'avvio.
- I Messaggi salvati, l'ultima chat, sono il tuo cloud: mandaci file o
  appunti dal telefono o dal PC e riprendili sull'Amiga, o al contrario.
  Non si possono togliere.

Messaggi
- Scorri con la rotella, la barra o le frecce. In cima si caricano i
  messaggi piu' vecchi e il punto in cui eri resta fermo.
- Clicca la riga di input per scrivere; un click dentro il testo sposta
  il cursore. Invio manda, Shift+Invio va a capo. Un messaggio che non
  si riesce a mandare resta nella riga di input, e la riga di stato
  dice perche'.
- Clicca un messaggio per rispondere. Il tasto destro su un messaggio
  apre il suo menu: React..., Reply, Forward to Saved, Forward to..., e
  sui tuoi messaggi Edit e Delete (per tutti).
- In una chat con una persona Delete c'e' su ogni messaggio, anche sui
  suoi, e chiede come: per te e per l'altra persona, o solo per te.
- Una chat con messaggi fissati mostra l'ultimo sotto l'intestazione,
  "Pinned:" e come comincia; un click mostra la chat attorno a quel
  messaggio, e il click dopo quello fissato prima. Pin e Unpin sono nel
  menu di un messaggio. Pin chiede come: in una chat con una persona
  per tutti e due o solo per te; in un gruppo o in un canale, dove
  fissa solo chi puo', avvisando i membri o in silenzio.
- In un gruppo o in un canale, sul messaggio di un altro c'e' Hide
  message: esce dalla chat e resta fuori. "Show hidden messages" nel
  menu della chat lo riporta. In un gruppo semplice Telegram lo cancella
  anche per te su tutti i dispositivi, e non si torna indietro. Nel
  client testuale ci sono /hide (l'ultimo messaggio ricevuto, o
  /hide <id>) e /unhide.
- In un gruppo il menu del messaggio di un altro ha Message <nome>:
  apre una chat privata con quella persona. La chat entra nell'elenco
  se non c'era, e non parte niente finche' non scrivi.
- Una spunta vuol dire inviato, due spunte azzurre letto.
- Nei gruppi "<nome> is typing..." dice chi sta scrivendo.
- Per menzionare qualcuno in un gruppo scrivi @ nella riga di input: si
  apre un elenco di persone, per primo chi ha scritto da poco; continua
  a scrivere per restringerlo per nome o username (da tre lettere in su
  Telegram cerca fra i membri quelli che corrispondono). L'elenco
  mostra otto persone alla volta e scorre: Su/Giu' o la rotella lo
  percorrono, Invio, Tab o un click inserisce, Esc chiude. Chi ha uno
  username viene scritto come @username; chi non lo ha viene scritto
  col nome, e il messaggio gli arriva lo stesso come menzione finche' il
  nome resta com'e' stato inserito.
- La formattazione si puo' scrivere: **grassetto**, __corsivo__,
  \`codice\`, ~~barrato~~, ||spoiler||, e tre accenti gravi prima e
  dopo un blocco di codice. Quando il messaggio parte i marcatori
  escono dal testo e gli altri client mostrano gli stili; qui uno
  spoiler si vede come testo normale. Un marcatore vale solo
  all'inizio e alla fine di una parola, e uno stile di una riga finisce
  su quella riga: cosi' 2 ** 3, a || b o __init__.py partono come sono.
  "Settings > Markdown formatting" lo spegne, e allora tutto parte
  com'e' scritto.
- Edit riporta un messaggio nella riga di input con la sua
  formattazione scritta con quei marcatori, cosi' salvandolo gli stili
  restano. Dove non si puo' (un grassetto in mezzo a una parola, per
  esempio) torna il testo semplice e la riga di stato dice che il
  salvataggio cambia la formattazione. INVIO su un testo che non hai
  cambiato non manda niente; se una modifica non si salva il testo
  resta nella riga di input.
- Trascina sul testo per selezionarlo. Amiga-destro+C copia,
  Amiga-destro+X taglia dalla riga di input, Amiga-destro+V incolla; il
  menu Edit fa lo stesso.
- Le reazioni compaiono sotto un messaggio con il loro numero, la tua
  evidenziata. React... apre una griglia con le reazioni che la chat
  permette: un click ne aggiunge una, un click sulla tua la toglie. Se
  la chat ne permette piu' di dieci, l'ultima casella e' una freccia che
  le apre tutte. Nel client testuale /react <nome> reagisce all'ultimo
  messaggio ricevuto, /reactions elenca i nomi e /ids mostra i numeri
  dei messaggi che /react <id> <nome>, /forward e /forwardto accettano.
- I messaggi modificati da un altro dispositivo cambiano anche qui.
- Per agire su piu' messaggi insieme scegli Select nel menu di uno di
  loro: viene segnato, e un click su un altro messaggio lo segna o gli
  toglie il segno; Shift+click segna tutto dal messaggio cliccato per
  ultimo fino a quello. La riga di input lascia il posto a una barra
  che dice quanti sono selezionati, con Forward, Delete e Cancel; anche
  Esc annulla, e cosi' aprire un'altra chat. Forward chiede la chat e
  li manda in ordine. Delete chiede una volta sola per tutti: in un
  gruppo i tuoi messaggi spariscono per tutti e quelli degli altri
  vengono nascosti qui; in una chat con una persona chiede se vanno via
  per tutti e due o solo per te.
- Un sondaggio mostra la domanda e le risposte, numerate. Per votare
  apri il menu del messaggio del sondaggio (tasto destro) e scegli
  Vote...: le risposte prendono il posto del menu, e un click vota. Se
  il sondaggio permette piu' risposte ognuna ha un segno, un click lo
  cambia, e Send vote le manda. Dopo il voto il messaggio mostra la
  percentuale di ogni risposta e il numero dei voti, e segue i voti
  degli altri finche' la chat e' aperta. Take back vote, nello stesso
  menu, lo ritira; la risposta data a un quiz resta.
- "Create poll..." nel primo menu crea un sondaggio nella chat aperta:
  una domanda, da due a dieci risposte, e un interruttore per
  permettere piu' risposte. TAB o i tasti cursore passano da una riga
  all'altra, INVIO va alla successiva e, su Create, lo manda. I voti
  sono anonimi. Telegram accetta i sondaggi nei gruppi e nei canali,
  non nella chat con una persona sola.
- Stop poll, nel menu di un sondaggio fatto da te, lo chiude dopo una
  conferma: nessuno puo' piu' votare, il messaggio mostra i risultati
  finali, e un sondaggio fermato non si riapre.

Emoji
- La faccina accanto a Send, o "Insert emoji..." (Amiga+E), apre il
  pannello: prima le emoji recenti, poi tutte. Le frecce si muovono,
  Invio inserisce, Esc chiude; anche un click inserisce.
- Con "Settings > Emoticons become emoji", un :) :D ;) :P :( :'( :O :*
  <3 o xD scritto diventa la sua emoji dopo uno spazio o all'invio.
  Backspace subito dopo lo annulla.
- Con "Settings > Enable emoji" spento, le emoji diventano emoticon
  testuali.
- Gli sticker mostrano l'emoji che rappresentano: il formato della loro
  immagine qui non si decodifica.

Foto e anteprime
- Le foto compaiono nel messaggio, prima sfocate, poi nitide. I video
  mostrano un fotogramma e i link un'anteprima, allo stesso modo.
- Clicca una foto per vederla piu' grande. "Settings > Full-size
  photos" apre la copia piu' grande che lo schermo puo' mostrare. "Save
  photo as..." (tasto destro, o S nel visualizzatore) la salva nel suo
  formato.
- Con "Settings > Show inline photos" spento, le foto restano etichette
  [Photo] e non si carica niente in sottofondo; clicca un'etichetta per
  vedere quella foto.
- Con "Settings > Show avatars" spento, le chat tengono le iniziali
  colorate e non si scaricano le foto profilo.
- Foto, emoji e avatar partono spenti sugli schermi AGA/ECS/OCS (OS4
  classic compreso) e sulle CPU 68k sotto il 68040. Una scelta nel menu
  Settings vale al posto di questa e resta memorizzata.
- "Settings > Photo dithering" (Full, Light, Off) vale per gli schermi a
  palette. Gli schermi RTG di MorphOS, AmigaOS 4 e AROS usano i colori
  veri.
- "Settings > Photo cache limit" tiene photos/ entro 10, 50 (il valore
  di partenza) o 200 MB, oppure Unlimited. "Clear cache..." svuota
  photos/ e non tocca avatars/.

File
- Un file compare come [File: nome (dimensione)]. Tasto destro >
  Download lo salva in downloads/, o nel cassetto scelto con "Settings >
  Download drawer..." (RAM: e' molto piu' veloce di un disco lento, e un
  riavvio la svuota). Un file mandato senza nome ne prende uno dal suo
  tipo, come video_1a2b3c4d.mp4.
- Quando un file e' stato scaricato, il menu del suo messaggio offre
  Open e Show in drawer al posto di Download. Open passa il file al
  Workbench, come un doppio click sulla sua icona; Show in drawer apre
  il cassetto in cui si trova. Un programma viene avviato solo dopo la
  tua conferma: avvialo solo se ti fidi di chi l'ha mandato. Su un
  Workbench che non sa aprire i file per conto di un programma (3.0,
  3.1) la riga di stato dice dov'e' il file. Se sposti o cancelli il
  file, il menu torna a offrire Download.
- Manda un file con "Send file..." (Amiga+F) o trascinandone l'icona
  sulla finestra: fino a $upload_limit su questa build. "Send photo..."
  (Amiga+P) manda un JPEG o un PNG come foto, e un'immagine trascinata
  chiede Photo, File o Cancel. La graffetta a sinistra della riga di
  input apre un piccolo menu con le stesse due, Photo... e File..., e
  Poll... dove Telegram accetta i sondaggi; un tasto qualsiasi o un
  click altrove lo chiude. Telegram puo' ricodificare le foto in JPEG:
  manda un PNG come File per tenerlo com'e'. Una foto oltre 10 MiB
  parte come file.
- Piu' file insieme: nel requester tieni premuto Shift e fai click su
  piu' di uno, dieci al massimo. Le foto scelte insieme con "Send
  photo..." partono come album, un solo messaggio:
  una finestrella chiede la didascalia, che va sulla prima foto, e
  offre Album, Files o Cancel. Gli altri file, o foto e altri file
  scelti insieme, partono un messaggio ciascuno, uno dopo l'altro.
  Vanno tutti nella chat che era aperta
  quando li hai scelti, anche se nel frattempo ne apri un'altra. La
  riga di stato dice quale sta salendo (2/5), ed Esc li ferma tutti;
  un file che non si puo' mandare viene lasciato fuori e gli altri
  partono lo stesso.
- La riga di stato mostra avanzamento e velocita', ed Esc annulla.
  Durante un trasferimento puoi continuare a scrivere.

Notifiche
- "Settings > Notifications" sceglie fra niente, il numero nel titolo
  della finestra, il numero con un pop-up, e un pop-up con il testo.
- Con Beep acceso, una chat che suona fa sentire il campanello interno
  o un file scelto con "Sound file..." (IFF 8SVX, o WAV dove un datatype
  lo legge, fino a 1 MB). Il cassetto sounds/ ne ha sei per cominciare.
- Le chat dirette suonano; gruppi e canali restano zitti finche' non
  scegli Enable ring nel loro menu. Una chat silenziata in Telegram resta
  silenziata anche qui. Nel client testuale /ring on e /ring off fanno
  lo stesso per la chat aperta.

Impostazioni
- "Settings..." nel primo menu (Amiga+T) apre le impostazioni sopra la
  chat: ogni preferenza e' una riga. Su e Giu' si spostano, SPAZIO,
  INVIO o un click cambiano la riga, SINISTRA e DESTRA fanno girare una
  scelta indietro e avanti. Lo chiudono il pulsante Close in alto a
  destra, ESC, o un click sull'elenco delle chat. Dove questo manuale
  dice "Settings > ...", e' una riga di quel pannello.$update_manual_it

Finestra e schermo
- La finestra ricorda dimensione e posizione
  (data/telegram-gui-win.txt).
- "Settings > Use own screen" apre il client su uno schermo suo; se lo
  schermo non si apre, torna alla finestra.
- "Iconify" (Amiga+I) lascia un'icona sul Workbench; doppio click per
  tornare.

ARexx
- Il programma ha una porta ARexx di nome AMIGRAM (AMIGRAM.1 per una
  seconda copia), per gli script e la Shell:
      rx "address AMIGRAM 'SHOW'"
  SHOW porta su la finestra, anche dall'icona; HIDE la lascia sulla sua
  icona; OPEN <nome> apre la chat con quel nome, o la prima il cui nome
  comincia cosi' o lo contiene; QUIT chiude il programma.
- UNREAD, CHAT e VERSION rispondono con un testo in RESULT (allo script
  serve "options results"): quanti messaggi non letti ci sono, il nome
  della chat aperta, la versione del programma.
- SEND <chat> <testo> manda un messaggio: SEND "Mario Rossi" build
  finita. Il nome della chat deve essere quello intero, tra virgolette
  se ha spazi. E' spento finche' non lo permetti tu: altrimenti
  qualunque programma sulla macchina potrebbe scrivere a tuo nome.
- Il codice di ritorno e' 0; 5 se nessuna chat ha quel nome; 10 per un
  comando sconosciuto, un argomento mancante, o SEND mentre e' spento;
  20 se Telegram non e' raggiungibile.
- I due interruttori sono nelle impostazioni: "ARexx port" e "Scripts
  may send messages". La porta e' accesa e l'invio e' spento finche'
  non li cambi.
EOT
}

write_manual_en() {
    window_section=$(manual_window_en)
    [ -z "$window_section" ] || window_section="$window_section

"
    cat > "$1" <<EOF
$APP_TITLE
$(printf '%s' "$APP_TITLE" | tr -c '\n' '=')
User manual

Platform: $2
Version: $VERSION   Build: $COMMIT_ID

A native Telegram client written from scratch on Telegram's MTProto
protocol. You log in with a normal Telegram account. It needs no MUI, no
ixemul and no AmiSSL: the cryptography is built in. It uses the Telegram
API but is not made by Telegram.

System requirements
-------------------
$req_en

$launchers_title_en
$gui_entry_en$tui_entry_en

The text client
---------------
Long lines wrap by words and the composer grows to three rows.
Shift+Up/Down scrolls by whole messages. To send a file, type "/sendfile "
for a document or "/photo " for a JPEG or PNG (words after the path become
the caption), then drop the file's icon on the console window, or on the
"TG drop" icon on AmigaOS 4, where the system keeps window drops for
itself. The path appears in the input line and Enter sends. Quit with
/quit, Ctrl+C or the close gadget; the window stays open so you can read
the last lines, and one more click on the gadget closes it.

First start
-----------
$first_start_en

${coming_en}${window_section}Message times
-------------
Times follow the Amiga clock, so set it in the system preferences. If
the clock was never set (no battery clock), the client takes Telegram's
time with the time zone from the Locale preferences.

$notes_en

Privacy
-------
telegram-auth.bin is your logged-in session: never share it, and never
use someone else's. In screenshots hide phone numbers, codes, passwords
and private messages.

The client carries its own Telegram api credentials. To use yours, put
them in data/telegram-api.txt: two lines, api_id then api_hash.

Credits
-------
Contributions: Javier de las Rivas (javierdlr), and Bohun (bohunamiga),
who took the build to AROS on ARM. The icon is our own design: Carlo
Spadoni optimised it for each system, put it on a standard drawer for
the drawer icon, and let me ship his versions.
${thanks68k_line_en}Thanks to the testers who run it on real hardware and report what they
find.

The emoji pictures are Noto Emoji glyphs (Google, SIL Open Font License
1.1) reduced to sixteen pixels; the licence text is in the archive.

Support my work on Patreon: $SUPPORT_URL
or with a one-time tip on Ko-fi: $KOFI_URL
It is voluntary: nothing is needed to use the program. Both links are
also under "Support development..." in the Amigram menu and at the end
of /help in the text client.

License: GNU GPL version 3 or later (MIT up to the 0.1 beta); the
text is in the LICENSE file. A non-commercial community project. Diary:
https://androidlab.it/en/telegram-amiga-mtproto-client-development-diary/
EOF
}

write_manual_it() {
    window_section=$(manual_window_it)
    [ -z "$window_section" ] || window_section="$window_section

"
    cat > "$1" <<EOF
$APP_TITLE
$(printf '%s' "$APP_TITLE" | tr -c '\n' '=')
Manuale

Piattaforma: $2
Versione: $VERSION   Build: $COMMIT_ID

Un client Telegram nativo, scritto da zero sul protocollo MTProto di
Telegram. Accedi con un normale account Telegram. Non servono MUI, ixemul
o AmiSSL: la crittografia e' integrata. Usa le API di Telegram ma non e'
fatto da Telegram.

Requisiti di sistema
--------------------
$req_it

$launchers_title_it
$gui_entry_it$tui_entry_it

Il client testuale
------------------
Le righe lunghe vanno a capo per parole e il composer cresce fino a tre
righe. Shift+Su/Giu' scorre per messaggi interi. Per mandare un file
scrivi "/sendfile " per un documento o "/photo " per un JPEG o PNG (le
parole dopo il percorso diventano la didascalia), poi trascina l'icona
del file sulla finestra console, o sull'icona "TG drop" su AmigaOS 4,
dove i drop sulle finestre li tiene il sistema. Il percorso compare nella
riga di input e Invio lo manda. Per uscire: /quit, Ctrl+C o il gadget di
chiusura; la finestra resta aperta per leggere le ultime righe, e un
altro click sul gadget la chiude.

Primo avvio
-----------
$first_start_it

${coming_it}${window_section}Orari dei messaggi
------------------
Gli orari seguono l'orologio dell'Amiga, quindi impostalo nelle
preferenze di sistema. Se l'orologio non e' mai stato impostato (niente
orologio a batteria), il client prende l'ora di Telegram con il fuso
delle preferenze Locale.

$notes_it

Privacy
-------
telegram-auth.bin e' la tua sessione: non condividerlo mai e non usare
quello di qualcun altro. Negli screenshot nascondi numeri di telefono,
codici, password e messaggi privati.

Il client ha le sue credenziali api di Telegram. Per usare le tue,
mettile in data/telegram-api.txt: due righe, api_id e poi api_hash.

Riconoscimenti
--------------
Contributi: Javier de las Rivas (javierdlr) e Bohun (bohunamiga), che ha
portato la build su AROS ARM. Il disegno dell'icona e' nostro: Carlo
Spadoni l'ha ottimizzata per ogni sistema, l'ha messa su un cassetto
standard per l'icona del cassetto e mi ha permesso di distribuire le sue
versioni.
${thanks68k_line_it}Grazie ai tester che lo provano su hardware vero e raccontano quello che
trovano.

Le immagini delle emoji sono glifi Noto Emoji (Google, SIL Open Font
License 1.1) ridotti a sedici pixel; il testo della licenza e'
nell'archivio.

Sostieni il mio lavoro su Patreon: $SUPPORT_URL
oppure con un contributo una tantum su Ko-fi: $KOFI_URL
E' volontario: per usare il programma non serve niente. I due link sono
anche in "Support development..." nel menu Amigram e in fondo a /help
nel client testuale.

Licenza: GNU GPL versione 3 o successive (MIT fino alla beta 0.1); il
testo e' nel file LICENSE. Un progetto di comunita' non commerciale.
Diario:
https://androidlab.it/telegram-amiga-diario-sviluppo-client-mtproto/
EOF
}

# Aminet per-architecture metadata. Sets: archtag (FILENAME suffix), archval
# (the readme Architecture: value), requires. NB the Aminet enum has no
# x86_64-aros token -> 64-bit AROS uses archval i386-aros, distinguished only by
# the x86_64-aros FILENAME (verified live, e.g. filesysbox.x86_64-aros).
# From 0.0.6 the archives are named after the binary (Amigram since 0.1) in the
# classic Aminet suffix style -- the 30-char filename limit rules out the long
# arch tags (Amigram.m68k-amigaos.readme fits, the old name did not). Replaces: names
# the previous release of the same archive (lhaold below): Aminet refuses an
# upload whose Replaces: points at an archive it no longer has, which is what
# the archive names from before 0.0.6 did to all five 0.0.94 uploads (0.0.93
# still passed with them in September 2026).
aminet_meta() {
    aminet_upload=1 # 0 = the lha is built for The AROS Archives only
    # Short: is at most 40 characters: the tagline fits (32), the name is
    # the archive's own file name anyway.
    aminet_short=${TAGLINE:-Native Telegram chat client}
    aminet_kind=full
    case "$1" in
    amigaos3)    archtag="m68k-amigaos"; archval="m68k-amigaos >= 3.0.0"
                 lhaname="$APP_NAME"
                 requires="68020+ CPU and a bsdsocket.library TCP/IP stack" ;;
    morphos)     archtag="ppc-morphos";  archval="ppc-morphos"
                 lhaname="$APP_NAME-MOS"
                 requires="MorphOS 3.x with its TCP/IP stack" ;;
    amigaos4)    archtag="ppc-amigaos";  archval="ppc-amigaos >= 4.0.0"
                 lhaname="$APP_NAME-OS4"
                 requires="AmigaOS 4.x with its TCP/IP stack" ;;
    aros-i386)   archtag="i386-aros";    archval="i386-aros"
                 lhaname="$APP_NAME-AROS"
                 requires="AROS (i386) with a TCP/IP stack (AROSTCP)" ;;
    aros-x86_64) archtag="x86_64-aros";  archval="i386-aros"
                 lhaname="$APP_NAME-AROS64"
                 requires="AROS (x86_64) with a TCP/IP stack (AROSTCP)" ;;
    # The ARM64 lane is an Aminet upload too since 0.0.95 (first one sent on
    # 2026-10-08). Aminet's architecture list has no ARM token (m68k-amigaos,
    # ppc-amigaos, ppc-morphos, i386-aros, i386-amithlon, ppc-warpup,
    # ppc-powerup, generic, other). Up to 0.0.97 it said "other", which shows
    # no AROS icon on the archive's page; from 0.0.98 it says i386-aros like
    # the x86_64 lane and our BebboSSH port's aarch64 upload, so it is listed
    # with the other AROS builds. The file name and Requires: say it is ARM.
    aros-aarch64) archtag="aarch64-aros"; archval="i386-aros"
                 lhaname="$APP_NAME-ARM64"
                 requires="AROS aarch64 (ABIv1, Raspberry Pi 4/400/5) with AROSTCP" ;;
    # The plain-68000 text client, on Aminet from 0.0.96 under its own name:
    # sharing the 68020 package's name would replace that one. Its first
    # upload (0.0.96) went with AMINET_NEW and no Replaces:.
    amigaos3-68000) archtag="m68k-amigaos"; archval="m68k-amigaos >= 3.0.0"
                 lhaname="$APP_NAME-68000"
                 if [ -n "$TAGLINE" ]; then
                     aminet_short="$TAGLINE, 68000"
                 else
                     aminet_short="Telegram text client for 68000 machines"
                 fi
                 aminet_kind=tui68k
                 requires="68000, 2 MB fast RAM, TCP/IP, login made on a faster Amiga" ;;
    *) echo "aminet_meta: unknown arch $1" >&2; exit 1 ;;
    esac
    # Replaces: names the same lane's archive under the previous name.
    if [ "$aminet_upload" = 1 ]; then lhaold="comm/tcp/$PREV_NAME${lhaname#"$APP_NAME"}.lha"; else lhaold=""; fi
    # AMINET_NEW lists archives Aminet does not have yet (a first upload):
    # Replaces: must name an archive the site has, so those get none.
    case " ${AMINET_NEW:-} " in *" $lhaname "*) lhaold="" ;; esac
}

# The Aminet .readme: machine-readable header (Short/Uploader/Author/Type/
# Version/Architecture/Requires) FIRST, blank line, then the body. LF-only (the
# heredoc emits LF), lines <= 78 cols, version ONLY here (never in the filename).
# Plain-text rendering of THIS release's CHANGELOG section, for the Aminet
# readme (readers there expect the changes in the readme itself, not only
# inside the archive). Markdown in, 78-column plain text out: "### Added"
# becomes a heading, bullets keep their dash, `code` loses its backticks,
# and paragraphs are re-wrapped so no line breaks the Aminet limit.
changelog_section_text() {
    awk -v ver="$VERSION" '
        $0 ~ "^## \\[" ver "\\]" { inside = 1; next }
        inside && /^## \[/          { exit }
        inside                       { print }
    ' "$ROOT_DIR/CHANGELOG.md" | sed 's/`//g' | awk '
        function flush(  n, w, i, line, first) {
            if (buf == "") return
            n = split(buf, w, " ")
            line = indent w[1]        # first line keeps the bullet
            first = 0
            for (i = 2; i <= n; ++i) {
                if (length(line) + 1 + length(w[i]) > 78) {
                    print line
                    line = cont w[i]  # wrapped lines are indented, not bulleted
                } else {
                    line = line " " w[i]
                }
            }
            print line
            buf = ""
        }
        /^### / { flush(); sub(/^### /, ""); print ""; print toupper($0); next }
        /^- /   { flush(); indent = "- "; cont = "  "; buf = substr($0, 3); next }
        /^  /   { buf = buf " " substr($0, 3); next }
        /^$/    { flush(); indent = ""; cont = ""; next }
                { flush(); indent = ""; cont = ""; buf = $0 }
        END     { flush() }
    '
}

# The two readme sections that differ between the window client and the
# plain-68000 text client; every other lane keeps the first text as it was.
aminet_features_text() {
    if [ "$aminet_kind" = "tui68k" ]; then
        cat <<'EOT'
WHAT CAN I DO WITH IT?
----------------------
This archive is the text client alone, for a plain 68000 Amiga (A500,
A600, A1000, A2000, CDTV) with about 2 MB of free fast RAM. The window,
its renderer and the JPEG decoder are left out, so the program loads in
about 1.5 MB.

Read and send messages in private chats, groups and channels, in a
full-screen console with colours and a composer of up to three rows.
Open chats with the F-keys, receive new messages live, reply, edit and
delete your own, see read receipts, download files (/getfile) and send
them (/sendfile). Saved Messages works as a transfer drawer between the
Amiga and your phone or PC.

Limits: 12 chats in the list, 8 messages per page, message text up to
2 KB, downloads in 16 KB pieces. With a 68020 or better take the AmigaOS
3.x archive (Amigram.lha), which has the window, photos and emoji.
EOT
        return
    fi
    cat <<EOT
WHAT CAN I DO WITH IT?
----------------------
Read and send messages in private chats, groups and channels. Reply,
react, forward, edit and delete your own messages, hide other people's
messages in groups. See read receipts and who is typing. Get a pop-up
and a sound for new messages, chosen per chat. Download and send files,
up to $upload_limit on this build. Photos, video frames and link
previews show in the chat, with a viewer for a larger copy; a text-only
mode keeps slow machines quick. Saved Messages works as a transfer
drawer between the Amiga and your phone or PC. The window remembers
where you left it and can open on its own screen.
EOT
}

aminet_start_text() {
    if [ "$aminet_kind" = "tui68k" ]; then
        cat <<'EOT'
GETTING STARTED
---------------
1. Log in once with the normal archive on a 68020 or better, or in an
   emulator: the login's key exchange is too much for a 68000.
2. Copy this drawer to a writable volume of the 68000 machine, then copy
   telegram-auth.bin and the data drawer from the other setup into it.
3. Double-click Amigram-TUI. The account stays usable on both.

telegram-auth.bin holds your login. Never share it: whoever has it is in
your Telegram account. The archive has full manuals in English and
Italian, with notes for the plain 68000.
EOT
        return
    fi
    cat <<EOT
GETTING STARTED
---------------
1. Copy this drawer to a writable volume.
2. Double-click Amigram$aminet_tui_hint.
3. Sign in: phone number, the code Telegram sends, and the cloud
   password if you use 2FA. Next time it opens on your chats.

The login is kept in telegram-auth.bin next to the program. Never share
it: whoever has it is in your Telegram account. The archive has full
manuals in English and Italian, with notes for each platform.
EOT
}

# The rename notice, in the readme's upper-case headings.
aminet_coming_text() {
    [ "$PREV_NAME" != "$APP_NAME" ] || return 0
    printf '%s\n' "$coming_en" | awk 'NR == 1 { print toupper($0); next } NR == 2 { gsub(/./, "-"); print; next } { print }'
}

write_aminet_readme() {
    # $4 = the Aminet path this upload replaces; empty for a first upload,
    # and then the Replaces: line is dropped (an empty one would be refused)
    out=$1; archval=$2; requires=$3; replaces=$4
    coming_block=$(aminet_coming_text)
    [ -z "$coming_block" ] || coming_block="$coming_block

"
    cat > "$out" <<EOF
Short:        $aminet_short
Uploader:     $AMINET_UPLOADER
Author:       $AMINET_AUTHOR
Type:         comm/tcp
Version:      $VERSION
Replaces:     $replaces
Architecture: $archval
Requires:     $requires

WHAT IS THIS?
-------------
$APP_TITLE.

A native Telegram client for the Amiga, written from scratch on
Telegram's own MTProto protocol: no gateway, no proxy, no web page. You
sign in with your normal Telegram account. The cryptography (RSA,
Diffie-Hellman, AES, SHA and the SRP login for 2FA) is built in, so it
needs no MUI, no ixemul.library and no AmiSSL, only the system's
bsdsocket stack.

Unofficial: it uses the Telegram API but is not made by Telegram.

$aminet_programs

$(aminet_features_text)

$(aminet_start_text)

${coming_block}WHAT IS NEW IN $VERSION
$(printf '%*s' $((15 + ${#VERSION})) '' | tr ' ' '-')
$(changelog_section_text)

A COMMUNITY PROJECT
-------------------
Free software under the GNU GPL version 3 or later, and non-commercial.
Bug reports and wishes are welcome: testers on real hardware are what
moves this project forward.

${thanks68k_para_en}The icon is our own design. Carlo Spadoni optimised it for each system,
put it on a standard drawer for the drawer icon, and let me ship his
versions.

  Source and issues:
  $REPO_URL
  Development diary:
  $DIARY_URL
  Support my work on Patreon, or with a one-time tip on Ko-fi
  (voluntary: nothing is needed to use the program):
  $SUPPORT_URL
  $KOFI_URL
EOF
    if [ -z "$replaces" ]; then
        awk 'NR <= 10 && /^Replaces:/ { next } { print }' "$out" > "$out.tmp" && mv "$out.tmp" "$out"
    fi
}

package_one() {
    platform=$1
    binary=$2
    suffix=$3
    expected=$4

    if [ ! -f "$binary" ]; then
        echo "Skipping $platform: binary not found: $binary" >&2
        return 0
    fi

    file_output=$(file "$binary")
    case "$expected" in
        amigaos3)   echo "$file_output" | grep -q "AmigaOS loadseg" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        morphos)    echo "$file_output" | grep -q "ELF 32-bit MSB relocatable, PowerPC" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        amigaos4)   echo "$file_output" | grep -q "ELF 32-bit MSB executable, PowerPC" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        aros-i386)  echo "$file_output" | grep -q "ELF 32-bit LSB relocatable, Intel 80386.*AROS" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        aros-x86_64) echo "$file_output" | grep -q "ELF 64-bit LSB relocatable, x86-64.*AROS" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        # "AROS" in the OS/ABI field is what the plain make output carries; a
        # binary relinked by hand with ld -r -d says SYSV instead, so this also
        # refuses the pre-6fbf9d0 way of building the lane.
        aros-aarch64) echo "$file_output" | grep -q "ELF 64-bit LSB relocatable, ARM aarch64.*AROS" || { echo "Skipping $platform: $file_output" >&2; return 0; } ;;
        *) echo "Unknown expected type: $expected" >&2; exit 1 ;;
    esac

    case "$expected" in
        amigaos4|aros-i386|aros-x86_64|aros-aarch64)
            if ! strings "$binary" | grep -F '$STACK:1048576' >/dev/null; then
                echo "ERROR $platform: binary lacks the 1 MiB AmigaDOS stack cookie." >&2
                exit 1
            fi
            ;;
    esac

    # The 68000 package must hold the low-memory text client: a 68000 build
    # with the window inside, or a tester's diagnostic build, looks the same
    # to `file` and would ship without a word.
    if [ "$suffix" = "amigaos3-68000" ]; then
        if ! strings "$binary" | grep -F '$STACK:393216' >/dev/null; then
            echo "ERROR $platform: not the low-memory build (no 384 KB stack cookie); run scripts/build-68000-lowmem.sh" >&2
            exit 1
        fi
        if strings "$binary" | grep -E 'gui window: open|diag: ' >/dev/null; then
            echo "ERROR $platform: binary carries the GUI or the diagnostic trace; run scripts/build-68000-lowmem.sh" >&2
            exit 1
        fi
    fi

    # Staleness guard: a binary OLDER than the sources means a forgotten rebuild
    # -- exactly how 0.0.2 shipped the previous OS4 binary. Override: ALLOW_STALE=1.
    if [ -z "${ALLOW_STALE:-}" ]; then
        newer=$(find "$ROOT_DIR/core" "$ROOT_DIR/include" "$ROOT_DIR/src" \
                     "$ROOT_DIR/third_party" "$ROOT_DIR/platforms" -type f \
                     -newer "$binary" 2>/dev/null | head -3)
        if [ -n "$newer" ]; then
            echo "ERROR $platform: $binary is OLDER than the sources -- rebuild it first." >&2
            echo "$newer" | sed 's/^/  newer source: /' >&2
            echo "  (set ALLOW_STALE=1 to override)" >&2
            exit 1
        fi
    fi

    drawer="$APP_NAME-$suffix-$DATE_STAMP"
    dest="$PACKAGE_ROOT/$drawer"
    rm -rf "$dest"
    mkdir -p "$dest"

    # Per-lane text and icon policy first: the icon block below reads gui_icon.
    fill_platform_text "$platform"

    cp "$binary" "$dest/$APP_NAME"
    # Self-launching, script-free (papiosaur / Easy2Install suggestion). Two
    # byte-identical flashless icons (DefaultTool = Amigram, Stack 1 MiB)
    # both point at the one binary; the binary reads the WBStartup arg names and
    # picks GUI vs TUI. Amigram.info owns the binary itself -> GUI.
    # Amigram-TUI.info owns a 0-byte marker whose name carries "TUI" -> the
    # binary opens a CON: window and runs the console client. No IconX, no shell
    # scripts.
    # One icon set per platform: AmigaOS 3.x gets an OS3.5 colour icon built
    # from the artwork, the other lanes the files as drawn; every set carries
    # the same three launcher fields (see scripts/build-icons.sh).
    icons="$ROOT_DIR/assets/icons/$(icon_lane "$platform")"
    if [ "$gui_icon" != "0" ]; then
        cp "$icons/$APP_NAME.info" "$dest/$APP_NAME.info"
    fi
    # From 0.0.9 (Michele) the TUI icon ships ONLY on the 68k line, where a
    # console client is what those machines actually want. The PPC and x86
    # packages carry the GUI icon alone; their manuals explain the Shell
    # command for anyone who still wants the text client.
    if [ "$tui_icon" = "1" ]; then
        : > "$dest/$APP_NAME-TUI"
        # The 68000 set asks for the same 384 KB the binary's cookie does, or
        # a Workbench launch would reserve the full megabyte and hand back the
        # memory that build just saved.
        cp "$icons/$APP_NAME-TUI.info" "$dest/$APP_NAME-TUI.info"
    fi
    # The drawer's own icon sits NEXT to the drawer, under the drawer's name.
    cp "$icons/drawer.info" "$PACKAGE_ROOT/$drawer.info"
    mkdir -p "$dest/data"
    # From 0.1 the api pair lives in the binary (include/tg_api_default.h);
    # data/telegram-api.txt is only the user's own override and no longer
    # ships. Check the binary really carries the pair, without printing it:
    # a build made without the local header would leave every user stuck at
    # the login.
    api_id_built=$(sed -n 's/^#define TG_API_DEFAULT_ID "\([0-9]*\)".*/\1/p' "$ROOT_DIR/include/tg_api_default.local.h" 2>/dev/null || true)
    api_hash_built=$(sed -n 's/^#define TG_API_DEFAULT_HASH "\([0-9a-f]*\)".*/\1/p' "$ROOT_DIR/include/tg_api_default.local.h" 2>/dev/null || true)
    # (|| true: under set -e a missing header would end the run in silence
    # here, before the message below could say why.)
    if [ -z "$api_id_built" ] || [ -z "$api_hash_built" ] ||
       ! LC_ALL=C grep -q -a -F "$api_id_built" "$binary" ||
       ! LC_ALL=C grep -q -a -F "$api_hash_built" "$binary"; then
        echo "ERROR $platform: $binary does not carry the built-in api pair" >&2
        echo "  (include/tg_api_default.local.h missing at build time? rebuild)" >&2
        exit 1
    fi
    # Notification sounds for the GUI (0.0.98): short 8SVX samples made by
    # scripts/make-sounds.py, ours and readable by every system's datatype.
    # The 68000 text client only flashes the screen, so it gets none.
    if [ "$gui_icon" != "0" ]; then
        mkdir -p "$dest/sounds"
        cp "$ROOT_DIR"/assets/sounds/*.8svx "$dest/sounds/"
    fi

    write_readme "$dest/README.txt" "$platform"
    write_manual_en "$dest/Manual-EN.txt" "$platform"
    write_manual_it "$dest/Manuale-IT.txt" "$platform"
    cp "$ROOT_DIR/LICENSE" "$dest/LICENSE"
    # The emoji sheet is derived from Noto Emoji (OFL 1.1): the licence and the
    # notice travel with every package, next to our own MIT text, so the
    # credit line in the readme points at something the user actually has.
    cp "$ROOT_DIR/third_party/noto-emoji/LICENSE" "$dest/LICENSE-NotoEmoji.txt"
    cp "$ROOT_DIR/third_party/noto-emoji/NOTICE.txt" "$dest/NOTICE-NotoEmoji.txt"
    # Full changelog in every package (release rule since 0.0.8): the repo
    # CHANGELOG.md is the single source; refuse to package a release whose
    # version is not in it yet (the Unreleased section must be promoted).
    if ! grep -q "^## \[$VERSION\]" "$ROOT_DIR/CHANGELOG.md"; then
        echo "ERROR: CHANGELOG.md has no '## [$VERSION]' section - promote Unreleased first" >&2
        exit 1
    fi
    cp "$ROOT_DIR/CHANGELOG.md" "$dest/CHANGELOG.txt"

    if command -v zip >/dev/null 2>&1; then
        (cd "$PACKAGE_ROOT" && rm -f "$drawer.zip" && zip -qr "$drawer.zip" "$drawer" "$drawer.info")
        # Post-zip guard: the archive must hold THIS binary and no private file.
        unzip -p "$PACKAGE_ROOT/$drawer.zip" "*/$APP_NAME" > "$PACKAGE_ROOT/.zipbin" 2>/dev/null
        if [ "$(md5of "$PACKAGE_ROOT/.zipbin")" != "$(md5of "$binary")" ]; then
            rm -f "$PACKAGE_ROOT/.zipbin"
            echo "ERROR $platform: packaged binary != built binary ($binary)" >&2; exit 1
        fi
        rm -f "$PACKAGE_ROOT/.zipbin"
        if unzip -l "$PACKAGE_ROOT/$drawer.zip" | grep -qiE "telegram-(auth|peers|seed|password|token|api)|phone-code-hash"; then
            echo "ERROR $platform: SESSION FILE LEAK in $drawer.zip" >&2; exit 1
        fi
        echo "$PACKAGE_ROOT/$drawer.zip  [bin $(md5of "$binary" | cut -c1-8)]"
    else
        echo "$dest"
    fi

    # --- Aminet: Amigram<suffix>.lha + matching .readme ----------------
    # Reuses the assembled drawer ($dest, incl. LICENSE) but under a clean
    # top-level name ($AMINET_DRAWER) so unpacking yields one tidy directory.
    # Each lane has its own Aminet name, looked up by the package suffix: the
    # 68000 package (an Aminet upload from 0.0.96) once took the AmigaOS 3.x
    # name and would have replaced that lha when both lanes ran in one call
    # (seen in the 0.0.94 test round, before it was kept off Aminet).
    if [ "$AMINET" = "1" ]; then
        if [ ! -x "$LHA_BIN" ]; then
            echo "ERROR $platform: LhA encoder not found at $LHA_BIN (build jca02266/lha or set LHA_BIN=, or AMINET=0)" >&2
            exit 1
        fi
        aminet_meta "$suffix"
        amiwork="$AMINET_ROOT/$AMINET_DRAWER"
        lhafile="$AMINET_ROOT/$lhaname.lha"
        rm -rf "$amiwork" "$amiwork.info"; mkdir -p "$amiwork"
        cp -R "$dest"/* "$amiwork"/ # -R: the package now contains data/
        cp "$icons/drawer.info" "$amiwork.info" # the drawer's icon, beside it
        rm -f "$lhafile"
        ( cd "$AMINET_ROOT" && "$LHA_BIN" a "$lhaname.lha" "$AMINET_DRAWER" "$AMINET_DRAWER.info" >/dev/null )
        rm -rf "$amiwork" "$amiwork.info"
        # Verify it extracts under UNIX lha (Aminet's own checklist requirement).
        if ! "$LHA_BIN" t "$lhafile" >/dev/null 2>&1; then
            echo "ERROR $platform: $lhafile fails lha integrity test" >&2; exit 1
        fi
        # Same guards as the zip path: no leaked session file; the packaged
        # binary is byte-identical to the built one.
        if "$LHA_BIN" l "$lhafile" | grep -qiE "telegram-(auth|peers|seed|password|token|api)|phone-code-hash"; then
            echo "ERROR $platform: SESSION FILE LEAK in $lhafile" >&2; exit 1
        fi
        lhatmp=$(mktemp -d)
        ( cd "$lhatmp" && "$LHA_BIN" xq "$lhafile" >/dev/null 2>&1 )
        if [ "$(md5of "$lhatmp/$AMINET_DRAWER/$APP_NAME")" != "$(md5of "$binary")" ]; then
            rm -rf "$lhatmp"
            echo "ERROR $platform: lha binary != built binary ($binary)" >&2; exit 1
        fi
        # Architecture guard: the binary INSIDE the lha must be the right
        # kind for the lane (an OS4 ELF inside the OS3 package shipped in
        # 0.0.7: LoadSeg fails and Workbench says "unable to open tool").
        case "$archtag" in
            m68k-amigaos) arch_want="loadseg" ;;
            ppc-morphos|ppc-amigaos) arch_want="PowerPC" ;;
            i386-aros) arch_want="Intel 80386" ;;
            x86_64-aros) arch_want="x86-64" ;;
            aarch64-aros) arch_want="aarch64" ;;
            *) arch_want="" ;;
        esac
        if [ -n "$arch_want" ] && \
           ! file -b "$lhatmp/$AMINET_DRAWER/$APP_NAME" | grep -q "$arch_want"; then
            rm -rf "$lhatmp"
            echo "ERROR $platform: wrong-arch binary inside $lhafile (want $arch_want)" >&2
            exit 1
        fi
        rm -rf "$lhatmp"
        write_aminet_readme "$AMINET_ROOT/$lhaname.readme" "$archval" "$requires" "$lhaold"
        if [ "$aminet_upload" = 1 ]; then
            echo "$lhafile  +  $lhaname.readme  [Architecture: $archval]"
        else
            mkdir -p "$AROSARCHIVES_ROOT"
            mv "$lhafile" "$AMINET_ROOT/$lhaname.readme" "$AROSARCHIVES_ROOT/"
            echo "$AROSARCHIVES_ROOT/$lhaname.lha  (staged for The AROS Archives only, no Aminet upload)"
        fi
    fi
}

package_one "AmigaOS 3.x" "$AMIGAOS3_BINARY" "amigaos3" "amigaos3"
package_one "MorphOS" "$MORPHOS_BINARY" "morphos" "morphos"
package_one "AmigaOS 4.x" "$AMIGAOS4_BINARY" "amigaos4" "amigaos4"
package_one "AROS i386 ABIv0" "$AROS_I386_BINARY" "aros-i386" "aros-i386"
package_one "AROS x86_64" "$AROS_X86_64_BINARY" "aros-x86_64" "aros-x86_64"
package_one "AROS aarch64" "$AROS_AARCH64_BINARY" "aros-aarch64" "aros-aarch64"
package_one "AmigaOS 3.x (68000)" "$AMIGAOS3_68000_BINARY" "amigaos3-68000" "amigaos3"

# --- checksums ---------------------------------------------------------------
( cd "$PACKAGE_ROOT" && ls "$APP_NAME"-*-"$DATE_STAMP".zip >/dev/null 2>&1 &&
  sha_cmd "$APP_NAME"-*-"$DATE_STAMP".zip > SHA256SUMS-github.txt &&
  echo "$PACKAGE_ROOT/SHA256SUMS-github.txt" ) || true
if [ "$AMINET" = "1" ] && ls "$AMINET_ROOT"/"$APP_NAME"*.lha >/dev/null 2>&1; then
    ( cd "$AMINET_ROOT" && sha_cmd "$APP_NAME"*.lha "$APP_NAME"*.readme > SHA256SUMS-aminet.txt )
    echo
    echo "Aminet artifacts ready in: $AMINET_ROOT  (version $VERSION, Type comm/tcp)"
    aminet_pairs=$(ls "$AMINET_ROOT"/"$APP_NAME"*.lha | wc -l | tr -d ' ')
    echo "Upload (Michele): FTP main.aminet.net -> cd /new -> binary -> put each"
    echo "  $APP_NAME[<-suffix>].lha AND matching .readme ($aminet_pairs + $aminet_pairs files)."
    echo "  Web form may be back at https://aminet.net/upload. See memory/aminet-publishing.md"
fi

# --- OS4Depot (fixed release channel since 0.0.6) ----------------------------
# Anonymous FTP to os4depot.net /upload: the archive FIRST, the readme LAST
# (their processor keys off the readme). Readme = THEIR header format
# (name:/description:/.../hend:) + our Aminet body. Queue shows on
# https://os4depot.net/index.php?function=uploads after ~15 min.
# In its OWN subdirectory: "amigram.lha" next to "Amigram.lha"
# SILENTLY OVERWRITES the Aminet OS3 package on a case-insensitive macOS
# filesystem -- that shipped an OS4 ELF to every Aminet OS3 downloader in
# 0.0.7 (field report 2026-07-26, "unable to open tool").
OS4DEPOT_ROOT=${OS4DEPOT_ROOT:-"$PACKAGE_ROOT/os4depot"}
if [ "$AMINET" = "1" ] && [ -f "$AMINET_ROOT/$APP_NAME-OS4.lha" ]; then
    mkdir -p "$OS4DEPOT_ROOT"
    cp "$AMINET_ROOT/$APP_NAME-OS4.lha" "$OS4DEPOT_ROOT/$app_lower.lha"
    {
        printf 'name:%s\n' "$APP_NAME"
        printf 'description:%s\n' "${TAGLINE:-Native MTProto Telegram chat client}"
        printf 'version:%s\n' "$VERSION"
        printf 'author:Michele Dipace\n'
        printf 'submitter:Michele Dipace\n'
        printf 'email:michele.dipace@kaffeine.net\n'
        printf 'url:%s\n' "$REPO_URL"
        printf 'donateurl:%s\n' "$KOFI_URL"
        printf 'category:network/chat\n'
        printf 'requirements:AmigaOS 4.x with its TCP/IP stack\n'
        # license must be one of THEIR values (help/help_submit.txt):
        # Other APL BSD Commercial Emailware Freeware GPL LGPL MPL
        # "Public domain" Shareware -- GPL from 0.1.x (MIT up to 0.1 was Other).
        # replaces is REQUIRED when updating an existing entry (0.0.8 failed
        # validation on both, 2026-07-31; 0.0.7 passed only because it was new).
        printf 'license:GPL\n'
        printf 'replaces:network/chat/%s.lha\n' "$prev_lower"
        printf 'distribute:yes\n'
        printf 'minosversion:4.0\n'
        printf 'hend:\n\n'
        # body: the Aminet OS4 readme minus its header block
        awk 'flip { print } /^$/ && !flip { flip = 1 }' \
            "$AMINET_ROOT/$APP_NAME-OS4.readme"
    } > "$OS4DEPOT_ROOT/${app_lower}_lha.readme"
    rm -f "$AMINET_ROOT/telegramamiga_lha.readme" # pre-0.0.8 location, stale
    echo
    echo "OS4Depot pair ready in: $OS4DEPOT_ROOT ($app_lower.lha + readme)"
    echo "Upload: curl -T $app_lower.lha ftp://os4depot.net/upload/ --user anonymous:"
    echo "        then the readme (LAST). Queue: os4depot.net ?function=uploads"
fi

# --- The AROS Archives (fixed release channel since 0.0.92) ------------------
# archives.arosworld.org (ex archives.aros-exec.org) runs the OS4Depot software,
# so the readme is the same header format (name:/.../hend:) plus our Aminet
# body, minus minosversion. Naming is theirs: lowercase, platform in the file
# name, and "-v11" marks the x86_64 ABIv11 build. Web form only, no FTP:
# index.php?function=submit (no captcha; fields f_name f_description f_version
# f_author f_submitter f_email f_url f_donateurl f_category f_userfile
# f_replaces f_requirements f_license f_distribute f_text=body). A replace of
# a file we did not upload ourselves waits for the original uploader's
# confirmation.
# Own subdirectory for the same reason as OS4Depot: never a lowercase twin of
# an Aminet archive in the same directory on a case-insensitive filesystem.
# (AROSARCHIVES_ROOT is set at the top, next to AMINET_ROOT.)
write_arosarchives_pair() {
    # $1 Aminet base (Amigram-AROS / -AROS64), $2 their file name,
    # $3 requirements text
    # $4 "new" = first upload of this file: the form refuses a replaces: line
    # for a file the site does not have yet; drop the argument from the next
    # release on, when the site wants replaces:network/chat/<name>.lha.
    aa_src="$AMINET_ROOT/$1.lha"; aa_readme="$AMINET_ROOT/$1.readme"
    if [ ! -f "$aa_src" ]; then aa_src="$AROSARCHIVES_ROOT/$1.lha"; aa_readme="$AROSARCHIVES_ROOT/$1.readme"; fi
    [ -f "$aa_src" ] || return 0
    cp "$aa_src" "$AROSARCHIVES_ROOT/$2.lha"
    {
        printf 'name:%s\n' "$APP_NAME"
        printf 'description:%s\n' "${TAGLINE:-Native MTProto Telegram chat client}"
        printf 'version:%s\n' "$VERSION"
        printf 'author:Michele Dipace\n'
        printf 'submitter:Michele Dipace\n'
        printf 'email:michele.dipace@kaffeine.net\n'
        printf 'url:%s\n' "$REPO_URL"
        printf 'donateurl:%s\n' "$KOFI_URL"
        printf 'category:network/chat\n'
        if [ "${4:-}" != "new" ]; then printf 'replaces:network/chat/%s.lha\n' "$prev_lower${2#"$app_lower"}"; fi
        printf 'requirements:%s\n' "$3"
        printf 'license:GPL\n'
        printf 'distribute:yes\n'
        printf 'hend:\n\n'
        awk 'flip { print } /^$/ && !flip { flip = 1 }' "$aa_readme"
    } > "$AROSARCHIVES_ROOT/${2}_lha.readme"
}
if [ "$AMINET" = "1" ] && { [ -f "$AMINET_ROOT/$APP_NAME-AROS.lha" ] || [ -f "$AMINET_ROOT/$APP_NAME-AROS64.lha" ] || [ -f "$AMINET_ROOT/$APP_NAME-ARM64.lha" ]; }; then
    mkdir -p "$AROSARCHIVES_ROOT"
    write_arosarchives_pair "$APP_NAME-AROS" "$app_lower.i386-aros" \
        "AROS i386 ABIv0 (AROS One, Icaros) with its TCP/IP stack"
    write_arosarchives_pair "$APP_NAME-AROS64" "$app_lower.x86_64-aros-v11" \
        "AROS x86_64 ABIv11 (AROS One x64) with its TCP/IP stack"
    # Sixth package since 0.0.94: the site already files ARM64 uploads under
    # the aarch64-aros suffix (pintp, commander_keen_4, super_trevor_land).
    # 0.0.94 was its first upload (with "new"); from 0.0.95 on it replaces
    # network/chat/telegramamiga.aarch64-aros.lha like the other two.
    write_arosarchives_pair "$APP_NAME-ARM64" "$app_lower.aarch64-aros" \
        "AROS aarch64 ABIv1 (Raspberry Pi 4, 400 and 5; native image of 2026-08-22 or newer) with its TCP/IP stack"
    echo
    echo "AROS Archives pairs ready in: $AROSARCHIVES_ROOT (up to 3 x .lha + _lha.readme)"
    echo "Submit: https://archives.arosworld.org/index.php?function=submit (web form,"
    echo "        no FTP); queue: index.php?function=uploads"
    echo "        Set f_passphrase on OUR uploads (value in SECRETS, never here):"
    echo "        without it every replace waits for the previous uploader's ok."
fi

# --- MorphOS-Storage (fixed release channel since 0.0.6) ---------------------
# Web form only (https://www.morphos-storage.net/?page=submit), no account but
# a CAPTCHA: Michele submits. Hand him Amigram-MOS.lha + its readme and
# the description block; agent prepares, human clicks.
if [ "$AMINET" = "1" ] && [ -f "$AMINET_ROOT/$APP_NAME-MOS.lha" ]; then
    echo
    echo "MorphOS-Storage (Michele, web form + captcha):"
    echo "  https://www.morphos-storage.net/?page=submit"
    echo "  archive: $AMINET_ROOT/$APP_NAME-MOS.lha"
    echo "  readme:  $AMINET_ROOT/$APP_NAME-MOS.readme"
    echo "  name: $APP_NAME  version: $VERSION  title: $APP_TITLE"
fi
