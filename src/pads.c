#include "pads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <dinput.h>
#include <xinput.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "log.h"

#define AXIS_THRESHOLD 500     // of 1000, like the game's own stick reading
#define CAPTURE_THRESHOLD 650
#define ADDR_GAME_ENUM_JOYSTICK 0x4355e0u  // the game's EnumDevices callback

enum { DEV_XINPUT, DEV_DINPUT };

typedef struct {
    uint8_t buttons[32];
    uint8_t pov[4][4];  // up, down, left, right
    int16_t axes[8];    // -1000..1000; XInput: LX, LY, RX, RY (up is negative), LT, RT
} PadState;

typedef struct {
    int type;
    int xslot;
    IDirectInputDevice8A *dev;
    GUID instance;
    char name[64];
    char key[48];      // which device (assignment)
    char profile[32];  // which kind of device (bindings)
    PadState st, prev;
    bool seen;
} Pad;

static Pad pads[PAD_MAX_DEVICES];
#ifdef HINOROLL_HARNESS
static int fake_device = -1;
static PadControl fake_control;
static DWORD fake_until;
static bool fake_focus;
#endif
static int pad_count;
static IDirectInput8A *dinput;
static bool enabled = true;
static bool ui_open;
static DWORD last_update, last_xinput_scan, rescan_at;
static char ini_path[MAX_PATH];

typedef DWORD(WINAPI *xinput_get_fn)(DWORD, XINPUT_STATE *);
static xinput_get_fn xinput_get;
static bool xinput_connected[4];

// Player assignment (device key, "" = automatic, "none" = keyboard only) and bindings.
static char assigned[2][48];
typedef struct {
    char profile[32];
    PadControl binds[PAD_ACTIONS][PAD_MAX_BINDS];
} Profile;
static Profile player_profile[2];  // bindings for the device type each player has now

static const char *action_names[PAD_ACTIONS] = {"Up", "Down", "Left", "Right", "A", "B", "C", "D", "Pause", "A+D", "C+D"};
static const char *action_keys[PAD_ACTIONS] = {"up", "down", "left", "right", "a", "b", "c", "d", "pause", "ad", "cd"};
static const uint16_t action_bits[PAD_ACTIONS] = {0x08, 0x02, 0x04, 0x01, 0x10, 0x20, 0x40, 0x80, 0x4000, 0x90, 0xc0};

const char *pads_action_name(int action) {
    return action >= 0 && action < PAD_ACTIONS ? action_names[action] : "";
}

void *dinput8_real_create(void);  // dllmain.c

// ---- reading devices ----
static void read_xinput(Pad *p) {
    XINPUT_STATE xs;
    memset(&p->st, 0, sizeof p->st);
    if (!xinput_get || xinput_get((DWORD)p->xslot, &xs) != ERROR_SUCCESS) {
        return;
    }
    for (int b = 0; b < 16; b++) {
        p->st.buttons[b] = (xs.Gamepad.wButtons >> b) & 1;
    }
    p->st.axes[0] = (int16_t)(xs.Gamepad.sThumbLX * 1000 / 32767);
    p->st.axes[1] = (int16_t)(-xs.Gamepad.sThumbLY * 1000 / 32767);
    p->st.axes[2] = (int16_t)(xs.Gamepad.sThumbRX * 1000 / 32767);
    p->st.axes[3] = (int16_t)(-xs.Gamepad.sThumbRY * 1000 / 32767);
    p->st.axes[4] = (int16_t)(xs.Gamepad.bLeftTrigger * 1000 / 255);
    p->st.axes[5] = (int16_t)(xs.Gamepad.bRightTrigger * 1000 / 255);
}

static int16_t clamp1000(LONG v) {
    return (int16_t)(v < -1000 ? -1000 : v > 1000 ? 1000 : v);
}

static void read_dinput(Pad *p) {
    DIJOYSTATE js;
    memset(&p->st, 0, sizeof p->st);
    IDirectInputDevice8_Poll(p->dev);
    HRESULT hr = IDirectInputDevice8_GetDeviceState(p->dev, sizeof js, &js);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        IDirectInputDevice8_Acquire(p->dev);
        IDirectInputDevice8_Poll(p->dev);
        hr = IDirectInputDevice8_GetDeviceState(p->dev, sizeof js, &js);
    }
    if (FAILED(hr)) {
        return;
    }
    for (int b = 0; b < 32; b++) {
        p->st.buttons[b] = (js.rgbButtons[b] & 0x80) != 0;
    }
    for (int i = 0; i < 4; i++) {
        DWORD a = js.rgdwPOV[i];
        if (LOWORD(a) == 0xffff) {
            continue;
        }
        // Each direction covers +-67.5 degrees, so diagonals give two.
        p->st.pov[i][0] = a > 29250 || a < 6750;
        p->st.pov[i][3] = a > 2250 && a < 15750;
        p->st.pov[i][1] = a > 11250 && a < 24750;
        p->st.pov[i][2] = a > 20250 && a < 33750;
    }
    LONG axes[8] = {js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1]};
    for (int i = 0; i < 8; i++) {
        p->st.axes[i] = clamp1000(axes[i]);
    }
}

bool pads_control_active(int device, PadControl c) {
    if (device < 0 || device >= pad_count || !c) {
        return false;
    }
    const PadState *s = &pads[device].st;
    int i = PAD_INDEX(c);
    switch (PAD_KIND(c)) {
    case CTL_BUTTON: return i < 32 && s->buttons[i];
    case CTL_POV_UP: return i < 4 && s->pov[i][0];
    case CTL_POV_DOWN: return i < 4 && s->pov[i][1];
    case CTL_POV_LEFT: return i < 4 && s->pov[i][2];
    case CTL_POV_RIGHT: return i < 4 && s->pov[i][3];
    case CTL_AXIS_NEG: return i < 8 && s->axes[i] < -AXIS_THRESHOLD;
    case CTL_AXIS_POS: return i < 8 && s->axes[i] > AXIS_THRESHOLD;
    }
    return false;
}

// ---- devices ----
static void make_profile(Pad *p, const DIDEVICEINSTANCEA *inst) {
    if (p->type == DEV_XINPUT) {
        snprintf(p->profile, sizeof p->profile, "xinput");
        snprintf(p->key, sizeof p->key, "xinput%d", p->xslot);
        snprintf(p->name, sizeof p->name, "XInput controller %d", p->xslot + 1);
        return;
    }
    // guidProduct.Data1 is the USB product and vendor ID.
    snprintf(p->profile, sizeof p->profile, "dinput-%08lx", (unsigned long)inst->guidProduct.Data1);
    const GUID *g = &inst->guidInstance;
    snprintf(p->key, sizeof p->key, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned long)g->Data1, g->Data2,
             g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
             g->Data4[7]);
    snprintf(p->name, sizeof p->name, "%s", inst->tszProductName);
}

// XInput pads also show up in DirectInput; their device path has "IG_".
static bool is_xinput_device(IDirectInputDevice8A *dev) {
    DIPROPGUIDANDPATH gp;
    memset(&gp, 0, sizeof gp);
    gp.diph.dwSize = sizeof gp;
    gp.diph.dwHeaderSize = sizeof gp.diph;
    gp.diph.dwHow = DIPH_DEVICE;
    if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph))) {
        return false;
    }
    for (WCHAR *w = gp.wszPath; *w; w++) {
        if ((w[0] == 'I' || w[0] == 'i') && (w[1] == 'G' || w[1] == 'g') && w[2] == '_') {
            return true;
        }
    }
    return false;
}

static void remove_pad(int i) {
    if (pads[i].dev) {
        IDirectInputDevice8_Unacquire(pads[i].dev);
        IDirectInputDevice8_Release(pads[i].dev);
    }
    memmove(&pads[i], &pads[i + 1], (pad_count - i - 1) * sizeof(Pad));
    pad_count--;
}

static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    for (int i = 0; i < pad_count; i++) {
        if (pads[i].type == DEV_DINPUT && IsEqualGUID(&pads[i].instance, &inst->guidInstance)) {
            pads[i].seen = true;
            return DIENUM_CONTINUE;
        }
    }
    if (pad_count == PAD_MAX_DEVICES) {
        return DIENUM_STOP;
    }
    IDirectInputDevice8A *dev = NULL;
    if (FAILED(IDirectInput8_CreateDevice(dinput, &inst->guidInstance, &dev, NULL))) {
        return DIENUM_CONTINUE;
    }
    if (xinput_get && is_xinput_device(dev)) {
        IDirectInputDevice8_Release(dev);
        return DIENUM_CONTINUE;
    }
    void *app = frame_app();
    HWND hwnd = app ? (HWND)APP_PTR(app, APP_HWND) : NULL;
    if (FAILED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick)) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(dev, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        IDirectInputDevice8_Release(dev);
        return DIENUM_CONTINUE;
    }
    DIPROPRANGE range;
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwObj = 0;
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = -1000;
    range.lMax = 1000;
    IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &range.diph);
    IDirectInputDevice8_Acquire(dev);
    Pad *p = &pads[pad_count++];
    memset(p, 0, sizeof *p);
    p->type = DEV_DINPUT;
    p->dev = dev;
    p->instance = inst->guidInstance;
    p->seen = true;
    make_profile(p, inst);
    log_printf("pads: %s (DirectInput, %s)", p->name, p->profile);
    return DIENUM_CONTINUE;
}

static void scan_xinput(void) {
    if (!xinput_get) {
        return;
    }
    for (int s = 0; s < 4; s++) {
        XINPUT_STATE xs;
        bool now = xinput_get((DWORD)s, &xs) == ERROR_SUCCESS;
        if (now == xinput_connected[s]) {
            continue;
        }
        xinput_connected[s] = now;
        if (now && pad_count < PAD_MAX_DEVICES) {
            Pad *p = &pads[pad_count++];
            memset(p, 0, sizeof *p);
            p->type = DEV_XINPUT;
            p->xslot = s;
            make_profile(p, NULL);
            log_printf("pads: %s connected", p->name);
        } else if (!now) {
            for (int i = 0; i < pad_count; i++) {
                if (pads[i].type == DEV_XINPUT && pads[i].xslot == s) {
                    log_printf("pads: %s disconnected", pads[i].name);
                    remove_pad(i);
                    break;
                }
            }
        }
    }
}

void pads_rescan(void) {
    if (!dinput) {
        return;
    }
    for (int i = 0; i < pad_count; i++) {
        pads[i].seen = pads[i].type == DEV_XINPUT;
    }
    IDirectInput8_EnumDevices(dinput, DI8DEVCLASS_GAMECTRL, enum_cb, NULL, DIEDFL_ATTACHEDONLY);
    for (int i = pad_count - 1; i >= 0; i--) {
        if (!pads[i].seen) {
            log_printf("pads: %s removed", pads[i].name);
            remove_pad(i);
        }
    }
    last_xinput_scan = 0;
}

// ---- bindings and assignment (hinoroll.ini [pads]) ----
static void control_text(PadControl c, char *out, int n) {
    static const char *kinds[] = {"", "b", "hu", "hd", "hl", "hr", "a-", "a+"};
    int k = PAD_KIND(c);
    snprintf(out, n, "%s%d", k < 8 ? kinds[k] : "?", PAD_INDEX(c));
}

static PadControl control_parse(const char *s) {
    static const struct {
        const char *prefix;
        int kind;
    } kinds[] = {{"hu", CTL_POV_UP}, {"hd", CTL_POV_DOWN}, {"hl", CTL_POV_LEFT}, {"hr", CTL_POV_RIGHT},
                 {"a-", CTL_AXIS_NEG}, {"a+", CTL_AXIS_POS}, {"b", CTL_BUTTON}};
    for (int i = 0; i < 7; i++) {
        size_t l = strlen(kinds[i].prefix);
        if (strncmp(s, kinds[i].prefix, l) == 0 && s[l] >= '0' && s[l] <= '9') {
            return PAD_CONTROL(kinds[i].kind, atoi(s + l) & 0xff);
        }
    }
    return 0;
}

static void default_bindings(int player, Profile *pr) {
    memset(pr->binds, 0, sizeof pr->binds);
    if (strcmp(pr->profile, "xinput") == 0) {
        // D-pad and left stick; A B C D on the face buttons like a Neo Geo pad
        // (X left, Y top, A bottom, B right); Start pauses; RB = A+D, RT = C+D.
        pr->binds[ACT_UP][0] = PAD_CONTROL(CTL_BUTTON, 0);
        pr->binds[ACT_UP][1] = PAD_CONTROL(CTL_AXIS_NEG, 1);
        pr->binds[ACT_DOWN][0] = PAD_CONTROL(CTL_BUTTON, 1);
        pr->binds[ACT_DOWN][1] = PAD_CONTROL(CTL_AXIS_POS, 1);
        pr->binds[ACT_LEFT][0] = PAD_CONTROL(CTL_BUTTON, 2);
        pr->binds[ACT_LEFT][1] = PAD_CONTROL(CTL_AXIS_NEG, 0);
        pr->binds[ACT_RIGHT][0] = PAD_CONTROL(CTL_BUTTON, 3);
        pr->binds[ACT_RIGHT][1] = PAD_CONTROL(CTL_AXIS_POS, 0);
        pr->binds[ACT_A][0] = PAD_CONTROL(CTL_BUTTON, 14);
        pr->binds[ACT_B][0] = PAD_CONTROL(CTL_BUTTON, 15);
        pr->binds[ACT_C][0] = PAD_CONTROL(CTL_BUTTON, 12);
        pr->binds[ACT_D][0] = PAD_CONTROL(CTL_BUTTON, 13);
        pr->binds[ACT_PAUSE][0] = PAD_CONTROL(CTL_BUTTON, 4);
        pr->binds[ACT_AD][0] = PAD_CONTROL(CTL_BUTTON, 9);
        pr->binds[ACT_CD][0] = PAD_CONTROL(CTL_AXIS_POS, 5);
        return;
    }
    // DirectInput: hat and stick for directions, buttons from config.exe's Joypad settings.
    pr->binds[ACT_UP][0] = PAD_CONTROL(CTL_POV_UP, 0);
    pr->binds[ACT_UP][1] = PAD_CONTROL(CTL_AXIS_NEG, 1);
    pr->binds[ACT_DOWN][0] = PAD_CONTROL(CTL_POV_DOWN, 0);
    pr->binds[ACT_DOWN][1] = PAD_CONTROL(CTL_AXIS_POS, 1);
    pr->binds[ACT_LEFT][0] = PAD_CONTROL(CTL_POV_LEFT, 0);
    pr->binds[ACT_LEFT][1] = PAD_CONTROL(CTL_AXIS_NEG, 0);
    pr->binds[ACT_RIGHT][0] = PAD_CONTROL(CTL_POV_RIGHT, 0);
    pr->binds[ACT_RIGHT][1] = PAD_CONTROL(CTL_AXIS_POS, 0);
    int map[5] = {0, 1, 2, 3, 6};
    void *app = frame_app();
    uint8_t *input = app ? APP_PTR(app, APP_INPUT) : NULL;
    if (input) {
        for (int i = 0; i < 5; i++) {
            map[i] = *(int32_t *)(input + 0x38c + player * 0x40 + i * 4);
        }
    }
    static const int acts[5] = {ACT_A, ACT_B, ACT_C, ACT_D, ACT_PAUSE};
    for (int i = 0; i < 5; i++) {
        if (map[i] >= 0 && map[i] < 32) {
            pr->binds[acts[i]][0] = PAD_CONTROL(CTL_BUTTON, map[i]);
        }
    }
}

static void load_profile(int player, const char *profile) {
    Profile *pr = &player_profile[player];
    if (strcmp(pr->profile, profile) == 0) {
        return;
    }
    snprintf(pr->profile, sizeof pr->profile, "%s", profile);
    default_bindings(player, pr);
    for (int a = 0; a < PAD_ACTIONS; a++) {
        char key[96], val[128];
        snprintf(key, sizeof key, "p%d.%s.%s", player + 1, profile, action_keys[a]);
        GetPrivateProfileStringA("pads", key, "\x01", val, sizeof val, ini_path);
        if (val[0] == '\x01') {
            continue;  // not saved: keep the default
        }
        memset(pr->binds[a], 0, sizeof pr->binds[a]);
        int n = 0;
        for (char *tok = strtok(val, ", "); tok && n < PAD_MAX_BINDS; tok = strtok(NULL, ", ")) {
            PadControl c = control_parse(tok);
            if (c) {
                pr->binds[a][n++] = c;
            }
        }
    }
}

static void save_profile(int player) {
    Profile *pr = &player_profile[player];
    if (!pr->profile[0]) {
        return;
    }
    for (int a = 0; a < PAD_ACTIONS; a++) {
        char key[96], val[128] = "", t[16];
        snprintf(key, sizeof key, "p%d.%s.%s", player + 1, pr->profile, action_keys[a]);
        for (int i = 0; i < PAD_MAX_BINDS; i++) {
            if (pr->binds[a][i]) {
                control_text(pr->binds[a][i], t, sizeof t);
                if (val[0]) {
                    strcat(val, ",");
                }
                strcat(val, t);
            }
        }
        WritePrivateProfileStringA("pads", key, val, ini_path);
    }
}

static int find_key(const char *key) {
    for (int i = 0; i < pad_count; i++) {
        if (strcmp(pads[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// A player's saved device: >= 0 present, -1 automatic (none saved or not
// plugged in), -2 keyboard only.
static int explicit_device(int player) {
    if (strcmp(assigned[player], "none") == 0) {
        return -2;
    }
    return assigned[player][0] ? find_key(assigned[player]) : -1;
}

// Automatic: P1 takes the first device the other player hasn't claimed, P2 the
// next one.
int pads_player_device(int player) {
    int e0 = explicit_device(0), e1 = explicit_device(1);
    int mine = player ? e1 : e0, other = player ? e0 : e1;
    if (mine >= 0) {
        return mine;
    }
    if (mine == -2) {
        return -1;
    }
    int p1_auto = -1;
    if (player == 1 && e0 == -1) {
        p1_auto = pad_count > 0 ? 0 : -1;
    }
    for (int i = 0; i < pad_count; i++) {
        if (i != other && i != p1_auto) {
            return i;
        }
    }
    return -1;
}

void pads_assign(int player, int device) {
    if (device < 0) {
        snprintf(assigned[player], sizeof assigned[player], "none");
    } else {
        snprintf(assigned[player], sizeof assigned[player], "%s", pads[device].key);
        if (strcmp(assigned[1 - player], pads[device].key) == 0) {
            assigned[1 - player][0] = 0;  // the other player gives it up
            WritePrivateProfileStringA("pads", 1 - player ? "p2" : "p1", "", ini_path);
        }
    }
    WritePrivateProfileStringA("pads", player ? "p2" : "p1", assigned[player], ini_path);
}

int pads_device_count(void) {
    return pad_count;
}

const char *pads_device_name(int device) {
    return device >= 0 && device < pad_count ? pads[device].name : "";
}

const PadControl *pads_bindings(int player, int action) {
    int d = pads_player_device(player);
    if (d >= 0) {
        load_profile(player, pads[d].profile);
    }
    return player_profile[player].binds[action];
}

void pads_bind(int player, int action, PadControl c) {
    pads_bindings(player, action);
    PadControl *b = player_profile[player].binds[action];
    for (int i = 0; i < PAD_MAX_BINDS; i++) {
        if (b[i] == c) {
            return;
        }
    }
    for (int i = 0; i < PAD_MAX_BINDS; i++) {
        if (!b[i]) {
            b[i] = c;
            save_profile(player);
            return;
        }
    }
    memmove(b, b + 1, (PAD_MAX_BINDS - 1) * sizeof *b);
    b[PAD_MAX_BINDS - 1] = c;
    save_profile(player);
}

void pads_clear(int player, int action) {
    pads_bindings(player, action);
    memset(player_profile[player].binds[action], 0, sizeof player_profile[player].binds[action]);
    save_profile(player);
}

void pads_reset(int player) {
    pads_bindings(player, 0);
    default_bindings(player, &player_profile[player]);
    save_profile(player);
}

void pads_control_name(int device, PadControl c, char *out, int n) {
    int i = PAD_INDEX(c), k = PAD_KIND(c);
    bool x = device >= 0 && device < pad_count && pads[device].type == DEV_XINPUT;
    if (!c) {
        snprintf(out, n, "-");
    } else if (x && k == CTL_BUTTON) {
        static const char *names[16] = {"D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Start", "Back",
                                        "L Stick Click", "R Stick Click", "LB", "RB", "?", "?", "A", "B", "X", "Y"};
        snprintf(out, n, "%s", i < 16 ? names[i] : "?");
    } else if (x && (k == CTL_AXIS_NEG || k == CTL_AXIS_POS)) {
        static const char *neg[6] = {"L Stick Left", "L Stick Up", "R Stick Left", "R Stick Up", "LT", "RT"};
        static const char *pos[6] = {"L Stick Right", "L Stick Down", "R Stick Right", "R Stick Down", "LT", "RT"};
        snprintf(out, n, "%s", i < 6 ? (k == CTL_AXIS_NEG ? neg[i] : pos[i]) : "?");
    } else if (k == CTL_BUTTON) {
        snprintf(out, n, "Button %d", i + 1);
    } else if (k >= CTL_POV_UP && k <= CTL_POV_RIGHT) {
        static const char *dirs[4] = {"Up", "Down", "Left", "Right"};
        if (i == 0) {
            snprintf(out, n, "Hat %s", dirs[k - CTL_POV_UP]);
        } else {
            snprintf(out, n, "Hat%d %s", i + 1, dirs[k - CTL_POV_UP]);
        }
    } else {
        static const char *axes[8] = {"X", "Y", "Z", "RX", "RY", "RZ", "Slider 1", "Slider 2"};
        snprintf(out, n, "%s%s", i < 8 ? axes[i] : "?", k == CTL_AXIS_NEG ? "-" : "+");
    }
}

// ---- per frame ----
void pads_update(void) {
    if (!enabled) {
        return;
    }
    DWORD now = GetTickCount();
    if (last_update && now - last_update < 3) {
        return;
    }
    last_update = now;
    if (!dinput) {
        // XInput is loaded here, not in DllMain (loader lock).
        static const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (int i = 0; i < 3 && !xinput_get; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) {
                xinput_get = (xinput_get_fn)GetProcAddress(m, "XInputGetState");
            }
        }
        typedef HRESULT(WINAPI * create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
        create_fn create = (create_fn)dinput8_real_create();
        if (!create || !frame_app() ||
            FAILED(create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&dinput, NULL))) {
            return;
        }
        pads_rescan();
    }
    if (rescan_at && now >= rescan_at) {
        rescan_at = 0;
        pads_rescan();
    }
    if (!last_xinput_scan || now - last_xinput_scan > 2000) {
        last_xinput_scan = now;
        scan_xinput();
    }
    for (int i = 0; i < pad_count; i++) {
        pads[i].prev = pads[i].st;
        if (pads[i].type == DEV_XINPUT) {
            read_xinput(&pads[i]);
        } else {
            read_dinput(&pads[i]);
        }
#ifdef HINOROLL_HARNESS
        if (i == fake_device && now < fake_until) {
            PadState *st = &pads[i].st;
            int idx = PAD_INDEX(fake_control);
            switch (PAD_KIND(fake_control)) {
            case CTL_BUTTON: st->buttons[idx & 31] = 1; break;
            case CTL_AXIS_NEG: st->axes[idx & 7] = -1000; break;
            case CTL_AXIS_POS: st->axes[idx & 7] = 1000; break;
            default: if (PAD_KIND(fake_control) >= CTL_POV_UP) st->pov[idx & 3][PAD_KIND(fake_control) - CTL_POV_UP] = 1;
            }
        }
#endif
    }
}

static bool game_focused(void) {
#ifdef HINOROLL_HARNESS
    if (fake_focus) {
        return true;
    }
#endif
    void *app = frame_app();
    return app && GetForegroundWindow() == (HWND)APP_PTR(app, APP_HWND);
}

uint16_t pads_word(int player) {
    if (!enabled || ui_open || !game_focused()) {
        return 0;
    }
    int d = pads_player_device(player);
    if (d < 0) {
        return 0;
    }
    load_profile(player, pads[d].profile);
    uint16_t w = 0;
    for (int a = 0; a < PAD_ACTIONS; a++) {
        for (int i = 0; i < PAD_MAX_BINDS; i++) {
            if (pads_control_active(d, player_profile[player].binds[a][i])) {
                w |= action_bits[a];
                break;
            }
        }
    }
    // Opposite directions cancel out (a stick and a D-pad pointing apart).
    if ((w & 0x0c) == 0x0c) {
        w &= ~0x0c;
    }
    if ((w & 0x03) == 0x03) {
        w &= ~0x03;
    }
    return w;
}

// ---- capture (binding a control) ----
static int capture_device = -1;
static bool capture_wait_release;

bool pads_device_idle(int device) {
    if (device < 0 || device >= pad_count) {
        return true;
    }
    const PadState *s = &pads[device].st;
    for (int i = 0; i < 32; i++) {
        if (s->buttons[i]) {
            return false;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (s->pov[i][0] || s->pov[i][1] || s->pov[i][2] || s->pov[i][3]) {
            return false;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (s->axes[i] > CAPTURE_THRESHOLD || s->axes[i] < -CAPTURE_THRESHOLD) {
            return false;
        }
    }
    return true;
}

void pads_capture_begin(int device) {
    capture_device = device;
    capture_wait_release = true;
}

PadControl pads_capture_poll(void) {
    int d = capture_device;
    if (d < 0 || d >= pad_count) {
        return 0;
    }
    if (capture_wait_release) {
        capture_wait_release = !pads_device_idle(d);
        return 0;
    }
    const PadState *s = &pads[d].st;
    bool x = pads[d].type == DEV_XINPUT;
    for (int i = 0; i < 32; i++) {
        if (s->buttons[i]) {
            return PAD_CONTROL(CTL_BUTTON, i);
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int dir = 0; dir < 4; dir++) {
            if (s->pov[i][dir]) {
                return PAD_CONTROL(CTL_POV_UP + dir, i);
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        // Triggers rest at 0 on XInput; on DirectInput some rest at an end, so
        // take an axis only when it moves away from where it was.
        int v = s->axes[i], was = pads[d].prev.axes[i];
        if (x && i >= 4) {
            if (v > CAPTURE_THRESHOLD) {
                return PAD_CONTROL(CTL_AXIS_POS, i);
            }
            continue;
        }
        if (v > CAPTURE_THRESHOLD && was <= CAPTURE_THRESHOLD) {
            return PAD_CONTROL(CTL_AXIS_POS, i);
        }
        if (v < -CAPTURE_THRESHOLD && was >= -CAPTURE_THRESHOLD) {
            return PAD_CONTROL(CTL_AXIS_NEG, i);
        }
    }
    return 0;
}

// ---- menu navigation from any controller ----
unsigned pads_ui_buttons(void) {
    unsigned pressed = 0;
    for (int d = 0; d < pad_count; d++) {
        const PadState *s = &pads[d].st, *p = &pads[d].prev;
        unsigned now = 0, before = 0;
        if (pads[d].type == DEV_XINPUT) {
            now |= (s->buttons[0] || s->axes[1] < -AXIS_THRESHOLD) << 0;
            now |= (s->buttons[1] || s->axes[1] > AXIS_THRESHOLD) << 1;
            now |= (s->buttons[2] || s->axes[0] < -AXIS_THRESHOLD) << 2;
            now |= (s->buttons[3] || s->axes[0] > AXIS_THRESHOLD) << 3;
            now |= s->buttons[12] << 4;  // A
            now |= s->buttons[13] << 5;  // B
            before |= (p->buttons[0] || p->axes[1] < -AXIS_THRESHOLD) << 0;
            before |= (p->buttons[1] || p->axes[1] > AXIS_THRESHOLD) << 1;
            before |= (p->buttons[2] || p->axes[0] < -AXIS_THRESHOLD) << 2;
            before |= (p->buttons[3] || p->axes[0] > AXIS_THRESHOLD) << 3;
            before |= p->buttons[12] << 4;
            before |= p->buttons[13] << 5;
        } else {
            now |= (s->pov[0][0] || s->axes[1] < -AXIS_THRESHOLD) << 0;
            now |= (s->pov[0][1] || s->axes[1] > AXIS_THRESHOLD) << 1;
            now |= (s->pov[0][2] || s->axes[0] < -AXIS_THRESHOLD) << 2;
            now |= (s->pov[0][3] || s->axes[0] > AXIS_THRESHOLD) << 3;
            now |= s->buttons[0] << 4;
            now |= s->buttons[1] << 5;
            before |= (p->pov[0][0] || p->axes[1] < -AXIS_THRESHOLD) << 0;
            before |= (p->pov[0][1] || p->axes[1] > AXIS_THRESHOLD) << 1;
            before |= (p->pov[0][2] || p->axes[0] < -AXIS_THRESHOLD) << 2;
            before |= (p->pov[0][3] || p->axes[0] > AXIS_THRESHOLD) << 3;
            before |= p->buttons[0] << 4;
            before |= p->buttons[1] << 5;
        }
        pressed |= now & ~before;
    }
    return pressed;
}

void pads_set_ui_open(bool open) {
    ui_open = open;
    if (open) {
        rescan_at = GetTickCount();
    }
}

bool pads_ui_open(void) {
    return ui_open;
}

// The game enumerates controllers once at startup; while the mod reads them,
// it gets none (its own reading only knows the stick and five buttons).
static BOOL CALLBACK no_game_joysticks(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    return DIENUM_CONTINUE;
}

void pads_device_change(void) {
    rescan_at = GetTickCount() + 1500;
}

void pads_install(void) {
    snprintf(ini_path, sizeof ini_path, "%s\\hinoroll.ini", game_dir());
    enabled = GetPrivateProfileIntA("pads", "enabled", 1, ini_path) != 0;
    if (!enabled) {
        log_printf("pads: off (hinoroll.ini [pads] enabled=0): the game reads controllers itself");
        return;
    }
    GetPrivateProfileStringA("pads", "p1", "", assigned[0], sizeof assigned[0], ini_path);
    GetPrivateProfileStringA("pads", "p2", "", assigned[1], sizeof assigned[1], ini_path);
    void *orig;
    hook_code(ADDR_GAME_ENUM_JOYSTICK, (void *)no_game_joysticks, &orig, "game joystick enumeration");
}

#ifdef HINOROLL_HARNESS
#include "harness.h"

// "pads": devices, assignments, what is held right now.
static int pads_cmd(const char *cmd, const char *args) {
    if (strcmp(cmd, "pads") != 0) {
        return 0;
    }
    pads_update();
    for (int d = 0; d < pad_count; d++) {
        char held[256] = "";
        for (int i = 0; i < 32; i++) {
            if (pads[d].st.buttons[i]) {
                char n[48];
                pads_control_name(d, PAD_CONTROL(CTL_BUTTON, i), n, sizeof n);
                snprintf(held + strlen(held), sizeof held - strlen(held), "[%s] ", n);
            }
        }
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 4; k++) {
                if (pads[d].st.pov[i][k]) {
                    char n[48];
                    pads_control_name(d, PAD_CONTROL(CTL_POV_UP + k, i), n, sizeof n);
                    snprintf(held + strlen(held), sizeof held - strlen(held), "[%s] ", n);
                }
            }
        }
        harness_reply("pad %d: %s (%s, key %s) axes %d %d %d %d %d %d %d %d held %s", d, pads[d].name, pads[d].profile,
                      pads[d].key, pads[d].st.axes[0], pads[d].st.axes[1], pads[d].st.axes[2], pads[d].st.axes[3],
                      pads[d].st.axes[4], pads[d].st.axes[5], pads[d].st.axes[6], pads[d].st.axes[7], held);
    }
    for (int p = 0; p < 2; p++) {
        int d = pads_player_device(p);
        harness_reply("player %d: device %d (%s) word %04x", p + 1, d, d >= 0 ? pads[d].name : "keyboard", pads_word(p));
    }
    harness_reply("pads: %d devices%s", pad_count, xinput_get ? "" : " (no XInput)");
    return 1;
}

// "padfake DEVICE CONTROL MS": hold a control (b12, a+0, hu0...) for MS ms;
// "padfake focus 1|0": treat the game as focused.
static int padfake_cmd(const char *cmd, const char *args) {
    if (strcmp(cmd, "padfake") != 0) {
        return 0;
    }
    if (strncmp(args, "focus", 5) == 0) {
        fake_focus = atoi(args + 5) != 0;
        harness_reply("padfake focus %d", fake_focus);
        return 1;
    }
    char ctl[16] = "";
    int dev = -1, ms = 100;
    sscanf(args, "%d %15s %d", &dev, ctl, &ms);
    fake_device = dev;
    fake_control = control_parse(ctl);
    fake_until = GetTickCount() + ms;
    harness_reply("padfake device %d control %04x for %d ms", dev, fake_control, ms);
    return 1;
}

void pads_harness_install(void) {
    harness_add_handler(pads_cmd);
    harness_add_handler(padfake_cmd);
}
#endif
