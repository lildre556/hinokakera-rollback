#pragma once
// Addresses and layouts in hinokakera.exe of Hinokakera Chaotic Eclipse ver.1.01
// (2011-08-30, 1605632 bytes, PE timestamp 0x4e5cca86). docs/rollback.md explains them.

#include <stdint.h>

#define THISCALL __attribute__((thiscall))

#define GAME_TIMESTAMP 0x4e5cca86u

// --- the application object (lives on WinMain's stack; G_APP points at it) ---
#define G_APP (*(void *volatile *)0x587e30)
#define APP_VTABLE 0x54dbdcu
#define APP_VT_MSGPROC 1            // +0x04: MsgProc(hwnd, msg, wparam, lparam)
#define APP_VT_GAME_INIT 7          // +0x1c: creates the system object, SetMode(3)
#define APP_VT_FRAME 13             // +0x34: one frame (FrameMoves, Render, pacing)
#define APP_VT_FRAMEMOVE 14         // +0x38: one logic tick
#define APP_VT_RENDER 15            // +0x3c: draw the scene and Present
#define ADDR_MSGPROC 0x412250u
#define ADDR_FRAME 0x4124e0u
#define ADDR_FRAMEMOVE 0x495030u
#define ADDR_RENDER 0x496630u
#define ADDR_LOAD_CONFIG 0x496830u  // thiscall-ish fastcall(app): reads config.dat (before the window exists)
#define ADDR_SET_MODE 0x495bc0u     // thiscall(app, mode): builds the scene for a mode
#define ADDR_RESET_DEVICE 0x411ea0u // fastcall(app): device Reset (after loss or a mode switch)
#define ADDR_TOGGLE_FULLSCREEN 0x4120c0u  // fastcall(app): F4 (WM_SIZE SIZE_MAXIMIZED from FrameMove)

#define APP_HWND 0x8
#define APP_WINDOWED 0xc            // 1 windowed, 0 fullscreen
#define APP_WINDOW_RECT 0x14        // RECT of the window in windowed mode
#define APP_WIDTH 0x9c
#define APP_HEIGHT 0xa0
#define APP_DEVICE 0x2b820          // IDirect3DDevice9*
#define APP_ACTIVE 0x2b860          // cleared on WM_ACTIVATE inactive / minimize
#define APP_READY 0x2b864           // frames run while set
#define APP_RENDERER 0x2b884        // renderer wrapper (0x1dcc bytes; camera, texture cache...)
#define APP_BACKGROUND 0x2b888      // run while inactive (always 1 in this build)
#define APP_FPS 0x2b890             // drawn frames per second (config); ticks are always 60/s
#define APP_SYS 0x2b894             // System*: input, sound, replay, vanilla netplay, match flow
#define APP_SCENE 0x2b898           // the current scene (vtable: +4 init, +8 update, +0xc render)
#define APP_MODE 0x2b8a4            // 3 title, 4 select, 5 VS screen, 6 battle, 7 after battle, 8 config...
#define APP_CONFIG 0x2b8b0          // config.dat as loaded (0xc0 bytes; the system object keeps a copy)
#define APP_MS_ACC 0x2b970          // float: ms accumulator (FrameMove)
#define APP_TICKS 0x2b974           // FrameMove count
#define APP_SIZE 0x2b97c

#define MODE_TITLE 3
#define MODE_SELECT 4
#define MODE_VS 5
#define MODE_BATTLE 6
#define MODE_CONFIG 8

// --- the system object (app+APP_SYS, 0x19a8 bytes) ---
#define SYS_INPUT 0x0               // Input*
#define SYS_SOUND 0x4               // Sound*
#define SYS_REPLAY 0x30             // replay recorder (0x50 bytes)
#define SYS_NET 0x34                // the game's own netplay (0x37a18 bytes); +0x370d8 connected
#define SYS_CONFIG 0x38             // live copy of the config (0xc0 bytes)
#define SYS_SIZE 0x19a8
// Config words (APP_CONFIG / SYS_CONFIG): 0 windowed, 1 width, 2 height, 3 colour bits, 4 FPS,
// 5 texture, 6 visual effects, 7 field effects, 8 SE volume, 9 BGM volume (0..100), 10 COM level,
// 11 speed, 12 win count, 13 time, 14..15 cut-in/blood flags, 16.. keys and pad buttons.
#define CFG_SE_VOLUME 8
#define CFG_BGM_VOLUME 9

// --- globals ---
#define G_ARCHIVE (*(void *volatile *)0x587e34)
#define G_RNG (*(uint32_t *volatile *)0x587e38)     // {count, uint16 *table, index}
#define G_CRT_TLS_INDEX 0x577020u                   // the C runtime's TLS slot (_tiddata; +0x14 rand seed)

// --- input (Input object at sys+SYS_INPUT) ---
#define ADDR_INPUT_POLL 0x412ae0u   // thiscall(Input*, int ms): shift held->prev, then read devices
#define ADDR_INPUT_GET 0x413250u    // thiscall(Input*, int player) -> button word
#define ADDR_INPUT_SET 0x413430u    // thiscall(Input*, int player, uint word): held + edges
#define ADDR_INPUT_INIT 0x4127d0u   // thiscall(Input*, hwnd, hinst, exclusive, foreground): DirectInput devices
#define ADDR_SYS_TICK_INPUT 0x499c90u  // thiscall(System*, ms): poll + the game's own netplay exchange
// Per player p (0/1) and button b (0..9) a 16-byte record at +0x224 + p*0xa0 + b*0x10:
// {held, prev, pressed edge, lockout ms}. The poll shifts held->prev and counts the lockout down.
#define INPUT_REC 0x224
#define INPUT_REC_SIZE 0x140
#define INPUT_BUTTONS 10
// Button word bits (FUN_00413250): rec0 4, rec1 1, rec2 8, rec3 2, rec4..7 0x10..0x80, rec8 0x4000, rec9 0x8000.
#define INPUT_WORD_MASK 0xc0ffu
#define INPUT_PAD_MAP 0x38c         // per player (+0x40): the game's joystick button numbers
#define ADDR_GAME_ENUM_JOYSTICK 0x412670u  // the game's EnumDevices callback

// --- scenes ---
#define ADDR_BATTLE_UPDATE 0x47fdf0u  // thiscall(Battle*) -> 1 when the battle is over
#define BATTLE_VTABLE 0x54d9e8u
#define BATTLE_STATE 0x11c            // 4 fighting, 7 finishing (58+ frames before the scene ends)
#define BATTLE_FIGHTER 0x28           // Fighter* [2]
#define FIGHTER_LIFE 0x47c
#define FIGHTER_POS 0xa4              // float[3]
#define ADDR_TITLE_UPDATE 0x492b90u   // thiscall(Title*)
#define TITLE_STATE 0x10              // 1 press start, 2 menu, 8 leaving
#define TITLE_IDLE 0x14               // frames without input (attract demo after 600)
#define TITLE_ITEM 0x2c               // menu cursor
#define ADDR_SAVE_RECORDS 0x49a0b0u   // fastcall(System*): writes record.dat
#define ADDR_SAVE_CONFIG 0x4966b0u    // fastcall(app): writes config.dat (Config menu, leaving Training)

// --- sound (Sound object at sys+SYS_SOUND) ---
#define ADDR_SND_SET_SE_VOLUME 0x499650u   // thiscall(Sound*, 0..100)
#define ADDR_SND_SET_BGM_VOLUME 0x499610u  // thiscall(Sound*, 0..100)
#define ADDR_BGM_PLAY 0x4994d0u      // thiscall(Sound*, track, loop): new OGG stream (decoded on its own thread)
#define ADDR_CSOUND_PLAY 0x410a00u   // thiscall(CSound*, prio, flags, volume, freq, pan) (DX SDK CSound)
#define ADDR_CSOUND_STOP 0x410af0u   // thiscall(CSound*)

// --- renderer: texture cache (999 entries of 0x78 bytes; index 1..count) ---
#define ADDR_TEXCACHE_ACQUIRE 0x40bcb0u  // thiscall(cache, uint *out_idx, const char *name, int flag) -> HRESULT
#define ADDR_TEXCACHE_RELEASE 0x40cfa0u  // thiscall(cache, int idx)
#define TEX_ENTRY_SIZE 0x78
#define TEX_PTR_OFF 0x1c
#define TEX_REFS_OFF 0x84

// --- misc ---
#define ADDR_GAME_MSG 0x413e30u     // cdecl(show, error, text): the game's message/log function
#define IAT_GETASYNCKEYSTATE 0x54a958u
#define IAT_DIRECTSOUNDCREATE8 0x54a0ccu
#define IAT_FINDWINDOWA 0x54a9b4u

// --- CRT heap (VC6 static debug CRT) ---
#define ADDR_NH_MALLOC_DBG 0x4a2580u  // _nh_malloc_dbg(size, nhFlag, blockType, file, line)
#define ADDR_REALLOC_DBG 0x4a29a0u    // _realloc_dbg(p, size, blockType, file, line)
#define ADDR_EXPAND_DBG 0x4a2f20u     // _expand_dbg(p, size, blockType, file, line)
#define ADDR_FREE_DBG 0x4a2f80u       // _free_dbg(p, blockType)
#define ADDR_MSIZE_DBG 0x4a33c0u      // _msize_dbg(p, blockType)

// --- image ---
#define IMAGE_TEXT_START 0x401000u
#define IMAGE_TEXT_END 0x54a000u
#define IMAGE_DATA_START 0x566000u  // .data (incl. .bss)
#define IMAGE_DATA_END 0x58b584u

static inline uint8_t *app_ptr(void *app, unsigned off) {
    return (uint8_t *)app + off;
}
#define APP_U32(app, off) (*(volatile uint32_t *)app_ptr((app), (off)))
#define APP_PTR(app, off) (*(void *volatile *)app_ptr((app), (off)))
#define PTR_U32(p, off) (*(volatile uint32_t *)((uint8_t *)(p) + (off)))
#define PTR_PTR(p, off) (*(void *volatile *)((uint8_t *)(p) + (off)))

static inline uint8_t *game_sys(void *app) {
    return app ? (uint8_t *)APP_PTR(app, APP_SYS) : NULL;
}

static inline uint8_t *game_input(void *app) {
    uint8_t *sys = game_sys(app);
    return sys ? (uint8_t *)PTR_PTR(sys, SYS_INPUT) : NULL;
}

// The battle scene while one runs (mode 6), else NULL.
static inline uint8_t *game_battle(void *app) {
    if (!app || APP_U32(app, APP_MODE) != MODE_BATTLE) {
        return NULL;
    }
    uint8_t *scene = APP_PTR(app, APP_SCENE);
    return scene && PTR_U32(scene, 0) == BATTLE_VTABLE ? scene : NULL;
}
