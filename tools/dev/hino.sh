#!/usr/bin/env bash
# Sends harness commands to a running game and prints the replies.
#   hino.sh "shot title" "input 1 a*2"        send commands, wait for one reply each
#   hino.sh --wait "input done 1" "input 1 6*30"   wait for a specific reply instead
#   hino.sh --png NAME                        convert hinoroll/NAME.bmp to NAME.png
# HINO_DIR picks the game folder (default /e/Hinokakera/dev).
set -euo pipefail
GAME_DIR="${HINO_DIR:-/e/Hinokakera/dev}"
H="$GAME_DIR/hinoroll"
PY="$(dirname "$0")/../../.venv/Scripts/python"
if [ "${1:-}" = "--png" ]; then
    "$PY" -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$H/$2.bmp" "$H/$2.png"
    echo "$H/$2.png"
    exit 0
fi
want=""
timeout=600
if [ "${1:-}" = "--wait" ]; then
    want="$2"
    shift 2
fi
if [ "${1:-}" = "--timeout" ]; then
    timeout=$(( $2 * 10 ))
    shift 2
fi
mkdir -p "$H"
before=$(wc -l < "$H/reply.txt" 2>/dev/null || echo 0)
printf '%s\n' "$@" >> "$H/cmd.txt"
expect=$#
for _ in $(seq 1 $timeout); do
    sleep 0.1
    now=$(wc -l < "$H/reply.txt" 2>/dev/null || echo 0)
    if [ -n "$want" ]; then
        if tail -n +$((before + 1)) "$H/reply.txt" 2>/dev/null | grep -q "$want"; then break; fi
    elif [ $((now - before)) -ge $expect ]; then
        break
    fi
done
tail -n +$((before + 1)) "$H/reply.txt" 2>/dev/null || true
