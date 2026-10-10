// Effects whose draw function changes game state. Render must not change
// anything logic reads: rollback re-runs frames without drawing them, so such
// an effect would live longer or shorter than on the other machine. Fixes move
// the draw-time state changes into the update (see the 3.00 version of this
// file for the pattern). None found in this build yet.
#include "game.h"
#include "hooks.h"

void effects_install(void) {
}
