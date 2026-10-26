/*
 * test_host.c - desktop (Win32) harness that plays the role of host/ (workstream A):
 * fills an xrl_boot_context, provides log + stub op tables, and calls xrl_layer_boot.
 *
 *   xrl_test_host.exe <rootfs_dir> <guest_elf_path_inside_rootfs> [guest args...]
 *
 * Env: XRL_TRACE=1 traces every syscall. Exit code = guest exit code.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "xrl_abi.h"

static void log_cb(int level, const char *tag, const char *msg)
{
    static const char *lv[] = { "T", "I", "W", "E" };
    printf("[%s %-9s] %s\n", lv[level & 3], tag, msg);
    fflush(stdout);
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
static xrl_status host_pad(uint32_t slot, xrl_pad_state *o) { memset(o, 0, sizeof *o); o->buttons = 0x1000 | slot; o->connected = 1; return XRL_OK; }
static xrl_status host_xaudio_create(xrl_handle *o) { *o = 0xA0D10ull; return XRL_OK; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <rootfs> <guest-elf> [args]\n", argv[0]); return 2; }
    static xrl_gfx_ops gfx; static xrl_audio_ops audio; static xrl_input_ops input;
    gfx.struct_size = sizeof gfx; gfx.d3d12_create_device = host_create_device;
    gfx.create_swapchain_for_corewindow = host_swapchain; gfx.release = host_release;
    audio.struct_size = sizeof audio; audio.xaudio2_create = host_xaudio_create; audio.release = host_release;
    input.struct_size = sizeof input; input.get_pad_state = host_pad;

    const char *gargv[32]; int n = 0;
    gargv[n++] = argv[2];
    for (int i = 3; i < argc && n < 30; i++) gargv[n++] = argv[i];
    gargv[n] = NULL;
    /* Guest environment: defaults, plus every host variable XRL_ENV_<NAME>=v as
     * <NAME>=v (e.g. XRL_ENV_WINEPREFIX=/prefix), so any guest can be run. */
    static const char *envp[64]; int e = 0;
    envp[e++] = "HOME=/home/xbox"; envp[e++] = "PATH=/usr/bin:/bin";
    static char trace[16]; if (GetEnvironmentVariableA("XRL_TRACE", trace + 0, 15)) { static char t2[24]; sprintf(t2, "XRL_TRACE=%s", trace); envp[e++] = t2; }
    for (char *s = GetEnvironmentStringsA(); s && *s && e < 63; s += strlen(s) + 1)
        if (!strncmp(s, "XRL_ENV_", 8)) envp[e++] = s + 8;
    envp[e] = NULL;

    xrl_boot_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.struct_size = sizeof ctx; ctx.abi_version = XRL_ABI_VERSION;
    ctx.vfs_root = argv[1]; ctx.prefix_path = "/prefix"; ctx.game_path = "/game";
    ctx.argv = gargv; ctx.envp = envp; ctx.log = log_cb;
    ctx.gfx = &gfx; ctx.audio = &audio; ctx.input = &input;
    char mb[16];
    ctx.guest_mem_budget = (GetEnvironmentVariableA("XRL_TEST_BUDGET_MB", mb, sizeof mb) ? strtoull(mb, NULL, 10) : 1024ull) << 20;
    int rc = xrl_layer_boot(&ctx);
    printf("xrl_layer_boot returned %d\n", rc);
    return rc;
}
