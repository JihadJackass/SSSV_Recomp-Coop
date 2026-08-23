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
#define COOP_VERSION 4  // proto v4: adds MSG_LEVEL (host-selected missions)

enum { MSG_HELLO = 1, MSG_WELCOME = 2, MSG_PING = 3, MSG_PONG = 4, MSG_STATE = 6, MSG_LEVEL = 7 };

#pragma pack(push, 1)
typedef struct {
    int16_t level, species, x, z, y, heading, yrot;
    uint16_t pose[109];
} StatePayload;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {            // mission trio: which level the sender is in
    int16_t level;
    int16_t in_level;       // 1 = actually playing it (not ship/menus)
    int16_t epoch;          // increments on every level ENTRY (incl. replays)
} LevelPayload;
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
static LevelPayload g_own_level  = { 0, 0, 0 };
static LevelPayload g_peer_level = { 0, 0, 0 };
static double g_peer_level_time = -1.0;   // <0 = never received
static double g_last_level_tx = 0.0;
#define LEVEL_STALE_S 5.0   // no MSG_LEVEL for this long -> treat as not in-level
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
    {
        FILE* probe = fopen(path, "rb");
        if (probe) {
            long sz;
            fseek(probe, 0, SEEK_END);
            sz = ftell(probe);
            fclose(probe);
            if (sz > 2*1024*1024) {
                FILE* trunc = fopen(path, "w");  // cap at 2MB: start fresh
                if (trunc) fclose(trunc);
            }
        }
    }
    g_log = fopen(path, "a");
    if (g_log) {
        fprintf(g_log, "--- log path: %s ---\n", path);
    } else {
        g_log = fopen("coop_log.txt", "a");
        if (g_log) fprintf(g_log, "--- primary path failed (%s), using cwd ---\n", path);
    }
    if (g_log) fflush(g_log);
}


enum { LC_SYS = 0, LC_NET = 1, LC_GHOST = 2, LC_POSE = 3 };
static const char* lc_name[4] = { "SYS  ", "NET  ", "GHOST", "POSE " };

static void log_timestamp(char* buf, size_t n) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    snprintf(buf, n, "%02d:%02d:%02d.%03ld",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
#endif
}

static void coop_logc(int cat, const char* fmt, ...) {
    va_list ap;
    char ts[16];
    log_open();
    if (!g_log) return;
    log_timestamp(ts, sizeof(ts));
    fprintf(g_log, "[%s] %s | ", ts, lc_name[cat & 3]);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

static void coop_log(const char* fmt, ...) {   // legacy: SYS category
    va_list ap;
    char ts[16];
    char line[512];
    log_open();
    if (!g_log) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_timestamp(ts, sizeof(ts));
    fprintf(g_log, "[%s] %s | %s\n", ts, lc_name[LC_SYS], line);
    fflush(g_log);
}

// Runs the moment the library is loaded by the runtime, before any calls.
// If the log file exists at all, the DLL/SO was found and loaded.
#if defined(_WIN32) || defined(__GNUC__)
__attribute__((constructor))
static void coop_on_load(void) {
    log_open();
    if (g_log) {
        fprintf(g_log, "\n============================================================\n");
        fflush(g_log);
    }
    coop_logc(LC_SYS, "SSSV Co-op native loaded  (build 1.2.2)  -- session start");
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

static void send_level(const LevelPayload* lv) {
    char buf[sizeof(PacketHeader) + sizeof(LevelPayload)];
    PacketHeader* h = (PacketHeader*)buf;
    h->magic = COOP_MAGIC;
    h->version = COOP_VERSION;
    h->type = MSG_LEVEL;
    h->reserved = 0;
    memcpy(buf + sizeof(PacketHeader), lv, sizeof(LevelPayload));
    sendto(g_sock, buf, sizeof(buf), 0,
           (const struct sockaddr*)&g_peer, sizeof(g_peer));
    g_last_tx = now_seconds();
    g_last_level_tx = g_last_tx;
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
    if (g_sock == SOCK_INVALID) { coop_logc(LC_NET, "ERROR: socket() failed"); return 0; }
    sock_set_nonblocking(g_sock);

    if (mode == MODE_HOST) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        if (bind(g_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            coop_logc(LC_NET, "ERROR: bind() on port %d failed (port in use?)", port);
            net_teardown();
            return 0;
        }
        g_status = CST_LISTENING;
        coop_logc(LC_NET, "listening (host) on UDP %d", port);
    } else { // MODE_JOIN
        memset(&g_peer, 0, sizeof(g_peer));
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, ip, &g_peer.sin_addr) != 1) {
            coop_logc(LC_NET, "ERROR: bad IP string [%s]", ip);
            net_teardown();
            return 0;
        }
        g_have_peer = 0;  // becomes 1 on WELCOME
        g_status = CST_CONNECTING;
        coop_logc(LC_NET, "joining %s:%d", ip, port);
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
                if (!g_have_peer) coop_logc(LC_NET, "peer connected: HELLO from %s", inet_ntoa(from.sin_addr));
                g_have_peer = 1;
                g_last_rx = t;
                send_msg(MSG_WELCOME, &g_peer);
                g_status = CST_CONNECTED;
            }
            break;
        case MSG_WELCOME:
            if (g_mode == MODE_JOIN) {
                if (!g_have_peer) coop_logc(LC_NET, "connected: WELCOME received from host");
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
        case MSG_LEVEL:
            if (g_have_peer && (size_t)n >= sizeof(PacketHeader) + sizeof(LevelPayload)) {
                LevelPayload prev = g_peer_level;
                memcpy(&g_peer_level, buf + sizeof(PacketHeader), sizeof(LevelPayload));
                g_peer_level_time = t;
                g_last_rx = t;
                if (prev.epoch != g_peer_level.epoch || prev.level != g_peer_level.level ||
                    prev.in_level != g_peer_level.in_level) {
                    coop_logc(LC_NET, "peer level: %d in_level=%d epoch=%d",
                              g_peer_level.level, g_peer_level.in_level, g_peer_level.epoch);
                }
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
        coop_logc(LC_NET, "peer lost: no packets for %.0fs, reconnecting", PEER_TIMEOUT_S);
        g_have_peer = 0;
        g_peer_state_time = -1.0;
        g_peer_level_time = -1.0;
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
EXPORT void SSSVCoop_Debug(uint8_t* rdram, recomp_context* ctx) {
    int tag = (int)(int32_t)ctx->r4;
    int v = (int)(int32_t)ctx->r5;
    (void)rdram;
    switch (tag) {
    case 10: coop_logc(LC_GHOST, "spawned in slot %d", v); break;
    case 11: coop_logc(LC_GHOST, "despawned slot %d", v); break;
    case 12: coop_logc(LC_GHOST, "teleport re-place, slot %d", v); break;
    case 13: coop_logc(LC_GHOST, "species %d", v); break;
    case 14: coop_logc(LC_GHOST, "killed by engine (mode %d), letting death play out", v); break;
    case 15: coop_logc(LC_GHOST, "corpse cleaned, slot %d freed", v); break;
    case 16: coop_logc(LC_SYS,   "MISSION: following host into level %d", v); break;
    case 17: coop_logc(LC_SYS,   "MISSION: host selected level %d; will follow from the ship", v); break;
    default: coop_logc(LC_GHOST, "event %d = %d", tag, v); break;
    }
}

// One-line ghost status. Args packed into MIPS register args:
//   a0 = slot | mode<<16, a1 = x, a2 = y, a3 = species | level<<16
EXPORT void SSSVCoop_GhostStatus(uint8_t* rdram, recomp_context* ctx) {
    int sm = (int)(int32_t)ctx->r4;
    int x = (int)(int32_t)ctx->r5;
    int y = (int)(int32_t)ctx->r6;
    int sl = (int)(int32_t)ctx->r7;
    (void)rdram;
    coop_logc(LC_GHOST, "status: slot=%d mode=%d pos=(%d,%d) species=%d peer_level=%d",
              (int16_t)(sm & 0xFFFF), (int16_t)((sm >> 16) & 0xFFFF), x, y,
              (int16_t)(sl & 0xFFFF), (int16_t)((sl >> 16) & 0xFFFF));
}

// IK field-fight report: logs ONLY the words where the engine's current
// values differ from what the mod last wrote, as idx:written->engine pairs,
// plus the accumulated changed-word mask for the last second.
//   a0 = engine's 40 current words, a1 = mod's 40 written words,
//   a2 = accumulated mask lo, a3 = accumulated mask hi
EXPORT void SSSVCoop_IkReport(uint8_t* rdram, recomp_context* ctx) {
    gpr cur = ctx->r4;
    gpr wr  = ctx->r5;
    uint32_t mlo = (uint32_t)ctx->r6;
    uint32_t mhi = (uint32_t)ctx->r7;
    char line[512];
    int pos = 0, k, diffs = 0;
    pos += snprintf(line + pos, sizeof(line) - pos,
                    "ik-fight mask=%02X%08X diffs:", mhi & 0xFF, mlo);
    for (k = 0; k < 40 && pos < (int)sizeof(line) - 24; k++) {
        uint16_t c = (uint16_t)MEM_H(2*k, cur);
        uint16_t w = (uint16_t)MEM_H(2*k, wr);
        if (c != w) {
            pos += snprintf(line + pos, sizeof(line) - pos,
                            " %d:%04X->%04X", k, w, c);
            diffs++;
        }
    }
    if (diffs == 0) {
        snprintf(line + pos, sizeof(line) - pos, " none");
    }
    coop_logc(LC_POSE, "%s", line);
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
        coop_logc(LC_SYS, "hook + config OK  (mode=%d ip=[%s] port=%d)", mode, ip, port);
    }
    {
        static int last_logged_status = -1;
        static const char* stname[5] = {"off","listening","connecting","CONNECTED","error"};
        double t = now_seconds();
        int sidx = (g_status >= 0 && g_status <= 4) ? g_status : 4;
        if (g_status != last_logged_status) {
            last_logged_status = g_status;
            last_summary = t;
            if (mode == 2) {
                coop_logc(LC_NET, "status: %s  (join, target=%s port=%d)", stname[sidx], ip, port);
            } else {
                coop_logc(LC_NET, "status: %s  (host, port=%d)", stname[sidx], port);
            }
        } else if (t - last_summary >= 30.0) {
            last_summary = t;
            coop_logc(LC_NET, "alive: %s, peer=%s", stname[sidx], g_have_peer ? "yes" : "no");
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
        // Outgoing: mission trio (io[125..127]); send on change or 1/s.
        {
            LevelPayload lv;
            lv.level    = MEM_H(0xFA, io);
            lv.in_level = MEM_H(0xFC, io);
            lv.epoch    = MEM_H(0xFE, io);
            if (g_have_peer) {
                double t2 = now_seconds();
                if (lv.level != g_own_level.level ||
                    lv.in_level != g_own_level.in_level ||
                    lv.epoch != g_own_level.epoch ||
                    t2 - g_last_level_tx >= HEARTBEAT_INTERVAL_S) {
                    send_level(&lv);
                }
            }
            g_own_level = lv;
        }
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
                for (k = 0; k < 109; k++) {
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
                for (k = 0; k < 109; k++) {
                    MEM_H(0x100 + 2*k, io) = (int16_t)g_peer_state.pose[k];  // io[128+k]
                }
            }
        }
        // Incoming: peer mission trio (io[237..239]); a stale trio (no
        // MSG_LEVEL for LEVEL_STALE_S) reports in_level = 0 so the mod
        // never follows a dead link into a level.
        {
            int fresh = g_have_peer && g_peer_level_time >= 0.0 &&
                        (now_seconds() - g_peer_level_time) <= LEVEL_STALE_S;
            MEM_H(0x1DA, io) = g_peer_level.level;
            MEM_H(0x1DC, io) = fresh ? g_peer_level.in_level : 0;
            MEM_H(0x1DE, io) = g_peer_level.epoch;
        }
    }

    ctx->r2 = (gpr)(int32_t)g_status;
}
