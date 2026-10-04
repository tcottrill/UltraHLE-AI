# UltraHLE for Linux (SteamOS). Windows builds with Scripts/UltraHLE.sln.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Scripts/linux/build.sh does this in WSL, off /mnt/c. The core is the same
# source the Windows build uses; src/linux/ is the Linux front end, partly
# taken from AAE (src/linux/aae/: X11+GLX window, PulseAudio, evdev input).

cmake_minimum_required(VERSION 3.20)
project(UltraHLE C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)        # gnu11
set(CMAKE_CXX_STANDARD 17)

# The emulator core, as in the Windows project (x64: no recompiler).
set(UHLE_CORE
    src/boot.c src/cart.c src/cmd.cpp src/cpu.c src/cpuc.c src/cpucache.c
    src/cpustub.c src/debugui.c src/disasm.c src/disasm86.c src/dlist.c
    src/flash.c src/hw.c src/inifile.c src/lle.c src/log.c src/main.c
    src/mem.c src/musyx.c src/os.c src/pad.c src/patch.c src/pif.c src/rdp.c
    src/rdp_soft.c src/rsp.c src/rsp_cxd4.c src/slist.c src/sym.c src/sync.c
    src/timer.c src/zlist.c
    src/xgl/xgl_fbo.c src/xgl/xgl_geom.c src/xgl/xgl_main.c
    src/xgl/xgl_shader.c src/xgl/xgl_state.c src/xgl/xgl_tex.c
    src/zip/miniz.c src/zip/romzip.c
)

# Linux front end: what the Windows GUI provides there (ui.h, console.h,
# sound.h, padin.h, xgl_ctx_*, bootlog.h)
file(GLOB UHLE_LINUX src/linux/*.c src/linux/*.cpp src/linux/aae/*.cpp)

add_executable(ultrahle ${UHLE_CORE} ${UHLE_LINUX})

target_include_directories(ultrahle PRIVATE src src/xgl src/linux src/linux/aae)
target_compile_definitions(ultrahle PRIVATE
    $<$<CONFIG:Release>:RELEASE=1 NDEBUG>)

# MSVC semantics the core was written against: no strict aliasing (structs
# are read through dword pointers), wrapping signed overflow, and common
# tentative definitions (some headers define globals).
target_compile_options(ultrahle PRIVATE
    -msse4.1 -fno-strict-aliasing -fwrapv -w
    $<$<COMPILE_LANGUAGE:C>:-fcommon>)

# K&R style source (implicit int)
set_source_files_properties(src/disasm86.c PROPERTIES COMPILE_OPTIONS -std=gnu89)
# the R4300 FPU changes the host rounding mode (fenv_access on MSVC)
set_source_files_properties(src/cpuc.c PROPERTIES COMPILE_OPTIONS -frounding-math)

find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)
find_library(PULSE_SIMPLE pulse-simple REQUIRED)
find_library(PULSE pulse REQUIRED)
find_path(PULSE_INCLUDE pulse/simple.h REQUIRED)
target_include_directories(ultrahle PRIVATE ${PULSE_INCLUDE})

target_link_libraries(ultrahle PRIVATE
    GLEW::GLEW OpenGL::GL X11::X11 Threads::Threads
    ${PULSE_SIMPLE} ${PULSE} ${CMAKE_DL_LIBS} m)

# next to ULTRA.INI, as on Windows
set_target_properties(ultrahle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build/linux)
