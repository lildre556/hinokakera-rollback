#pragma once
#include <stdbool.h>
#include <stdint.h>

// Hooks around the game's logic tick (FrameMove) and input.
void frame_install(void);

// The application object (known after the first tick).
void *frame_app(void);
int frame_mode(void);

// One logic tick with these button words for P1 and P2 (the devices are not
// read). `resim` marks a tick re-run by rollback: no sounds.
int frame_run(uint16_t p1, uint16_t p2, bool resim);
// One logic tick exactly as the game does it (devices, harness overrides).
int frame_run_vanilla(void);
bool frame_resimulating(void);
bool frame_in_render(void);
bool frame_in_logic(void);

// What the local devices hold right now for a player's controls (0 = P1
// config, 1 = P2 config), without changing the game's input state.
uint16_t frame_poll_local(int player);

// A driver replaces the plain tick (netplay, synctest). It is called once per
// game tick from FrameMove and decides what to run.
typedef int (*frame_driver_fn)(void *app);
void frame_set_driver(frame_driver_fn fn);
bool frame_has_driver(void);

// Keep running while the window is in the background (netplay, harness).
void frame_keep_active(bool on);

// Called once per presented frame, right before Present, on the game thread
// (overlays draw here).
typedef void (*frame_present_fn)(void *app);
void frame_add_present_hook(frame_present_fn fn);
void frame_before_present(void);

// The battle scene wants to end on a frame that isn't confirmed yet: the
// driver decides (netplay.c). Return true to let it end now.
typedef bool (*frame_battle_end_fn)(void);
void frame_set_battle_end_gate(frame_battle_end_fn fn);

// Called around the title menu's update while a driver runs (before = true,
// then false): netplay keeps the attract demo and offline-only items away.
typedef void (*frame_title_fn)(void *title, bool before);
void frame_set_title_filter(frame_title_fn fn);

// Ticks run since start (all kinds).
uint32_t frame_ticks(void);

// The C runtime's rand() seed on the game thread.
uint32_t *frame_holdrand(void);

// Analysis: called right before/after every logic tick and every Render (up to 4 observers).
typedef void (*frame_observer_fn)(bool after, bool render);
void frame_set_observer(frame_observer_fn fn);

// Harness input override: while set, the player's devices are replaced by `word`.
void frame_override_input(int player, bool on, uint16_t word);
