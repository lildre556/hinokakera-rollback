# Hinokakera rollback (GGPO) mod: working notes

- Game: Hinokakera Chaotic Eclipse ver.1.01 (Reddish Region, exe 2011-08-30).
  Pristine copy in `E:\Hinokakera\ce_vanilla` (from the user's
  hinokakeraCE.zip); test copies `E:\Hinokakera\ce_dev` (player 1) and
  `E:\Hinokakera\ce_dev2` (player 2; its net.txt uses port 10997 so both copies
  can run), big data files hardlinked. C: is nearly full: keep game copies and
  big outputs on E:.
- `main` is the Chaotic Eclipse version (hinoroll 2.x, release v2.0). Branch
  `v1` / release v1.1 is the fragment:Eclipse ver.3.00 version (old copies in
  `E:\Hinokakera\dev`, `dev2`, `vanilla`).
- README.md is the user's own short text: keep its voice, only update facts.
- The mod is `dinput8.dll` (proxy) + MinHook + vendored GGPO. Read
  docs/rollback.md first (engine facts, what the saved state is, what broke
  determinism and how it was fixed, the driver, display/audio).
- Decompilation: `E:\Hinokakera\re_ce\decomp\all.c` via `tools/re/fn.py
  [-nLINES] ADDR`, disassembly `tools/re/disasm.py ADDR [N]` (both default to
  the CE exe; HINO_DECOMP / HINO_EXE pick others). Rebuild with Ghidra 12.1.4
  (`E:\kof2002umdecompilation\kof2002um-decomp\tools\ghidra_12.1.4_PUBLIC`,
  JAVA_HOME = C:\Program Files\Java\jdk-23): project `E:\Hinokakera\re_ce`,
  import + `tools/ghidra_scripts/CreateFuncs.java
  E:\Hinokakera\re_ce\missing_funcs.txt` (vtable methods Ghidra misses) +
  `ExportDecomp.java`. `E:\Hinokakera\re_ce\xref.py ADDR` finds code
  referencing an address.

## Build

`tools/dev/build.sh [--deploy | --release]` (MSYS2 MINGW32 gcc for the 32-bit
game; cmake/ninja from mingw64). `--deploy` closes running copies and copies
the test DLL into ce_dev and ce_dev2. Release: `tools/dev/release.sh VERSION`.

## Testing (the harness, test builds only)

- `tools/dev/restart.sh [DIR] [args]` restarts a copy and waits for its first
  tick. Test builds mute the process's audio session, give the focus back, sit
  behind other windows, ignore the keyboard unless focused, open the netplay
  window unfocused and behind, and make "borderless" a 960x540 window instead
  of covering the monitor. The user works on this PC while tests run: never
  use computer-use, never go exclusive fullscreen; config.dat in the dev
  copies is windowed.
- `tools/dev/hino.sh` / `hino.py` (Game class): commands `ping`, `mode`, `app`,
  `input P script`, `shot NAME`, `peek/poke`, `stats`, `ww on|off|report`,
  `hwbp ADDR [LEN] [rw]`/`hwbp list`, `stress DEPTH EVERY`/`stress report`/
  `stress life N`, `sound on|off`, `pads`, `padfake DEV CONTROL MS` /
  `padfake focus 1|0`, `padui open|close|page N|row N|key
  enter|esc|del|tab|up|down|left|right` (one key per frame), `alttab
  focus|menu|min MS|lost MS`. `tools/dev/capture.py DIR OUT.png` captures a
  copy's window as shown (borderless checks).
- Offline rollback check: `stress 8 1`, then `to_battle.py [--chars A B]` and
  `ko_test.py N SEED` (whole battles, KOs, battle ends). Must report 0
  failures; "MISMATCH" lines in hinoroll.log show what differed.
- Online check: start ce_dev with `--netplay-direct 1 7001 127.0.0.1:7002 1`
  and ce_dev2 with `--netplay-direct 2 7002 127.0.0.1:7001 1` (restart.sh
  passes the args; prefix with `env 'ggpo.network.delay=80'` for latency),
  then `net_test.py BATTLES SEED` or `alttab_test.py BATTLES SEED`; they
  compare the per-frame "state frame" log lines of both copies (must be 0
  differ). `dialog_connect.py` drives two `--netplay` connection windows.

## Rules

- Any new state battle logic keeps outside the battle arena must be added to
  snapshot.c's regions; check with `ww` and the stress test.
- Battle logic must not create external resources without the journal
  (src/resources.c); Render must not change game state (src/effects.c);
  objects other threads write (the music stream) must not be in the battle
  arena.
- Saves must come in frame order (the journal): call snap_reset() before
  frame numbers start over.
- If the game says "device creation failed" (デバイスの作成に失敗), Direct3D
  couldn't make a device right then: retry later, it isn't the mod.
- Write C with the Write/Edit tools or a Python patch script file; bash heredocs
  eat backslashes.
- Gameplay-affecting changes: bump BUILD_ID in src/session.c so different
  builds refuse to play each other.
