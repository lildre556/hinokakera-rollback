#!/usr/bin/env bash
# Builds the release DLL (no test harness) and packages it:
#   dist/hinoroll-VERSION.zip          dinput8.dll, Hinokakera Netplay.bat, hinoroll-README.txt, hinoroll-LICENSES.txt
#   dist/hinoroll-VERSION-source.zip   the sources (no build output, no Ghidra project)
#   release.sh VERSION
set -euo pipefail
cd "$(dirname "$0")/../.."
VERSION="${1:?usage: release.sh VERSION}"
bash tools/dev/build.sh --release
OUT="dist/hinoroll-$VERSION"
rm -rf "$OUT"
mkdir -p "$OUT"
/c/msys64/mingw32/bin/strip -o "$OUT/dinput8.dll" build-release/dinput8.dll
sed 's/$//' "dist_files/Hinokakera Netplay.bat" > "$OUT/Hinokakera Netplay.bat"
sed "s/VERSION/$VERSION/" dist_files/README.txt | sed 's/$/\r/' > "$OUT/hinoroll-README.txt"
{
    echo "== GGPO =="; cat third_party/ggpo/LICENSE; echo
    echo "== MinHook =="; cat third_party/minhook/LICENSE.txt; echo
    echo "== dlmalloc =="; echo "dlmalloc 2.8.6 by Doug Lea, released to the public domain (CC0 1.0)."
} | sed 's/$/\r/' > "$OUT/hinoroll-LICENSES.txt"
.venv/Scripts/python - "$OUT" "$VERSION" <<'PY'
import os, sys, zipfile
out, version = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(f"dist/hinoroll-{version}.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(out)):
        z.write(os.path.join(out, name), name)
skip_dirs = {"build", "build-release", "dist", "ghidra", ".venv", "__pycache__", "dll_resources"}
with zipfile.ZipFile(f"dist/hinoroll-{version}-source.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in skip_dirs and not d.startswith(".")]
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.join(f"hinoroll-{version}-source", os.path.relpath(p, ".")))
print("dist/hinoroll-%s.zip" % version)
PY
ls -la dist/
