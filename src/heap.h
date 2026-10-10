#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The game's CRT heap (malloc, free, realloc, calloc, _msize, _expand) is
// replaced by two dlmalloc arenas in write-watched memory:
//   main:   everything else (startup, menus, rendering, other threads)
//   battle: what the main thread allocates while battle logic runs (the
//           battle scene, fighters, effects...). Its pages are what rollback
//           snapshots (src/snapshot.c), allocator bookkeeping included, so
//           restoring them also undoes allocations and frees.
// heap_install runs in DllMain, before the game's CRT starts.
void heap_install(void);

// Main thread only: while the count is positive, allocations go to the battle arena.
void heap_route_push(bool battle);
void heap_route_pop(void);
bool heap_routing_battle(void);
// Battle logic is running on this thread right now.
bool heap_battle_logic_now(void);

typedef struct {
    uint8_t *base;
    size_t size;
} HeapRegion;
HeapRegion heap_battle_region(void);
HeapRegion heap_main_region(void);
// Bytes from the arena base that hold everything in use (page aligned).
size_t heap_main_extent(void);
size_t heap_battle_extent(void);
bool heap_is_battle(const void *p);
bool heap_is_main(const void *p);

typedef struct {
    unsigned main_allocs, battle_allocs;
    unsigned leaked_frees;     // frees of main blocks during battle logic (kept alive)
    unsigned leaked_reallocs;  // main blocks grown during battle logic (copied to battle)
    size_t main_used, battle_used;
    size_t main_peak, battle_peak;
} HeapStats;
HeapStats heap_stats(void);

// Analysis: record allocation sites (return addresses into the game) for main-arena blocks.
void heap_track_sites(bool on);
// Finds the main/battle block containing p: start, size, and up to 4 game return addresses.
bool heap_block_info(const void *p, uintptr_t *start, size_t *size, uintptr_t sites[4]);

// Stress test: record battle-arena allocations made on the main thread.
typedef struct {
    char op;  // 'A'lloc, 'F'ree, 'R'ealloc
    uint32_t size;
    uintptr_t ptr;
    uintptr_t site, site2;
} HeapTraceEntry;
void heap_trace(HeapTraceEntry *buf, int cap);  // NULL stops
int heap_trace_len(void);
