# SSSV Co-op - Project Handoff / Status Document
Updated: Aug 2026, current build 1.2.0. Purpose: resume development in a
fresh conversation with zero knowledge loss. Give this file plus the project
zip to the assistant.

## What this is
Two-player online co-op mod for Space Station Silicon Valley: Recompiled
(Cellenseres/SSSV_Recomp, an N64Recomp static-recompilation PC port).
Author: JihadiJackassStudios. End goal: play SSSV with girlfriend over
Radmin VPN. Architecture: parallel worlds - each player runs their own full
game; the remote player appears as a synced "ghost" animal. Long-term
roadmap ends in host-authoritative shared enemy simulation.

## Build system (all verified working)
- Mod (.nrm): MIPS via clang-18 + ld.lld-18 (LLVM 18.x REQUIRED, 19.1.0
  broken for MIPS), then RecompModTool (N64Recomp releases). `make
  CC=clang-18 LD=ld.lld-18 && ./RecompModTool mod.toml build`
- Native lib: plain C, zero deps. Linux: gcc -shared -fPIC. Windows:
  x86_64-w64-mingw32-gcc -shared -static-libgcc -lws2_32. Exports MUST
  include recomp_api_version=1 (uint32_t data export).
- syms/ = reference symbols from SSSVRecompSyms. Mods can call any function
  IN the port's symbol table; functions in gaps (e.g. spawn_animal at
  0x802C7F88) CANNOT be referenced and were transcribed into the mod
  instead. RECOMP_HOOK = run before function; RECOMP_PATCH = replace it.
- Mod cannot touch sockets/files; the native companion lib (loaded via
  manifest native_libraries) owns UDP + logging. Mod<->native via s16 io[240]
  array + MEM_H(offset^2 addressing) on the native side.

## Key game facts (from mkst/sssv decomp, all verified)
- gAnimalState @ 0x801D9ED8: speciesData[68][0xEC] (+0), Animal2
  animals[50] (+0x3EB0, {species_ptr, animal_ptr} pairs), Animal pool[50]
  (+0x4040, 0x3D4 each). gNumAnimalsInLevel @ 0x803D553E is the live count;
  all engine loops iterate i < count.
- Animal struct essentials: state 0x00, pos x/z/y s32<<16 @ 0x04/08/0C,
  newPos 0x10, velocities 0x1C/20/24 (PER-FRAME deltas: pos += vel),
  yRotation 0x2C, health 0x14C, unk160 land/water, movementState 0x162,
  speciesPtr 0x16C, aiFlags 0x272, gait cluster 0x2F2-0x2FA (0x2FA = stride
  divisor, SMALL number ~20-40; 0 or huge = frozen legs), heading 0x302,
  movementMode 0x366 (1=NORMAL 3=INJURED 4=dying 5=DEACTIVATED 6=DELETED),
  FOUR LimbIKState blocks 0x370/384/398/3AC (10 s16 each: unk0, unk2,
  jointA/B/C @ +4/6/8, blendA/B/C, mode, timer), tail handle 0x3C8+,
  Animal* POINTER at 0x330 (never sync).
- Species = index into speciesData; unk9C @ +0x9C of species entry indexes
  BOTH behaviour_lut AND the render dispatch switch (so a species copy with
  altered unk9C renders wrong - dead end, tried it).
- Behavior dispatch = func_80389764_79AE14 (one-liner). We RECOMP_PATCH it:
  vanilla verbatim + skip when D_803D5520 == our ghost entry. ratBehaviorMode
  static = raw 0x803F63F0.
- Pose is PROCEDURAL: per-species render functions compute limbs from the
  Animal fields at draw time. No keyframe animations exist.
- Spawning: coop_spawn_animal() in coop.c is a faithful transcription of
  the game's spawn_animal (finds DELETED slot or appends, memsets, wires
  pointers, sets context globals D_803D5520/24/28/2C/30/38/3A/3C, ground
  clamp via sample_ground_height_at_xz, water via func_80310F58_722608,
  func_802B2EA8_6C4558, mass, func_802DADA0_6EC450 (collision cell
  REGISTER), load_dynamic_tail, func_802C9BA4_6DB254).
- COLLISION PROTOCOL (source of 5 fixed crashes/hangs): registration wipes
  chain nodes in place. NEVER re-register while linked; ALWAYS
  remove_collision_list(a) first. Never write position/newPosition directly
  (engine derives newPos and cells from velocity); move via velocities.
  Never reuse a DELETED slot same-frame (cooldown 20f lets engine sweep).
- EVO pseudo-species 61/62/63/67 (microchip/transfer/evo/shellsuit) must
  never be bare-spawned (crash). 64/65/66 are real animals. Ghost spawns
  only species already present in local level roster.
- gGameState @ 0x803F2D30, level s16 @ +0x8. gCurrentAnimalIndex 0x803D5534,
  gCurrentAnimalId 0x803E9824 (species id; equals EVO ids in soul states).
- SFX: func_8032C508_73DBB8(id, 0x4000, 0, 1.0f); 144/145 = menu up/down.
- Score/health check per animal: func_80328520_739BD0. Update loop with
  render dispatch: overlay2_6D9AF0.c. Player movement + gaitPhase++:
  overlay2_6B5A40.c. Limb IK: overlay2_7312E0.c. gaitPhaseOffset derivation
  (uses 0x2FA divisor, states 3/4/6/182/183/185): overlay2_6CA7E0.c.

## Current mod behavior (1.2.0)
- Phase 0/1 DONE: UDP host/join (config: mode/ip/port + debug_logging),
  self-healing handshake+heartbeat (10s timeout), state @30Hz.
- Ghost lifecycle DONE: spawn on fresh peer state + same level +
  spawnable species (present in roster, not EVO family, stable 10 frames);
  despawn (remove_collision_list -> DELETED -> 20f cooldown) on
  leave/stale/species change; death handoff (health<=0 or mode not in
  {NORMAL,INJURED}) -> let engine play death (scores once) -> corpse
  watchdog 150f -> dizzy stars + safe delete (guard: never touch slot ==
  gCurrentAnimalIndex); own level change = forget silently.
- Motion: dead reckoning (peer velocity<<8 + delta>>3 correction); >64 unit
  jump = teleport (unlink, place, re-register).
- Pose sync ("wear your friend's skeleton"): 3 pointer-free ranges
  {0x2E8,36w} {0x334,24w} {0x368,44w} + state + movementState|attack<<8 +
  velocities = 109 words, applied ONLY on fresh packets (io[IO_PEER_AGE]==0);
  12 joint-angle words excluded from direct apply and instead blended
  cur += (target-cur)>>1 per frame. movementMode forced NORMAL every frame;
  aiFlags 0; behavior skipped via the dispatch patch. unk2FA==0 -> 32.
- Logging: [HH:MM:SS.mmm] CAT | msg; categories SYS/NET/GHOST/POSE; NET on
  change + 30s alive; 2MB cap; session banners; tools/watch-log.bat =
  color live tail. Debug mode: 2s ghost status + 1s IK fight report
  (ik-fight mask + idx:written->engine diffs for the 40 IK words).

## Open issues / next steps (in order)
1. DOG-SPECIFIC leg twitch (sheep is clean!). IK field-fight detector is
   armed; awaiting a capture: debug ON (host), both dogs, 30s walking +
   15s idle, read POSE lines. Escalation ladder: soften blend to >>2;
   blend IK unk0/unk2 words too; stop syncing IK mode/timer; if mask
   empty, hunt in dog.c for dog-only fields outside synced ranges.
2. Head-turn does not follow peer (idle look-around + blink work, directed
   turn does not). Source not yet found; not in synced ranges or IK blocks
   apparently. Suspect player-control path or 0x306-0x312 semantics.
3. Jump anticipation pose imperfect (tuck vs squash) - reevaluate after 1.
4. Phase 3: host-selected missions. init_level @ 0x802961D4 in syms;
   broadcast gGameState.level + hook to auto-load on client. Design TBD.
5. Phase 4/5: host-authoritative world mirror (suppress client behaviors
   via the same dispatch patch, mirror all slots, combat events, enemies
   target both players via the "get player position" helpers). User WANTS
   full shared enemies. Charm > completeness per stated priorities.
6. Parked: PvP damage transfer config (Off/cosmetic/full), possession-of-
   ghost experiment (never tested), Radmin polish (interpolation), death
   effect could use proper explosion instead of dizzy stars.

## Working protocol with the user
One change per build, versioned x.y.z (their scheme), both machines updated
together (protocol version enforces this), user tests + reports (their
observational reports are excellent - encourage plain language), logs +
crash dumps (minidump python package parses them; all 3 past dumps shared
the +0x185869 collision-corruption signature). Iterate. The freecam mod
(separate project, released on their GitHub) is done and stable.
