# sco-titanlink-northstar

The Titanfall 2 (Northstar) side of sc-offline's **TitanLink** bridge: a Northstar plugin (a
native DLL, `TitanLink.dll`) and a small Northstar mod (`TitanLink`, one Squirrel server script).
With sc-offline's TitanLink built-in on the same PC, it starts a local Titanfall 2 match and moves
its pilot the way Star Citizen tells it to: you stand in Star Citizen, the match is anchored at
your feet, and V calls a Titan down next to you.

Everything stays on your PC: the two games talk through one shared-memory channel for the current
Windows user (`Local\SCO_titanlink.link`), and the match is a local one that nobody else can see
or join. Nothing here connects to an online service, and nothing here touches any anti-cheat.

MIT licensed. It's a separate project from sc-offline and sco-core and depends only on sco-core's
two MIT wire headers (below).

## Install

1. Install [Northstar](https://northstar.tf) for Titanfall 2 as usual.
2. Download the `TitanLink-northstar` artifact from this repository's latest green CI run (Actions,
   `build`) and unzip it.
3. Copy its `R2Northstar` folder over your Titanfall 2 folder's `R2Northstar` folder, so you get
   - `Titanfall2\R2Northstar\plugins\TitanLink.dll`
   - `Titanfall2\R2Northstar\mods\TitanLink\` (`mod.json`, `mod\scripts\vscripts\titanlink.nut`)
4. Start Titanfall 2 through `NorthstarLauncher.exe` once and check Northstar's log
   (`R2Northstar\logs\`) for `TITANLINK` lines such as
   `TitanLink loaded: sc_ipc wire 1, layout 0x314B4C54 v1, sco-core SDK ...`.

The mod's script lists the plugin in `PluginDependencies` and only compiles when it's loaded, so
without `TitanLink.dll` the mod does nothing at all.

## Using it with sc-offline

The Star Citizen side is in sc-offline, off by default and never in its release zip. It's in the
**bridges** build (CMake `-DSCO_BRIDGE_TITANLINK=ON`; sc-offline's CI uploads it as the
`bridges-test` artifact). See sc-offline's `docs/bridges.md`.

1. Start Star Citizen with the bridges build of sc-offline.
2. Press **F9** in game (or **Start pilot mode** in the **Titanfall** tab). sc-offline opens the
   channel and, if Titanfall 2 isn't running, starts `NorthstarLauncher.exe` from the `game`
   folder in `data\titanlink.txt`.
3. On Titanfall 2's main menu the plugin attaches (the tab shows its first message,
   `TitanLink (Northstar) attached, SDK ...`) and receives `START_MATCH`. A few seconds later it
   starts the local match (`mode` on `map`, `tdm` on `mp_coliseum` by default).
4. When the match has loaded, pilot mode runs: your position and facing go across ten times a
   second, the mouse buttons, R, F, G, Q, Y and X (and C and V in the Titan) are held as
   Titanfall's own commands, **V** calls a Titan, **E** gets in and out.
5. **F9** again ends pilot mode (held buttons are released). **Close link** in the tab ends the
   link: the plugin leaves the local match at once and returns to the main menu.

If sc-offline stops beating for 4 s (it crashed or hung), the plugin releases every button and
stops moving the pilot, but stays in the match; a new link resumes it.

## What it does, and doesn't, in Titanfall 2

**It only ever acts in the local match it started itself.** The plugin notes when it issues the
`map` command; the server that loads next in the same process is "its" match. In any other game
(a public or private server you joined, or a match you started yourself) it stays idle: no
commands, no camera, no buttons, no settings, no match start, and it tells sc-offline's tab
`in a game TitanLink didn't start`. A `START_MATCH` that arrives while you're in such a game is
refused. If a second player is ever in its match, it stops acting there too.

**Settings it changes, only for that local match.** Right before its `map` command it runs:

| Setting | Value | Why |
|---|---|---|
| `ns_auth_allow_insecure` | `1` | The local server lets its own player in without asking Northstar's master server to sign them in. Without it the local match needs an online sign-in; with it, nothing goes online. |
| `ns_report_server_to_masterserver`, `ns_report_sp_server_to_masterserver` | `0` | The local match is never listed on Northstar's public server browser. |
| `setplaylist private_match`, playlist override `max_players 1` | | A private match only you can be in. |
| `mp_gamemode`, `map` | from `START_MATCH` | Only names on the plugin's own lists of Titanfall 2's multiplayer maps and modes (`src/link.cpp`); anything else is refused. |

All of them are settings of the server *you host*: they never affect a server you join. When the
local match ends (you leave it, the link closes, or the game shuts down), the plugin puts
Northstar's defaults back (`ns_auth_allow_insecure 0`, `ns_report_server_to_masterserver 1`,
`ns_report_sp_server_to_masterserver 0`). The `max_players 1` playlist override stays until
Northstar or another private match sets its own.

In the match, the server script makes the local player invulnerable and untargetable, moves them
in noclip to where Star Citizen says you are, and keeps the match from timing out. It also sets
`cl_fovScale` from Star Citizen's field of view (1.0 to 1.7).

**`sv_cheats` is not set.** The fork this is ported from set `sv_cheats 1` only to hide
Titanfall's world (`r_drawworld 0` and friends) behind the shared picture. This version shares no
picture (follow-ups), so it needs no cheat-protected setting at all.

### Buttons

`tl_sc_state.buttons` bit *i* holds Titanfall's `+command` *i*: attack (left mouse), zoom (right
mouse), reload (R), melee (F), offhand0 (G), offhand1 (Q), offhand2 (C, in the Titan), ability
(V, in the Titan), weaponCycle (Y), useAndReload (X), the same commands the fork used.

## The wire and the SDK pin

The plugin opens the channel as the peer, with nothing but the two MIT headers from sco-core's
SDK, vendored byte for byte in `third_party/sco-sdk/`:

- `sc_ipc.h`: the sco.ipc wire (header, seqlock blocks, rings), wire version `SC_IPC_VERSION` 1;
- `sc_titanlink.h`: the TitanLink layout, `TL_LAYOUT_ID` `0x314B4C54` ("TLK1"),
  `TL_LAYOUT_VERSION` 1.

`third_party/sco-sdk/SCO_SDK_PIN` names the sco-core commit (or SDK tag) they come from. Four
checks keep both sides on the same wire:

- **CI** downloads both headers from sco-core at the pin (`tools/check-sdk.sh`) and fails if
  either differs by a byte.
- **The build** fails if the vendored headers carry another wire version, layout id, layout
  version, offset or size than `src/link.cpp` was written for (`static_assert`s).
- **Golden tests** in `tests/wire_golden.cpp` compare literal little-endian bytes for the channel
  header, ring headers/counters, seqlock headers, all five payload layouts and message framing.
  Expected sizes, offsets and IEEE-754 words are independent of the vendored declarations; do
  not regenerate them to bless a changed header. They also exercise wire/layout refusal,
  heartbeat timeout boundaries, live-epoch recovery, torn/wrong-size blocks, close behavior,
  malformed raw ring frames and valid wrap padding against the real `src/link.cpp`.
- **At run time**, `sc_ipc_attach` refuses a channel of another wire or layout version. The plugin
  then logs, once, `REFUSING to attach: sc-offline's channel speaks ...; this plugin was built for
  ...`, and doesn't attach. Update whichever side is behind.

To move to a newer SDK: copy `include/sc_ipc.h` and `include/sc_titanlink.h` from that sco-core
version into `third_party/sco-sdk/`, put its commit or tag in `SCO_SDK_PIN`, and adjust
`src/link.cpp` if the build's `static_assert`s say the layout changed.

**Hostile peer.** Everything sc-offline writes is checked before the game sees it: flags and
buttons are masked to the known bits, every coordinate must be finite and within +-1,000,000
(units/s for velocity within +-100,000), pitch is clamped to +-89 degrees, map and mode must be
NUL-terminated `[a-z0-9_]` names on the plugin's lists, a message of the wrong size or an unknown
type is skipped (logged once per kind), only one `START_MATCH` is accepted per link, and a ring
that fails `sc_ipc.h`'s validation is ignored until the next link. `tests/link_selftest.cpp`
plays a hostile sc-offline against the link code in CI.

## Build

MSVC x64 (Visual Studio 2022 or newer with the C++ workload) and CMake 3.21+:

```text
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To run only the golden test with native Win32 mappings after configuring/building on Windows:

```text
cmake --build build --config Release --target wire_golden
ctest --test-dir build -C Release -R "^wire_golden$" --output-on-failure
```

For a single-file development check on macOS/Linux with Clang (or `CXX=g++`), including
AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
bash tools/check-sdk.sh
bash tools/test-wire-golden.sh
```

The portable runner compiles the same test and real `src/link.cpp`, but includes the explicit
**test-only, single-threaded in-process mapping/CRT shim** in `tests/portable/windows.h`. It does
not test Windows shared-memory security, cross-process behavior, scheduling or game integration;
those still require Windows/owner acceptance. MSVC builds use real Win32 APIs, never the shim.
No sco-core host code or headers beyond the two MIT wire headers are compiled by either test.

`build\Release\TitanLink.dll` is the plugin; `mod\TitanLink` is the mod. CI (`.github/workflows/
build.yml`, windows-latest) checks the SDK pin, builds, runs the self-test and golden test, and
uploads both, laid out as above, as the `TitanLink-northstar` artifact.

## How it talks to Titanfall 2

Through Northstar's plugin interface (plugins v3: `PluginId001`, `PluginCallbacks001`,
`NSSys001` for logging), which gives it a per-frame callback and the script VMs as they're
created. In the server VM it registers a few `TL_*` functions for the mod's script. To run
console commands it uses `engine.dll`'s command buffer, and for Squirrel `server.dll`'s script
API, at the same addresses NorthstarLauncher itself binds (`primedev/engine/r2engine.cpp`,
`primedev/squirrel/squirrel.cpp`). The camera uses the client's view-angle setter, which Northstar
doesn't bind, so its first bytes are checked before use; on another `engine.dll` the camera just
doesn't follow, and the log says so. A fault in the per-frame update stops the plugin for the
session (logged) instead of retrying.

## Not ported yet (follow-ups)

- **The shared picture** (`tl_frame` textures). The fork hooked Titanfall 2's Direct3D 11 swap
  chain and device context (vtable patches on `Present`, `ClearDepthStencilView`,
  `OMSetRenderTargets`), cut the match out of its depth buffer with a shader, and shared two
  keyed-mutex textures. Northstar's plugin interface has no rendering callback, so that's game
  rendering hooks outside the plugin model; it needs its own design and review. Until then
  `tl_frame` carries no handles (sc-offline's overlay stays clear) and its `status` says why. With
  it would come back the world hiding (`r_drawworld` etc., which is where `sv_cheats` was needed),
  zoom matching and the Titan view checks.
- **Giving Star Citizen the keyboard focus back** when Titanfall 2's window takes it: sc-offline
  places the windows; the fork's focus juggling isn't ported.
- **Per-frame poses:** the script applies the last pose each server frame; sc-offline sends ten a
  second. Interpolating with `vel` and `time_ms` is a follow-up.
- **Restoring the previous `max_players`** playlist override after the local match.

## License

MIT, see `LICENSE`. `third_party/sco-sdk/` is sco-core's MIT interface exception, under its own
MIT notice.
