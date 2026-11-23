#!/bin/sh
# Builds the signed update of a release, for Amigram's own update
# (include/tg_update.h): one program per platform that can update itself
# and the manifest that names them, signed with the release key.
#
#   sh scripts/update-manifest.sh <version> [private-key.pem]
#
# The key defaults to ~/.config/amigram-release/update-key.pem and must be
# the one whose public half is in include/tg_update_key.h. Output goes to
# build/human-releases/update-<version>/: post the programs in the update
# channel first and the manifest last, then the "Amigram <version>"
# announcement. The binaries are the ones the packaging takes (same
# variables to point elsewhere). The 68000 text client is left out: it
# has no window to update itself from.
set -eu
cd "$(dirname "$0")/.." || exit 1
VERSION=${1:?usage: sh scripts/update-manifest.sh <version> [key.pem]}
KEY=${2:-"$HOME/.config/amigram-release/update-key.pem"}
OUT=build/human-releases/update-$VERSION

case "$VERSION" in
    *[!0-9.]*|.*|"") echo "bad version: $VERSION" >&2; exit 1 ;;
esac
[ -r "$KEY" ] || { echo "no private key at $KEY" >&2; exit 1; }

# The key that signs must be the one the clients check.
want=$(openssl rsa -in "$KEY" -noout -modulus 2>/dev/null | sed 's/Modulus=//' | tr 'A-F' 'a-f')
have=$(sed -n '/tg_update_release_modulus/,/};/p' include/tg_update_key.h | grep -o '0x[0-9a-f][0-9a-f]' | sed 's/0x//' | tr -d '\n')
[ -n "$want" ] && [ "$want" = "$have" ] || {
    echo "the key does not match include/tg_update_key.h" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
manifest="$OUT/Amigram-update-$VERSION.txt"
printf 'Amigram update 1\nversion %s\n' "$VERSION" > "$manifest.part"

lane() { # $1 lane, $2 binary
    bin=$2
    [ -f "$bin" ] || { echo "missing $bin" >&2; exit 1; }
    strings "$bin" | grep -q "Amigram $VERSION" || {
        echo "$bin does not say Amigram $VERSION" >&2; exit 1; }
    name="Amigram-$VERSION-$1"
    cp -p "$bin" "$OUT/$name"
    size=$(wc -c < "$OUT/$name" | tr -d ' ')
    sum=$(shasum -a 256 "$OUT/$name" | cut -d' ' -f1)
    printf '%s %s %s %s\n' "$1" "$name" "$size" "$sum" >> "$manifest.part"
}
lane os3 "${AMIGAOS3_BINARY:-build/amigaos3-clib2/Amigram}"
lane os4 "${AMIGAOS4_BINARY:-build/amigaos4/Amigram}"
lane morphos "${MORPHOS_BINARY:-build/morphos-cross/Amigram}"
lane aros-i386 "${AROS_I386_BINARY:-build/aros-i386-abiv0/Amigram}"
lane aros-x86_64 "${AROS_X86_64_BINARY:-build/aros-x86_64/Amigram}"
lane aros-aarch64 "${AROS_AARCH64_BINARY:-build/aros-aarch64/Amigram}"

openssl dgst -sha256 -sign "$KEY" -out "$OUT/.sig" "$manifest.part"
openssl rsa -in "$KEY" -pubout -out "$OUT/.pub" 2>/dev/null
openssl dgst -sha256 -verify "$OUT/.pub" -signature "$OUT/.sig" "$manifest.part" >/dev/null
{ cat "$manifest.part"; printf 'signature %s\n' "$(xxd -p "$OUT/.sig" | tr -d '\n')"; } > "$manifest"
rm -f "$manifest.part" "$OUT/.sig" "$OUT/.pub"

echo "Signed update $VERSION in $OUT. Post in the update channel, in this order:"
ls "$OUT" | grep -v "^Amigram-update-" | sed 's/^/  /'
echo "  Amigram-update-$VERSION.txt   (last)"
echo "then the announcement \"Amigram $VERSION\"."
