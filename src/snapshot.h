#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A saved game state: the battle arena (everything battle logic allocated,
// with the allocator's own bookkeeping) plus the few places outside it that
// logic writes (docs/rollback.md lists them).
typedef struct Snap {
    int frame;
    size_t arena_len;
    size_t arena_cap;
    uint8_t *arena;
    uint8_t *statics;
    uint32_t checksum;
} Snap;

// Call once the application object exists (first tick).
void snap_init(void *app);
Snap *snap_new(void);
void snap_free(Snap *s);
void snap_save(Snap *s, int frame);
void snap_load(const Snap *s);
// Fast checksum of the saved state.
uint32_t snap_checksum(const Snap *s);
size_t snap_statics_size(void);

// A saved region by name ("app", "input", ...), and where a live battle-arena
// address is inside a snapshot's copy (NULL if outside).
const uint8_t *snap_region(const Snap *s, const char *name, size_t *len);
const uint8_t *snap_arena_ptr(const Snap *s, const void *live);

// Writes the differences between two snapshots to the harness/log (at most
// `max` ranges). Returns the number of differing ranges.
typedef void (*snap_report_fn)(const char *line);
int snap_diff(const Snap *a, const Snap *b, int max, snap_report_fn report, const uint8_t *ignore_arena_mask);
