# Hinokakera rollback (GGPO) mod: working notes

- Game: Hinokakera fragment:Eclipse ver.3.00 (Reddish Region). Pristine copy in
  `E:\Hinokakera\vanilla`; test copies `E:\Hinokakera\dev` (player 1) and
  `E:\Hinokakera\dev2` (player 2), big data files hardlinked. C: is nearly
  full: keep game copies and big outputs on E:.
- The mod is `dinput8.dll` (proxy) + MinHook + vendored GGPO. Read
  docs/rollback.md first (engine facts, what the saved state is, what broke
  determinism and how it was fixed, the driver).
- Decompilation: `ghidra/decomp/all.c` via `tools/re/fn.py ADDR`,
  disassembly `tools/re/disasm.py ADDR [N]`. Rebuild ghidra/ with Ghidra 12.1.4
  (`E:\kof2002umdecompilation\kof2002um-decomp\tools\ghidra_12.1.4_PUBLIC`,
  JAVA_HOME = C:\Program Files\Java\jdk-23): import + `tools/ghidra_scripts/
  CreateFuncs.java ghidra/missing_funcs.txt` (vtable methods Ghidra misses) +
  `ExportDecomp.java`.

## Build

`tools/dev/build.sh [--deploy | --release]` (MSYS2 MINGW32 gcc for the 32-bit
game; cmake/ninja from mingw64). `--deploy` closes running copies and copies
the test DLL into dev and dev2. Release: `tools/dev/release.sh VERSION`.

## Testing (the harness, test builds only)

- `tools/dev/restart.sh [DIR] [args]` restarts a copy and waits for its first
  tick. Test builds are muted, give the focus back, sit behind other windows,
  ignore the keyboard unless focused, and allow two copies at once. The user
  works on this PC while tests run: never use computer-use; config.dat in the
  dev copies is windowed (fullscreen took over the user's screen once).
- `tools/dev/hino.sh` / `hino.py` (Game class): commands `ping`, `mode`, `app`,
  `input P script`, `shot NAME`, `peek/poke`, `stats`, `ww on|off|report`,
  `hwbp ADDR [LEN] [rw]`/`hwbp list`, `stress DEPTH EVERY`/`stress report`/
  `stress life N`, `sound on|off`, `pads`, `padfake DEV CONTROL MS` /
  `padfake focus 1|0` (controller input without touching the user's pad;
  release harness `input P off` first, a finished script leaves an override),
  `padui open|close|row N|key enter|esc|del|up|down|left|right`.
- Offline rollback check: `stress 8 1`, then `to_battle.py --chars A B` and
  `ko_test.py N SEED` (whole battles, KOs, battle ends). Must report 0
  failures; "MISMATCH" lines in hinoroll.log show what differed (first frame
  that differs, allocation traces).
- Online check: start dev with `--netplay-direct 1 7001 127.0.0.1:7002 1` and
  dev2 with `--netplay-direct 2 7002 127.0.0.1:7001 1` (restart.sh passes the
  args; prefix with `env 'ggpo.network.delay=80'` for latency), then
  `net_test.py BATTLES SEED`; it compares the per-frame "state frame" log lines
  of both copies (must be 0 differ). `dialog_connect.py` drives two `--netplay`
  connection windows.

## Rules

- Any new state battle logic keeps outside the battle arena must be added to
  snapshot.c's regions; check with `ww` and the stress test.
- Battle logic must not create external resources without the journal
  (src/resources.c); Render must not change game state (src/effects.c shows the
  fix pattern).
- If the game says "device creation failed" (デバイスの作成に失敗), Direct3D
  couldn't make a device right then (seen while the user changed display
  settings): retry later, it isn't the mod.
- Write C with the Write/Edit tools or a Python patch script file; bash heredocs
  eat backslashes.
- Gameplay-affecting changes: bump BUILD_ID in src/session.c so different
  builds refuse to play each other.
