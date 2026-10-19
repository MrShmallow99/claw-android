cmake_minimum_required(VERSION 3.18)
project(gameport C)

set(CMAKE_C_STANDARD 11)
# GAME selects games/<GAME>/game.ini (compiled in); GEN_DIR is codegen.py's output.
set(GAME "claw" CACHE STRING "game profile under games/")
set(GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gen CACHE PATH "generated C (recomp/codegen.py output)")
set(GAME_INI ${CMAKE_CURRENT_SOURCE_DIR}/games/${GAME}/game.ini)
if(NOT EXISTS ${GAME_INI})
    message(FATAL_ERROR "no ${GAME_INI}")
endif()
# Embed game.ini as a C array (re-run when it changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAME_INI})
file(READ ${GAME_INI} GAME_INI_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," GAME_INI_HEX "${GAME_INI_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gamecfg_embedded.h
     "/* generated from games/${GAME}/game.ini by CMake */\nstatic const char g_game_ini[] = {${GAME_INI_HEX}0};\n")
file(GLOB GEN_SOURCES ${GEN_DIR}/code_*.c)
# LZMA SDK encoder (public domain) for the on-device packer (runtime/pack/packer.c), single-threaded
set(LZMA_ENC_SOURCES third_party/lzma-enc/LzmaEnc.c third_party/lzma-enc/LzFind.c third_party/lzma-enc/CpuArch.c)
set_source_files_properties(${LZMA_ENC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "Z7_ST")
set(RUNTIME_SOURCES
    runtime/main.c runtime/gamecfg.c runtime/profiler.c runtime/cpu.c runtime/memory.c runtime/input.c
    runtime/platform_sdl.c runtime/overlay.c runtime/gamepad.c runtime/cheatmenu.c runtime/widescreen.c
    runtime/win32/kernel32.c runtime/win32/kernel32_ext.c runtime/win32/resource.c runtime/win32/advapi32.c
    runtime/win32/user32.c runtime/win32/user32_dlg.c runtime/win32/gdi32.c runtime/win32/fonts_embed.c
    runtime/win32/winmm.c runtime/win32/modules.c runtime/win32/dsound.c runtime/win32/mci.c runtime/win32/music.c runtime/win32/ddraw.c
    runtime/win32/mss32.c runtime/win32/stubs_misc.c runtime/win32/avi.c runtime/xmidi.c
    runtime/lua/lua_bridge.c runtime/lua/crazyhook.c runtime/win32/smack.c runtime/movie.c runtime/movie_audio.c
    runtime/pack/vfs.c runtime/pack/edlrec.c runtime/pack/drflac_impl.c third_party/lzma/LzmaDec.c
    runtime/pack/packer.c ${LZMA_ENC_SOURCES}
    ${GEN_DIR}/table.c ${GEN_DIR}/patches.c ${GEN_DIR}/stubs.c)

# The generated code is huge and mechanical: optimise it, skip warnings.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")
set_source_files_properties(runtime/win32/music.c runtime/win32/ddraw.c runtime/pack/drflac_impl.c third_party/lzma/LzmaDec.c
    runtime/xmidi.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(runtime/win32/fonts_embed.c PROPERTIES
    COMPILE_DEFINITIONS "FONT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LiberationSans-Regular.ttf")

if(ANDROID)
    # SDL2 is built from source alongside the game.
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/SDL2-2.30.9 sdl2)
    add_library(main SHARED ${RUNTIME_SOURCES} ${GEN_SOURCES})
    target_link_libraries(main SDL2::SDL2 m log)
    # drop unused code and merge identical functions (no effect on behaviour)
    target_compile_options(main PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(main PRIVATE -Wl,--gc-sections -Wl,--icf=all)
    set(TARGET main)
else()
    # desktop build of the game (development and testing): needs the SDL2 dev files
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
    if(SDL2_FOUND AND GEN_SOURCES)
        add_executable(gameport ${RUNTIME_SOURCES} ${GEN_SOURCES})
        target_include_directories(gameport PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(gameport ${SDL2_LINK_LIBRARIES} m pthread)
        target_link_options(gameport PRIVATE -rdynamic)
        set(TARGET gameport)
    else()
        message(STATUS "desktop game build (gameport) skipped: needs SDL2 dev files and gen/")
    endif()
endif()
if(ANDROID AND ANDROID_ABI STREQUAL "x86_64")
    set(ABI_SUFFIX "-android-x86_64")
elseif(ANDROID)
    set(ABI_SUFFIX "-android")
else()
    set(ABI_SUFFIX "-host")
endif()
set(LUAJIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build-luajit${ABI_SUFFIX}/src)
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg${ABI_SUFFIX})
if(DEFINED TARGET)
target_include_directories(${TARGET} PRIVATE runtime ${GEN_DIR} ${CMAKE_CURRENT_BINARY_DIR})
# LuaJIT with the guest-memory FFI (tools/build_luajit.sh host|android)
target_include_directories(${TARGET} PRIVATE ${LUAJIT_DIR})
target_link_libraries(${TARGET} ${LUAJIT_DIR}/libluajit.a dl)
# minimal FFmpeg for the movies and the packer's FLAC (tools/build_ffmpeg.sh host|android)
target_include_directories(${TARGET} PRIVATE ${FFMPEG_DIR}/include)
target_compile_definitions(${TARGET} PRIVATE PORT_HAVE_FFMPEG)   # FLAC in the on-device packer
target_link_libraries(${TARGET} ${FFMPEG_DIR}/lib/libavformat.a ${FFMPEG_DIR}/lib/libavcodec.a
    ${FFMPEG_DIR}/lib/libswscale.a ${FFMPEG_DIR}/lib/libswresample.a ${FFMPEG_DIR}/lib/libavutil.a m)
target_compile_options(${TARGET} PRIVATE -fno-strict-aliasing)
endif()

# desktop: port-supplied data beside the binary (platform_read_port_file)
if(NOT ANDROID AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/soundfont/GeneralUser-GS.sf2)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/port_data)
  file(CREATE_LINK ${CMAKE_SOURCE_DIR}/third_party/soundfont/GeneralUser-GS.sf2
       ${CMAKE_BINARY_DIR}/port_data/soundfont.sf2 SYMBOLIC)
endif()

# desktop: tools/devicepack, the on-device packer as a command (tests it against mkpack/packverify)
if(NOT ANDROID)
    add_executable(devicepack tools/devicepack.c runtime/pack/packer.c runtime/pack/vfs.c runtime/pack/edlrec.c
        runtime/pack/drflac_impl.c runtime/gamecfg.c third_party/lzma/LzmaDec.c ${LZMA_ENC_SOURCES})
    target_include_directories(devicepack PRIVATE runtime ${CMAKE_CURRENT_BINARY_DIR})
    set_source_files_properties(runtime/pack/drflac_impl.c third_party/lzma/LzmaDec.c PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(devicepack pthread m)
    if(EXISTS ${FFMPEG_DIR}/lib/libavcodec.a)
        target_compile_definitions(devicepack PRIVATE PORT_HAVE_FFMPEG)
        target_include_directories(devicepack PRIVATE ${FFMPEG_DIR}/include)
        target_link_libraries(devicepack ${FFMPEG_DIR}/lib/libavcodec.a ${FFMPEG_DIR}/lib/libswresample.a ${FFMPEG_DIR}/lib/libavutil.a m pthread)
    endif()
endif()
