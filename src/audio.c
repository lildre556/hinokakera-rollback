// Volume levels for the settings screen: music (BGM) and effects (SE) are
// the game's own config values (0..100, the same numbers its Config menu
// shows), applied through the game's own setters; master scales both on top.
// Saved in hinoroll.ini [audio] (and the game's config, which it saves to
// config.dat from its Config menu). Not game state: nothing here is part of
// the netplay sync.
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <windows.h>

#include "frame.h"
#include "game.h"
#include "hooks.h"
#include "log.h"

typedef int(THISCALL *set_volume_fn)(void *snd, int level);
typedef void(THISCALL *bgm_set_db_fn)(void *bgm, int db);
static set_volume_fn orig_set_se, orig_set_bgm;
static const bgm_set_db_fn bgm_set_db = (bgm_set_db_fn)0x40fdf0u;

static int levels[AUDIO_KINDS] = {100, 100, 100};
static bool have_ini[AUDIO_KINDS];
static char ini_path[MAX_PATH];
static const char *keys[AUDIO_KINDS] = {"master", "music", "effects"};

// The game's curve: level 0..100 -> hundredths of a dB.
static int game_db(int level) {
    return -(((105 - level) * (105 - level)) / 2);
}

static int master_db(void) {
    int m = levels[AUDIO_MASTER];
    if (m <= 0) {
        return -10000;
    }
    return (int)lround(2000.0 * log10(m / 100.0));
}

static int clamp_db(int db) {
    return db < -10000 ? -10000 : db > 0 ? 0 : db;
}

static void save(int kind) {
    char v[16];
    snprintf(v, sizeof v, "%d", levels[kind]);
    WritePrivateProfileStringA("audio", keys[kind], v, ini_path);
}

// sound+0: the DirectSound wrapper (+0x18: the effects volume every SE plays at).
static int THISCALL hk_set_se(void *snd, int level) {
    int r = orig_set_se(snd, level);
    void *ds = PTR_PTR(snd, 0);
    if (ds) {
        PTR_U32(ds, 0x18) = (uint32_t)clamp_db(game_db(level) + master_db());
    }
    if (levels[AUDIO_EFFECTS] != level) {
        levels[AUDIO_EFFECTS] = level;
        save(AUDIO_EFFECTS);
    }
    return r;
}

// sound+8: the music player (also called for every new track).
static int THISCALL hk_set_bgm(void *snd, int level) {
    int r = orig_set_bgm(snd, level);
    void *bgm = PTR_PTR(snd, 8);
    if (bgm) {
        bgm_set_db(bgm, clamp_db(game_db(level) + master_db()));
    }
    if (levels[AUDIO_MUSIC] != level) {
        levels[AUDIO_MUSIC] = level;
        save(AUDIO_MUSIC);
    }
    return r;
}

int audio_level(int kind) {
    return kind >= 0 && kind < AUDIO_KINDS ? levels[kind] : 0;
}

static void *sound(void) {
    uint8_t *sys = game_sys(frame_app());
    return sys ? PTR_PTR(sys, SYS_SOUND) : NULL;
}

// Main thread (the settings screen).
void audio_set_level(int kind, int level) {
    if (kind < 0 || kind >= AUDIO_KINDS) {
        return;
    }
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    levels[kind] = level;
    save(kind);
    void *app = frame_app();
    uint8_t *sys = game_sys(app);
    if (kind == AUDIO_EFFECTS || kind == AUDIO_MUSIC) {
        int word = kind == AUDIO_EFFECTS ? CFG_SE_VOLUME : CFG_BGM_VOLUME;
        if (app) {
            APP_U32(app, APP_CONFIG + word * 4) = (uint32_t)level;
        }
        if (sys) {
            PTR_U32(sys, SYS_CONFIG + word * 4) = (uint32_t)level;
        }
    }
    void *snd = sound();
    if (snd) {
        if (kind != AUDIO_MUSIC) {
            hk_set_se(snd, levels[AUDIO_EFFECTS]);
        }
        if (kind != AUDIO_EFFECTS) {
            hk_set_bgm(snd, levels[AUDIO_MUSIC]);
        }
    }
}

// config.dat was read: the levels saved here win (the game copies them into
// its sound object when that is made).
void audio_config_loaded(void *app) {
    snprintf(ini_path, sizeof ini_path, "%s\\hinoroll.ini", game_dir());
    for (int k = 0; k < AUDIO_KINDS; k++) {
        int v = (int)GetPrivateProfileIntA("audio", keys[k], -1, ini_path);
        have_ini[k] = v >= 0;
        if (have_ini[k]) {
            levels[k] = v > 100 ? 100 : v;
        }
    }
    if (have_ini[AUDIO_EFFECTS]) {
        APP_U32(app, APP_CONFIG + CFG_SE_VOLUME * 4) = (uint32_t)levels[AUDIO_EFFECTS];
    } else {
        levels[AUDIO_EFFECTS] = (int)APP_U32(app, APP_CONFIG + CFG_SE_VOLUME * 4);
    }
    if (have_ini[AUDIO_MUSIC]) {
        APP_U32(app, APP_CONFIG + CFG_BGM_VOLUME * 4) = (uint32_t)levels[AUDIO_MUSIC];
    } else {
        levels[AUDIO_MUSIC] = (int)APP_U32(app, APP_CONFIG + CFG_BGM_VOLUME * 4);
    }
    log_printf("audio: master %d, music %d, effects %d", levels[AUDIO_MASTER], levels[AUDIO_MUSIC],
               levels[AUDIO_EFFECTS]);
}

void audio_install(void) {
    hook_code(ADDR_SND_SET_SE_VOLUME, (void *)hk_set_se, (void **)&orig_set_se, "SE volume");
    hook_code(ADDR_SND_SET_BGM_VOLUME, (void *)hk_set_bgm, (void **)&orig_set_bgm, "BGM volume");
}
