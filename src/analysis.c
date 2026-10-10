// Write analysis (harness command "ww"): which memory outside the battle arena
// does a logic tick change, and what does Render change? Byte diffs of the
// image's .data/.bss, the application object and the main arena around every
// tick, plus the battle arena around every Render.
//   ww on [all]     start (battle ticks only, or every mode)
//   ww off          stop
//   ww report NAME  write hinoroll\ww_NAME.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "harness.h"
#include "heap.h"
#include "log.h"

typedef struct {
    const char *name;
    uint8_t *base;
    size_t size;
    uint8_t *before;
    uint8_t *logic;   // ticks that changed each byte (saturating)
    uint8_t *render;  // renders that changed each byte
} Area;

enum { AREA_DATA, AREA_APP, AREA_MAIN, AREA_BATTLE, AREA_COUNT };
static Area areas[AREA_COUNT];
static bool on, all_modes;
static unsigned logic_ticks, renders;

static bool area_setup(Area *a, const char *name, void *base, size_t size) {
    free(a->before);
    free(a->logic);
    free(a->render);
    a->name = name;
    a->base = base;
    a->size = size;
    a->before = malloc(size);
    a->logic = calloc(size, 1);
    a->render = calloc(size, 1);
    if (!a->before || !a->logic || !a->render) {
        free(a->before);
        free(a->logic);
        free(a->render);
        a->before = a->logic = a->render = NULL;
        a->base = NULL;
        log_printf("ww: no memory for the %s area (%u KB)", name, (unsigned)(size >> 10));
        return false;
    }
    return true;
}

static void observe(bool after, bool render) {
    if (!on) {
        return;
    }
    if (!all_modes && frame_mode() != MODE_BATTLE) {
        return;
    }
    for (int i = 0; i < AREA_COUNT; i++) {
        Area *a = &areas[i];
        if (!a->base) {
            continue;
        }
        // Logic ticks: everything but the battle arena (it is snapshotted whole).
        // Render: everything.
        if (!render && i == AREA_BATTLE) {
            continue;
        }
        if (!after) {
            memcpy(a->before, a->base, a->size);
            continue;
        }
        uint8_t *counts = render ? a->render : a->logic;
        const uint32_t *b = (const uint32_t *)a->before, *n = (const uint32_t *)a->base;
        size_t words = a->size / 4;
        for (size_t w = 0; w < words; w++) {
            if (b[w] != n[w]) {
                for (int k = 0; k < 4; k++) {
                    size_t off = w * 4 + k;
                    if (a->before[off] != a->base[off] && counts[off] < 0xff) {
                        counts[off]++;
                    }
                }
            }
        }
    }
    if (after) {
        if (render) {
            renders++;
        } else {
            logic_ticks++;
        }
    }
}

static void start(bool all) {
    void *app = frame_app();
    if (!app) {
        harness_reply("ww: no app yet");
        return;
    }
    area_setup(&areas[AREA_DATA], ".data", (void *)IMAGE_DATA_START, IMAGE_DATA_END - IMAGE_DATA_START);
    area_setup(&areas[AREA_APP], "app", app, APP_SIZE);
    HeapRegion m = heap_main_region();
    area_setup(&areas[AREA_MAIN], "main", m.base, heap_main_extent() + (1 << 20));
    HeapRegion b = heap_battle_region();
    area_setup(&areas[AREA_BATTLE], "battle", b.base, heap_battle_extent() + (1 << 20));
    logic_ticks = renders = 0;
    all_modes = all;
    on = true;
    heap_track_sites(true);
    harness_reply("ww on (%s)", all ? "all modes" : "battle");
}

static void report_area(FILE *f, Area *a, bool render) {
    uint8_t *c = render ? a->render : a->logic;
    fprintf(f, "\n== %s written by %s\n", a->name, render ? "Render" : "logic ticks");
    size_t i = 0;
    unsigned ranges = 0;
    while (i < a->size) {
        if (!c[i]) {
            i++;
            continue;
        }
        size_t s = i;
        unsigned maxc = 0;
        while (i < a->size && (c[i] || (i + 1 < a->size && c[i + 1] && i - s < 64))) {
            if (c[i] > maxc) {
                maxc = c[i];
            }
            i++;
        }
        uintptr_t addr = (uintptr_t)a->base + s;
        ranges++;
        if (a == &areas[AREA_MAIN] || a == &areas[AREA_BATTLE]) {
            uintptr_t bs;
            size_t bl;
            uintptr_t sites[4];
            if (heap_block_info((void *)addr, &bs, &bl, sites)) {
                fprintf(f, "%08x+%-5u (max %u)  block %08x size %u +%x  sites %06x %06x %06x %06x\n", (unsigned)addr,
                        (unsigned)(i - s), maxc, (unsigned)bs, (unsigned)bl, (unsigned)(addr - bs), (unsigned)sites[0],
                        (unsigned)sites[1], (unsigned)sites[2], (unsigned)sites[3]);
                continue;
            }
        }
        if (a == &areas[AREA_APP]) {
            fprintf(f, "app+%05x+%-5u (max %u)\n", (unsigned)s, (unsigned)(i - s), maxc);
        } else {
            fprintf(f, "%08x+%-5u (max %u)\n", (unsigned)addr, (unsigned)(i - s), maxc);
        }
    }
    fprintf(f, "(%u ranges)\n", ranges);
}

static void report(const char *name) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ww_%s.txt", harness_dir(), *name ? name : "report");
    FILE *f = fopen(path, "w");
    if (!f) {
        harness_reply("ww: can't write %s", path);
        return;
    }
    fprintf(f, "logic ticks %u, renders %u\n", logic_ticks, renders);
    for (int i = 0; i < AREA_COUNT; i++) {
        if (areas[i].base && i != AREA_BATTLE) {
            report_area(f, &areas[i], false);
        }
    }
    for (int i = 0; i < AREA_COUNT; i++) {
        if (areas[i].base) {
            report_area(f, &areas[i], true);
        }
    }
    fclose(f);
    harness_reply("ww report %s (%u ticks, %u renders)", path, logic_ticks, renders);
}

static int handle(const char *cmd, const char *args) {
    if (strcmp(cmd, "ww") != 0) {
        return 0;
    }
    if (strncmp(args, "on", 2) == 0) {
        start(strstr(args, "all") != NULL);
    } else if (strncmp(args, "off", 3) == 0) {
        on = false;
        harness_reply("ww off");
    } else if (strncmp(args, "report", 6) == 0) {
        const char *n = args + 6;
        while (*n == ' ') {
            n++;
        }
        report(n);
    } else {
        harness_reply("ww on [all] | off | report NAME");
    }
    return 1;
}

void analysis_install(void) {
    frame_set_observer(observe);
    harness_add_handler(handle);
}
