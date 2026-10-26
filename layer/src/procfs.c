/*
 * procfs.c - synthesized /proc and /sys files.
 *
 * Everything here is generated on open from layer state (VMA list, thread
 * list, fd table, boot context). Only what Wine/musl plausibly read is
 * provided; anything else is -ENOENT. Add entries to proc_init() when the
 * ENOENT log (vfs: ENOENT proc ...) shows Wine asking for more.
 */
#include "xrl_internal.h"

typedef char *(*gen_fn)(const char *path, size_t *len);
enum { K_FILE = 1, K_DIR = 2, K_LINK = 3 };

typedef struct pent { char *path; int kind; gen_fn gen; } pent;
static pent *g_pe;
static size_t g_npe, g_cappe;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

slong xrl_fd_close_range(unsigned first, unsigned last, unsigned flags);
int  xrl_fd_list(int *out, int cap);

static void padd(const char *path, int kind, gen_fn gen)
{
    if (g_npe == g_cappe) {
        g_cappe = g_cappe ? g_cappe * 2 : 128;
        g_pe = (pent *)realloc(g_pe, g_cappe * sizeof(pent));
    }
    g_pe[g_npe].path = _strdup(path);
    g_pe[g_npe].kind = kind;
    g_pe[g_npe].gen = gen;
    g_npe++;
}

static char *dupfmt(size_t *len, const char *fmt, ...)
{
    char *b = (char *)malloc(8192);
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(b, 8192, _TRUNCATE, fmt, ap);
    va_end(ap);
    *len = n < 0 ? strlen(b) : (size_t)n;
    return b;
}

/* ------------------------------------------------------------ generators */
static char *g_maps(const char *p, size_t *len)
{
    (void)p;
    size_t cap = 1 << 20;
    char *b = (char *)malloc(cap);
    *len = xrl_mem_format_maps(b, cap);
    return b;
}
static char *g_cmdline(const char *p, size_t *len)
{
    (void)p;
    size_t n = 0;
    for (int i = 0; i < G.argc; i++) n += strlen(G.argv[i]) + 1;
    char *b = (char *)malloc(n + 1), *w = b;
    for (int i = 0; i < G.argc; i++) { size_t l = strlen(G.argv[i]) + 1; memcpy(w, G.argv[i], l); w += l; }
    *len = n;
    return b;
}
static char *g_environ(const char *p, size_t *len)
{
    (void)p;
    size_t n = 0;
    for (int i = 0; i < G.envc; i++) n += strlen(G.envp[i]) + 1;
    char *b = (char *)malloc(n + 1), *w = b;
    for (int i = 0; i < G.envc; i++) { size_t l = strlen(G.envp[i]) + 1; memcpy(w, G.envp[i], l); w += l; }
    *len = n;
    return b;
}
static const char *comm_name(void)
{
    const char *b = G.exec_path ? strrchr(G.exec_path, '/') : NULL;
    return b ? b + 1 : (G.exec_path ? G.exec_path : "wine");
}
static char *g_comm(const char *p, size_t *len)
{
    (void)p;
    xrl_thread *t = xrl_cur();
    return dupfmt(len, "%.15s\n", t && t->name[0] ? t->name : comm_name());
}
static char *g_status(const char *p, size_t *len)
{
    (void)p;
    u64 vm = xrl_mem_committed() >> 10;
    return dupfmt(len,
        "Name:\t%.15s\nUmask:\t0022\nState:\tR (running)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t1\nTracerPid:\t0\n"
        "Uid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\nFDSize:\t256\nGroups:\t\n"
        "VmPeak:\t%llu kB\nVmSize:\t%llu kB\nVmRSS:\t%llu kB\nVmData:\t%llu kB\nVmStk:\t132 kB\nThreads:\t%d\n"
        "SigQ:\t0/31000\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\n"
        "SigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\nCapInh:\t0000000000000000\nCapPrm:\t0000000000000000\n"
        "CapEff:\t0000000000000000\nCpus_allowed:\tff\nCpus_allowed_list:\t0-7\nvoluntary_ctxt_switches:\t1\nnonvoluntary_ctxt_switches:\t1\n",
        comm_name(), G.pid, G.pid, vm, vm, vm, vm, (int)G.nthreads);
}
static char *g_stat(const char *p, size_t *len)
{
    (void)p;
    u64 pages = xrl_mem_committed() >> 12;
    u64 up = (xrl_mono_ns() - 0) / 10000000ull;
    return dupfmt(len,
        "%d (%.15s) R 1 %d %d 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 %d 0 %llu %llu %llu 18446744073709551615 "
        "0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        G.pid, comm_name(), G.pid, G.pid, (int)G.nthreads, up, pages << 12, pages);
}
static char *g_statm(const char *p, size_t *len)
{
    (void)p;
    u64 pages = xrl_mem_committed() >> 12;
    return dupfmt(len, "%llu %llu 0 0 0 %llu 0\n", pages, pages, pages);
}
static char *g_limits(const char *p, size_t *len)
{
    (void)p;
    return dupfmt(len,
        "Limit                     Soft Limit           Hard Limit           Units     \n"
        "Max stack size            8388608              unlimited            bytes     \n"
        "Max open files            65536                65536                files     \n"
        "Max address space         unlimited            unlimited            bytes     \n");
}
static char *g_mounts(const char *p, size_t *len) { (void)p; return dupfmt(len, "rootfs / rootfs rw 0 0\nproc /proc proc rw 0 0\n"); }
static char *g_mountinfo(const char *p, size_t *len) { (void)p; return dupfmt(len, "1 1 0:1 / / rw - rootfs rootfs rw\n"); }
static char *g_cpuinfo(const char *p, size_t *len) { (void)p; return xrl_cpuinfo_text(len); }
static char *g_meminfo(const char *p, size_t *len)
{
    (void)p;
    u64 tot = (G.mem_budget ? G.mem_budget : (3ull << 30)) >> 10;
    u64 used = xrl_mem_committed() >> 10;
    u64 fr = tot > used ? tot - used : 0;
    return dupfmt(len,
        "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:               0 kB\nCached:                0 kB\n"
        "SwapCached:            0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\nCommitLimit:    %llu kB\nCommitted_AS:   %llu kB\n"
        "VmallocTotal:   34359738367 kB\nHugePages_Total:       0\nHugepagesize:       2048 kB\n",
        tot, fr, fr, tot, used);
}
static char *g_procstat(const char *p, size_t *len)
{
    (void)p;
    int n = xrl_nprocs();
    char *b = (char *)malloc(n * 100 + 400), *w = b;
    u64 up = xrl_mono_ns() / 10000000ull;
    w += sprintf(w, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n", up, up / 4, up * (u64)n);
    for (int i = 0; i < n; i++) w += sprintf(w, "cpu%d %llu 0 %llu %llu 0 0 0 0 0 0\n", i, up / n, up / 4 / n, up);
    w += sprintf(w, "intr 0\nctxt 0\nbtime %llu\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n", xrl_real_ns() / 1000000000ull - xrl_mono_ns() / 1000000000ull, G.pid);
    *len = (size_t)(w - b);
    return b;
}
static char *g_uptime(const char *p, size_t *len) { (void)p; u64 ms = xrl_mono_ns() / 10000000ull; return dupfmt(len, "%llu.%02llu %llu.%02llu\n", ms / 100, ms % 100, ms / 100, ms % 100); }
static char *g_loadavg(const char *p, size_t *len) { (void)p; return dupfmt(len, "0.10 0.10 0.10 1/%d %d\n", (int)G.nthreads, G.pid); }
static char *g_version(const char *p, size_t *len) { (void)p; return dupfmt(len, "Linux version 6.1.0-xrl (xbox-rl@layer) (gcc) #1 SMP\n"); }
static char *g_filesystems(const char *p, size_t *len) { (void)p; return dupfmt(len, "nodev\tproc\nnodev\ttmpfs\n\text4\n"); }
static char *g_osrelease(const char *p, size_t *len) { (void)p; return dupfmt(len, "6.1.0-xrl\n"); }
static char *g_ostype(const char *p, size_t *len) { (void)p; return dupfmt(len, "Linux\n"); }
static char *g_hostname(const char *p, size_t *len) { (void)p; return dupfmt(len, "%s\n", xrl_hostname()); }
static char *g_pidmax(const char *p, size_t *len) { (void)p; return dupfmt(len, "4194304\n"); }
static char *g_bootid(const char *p, size_t *len) { (void)p; return dupfmt(len, "5a1b0000-0000-4000-8000-0000000b00c1\n"); }
static char *g_zero(const char *p, size_t *len) { (void)p; return dupfmt(len, "0\n"); }
static char *g_two(const char *p, size_t *len) { (void)p; return dupfmt(len, "2\n"); }
static char *g_mapcount(const char *p, size_t *len) { (void)p; return dupfmt(len, "1048576\n"); }
static char *g_mmapmin(const char *p, size_t *len) { (void)p; return dupfmt(len, "65536\n"); }
static char *g_filemax(const char *p, size_t *len) { (void)p; return dupfmt(len, "1048576\n"); }
static char *g_cpurange(const char *p, size_t *len) { (void)p; return dupfmt(len, "0-%d\n", xrl_nprocs() - 1); }
static char *g_never(const char *p, size_t *len) { (void)p; return dupfmt(len, "always madvise [never]\n"); }
static char *g_auxv(const char *p, size_t *len) { (void)p; *len = 0; return (char *)malloc(1); }
static char *g_cpuid_n(const char *p, size_t *len)
{
    /* /sys/devices/system/cpu/cpuN/topology/<leaf> */
    int n = 0; sscanf(p, "/sys/devices/system/cpu/cpu%d", &n);
    const char *leaf = strrchr(p, '/') + 1;
    if (!strcmp(leaf, "core_id")) return dupfmt(len, "%d\n", xrl_cpu_core_id(n));
    if (!strcmp(leaf, "physical_package_id") || !strcmp(leaf, "die_id")) return dupfmt(len, "0\n");
    if (!strcmp(leaf, "thread_siblings_list") || !strcmp(leaf, "core_cpus_list")) {
        char l[300];
        xrl_cpu_siblings_list(n, l, sizeof l);
        return dupfmt(len, "%s", l);
    }
    return dupfmt(len, "0-%d\n", xrl_nprocs() - 1);   /* core_siblings_list / package_cpus_list */
}

/* ---------------------------------------------------------- table build */
static BOOL CALLBACK proc_init(PINIT_ONCE io, PVOID prm, PVOID *ctx)
{
    (void)io; (void)prm; (void)ctx;
    padd("/proc", K_DIR, NULL);
    padd("/proc/self", K_DIR, NULL);
    padd("/proc/self/fd", K_DIR, NULL);
    padd("/proc/self/task", K_DIR, NULL);
    padd("/proc/self/maps", K_FILE, g_maps);
    padd("/proc/self/cmdline", K_FILE, g_cmdline);
    padd("/proc/self/environ", K_FILE, g_environ);
    padd("/proc/self/status", K_FILE, g_status);
    padd("/proc/self/stat", K_FILE, g_stat);
    padd("/proc/self/statm", K_FILE, g_statm);
    padd("/proc/self/comm", K_FILE, g_comm);
    padd("/proc/self/limits", K_FILE, g_limits);
    padd("/proc/self/mounts", K_FILE, g_mounts);
    padd("/proc/self/mountinfo", K_FILE, g_mountinfo);
    padd("/proc/self/auxv", K_FILE, g_auxv);
    padd("/proc/self/exe", K_LINK, NULL);
    padd("/proc/self/cwd", K_LINK, NULL);
    padd("/proc/self/root", K_LINK, NULL);
    padd("/proc/cpuinfo", K_FILE, g_cpuinfo);
    padd("/proc/meminfo", K_FILE, g_meminfo);
    padd("/proc/stat", K_FILE, g_procstat);
    padd("/proc/uptime", K_FILE, g_uptime);
    padd("/proc/loadavg", K_FILE, g_loadavg);
    padd("/proc/version", K_FILE, g_version);
    padd("/proc/filesystems", K_FILE, g_filesystems);
    padd("/proc/mounts", K_FILE, g_mounts);
    padd("/proc/sys", K_DIR, NULL);
    padd("/proc/sys/kernel", K_DIR, NULL);
    padd("/proc/sys/kernel/osrelease", K_FILE, g_osrelease);
    padd("/proc/sys/kernel/ostype", K_FILE, g_ostype);
    padd("/proc/sys/kernel/hostname", K_FILE, g_hostname);
    padd("/proc/sys/kernel/pid_max", K_FILE, g_pidmax);
    padd("/proc/sys/kernel/randomize_va_space", K_FILE, g_zero);
    padd("/proc/sys/kernel/random", K_DIR, NULL);
    padd("/proc/sys/kernel/random/boot_id", K_FILE, g_bootid);
    padd("/proc/sys/vm", K_DIR, NULL);
    padd("/proc/sys/vm/max_map_count", K_FILE, g_mapcount);
    padd("/proc/sys/vm/overcommit_memory", K_FILE, g_zero);
    padd("/proc/sys/vm/mmap_min_addr", K_FILE, g_mmapmin);
    padd("/proc/sys/fs", K_DIR, NULL);
    padd("/proc/sys/fs/file-max", K_FILE, g_filemax);
    padd("/proc/sys/fs/nr_open", K_FILE, g_filemax);
    padd("/sys", K_DIR, NULL);
    padd("/sys/devices", K_DIR, NULL);
    padd("/sys/devices/system", K_DIR, NULL);
    padd("/sys/devices/system/cpu", K_DIR, NULL);
    padd("/sys/devices/system/cpu/online", K_FILE, g_cpurange);
    padd("/sys/devices/system/cpu/possible", K_FILE, g_cpurange);
    padd("/sys/devices/system/cpu/present", K_FILE, g_cpurange);
    padd("/sys/devices/system/cpu/kernel_max", K_FILE, g_cpurange);
    padd("/sys/kernel", K_DIR, NULL);
    padd("/sys/kernel/mm", K_DIR, NULL);
    padd("/sys/kernel/mm/transparent_hugepage", K_DIR, NULL);
    padd("/sys/kernel/mm/transparent_hugepage/enabled", K_FILE, g_never);
    (void)g_two;
    int n = xrl_nprocs();
    for (int i = 0; i < n; i++) {
        char p[160];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d", i); padd(p, K_DIR, NULL);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology", i); padd(p, K_DIR, NULL);
        static const char *leaves[] = { "core_id", "physical_package_id", "die_id", "thread_siblings_list", "core_cpus_list", "core_siblings_list", "package_cpus_list" };
        for (size_t k = 0; k < ARRAY_LEN(leaves); k++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/%s", i, leaves[k]);
            padd(p, K_FILE, g_cpuid_n);
        }
    }
    return TRUE;
}

/* /proc/<digits>/... and /proc/thread-self -> /proc/self/... */
static void normalize(const char *in, char *out, size_t cap)
{
    if (!strncmp(in, "/proc/", 6) && in[6] >= '0' && in[6] <= '9') {
        const char *p = in + 6;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (v == G.pid || xrl_thread_by_tid(v)) { snprintf(out, cap, "/proc/self%s", p); return; }
    }
    if (!strncmp(in, "/proc/thread-self", 17)) { snprintf(out, cap, "/proc/self%s", in + 17); return; }
    snprintf(out, cap, "%s", in);
}

static pent *plookup(const char *norm)
{
    InitOnceExecuteOnce(&g_once, proc_init, NULL, NULL);
    for (size_t i = 0; i < g_npe; i++) if (!strcmp(g_pe[i].path, norm)) return &g_pe[i];
    return NULL;
}

/* dynamic directory contents: /proc/self/fd, /proc/self/task, /proc/self/task/<tid> */
static int dyn_kind(const char *norm, int *is_fd_entry, int *num)
{
    *is_fd_entry = 0;
    if (!strncmp(norm, "/proc/self/fd/", 14) && norm[14]) { *is_fd_entry = 1; *num = atoi(norm + 14); return K_LINK; }
    if (!strncmp(norm, "/proc/self/task/", 16)) {
        int tid = atoi(norm + 16);
        const char *rest = strchr(norm + 16, '/');
        if (!xrl_thread_by_tid(tid) && tid != G.pid) return 0;
        if (!rest) { *num = tid; return K_DIR; }
        if (!strcmp(rest, "/comm") || !strcmp(rest, "/stat") || !strcmp(rest, "/status")) { *num = tid; return K_FILE; }
    }
    return 0;
}

/* ------------------------------------------------------------- file object */
typedef struct memfile { char *data; size_t len; } memfile;

static slong mf_read(xfile *f, void *buf, u64 n, s64 off)
{
    memfile *m = (memfile *)f->priv;
    int use_pos = off < 0;
    if (use_pos) { AcquireSRWLockExclusive(&f->lock); off = f->pos; }
    slong r = 0;
    if ((u64)off < m->len) {
        u64 c = m->len - (u64)off;
        if (c > n) c = n;
        r = xrl_ctu((u64)buf, m->data + off, (size_t)c) ? -LX_EFAULT : (slong)c;
        if (use_pos && r > 0) f->pos = off + r;
    }
    if (use_pos) ReleaseSRWLockExclusive(&f->lock);
    return r;
}
static slong mf_write(xfile *f, const void *b, u64 n, s64 o) { (void)f; (void)b; (void)o; return n ? -LX_EACCES : 0; }
static slong mf_lseek(xfile *f, s64 off, int wh)
{
    memfile *m = (memfile *)f->priv;
    s64 np = wh == 0 ? off : wh == 1 ? f->pos + off : (s64)m->len + off;
    if (np < 0) return -LX_EINVAL;
    f->pos = np;
    return (slong)np;
}
static slong mf_fstat(xfile *f, lx_stat *st)
{
    memfile *m = (memfile *)f->priv;
    xrl_stat_synth(st, LX_S_IFREG | 0444, (u64)(uintptr_t)f->gpath, 0);
    st->st_size = 0;   /* procfs reports 0 */
    (void)m;
    return 0;
}
static u32 mf_poll(xfile *f, u32 ev) { (void)f; return ev & (LX_POLLIN | LX_POLLOUT); }
static void mf_close(xfile *f) { memfile *m = (memfile *)f->priv; if (m) { free(m->data); free(m); } }
static const xfile_ops g_mf_ops = { "procfile", mf_read, mf_write, mf_lseek, mf_fstat, NULL, NULL, NULL, NULL, mf_poll, mf_close, mf_read };

/* directory listing object */
typedef struct pdirent { char name[128]; int type; } pdirent;
typedef struct pdir { pdirent *e; size_t n; size_t cur; } pdir;

static slong pd_getdents(xfile *f, void *ubuf, u64 cap)
{
    pdir *d = (pdir *)f->priv;
    char *kb = (char *)malloc((size_t)(cap > 65536 ? 65536 : cap));
    if (cap > 65536) cap = 65536;
    u64 used = 0;
    while (d->cur < d->n) {
        pdirent *e = &d->e[d->cur];
        size_t nl = strlen(e->name);
        u64 rl = (19 + nl + 1 + 7) & ~7ull;
        if (used + rl > cap) break;
        lx_dirent64 *de = (lx_dirent64 *)(kb + used);
        memset(de, 0, (size_t)rl);
        de->d_ino = 1000 + d->cur; de->d_off = (s64)d->cur + 1; de->d_reclen = (u16)rl; de->d_type = (u8)e->type;
        memcpy(de->d_name, e->name, nl + 1);
        used += rl; d->cur++;
    }
    slong r = used ? (xrl_ctu((u64)ubuf, kb, (size_t)used) ? -LX_EFAULT : (slong)used) : 0;
    free(kb);
    return r;
}
static slong pd_lseek(xfile *f, s64 off, int wh) { pdir *d = (pdir *)f->priv; if (wh == 0 && off == 0) { d->cur = 0; return 0; } return -LX_EINVAL; }
static slong pd_fstat(xfile *f, lx_stat *st) { xrl_stat_synth(st, LX_S_IFDIR | 0555, (u64)(uintptr_t)f->gpath, 0); st->st_nlink = 2; return 0; }
static void pd_close(xfile *f) { pdir *d = (pdir *)f->priv; if (d) { free(d->e); free(d); } }
static const xfile_ops g_pd_ops = { "procdir", NULL, NULL, pd_lseek, pd_fstat, pd_getdents, NULL, NULL, NULL, mf_poll, pd_close, NULL };

static void pdir_add(pdir *d, const char *name, int type)
{
    d->e = (pdirent *)realloc(d->e, (d->n + 1) * sizeof(pdirent));
    strncpy_s(d->e[d->n].name, sizeof d->e[d->n].name, name, _TRUNCATE);
    d->e[d->n].type = type;
    d->n++;
}

static xfile *open_dir(const char *norm, const char *orig)
{
    pdir *d = (pdir *)calloc(1, sizeof *d);
    pdir_add(d, ".", LX_DT_DIR);
    pdir_add(d, "..", LX_DT_DIR);
    if (!strcmp(norm, "/proc/self/fd")) {
        int fds[1024]; int n = xrl_fd_list(fds, 1024);
        for (int i = 0; i < n; i++) { char b[16]; sprintf(b, "%d", fds[i]); pdir_add(d, b, LX_DT_LNK); }
    } else if (!strcmp(norm, "/proc/self/task")) {
        AcquireSRWLockShared(&G.thread_lock);
        for (xrl_thread *t = G.threads; t; t = t->next) { char b[16]; sprintf(b, "%d", t->tid); pdir_add(d, b, LX_DT_DIR); }
        ReleaseSRWLockShared(&G.thread_lock);
    } else {
        size_t pl = strlen(norm);
        for (size_t i = 0; i < g_npe; i++) {
            const char *p = g_pe[i].path;
            if (strncmp(p, norm, pl) || p[pl] != '/' || strchr(p + pl + 1, '/')) continue;
            pdir_add(d, p + pl + 1, g_pe[i].kind == K_DIR ? LX_DT_DIR : g_pe[i].kind == K_LINK ? LX_DT_LNK : LX_DT_REG);
        }
        if (!strcmp(norm, "/proc")) { char b[16]; sprintf(b, "%d", G.pid); pdir_add(d, b, LX_DT_DIR); }
    }
    xfile *f = xfile_new(&g_pd_ops, LX_O_RDONLY);
    f->priv = d;
    f->gpath = _strdup(orig);
    f->mode = 1;
    return f;
}

xfile *xrl_proc_open(const char *gabs, int flags, int *err)
{
    char norm[512];
    normalize(gabs, norm, sizeof norm);
    pent *e = plookup(norm);
    int isfd = 0, num = 0;
    int kind = e ? e->kind : dyn_kind(norm, &isfd, &num);
    if (!kind) {
        if (G.trace_syscalls) LOGW("vfs: ENOENT proc %s", gabs);
        *err = LX_ENOENT;
        return NULL;
    }
    if (kind == K_DIR) return open_dir(norm, gabs);
    if (kind == K_LINK) { *err = LX_ENOENT; return NULL; }   /* callers resolve links via readlink first */
    char *data; size_t len;
    if (e) data = e->gen(norm, &len);
    else {
        const char *leaf = strrchr(norm, '/') + 1;
        if (!strcmp(leaf, "comm")) {
            xrl_thread *t = xrl_thread_by_tid(num);
            data = dupfmt(&len, "%.15s\n", t ? t->name : comm_name());
        } else data = g_stat(norm, &len);
    }
    memfile *m = (memfile *)calloc(1, sizeof *m);
    m->data = data; m->len = len;
    xfile *f = xfile_new(&g_mf_ops, flags & LX_O_ACCMODE);
    f->priv = m;
    f->gpath = _strdup(gabs);
    return f;
}

slong xrl_proc_readlink(const char *gabs, char *buf, u64 cap)
{
    char norm[512];
    normalize(gabs, norm, sizeof norm);
    char tmp[2048];
    tmp[0] = 0;
    if (!strcmp(norm, "/proc/self/exe")) snprintf(tmp, sizeof tmp, "%s", G.exec_path ? G.exec_path : "/bin/wine");
    else if (!strcmp(norm, "/proc/self/cwd")) { AcquireSRWLockShared(&G.cwd_lock); snprintf(tmp, sizeof tmp, "%s", G.cwd); ReleaseSRWLockShared(&G.cwd_lock); }
    else if (!strcmp(norm, "/proc/self/root")) snprintf(tmp, sizeof tmp, "/");
    else if (!strncmp(norm, "/proc/self/fd/", 14)) {
        xfile *f = xrl_fd_get(atoi(norm + 14));
        if (!f) return -LX_ENOENT;
        if (f->gpath) snprintf(tmp, sizeof tmp, "%s", f->gpath);
        else snprintf(tmp, sizeof tmp, "%s:[%p]", f->ops->name, (void *)f);
        xfile_unref(f);
    } else return -LX_ENOENT;
    size_t n = strlen(tmp);
    if (n > cap) n = (size_t)cap;
    memcpy(buf, tmp, n);
    return (slong)n;
}

/* Returns 1 if handled. st->st_nlink == 0 on return means "handled: ENOENT". */
int xrl_proc_stat(const char *gabs, lx_stat *st)
{
    if (strncmp(gabs, "/proc", 5) && strncmp(gabs, "/sys", 4)) return 0;
    char norm[512];
    normalize(gabs, norm, sizeof norm);
    pent *e = plookup(norm);
    int isfd, num;
    int kind = e ? e->kind : dyn_kind(norm, &isfd, &num);
    memset(st, 0, sizeof *st);
    if (!kind) return 1;   /* st_nlink==0 -> ENOENT */
    xrl_stat_synth(st, kind == K_DIR ? (LX_S_IFDIR | 0555) : kind == K_LINK ? (LX_S_IFLNK | 0777) : (LX_S_IFREG | 0444),
                   (u64)(uintptr_t)(e ? e->path : gabs), 0);
    if (kind == K_DIR) st->st_nlink = 2;
    return 1;
}

int xrl_proc_is_virtual(const char *gabs) { return !strncmp(gabs, "/proc", 5) || !strncmp(gabs, "/sys", 4); }
