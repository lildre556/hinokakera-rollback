# Hinokakera rollback: design and engine notes

Target: `hinokakera.exe` of Hinokakera Chaotic Eclipse ver.1.01 (2011-08-30,
1605632 bytes, PE timestamp 0x4e5cca86), 32-bit, Direct3D 9 (+ d3dx9_37),
DirectInput 8, DirectSound, statically linked VC6 **debug** CRT and MFC 4.2
(the game code itself is optimized). The mod is `dinput8.dll` (the game
imports `DirectInput8Create`; we forward it to the system DLL) and refuses to
hook any other build. Addresses are in src/game.h. hinoroll 1.x targeted
fragment:Eclipse ver.3.00 (D3D8, VC7.1 CRT); that code is on the `v1`
branch / the v1.1 release.

## The application

- The application object lives on WinMain's stack; `G_APP` (0x587e30) points
  at it (set in the base constructor, before anything runs). vtable 0x54dbdc:
  +0x04 MsgProc (0x412250), +0x1c game init (0x494f20: creates the system
  object, SetMode(3)), +0x34 frame (0x4124e0), +0x38 FrameMove (0x495030, one
  1/60 s logic tick), +0x3c Render (0x496630: BeginScene, scene render,
  EndScene, **Present**).
- WinMain reads config.dat (0x496830) into app+0x2b8b0 before creating the
  window; the netplay handshake runs right after (src/frame.c hk_load_config),
  so the host's settings are applied before anything copies them.
- The frame (0x4124e0): TestCooperativeLevel; DEVICELOST returns, NOTRESET
  resets (0x411ea0). Otherwise `(acc + 60) / FPS` FrameMoves (FPS app+0x2b890
  is the drawing rate; ticks are always 60/s), Render, timeGetTime/Sleep
  pacing. A driver (netplay) keeps ticking at 60 Hz while the device is lost.
- The message loop runs frames whenever `ready` (app+0x2b864) is set and
  `active` (app+0x2b860) or `background` (app+0x2b888, always 1 in this
  build): the game never stops when unfocused or minimized.
- F4: FrameMove reads it with GetAsyncKeyState and posts WM_SIZE
  SIZE_MAXIMIZED, which MsgProc turns into a device Reset into the other
  mode (0x4120c0). The maximize button does the same. The mod takes both over
  (src/window.c).
- Scenes: app+0x2b898, vtable +4 init, +8 update (1 = done), +0xc render. The
  mode is app+0x2b8a4: 3 title, 4 select, 5 VS screen, 6 battle, 7 after
  battle (arcade), 8 config, 9/10 record/replay, 13/14 story, 16 network.
  FrameMove calls NextMode (0x495120) when the update returns 1, which picks
  the next mode and calls SetMode (0x495bc0).
- The system object (app+0x2b894, 0x19a8 bytes): +0 Input, +4 Sound, +0x30
  replay recorder, +0x34 the game's own UDP netplay, +0x38 a copy of the
  config (0xc0 bytes), then match flow, records (from record.dat) and
  unlocks. Config words: 0 windowed, 1/2 size, 3 colour bits, 4 FPS, 5
  texture, 6 visual effects, 7 field effects, 8 SE volume, 9 BGM volume
  (0..100), 10 COM level, 11 speed, 12 win count, 13 time, 14/15 training
  options, then keys.
- The game's own netplay binds UDP 10996 (net.txt) at startup and fails to
  start if it can't: two copies on one PC need different ports (the dev2
  copy's net.txt says 10997).

## Input

Input object (sys+0): same layout as 3.00. Per player p and button b a record
at +0x224 + p*0xa0 + b*0x10: {held, previous, pressed edge, lockout}. The poll
(0x412ae0) shifts held to previous and counts lockouts down, then reads the
keyboard (DirectInput, foreground, non-exclusive), mouse and joysticks.
0x413430 sets a player's held state from a button word, 0x413250 reads it.
Button word: left 0x04, right 0x01, up 0x08, down 0x02, A..D 0x10..0x80,
Start 0x4000, 0x8000. Netplay injects both words through the poll hook and
reads the local devices separately (`frame_poll_local`, which restores the
records afterwards). The game's joystick enumeration (callback 0x412670) is
hooked off; src/pads.c reads controllers.

GetAsyncKeyState (Escape in titles and battles, F4) sees the keyboard even
unfocused and isn't synchronized: the mod answers only for the focused
window and never to logic under a driver; F4 becomes a request to
src/window.c.

## What a battle's state is

The CRT heap is replaced by two dlmalloc arenas in write-watched memory
(src/heap.c). The debug CRT funnels everything through five functions:
`_nh_malloc_dbg` 0x4a2580 (malloc, calloc, new...), `_realloc_dbg` 0x4a29a0,
`_expand_dbg` 0x4a2f20, `_free_dbg` 0x4a2f80, `_msize_dbg` 0x4a33c0. New
blocks are filled with 0xcd as the debug CRT does. Main-thread allocations
during battle logic (FrameMove in mode 6, SetMode(6)) go to the battle arena;
~13 MB is in use in a battle, almost all of it character data written once
while loading. Battle logic never frees or moves main-arena blocks (frees
are leaked, reallocs copied).

Saves are a journal (src/snapshot.c): a shadow copy of the arena as of the
last save, and per save the previous contents of the pages written since
(GetWriteWatch). Loading a frame restores the pages written since the last
save from the shadow, then undoes later saves newest first. A battle frame
writes ~200-300 pages (about 1 MB). Plus, outside the arena:
app+0x2b970 (ms accumulator, tick count), sys+0xf8..0x19a8 (match flow,
records, play time), the input records, the table RNG (`G_RNG` 0x587e38:
{count, table, index}), the CRT rand() seed (main thread _tiddata+0x14, TLS
index at 0x577020), the replay recorder (sys+0x30, 0x50 bytes), the
renderer's camera (renderer app+0x2b884: +0xd70 projection, +0xdb0 view,
+0xdf0 light, to +0xe20), and the logic copy of the device transforms. Found
with `ww` (write analysis); the stress test confirms.

## Things that broke determinism (all fixed)

- The music: each new track's OGG stream object (0x4994d0) is allocated while
  a battle loads and is filled by the decoder thread all the time; it lived
  in the battle arena. Music playback allocates in the main arena now.
- Sprites copy the camera's rotation (renderer+0xdb0) during logic (billboards,
  0x40b5f0): the renderer's camera is saved state.
- rand() is called by Render and by sound playback (CSound::Play 0x410a00
  picks a buffer with it): the seed is saved and restored around both; re-run
  frames play no sounds.
- Facing is decided from the view matrix read back from the device; battle
  logic gets its own copy of the transforms (src/resources.c).
- The texture cache (renderer +4 object, entries of 0x78 bytes: texture +0x1c,
  name +0x20, refcount +0x84; acquire 0x40bcb0, release 0x40cfa0) and
  Direct3D vertex buffers/textures are created and released by battle logic
  as effects come and go: src/resources.c journals them (releases deferred
  until their frame is confirmed, creations released when their frame is
  rolled back, textures kept loaded at zero references during a battle).
  Everything is D3DPOOL_MANAGED, so device Resets don't involve them.

## The netplay driver (src/netplay.c)

GGPO (pond3r/ggpo, MIT) vendored with small patches marked `hinoroll`.

- Battles: rollback. Snapshot statics in a 16-slot ring keyed by frame, the
  arena in the journal; GGPO's buffers are just frame numbers.
- Everything else, and a battle in its finishing state (battle+0x11c == 7,
  58+ frames before the scene changes): lockstep. If a battle still tries to
  end on an unconfirmed frame (battle update 0x47fdf0 returns 1), the end is
  held and the frame re-run when confirmed.
- Online: no pause (Start masked in battles), no attract demo, the title menu
  answers only VS Human, Training and Watch (cursor title+0x2c; others would
  read local files or records). At the start both games blank the records and
  unlock everything (sys+0x100 = 13 characters, 4 colours each) with default
  picks, since those come from each player's record.dat / story save.
  record.dat and config.dat aren't saved online.
- State checks: every 30th battle frame each side hashes, right after saving
  it, the tick count, input records, RNG index, rand seed, round counters and
  battle state/timer and both fighters' life (fighter+0x47c) and position
  (+0xa4); confirmed frames' hashes are exchanged. "DESYNC DETECTED" in the
  title on a mismatch.
- Session setup (src/session.c, src/stun.c): host's config words 5-7 and 10-15
  are applied on the guest; build id = mod version + hash of vsdat.dat,
  vsobj2.dat, nvdat.dat, hinokakera.exe.

## Alt-tab

Tested with `alttab_test.py` (two copies online, random focus loss messages,
the Alt menu loop, minimize/restore and device loss + Reset on both sides
during battles): no state differences. The game keeps ticking unfocused and
minimized (the background flag), the driver ticks through device loss, the
Reset path touches no game state (managed resources only). Borderless mode
(src/window.c) avoids device loss altogether.

## Display and audio (src/window.c, src/audio.c)

- Borderless: the device stays windowed at the game's resolution, the window
  becomes WS_POPUP over the monitor, and Present (hooked, vtable 17) gets a
  destination rectangle that keeps the shape; WM_PAINT/WM_ERASEBKGND paint
  the bars black. Exclusive fullscreen is the game's own switch (0x4120c0).
  Overlays draw in the Present hook (Render presents itself in this build).
- Volume: the game's own setters (sound 0x499650 SE, 0x499610 BGM, levels
  0..100, curve -((105-v)^2)/2 hundredths of a dB) are hooked to add the
  master level; the settings screen calls them and updates both config
  copies.

## Testing

- `stress DEPTH EVERY`: every EVERY battle frames roll back DEPTH frames
  through the journal, re-run them, and compare a full copy of the arena and
  statics with the original run (bytes Render writes are left out). On a
  mismatch the log shows the ranges, the block and allocation sites, and for
  freed memory the allocation that last held it.
- tools/dev/net_test.py / alttab_test.py with two copies started with
  --netplay-direct (optionally `ggpo.network.delay` for latency); they compare
  the per-frame state lines of both logs.
- `alttab focus|menu|min MS|lost MS` reproduces what alt-tabbing does without
  touching the screen.
