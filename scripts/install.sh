#!/bin/bash
# Install the built bridge into a CrossOver bottle.
# Does not replace Valve's steamclient64.dll under Program Files\Steam.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOTTLE=""
CX_APP=""
HIDE=1

usage() {
    echo "usage: install.sh --bottle NAME [--crossover /Applications/CrossOver.app] [--no-hide-controller]" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle) BOTTLE="$2"; shift 2 ;;
        --crossover) CX_APP="$2"; shift 2 ;;
        --no-hide-controller) HIDE=0; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done
[[ -n "$BOTTLE" ]] || usage

if [[ -z "$CX_APP" ]]; then
    for candidate in \
        "/Applications/CrossOver Preview.app" \
        "/Applications/CrossOver.app"
    do
        [[ -d "$candidate" ]] && CX_APP="$candidate" && break
    done
fi
[[ -n "$CX_APP" && -d "$CX_APP" ]] || { echo "CrossOver.app not found. Pass --crossover." >&2; exit 1; }

CX_ROOT="$CX_APP/Contents/SharedSupport/CrossOver"
WINE="$CX_ROOT/bin/wine"
PREFIX="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
[[ -x "$WINE" ]] || { echo "no wine at $WINE" >&2; exit 1; }
[[ -d "$PREFIX/drive_c" ]] || { echo "bottle not found: $PREFIX" >&2; exit 1; }

DLL="$ROOT/build/steamclient64.dll"
SO="$ROOT/build/steamclient64.so"
[[ -f "$DLL" && -f "$SO" ]] || { echo "run make first ($ROOT)" >&2; exit 1; }

SYS32="$PREFIX/drive_c/windows/system32/steamclient64.dll"
WIN_DLL="$CX_ROOT/lib/wine/x86_64-windows/steamclient64.dll"
UNIX_SO="$CX_ROOT/lib/wine/x86_64-unix/steamclient64.so"

# Guard the real Steam client library. Ours is a few kilobytes; Valve's is tens of megabytes.
for dest in "$SYS32" "$WIN_DLL"; do
    if [[ -f "$dest" ]] && [[ $(stat -f%z "$dest") -gt 1000000 ]]; then
        echo "refusing to overwrite a large steamclient64.dll: $dest" >&2
        exit 1
    fi
done

cp -f "$DLL" "$SYS32"
cp -f "$DLL" "$WIN_DLL"
cp -f "$SO" "$UNIX_SO"
echo "installed into bottle $BOTTLE"
echo "  $SYS32"
echo "  $WIN_DLL"
echo "  $UNIX_SO"

if [[ "$HIDE" == 1 ]]; then
    "$WINE" --bottle "$BOTTLE" regedit /s "$ROOT/scripts/hide-controller.reg"
    echo "DualSense is hidden from Wine (libScePad / XInput). Re-run with --no-hide-controller to skip that."
    echo "Quit every program in this bottle so Wine reloads that setting."
fi
