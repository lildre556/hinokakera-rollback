# hinoroll: GGPO rollback netcode for Hinokakera fragment:Eclipse

> **Made entirely with generative AI.** Every line of the mod's code, the
> reverse engineering behind it, the test tools and this README were produced
> by an AI model (Anthropic's Claude, working in Claude Code) following a
> user's prompts. No human programmer wrote or reviewed the code.
> **It may be buggy.** It has been tested a lot on one PC (details below), but
> never in a real online match between two PCs. Use it at your own risk and keep
> a backup of your game folder.

hinoroll adds [GGPO](https://github.com/pond3r/ggpo) rollback netplay to
*Hinokakera fragment:Eclipse* ver.3.00, a 2008 doujin 3D fighting game by
Reddish Region. The game shipped with only an experimental TCP lockstep
netplay (netvs.exe) that needed port forwarding and stuttered with latency.

This repository has no game files and no decompiled game code. You need your
own copy of the game. It isn't affiliated with or endorsed by Reddish Region.

## Features

- **Rollback battles.** Your inputs show up at once. When the opponent's
  inputs arrive, the game rewinds and re-simulates (GGPO). Up to 8 frames of
  rollback, with adjustable input delay (Auto picks it from the ping).
- **Lockstep menus.** Title, character select, loading and the end of a battle
  wait for both players' inputs, so scene changes never get rolled back.
- **Friend codes.** A connection window shows a `hino-XXXXX-...` code built
  from your public address (found with STUN) and your LAN address. Both
  players paste each other's code and press Connect; UDP hole punching does
  the rest. `IP:port` works too, for port forwarding.
- **Desync check.** Every 30 battle frames both sides compare a hash of the
  match state. The window title shows ping, delay, rollback frames per second,
  and "DESYNC DETECTED" if the games ever disagree.
- **Controller setup (F8).** An in-game screen for binding XInput (Xbox-style)
  and DirectInput pads, D-pad, sticks and triggers included. The original game
  only read a pad's analog stick and five buttons. A+D and C+D shortcuts.
- Offline play keeps working as before; the mod only changes how the game
  reads controllers and a few internals listed below.

## Using it

Build `dinput8.dll` (below) or take it from a release zip if one is posted, and copy it and
`Hinokakera Netplay.bat` next to `hinokakera.exe`. Start the .bat to play
online and `hinokakera.exe` for offline. Both players need the same mod
version and the same game files.
[dist_files/README.txt](dist_files/README.txt) is the players' guide (it ships
in the zip as `hinoroll-README.txt`).

## How it was built

This was one long Claude Code session. Starting from a `.rar` of the game,
the AI planned the approach, reverse engineered the game, wrote the mod and
the tools to test it, and fixed what the tests turned up. The steps:

### 1. Reverse engineering

- `hinokakera.exe` is a 32-bit Windows program: Direct3D 8, DirectInput 8,
  DirectSound, with the Visual C++ 2003 runtime and MFC built in. It was loaded
  into **Ghidra**, run headless with scripts in `tools/ghidra_scripts`. One
  script creates the ~430 virtual methods Ghidra misses; another exports all
  ~3,800 decompiled functions to a text file for searching (not published).
- The game loop is the DirectX SDK's `CD3DApplication` framework.
  `FrameMove` (one 16 ms logic tick) and `Render` are separate virtual
  methods. That clean split is what made rollback possible.
- The old netplay showed where inputs enter the game: a per-player 16-bit
  button word set through one function. That's where the mod feeds in GGPO's
  inputs.

### 2. Hooking in

The mod is a `dinput8.dll` proxy. The game imports DirectInput, so Windows
loads this DLL from the game folder; it forwards the real calls and patches the
game with [MinHook](https://github.com/TsudaKageyu/minhook) and vtable patches.
It only hooks the exact ver.3.00 executable.

### 3. Making the game state saveable

Rollback needs a save and load of the whole game state every frame.
- **The heap.** The game's C runtime heap was replaced with two
  [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) arenas. Everything
  battle logic allocates goes to a "battle arena" (~1.5 MB). Restoring that
  arena, allocator bookkeeping included, also undoes allocations and frees.
- **The rest of the state.** The few places outside the arena that battle
  logic writes were found with a write analysis (byte diffs of memory around
  every tick): match state in the application object, both players' input
  records, the game's table RNG, the C runtime `rand()` seed, the replay index
  and the camera.
- **Resource journal.** Battle logic creates and frees Direct3D vertex buffers
  and textures as effects come and go. A journal defers releases until their
  frame is confirmed, and frees resources whose frame was rolled back.

### 4. Finding what broke determinism

A **stress test** rolls the battle back 8 frames on every frame, re-runs the
frames, and compares the result byte for byte with the original run. On a
mismatch it reports the first frame that differs and an allocation trace.
That turned up:

- **`rand()` side effects.** Rendering and the sound player called `rand()`,
  so the random sequence depended on how often frames were drawn. Fix: their
  calls no longer advance the seed.
- **Facing used the GPU's camera.** Which way a fighter faces was read back
  from the Direct3D view matrix, which isn't part of the saved state. Fix:
  battle logic gets its own copy of the transforms, saved with the state.
- **Draw-time state changes.** Three effect types changed their lifetime in
  their draw function. Fix: that work moved into their logic update.
- **Uneven scratch allocations.** Sound playback and texture loading made
  scratch allocations that a re-run doesn't. Fix: those allocations moved out
  of the battle arena.
- **Unfocused keyboard reads.** The game read Escape and F4 from the keyboard
  even when unfocused, outside the synchronized input. Fix: they're ignored
  under netplay and when unfocused.

### 5. Netplay

GGPO is vendored with small patches (marked `hinoroll`):
- a "confirmed frame" query
- a shared socket, so NAT hole punching and GGPO use one port
- no crash on send errors
- a fix for lockstep frames
- logged asserts

Battles run with rollback. Menus, and battles in their final 58+ frames, run
lockstep, so a scene change always happens on a confirmed frame. The host's
match settings are copied to the guest at startup, as the original netplay did.

### 6. Testing

- **Stress test:** about 14,000 depth-8 rollbacks across whole battles (KOs,
  rounds, battle ends) and several character pairs, with 0 mismatches after
  the fixes.
- **Two copies over GGPO on one PC:** full battles, including simulated
  120–150 ms ping, which means about 45–50 re-simulated frames per second. The
  per-frame state logs of both players were identical.
- **Harness:** a test-only command channel (`tools/dev`) for scripted inputs,
  screenshots, memory reads, hardware breakpoints and fake controller presses.

## Building

Requirements: Windows, [MSYS2](https://www.msys2.org/) with
`mingw-w64-i686-gcc` (the game is 32-bit) and CMake/Ninja (from mingw64), and
Python 3 for the tools.

```bash
bash tools/dev/build.sh            # test build (with the harness) -> build/dinput8.dll
bash tools/dev/build.sh --release  # release build -> build-release/dinput8.dll
bash tools/dev/release.sh 1.1      # release zip + source zip in dist/
```

[CLAUDE.md](CLAUDE.md) holds the AI's working notes: workflow, test commands
and rules. [docs/rollback.md](docs/rollback.md) has the engine facts and the
design in detail.

## Layout

| Path | What |
|------|------|
| `src/` | the mod (C) |
| `third_party/ggpo` | GGPO, upstream commit `7ddadef` with the hinoroll patches |
| `third_party/minhook`, `third_party/dlmalloc` | MinHook, dlmalloc 2.8.6 |
| `tools/dev` | harness clients, stress/netplay tests, build and release scripts |
| `tools/re`, `tools/ghidra_scripts` | reverse engineering helpers |
| `dist_files` | files shipped in the release zip |

## Known limitations

- Never tested between two real PCs over the internet. Hole punching
  depends on both routers. If it fails, one player forwards UDP 7000.
- No pause in online battles (the pause menu's Exit would end the match).
  Escape doesn't quit online; close the window instead.
- Menus run lockstep, so they have input lag equal to the ping.
- Status (ping, rollback, desync) is shown in the window title, so it isn't
  visible in fullscreen.
- Only ver.3.00 of the game is supported.

## License

There is no license for this mod's own code. Third-party code keeps its
licenses: GGPO (MIT), MinHook (BSD 2-clause), dlmalloc (public domain / CC0).
*Hinokakera* is © Reddish Region.
