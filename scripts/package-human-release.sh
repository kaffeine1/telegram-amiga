#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Build the human-facing release packages for Telegram Amiga.
# Version comes from include/tg_version.h (override with VERSION=... if needed).
#
# Each package contains ONLY the program, the two launchers (GUI + TUI) with
# their icons, the PUBLIC Telegram API app credentials and per-architecture
# IT/EN manuals. No user session is ever bundled: telegram-auth.bin and the
# peer cache are created locally by the user on first login (see the manual).

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

AMIGAOS3_BINARY=${AMIGAOS3_BINARY:-"$ROOT_DIR/build/amigaos3-clib2/telegram-test"}
MORPHOS_BINARY=${MORPHOS_BINARY:-"$ROOT_DIR/build/morphos-cross/telegram-test"}
AMIGAOS4_BINARY=${AMIGAOS4_BINARY:-"$ROOT_DIR/build/amigaos4/telegram-test"}
AROS_I386_BINARY=${AROS_I386_BINARY:-"$ROOT_DIR/build/aros-i386-abiv0/telegram-test"}
AROS_X86_64_BINARY=${AROS_X86_64_BINARY:-"$ROOT_DIR/build/aros-x86_64/telegram-test"}

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
    case "$1" in
    "AmigaOS 3.x")
        req_en="- AmigaOS 3.x (3.1 / 3.1.4 / 3.2) with a TCP/IP stack (Roadshow, AmiTCP,
  Miami/MiamiDx) providing bsdsocket.library, and an internet connection.
- A 68020 or better CPU (this build uses the 68020 32x32 multiply and will
  NOT run on a plain 68000). Tested on 68080/Vampire; 020/030/040/060 welcome.
- A few MB of free RAM. No ixemul.library and no AmiSSL are needed."
        notes_en="Notes for AmigaOS 3.x
---------------------
- Photos/files are shown as [Photo] / [File] labels (no image decoding on 68k).
- The cloud-password (2FA) step is heavy on a 68k (PBKDF2) and can take a while.
- First login: on some 68k setups the in-GUI fresh login is still rough. The
  reliable path is to log in ONCE with TelegramTUI (the console launcher), then
  use TelegramGUI from then on (it reuses the saved login).
- Emoji are drawn as text emoticons (:) :D <3); the console has no emoji font."
        req_it="- AmigaOS 3.x (3.1 / 3.1.4 / 3.2) con uno stack TCP/IP (Roadshow, AmiTCP,
  Miami/MiamiDx) che fornisca bsdsocket.library, e una connessione internet.
- Una CPU 68020 o superiore (questa build usa la moltiplicazione 32x32 del
  68020 e NON gira su un 68000 liscio). Provata su 68080/Vampire; 020/030/040/060
  benvenute.
- Qualche MB di RAM libera. Non servono ixemul.library ne' AmiSSL."
        notes_it="Note per AmigaOS 3.x
--------------------
- Foto/file appaiono come etichette [Photo] / [File] (niente decodifica immagini
  sul 68k).
- Il passo della password cloud (2FA) e' pesante su 68k (PBKDF2) e puo' metterci
  un po'.
- Primo accesso: su alcune configurazioni 68k il login fresco dentro la GUI e'
  ancora instabile. La via affidabile e' accedere UNA volta con TelegramTUI (il
  launcher console), poi usare TelegramGUI (riusa il login salvato)."
        ;;
    "MorphOS")
        req_en="- MorphOS (3.x) with its TCP/IP stack and an internet connection.
- A few MB of free RAM."
        notes_en="Notes for MorphOS
-----------------
- The chat list is built with Search: getDialogs is not auto-loaded here (it
  stalls MorphOS' TCP stack), so type a name in the search box to find/add a
  chat. Removed/reordered chats and unread badges still persist.
- In groups the typing line shows \"someone is typing\" (the per-member name
  fetch is skipped on MorphOS on purpose, to avoid a freeze).
- Auto-read runs at a gentle pace; emoji are text emoticons.
- Avatars stay as blurred previews / initials (the crisp-photo download is
  disabled on MorphOS on purpose); the @ member autocomplete is off too.
- Own-screen mode: MorphOS 3.16 or newer is recommended."
        req_it="- MorphOS (3.x) con il suo stack TCP/IP e una connessione internet.
- Qualche MB di RAM libera."
        notes_it="Note per MorphOS
----------------
- La lista chat si costruisce con la Ricerca: qui getDialogs non viene caricato
  in automatico (blocca lo stack TCP di MorphOS), quindi digita un nome nella
  casella di ricerca per trovare/aggiungere una chat. Rimozioni/riordino e
  badge non letti restano comunque persistenti.
- Nei gruppi la riga di scrittura mostra \"someone is typing\" (il recupero del
  nome del membro e' disattivato su MorphOS apposta, per evitare un freeze).
- Gli avatar restano anteprime sfocate / iniziali (il download della foto
  nitida e' disattivato su MorphOS apposta); anche l'autocompletamento @ dei
  membri e' spento.
- Modalita' schermo proprio: consigliato MorphOS 3.16 o piu' recente."
        ;;
    *)
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
no ixemul, no AmiSSL. Two clients, one engine:

  TelegramGUI  - graphical (Intuition), with scrollbars + mouse.
  TelegramTUI  - text-mode / console.

Quick start: copy this drawer to a WRITABLE volume, then double-click
TelegramGUI (or TelegramTUI). First run signs you in (phone -> code -> 2FA).

New in $VERSION: real profile-picture avatars in the chat list (instant blurred
previews everywhere; they turn crisp after you open a chat -- not on MorphOS),
@username autocomplete in groups (type @), the window remembers its position,
an optional own screen, and stronger first-login randomness. (0.0.4 added
edit/delete, live read receipts, multi-device sync and system-clock times.)

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

Two launchers
-------------
- TelegramGUI -- the native Intuition/GadTools GUI (chat list + conversation).
  Double-click it: it starts the GUI directly, with no flashing console window.
- TelegramTUI -- a full-screen text/console client for low-end or mouse-less
  setups. Both share the same login and the same saved session.

First start (logging in)
------------------------
1. Unpack the drawer and double-click TelegramGUI (or TelegramTUI).
2. If there is no saved login, a login panel appears. Enter your phone number
   in full international form (for example +39 333 1234567), then the code
   Telegram sends you. If your account has a cloud password (2FA), type it on
   the masked screen (if you do NOT have one, just press Enter to continue).
3. The client logs in and writes telegram-auth.bin in this drawer. After that
   it reuses the saved login -- you are not asked for the phone/code again.

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
- In groups, type @ in the composer to autocomplete a member: a small list
  pops up above the input line -- Up/Down select, Enter or Tab inserts
  @username, Esc closes, typing narrows the matches.
- F1..F10 jump to chats 1..10 (Shift+F1..F10 to 11..20).
- Search box (top-left): type a name and press Enter to find a chat on Telegram
  and add it to the list -- useful for chats not shown yet.
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

I due launcher
--------------
- TelegramGUI -- la GUI nativa Intuition/GadTools (lista chat + conversazione).
  Doppio click: avvia direttamente la GUI, senza finestra console che lampeggia.
- TelegramTUI -- un client testuale a schermo intero per macchine leggere o
  senza mouse. Condividono lo stesso login e la stessa sessione salvata.

Primo avvio (accesso)
---------------------
1. Scompatta il drawer e fai doppio click su TelegramGUI (o TelegramTUI).
2. Se non c'e' un login salvato, compare il pannello di accesso. Inserisci il
   numero di telefono in formato internazionale completo (es. +39 333 1234567),
   poi il codice che Telegram ti invia. Se il tuo account ha una password cloud
   (2FA), digitala sulla schermata mascherata (se NON ce l'hai, premi Invio).
3. Il client accede e scrive telegram-auth.bin in questo drawer. Da li' in poi
   riusa il login salvato -- non ti richiede piu' telefono/codice.

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
- Nei gruppi, digita @ nel composer per completare un membro: compare una
  listina sopra la riga di input -- Su/Giu' selezionano, Invio o Tab inserisce
  @username, Esc chiude, digitando filtri i risultati.
- F1..F10 saltano alle chat 1..10 (Shift+F1..F10 alle 11..20).
- Casella di ricerca (in alto a sinistra): scrivi un nome e premi Invio per
  trovare una chat su Telegram e aggiungerla alla lista -- utile per chat non
  ancora mostrate.
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

Licenza: MIT -- progetto di comunita' non commerciale. Diario:
https://androidlab.it/telegram-amiga-diario-sviluppo-client-mtproto/
EOF
}

# Aminet per-architecture metadata. Sets: archtag (FILENAME suffix), archval
# (the readme Architecture: value), requires. NB the Aminet enum has no
# x86_64-aros token -> 64-bit AROS uses archval i386-aros, distinguished only by
# the x86_64-aros FILENAME (verified live, e.g. filesysbox.x86_64-aros).
aminet_meta() {
    case "$1" in
    amigaos3)    archtag="m68k-amigaos"; archval="m68k-amigaos >= 3.0.0"
                 requires="68020+ CPU and a bsdsocket.library TCP/IP stack" ;;
    morphos)     archtag="ppc-morphos";  archval="ppc-morphos"
                 requires="MorphOS 3.x with its TCP/IP stack" ;;
    amigaos4)    archtag="ppc-amigaos";  archval="ppc-amigaos >= 4.0.0"
                 requires="AmigaOS 4.x with its TCP/IP stack" ;;
    aros-i386)   archtag="i386-aros";    archval="i386-aros"
                 requires="AROS (i386) with a TCP/IP stack (AROSTCP)" ;;
    aros-x86_64) archtag="x86_64-aros";  archval="i386-aros"
                 requires="AROS (x86_64) with a TCP/IP stack (AROSTCP)" ;;
    *) echo "aminet_meta: unknown arch $1" >&2; exit 1 ;;
    esac
}

# The Aminet .readme: machine-readable header (Short/Uploader/Author/Type/
# Version/Architecture/Requires) FIRST, blank line, then the body. LF-only (the
# heredoc emits LF), lines <= 78 cols, version ONLY here (never in the filename).
write_aminet_readme() {
    out=$1; archval=$2; requires=$3
    cat > "$out" <<EOF
Short:        Native MTProto Telegram chat client
Uploader:     $AMINET_UPLOADER
Author:       $AMINET_AUTHOR
Type:         comm/tcp
Version:      $VERSION
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

Two programs share one engine and one saved login:

  TelegramGUI  - the native Intuition/GadTools GUI: chat list with real
                 profile-picture avatars, message bubbles, scrollbars,
                 mouse wheel, context menus.
  TelegramTUI  - the text/console client, at home on a 68030 with a
                 serial console or an ssh session.

WHAT CAN I ACTUALLY DO WITH IT?
-------------------------------
Read and send messages in private chats, groups and channels. Reply to a
specific message (right-click it). Edit or delete your own messages. See
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
2. Double-click TelegramGUI (or TelegramTUI on very low-end setups).
3. First run walks you through the normal Telegram login: phone number,
   the code Telegram sends you, and your cloud password if you use
   two-factor. That is all -- next time it goes straight to your chats.

The login is stored in telegram-auth.bin next to the program. Treat that
file like a house key: NEVER copy it around or share it -- anyone who has
it has your Telegram session. Full EN and IT manuals are in the archive,
including per-platform notes and troubleshooting.

A COMMUNITY PROJECT
-------------------
MIT licensed, non-commercial, written for the love of the platform.
Bug reports and wishes are very welcome -- testers on real hardware
(A1200s, A4000s, Pegasos, Sam, FPGA machines) are what moves this
project forward.

  Source + issues:    $REPO_URL
  Development diary:  $DIARY_URL
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

    cp "$binary" "$dest/telegram-test"
    # Launchers (shared with the repo). TelegramGUI's icon is flashless
    # (DefaultTool = telegram-test, Stack 1 MiB): a double-click starts the GUI
    # directly. TelegramTUI keeps the IconX console launcher.
    cp "$ROOT_DIR/scripts/TelegramGUI" "$dest/TelegramGUI"
    cp "$ROOT_DIR/scripts/TelegramTUI" "$dest/TelegramTUI"
    cp "$ROOT_DIR/assets/TelegramGUI.info" "$dest/TelegramGUI.info"
    cp "$ROOT_DIR/assets/TelegramAmigaOS4.info" "$dest/TelegramTUI.info"
    mkdir -p "$dest/data"
    cp "$ROOT_DIR/assets/public-telegram-api.txt" "$dest/data/telegram-api.txt"

    fill_platform_text "$platform"
    write_readme "$dest/README.txt" "$platform"
    write_manual_en "$dest/Manual-EN.txt" "$platform"
    write_manual_it "$dest/Manuale-IT.txt" "$platform"
    cp "$ROOT_DIR/LICENSE" "$dest/LICENSE"

    if command -v zip >/dev/null 2>&1; then
        (cd "$PACKAGE_ROOT" && rm -f "$drawer.zip" && zip -qr "$drawer.zip" "$drawer")
        # Post-zip guard: the archive must hold THIS binary and no private file.
        unzip -p "$PACKAGE_ROOT/$drawer.zip" "*/telegram-test" > "$PACKAGE_ROOT/.zipbin" 2>/dev/null
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
        lhafile="$AMINET_ROOT/$AMINET_BASE.$archtag.lha"
        rm -rf "$amiwork"; mkdir -p "$amiwork"
        cp -R "$dest"/* "$amiwork"/ # -R: the package now contains data/
        rm -f "$lhafile"
        ( cd "$AMINET_ROOT" && "$LHA_BIN" a "$AMINET_BASE.$archtag.lha" "$AMINET_DRAWER" >/dev/null )
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
        if [ "$(md5of "$lhatmp/$AMINET_DRAWER/telegram-test")" != "$(md5of "$binary")" ]; then
            rm -rf "$lhatmp"
            echo "ERROR $platform: lha binary != built binary ($binary)" >&2; exit 1
        fi
        rm -rf "$lhatmp"
        write_aminet_readme "$AMINET_ROOT/$AMINET_BASE.$archtag.readme" "$archval" "$requires"
        echo "$lhafile  +  $AMINET_BASE.$archtag.readme  [Architecture: $archval]"
    fi
}

package_one "AmigaOS 3.x" "$AMIGAOS3_BINARY" "amigaos3" "amigaos3"
package_one "MorphOS" "$MORPHOS_BINARY" "morphos" "morphos"
package_one "AmigaOS 4.x" "$AMIGAOS4_BINARY" "amigaos4" "amigaos4"
package_one "AROS i386 ABIv0" "$AROS_I386_BINARY" "aros-i386" "aros-i386"
package_one "AROS x86_64" "$AROS_X86_64_BINARY" "aros-x86_64" "aros-x86_64"

# --- checksums ---------------------------------------------------------------
( cd "$PACKAGE_ROOT" && ls Telegram-*-"$DATE_STAMP".zip >/dev/null 2>&1 &&
  sha_cmd Telegram-*-"$DATE_STAMP".zip > SHA256SUMS-github.txt &&
  echo "$PACKAGE_ROOT/SHA256SUMS-github.txt" ) || true
if [ "$AMINET" = "1" ] && ls "$AMINET_ROOT"/"$AMINET_BASE".*.lha >/dev/null 2>&1; then
    ( cd "$AMINET_ROOT" && sha_cmd "$AMINET_BASE".*.lha "$AMINET_BASE".*.readme > SHA256SUMS-aminet.txt )
    echo
    echo "Aminet artifacts ready in: $AMINET_ROOT  (version $VERSION, Type comm/tcp)"
    echo "Upload (Michele): FTP main.aminet.net -> cd /new -> binary -> put each"
    echo "  $AMINET_BASE.<arch>.lha AND $AMINET_BASE.<arch>.readme (5 + 5 files)."
    echo "  Web form may be back at https://aminet.net/upload. See memory/aminet-publishing.md"
fi
