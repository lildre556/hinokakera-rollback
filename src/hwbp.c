// Hardware data breakpoints on the game thread (harness "hwbp"):
//   hwbp ADDR [LEN] [w|rw]   watch writes (or reads+writes) of 1/2/4 bytes
//   hwbp list                writers seen so far (EIP after the access, count, mode)
//   hwbp clear
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "frame.h"
#include "harness.h"
#include "log.h"

static DWORD main_tid;
static uintptr_t watch_addr[4];
static int watch_len[4], watch_rw[4];
static int watch_count;

typedef struct {
    uintptr_t eip;
    uintptr_t caller;
    int slot;
    int mode;
    unsigned count;
} Hit;
static Hit hits[64];
static int hit_count;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *c = ep->ContextRecord;
    DWORD dr6 = c->Dr6;
    if (!(dr6 & 0xf)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    int slot = __builtin_ctz(dr6 & 0xf);
    uintptr_t eip = c->Eip;
    // Best-effort caller: the dword at ESP if it looks like a return address.
    uintptr_t caller = 0;
    uintptr_t *sp = (uintptr_t *)c->Esp;
    for (int i = 0; i < 16 && !IsBadReadPtr(sp + i, 4); i++) {
        if (sp[i] >= 0x401000 && sp[i] < 0x4ba2d4) {
            caller = sp[i];
            break;
        }
    }
    int mode = frame_resimulating() ? 2 : frame_in_render() ? 1 : 0;
    int i;
    for (i = 0; i < hit_count; i++) {
        if (hits[i].eip == eip && hits[i].caller == caller && hits[i].slot == slot && hits[i].mode == mode) {
            hits[i].count++;
            break;
        }
    }
    if (i == hit_count && hit_count < 64) {
        hits[hit_count++] = (Hit){eip, caller, slot, mode, 1};
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI apply_thread(LPVOID arg) {
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, main_tid);
    if (!t) {
        return 1;
    }
    SuspendThread(t);
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(t, &c);
    c.Dr7 = 0;
    DWORD *drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
    for (int i = 0; i < watch_count; i++) {
        *drs[i] = watch_addr[i];
        unsigned len = watch_len[i] == 1 ? 0 : watch_len[i] == 2 ? 1 : 3;
        unsigned rw = watch_rw[i] ? 3 : 1;
        c.Dr7 |= (1u << (i * 2)) | (rw << (16 + i * 4)) | (len << (18 + i * 4));
    }
    SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

static void apply(void) {
    HANDLE h = CreateThread(NULL, 0, apply_thread, NULL, 0, NULL);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
}

static int handle(const char *cmd, const char *args) {
    if (strcmp(cmd, "hwbp") != 0) {
        return 0;
    }
    if (strncmp(args, "clear", 5) == 0) {
        watch_count = 0;
        hit_count = 0;
        apply();
        harness_reply("hwbp cleared");
    } else if (strncmp(args, "list", 4) == 0) {
        for (int i = 0; i < hit_count; i++) {
            harness_reply("hwbp slot %d %08x eip %06x caller %06x x%u %s", hits[i].slot, (unsigned)watch_addr[hits[i].slot],
                          (unsigned)hits[i].eip, (unsigned)hits[i].caller, hits[i].count,
                          hits[i].mode == 2 ? "resim" : hits[i].mode == 1 ? "RENDER" : "logic");
        }
        harness_reply("hwbp %d writers", hit_count);
    } else {
        if (watch_count >= 4) {
            harness_reply("hwbp: all 4 slots used");
            return 1;
        }
        char *rest;
        uintptr_t addr = strtoul(args, &rest, 16);
        int len = (int)strtol(rest, &rest, 0);
        if (len != 1 && len != 2) {
            len = 4;
        }
        addr &= ~(uintptr_t)(len - 1);
        while (*rest == ' ') {
            rest++;
        }
        watch_addr[watch_count] = addr;
        watch_len[watch_count] = len;
        watch_rw[watch_count] = strncmp(rest, "rw", 2) == 0;
        watch_count++;
        apply();
        harness_reply("hwbp %d: %08x len %d %s", watch_count - 1, (unsigned)addr, len, watch_rw[watch_count - 1] ? "rw" : "w");
    }
    return 1;
}

void hwbp_install(void) {
    main_tid = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, veh);
    harness_add_handler(handle);
}
