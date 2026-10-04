#!/bin/bash
# Builds the Linux UltraHLE in WSL (or on Linux):
#
#   wsl -d Ubuntu -- bash /mnt/c/Source2026/UltraHLE-main/Scripts/linux/build.sh [Debug|Release]
#
# Run it as a file from PowerShell; Git Bash mangles the /mnt path. The
# build tree is in $HOME (the /mnt/c drive is slow for many small files);
# the program lands in Build/linux/ultrahle next to the Windows exe.
set -e -o pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TYPE="${1:-Release}"
BUILD="$HOME/ultrahle-build-$TYPE"

# libpulse-dev unpacked without root (apt-get download + dpkg -x) when it
# is not installed: ~/pulsedev/root/usr
PREFIX=""
[ -d "$HOME/pulsedev/root/usr" ] && PREFIX="-DCMAKE_PREFIX_PATH=$HOME/pulsedev/root/usr"

cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$TYPE" $PREFIX
cmake --build "$BUILD" 2>&1 | tee "$BUILD/build.log"
echo "built $REPO/Build/linux/ultrahle"
