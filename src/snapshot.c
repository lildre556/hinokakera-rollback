#include "snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "game.h"
#include "heap.h"
#include "log.h"
#include "resources.h"

typedef struct {
    const char *name;
    uint8_t *addr;
    size_t len;
} Region;

#define MAX_REGIONS 16
static Region regions[MAX_REGIONS];
static int region_count;
static size_t statics_size;

static void add_region(const char *name, void *addr, size_t len) {
    if (!addr || !len) {
        log_printf("snapshot: region %s missing", name);
        return;
    }
    if (region_count == MAX_REGIONS) {
        log_printf("snapshot: can't add region %s", name);
        return;
    }
    regions[region_count++] = (Region){name, addr, len};
    statics_size += len;
}

void snap_init(void *app) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
    // Match flow and the FrameMove timers live in the application object.
    add_region("app", app_ptr(app, 0x2a6d0), 0x2b9b4 - 0x2a6d0);
    // Held/previous/edge/lockout records of both players.
    add_region("input", (uint8_t *)APP_PTR(app, APP_INPUT) + INPUT_REC, INPUT_REC_SIZE);
    // The game's table RNG {count, table, index}.
    add_region("rng", G_RNG, 12);
    // The C runtime's rand() seed in the main thread's _tiddata (+0x14).
    void *ptd = TlsGetValue(*(DWORD *)0x4dac40);
    add_region("holdrand", ptd ? (uint8_t *)ptd + 0x14 : NULL, 4);
    // Replay recorder (battle writes the input log index each tick).
    add_region("replay", APP_PTR(app, APP_REPLAY), 0x44);
    // The renderer wrapper's camera: battle logic sets the view each tick.
    add_region("camera", (uint8_t *)APP_PTR(app, APP_SND) + 0x2040, 0x90);
    // Effect manager pointers set at battle start.
    add_region("globals", (void *)0x4dc268, 8);
    // Device transforms as battle logic sees them (src/resources.c).
    size_t xf_len;
    void *xf = res_logic_state(&xf_len);
    add_region("transforms", xf, xf_len);
    log_printf("snapshot: %d regions, %u bytes outside the battle arena", region_count, (unsigned)statics_size);
}

size_t snap_statics_size(void) {
    return statics_size;
}

Snap *snap_new(void) {
    Snap *s = calloc(1, sizeof *s);
    s->statics = malloc(statics_size ? statics_size : 1);
    s->frame = -1;
    return s;
}

void snap_free(Snap *s) {
    if (s) {
        VirtualFree(s->arena, 0, MEM_RELEASE);
        free(s->statics);
        free(s);
    }
}

void snap_save(Snap *s, int frame) {
    HeapRegion b = heap_battle_region();
    size_t len = heap_battle_extent();
    if (len > s->arena_cap) {
        VirtualFree(s->arena, 0, MEM_RELEASE);
        s->arena_cap = (len + (4 << 20)) & ~(size_t)0xfffff;
        s->arena = VirtualAlloc(NULL, s->arena_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    memcpy(s->arena, b.base, len);
    s->arena_len = len;
    uint8_t *p = s->statics;
    for (int i = 0; i < region_count; i++) {
        memcpy(p, regions[i].addr, regions[i].len);
        p += regions[i].len;
    }
    s->frame = frame;
    s->checksum = 0;
}

void snap_load(const Snap *s) {
    HeapRegion b = heap_battle_region();
    memcpy(b.base, s->arena, s->arena_len);
    const uint8_t *p = s->statics;
    for (int i = 0; i < region_count; i++) {
        memcpy(regions[i].addr, p, regions[i].len);
        p += regions[i].len;
    }
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n) {
    // FNV-1a over 32-bit words (n is a multiple of 4 for the arena).
    const uint32_t *w = (const uint32_t *)p;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    for (size_t i = words * 4; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

uint32_t snap_checksum(const Snap *s) {
    uint32_t h = 2166136261u;
    h = fnv(h, s->arena, s->arena_len);
    h = fnv(h, s->statics, statics_size);
    return h;
}

int snap_diff(const Snap *a, const Snap *b, int max, snap_report_fn report, const uint8_t *ignore) {
    char line[256];
    int ranges = 0;
    // Outside regions.
    size_t off = 0;
    for (int r = 0; r < region_count; r++) {
        const uint8_t *x = a->statics + off, *y = b->statics + off;
        for (size_t i = 0; i < regions[r].len;) {
            if (x[i] == y[i]) {
                i++;
                continue;
            }
            size_t s = i;
            while (i < regions[r].len && (x[i] != y[i] || (i + 1 < regions[r].len && x[i + 1] != y[i + 1]))) {
                i++;
            }
            if (ranges++ < max) {
                snprintf(line, sizeof line, "  %s+%x (%p) len %u", regions[r].name, (unsigned)s, regions[r].addr + s,
                         (unsigned)(i - s));
                report(line);
            }
        }
        off += regions[r].len;
    }
    // Battle arena.
    HeapRegion base = heap_battle_region();
    size_t n = a->arena_len < b->arena_len ? a->arena_len : b->arena_len;
    if (a->arena_len != b->arena_len) {
        snprintf(line, sizeof line, "  arena length %u vs %u", (unsigned)a->arena_len, (unsigned)b->arena_len);
        report(line);
        ranges++;
    }
    for (size_t i = 0; i < n;) {
        if (a->arena[i] == b->arena[i] || (ignore && ignore[i])) {
            i++;
            continue;
        }
        // Handles of Direct3D objects re-created by the re-run differ; they aren't game state.
        size_t w = i & ~(size_t)3;
        uint32_t pa, pb;
        memcpy(&pa, a->arena + w, 4);
        memcpy(&pb, b->arena + w, 4);
        if (res_is_object((void *)pa) || res_is_object((void *)pb)) {
            i = w + 4;
            continue;
        }
        size_t s = i;
        while (i < n && ((a->arena[i] != b->arena[i] && !(ignore && ignore[i])) ||
                         (i + 4 < n && a->arena[i + 4] != b->arena[i + 4]))) {
            i++;
        }
        if (ranges++ < max) {
            uintptr_t addr = (uintptr_t)base.base + s, bs = 0;
            size_t bl = 0;
            uintptr_t sites[4] = {0};
            bool found = heap_block_info((void *)addr, &bs, &bl, sites);
            uint32_t va = 0, vb = 0;
            memcpy(&va, a->arena + (s & ~3u), 4);
            memcpy(&vb, b->arena + (s & ~3u), 4);
            if (found) {
                snprintf(line, sizeof line, "  arena %08x len %u: block %08x size %u +%x sites %06x %06x %06x  %08x vs %08x",
                         (unsigned)addr, (unsigned)(i - s), (unsigned)bs, (unsigned)bl, (unsigned)(addr - bs),
                         (unsigned)sites[0], (unsigned)sites[1], (unsigned)sites[2], va, vb);
            } else {
                snprintf(line, sizeof line, "  arena %08x len %u (no block)  %08x vs %08x", (unsigned)addr,
                         (unsigned)(i - s), va, vb);
            }
            report(line);
        }
    }
    return ranges;
}

const uint8_t *snap_region(const Snap *s, const char *name, size_t *len) {
    size_t off = 0;
    for (int i = 0; i < region_count; i++) {
        if (strcmp(regions[i].name, name) == 0) {
            *len = regions[i].len;
            return s->statics + off;
        }
        off += regions[i].len;
    }
    *len = 0;
    return NULL;
}

const uint8_t *snap_arena_ptr(const Snap *s, const void *live) {
    HeapRegion b = heap_battle_region();
    if ((const uint8_t *)live < b.base || (const uint8_t *)live >= b.base + s->arena_len) {
        return NULL;
    }
    return s->arena + ((const uint8_t *)live - b.base);
}
