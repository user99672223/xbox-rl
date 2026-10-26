// desktop_audio.cpp - real audio for the desktop test host.
//
// The console host's xrl_audio_ops (host/src/audio_ops.cpp, native XAudio2 plus per-voice
// buffer/level counters) is compiled into the test host unchanged; this file supplies what that
// file takes from the host project: the xrl::logf logger, routed into the test host's log.
// XRL_NO_AUDIO=1 in the environment keeps the old stub table (no sound device needed).
#include <windows.h>
#include <cstdarg>
#include <cstdio>

#include "xrl_host.h"

namespace {
xrl_log_fn g_log;
}

extern "C" void xrl_desktop_audio_set_log(xrl_log_fn log) { g_log = log; }

namespace xrl {
void logf(int level, const char* tag, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(level, tag, buf);
}
}  // namespace xrl
