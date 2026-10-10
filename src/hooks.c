#include "hooks.h"

#include <windows.h>

#include "MinHook.h"
#include "log.h"

bool hooks_init(void) {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        log_printf("MH_Initialize failed: %d", s);
        return false;
    }
    return true;
}

bool hook_code(uintptr_t addr, void *detour, void **orig, const char *name) {
    MH_STATUS s = MH_CreateHook((void *)addr, detour, orig);
    if (s == MH_OK) {
        s = MH_EnableHook((void *)addr);
    }
    if (s != MH_OK) {
        log_printf("hook %s at %#x failed: %d", name, (unsigned)addr, s);
        return false;
    }
    return true;
}

void patch_bytes(uintptr_t addr, const void *bytes, unsigned len) {
    DWORD old;
    VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, bytes, len);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
}

void *hook_vtable(uintptr_t vtable, int index, void *fn) {
    void **slot = (void **)vtable + index;
    void *old = *slot;
    patch_bytes((uintptr_t)slot, &fn, sizeof fn);
    return old;
}
