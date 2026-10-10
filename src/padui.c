// The controller setup screen (F8), drawn over the game after Render: a panel
// painted with GDI into a texture. Keyboard (arrows, Enter, Delete, Esc) or
// any controller (D-pad/stick, A/Button 1 to choose, B/Button 2 to go back).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define COBJMACROS
#include <d3d8.h>

#include "frame.h"
#include "game.h"
#include "log.h"
#include "pads.h"

#define CANVAS_W 640
#define CANVAS_H 480
#define TEX_W 1024
#define TEX_H 512

enum { ROW_DEVICE = 0, ROW_FIRST_ACTION = 1, ROW_RESET = ROW_FIRST_ACTION + PAD_ACTIONS, ROWS };
enum { MODE_NAV, MODE_BIND, MODE_ASSIGN };

static bool open_;
static int row, col, mode;
static DWORD mode_since, closed_at, toast_until;
static char message[160];
static bool dirty = true;

static HDC dc;
static HBITMAP dib;
static uint32_t *pixels;
static HFONT font, font_big, font_small;
static IDirect3DTexture8 *tex;
static void *tex_device;
static char toast[128];
static IDirect3DTexture8 *toast_tex;
static bool toast_dirty;

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
    dirty = true;
    if (!on) {
        closed_at = GetTickCount();
    }
    log_printf("controller setup %s", on ? "opened" : "closed");
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

// Test builds: select/bind from the harness (keys need the focus).
#ifdef HINOROLL_HARNESS
static int harness_key;
#endif

static void start_bind(void) {
    int d = pads_player_device(col);
    if (d < 0) {
        snprintf(message, sizeof message, "Player %d has no controller: choose one first (top row).", col + 1);
        return;
    }
    mode = MODE_BIND;
    mode_since = GetTickCount();
    pads_capture_begin(d);
    snprintf(message, sizeof message, "Press the control for Player %d %s on %s   (Esc: cancel)", col + 1,
             pads_action_name(row - ROW_FIRST_ACTION), pads_device_name(d));
}

static void choose(void) {
    if (row == ROW_DEVICE) {
        mode = MODE_ASSIGN;
        mode_since = GetTickCount();
        snprintf(message, sizeof message,
                 "Press a button on the controller for Player %d   (Delete: keyboard only, Esc: cancel)", col + 1);
    } else if (row == ROW_RESET) {
        pads_reset(col);
        snprintf(message, sizeof message, "Player %d's controls are back to the defaults.", col + 1);
    } else {
        start_bind();
    }
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
    unsigned pad = pads_ui_buttons();
    bool k_up = focused && key_pressed(VK_UP), k_down = focused && key_pressed(VK_DOWN);
    bool k_left = focused && key_pressed(VK_LEFT), k_right = focused && key_pressed(VK_RIGHT);
    bool k_enter = focused && key_pressed(VK_RETURN), k_esc = focused && key_pressed(VK_ESCAPE);
    bool k_del = focused && (key_pressed(VK_DELETE) | key_pressed(VK_BACK));
#ifdef HINOROLL_HARNESS
    k_up |= harness_key == VK_UP;
    k_down |= harness_key == VK_DOWN;
    k_left |= harness_key == VK_LEFT;
    k_right |= harness_key == VK_RIGHT;
    k_enter |= harness_key == VK_RETURN;
    k_esc |= harness_key == VK_ESCAPE;
    k_del |= harness_key == VK_DELETE;
    harness_key = 0;
#endif

    if (mode == MODE_BIND) {
        PadControl c = pads_capture_poll();
        if (c) {
            int action = row - ROW_FIRST_ACTION;
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
    if (k_up || (pad & 1)) {
        row = (row + ROWS - 1) % ROWS;
        message[0] = 0;
        dirty = true;
    }
    if (k_down || (pad & 2)) {
        row = (row + 1) % ROWS;
        message[0] = 0;
        dirty = true;
    }
    if (k_left || k_right || (pad & 12)) {
        col ^= 1;
        message[0] = 0;
        dirty = true;
    }
    if (k_enter || (pad & 16)) {
        choose();
        pressed_device();  // don't count this press as a pick
        dirty = true;
    }
    if (k_del && row >= ROW_FIRST_ACTION && row < ROW_RESET) {
        pads_clear(col, row - ROW_FIRST_ACTION);
        snprintf(message, sizeof message, "Cleared Player %d %s.", col + 1, pads_action_name(row - ROW_FIRST_ACTION));
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

static void paint_panel(void) {
    memset(pixels, 0, TEX_W * TEX_H * 4);
    fill(0, 0, CANVAS_W, CANVAS_H, RGB(18, 16, 28));
    fill(0, 0, CANVAS_W, 3, RGB(170, 60, 90));
    text(20, 10, 400, font_big, RGB(255, 255, 255), "Controller setup");
    text(400, 16, 230, font_small, RGB(150, 150, 170), "F8 or Esc to close");
    const int label_x = 20, cx[2] = {150, 395}, cw = 235, top = 50, rh = 24;
    text(cx[0], top, cw, font, RGB(255, 200, 120), "Player 1");
    text(cx[1], top, cw, font, RGB(120, 190, 255), "Player 2");
    for (int r = 0; r < ROWS; r++) {
        int y = top + 28 + r * rh + (r >= ROW_FIRST_ACTION ? 6 : 0) + (r == ROW_RESET ? 6 : 0);
        const char *label = r == ROW_DEVICE ? "Controller" : r == ROW_RESET ? "" : pads_action_name(r - ROW_FIRST_ACTION);
        text(label_x, y + 3, 125, font, RGB(200, 200, 215), label);
        for (int c = 0; c < 2; c++) {
            bool sel = r == row && c == col;
            if (sel) {
                fill(cx[c] - 6, y, cw, rh - 2, mode == MODE_NAV ? RGB(70, 55, 120) : RGB(130, 90, 30));
            }
            char buf[160] = "";
            int d = pads_player_device(c);
            if (r == ROW_DEVICE) {
                snprintf(buf, sizeof buf, "%s", d >= 0 ? pads_device_name(d) : "Keyboard only");
            } else if (r == ROW_RESET) {
                snprintf(buf, sizeof buf, "Reset to defaults");
            } else if (d < 0) {
                snprintf(buf, sizeof buf, "-");
            } else {
                const PadControl *b = pads_bindings(c, r - ROW_FIRST_ACTION);
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
            text(cx[c], y + 3, cw - 12, font, sel ? RGB(255, 255, 255) : RGB(225, 225, 235), buf);
        }
    }
    fill(0, CANVAS_H - 66, CANVAS_W, 1, RGB(70, 60, 100));
    if (message[0]) {
        text(20, CANVAS_H - 58, 600, font_small, mode == MODE_NAV ? RGB(150, 230, 150) : RGB(255, 220, 110), message);
    } else {
        text(20, CANVAS_H - 58, 600, font_small, RGB(150, 150, 170),
             "Each action can have up to 4 controls. Keyboard keys are set in config.exe.");
    }
    text(20, CANVAS_H - 34, 600, font_small, RGB(150, 150, 170),
         "Arrows / D-pad: move     Enter / A: change     Delete: clear     Esc / B: close");
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
    fill(0, 0, 3, 30, RGB(170, 60, 90));
    text(12, 5, w, font, RGB(235, 235, 245), toast);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < w && x < TEX_W; x++) {
            pixels[y * TEX_W + x] = (pixels[y * TEX_W + x] & 0x00ffffff) | 0xd8000000u;
        }
    }
}

// ---- Direct3D ----
typedef struct {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} Vtx;
#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static bool upload(IDirect3DDevice8 *dev, IDirect3DTexture8 **t, int rows) {
    if (!*t && FAILED(IDirect3DDevice8_CreateTexture(dev, TEX_W, TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t))) {
        return false;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture8_LockRect(*t, 0, &lr, NULL, 0))) {
        return false;
    }
    for (int y = 0; y < rows; y++) {
        memcpy((uint8_t *)lr.pBits + y * lr.Pitch, pixels + y * TEX_W, TEX_W * 4);
    }
    IDirect3DTexture8_UnlockRect(*t, 0);
    return true;
}

// Only the states this changes are saved and put back (a full state block
// every frame cost the game several frames per second).
static const D3DRENDERSTATETYPE saved_rs[] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
};
static const D3DTEXTURESTAGESTATETYPE saved_tss[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_MINFILTER, D3DTSS_MAGFILTER, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
};
#define NRS (sizeof saved_rs / sizeof saved_rs[0])
#define NTSS (sizeof saved_tss / sizeof saved_tss[0])

static void draw_quad(IDirect3DDevice8 *dev, IDirect3DTexture8 *t, float x, float y, float w, float h, float tw, float th) {
    DWORD rs[NRS], tss0[NTSS], tss1_color, tss1_alpha, vs, ps;
    IDirect3DBaseTexture8 *old_tex = NULL;
    for (unsigned i = 0; i < NRS; i++) {
        IDirect3DDevice8_GetRenderState(dev, saved_rs[i], &rs[i]);
    }
    for (unsigned i = 0; i < NTSS; i++) {
        IDirect3DDevice8_GetTextureStageState(dev, 0, saved_tss[i], &tss0[i]);
    }
    IDirect3DDevice8_GetTextureStageState(dev, 1, D3DTSS_COLOROP, &tss1_color);
    IDirect3DDevice8_GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &tss1_alpha);
    IDirect3DDevice8_GetTexture(dev, 0, &old_tex);
    IDirect3DDevice8_GetVertexShader(dev, &vs);
    IDirect3DDevice8_GetPixelShader(dev, &ps);

    IDirect3DDevice8_BeginScene(dev);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
    IDirect3DDevice8_SetVertexShader(dev, VTX_FVF);
    IDirect3DDevice8_SetPixelShader(dev, 0);
    float u = tw / TEX_W, v = th / TEX_H;
    Vtx q[4] = {
        {x - 0.5f, y - 0.5f, 0, 1, 0xffffffff, 0, 0},
        {x + w - 0.5f, y - 0.5f, 0, 1, 0xffffffff, u, 0},
        {x - 0.5f, y + h - 0.5f, 0, 1, 0xffffffff, 0, v},
        {x + w - 0.5f, y + h - 0.5f, 0, 1, 0xffffffff, u, v},
    };
    IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
    IDirect3DDevice8_EndScene(dev);

    for (unsigned i = 0; i < NRS; i++) {
        IDirect3DDevice8_SetRenderState(dev, saved_rs[i], rs[i]);
    }
    for (unsigned i = 0; i < NTSS; i++) {
        IDirect3DDevice8_SetTextureStageState(dev, 0, saved_tss[i], tss0[i]);
    }
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_COLOROP, tss1_color);
    IDirect3DDevice8_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, tss1_alpha);
    IDirect3DDevice8_SetTexture(dev, 0, old_tex);
    if (old_tex) {
        IDirect3DBaseTexture8_Release(old_tex);
    }
    IDirect3DDevice8_SetVertexShader(dev, vs);
    IDirect3DDevice8_SetPixelShader(dev, ps);
}

static void screen_size(IDirect3DDevice8 *dev, float *w, float *h) {
    IDirect3DSurface8 *bb = NULL;
    *w = 800;
    *h = 600;
    if (SUCCEEDED(IDirect3DDevice8_GetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC d;
        IDirect3DSurface8_GetDesc(bb, &d);
        *w = (float)d.Width;
        *h = (float)d.Height;
        IDirect3DSurface8_Release(bb);
    }
}

static void on_present(void *app) {
    handle_input();
    IDirect3DDevice8 *dev = APP_PTR(app, APP_DEVICE);
    if (!dev || !gdi_init()) {
        return;
    }
    if (dev != tex_device) {
        tex = NULL;  // new device: the old textures went with it
        toast_tex = NULL;
        tex_device = dev;
        dirty = toast_dirty = true;
    }
    if (IDirect3DDevice8_TestCooperativeLevel(dev) != D3D_OK) {
        return;
    }
    float sw, sh;
    screen_size(dev, &sw, &sh);
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
    padui_toast("F8: controller setup", 6000);
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
        row = atoi(args + 3) % ROWS;
        dirty = true;
    } else if (strncmp(args, "key ", 4) == 0) {
        static const struct {
            const char *name;
            int vk;
        } keys[] = {{"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
                    {"enter", VK_RETURN}, {"esc", VK_ESCAPE}, {"del", VK_DELETE}};
        for (int i = 0; i < 7; i++) {
            if (strcmp(args + 4, keys[i].name) == 0) {
                harness_key = keys[i].vk;
            }
        }
    }
    harness_reply("padui %s row %d col %d mode %d: %s", open_ ? "open" : "closed", row, col, mode, message);
    return 1;
}

void padui_harness_install(void) {
    harness_add_handler(padui_cmd);
}
#endif
