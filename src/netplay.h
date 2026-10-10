#pragma once
#include <stdbool.h>
#include <stdint.h>

// GGPO session driver. Battles run with rollback; everything else (title,
// menus, character select, loading) runs in lockstep on confirmed inputs, so a
// rollback never crosses a scene change. A battle may only end on a confirmed
// frame (the end is held back and the frame re-run once confirmed).

typedef struct {
    int local_player;          // 1 or 2
    uintptr_t sock;            // bound UDP socket (NAT already punched), handed to GGPO
    uint32_t peer_ip;          // the opponent's endpoint (network byte order)
    uint16_t peer_port;        // (host byte order)
    int delay;                 // local input delay in frames
    char peer_name[64];
} NetplaySetup;

bool netplay_start(const NetplaySetup *setup);
bool netplay_active(void);
void netplay_stop(const char *why);

typedef struct {
    bool running;
    int frame;
    int ping;
    int delay;
    int rollback_frames;   // frames re-simulated during the last second
    int frames_ahead;
    bool interrupted;
    bool desynced;
    char status[128];
} NetplayStatus;
NetplayStatus netplay_status(void);
