#pragma once
#include <stdbool.h>
#include <stdint.h>

bool hooks_init(void);
// MinHook detour on code at addr; *orig receives the trampoline. Enabled at once.
bool hook_code(uintptr_t addr, void *detour, void **orig, const char *name);
// Replaces entry `index` of the vtable at `vtable`; returns the old entry.
void *hook_vtable(uintptr_t vtable, int index, void *fn);
// Writes bytes into code/rdata (temporarily unprotecting it).
void patch_bytes(uintptr_t addr, const void *bytes, unsigned len);
