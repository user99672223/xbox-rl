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

/* ELF types for walking the interpreter's program headers in memory. */
#pragma pack(push, 1)
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff;
                 u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } boot_Ehdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } boot_Phdr;
typedef struct { s64 tag; u64 val; } boot_Dyn;
#pragma pack(pop)

#define BOOT_PT_DYNAMIC 2
#define BOOT_DT_HASH       4
#define BOOT_DT_STRTAB     5
#define BOOT_DT_SYMTAB     6
#define BOOT_DT_GNU_HASH   0x6ffffef5u
#define BOOT_DT_VERSYM     0x6ffffff0u

/* Make the already-loaded interpreter (musl) visible to the EAC bootstrapper as
 * "/usr/lib/libc.so" and fix up its dynamic section entries to absolute addresses.
 *
 * The bootstrapper's algorithm:
 *   1. read /proc/<pid>/maps, find the first line whose path contains "libc.so"
 *   2. read the ELF header at that base address, walk PT_DYNAMIC
 *   3. DT_HASH/DT_GNU_HASH/DT_SYMTAB/DT_STRTAB/DT_VERSYM are treated as absolute
 *      pointers (glibc's ld.so leaves them so; musl leaves link-time offsets)
 *   4. look up "dlopen" and "dlsym" and call them
 *
 * The interpreter IS the live musl with an initialized dlopen. We just need:
 *   (a) its maps entries to say "libc.so" (not "ld-musl-x86_64.so.1")
 *   (b) its dynamic section entries to be absolute (the glibc-compat shim's job,
 *       but Wine isn't built with that patch on Xbox)
 */
static volatile u64 g_dynfix_base;   /* nonzero = deferred fixup pending */
static u64 g_boot_libc_lo, g_boot_libc_hi;   /* the first (launcher) musl span, so Plan C can re-tag it */

static void fixup_libc_for_eac(const xrl_elf_image *img)
{
    if (!img->base || !img->interp_lo) {
        LOGW("boot: no interpreter loaded (EAC libc fixup skipped)");
        return;
    }
    u64 lo = img->interp_lo, hi = img->interp_hi;

    /* (a) Rename the interpreter's VMAs so /proc/self/maps says "libc.so" — safe
     *     to do now; it only touches our VMA name table, not the interpreter's data. */
    xrl_mem_rename_range(lo, hi, "/usr/lib/libc.so");
    g_boot_libc_lo = lo; g_boot_libc_hi = hi;
    LOGI("boot: interpreter VMAs [%llx,%llx) renamed to /usr/lib/libc.so", lo, hi);

    /* (b) Defer the dynamic-section fixup: musl's own startup walks these entries
     *     as offsets, so we must not convert them to absolute before it has finished
     *     initialising.  The fixup fires on the first /proc/self/maps read (which is
     *     when EAC's bootstrapper actually needs the absolute pointers). */
    g_dynfix_base = img->base;
    LOGI("boot: interpreter dynamic fixup deferred (base=%llx)", img->base);
}

/* Plan C (in-process EAC injection): EAC finds libc by the FIRST /proc/<pid>/maps line containing
 * "libc.so", resolves dlopen there, and injects it into the GAME. With the launcher and game sharing one
 * address space (two musl instances), the boot-time rename tagged the LAUNCHER's musl "libc.so", so EAC
 * resolved the launcher's dlopen -- whose dl lock the launcher contends -> the injected call deadlocks on
 * the console (the laptop only survives by lock-timing luck). Move the "libc.so" identity and the deferred
 * DT fixup onto the GAME's own musl, and un-tag the launcher's, so EAC injects the game's dlopen instead. */
void xrl_boot_move_libc_to_game(const xrl_elf_image *img)
{
    if (!img || !img->base || !img->interp_lo) { LOGW("inproc: game has no interpreter; libc identity not moved"); return; }
    if (g_boot_libc_lo) {
        xrl_mem_rename_range(g_boot_libc_lo, g_boot_libc_hi, "/lib/ld-musl-x86_64.so.1");
        LOGI("inproc: launcher libc VMAs [%llx,%llx) un-tagged (EAC must not resolve the launcher's dlopen)", g_boot_libc_lo, g_boot_libc_hi);
    }
    xrl_mem_rename_range(img->interp_lo, img->interp_hi, "/usr/lib/libc.so");
    g_dynfix_base = img->base;
    LOGI("inproc: game libc VMAs [%llx,%llx) renamed to /usr/lib/libc.so; dynfix deferred (base=%llx)",
         img->interp_lo, img->interp_hi, img->base);
}

void xrl_boot_apply_dynfix(void)
{
    u64 base = (u64)InterlockedExchange64((volatile LONG64 *)&g_dynfix_base, 0);
    if (!base) return;   /* already applied (or nothing pending) */

    const boot_Ehdr *eh = (const boot_Ehdr *)base;
    if (memcmp(eh->ident, "\177ELF", 4) || eh->machine != 0x3e || eh->type != 3) {
        LOGW("boot: interpreter at %llx is not a valid ELF (deferred dynfix skipped)", base);
        return;
    }
    const boot_Phdr *ph = (const boot_Phdr *)(base + eh->phoff);
    u64 dyn_va = 0, dyn_memsz = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == BOOT_PT_DYNAMIC) {
            dyn_va = ph[i].vaddr + base;
            dyn_memsz = ph[i].memsz;
            break;
        }
    }
    if (!dyn_va) { LOGW("boot: interpreter has no PT_DYNAMIC (deferred dynfix)"); return; }

    u64 dyn_page = ALIGN_DOWN(dyn_va, LX_PAGE);
    u64 dyn_end = ALIGN_UP(dyn_va + dyn_memsz, LX_PAGE);
    xrl_mem_mprotect(dyn_page, dyn_end - dyn_page, LX_PROT_READ | LX_PROT_WRITE);
    int fixed = 0;
    for (boot_Dyn *d = (boot_Dyn *)dyn_va; d->tag != 0; d++) {
        switch ((u32)d->tag) {
        case BOOT_DT_HASH: case BOOT_DT_GNU_HASH: case BOOT_DT_SYMTAB:
        case BOOT_DT_STRTAB: case BOOT_DT_VERSYM:
            if (d->val < base) { d->val += base; fixed++; }
            break;
        }
    }
    xrl_mem_mprotect(dyn_page, dyn_end - dyn_page, LX_PROT_READ);
    LOGI("boot: deferred dynfix applied: %d entries converted to absolute (base=%llx)", fixed, base);
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
    { char a0[1024]; if (GetEnvironmentVariableA("XRL_ARGV0", a0, sizeof a0) && G.argc) { free(G.argv[0]); G.argv[0] = _strdup(a0); } }   /* execve's argv[0], when it is not the program's path */
    G.mem_budget = ctx->guest_mem_budget;
    G.exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeSRWLock(&G.cwd_lock);
    InitializeSRWLock(&G.thread_lock);
    G.umask = 022;

    const char *tr = env_get("XRL_TRACE");
    G.trace_syscalls = tr && tr[0] ? atoi(tr) : 0;   /* 1 raw numbers, 2 decoded (systrace.c) */

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
    xrl_perf_start();
    xrl_fd_init();
    if (xrl_vfs_init(ctx->vfs_root)) return XRL_ERR_GENERIC;
    xrl_fd_inherit_boot();   /* memfds a parent passed on through exec (task.c) */

    xrl_elf_image img;
    if (xrl_elf_load(G.exec_path, &img)) { LOGE("boot: failed to load %s", G.exec_path); return XRL_ERR_GENERIC; }
    fixup_libc_for_eac(&img);
    if (xrl_guest_ctx_build()) { LOGE("boot: failed to build guest ctx"); return XRL_ERR_GENERIC; }

    LOGI("boot: building initial stack");
    u64 slo, shi;
    u64 sp = xrl_elf_build_stack(&img, &slo, &shi);
    if (!sp) { LOGE("boot: cannot allocate initial stack"); return XRL_ERR_GENERIC; }
    xrl_tracee_boot(&img.entry, &sp);   /* a program exec'd by a traced fork child waits here for its tracer (tracee.c) */

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
    xrl_dbt_dump_faults();   /* dbt.c: print any faults the DBT let escape (safe context) */
    xrl_mem_dump_faults();   /* mem.c: why the memory handler could not back a faulting write + full map */
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
