// Rollback stress test (harness "stress"): during battles, every `every` ticks
// roll back `depth` frames, re-run them with the same inputs and compare the
// result with the original run. Bytes Render writes in the battle arena are
// left out of the comparison (they are redrawn every frame).
//   stress DEPTH [EVERY] [verbose]   start (inputs come from the devices / harness scripts)
//   stress off
//   stress report                    checks done, failures
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "harness.h"
#include "heap.h"
#include "log.h"
#include "resources.h"
#include "snapshot.h"

#define RING 32

static bool on, verbose;
static int depth = 4, every = 1;
static int frame;          // frames run in this battle
static bool in_battle;
static Snap *ring[RING];
static uint16_t inputs[RING][2];
static Snap *orig_snap, *resim_snap;
static unsigned checks, failures, max_report = 24;
static uint8_t *render_mask;
static size_t render_mask_len;
static uint8_t *before_render;
static unsigned ticks_since_report;
// "stress life N": set both fighters' life at the next battle tick, before its
// state is saved, and don't roll back across that tick (the poke isn't an input).
static int pending_life = -1;
static int no_check_until;

static void apply_life(void *app) {
    uint8_t *battle = game_battle(app);
    if (!battle) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        uint8_t *f = *(uint8_t **)(battle + BATTLE_FIGHTER + i * 4);
        if (f && *(int32_t *)(f + FIGHTER_LIFE) > pending_life) {
            *(int32_t *)(f + FIGHTER_LIFE) = pending_life;
        }
    }
    pending_life = -1;
    no_check_until = frame + depth + 2;
}

static void report_line(const char *l) {
    harness_reply("%s", l);
    log_printf("%s", l);
}

static void observe(bool after, bool render) {
    if (!on || !render || !in_battle) {
        return;
    }
    HeapRegion b = heap_battle_region();
    size_t len = heap_battle_extent();
    if (len > render_mask_len) {
        render_mask = realloc(render_mask, len);
        memset(render_mask + render_mask_len, 0, len - render_mask_len);
        before_render = realloc(before_render, len);
        render_mask_len = len;
    }
    if (!after) {
        memcpy(before_render, b.base, len);
        return;
    }
    const uint32_t *x = (const uint32_t *)before_render, *y = (const uint32_t *)b.base;
    for (size_t w = 0; w < len / 4; w++) {
        if (x[w] != y[w]) {
            for (int k = 0; k < 4; k++) {
                if (before_render[w * 4 + k] != b.base[w * 4 + k]) {
                    render_mask[w * 4 + k] = 1;
                }
            }
        }
    }
}

static bool gate_battle_end(void) {
    // Never let a re-run frame end the battle.
    return !frame_resimulating();
}

// Battle-arena allocations per frame, original run vs re-run.
#define TRACE_CAP 2048
static HeapTraceEntry traces[RING][TRACE_CAP];
static int trace_lens[RING];
static HeapTraceEntry resim_trace[TRACE_CAP];
static Snap *step_snap;
static unsigned trace_reports;

static void compare_traces(int f, int resim_len) {
    const HeapTraceEntry *a = traces[f % RING], *b = resim_trace;
    int na = trace_lens[f % RING] < TRACE_CAP ? trace_lens[f % RING] : TRACE_CAP;
    int nb = resim_len < TRACE_CAP ? resim_len : TRACE_CAP;
    int i = 0;
    while (i < na && i < nb && a[i].op == b[i].op && a[i].size == b[i].size && a[i].ptr == b[i].ptr) {
        i++;
    }
    if ((i < na || i < nb) && trace_reports < 12) {
        trace_reports++;
        char line[200];
        snprintf(line, sizeof line, "  alloc trace of frame %d differs at #%d (original %d ops, re-run %d ops):", f, i,
                 trace_lens[f % RING], resim_len);
        report_line(line);
        for (int k = i; k < i + 3; k++) {
            if (k < na) {
                snprintf(line, sizeof line, "    original %c %u at %06x/%06x ptr %08x", a[k].op, a[k].size,
                         (unsigned)a[k].site, (unsigned)a[k].site2, (unsigned)a[k].ptr);
                report_line(line);
            }
            if (k < nb) {
                snprintf(line, sizeof line, "    re-run   %c %u at %06x/%06x ptr %08x", b[k].op, b[k].size,
                         (unsigned)b[k].site, (unsigned)b[k].site2, (unsigned)b[k].ptr);
                report_line(line);
            }
        }
    }
}

// Which battle-logic allocation last held a now free address (from the traces).
static void explain_free(uintptr_t addr, char *out, size_t n) {
    for (int back = 0; back < RING; back++) {
        int f = frame - 1 - back;
        if (f < 0) {
            break;
        }
        const HeapTraceEntry *t = traces[f % RING];
        int len = trace_lens[f % RING] < TRACE_CAP ? trace_lens[f % RING] : TRACE_CAP;
        for (int i = len - 1; i >= 0; i--) {
            if ((t[i].op == 'A' || t[i].op == 'R') && addr >= t[i].ptr && addr < t[i].ptr + t[i].size) {
                snprintf(out, n, ", was %u bytes at %08x from %06x/%06x frame %d", t[i].size, (unsigned)t[i].ptr,
                         (unsigned)t[i].site, (unsigned)t[i].site2, f);
                return;
            }
        }
    }
}

static void report_statics_diff(const uint8_t *a, const uint8_t *b) {
    // Which outside regions differ (by name).
    static const char *names[] = {"app", "sys", "input", "rng", "holdrand", "replay", "camera", "transforms"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n;
        const uint8_t *ra = snap_region(step_snap, names[i], &n);
        if (!ra) {
            continue;
        }
        size_t off = (size_t)(ra - step_snap->statics);
        if (memcmp(a + off, b + off, n) != 0) {
            for (size_t k = 0; k < n; k++) {
                if (a[off + k] != b[off + k]) {
                    char line[160];
                    snprintf(line, sizeof line, "  %s+%x differs (%02x vs %02x)", names[i], (unsigned)k, a[off + k],
                             b[off + k]);
                    report_line(line);
                    break;
                }
            }
        }
    }
}

static void check(void) {
    snap_capture(orig_snap, frame);
    int from = frame - depth;
    snap_load(ring[from % RING]);
    res_rollback(from);
    int first_step = -1;
    bool first_step_statics = false;
    static uint8_t *old_statics, *step_resim_statics;
    if (!old_statics) {
        old_statics = malloc(snap_statics_size());
        step_resim_statics = malloc(snap_statics_size());
    }
    for (int f = from; f < frame; f++) {
        if (f > from) {
            // The re-run replaces the saved states (like GGPO does): the old ones
            // refer to resources the rollback just released. Comparing the
            // re-saved state with the original one shows the first frame where
            // the re-run went another way.
            Snap *r = ring[f % RING];
            uint32_t old_sum = r->arena_sum;
            memcpy(old_statics, r->statics, snap_statics_size());
            snap_save(r, f);
            r->arena_sum = snap_live_checksum(render_mask, render_mask_len);
            bool statics_differ = memcmp(old_statics, r->statics, snap_statics_size()) != 0;
            if (first_step < 0 && (statics_differ || old_sum != r->arena_sum)) {
                first_step = f;
                first_step_statics = statics_differ;
                if (statics_differ) {
                    memcpy(step_snap->statics, old_statics, snap_statics_size());
                    memcpy(step_resim_statics, r->statics, snap_statics_size());
                }
            }
        }
        res_set_frame(f);
        heap_trace(resim_trace, TRACE_CAP);
        frame_run(inputs[f % RING][0], inputs[f % RING][1], true);
        int n = heap_trace_len();
        heap_trace(NULL, 0);
        compare_traces(f, n);
        memcpy(traces[f % RING], resim_trace, sizeof resim_trace);
        trace_lens[f % RING] = n;
    }
    snap_capture(resim_snap, frame);
    checks++;
    if (verbose || memcmp(orig_snap->statics, resim_snap->statics, snap_statics_size()) != 0 ||
        orig_snap->arena_len != resim_snap->arena_len || memcmp(orig_snap->arena, resim_snap->arena, orig_snap->arena_len) != 0) {
        int n = snap_diff(orig_snap, resim_snap, 0, report_line, render_mask);
        if (n > 0) {
            failures++;
            if (failures <= 5) {
                harness_reply("stress MISMATCH at frame %d (rolled back %d), %d ranges:", frame, depth, n);
                log_printf("stress MISMATCH at frame %d (rolled back %d), %d ranges:", frame, depth, n);
                if (first_step >= 0) {
                    log_printf("  the re-run first differed before frame %d (%s)", first_step,
                               first_step_statics ? "outside the arena" : "arena checksum");
                    if (first_step_statics) {
                        report_statics_diff(step_snap->statics, step_resim_statics);
                    }
                }
                snap_diff(orig_snap, resim_snap, (int)max_report, report_line, render_mask);
            }
        }
    }
}

static int driver(void *app) {
    uint16_t p1 = frame_poll_local(0), p2 = frame_poll_local(1);
    if (frame_mode() != MODE_BATTLE) {
        if (in_battle) {
            in_battle = false;
            res_set_journal(false);
            log_printf("stress: battle over after %d frames, %u checks, %u failures", frame, checks, failures);
        }
        return frame_run(p1, p2, false);
    }
    if (!in_battle) {
        in_battle = true;
        frame = 0;
        no_check_until = 0;
        snap_reset();  // frame numbers start over
        res_set_journal(true);
        if (render_mask) {
            memset(render_mask, 0, render_mask_len);
        }
    }
    if (pending_life >= 0) {
        apply_life(app);
    }
    snap_save(ring[frame % RING], frame);
    ring[frame % RING]->arena_sum = snap_live_checksum(render_mask, render_mask_len);
    inputs[frame % RING][0] = p1;
    inputs[frame % RING][1] = p2;
    res_set_frame(frame);
    heap_trace(traces[frame % RING], TRACE_CAP);
    int r = frame_run(p1, p2, false);
    trace_lens[frame % RING] = heap_trace_len();
    heap_trace(NULL, 0);
    frame++;
    if (frame_mode() == MODE_BATTLE && frame > depth + 1 && frame >= no_check_until && frame % every == 0) {
        check();
    }
    res_confirm(frame - depth - 2);
    if (++ticks_since_report >= 600) {
        ticks_since_report = 0;
        ResStats rs = res_stats();
        SnapStats ss = snap_stats();
        log_printf("stress: frame %d, %u checks, %u failures; journal %u, creates %u, deferred %u, done %u, undone %u; "
                   "pages per save %u (max %u), page journal %u KB",
                   frame, checks, failures, rs.journal_len, rs.creates, rs.releases_deferred, rs.releases_done, rs.undone,
                   ss.pages_last_save, ss.pages_max_save, (unsigned)(ss.journal_bytes >> 10));
    }
    return r;
}

static int handle(const char *cmd, const char *args) {
    if (strcmp(cmd, "stress") != 0) {
        return 0;
    }
    if (strncmp(args, "off", 3) == 0) {
        on = false;
        frame_set_driver(NULL);
        res_set_journal(false);
        in_battle = false;
        harness_reply("stress off: %u checks, %u failures", checks, failures);
    } else if (strncmp(args, "life", 4) == 0) {
        pending_life = atoi(args + 4);
        harness_reply("stress life %d", pending_life);
    } else if (strncmp(args, "report", 6) == 0) {
        ResStats rs = res_stats();
        harness_reply("stress %s: frame %d, %u checks, %u failures; res journal %u creates %u deferred %u done %u undone %u",
                      on ? "on" : "off", frame, checks, failures, rs.journal_len, rs.creates, rs.releases_deferred,
                      rs.releases_done, rs.undone);
    } else {
        char *rest;
        depth = (int)strtol(args, &rest, 0);
        if (depth < 1 || depth > RING - 4) {
            depth = 4;
        }
        every = (int)strtol(rest, &rest, 0);
        if (every < 1) {
            every = 1;
        }
        verbose = strstr(rest, "verbose") != NULL;
        if (!frame_app()) {
            harness_reply("stress: no app yet");
            return 1;
        }
        snap_init(frame_app());
        for (int i = 0; i < RING; i++) {
            if (!ring[i]) {
                ring[i] = snap_new();
            }
        }
        if (!orig_snap) {
            orig_snap = snap_new();
            resim_snap = snap_new();
            step_snap = snap_new();
        }
        trace_reports = 0;
        checks = failures = 0;
        in_battle = false;
        on = true;
        frame_set_battle_end_gate(gate_battle_end);
        frame_set_driver(driver);
        harness_reply("stress on: depth %d every %d", depth, every);
    }
    return 1;
}

void stress_install(void) {
    snap_set_free_explainer(explain_free);
    harness_add_handler(handle);
    frame_set_observer(observe);
}
