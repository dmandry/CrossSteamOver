#!/bin/bash
# Launch a Windows game exe so its steam_api64.dll talks to the Mac Steam client.
# Mac Steam must already be running and signed in. This does not start Windows Steam.
set -euo pipefail

BOTTLE="${CROSSSTEAM_BOTTLE:-}"
APP_ID="${CROSSSTEAM_APP_ID:-}"
EXE=""

usage() {
    echo "usage: launch.sh --bottle NAME --exe /path/to/Game.exe [--appid N]" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bottle) BOTTLE="$2"; shift 2 ;;
        --exe) EXE="$2"; shift 2 ;;
        --appid) APP_ID="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done
[[ -n "$BOTTLE" && -n "$EXE" && -f "$EXE" ]] || usage

if [[ -z "$APP_ID" && -f "$(dirname "$EXE")/steam_appid.txt" ]]; then
    APP_ID="$(tr -dc 0-9 < "$(dirname "$EXE")/steam_appid.txt")"
fi
[[ -n "$APP_ID" ]] || { echo "pass --appid or put steam_appid.txt next to the exe" >&2; exit 1; }

CX_APP=""
for candidate in "/Applications/CrossOver Preview.app" "/Applications/CrossOver.app"; do
    [[ -d "$candidate" ]] && CX_APP="$candidate" && break
done
[[ -n "${CROSSSTEAM_CROSSOVER:-}" ]] && CX_APP="$CROSSSTEAM_CROSSOVER"
WINE="$CX_APP/Contents/SharedSupport/CrossOver/bin/wine"
[[ -x "$WINE" ]] || { echo "CrossOver wine not found" >&2; exit 1; }

if ! pgrep -f "Steam.AppBundle/Steam/Contents/MacOS/steam_osx" >/dev/null; then
    echo "Mac Steam is not running. Open Steam and sign in, then run this again." >&2
    exit 1
fi

export SteamAppId="$APP_ID"
export SteamGameId="$APP_ID"
LOG="${CROSSSTEAM_LOG:-/tmp/crosssteamover.log}"
rm -f /tmp/steambridge.log

"$WINE" --bottle "$BOTTLE" --dll steamclient64=b rundll32.exe steamclient64.dll,SteamBridgeWait >/tmp/crosssteamover-holder.log 2>&1 &
HOLDER=$!
PID=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    PID="$(sed -n 's/ActiveProcess pid //p' /tmp/steambridge.log 2>/dev/null | head -1 || true)"
    if [[ -n "$PID" && "$PID" != 0 ]]; then break; fi
    if ! kill -0 "$HOLDER" 2>/dev/null; then
        echo "Steam holder exited. See /tmp/crosssteamover-holder.log" >&2
        exit 1
    fi
    sleep 0.25
done
if [[ -z "$PID" || "$PID" == 0 ]]; then
    echo "Steam holder did not publish a process id." >&2
    kill "$HOLDER" 2>/dev/null || true
    exit 1
fi

"$WINE" --bottle "$BOTTLE" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" /v pid /t REG_DWORD /d "$PID" /f >/dev/null
"$WINE" --bottle "$BOTTLE" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" /v ActiveUser /t REG_DWORD /d 1 /f >/dev/null
"$WINE" --bottle "$BOTTLE" reg add "HKCU\\Software\\Valve\\Steam\\ActiveProcess" /v SteamClientDll64 /t REG_SZ /d "C:\\windows\\system32\\steamclient64.dll" /f >/dev/null

cleanup() {
    "$WINE" --bottle "$BOTTLE" taskkill /IM rundll32.exe /F >/dev/null 2>&1 || true
    kill "$HOLDER" 2>/dev/null || true
}
trap cleanup EXIT

GAME_DIR="$(cd "$(dirname "$EXE")" && pwd)"
cd "$GAME_DIR"
echo "Starting $(basename "$EXE") (Steam app $APP_ID). Bridge log: /tmp/steambridge.log"
set +e
# macOS bash 3.2 treats "${array[@]}" as unbound when the array is empty and set -u is on.
if [[ -n "${CROSSSTEAM_DEBUG:-}" ]]; then
    "$WINE" --bottle "$BOTTLE" --dll steamclient64=b --debugmsg err+all,seh "$EXE" >"$LOG" 2>&1
else
    "$WINE" --bottle "$BOTTLE" --dll steamclient64=b "$EXE" >"$LOG" 2>&1
fi
echo "$(basename "$EXE") exited $?"
