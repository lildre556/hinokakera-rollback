// Crash logging: the faulting address, registers and the game return addresses
// on the stack go to hinoroll.log (first-chance access violations in game or
// Direct3D code, and anything unhandled).
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

#include "log.h"

static int logged;

static void module_of(uintptr_t addr, char *out, size_t n) {
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, sizeof path);
        const char *slash = strrchr(path, '\\');
        snprintf(out, n, "%s+%x", slash ? slash + 1 : path, (unsigned)(addr - (uintptr_t)m));
    } else {
        snprintf(out, n, "?");
    }
}

static void dump(EXCEPTION_POINTERS *ep, const char *kind) {
    CONTEXT *c = ep->ContextRecord;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    char mod[128];
    module_of(c->Eip, mod, sizeof mod);
    log_printf("%s: code %08lx at %08lx (%s) access %s %08lx", kind, r->ExceptionCode, c->Eip, mod,
               r->NumberParameters > 1 ? (r->ExceptionInformation[0] ? "write" : "read") : "-",
               r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0ul);
    log_printf("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
               c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    char line[512];
    int n = 0;
    uintptr_t *sp = (uintptr_t *)c->Esp;
    for (int i = 0, found = 0; i < 512 && found < 16; i++) {
        if (IsBadReadPtr(sp + i, 4)) {
            break;
        }
        uintptr_t v = sp[i];
        if (v >= 0x401000 && v < 0x4ba2d4) {
            n += snprintf(line + n, sizeof line - n, " %06x", (unsigned)v);
            found++;
        }
    }
    log_printf("  game addresses on the stack:%s", line);
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t eip = ep->ContextRecord->Eip;
    // IsBadReadPtr and friends fault on purpose inside kernelbase; only report game/D3D/our code.
    char mod[128];
    module_of(eip, mod, sizeof mod);
    bool interesting = (eip >= 0x400000 && eip < 0x4e3000) || strncmp(mod, "d3d", 3) == 0 ||
                       strncmp(mod, "dinput8.dll", 11) == 0 || strncmp(mod, "dsound", 6) == 0;
    if (interesting && logged < 4) {
        logged++;
        dump(ep, "fault");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) {
    dump(ep, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

// The game's own message function (show, error, text): most calls show
// nothing, so log them all (Shift-JIS text converted to UTF-8).
typedef void(__cdecl *game_msg_fn)(int show, int error, const char *text);
static game_msg_fn orig_game_msg;
static void __cdecl hk_game_msg(int show, int error, const char *text) {
    WCHAR w[512];
    char u[1024];
    if (text && MultiByteToWideChar(932, 0, text, -1, w, 512) > 0 && WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL) > 0) {
        log_printf("game %s: %s", error ? "error" : "message", u);
    }
    orig_game_msg(show, error, text);
}

bool hook_code(uintptr_t addr, void *detour, void **orig, const char *name);

void crash_install(void) {
    hook_code(0x488860, (void *)hk_game_msg, (void **)&orig_game_msg, "game messages");
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled);
}
