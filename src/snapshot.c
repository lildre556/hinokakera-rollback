#include "snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "game.h"
#include "heap.h"
#include "log.h"
#include "frame.h"
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
    uint8_t *sys = game_sys(app);
    // FrameMove's ms accumulator and tick count.
    add_region("app", app_ptr(app, APP_MS_ACC), 8);
    // The system object's match flow, records and per-match state (not its
    // device pointers or its copy of the config).
    add_region("sys", sys ? sys + 0xf8 : NULL, SYS_SIZE - 0xf8);
    // Held/previous/edge/lockout records of both players.
    add_region("input", game_input(app) + INPUT_REC, INPUT_REC_SIZE);
    // The game's table RNG {count, table, index}.
    add_region("rng", G_RNG, 12);
    // The C runtime's rand() seed in the main thread's _tiddata (+0x14).
    add_region("holdrand", frame_holdrand(), 4);
    // Replay recorder (battle writes the input log index each tick).
    add_region("replay", sys ? PTR_PTR(sys, SYS_REPLAY) : NULL, 0x50);
    // The renderer's camera: battle logic sets the view (and may set the
    // projection and light) each tick, and sprites copy the view's rotation
    // during logic (renderer +0xd70 projection, +0xdb0 view, +0xdf0 light).
    uint8_t *renderer = APP_PTR(app, APP_RENDERER);
    add_region("camera", renderer ? renderer + 0xd70 : NULL, 0xe20 - 0xd70);
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

// ---- the arena journal ----
#define PAGE 4096u
#define KEEP_FRAMES 40  // journal records kept (GGPO rolls back at most 8; the ring holds 16)

typedef struct {
    int frame;
    unsigned count;
    uint32_t *pages;  // page indices from the arena base
    uint8_t *data;    // their contents at the previous save
} Record;

static Record *records;
static int record_count, record_cap;
static uint8_t *shadow;          // the arena as of the last save
static size_t shadow_committed;  // bytes
static size_t shadow_len;        // arena extent covered by the last save
static bool have_shadow;
static int last_frame = -1;
static void **dirty;
static ULONG_PTR dirty_cap;
static SnapStats stats;

static void drop_record(Record *r) {
    free(r->pages);
    free(r->data);
    stats.journal_bytes -= (size_t)r->count * (PAGE + sizeof(uint32_t));
    r->pages = NULL;
    r->data = NULL;
}

void snap_reset(void) {
    for (int i = 0; i < record_count; i++) {
        drop_record(&records[i]);
    }
    record_count = 0;
    have_shadow = false;
    last_frame = -1;
}

static bool shadow_reserve(size_t len) {
    HeapRegion b = heap_battle_region();
    if (!shadow) {
        shadow = VirtualAlloc(NULL, b.size, MEM_RESERVE, PAGE_READWRITE);
        if (!shadow) {
            log_printf("snapshot: can't reserve the shadow copy (%u MB)", (unsigned)(b.size >> 20));
            return false;
        }
    }
    if (len > shadow_committed) {
        size_t want = (len + (4u << 20)) & ~(size_t)0xfffff;
        if (want > b.size) {
            want = b.size;
        }
        if (!VirtualAlloc(shadow, want, MEM_COMMIT, PAGE_READWRITE)) {
            log_printf("snapshot: can't commit %u MB for the shadow copy", (unsigned)(want >> 20));
            return false;
        }
        shadow_committed = want;
    }
    return true;
}

// Pages of the arena written since the last reset of the write watch (and resets it).
static ULONG_PTR written_pages(size_t len) {
    HeapRegion b = heap_battle_region();
    ULONG_PTR want = len / PAGE + 1;
    if (want > dirty_cap) {
        free(dirty);
        dirty_cap = want + 1024;
        dirty = malloc(dirty_cap * sizeof(void *));
    }
    ULONG_PTR count = dirty_cap;
    DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, b.base, len, dirty, &count, &gran) != 0) {
        log_printf("snapshot: GetWriteWatch failed (%lu)", GetLastError());
        return 0;
    }
    return count;
}

static void save_statics(Snap *s) {
    uint8_t *p = s->statics;
    for (int i = 0; i < region_count; i++) {
        memcpy(p, regions[i].addr, regions[i].len);
        p += regions[i].len;
    }
}

static void load_statics(const Snap *s) {
    const uint8_t *p = s->statics;
    for (int i = 0; i < region_count; i++) {
        memcpy(regions[i].addr, p, regions[i].len);
        p += regions[i].len;
    }
}

void snap_save(Snap *s, int frame) {
    HeapRegion b = heap_battle_region();
    size_t len = heap_battle_extent();
    stats.saves++;
    save_statics(s);
    s->frame = frame;
    s->battle = game_battle(frame_app()) != NULL;
    s->hashed = false;
    if (!shadow_reserve(len)) {
        return;
    }
    if (!have_shadow) {
        written_pages(b.size);  // reset
        memcpy(shadow, b.base, len);
        shadow_len = len;
        have_shadow = true;
        last_frame = frame;
        return;
    }
    if (frame <= last_frame) {
        log_printf("snapshot: save of frame %d after frame %d without a load", frame, last_frame);
    }
    ULONG_PTR n = written_pages(len > shadow_len ? len : shadow_len);
    // Old records nobody can load any more.
    int drop = 0;
    while (drop < record_count && records[drop].frame <= frame - KEEP_FRAMES) {
        drop_record(&records[drop++]);
    }
    if (drop) {
        memmove(records, records + drop, (record_count - drop) * sizeof(Record));
        record_count -= drop;
    }
    if (record_count == record_cap) {
        record_cap = record_cap ? record_cap * 2 : 64;
        records = realloc(records, record_cap * sizeof(Record));
    }
    Record *r = &records[record_count++];
    r->frame = frame;
    r->count = (unsigned)n;
    r->pages = malloc((n ? n : 1) * sizeof(uint32_t));
    r->data = malloc((n ? n : 1) * PAGE);
    for (ULONG_PTR i = 0; i < n; i++) {
        size_t off = (size_t)((uint8_t *)dirty[i] - b.base);
        r->pages[i] = (uint32_t)(off / PAGE);
        memcpy(r->data + i * PAGE, shadow + off, PAGE);
        memcpy(shadow + off, b.base + off, PAGE);
    }
    if (len > shadow_len) {
        shadow_len = len;
    }
    stats.journal_bytes += (size_t)n * (PAGE + sizeof(uint32_t));
    stats.pages_last_save = (unsigned)n;
    if (n > stats.pages_max_save) {
        stats.pages_max_save = (unsigned)n;
    }
    stats.journal_frames = (unsigned)record_count;
    last_frame = frame;
}

void snap_load(const Snap *s) {
    HeapRegion b = heap_battle_region();
    stats.loads++;
    if (!have_shadow) {
        log_printf("snapshot: load of frame %d with nothing saved", s->frame);
        return;
    }
    // Back to the last save: pages written since then come from the shadow.
    ULONG_PTR n = written_pages(shadow_len);
    for (ULONG_PTR i = 0; i < n; i++) {
        size_t off = (size_t)((uint8_t *)dirty[i] - b.base);
        memcpy(b.base + off, shadow + off, PAGE);
    }
    // Then undo the saves after the loaded frame, newest first.
    while (record_count > 0 && records[record_count - 1].frame > s->frame) {
        Record *r = &records[--record_count];
        for (unsigned i = 0; i < r->count; i++) {
            size_t off = (size_t)r->pages[i] * PAGE;
            memcpy(b.base + off, r->data + i * PAGE, PAGE);
            memcpy(shadow + off, r->data + i * PAGE, PAGE);
        }
        drop_record(r);
    }
    if (last_frame > s->frame && record_count > 0 && records[record_count - 1].frame < s->frame) {
        log_printf("snapshot: frame %d isn't in the journal (oldest kept %d)", s->frame,
                   record_count ? records[0].frame : -1);
    }
    ResetWriteWatch(b.base, shadow_len);
    stats.journal_frames = (unsigned)record_count;
    last_frame = s->frame;
    load_statics(s);
}

SnapStats snap_stats(void) {
    return stats;
}

void snap_capture(Snap *s, int frame) {
    HeapRegion b = heap_battle_region();
    size_t len = heap_battle_extent();
    if (len > s->arena_cap) {
        VirtualFree(s->arena, 0, MEM_RELEASE);
        s->arena_cap = (len + (4 << 20)) & ~(size_t)0xfffff;
        s->arena = VirtualAlloc(NULL, s->arena_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!s->arena) {
            log_printf("snapshot: can't allocate a %u MB copy", (unsigned)(s->arena_cap >> 20));
            s->arena_cap = s->arena_len = 0;
            return;
        }
    }
    memcpy(s->arena, b.base, len);
    s->arena_len = len;
    save_statics(s);
    s->frame = frame;
}

uint32_t snap_live_checksum(const uint8_t *ignore, size_t ignore_len) {
    HeapRegion b = heap_battle_region();
    size_t len = heap_battle_extent();
    const uint32_t *w = (const uint32_t *)b.base;
    const uint32_t *m = (const uint32_t *)ignore;
    size_t words = len / 4, mwords = ignore ? ignore_len / 4 : 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; i++) {
        if (i < mwords && m[i]) {
            continue;
        }
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static snap_explain_fn free_explainer;

void snap_set_free_explainer(snap_explain_fn fn) {
    free_explainer = fn;
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
                char why[96] = "";
                if (free_explainer) {
                    free_explainer(addr, why, sizeof why);
                }
                snprintf(line, sizeof line, "  arena %08x len %u (no block%s)  %08x vs %08x", (unsigned)addr,
                         (unsigned)(i - s), why, va, vb);
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

