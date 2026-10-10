#!/usr/bin/env bash
# Restarts the game in a dev folder and waits until the harness answers.
#   restart.sh [game_dir] [extra args for hinokakera.exe]
set -euo pipefail
DIR="${1:-${HINO_DIR:-/e/Hinokakera/ce_dev}}"
shift || true
WINPY="$(dirname "$0")/../../.venv/Scripts/python"
# Close only the instance running from this folder.
"$WINPY" - "$DIR" <<'PY'
import subprocess, sys, os
d = os.path.normcase(os.path.abspath(sys.argv[1].replace("/e/", "E:/")))
out = subprocess.run(["powershell", "-NoProfile", "-Command",
    "Get-Process hinokakera -ErrorAction SilentlyContinue | ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
    capture_output=True, text=True).stdout
for line in out.splitlines():
    if "|" in line:
        pid, path = line.split("|", 1)
        if os.path.normcase(os.path.dirname(path)) == d:
            subprocess.run(["taskkill", "/F", "/PID", pid], capture_output=True)
PY
sleep 0.5
rm -f "$DIR/hinoroll/cmd.txt" "$DIR/hinoroll/reply.txt"
(cd "$DIR" && ./hinokakera.exe "$@" >/dev/null 2>&1 &)
for _ in $(seq 1 100); do
    sleep 0.3
    if [ -f "$DIR/hinoroll.log" ] && grep -q "first tick" "$DIR/hinoroll.log"; then
        echo "started $DIR"
        exit 0
    fi
done
echo "game didn't start" >&2
exit 1
