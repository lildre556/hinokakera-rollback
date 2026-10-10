#pragma once

// Test harness: tools/dev/hino.sh appends commands to hinoroll\cmd.txt in the
// game folder; they run once per presented frame and answer in
// hinoroll\reply.txt. Only in builds with HINOROLL_HARNESS.
void harness_install(void);
void harness_reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *harness_dir(void);

// Extra commands: return nonzero if the command was handled.
typedef int (*harness_cmd_fn)(const char *cmd, const char *args);
void harness_add_handler(harness_cmd_fn fn);

void analysis_install(void);
void hwbp_install(void);
void stress_install(void);
void quiet_install(void);
void pads_harness_install(void);
void padui_harness_install(void);
