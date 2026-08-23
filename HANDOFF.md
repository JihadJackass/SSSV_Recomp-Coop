# SSSV Co-op - Project Handoff / Status Document
Updated: Aug 2026, current build 1.2.0. Purpose: resume development in a
fresh conversation with zero knowledge loss. Give this file plus the project
zip to the assistant.

## How to resume (fresh conversation quickstart)
1. Read this file fully. The zip contains complete current source.
2. Rebuild environment: clone mkst/sssv (decomp reference) and
   Cellenseres/SSSV_Recomp with SSSVRecompSyms; apt install clang-18 lld-18
   gcc-mingw-w64-x86-64; download RecompModTool from
   github.com/N64Recomp/N64Recomp/releases/tag/mod-tool-release.
3. Current version 1.4.0, wire protocol v4. Bump protocol on wire changes.
4. IMMEDIATE TASK: run the deferred validation batch (item 4b below), which
   now includes Phase 3 end-to-end. Phase 3 is IMPLEMENTED, UNTESTED.
5. After validation: Phase 4/5 research (host-authoritative world mirror).

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
- MENU/SHIP FACTS (verified in decomp for Phase 3): the main loop
  (overlay2_6A6500.c func_80294E50, NOT in port syms) branches on
  gOverlayMenuState.unk0: nonzero -> func_8038FF68_7A1618 (menu machine, IN
  syms, hookable), zero -> gameplay incl. get_controller_input. So unk0 is
  the real "menu active" flag; gInitialisationState is only a 3-frame
  transition counter (menu init sets 1, loop counts to 3, then unk0=1 and
  back to 0) -- NOT an in-menu flag. gOverlayMenuState.unk18 (s16 @ +0x18)
  is the shared menu state machine: 4 = ship zone select (the rings, in
  ui_main_menu.c), 10 = mission brief; pause lives in 20/30 states; credits
  40s. Confirm on the rings does gGameState.level = D_803F7DA8.currentLevel
  + 1 (currentLevel is s8 @ D_803F7DA8+0x2D, 0-based). The one-shot load
  recipe (proven by load_smashing_start, callable from menu state 4): set
  currentLevel + gGameState.level, load_level_text_data(gEepromGlobal
  .language [s8 @ +0xE], level-1, D_803F3330, D_803F34C0), then
  func_8038FC58_7A1308() = init_level + reset_player_progress + fade +
  volume ramps. Playable levels 1..32 (SMASHING_START..SECRET_LEVEL);
  33/34 empty, 35 credits, 36 DMA_INTRO.
- Score/health check per animal: func_80328520_739BD0. Update loop with
  render dispatch: overlay2_6D9AF0.c. Player movement + gaitPhase++:
  overlay2_6B5A40.c. Limb IK: overlay2_7312E0.c. gaitPhaseOffset derivation
  (uses 0x2FA divisor, states 3/4/6/182/183/185): overlay2_6CA7E0.c.

## Current mod behavior (1.4.0)
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
- Phase 3 DONE (1.4.0, untested): host-selected missions. Proto v4 adds
  MSG_LEVEL {level,in_level,epoch} (epoch increments on every level ENTRY
  incl. replays of the same level; native sends on change + 1/s, reports
  peer trio in io[237..239], forcing in_level=0 if >5s stale; own trio out
  in io[125..127]). The frame tick now runs from TWO hooks --
  get_controller_input (gameplay) and func_8038FF68_7A1618 (menus) -- which
  are mutually exclusive branches of the main loop, so exactly once per
  frame everywhere; the link therefore no longer times out in menus.
  IO_OWN_VALID is now gated on gOverlayMenuState.unk0 == 0 so no stale
  state streams from menus (preserves old ghost-despawn-on-menu behavior).
  Join side: each unseen host epoch with in_level and level 1..32 -> if
  already in that level, latch; if on the ship at state 4 or 10, run the
  one-shot load recipe (coop_load_level) and return; else chirp once (SFX
  144) and keep it queued until the ship. Host never follows the client.
  Log lines are "MISSION: ..." (SYS category, debug tags 16/17).
- Logging: [HH:MM:SS.mmm] CAT | msg; categories SYS/NET/GHOST/POSE; NET on
  change + 30s alive; 2MB cap; session banners; tools/watch-log.bat =
  color live tail. Debug mode: 2s ghost status + 1s IK fight report
  (ik-fight mask + idx:written->engine diffs for the 40 IK words).

## Open issues / next steps (in order)
0. SOLVED (1.2.2/1.2.3): dog leg twitch/floating. Root cause was WRITE
   ORDERING: dog legs are IK-stepped by behavior-side locomotion the ghost
   never runs, so peer IK must be transplanted; but input-time writes lost
   to the engine's later idle micro-animation. Fix: hold peer's 40 IK words,
   apply them in a RECOMP_HOOK on func_80328520_739BD0 (runs per animal
   right before its render dispatch, context D_803D5520 identifies the
   ghost) = last writer before draw. Engine now only writes blend fields
   (mask 380E0380E0 = cooperative). Sheep legs are render-procedural and
   never needed this. 1.2.3 added: peer in EVO soul state freezes the
   vacated ghost body in place (ik_hold released -> engine idle anim,
   velocities zeroed) instead of despawning; species-change path replaces
   it on next possession.
2. Head-turn does not follow peer (idle look-around + blink work, directed
   turn does not). Not in synced ranges or IK blocks. Suspect player-
   control path or 0x306-0x312 semantics. Consider same late-frame apply.
2b. EVO soul visibility: 1.3.0 adds config-gated experiment "evo_ghost"
   (default Off): with no vacated body present, spawn the soul as a real
   EVO entity (species 63; spawnability gate bypassed only under the flag).
   Hypothesis: the historical EVO-spawn crash was the since-fixed collision
   corruption, not EVO-specific. If it survives -> build proper DUAL-ghost
   (vacated body frozen + soul entity simultaneously; needs 2nd slot
   tracking). If it crashes -> dump reveals missing EVO init. User goal:
   full single-player parity (body + visible soul). User is willing to
   fork/extend SSSV_Recomp itself (F4SE-style) if the mod toolkit hits a
   wall; agreed policy: fork only on proven impossibility (most likely
   candidate: Phase 4 needs). Vacated-body physics: freeze stops network
   driving only; engine physics (water bobbing, gravity, shoves) still
   runs -- verify with a water body test.
3. Jump anticipation pose imperfect (tuck vs squash) - reevaluate after 1.
4. Phase 3 host-selected missions: IMPLEMENTED in 1.4.0 (see Current mod
   behavior). Untested; part of the validation batch below. Design followed
   the v1 sketch: client auto-loads from the ship only (states 4/10), queue
   + one chirp when mid-level, epoch handles replays of the same level.
   Not done (possible later polish): a config toggle to disable following;
   an on-screen indicator of the host's level; two-way follow.

4b. == CURRENT TASK == DEFERRED VALIDATION BATCH (run now that Phase 3 is
   ready -- this is the reminder the user asked for). MUCH of this is
   one-PC testable: run two PORTABLE copies of the port on the main PC
   (separate mods/ folders), one Host one Join via 127.0.0.1; shared log
   file interleaves but lines are timestamped. Only Radmin-specific feel
   needs the laptop.
   - 1.2.3 soul-freeze: vacated body persists + idles when peer goes EVO;
     re-entry resumes; body-in-WATER still bobs (engine physics through
     freeze is the design; verify reality).
   - 1.3.0 EVO experiment: Configure > "EVO ghost (experimental)" On (both
     machines); peer floats as soul with no vacated body; either a visible
     EVO drifts (-> build dual-ghost: frozen body + soul simultaneously,
     needs 2nd slot tracking) or it crashes (-> dump names missing init).
   - Phase 3 end-to-end: host picks level from ship -> client on ship
     follows (from zone select AND from mission brief); client mid-level
     gets one chirp then follows on returning to ship; host replays the
     SAME level -> client follows again (epoch check); host picks credits/
     intro -> client ignores; client in pause is NOT yanked; link now stays
     CONNECTED through menus (no more timeout cycles in the log).
5. Phase 4/5: host-authoritative world mirror (suppress client behaviors
   via the same dispatch patch, mirror all slots, combat events, enemies
   target both players via the "get player position" helpers). User WANTS
   full shared enemies. Charm > completeness per stated priorities.
6. Parked: PvP damage transfer config (Off/cosmetic/full), possession-of-
   ghost experiment (never tested), Radmin polish (interpolation), death
   effect could use proper explosion instead of dizzy stars.

## Testing-burden note (important)
The user runs host PC + spare laptop for every test and finds constant
two-machine cycles burdensome. Therefore: batch related changes where risk
allows (relaxing the strict one-change rule when changes are independent),
prefer designs partially verifiable on one machine, only request debug
captures when they will be decisive, and consolidate test requests into
single clear checklists per session.

## Working protocol with the user
One change per build, versioned x.y.z (their scheme), both machines updated
together (protocol version enforces this), user tests + reports (their
observational reports are excellent - encourage plain language), logs +
crash dumps (minidump python package parses them; all 3 past dumps shared
the +0x185869 collision-corruption signature). Iterate. The freecam mod
(separate project, released on their GitHub) is done and stable.
