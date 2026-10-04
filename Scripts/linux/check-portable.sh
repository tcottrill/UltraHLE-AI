#!/bin/bash
# Compiles every portable UltraHLE source with Linux GCC (syntax only), so the
# core stays buildable for Linux while it is edited on Windows.
#
#   wsl -d Ubuntu -- bash /mnt/c/Source2026/UltraHLE-main/Scripts/linux/check-portable.sh
#
# Run it as a file from PowerShell; Git Bash mangles the /mnt path. One line
# per file, FAIL lines show the first error; exit code 1 if any failed.
# Windows-only files (the Win32 front end) are skipped: their Linux
# counterparts implement the same headers (ui.h, console.h, sound.h,
# padin.h, xgl_internal.h's xgl_ctx_*, bootlog.h).

cd "$(dirname "$0")/../../src" || exit 1

WINDOWS_ONLY='^(ultrahle|listview|loadsave|console|sound|bootlog|ui_win32|pad_win32|cpua|cpuanew|cpuautil)\.c$|^xgl/(xgl_wgl|xgl_window|glew)\.c$'

out=/tmp/uhle_check
rm -rf $out; mkdir -p $out
failed=0
for f in $(ls *.c *.cpp xgl/*.c zip/*.c | grep -v -E "$WINDOWS_ONLY"); do
    case $f in
        *.cpp)       cc="g++ -std=gnu++17" ;;
        disasm86.c)  cc="gcc -std=gnu89" ;;   # K&R style implicit ints
        *)           cc="gcc -std=gnu11" ;;
    esac
    log=$out/$(echo $f | tr / _).log
    if $cc -fsyntax-only -w -msse4.1 -I. -Ixgl "$f" >"$log" 2>&1; then
        echo "OK    $f"
    else
        echo "FAIL  $f: $(grep -m1 error "$log" | cut -c1-140)"
        failed=1
    fi
done
exit $failed
