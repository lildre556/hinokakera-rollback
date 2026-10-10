// Test builds stay out of the way of whoever is using the PC: no sound, and
// the game window gives the focus back and goes behind other windows once it
// is up. Sound: the process's Windows audio session is muted (that covers
// every DirectSound buffer, the music stream's IDirectSoundBuffer8 included),
// and the Play of the plain buffers does nothing as well. Harness "sound on"
// brings it back.
#define COBJMACROS
#include <string.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include "frame.h"
#include "game.h"
#include "harness.h"
#include "hooks.h"
#include "log.h"

static volatile bool muted = true;
static HWND user_window;

typedef HRESULT(WINAPI *dscreate_fn)(const GUID *, void **, void *);
typedef HRESULT(WINAPI *create_buffer_fn)(void *, const void *, void **, void *);
typedef HRESULT(WINAPI *play_fn)(void *, DWORD, DWORD, DWORD);
static dscreate_fn real_create;
static create_buffer_fn orig_create_buffer;
static play_fn orig_play;

static const CLSID clsid_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID iid_enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID iid_session_manager = {0xbfa971f1, 0x4d5e, 0x40bb, {0x93, 0x5e, 0x96, 0x70, 0x39, 0xbf, 0xbe, 0xe4}};

// Mute (or unmute) this process's audio session on the default output devices.
static DWORD WINAPI session_mute_thread(LPVOID unused) {
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        return 0;
    }
    IMMDeviceEnumerator *en = NULL;
    if (SUCCEEDED(CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL, &iid_enumerator, (void **)&en))) {
        ERole roles[2] = {eConsole, eMultimedia};
        for (int i = 0; i < 2; i++) {
            IMMDevice *dev = NULL;
            if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, roles[i], &dev))) {
                continue;
            }
            IAudioSessionManager *m = NULL;
            if (SUCCEEDED(IMMDevice_Activate(dev, &iid_session_manager, CLSCTX_ALL, NULL, (void **)&m))) {
                ISimpleAudioVolume *v = NULL;
                if (SUCCEEDED(IAudioSessionManager_GetSimpleAudioVolume(m, NULL, FALSE, &v))) {
                    ISimpleAudioVolume_SetMute(v, muted, NULL);
                    BOOL now = FALSE;
                    ISimpleAudioVolume_GetMute(v, &now);
                    log_printf("sound: audio session (%s device) %s", i ? "multimedia" : "console", now ? "muted" : "on");
                    ISimpleAudioVolume_Release(v);
                }
                IAudioSessionManager_Release(m);
            }
            IMMDevice_Release(dev);
        }
        IMMDeviceEnumerator_Release(en);
    }
    CoUninitialize();
    return 0;
}

static void apply_session_mute(void) {
    HANDLE t = CreateThread(NULL, 0, session_mute_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
}

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
    if (SUCCEEDED(hr)) {
        apply_session_mute();  // the session exists for sure now
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

static int handle(const char *cmd, const char *args) {
    if (strcmp(cmd, "sound") != 0) {
        return 0;
    }
    muted = strncmp(args, "on", 2) != 0;
    apply_session_mute();
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
    apply_session_mute();
    frame_add_present_hook(on_present);
    harness_add_handler(handle);
}
