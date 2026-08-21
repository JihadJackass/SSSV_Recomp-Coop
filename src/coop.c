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
    /* 0x02E */ u8  pad02E[0x272 - 0x2E];
    /* 0x272 */ u16 aiFlags;
    /* 0x274 */ u8  pad274[0x302 - 0x274];
    /* 0x302 */ s16 heading;
    /* 0x304 */ s16 previousHeading;
    /* 0x306 */ u8  pad306[0x366 - 0x306];
    /* 0x366 */ u8  movementMode;
    /* 0x367 */ u8  pad367[0x3D4 - 0x367];
} Animal;

_Static_assert(sizeof(Animal) == 0x3D4, "Animal size");
_Static_assert(__builtin_offsetof(Animal, aiFlags) == 0x272, "aiFlags off");
_Static_assert(__builtin_offsetof(Animal, heading) == 0x302, "heading off");
_Static_assert(__builtin_offsetof(Animal, movementMode) == 0x366, "movementMode off");

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

// The game's own spawner (symbol added to our local syms at 0x802C7F88):
// finds a free/deleted slot, wires species + animal pool pointers, places at
// (x, z, y) snapped above ground, sets heading/health, returns the entry.
// flag=0 avoids a camera-related call reserved for scripted spawns.
extern Animal2* spawn_animal(s16 x, s16 z, s16 y, s16 rotation, s16 health, s16 id, s8 flag);

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

#define COOP_STATUS_CONNECTED 3
#define IO_OWN_VALID   0
#define IO_OWN_LEVEL   1
#define IO_PEER_AGE    8
#define IO_PEER_LEVEL  9
#define PEER_STALE_FRAMES 60   // 2 seconds without peer state = hide ghost

static s16  io[20];
static s32  ghost_slot = -1;
static s16  ghost_species = -1;
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
    }
    ghost_forget();
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
                   io[10] >= 0 && io[10] < 68;   // sane species id

        if (!want) {
            if (ghost_slot >= 0) ghost_despawn();
            return;
        }
    }

    // ---- Spawn / respawn --------------------------------------------------
    if (ghost_valid() && ghost_species != io[10]) {
        ghost_despawn();  // peer possessed a new animal
    }
    if (!ghost_valid()) {
        Animal2* e;
        ghost_forget();
        if (gNumAnimalsInLevel >= 49) return;  // no room; try again later
        e = spawn_animal(io[11], io[12], io[13], io[14], 999, io[10], 0);
        if (e == 0) return;
        ghost_slot = ((s32)((u32)e - (u32)&gAnimalState.animals[0])) / (s32)sizeof(Animal2);
        ghost_species = io[10];
        if (!ghost_valid()) { ghost_forget(); return; }
    }

    // ---- Drive the ghost from peer state ----------------------------------
    g = &gAnimalState.animalPool[ghost_slot];
    g->xPos = ((s32)io[11]) << 16;
    g->zPos = ((s32)io[12]) << 16;
    g->yPos = ((s32)io[13]) << 16;
    g->newXPos = g->xPos;
    g->newZPos = g->zPos;
    g->newYPos = g->yPos;
    g->xVelocity = 0;
    g->zVelocity = 0;
    g->yVelocity = 0;
    g->heading = io[14];
    g->previousHeading = io[14];
    g->yRotation = io[15];
    g->aiFlags = 0;                          // inert every frame
    g->movementMode = MOVEMENT_MODE_NORMAL;  // keep it alive and rendered
}
