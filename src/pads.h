#pragma once
#include <stdbool.h>
#include <stdint.h>

// Controllers, read by the mod instead of the game: XInput pads (Xbox and
// most modern pads) and DirectInput pads (D-pad/POV hats, all buttons, axes,
// triggers). Each player has a controller and bindings for the game's actions
// (saved in hinoroll.ini); src/padui.c is the in-game setup screen (F8).
// The game's own joystick reading is switched off so nothing fires twice;
// the keyboard stays the game's (config.exe).

enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_A, ACT_B, ACT_C, ACT_D, ACT_PAUSE,
    ACT_AD, ACT_CD,  // shortcuts: A+D, C+D
    PAD_ACTIONS
};
#define PAD_MAX_BINDS 4
#define PAD_MAX_DEVICES 12

// A control on a device: kind << 8 | index. 0 = none.
enum { CTL_NONE, CTL_BUTTON, CTL_POV_UP, CTL_POV_DOWN, CTL_POV_LEFT, CTL_POV_RIGHT, CTL_AXIS_NEG, CTL_AXIS_POS };
typedef uint16_t PadControl;
#define PAD_CONTROL(kind, index) ((PadControl)(((kind) << 8) | (index)))
#define PAD_KIND(c) ((c) >> 8)
#define PAD_INDEX(c) ((c) & 0xff)

void pads_install(void);
// Reads the devices (at most once every couple of milliseconds).
void pads_update(void);
// The game button word a player's controller holds right now (0 when the
// game window isn't focused or the setup screen is open).
uint16_t pads_word(int player);
void pads_rescan(void);
// A device was plugged in or out (WM_DEVICECHANGE): look again shortly.
void pads_device_change(void);

// ---- for the setup screen ----
const char *pads_action_name(int action);
int pads_device_count(void);
const char *pads_device_name(int device);
int pads_player_device(int player);           // -1: none
void pads_assign(int player, int device);     // also saves
// The first control newly active on a device since pads_capture_begin (0 = none yet).
void pads_capture_begin(int device);
PadControl pads_capture_poll(void);
bool pads_device_idle(int device);            // nothing held
bool pads_control_active(int device, PadControl c);
const PadControl *pads_bindings(int player, int action);  // PAD_MAX_BINDS entries
void pads_bind(int player, int action, PadControl c);     // adds (replaces the oldest when full)
void pads_clear(int player, int action);
void pads_reset(int player);
void pads_control_name(int device, PadControl c, char *out, int n);
// UI navigation from any controller: bit 0 up, 1 down, 2 left, 3 right, 4 select, 5 back (pressed this frame).
unsigned pads_ui_buttons(void);
void pads_set_ui_open(bool open);
bool pads_ui_open(void);
