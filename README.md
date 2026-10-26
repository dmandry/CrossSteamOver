# CrossSteamOver

Run a Windows Steam game under CrossOver on a Mac, with the game's `steam_api64.dll`
talking to the Mac Steam client. Mac Steam stays in charge of the account, Steam Input
(DualSense adaptive triggers and vibration included), and achievements. The Mac Steam
application is not patched, and Valve's `steamclient64.dll` inside a Windows Steam
install is not replaced.

The name is CrossOver plus Steam, and it is the right size for what this is: a small
bridge, not a launcher suite.

This is the same route [VXRuntime](https://github.com/Ag3497120/VeRantyx) uses when
`VXRT_STEAM_BRIDGE=1`. VXRuntime is MIT licensed (Copyright (c) 2026 Ag3497120). The
bridge in `src/steamclient.c` is adapted from that code, so the MIT notice travels
with it. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## What you need

- A Mac with CrossOver or CrossOver Preview, and a 64-bit bottle.
- Mac Steam, installed, running, and signed in to the account that owns the game.
- Apple's command-line tools (`clang`, `make`, `python3`). No MinGW toolchain.
- `steamcmd` if the Windows build of the game is not already on disk.

The game itself is a normal Windows build: `Game.exe` and `steam_api64.dll` side by
side. You launch that exe. You do not launch it through the Windows Steam client in
the bottle.

## Build

```bash
git clone https://github.com/dmandry/CrossSteamOver.git
cd CrossSteamOver
make
```

`make` produces two files:

| File | What it is |
| --- | --- |
| `build/steamclient64.dll` | A small Windows stub. CrossOver loads it instead of Valve's client library. |
| `build/steamclient64.so` | The Mac side. It forwards calls into `steamclient.dylib` from the Mac Steam app. |

The stub is marked `Wine builtin DLL` and is linked at image base `0x1A0000000`.
That address matters: a lot of game DLLs prefer `0x180000000`, and this stub has no
relocation table, so it has to land somewhere those DLLs are not.

## Install into a bottle

Create a win64 bottle in CrossOver first (any name). Windows Steam does not have to
be installed in it.

```bash
scripts/install.sh --bottle "MyBottle"
```

The script looks for CrossOver Preview, then CrossOver, under `/Applications`. Pass
`--crossover /Applications/CrossOver.app` if yours lives somewhere else.

It copies:

- `steamclient64.dll` to the bottle's `drive_c/windows/system32/`
- the same DLL to `CrossOver.app/.../lib/wine/x86_64-windows/` so the builtin override can find it
- `steamclient64.so` to `CrossOver.app/.../lib/wine/x86_64-unix/`

It will refuse to overwrite a steamclient DLL larger than a megabyte, which is how
Valve's real library is distinguished from this stub. A CrossOver update can wipe the
copies inside the app bundle. Run `install.sh` again after an update.

By default the install also hides a DualSense (`054c:0ce6`) and a DualSense Edge
(`054c:0df2`) from Wine. Wine's HID stack misreads the Bluetooth report: face buttons
come out swapped and the right trigger does not register. With those devices hidden,
the game sees the pad only through Mac Steam Input. Skip that with
`--no-hide-controller` for a game that does not use a DualSense. Quit every program
in the bottle after changing it so Wine reloads the setting.

## Fetch the Windows game with steamcmd

Mac Steam's own library download is the macOS build. `steamcmd` can pull the Windows
depots of an app you own into a folder inside the bottle.

Install steamcmd once:

```bash
mkdir -p "$HOME/steamcmd"
cd "$HOME/steamcmd"
curl -fsSL -o steamcmd_osx.tar.gz \
  "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz"
tar -xzf steamcmd_osx.tar.gz
```

Download a Windows app straight into the bottle. `3280350` is Death Stranding 2; use
the app id of the game you own. `@sSteamCmdForcePlatformType` has to come before
`login`.

```bash
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/MyBottle"
INSTALL="$BOTTLE/drive_c/Games/3280350"
mkdir -p "$INSTALL"

"$HOME/steamcmd/steamcmd.sh" \
  +@sSteamCmdForcePlatformType windows \
  +force_install_dir "$INSTALL" \
  +login "$STEAM_USER" \
  +app_update 3280350 validate \
  +quit
```

Steam Guard will ask for a code on the first login from this machine. The Windows
files land under `drive_c/Games/3280350`, which the bottle sees as
`C:\Games\3280350\`. The game executable and `steam_api64.dll` are in that tree
(sometimes one folder down, such as `bin` or the game's own name).

If the exe exits immediately because `vcruntime140.dll` or `msvcp140.dll` is missing,
install the Visual C++ 2015–2022 x64 redistributable into the same bottle from
CrossOver. Graphics settings already stored in the bottle (`CX_GRAPHICS_BACKEND`,
`D3DM_ENABLE_METALFX`, and the rest in `cxbottle.conf`) are picked up by CrossOver's
`wine` wrapper. This project does not change them.

## Launch

Mac Steam has to be open and signed in.

```bash
scripts/launch.sh \
  --bottle "MyBottle" \
  --appid 3280350 \
  --exe "$HOME/Library/Application Support/CrossOver/Bottles/MyBottle/drive_c/Games/3280350/Game.exe"
```

If `steam_appid.txt` sits next to the exe, `--appid` can be omitted.

What the launcher does:

1. Starts a tiny holder (`rundll32 steamclient64.dll,SteamBridgeWait`) whose Windows
   process id is written to `HKCU\Software\Valve\Steam\ActiveProcess`. `steam_api64.dll`
   checks that a live process is there before it will load the client library, and it
   does that check before the game's own copy of the DLL can publish the id.
2. Points `SteamClientDll64` at `C:\windows\system32\steamclient64.dll`.
3. Starts the game with CrossOver's `--dll steamclient64=b`, which forces the builtin
   stub even if something else asks for Valve's library by path.
4. Stops the holder when the game exits.

The bridge log is `/tmp/steambridge.log`. A healthy start contains
`CreateInterface(SteamClient020)` (or `SteamClient017`), then the game's interfaces,
including `SteamInput006` when the game uses Steam Input. The game's own console log
goes to `/tmp/crosssteamover.log`.

A quick check that the Mac client answers, without starting the game:

```bash
"/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/wine" \
  --bottle "MyBottle" --dll steamclient64=b \
  rundll32.exe steamclient64.dll,SteamBridgeProbe
```

Then read `/tmp/steambridge.log`. `CreateInterface(SteamClient020)` should be a
non-null pointer.

## DualSense

With the controller hidden from Wine, buttons, sticks, and both triggers come from
Steam Input, including a game's `steaminput/steam_input_manifest.vdf` next to the
exe. Adaptive triggers and rumble are the Steam Input calls `SetDualSenseTriggerEffect`
and `TriggerVibration`. The Mac client has to be the one that opened the pad, which
is why Mac Steam stays running.

## Achievements

Some games report every trophy again on launch. Steam then shows the ones you unlocked
earlier as a batch of toasts, often when the window loses focus. The bridge reads
`~/Library/Application Support/Steam/appcache/stats` for this app id and drops a
repeat: a stat whose value is already in that cache, an achievement that already has
an unlock time, and a `StoreStats` publish when nothing new was submitted. A trophy
you actually just earned is still forwarded.

That matches games which name stats `trophy_###` or `trophy_###_stat` (Death Stranding 2
does). A game that uses different achievement API names will still publish normally.

## What this does not do

- It does not patch the Mac Steam client, and it does not install NotProton.
- It does not replace `Program Files (x86)\Steam\steamclient64.dll`.
- It does not run the game through the Windows Steam client in the bottle.
- The unix library is x86_64, the same as CrossOver's 64-bit Windows processes on Apple
  silicon. The game runs under Rosetta, which CrossOver already requires for win64 bottles.
