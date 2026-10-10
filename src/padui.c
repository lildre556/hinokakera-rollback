// The settings screen (F8), drawn over the game right before Present: a
// panel painted with GDI into a texture. Three pages: Controls (each player's
// controller and bindings), Display (window, borderless window or
// fullscreen, what F4 switches to, the picture's shape) and Audio (master,
// music and effects volume). Keyboard (arrows, Enter, Delete, Esc, Tab) or
// any controller (D-pad/stick, A/Button 1 to choose, B/Button 2 to go back,
// LB/RB to change pages). Changes apply at once and are saved in
// hinoroll.ini; none of them touch the game's state, so they work online too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>

#include "audio.h"
#include "frame.h"
#include "game.h"
#include "log.h"
#include "pads.h"
#include "window.h"

#define CANVAS_W 640
#define CANVAS_H 480
#define TEX_W 1024
#define TEX_H 512

enum { PAGE_CONTROLS, PAGE_DISPLAY, PAGE_AUDIO, PAGES };
static const char *page_names[PAGES] = {"Controls", "Display", "Audio"};

// Rows of each page; row 0 is always the page tabs.
enum { C_DEVICE = 1, C_FIRST_ACTION = 2, C_RESET = C_FIRST_ACTION + PAD_ACTIONS, C_ROWS };
enum { D_MODE = 1, D_F4, D_PICTURE, D_ROWS };
enum { A_MASTER = 1, A_MUSIC, A_EFFECTS, A_ROWS };
static const int page_rows[PAGES] = {C_ROWS, D_ROWS, A_ROWS};

enum { MODE_NAV, MODE_BIND, MODE_ASSIGN };

static bool open_;
static int page, row = 1, col, mode;
static int pending_display = -1;  // window mode chosen, not applied yet
static DWORD mode_since, closed_at, toast_until;
static char message[160];
static bool dirty = true;

static HDC dc;
static HBITMAP dib;
static uint32_t *pixels;
static HFONT font, font_big, font_small;
static IDirect3DTexture9 *tex;
static void *tex_device;
static char toast[128];
static IDirect3DTexture9 *toast_tex;
static bool toast_dirty;
static float screen_w = 800, screen_h = 600;

bool padui_is_open(void) {
    return open_;
}

// Keys the game must not see right after the screen used them (Escape quits
// battles and the game).
bool padui_swallow_key(int vk) {
    return vk == VK_ESCAPE && (open_ || GetTickCount() - closed_at < 600);
}

static void set_open(bool on) {
    if (on == open_) {
        return;
    }
    open_ = on;
    pads_set_ui_open(on);
    mode = MODE_NAV;
    message[0] = 0;
    pending_display = -1;
    dirty = true;
    if (!on) {
        closed_at = GetTickCount();
    }
    log_printf("settings %s", on ? "opened" : "closed");
}

void padui_toast(const char *text, int ms) {
    snprintf(toast, sizeof toast, "%s", text);
    toast_until = GetTickCount() + ms;
    toast_dirty = true;
}

// ---- input for the screen ----
static SHORT key_state[256];

static bool key_pressed(int vk) {
    SHORT s = GetAsyncKeyState(vk);
    bool down = s < 0, was = key_state[vk] < 0;
    key_state[vk] = s;
    return down && !was;
}

static bool game_focused(void) {
    void *app = frame_app();
    return app && GetForegroundWindow() == (HWND)APP_PTR(app, APP_HWND);
}

// Test builds: keys from the harness (real keys need the focus).
#ifdef HINOROLL_HARNESS
static int harness_key;
#endif

// Held arrows repeat (sliders, long lists): after 350 ms, every 60 ms.
static bool repeat(int dir, bool held) {
    static DWORD since[4], last[4];
    static bool was[4];
    DWORD now = GetTickCount();
    if (!held) {
        was[dir] = false;
        return false;
    }
    if (!was[dir]) {
        was[dir] = true;
        since[dir] = last[dir] = now;
        return false;  // the press itself counts elsewhere
    }
    if (now - since[dir] > 350 && now - last[dir] >= 60) {
        last[dir] = now;
        return true;
    }
    return false;
}

static void start_bind(void) {
    int d = pads_player_device(col);
    if (d < 0) {
        snprintf(message, sizeof message, "Player %d has no controller: choose one first (Controller row).", col + 1);
        return;
    }
    mode = MODE_BIND;
    mode_since = GetTickCount();
    pads_capture_begin(d);
    snprintf(message, sizeof message, "Press the control for Player %d %s on %s   (Esc: cancel)", col + 1,
             pads_action_name(row - C_FIRST_ACTION), pads_device_name(d));
}

// A button newly pressed on some device (for picking a player's controller).
static int pressed_device(void) {
    for (int d = 0; d < pads_device_count(); d++) {
        static bool was_idle[PAD_MAX_DEVICES];
        bool idle = pads_device_idle(d);
        bool fresh = !idle && was_idle[d];
        was_idle[d] = idle;
        if (fresh) {
            return d;
        }
    }
    return -1;
}

static void change_page(int delta) {
    page = (page + PAGES + delta) % PAGES;
    if (row >= page_rows[page]) {
        row = page_rows[page] - 1;
    }
    message[0] = 0;
    pending_display = -1;
    dirty = true;
}

// Left/right on a row (delta -1 / +1).
static void adjust(int delta) {
    switch (page) {
    case PAGE_CONTROLS:
        col ^= 1;
        message[0] = 0;
        break;
    case PAGE_DISPLAY:
        if (row == D_MODE) {
            int cur = pending_display >= 0 ? pending_display : window_mode();
            pending_display = (cur + DISPLAY_MODES + delta) % DISPLAY_MODES;
            if (pending_display == window_mode()) {
                pending_display = -1;
                message[0] = 0;
            } else {
                snprintf(message, sizeof message, "Enter / A: switch to %s.",
                         pending_display == DISPLAY_WINDOWED     ? "the window"
                         : pending_display == DISPLAY_BORDERLESS ? "a borderless window over the whole monitor"
                                                                 : "exclusive fullscreen");
            }
        } else if (row == D_F4) {
            window_set_fullscreen_kind(window_fullscreen_kind() == DISPLAY_BORDERLESS ? DISPLAY_FULLSCREEN
                                                                                      : DISPLAY_BORDERLESS);
        } else if (row == D_PICTURE) {
            window_set_stretch(!window_stretch());
        }
        break;
    case PAGE_AUDIO: {
        int kind = row == A_MASTER ? AUDIO_MASTER : row == A_MUSIC ? AUDIO_MUSIC : AUDIO_EFFECTS;
        audio_set_level(kind, audio_level(kind) + delta * 5);
        break;
    }
    }
    dirty = true;
}

static void choose(void) {
    if (page == PAGE_CONTROLS) {
        if (row == C_DEVICE) {
            mode = MODE_ASSIGN;
            mode_since = GetTickCount();
            snprintf(message, sizeof message,
                     "Press a button on the controller for Player %d   (Delete: keyboard only, Esc: cancel)", col + 1);
        } else if (row == C_RESET) {
            pads_reset(col);
            snprintf(message, sizeof message, "Player %d's controls are back to the defaults.", col + 1);
        } else if (row >= C_FIRST_ACTION) {
            start_bind();
        }
    } else if (page == PAGE_DISPLAY) {
        if (row == D_MODE && pending_display >= 0) {
            window_set_mode(pending_display);
            snprintf(message, sizeof message, "Display: %s.", window_mode_name(pending_display));
            pending_display = -1;
        } else {
            adjust(1);
        }
    } else if (page == PAGE_AUDIO) {
        int kind = row == A_MASTER ? AUDIO_MASTER : row == A_MUSIC ? AUDIO_MUSIC : AUDIO_EFFECTS;
        audio_set_level(kind, audio_level(kind) > 0 ? 0 : 100);  // Enter: mute / full
    }
}

static void handle_input(void) {
    bool focused = game_focused();
    if (focused && key_pressed(VK_F8)) {
        set_open(!open_);
        return;
    }
    if (!open_) {
        return;
    }
    pads_update();
    unsigned pad = pads_ui_buttons(), held = pads_ui_held();
    bool k_up = focused && key_pressed(VK_UP), k_down = focused && key_pressed(VK_DOWN);
    bool k_left = focused && key_pressed(VK_LEFT), k_right = focused && key_pressed(VK_RIGHT);
    bool k_enter = focused && key_pressed(VK_RETURN), k_esc = focused && key_pressed(VK_ESCAPE);
    bool k_del = focused && (key_pressed(VK_DELETE) | key_pressed(VK_BACK));
    bool k_tab = focused && key_pressed(VK_TAB);
    bool shift = focused && GetAsyncKeyState(VK_SHIFT) < 0;
    bool h_up = (focused && GetAsyncKeyState(VK_UP) < 0) || (held & 1);
    bool h_down = (focused && GetAsyncKeyState(VK_DOWN) < 0) || (held & 2);
    bool h_left = (focused && GetAsyncKeyState(VK_LEFT) < 0) || (held & 4);
    bool h_right = (focused && GetAsyncKeyState(VK_RIGHT) < 0) || (held & 8);
#ifdef HINOROLL_HARNESS
    k_up |= harness_key == VK_UP;
    k_down |= harness_key == VK_DOWN;
    k_left |= harness_key == VK_LEFT;
    k_right |= harness_key == VK_RIGHT;
    k_enter |= harness_key == VK_RETURN;
    k_esc |= harness_key == VK_ESCAPE;
    k_del |= harness_key == VK_DELETE;
    k_tab |= harness_key == VK_TAB;
    harness_key = 0;
#endif

    if (mode == MODE_BIND) {
        PadControl c = pads_capture_poll();
        if (c) {
            int action = row - C_FIRST_ACTION;
            pads_bind(col, action, c);
            char name[48];
            pads_control_name(pads_player_device(col), c, name, sizeof name);
            snprintf(message, sizeof message, "Player %d %s: added %s.", col + 1, pads_action_name(action), name);
            mode = MODE_NAV;
            dirty = true;
        } else if (k_esc || GetTickCount() - mode_since > 10000) {
            mode = MODE_NAV;
            snprintf(message, sizeof message, "Cancelled.");
            dirty = true;
        }
        return;
    }
    if (mode == MODE_ASSIGN) {
        int d = pressed_device();
        if (d >= 0 && GetTickCount() - mode_since > 250) {
            pads_assign(col, d);
            snprintf(message, sizeof message, "Player %d: %s.", col + 1, pads_device_name(d));
            mode = MODE_NAV;
            dirty = true;
        } else if (k_del) {
            pads_assign(col, -1);
            snprintf(message, sizeof message, "Player %d: keyboard only.", col + 1);
            mode = MODE_NAV;
            dirty = true;
        } else if (k_esc) {
            mode = MODE_NAV;
            snprintf(message, sizeof message, "Cancelled.");
            dirty = true;
        }
        return;
    }
    if (k_tab || (pad & 0xc0)) {
        change_page((k_tab && shift) || (pad & 0x40) ? -1 : 1);
        return;
    }
    if (k_up || (pad & 1) || repeat(0, h_up)) {
        row = (row + page_rows[page] - 1) % page_rows[page];
        if (page != PAGE_DISPLAY || row != D_MODE) {
            pending_display = -1;
        }
        message[0] = 0;
        dirty = true;
    }
    if (k_down || (pad & 2) || repeat(1, h_down)) {
        row = (row + 1) % page_rows[page];
        message[0] = 0;
        pending_display = -1;
        dirty = true;
    }
    bool left = k_left || (pad & 4) || (page == PAGE_AUDIO && repeat(2, h_left));
    bool right = k_right || (pad & 8) || (page == PAGE_AUDIO && repeat(3, h_right));
    if (left || right) {
        if (row == 0) {
            change_page(left ? -1 : 1);
        } else {
            adjust(left ? -1 : 1);
        }
    }
    if (k_enter || (pad & 16)) {
        if (row == 0) {
            row = 1;
        } else {
            choose();
            pressed_device();  // don't count this press as a pick
        }
        dirty = true;
    }
    if (k_del && page == PAGE_CONTROLS && row >= C_FIRST_ACTION && row < C_RESET) {
        pads_clear(col, row - C_FIRST_ACTION);
        snprintf(message, sizeof message, "Cleared Player %d %s.", col + 1, pads_action_name(row - C_FIRST_ACTION));
        dirty = true;
    }
    if (k_esc || (pad & 32)) {
        set_open(false);
    }
}

// ---- painting ----
static bool gdi_init(void) {
    if (dc) {
        return true;
    }
    dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = TEX_W;
    bi.bmiHeader.biHeight = -TEX_H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!dib) {
        return false;
    }
    SelectObject(dc, dib);
    font = CreateFontA(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    font_big = CreateFontA(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    font_small = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    return true;
}

static void fill(int x, int y, int w, int h, COLORREF c) {
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text(int x, int y, int w, HFONT f, COLORREF c, const char *s) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT r = {x, y, x + w, y + 30};
    DrawTextA(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

#define C_TEXT RGB(225, 225, 235)
#define C_LABEL RGB(200, 200, 215)
#define C_DIM RGB(150, 150, 170)
#define C_SEL RGB(70, 55, 120)
#define C_ACCENT RGB(170, 60, 90)

static void paint_tabs(void) {
    int x = 20, y = 46;
    for (int p = 0; p < PAGES; p++) {
        SelectObject(dc, font);
        SIZE sz;
        GetTextExtentPoint32A(dc, page_names[p], (int)strlen(page_names[p]), &sz);
        int w = sz.cx + 28;
        bool cur = p == page;
        if (cur) {
            fill(x, y, w, 28, row == 0 ? C_SEL : RGB(45, 38, 70));
            fill(x, y + 26, w, 2, C_ACCENT);
        }
        text(x + 14, y + 4, w, font, cur ? RGB(255, 255, 255) : C_DIM, page_names[p]);
        x += w + 6;
    }
    fill(0, y + 30, CANVAS_W, 1, RGB(70, 60, 100));
}

static void row_box(int y, int x, int w, bool sel) {
    if (sel) {
        fill(x - 6, y, w, 24 - 2, mode == MODE_NAV ? C_SEL : RGB(130, 90, 30));
    }
}

static void paint_controls(void) {
    const int label_x = 20, cx[2] = {150, 395}, cw = 235, top = 84, rh = 24;
    text(cx[0], top, cw, font, RGB(255, 200, 120), "Player 1");
    text(cx[1], top, cw, font, RGB(120, 190, 255), "Player 2");
    for (int r = C_DEVICE; r < C_ROWS; r++) {
        int y = top + 26 + (r - C_DEVICE) * rh + (r >= C_FIRST_ACTION ? 4 : 0) + (r == C_RESET ? 4 : 0);
        const char *label = r == C_DEVICE ? "Controller" : r == C_RESET ? "" : pads_action_name(r - C_FIRST_ACTION);
        text(label_x, y + 3, 125, font, C_LABEL, label);
        for (int c = 0; c < 2; c++) {
            bool sel = r == row && c == col;
            row_box(y, cx[c], cw, sel);
            char buf[160] = "";
            int d = pads_player_device(c);
            if (r == C_DEVICE) {
                snprintf(buf, sizeof buf, "%s", d >= 0 ? pads_device_name(d) : "Keyboard only");
            } else if (r == C_RESET) {
                snprintf(buf, sizeof buf, "Reset to defaults");
            } else if (d < 0) {
                snprintf(buf, sizeof buf, "-");
            } else {
                const PadControl *b = pads_bindings(c, r - C_FIRST_ACTION);
                for (int i = 0; i < PAD_MAX_BINDS; i++) {
                    if (b[i]) {
                        char n[48];
                        pads_control_name(d, b[i], n, sizeof n);
                        if (buf[0]) {
                            strncat(buf, ", ", sizeof buf - strlen(buf) - 1);
                        }
                        strncat(buf, n, sizeof buf - strlen(buf) - 1);
                    }
                }
                if (!buf[0]) {
                    snprintf(buf, sizeof buf, "-");
                }
            }
            text(cx[c], y + 3, cw - 12, font, sel ? RGB(255, 255, 255) : C_TEXT, buf);
        }
    }
}

// "<  value  >" choice box.
static void choice(int y, bool sel, const char *value, bool pending) {
    const int x = 250, w = 360;
    row_box(y, x, w, sel);
    char buf[96];
    snprintf(buf, sizeof buf, sel ? "<   %s   >" : "%s", value);
    text(x, y + 3, w - 12, font, pending ? RGB(255, 220, 110) : sel ? RGB(255, 255, 255) : C_TEXT, buf);
}

static void paint_display(void) {
    const int top = 96, rh = 34;
    static const char *mode_text[DISPLAY_MODES] = {"Window", "Borderless window", "Fullscreen (exclusive)"};
    int shown = pending_display >= 0 ? pending_display : window_mode();
    text(20, top + 3, 220, font, C_LABEL, "Window mode");
    choice(top, row == D_MODE, mode_text[shown], pending_display >= 0);
    text(20, top + rh + 3, 220, font, C_LABEL, "F4 / maximize switches to");
    choice(top + rh, row == D_F4,
           window_fullscreen_kind() == DISPLAY_BORDERLESS ? "Borderless window" : "Fullscreen (exclusive)", false);
    text(20, top + 2 * rh + 3, 220, font, C_LABEL, "Borderless picture");
    choice(top + 2 * rh, row == D_PICTURE, window_stretch() ? "Fill the monitor" : "Keep the game's shape", false);
    text(20, top + 3 * rh + 22, 600, font_small, C_DIM,
         "Borderless: a window covering the monitor. Alt-Tab is instant and never");
    text(20, top + 3 * rh + 42, 600, font_small, C_DIM,
         "interrupts the game; exclusive fullscreen loses the screen on Alt-Tab.");
    text(20, top + 3 * rh + 62, 600, font_small, C_DIM,
         "The game still draws at the size set in config.exe; it is scaled up.");
}

static void slider(int y, bool sel, int level) {
    const int x = 250, w = 300, h = 10;
    row_box(y, x, w + 70, sel);
    int by = y + 7;
    fill(x, by, w, h, RGB(40, 36, 58));
    fill(x, by, w * level / 100, h, sel ? RGB(220, 120, 150) : C_ACCENT);
    char v[16];
    snprintf(v, sizeof v, "%d", level);
    text(x + w + 14, y + 1, 50, font, sel ? RGB(255, 255, 255) : C_TEXT, v);
}

static void paint_audio(void) {
    const int top = 96, rh = 34;
    static const char *labels[] = {"Master", "Music", "Effects"};
    static const int kinds[] = {AUDIO_MASTER, AUDIO_MUSIC, AUDIO_EFFECTS};
    for (int i = 0; i < 3; i++) {
        text(20, top + i * rh + 1, 220, font, C_LABEL, labels[i]);
        slider(top + i * rh, row == A_MASTER + i, audio_level(kinds[i]));
    }
    text(20, top + 3 * rh + 22, 600, font_small, C_DIM,
         "Music and Effects are the game's own volume settings (its Config menu");
    text(20, top + 3 * rh + 42, 600, font_small, C_DIM, "shows the same numbers); Master turns both down together.");
}

static void paint_panel(void) {
    memset(pixels, 0, TEX_W * TEX_H * 4);
    fill(0, 0, CANVAS_W, CANVAS_H, RGB(18, 16, 28));
    fill(0, 0, CANVAS_W, 3, C_ACCENT);
    text(20, 10, 400, font_big, RGB(255, 255, 255), "Settings");
    text(420, 16, 210, font_small, C_DIM, "F8 or Esc to close");
    paint_tabs();
    if (page == PAGE_CONTROLS) {
        paint_controls();
    } else if (page == PAGE_DISPLAY) {
        paint_display();
    } else {
        paint_audio();
    }
    fill(0, CANVAS_H - 66, CANVAS_W, 1, RGB(70, 60, 100));
    if (message[0]) {
        text(20, CANVAS_H - 58, 600, font_small, mode == MODE_NAV ? RGB(150, 230, 150) : RGB(255, 220, 110), message);
    } else if (page == PAGE_CONTROLS) {
        text(20, CANVAS_H - 58, 600, font_small, C_DIM,
             "Each action can have up to 4 controls. Keyboard keys are set in config.exe.");
    } else if (page == PAGE_AUDIO) {
        text(20, CANVAS_H - 58, 600, font_small, C_DIM, "Left / Right: change (hold to slide)     Enter / A: mute / full");
    } else {
        text(20, CANVAS_H - 58, 600, font_small, C_DIM, "Left / Right: choose     Enter / A: apply");
    }
    text(20, CANVAS_H - 34, 610, font_small, C_DIM,
         page == PAGE_CONTROLS ? "Arrows / D-pad: move   Enter / A: change   Delete: clear   Tab / LB RB: page   Esc / B: close"
                               : "Arrows / D-pad: move     Tab / LB RB: page     Esc / B: close");
    for (int y = 0; y < CANVAS_H; y++) {
        uint32_t *line = pixels + y * TEX_W;
        for (int x = 0; x < CANVAS_W; x++) {
            line[x] = (line[x] & 0x00ffffff) | 0xec000000u;
        }
    }
}

static void paint_toast(void) {
    memset(pixels, 0, TEX_W * 64 * 4);
    SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32A(dc, toast, (int)strlen(toast), &sz);
    int w = sz.cx + 24;
    fill(0, 0, w, 30, RGB(18, 16, 28));
    fill(0, 0, 3, 30, C_ACCENT);
    text(12, 5, w, font, RGB(235, 235, 245), toast);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < w && x < TEX_W; x++) {
            pixels[y * TEX_W + x] = (pixels[y * TEX_W + x] & 0x00ffffff) | 0xd8000000u;
        }
    }
}

// ---- Direct3D ----
static bool upload(IDirect3DDevice9 *dev, IDirect3DTexture9 **t, int rows) {
    if (!*t && FAILED(IDirect3DDevice9_CreateTexture(dev, TEX_W, TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t, NULL))) {
        return false;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(*t, 0, &lr, NULL, 0))) {
        return false;
    }
    for (int y = 0; y < rows; y++) {
        memcpy((uint8_t *)lr.pBits + y * lr.Pitch, pixels + y * TEX_W, TEX_W * 4);
    }
    IDirect3DTexture9_UnlockRect(*t, 0);
    return true;
}

// Only the states this changes are saved and put back (a full state block
// every frame cost the game several frames per second).
static const D3DRENDERSTATETYPE saved_rs[] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP,
};
static const D3DTEXTURESTAGESTATETYPE saved_tss[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
};
static const D3DSAMPLERSTATETYPE saved_ss[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
};
#define NRS (sizeof saved_rs / sizeof saved_rs[0])
#define NTSS (sizeof saved_tss / sizeof saved_tss[0])
#define NSS (sizeof saved_ss / sizeof saved_ss[0])

typedef struct {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} Vtx;
#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void draw_quad(IDirect3DDevice9 *dev, IDirect3DTexture9 *t, float x, float y, float w, float h, float tw, float th) {
    DWORD rs[NRS], tss0[NTSS], ss0[NSS], tss1_color, tss1_alpha, fvf = 0;
    IDirect3DBaseTexture9 *old_tex = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DVertexDeclaration9 *decl = NULL;
    D3DVIEWPORT9 vp;
    for (unsigned i = 0; i < NRS; i++) {
        IDirect3DDevice9_GetRenderState(dev, saved_rs[i], &rs[i]);
    }
    for (unsigned i = 0; i < NTSS; i++) {
        IDirect3DDevice9_GetTextureStageState(dev, 0, saved_tss[i], &tss0[i]);
    }
    for (unsigned i = 0; i < NSS; i++) {
        IDirect3DDevice9_GetSamplerState(dev, 0, saved_ss[i], &ss0[i]);
    }
    IDirect3DDevice9_GetTextureStageState(dev, 1, D3DTSS_COLOROP, &tss1_color);
    IDirect3DDevice9_GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &tss1_alpha);
    IDirect3DDevice9_GetTexture(dev, 0, &old_tex);
    IDirect3DDevice9_GetVertexShader(dev, &vs);
    IDirect3DDevice9_GetPixelShader(dev, &ps);
    IDirect3DDevice9_GetVertexDeclaration(dev, &decl);
    IDirect3DDevice9_GetFVF(dev, &fvf);
    IDirect3DDevice9_GetViewport(dev, &vp);

    IDirect3DDevice9_BeginScene(dev);
    D3DVIEWPORT9 full = {0, 0, (DWORD)screen_w, (DWORD)screen_h, 0.0f, 1.0f};
    IDirect3DDevice9_SetViewport(dev, &full);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, VTX_FVF);
    float u = tw / TEX_W, v = th / TEX_H;
    Vtx q[4] = {
        {x - 0.5f, y - 0.5f, 0, 1, 0xffffffff, 0, 0},
        {x + w - 0.5f, y - 0.5f, 0, 1, 0xffffffff, u, 0},
        {x - 0.5f, y + h - 0.5f, 0, 1, 0xffffffff, 0, v},
        {x + w - 0.5f, y + h - 0.5f, 0, 1, 0xffffffff, u, v},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
    IDirect3DDevice9_EndScene(dev);

    IDirect3DDevice9_SetViewport(dev, &vp);
    for (unsigned i = 0; i < NRS; i++) {
        IDirect3DDevice9_SetRenderState(dev, saved_rs[i], rs[i]);
    }
    for (unsigned i = 0; i < NTSS; i++) {
        IDirect3DDevice9_SetTextureStageState(dev, 0, saved_tss[i], tss0[i]);
    }
    for (unsigned i = 0; i < NSS; i++) {
        IDirect3DDevice9_SetSamplerState(dev, 0, saved_ss[i], ss0[i]);
    }
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, tss1_color);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, tss1_alpha);
    IDirect3DDevice9_SetTexture(dev, 0, old_tex);
    if (old_tex) {
        IDirect3DBaseTexture9_Release(old_tex);
    }
    if (fvf) {
        IDirect3DDevice9_SetFVF(dev, fvf);
    } else {
        IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    }
    if (decl) {
        IDirect3DVertexDeclaration9_Release(decl);
    }
    IDirect3DDevice9_SetVertexShader(dev, vs);
    if (vs) {
        IDirect3DVertexShader9_Release(vs);
    }
    IDirect3DDevice9_SetPixelShader(dev, ps);
    if (ps) {
        IDirect3DPixelShader9_Release(ps);
    }
}

static void screen_size(IDirect3DDevice9 *dev, float *w, float *h) {
    IDirect3DSurface9 *bb = NULL;
    *w = 800;
    *h = 600;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC d;
        IDirect3DSurface9_GetDesc(bb, &d);
        *w = (float)d.Width;
        *h = (float)d.Height;
        IDirect3DSurface9_Release(bb);
    }
}

static void on_present(void *app) {
    handle_input();
    IDirect3DDevice9 *dev = APP_PTR(app, APP_DEVICE);
    if (!dev || !gdi_init()) {
        return;
    }
    if (dev != tex_device) {
        tex = NULL;  // new device: the old textures went with it
        toast_tex = NULL;
        tex_device = dev;
        dirty = toast_dirty = true;
    }
    if (IDirect3DDevice9_TestCooperativeLevel(dev) != D3D_OK) {
        return;
    }
    float sw, sh;
    screen_size(dev, &sw, &sh);
    screen_w = sw;
    screen_h = sh;
    if (open_) {
        static DWORD last_paint;
        if (dirty || GetTickCount() - last_paint > 500) {  // device names can change
            paint_panel();
            upload(dev, &tex, CANVAS_H);
            dirty = false;
            last_paint = GetTickCount();
            toast_dirty = true;  // the pixel buffer was reused
        }
        float s = sw / CANVAS_W < sh / CANVAS_H ? sw / CANVAS_W : sh / CANVAS_H;
        s *= 0.94f;
        draw_quad(dev, tex, (sw - CANVAS_W * s) / 2, (sh - CANVAS_H * s) / 2, CANVAS_W * s, CANVAS_H * s, CANVAS_W, CANVAS_H);
    } else if (GetTickCount() < toast_until) {
        if (toast_dirty) {
            paint_toast();
            upload(dev, &toast_tex, 32);
            toast_dirty = false;
            dirty = true;
        }
        float s = sh / CANVAS_H;
        draw_quad(dev, toast_tex, 10 * s, 10 * s, TEX_W * s, 32 * s, TEX_W, 32);
    }
}

void padui_install(void) {
    frame_add_present_hook(on_present);
    padui_toast("F8: settings (controls, display, audio)", 6000);
}

#ifdef HINOROLL_HARNESS
#include "harness.h"

static int padui_cmd(const char *cmd, const char *args) {
    if (strcmp(cmd, "padui") != 0) {
        return 0;
    }
    if (strncmp(args, "open", 4) == 0) {
        set_open(true);
    } else if (strncmp(args, "close", 5) == 0) {
        set_open(false);
    } else if (strncmp(args, "row", 3) == 0) {
        row = atoi(args + 3) % page_rows[page];
        dirty = true;
    } else if (strncmp(args, "page", 4) == 0) {
        page = atoi(args + 4) % PAGES;
        row = 1;
        dirty = true;
    } else if (strncmp(args, "key ", 4) == 0) {
        static const struct {
            const char *name;
            int vk;
        } keys[] = {{"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
                    {"enter", VK_RETURN}, {"esc", VK_ESCAPE}, {"del", VK_DELETE}, {"tab", VK_TAB}};
        for (int i = 0; i < 8; i++) {
            if (strcmp(args + 4, keys[i].name) == 0) {
                harness_key = keys[i].vk;
            }
        }
    }
    harness_reply("padui %s page %d row %d col %d mode %d: %s", open_ ? "open" : "closed", page, row, col, mode,
                  message);
    return 1;
}

void padui_harness_install(void) {
    harness_add_handler(padui_cmd);
}
#endif
