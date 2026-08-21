# SSSV Co-op - Phase 0: Link Test

Goal of this phase: prove the whole pipeline works on your two machines
before any gameplay sync exists. Success = both machines chirp.

## Install (both machines)

Copy these two files into the mods folder on EACH machine:

- `dist/sssv_coop.nrm`
- `dist/sssv_coop_native.dll` (Windows) or `dist/sssv_coop_native.so` (Linux)

Mods folder: `%LOCALAPPDATA%\SSSVRecompiled\mods` (Windows),
`~/.config/SSSVRecompiled/mods` (Linux), or `mods/` next to the exe in
portable mode. Enable the mod in the Mods menu on both machines.

The freecam mod can stay installed; they don't conflict.

## Configure

Main PC (host):
- Mode: **Host**
- Port: 7642 (leave it)

Laptop (join):
- Mode: **Join**
- Host IP address: your main PC's LAN IP
  - Find it on the host: `ipconfig` in a terminal, use the IPv4 address of
    your active adapter (usually 192.168.x.x)
- Port: 7642

Later with your friend over Radmin: identical, except the joiner enters the
host's Radmin IP (the 26.x.x.x one shown in the Radmin window).

## Firewall (the #1 gotcha)

The HOST machine must allow inbound UDP 7642. On Windows, the simplest
reliable option (admin PowerShell on the host):

    netsh advfirewall firewall add rule name="SSSV Coop" dir=in action=allow protocol=UDP localport=7642

Windows may also pop its own allow dialog for SSSVRecompiled the first time;
allow it for private networks. The JOIN side needs no firewall changes.

## Test procedure

1. Launch the game on both machines and get INTO GAMEPLAY on both (start or
   load a file, be standing in a level or the ship). The link only pumps
   during gameplay, not on the title screen.
2. Within ~2 seconds of both being in-game you should hear a menu "up"
   chirp on BOTH machines. That's the connection.
3. Verify resilience: quit to the title screen on one machine. After ~10
   seconds the other machine should chirp "down" (link lost). Go back
   in-game and both should chirp "up" again on their own. No restarts needed.
4. Optional: pause one game for 15+ seconds - same down/up cycle proves the
   auto-reconnect.

## If it doesn't chirp

- No chirp on either machine, ever: check the mod is enabled on both, and
  that the native library file is next to the .nrm (a load error in the
  Mods menu means it isn't, or it's the wrong platform's build).
- Host never chirps, join never chirps: firewall on the host (see above),
  or wrong IP. Sanity-check connectivity with `ping <host ip>` from the
  laptop.
- Chirps once then "down" chirps repeatedly: something is eating packets
  intermittently - tell me, that's a timeout tuning issue on my end.
- Anything weird: note exactly which machine chirped what and when, and
  whether the port's console/log printed anything. That's the debug info I
  need.

## What's next (Phase 1)

Same pipeline, plus each side streams its animal's position/heading/species
every frame and the other side spawns a ghost animal in a free slot to
mirror it. The milestone: seeing each other move around the same level.
