#pragma once

// Volume levels 0..100 (src/audio.c).
enum { AUDIO_MASTER, AUDIO_MUSIC, AUDIO_EFFECTS, AUDIO_KINDS };

void audio_install(void);
void audio_config_loaded(void *app);
int audio_level(int kind);
void audio_set_level(int kind, int level);
