#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A saved game state: the battle arena (everything battle logic allocated,
// with the allocator's own bookkeeping) plus the few places outside it that
// logic writes (docs/rollback.md lists them).
//
// The arena holds ~20 MB in a battle, nearly all of it character data that is
// written once while the battle loads. Saves therefore keep a journal instead
// of copies: the arena's pages are write-watched, a shadow copy holds the
// arena as of the last save, and each save records the previous contents of
// the pages written since then. Loading a frame puts back the pages written
// since its save, newest first. Saves must come in frame order (a load drops
// the journal after the loaded frame), which is how GGPO and the stress test
// use them.
typedef struct Snap {
    int frame;
    uint8_t *statics;
    bool battle;  // saved during a battle
    // Netplay state check of this frame (src/netplay.c fills these at save time).
    bool hashed;
    uint32_t hash;
    uint32_t hash_parts[8];
    // Stress test: checksum of the whole arena when saved.
    uint32_t arena_sum;
    // A full copy of the arena (snap_capture), for comparisons in tests.
    size_t arena_len;
    size_t arena_cap;
    uint8_t *arena;
} Snap;

// Call once the application object exists (first tick).
void snap_init(void *app);
Snap *snap_new(void);
void snap_free(Snap *s);
void snap_save(Snap *s, int frame);
void snap_load(const Snap *s);
// Forget the journal (the next save starts over from a full shadow copy).
void snap_reset(void);
size_t snap_statics_size(void);

typedef struct {
    unsigned saves, loads, journal_frames;
    unsigned pages_last_save, pages_max_save;
    size_t journal_bytes;
} SnapStats;
SnapStats snap_stats(void);

// Tests: a full copy of the live arena and statics into s (s->frame = frame).
void snap_capture(Snap *s, int frame);
// Checksum of the live arena, skipping words where `ignore` (per byte, may be NULL) is set.
uint32_t snap_live_checksum(const uint8_t *ignore, size_t ignore_len);

// A saved region by name ("app", "input", ...) in a snapshot's statics.
const uint8_t *snap_region(const Snap *s, const char *name, size_t *len);

// Writes the differences between two captured snapshots to the harness/log (at
// most `max` ranges). Returns the number of differing ranges.
typedef void (*snap_report_fn)(const char *line);
int snap_diff(const Snap *a, const Snap *b, int max, snap_report_fn report, const uint8_t *ignore_arena_mask);
// Tests: describes an arena address no live block holds (e.g. who allocated it last).
typedef void (*snap_explain_fn)(uintptr_t addr, char *out, size_t n);
void snap_set_free_explainer(snap_explain_fn fn);
