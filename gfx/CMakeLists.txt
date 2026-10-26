# gfx/ — Workstream D. Static library `xrl_gfx` + desktop test harness.
#
# Desktop (dev-PC) build, harness included:
#   cmake -S gfx -B gfx/build-desktop -G "Visual Studio 17 2022" -A x64
#   cmake --build gfx/build-desktop --config Release
#
# UWP-family build (what the host links on the console; library only):
#   cmake -S gfx -B gfx/build-uwp -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0
#   cmake --build gfx/build-uwp --config Release
#
# See gfx/build.ps1 which runs both. The host (a UWP .vcxproj) may instead just
# add gfx/src/*.cpp + gfx/include to its project; see gfx/notes-D.md.
cmake_minimum_required(VERSION 3.21)
project(xrl_gfx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XRL_GFX_SOURCES
    src/core.cpp
    src/device.cpp
    src/caps.cpp
    src/swapchain.cpp
    src/d3d11_swapchain.cpp
    src/d3d11on12.cpp
    src/stub12.cpp
    src/ops.cpp
    src/selftest.cpp)

add_library(xrl_gfx STATIC ${XRL_GFX_SOURCES})
target_include_directories(xrl_gfx
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../include   # xrl_abi.h (frozen, not ours)
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(xrl_gfx PRIVATE UNICODE _UNICODE)
target_compile_options(xrl_gfx PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(xrl_gfx PUBLIC d3d12 dxgi d3d11)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  # A static lib is not link-checked. This DLL pulls every object through the
  # real UWP linker (WindowsApp.lib + /APPCONTAINER), so a successful build proves
  # every import (D3D12CreateDevice, D3D11On12CreateDevice, CreateDXGIFactory2,
  # CoInitializeEx, ...) resolves against the UWP-allowed API set. It is a build
  # check only; the host links the static lib (or compiles gfx/src/*.cpp in).
  add_library(xrl_gfx_uwp_linkcheck SHARED ${XRL_GFX_SOURCES})
  target_include_directories(xrl_gfx_uwp_linkcheck
      PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../include
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(xrl_gfx_uwp_linkcheck PRIVATE UNICODE _UNICODE XRL_GFX_SHARED XRL_GFX_BUILDING)
  target_compile_options(xrl_gfx_uwp_linkcheck PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(xrl_gfx_uwp_linkcheck PRIVATE d3d12 dxgi d3d11)
else()
  target_link_libraries(xrl_gfx PUBLIC ole32)
  add_executable(xrl_gfx_harness harness/desktop_harness.cpp)
  target_link_libraries(xrl_gfx_harness PRIVATE xrl_gfx)
  target_compile_options(xrl_gfx_harness PRIVATE /W4 /permissive- /EHsc)
endif()
