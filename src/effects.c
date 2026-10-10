// Effects whose draw function changes game state. The effect manager runs
// each effect's update (vtable +4: age it, report when it's done) in logic and
// its draw (vtable +8) in Render. Three effect classes also change state in
// draw, which then depends on how often a frame is drawn: rollback re-runs
// frames without drawing them, so the effect would live longer or shorter.
// Their draw-time state changes move into the update, where they happen once
// per logic frame (the same as the game at 60 fps); the draws then find
// nothing left to change.
#include <stdbool.h>
#include <stdint.h>

#include "game.h"
#include "hooks.h"

#define EFFECT_AGE(e) (*(int32_t *)((uint8_t *)(e) + 0x0c))
#define EFFECT_LIFE(e) (*(int32_t *)((uint8_t *)(e) + 0x08))

typedef int(THISCALL *update_fn)(void *effect);
static const update_fn base_update = (update_fn)0x437050u;  // age++, done when age >= life

// Falling sparks (eff101, vtable 0x4bd708, draw FUN_004592a0): move and fall
// once per age step; touching the ground leaves them 3 more frames.
static int THISCALL spark_update(void *e) {
    int done = base_update(e) & 0xff;
    uint8_t *p = e;
    int32_t age = EFFECT_AGE(e);
    if (*(int32_t *)(p + 0x38) < age) {
        volatile float *y = (float *)(p + 0x24), *vy = (float *)(p + 0x34);
        *y = *y + *vy;
        if (-1.0f < *vy) {
            *vy = *vy - 0.2f;
        }
        if (0.0f < *y) {
            EFFECT_LIFE(e) = age + 3;
        }
        *(int32_t *)(p + 0x38) = age;
    }
    return done;
}

// Effects kept alive while they are drawn (draw sets life = age + 1000 / + 100).
static int THISCALL keepalive1000_update(void *e) {
    EFFECT_LIFE(e) = EFFECT_AGE(e) + 1000;
    return base_update(e) & 0xff;
}

static int THISCALL keepalive100_update(void *e) {
    EFFECT_LIFE(e) = EFFECT_AGE(e) + 100;
    return base_update(e) & 0xff;
}

void effects_install(void) {
    hook_vtable(0x4bd708u, 1, (void *)spark_update);
    hook_vtable(0x4bcd94u, 1, (void *)keepalive1000_update);
    hook_vtable(0x4bcefcu, 1, (void *)keepalive100_update);
}
