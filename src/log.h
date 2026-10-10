#pragma once

// hinoroll.log in the game folder. Thread-safe; flushed on every line.
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// The game folder (no trailing slash).
const char *game_dir(void);
