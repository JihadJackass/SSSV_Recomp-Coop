// SSSV Co-op - native networking library (Phase 0).
//
// Cross-platform (Winsock2 / BSD sockets) non-blocking UDP with a tiny
// self-healing protocol:
//
//   Host: binds 0.0.0.0:<port>, waits for HELLO, replies WELCOME, then both
//         sides exchange PING/PONG once a second.
//   Join: sends HELLO to <ip>:<port> once a second until WELCOME arrives.
//   Either side: no packet from peer for TIMEOUT seconds -> drop back to
//         listening/connecting and keep trying. Nothing is ever fatal.
//
// The packet header already carries a version byte and message type so
// Phase 1 (player state sync) extends this without a rewrite.
//
// Build:
//   Linux:   gcc -shared -fPIC -O2 -o sssv_coop_native.so sssv_coop_native.c
//   Windows: x86_64-w64-mingw32-gcc -shared -O2 -o sssv_coop_native.dll sssv_coop_native.c -static-libgcc -lws2_32
//   (MSVC:   cl /LD /O2 sssv_coop_native.c /Fe:sssv_coop_native.dll ws2_32.lib)

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define EXPORT __declspec(dllexport)
    typedef SOCKET sock_t;
    #define SOCK_INVALID INVALID_SOCKET
    static int sock_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
    static void sock_set_nonblocking(sock_t s) { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #define EXPORT __attribute__((visibility("default")))
    typedef int sock_t;
    #define SOCK_INVALID (-1)
    static int sock_would_block(void) { return errno == EWOULDBLOCK || errno == EAGAIN; }
    static void sock_set_nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
    #define closesocket close
#endif

// --------------------------------------------------------------------------
// Recomp runtime ABI (matches N64Recomp/include/recomp.h)
// --------------------------------------------------------------------------
EXPORT uint32_t recomp_api_version = 1;

typedef uint64_t gpr;
typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
} recomp_context;

// Sub-word loads/stores in emulated RDRAM use XOR'd addressing.
#define MEM_B(offset, reg) \
    (*(int8_t*)(rdram + ((((reg) + (offset)) ^ 3) - 0xFFFFFFFF80000000ull)))
#define MEM_H(offset, reg) \
    (*(int16_t*)(rdram + ((((reg) + (offset)) ^ 2) - 0xFFFFFFFF80000000ull)))

// --------------------------------------------------------------------------
// Protocol
// --------------------------------------------------------------------------
#define COOP_MAGIC   0x5353434Fu  /* 'SSCO' */
#define COOP_VERSION 1  // proto v1: pose bundle added; incompatible with v0 builds

enum { MSG_HELLO = 1, MSG_WELCOME = 2, MSG_PING = 3, MSG_PONG = 4, MSG_STATE = 6 };

#pragma pack(push, 1)
typedef struct {
    int16_t level, species, x, z, y, heading, yrot;
    uint16_t pose[37];
} StatePayload;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t reserved;
} PacketHeader;
#pragma pack(pop)

// Modes from the mod (must match mod.toml enum order)
enum { MODE_OFF = 0, MODE_HOST = 1, MODE_JOIN = 2 };

// Status codes returned to the mod
enum { CST_OFF = 0, CST_LISTENING = 1, CST_CONNECTING = 2, CST_CONNECTED = 3, CST_ERROR = 4 };

#define HEARTBEAT_INTERVAL_S 1.0
#define PEER_TIMEOUT_S       10.0

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------
static sock_t g_sock = SOCK_INVALID;
static int    g_mode = MODE_OFF;          // mode the socket was opened for
static int    g_port = 0;
static char   g_target_ip[64] = "";
static int    g_status = CST_OFF;
static int    g_have_peer = 0;
static struct sockaddr_in g_peer;
static double g_last_rx = 0.0;
static StatePayload g_peer_state;
static double g_peer_state_time = -1.0;   // <0 = never received
static double g_last_tx = 0.0;
static int    g_wsa_init = 0;

static double now_seconds(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

// --------------------------------------------------------------------------
// Logging: appends to coop_log.txt in the SSSVRecompiled config folder
// (%LOCALAPPDATA%\SSSVRecompiled on Windows, ~/.config/SSSVRecompiled on
// Linux), falling back to the current directory.
// --------------------------------------------------------------------------
static FILE* g_log = NULL;
static int   g_log_tried = 0;

static void log_open(void) {
    char path[512];
    if (g_log || g_log_tried) return;
    g_log_tried = 1;
#ifdef _WIN32
    const char* base = getenv("LOCALAPPDATA");
    if (base) snprintf(path, sizeof(path), "%s\\SSSVRecompiled\\coop_log.txt", base);
    else      snprintf(path, sizeof(path), "coop_log.txt");
#else
    const char* base = getenv("HOME");
    if (base) snprintf(path, sizeof(path), "%s/.config/SSSVRecompiled/coop_log.txt", base);
    else      snprintf(path, sizeof(path), "coop_log.txt");
#endif
    g_log = fopen(path, "a");
    if (g_log) {
        fprintf(g_log, "--- log path: %s ---\n", path);
    } else {
        g_log = fopen("coop_log.txt", "a");
        if (g_log) fprintf(g_log, "--- primary path failed (%s), using cwd ---\n", path);
    }
    if (g_log) fflush(g_log);
}


static void coop_log(const char* fmt, ...) {
    va_list ap;
    log_open();
    if (!g_log) return;
    fprintf(g_log, "[%10.3f] ", now_seconds());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

// Runs the moment the library is loaded by the runtime, before any calls.
// If the log file exists at all, the DLL/SO was found and loaded.
#if defined(_WIN32) || defined(__GNUC__)
__attribute__((constructor))
static void coop_on_load(void) {
    coop_log("=== sssv_coop_native loaded (build: phase2-v1.1.0) ===");
}
#endif

static void net_teardown(void) {
    if (g_sock != SOCK_INVALID) {
        closesocket(g_sock);
        g_sock = SOCK_INVALID;
    }
    g_have_peer = 0;
    g_status = CST_OFF;
    g_peer_state_time = -1.0;
}

static void send_msg(uint8_t type, const struct sockaddr_in* to) {
    PacketHeader h;
    h.magic = COOP_MAGIC;
    h.version = COOP_VERSION;
    h.type = type;
    h.reserved = 0;
    sendto(g_sock, (const char*)&h, sizeof(h), 0,
           (const struct sockaddr*)to, sizeof(*to));
    g_last_tx = now_seconds();
}

static void send_state(const StatePayload* st) {
    char buf[sizeof(PacketHeader) + sizeof(StatePayload)];
    PacketHeader* h = (PacketHeader*)buf;
    h->magic = COOP_MAGIC;
    h->version = COOP_VERSION;
    h->type = MSG_STATE;
    h->reserved = 0;
    memcpy(buf + sizeof(PacketHeader), st, sizeof(StatePayload));
    sendto(g_sock, buf, sizeof(buf), 0,
           (const struct sockaddr*)&g_peer, sizeof(g_peer));
    g_last_tx = now_seconds();
}

// Open (or reopen) the socket for the requested mode/target.
static int net_setup(int mode, const char* ip, int port) {
    struct sockaddr_in addr;

#ifdef _WIN32
    if (!g_wsa_init) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        g_wsa_init = 1;
    }
#endif

    net_teardown();

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == SOCK_INVALID) { coop_log("ERROR: socket() failed"); return 0; }
    sock_set_nonblocking(g_sock);

    if (mode == MODE_HOST) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        if (bind(g_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            coop_log("ERROR: bind() on port %d failed (port in use?)", port);
            net_teardown();
            return 0;
        }
        g_status = CST_LISTENING;
        coop_log("HOST: listening on UDP %d", port);
    } else { // MODE_JOIN
        memset(&g_peer, 0, sizeof(g_peer));
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip, &g_peer.sin_addr) != 1) {
            coop_log("ERROR: bad IP string '%s'", ip);
            net_teardown();
            return 0;
        }
        g_have_peer = 0;  // becomes 1 on WELCOME
        g_status = CST_CONNECTING;
        coop_log("JOIN: targeting %s:%d", ip, port);
    }

    g_mode = mode;
    g_port = port;
    strncpy(g_target_ip, ip ? ip : "", sizeof(g_target_ip) - 1);
    g_target_ip[sizeof(g_target_ip) - 1] = '\0';
    g_last_rx = now_seconds();
    return 1;
}

static void net_pump(void) {
    char buf[512];
    struct sockaddr_in from;
    double t = now_seconds();

    // Receive everything pending.
    for (;;) {
#ifdef _WIN32
        int fromlen = sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        int n = recvfrom(g_sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&from, &fromlen);
        if (n < 0) {
            if (!sock_would_block()) { /* transient error, ignore */ }
            break;
        }
        if ((size_t)n < sizeof(PacketHeader)) continue;

        PacketHeader* h = (PacketHeader*)buf;
        if (h->magic != COOP_MAGIC || h->version != COOP_VERSION) continue;

        switch (h->type) {
        case MSG_HELLO:
            if (g_mode == MODE_HOST) {
                g_peer = from;          // adopt/refresh the peer address
                if (!g_have_peer) coop_log("HOST: HELLO from %s -> CONNECTED", inet_ntoa(from.sin_addr));
                g_have_peer = 1;
                g_last_rx = t;
                send_msg(MSG_WELCOME, &g_peer);
                g_status = CST_CONNECTED;
            }
            break;
        case MSG_WELCOME:
            if (g_mode == MODE_JOIN) {
                if (!g_have_peer) coop_log("JOIN: WELCOME received -> CONNECTED");
                g_have_peer = 1;
                g_last_rx = t;
                g_status = CST_CONNECTED;
            }
            break;
        case MSG_PING:
            if (g_have_peer) {
                g_last_rx = t;
                send_msg(MSG_PONG, &g_peer);
            }
            break;
        case MSG_PONG:
            if (g_have_peer) {
                g_last_rx = t;
            }
            break;
        case MSG_STATE:
            if (g_have_peer && (size_t)n >= sizeof(PacketHeader) + sizeof(StatePayload)) {
                memcpy(&g_peer_state, buf + sizeof(PacketHeader), sizeof(StatePayload));
                g_peer_state_time = t;
                g_last_rx = t;
            }
            break;
        default:
            break;
        }
    }

    // Periodic sends.
    if (t - g_last_tx >= HEARTBEAT_INTERVAL_S) {
        if (g_mode == MODE_JOIN && !g_have_peer) {
            send_msg(MSG_HELLO, &g_peer);           // keep knocking
        } else if (g_have_peer) {
            send_msg(MSG_PING, &g_peer);
        }
    }

    // Peer timeout -> fall back and keep trying.
    if (g_have_peer && (t - g_last_rx > PEER_TIMEOUT_S)) {
        coop_log("TIMEOUT: no packets from peer for %.0fs, dropping", PEER_TIMEOUT_S);
        g_have_peer = 0;
        g_peer_state_time = -1.0;
        g_status = (g_mode == MODE_HOST) ? CST_LISTENING : CST_CONNECTING;
    }
}

// --------------------------------------------------------------------------
// Export: called once per game frame.
//   a0 (r4) = mode (0 off, 1 host, 2 join)
//   a1 (r5) = rdram address of NUL-terminated IP string
//   a2 (r6) = port
// Returns status in v0 (r2).
// --------------------------------------------------------------------------
static const char* dbg_tag_name(int t) {
    switch (t) {
    case 1: return "ghost_slot"; case 2: return "ghost_mode";
    case 5: return "peer_species"; case 6: return "peer_level";
    case 3: return "ghost_x"; case 4: return "ghost_y";
    case 10: return "SPAWN slot"; case 11: return "DESPAWN slot";
    case 12: return "TELEPORT slot"; case 13: return "species";
    case 14: return "GHOST KILLED, engine mode";
    case 15: return "CORPSE CLEANED, slot";
    default: return "?";
    }
}

EXPORT void SSSVCoop_Debug(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    coop_log("DBG %s = %d", dbg_tag_name((int)(int32_t)ctx->r4), (int)(int32_t)ctx->r5);
}

EXPORT void SSSVCoop_Update(uint8_t* rdram, recomp_context* ctx) {
    static int first_call = 1;
    static double last_summary = 0.0;
    int mode = (int)(int32_t)ctx->r4;
    gpr ip_addr = ctx->r5;
    int port = (int)(int32_t)ctx->r6;
    gpr io = ctx->r7;

    char ip[64];
    int i;

    // Copy the IP string out of emulated memory.
    ip[0] = '\0';
    if (ip_addr != 0) {
        for (i = 0; i < 63; i++) {
            char c = (char)MEM_B(i, ip_addr);
            ip[i] = c;
            if (c == '\0') break;
        }
        ip[63] = '\0';
    }

    if (port <= 0 || port > 65535) port = 7642;

    if (first_call) {
        first_call = 0;
        coop_log("=== SSSVCoop_Update first call: mode=%d ip='%s' port=%d (hook + config OK) ===",
                 mode, ip, port);
    }
    {
        double t = now_seconds();
        if (t - last_summary >= 5.0) {
            last_summary = t;
            coop_log("status=%d mode=%d ip='%s' port=%d peer=%d",
                     g_status, mode, ip, port, g_have_peer);
        }
    }

    if (mode == MODE_OFF) {
        if (g_status != CST_OFF) net_teardown();
        ctx->r2 = CST_OFF;
        return;
    }

    // (Re)configure if the socket isn't up or settings changed.
    if (g_sock == SOCK_INVALID || mode != g_mode || port != g_port ||
        (mode == MODE_JOIN && strncmp(ip, g_target_ip, sizeof(g_target_ip)) != 0)) {
        if (!net_setup(mode, ip, port)) {
            ctx->r2 = CST_ERROR;
            return;
        }
    }

    net_pump();

    if (io != 0) {
        // Outgoing: send our state every frame while connected.
        if (g_have_peer && MEM_H(0x0, io) != 0) {
            StatePayload st;
            st.level   = MEM_H(0x2,  io);
            st.species = MEM_H(0x4,  io);
            st.x       = MEM_H(0x6,  io);
            st.z       = MEM_H(0x8,  io);
            st.y       = MEM_H(0xA,  io);
            st.heading = MEM_H(0xC,  io);
            st.yrot    = MEM_H(0xE,  io);
            {
                int k;
                for (k = 0; k < 37; k++) {
                    st.pose[k] = (uint16_t)MEM_H(0x20 + 2*k, io);  // io[16+k]
                }
            }
            send_state(&st);
        }
        // Incoming: latest peer state + age in frames (999 = none).
        {
            int age = 999;
            if (g_have_peer && g_peer_state_time >= 0.0) {
                double a = (now_seconds() - g_peer_state_time) * 30.0;
                age = (a < 0) ? 0 : (a > 999 ? 999 : (int)a);
            }
            MEM_H(0x10, io) = (int16_t)age;
            MEM_H(0x12, io) = g_peer_state.level;
            MEM_H(0x14, io) = g_peer_state.species;
            MEM_H(0x16, io) = g_peer_state.x;
            MEM_H(0x18, io) = g_peer_state.z;
            MEM_H(0x1A, io) = g_peer_state.y;
            MEM_H(0x1C, io) = g_peer_state.heading;
            MEM_H(0x1E, io) = g_peer_state.yrot;
            {
                int k;
                for (k = 0; k < 37; k++) {
                    MEM_H(0x70 + 2*k, io) = (int16_t)g_peer_state.pose[k];  // io[56+k]
                }
            }
        }
    }

    ctx->r2 = (gpr)(int32_t)g_status;
}
