#pragma once
#include <stdbool.h>
#include <windows.h>

// Display modes: the game's own window and exclusive fullscreen, plus a
// borderless window covering the monitor (the game keeps rendering at its
// resolution; Present scales it up with black bars to keep the shape). F4 and
// the window's maximize button switch between the window and the chosen kind
// of fullscreen. Saved in hinoroll.ini [display].
enum { DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN, DISPLAY_MODES };

void window_install(void);
// config.dat was just read (before the window exists): the start mode.
void window_config_loaded(void *app);
// Main thread, before each frame (outside logic): carries out mode changes.
void window_before_frame(void *app);
// MsgProc filter: returns true when the message was handled (result in *out).
bool window_msgproc(void *app, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out);
// The device exists (first tick): hooks Present.
void window_device_ready(void *device);

// F4: window <-> the chosen fullscreen kind (done before the next frame).
void window_request_toggle(void);
int window_mode(void);
void window_set_mode(int mode);           // also saves
int window_fullscreen_kind(void);         // DISPLAY_BORDERLESS or DISPLAY_FULLSCREEN
void window_set_fullscreen_kind(int kind);
const char *window_mode_name(int mode);
// Borderless: keep the game's shape (black bars) or fill the monitor.
bool window_stretch(void);
void window_set_stretch(bool on);
