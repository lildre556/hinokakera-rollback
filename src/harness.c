#include "harness.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "d3d.h"
#include "frame.h"
#include "game.h"
#include "heap.h"
#include "log.h"

static char dir[MAX_PATH];
static unsigned presents;
static harness_cmd_fn handlers[32];
static int handler_count;
static long cmd_offset;

const char *harness_dir(void) {
    return dir;
}

void harness_reply(const char *fmt, ...) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\reply.txt", dir);
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "[%u] ", presents);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void harness_add_handler(harness_cmd_fn fn) {
    if (handler_count < (int)(sizeof handlers / sizeof handlers[0])) {
        handlers[handler_count++] = fn;
    } else {
        log_printf("harness: no room for another command handler");
    }
}

// ---- scripted input: per player a list of (word, frames), one step per presented frame ----
typedef struct {
    uint16_t word;
    int frames;
} Step;
typedef struct {
    Step steps[512];
    int count, pos, left;
    bool hold;  // keep the last word after the script ends
    bool active;
} Script;
static Script scripts[2];

static uint16_t parse_word(const char *tok, const char **end) {
    static const uint16_t dirs[10] = {0, 0x02 | 0x04, 0x02, 0x02 | 0x01, 0x04, 0, 0x01, 0x08 | 0x04, 0x08, 0x08 | 0x01};
    uint16_t w = 0;
    const char *p = tok;
    if (*p >= '1' && *p <= '9') {
        w |= dirs[*p - '0'];
        p++;
    }
    for (; *p && *p != '*'; p++) {
        switch (tolower((unsigned char)*p)) {
        case 'a': w |= 0x10; break;
        case 'b': w |= 0x20; break;
        case 'c': w |= 0x40; break;
        case 'd': w |= 0x80; break;
        case 's': w |= 0x4000; break;
        case 'x': w |= 0x8000; break;
        case '.': break;
        default: break;
        }
    }
    *end = p;
    return w;
}

static void cmd_input(char *args) {
    int player = atoi(args) - 1;
    if (player < 0 || player > 1) {
        harness_reply("input: player 1 or 2");
        return;
    }
    char *rest = strchr(args, ' ');
    Script *s = &scripts[player];
    if (!rest || strncmp(rest + 1, "off", 3) == 0) {
        s->active = false;
        frame_override_input(player, false, 0);
        harness_reply("input %d off", player + 1);
        return;
    }
    s->count = s->pos = s->left = 0;
    s->hold = false;
    for (char *tok = strtok(rest + 1, " "); tok; tok = strtok(NULL, " ")) {
        if (strcmp(tok, "hold") == 0) {
            s->hold = true;
            continue;
        }
        const char *end;
        uint16_t w = parse_word(tok, &end);
        int n = *end == '*' ? atoi(end + 1) : 1;
        if (s->count < (int)(sizeof s->steps / sizeof s->steps[0]) && n > 0) {
            s->steps[s->count++] = (Step){w, n};
        }
    }
    s->active = s->count > 0;
    int total = 0;
    for (int i = 0; i < s->count; i++) {
        total += s->steps[i].frames;
    }
    harness_reply("input %d queued %d steps, %d frames", player + 1, s->count, total);
}

static void step_scripts(void) {
    for (int p = 0; p < 2; p++) {
        Script *s = &scripts[p];
        if (!s->active) {
            continue;
        }
        if (s->left == 0) {
            if (s->pos >= s->count) {
                if (!s->hold) {
                    s->active = false;
                    frame_override_input(p, true, 0);
                    harness_reply("input done %d", p + 1);
                }
                continue;
            }
            s->left = s->steps[s->pos++].frames;
        }
        frame_override_input(p, true, s->steps[s->pos - 1].word);
        s->left--;
    }
}

// ---- delayed commands ----
typedef struct {
    unsigned at;
    char line[256];
} Delayed;
static Delayed delayed[16];

static void run_command(char *line);

static void cmd_after(char *args) {
    char *rest;
    unsigned n = (unsigned)strtoul(args, &rest, 0);
    while (*rest == ' ') {
        rest++;
    }
    for (int i = 0; i < (int)(sizeof delayed / sizeof delayed[0]); i++) {
        if (!delayed[i].line[0]) {
            delayed[i].at = presents + n;
            snprintf(delayed[i].line, sizeof delayed[i].line, "%s", rest);
            return;
        }
    }
    harness_reply("after: queue full");
}

static void run_delayed(void) {
    for (int i = 0; i < (int)(sizeof delayed / sizeof delayed[0]); i++) {
        if (delayed[i].line[0] && presents >= delayed[i].at) {
            char line[256];
            strcpy(line, delayed[i].line);
            delayed[i].line[0] = 0;
            run_command(line);
        }
    }
}

static void cmd_peek(char *args) {
    char *rest;
    uintptr_t addr = strtoul(args, &rest, 16);
    unsigned len = (unsigned)strtoul(rest, NULL, 0);
    if (!len) {
        len = 64;
    }
    if (len > 4096) {
        len = 4096;
    }
    if (IsBadReadPtr((void *)addr, len)) {
        harness_reply("peek %08x: unreadable", (unsigned)addr);
        return;
    }
    char line[3 * 32 + 32];
    for (unsigned off = 0; off < len; off += 32) {
        int n = 0;
        for (unsigned i = 0; i < 32 && off + i < len; i++) {
            n += snprintf(line + n, sizeof line - n, "%02x", ((uint8_t *)addr)[off + i]);
            if ((i & 3) == 3) {
                line[n++] = ' ';
                line[n] = 0;
            }
        }
        harness_reply("%08x: %s", (unsigned)(addr + off), line);
    }
}

static void cmd_poke(char *args) {
    char *rest;
    uintptr_t addr = strtoul(args, &rest, 16);
    while (*rest == ' ') {
        rest++;
    }
    uint8_t bytes[256];
    unsigned n = 0;
    for (char *p = rest; p[0] && p[1] && n < sizeof bytes; p += 2) {
        if (*p == ' ') {
            p--;
            continue;
        }
        char h[3] = {p[0], p[1], 0};
        bytes[n++] = (uint8_t)strtoul(h, NULL, 16);
    }
    DWORD old;
    VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)addr, bytes, n);
    VirtualProtect((void *)addr, n, old, &old);
    harness_reply("poke %08x %u bytes", (unsigned)addr, n);
}

static void run_command(char *line) {
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }
    char cmd[32];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < 31) {
        cmd[i] = line[i];
        i++;
    }
    cmd[i] = 0;
    char *args = line + i;
    while (*args == ' ') {
        args++;
    }
    void *app = frame_app();
    if (strcmp(cmd, "ping") == 0) {
        harness_reply("pong mode=%d ticks=%u", frame_mode(), frame_ticks());
    } else if (strcmp(cmd, "mode") == 0) {
        harness_reply("mode %d", frame_mode());
    } else if (strcmp(cmd, "input") == 0) {
        cmd_input(args);
    } else if (strcmp(cmd, "after") == 0) {
        cmd_after(args);
    } else if (strcmp(cmd, "peek") == 0) {
        cmd_peek(args);
    } else if (strcmp(cmd, "poke") == 0) {
        cmd_poke(args);
    } else if (strcmp(cmd, "shot") == 0) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s.bmp", dir, *args ? args : "shot");
        bool ok = app && d3d_screenshot(APP_PTR(app, APP_DEVICE), path);
        harness_reply("shot %s %s", path, ok ? "ok" : "FAILED");
    } else if (strcmp(cmd, "app") == 0) {
        uint8_t *sys = game_sys(app);
        harness_reply("app %p sys %p input %p scene %p battle %p rng %p net %p renderer %p holdrand %p", app, sys,
                      game_input(app), app ? APP_PTR(app, APP_SCENE) : NULL, game_battle(app), (void *)G_RNG,
                      sys ? PTR_PTR(sys, SYS_NET) : NULL, app ? APP_PTR(app, APP_RENDERER) : NULL,
                      (void *)frame_holdrand());
    } else if (strcmp(cmd, "stats") == 0) {
        HeapStats s = heap_stats();
        harness_reply("heap main %u KB (peak %u KB, %u allocs) battle %u KB (peak %u KB, %u allocs) leaked frees %u reallocs %u",
                      (unsigned)(s.main_used >> 10), (unsigned)(s.main_peak >> 10), s.main_allocs,
                      (unsigned)(s.battle_used >> 10), (unsigned)(s.battle_peak >> 10), s.battle_allocs, s.leaked_frees,
                      s.leaked_reallocs);
    } else if (strcmp(cmd, "quit") == 0) {
        harness_reply("quit");
        if (app) {
            PostMessageA((HWND)APP_PTR(app, APP_HWND), WM_CLOSE, 0, 0);
        }
    } else {
        for (int h = 0; h < handler_count; h++) {
            if (handlers[h](cmd, args)) {
                return;
            }
        }
        harness_reply("unknown command '%s'", cmd);
    }
}

static void poll_commands(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\cmd.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < cmd_offset) {
        cmd_offset = 0;  // file was recreated
    }
    if (size == cmd_offset) {
        fclose(f);
        return;
    }
    fseek(f, cmd_offset, SEEK_SET);
    static char line[32768];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n == 0 || line[n - 1] != '\n') {
            break;  // partial line: read it next time
        }
        cmd_offset += (long)n;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        run_command(line);
    }
    fclose(f);
}

static void on_present(void *app) {
    presents++;
    step_scripts();
    run_delayed();
    poll_commands();
}

void harness_install(void) {
    snprintf(dir, sizeof dir, "%s\\hinoroll", game_dir());
    CreateDirectoryA(dir, NULL);
    // Start fresh: old commands must not run again.
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\cmd.txt", dir);
    DeleteFileA(path);
    snprintf(path, sizeof path, "%s\\reply.txt", dir);
    DeleteFileA(path);
    frame_add_present_hook(on_present);
    analysis_install();
    hwbp_install();
    stress_install();
    quiet_install();
    pads_harness_install();
    padui_harness_install();
    heap_track_sites(true);
    frame_keep_active(true);
    log_printf("harness ready (%s)", dir);
}
