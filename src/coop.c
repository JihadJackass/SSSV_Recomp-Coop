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
    /* 0x2FA */ u8  pad2FA[0x302 - 0x2FA];
    /* 0x302 */ s16 heading;
    /* 0x304 */ s16 previousHeading;
    /* 0x306 */ u8  pad306[0x31A - 0x306];
    /* 0x31A */ s16 unk31A;
    /* 0x31C */ s32 unk31C;         // spawn timestamp
    /* 0x320 */ u8  pad320[0x366 - 0x320];
    /* 0x366 */ u8  movementMode;
    /* 0x367 */ u8  pad367[0x3CA - 0x367];
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
_Static_assert(__builtin_offsetof(Animal, movementMode) == 0x366, "movementMode off");
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
extern s16  load_dynamic_tail(s16 id);
extern void func_802C9BA4_6DB254(Animal* a);           // per-animal init

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

#define MOVEMENT_MODE_NORMAL  1
#define MOVEMENT_MODE_DELETED 6

// ---------------------------------------------------------------------------
// Native link. io is 20 s16 slots shared both ways each frame:
//   [0]  out: own state valid (1/0)
//   [1..7]  out: level, species, x, z, y, heading, yRotation
//   [8]  in:  peer state age in frames (999 = none/stale)
//   [9..15] in:  peer level, species, x, z, y, heading, yRotation
// Return: connection status (3 = connected).
// ---------------------------------------------------------------------------
RECOMP_IMPORT(".", s32 SSSVCoop_Update(s32 mode, char* ip, s32 port, s16* io));
RECOMP_IMPORT(".", void SSSVCoop_Debug(s32 tag, s32 value));

#define COOP_STATUS_CONNECTED 3
#define IO_OWN_VALID   0
#define IO_OWN_LEVEL   1
#define IO_PEER_AGE    8
#define IO_PEER_LEVEL  9
#define PEER_STALE_FRAMES 60   // 2 seconds without peer state = hide ghost

static s16  io[20];
static s32  ghost_slot = -1;
static s16  ghost_species = -1;
static s16  pending_species = -1;   // species-change damping
static s16  pending_frames = 0;
static s16  last_level = -1;
static char cached_ip[64] = "";
static s32  cached_mode = 0;
static s32  cached_port = 7642;
static u32  frame_counter = 0;

static void refresh_config(void) {
    char* ip;
    s32 i;
    cached_mode = (s32)recomp_get_config_u32("mode");
    cached_port = (s32)recomp_get_config_double("port");
    ip = recomp_get_config_string("host_ip");
    if (ip != 0) {
        for (i = 0; i < 63 && ip[i] != '\0'; i++) cached_ip[i] = ip[i];
        cached_ip[i] = '\0';
        recomp_free_config_string(ip);
    }
}

// Forget the ghost without touching game memory (used when the world was
// reset under us, e.g. our own level change).
static void ghost_forget(void) {
    ghost_slot = -1;
    ghost_species = -1;
}

// Actively remove the ghost using the game's own free-slot convention.
static void ghost_despawn(void) {
    if (ghost_slot >= 0 && ghost_slot < gNumAnimalsInLevel &&
        gAnimalState.animals[ghost_slot].animal == &gAnimalState.animalPool[ghost_slot]) {
        gAnimalState.animalPool[ghost_slot].movementMode = MOVEMENT_MODE_DELETED;
        SSSVCoop_Debug(11, ghost_slot);       // despawn event
    }
    ghost_forget();
}

// A species is safe to spawn only if it's a normal animal id -- the EVO
// pseudo-species (MICROCHIP 61, TRANSFER 62, EVO 63, SHELLSUIT 67) have
// special handling and crash when bare-spawned; that was the exact failure
// in the logs (peer was the microchip, then the soul, at level start).
// King Penguin (64), Racing Tortoise (65), and Cool Cod (66) are real
// animals and stay allowed. The species must also exist among OUR level's
// current roster, guaranteeing its assets and species data are loaded here.
static s32 species_spawnable_here(s16 id) {
    s32 i;
    if (id < 0 || id > 67) return 0;
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

RECOMP_HOOK("get_controller_input")
void coop_frame_update(void) {
    static s32 prev_connected = 0;
    s32 status, connected;
    s16 own_level;
    Animal* own;
    Animal* g;
    s32 i;

    if ((frame_counter % 30) == 0) refresh_config();
    frame_counter++;

    // ---- Gather own state -------------------------------------------------
    own_level = gGameState.level;
    io[IO_OWN_VALID] = 0;
    i = gCurrentAnimalIndex;
    if (i >= 0 && i < 50 && i < gNumAnimalsInLevel) {
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

    // ---- Our own level change wipes every slot: just forget ---------------
    if (own_level != last_level) {
        last_level = own_level;
        ghost_forget();
    }

    // ---- Decide whether the ghost should exist ----------------------------
    {
        s32 want = connected &&
                   io[IO_OWN_VALID] &&
                   io[IO_PEER_AGE] < PEER_STALE_FRAMES &&
                   io[IO_PEER_LEVEL] == own_level &&
                   species_spawnable_here(io[10]);

        if (!want) {
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
            // Peer warped: hard place + re-register collision cells.
            g->xPos = tx; g->zPos = tz; g->yPos = ty;
            g->newXPos = tx; g->newZPos = tz; g->newYPos = ty;
            g->xVelocity = 0; g->zVelocity = 0; g->yVelocity = 0;
            func_802DADA0_6EC450(g);
            SSSVCoop_Debug(12, ghost_slot);  // teleport event
        } else {
            g->xVelocity = dx;   // engine: position += velocity this frame
            g->zVelocity = dz;
            g->yVelocity = dy;
        }
    }
    g->heading = io[14];
    g->previousHeading = io[14];
    g->yRotation = io[15];
    g->aiFlags = 0;                           // inert every frame
    g->movementMode = MOVEMENT_MODE_INJURED;  // safe vanilla bare-spawn state

    if ((frame_counter % 60) == 0) {          // 2s heartbeat of ghost state
        SSSVCoop_Debug(5, io[10]);            // peer species
        SSSVCoop_Debug(6, io[IO_PEER_LEVEL]);
        SSSVCoop_Debug(1, ghost_slot);
        SSSVCoop_Debug(2, g->movementMode);
        SSSVCoop_Debug(3, g->xPos >> 16);
        SSSVCoop_Debug(4, g->yPos >> 16);
    }
}
