#include "frame.h"

#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "game.h"
#include "heap.h"
#include "hooks.h"
#include "log.h"
#include "netplay.h"
#include "pads.h"
#include "session.h"
#include "resources.h"
#include "snapshot.h"

typedef int(THISCALL *app_fn)(void *app);
typedef int(THISCALL *set_mode_fn)(void *app, int mode);
typedef int(THISCALL *poll_fn)(void *input, int ms);
typedef uint32_t(THISCALL *get_fn)(void *input, int player);
typedef int(THISCALL *set_fn)(void *input, int player, uint32_t word);
typedef int(THISCALL *battle_update_fn)(void *battle, void *input);
typedef int(THISCALL *title_update_fn)(void *title, void *input);
typedef LRESULT(THISCALL *msgproc_fn)(void *app, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
typedef int(THISCALL *snd_play_fn)(void *buf, int a, int b, int c, int d, int e);
typedef int(THISCALL *snd_stop_fn)(void *buf);

static app_fn orig_framemove;
static app_fn orig_init;
static app_fn orig_render;
static set_mode_fn orig_set_mode;
static poll_fn orig_poll;
static battle_update_fn orig_battle_update;
static title_update_fn orig_title_update;
static msgproc_fn orig_msgproc;
static snd_play_fn orig_snd_play;
static snd_stop_fn orig_snd_stop;

static const get_fn input_get = (get_fn)ADDR_INPUT_GET;
static const set_fn input_set = (set_fn)ADDR_INPUT_SET;

static void *app;
bool padui_is_open(void);
bool padui_swallow_key(int vk);
static uint32_t *holdrand(void);
static DWORD main_tid;
static frame_driver_fn driver;
static frame_battle_end_fn battle_end_gate;
static bool keep_active;
static uint32_t ticks;
static frame_present_fn present_hooks[8];
static int present_hook_count;

// Inputs for frame_run (instead of the devices).
static bool inject;
static uint16_t inject_words[2];
static bool resim;

static bool override_on[2];
static uint16_t override_word[2];

void *frame_app(void) {
    return app;
}

int frame_mode(void) {
    return app ? (int)APP_U32(app, APP_MODE) : -1;
}

uint32_t frame_ticks(void) {
    return ticks;
}

bool frame_resimulating(void) {
    return resim;
}

void frame_set_driver(frame_driver_fn fn) {
    driver = fn;
}

void frame_set_battle_end_gate(frame_battle_end_fn fn) {
    battle_end_gate = fn;
}

void frame_keep_active(bool on) {
    keep_active = on;
    if (on && app) {
        APP_U32(app, APP_ACTIVE) = 1;
    }
}

void frame_add_present_hook(frame_present_fn fn) {
    if (present_hook_count < (int)(sizeof present_hooks / sizeof present_hooks[0])) {
        present_hooks[present_hook_count++] = fn;
    }
}

void frame_override_input(int player, bool on, uint16_t word) {
    if (player >= 0 && player < 2) {
        override_on[player] = on;
        override_word[player] = word;
    }
}

// The logic half of the input poll: last tick's held state becomes "prev",
// and button lockouts count down (FUN_00435a50's first loop).
static void input_logic_step(uint8_t *input, int ms) {
    for (int i = 0; i < 2 * INPUT_BUTTONS; i++) {
        int32_t *rec = (int32_t *)(input + INPUT_REC + i * 0x10);
        rec[1] = rec[0];
        rec[0] = 0;
        if (rec[3] > 0) {
            rec[3] -= ms;
            if (rec[3] < 1) {
                rec[3] = 0;
            }
        }
    }
}

static int THISCALL hk_poll(void *input, int ms) {
    if (inject) {
        input_logic_step(input, ms);
        input_set(input, 0, inject_words[0]);
        input_set(input, 1, inject_words[1]);
        return 0;
    }
    int r = orig_poll(input, ms);
    pads_update();
    for (int p = 0; p < 2; p++) {
        uint16_t pad = pads_word(p);
        if (pad) {
            input_set(input, p, (input_get(input, p) & INPUT_WORD_MASK) | pad);
        }
        if (override_on[p]) {
            input_set(input, p, override_word[p]);
        }
    }
    return r;
}

uint16_t frame_poll_local(int player) {
    if (!app || !APP_PTR(app, APP_INPUT)) {
        return 0;
    }
    uint8_t *input = APP_PTR(app, APP_INPUT);
    uint8_t saved[INPUT_REC_SIZE];
    memcpy(saved, input + INPUT_REC, sizeof saved);
    orig_poll(input, 0);
    uint16_t word = (uint16_t)(input_get(input, player) & INPUT_WORD_MASK);
    memcpy(input + INPUT_REC, saved, sizeof saved);
    pads_update();
    word |= pads_word(player);
    if (padui_is_open()) {
        word = 0;  // the setup screen has the controls
    }
    if (override_on[player]) {
        word = override_word[player];
    }
    return word;
}

static frame_observer_fn observers[4];
static int observer_count;

void frame_set_observer(frame_observer_fn fn) {
    if (observer_count < 4) {
        observers[observer_count++] = fn;
    }
}

static void observe(bool after, bool render) {
    for (int i = 0; i < observer_count; i++) {
        observers[i](after, render);
    }
}

static bool in_logic;

// The game polls Escape (leave battle / quit from the title) and F4 with
// GetAsyncKeyState, which sees the keyboard even when the game isn't focused,
// and isn't part of the synchronized input. Keys reach the game only while its
// window has the focus, and never reach logic while a driver (netplay, stress
// test) runs it.
static SHORT WINAPI hk_get_async_key_state(int vk) {
    if (padui_swallow_key(vk)) {
        return 0;
    }
    HWND w = app ? (HWND)APP_PTR(app, APP_HWND) : NULL;
    if (w && GetForegroundWindow() != w) {
        return 0;
    }
    if (in_logic && driver) {
        // F4 only toggles fullscreen (a window message): fine on real frames.
        return vk == VK_F4 && !resim ? GetAsyncKeyState(vk) : 0;
    }
    return GetAsyncKeyState(vk);
}

static int call_framemove(void) {
    observe(false, false);
    heap_route_push(APP_U32(app, APP_MODE) == MODE_BATTLE);
    in_logic = true;
    int r = orig_framemove(app);
    in_logic = false;
    heap_route_pop();
    ticks++;
    observe(true, false);
    return r;
}

int frame_run(uint16_t p1, uint16_t p2, bool is_resim) {
    inject = true;
    inject_words[0] = p1 & INPUT_WORD_MASK;
    inject_words[1] = p2 & INPUT_WORD_MASK;
    resim = is_resim;
    int r = call_framemove();
    resim = false;
    inject = false;
    return r;
}

int frame_run_vanilla(void) {
    return call_framemove();
}

static int THISCALL hk_framemove(void *a) {
    if (!app) {
        app = a;
        log_printf("first tick: app %p, mode %u", a, (unsigned)APP_U32(a, APP_MODE));
        res_install(APP_PTR(a, APP_DEVICE));
        snap_init(a);
        NetplaySetup setup;
        if (session_take(&setup)) {
            // Both sides start from the same random seeds.
            uint32_t *seed = holdrand();
            if (seed) {
                *seed = 0x48494e4f;
            }
            if (G_RNG) {
                G_RNG[2] = 0;
            }
            netplay_start(&setup);
        }
        if (keep_active) {
            APP_U32(app, APP_ACTIVE) = 1;
        }
    }
    if (driver) {
        return driver(a);
    }
    if (padui_is_open()) {
        return 0;  // offline the game waits while the controller setup is open
    }
    return call_framemove();
}

static bool in_render;

// The C runtime's rand() seed of the main thread (_tiddata + 0x14). Rendering
// and sound playback call rand() too; they get to use it but not to advance
// it, so the logic's random sequence doesn't depend on how often a frame is
// drawn or which sounds actually played.
static uint32_t *holdrand(void) {
    static uint32_t *p;
    if (!p) {
        uint8_t *ptd = TlsGetValue(*(DWORD *)0x4dac40);
        p = ptd ? (uint32_t *)(ptd + 0x14) : NULL;
    }
    return p;
}

bool frame_in_render(void) {
    return in_render;
}

// While the Direct3D device is lost (another program took the screen, a
// fullscreen game was alt-tabbed out of...) the game's loop neither renders
// nor ticks. A driver (netplay) keeps ticking at 60 Hz so the session survives.
typedef int(THISCALL *render3d_fn)(void *app);
static render3d_fn orig_render3d;
static int THISCALL hk_render3d(void *a) {
    int r = orig_render3d(a);
    if (driver && res_device_lost()) {
        static double next;
        double now = timeGetTime();
        if (now >= next) {
            next = now - next > 100 ? now + 1000.0 / 60 : next + 1000.0 / 60;
            driver(a);
        } else {
            Sleep(1);
        }
    }
    return r;
}

// The application's one-time init (vtable +0xc): the netplay handshake runs
// first, so the host's settings are in place before the game copies them.
static int THISCALL hk_init(void *a) {
    session_before_init(a);
    return orig_init(a);
}

static int THISCALL hk_render(void *a) {
    observe(false, true);
    in_render = true;
    uint32_t *seed = holdrand();
    uint32_t saved = seed ? *seed : 0;
    int r = orig_render(a);
    if (seed) {
        *seed = saved;
    }
    in_render = false;
    observe(true, true);
    for (int i = 0; i < present_hook_count; i++) {
        present_hooks[i](a);
    }
    return r;
}

static int THISCALL hk_set_mode(void *a, int mode) {
    if (mode == MODE_BATTLE) {
        res_reset_logic_state();  // a new battle reads the camera from the device again
    }
    heap_route_push(mode == MODE_BATTLE);
    int r = orig_set_mode(a, mode);
    heap_route_pop();
    log_printf("mode -> %d (%d)", mode, r);
    return r;
}

static int THISCALL hk_battle_update(void *battle, void *input) {
    int r = orig_battle_update(battle, input);
    if (r == 1 && battle_end_gate && !battle_end_gate()) {
        return 0;  // not yet: the driver re-runs this frame once it is confirmed
    }
    return r;
}

static int THISCALL hk_title_update(void *title, void *input) {
    // Online, the title menu greys out Config and Record like the game's own netplay.
    void *saved = G_NET;
    if (driver && !saved) {
        G_NET = (void *)1;
        // No attract demo while online (the title starts one after 600 idle frames).
        int32_t *idle = (int32_t *)((uint8_t *)title + 0x1c);
        int32_t state = *(int32_t *)((uint8_t *)title + 0x18);
        if ((state == 1 || state == 2) && *idle > 500) {
            *idle = 0;
        }
    }
    int r = orig_title_update(title, input);
    G_NET = saved;
    return r;
}

static LRESULT THISCALL hk_msgproc(void *a, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_SIZE || msg == WM_SYSCOMMAND || msg == WM_CLOSE) {
        log_printf("window: msg %04x wp %08x lp %08x", msg, (unsigned)wp, (unsigned)lp);
    }
    if (msg == WM_DEVICECHANGE) {
        pads_device_change();
    }
    LRESULT r = orig_msgproc(a, hwnd, msg, wp, lp);
    if (keep_active && (msg == WM_ACTIVATE || msg == WM_SIZE)) {
        APP_U32(a, APP_ACTIVE) = 1;
    }
    return r;
}

// DirectSound buffer play/stop (FUN_0048c0b0 / FUN_0048c1a0): silent while re-simulating.
static int THISCALL hk_snd_play(void *buf, int a, int b, int c, int d, int e) {
    if (resim) {
        return 0;
    }
    bool main = GetCurrentThreadId() == main_tid;
    uint32_t *seed = main ? holdrand() : NULL;
    uint32_t saved = seed ? *seed : 0;
    if (main) {
        heap_route_push(false);  // playback isn't game state: keep it out of the battle arena
    }
    int r = orig_snd_play(buf, a, b, c, d, e);
    if (main) {
        heap_route_pop();
    }
    if (seed) {
        *seed = saved;
    }
    return r;
}

static int THISCALL hk_snd_stop(void *buf) {
    if (resim) {
        return 0;
    }
    return orig_snd_stop(buf);
}

void frame_install(void) {
    main_tid = GetCurrentThreadId();
    orig_framemove = (app_fn)hook_vtable(APP_VTABLE, APP_VT_FRAMEMOVE, (void *)hk_framemove);
    orig_render = (app_fn)hook_vtable(APP_VTABLE, APP_VT_RENDER, (void *)hk_render);
    orig_init = (app_fn)hook_vtable(APP_VTABLE, 3, (void *)hk_init);
    hook_code(0x42caa0, (void *)hk_render3d, (void **)&orig_render3d, "Render3DEnvironment");
    hook_code(ADDR_SET_MODE, (void *)hk_set_mode, (void **)&orig_set_mode, "SetMode");
    hook_code(ADDR_INPUT_POLL, (void *)hk_poll, (void **)&orig_poll, "input poll");
    hook_code(ADDR_BATTLE_UPDATE, (void *)hk_battle_update, (void **)&orig_battle_update, "battle update");
    hook_code(ADDR_TITLE_UPDATE, (void *)hk_title_update, (void **)&orig_title_update, "title update");
    hook_code(ADDR_MSGPROC, (void *)hk_msgproc, (void **)&orig_msgproc, "MsgProc");
    void *gaks = (void *)hk_get_async_key_state;
    patch_bytes(0x4bb3d4, &gaks, sizeof gaks);  // USER32!GetAsyncKeyState import
    hook_code(0x48c0b0, (void *)hk_snd_play, (void **)&orig_snd_play, "sound play");
    hook_code(0x48c1a0, (void *)hk_snd_stop, (void **)&orig_snd_stop, "sound stop");
    if (orig_framemove != (app_fn)ADDR_FRAMEMOVE || orig_render != (app_fn)ADDR_RENDER) {
        log_printf("unexpected vtable entries %p %p", (void *)orig_framemove, (void *)orig_render);
    }
}
