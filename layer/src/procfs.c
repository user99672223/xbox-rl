/*
 * procfs.c - synthesized /proc and /sys files.
 *
 * Everything here is generated on open from layer state (VMA list, thread
 * list, fd table, boot context). /proc/<pid>/... shows that guest process
 * (proc.c); /proc/self and a pid of the caller's own process show the caller's. Only what Wine/musl plausibly read is
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

/* The process /proc/<pid>/... names while a file is opened, stat'ed or read-linked (set by norm_enter); the
 * calling thread's own for /proc/self. */
static __declspec(thread) xrl_proc *t_target;
static xrl_proc *tgt(void) { return t_target ? t_target : xrl_curproc(); }

/* ------------------------------------------------------------ generators */
static char *g_maps(const char *p, size_t *len)
{
    (void)p;
    xrl_boot_apply_dynfix();   /* convert interpreter DT entries to absolute (once) before EAC reads maps */
    size_t cap = 1 << 20;
    char *b = (char *)malloc(cap);
    xrl_proc *t = tgt();
    *len = t->mm ? xrl_mem_format_maps_mm(t->mm->id, b, cap) : 0;
    return b;
}
static char *g_cmdline(const char *p, size_t *len)
{
    (void)p;
    xrl_proc *t = tgt();
    size_t n = 0;
    for (int i = 0; i < t->argc; i++) n += strlen(t->argv[i]) + 1;
    char *b = (char *)malloc(n + 1), *w = b;
    for (int i = 0; i < t->argc; i++) { size_t l = strlen(t->argv[i]) + 1; memcpy(w, t->argv[i], l); w += l; }
    *len = n;
    return b;
}
static char *g_environ(const char *p, size_t *len)
{
    (void)p;
    xrl_proc *t = tgt();
    size_t n = 0;
    for (int i = 0; i < t->envc; i++) n += strlen(t->envp[i]) + 1;
    char *b = (char *)malloc(n + 1), *w = b;
    for (int i = 0; i < t->envc; i++) { size_t l = strlen(t->envp[i]) + 1; memcpy(w, t->envp[i], l); w += l; }
    *len = n;
    return b;
}
static const char *comm_name(void)
{
    static __declspec(thread) char c[16];
    xrl_proc *t = tgt();
    xrl_proc_comm(t, c);
    if (!c[0]) { const char *b = t->exec_path ? strrchr(t->exec_path, '/') : NULL; return b ? b + 1 : "wine"; }
    return c;
}
static char *g_comm(const char *p, size_t *len)
{
    if (!strncmp(p, "/proc/1/", 8)) return dupfmt(len, "init\n");
    xrl_thread *t = xrl_cur();
    return dupfmt(len, "%.15s\n", t && t->name[0] ? t->name : comm_name());
}
/* ---- status / stat of one task: this process, one of its threads, or the nominal init ---- */
/* The layer has no init process, as WSL1 has none; /proc/1 describes a nominal one (name "init", root,
 * one thread, nothing mapped) so that what every Linux has (PPid 1 -> a process 1 with a comm, a status,
 * a cgroup...) is there to read. What a non-root user cannot read of pid 1 on Linux (exe, cwd, root,
 * environ, maps, fd) fails with EACCES here too. */
typedef struct pwho { int init; int tid; char name[16]; u64 sigblk, sigpnd; int nthreads; } pwho;

static void pwho_of(const char *path, pwho *w)
{
    memset(w, 0, sizeof *w);
    if (!strncmp(path, "/proc/1/", 8)) {
        w->init = 1; w->tid = 1; w->nthreads = 1;
        strcpy_s(w->name, sizeof w->name, "init");
        return;
    }
    xrl_proc *pr = tgt();
    w->nthreads = (int)pr->nthreads;
    xrl_thread *t = NULL;
    if (!strncmp(path, "/proc/self/task/", 16)) t = xrl_thread_by_tid(atoi(path + 16));
    if (t) {
        w->tid = t->tid; w->sigblk = t->sigmask; w->sigpnd = t->sigpending;
        snprintf(w->name, sizeof w->name, "%.15s", t->name);
    } else {
        w->tid = pr->pid;
        snprintf(w->name, sizeof w->name, "%.15s", comm_name());
    }
}

/* affinity mask as the kernel prints it: 32-bit hex groups, most significant first, comma separated */
static void cpumask_hex(char *o, size_t cap, int n)
{
    size_t w = 0;
    for (int g = (n - 1) / 32; g >= 0; g--) {
        int bits = n - g * 32; if (bits > 32) bits = 32;
        u32 v = bits == 32 ? 0xffffffffu : ((1u << bits) - 1);
        w += (size_t)snprintf(o + w, cap - w, g == (n - 1) / 32 ? "%x" : ",%08x", v);
    }
}

static char *g_status(const char *p, size_t *len)
{
    pwho w; pwho_of(p, &w);
    u64 ign, cgt; xrl_sig_proc_masks(&ign, &cgt);
    lx_rlimit sigp; xrl_rlimit_get(11, &sigp);
    int n = xrl_nprocs();
    char mask[96]; cpumask_hex(mask, sizeof mask, n);
    xrl_proc *pr = tgt();
    u64 vm = w.init ? 4096 : pr->mm ? xrl_mem_mm_size(pr->mm->id) >> 10 : xrl_mem_committed() >> 10;
    u64 pte = vm / 512 + 8;
    int pid = w.init ? 1 : pr->pid, ppid = w.init ? 0 : pr->ppid;
    u32 uid = w.init ? 0 : pr->uid, gid = w.init ? 0 : pr->gid;
    /* a process reading its own status is running; another one, as seen from here, sleeps (or is stopped, a zombie) */
    const char *state = w.init ? "S (sleeping)" : pr->state == XP_ZOMBIE ? "Z (zombie)" : pr->state == XP_STOPPED ? "T (stopped)" :
                        pr == xrl_curproc() ? "R (running)" : "S (sleeping)";
    const u64 capall = 0x000001ffffffffffull;   /* CAP_LAST_CAP 40 on 6.1 */
    char *b = (char *)malloc(8192);
    int k = snprintf(b, 8192,
        "Name:\t%s\nUmask:\t%04o\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
        "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t%d\nGroups:\t\n"
        "NStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n"
        "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n"
        "RssAnon:\t%8llu kB\nRssFile:\t       0 kB\nRssShmem:\t       0 kB\nVmData:\t%8llu kB\nVmStk:\t     132 kB\n"
        "VmExe:\t       0 kB\nVmLib:\t       0 kB\nVmPTE:\t%8llu kB\nVmSwap:\t       0 kB\nHugetlbPages:\t       0 kB\n"
        "CoreDumping:\t0\nTHP_enabled:\t0\nThreads:\t%d\nSigQ:\t0/%llu\n"
        "SigPnd:\t%016llx\nShdPnd:\t%016llx\nSigBlk:\t%016llx\nSigIgn:\t%016llx\nSigCgt:\t%016llx\n"
        "CapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\nCapBnd:\t%016llx\nCapAmb:\t0000000000000000\n"
        "NoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\nSpeculation_Store_Bypass:\tvulnerable\nSpeculationIndirectBranch:\tunsupported\n"
        "Cpus_allowed:\t%s\nCpus_allowed_list:\t0-%d\nMems_allowed:\t00000001\nMems_allowed_list:\t0\n"
        "voluntary_ctxt_switches:\t1\nnonvoluntary_ctxt_switches:\t1\n",
        w.name, w.init ? 022u : (unsigned)pr->umask & 0777, state, pid, w.tid, ppid,
        uid, uid, uid, uid, gid, gid, gid, gid, w.init ? 64 : 256, pid, pid, pid, pid,
        vm, vm, vm, vm, vm, vm, pte, w.nthreads, sigp.rlim_cur,
        w.sigpnd, 0ull, w.sigblk, w.init ? 0ull : ign, w.init ? 0ull : cgt,
        0ull, w.init ? capall : 0ull, w.init ? capall : 0ull, capall,
        mask, n - 1);
    *len = k < 0 ? 0 : (size_t)k;
    return b;
}
static char *g_stat(const char *p, size_t *len)
{
    pwho w; pwho_of(p, &w);
    xrl_proc *pr = tgt();
    u64 pages = w.init ? 1024 : (pr->mm ? xrl_mem_mm_size(pr->mm->id) : xrl_mem_committed()) >> 12;
    u64 up = w.init ? 0 : pr->start_ns / 10000000ull;
    int pid = w.init ? 1 : pr->pid;
    char st = w.init ? 'S' : pr->state == XP_ZOMBIE ? 'Z' : pr->state == XP_STOPPED ? 'T' : pr == xrl_curproc() ? 'R' : 'S';
    /* the fields of proc(5) up to exit_code; the ones we do not keep are 0 */
    return dupfmt(len,
        "%d (%s) %c %d %d %d 0 %d 4194304 0 0 0 0 0 0 0 0 20 0 %d 0 %llu %llu %llu 18446744073709551615 "
        "0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        w.tid, w.name, st, w.init ? 0 : pr->ppid, w.init ? 1 : pr->pgid, w.init ? 1 : pr->sid, w.init ? -1 : xrl_tty_pgrp(),
        w.nthreads, up, pages << 12, pages);
}
static char *g_statm(const char *p, size_t *len)
{
    pwho w; pwho_of(p, &w);
    u64 pages = w.init ? 1024 : xrl_mem_committed() >> 12;
    return dupfmt(len, "%llu %llu 0 0 0 %llu 0\n", pages, pages, pages);
}
static char *g_limits(const char *p, size_t *len)
{
    (void)p;
    static const struct { const char *name; const char *unit; } k[] = {
        { "Max cpu time", "seconds" }, { "Max file size", "bytes" }, { "Max data size", "bytes" }, { "Max stack size", "bytes" },
        { "Max core file size", "bytes" }, { "Max resident set", "bytes" }, { "Max processes", "processes" }, { "Max open files", "files" },
        { "Max locked memory", "bytes" }, { "Max address space", "bytes" }, { "Max file locks", "locks" }, { "Max pending signals", "signals" },
        { "Max msgqueue size", "bytes" }, { "Max nice priority", "" }, { "Max realtime priority", "" }, { "Max realtime timeout", "us" } };
    char *b = (char *)malloc(4096);
    int n = snprintf(b, 4096, "%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
    for (int i = 0; i < 16; i++) {
        lx_rlimit r; xrl_rlimit_get(i, &r);
        char c[24], m[24];
        if (r.rlim_cur == LX_RLIM_INFINITY) strcpy_s(c, sizeof c, "unlimited"); else snprintf(c, sizeof c, "%llu", r.rlim_cur);
        if (r.rlim_max == LX_RLIM_INFINITY) strcpy_s(m, sizeof m, "unlimited"); else snprintf(m, sizeof m, "%llu", r.rlim_max);
        n += snprintf(b + n, 4096 - n, "%-25s %-20s %-20s %-10s\n", k[i].name, c, m, k[i].unit);
    }
    *len = (size_t)n;
    return b;
}
static char *g_cgroup(const char *p, size_t *len) { (void)p; return dupfmt(len, "0::/\n"); }
static char *g_init_cmdline(const char *p, size_t *len) { (void)p; char *b = (char *)malloc(8); memcpy(b, "/init\0", 6); *len = 6; return b; }
static char *g_oomscore(const char *p, size_t *len) { (void)p; return dupfmt(len, "0\n"); }
static char *g_personality(const char *p, size_t *len) { (void)p; return dupfmt(len, "00000000\n"); }
static char *g_uuid(const char *p, size_t *len)
{
    (void)p;
    u8 r[16]; xrl_rng_fill(r, sizeof r);
    r[6] = (u8)((r[6] & 0x0f) | 0x40); r[8] = (u8)((r[8] & 0x3f) | 0x80);
    return dupfmt(len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", r[0], r[1], r[2], r[3], r[4], r[5],
                  r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}
static char *g_domainname(const char *p, size_t *len) { (void)p; return dupfmt(len, "(none)\n"); }
static char *g_uts_version(const char *p, size_t *len) { (void)p; return dupfmt(len, XRL_UTS_VERSION "\n"); }
static char *g_threadsmax(const char *p, size_t *len) { (void)p; return dupfmt(len, "63000\n"); }
static char *g_corepattern(const char *p, size_t *len) { (void)p; return dupfmt(len, "core\n"); }
static char *g_one(const char *p, size_t *len) { (void)p; return dupfmt(len, "1\n"); }
static char *g_paranoid(const char *p, size_t *len) { (void)p; return dupfmt(len, "2\n"); }
/* the filesystems the layer really provides: the guest's root, and the virtual /proc, /sys and /dev */
static char *g_mounts(const char *p, size_t *len)
{
    (void)p;
    return dupfmt(len,
        "rootfs / rootfs rw 0 0\n"
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
        "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
        "devtmpfs /dev devtmpfs rw,nosuid,relatime,mode=755 0 0\n");
}
static char *g_mountinfo(const char *p, size_t *len)
{
    (void)p;
    return dupfmt(len,
        "1 1 0:1 / / rw - rootfs rootfs rw\n"
        "2 1 0:2 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
        "3 1 0:3 / /sys rw,nosuid,nodev,noexec,relatime - sysfs sysfs rw\n"
        "4 1 0:4 / /dev rw,nosuid,relatime - devtmpfs devtmpfs rw,mode=755\n");
}
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
static char *g_loadavg(const char *p, size_t *len) { (void)p; return dupfmt(len, "0.10 0.10 0.10 1/%d %d\n", (int)G.nthreads, xrl_curproc()->pid); }
static char *g_version(const char *p, size_t *len) { (void)p; return dupfmt(len, "Linux version 6.1.0-21-amd64 (debian-kernel@lists.debian.org) (gcc-12 (Debian 12.2.0-14) 12.2.0, GNU ld (GNU Binutils for Debian) 2.40) " XRL_UTS_VERSION "\n"); }
static char *g_filesystems(const char *p, size_t *len) { (void)p; return dupfmt(len, "nodev\trootfs\nnodev\tproc\nnodev\tsysfs\nnodev\tdevtmpfs\nnodev\ttmpfs\n\text4\n"); }
static char *g_osrelease(const char *p, size_t *len) { (void)p; return dupfmt(len, "6.1.0-21-amd64\n"); }
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
/* ---- /sys/class/dmi/id/* : a faithful SMBIOS/DMI identity.
 * Real Linux exposes the machine's firmware identity here; EAC's launcher reads
 * sys_vendor + product_name (and tools read the rest) for hardware/container
 * detection. We present an ordinary self-built desktop's DMI: ASUS board, AMI
 * BIOS, with the system fields left at the vendor defaults ("System manufacturer"
 * / "System Product Name") exactly as millions of DIY Linux machines report them.
 * No value here names the real host hardware. The identifying fields the kernel
 * makes root-only (product_uuid, *_serial, product_sku) return EACCES for our
 * non-root uid, as they would on a real machine read by a normal user. */
#define DMI_BIOS_VENDOR  "American Megatrends International, LLC."
#define DMI_BIOS_VERSION "2803"
#define DMI_BIOS_DATE    "10/18/2022"
#define DMI_BIOS_RELEASE "5.17"
#define DMI_SYS_VENDOR   "System manufacturer"
#define DMI_PRODUCT_NAME "System Product Name"
#define DMI_PRODUCT_VER  "System Version"
#define DMI_PRODUCT_FAM  "To be filled by O.E.M."
#define DMI_BOARD_VENDOR "ASUSTeK COMPUTER INC."
#define DMI_BOARD_NAME   "TUF GAMING B550-PLUS"
#define DMI_BOARD_VER    "Rev X.0x"
#define DMI_CHASSIS_VEN  "Default string"
#define DMI_CHASSIS_VER  "Default string"
#define DMI_CHASSIS_TYPE "3"   /* SMBIOS chassis type 3 = Desktop */

static char *g_dmi(const char *p, size_t *len)
{
    const char *leaf = strrchr(p, '/') + 1;
    const char *v = "";
    if      (!strcmp(leaf, "bios_vendor"))      v = DMI_BIOS_VENDOR;
    else if (!strcmp(leaf, "bios_version"))     v = DMI_BIOS_VERSION;
    else if (!strcmp(leaf, "bios_date"))        v = DMI_BIOS_DATE;
    else if (!strcmp(leaf, "bios_release"))     v = DMI_BIOS_RELEASE;
    else if (!strcmp(leaf, "sys_vendor"))       v = DMI_SYS_VENDOR;
    else if (!strcmp(leaf, "product_name"))     v = DMI_PRODUCT_NAME;
    else if (!strcmp(leaf, "product_version"))  v = DMI_PRODUCT_VER;
    else if (!strcmp(leaf, "product_family"))   v = DMI_PRODUCT_FAM;
    else if (!strcmp(leaf, "board_vendor"))     v = DMI_BOARD_VENDOR;
    else if (!strcmp(leaf, "board_name"))       v = DMI_BOARD_NAME;
    else if (!strcmp(leaf, "board_version"))    v = DMI_BOARD_VER;
    else if (!strcmp(leaf, "chassis_vendor"))   v = DMI_CHASSIS_VEN;
    else if (!strcmp(leaf, "chassis_version"))  v = DMI_CHASSIS_VER;
    else if (!strcmp(leaf, "chassis_type"))     v = DMI_CHASSIS_TYPE;
    else if (!strcmp(leaf, "modalias")) {
        return dupfmt(len, "dmi:bvn" DMI_BIOS_VENDOR ":bvr" DMI_BIOS_VERSION ":bd" DMI_BIOS_DATE
            ":br" DMI_BIOS_RELEASE ":svn" DMI_SYS_VENDOR ":pn" DMI_PRODUCT_NAME ":pvr" DMI_PRODUCT_VER
            ":rvn" DMI_BOARD_VENDOR ":rn" DMI_BOARD_NAME ":rvr" DMI_BOARD_VER
            ":cvn" DMI_CHASSIS_VEN ":ct" DMI_CHASSIS_TYPE ":cvr" DMI_CHASSIS_VER ":\n");
    }
    else if (!strcmp(leaf, "uevent")) {
        return dupfmt(len, "MODALIAS=dmi:bvn" DMI_BIOS_VENDOR ":bvr" DMI_BIOS_VERSION ":bd" DMI_BIOS_DATE
            ":br" DMI_BIOS_RELEASE ":svn" DMI_SYS_VENDOR ":pn" DMI_PRODUCT_NAME ":pvr" DMI_PRODUCT_VER
            ":rvn" DMI_BOARD_VENDOR ":rn" DMI_BOARD_NAME ":rvr" DMI_BOARD_VER
            ":cvn" DMI_CHASSIS_VEN ":ct" DMI_CHASSIS_TYPE ":cvr" DMI_CHASSIS_VER ":\n");
    }
    return dupfmt(len, "%s\n", v);
}
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
    if (!strcmp(leaf, "thread_siblings") || !strcmp(leaf, "core_cpus") || !strcmp(leaf, "core_siblings") || !strcmp(leaf, "package_cpus")) {
        u64 m = 0; char l[200];
        int pkg = leaf[0] == 'p' || !strcmp(leaf, "core_siblings");
        for (int i = 0; i < xrl_nprocs() && i < 64; i++)
            if (pkg || xrl_cpu_core_id(i) == xrl_cpu_core_id(n)) m |= 1ull << i;
        xrl_cpu_mask_str(m, l, sizeof l);
        return dupfmt(len, "%s", l);
    }
    return dupfmt(len, "0-%d\n", xrl_nprocs() - 1);   /* core_siblings_list / package_cpus_list */
}
/* /sys/devices/system/cpu/cpuN/cache/indexK/<leaf> */
static char *g_cache_n(const char *p, size_t *len)
{
    int n = 0, k = 0;
    sscanf(p, "/sys/devices/system/cpu/cpu%d/cache/index%d", &n, &k);
    const char *leaf = strrchr(p, '/') + 1;
    xrl_cache c[8];
    if (k >= xrl_cpu_caches(n, c, ARRAY_LEN(c))) return dupfmt(len, "");
    const xrl_cache *x = &c[k];
    char l[300];
    if (!strcmp(leaf, "id")) return dupfmt(len, "%d\n", x->id);
    if (!strcmp(leaf, "level")) return dupfmt(len, "%d\n", x->level);
    if (!strcmp(leaf, "type")) return dupfmt(len, "%s\n", x->type == 0 ? "Data" : x->type == 1 ? "Instruction" : "Unified");
    if (!strcmp(leaf, "size")) return dupfmt(len, "%uK\n", x->size >> 10);
    if (!strcmp(leaf, "ways_of_associativity")) return dupfmt(len, "%u\n", x->ways);
    if (!strcmp(leaf, "coherency_line_size")) return dupfmt(len, "%u\n", x->line);
    if (!strcmp(leaf, "number_of_sets")) return dupfmt(len, "%u\n", x->ways && x->line ? x->size / (x->ways * x->line) : 1);
    if (!strcmp(leaf, "physical_line_partition")) return dupfmt(len, "1\n");
    if (!strcmp(leaf, "shared_cpu_map")) { xrl_cpu_mask_str(x->mask, l, sizeof l); return dupfmt(len, "%s", l); }
    /* shared_cpu_list: ranges, as in "0-1" or "0-7" */
    size_t w = 0;
    for (int i = 0; i < 64; i++) {
        if (!(x->mask >> i & 1)) continue;
        int j = i;
        while (j + 1 < 64 && (x->mask >> (j + 1) & 1)) j++;
        w += (size_t)snprintf(l + w, sizeof l - w, j > i ? "%s%d-%d" : "%s%d", w ? "," : "", i, j);
        i = j;
    }
    return dupfmt(len, "%s\n", l);
}

/* /proc/bus/pci/devices: the PCI devices of the desktop the DMI identity describes (ASUS TUF GAMING
 * B550-PLUS with a Ryzen 5000 and a Radeon RX 6600), in the kernel's format (drivers/pci/proc.c
 * show_device): bus+devfn, vendor+device, irq, 7 base addresses, 7 sizes, the bound driver. */
static char *g_pcidevs(const char *p, size_t *len)
{
    (void)p;
    static const struct { unsigned bd, vd, irq; unsigned long long bar0, bar2, size0, size2; const char *drv; } d[] = {
        { 0x0000, 0x10221480,   0, 0, 0, 0, 0, "" },                       /* 00:00.0 Starship/Matisse Root Complex */
        { 0x0002, 0x10221481,  27, 0, 0, 0, 0, "" },                       /* 00:00.2 IOMMU */
        { 0x0008, 0x10221482,   0, 0, 0, 0, 0, "" },                       /* 00:01.0 host bridge */
        { 0x000b, 0x10221483,  28, 0, 0, 0, 0, "pcieport" },               /* 00:01.3 GPP bridge */
        { 0x0010, 0x10221482,   0, 0, 0, 0, 0, "" },                       /* 00:02.0 host bridge */
        { 0x0018, 0x10221482,   0, 0, 0, 0, 0, "" },                       /* 00:03.0 host bridge */
        { 0x0019, 0x10221483,  29, 0, 0, 0, 0, "pcieport" },               /* 00:03.1 GPP bridge (GPU) */
        { 0x0038, 0x10221482,   0, 0, 0, 0, 0, "" },                       /* 00:07.0 host bridge */
        { 0x0039, 0x10221484,  31, 0, 0, 0, 0, "pcieport" },               /* 00:07.1 internal bridge */
        { 0x0040, 0x10221482,   0, 0, 0, 0, 0, "" },                       /* 00:08.0 host bridge */
        { 0x0041, 0x10221484,  32, 0, 0, 0, 0, "pcieport" },               /* 00:08.1 internal bridge */
        { 0x00a0, 0x1022790b,   0, 0, 0, 0, 0, "piix4_smbus" },            /* 00:14.0 FCH SMBus */
        { 0x00a3, 0x1022790e,   0, 0, 0, 0, 0, "" },                       /* 00:14.3 FCH LPC */
        { 0x00c0, 0x10221440,   0, 0, 0, 0, 0, "" },                       /* 00:18.0-7 data fabric */
        { 0x00c1, 0x10221441,   0, 0, 0, 0, 0, "" },
        { 0x00c2, 0x10221442,   0, 0, 0, 0, 0, "" },
        { 0x00c3, 0x10221443,   0, 0, 0, 0, 0, "k10temp" },
        { 0x00c4, 0x10221444,   0, 0, 0, 0, 0, "" },
        { 0x00c5, 0x10221445,   0, 0, 0, 0, 0, "" },
        { 0x00c6, 0x10221446,   0, 0, 0, 0, 0, "" },
        { 0x00c7, 0x10221447,   0, 0, 0, 0, 0, "" },
        { 0x0100, 0x144dA80a,  33, 0xfcf00004ull, 0, 0x4000, 0, "nvme" },  /* 01:00.0 Samsung 980 PRO NVMe */
        { 0x0200, 0x102243e9,   0, 0, 0, 0, 0, "pcieport" },               /* 02:00.0 B550 switch upstream */
        { 0x0240, 0x102243ea,  34, 0, 0, 0, 0, "pcieport" },               /* 02:08.0 B550 switch downstream */
        { 0x0300, 0x102243eb,  35, 0, 0, 0, 0, "ahci" },                   /* 03:00.0 B550 SATA */
        { 0x0400, 0x10ec8168,  36, 0xf001ull, 0xfcc04004ull, 0x100, 0x4000, "r8169" },   /* 04:00.0 RTL8111 */
        { 0x0700, 0x100273ff, 120, 0xd000000cull, 0xf000000cull, 0x10000000ull, 0x200000ull, "amdgpu" },   /* 07:00.0 Navi 23 */
        { 0x0701, 0x1002ab28,  73, 0xfcb20000ull, 0, 0x4000, 0, "snd_hda_intel" },   /* 07:00.1 Navi 21/23 HDMI audio */
        { 0x0900, 0x1022148c,  39, 0xfc800000ull, 0, 0x100000, 0, "xhci_hcd" },      /* 09:00.0 Starship USB 3.0 */
        { 0x0903, 0x10221487,  75, 0xfc900000ull, 0, 0x8000, 0, "snd_hda_intel" },   /* 09:00.4 Starship HD audio */
    };
    char *b = (char *)malloc(ARRAY_LEN(d) * 320 + 1), *w = b;
    for (size_t i = 0; i < ARRAY_LEN(d); i++) {
        w += sprintf(w, "%04x\t%08x\t%x", d[i].bd, d[i].vd, d[i].irq);
        unsigned long long bars[7] = { d[i].bar0, 0, d[i].bar2, 0, 0, 0, 0 }, sizes[7] = { d[i].size0, 0, d[i].size2, 0, 0, 0, 0 };
        for (int k = 0; k < 7; k++) w += sprintf(w, "\t%16llx", bars[k]);
        for (int k = 0; k < 7; k++) w += sprintf(w, "\t%16llx", sizes[k]);
        w += sprintf(w, "\t%s\n", d[i].drv);
    }
    *len = (size_t)(w - b);
    return b;
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
    padd("/proc/self/cgroup", K_FILE, g_cgroup);
    padd("/proc/self/oom_score", K_FILE, g_oomscore);
    padd("/proc/self/oom_score_adj", K_FILE, g_oomscore);
    padd("/proc/self/personality", K_FILE, g_personality);
    padd("/proc/self/mounts", K_FILE, g_mounts);
    padd("/proc/self/mountinfo", K_FILE, g_mountinfo);
    padd("/proc/self/auxv", K_FILE, g_auxv);
    padd("/proc/self/exe", K_LINK, NULL);
    padd("/proc/self/cwd", K_LINK, NULL);
    padd("/proc/self/root", K_LINK, NULL);
    padd("/proc/cpuinfo", K_FILE, g_cpuinfo);
    padd("/proc/meminfo", K_FILE, g_meminfo);
    padd("/proc/bus", K_DIR, NULL);
    padd("/proc/bus/pci", K_DIR, NULL);
    padd("/proc/bus/pci/devices", K_FILE, g_pcidevs);
    padd("/proc/stat", K_FILE, g_procstat);
    padd("/proc/uptime", K_FILE, g_uptime);
    padd("/proc/loadavg", K_FILE, g_loadavg);
    padd("/proc/version", K_FILE, g_version);
    padd("/proc/filesystems", K_FILE, g_filesystems);
    padd("/proc/mounts", K_FILE, g_mounts);
    /* the nominal init (see g_status) */
    padd("/proc/1", K_DIR, NULL);
    padd("/proc/1/comm", K_FILE, g_comm);
    padd("/proc/1/cmdline", K_FILE, g_init_cmdline);
    padd("/proc/1/status", K_FILE, g_status);
    padd("/proc/1/stat", K_FILE, g_stat);
    padd("/proc/1/statm", K_FILE, g_statm);
    padd("/proc/1/cgroup", K_FILE, g_cgroup);
    padd("/proc/1/limits", K_FILE, g_limits);
    padd("/proc/1/oom_score", K_FILE, g_oomscore);
    padd("/proc/1/oom_score_adj", K_FILE, g_oomscore);
    padd("/proc/1/exe", K_LINK, NULL);
    padd("/proc/1/cwd", K_LINK, NULL);
    padd("/proc/1/root", K_LINK, NULL);
    padd("/proc/1/environ", K_FILE, NULL);
    padd("/proc/1/maps", K_FILE, NULL);
    padd("/proc/1/mem", K_FILE, NULL);
    padd("/proc/1/fd", K_DIR, NULL);
    padd("/proc/sys", K_DIR, NULL);
    padd("/proc/sys/kernel", K_DIR, NULL);
    padd("/proc/sys/kernel/version", K_FILE, g_uts_version);
    padd("/proc/sys/kernel/domainname", K_FILE, g_domainname);
    padd("/proc/sys/kernel/threads-max", K_FILE, g_threadsmax);
    padd("/proc/sys/kernel/core_pattern", K_FILE, g_corepattern);
    padd("/proc/sys/kernel/perf_event_paranoid", K_FILE, g_paranoid);
    padd("/proc/sys/kernel/kptr_restrict", K_FILE, g_one);
    padd("/proc/sys/kernel/random/uuid", K_FILE, g_uuid);
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
    /* SMBIOS/DMI identity (see g_dmi). On real Linux the files live under
     * /sys/devices/virtual/dmi/id and /sys/class/dmi/id symlinks to it; EAC reads
     * both spellings, so we expose the tree under BOTH prefixes. */
    static const char *dmi_pub[] = {
        "bios_vendor", "bios_version", "bios_date", "bios_release",
        "sys_vendor", "product_name", "product_version", "product_family",
        "board_vendor", "board_name", "board_version",
        "chassis_vendor", "chassis_version", "chassis_type", "modalias", "uevent" };
    /* ...and the identifying fields the kernel makes root-only (0400): a normal
     * user gets EACCES here, so gen==NULL (xrl_proc_open maps that to EACCES). */
    static const char *dmi_priv[] = {
        "product_uuid", "product_serial", "board_serial", "chassis_serial",
        "product_sku", "chassis_asset_tag" };
    static const char *dmi_dirs[] = { "/sys/class", "/sys/class/dmi", "/sys/class/dmi/id",
        "/sys/devices/virtual", "/sys/devices/virtual/dmi", "/sys/devices/virtual/dmi/id" };
    for (size_t k = 0; k < ARRAY_LEN(dmi_dirs); k++) padd(dmi_dirs[k], K_DIR, NULL);
    static const char *dmi_bases[] = { "/sys/class/dmi/id", "/sys/devices/virtual/dmi/id" };
    for (size_t b = 0; b < ARRAY_LEN(dmi_bases); b++) {
        for (size_t k = 0; k < ARRAY_LEN(dmi_pub); k++) {
            char p[96]; snprintf(p, sizeof p, "%s/%s", dmi_bases[b], dmi_pub[k]);
            padd(p, K_FILE, g_dmi);
        }
        for (size_t k = 0; k < ARRAY_LEN(dmi_priv); k++) {
            char p[96]; snprintf(p, sizeof p, "%s/%s", dmi_bases[b], dmi_priv[k]);
            padd(p, K_FILE, NULL);
        }
    }
    (void)g_two;
    int n = xrl_nprocs();
    for (int i = 0; i < n; i++) {
        char p[160];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d", i); padd(p, K_DIR, NULL);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology", i); padd(p, K_DIR, NULL);
        static const char *leaves[] = { "core_id", "physical_package_id", "die_id", "thread_siblings_list", "core_cpus_list", "core_siblings_list", "package_cpus_list",
                                        "thread_siblings", "core_cpus", "core_siblings", "package_cpus" };
        for (size_t k = 0; k < ARRAY_LEN(leaves); k++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/%s", i, leaves[k]);
            padd(p, K_FILE, g_cpuid_n);
        }
        xrl_cache c[8];
        int nc = xrl_cpu_caches(i, c, ARRAY_LEN(c));
        if (nc) { snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache", i); padd(p, K_DIR, NULL); }
        static const char *cleaves[] = { "id", "level", "type", "size", "ways_of_associativity", "coherency_line_size", "number_of_sets",
                                         "physical_line_partition", "shared_cpu_map", "shared_cpu_list" };
        for (int k = 0; k < nc; k++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d", i, k); padd(p, K_DIR, NULL);
            for (size_t m = 0; m < ARRAY_LEN(cleaves); m++) {
                snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/%s", i, k, cleaves[m]);
                padd(p, K_FILE, g_cache_n);
            }
        }
    }
    return TRUE;
}

/* /proc/<digits>/... and /proc/thread-self -> /proc/self/..., with t_target the process it names (a reference,
 * dropped by norm_leave) */
static void normalize(const char *in, char *out, size_t cap)
{
    if (!strncmp(in, "/proc/", 6) && in[6] >= '0' && in[6] <= '9') {
        const char *p = in + 6;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        xrl_proc *self = xrl_curproc();
        if (v == self->pid) { snprintf(out, cap, "/proc/self%s", p); return; }
        xrl_proc *q = xrl_proc_by_pid(v);
        if (!q) { xrl_thread *th = xrl_thread_by_tid(v); if (th && th->proc) { q = th->proc; xrl_proc_ref(q); } }
        if (q) { if (q == self) xrl_proc_put(q); else t_target = q; snprintf(out, cap, "/proc/self%s", p); return; }
    }
    if (!strncmp(in, "/proc/thread-self", 17)) { snprintf(out, cap, "/proc/self%s", in + 17); return; }
    snprintf(out, cap, "%s", in);
}
static void norm_leave(void)
{
    if (t_target) { xrl_proc_put(t_target); t_target = NULL; }
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
        if (!xrl_thread_by_tid(tid) && tid != tgt()->pid) return 0;
        if (!rest) { *num = tid; return K_DIR; }
        if (!strcmp(rest, "/comm") || !strcmp(rest, "/stat") || !strcmp(rest, "/status") || !strcmp(rest, "/statm")) { *num = tid; return K_FILE; }
    }
    return 0;
}

/* ------------------------------------------------------------- file object */
typedef struct memfile { char *data; size_t len; int comm_tid; } memfile;   /* comm_tid: a writable comm (-1 = the caller) */

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
/* Only a comm file is writable: it names a thread of this process (as on Linux, up to 15 bytes, the
 * trailing newline dropped). Wine forwards SetThreadDescription here (pthread_setname_np). */
static slong mf_write(xfile *f, const void *b, u64 n, s64 o)
{
    (void)o;
    memfile *m = (memfile *)f->priv;
    if (!n) return 0;
    if (!m->comm_tid) return -LX_EACCES;
    char name[16] = { 0 };
    if (xrl_cfu(name, (u64)(uintptr_t)b, n < 15 ? (size_t)n : 15)) return -LX_EFAULT;
    char *nlc = strchr(name, '\n');
    if (nlc) *nlc = 0;
    xrl_thread *t = m->comm_tid < 0 ? xrl_cur() : xrl_thread_by_tid(m->comm_tid);
    if (!t) return -LX_ESRCH;
    memcpy(t->name, name, sizeof name);
    return (slong)n;
}
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
        int fds[1024]; int n = xrl_files_list(tgt()->files, fds, 1024);
        for (int i = 0; i < n; i++) { char b[16]; sprintf(b, "%d", fds[i]); pdir_add(d, b, LX_DT_LNK); }
    } else if (!strcmp(norm, "/proc/self/task")) {
        AcquireSRWLockShared(&G.thread_lock);
        for (xrl_thread *t = G.threads; t; t = t->next) if (t->proc == tgt()) { char b[16]; sprintf(b, "%d", t->tid); pdir_add(d, b, LX_DT_DIR); }
        ReleaseSRWLockShared(&G.thread_lock);
    } else {
        size_t pl = strlen(norm);
        for (size_t i = 0; i < g_npe; i++) {
            const char *p = g_pe[i].path;
            if (strncmp(p, norm, pl) || p[pl] != '/' || strchr(p + pl + 1, '/')) continue;
            pdir_add(d, p + pl + 1, g_pe[i].kind == K_DIR ? LX_DT_DIR : g_pe[i].kind == K_LINK ? LX_DT_LNK : LX_DT_REG);
        }
        if (!strcmp(norm, "/proc")) {
            int pids[512], n = xrl_proc_list(pids, 512);
            for (int i = 0; i < n; i++) { char b[16]; sprintf(b, "%d", pids[i]); pdir_add(d, b, LX_DT_DIR); }
        }
    }
    xfile *f = xfile_new(&g_pd_ops, LX_O_RDONLY);
    f->priv = d;
    f->gpath = _strdup(orig);
    f->mode = 1;
    return f;
}

/* /proc/<pid>/{maps,exe} of a traced second process (task.c); 1 and sets *pid/leaf when gabs is such a path */
char *xrl_task_remote_proc(int pid, const char *leaf, size_t *len);
static int remote_path(const char *g, int *pid, const char **leaf)
{
    if (strncmp(g, "/proc/", 6) || g[6] < '0' || g[6] > '9') return 0;
    const char *p = g + 6; int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p != '/' || v == G.pid || v == 1 || xrl_thread_by_tid(v)) return 0;
    { xrl_proc *q = xrl_proc_by_pid(v); if (q) { xrl_proc_put(q); return 0; } }   /* a guest process of this one (proc.c) */
    if (strcmp(p + 1, "maps") && strcmp(p + 1, "exe")) return 0;
    *pid = v; *leaf = p + 1;
    return 1;
}

static xfile *proc_open(const char *gabs, int flags, int *err)
{
    char norm[512];
    {   int rp; const char *rl;
        if (remote_path(gabs, &rp, &rl) && !strcmp(rl, "maps")) {
            size_t n; char *d = xrl_task_remote_proc(rp, rl, &n);
            if (!d) { *err = LX_ENOENT; return NULL; }
            memfile *m = (memfile *)calloc(1, sizeof *m);
            m->data = d; m->len = n;
            xfile *f = xfile_new(&g_mf_ops, flags & LX_O_ACCMODE);
            f->priv = m; f->gpath = _strdup(gabs);
            return f;
        }
    }
    normalize(gabs, norm, sizeof norm);
    pent *e = plookup(norm);
    int isfd = 0, num = 0;
    int kind = e ? e->kind : dyn_kind(norm, &isfd, &num);
    if (!kind) {
        if (G.trace_syscalls == 1) LOGW("vfs: ENOENT proc %s", gabs);
        *err = LX_ENOENT;
        return NULL;
    }
    if (e && !strncmp(norm, "/proc/1/", 8) && (!e->gen || !strcmp(norm, "/proc/1/fd"))) { *err = LX_EACCES; return NULL; }   /* root's, as for a normal user */
    if (e && e->kind == K_FILE && !e->gen && strstr(norm, "/dmi/id/")) { *err = LX_EACCES; return NULL; }   /* root-only DMI ids */
    if (kind == K_DIR) return open_dir(norm, gabs);
    if (kind == K_LINK) { *err = LX_ENOENT; return NULL; }   /* callers resolve links via readlink first */
    char *data; size_t len;
    if (e) data = e->gen(norm, &len);
    else {
        const char *leaf = strrchr(norm, '/') + 1;
        if (!strcmp(leaf, "comm")) {
            xrl_thread *t = xrl_thread_by_tid(num);
            data = dupfmt(&len, "%.15s\n", t ? t->name : comm_name());
        } else if (!strcmp(leaf, "status")) data = g_status(norm, &len);
        else if (!strcmp(leaf, "statm")) data = g_statm(norm, &len);
        else data = g_stat(norm, &len);
    }
    if (!strcmp(norm, "/proc/self/maps") && data && len > 0) {
        LOGW("proc maps: %zu bytes, first 400: %.400s", len, data);
        if (len > 400) LOGW("proc maps tail: ...%.400s", data + len - (len > 400 ? 400 : len));
        LOGW("proc maps check: libc=%d ld=%d stack=%d vdso=%d",
             strstr(data, "libc") != NULL, strstr(data, "ld-") != NULL,
             strstr(data, "[stack]") != NULL, strstr(data, "[vdso]") != NULL);
        char dv[4];   /* DIAG (XRL_MAPS_DUMP=1): every line of what the reader gets */
        if (GetEnvironmentVariableA("XRL_MAPS_DUMP", dv, sizeof dv) && dv[0] == '1') {
            static volatile LONG seq;
            LONG n = InterlockedIncrement(&seq);
            for (const char *l = data, *end = data + len; l < end; ) {
                const char *nl = memchr(l, '\n', (size_t)(end - l));
                size_t ll = nl ? (size_t)(nl - l) : (size_t)(end - l);
                LOGW("maps-dump[%ld] %.*s", (long)n, (int)ll, l);
                l += ll + 1;
            }
        }
    }
    memfile *m = (memfile *)calloc(1, sizeof *m);
    m->data = data; m->len = len;
    if (!strcmp(strrchr(norm, '/') + 1, "comm") && strncmp(norm, "/proc/1/", 8)) m->comm_tid = e ? -1 : num;
    xfile *f = xfile_new(&g_mf_ops, flags & LX_O_ACCMODE);
    f->priv = m;
    f->gpath = _strdup(gabs);
    return f;
}

xfile *xrl_proc_open(const char *gabs, int flags, int *err)
{
    xfile *f = proc_open(gabs, flags, err);
    norm_leave();
    return f;
}

static slong proc_readlink(const char *gabs, char *buf, u64 cap)
{
    char norm[512];
    {   int rp; const char *rl;
        if (remote_path(gabs, &rp, &rl) && !strcmp(rl, "exe")) {
            size_t n; char *d = xrl_task_remote_proc(rp, rl, &n);
            if (!d) return -LX_ENOENT;
            if (n > cap) n = (size_t)cap;
            memcpy(buf, d, n); free(d);
            return (slong)n;
        }
    }
    normalize(gabs, norm, sizeof norm);
    char tmp[2048];
    tmp[0] = 0;
    if (!strcmp(norm, "/proc/1/exe") || !strcmp(norm, "/proc/1/cwd") || !strcmp(norm, "/proc/1/root")) return -LX_EACCES;
    xrl_proc *tp = tgt();
    if (!strcmp(norm, "/proc/self/exe")) snprintf(tmp, sizeof tmp, "%s", tp->exec_path && tp->exec_path[0] ? tp->exec_path : "/bin/wine");
    else if (!strcmp(norm, "/proc/self/cwd")) { AcquireSRWLockShared(&G.cwd_lock); snprintf(tmp, sizeof tmp, "%s", tp->cwd); ReleaseSRWLockShared(&G.cwd_lock); }
    else if (!strcmp(norm, "/proc/self/root")) snprintf(tmp, sizeof tmp, "/");
    else if (!strncmp(norm, "/proc/self/fd/", 14)) {
        xfile *f = xrl_files_get(tp->files, atoi(norm + 14));
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

slong xrl_proc_readlink(const char *gabs, char *buf, u64 cap)
{
    slong r = proc_readlink(gabs, buf, cap);
    norm_leave();
    return r;
}

/* Returns 1 if handled. st->st_nlink == 0 on return means "handled: ENOENT". */
static int proc_stat(const char *gabs, lx_stat *st)
{
    if (strncmp(gabs, "/proc", 5) && strncmp(gabs, "/sys", 4)) return 0;
    {   int rp; const char *rl;
        if (remote_path(gabs, &rp, &rl)) {
            memset(st, 0, sizeof *st);
            size_t n; char *d = xrl_task_remote_proc(rp, rl, &n);
            if (!d) return 1;   /* ENOENT */
            free(d);
            xrl_stat_synth(st, !strcmp(rl, "exe") ? (LX_S_IFLNK | 0777) : (LX_S_IFREG | 0444), (u64)rp, 0);
            return 1;
        }
    }
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

int xrl_proc_stat(const char *gabs, lx_stat *st)
{
    int r = proc_stat(gabs, st);
    norm_leave();
    return r;
}

int xrl_proc_is_virtual(const char *gabs) { return !strncmp(gabs, "/proc", 5) || !strncmp(gabs, "/sys", 4); }
