#!/usr/bin/env bash
# Adds OpenRasa to your desktop's application menu, with its icon (dock, app overview, taskbar).
# Run it once from the game folder you unzipped OpenRasa into:
#   ./OpenRasa-install-desktop-entry.sh            install (for your user only, no root needed)
#   ./OpenRasa-install-desktop-entry.sh --remove   remove it again
# Run it again if you move the game folder: the entry points at the folder it was installed from.
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
entry="$data/applications/OpenRasa.desktop"
icons="$data/icons/hicolor"

refresh() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$data/applications" || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$icons" 2>/dev/null || true
}

if [ "${1:-}" = "--remove" ]; then
    rm -f "$entry"
    for size in 16 32 48 64 128 256; do rm -f "$icons/${size}x${size}/apps/OpenRasa.png"; done
    refresh
    echo "Removed the OpenRasa menu entry."
    exit 0
fi

[ -x "$here/OpenRasa" ] || { echo "OpenRasa was not found beside this script ($here)." >&2; exit 1; }
[ -f "$here/tabula_rasa.cfg" ] || echo "Note: $here holds no tabula_rasa.cfg - OpenRasa is meant to be unzipped into the game folder."

# The icon in the user's icon theme folder, at every size the package carries.
for size in 16 32 48 64 128 256; do
    src="$here/OpenRasa-icons/openrasa-$size.png"
    [ -f "$src" ] || continue
    mkdir -p "$icons/${size}x${size}/apps"
    cp "$src" "$icons/${size}x${size}/apps/OpenRasa.png"
done

# Exec takes the program in double quotes (the folder may hold spaces): \ " ` $ escaped inside them, and every \ doubled again
# because the desktop entry format itself reads backslash escapes.
quoted=$(printf '%s' "$here/OpenRasa" | sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g')
mkdir -p "$data/applications"
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=OpenRasa
GenericName=Tabula Rasa client
Comment=Open-source client for Tabula Rasa
Exec="$quoted"
Path=$here
Icon=OpenRasa
Terminal=false
Categories=Game;RolePlaying;
StartupWMClass=OpenRasa
EOF
chmod 644 "$entry"
refresh
echo "Added OpenRasa to the application menu ($entry)."
