// SSSV Co-op - Phase 1: presence sync (see each other in the same level)
//
// Each side streams its possessed animal's state at 30Hz. When both players
// are connected AND in the same level, the remote player appears as a real
// animal ("ghost") spawned via the game's own spawn_animal() into a free
// slot -- never touching existing level animals -- and driven from network
// data with its AI suppressed every frame.
//
// Lifecycle: spawn on first fresh peer state in a matching level; respawn on
// species change (peer possessed something new); despawn (movementMode =
// DELETED, the game's own free-slot convention) when the peer leaves the
// level, goes stale, or disconnects; silently forgotten when WE change
// levels (the world reset wipes all slots anyway).

#include "modding.h"
#include "recompconfig.h"

typedef signed char        s8;
typedef unsigned char      u8;
typedef signed short       s16;
typedef unsigned short     u16;
typedef signed int         s32;
typedef unsigned int       u32;
typedef float              f32;

// ---------------------------------------------------------------------------
// Game structures (minimal mirrors; offsets verified against mkst/sssv
// include/structs.h and the port's data symbols)
// ---------------------------------------------------------------------------

typedef struct Animal {
    /* 0x000 */ u16 state;
    /* 0x002 */ u8  pad002[0x2];
    /* 0x004 */ s32 xPos;            // world coord in top 16 bits
    /* 0x008 */ s32 zPos;
    /* 0x00C */ s32 yPos;
    /* 0x010 */ s32 newXPos;
    /* 0x014 */ s32 newZPos;
    /* 0x018 */ s32 newYPos;
    /* 0x01C */ s32 xVelocity;
    /* 0x020 */ s32 zVelocity;
    /* 0x024 */ s32 yVelocity;
    /* 0x028 */ s16 unk28;
    /* 0x02A */ s16 unk2A;
    /* 0x02C */ s16 yRotation;
    /* 0x02E */ u8  pad02E[0x46 - 0x2E];
    /* 0x046 */ u16 unk46;          // runtime mass copy
    /* 0x048 */ u8  pad048[0x4F - 0x48];
    /* 0x04F */ u8  flags4F;        // bitfield byte: |= 0x1C at spawn
    /* 0x050 */ u8  pad050[0x14C - 0x50];
    /* 0x14C */ s16 health;         // Info.health
    /* 0x14E */ u8  pad14E[0x160 - 0x14E];
    /* 0x160 */ u8  unk160;         // land/water flag
    /* 0x161 */ u8  unk161;
    /* 0x162 */ u8  movementState;
    /* 0x163 */ u8  pad163[0x16C - 0x163];
    /* 0x16C */ void* speciesPtr;   // unk16C
    /* 0x170 */ u8  pad170[0x272 - 0x170];
    /* 0x272 */ u16 aiFlags;
    /* 0x274 */ u8  pad274[0x2E0 - 0x274];
    /* 0x2E0 */ s16 energy0;        // energy[0].unk0
    /* 0x2E2 */ s16 pad2E2;
    /* 0x2E4 */ s16 energy1;        // energy[1].unk0
    /* 0x2E6 */ u8  pad2E6[0x2F2 - 0x2E6];
    /* 0x2F2 */ u16 gaitPhase;
    /* 0x2F4 */ u16 unk2F4;
    /* 0x2F6 */ u16 gaitPhaseOffset;
    /* 0x2F8 */ u16 prevGaitPhaseOffset;
    /* 0x2FA */ u16 unk2FA;           // gait divisor: MUST be nonzero
    /* 0x2FC */ u8  pad2FC[0x302 - 0x2FC];
    /* 0x302 */ s16 heading;
    /* 0x304 */ s16 previousHeading;
    /* 0x306 */ u8  pad306[0x31A - 0x306];
    /* 0x31A */ s16 unk31A;
    /* 0x31C */ s32 unk31C;         // spawn timestamp
    /* 0x320 */ u8  pad320[0x365 - 0x320];
    /* 0x365 */ u8  unk365;           // current attack
    /* 0x366 */ u8  movementMode;
    /* 0x367 */ u8  pad367[0x36C - 0x367];
    /* 0x36C */ u8  unk36C;
    /* 0x36D */ u8  pad36D;
    /* 0x36E */ s8  unk36E;
    /* 0x36F */ u8  pad36F;
    /* 0x370 */ struct { s16 unk0, unk2, jointA, jointB, jointC, blendA, blendB, blendC; u16 mode; s16 timer; } ik0;
    /* 0x384 */ struct { s16 unk0, unk2, jointA, jointB, jointC, blendA, blendB, blendC; u16 mode; s16 timer; } ik1;
    /* 0x398 */ struct { s16 unk0, unk2, jointA, jointB, jointC, blendA, blendB, blendC; u16 mode; s16 timer; } ik2;
    /* 0x3AC */ struct { s16 unk0, unk2, jointA, jointB, jointC, blendA, blendB, blendC; u16 mode; s16 timer; } ik3;
    /* 0x3C0 */ u8  pad3C0[0x3CA - 0x3C0];
    /* 0x3CA */ s16 tailIndex;      // unk3C8.unk2 = load_dynamic_tail(id)
    /* 0x3CC */ u8  pad3CC[0x3D4 - 0x3CC];
} Animal;

_Static_assert(sizeof(Animal) == 0x3D4, "Animal size");
_Static_assert(__builtin_offsetof(Animal, unk46) == 0x46, "unk46 off");
_Static_assert(__builtin_offsetof(Animal, flags4F) == 0x4F, "flags4F off");
_Static_assert(__builtin_offsetof(Animal, health) == 0x14C, "health off");
_Static_assert(__builtin_offsetof(Animal, movementState) == 0x162, "movementState off");
_Static_assert(__builtin_offsetof(Animal, speciesPtr) == 0x16C, "speciesPtr off");
_Static_assert(__builtin_offsetof(Animal, aiFlags) == 0x272, "aiFlags off");
_Static_assert(__builtin_offsetof(Animal, energy0) == 0x2E0, "energy0 off");
_Static_assert(__builtin_offsetof(Animal, gaitPhase) == 0x2F2, "gaitPhase off");
_Static_assert(__builtin_offsetof(Animal, heading) == 0x302, "heading off");
_Static_assert(__builtin_offsetof(Animal, unk31A) == 0x31A, "unk31A off");
_Static_assert(__builtin_offsetof(Animal, unk2FA) == 0x2FA, "unk2FA off");
_Static_assert(__builtin_offsetof(Animal, unk365) == 0x365, "unk365 off");
_Static_assert(__builtin_offsetof(Animal, movementMode) == 0x366, "movementMode off");
_Static_assert(__builtin_offsetof(Animal, unk36C) == 0x36C, "unk36C off");
_Static_assert(__builtin_offsetof(Animal, unk36E) == 0x36E, "unk36E off");
_Static_assert(__builtin_offsetof(Animal, ik0) == 0x370, "ik0 off");
_Static_assert(__builtin_offsetof(Animal, ik3) == 0x3AC, "ik3 off");
_Static_assert(__builtin_offsetof(Animal, tailIndex) == 0x3CA, "tailIndex off");

typedef struct {
    void*   species;   // -> gAnimalState.speciesData[id]
    Animal* animal;    // -> gAnimalState.animalPool[slot]
} Animal2;

typedef struct {
    /* 0x0000 */ u8      speciesData[68][0xEC];
    /* 0x3EB0 */ Animal2 animals[50];
    /* 0x4040 */ Animal  animalPool[50];
} AnimalStateMin;

_Static_assert(__builtin_offsetof(AnimalStateMin, animals) == 0x3EB0, "animals off");
_Static_assert(__builtin_offsetof(AnimalStateMin, animalPool) == 0x4040, "pool off");

typedef struct {
    /* 0x0 */ s32 score;
    /* 0x4 */ s16 unk4;
    /* 0x6 */ s16 powercells2;
    /* 0x8 */ s16 level;
} GameStateMin;

extern AnimalStateMin gAnimalState;
extern s16 gNumAnimalsInLevel;
extern s16 gCurrentAnimalIndex;
extern s16 gCurrentAnimalId;      // species of the possessed animal
extern GameStateMin gGameState;   // gGameState is the vanilla symbol name

// ---------------------------------------------------------------------------
// Vanilla helpers (all present in the port's function table) and the
// per-animal processing-context globals that spawning must set.
// ---------------------------------------------------------------------------
extern void memset_bytes(u8* dst, u8 value, s32 size);
extern s32  func_801282C4(void);                       // global timestamp
extern s32  sample_ground_height_at_xz(s16 x, s16 z);
extern s32  func_80310F58_722608(s16 x, s16 z);        // water level probe
extern void func_802B2EA8_6C4558(void);                // context-based init
extern void func_802DADA0_6EC450(Animal* a);           // collision/cell registration
extern void remove_collision_list(Animal* a);          // cell UNregistration
extern s16  load_dynamic_tail(s16 id);
extern void func_802C9BA4_6DB254(Animal* a);           // per-animal init
extern void spawn_dizzy_stars_big(void);               // context-driven effect
extern void (*behaviour_lut[])(void);                  // species behavior table

extern Animal2* D_803D5520;   // current entry being processed
extern void*    D_803D5524;   // its species data
extern Animal*  D_803D5528;
extern Animal*  D_803D552C;
extern Animal*  D_803D5530;
extern s16      D_803D5538;   // "is the possessed animal" flag
extern s16      D_803D553A;
extern s16      D_803D553C;   // slot being processed

#define MOVEMENT_MODE_NORMAL  1
#define MOVEMENT_MODE_INJURED 3
#define MOVEMENT_MODE_DELETED 6

// Transcription of the game's spawn_animal() (overlay2_6D9330.c) -- the
// function itself sits in a gap of the port's symbol table so we cannot call
// it, but every helper it uses is callable. Follows the arg6 == 0 path
// (no scripted-spawn camera call) with health > 0.
static Animal2* coop_spawn_animal(s16 x, s16 z, s16 y, s16 rotation, s16 health, s16 id) {
    Animal* a;
    s16 slot;
    s16 i;
    s32 h;
    u8  maxHealth;

    for (i = 0; i < gNumAnimalsInLevel; i++) {
        if (gAnimalState.animals[i].animal->movementMode == MOVEMENT_MODE_DELETED) break;
    }
    if (i == gNumAnimalsInLevel) {
        slot = gNumAnimalsInLevel;
        gNumAnimalsInLevel++;
    } else {
        slot = i;
    }

    memset_bytes((u8*)&gAnimalState.animals[slot], 0, sizeof(Animal2));
    memset_bytes((u8*)&gAnimalState.animalPool[slot], 0, sizeof(Animal));

    gAnimalState.animals[slot].species = &gAnimalState.speciesData[id][0];
    gAnimalState.animals[slot].animal  = &gAnimalState.animalPool[slot];

    D_803D5520 = &gAnimalState.animals[slot];
    D_803D5524 = &gAnimalState.speciesData[id][0];
    a = &gAnimalState.animalPool[slot];
    D_803D5530 = a;
    D_803D552C = a;
    D_803D5528 = a;
    D_803D5538 = (slot == gCurrentAnimalIndex) ? 1 : 0;
    D_803D553C = slot;
    D_803D553A = 0;

    a->movementMode = MOVEMENT_MODE_INJURED;  // arg6==0, health>0 path
    a->unk31A = 0;
    a->yRotation = rotation;
    a->heading = rotation;
    a->gaitPhase = 0;
    a->unk2F4 = 0;
    a->gaitPhaseOffset = 0;
    a->prevGaitPhaseOffset = 0;
    a->state = 2;
    a->movementState = 1;
    a->unk160 = 0;
    a->speciesPtr = D_803D5524;
    maxHealth = gAnimalState.speciesData[id][0x8A];
    a->health = (health < (s16)maxHealth) ? health : (s16)maxHealth;
    a->flags4F |= 0x1C;
    a->unk31C = func_801282C4();

    h = sample_ground_height_at_xz(x, z) >> 16;
    if (y < h) y = (s16)h;  // don't spawn below ground

    h = func_80310F58_722608(x, z) >> 16;
    if (h == 0x4000)      a->unk160 = 0;
    else if (y >= h)      a->unk160 = 2;
    else                  a->unk160 = 1;

    func_802B2EA8_6C4558();

    a->xPos = ((s32)x) << 16;
    a->zPos = ((s32)z) << 16;
    a->yPos = ((s32)y) << 16;
    a->xVelocity = 0;
    a->zVelocity = 0;
    a->yVelocity = -1;
    a->unk46 = *(u16*)&gAnimalState.speciesData[id][0x7C];  // mass

    func_802DADA0_6EC450(a);
    a->tailIndex = load_dynamic_tail(id);
    a->aiFlags = 0;  // vanilla sets a flag soup here; the ghost stays inert
    func_802C9BA4_6DB254(a);
    a->energy0 = 0x3FF;
    a->energy1 = 0x3FF;

    return &gAnimalState.animals[slot];
}

extern void func_8032C508_73DBB8(s16 sfxId, s32 volume, s32 pan, f32 pitch);
#define SFX_MENU_NAVIGATE_UP    144
#define SFX_MENU_NAVIGATE_DOWN  145

// ---------------------------------------------------------------------------
// Native link. io is 240 s16 slots shared both ways each frame:
//   [0]       out: own state valid (1/0) -- gates STATE packets
//   [1..7]    out: level, species, x, z, y, heading, yRotation
//   [8]       in:  peer state age in frames (999 = none/stale)
//   [9..15]   in:  peer level, species, x, z, y, heading, yRotation
//   [16..124] out: own pose bundle (109 words, see pose_ranges)
//   [125..127]out: own mission trio: level, in-level flag, level epoch
//   [128..236]in:  peer pose bundle
//   [237..239]in:  peer mission trio (in-level flag zeroed if stale)
// Return: connection status (3 = connected).
// ---------------------------------------------------------------------------
RECOMP_IMPORT(".", s32 SSSVCoop_Update(s32 mode, char* ip, s32 port, s16* io));
RECOMP_IMPORT(".", void SSSVCoop_Debug(s32 tag, s32 value));
RECOMP_IMPORT(".", void SSSVCoop_GhostStatus(s32 slot_mode, s32 x, s32 y, s32 species_level));
RECOMP_IMPORT(".", void SSSVCoop_IkReport(s16* cur, s16* written, s32 mask_lo, s32 mask_hi));

#define COOP_STATUS_CONNECTED 3

// Pose sync: raw copy of three pointer-free ranges of the Animal struct --
// R1 0x2E8..0x330 (missiles, gait/walk-cycle cluster, heading, head/ear
// fields), R2 0x334..0x364 (attack timer, dizziness, laughter, bounce), and
// R3 0x368..0x3C0 (misc pose bytes plus the FOUR LimbIKState blocks, one per
// leg: local-space joint angles and IK blend state, i.e. the actual visible
// skeleton). Deliberately excluded: the Animal* pointer at 0x330, the
// lifecycle-owned movementMode/unk367 at 0x366, the local tail handle at
// 0x3C8, and the unknown struct113 pair at 0x3C0. Packed words after the
// ranges: [104]=state, [105]=movementState | attack<<8; [106..108] = peer
// velocities >> 8 for dead reckoning.
#define POSE_WORDS      109
#define IO_OWN_POSE     16
#define IO_PEER_POSE    128
static const struct { u16 off; u16 words; } pose_ranges[3] = {
    { 0x2E8, 36 }, { 0x334, 24 }, { 0x368, 44 },
};

// The four LimbIKState blocks (R3 words 4..43, 40 words) are not applied at
// input time: those writes lost to the engine's later idle micro-animation
// each frame (the 1.2.x shimmer). Instead the latest packet's 40 words are
// held in ik_hold and applied by coop_pre_render_ik -- a hook on the
// per-animal pre-render function -- so the peer's skeleton gets the last
// word before drawing. (An earlier per-frame blend approach is superseded.)
// Field-fight detector (debug mode): remember exactly what we last wrote to
// the ghost's four IK blocks; any word that differs at the next frame's
// entry was rewritten by the ENGINE locally. Those are the fields the sync
// and the engine are fighting over -- i.e. the twitch, named precisely.
static u16 ik_hold[40];      // peer's four LimbIKState blocks, latest packet
static u8  ik_hold_valid = 0;
static u16 ik_written[40];
static u8  ik_written_valid = 0;
static u32 ik_fight_mask_lo = 0;   // accumulated changed-word mask (40 bits)
static u32 ik_fight_mask_hi = 0;
static s32 debug_mode = 0;
static s32 evo_ghost_experiment = 0;

#define IO_OWN_VALID   0
#define IO_OWN_LEVEL   1
#define IO_PEER_AGE    8
#define IO_PEER_LEVEL  9
#define PEER_STALE_FRAMES 60   // 2 seconds without peer state = hide ghost

// ---------------------------------------------------------------------------
// Phase 3: host-selected missions.
//
// How the ship does it (from mkst/sssv, ui_main_menu.c + overlay2_7A0DA0.c):
// the zone-select rings (menu state 4) set gGameState.level =
// D_803F7DA8.currentLevel + 1 on confirm; later states fade out and call
// func_8038FC58_7A1308() = init_level + reset_player_progress + fade-in +
// volume ramps. load_smashing_start() proves the one-shot form of that
// recipe (set level + currentLevel, load the level text bank, call
// func_8038FC58) is callable directly from menu state 4. We reuse it
// verbatim with the host's level.
//
// "On the ship" detection: gOverlayMenuState.unk0 is the menu-active flag
// (the main loop branches on it), and unk18 is the shared menu state
// machine -- 4 = zone select, 10 = mission brief (the two interactive ship
// screens). The pause menu also runs with unk0 != 0 but sits in states
// 20/30, so gating on 4/10 never yanks a player out of pause.
// (gInitialisationState is only a 3-frame transition counter, not an
// in-menu flag -- do not use it for this.)
// ---------------------------------------------------------------------------
extern s16 gLoadedMessageCount;
extern u16 D_803F3330[];              // level text: message offsets
extern s16 D_803F34C0[];              // level text: data
extern s16 load_level_text_data(s16 language, s16 level, u16* msgOffsets, s16* dst);
extern void func_8038FC58_7A1308(void);  // init_level + reset progress + fades

typedef struct {                      // struct030: level-select state
    u8 pad0[0x2C];
    s8 biome;                         // 0x2C
    s8 currentLevel;                  // 0x2D  0-based: gGameState.level - 1
    s8 bank;                          // 0x2E
    u8 pad2F[3];
    s8 previousLevel;                 // 0x32
} LevelSelect;
extern LevelSelect D_803F7DA8;
_Static_assert(__builtin_offsetof(LevelSelect, currentLevel) == 0x2D, "currentLevel off");

typedef struct {                      // struct027: overlay menu state
    s16 unk0;                         // 0x00 menu active (main loop branch)
    u8  pad2[0x18 - 0x2];
    s16 unk18;                        // 0x18 menu state machine
} OverlayMenu;
extern OverlayMenu gOverlayMenuState;
_Static_assert(__builtin_offsetof(OverlayMenu, unk18) == 0x18, "unk18 off");

typedef struct {                      // Eeprom (settings)
    u8 pad0[0xE];
    s8 language;                      // 0x0E
} EepromMin;
extern EepromMin gEepromGlobal;

#define MENU_STATE_ZONE_SELECT    4
#define MENU_STATE_MISSION_BRIEF  10
#define LEVEL_FIRST_PLAYABLE      1   // SMASHING_START
#define LEVEL_LAST_PLAYABLE       32  // SECRET_LEVEL (33/34 empty, 35 credits, 36 intro)

// io slots for the mission trio (free tails of the two pose blocks)
#define IO_OWN_MLEVEL   125
#define IO_OWN_INLEVEL  126
#define IO_OWN_EPOCH    127
#define IO_PEER_MLEVEL  237
#define IO_PEER_INLEVEL 238
#define IO_PEER_EPOCH   239

static s16 own_epoch = 0;        // increments each time WE enter a playable level
static s16 was_in_level = 0;
static s16 followed_epoch = 0;   // host level-entry epochs already acted on
static s16 prompted_epoch = 0;   // epochs we already chirped about mid-level

// The vanilla one-shot load recipe (load_smashing_start's body,
// parameterized): keep the ship UI's 0-based selection in step, set the
// level, load its text bank, then init_level + reset progress + fades.
static void coop_load_level(s16 target) {
    D_803F7DA8.currentLevel = (s8)(target - 1);
    gGameState.level = target;
    gLoadedMessageCount = load_level_text_data((s16)gEepromGlobal.language,
                                               (s16)(target - 1),
                                               D_803F3330, D_803F34C0);
    func_8038FC58_7A1308();
}

static s16  io[240];
static s32  ghost_slot = -1;
static s16  ghost_species = -1;
static s16  pending_species = -1;   // species-change damping
static s16  pending_frames = 0;
static s16  respawn_cooldown = 0;   // frames until a new spawn is allowed
static s32  dying_slot = -1;        // corpse cleanup tracking
static s16  dying_timer = 0;
static s16  last_level = -1;
static char cached_ip[64] = "";
static s32  cached_mode = 0;
static s32  cached_port = 7642;
static u32  frame_counter = 0;

static void refresh_config(void) {
    char* ip;
    s32 i;
    cached_mode = (s32)recomp_get_config_u32("mode");
    debug_mode  = (s32)recomp_get_config_u32("debug_logging");
    evo_ghost_experiment = (s32)recomp_get_config_u32("evo_ghost");
    cached_port = (s32)recomp_get_config_double("port");
    ip = recomp_get_config_string("host_ip");
    if (ip != 0) {
        for (i = 0; i < 63 && ip[i] != '\0'; i++) cached_ip[i] = ip[i];
        cached_ip[i] = '\0';
        recomp_free_config_string(ip);
    }
}

// The game's behavior dispatch is this one tiny wrapper (behaviours.c):
// it stores its argument in a file-static and calls the species' behavior.
// We replace it verbatim, with one addition: when the animal being processed
// is our ghost, skip the behavior call entirely. This lets the ghost run in
// NORMAL movement mode (required for correct rendering and the walk-cycle
// pipeline) while its AI never executes. Everything else is byte-identical
// vanilla. 0x803F63F0 is ratBehaviorMode, the wrapper's file-static (from
// the decomp's bss map; not in the port's data symbols).
RECOMP_PATCH void func_80389764_79AE14(u8 arg0) {
    *(s16*)0x803F63F0 = arg0;
    if (ghost_slot >= 0 &&
        D_803D5520 == &gAnimalState.animals[ghost_slot]) {
        return;  // ghost: no behavior
    }
    behaviour_lut[*(u16*)((u8*)D_803D5524 + 0x9C)]();
}

// Runs per animal, right before that animal's render dispatch in the main
// update loop -- i.e. AFTER the engine's own limb/idle animation writes.
// For the ghost, this is where the peer's skeleton gets the last word.
extern void func_80328520_739BD0(void);
RECOMP_HOOK("func_80328520_739BD0")
void coop_pre_render_ik(void) {
    if (ghost_slot >= 0 && ik_hold_valid &&
        D_803D5520 == &gAnimalState.animals[ghost_slot]) {
        u16* dst = (u16*)((u8*)&gAnimalState.animalPool[ghost_slot] + 0x370);
        s32 k;
        for (k = 0; k < 40; k++) {
            dst[k] = ik_hold[k];
        }
    }
}

// Forget the ghost without touching game memory (used when the world was
// reset under us, e.g. our own level change).
static void ghost_forget(void) {
    ghost_slot = -1;
    ghost_species = -1;
    ik_written_valid = 0;
    ik_hold_valid = 0;
}

// Actively remove the ghost using the game's own deletion protocol:
// unlink its collision nodes FIRST (the register function wipes nodes in
// place, so a later slot reuse would orphan live chain links -- the cause of
// the possession-switch crash), then mark the slot deleted, then hold a
// cooldown so the engine sweeps the corpse before we recycle the slot.
static void ghost_despawn(void) {
    if (ghost_slot >= 0 && ghost_slot < gNumAnimalsInLevel &&
        gAnimalState.animals[ghost_slot].animal == &gAnimalState.animalPool[ghost_slot]) {
        remove_collision_list(&gAnimalState.animalPool[ghost_slot]);
        gAnimalState.animalPool[ghost_slot].movementMode = MOVEMENT_MODE_DELETED;
        SSSVCoop_Debug(11, ghost_slot);       // despawn event
    }
    ghost_forget();
    if (respawn_cooldown < 20) respawn_cooldown = 20;
}

// A species is safe to spawn only if it's a normal animal id -- the EVO
// pseudo-species (MICROCHIP 61, TRANSFER 62, EVO 63, SHELLSUIT 67) have
// special handling and crash when bare-spawned; that was the exact failure
// in the logs (peer was the microchip, then the soul, at level start).
// King Penguin (64), Racing Tortoise (65), and Cool Cod (66) are real
// animals and stay allowed. The species must also exist among OUR level's
// current roster, guaranteeing its assets and species data are loaded here.
static s32 species_is_evo(s16 id) {
    return id == 61 || id == 62 || id == 63 || id == 67;
}

static s32 species_spawnable_here(s16 id) {
    s32 i;
    if (id < 0 || id > 67) return 0;
    if (id == 63 && evo_ghost_experiment) return 1;  // experiment: real EVO
    if (id == 61 || id == 62 || id == 63 || id == 67) return 0;  // EVO family
    for (i = 0; i < gNumAnimalsInLevel && i < 50; i++) {
        if (gAnimalState.animals[i].species == &gAnimalState.speciesData[id][0]) {
            return 1;
        }
    }
    return 0;
}

// Is our remembered ghost slot still the animal we spawned?
static s32 ghost_valid(void) {
    if (ghost_slot < 0 || ghost_slot >= gNumAnimalsInLevel) return 0;
    if (gAnimalState.animals[ghost_slot].animal != &gAnimalState.animalPool[ghost_slot]) return 0;
    if (gAnimalState.animalPool[ghost_slot].movementMode == MOVEMENT_MODE_DELETED) return 0;
    return 1;
}

// The main loop is `if (menu active) func_8038FF68_7A1618() else { ...
// get_controller_input() ... }` -- the two branches are mutually exclusive,
// so hooking BOTH runs the tick exactly once per frame everywhere: in
// levels (as before) and now also on the ship and in pause menus. That is
// what lets the client follow the host from the ship, and as a side effect
// the connection no longer times out while sitting in menus.
static void coop_tick(void);

RECOMP_HOOK("get_controller_input")
void coop_frame_update(void) { coop_tick(); }

RECOMP_HOOK("func_8038FF68_7A1618")
void coop_menu_update(void) { coop_tick(); }

static void coop_tick(void) {
    static s32 prev_connected = 0;
    s32 status, connected;
    s16 own_level;
    Animal* own;
    Animal* g;
    s32 i;

    if ((frame_counter % 30) == 0) refresh_config();
    frame_counter++;
    if (respawn_cooldown > 0) respawn_cooldown--;

    // ---- Corpse cleanup ---------------------------------------------------
    // A killed ghost's death sequence may stall partway (its AI state is
    // ours, not the game's), leaving a lingering possessable husk that leaks
    // an animal slot per kill. Give the sequence time to explode and score,
    // then finish the deletion with the safe despawn protocol. NEVER touch
    // the slot if a player has possessed the body in the meantime.
    if (dying_slot >= 0) {
        if (dying_slot >= gNumAnimalsInLevel ||
            gAnimalState.animals[dying_slot].animal != &gAnimalState.animalPool[dying_slot] ||
            gAnimalState.animalPool[dying_slot].movementMode == MOVEMENT_MODE_DELETED) {
            dying_slot = -1;  // engine cleaned it up itself (or level reset)
        } else if (dying_slot == gCurrentAnimalIndex) {
            dying_slot = -1;  // a player possessed the corpse: hands off forever
        } else if (dying_timer > 0) {
            dying_timer--;
        } else {
            Animal2* save20; void* save24; Animal *save28, *save2C, *save30;
            Animal* corpse = &gAnimalState.animalPool[dying_slot];
            // A little send-off so the body doesn't just blink out: run the
            // context-driven dizzy-stars effect at the corpse, then delete.
            save20 = D_803D5520; save24 = D_803D5524; save28 = D_803D5528;
            save2C = D_803D552C; save30 = D_803D5530;
            D_803D5520 = &gAnimalState.animals[dying_slot];
            D_803D5524 = gAnimalState.animals[dying_slot].species;
            D_803D5528 = corpse; D_803D552C = corpse; D_803D5530 = corpse;
            spawn_dizzy_stars_big();
            D_803D5520 = save20; D_803D5524 = save24; D_803D5528 = save28;
            D_803D552C = save2C; D_803D5530 = save30;
            remove_collision_list(corpse);
            corpse->movementMode = MOVEMENT_MODE_DELETED;
            SSSVCoop_Debug(15, dying_slot);   // corpse cleanup event
            dying_slot = -1;
        }
    }

    // ---- Gather own state -------------------------------------------------
    // Own state is only "valid" (streamed as a STATE packet) while actually
    // in-level: with the tick now also running in menus, streaming there
    // would send stale animal state under the old level id and leave our
    // ghost lingering in the peer's world. Menu = invalid = the peer's
    // stale-age despawn handles it, exactly like the pre-menu-hook builds.
    own_level = gGameState.level;
    io[IO_OWN_VALID] = 0;
    i = gCurrentAnimalIndex;
    if (gOverlayMenuState.unk0 == 0 &&
        i >= 0 && i < 50 && i < gNumAnimalsInLevel) {
        own = gAnimalState.animals[i].animal;
        if (own != 0) {
            io[IO_OWN_VALID] = 1;
            io[1] = own_level;
            io[2] = gCurrentAnimalId;
            io[3] = (s16)(own->xPos >> 16);
            io[4] = (s16)(own->zPos >> 16);
            io[5] = (s16)(own->yPos >> 16);
            io[6] = own->heading;
            io[7] = own->yRotation;
            {
                s32 w = 0, r, k;
                for (r = 0; r < 3; r++) {
                    u16* src = (u16*)((u8*)own + pose_ranges[r].off);
                    for (k = 0; k < pose_ranges[r].words; k++) {
                        io[IO_OWN_POSE + w++] = (s16)src[k];
                    }
                }
                io[IO_OWN_POSE + 104] = (s16)own->state;
                io[IO_OWN_POSE + 105] = (s16)(own->movementState | ((u16)own->unk365 << 8));
                io[IO_OWN_POSE + 106] = (s16)(own->xVelocity >> 8);
                io[IO_OWN_POSE + 107] = (s16)(own->zVelocity >> 8);
                io[IO_OWN_POSE + 108] = (s16)(own->yVelocity >> 8);
            }
        }
    }

    // ---- Pump network -----------------------------------------------------
    status = SSSVCoop_Update(cached_mode, cached_ip, cached_port, io);
    connected = (status == COOP_STATUS_CONNECTED) ? 1 : 0;
    if (connected && !prev_connected) {
        func_8032C508_73DBB8(SFX_MENU_NAVIGATE_UP, 0x4000, 0, 1.0f);
    } else if (!connected && prev_connected) {
        func_8032C508_73DBB8(SFX_MENU_NAVIGATE_DOWN, 0x4000, 0, 1.0f);
    }
    prev_connected = connected;

    // ---- Phase 3: host-selected missions ----------------------------------
    {
        s32 in_level = (gOverlayMenuState.unk0 == 0) &&
                       own_level >= LEVEL_FIRST_PLAYABLE &&
                       own_level <= LEVEL_LAST_PLAYABLE;
        if (in_level && !was_in_level) own_epoch++;   // each level ENTRY,
        was_in_level = (s16)in_level;                 // including replays
        io[IO_OWN_MLEVEL]  = own_level;
        io[IO_OWN_INLEVEL] = (s16)in_level;
        io[IO_OWN_EPOCH]   = own_epoch;

        // Join side only: each host level entry (epoch) is handled once.
        if (cached_mode == 2 && connected &&
            io[IO_PEER_INLEVEL] && io[IO_PEER_EPOCH] != followed_epoch) {
            s16 target = io[IO_PEER_MLEVEL];
            if (target < LEVEL_FIRST_PLAYABLE || target > LEVEL_LAST_PLAYABLE) {
                followed_epoch = io[IO_PEER_EPOCH];   // credits/intro: ignore
            } else if (in_level && own_level == target) {
                followed_epoch = io[IO_PEER_EPOCH];   // already together
            } else if (gOverlayMenuState.unk0 != 0 &&
                       (gOverlayMenuState.unk18 == MENU_STATE_ZONE_SELECT ||
                        gOverlayMenuState.unk18 == MENU_STATE_MISSION_BRIEF)) {
                // On the ship at an interactive screen: follow the host.
                followed_epoch = io[IO_PEER_EPOCH];
                SSSVCoop_Debug(16, target);
                coop_load_level(target);
                return;  // world resets under us; next tick re-syncs via
                         // the own-level-change path
            } else if (prompted_epoch != io[IO_PEER_EPOCH]) {
                // Mid-level (or paused/title): don't yank the player.
                // Chirp once so they know, keep the epoch queued; the load
                // fires automatically when they reach the ship.
                prompted_epoch = io[IO_PEER_EPOCH];
                SSSVCoop_Debug(17, target);
                func_8032C508_73DBB8(SFX_MENU_NAVIGATE_UP, 0x4000, 0, 1.0f);
            }
        }
    }

    // ---- Our own level change wipes every slot: just forget ---------------
    if (own_level != last_level) {
        last_level = own_level;
        ghost_forget();
        dying_slot = -1;
    }

    // ---- Decide whether the ghost should exist ----------------------------
    {
        s32 linked = connected &&
                     io[IO_OWN_VALID] &&
                     io[IO_PEER_AGE] < PEER_STALE_FRAMES &&
                     io[IO_PEER_LEVEL] == own_level;

        // Peer is the EVO soul/microchip (between animals): their vacated
        // body should remain standing where they left it, not vanish. Keep
        // the existing ghost but freeze it: zero motion once, release the
        // limb hold so the engine's idle animation takes over, and skip all
        // driving until they possess something (the species-change path then
        // despawns this body and spawns the new one).
        if (linked && species_is_evo(io[10])) {
            if (ghost_valid() && !species_is_evo(ghost_species)) {
                Animal* fg = &gAnimalState.animalPool[ghost_slot];
                if (ik_hold_valid) {
                    ik_hold_valid = 0;
                    fg->xVelocity = 0;
                    fg->zVelocity = 0;
                    fg->yVelocity = 0;
                }
                return;  // frozen: engine idle-animates the vacated body
            }
            // EXPERIMENT (config-gated): no vacated body to show -- try
            // representing the soul as a real EVO entity (species 63). The
            // historical "EVO spawn crashes" predates the collision
            // protocol, behavior kill-switch, and NORMAL-mode fixes; this
            // tests whether it was ever specially cursed or just another
            // victim of the since-fixed corruption. If it crashes, the dump
            // tells us what the EVO needs that a bare spawn lacks.
            if (!evo_ghost_experiment) {
                return;  // experiment off: soul stays invisible
            }
            // fall through with the species forced to EVO (63)
            io[10] = 63;
        }

        if (!(linked && species_spawnable_here(io[10]))) {
            if (ghost_slot >= 0) ghost_despawn();
            return;
        }
    }

    // ---- Spawn / respawn --------------------------------------------------
    if (io[10] != pending_species) {
        pending_species = io[10];
        pending_frames = 0;
    } else if (pending_frames < 30) {
        pending_frames++;
    }
    if (pending_frames < 10) {
        // Species not stable yet: keep whatever ghost exists (or none), and
        // don't churn spawns on transient values.
        if (!ghost_valid()) return;
    }
    if (ghost_valid() && ghost_species != io[10] && pending_frames >= 10) {
        ghost_despawn();  // peer really possessed a new animal
    }
    if (!ghost_valid()) {
        Animal2* e;
        Animal2* save20; void* save24; Animal *save28, *save2C, *save30;
        s16 save38, save3A, save3C;
        if (respawn_cooldown > 0) return;  // let the engine sweep first
        ghost_forget();
        if (gNumAnimalsInLevel >= 49) return;  // no room; try again later
        save20 = D_803D5520; save24 = D_803D5524; save28 = D_803D5528;
        save2C = D_803D552C; save30 = D_803D5530;
        save38 = D_803D5538; save3A = D_803D553A; save3C = D_803D553C;
        e = coop_spawn_animal(io[11], io[12], io[13], io[14], 999, io[10]);
        D_803D5520 = save20; D_803D5524 = save24; D_803D5528 = save28;
        D_803D552C = save2C; D_803D5530 = save30;
        D_803D5538 = save38; D_803D553A = save3A; D_803D553C = save3C;
        if (e == 0) return;
        ghost_slot = ((s32)((u32)e - (u32)&gAnimalState.animals[0])) / (s32)sizeof(Animal2);
        ghost_species = io[10];
        SSSVCoop_Debug(10, ghost_slot);       // spawn event
        SSSVCoop_Debug(13, ghost_species);
        if (!ghost_valid()) { ghost_forget(); return; }
    }

    // ---- Field-fight detector (debug mode only) ---------------------------
    if (debug_mode && ghost_valid() && ik_written_valid) {
        u16* cur = (u16*)((u8*)&gAnimalState.animalPool[ghost_slot] + 0x370);
        s32 k;
        for (k = 0; k < 40; k++) {
            if (cur[k] != ik_written[k]) {
                if (k < 32) ik_fight_mask_lo |= (1u << k);
                else        ik_fight_mask_hi |= (1u << (k - 32));
            }
        }
        if ((frame_counter % 30) == 0) {   // once a second
            SSSVCoop_IkReport((s16*)cur, (s16*)ik_written,
                              (s32)ik_fight_mask_lo, (s32)ik_fight_mask_hi);
            ik_fight_mask_lo = 0;
            ik_fight_mask_hi = 0;
        }
    }

    // ---- Death handoff ----------------------------------------------------
    // If the engine changed the ghost's state under us (someone killed it),
    // stop driving and let the death sequence complete naturally. Forcing
    // the mode back to INJURED every frame kept the kill from ever
    // finishing, which made the score award repeat endlessly. The engine
    // deletes the slot itself when the sequence ends; we respawn afterward.
    g = &gAnimalState.animalPool[ghost_slot];
    if (g->health <= 0 ||
        (g->movementMode != MOVEMENT_MODE_INJURED &&
         g->movementMode != MOVEMENT_MODE_NORMAL)) {
        SSSVCoop_Debug(14, g->movementMode);  // ghost death/handoff event
        dying_slot = ghost_slot;              // watch the corpse for cleanup
        dying_timer = 150;                    // ~5s: let the death play out
        ghost_forget();
        if (respawn_cooldown < 90) respawn_cooldown = 90;  // ~3s
        return;
    }

    // ---- Drive the ghost from peer state ----------------------------------
    // Move it the way the game moves animals: write per-frame velocities and
    // let the engine integrate position, derive newPosition, and maintain the
    // collision-cell registration. Writing position/newPosition directly
    // bypassed that bookkeeping and corrupted the collision lists (the cause
    // of the +0x16C access-violation crashes). Only a large jump (peer
    // teleported/warped) is applied directly, followed by an explicit cell
    // re-registration exactly like a fresh spawn.
    g = &gAnimalState.animalPool[ghost_slot];
    {
        s32 tx = ((s32)io[11]) << 16;
        s32 tz = ((s32)io[12]) << 16;
        s32 ty = ((s32)io[13]) << 16;
        s32 dx = tx - g->xPos;
        s32 dz = tz - g->zPos;
        s32 dy = ty - g->yPos;
        s32 thresh = 64 << 16;

        if (dx > thresh || dx < -thresh || dz > thresh || dz < -thresh ||
            dy > thresh || dy < -thresh) {
            // Peer warped: hard place, then UNLINK from the collision cell
            // chains before re-registering -- the register function wipes the
            // animal's chain nodes in place, so calling it while still linked
            // orphans live list links. Broken chains that fault were the
            // earlier crashes; broken chains that cycle were the host
            // lockups under heavy movement. The game's own deletion path
            // (remove_collision_list before relist) is the correct protocol.
            remove_collision_list(g);
            g->xPos = tx; g->zPos = tz; g->yPos = ty;
            g->newXPos = tx; g->newZPos = tz; g->newYPos = ty;
            g->xVelocity = 0; g->zVelocity = 0; g->yVelocity = 0;
            func_802DADA0_6EC450(g);
            SSSVCoop_Debug(12, ghost_slot);  // teleport event
        } else {
            // Dead reckoning: move at the peer's actual (smooth) velocity,
            // plus a gentle drift toward the true position. Setting velocity
            // to the raw remaining delta made it oscillate between zero and
            // full stride at packet cadence, and the renderer reads velocity
            // for lean and foot placement -- that oscillation was the twitch.
            g->xVelocity = (((s32)io[IO_PEER_POSE + 106]) << 8) + (dx >> 3);
            g->zVelocity = (((s32)io[IO_PEER_POSE + 107]) << 8) + (dz >> 3);
            g->yVelocity = (((s32)io[IO_PEER_POSE + 108]) << 8) + (dy >> 3);
        }
    }
    // Apply the peer's pose bundle ONLY when a fresh packet arrived this
    // frame (age 0). The peer's fields are internally consistent per frame;
    // re-applying a stale snapshot every frame mixed an old pose into the
    // locally advancing state, which was the twitch. Between packets the
    // game's own systems carry the pose forward coherently. The full blob is
    // synced again, including the gait cluster: the 0x2FA stride divisor is
    // the peer's real walking cadence and exactly what the ghost's limb IK
    // needs. (The old "never zero" fallback of 0x2000 meant an 8192-frame
    // stride, i.e. legs frozen solid. Divisors are small: ~frames-per-step.)
    if (io[IO_PEER_AGE] == 0) {
        s32 w = 0, r, k;
        for (r = 0; r < 3; r++) {
            u16* dst = (u16*)((u8*)g + pose_ranges[r].off);
            for (k = 0; k < pose_ranges[r].words; k++, w++) {
                // The four LimbIKState blocks (R3 words 4..43): the dog's
                // walking legs are IK-stepped by locomotion code inside
                // behaviors, which the ghost never runs, so the peer's IK is
                // the ghost's only source of leg motion. But writing it HERE
                // (input time) loses to the engine's later idle micro-
                // animation each frame -- the shimmer. So it's captured into
                // a hold buffer and applied late-frame by the pre-render
                // hook below, where we get the last word before drawing.
                if (r == 2 && k >= 4) {
                    ik_hold[k - 4] = (u16)io[IO_PEER_POSE + w];
                    continue;
                }
                dst[k] = (u16)io[IO_PEER_POSE + w];
            }
        }
        ik_hold_valid = 1;
        if (g->unk2FA == 0) g->unk2FA = 32;
        g->state = (u16)io[IO_PEER_POSE + 104];
        g->movementState = (u8)(io[IO_PEER_POSE + 105] & 0xFF);
        g->unk365 = (u8)((io[IO_PEER_POSE + 105] >> 8) & 0xFF);
    }
    if (debug_mode) {
        u16* cur = (u16*)((u8*)g + 0x370);
        s32 k;
        for (k = 0; k < 40; k++) ik_written[k] = cur[k];
        ik_written_valid = 1;
    }
    g->yRotation = io[15];
    g->aiFlags = 0;                          // belt-and-suspenders with the patch
    g->movementMode = MOVEMENT_MODE_NORMAL;  // full render/gait pipeline; the
                                             // behavior dispatch patch keeps
                                             // its AI from ever running

    if (debug_mode && (frame_counter % 60) == 0) {  // 2s one-line status
        SSSVCoop_GhostStatus((s32)((u16)ghost_slot) | (((s32)g->movementMode) << 16),
                             g->xPos >> 16, g->yPos >> 16,
                             (s32)((u16)io[10]) | (((s32)io[IO_PEER_LEVEL]) << 16));
    }
}
