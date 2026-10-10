#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Rollback-safe resource lifetimes. Battle logic creates and frees Direct3D
// vertex/index buffers and textures, and takes and drops texture-cache
// references, as effects come and go. While journaling is on, during battle
// logic:
//   - releases (D3D Release, texture-cache release) are deferred: they happen
//     when their frame is confirmed, or never if that frame is rolled back;
//   - creations and acquisitions are recorded: rolling their frame back
//     releases them (the restored state doesn't know them).
// res_install needs the D3D device (call it on the first tick).
void res_install(void *device);
void res_set_journal(bool on);
bool res_journal_on(void);
// The frame being simulated (stamped on journal entries).
void res_set_frame(int frame);
// Undo everything recorded for frames >= frame (call after restoring the state of `frame`).
void res_rollback(int frame);
// Frames <= frame are final: carry out their deferred releases.
void res_confirm(int frame);
// Carry out everything (journaling off, battle over).
void res_flush(void);

typedef struct {
    unsigned journal_len, creates, releases_deferred, releases_done, undone;
    unsigned known_objects;
} ResStats;
ResStats res_stats(void);
// The D3D device is lost (the game neither renders nor ticks until it's back).
bool res_device_lost(void);
// Battle logic's copy of the device transforms (part of the saved state).
void *res_logic_state(size_t *len);
void res_reset_logic_state(void);
// A Direct3D object the game created through the device (an external handle).
bool res_is_object(const void *p);
