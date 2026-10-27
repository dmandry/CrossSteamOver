#!/bin/bash
# Build a double-clickable Mac app for one Windows game and put it on the Desktop.
# The icon is taken from the exe unless --icon is given.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE=""
EXE=""
APP_ID=""
NAME=""
ICON=""
DEST=""

usage() {
    echo "usage: desktop-icon.sh --bottle NAME --exe /path/to/Game.exe [--appid N] [--name \"Title\"] [--icon file.png] [--output ~/Desktop/Title.app]" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle) BOTTLE="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --appid) APP_ID="$2"; shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        --icon) ICON="$2"; shift 2 ;;
        --output) DEST="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done
[[ -n "$BOTTLE" && -n "$EXE" && -f "$EXE" ]] || usage

if [[ -z "$APP_ID" && -f "$(dirname "$EXE")/steam_appid.txt" ]]; then
    APP_ID="$(tr -dc 0-9 < "$(dirname "$EXE")/steam_appid.txt")"
fi
[[ -n "$APP_ID" ]] || { echo "pass --appid or put steam_appid.txt next to the exe" >&2; exit 1; }

if [[ -z "$NAME" ]]; then
    NAME="$(basename "$EXE")"
    NAME="${NAME%.exe}"
    NAME="${NAME%.EXE}"
fi

safe="$(printf '%s' "$NAME" | tr '/:' '__')"
if [[ -z "$DEST" ]]; then
    DEST="$HOME/Desktop/${safe}.app"
fi
[[ "$DEST" == *.app ]] || DEST="${DEST}.app"

if [[ -e "$DEST" ]]; then
    if [[ ! -f "$DEST/Contents/Resources/crosssteamover-launcher" ]]; then
        echo "refusing to replace $DEST (it was not made by this script)" >&2
        exit 1
    fi
    rm -rf "$DEST"
fi

mkdir -p "$DEST/Contents/MacOS" "$DEST/Contents/Resources"
: > "$DEST/Contents/Resources/crosssteamover-launcher"

png="$(mktemp -t crosssteamover-icon).png"
iconset_parent=""
cleanup_tmp() {
    rm -f "$png"
    [[ -n "$iconset_parent" ]] && rm -rf "$iconset_parent"
}
trap cleanup_tmp EXIT

got_icon=0
if [[ -n "$ICON" ]]; then
    sips -s format png "$ICON" --out "$png" >/dev/null
    got_icon=1
elif python3 "$ROOT/tools/extract_exe_icon.py" "$EXE" "$png" >/dev/null; then
    got_icon=1
else
    echo "no icon in $(basename "$EXE"); the Desktop app will use a generic icon" >&2
fi

if [[ "$got_icon" == 1 ]]; then
    iconset_parent="$(mktemp -d -t crosssteamover-iconset)"
    iconset="$iconset_parent/AppIcon.iconset"
    mkdir -p "$iconset"
    for spec in 16:icon_16x16.png 32:icon_16x16@2x.png 32:icon_32x32.png 64:icon_32x32@2x.png \
                128:icon_128x128.png 256:icon_128x128@2x.png 256:icon_256x256.png 512:icon_256x256@2x.png \
                512:icon_512x512.png 1024:icon_512x512@2x.png; do
        px="${spec%%:*}"
        file="${spec#*:}"
        sips -z "$px" "$px" "$png" --out "$iconset/$file" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$DEST/Contents/Resources/AppIcon.icns"
fi

ident="com.crosssteamover.$(printf '%s' "$BOTTLE" | tr -c 'A-Za-z0-9' '-').${APP_ID}"
plist_name="$(python3 -c 'import html,sys; print(html.escape(sys.argv[1], quote=False))' "$NAME")"
cat > "$DEST/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>${plist_name}</string>
  <key>CFBundleDisplayName</key>
  <string>${plist_name}</string>
  <key>CFBundleIdentifier</key>
  <string>${ident}</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>CFBundleShortVersionString</key>
  <string>1</string>
  <key>CFBundleExecutable</key>
  <string>launch</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>LSMinimumSystemVersion</key>
  <string>12.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
PLIST

# macOS draws a prohibit sign on an app whose executable is a shell script.
# The script lives in Resources; Contents/MacOS/launch is a real program that runs it.
python3 - "$DEST/Contents/Resources/run.sh" "$ROOT/scripts/launch.sh" "$BOTTLE" "$EXE" "$APP_ID" "$NAME" << 'PY'
import shlex
import sys
path, launch, bottle, exe, appid, name = sys.argv[1:]
safe = name.replace("\\", "\\\\").replace('"', '\\"')
script = """#!/bin/bash
# Built by CrossSteamOver scripts/desktop-icon.sh. Double-click starts the game.
err="$(mktemp -t crosssteamover-launch)"
if ! %s --bottle %s --exe %s --appid %s >"$err" 2>&1; then
    msg="$(tail -n 12 "$err" | tr '\\n"\\\\' '   ')"
    osascript -e "display alert \\"Could not start %s\\" message \\"$msg\\"" >/dev/null 2>&1 || true
    rm -f "$err"
    exit 1
fi
rm -f "$err"
""" % (
    shlex.quote(launch),
    shlex.quote(bottle),
    shlex.quote(exe),
    shlex.quote(appid),
    safe,
)
open(path, "w").write(script)
PY
chmod +x "$DEST/Contents/Resources/run.sh"
runner="$(mktemp -t crosssteamover-runner).c"
cat > "$runner" << CSOURCE
#include <unistd.h>
#include <stdio.h>
int main(void) {
    execl("/bin/bash", "bash", "$DEST/Contents/Resources/run.sh", (char *)0);
    perror("CrossSteamOver");
    return 1;
}
CSOURCE
clang -arch arm64 -arch x86_64 -O2 -o "$DEST/Contents/MacOS/launch" "$runner"
rm -f "$runner"
codesign --force --sign - "$DEST" >/dev/null
xattr -cr "$DEST"
echo "$DEST"
