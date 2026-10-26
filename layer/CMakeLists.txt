# layer/ - Workstream C: ELF loader + Linux-syscall backend on Win32.
#
#   cmake -S layer -B layer/build/desktop -G Ninja            # desktop Win32 flavour (+ test host)
#   cmake -S layer -B layer/build/uwp -G Ninja -DXRL_UWP=ON   # UWP / Xbox flavour (what host/ links)
#
# Use scripts/build.ps1, which sets up the VS2022 environment first.
cmake_minimum_required(VERSION 3.20)
project(xrl_layer C)

option(XRL_UWP "Compile for the UWP app container (WINAPI_FAMILY_APP)" OFF)
option(XRL_BUILD_TESTS "Build the desktop test host" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")   # /MD (UWP requires the dynamic CRT)

set(XRL_SRC
    src/boot.c src/com.c src/dbt.c src/elf.c src/exec.c src/fd.c src/futex.c src/guestctx.c src/ipc.c src/mem.c src/misc.c src/ntsync.c
    src/cpuinfo.c src/hangdump.c src/net.c src/perf.c src/procfs.c src/rewrite.c src/rewrite_core.c src/signal.c src/syscall.c src/sysnames.c src/teb.c src/thread.c src/unix.c
    src/util.c src/vfs.c src/x86dec.c
    )

# MASM thunks: assembled by hand (CMake's ASM_MASM + Ninja mangles rule names here)
set(THUNK_OBJ ${CMAKE_CURRENT_BINARY_DIR}/thunks.obj)
add_custom_command(OUTPUT ${THUNK_OBJ}
    COMMAND ml64 /nologo /c /Zi /Fo${THUNK_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/asm/thunks.asm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/asm/thunks.asm VERBATIM)
set(RAWSYS_OBJ ${CMAKE_CURRENT_BINARY_DIR}/rawsys.obj)
add_custom_command(OUTPUT ${RAWSYS_OBJ}
    COMMAND ml64 /nologo /c /Zi /Fo${RAWSYS_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/asm/rawsys.asm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/asm/rawsys.asm VERBATIM)
set(DBT_OBJ ${CMAKE_CURRENT_BINARY_DIR}/dbt.obj)
add_custom_command(OUTPUT ${DBT_OBJ}
    COMMAND ml64 /nologo /c /Zi /Fo${DBT_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/asm/dbt.asm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/asm/dbt.asm VERBATIM)
add_library(xrl_layer STATIC ${XRL_SRC} ${THUNK_OBJ} ${RAWSYS_OBJ} ${DBT_OBJ})
set_source_files_properties(${THUNK_OBJ} ${RAWSYS_OBJ} ${DBT_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_include_directories(xrl_layer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include PRIVATE src)
target_compile_definitions(xrl_layer PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(xrl_layer PRIVATE /W3 /utf-8 /Zi /Gy $<$<COMPILE_LANGUAGE:C>:/std:c11>)
if(XRL_UWP)
    target_compile_definitions(xrl_layer PRIVATE WINAPI_FAMILY=WINAPI_FAMILY_APP _WIN32_WINNT=0x0A00 XRL_UWP=1)
    target_link_libraries(xrl_layer PUBLIC WindowsApp bcrypt synchronization)   # WindowsApp.lib exports Winsock too
else()
    target_compile_definitions(xrl_layer PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(xrl_layer PUBLIC bcrypt synchronization ws2_32)
endif()

if(XRL_BUILD_TESTS AND NOT XRL_UWP)
    # gfx/ in its desktop mode, for XRL_REAL_GFX=1 (tests/desktop_gfx.cpp)
    enable_language(CXX)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gfx ${CMAKE_CURRENT_BINARY_DIR}/gfx EXCLUDE_FROM_ALL)
    # host/src/audio_ops.cpp (XAudio2) unchanged, with a stand-in for the UWP project's pch.h
    add_executable(xrl_test_host tests/test_host.c tests/desktop_gfx.cpp tests/desktop_audio.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../host/src/audio_ops.cpp)
    target_include_directories(xrl_test_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                               ${CMAKE_CURRENT_SOURCE_DIR}/tests/shim ${CMAKE_CURRENT_SOURCE_DIR}/../host/src)
    target_compile_definitions(xrl_test_host PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../host/src/audio_ops.cpp PROPERTIES COMPILE_OPTIONS "/EHsc;/utf-8")
    target_link_libraries(xrl_test_host PRIVATE xrl_layer xrl_gfx user32 xaudio2)
    add_executable(xrl_catch tests/catch.c)
    target_compile_definitions(xrl_catch PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(xrl_catch PRIVATE dbghelp)
endif()
