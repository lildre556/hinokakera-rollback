#pragma once
#include <stdbool.h>

#include "netplay.h"

// Netplay setup before the game initializes (hooked into the application's
// init, where the old netvs handshake also happened, so the host's settings are
// in place before the game uses them):
//   hinokakera.exe --netplay                         connection window (codes)
//   hinokakera.exe --netplay-direct P PORT IP:PORT [DELAY]   no window (testing)
// Returns true when a session was agreed; netplay starts on the first tick.
bool session_before_init(void *app);
bool session_take(NetplaySetup *out);
