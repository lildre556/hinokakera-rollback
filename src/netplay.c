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
    uint8_t *battle = game_battle(frame_app());
    return !battle || *(int32_t *)(battle + BATTLE_STATE) == 7;
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

// Hash of the live state (called right after saving frame s->frame, so it is that frame's).
static void state_hash(Snap *s) {
    const uint32_t seed = 2166136261u;
    uint32_t h = seed, parts[8] = {0};
    void *app = frame_app();
    uint8_t *sys = game_sys(app);
    uint8_t *input = game_input(app);
    parts[0] = APP_U32(app, APP_TICKS);
    parts[1] = fnv32(seed, input + INPUT_REC, INPUT_REC_SIZE);
    parts[2] = G_RNG ? G_RNG[2] : 0;
    parts[3] = frame_holdrand() ? *frame_holdrand() : 0;
    // Round frame counter, round/win state of the match.
    parts[4] = fnv32(fnv32(seed, sys + 0x37c, 4), sys + 0x388, 8);
    // Battle state and its timer, both fighters' life and position.
    const uint8_t *battle = game_battle(app);
    if (battle) {
        parts[5] = fnv32(fnv32(seed, battle + BATTLE_STATE, 4), battle + 0x14, 4);
        for (int i = 0; i < 2; i++) {
            const uint8_t *f = *(const uint8_t *const *)(battle + BATTLE_FIGHTER + i * 4);
            if (f) {
                parts[6 + i] = fnv32(fnv32(seed, f + FIGHTER_LIFE, 4), f + FIGHTER_POS, 12);
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        h = fnv32(h, &parts[i], 4);
    }
    memcpy(s->hash_parts, parts, sizeof parts);
    s->hash = h;
    s->hashed = true;
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
        if (!s->battle || !s->hashed) {
            last_hashed = f;
            continue;
        }
        uint32_t h = s->hash;
#ifdef HINOROLL_HARNESS
        const uint32_t *pt = s->hash_parts;
        log_printf("state frame %d: ticks %08x input %08x rng %u holdrand %08x match %08x battle %08x fighters %08x %08x",
                   f, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7]);
#endif
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
    if (s->battle && frame % DESYNC_EVERY == 0) {
        state_hash(s);
    }
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
                if (ring[cur % RING]->battle && cur % DESYNC_EVERY == 0) {
                    state_hash(ring[cur % RING]);
                }
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

// ---- what both games must agree on outside battles ----
// The title menu online: no attract demo (it would start a battle on its
// own), and only the two-player modes: VS Human, Training, Watch. Arcade and
// VS Com work on records and unlocks after each battle, the rest reads or
// writes local files or is the game's own netplay. Both games see the same
// inputs, so both refuse the same presses.
static const bool offline_only[11] = {
    [0] = true, [1] = true, [3] = true, [6] = true, [7] = true, [8] = true, [9] = true, [10] = true,
};

static void title_filter(void *title, bool before) {
    if (!before) {
        return;
    }
    int32_t state = (int32_t)PTR_U32(title, TITLE_STATE);
    int32_t *idle = (int32_t *)((uint8_t *)title + TITLE_IDLE);
    if (state == 1 && *idle > 500) {
        // "Press start": the counter runs one behind +0x18 (the base scene's fade length).
        *idle -= 300;
        *(int32_t *)((uint8_t *)title + 0x18) -= 300;
    } else if (state == 2 && *idle > 500) {
        *idle = 0;
    }
    int32_t item = (int32_t)PTR_U32(title, TITLE_ITEM);
    uint8_t *input = game_input(frame_app());
    if (state == 2 && input && item >= 0 && item < 11 && offline_only[item]) {
        for (int p = 0; p < 2; p++) {
            *(int32_t *)(input + INPUT_REC + p * 0xa0 + 4 * 0x10 + 8) = 0;  // A: no "pressed" edge
        }
    }
}

// Unlocks, records and the select screens' starting picks come from each
// player's record.dat and story save: online both games use the same:
// empty records, everything unlocked (all 13 characters with their 4
// colours), default picks. (record.dat isn't saved online, see frame.c.)
static void same_records(void) {
    uint8_t *sys = game_sys(frame_app());
    if (!sys) {
        return;
    }
    // FUN_0049a260's layout: +0x490 version, +0x494.. per-character records,
    // +0xee8.. last picks, +0xf0c (20) and +0xf5c (100) more counters.
    memset(sys + 0x494, 0, 0xee8 - 0x494);
    memset(sys + 0xf0c, 0, 20 * 4);
    memset(sys + 0xf5c, 0, 100 * 4);
    PTR_U32(sys, 0x100) = 13;
    for (int i = 0; i < 13; i++) {
        PTR_U32(sys, 0x104 + i * 4) = 1;
        PTR_U32(sys, 0x138 + i * 4) = 4;
    }
    for (int p = 0; p < 2; p++) {
        PTR_U32(sys, 0xee8 + p * 4) = (uint32_t)p;  // last characters, colours...
        PTR_U32(sys, 0xef0 + p * 4) = 0;
        PTR_U32(sys, 0xef8 + p * 4) = 0;
        PTR_U32(sys, 0x318 + p * 0x28) = (uint32_t)p;  // ...and the copy the select screen starts from
        PTR_U32(sys, 0x31c + p * 0x28) = 0;
        PTR_U32(sys, 0x320 + p * 0x28) = 0;
    }
    PTR_U32(sys, 0xf00) = 0;  // stage, music
    PTR_U32(sys, 0xf04) = 0;
    PTR_U32(sys, 0xf08) = 0;
    PTR_U32(sys, 0x360) = 0;
    PTR_U32(sys, 0x364) = 0;
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
    same_records();
    frame_set_title_filter(title_filter);
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
    frame_set_title_filter(NULL);
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
