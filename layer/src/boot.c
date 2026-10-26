/*
 * boot.c - xrl_layer_boot / xrl_layer_request_exit (ABI seam 2).
 *
 * Boot sequence
 *   1. validate struct_size + abi_version
 *   2. deep-copy the boot context (host buffers may not outlive the call)
 *   3. init: log, clocks, threads (FSGSBASE probe), futex, mem arena, rng, fd
 *      table (stdio), VFS, signals/VEH
 *   4. load the ELF named by argv[0] (a *guest* path, resolved through the VFS)
 *   5. build the guest-visible context (SysV stubs for the host op tables) and
 *      the initial stack/auxv
 *   6. start the main guest thread; optionally start the wineserver thread hook
 *   7. block until the guest exits (exit_group / last thread exits /
 *      xrl_layer_request_exit); terminate stragglers; return the exit code
 *
 * Only one boot per process is supported (global state).
 */
#include "xrl_internal.h"

void xrl_threads_init(void);
int  xrl_threads_start_fn(u64 fn, const char *name);

static char *dupstr(const char *s) { return s ? _strdup(s) : NULL; }

static char **dup_strv(const char *const *v, int *count)
{
    int n = 0;
    while (v && v[n]) n++;
    char **o = (char **)calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) o[i] = _strdup(v[i]);
    *count = n;
    return o;
}

static const char *env_get(const char *name)
{
    size_t l = strlen(name);
    for (int i = 0; i < G.envc; i++) if (!strncmp(G.envp[i], name, l) && G.envp[i][l] == '=') return G.envp[i] + l + 1;
    return NULL;
}

int xrl_layer_boot(const xrl_boot_context *ctx)
{
    static volatile LONG booted;
    if (!ctx || ctx->struct_size < offsetof(xrl_boot_context, guest_mem_budget) + sizeof(uint64_t)) return XRL_ERR_GENERIC;
    /* v2 is append-only over v1: accept both; v2 fields are read only when
     * struct_size covers them. */
    if (ctx->abi_version < 1 || ctx->abi_version > XRL_ABI_VERSION) {
        if (ctx->log) ctx->log(3, "layer", "xrl_layer_boot: unsupported ABI version");
        return XRL_ERR_GENERIC;
    }
    if (!ctx->vfs_root || !ctx->argv || !ctx->argv[0] || !ctx->gfx || !ctx->audio || !ctx->input) {
        if (ctx->log) ctx->log(3, "layer", "xrl_layer_boot: vfs_root/argv/op tables must be non-NULL");
        return XRL_ERR_GENERIC;
    }
    if (InterlockedExchange(&booted, 1)) {
        if (ctx->log) ctx->log(3, "layer", "xrl_layer_boot: already booted once in this process");
        return XRL_ERR_GENERIC;
    }

    memset(&G, 0, sizeof G);
    /* copy only what the host's struct actually has (a v1 host is smaller) */
    memcpy(&G.ctx, ctx, ctx->struct_size < sizeof G.ctx ? ctx->struct_size : sizeof G.ctx);
    G.ctx.struct_size = sizeof G.ctx;
    G.vfs_root = NULL;
    G.prefix_path = dupstr(ctx->prefix_path);
    G.game_path = dupstr(ctx->game_path);
    G.argv = dup_strv(ctx->argv, &G.argc);
    G.envp = dup_strv(ctx->envp, &G.envc);
    {
        const char *ep = NULL;
        if (ctx->struct_size >= offsetof(xrl_boot_context, exec_path) + sizeof(ctx->exec_path)) ep = ctx->exec_path;
        G.exec_path = _strdup(ep && *ep ? ep : G.argv[0]);
    }
    G.mem_budget = ctx->guest_mem_budget;
    G.exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeSRWLock(&G.cwd_lock);
    InitializeSRWLock(&G.thread_lock);
    G.umask = 022;

    const char *tr = env_get("XRL_TRACE");
    G.trace_syscalls = tr && tr[0] && tr[0] != '0';

    LOGI("layer %s booting (ABI v%u), argv[0]=%s, %d args, %d env, budget=%llu MiB", XRL_LAYER_VERSION, XRL_ABI_VERSION,
         G.exec_path, G.argc, G.envc, G.mem_budget >> 20);

    xrl_time_init();
    xrl_threads_init();
    xrl_teb_init();
    xrl_futex_init();
    xrl_rng_init();
    if (xrl_mem_init(G.mem_budget)) return XRL_ERR_GENERIC;
    xrl_sig_init();
    xrl_sig_install_veh();
    xrl_hang_watch_start();
    xrl_fd_init();
    if (xrl_vfs_init(ctx->vfs_root)) return XRL_ERR_GENERIC;

    xrl_elf_image img;
    if (xrl_elf_load(G.exec_path, &img)) { LOGE("boot: failed to load %s", G.exec_path); return XRL_ERR_GENERIC; }
    if (xrl_guest_ctx_build()) { LOGE("boot: failed to build guest ctx"); return XRL_ERR_GENERIC; }

    LOGI("boot: building initial stack");
    u64 slo, shi;
    u64 sp = xrl_elf_build_stack(&img, &slo, &shi);
    if (!sp) { LOGE("boot: cannot allocate initial stack"); return XRL_ERR_GENERIC; }

    LOGI("boot: stack sp=%llx", sp);
    xrl_thread *mt = NULL;
    if (xrl_threads_start_main(img.entry, sp, &mt)) { LOGE("boot: cannot create main thread"); return XRL_ERR_GENERIC; }
    mt->guest_stack_lo = slo; mt->guest_stack_hi = shi;
    LOGI("boot: guest main thread tid=%d entry=%llx sp=%llx", mt->tid, img.entry, sp);

    if (img.exports_xrl_wineserver_main) {
        int tid = xrl_threads_start_fn(img.exports_xrl_wineserver_main, "wineserver");
        LOGI("boot: wineserver thread hook xrl_wineserver_main=%llx -> tid %d", img.exports_xrl_wineserver_main, tid);
    }

    WaitForSingleObject(G.exit_event, INFINITE);
    int code = (int)G.exit_code;
    { extern volatile LONG64 xrl_fs_rearms; LOGI("boot: FS-base re-arm faults handled: %lld", (long long)xrl_fs_rearms); }
    LOGI("boot: guest exiting with code %d; draining %lld thread(s)", code, (slong)G.nthreads);
    for (int i = 0; i < 50 && G.nthreads > 0; i++) Sleep(10);
    if (G.nthreads > 0) { LOGW("boot: terminating %lld straggler thread(s)", (slong)G.nthreads); xrl_threads_terminate_all(); }
    return code;
}

void xrl_layer_request_exit(int exit_code)
{
    if (!G.exit_event) return;
    InterlockedExchange(&G.exit_code, exit_code);
    InterlockedExchange(&G.exiting, 1);
    SetEvent(G.exit_event);
}
