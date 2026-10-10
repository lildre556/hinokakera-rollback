// Netplay status in the window title (ping, input delay, rollback), updated
// once a second while a session runs.
#include <stdio.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "netplay.h"

static DWORD last;
static char original[256];

static void on_present(void *app) {
    DWORD now = GetTickCount();
    if (now - last < 1000 || !app) {
        return;
    }
    last = now;
    HWND w = (HWND)APP_PTR(app, APP_HWND);
    if (!original[0]) {
        GetWindowTextA(w, original, sizeof original);
    }
    char title[256];
    if (netplay_active()) {
        NetplayStatus s = netplay_status();
        if (!s.running) {
            snprintf(title, sizeof title, "%s - netplay: %s", original, s.status);
        } else {
            snprintf(title, sizeof title, "%s - online | ping %d ms | delay %d | rollback %d f/s%s%s", original, s.ping,
                     s.delay, s.rollback_frames, s.interrupted ? " | CONNECTION INTERRUPTED" : "",
                     s.desynced ? " | DESYNC DETECTED" : "");
        }
    } else {
        snprintf(title, sizeof title, "%s", original);
    }
    char cur[256];
    GetWindowTextA(w, cur, sizeof cur);
    if (strcmp(cur, title) != 0) {
        SetWindowTextA(w, title);
    }
}

void overlay_install(void) {
    frame_add_present_hook(on_present);
}
