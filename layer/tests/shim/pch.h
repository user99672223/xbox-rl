// pch.h for the desktop test host: host/src/audio_ops.cpp (the host's real XAudio2 op table) is
// compiled into xrl_test_host, and it expects the UWP project's precompiled header. It only needs
// the Win32 basics.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
