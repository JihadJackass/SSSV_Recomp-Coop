// SSSV Co-op - Phase 0: connection scaffold
//
// Establishes and maintains a UDP link between two copies of SSSV Recompiled
// (host + join) and signals connection state in-game with menu sounds:
//   - "menu navigate up" chirp   = connected
//   - "menu navigate down" chirp = disconnected / lost peer
//
// No gameplay is synced yet. This phase exists to prove the pipeline:
// config -> mod -> native library -> sockets -> the other machine, running
// inside the real game on both ends.
//
// The per-frame pump hooks get_controller_input, which the main gameplay
// loop calls once per frame. Note this does NOT run on the title/main menu,
// so both players must be in actual gameplay for the link to establish.
// The protocol is self-healing (heartbeats + timeouts + auto-reconnect), so
// pauses, menus, and level loads only suspend it, never break it permanently.

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
// Vanilla game functions we call directly (resolved via reference symbols).
// ---------------------------------------------------------------------------

// Plays a sound effect. Signature and typical args observed in the decomp
// (camera.c): func_8032C508_73DBB8(SFX_ID, 0x4000, 0, 1.0f);
extern void func_8032C508_73DBB8(s16 sfxId, s32 volume, s32 pan, f32 pitch);

#define SFX_MENU_NAVIGATE_UP    144
#define SFX_MENU_NAVIGATE_DOWN  145

// ---------------------------------------------------------------------------
// Native import. One call per frame:
//   mode: 0 = Off, 1 = Host, 2 = Join
//   ip:   pointer to a NUL-terminated IP string (only used in Join mode)
//   port: UDP port
// Returns a status code:
//   0 = off, 1 = listening (host), 2 = connecting (join),
//   3 = CONNECTED, 4 = error (bad IP / socket failure)
// ---------------------------------------------------------------------------
RECOMP_IMPORT(".", s32 SSSVCoop_Update(s32 mode, char* ip, s32 port));

// Mode enum indices must match the "mode" config option order in mod.toml.
#define COOP_MODE_OFF  0
#define COOP_MODE_HOST 1
#define COOP_MODE_JOIN 2

#define COOP_STATUS_CONNECTED 3

// ---------------------------------------------------------------------------
// Config cache. Config strings are heap-allocated per query and must be
// freed, so re-read them once a second instead of every frame.
// ---------------------------------------------------------------------------
static char cached_ip[64] = "";
static s32  cached_mode = COOP_MODE_OFF;
static s32  cached_port = 7642;
static u32  frame_counter = 0;

static void refresh_config(void) {
    char* ip;
    s32 i;

    cached_mode = (s32)recomp_get_config_u32("mode");
    cached_port = (s32)recomp_get_config_double("port");

    ip = recomp_get_config_string("host_ip");
    if (ip != 0) {
        for (i = 0; i < 63 && ip[i] != '\0'; i++) {
            cached_ip[i] = ip[i];
        }
        cached_ip[i] = '\0';
        recomp_free_config_string(ip);
    }
}

// ---------------------------------------------------------------------------
// Per-frame pump.
// ---------------------------------------------------------------------------
RECOMP_HOOK("get_controller_input")
void coop_frame_update(void) {
    static s32 prev_connected = 0;
    s32 status;
    s32 connected;

    if ((frame_counter % 30) == 0) {  // once a second at 30fps
        refresh_config();
    }
    frame_counter++;

    status = SSSVCoop_Update(cached_mode, cached_ip, cached_port);

    connected = (status == COOP_STATUS_CONNECTED) ? 1 : 0;
    if (connected && !prev_connected) {
        func_8032C508_73DBB8(SFX_MENU_NAVIGATE_UP, 0x4000, 0, 1.0f);
    } else if (!connected && prev_connected) {
        func_8032C508_73DBB8(SFX_MENU_NAVIGATE_DOWN, 0x4000, 0, 1.0f);
    }
    prev_connected = connected;
}
