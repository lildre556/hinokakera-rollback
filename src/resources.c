#include "resources.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "game.h"
#include "heap.h"
#include "hooks.h"
#include "log.h"

// Texture cache (the renderer's): 999 entries of 0x78 bytes, index 1..:
// {IDirect3DTexture9 *tex (+0x1c); char name[] (+0x20); ... refcount (+0x84); size (+0x88)}.
#define TEX_ENTRY(cache, idx) ((uint8_t *)(cache) + (idx) * TEX_ENTRY_SIZE)
#define TEX_PTR(cache, idx) (*(void **)(TEX_ENTRY(cache, idx) + TEX_PTR_OFF))
#define TEX_REFS(cache, idx) (*(int32_t *)(TEX_ENTRY(cache, idx) + TEX_REFS_OFF))
#define TEX_SLOTS 999

enum { OP_CREATE, OP_ADDREF, OP_RELEASE, OP_TEX_ACQUIRE, OP_TEX_RELEASE };
typedef struct {
    int frame;
    int op;
    void *obj;    // D3D object or texture cache
    int idx;      // texture cache entry
} Entry;

static Entry *journal;
static int journal_len, journal_cap;
static bool journal_on;
static int cur_frame;
static int in_texcache;  // inside the texture cache's own code: its D3D calls are its business
static ResStats stats;

// Texture cache entries kept loaded at zero references while journaling, so a
// rollback that re-runs an acquisition doesn't reload the file.
static void *zombie_cache;
static uint8_t zombie[TEX_SLOTS + 1];

// ---- known D3D objects (created through the device) ----
#define KNOWN_SLOTS 16384
static void *known[KNOWN_SLOTS];  // open addressing, (void*)1 = tombstone
static unsigned known_count;

static unsigned khash(void *p) {
    return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (KNOWN_SLOTS - 1);
}

static bool known_has(void *p) {
    unsigned h = khash(p);
    for (unsigned i = 0; i < KNOWN_SLOTS; i++) {
        void *v = known[(h + i) & (KNOWN_SLOTS - 1)];
        if (v == p) {
            return true;
        }
        if (!v) {
            return false;
        }
    }
    return false;
}

static void known_add(void *p) {
    unsigned h = khash(p);
    for (unsigned i = 0; i < KNOWN_SLOTS; i++) {
        void **v = &known[(h + i) & (KNOWN_SLOTS - 1)];
        if (*v == p) {
            return;
        }
        if (!*v || *v == (void *)1) {
            *v = p;
            known_count++;
            return;
        }
    }
    log_printf("res: known-object table full");
}

static void known_remove(void *p) {
    unsigned h = khash(p);
    for (unsigned i = 0; i < KNOWN_SLOTS; i++) {
        void **v = &known[(h + i) & (KNOWN_SLOTS - 1)];
        if (*v == p) {
            *v = (void *)1;
            known_count--;
            return;
        }
        if (!*v) {
            return;
        }
    }
}

bool res_is_object(const void *p) {
    return p && known_has((void *)p);
}

static bool journaling_now(void) {
    return journal_on && !in_texcache && heap_battle_logic_now();
}

static void add(int op, void *obj, int idx) {
    if (journal_len == journal_cap) {
        journal_cap = journal_cap ? journal_cap * 2 : 4096;
        journal = realloc(journal, journal_cap * sizeof(Entry));
    }
    journal[journal_len++] = (Entry){cur_frame, op, obj, idx};
}

// ---- Direct3D hooks ----
typedef ULONG(WINAPI *refcount_fn)(void *);
#define MAX_CLASSES 6
static refcount_fn rel_orig[MAX_CLASSES], addref_orig[MAX_CLASSES];
static void *rel_target[MAX_CLASSES], *addref_target[MAX_CLASSES];
static int class_count;

static ULONG real_release(void *o, int k) {
    ULONG r = rel_orig[k](o);
    if (r == 0) {
        known_remove(o);
    }
    return r;
}

// Release through the object's own vtable entry (whichever class it is).
static ULONG release_any(void *o) {
    void *fn = (*(void ***)o)[2];
    for (int k = 0; k < class_count; k++) {
        if (rel_target[k] == fn) {
            return real_release(o, k);
        }
    }
    return ((refcount_fn)fn)(o);
}

#define REL_HOOK(k)                                                \
    static ULONG WINAPI hk_release##k(void *o) {                   \
        if (journaling_now() && known_has(o)) {                    \
            add(OP_RELEASE, o, 0);                                 \
            stats.releases_deferred++;                             \
            return 1;                                              \
        }                                                          \
        return real_release(o, k);                                 \
    }                                                              \
    static ULONG WINAPI hk_addref##k(void *o) {                    \
        ULONG r = addref_orig[k](o);                               \
        if (journaling_now() && known_has(o)) {                    \
            add(OP_ADDREF, o, 0);                                  \
        }                                                          \
        return r;                                                  \
    }
REL_HOOK(0)
REL_HOOK(1)
REL_HOOK(2)
REL_HOOK(3)
REL_HOOK(4)
REL_HOOK(5)
static void *rel_hooks[MAX_CLASSES] = {hk_release0, hk_release1, hk_release2, hk_release3, hk_release4, hk_release5};
static void *addref_hooks[MAX_CLASSES] = {hk_addref0, hk_addref1, hk_addref2, hk_addref3, hk_addref4, hk_addref5};

static void created(HRESULT hr, void **out) {
    if (FAILED(hr) || !out || !*out) {
        return;
    }
    known_add(*out);
    if (journaling_now()) {
        add(OP_CREATE, *out, 0);
        stats.creates++;
    }
}

// Direct3D 9: every Create* takes a trailing shared-handle pointer.
typedef HRESULT(WINAPI *create_tex_fn)(void *, UINT, UINT, UINT, DWORD, UINT, UINT, void **, void *);
typedef HRESULT(WINAPI *create_vol_fn)(void *, UINT, UINT, UINT, UINT, DWORD, UINT, UINT, void **, void *);
typedef HRESULT(WINAPI *create_cube_fn)(void *, UINT, UINT, DWORD, UINT, UINT, void **, void *);
typedef HRESULT(WINAPI *create_buf_fn)(void *, UINT, DWORD, DWORD, UINT, void **, void *);
static create_tex_fn orig_create_tex;
static create_vol_fn orig_create_vol;
static create_cube_fn orig_create_cube;
static create_buf_fn orig_create_vb, orig_create_ib;

static HRESULT WINAPI hk_create_tex(void *d, UINT w, UINT h, UINT l, DWORD u, UINT f, UINT p, void **out, void *sh) {
    HRESULT hr = orig_create_tex(d, w, h, l, u, f, p, out, sh);
    created(hr, out);
    return hr;
}
static HRESULT WINAPI hk_create_vol(void *d, UINT w, UINT h, UINT dd, UINT l, DWORD u, UINT f, UINT p, void **out,
                                    void *sh) {
    HRESULT hr = orig_create_vol(d, w, h, dd, l, u, f, p, out, sh);
    created(hr, out);
    return hr;
}
static HRESULT WINAPI hk_create_cube(void *d, UINT e, UINT l, DWORD u, UINT f, UINT p, void **out, void *sh) {
    HRESULT hr = orig_create_cube(d, e, l, u, f, p, out, sh);
    created(hr, out);
    return hr;
}
static HRESULT WINAPI hk_create_vb(void *d, UINT len, DWORD u, DWORD fvf, UINT p, void **out, void *sh) {
    HRESULT hr = orig_create_vb(d, len, u, fvf, p, out, sh);
    created(hr, out);
    return hr;
}
static HRESULT WINAPI hk_create_ib(void *d, UINT len, DWORD u, DWORD fmt, UINT p, void **out, void *sh) {
    HRESULT hr = orig_create_ib(d, len, u, fmt, p, out, sh);
    created(hr, out);
    return hr;
}

// ---- device transforms as battle logic sees them ----
// Battle logic reads the view matrix back from the device (GetTransform) to
// tell which way a fighter faces on screen, and sets the camera at the end of
// its tick. The device's matrices aren't part of the saved state, so logic
// gets its own copy that is: SetTransform during battle logic updates it,
// GetTransform during battle logic reads it.
typedef struct {
    uint32_t type;
    uint32_t valid;
    float m[16];
} Xform;
#define XFORMS 4
static Xform logic_xf[XFORMS];

typedef HRESULT(WINAPI *xform_fn)(void *, UINT, void *);
static xform_fn orig_set_xf, orig_get_xf;

static Xform *xf_slot(UINT type, bool create) {
    for (int i = 0; i < XFORMS; i++) {
        if (logic_xf[i].valid && logic_xf[i].type == type) {
            return &logic_xf[i];
        }
    }
    if (create) {
        for (int i = 0; i < XFORMS; i++) {
            if (!logic_xf[i].valid) {
                logic_xf[i].type = type;
                logic_xf[i].valid = 1;
                return &logic_xf[i];
            }
        }
    }
    return NULL;
}

static HRESULT WINAPI hk_set_xf(void *d, UINT type, void *m) {
    if (m && heap_battle_logic_now()) {
        Xform *x = xf_slot(type, true);
        if (x) {
            memcpy(x->m, m, sizeof x->m);
        }
    }
    return orig_set_xf(d, type, m);
}

static HRESULT WINAPI hk_get_xf(void *d, UINT type, void *m) {
    if (m && heap_battle_logic_now()) {
        Xform *x = xf_slot(type, false);
        if (x) {
            memcpy(m, x->m, sizeof x->m);
            return 0;
        }
        HRESULT hr = orig_get_xf(d, type, m);
        if (SUCCEEDED(hr) && (x = xf_slot(type, true))) {
            memcpy(x->m, m, sizeof x->m);
        }
        return hr;
    }
    return orig_get_xf(d, type, m);
}

// Device loss (another program going fullscreen, the session locking...):
// the game's loop then neither renders nor runs logic. Logged so it can be told
// apart from a hang.
typedef HRESULT(WINAPI *tcl_fn)(void *);
typedef HRESULT(WINAPI *reset_fn)(void *, void *);
static reset_fn orig_reset;
static unsigned resets;
static tcl_fn orig_tcl;
static HRESULT last_tcl;
// Test builds: pretend the device is lost for a while, then needs a Reset
// (what alt-tabbing out of exclusive fullscreen does), without touching the screen.
static volatile LONG sim_lost_ms_until;
static volatile LONG sim_reset_pending;

void res_simulate_device_loss(int ms) {
    sim_lost_ms_until = (LONG)(GetTickCount() + (DWORD)ms);
    sim_reset_pending = 1;
}

static HRESULT WINAPI hk_tcl(void *d) {
    HRESULT hr = orig_tcl(d);
    if (hr == 0 && sim_reset_pending) {
        hr = (LONG)(GetTickCount() - (DWORD)sim_lost_ms_until) < 0 ? (HRESULT)0x88760868 /* DEVICELOST */
                                                                     : (HRESULT)0x88760869; /* DEVICENOTRESET */
    }
    if (hr != last_tcl) {
        log_printf("device: TestCooperativeLevel %08lx -> %08lx", (unsigned long)last_tcl, (unsigned long)hr);
        last_tcl = hr;
    }
    return hr;
}

static HRESULT WINAPI hk_reset(void *d, void *pp) {
    HRESULT hr = orig_reset(d, pp);
    resets++;
    log_printf("device: Reset -> %08lx (%u resets, %u live objects)", (unsigned long)hr, resets, known_count);
    if (SUCCEEDED(hr)) {
        sim_reset_pending = 0;
    }
    return hr;
}

bool res_device_lost(void) {
    return last_tcl != 0;
}

void *res_logic_state(size_t *len) {
    *len = sizeof logic_xf;
    return logic_xf;
}

void res_reset_logic_state(void) {
    memset(logic_xf, 0, sizeof logic_xf);
}

static void hook_class(IUnknown *obj, const char *name) {
    if (!obj) {
        return;
    }
    void **vt = *(void ***)obj;
    void *addref = vt[1], *rel = vt[2];
    bool have = false;
    for (int k = 0; k < class_count; k++) {
        if (rel_target[k] == rel) {
            have = true;
        }
    }
    if (!have && class_count < MAX_CLASSES) {
        int k = class_count++;
        rel_target[k] = rel;
        addref_target[k] = addref;
        hook_code((uintptr_t)rel, rel_hooks[k], (void **)&rel_orig[k], name);
        hook_code((uintptr_t)addref, addref_hooks[k], (void **)&addref_orig[k], name);
    }
    release_any(obj);
}

// ---- texture cache hooks ----
typedef int(THISCALL *tex_acquire_fn)(void *cache, uint32_t *out, const char *name, int flag);
typedef int(THISCALL *tex_release_fn)(void *cache, int idx);
static tex_acquire_fn orig_tex_acquire;
static tex_release_fn orig_tex_release;

static void tex_release_real(void *cache, int idx) {
    if (idx <= 0 || idx > TEX_SLOTS || !TEX_PTR(cache, idx)) {
        return;
    }
    if (journal_on && TEX_REFS(cache, idx) == 1) {
        // Keep it loaded with no references; a re-run acquisition finds it by name.
        TEX_REFS(cache, idx) = 0;
        zombie[idx] = 1;
        zombie_cache = cache;
        return;
    }
    in_texcache++;
    orig_tex_release(cache, idx);
    in_texcache--;
}

static int THISCALL hk_tex_acquire(void *cache, uint32_t *out, const char *name, int flag) {
    bool j = journaling_now();
    in_texcache++;
    // Loading a texture reads the file into scratch buffers; a re-run finds it
    // cached instead. Keep that scratch out of the battle arena so both runs
    // leave the arena identical.
    heap_route_push(false);
    int r = orig_tex_acquire(cache, out, name, flag);
    heap_route_pop();
    in_texcache--;
    if (r >= 0 && out && *out > 0 && *out <= TEX_SLOTS) {
        zombie[*out] = 0;
        if (j) {
            add(OP_TEX_ACQUIRE, cache, (int)*out);
        }
    }
    return r;
}

static int THISCALL hk_tex_release(void *cache, int idx) {
    if (journaling_now()) {
        add(OP_TEX_RELEASE, cache, idx);
        stats.releases_deferred++;
        return 0;
    }
    tex_release_real(cache, idx);
    return 0;
}

// ---- journal processing ----
void res_set_journal(bool on) {
    if (journal_on && !on) {
        res_flush();
    }
    journal_on = on;
}

bool res_journal_on(void) {
    return journal_on;
}

void res_set_frame(int frame) {
    cur_frame = frame;
}

void res_rollback(int frame) {
    while (journal_len > 0 && journal[journal_len - 1].frame >= frame) {
        Entry e = journal[--journal_len];
        switch (e.op) {
        case OP_CREATE:
        case OP_ADDREF:
            release_any(e.obj);
            stats.undone++;
            break;
        case OP_TEX_ACQUIRE:
            tex_release_real(e.obj, e.idx);
            stats.undone++;
            break;
        default:  // a release that never happened
            break;
        }
    }
}

void res_confirm(int frame) {
    int n = 0;
    while (n < journal_len && journal[n].frame <= frame) {
        Entry e = journal[n++];
        if (e.op == OP_RELEASE) {
            release_any(e.obj);
            stats.releases_done++;
        } else if (e.op == OP_TEX_RELEASE) {
            tex_release_real(e.obj, e.idx);
            stats.releases_done++;
        }
    }
    if (n) {
        memmove(journal, journal + n, (journal_len - n) * sizeof(Entry));
        journal_len -= n;
    }
}

void res_flush(void) {
    bool was = journal_on;
    res_confirm(0x7fffffff);
    journal_on = false;
    // Now really drop the textures kept at zero references.
    if (zombie_cache) {
        for (int i = 1; i <= TEX_SLOTS; i++) {
            if (zombie[i]) {
                zombie[i] = 0;
                if (TEX_PTR(zombie_cache, i) && TEX_REFS(zombie_cache, i) == 0) {
                    TEX_REFS(zombie_cache, i) = 1;
                    tex_release_real(zombie_cache, i);
                }
            }
        }
    }
    journal_on = was;
}

ResStats res_stats(void) {
    ResStats s = stats;
    s.journal_len = journal_len;
    s.known_objects = known_count;
    return s;
}

void res_install(void *device) {
    static bool done;
    if (done || !device) {
        return;
    }
    done = true;
    void **vt = *(void ***)device;
    // IDirect3DDevice9 methods.
    hook_code((uintptr_t)vt[23], (void *)hk_create_tex, (void **)&orig_create_tex, "CreateTexture");
    hook_code((uintptr_t)vt[24], (void *)hk_create_vol, (void **)&orig_create_vol, "CreateVolumeTexture");
    hook_code((uintptr_t)vt[25], (void *)hk_create_cube, (void **)&orig_create_cube, "CreateCubeTexture");
    hook_code((uintptr_t)vt[26], (void *)hk_create_vb, (void **)&orig_create_vb, "CreateVertexBuffer");
    hook_code((uintptr_t)vt[27], (void *)hk_create_ib, (void **)&orig_create_ib, "CreateIndexBuffer");
    hook_code((uintptr_t)vt[3], (void *)hk_tcl, (void **)&orig_tcl, "TestCooperativeLevel");
    hook_code((uintptr_t)vt[16], (void *)hk_reset, (void **)&orig_reset, "Reset");
    hook_code((uintptr_t)vt[44], (void *)hk_set_xf, (void **)&orig_set_xf, "SetTransform");
    hook_code((uintptr_t)vt[45], (void *)hk_get_xf, (void **)&orig_get_xf, "GetTransform");
    // One throwaway object of each kind gives each class's AddRef/Release.
    void *o = NULL;
    if (SUCCEEDED(orig_create_vb(device, 64, 0, 2, 1, &o, NULL))) {
        hook_class(o, "VB AddRef/Release");
    }
    o = NULL;
    if (SUCCEEDED(orig_create_ib(device, 64, 0, 101 /* D3DFMT_INDEX16 */, 1, &o, NULL))) {
        hook_class(o, "IB AddRef/Release");
    }
    o = NULL;
    if (SUCCEEDED(orig_create_tex(device, 4, 4, 1, 0, 21 /* A8R8G8B8 */, 1, &o, NULL))) {
        hook_class(o, "Texture AddRef/Release");
    }
    o = NULL;
    if (SUCCEEDED(orig_create_cube(device, 4, 1, 0, 21, 1, &o, NULL))) {
        hook_class(o, "Cube AddRef/Release");
    }
    hook_code(ADDR_TEXCACHE_ACQUIRE, (void *)hk_tex_acquire, (void **)&orig_tex_acquire, "texcache acquire");
    hook_code(ADDR_TEXCACHE_RELEASE, (void *)hk_tex_release, (void **)&orig_tex_release, "texcache release");
    log_printf("res: hooked %d resource classes", class_count);
}
