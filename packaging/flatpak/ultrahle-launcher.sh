#!/bin/sh
# UltraHLE Flatpak launcher (the manifest's command).
#
#   flatpak run io.github.tcottrill.UltraHLE                 (ROM picker)
#   flatpak run io.github.tcottrill.UltraHLE <rom> [command ; command ...]
#
# UltraHLE keeps ultra.ini, ultra.log, startup.log, saves/, states/ and snap/
# in one folder, its "home" ($ULTRAHLE_HOME, see src/linux/ui_linux.cpp).
# /app is read only, so that is the app's data folder:
#   ~/.var/app/io.github.tcottrill.UltraHLE/data/ultrahle/
# ROMs go in ~/UltraHLE/roms (made here), outside the data folder so that
# 'flatpak uninstall --delete-data' never takes them.
set -e

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/ultrahle"
APP_DATA="/app/share/ultrahle"
USER_DIR="$HOME/UltraHLE"

mkdir -p "$DATA_DIR"

# ultra.ini: a link to the packaged one, so an update brings its new game
# settings. Replace the link with a real file to keep your own edits.
if [ ! -f "$DATA_DIR/ultra.ini" ] || [ -L "$DATA_DIR/ultra.ini" ]; then
    ln -sfn "$APP_DATA/ultra.ini" "$DATA_DIR/ultra.ini"
fi

mkdir -p "$USER_DIR/roms"
# rewritten each start, so it describes the installed version
cat > "$USER_DIR/README.txt" <<'EOF'
UltraHLE
========
Put N64 ROMs (.z64, .v64, .n64 or .zip) in roms/. Starting UltraHLE
without a ROM shows them in a list; add it to Steam as a non-Steam game
(command: flatpak run io.github.tcottrill.UltraHLE) to choose games with
the controller in game mode. A ROM can also be given directly:

    flatpak run io.github.tcottrill.UltraHLE ~/UltraHLE/roms/<game>.z64

In the list: up/down (d-pad or stick) choose, LB/RB a page at a time,
A or Start plays, B goes back to the game you left, Y looks for new ROMs,
Back+Start quits. Keyboard: arrows, Page Up/Down, Enter, Backspace,
F5, Esc.

In a game (Back is the View button on the Steam Deck):
Back+Start back to the list (quit when the ROM was given directly),
Back+RB quicksave, Back+LB quickload, Back+Y screenshot, Back+X fullscreen.
Keys: Esc back to the list, F7 pause, F6 quicksave, F9 quickload,
F10 screenshot, F11 or Alt+Enter fullscreen.

Saves, save states, screenshots and the logs are in
~/.var/app/io.github.tcottrill.UltraHLE/data/ultrahle/
(ultra.log here is a link to the newest log).
EOF
ln -sfn "$DATA_DIR/ultra.log" "$USER_DIR/ultra.log" 2>/dev/null || true

export ULTRAHLE_HOME="$DATA_DIR"
export ULTRAHLE_ASSETS="$APP_DATA/assets"
export ULTRAHLE_ROMS="$USER_DIR/roms"   # the ROM picker's folder
exec /app/bin/ultrahle "$@"
