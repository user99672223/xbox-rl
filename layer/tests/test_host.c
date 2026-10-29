/*
 * test_host.c - desktop (Win32) harness that plays the role of host/ (workstream A):
 * fills an xrl_boot_context, provides log + stub op tables, and calls xrl_layer_boot.
 *
 *   xrl_test_host.exe <rootfs_dir> <guest_elf_path_inside_rootfs> [guest args...]
 *
 * Env: XRL_TRACE=1 traces every syscall. XRL_REAL_GFX=1 replaces the stub gfx
 * table with gfx/'s real one on a desktop window. Audio goes to XAudio2 through the console
 * host's own op table unless XRL_NO_AUDIO=1. Exit code = guest exit code.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "xrl_abi.h"

static void pad_script_event(const char *msg);

static void log_cb(int level, const char *tag, const char *msg)
{
    static const char *lv[] = { "T", "I", "W", "E" };
    printf("[%s %-9s] %s\n", lv[level & 3], tag, msg);
    fflush(stdout);
    pad_script_event(msg);
}

/* Win64 host functions the guest reaches through generated SysV stubs. */
static int g_release_calls;
static void host_release(xrl_handle h) { g_release_calls++; printf("[host] gfx release(%llx)\n", (unsigned long long)h); fflush(stdout); }
static xrl_status host_create_device(xrl_handle *out) { *out = 0xD3D12DEull; return XRL_OK; }
static xrl_status host_swapchain(xrl_handle d, xrl_handle q, uint32_t w, uint32_t h, uint32_t n, uint32_t fmt, xrl_handle *out)
{
    /* 7 args: checks stack-passed argument marshalling */
    *out = d ^ q ^ ((uint64_t)w << 32 | h) ^ ((uint64_t)n << 8) ^ fmt;
    return XRL_OK;
}
/* Pad 0: connected, idle. XRL_PAD_SCRIPT="130:A,135:DOWN,140:0x1000,150:RT:3" presses
 * that button (XInput name or mask; LT/RT are the triggers) N seconds after start, for
 * 200 ms or the given seconds: a scripted player for menus and a drive. Pads 1..3 are absent. */
#define PAD_LT 0x10000000u   /* script-only bits: the analog triggers, fully pressed */
#define PAD_RT 0x20000000u
static struct { double at, len; uint32_t mask; } g_pad_steps[64];
static int g_pad_nsteps;
static volatile ULONGLONG g_pad_t0;
/* XRL_PAD_AFTER="<text>": the script's clock starts at the first log line containing
 * <text> (a game event, e.g. "LoadMap: MENU_Main_p") instead of at start */
static char g_pad_after[128];
static volatile LONG g_pad_armed;
static void pad_script_event(const char *msg)
{
    if (g_pad_after[0] && !g_pad_armed && strstr(msg, g_pad_after) && !InterlockedExchange(&g_pad_armed, 1)) {
        g_pad_t0 = GetTickCount64();
        printf("[host] pad script: clock starts now (\"%s\")\n", g_pad_after);
        fflush(stdout);
        char mark[MAX_PATH];   /* XRL_PAD_MARK=<file>: created now, for tools that time screenshots */
        if (GetEnvironmentVariableA("XRL_PAD_MARK", mark, sizeof mark)) {
            HANDLE h = CreateFileA(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
}
static void pad_script_init(void)
{
    static const struct { const char *n; uint32_t m; } names[] = {
        { "UP", 0x1 }, { "DOWN", 0x2 }, { "LEFT", 0x4 }, { "RIGHT", 0x8 }, { "START", 0x10 }, { "BACK", 0x20 },
        { "LB", 0x100 }, { "RB", 0x200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
        { "LT", PAD_LT }, { "RT", PAD_RT } };
    char s[1024];
    g_pad_t0 = GetTickCount64();
    if (GetEnvironmentVariableA("XRL_PAD_AFTER", g_pad_after, sizeof g_pad_after)) g_pad_t0 = ~0ull >> 1;   /* not yet */
    if (!GetEnvironmentVariableA("XRL_PAD_SCRIPT", s, sizeof s)) return;
    for (char *tok = strtok(s, ","); tok && g_pad_nsteps < 64; tok = strtok(NULL, ",")) {
        char *c = strchr(tok, ':');
        if (!c) continue;
        *c++ = 0;
        char *d = strchr(c, ':');
        if (d) *d++ = 0;
        uint32_t m = (uint32_t)strtoul(c, NULL, 0);
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!_stricmp(c, names[i].n)) m = names[i].m;
        g_pad_steps[g_pad_nsteps].at = atof(tok);
        g_pad_steps[g_pad_nsteps].len = d ? atof(d) : 0.2;
        g_pad_steps[g_pad_nsteps++].mask = m;
    }
    printf("[host] pad script: %d presses\n", g_pad_nsteps);
}
static xrl_status host_pad(uint32_t slot, xrl_pad_state *o)
{
    memset(o, 0, sizeof *o);
    if (slot) return XRL_ERR_NOTAVAIL;
    o->connected = 1;
    if (g_pad_after[0] && !g_pad_armed) return XRL_OK;   /* waiting for XRL_PAD_AFTER's event */
    double t = (double)(GetTickCount64() - g_pad_t0) / 1000.0;
    uint32_t m = 0;
    for (int i = 0; i < g_pad_nsteps; i++)
        if (t >= g_pad_steps[i].at && t < g_pad_steps[i].at + g_pad_steps[i].len) m |= g_pad_steps[i].mask;
    o->buttons = m & 0xffff;
    if (m & PAD_LT) o->left_trigger = 255;
    if (m & PAD_RT) o->right_trigger = 255;
    return XRL_OK;
}
static xrl_status host_xaudio_create(xrl_handle *o) { *o = 0xA0D10ull; return XRL_OK; }

const xrl_gfx_ops *xrl_desktop_gfx(void (*log)(int, const char *, const char *));   /* desktop_gfx.cpp */
const xrl_audio_ops *xrl_host_audio_ops(void);                       /* host/src/audio_ops.cpp (XAudio2) */
void xrl_desktop_audio_set_log(xrl_log_fn log);                      /* desktop_audio.cpp */

static void build_envp(const char **envp)
{
    /* Guest environment: defaults, plus every host variable XRL_ENV_<NAME>=v as
     * <NAME>=v (e.g. XRL_ENV_WINEPREFIX=/prefix), so any guest can be run. */
    int e = 0;
    envp[e++] = "HOME=/home/xbox"; envp[e++] = "PATH=/usr/bin:/bin";
    static char trace[16]; if (GetEnvironmentVariableA("XRL_TRACE", trace + 0, 15)) { static char t2[24]; sprintf(t2, "XRL_TRACE=%s", trace); envp[e++] = t2; }
    for (char *s = GetEnvironmentStringsA(); s && *s && e < 63; s += strlen(s) + 1)
        if (!strncmp(s, "XRL_ENV_", 8)) envp[e++] = s + 8;
    /* the real display for Wine's mode list (win32u, patches/wine/0025): this desktop's mode,
     * unless XRL_ENV_XRL_DISPLAY gave one */
    static char disp[48]; DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!GetEnvironmentVariableA("XRL_ENV_XRL_DISPLAY", NULL, 0) && e < 63 && EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        sprintf(disp, "XRL_DISPLAY=%lux%lu@%lu", (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency);
        envp[e++] = disp;
    }
    envp[e] = NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <rootfs> <guest-elf> [args]\n", argv[0]); return 2; }
    static xrl_gfx_ops gfx; static xrl_audio_ops audio; static xrl_input_ops input;
    gfx.struct_size = sizeof gfx; gfx.d3d12_create_device = host_create_device;
    gfx.create_swapchain_for_corewindow = host_swapchain; gfx.release = host_release;
    audio.struct_size = sizeof audio; audio.xaudio2_create = host_xaudio_create; audio.release = host_release;
    input.struct_size = sizeof input; input.get_pad_state = host_pad;
    pad_script_init();

    const char *gargv[32]; int n = 0;
    gargv[n++] = argv[2];
    for (int i = 3; i < argc && n < 30; i++) gargv[n++] = argv[i];
    gargv[n] = NULL;
    static const char *envp[64];
    build_envp(envp);
    /* XRL_EXEC_HOST=1: start the program as a child and become what it execs traced (layer/src/task.c),
     * as the console's window-owning first process does */
    { char xh[4];
      if (!GetEnvironmentVariableA("XRL_SPAWN_DEPTH", NULL, 0) && GetEnvironmentVariableA("XRL_EXEC_HOST", xh, sizeof xh) && xh[0] == '1') {
          char **nargv = NULL; int ec = 0;
          int h = xrl_exec_host_run(argv[1], gargv, envp, log_cb, &nargv, &ec);
          if (h == 0) { printf("[host] exec host: the child ended (%d) without handing over\n", ec); return ec; }
          if (h > 0) {
              n = 0;
              for (char **a = nargv; a && *a && n < 31; a++) gargv[n++] = *a;
              gargv[n] = NULL;
              build_envp(envp);
          }
      } }

    xrl_boot_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.struct_size = sizeof ctx; ctx.abi_version = XRL_ABI_VERSION;
    ctx.vfs_root = argv[1]; ctx.prefix_path = "/prefix"; ctx.game_path = "/game";
    ctx.argv = gargv; ctx.envp = envp; ctx.log = log_cb;
    ctx.gfx = &gfx; ctx.audio = &audio; ctx.input = &input;
    char mb[16];
    /* XRL_REAL_GFX=1: gfx/'s real op table on a desktop window (laptop GPU) */
    if (GetEnvironmentVariableA("XRL_REAL_GFX", mb, sizeof mb) && mb[0] == '1') {
        const xrl_gfx_ops *real = xrl_desktop_gfx(log_cb);
        if (!real) return 3;
        ctx.gfx = real;
    }
    /* real audio (the console host's XAudio2 op table) unless XRL_NO_AUDIO=1; the guest test
     * suite (--fake-host) checks the stub table's values */
    int fake_host = 0;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--fake-host")) fake_host = 1;
    if (!fake_host && !(GetEnvironmentVariableA("XRL_NO_AUDIO", mb, sizeof mb) && mb[0] == '1')) {
        xrl_desktop_audio_set_log(log_cb);
        ctx.audio = xrl_host_audio_ops();
    }
    ctx.guest_mem_budget = (GetEnvironmentVariableA("XRL_TEST_BUDGET_MB", mb, sizeof mb) ? strtoull(mb, NULL, 10) : 1024ull) << 20;
    /* XRL_TEST_JOB_MB=N: a process commit limit below the budget, as on the console, where the
     * GPU and the host share Windows' limit with the guest (commits fail with ERROR_COMMITMENT_LIMIT) */
    if (GetEnvironmentVariableA("XRL_TEST_JOB_MB", mb, sizeof mb)) {
        HANDLE job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        li.ProcessMemoryLimit = (SIZE_T)strtoull(mb, NULL, 10) << 20;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
            !AssignProcessToJobObject(job, GetCurrentProcess()))
            printf("XRL_TEST_JOB_MB: job limit not applied (err %lu)\n", GetLastError());
    }
    int rc = xrl_layer_boot(&ctx);
    printf("xrl_layer_boot returned %d\n", rc);
    return rc;
}
