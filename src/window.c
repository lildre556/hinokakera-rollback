// Display modes (see window.h). The game knows a window and exclusive
// fullscreen (F4, or the maximize button, resets the device into the other).
// Exclusive fullscreen loses the device whenever another program takes the
// screen (alt-tab included); a borderless window covering the monitor never
// does, and alt-tabs instantly. The game still renders at its resolution in
// a windowed device; Present scales it into the middle of the monitor with
// black bars on the sides.
#include "window.h"

#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "log.h"

typedef int(THISCALL *toggle_fn)(void *app);
static const toggle_fn game_toggle_fullscreen = (toggle_fn)ADDR_TOGGLE_FULLSCREEN;

typedef HRESULT(WINAPI *present_fn)(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty);
static present_fn orig_present;

static int cur_mode = DISPLAY_WINDOWED;
static int want_mode = -1;
static int fs_kind = DISPLAY_BORDERLESS;
static bool toggle_requested;
static bool stretch;
static bool restore_from_maximized;
static LONG saved_style;
static RECT saved_rect;
static bool have_saved;
static char ini_path[MAX_PATH];

static const char *names[DISPLAY_MODES] = {"windowed", "borderless", "fullscreen"};

const char *window_mode_name(int mode) {
    return mode >= 0 && mode < DISPLAY_MODES ? names[mode] : "?";
}

static int parse_mode(const char *s, int def) {
    for (int i = 0; i < DISPLAY_MODES; i++) {
        if (_stricmp(s, names[i]) == 0) {
            return i;
        }
    }
    return def;
}

static void save(void) {
    WritePrivateProfileStringA("display", "mode", names[cur_mode], ini_path);
    WritePrivateProfileStringA("display", "fullscreen", names[fs_kind], ini_path);
    WritePrivateProfileStringA("display", "picture", stretch ? "stretch" : "keep", ini_path);
}

bool window_stretch(void) {
    return stretch;
}

void window_set_stretch(bool on) {
    stretch = on;
    save();
    void *app = frame_app();
    if (app && cur_mode == DISPLAY_BORDERLESS) {
        InvalidateRect((HWND)APP_PTR(app, APP_HWND), NULL, TRUE);
    }
}

int window_mode(void) {
    return want_mode >= 0 ? want_mode : cur_mode;
}

void window_set_mode(int mode) {
    if (mode >= 0 && mode < DISPLAY_MODES) {
        want_mode = mode;
        if (mode != DISPLAY_WINDOWED) {
            fs_kind = mode;
        }
    }
}

int window_fullscreen_kind(void) {
    return fs_kind;
}

void window_set_fullscreen_kind(int kind) {
    if (kind == DISPLAY_BORDERLESS || kind == DISPLAY_FULLSCREEN) {
        fs_kind = kind;
        save();
    }
}

void window_request_toggle(void) {
    toggle_requested = true;
}

void window_config_loaded(void *app) {
    snprintf(ini_path, sizeof ini_path, "%s\\hinoroll.ini", game_dir());
    char buf[32];
    GetPrivateProfileStringA("display", "fullscreen", "borderless", buf, sizeof buf, ini_path);
    fs_kind = parse_mode(buf, DISPLAY_BORDERLESS);
    if (fs_kind == DISPLAY_WINDOWED) {
        fs_kind = DISPLAY_BORDERLESS;
    }
    GetPrivateProfileStringA("display", "picture", "keep", buf, sizeof buf, ini_path);
    stretch = _stricmp(buf, "stretch") == 0;
    bool windowed = APP_U32(app, APP_WINDOWED) != 0;
    GetPrivateProfileStringA("display", "mode", "", buf, sizeof buf, ini_path);
    int mode = parse_mode(buf, windowed ? DISPLAY_WINDOWED : DISPLAY_FULLSCREEN);
    // The device is made windowed for both the window and borderless; the
    // window becomes borderless before the first frame.
    windowed = mode != DISPLAY_FULLSCREEN;
    APP_U32(app, APP_WINDOWED) = windowed;
    APP_U32(app, APP_CONFIG) = windowed;
    cur_mode = windowed ? DISPLAY_WINDOWED : DISPLAY_FULLSCREEN;
    if (mode == DISPLAY_BORDERLESS) {
        want_mode = DISPLAY_BORDERLESS;
    }
    log_printf("display: start %s (F4: %s)", names[mode], names[fs_kind]);
}

static HWND game_window(void *app) {
    return app ? (HWND)APP_PTR(app, APP_HWND) : NULL;
}

static RECT monitor_rect(HWND w) {
#ifdef HINOROLL_HARNESS
    // Test builds never cover anyone's screen: a 960x540 "monitor" where the window is.
    RECT t;
    GetWindowRect(w, &t);
    t.right = t.left + 960;
    t.bottom = t.top + 540;
    return t;
#endif
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

static void to_borderless(void *app) {
    HWND w = game_window(app);
    if (!have_saved) {
        saved_style = GetWindowLongA(w, GWL_STYLE);
        GetWindowRect(w, &saved_rect);
        have_saved = true;
    }
    RECT m = monitor_rect(w);
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
#ifdef HINOROLL_HARNESS
    SetWindowPos(w, HWND_BOTTOM, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
#else
    SetWindowPos(w, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
#endif
    InvalidateRect(w, NULL, TRUE);
}

static void to_window(void *app) {
    HWND w = game_window(app);
    if (have_saved) {
        SetWindowLongA(w, GWL_STYLE, saved_style);
#ifdef HINOROLL_HARNESS
        SetWindowPos(w, HWND_BOTTOM, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left,
                     saved_rect.bottom - saved_rect.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
#else
        SetWindowPos(w, HWND_NOTOPMOST, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left,
                     saved_rect.bottom - saved_rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
#endif
        have_saved = false;
    }
}

// The game's own switch (F4): resets the device into the other mode.
static bool game_toggle(void *app) {
    bool was_windowed = APP_U32(app, APP_WINDOWED) != 0;
    game_toggle_fullscreen(app);
    bool now_windowed = APP_U32(app, APP_WINDOWED) != 0;
    if (now_windowed == was_windowed) {
        log_printf("display: the game couldn't switch %s", was_windowed ? "to fullscreen" : "to a window");
        return false;
    }
    return true;
}

static void switch_mode(void *app, int to) {
    int from = cur_mode;
    if (from == to) {
        return;
    }
    if (from == DISPLAY_FULLSCREEN) {
        if (!game_toggle(app)) {
            return;
        }
        cur_mode = DISPLAY_WINDOWED;
    } else if (from == DISPLAY_BORDERLESS) {
        to_window(app);
        cur_mode = DISPLAY_WINDOWED;
    }
    if (to == DISPLAY_BORDERLESS) {
        to_borderless(app);
        cur_mode = DISPLAY_BORDERLESS;
    } else if (to == DISPLAY_FULLSCREEN) {
        if (game_toggle(app)) {
            cur_mode = DISPLAY_FULLSCREEN;
        }
    }
    log_printf("display: %s -> %s", names[from], names[cur_mode]);
}

#ifdef HINOROLL_HARNESS
static DWORD restore_at;  // test: un-minimize then
#endif

void window_before_frame(void *app) {
    static bool first = true;
#ifdef HINOROLL_HARNESS
    if (restore_at && (LONG)(GetTickCount() - restore_at) >= 0) {
        restore_at = 0;
        ShowWindow(game_window(app), SW_SHOWNOACTIVATE);
        log_printf("test: window restored");
    }
#endif
    if (toggle_requested) {
        toggle_requested = false;
        want_mode = cur_mode == DISPLAY_WINDOWED ? fs_kind : DISPLAY_WINDOWED;
    }
    if (restore_from_maximized) {
        restore_from_maximized = false;
        if (IsZoomed(game_window(app))) {
            ShowWindow(game_window(app), SW_RESTORE);
        }
    }
    if (want_mode < 0) {
        first = false;
        return;
    }
    int to = want_mode;
    want_mode = -1;
    switch_mode(app, to);
    if (!first) {
        save();
    }
    first = false;
}

// The part of the client area the game's picture goes in (whole client when
// the shapes match).
static RECT picture_rect(void *app, HWND w) {
    RECT c;
    GetClientRect(w, &c);
    int cw = c.right, ch = c.bottom;
    int gw = (int)APP_U32(app, APP_WIDTH), gh = (int)APP_U32(app, APP_HEIGHT);
    if (stretch || gw <= 0 || gh <= 0 || cw <= 0 || ch <= 0) {
        return c;
    }
    int w2 = cw, h2 = (int)((long long)cw * gh / gw);
    if (h2 > ch) {
        h2 = ch;
        w2 = (int)((long long)ch * gw / gh);
    }
    RECT r = {(cw - w2) / 2, (ch - h2) / 2, (cw - w2) / 2 + w2, (ch - h2) / 2 + h2};
    return r;
}

static void paint_bars(void *app, HWND w, HDC dc) {
    RECT c, p = picture_rect(app, w);
    GetClientRect(w, &c);
    HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bars[4] = {
        {c.left, c.top, c.right, p.top},
        {c.left, p.bottom, c.right, c.bottom},
        {c.left, p.top, p.left, p.bottom},
        {p.right, p.top, c.right, p.bottom},
    };
    for (int i = 0; i < 4; i++) {
        if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) {
            FillRect(dc, &bars[i], b);
        }
    }
}

bool window_msgproc(void *app, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out) {
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MAXIMIZED) {
            // The maximize button (the game turned it into its fullscreen switch).
            restore_from_maximized = true;
            if (cur_mode == DISPLAY_WINDOWED) {
                want_mode = fs_kind;
            }
            *out = 0;
            return true;
        }
        break;
    case WM_ERASEBKGND:
        if (cur_mode == DISPLAY_BORDERLESS) {
            paint_bars(app, hwnd, (HDC)wp);
            *out = 1;
            return true;
        }
        break;
    case WM_PAINT:
        if (cur_mode == DISPLAY_BORDERLESS) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paint_bars(app, hwnd, dc);
            EndPaint(hwnd, &ps);
            *out = 0;
            return true;
        }
        break;
    case WM_SETCURSOR:
        if (cur_mode == DISPLAY_BORDERLESS && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            *out = 1;
            return true;
        }
        break;
    case WM_DISPLAYCHANGE:
        if (cur_mode == DISPLAY_BORDERLESS) {
            want_mode = DISPLAY_BORDERLESS;
            cur_mode = DISPLAY_WINDOWED;  // re-fit to the monitor
        }
        break;
    }
    return false;
}

static HRESULT WINAPI hk_present(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty) {
    void *app = frame_app();
    if (frame_in_render()) {
        frame_before_present();
    }
    if (app && cur_mode == DISPLAY_BORDERLESS && !src && !dst) {
        HWND w = game_window(app);
        RECT p = picture_rect(app, w);
        return orig_present(dev, NULL, &p, wnd, dirty);
    }
    return orig_present(dev, src, dst, wnd, dirty);
}

void window_device_ready(void *device) {
    static bool done;
    if (done || !device) {
        return;
    }
    done = true;
    void **vt = *(void ***)device;
    hook_code((uintptr_t)vt[17], (void *)hk_present, (void **)&orig_present, "Present");
}

#ifdef HINOROLL_HARNESS
#include <stdlib.h>

#include "harness.h"
#include "resources.h"

// What alt-tabbing does to the game, without taking anyone's screen:
//   alttab focus     the focus-loss messages, then the focus-gain ones
//   alttab menu      Alt alone: the window menu loop starts and ends
//   alttab min MS    minimized for MS, then shown again (not activated)
//   alttab lost MS   the device reports lost for MS, then needs a Reset (exclusive fullscreen)
static int alttab_cmd(const char *cmd, const char *args) {
    if (strcmp(cmd, "alttab") != 0) {
        return 0;
    }
    void *app = frame_app();
    HWND w = game_window(app);
    if (!w) {
        harness_reply("alttab: no window");
        return 1;
    }
    if (strncmp(args, "focus", 5) == 0) {
        PostMessageA(w, WM_ACTIVATEAPP, FALSE, 0);
        PostMessageA(w, WM_ACTIVATE, WA_INACTIVE, 0);
        PostMessageA(w, WM_KILLFOCUS, 0, 0);
        PostMessageA(w, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageA(w, WM_ACTIVATE, WA_ACTIVE, 0);
    } else if (strncmp(args, "menu", 4) == 0) {
        PostMessageA(w, WM_ENTERMENULOOP, FALSE, 0);
        PostMessageA(w, WM_EXITMENULOOP, FALSE, 0);
    } else if (strncmp(args, "min", 3) == 0) {
        int ms = atoi(args + 3);
        ShowWindow(w, SW_SHOWMINNOACTIVE);
        restore_at = GetTickCount() + (DWORD)(ms > 0 ? ms : 2000);
    } else if (strncmp(args, "lost", 4) == 0) {
        int ms = atoi(args + 4);
        res_simulate_device_loss(ms > 0 ? ms : 2000);
    } else {
        harness_reply("alttab focus | menu | min MS | lost MS");
        return 1;
    }
    harness_reply("alttab %s", args);
    return 1;
}
#endif

void window_install(void) {
#ifdef HINOROLL_HARNESS
    harness_add_handler(alttab_cmd);
#endif
}
