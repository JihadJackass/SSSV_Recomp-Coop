# SSSV Co-op for Space Station Silicon Valley: Recompiled

A two-player online co-op mod for
[Space Station Silicon Valley: Recompiled](https://github.com/Cellenseres/SSSV_Recomp).
Each player runs their own copy of the game; when both are in the same level,
the other player appears in your world as a live, synced animal. Works over
LAN, Radmin VPN, or any IP network.

Made for playing with a friend; both players need their own legally obtained copy of the game.


**Current state:** presence co-op with animation sync and host-selected
missions. You see each other move (and walk, and jump) through the level in
real time, each playing your own copy of the mission; when the host picks a
mission on the ship, the other player loads it automatically. Shared world
state is on the roadmap below.

## Features

- See the other player in your level as their currently possessed animal,
  position, facing, and full skeleton pose synced at 30Hz
- Host-selected missions: when the host enters a level, the joining player
  follows automatically from the ship's mission select (with a chirp and a
  queued follow if they are mid-level at the time)
- Possession-aware: when they jump into a new animal, their ghost changes
  species; while they are the floating EVO soul between animals, the ghost
  vanishes (fittingly) and returns on the next possession
- Ghosts spawn via the game's own animal spawner into a free slot, so level
  animals, objectives, and anything you need to herd or fight are never
  touched
- Self-healing connection: heartbeats, timeouts, and automatic reconnection;
  menus, pauses, and level loads only suspend the link, never break it
- Audible link status: a menu "up" chirp when the connection establishes, a
  "down" chirp if it drops
- All networking configured in-game, no files to edit

## Requirements

- Two machines, each with its own legally obtained US 1.0 ROM and
  SSSV Recompiled v0.1.2 or later
- Windows or Linux (prebuilt); macOS requires building the small native
  library (one command, below)
- Any IP connectivity between the machines: same LAN, Radmin VPN, or other
  VPN/tunnel

This mod contains no game assets or game code.

## Installation

On EACH machine, copy these two files into the mods folder, side by side:

| File | What it is |
|---|---|
| `sssv_coop.nrm` | The mod |
| `sssv_coop_native.dll` / `.so` | Networking companion library for your platform |

Mods folder: `%LOCALAPPDATA%\SSSVRecompiled\mods` (Windows),
`~/.config/SSSVRecompiled/mods` (Linux), or `mods/` next to the exe in
portable mode. Enable the mod in the in-game Mods menu on both machines.

The host machine's firewall must allow inbound UDP on the chosen port.
Windows (admin PowerShell, one time):

    netsh advfirewall firewall add rule name="SSSV Coop" dir=in action=allow protocol=UDP localport=7642

The joining machine needs no firewall changes.

## Configuration

Open Mods, select the mod, then Configure:

| Option | Default | What it does |
|---|---|---|
| Mode | Off | Off disables networking. One player picks Host, the other Join. |
| Host IP address | 192.168.1.50 | Join mode only: the host's IP. LAN: the host's IPv4 from `ipconfig`. Radmin: the host's 26.x.x.x address. |
| UDP port | 7642 | Must match on both machines. |

## Playing together

1. Both machines in-game (the link only runs during gameplay, not on the
   title screen). Both chirp when connected.
2. The host picks the mission. The joining player follows automatically
   the moment they are on the ship (zone select or mission brief screen);
   if they are mid-level, they hear a chirp and follow as soon as they
   return to the ship. Loading the same level manually still works too.
3. Once you are both inside your starting animals, the other player appears.
   Each of you plays your own copy of the mission; objectives and collectibles
   are per-player at this stage.
4. If one player leaves the level or disconnects, their ghost despawns after
   a couple of seconds and returns when they are back.

## Troubleshooting

Everything logs to `coop_log.txt` in the SSSVRecompiled config folder
(`%LOCALAPPDATA%\SSSVRecompiled` on Windows, `~/.config/SSSVRecompiled` on
Linux). Reading it top down:

- `sssv_coop_native loaded`: the mod and library loaded. If the file never
  appears, the library is missing from the mods folder or the mod is
  disabled.
- `first call: mode=... ip=...`: the per-frame hook and config are working;
  check the values match what you set.
- `HOST: listening` but never `HELLO from ...`: packets are not arriving.
  Firewall on the host, or wrong IP on the joiner. `netstat -ano | findstr
  7642` on the host shows whether the socket is up.
- `TIMEOUT ... dropping` followed by a reconnect is normal when either player
  sits in menus or loading screens.
- `DBG SPAWN/DESPAWN/TELEPORT` lines narrate the ghost's lifecycle;
  the periodic `ghost_*` lines are its live state.
- `MISSION:` lines narrate host-selected missions: what the host picked and
  when the joiner follows.

Tip: you can test co-op alone on one PC by running two portable copies of
SSSV Recompiled (each with its own `mods/` folder next to the exe), one set
to Host and one set to Join with IP 127.0.0.1. Their log lines interleave in
the shared log file but every line is timestamped.

## Building from source

The mod (`.nrm`) needs clang with MIPS support (LLVM 18.x; 19.1.0's release
binaries have broken MIPS support), `ld.lld`, and
[RecompModTool](https://github.com/N64Recomp/N64Recomp/releases/tag/mod-tool-release):

    make CC=clang-18 LD=ld.lld-18
    ./RecompModTool mod.toml build

The native library is a single C file with no build dependencies:

    # Linux
    gcc -shared -fPIC -O2 -o sssv_coop_native.so native/sssv_coop_native.c

    # macOS
    clang -dynamiclib -O2 -o sssv_coop_native.dylib native/sssv_coop_native.c

    # Windows (MSVC developer prompt)
    cl /LD /O2 native\sssv_coop_native.c /Fe:sssv_coop_native.dll ws2_32.lib

    # Windows cross-compile from Linux
    x86_64-w64-mingw32-gcc -shared -O2 -o sssv_coop_native.dll native/sssv_coop_native.c -static-libgcc -lws2_32

The `syms/` folder holds the reference symbol files from the port's
SSSVRecompSyms; if the port updates, replace them and rebuild.

## How it works

Each game streams its possessed animal's level, species, position, and
heading at 30Hz over UDP (a few hundred bytes per second). When the peer's
state is fresh and their level matches yours, the mod spawns their animal
using a transcription of the game's own spawn function into a free slot of
the 50-animal roster, then moves it every frame by writing velocities and
letting the engine integrate, so physics and collision bookkeeping stay
consistent. The ghost is kept in the game's bare-spawn injured state with AI
flags cleared, which renders and collides but never runs behavior logic.
Species are only spawned if they already exist in your level's roster
(guaranteeing assets are loaded) and never for the EVO pseudo-species.

The recompiled mod code cannot touch sockets, so a small native companion
library (loaded via the mod manifest) owns the UDP socket, the
handshake/heartbeat protocol, and the state exchange, and writes the debug
log.

All struct offsets and function symbols were verified against the mkst/sssv
decompilation and the port's shipped symbol files.

## Known limitations

- Objectives, collectibles, enemies, and switches are per-player; you are
  playing parallel copies of the level together, not one shared simulation
  yet.
- Mission follow is one-way (host leads); the joiner is only pulled in from
  the ship, never yanked out of a level they are playing.
- Ghost motion is tuned for LAN; expect a little more rubber-banding over
  Radmin until interpolation lands.

## Roadmap

1. Motion smoothing for internet latencies
2. Shared world state: synced switches and collectibles, then
   host-authoritative enemies and combat

## Credits

- [mkst](https://github.com/mkst/sssv) and the SSSV decompilation
  contributors, whose reverse engineering makes this mod possible
- [Cellenseres](https://github.com/Cellenseres/SSSV_Recomp) for the SSSV
  Recompiled port and its symbol files
- [Mr-Wiseguy and the N64Recomp project](https://github.com/N64Recomp/N64Recomp)
  for the recompilation and modding toolchain
- The [Zelda64Recomp mod template](https://github.com/Zelda64Recomp/MMRecompModTemplate),
  which the build setup is adapted from
