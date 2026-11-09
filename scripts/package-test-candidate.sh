#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Assemble one local, five-lane validation candidate. This script never
# publishes a release. AROS media deliberately uses Rock Ridge plus Joliet;
# CDVDFS does not reliably expose the long executable name without Rock Ridge.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_ROOT="$ROOT_DIR/build/test-candidates"

usage() {
    cat >&2 <<'EOF'
Usage:
  scripts/package-test-candidate.sh <phase> <os4-label> <aros-i386-label> <aros-x64-label>

Example:
  scripts/package-test-candidate.sh phase-j TG9JOS41 TG9JI391 TG9JX641

Labels must be new, unique ISO volume names containing only A-Z, 0-9 and _.
The source tree and index must be clean. This creates local test artifacts only.
EOF
    exit 2
}

[ "$#" -eq 4 ] || usage

PHASE=$1
OS4_LABEL=$2
AROS_I386_LABEL=$3
AROS_X64_LABEL=$4

case "$PHASE" in
    ''|*[!a-z0-9-]*)
        echo "ERROR: phase must contain only lowercase letters, digits and '-'." >&2
        exit 2
        ;;
esac

validate_label() {
    label=$1
    case "$label" in
        ''|*[!A-Z0-9_]*)
            echo "ERROR: invalid ISO label '$label' (use only A-Z, 0-9 and _)." >&2
            exit 2
            ;;
    esac
    if [ "${#label}" -gt 32 ]; then
        echo "ERROR: ISO label '$label' is longer than 32 characters." >&2
        exit 2
    fi
}

validate_label "$OS4_LABEL"
validate_label "$AROS_I386_LABEL"
validate_label "$AROS_X64_LABEL"

if [ "$OS4_LABEL" = "$AROS_I386_LABEL" ] || \
   [ "$OS4_LABEL" = "$AROS_X64_LABEL" ] || \
   [ "$AROS_I386_LABEL" = "$AROS_X64_LABEL" ]; then
    echo "ERROR: every ISO label must be unique." >&2
    exit 2
fi

for tool in git file strings zip unzip mkisofs isoinfo hdiutil shasum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not found: $tool" >&2
        exit 1
    fi
done

if [ -n "$(git -C "$ROOT_DIR" status --porcelain --untracked-files=no)" ]; then
    echo "ERROR: tracked files are dirty; commit or restore them before packaging." >&2
    exit 1
fi

COMMIT_ID=$(git -C "$ROOT_DIR" rev-parse --short HEAD)
CANDIDATE_NAME="0.0.9-$PHASE-$COMMIT_ID"
DEST_DIR="$BUILD_ROOT/$CANDIDATE_NAME"

if [ -e "$DEST_DIR" ]; then
    echo "ERROR: candidate already exists: $DEST_DIR" >&2
    exit 1
fi

mkdir -p "$BUILD_ROOT"
for label in "$OS4_LABEL" "$AROS_I386_LABEL" "$AROS_X64_LABEL"; do
    previous=$(find "$BUILD_ROOT" -type f -name "$label.iso" -print -quit 2>/dev/null || true)
    if [ -n "$previous" ]; then
        echo "ERROR: ISO label '$label' was already used: $previous" >&2
        exit 1
    fi
done

WORK_ROOT=$(mktemp -d "$BUILD_ROOT/.package-$PHASE-$COMMIT_ID.XXXXXX")
trap 'rm -rf "$WORK_ROOT"' EXIT HUP INT TERM
PACKAGE_ROOT="$WORK_ROOT/packages"
FINAL_ROOT="$WORK_ROOT/$CANDIDATE_NAME"
STAMP="$PHASE-$COMMIT_ID"

AMINET=0 \
PACKAGE_ROOT="$PACKAGE_ROOT" \
DATE_STAMP="$STAMP" \
COMMIT_ID="$COMMIT_ID" \
    "$ROOT_DIR/scripts/package-human-release.sh"

mkdir -p "$FINAL_ROOT"

move_package() {
    source_name=$1
    target_name=$2
    source_dir="$PACKAGE_ROOT/$source_name"
    if [ ! -d "$source_dir" ]; then
        echo "ERROR: expected package was not built: $source_dir" >&2
        exit 1
    fi
    mv "$source_dir" "$FINAL_ROOT/$target_name"
}

move_package "Amigram-amigaos3-$STAMP" AmigaOS3
move_package "Amigram-morphos-$STAMP" MorphOS
move_package "Amigram-amigaos4-$STAMP" AmigaOS4
move_package "Amigram-aros-i386-$STAMP" AROS-i386
move_package "Amigram-aros-x86_64-$STAMP" AROS-x86_64

cat > "$FINAL_ROOT/TEST-ONLY.txt" <<EOF
Amigram $PHASE validation candidate
Build: $COMMIT_ID

LOCAL VALIDATION ONLY. This is not a release and must not be redistributed.
No Telegram session is included. First start uses the normal login flow.
EOF

for drawer in AmigaOS3 MorphOS AmigaOS4 AROS-i386 AROS-x86_64; do
    cp "$FINAL_ROOT/TEST-ONLY.txt" "$FINAL_ROOT/$drawer/TEST-ONLY.txt"
    (cd "$FINAL_ROOT" && zip -qr "$drawer.zip" "$drawer")
done

# OS4's CD filesystem accepts the macOS hybrid image. AROS CDVDFS requires
# Rock Ridge for long Amiga filenames, so never replace the two mkisofs calls
# with hdiutil makehybrid.
hdiutil makehybrid -quiet -iso -joliet \
    -iso-volume-name "$OS4_LABEL" -joliet-volume-name "$OS4_LABEL" \
    -o "$FINAL_ROOT/$OS4_LABEL.iso" "$FINAL_ROOT/AmigaOS4"
mkisofs -quiet -r -J -V "$AROS_I386_LABEL" \
    -o "$FINAL_ROOT/$AROS_I386_LABEL.iso" "$FINAL_ROOT/AROS-i386"
mkisofs -quiet -r -J -V "$AROS_X64_LABEL" \
    -o "$FINAL_ROOT/$AROS_X64_LABEL.iso" "$FINAL_ROOT/AROS-x86_64"

md5of() {
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$1"
    else
        md5sum "$1" | awk '{print $1}'
    fi
}

verify_zip() {
    drawer=$1
    archive="$FINAL_ROOT/$drawer.zip"
    extracted="$WORK_ROOT/$drawer.bin"
    unzip -p "$archive" "$drawer/Amigram" > "$extracted"
    if [ "$(md5of "$extracted")" != "$(md5of "$FINAL_ROOT/$drawer/Amigram")" ]; then
        echo "ERROR: binary mismatch in $archive" >&2
        exit 1
    fi
}

verify_iso_binary() {
    extension=$1
    iso=$2
    drawer=$3
    extracted="$WORK_ROOT/$drawer-iso.bin"
    isoinfo "$extension" -i "$iso" -x /Amigram > "$extracted"
    if [ "$(md5of "$extracted")" != "$(md5of "$FINAL_ROOT/$drawer/Amigram")" ]; then
        echo "ERROR: binary mismatch in $iso" >&2
        exit 1
    fi
}

for drawer in AmigaOS3 MorphOS AmigaOS4 AROS-i386 AROS-x86_64; do
    verify_zip "$drawer"
done

for iso in "$FINAL_ROOT/$AROS_I386_LABEL.iso" "$FINAL_ROOT/$AROS_X64_LABEL.iso"; do
    description=$(isoinfo -d -i "$iso")
    echo "$description" | grep -q "Joliet with UCS level"
    echo "$description" | grep -q "Rock Ridge signatures version"
done

verify_iso_binary -J "$FINAL_ROOT/$OS4_LABEL.iso" AmigaOS4
verify_iso_binary -R "$FINAL_ROOT/$AROS_I386_LABEL.iso" AROS-i386
verify_iso_binary -R "$FINAL_ROOT/$AROS_X64_LABEL.iso" AROS-x86_64

if find "$FINAL_ROOT" -type f -print | \
   grep -qiE 'telegram-(auth|peers|seed|password|token)|phone-code-hash'; then
    echo "ERROR: private session file found in candidate." >&2
    exit 1
fi

(
    cd "$FINAL_ROOT"
    shasum -a 256 \
        AmigaOS3.zip MorphOS.zip AmigaOS4.zip AROS-i386.zip AROS-x86_64.zip \
        "$OS4_LABEL.iso" "$AROS_I386_LABEL.iso" "$AROS_X64_LABEL.iso" \
        > SHA256SUMS.txt
)

mv "$FINAL_ROOT" "$DEST_DIR"
trap - EXIT HUP INT TERM
rm -rf "$WORK_ROOT"

echo "Candidate ready: $DEST_DIR"
echo "  OS4 ISO:       $OS4_LABEL.iso"
echo "  AROS i386 ISO: $AROS_I386_LABEL.iso (Rock Ridge + Joliet)"
echo "  AROS x64 ISO:  $AROS_X64_LABEL.iso (Rock Ridge + Joliet)"
