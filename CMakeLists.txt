cmake_minimum_required(VERSION 3.20)
project(hinoroll C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

# Hinokakera is a 32-bit (x86) game: build with MSYS2 MINGW32 (tools/dev/build.sh).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "build with the 32-bit MinGW toolchain (C:/msys64/mingw32/bin)")
endif()

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# GGPO (pond3r/ggpo, MIT), with the small additions marked "hinoroll" in its sources.
file(GLOB GGPO_SRC
    third_party/ggpo/src/lib/ggpo/*.cpp
    third_party/ggpo/src/lib/ggpo/backends/*.cpp
    third_party/ggpo/src/lib/ggpo/network/*.cpp
)
list(FILTER GGPO_SRC EXCLUDE REGEX "platform_linux\\.cpp$")
add_library(ggpo STATIC ${GGPO_SRC})
target_include_directories(ggpo PUBLIC third_party/ggpo/src/include PRIVATE third_party/ggpo/src/lib/ggpo)
target_compile_definitions(ggpo PRIVATE _WINDOWS WIN32_LEAN_AND_MEAN)
target_compile_options(ggpo PRIVATE -O2 -g -Wno-unknown-pragmas -Wno-format -Wno-sign-compare -fpermissive)

# The test harness (scripted input, screenshots, memory tools, analysis) is for
# development; release builds leave it out.
option(HINOROLL_HARNESS "Build the test harness into the DLL" ON)

# The mod ships as dinput8.dll: the game imports DirectInput8Create, so Windows
# loads our copy from the game folder and we forward the call to the system one.
add_library(hinoroll SHARED
    src/dllmain.c
    src/log.c
    src/crash.c
    src/heap.c
    src/hooks.c
    src/frame.c
    src/snapshot.c
    src/resources.c
    src/effects.c
    src/pads.c
    src/padui.c
    src/netplay.c
    src/session.c
    src/stun.c
    src/overlay.c
    src/window.c
    src/audio.c
    src/dinput8.def
)
if(HINOROLL_HARNESS)
    target_sources(hinoroll PRIVATE src/harness.c src/d3d.c src/analysis.c src/hwbp.c src/stress.c src/quiet.c)
    target_compile_definitions(hinoroll PRIVATE HINOROLL_HARNESS)
endif()
target_include_directories(hinoroll PRIVATE src third_party/dlmalloc)
set_target_properties(hinoroll PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
# dinput8/dxguid only for c_dfDIJoystick and GUIDs: the system DirectInput8Create is
# loaded at run time (this DLL is dinput8.dll itself).
target_link_libraries(hinoroll PRIVATE minhook ggpo ws2_32 winmm iphlpapi dinput8 dxguid ole32)
target_link_options(hinoroll PRIVATE -static -static-libgcc -static-libstdc++)
# -fno-indirect-inlining: calls into engine code at fixed addresses through
# inlined helpers must not be treated as unreachable (see the KOF mod notes).
target_compile_options(hinoroll PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
                       -Wno-cast-function-type -O2 -g -fno-indirect-inlining -fno-strict-aliasing)
