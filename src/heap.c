#include "heap.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "game.h"
#include "hooks.h"
#include "log.h"

// ---- dlmalloc, configured for fixed arenas: no locks of its own (heap_lock
// guards it), never mmaps (large blocks and growth stay inside the arena), all
// bookkeeping inside the arena memory.
static void heap_fail(const char *what, const void *p);
#define ONLY_MSPACES 1
#define USE_LOCKS 0
#define MALLOC_ALIGNMENT ((size_t)16U)
#define FOOTERS 0
#define MALLOC_INSPECT_ALL 1
#undef FORCEINLINE  // winnt.h's carries "extern"
#define FORCEINLINE inline
#define MMAP(s) MFAIL
#define DIRECT_MMAP(s) MFAIL
#define MUNMAP(a, s) (-1)
#define ABORT heap_fail("abort", NULL)
#define CORRUPTION_ERROR_ACTION(m) heap_fail("corruption", m)
#define USAGE_ERROR_ACTION(m, p) heap_fail("bad pointer", p)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "dlmalloc.c"
#pragma GCC diagnostic pop

#define MAIN_ARENA_MB 384
#define BATTLE_ARENA_MB 192

static mspace ms_main, ms_battle;
static uint8_t *main_base, *battle_base;
static size_t main_size, battle_size;
static DWORD main_tid;
static int route_depth;   // >0: battle
static int route_stack[16];
static CRITICAL_SECTION heap_lock;
static HeapStats stats;

typedef void *(__cdecl *heap_alloc_fn)(size_t);
typedef void(__cdecl *free_fn)(void *);
typedef void *(__cdecl *realloc_fn)(void *, size_t);
typedef void *(__cdecl *calloc_fn)(size_t, size_t);
typedef size_t(__cdecl *msize_fn)(void *);
typedef void *(__cdecl *expand_fn)(void *, size_t);
static heap_alloc_fn orig_heap_alloc;
static free_fn orig_free;
static realloc_fn orig_realloc;
static calloc_fn orig_calloc;
static msize_fn orig_msize;
static expand_fn orig_expand;

static void heap_fail(const char *what, const void *p) {
    log_printf("heap: %s (%p) - crashing on purpose", what, p);
    RaiseException(0xE0484550u /* 'HEAP' */, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static uint8_t *reserve_arena(size_t size, const char *name) {
    uint8_t *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p && !VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(p, 0, MEM_RELEASE);
        p = NULL;
    }
    if (!p) {
        log_printf("heap: can't reserve the %s arena (%u MB)", name, (unsigned)(size >> 20));
    }
    return p;
}

bool heap_is_battle(const void *p) {
    return (const uint8_t *)p >= battle_base && (const uint8_t *)p < battle_base + battle_size;
}

bool heap_is_main(const void *p) {
    return (const uint8_t *)p >= main_base && (const uint8_t *)p < main_base + main_size;
}

static mspace owner(const void *p) {
    if (heap_is_battle(p)) {
        return ms_battle;
    }
    if (heap_is_main(p)) {
        return ms_main;
    }
    return NULL;
}

static bool on_main_thread(void) {
    return GetCurrentThreadId() == main_tid;
}

// Battle logic is running on this thread: allocations go to the battle arena,
// and main-arena blocks must not be freed or moved (a rollback could bring
// back the pointers to them).
static bool battle_logic(void) {
    return route_depth > 0 && route_stack[route_depth - 1] && on_main_thread();
}

void heap_route_push(bool battle) {
    if (route_depth < (int)(sizeof route_stack / sizeof route_stack[0])) {
        route_stack[route_depth] = battle;
    }
    route_depth++;
}

void heap_route_pop(void) {
    if (route_depth > 0) {
        route_depth--;
    }
}

bool heap_battle_logic_now(void) {
    return battle_logic();
}

bool heap_routing_battle(void) {
    return route_depth > 0 && route_stack[route_depth - 1];
}

// ---- allocation sites (analysis only) ----
typedef struct {
    uintptr_t ptr;
    uintptr_t sites[4];
} SiteEntry;
#define SITE_SLOTS (1u << 20)
static SiteEntry *sites;
static bool track_sites;

static unsigned site_hash(uintptr_t p) {
    return (unsigned)((p >> 4) * 2654435761u) & (SITE_SLOTS - 1);
}

// Game return addresses on the stack: values in .text right after a call.
static void scan_stack(uintptr_t out[4]) {
    uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
    int n = 0;
    for (int i = 0; i < 160 && n < 4; i++) {
        uintptr_t v;
        if (IsBadReadPtr(sp + i, 4)) {
            break;
        }
        v = sp[i];
        if (v < 0x401006 || v >= 0x4ba2d4) {
            continue;
        }
        const uint8_t *c = (const uint8_t *)v;
        if (c[-5] == 0xe8 || (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10) ||
            (c[-6] == 0xff && (c[-5] & 0x38) == 0x10)) {
            // Skip the CRT/operator new wrappers themselves.
            if (v >= 0x4a0000 && v < 0x4b0000) {
                continue;
            }
            out[n++] = v;
        }
    }
    while (n < 4) {
        out[n++] = 0;
    }
}

static void site_add(void *p) {
    if (!track_sites || !sites || !p) {
        return;
    }
    unsigned h = site_hash((uintptr_t)p);
    for (unsigned i = 0; i < 64; i++) {
        SiteEntry *e = &sites[(h + i) & (SITE_SLOTS - 1)];
        if (e->ptr == 0 || e->ptr == 1 || e->ptr == (uintptr_t)p) {
            e->ptr = (uintptr_t)p;
            scan_stack(e->sites);
            return;
        }
    }
}

static void site_remove(void *p) {
    if (!sites) {
        return;
    }
    unsigned h = site_hash((uintptr_t)p);
    for (unsigned i = 0; i < 64; i++) {
        SiteEntry *e = &sites[(h + i) & (SITE_SLOTS - 1)];
        if (e->ptr == (uintptr_t)p) {
            e->ptr = 1;  // tombstone
            return;
        }
        if (e->ptr == 0) {
            return;
        }
    }
}

void heap_track_sites(bool on) {
    if (on && !sites) {
        sites = VirtualAlloc(NULL, SITE_SLOTS * sizeof(SiteEntry), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    track_sites = on && sites;
}

typedef struct {
    uintptr_t want;
    uintptr_t start;
    size_t size;
    bool found;
} FindCtx;

static void find_chunk(void *start, void *end, size_t used, void *arg) {
    FindCtx *f = arg;
    if (used && f->want >= (uintptr_t)start && f->want < (uintptr_t)end) {
        f->start = (uintptr_t)start;
        f->size = (uintptr_t)end - (uintptr_t)start;
        f->found = true;
    }
}

bool heap_block_info(const void *p, uintptr_t *start, size_t *size, uintptr_t out_sites[4]) {
    mspace m = owner(p);
    if (!m) {
        return false;
    }
    FindCtx f = {(uintptr_t)p, 0, 0, false};
    EnterCriticalSection(&heap_lock);
    mspace_inspect_all(m, find_chunk, &f);
    LeaveCriticalSection(&heap_lock);
    if (!f.found) {
        return false;
    }
    *start = f.start;
    *size = f.size;
    memset(out_sites, 0, 4 * sizeof(uintptr_t));
    if (sites) {
        unsigned h = site_hash(f.start);
        for (unsigned i = 0; i < 64; i++) {
            SiteEntry *e = &sites[(h + i) & (SITE_SLOTS - 1)];
            if (e->ptr == f.start) {
                memcpy(out_sites, e->sites, 4 * sizeof(uintptr_t));
                break;
            }
            if (e->ptr == 0) {
                break;
            }
        }
    }
    return true;
}

// ---- allocation tracing (stress test) ----
static HeapTraceEntry *trace_buf;
static int trace_cap, trace_len;

void heap_trace(HeapTraceEntry *buf, int cap) {
    trace_buf = buf;
    trace_cap = cap;
    trace_len = 0;
}

int heap_trace_len(void) {
    return trace_len;
}

static void trace(char op, const void *p, size_t n) {
    if (!trace_buf || !heap_is_battle(p) || !on_main_thread()) {
        return;
    }
    if (trace_len < trace_cap) {
        uintptr_t sites[4];
        scan_stack(sites);
        trace_buf[trace_len] = (HeapTraceEntry){op, (uint32_t)n, (uintptr_t)p, sites[0], sites[1]};
    }
    trace_len++;
}

// ---- the CRT replacements ----
static void count_alloc(mspace m, void *p) {
    if (!p) {
        return;
    }
    if (m == ms_battle) {
        stats.battle_allocs++;
    } else {
        stats.main_allocs++;
    }
    site_add(p);
}

static void *__cdecl hk_heap_alloc(size_t n) {
    if (n == 0) {
        n = 1;
    }
    EnterCriticalSection(&heap_lock);
    mspace m = battle_logic() ? ms_battle : ms_main;
    void *p = mspace_malloc(m, n);
    count_alloc(m, p);
    trace('A', p, n);
    LeaveCriticalSection(&heap_lock);
    if (!p) {
        log_printf("heap: out of memory in the %s arena (%u bytes)", m == ms_battle ? "battle" : "main", (unsigned)n);
    }
    return p;
}

static void __cdecl hk_free(void *p) {
    if (!p) {
        return;
    }
    mspace m = owner(p);
    if (!m) {
        orig_free(p);
        return;
    }
    EnterCriticalSection(&heap_lock);
    if (m == ms_main && battle_logic()) {
        stats.leaked_frees++;  // keep it: rollback may resurrect pointers to it
    } else {
        trace('F', p, mspace_usable_size(p));
        site_remove(p);
        mspace_free(m, p);
    }
    LeaveCriticalSection(&heap_lock);
}

static void *__cdecl hk_realloc(void *p, size_t n) {
    if (!p) {
        return hk_heap_alloc(n);
    }
    if (n == 0) {
        hk_free(p);
        return NULL;
    }
    mspace m = owner(p);
    if (!m) {
        return orig_realloc(p, n);
    }
    EnterCriticalSection(&heap_lock);
    void *q;
    if (m == ms_main && battle_logic()) {
        // Copy into the battle arena and keep the old block.
        size_t old = mspace_usable_size(p);
        q = mspace_malloc(ms_battle, n);
        if (q) {
            memcpy(q, p, old < n ? old : n);
            stats.battle_allocs++;
        }
        stats.leaked_reallocs++;
    } else {
        trace('R', p, n);
        site_remove(p);
        q = mspace_realloc(m, p, n);
        site_add(q ? q : p);
    }
    LeaveCriticalSection(&heap_lock);
    return q;
}

static void *__cdecl hk_calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        return NULL;
    }
    size_t n = count * size;
    void *p = hk_heap_alloc(n ? n : 1);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

static size_t __cdecl hk_msize(void *p) {
    mspace m = owner(p);
    if (!m) {
        return orig_msize(p);
    }
    return mspace_usable_size(p);
}

static void *__cdecl hk_expand(void *p, size_t n) {
    mspace m = owner(p);
    if (!m) {
        return orig_expand(p, n);
    }
    if (m == ms_main && battle_logic()) {
        return mspace_usable_size(p) >= n ? p : NULL;
    }
    EnterCriticalSection(&heap_lock);
    void *q = mspace_realloc_in_place(m, p, n);
    LeaveCriticalSection(&heap_lock);
    return q;
}

HeapRegion heap_battle_region(void) {
    return (HeapRegion){battle_base, battle_size};
}

HeapRegion heap_main_region(void) {
    return (HeapRegion){main_base, main_size};
}

// Bytes from the arena base to the end of the top chunk's header: everything
// the allocator has handed out or keeps bookkeeping in lies below this.
static size_t extent(mspace m, uint8_t *base) {
    mstate st = (mstate)m;
    if (!st || !st->top) {
        return 0;
    }
    size_t e = (size_t)((uint8_t *)st->top - base) + TOP_FOOT_SIZE + sizeof(struct malloc_chunk);
    return (e + 4095) & ~(size_t)4095;
}

size_t heap_main_extent(void) {
    return extent(ms_main, main_base);
}

size_t heap_battle_extent(void) {
    return extent(ms_battle, battle_base);
}

HeapStats heap_stats(void) {
    EnterCriticalSection(&heap_lock);
    struct mallinfo mi = mspace_mallinfo(ms_main);
    stats.main_used = mi.uordblks;
    if (stats.main_used > stats.main_peak) {
        stats.main_peak = stats.main_used;
    }
    mi = mspace_mallinfo(ms_battle);
    stats.battle_used = mi.uordblks;
    if (stats.battle_used > stats.battle_peak) {
        stats.battle_peak = stats.battle_used;
    }
    HeapStats s = stats;
    LeaveCriticalSection(&heap_lock);
    return s;
}

void heap_install(void) {
    InitializeCriticalSectionAndSpinCount(&heap_lock, 4000);
    main_tid = GetCurrentThreadId();
    main_size = (size_t)MAIN_ARENA_MB << 20;
    battle_size = (size_t)BATTLE_ARENA_MB << 20;
    main_base = reserve_arena(main_size, "main");
    battle_base = reserve_arena(battle_size, "battle");
    if (!main_base || !battle_base) {
        log_printf("heap: arenas unavailable, leaving the game's heap alone");
        return;
    }
    ms_main = create_mspace_with_base(main_base, main_size, 0);
    ms_battle = create_mspace_with_base(battle_base, battle_size, 0);
    if (!ms_main || !ms_battle) {
        log_printf("heap: create_mspace_with_base failed");
        return;
    }
    bool ok = hook_code(ADDR_HEAP_ALLOC, (void *)hk_heap_alloc, (void **)&orig_heap_alloc, "_heap_alloc") &&
              hook_code(ADDR_FREE, (void *)hk_free, (void **)&orig_free, "free") &&
              hook_code(ADDR_REALLOC, (void *)hk_realloc, (void **)&orig_realloc, "realloc") &&
              hook_code(ADDR_CALLOC, (void *)hk_calloc, (void **)&orig_calloc, "calloc") &&
              hook_code(ADDR_MSIZE, (void *)hk_msize, (void **)&orig_msize, "_msize") &&
              hook_code(ADDR_EXPAND, (void *)hk_expand, (void **)&orig_expand, "_expand");
    log_printf("heap: main arena %p (%u MB), battle arena %p (%u MB)%s", main_base, MAIN_ARENA_MB, battle_base,
               BATTLE_ARENA_MB, ok ? "" : " - HOOKS FAILED");
}
