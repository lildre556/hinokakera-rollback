#include <windows.h>

#include "frame.h"
#include "harness.h"
#include "heap.h"
#include "hooks.h"
#include "game.h"
#include "log.h"

void crash_install(void);
void overlay_install(void);
void effects_install(void);
void padui_install(void);
void audio_install(void);
#include "pads.h"
#include "window.h"

typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8Create_t real_create;

static void load_real(void) {
    if (real_create) {
        return;
    }
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\dinput8.dll");
    HMODULE m = LoadLibraryA(path);
    if (m) {
        real_create = (DirectInput8Create_t)GetProcAddress(m, "DirectInput8Create");
    }
    if (!real_create) {
        log_printf("can't load the system dinput8.dll (%s)", path);
    }
}

// The system's DirectInput8Create (src/pads.c reads controllers with it).
void *dinput8_real_create(void) {
    load_real();
    return (void *)real_create;
}

HRESULT WINAPI proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID *out, LPUNKNOWN outer) {
    load_real();
    if (!real_create) {
        return E_FAIL;
    }
    return real_create(inst, version, iid, out, outer);
}

static bool is_game(void) {
    // Only hook the Chaotic Eclipse ver.1.01 hinokakera.exe (the addresses are for that build).
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == GAME_TIMESTAMP && nt->OptionalHeader.ImageBase == 0x400000;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        log_init();
        if (!is_game()) {
            log_printf("not hinokakera.exe of Hinokakera Chaotic Eclipse ver.1.01 (2011-08-30); the mod stays off");
            return TRUE;
        }
        log_printf("hinoroll loaded");
        if (hooks_init()) {
            crash_install();
            heap_install();
            frame_install();
            overlay_install();
            window_install();
            audio_install();
            effects_install();
            pads_install();
            padui_install();
#ifdef HINOROLL_HARNESS
            harness_install();
#endif
        }
    }
    return TRUE;
}
