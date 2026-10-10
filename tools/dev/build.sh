#!/usr/bin/env bash
# Builds dinput8.dll with MSYS2 MINGW32 (the game is 32-bit).
#   build.sh                 build only
#   build.sh --deploy        also copy it into the dev game folders (closes running copies)
#   build.sh --release       build the harness-free DLL into build-release/
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH="/c/msys64/mingw32/bin:/c/msys64/mingw64/bin:$PATH"
BUILD=build
HARNESS=ON
if [ "${1:-}" = "--release" ]; then
    BUILD=build-release
    HARNESS=OFF
fi
if [ ! -f $BUILD/build.ninja ]; then
    /c/msys64/mingw64/bin/cmake -S . -B $BUILD -G Ninja \
        -DCMAKE_C_COMPILER=/c/msys64/mingw32/bin/gcc.exe \
        -DCMAKE_CXX_COMPILER=/c/msys64/mingw32/bin/g++.exe \
        -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe \
        -DCMAKE_BUILD_TYPE=Release -DHINOROLL_HARNESS=$HARNESS >/dev/null
fi
/c/msys64/mingw64/bin/cmake --build $BUILD
if [ "${1:-}" = "--deploy" ]; then
    taskkill //F //IM hinokakera.exe >/dev/null 2>&1 || true
    sleep 0.5
    for d in ${HINO_DEPLOY_DIRS:-/e/Hinokakera/ce_dev /e/Hinokakera/ce_dev2}; do
        [ -d "$d" ] && cp $BUILD/dinput8.dll "$d/dinput8.dll" && echo "deployed to $d"
    done
fi
