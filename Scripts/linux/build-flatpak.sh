#!/bin/bash
# Builds the UltraHLE Flatpak and exports a single-file bundle for the Steam
# Deck (adapted from AAE's scripts/linux/build-flatpak.sh):
#
#   wsl -d Ubuntu -- bash /mnt/c/Source2026/UltraHLE-main/Scripts/linux/build-flatpak.sh
#
# Produces Build/linux/ultrahle.flatpak. On the Deck:
#   flatpak install --user ultrahle.flatpak
#   flatpak run io.github.tcottrill.UltraHLE ~/UltraHLE/roms/<game>.z64
#
# As in AAE: --disable-rofiles-fuse because the FUSE overlay cannot mount
# under WSL, the build state lives in $HOME (not on the slow /mnt/c drive),
# the whole log goes to Build/linux/flatpak-build.log, and the bundle is
# copied back under a temporary name first so a half-written file never
# looks finished. Always run this as a file, never pasted into
# `wsl -- bash -c "..."`: PowerShell expands $HOME first.
set -e

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="$REPO_DIR/packaging/flatpak/io.github.tcottrill.UltraHLE.yml"
APP_ID="io.github.tcottrill.UltraHLE"
OUT_DIR="$REPO_DIR/Build/linux"
WORK="$HOME/ultrahle-flatpak"
LOG="$OUT_DIR/flatpak-build.log"

mkdir -p "$WORK" "$OUT_DIR"
cd "$WORK"

echo "=== building $APP_ID ==="
echo "    manifest: $MANIFEST"
echo "    workdir:  $WORK"
echo "    log:      $LOG"

set +e
flatpak-builder \
    --user \
    --force-clean \
    --disable-rofiles-fuse \
    --repo="$WORK/repo" \
    "$WORK/build" \
    "$MANIFEST" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e

if [ "$rc" -ne 0 ]; then
    echo
    echo "=== BUILD FAILED (exit $rc) - first real errors ==="
    grep -nE "FAILED:|error:|Error:|undefined reference|No such file or directory" "$LOG" | head -20
    echo "Full log: $LOG"
    exit "$rc"
fi

echo
echo "=== exporting bundle ===" | tee -a "$LOG"
set +e
flatpak build-bundle "$WORK/repo" "$WORK/ultrahle.flatpak" "$APP_ID" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    echo "=== BUNDLE EXPORT FAILED (exit $rc) - see $LOG ===" >&2
    exit "$rc"
fi

TMP="$OUT_DIR/ultrahle.flatpak.new"
if ! cp "$WORK/ultrahle.flatpak" "$TMP"; then
    echo "error: could not write $TMP - is the destination open elsewhere?" >&2
    echo "       the finished bundle is still at $WORK/ultrahle.flatpak" >&2
    exit 1
fi
if ! mv -f "$TMP" "$OUT_DIR/ultrahle.flatpak"; then
    echo "error: could not replace $OUT_DIR/ultrahle.flatpak (file in use?)" >&2
    exit 1
fi

echo
echo "=== done ==="
ls -lh "$OUT_DIR/ultrahle.flatpak"
