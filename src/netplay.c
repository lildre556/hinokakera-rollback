#include "netplay.h"

#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "ggponet.h"
#include "log.h"
#include "resources.h"
#include "snapshot.h"

#define RING 16                // saved states (GGPO keeps at most 10)
#define DESYNC_EVERY 30        // battle frames between state checks
#define HASH_SLOTS 64

static GGPOSession *gs;
static GGPOPlayerHandle handles[2], local_handle, remote_handle;
static NetplaySetup setup;
static struct sockaddr_in peer_addr;
static bool running, local_added;
static int cur;                 // the frame about to be simulated
static Snap *ring[RING];
static uint16_t ring_inputs[RING][2];
static bool end_pending;        // the battle wanted to end on an unconfirmed frame
static int end_frame;
static int skip_ticks;          // GGPO time sync: we are ahead
static int resim_frames, resim_frames_last_second, frames_ahead;
static DWORD second_start;
static bool interrupted, desynced, stopped;
static char status_text[128];

// State checks: hash of a confirmed battle frame, compared with the peer's.
typedef struct {
    int frame;
    uint32_t hash;
} FrameHash;
static FrameHash local_hashes[HASH_SLOTS], remote_hashes[HASH_SLOTS];

#define PKT_MAGIC 0x4c524e48u  // "HNRL"
#define PKT_HASH 1
typedef struct {
    uint32_t magic;
    uint32_t type;
    int32_t frame;
    uint32_t hash;
} HashPacket;

bool netplay_active(void) {
    return gs != NULL;
}

static void set_status(const char *s) {
    snprintf(status_text, sizeof status_text, "%s", s);
}

// ---- one frame ----
static void run(const uint16_t in_raw[2], bool resim) {
    bool battle = frame_mode() == MODE_BATTLE;
    if (battle != res_journal_on()) {
        res_set_journal(battle);
    }
    uint16_t in[2] = {in_raw[0], in_raw[1]};
    if (battle) {
        // No pause menu online: its Exit ends the battle at once, and one
        // player pausing would stop the other's game.
        in[0] &= ~0x4000;
        in[1] &= ~0x4000;
    }
    ring_inputs[cur % RING][0] = in[0];
    ring_inputs[cur % RING][1] = in[1];
    res_set_frame(cur);
    frame_run(in[0], in[1], resim);
    cur++;
}

// Lockstep (advance only on confirmed inputs) outside battles, and once a
// battle is finishing (state 7 lasts 58+ frames before the scene changes), so
// the battle ends on a frame no rollback can reach.
static bool lockstep_now(void) {
    if (frame_mode() != MODE_BATTLE) {
        return true;
    }
    uint8_t *battle = APP_PTR(frame_app(), APP_SCENE_BATTLE);
    return !battle || *(int32_t *)(battle + 0x68) == 7;
}

static int confirmed_frame(void) {
    int f = -1;
    ggpo_get_confirmed_frame(gs, &f);
    return f;
}

// The battle may end only on a confirmed frame; otherwise the driver waits for
// the confirmation and re-runs that frame (see the driver below).
static bool battle_end_gate(void) {
    if (confirmed_frame() >= cur) {
        return true;
    }
    if (!end_pending) {
        end_pending = true;
        end_frame = cur;
    }
    return false;
}

// ---- state checks ----
static uint32_t fnv32(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static uint32_t state_hash(const Snap *s) {
    uint32_t h = 2166136261u, parts[8] = {0};
    size_t n;
    const uint8_t *app = snap_region(s, "app", &n);
    // Match flow (0x2abe8..0x2adb4) and the frame timers (0x2b95c..): not the
    // per-player config or records in between.
    const uint32_t seed = 2166136261u;
    parts[0] = fnv32(seed, app + (0x2abe8 - 0x2a6d0), 0x2adb4 - 0x2abe8);
    // Battle results, mode, timers; not 0x2b99c (per machine, unused by logic).
    parts[1] = fnv32(seed, app + (0x2b95c - 0x2a6d0), 0x2b99c - 0x2b95c);
    parts[1] = fnv32(parts[1], app + (0x2b9a0 - 0x2a6d0), 0x2b9b4 - 0x2b9a0);
    const uint8_t *p = snap_region(s, "input", &n);
    parts[2] = fnv32(seed, p, n);
    h = fnv32(fnv32(fnv32(h, &parts[0], 4), &parts[1], 4), &parts[2], 4);
    p = snap_region(s, "rng", &n);
    parts[3] = *(const uint32_t *)(p + 8);
    h = fnv32(h, p + 8, 4);
    p = snap_region(s, "holdrand", &n);
    parts[4] = *(const uint32_t *)p;
    h = fnv32(h, p, n);
    p = snap_region(s, "replay", &n);
    parts[5] = *(const uint32_t *)(p + 0x3c);
    h = fnv32(h, p + 0x3c, 4);
    // Both fighters' life and position, read from the saved battle arena.
    void *app_live = frame_app();
    const uint8_t *battle = snap_arena_ptr(s, app_live ? APP_PTR(app_live, APP_SCENE_BATTLE) : NULL);
    if (battle) {
        for (int i = 0; i < 2; i++) {
            const uint8_t *f = snap_arena_ptr(s, *(void *const *)(battle + 0x20 + i * 4));
            if (f) {
                h = fnv32(h, f + 0x52c, 4);
                h = fnv32(h, f + 0x448, 12);
                parts[6 + i] = fnv32(fnv32(2166136261u, f + 0x52c, 4), f + 0x448, 12);
            }
        }
    }
#ifdef HINOROLL_HARNESS
    log_printf("state frame %d: match %08x timers %08x input %08x rng %u holdrand %08x replay %u fighters %08x %08x", s->frame,
               parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7]);
#endif
    return h;
}

static void store_hash(FrameHash *table, int frame, uint32_t hash) {
    table[(frame / DESYNC_EVERY) % HASH_SLOTS] = (FrameHash){frame, hash};
}

static void compare_hashes(int frame) {
    FrameHash a = local_hashes[(frame / DESYNC_EVERY) % HASH_SLOTS];
    FrameHash b = remote_hashes[(frame / DESYNC_EVERY) % HASH_SLOTS];
    if (a.frame == frame && b.frame == frame && a.hash != b.hash) {
        static int reported;
        desynced = true;
        if (reported++ < 20) {
            log_printf("netplay: DESYNC at frame %d (local %08x, remote %08x)", frame, a.hash, b.hash);
        }
    }
}

static void send_hash(int frame, uint32_t hash) {
    HashPacket pkt = {PKT_MAGIC, PKT_HASH, frame, hash};
    sendto((SOCKET)ggpo_hinoroll_socket(), (const char *)&pkt, sizeof pkt, 0, (const struct sockaddr *)&peer_addr,
           sizeof peer_addr);
}

static int __cdecl raw_packet(const void *from, const unsigned char *data, int len) {
    if (len < 8 || *(const uint32_t *)data != PKT_MAGIC) {
        return 0;
    }
    const HashPacket *pkt = (const HashPacket *)data;
    if (pkt->type == PKT_HASH && len >= (int)sizeof *pkt) {
        store_hash(remote_hashes, pkt->frame, pkt->hash);
        compare_hashes(pkt->frame);
    }
    return 1;  // ours (also the handshake's late packets)
}

static int last_hashed = -1;

static void check_state(int confirmed) {
    // Hash confirmed battle frames whose saved state is still in the ring.
    for (int f = confirmed - RING + 2; f <= confirmed; f++) {
        if (f <= last_hashed || f < 0 || f % DESYNC_EVERY != 0) {
            continue;
        }
        Snap *s = ring[f % RING];
        if (s->frame != f) {
            continue;
        }
        size_t n;
        const uint8_t *app = snap_region(s, "app", &n);
        if (*(const uint32_t *)(app + (APP_MODE - 0x2a6d0)) != MODE_BATTLE) {
            last_hashed = f;
            continue;
        }
        uint32_t h = state_hash(s);
        store_hash(local_hashes, f, h);
        send_hash(f, h);
        compare_hashes(f);
        last_hashed = f;
    }
}

// ---- GGPO callbacks ----
static bool __cdecl cb_begin_game(const char *game) {
    return true;
}

static bool __cdecl cb_save(unsigned char **buffer, int *len, int *checksum, int frame) {
    Snap *s = ring[frame % RING];
    snap_save(s, frame);
    int *token = malloc(sizeof(int));
    *token = frame;
    *buffer = (unsigned char *)token;
    *len = sizeof(int);
    *checksum = 0;
    return true;
}

static bool __cdecl cb_load(unsigned char *buffer, int len) {
    int frame = *(int *)buffer;
    Snap *s = ring[frame % RING];
    if (s->frame != frame) {
        log_printf("netplay: no saved state for frame %d (slot has %d)", frame, s->frame);
        return false;
    }
    snap_load(s);
    res_rollback(frame);
    cur = frame;
    end_pending = false;  // re-evaluated while re-simulating
    return true;
}

static bool __cdecl cb_log(char *filename, unsigned char *buffer, int len) {
    return true;
}

static void __cdecl cb_free(void *buffer) {
    free(buffer);
}

static bool __cdecl cb_advance(int flags) {
    uint16_t in[2] = {0, 0};
    int disconnected = 0;
    if (ggpo_synchronize_input(gs, in, sizeof in, &disconnected) != GGPO_OK) {
        return false;
    }
    run(in, true);
    resim_frames++;
    ggpo_advance_frame(gs);
    return true;
}

static bool __cdecl cb_event(GGPOEvent *e) {
    switch (e->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        set_status("connected, synchronizing");
        log_printf("netplay: connected to peer");
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        snprintf(status_text, sizeof status_text, "synchronizing %d/%d", e->u.synchronizing.count,
                 e->u.synchronizing.total);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        log_printf("netplay: synchronized");
        break;
    case GGPO_EVENTCODE_RUNNING:
        running = true;
        set_status("running");
        log_printf("netplay: running (player %d, delay %d)", setup.local_player, setup.delay);
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        interrupted = true;
        log_printf("netplay: connection interrupted");
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        interrupted = false;
        log_printf("netplay: connection resumed");
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        log_printf("netplay: disconnected");
        netplay_stop("the opponent disconnected");
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        frames_ahead = e->u.timesync.frames_ahead;
        skip_ticks = frames_ahead;
        break;
    default:
        break;
    }
    return true;
}

// ---- the driver: one call per game tick ----
static int driver(void *app) {
    if (!gs) {
        return frame_run_vanilla();
    }
    ggpo_idle(gs, 0);
    if (!gs) {
        return 0;  // stopped inside idle (disconnect)
    }
    DWORD now = GetTickCount();
    if (now - second_start >= 1000) {
        static int seconds, resim_total;
        resim_frames_last_second = resim_frames;
        resim_total += resim_frames;
        resim_frames = 0;
        second_start = now;
        if (++seconds % 10 == 0 && running) {
            NetplayStatus st = netplay_status();
            log_printf("netplay: frame %d, ping %d ms, %d frames re-simulated in the last 10 s, ahead %d", cur, st.ping,
                       resim_total, frames_ahead);
            resim_total = 0;
        }
    }
    if (!running) {
        return 0;
    }
    int confirmed = confirmed_frame();
    res_confirm(confirmed);
    check_state(confirmed);

    if (end_pending) {
        // Wait for the end frame's inputs, then run it again and let the battle end.
        if (confirmed >= end_frame) {
            Snap *s = ring[end_frame % RING];
            if (s->frame == end_frame && cur == end_frame + 1) {
                snap_load(s);
                res_rollback(end_frame);
                cur = end_frame;
                end_pending = false;
                uint16_t in[2] = {ring_inputs[end_frame % RING][0], ring_inputs[end_frame % RING][1]};
                run(in, false);
                snap_save(ring[cur % RING], cur);
                log_printf("netplay: battle ended on frame %d", end_frame);
            } else {
                end_pending = false;
            }
        }
        return 0;
    }
    if (skip_ticks > 0) {
        skip_ticks--;
        return 0;
    }
    if (!local_added) {
        uint16_t local = frame_poll_local(0);
        GGPOErrorCode r = ggpo_add_local_input(gs, local_handle, &local, sizeof local);
        if (r != GGPO_OK) {
            return 0;  // not synchronized yet, or too far ahead of the opponent
        }
        local_added = true;
    }
    if (lockstep_now() && confirmed_frame() < cur) {
        return 0;  // wait for the opponent's input
    }
    uint16_t in[2] = {0, 0};
    int disconnected = 0;
    if (ggpo_synchronize_input(gs, in, sizeof in, &disconnected) != GGPO_OK) {
        return 0;
    }
    run(in, false);
    ggpo_advance_frame(gs);
    local_added = false;
    return 0;
}

bool netplay_start(const NetplaySetup *s) {
    setup = *s;
    memset(&peer_addr, 0, sizeof peer_addr);
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = s->peer_ip;
    peer_addr.sin_port = htons(s->peer_port);
    srand(GetTickCount() ^ GetCurrentProcessId());  // GGPO's session magic numbers
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int i = 0; i < RING; i++) {
        if (!ring[i]) {
            ring[i] = snap_new();
        }
    }
    GGPOSessionCallbacks cb = {0};
    cb.begin_game = cb_begin_game;
    cb.save_game_state = cb_save;
    cb.load_game_state = cb_load;
    cb.log_game_state = cb_log;
    cb.free_buffer = cb_free;
    cb.advance_frame = cb_advance;
    cb.on_event = cb_event;
    ggpo_hinoroll_use_socket((unsigned int)s->sock);
    ggpo_hinoroll_set_raw_handler(raw_packet);
    GGPOErrorCode r = ggpo_start_session(&gs, &cb, "hinokakera", 2, sizeof(uint16_t), 0);
    if (r != GGPO_OK) {
        log_printf("netplay: ggpo_start_session failed (%d)", r);
        gs = NULL;
        return false;
    }
    // Battle loading runs inside one tick (3+ s here, more on a slow disk) without
    // network polling: give it time before calling the opponent gone.
    ggpo_set_disconnect_timeout(gs, 20000);
    ggpo_set_disconnect_notify_start(gs, 3000);
    char ip[32];
    snprintf(ip, sizeof ip, "%s", inet_ntoa(peer_addr.sin_addr));
    for (int p = 1; p <= 2; p++) {
        GGPOPlayer pl = {0};
        pl.size = sizeof pl;
        pl.player_num = p;
        if (p == s->local_player) {
            pl.type = GGPO_PLAYERTYPE_LOCAL;
        } else {
            pl.type = GGPO_PLAYERTYPE_REMOTE;
            snprintf(pl.u.remote.ip_address, sizeof pl.u.remote.ip_address, "%s", ip);
            pl.u.remote.port = s->peer_port;
        }
        r = ggpo_add_player(gs, &pl, &handles[p - 1]);
        if (r != GGPO_OK) {
            log_printf("netplay: ggpo_add_player %d failed (%d)", p, r);
        }
    }
    local_handle = handles[s->local_player - 1];
    remote_handle = handles[2 - s->local_player];
    ggpo_set_frame_delay(gs, local_handle, s->delay);
    cur = 0;
    running = local_added = end_pending = interrupted = desynced = stopped = false;
    last_hashed = -1;
    memset(local_hashes, 0xff, sizeof local_hashes);
    memset(remote_hashes, 0xff, sizeof remote_hashes);
    second_start = GetTickCount();
    set_status("waiting for the opponent");
    frame_set_battle_end_gate(battle_end_gate);
    frame_set_driver(driver);
    frame_keep_active(true);
    log_printf("netplay: session started as player %d, peer %s:%d, delay %d", s->local_player, ip, s->peer_port,
               s->delay);
    return true;
}

void netplay_stop(const char *why) {
    if (!gs || stopped) {
        return;
    }
    stopped = true;
    log_printf("netplay: stopping (%s) at frame %d", why, cur);
    GGPOSession *g = gs;
    gs = NULL;
    frame_set_driver(NULL);
    frame_set_battle_end_gate(NULL);
    res_set_journal(false);
    ggpo_close_session(g);
    snprintf(status_text, sizeof status_text, "offline: %s", why);
#ifndef HINOROLL_HARNESS
    void *app = frame_app();
    if (app) {
        MessageBoxA((HWND)APP_PTR(app, APP_HWND), why, "Hinokakera netplay", MB_OK | MB_ICONINFORMATION);
    }
#endif
}

NetplayStatus netplay_status(void) {
    NetplayStatus st = {0};
    st.running = running && gs;
    st.frame = cur;
    st.delay = setup.delay;
    st.rollback_frames = resim_frames_last_second;
    st.frames_ahead = frames_ahead;
    st.interrupted = interrupted;
    st.desynced = desynced;
    if (gs) {
        GGPONetworkStats ns;
        if (ggpo_get_network_stats(gs, remote_handle, &ns) == GGPO_OK) {
            st.ping = ns.network.ping;
        }
    }
    snprintf(st.status, sizeof st.status, "%s", status_text);
    return st;
}

// GGPO's assertions (it exits afterwards): log them, and tell the player
// unless this is a test build running in the background.
void hinoroll_ggpo_assert(const char *msg) {
    log_printf("GGPO assertion: %s", msg);
#ifndef HINOROLL_HARNESS
    MessageBoxA(NULL, msg, "Hinokakera netplay: GGPO error", MB_OK | MB_ICONERROR);
#endif
}
