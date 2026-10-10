# Hinokakera rollback: design and engine notes

Target: `hinokakera.exe` ver.3.00 (2008-01-23, 913408 bytes, PE timestamp
0x4797503b), 32-bit, Direct3D 8, DirectInput 8, DirectSound, statically linked
VC7.1 CRT + MFC. The mod is `dinput8.dll` (the game imports
`DirectInput8Create`; we forward it to the system DLL) and refuses to hook any
other build.

## The game loop

- The application object (a `CD3DApplication` subclass, 0x2b9b4 bytes) lives on
  WinMain's stack (0x0017453c in every run so far). vtable 0x4bb480:
  +0xc init (0x401180), +0x14 FrameMove (0x401a90, one logic tick),
  +0x18 Render (0x4028c0).
- Render3DEnvironment (0x42caa0) runs `(acc + 60) / FPS` FrameMoves per drawn
  frame (FPS = app+0x2a518 from config; 60 = one each), renders, paces with
  timeGetTime/Sleep, presents. When the device is lost it returns without
  doing anything (the loop spins): src/frame.c keeps a netplay driver ticking.
- Inactive window: WM_ACTIVATE clears app+0x2a388 and the loop blocks in
  GetMessage. Netplay and test builds keep it set.
- FrameMove: if not paused, polls input (0x435a50), then (vanilla TCP netplay,
  `G_NET` 0x4dc264) exchanges inputs; then the scene for the mode
  app+0x2b990 (3 title, 4 select, 5 VS screen, 6 battle, 7 after battle,
  11 logo, 99 quit). SetMode (0x403e30) builds the next scene (loads data,
  creates textures).
- The game quits at startup if a window of its class exists (FindWindowA in
  0x42b170); test builds patch that out to run two copies.

## Input

Input object at app+0x2a68c. Per player p (0/1) and button b (0..9) a record
at +0x224 + p*0xa0 + b*0x10: {held, previous, triggered, lockout ms}. The poll
first moves held to previous and counts lockouts down by `ms`
((16*60)/FPS), then ORs in the devices and recomputes "triggered". 0x4363a0
sets a player's held state from a button word; 0x4361c0 reads it back.
Button word: left 0x04, right 0x01, up 0x08, down 0x02, A..D 0x10..0x80,
Start 0x4000, 0x8000. Netplay injects both players' words through the poll
hook (src/frame.c `frame_run`) and reads the local devices separately
(`frame_poll_local`, which restores the records afterwards).

GetAsyncKeyState (Escape quits battles and the game, F4 toggles fullscreen)
reads the keyboard even unfocused and isn't synchronized: the mod answers
only for the focused window and never to logic under a driver (F4 excepted).

## What a battle's state is

The CRT heap (malloc/free/realloc/calloc/_msize/_expand at 0x4a0fb0/0x4a0ffe/
0x4a85eb/0x4a322f/0x4a24cc/0x4a2511) is replaced by two dlmalloc arenas in
write-watched memory (src/heap.c). Main-thread allocations during battle logic
(FrameMove in mode 6, SetMode(6)) go to the battle arena; ~1.5 MB is in use in
a battle. The big assets live in Direct3D, not the heap. Battle logic never
frees or moves main-arena blocks (frees are leaked, reallocs copied).

A snapshot (src/snapshot.c) = the battle arena up to its top chunk (allocator
bookkeeping included, so restoring undoes allocations and frees) plus:
app+0x2a6d0..0x2b9b4 (match flow, timers), the input records, the table RNG
(G_RNG 0x4dc260: {count, table, index}), the CRT rand() seed (main thread
_tiddata+0x14, TLS index at 0x4dac40), the replay recorder (app+0x2a6cc,
index +0x3c), the renderer wrapper's camera (app+0x2a698 +0x2040..0x20d0),
0x4dc268 (effect manager), and the logic copy of the device transforms.
Found with the `ww` write analysis (byte diffs of .data, the app object and the
main arena around every tick; Render writes into the battle arena are vertex
scratch and harmless to restore).

## Things that broke determinism (all fixed)

- rand() is called by Render (0x43c4d0) and by the sound player's buffer pick
  (0x48c040): the seed is saved and restored around both.
- Sound playback and texture loading allocate scratch: routed to the main
  arena, so a re-run (which skips playback and finds textures cached) leaves
  the battle arena identical.
- Facing is decided from the view matrix read back with
  `device->GetTransform(VIEW)`; the camera is set at the end of the tick. The
  device's matrix isn't saved state, so battle logic gets its own copy
  (SetTransform/GetTransform hooks in src/resources.c, reset at SetMode(6)).
- Three effect classes change state in their draw function (vtables 0x4bd708
  falling sparks, 0x4bcd94 and 0x4bcefc keep-alive): moved into their update
  (src/effects.c).
- The texture cache (app+0x2a698 -> +4, 999 entries of 0x78 bytes, refcount at
  +0x88) and Direct3D vertex/index buffers and textures are created and
  released by battle logic as effects come and go (sprites: ctor 0x432880 makes
  a managed VB and takes a texture; 0x432810 frees them). src/resources.c
  journals them during battle logic: releases are deferred until their frame
  is confirmed, creations are released when their frame is rolled back,
  texture loads are kept at zero references until the battle ends.

## The netplay driver (src/netplay.c)

GGPO (pond3r/ggpo, MIT) vendored with small patches marked `hinoroll`:
confirmed-frame query, a handed-in socket and raw-packet filter (codes/NAT
punching and the mod's own packets share the port), no assert on sendto
errors, don't discard the frame not yet simulated (lockstep waits), DllMain
removed, assertions logged.

- Battles: rollback. Snapshots in a 16-slot ring keyed by frame; GGPO's buffers
  are just frame numbers.
- Everything else, and a battle in its finishing state (battle+0x68 == 7,
  58+ frames before the scene changes): lockstep, i.e. a frame only runs once
  both players' inputs are confirmed. A scene change never gets rolled back.
  If a battle still tries to end on an unconfirmed frame, the end is held and
  the frame re-run when confirmed.
- No pause menu online (Start masked in battles): its Exit ends the battle at
  once.
- No attract demo online.
- State checks: every 30th confirmed battle frame both sides hash the match
  state, input records, both RNGs, the replay index and both fighters' life and
  position and compare; the title bar shows "DESYNC DETECTED" on a mismatch.
  app+0x2b99c differs per machine and isn't read by logic: left out.
- Session setup (src/session.c, src/stun.c) runs in the app's init hook, before
  the game copies its settings, like the old netvs: the host's FPS, effects
  level, speed, COM level, life, win count and last picks are applied on the
  guest. A build id (mod version + hash of vsdat.dat, vsobj2.dat, nvdat.dat,
  hinokakera.exe) must match. Codes are base32 of the STUN-mapped public
  endpoint and the LAN endpoint; both sides send hellos to both until one
  answers (NAT hole punching).

## Testing

- `stress DEPTH EVERY`: every EVERY battle frames roll back DEPTH frames,
  re-run them, compare with the original run (bytes Render writes are left
  out), per-frame first difference and allocation traces on failure.
- tools/dev/net_test.py with two copies started with --netplay-direct
  (optionally `ggpo.network.delay` / `ggpo.oop.percent` environment variables
  for simulated latency); it compares the per-frame state lines of both logs.

## Controllers (src/pads.c, src/padui.c)

The game's own joystick reading (FUN_00435a50) knows only stick axes (+-500)
and five buttons per pad (input+0x38c + player*0x40: A, B, C, D, Pause, spare),
no POV hat, no XInput. The mod hooks the game's EnumDevices callback
(0x4355e0) so the game finds no controllers, and reads them itself: XInput
(xinput1_4, loaded on first use: never in DllMain) and DirectInput game
controllers through the system dinput8 (devices whose path has "IG_" are
XInput pads and skipped), DISCL_BACKGROUND with input only while the game
window has the focus. Each player has a device (saved by key: "xinput0" or
the DirectInput instance GUID) and bindings per device kind (xinput,
dinput-VIDPID) in hinoroll.ini [pads]. The word is ORed into the game's input
in the poll hook (offline) or frame_poll_local (netplay, local player).

The setup screen is a GDI-painted panel uploaded to a managed A8R8G8B8 texture
and drawn after Render with a textured quad; only the render/texture-stage
states it changes are saved and restored (a D3DSBT_ALL state block per frame
dropped the game to ~51 fps). Offline the game doesn't tick while it's open;
Escape is kept from the game while it's open and just after.
Harness: `pads` (devices, assignments, live words), `padfake DEV CONTROL MS`
and `padfake focus 1|0`, `padui open|close|row N|key NAME`.
