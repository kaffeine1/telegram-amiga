#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Build the human-facing release packages for Telegram Amiga.
# Version comes from include/tg_version.h (override with VERSION=... if needed).
#
# Each package contains ONLY the program (one binary), its self-launching icon
# (plus the TelegramAmiga-TUI console icon on the 68k line only; no scripts),
# the PUBLIC Telegram API app credentials and per-architecture IT/EN manuals.
# No user session is ever bundled: telegram-auth.bin and the peer cache are
# created locally by the user on first login (see the manual).

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PACKAGE_ROOT=${PACKAGE_ROOT:-"$ROOT_DIR/build/human-releases"}
DATE_STAMP=${DATE_STAMP:-$(date +%Y%m%d)}
COMMIT_ID=${COMMIT_ID:-$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)}
# Single source of truth: the version the binary itself reports (About box /
# startup banner) comes from include/tg_version.h, so the package always matches.
VERSION=${VERSION:-$(sed -n 's/.*define TG_VERSION "\([^"]*\)".*/\1/p' "$ROOT_DIR/include/tg_version.h" 2>/dev/null)}
VERSION=${VERSION:-0.0.0}

md5of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | awk '{print $1}'; fi; }

AMIGAOS3_BINARY=${AMIGAOS3_BINARY:-"$ROOT_DIR/build/amigaos3-clib2/TelegramAmiga"}
# Plain-68000 variant (A500/A600/A1000/CDTV class): built with M68K_CPU=68000
# and the LOWMEM caps. Packaged only when that binary exists.
AMIGAOS3_68000_BINARY=${AMIGAOS3_68000_BINARY:-"$ROOT_DIR/build/amigaos3-68000/TelegramAmiga"}
MORPHOS_BINARY=${MORPHOS_BINARY:-"$ROOT_DIR/build/morphos-cross/TelegramAmiga"}
AMIGAOS4_BINARY=${AMIGAOS4_BINARY:-"$ROOT_DIR/build/amigaos4/TelegramAmiga"}
AROS_I386_BINARY=${AROS_I386_BINARY:-"$ROOT_DIR/build/aros-i386-abiv0/TelegramAmiga"}
AROS_X86_64_BINARY=${AROS_X86_64_BINARY:-"$ROOT_DIR/build/aros-x86_64/TelegramAmiga"}

# --- Aminet artifacts (.lha + .readme) --------------------------------------
# Aminet requires a real LhA ENCODER (the Mac's lhasa is extract-only); we use
# Koji Arai's lha. Set AMINET=0 to skip the Aminet pass. See the verified
# procedure in memory/aminet-publishing.md (Type comm/tcp, 5 per-arch uploads,
# version in the .readme NOT the filename, FTP upload to /new by Michele).
AMINET=${AMINET:-1}
LHA_BIN=${LHA_BIN:-"$HOME/amiga-dev/tools/lha-src/src/lha"}
AMINET_ROOT=${AMINET_ROOT:-"$PACKAGE_ROOT/aminet"}
AMINET_BASE=${AMINET_BASE:-tgamiga}      # short base: keeps every name <= 30 chars incl. suffix
AMINET_DRAWER=${AMINET_DRAWER:-TelegramAmiga}
AMINET_UPLOADER=${AMINET_UPLOADER:-"michele.dipace@kaffeine.net (Michele Dipace)"}
AMINET_AUTHOR=${AMINET_AUTHOR:-"Michele Dipace <michele.dipace@kaffeine.net>"}
DIARY_URL="https://androidlab.it/en/telegram-amiga-mtproto-client-development-diary/"
REPO_URL="https://github.com/kaffeine1/telegram-amiga"

sha_cmd() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi; }

mkdir -p "$PACKAGE_ROOT"
rm -f "$PACKAGE_ROOT"/Telegram-*-"$DATE_STAMP".zip
rm -rf "$PACKAGE_ROOT"/Telegram-*-"$DATE_STAMP"
if [ "$AMINET" = "1" ]; then rm -rf "$AMINET_ROOT"; mkdir -p "$AMINET_ROOT"; fi

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
    gui_entry_en="- TelegramAmiga -- the native Intuition/GadTools GUI (chat list + conversation).
  Double-click it: it starts the GUI directly, with no flashing console window.
"
    gui_entry_it="- TelegramAmiga -- la GUI nativa Intuition/GadTools (lista chat + conversazione).
  Doppio click: avvia direttamente la GUI, senza finestra console che lampeggia.
"
    if [ "$tui_icon" = "1" ]; then
        launchers_title_en="Two launchers
-------------"
        launchers_title_it="I due launcher
--------------"
        tui_entry_en="- TelegramAmiga-TUI -- a full-screen text/console client for low-end or mouse-less
  setups. Both share the same login and the same saved session."
        tui_entry_it="- TelegramAmiga-TUI -- un client testuale a schermo intero per macchine leggere o
  senza mouse. Condividono lo stesso login e la stessa sessione salvata."
        first_start_en="1. Unpack the drawer and double-click TelegramAmiga (or TelegramAmiga-TUI).
2. If there is no saved login, a login panel appears. Enter your phone number
   in full international form (for example +39 333 1234567), then the code
   Telegram sends you. That code usually does NOT arrive by SMS: if you are
   signed in to Telegram anywhere else (phone, PC, web), it is delivered
   INSIDE Telegram, as a message from the official Telegram service chat.
   The client tells you on screen where it was sent.
   If your account has a cloud password (2FA), type it on the masked screen
   (if you do NOT have one, just press Enter to continue).
3. The client logs in and writes telegram-auth.bin in this drawer. After that
   it reuses the saved login -- you are not asked for the phone/code again."
        first_start_it="1. Scompatta il drawer e fai doppio click su TelegramAmiga (o TelegramAmiga-TUI).
2. Se non c'e' un login salvato, compare il pannello di accesso. Inserisci il
   numero di telefono in formato internazionale completo (es. +39 333 1234567),
   poi il codice che Telegram ti invia. Quel codice di solito NON arriva via
   SMS: se sei collegato a Telegram da qualche altra parte (telefono, PC,
   web), te lo consegna DENTRO Telegram, come messaggio della chat
   ufficiale Telegram. Il client ti dice a schermo dove e' stato spedito.
   Se il tuo account ha una password cloud (2FA), digitala sulla schermata
   mascherata (se NON ce l'hai, premi Invio).
3. Il client accede e scrive telegram-auth.bin in questo drawer. Da li' in poi
   riusa il login salvato -- non ti richiede piu' telefono/codice."
        readme_programs="  TelegramAmiga  - graphical (Intuition), with scrollbars + mouse.
  TelegramAmiga-TUI  - text-mode / console."
        readme_start="Quick start: copy this drawer to a WRITABLE volume, then double-click
TelegramAmiga (or TelegramAmiga-TUI). First run signs you in (phone -> code -> 2FA)."
        aminet_programs="Two programs share one engine and one saved login:

  TelegramAmiga  - the native Intuition/GadTools GUI: chat list with real
                 profile-picture avatars, message bubbles, scrollbars,
                 mouse wheel, context menus.
  TelegramAmiga-TUI  - the text/console client, at home on a 68030 with a
                 serial console or an ssh session."
        if [ "$gui_icon" = "0" ]; then
            launchers_title_en="One launcher
------------"
            launchers_title_it="Un solo launcher
----------------"
            tui_entry_en="- TelegramAmiga-TUI -- the full-screen text/console client. This package
  ships ONLY that icon: the Intuition GUI is too heavy for a plain 68000,
  so the text client is the way in here.
- From a Shell instead of the icon: TelegramAmiga-TUI is a 0-byte marker,
  not a program, so running it gives \"file is not executable\". Run the
  binary with the same arguments the icon uses, from inside the drawer:
      stack 393216
      TelegramAmiga --mtproto-start-file data/telegram-api.txt
        telegram-auth.bin data/phone-code-hash.txt data/telegram-peers.txt
  (the command is one single line). The stack line is needed once per
  Shell: a Shell hands out 4096 bytes by default, and the program says so
  rather than crashing. Add \"Protect TelegramAmiga +e\" once if the Shell
  refuses to run it."
            tui_entry_it="- TelegramAmiga-TUI -- il client testuale a schermo intero. Questo
  pacchetto porta SOLO quell'icona: la GUI Intuition e' troppo pesante per
  un 68000 liscio, qui si usa il client testuale.
- Da Shell invece che dall'icona: TelegramAmiga-TUI e' un marker da 0 byte,
  non un programma, quindi lanciarlo da' \"file is not executable\". Esegui
  il binario con gli stessi argomenti che usa l'icona, dentro il drawer:
      stack 393216
      TelegramAmiga --mtproto-start-file data/telegram-api.txt
        telegram-auth.bin data/phone-code-hash.txt data/telegram-peers.txt
  (il comando e' tutto su una riga sola). La riga stack serve una volta per
  Shell: una Shell ne assegna 4096 byte di default, e il programma te lo
  dice invece di crashare. Se la Shell si rifiuta di eseguirlo, dai una
  volta \"Protect TelegramAmiga +e\"."
            first_start_en="1. On a FASTER Amiga (68020 or better) or in an emulator (WinUAE,
   FS-UAE, vAmiga), install the standard AmigaOS 3.x package and log in
   there: phone number, the code Telegram sends you, and the cloud
   password (2FA) if your account has one.
2. Copy telegram-auth.bin from that drawer into THIS drawer, next to
   TelegramAmiga-TUI. Copy data/telegram-peers.txt too if you want your
   chat list ready. NEVER share telegram-auth.bin with anyone.
3. Unpack this drawer on a WRITABLE volume and double-click
   TelegramAmiga-TUI: it reuses that login and goes straight to the chats.
   The same account stays usable on both machines.

If your account has Two-Step Verification, pick a machine that is genuinely
fast for step 1: a 68030 or better, an emulator with JIT, or a PPC/AROS
system. Checking the cloud password derives a key with 100000 iterations of
SHA-512, about forty minutes on a stock 68020 -- long enough for Telegram to
expire the challenge, so the login never completes there. Turning Two-Step
Verification off for the few minutes of the login also works.

Why not log in here? The first login runs a Diffie-Hellman key exchange:
it is the heaviest thing this program does, and a plain 68000 cannot
finish it comfortably. The program says so and lets you try anyway, but
the route above is the supported one."
            first_start_it="1. Su un Amiga PIU' POTENTE (68020 o superiore) o in un emulatore
   (WinUAE, FS-UAE, vAmiga), installa il pacchetto AmigaOS 3.x standard e
   accedi li': numero di telefono, codice che Telegram ti invia e password
   cloud (2FA) se il tuo account ne ha una.
2. Copia telegram-auth.bin da quel drawer dentro QUESTO drawer, accanto a
   TelegramAmiga-TUI. Copia anche data/telegram-peers.txt se vuoi la lista
   chat gia' pronta. NON condividere MAI telegram-auth.bin con nessuno.
3. Scompatta questo drawer su un volume SCRIVIBILE e fai doppio click su
   TelegramAmiga-TUI: riusa quel login e va dritto alle chat. Lo stesso
   account resta utilizzabile su entrambe le macchine.

Se il tuo account ha la verifica in due passaggi, per il punto 1 scegli una
macchina davvero veloce: un 68030 o superiore, un emulatore con JIT oppure
un sistema PPC/AROS. Il controllo della password cloud deriva una chiave con
100000 iterazioni di SHA-512, circa quaranta minuti su un 68020 liscio: il
tempo che Telegram faccia scadere la richiesta, e li' l'accesso non si
completa. In alternativa disattiva la verifica in due passaggi per i pochi
minuti dell'accesso.

Perche' non accedere qui? Il primo login esegue uno scambio di chiavi
Diffie-Hellman: e' la cosa piu' pesante che il programma faccia, e un
68000 liscio non riesce a portarla a termine comodamente. Il programma te
lo dice e ti lascia comunque provare, ma la via sopra e' quella
supportata."
            readme_programs="  TelegramAmiga-TUI  - the text/console client (the only icon here:
                       the GUI needs a 68020 or better)."
            readme_start="Quick start: copy this drawer to a WRITABLE volume. FIRST log in on a
faster Amiga or an emulator and copy telegram-auth.bin here (see below),
then double-click TelegramAmiga-TUI."
            gui_entry_en=""
            gui_entry_it=""
            aminet_programs="One text/console client, built for the plain 68000:

  TelegramAmiga-TUI  - the full-screen text client. The Intuition GUI
                 needs a 68020 or better, so this package ships the
                 console face alone."
        fi
    else
        launchers_title_en="One icon, two clients
---------------------"
        launchers_title_it="Un'icona, due client
--------------------"
        tui_entry_en="- The text/console client is the SAME program: this package ships the GUI
  icon only, because that is what these machines want. If you prefer the
  full-screen text client, start it from a Shell, from inside the drawer:
      TelegramAmiga --mtproto-start-file data/telegram-api.txt telegram-auth.bin
        data/phone-code-hash.txt data/telegram-peers.txt
  (one single line). Add \"Protect TelegramAmiga +e\" once if the Shell
  refuses to run it. Both share the same login and the same saved session."
        tui_entry_it="- Il client testuale e' lo STESSO programma: questo pacchetto porta solo
  l'icona della GUI, perche' e' quella che serve su queste macchine. Se
  preferisci il client testuale a schermo intero, avvialo da Shell dentro
  il drawer:
      TelegramAmiga --mtproto-start-file data/telegram-api.txt telegram-auth.bin
        data/phone-code-hash.txt data/telegram-peers.txt
  (tutto su una riga sola). Se la Shell si rifiuta di eseguirlo, dai una
  volta \"Protect TelegramAmiga +e\". Login e sessione salvata sono gli stessi."
        first_start_en="1. Unpack the drawer and double-click TelegramAmiga.
2. If there is no saved login, a login panel appears. Enter your phone number
   in full international form (for example +39 333 1234567), then the code
   Telegram sends you. That code usually does NOT arrive by SMS: if you are
   signed in to Telegram anywhere else (phone, PC, web), it is delivered
   INSIDE Telegram, as a message from the official Telegram service chat.
   The client tells you on screen where it was sent.
   If your account has a cloud password (2FA), type it on the masked screen
   (if you do NOT have one, just press Enter to continue).
3. The client logs in and writes telegram-auth.bin in this drawer. After that
   it reuses the saved login -- you are not asked for the phone/code again."
        first_start_it="1. Scompatta il drawer e fai doppio click su TelegramAmiga.
2. Se non c'e' un login salvato, compare il pannello di accesso. Inserisci il
   numero di telefono in formato internazionale completo (es. +39 333 1234567),
   poi il codice che Telegram ti invia. Quel codice di solito NON arriva via
   SMS: se sei collegato a Telegram da qualche altra parte (telefono, PC,
   web), te lo consegna DENTRO Telegram, come messaggio della chat
   ufficiale Telegram. Il client ti dice a schermo dove e' stato spedito.
   Se il tuo account ha una password cloud (2FA), digitala sulla schermata
   mascherata (se NON ce l'hai, premi Invio).
3. Il client accede e scrive telegram-auth.bin in questo drawer. Da li' in poi
   riusa il login salvato -- non ti richiede piu' telefono/codice."
        readme_programs="  TelegramAmiga  - graphical (Intuition), with scrollbars + mouse.
                   The same program also runs the text/console client from a
                   Shell (see the manual)."
        readme_start="Quick start: copy this drawer to a WRITABLE volume, then double-click
TelegramAmiga. First run signs you in (phone -> code -> 2FA)."
        aminet_programs="One program, two faces, one engine and one saved login:

  TelegramAmiga  - the native Intuition/GadTools GUI: chat list with real
                 profile-picture avatars, message bubbles, scrollbars,
                 mouse wheel, context menus. This package ships its icon.
                 The same binary also runs a full-screen text/console client
                 from a Shell -- the manual gives the command line."
    fi
    case "$1" in
    "AmigaOS 3.x (68000)")
        upload_limit="125 MiB"
        req_en="- A plain 68000 Amiga (A500 / A600 / A1000 / A2000 / CDTV) with AmigaOS
  2.x/3.x, a TCP/IP stack providing bsdsocket.library (Roadshow, AmiTCP,
  Miami) and an internet connection.
- About 2 MB of free FAST RAM. This build is trimmed for such machines: the
  graphical client is not compiled in at all, and the chat list, history
  pages and message text are smaller than on the other packages.
- IMPORTANT: sign in ONCE on a faster Amiga or an emulator and copy
  telegram-auth.bin here (see \"First start\" below). The first login runs a
  Diffie-Hellman exchange that a 68000 cannot complete comfortably."
        notes_en="Notes for the plain 68000
-------------------------
- The graphical client is NOT in this package at all -- not merely without
  an icon: window, renderer and JPEG decoder are left out of the build, which
  is where most of the memory saving comes from. Intuition drawing plus photo
  decoding needs a 68020 or better anyway; use the standard AmigaOS 3.x
  package on such a machine.
- Preparing the login elsewhere: install the normal package on a 68020+
  Amiga or in WinUAE/FS-UAE/vAmiga, log in, then copy telegram-auth.bin (and
  data/telegram-peers.txt for the chat list) into this drawer. The same
  account can stay logged in on both machines.
- Limits of this profile: 12 chats in the list, 8 messages per page, message
  text up to 2 KB, downloads in 16 KB chunks. Everything else works as on any
  other Amiga: send and receive, files, replies, edits, read receipts.
- Long messages wrap by words; the composer grows to three rows. Quit with
  /quit, Ctrl+C or the close gadget, then RETURN dismisses the window."
        req_it="- Un Amiga con 68000 liscio (A500 / A600 / A1000 / A2000 / CDTV) con
  AmigaOS 2.x/3.x, uno stack TCP/IP che fornisca bsdsocket.library
  (Roadshow, AmiTCP, Miami) e una connessione internet.
- Circa 2 MB di FAST RAM libera. Questa build e' ridotta apposta per queste
  macchine: il client grafico non e' proprio compilato dentro, e lista chat,
  pagine di storia e testo dei messaggi sono piu' piccoli che negli altri
  pacchetti.
- IMPORTANTE: fai l'accesso UNA VOLTA su un Amiga piu' potente o in un
  emulatore e copia qui telegram-auth.bin (vedi \"Primo avvio\"). Il primo
  login esegue uno scambio di chiavi Diffie-Hellman che un 68000 non riesce
  a portare a termine comodamente."
        notes_it="Note per il 68000 liscio
------------------------
- Il client grafico NON e' in questo pacchetto, e non solo come icona:
  finestra, renderer e decoder JPEG sono esclusi dalla build, ed e' da li'
  che arriva gran parte del risparmio di memoria. Il disegno Intuition e la
  decodifica delle foto vogliono comunque un 68020 o superiore: su quelle
  macchine usa il pacchetto AmigaOS 3.x standard.
- Preparare il login altrove: installa il pacchetto normale su un Amiga
  68020+ o in WinUAE/FS-UAE/vAmiga, accedi, poi copia telegram-auth.bin (e
  data/telegram-peers.txt per avere la lista chat) dentro questo drawer. Lo
  stesso account puo' restare connesso su entrambe le macchine.
- Limiti di questo profilo: 12 chat in lista, 8 messaggi per pagina, testo
  fino a 2 KB per messaggio, download a blocchi da 16 KB. Il resto funziona
  come su qualsiasi Amiga: invio e ricezione, file, risposte, modifiche,
  conferme di lettura.
- I messaggi lunghi vanno a capo per parole e il composer cresce fino a tre
  righe. Per uscire: /quit, Ctrl+C o il gadget di chiusura, poi RETURN
  congeda la finestra."
        ;;
    "AmigaOS 3.x")
        # = 32 KiB part x 4000 parts. KEEP IN SYNC with TG_GUI_DL_CHUNK
        # (core/tg_mtproto_probe.c): this said "31 MiB" long after the m68k
        # chunk grew, so the 0.0.7 OS3 package shipped manuals understating
        # the real limit (the binary itself computes and reports 125).
        upload_limit="125 MiB"
        req_en="- AmigaOS 3.x (3.1 / 3.1.4 / 3.2) with a TCP/IP stack (Roadshow, AmiTCP,
  Miami/MiamiDx) providing bsdsocket.library, and an internet connection.
- A 68020 or better CPU (this build uses the 68020 32x32 multiply and will
  NOT run on a plain 68000). Tested on 68080/Vampire; 020/030/040/060 welcome.
- A few MB of free RAM. No ixemul.library and no AmiSSL are needed."
        notes_en="Notes for AmigaOS 3.x
---------------------
- Photo messages show an instant blurred preview from the message itself, then
  refine from a small bounded download. Decoded pixels are cached for reopen.
- The cloud-password (2FA) step is heavy on a 68k (PBKDF2) and can take a while.
- First login: the DH key exchange is heavy on a 68k, so the first start
  takes a while (it happens once -- the session is saved afterwards).
- Emoji are drawn as text emoticons (:) :D <3); the console has no emoji font."
        req_it="- AmigaOS 3.x (3.1 / 3.1.4 / 3.2) con uno stack TCP/IP (Roadshow, AmiTCP,
  Miami/MiamiDx) che fornisca bsdsocket.library, e una connessione internet.
- Una CPU 68020 o superiore (questa build usa la moltiplicazione 32x32 del
  68020 e NON gira su un 68000 liscio). Provata su 68080/Vampire; 020/030/040/060
  benvenute.
- Qualche MB di RAM libera. Non servono ixemul.library ne' AmiSSL."
        notes_it="Note per AmigaOS 3.x
--------------------
- Le foto mostrano subito l'anteprima sfocata inclusa nel messaggio, poi si
  rifiniscono con un download limitato. I pixel decodificati restano in cache.
- Il passo della password cloud (2FA) e' pesante su 68k (PBKDF2) e puo' metterci
  un po'.
- Primo accesso: lo scambio di chiavi DH e' pesante su 68k, quindi il primo
  avvio richiede un po' di pazienza (succede una volta sola: la sessione
  viene poi salvata)."
        ;;
    "MorphOS")
        upload_limit="250 MiB"
        req_en="- MorphOS (3.x) with its TCP/IP stack and an internet connection.
- A few MB of free RAM."
        notes_en="Notes for MorphOS
-----------------
- The chat list is not fetched automatically at first login here: use
  \"Reload chat list\" in the Telegram menu to pull it in, or type a name in
  the search box to find/add a single chat. Removed/reordered chats and
  unread badges persist as everywhere else.
- In groups the typing line shows \"someone is typing\" (the per-member name
  fetch is skipped on MorphOS on purpose, to avoid a freeze).
- Auto-read runs at a gentle pace; emoji are text emoticons.
- The @ member autocomplete is off on MorphOS (freeze guard).
- Own-screen mode: MorphOS 3.16 or newer is recommended."
        req_it="- MorphOS (3.x) con il suo stack TCP/IP e una connessione internet.
- Qualche MB di RAM libera."
        notes_it="Note per MorphOS
----------------
- Qui la lista chat non viene scaricata in automatico al primo accesso: usa
  \"Reload chat list\" nel menu Telegram per caricarla, oppure digita un nome
  nella casella di ricerca per aggiungere una singola chat. Rimozioni,
  riordino e badge non letti restano persistenti come altrove.
- Nei gruppi la riga di scrittura mostra \"someone is typing\" (il recupero del
  nome del membro e' disattivato su MorphOS apposta, per evitare un freeze).
- L'autocompletamento @ dei membri e' spento su MorphOS (guardia anti-freeze).
- Modalita' schermo proprio: consigliato MorphOS 3.16 o piu' recente."
        ;;
    *)
        upload_limit="250 MiB"
        req_en="- $1 with a working TCP/IP stack (bsdsocket) and an internet connection.
- A few MB of free RAM."
        notes_en="Notes for $1
-----------------
- Full feature set, including read receipts, typing names and history paging."
        req_it="- $1 con uno stack TCP/IP funzionante (bsdsocket) e una connessione internet.
- Qualche MB di RAM libera."
        notes_it="Note per $1
-----------------
- Set completo di funzioni: read receipt, nomi di chi scrive, paginazione storia."
        ;;
    esac
}

write_readme() {
    cat > "$1" <<EOF
Telegram Amiga - $2 - alpha $VERSION
========================================

A from-scratch, native Telegram (MTProto) client. Zero dependencies: no MUI,
no ixemul, no AmiSSL. One engine:

$readme_programs

$readme_start

Highlights in this build: message photos appear first as an instant blurred
preview, refine progressively, and reuse a decoded-pixel cache when reopened;
a larger viewer, an optional lightweight text-only mode, and native JPEG photo
uploads; one-click forwarding to Saved Messages and a destination picker for
other chats; hidden chats available in local search; and non-blocking file
transfers up to $upload_limit. Downloads pipeline their chunks and work across
Telegram datacenters. Drop a file icon on the window to send it, or pick the
download drawer from the menu. URLs are clickable, menus follow the system
colours, and AmiKit/AfA_OS text is handled.

Full instructions:
  Manuale-IT.txt   (Italiano)
  Manual-EN.txt    (English)

NEVER share telegram-auth.bin -- once you log in, it holds your Telegram session.

Version: $VERSION   Build: $COMMIT_ID
Author: Michele Dipace <michele.dipace@kaffeine.net>   License: MIT
EOF
}

write_manual_en() {
    cat > "$1" <<EOF
Telegram Amiga -- User Manual (English)
=======================================

Platform: $2
Version: $VERSION   Build: $COMMIT_ID

Telegram Amiga is a from-scratch, native MTProto Telegram client. You log in
with a normal Telegram account, see your chats and exchange messages. There are
no external dependencies (no MUI, no ixemul, no AmiSSL): all the cryptography
(RSA, Diffie-Hellman, SRP/2FA, AES, SHA) is built in.

System requirements
-------------------
$req_en

$launchers_title_en
$gui_entry_en$tui_entry_en
  Long transcript lines wrap by words on narrow screens; continuation rows are
  indented. The composer grows to three rows, and Shift+Up/Down scrolls by
  complete logical messages.
  To quit it: /quit, Ctrl+C, or the window's close gadget. The window then
  stays so you can read the last lines: one more click on the close gadget
  dismisses it.
  Sending by drag-and-drop: type "/sendfile " for a document or "/photo "
  for a JPEG photo (words after the path become the caption), then drop
  the icon straight onto the console window
  (AmigaOS 3.x, MorphOS, AROS) or onto the "TG drop" Workbench icon (AmigaOS
  4.x, where the system reserves window drops); the path appears in the input
  line, Enter sends.

First start (logging in)
------------------------
$first_start_en

Using the GUI
-------------
- Left: your chat list, with an unread count badge per chat (it persists across
  restarts and clears when you open the chat).
- Click a chat to open it; the conversation is on the right.
- Scroll with the wheel, the scrollbar knob or the arrow keys. Scroll to the
  very top to load older history (it pulls the previous page and keeps your
  place).
- Click the input line and type to compose; long messages wrap over several
  lines. Press Enter to send. To reply to a message, click its bubble (or
  right-click it for a context menu) -- it is sent as a reply, with the quoted
  line shown above it. The menu highlights the item under the pointer; on your
  OWN messages it also offers Edit (change the text) and Delete (remove for
  everyone). Sent messages show a delivery tick: one check = sent, two blue
  checks = read by the other side -- it updates live.
- Chat-list avatars show each peer's real profile picture: a blurred preview
  appears as soon as the chat list loads, and it turns crisp shortly after you
  open that chat (the photo is cached in the avatars/ drawer).
- Photos sent in a conversation appear inside their message bubble. The blurred
  preview embedded in the message appears without a network request, then the
  downloaded image replaces it through progressively sharper passes. JPEGs and
  decoded RGB pixels are cached in photos/, so a viewed chat reopens without
  repeating the JPEG decode. [Photo] remains the safe fallback on failure.
- Click a photo to open a fixed-size viewer with a larger copy. It appears first
  as one complete coarse image, then refines through sharper quality passes;
  the same window is reused. The larger cache has a -l.jpg suffix in photos/.
- Right-click a photo (or its [Photo] label) and choose "Save photo as..." to
  keep the original JPEG under any drawer and name. Press S in the open viewer
  for the same requester. An uncached photo is fetched first; replacing an
  existing file always requires confirmation.
- On AmigaOS 3, the first-run default is off when no RTG screen is available or
  the CPU is below a 68040; every explicit toggle overrides that default and is
  remembered. On any slower machine, uncheck "Settings > Show inline photos".
  The conversation returns to lightweight [Photo] labels and does no background
  photo fetch or decode work. Click an individual [Photo] label to load only
  that image in the viewer. The choice is remembered for the next run.
- "Settings > Photo dithering" controls pen-grid photo quality: Full is the
  default, Light uses a gentler pattern, and Off uses direct colour matching.
  It affects paletted screens only. Compatible MorphOS, AmigaOS 4 and AROS RTG
  screens use truecolour pixels and fall back to the pen path automatically if
  their graphics driver rejects it.
- "Settings > Photo cache limit" keeps photos/ bounded at 10, 50 or 200 MB
  (50 MB by default); Unlimited is available when disk space is not a concern.
  Old files are removed during idle time, while photos currently on screen stay
  available. "Settings > Clear cache..." asks for confirmation, deletes only
  photos/ (never avatars/) and keeps already displayed photos on screen.
- To send a JPEG as a Telegram photo, use "Send photo..." in the Telegram menu
  (Amiga+P). Dropping a .jpg/.jpeg on the GUI asks Photo, File or Cancel; ESC
  also cancels. A photo over 10 MiB is preserved and sent as a document instead.
- Right-click a message and choose "Forward to Saved Messages" for a one-click
  cloud copy, or "Forward to..." to select another chat with the normal search.
- In groups, type @ in the composer to autocomplete a member: a small list
  pops up above the input line -- Up/Down select, Enter or Tab inserts
  @username, Esc closes, typing narrows the matches.
- FILES: a message carrying a file shows as [File: name (size)]. Right-click
  it and pick Download -- the file lands in the downloads/ drawer. To send
  one, use "Send file..." (Amiga+F) from the Telegram menu or from the
  right-click menu in the conversation, where "Send photo..." sits next to
  it: a standard file requester picks it, up to $upload_limit on this build.
  You can also DROP a file icon on the window to send it. Transfers show
  the percentage and speed in the status line and ESC cancels them, while
  the client stays usable -- keep chatting while a file moves.
  To download somewhere else (a RAM: drawer is much quicker on a floppy
  or slow disk), pick it with "Settings > Download drawer..." -- or put
  that path on a single line in data/telegram-downloads.txt, e.g. RAM:TGdl
  -- remember RAM: is emptied by a reboot.
- SAVED MESSAGES: the last chat in the list is you. Send files or notes to
  it from your phone or PC and pick them up on the Amiga (or the other way
  round) -- Telegram's cloud as your transfer drawer. It cannot be removed.
- Click inside the composer or the search box to place the text cursor
  exactly where you want to edit.
- Drag across transcript or composer text to select it. Right-Amiga+C copies;
  Right-Amiga+X cuts selected composer text; Right-Amiga+V pastes. The same
  actions are in the Telegram menu.
- Edits made from another Telegram client update the visible message live.
  Incoming activity also keeps updating while you type.
- "Iconify" in the Telegram menu (Amiga+I) closes the window and leaves a
  TelegramAmiga icon on the Workbench: double-click it to come back.
- F1..F10 jump to chats 1..10 (Shift+F1..F10 to 11..20).
- Search box (top-left): type a name and press Enter to find a chat on Telegram
  and add it to the list -- useful for chats not shown yet. A removed chat stays
  available in this local filter with a (hidden) marker; opening it restores it.
- Remove a chat from the list: the Telegram menu (right mouse button), the Del
  key, or right-Amiga+R (with a confirm). Re-add it later via Search.
- Reorder the list by drag and drop. Removals, order and unread badges persist.
- In groups, "<name> is typing..." appears while someone writes.

Emoji and styling
-----------------
Emoji are shown as classic text emoticons (:) :D <3) because Amiga fonts have no
emoji glyphs; bold/italic/code formatting is rendered with the system font.

Message times
-------------
Message times follow your Amiga system clock -- the same time Workbench shows --
so they always match your machine. Just set the Amiga clock to your correct
local time in the system preferences; there is no separate timezone to set in
the client (it reads the system clock directly, DST and all).

Window position and own screen
------------------------------
The window remembers its size AND position across restarts
(telegram-gui-win.txt next to the program). To open the GUI on its OWN screen
(its own "page"), edit that file and append the word own to the geometry line,
e.g. "820 560 100 50 own"; remove the word to go back to a Workbench window.
If the screen cannot open (low memory), the program falls back to a normal
window automatically.

$notes_en

Privacy and safety
------------------
telegram-auth.bin holds the saved authorization for your Telegram account: keep
it private and never share it. Do not download or copy anyone else's
telegram-auth.bin. When sharing screenshots, hide phone numbers, login codes,
passwords and private messages.

Advanced: the bundled data/telegram-api.txt holds public Telegram API app
credentials. Advanced users may replace it with their own (two lines: api_id
then api_hash).

Contributions: Javier de las Rivas (javierdlr).
Thanks to the testers around the world who run this on real hardware and
send back what they find -- this client is what it is because of them.

License: MIT -- a non-commercial community project. Diary:
https://androidlab.it/en/telegram-amiga-mtproto-client-development-diary/
EOF
}

write_manual_it() {
    cat > "$1" <<EOF
Telegram Amiga -- Manuale Utente (Italiano)
===========================================

Piattaforma: $2
Versione: $VERSION   Build: $COMMIT_ID

Telegram Amiga e' un client MTProto per Telegram scritto da zero, nativo. Accedi
con un normale account Telegram, vedi le tue chat e scambi messaggi. Nessuna
dipendenza esterna (niente MUI, niente ixemul, niente AmiSSL): tutta la
crittografia (RSA, Diffie-Hellman, SRP/2FA, AES, SHA) e' integrata.

Requisiti di sistema
--------------------
$req_it

$launchers_title_it
$gui_entry_it$tui_entry_it
  Le righe lunghe vanno a capo per parole sugli schermi stretti, con un piccolo
  rientro nelle continuazioni. Il composer cresce fino a tre righe e
  Shift+Su/Giu scorre per messaggi logici completi.
  Per uscire: /quit, Ctrl+C o il gadget di chiusura della finestra. La
  finestra poi resta aperta per farti leggere le ultime righe: un altro
  click sul gadget la congeda.
  Per il drag-and-drop scrivi "/sendfile " per un documento oppure "/photo "
  per una foto JPEG (le parole dopo il percorso diventano la didascalia),
  poi trascina l'icona direttamente sulla finestra console
  (AmigaOS 3.x, MorphOS, AROS) o sull'icona Workbench "TG drop" (AmigaOS 4.x,
  dove i drop sulla finestra sono riservati al sistema); il percorso compare
  nella riga di input, Invio lo spedisce.

Primo avvio (accesso)
---------------------
$first_start_it

Usare la GUI
------------
- A sinistra: la lista chat, con un badge dei messaggi non letti per chat
  (persiste al riavvio e si azzera quando apri la chat).
- Clicca una chat per aprirla; la conversazione e' a destra.
- Scorri con la rotella, il cursore della barra o le frecce. Scorri fino in cima
  per caricare la storia piu' vecchia (tira la pagina precedente mantenendo la
  posizione).
- Clicca la riga di input e scrivi; i messaggi lunghi vanno a capo su piu' righe.
  Premi Invio per inviare. Per rispondere a un messaggio, clicca la sua bolla (o
  tasto destro per il menu contestuale) -- parte come risposta, con la citazione
  mostrata sopra. Il menu evidenzia la voce sotto il puntatore; sui TUOI messaggi
  offre anche Edit (modifica il testo) e Delete (elimina per tutti). I messaggi
  inviati mostrano la spunta: una = inviato, due azzurre = letto -- in tempo reale.
- Gli avatar della lista chat mostrano la vera foto profilo: un'anteprima
  sfocata appare subito col caricamento della lista, e diventa nitida poco dopo
  che apri quella chat (la foto viene salvata nel cassetto avatars/).
- Le foto inviate in conversazione appaiono dentro la loro bolla. L'anteprima
  sfocata inclusa nel messaggio compare senza richieste di rete, poi viene
  sostituita da passate sempre piu' nitide. JPEG e pixel RGB decodificati sono
  salvati in photos/, quindi riaprire una chat gia' vista non ripete il decode.
  [Photo] resta il ripiego se Telegram non rende disponibile l'immagine.
- Clicca una foto per aprire un viewer a dimensione fissa con una copia piu'
  grande. Appare subito intera e sgranata, poi si rifinisce con passate sempre
  piu' nitide; la stessa finestra viene riutilizzata. La copia grande in
  photos/ ha il suffisso -l.jpg.
- Click destro su una foto (o sulla sua etichetta [Photo]) e scegli
  "Save photo as..." per salvare il JPEG originale con drawer e nome a scelta.
  Nel viewer premi S per aprire lo stesso requester. Se la foto non e' in cache
  viene prima scaricata; la sostituzione di un file esistente chiede conferma.
- Su AmigaOS 3 il primo avvio parte senza foto inline quando non e' disponibile
  uno schermo RTG oppure la CPU e' inferiore al 68040; ogni scelta esplicita
  sostituisce il default e resta memorizzata. Su una macchina lenta togli la
  spunta da "Settings > Show inline photos" nel menu
  Telegram. La conversazione torna alle leggere etichette [Photo] e non avvia
  download o decodifiche in background. Clicca una singola [Photo] per caricare
  solo quella immagine nel viewer. La scelta resta memorizzata al riavvio.
- "Settings > Photo dithering" regola la resa delle foto a penne: Full e' il
  valore predefinito, Light usa una trama piu' leggera e Off usa i colori
  diretti. Vale solo sugli schermi a palette. Gli schermi RTG compatibili di
  MorphOS, AmigaOS 4 e AROS usano pixel truecolor e tornano automaticamente
  alle penne se il driver non accetta quel percorso.
- "Settings > Photo cache limit" mantiene photos/ entro 10, 50 o 200 MB
  (50 MB predefiniti); Unlimited e' disponibile quando lo spazio disco non e'
  un problema. I file piu' vecchi vengono rimossi durante i momenti inattivi,
  mentre le foto gia' visibili restano a schermo. "Settings > Clear cache..." chiede
  conferma, svuota solo photos/ (mai avatars/) e conserva le immagini mostrate.
- Per inviare un JPEG come vera foto Telegram usa "Send photo..." nel menu
  Telegram (Amiga+P). Trascinando un .jpg/.jpeg sulla GUI puoi scegliere Photo,
  File o Cancel; anche ESC annulla. Oltre 10 MiB viene inviato come file.
- Click destro su un messaggio: "Forward to Saved Messages" lo copia con un
  click nel proprio cloud; "Forward to..." permette di scegliere un'altra chat
  usando la ricerca normale.
- Nei gruppi, digita @ nel composer per completare un membro: compare una
  listina sopra la riga di input -- Su/Giu' selezionano, Invio o Tab inserisce
  @username, Esc chiude, digitando filtri i risultati.
- FILE: un messaggio con allegato appare come [File: nome (dimensione)].
  Click destro -> Download e il file finisce nel cassetto downloads/. Per
  inviarne uno usa "Send file..." (Amiga+F) dal menu Telegram oppure dal
  menu del click destro nella conversazione, dove accanto trovi anche
  "Send photo...": lo scegli dal requester di sistema, fino a $upload_limit
  su questa build. Puoi anche TRASCINARE l'icona di un file sulla finestra.
  Durante i trasferimenti vedi percentuale e velocita' nella riga di stato,
  ESC annulla, e il client resta usabile: puoi continuare a chattare.
  Per scaricare altrove (un cassetto in RAM: e' molto piu' rapido su
  floppy o dischi lenti) scegli "Settings > Download drawer...", oppure scrivi
  il percorso su una riga sola in
  data/telegram-downloads.txt, per esempio RAM:TGdl -- ricorda che RAM:
  si svuota al riavvio.
- MESSAGGI SALVATI: l'ultima chat della lista sei tu. Mandaci file o appunti
  dal telefono o dal PC e riprendili sull'Amiga (o viceversa) -- il cloud di
  Telegram come cassetto di scambio. Non si puo' rimuovere.
- Un click dentro il composer o la casella di ricerca posiziona il cursore
  esattamente dove vuoi correggere.
- Trascina sul testo della conversazione o del composer per selezionarlo.
  Amiga-destro+C copia; Amiga-destro+X taglia il testo selezionato nel composer;
  Amiga-destro+V incolla. Le stesse azioni sono nel menu Telegram.
- Le modifiche fatte da un altro client Telegram aggiornano il messaggio
  visibile in tempo reale. Anche l'attivita' in arrivo continua mentre scrivi.
- "Iconify" nel menu Telegram (Amiga+I) chiude la finestra e lascia
  un'icona TelegramAmiga sul Workbench: doppio click per tornare.
- F1..F10 saltano alle chat 1..10 (Shift+F1..F10 alle 11..20).
- Casella di ricerca (in alto a sinistra): scrivi un nome e premi Invio per
  trovare una chat su Telegram e aggiungerla alla lista -- utile per chat non
  ancora mostrate. Una chat rimossa resta nel filtro locale col marker (hidden):
  aprendola torna nella lista.
- Rimuovi una chat dalla lista: menu Telegram (tasto destro), tasto Del, oppure
  Amiga-destro+R (con conferma). La riaggiungi poi con la Ricerca.
- Riordina la lista col trascinamento. Rimozioni, ordine e badge persistono.
- Nei gruppi compare "<nome> sta scrivendo..." mentre qualcuno scrive.

Emoji e stile
-------------
Le emoji appaiono come emoticon testuali classiche (:) :D <3) perche' i font
Amiga non hanno glifi emoji; grassetto/corsivo/code usano il font di sistema.

Orari dei messaggi
------------------
Gli orari dei messaggi seguono l'orologio di sistema dell'Amiga -- lo stesso che
mostra Workbench -- quindi coincidono sempre con la tua macchina. Basta impostare
l'orologio dell'Amiga all'ora locale corretta nelle preferenze; non c'e' un fuso
orario separato da configurare nel client (legge l'orologio di sistema, DST
compresa).

Posizione finestra e schermo proprio
------------------------------------
La finestra ricorda dimensione E posizione tra i riavvii (telegram-gui-win.txt
accanto al programma). Per aprire la GUI su un SUO schermo (una "pagina"
propria), modifica quel file aggiungendo la parola own in coda alla riga della
geometria, es. "820 560 100 50 own"; togli la parola per tornare alla finestra
su Workbench. Se lo schermo non si puo' aprire (poca memoria), il programma
ripiega automaticamente sulla finestra normale.

$notes_it

Privacy e sicurezza
-------------------
telegram-auth.bin contiene l'autorizzazione salvata del tuo account Telegram:
tienilo privato e non condividerlo mai. Non scaricare ne' copiare il
telegram-auth.bin di altri. Negli screenshot nascondi numeri di telefono, codici
di accesso, password e messaggi privati.

Avanzato: il data/telegram-api.txt incluso contiene credenziali API pubbliche. Gli
utenti avanzati possono sostituirlo col proprio (due righe: api_id poi api_hash).

Contributi: Javier de las Rivas (javierdlr).
Grazie ai tester sparsi per il mondo che lo provano su hardware vero e
raccontano quello che trovano: questo client e' com'e' grazie a loro.

Licenza: MIT -- progetto di comunita' non commerciale. Diario:
https://androidlab.it/telegram-amiga-diario-sviluppo-client-mtproto/
EOF
}

# Aminet per-architecture metadata. Sets: archtag (FILENAME suffix), archval
# (the readme Architecture: value), requires. NB the Aminet enum has no
# x86_64-aros token -> 64-bit AROS uses archval i386-aros, distinguished only by
# the x86_64-aros FILENAME (verified live, e.g. filesysbox.x86_64-aros).
# From 0.0.6 the archives are named after the binary (TelegramAmiga) in the
# classic Aminet suffix style -- the 30-char filename limit rules out the long
# arch tags (TelegramAmiga.m68k-amigaos.readme would be 33). The old tgamiga.*
# pages are superseded via the Replaces: field (lhaold below).
aminet_meta() {
    case "$1" in
    amigaos3)    archtag="m68k-amigaos"; archval="m68k-amigaos >= 3.0.0"
                 lhaname="TelegramAmiga"
                 requires="68020+ CPU and a bsdsocket.library TCP/IP stack" ;;
    morphos)     archtag="ppc-morphos";  archval="ppc-morphos"
                 lhaname="TelegramAmiga-MOS"
                 requires="MorphOS 3.x with its TCP/IP stack" ;;
    amigaos4)    archtag="ppc-amigaos";  archval="ppc-amigaos >= 4.0.0"
                 lhaname="TelegramAmiga-OS4"
                 requires="AmigaOS 4.x with its TCP/IP stack" ;;
    aros-i386)   archtag="i386-aros";    archval="i386-aros"
                 lhaname="TelegramAmiga-AROS"
                 requires="AROS (i386) with a TCP/IP stack (AROSTCP)" ;;
    aros-x86_64) archtag="x86_64-aros";  archval="i386-aros"
                 lhaname="TelegramAmiga-AROS64"
                 requires="AROS (x86_64) with a TCP/IP stack (AROSTCP)" ;;
    *) echo "aminet_meta: unknown arch $1" >&2; exit 1 ;;
    esac
    lhaold="comm/tcp/tgamiga.$archtag.lha"
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

write_aminet_readme() {
    out=$1; archval=$2; requires=$3; replaces=$4
    cat > "$out" <<EOF
Short:        Native MTProto Telegram chat client
Uploader:     $AMINET_UPLOADER
Author:       $AMINET_AUTHOR
Type:         comm/tcp
Version:      $VERSION
Replaces:     $replaces
Architecture: $archval
Requires:     $requires

WHAT IS THIS?
-------------
Telegram Amiga brings real, live Telegram chat to the Amiga -- not through
a gateway, a proxy service or a web wrapper, but by speaking Telegram's
own MTProto protocol natively, from scratch, on your machine. You sign in
to your normal Telegram account, your chat list appears, and you talk to
people (and they talk back) on hardware that may well be older than they
are.

Everything is built in: RSA, Diffie-Hellman, AES, SHA and the SRP two-
factor login are implemented inside the program. Zero external
dependencies -- no MUI, no ixemul.library, no AmiSSL, no TCP helper
beyond your system's own bsdsocket stack.

$aminet_programs

WHAT CAN I ACTUALLY DO WITH IT?
-------------------------------
Read and send messages in private chats, groups and channels. Download a
received file (right-click -> Download) or send one from disk, up to
$upload_limit on this build, including files over 10 MiB.
Photos appear immediately as blurred previews, refine from a bounded download
and reuse decoded pixels from disk when reopened. Click one for a larger
progressive viewer, or disable inline loading on a slower machine and open
only the images you choose. Forward one message to Saved Messages in a
click, or choose another destination through chat search.
Use the pinned Saved Messages chat as a cloud transfer drawer between the
Amiga and your phone or PC. Reply to a specific message (right-click it).
Edit or delete your own messages. See
real delivery state: one tick = sent, two blue ticks = read, updating
live. See who is typing. Search for chats. Send messages from your desk
at work and find the conversation already synced when you get home to
the Amiga -- and the other way round.

Message times follow your Amiga clock. Unread badges and your chat order
survive restarts. The window remembers where you left it, and can open
on its own screen if you prefer a dedicated page for chatting.

GETTING STARTED
---------------
1. Copy this drawer to a WRITABLE volume (not from the archive directly).
2. Double-click TelegramAmiga (or TelegramAmiga-TUI on very low-end setups).
3. First run walks you through the normal Telegram login: phone number,
   the code Telegram sends you, and your cloud password if you use
   two-factor. That is all -- next time it goes straight to your chats.

The login is stored in telegram-auth.bin next to the program. Treat that
file like a house key: NEVER copy it around or share it -- anyone who has
it has your Telegram session. Full EN and IT manuals are in the archive,
including per-platform notes and troubleshooting.

WHAT IS NEW IN $VERSION
$(printf '%*s' $((15 + ${#VERSION})) '' | tr ' ' '-')
$(changelog_section_text)

A COMMUNITY PROJECT
-------------------
MIT licensed, non-commercial, written for the love of the platform.
Bug reports and wishes are very welcome -- testers on real hardware
(A1200s, A4000s, Pegasos, Sam, FPGA machines) are what moves this
project forward.

  Source + issues:
  $REPO_URL
  Development diary:
  $DIARY_URL
EOF
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
        *) echo "Unknown expected type: $expected" >&2; exit 1 ;;
    esac

    case "$expected" in
        amigaos4|aros-i386|aros-x86_64)
            if ! strings "$binary" | grep -F '$STACK:1048576' >/dev/null; then
                echo "ERROR $platform: binary lacks the 1 MiB AmigaDOS stack cookie." >&2
                exit 1
            fi
            ;;
    esac

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

    drawer="Telegram-$suffix-$DATE_STAMP"
    dest="$PACKAGE_ROOT/$drawer"
    rm -rf "$dest"
    mkdir -p "$dest"

    # Per-lane text and icon policy first: the icon block below reads gui_icon.
    fill_platform_text "$platform"

    cp "$binary" "$dest/TelegramAmiga"
    # Self-launching, script-free (papiosaur / Easy2Install suggestion). Two
    # byte-identical flashless icons (DefaultTool = TelegramAmiga, Stack 1 MiB)
    # both point at the one binary; the binary reads the WBStartup arg names and
    # picks GUI vs TUI. TelegramAmiga.info owns the binary itself -> GUI.
    # TelegramAmiga-TUI.info owns a 0-byte marker whose name carries "TUI" -> the
    # binary opens a CON: window and runs the console client. No IconX, no shell
    # scripts.
    if [ "$gui_icon" != "0" ]; then
        cp "$ROOT_DIR/assets/TelegramAmiga.info" "$dest/TelegramAmiga.info"
    fi
    # From 0.0.9 (Michele) the TUI icon ships ONLY on the 68k line, where a
    # console client is what those machines actually want. The PPC and x86
    # packages carry the GUI icon alone; their manuals explain the Shell
    # command for anyone who still wants the text client.
    if [ "$tui_icon" = "1" ]; then
        : > "$dest/TelegramAmiga-TUI"
        if [ "$gui_icon" = "0" ]; then
            # 68000 package: the icon must ask for the same 384 KB the binary's
            # cookie does, or a Workbench launch would reserve the full
            # megabyte and hand back the memory this build just saved.
            cp "$ROOT_DIR/assets/TelegramAmiga-TUI-68000.info" \
               "$dest/TelegramAmiga-TUI.info"
        else
            cp "$ROOT_DIR/assets/TelegramAmiga-TUI.info" \
               "$dest/TelegramAmiga-TUI.info"
        fi
    fi
    mkdir -p "$dest/data"
    cp "$ROOT_DIR/assets/public-telegram-api.txt" "$dest/data/telegram-api.txt"

    write_readme "$dest/README.txt" "$platform"
    write_manual_en "$dest/Manual-EN.txt" "$platform"
    write_manual_it "$dest/Manuale-IT.txt" "$platform"
    cp "$ROOT_DIR/LICENSE" "$dest/LICENSE"
    # Full changelog in every package (release rule since 0.0.8): the repo
    # CHANGELOG.md is the single source; refuse to package a release whose
    # version is not in it yet (the Unreleased section must be promoted).
    if ! grep -q "^## \[$VERSION\]" "$ROOT_DIR/CHANGELOG.md"; then
        echo "ERROR: CHANGELOG.md has no '## [$VERSION]' section - promote Unreleased first" >&2
        exit 1
    fi
    cp "$ROOT_DIR/CHANGELOG.md" "$dest/CHANGELOG.txt"

    if command -v zip >/dev/null 2>&1; then
        (cd "$PACKAGE_ROOT" && rm -f "$drawer.zip" && zip -qr "$drawer.zip" "$drawer")
        # Post-zip guard: the archive must hold THIS binary and no private file.
        unzip -p "$PACKAGE_ROOT/$drawer.zip" "*/TelegramAmiga" > "$PACKAGE_ROOT/.zipbin" 2>/dev/null
        if [ "$(md5of "$PACKAGE_ROOT/.zipbin")" != "$(md5of "$binary")" ]; then
            rm -f "$PACKAGE_ROOT/.zipbin"
            echo "ERROR $platform: packaged binary != built binary ($binary)" >&2; exit 1
        fi
        rm -f "$PACKAGE_ROOT/.zipbin"
        if unzip -l "$PACKAGE_ROOT/$drawer.zip" | grep -qiE "telegram-(auth|peers|seed|password|token)|phone-code-hash"; then
            echo "ERROR $platform: SESSION FILE LEAK in $drawer.zip" >&2; exit 1
        fi
        echo "$PACKAGE_ROOT/$drawer.zip  [bin $(md5of "$binary" | cut -c1-8)]"
    else
        echo "$dest"
    fi

    # --- Aminet: tgamiga.<archtag>.lha + matching tgamiga.<archtag>.readme ----
    # Reuses the assembled drawer ($dest, incl. LICENSE) but under a clean
    # top-level name ($AMINET_DRAWER) so unpacking yields one tidy directory.
    if [ "$AMINET" = "1" ]; then
        if [ ! -x "$LHA_BIN" ]; then
            echo "ERROR $platform: LhA encoder not found at $LHA_BIN (build jca02266/lha or set LHA_BIN=, or AMINET=0)" >&2
            exit 1
        fi
        aminet_meta "$expected"
        amiwork="$AMINET_ROOT/$AMINET_DRAWER"
        lhafile="$AMINET_ROOT/$lhaname.lha"
        rm -rf "$amiwork"; mkdir -p "$amiwork"
        cp -R "$dest"/* "$amiwork"/ # -R: the package now contains data/
        rm -f "$lhafile"
        ( cd "$AMINET_ROOT" && "$LHA_BIN" a "$lhaname.lha" "$AMINET_DRAWER" >/dev/null )
        rm -rf "$amiwork"
        # Verify it extracts under UNIX lha (Aminet's own checklist requirement).
        if ! "$LHA_BIN" t "$lhafile" >/dev/null 2>&1; then
            echo "ERROR $platform: $lhafile fails lha integrity test" >&2; exit 1
        fi
        # Same guards as the zip path: no leaked session file; the packaged
        # binary is byte-identical to the built one.
        if "$LHA_BIN" l "$lhafile" | grep -qiE "telegram-(auth|peers|seed|password|token)|phone-code-hash"; then
            echo "ERROR $platform: SESSION FILE LEAK in $lhafile" >&2; exit 1
        fi
        lhatmp=$(mktemp -d)
        ( cd "$lhatmp" && "$LHA_BIN" xq "$lhafile" >/dev/null 2>&1 )
        if [ "$(md5of "$lhatmp/$AMINET_DRAWER/TelegramAmiga")" != "$(md5of "$binary")" ]; then
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
            *) arch_want="" ;;
        esac
        if [ -n "$arch_want" ] && \
           ! file -b "$lhatmp/$AMINET_DRAWER/TelegramAmiga" | grep -q "$arch_want"; then
            rm -rf "$lhatmp"
            echo "ERROR $platform: wrong-arch binary inside $lhafile (want $arch_want)" >&2
            exit 1
        fi
        rm -rf "$lhatmp"
        write_aminet_readme "$AMINET_ROOT/$lhaname.readme" "$archval" "$requires" "$lhaold"
        echo "$lhafile  +  $lhaname.readme  [Architecture: $archval]"
    fi
}

package_one "AmigaOS 3.x" "$AMIGAOS3_BINARY" "amigaos3" "amigaos3"
package_one "MorphOS" "$MORPHOS_BINARY" "morphos" "morphos"
package_one "AmigaOS 4.x" "$AMIGAOS4_BINARY" "amigaos4" "amigaos4"
package_one "AROS i386 ABIv0" "$AROS_I386_BINARY" "aros-i386" "aros-i386"
package_one "AROS x86_64" "$AROS_X86_64_BINARY" "aros-x86_64" "aros-x86_64"
package_one "AmigaOS 3.x (68000)" "$AMIGAOS3_68000_BINARY" "amigaos3-68000" "amigaos3"

# --- checksums ---------------------------------------------------------------
( cd "$PACKAGE_ROOT" && ls Telegram-*-"$DATE_STAMP".zip >/dev/null 2>&1 &&
  sha_cmd Telegram-*-"$DATE_STAMP".zip > SHA256SUMS-github.txt &&
  echo "$PACKAGE_ROOT/SHA256SUMS-github.txt" ) || true
if [ "$AMINET" = "1" ] && ls "$AMINET_ROOT"/TelegramAmiga*.lha >/dev/null 2>&1; then
    ( cd "$AMINET_ROOT" && sha_cmd TelegramAmiga*.lha TelegramAmiga*.readme > SHA256SUMS-aminet.txt )
    echo
    echo "Aminet artifacts ready in: $AMINET_ROOT  (version $VERSION, Type comm/tcp)"
    echo "Upload (Michele): FTP main.aminet.net -> cd /new -> binary -> put each"
    echo "  TelegramAmiga[<-suffix>].lha AND matching .readme (5 + 5 files)."
    echo "  Web form may be back at https://aminet.net/upload. See memory/aminet-publishing.md"
fi

# --- OS4Depot (fixed release channel since 0.0.6) ----------------------------
# Anonymous FTP to os4depot.net /upload: the archive FIRST, the readme LAST
# (their processor keys off the readme). Readme = THEIR header format
# (name:/description:/.../hend:) + our Aminet body. Queue shows on
# https://os4depot.net/index.php?function=uploads after ~15 min.
# In its OWN subdirectory: "telegramamiga.lha" next to "TelegramAmiga.lha"
# SILENTLY OVERWRITES the Aminet OS3 package on a case-insensitive macOS
# filesystem -- that shipped an OS4 ELF to every Aminet OS3 downloader in
# 0.0.7 (field report 2026-07-26, "unable to open tool").
OS4DEPOT_ROOT=${OS4DEPOT_ROOT:-"$PACKAGE_ROOT/os4depot"}
if [ "$AMINET" = "1" ] && [ -f "$AMINET_ROOT/TelegramAmiga-OS4.lha" ]; then
    mkdir -p "$OS4DEPOT_ROOT"
    cp "$AMINET_ROOT/TelegramAmiga-OS4.lha" "$OS4DEPOT_ROOT/telegramamiga.lha"
    {
        printf 'name:TelegramAmiga\n'
        printf 'description:Native MTProto Telegram chat client\n'
        printf 'version:%s\n' "$VERSION"
        printf 'author:Michele Dipace\n'
        printf 'submitter:Michele Dipace\n'
        printf 'email:michele.dipace@kaffeine.net\n'
        printf 'url:%s\n' "$REPO_URL"
        printf 'category:network/chat\n'
        printf 'requirements:AmigaOS 4.x with its TCP/IP stack\n'
        # license must be one of THEIR values (help/help_submit.txt):
        # Other APL BSD Commercial Emailware Freeware GPL LGPL MPL
        # "Public domain" Shareware -- MIT is not in the list, so: Other.
        # replaces is REQUIRED when updating an existing entry (0.0.8 failed
        # validation on both, 2026-07-31; 0.0.7 passed only because it was new).
        printf 'license:Other\n'
        printf 'replaces:network/chat/telegramamiga.lha\n'
        printf 'distribute:yes\n'
        printf 'minosversion:4.0\n'
        printf 'hend:\n\n'
        # body: the Aminet OS4 readme minus its header block
        awk 'flip { print } /^$/ && !flip { flip = 1 }' \
            "$AMINET_ROOT/TelegramAmiga-OS4.readme"
    } > "$OS4DEPOT_ROOT/telegramamiga_lha.readme"
    rm -f "$AMINET_ROOT/telegramamiga_lha.readme" # pre-0.0.8 location, stale
    echo
    echo "OS4Depot pair ready in: $OS4DEPOT_ROOT (telegramamiga.lha + readme)"
    echo "Upload: curl -T telegramamiga.lha ftp://os4depot.net/upload/ --user anonymous:"
    echo "        then the readme (LAST). Queue: os4depot.net ?function=uploads"
fi

# --- MorphOS-Storage (fixed release channel since 0.0.6) ---------------------
# Web form only (https://www.morphos-storage.net/?page=submit), no account but
# a CAPTCHA: Michele submits. Hand him TelegramAmiga-MOS.lha + its readme and
# the description block; agent prepares, human clicks.
if [ "$AMINET" = "1" ] && [ -f "$AMINET_ROOT/TelegramAmiga-MOS.lha" ]; then
    echo
    echo "MorphOS-Storage (Michele, web form + captcha):"
    echo "  https://www.morphos-storage.net/?page=submit"
    echo "  archive: $AMINET_ROOT/TelegramAmiga-MOS.lha"
    echo "  readme:  $AMINET_ROOT/TelegramAmiga-MOS.readme"
    echo "  name: TelegramAmiga  version: $VERSION"
fi
