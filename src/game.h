#pragma once
// Addresses and layouts in hinokakera.exe ver.3.00 (2008-01-23, 913408 bytes).
// docs/engine.md explains them.

#include <stdint.h>

#define THISCALL __attribute__((thiscall))

// --- the application object (CD3DApplication subclass; lives on WinMain's stack) ---
#define APP_VTABLE 0x4bb480u
#define APP_VT_FRAMEMOVE 5          // +0x14: one logic tick
#define APP_VT_RENDER 6             // +0x18: draw the scene
#define ADDR_FRAMEMOVE 0x401a90u
#define ADDR_RENDER 0x4028c0u
#define ADDR_MSGPROC 0x42bde0u      // CD3DApplication::MsgProc(hwnd, msg, wparam, lparam)
#define ADDR_SET_MODE 0x403e30u     // app->SetMode(mode): builds the scene for a mode

#define APP_HWND 0x2a3cc
#define APP_DEVICE 0x2a3d4          // IDirect3DDevice8*
#define APP_ACTIVE 0x2a388          // m_bActive (cleared on WM_ACTIVATE inactive)
#define APP_PAUSED 0x2a604          // nonzero: FrameMove skips input
#define APP_FPS 0x2a518             // logic ticks per second (60; netvs' "Target FPS")
#define APP_INPUT 0x2a68c           // Input*
#define APP_SND 0x2a698             // sound/music manager
#define APP_RES 0x2a6a0             // resources
#define APP_SCENE_BATTLE 0x2a6a4    // Battle* while mode 6
#define APP_REPLAY 0x2a6cc
#define APP_MODE 0x2b990            // 3 title, 4 select, 5 vs screen, 6 battle, 7 after battle, ...
#define APP_TICK_MS 0x2b998         // 16

#define MODE_TITLE 3
#define MODE_SELECT 4
#define MODE_VS 5
#define MODE_BATTLE 6

// --- globals ---
#define G_NET (*(void **volatile *)0x4dc264)        // vanilla TCP netplay object (NULL offline)
#define G_RNG (*(uint32_t *volatile *)0x4dc260)     // {count, uint16 *table, index}
#define G_ARCHIVE (*(void **volatile *)0x4dc25c)

// --- input (Input object at app+APP_INPUT) ---
#define ADDR_INPUT_POLL 0x435a50u   // thiscall(Input*, int ms): shift held->prev, then read devices
#define ADDR_INPUT_GET 0x4361c0u    // thiscall(Input*, int player) -> button word
#define ADDR_INPUT_SET 0x4363a0u    // thiscall(Input*, int player, uint word): held + edges
// Per player p (0/1) and button b (0..9) a 16-byte record at +0x224 + p*0xa0 + b*0x10:
// {held, prev, pressed edge, lockout ms}. The poll shifts held->prev and counts the lockout down.
#define INPUT_REC 0x224
#define INPUT_REC_SIZE 0x140
#define INPUT_BUTTONS 10
// Button word bits (FUN_004361c0): rec0 4, rec1 1, rec2 8, rec3 2, rec4..7 0x10..0x80, rec8 0x4000, rec9 0x8000.
#define INPUT_WORD_MASK 0xc0ffu

// --- battle scene ---
#define ADDR_BATTLE_UPDATE 0x415c90u  // thiscall(Battle*, Input*) -> 1 when the battle is over
#define ADDR_TITLE_UPDATE 0x475c90u   // title menu; greys out Config/Record while G_NET is set

// --- CRT heap (VC7.1 static CRT) ---
#define ADDR_HEAP_ALLOC 0x4a0fb0u   // _heap_alloc(size)
#define ADDR_FREE 0x4a0ffeu         // free(p)
#define ADDR_REALLOC 0x4a85ebu      // realloc(p, size)
#define ADDR_CALLOC 0x4a322fu       // calloc(n, size)
#define ADDR_MSIZE 0x4a24ccu        // _msize(p)
#define ADDR_EXPAND 0x4a2511u       // _expand(p, size)

// --- image ---
#define IMAGE_DATA_START 0x4cb000u  // .data
#define IMAGE_DATA_END 0x4e1a7cu    // end of .data (incl. .bss)
#define IMAGE_DATA1_START 0x4e2000u // .data1
#define IMAGE_DATA1_END 0x4e2900u

static inline uint8_t *app_ptr(void *app, unsigned off) {
    return (uint8_t *)app + off;
}
#define APP_U32(app, off) (*(volatile uint32_t *)app_ptr((app), (off)))
#define APP_PTR(app, off) (*(void *volatile *)app_ptr((app), (off)))
