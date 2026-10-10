// Test builds stay out of the way of whoever is using the PC: no sound (all
// DirectSound buffers' Play does nothing, harness "sound on" brings it back),
// and the game window gives the focus back and goes behind other windows once
// it is up.
#include <string.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "harness.h"
#include "hooks.h"
#include "log.h"

#define IAT_DIRECTSOUNDCREATE8 0x4bb020u  // DSOUND.dll ordinal 11

static bool muted = true;
static HWND user_window;

typedef HRESULT(WINAPI *dscreate_fn)(const GUID *, void **, void *);
typedef HRESULT(WINAPI *create_buffer_fn)(void *, const void *, void **, void *);
typedef HRESULT(WINAPI *play_fn)(void *, DWORD, DWORD, DWORD);
static dscreate_fn real_create;
static create_buffer_fn orig_create_buffer;
static play_fn orig_play;

static HRESULT WINAPI hk_play(void *buf, DWORD r1, DWORD prio, DWORD flags) {
    if (muted) {
        return 0;
    }
    return orig_play(buf, r1, prio, flags);
}

static HRESULT WINAPI hk_create_buffer(void *ds, const void *desc, void **out, void *outer) {
    HRESULT hr = orig_create_buffer(ds, desc, out, outer);
    if (SUCCEEDED(hr) && out && *out && !orig_play) {
        hook_code((uintptr_t)(*(void ***)*out)[12], (void *)hk_play, (void **)&orig_play, "DirectSoundBuffer::Play");
    }
    return hr;
}

static HRESULT WINAPI quiet_create(const GUID *guid, void **out, void *outer) {
    HRESULT hr = real_create(guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !orig_create_buffer) {
        hook_code((uintptr_t)(*(void ***)*out)[3], (void *)hk_create_buffer, (void **)&orig_create_buffer,
                  "DirectSound::CreateSoundBuffer");
    }
    return hr;
}

static void on_present(void *app) {
    static bool placed;
    if (placed || !app) {
        return;
    }
    placed = true;
    HWND game = (HWND)APP_PTR(app, APP_HWND);
    if (game) {
        SetWindowPos(game, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (user_window && user_window != game && IsWindow(user_window)) {
        SetForegroundWindow(user_window);
    }
}

// The game quits when another copy's window exists (FindWindowA on its class);
// test builds run two copies side by side for local netplay tests.
static HWND WINAPI no_other_instance(LPCSTR cls, LPCSTR title) {
    return NULL;
}

static int handle(const char *cmd, const char *args) {
    if (strcmp(cmd, "sound") != 0) {
        return 0;
    }
    muted = strncmp(args, "on", 2) != 0;
    harness_reply("sound %s", muted ? "off" : "on");
    return 1;
}

void quiet_install(void) {
    user_window = GetForegroundWindow();
    HMODULE ds = LoadLibraryA("dsound.dll");
    real_create = ds ? (dscreate_fn)GetProcAddress(ds, (LPCSTR)11) : NULL;
    if (real_create) {
        void *fn = (void *)quiet_create;
        patch_bytes(IAT_DIRECTSOUNDCREATE8, &fn, sizeof fn);
    }
    void *fw = (void *)no_other_instance;
    patch_bytes(0x4bb3a0, &fw, sizeof fw);  // USER32!FindWindowA import (single-instance check only)
    frame_add_present_hook(on_present);
    harness_add_handler(handle);
}
