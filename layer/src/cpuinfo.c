/*
 * cpuinfo.c - /proc/cpuinfo and the CPU topology, from the real CPU.
 *
 * CPUID is unprivileged, so a guest reads the hardware directly; /proc/cpuinfo
 * must say the same thing (EAC and plenty of Linux programs compare the two).
 * Everything here comes from CPUID, XGETBV and the Windows processor topology:
 * flag names and order follow Linux's arch/x86/include/asm/cpufeatures.h, and
 * a feature is listed only where Linux would list it (AVX state the OS has not
 * enabled is dropped, as the kernel does).
 *
 * If the hardware CPUID brand string is a non-standard name (e.g. a console
 * APU marketing string), both /proc/cpuinfo and the DBT's CPUID intercept
 * present a standard AMD model name built from CPUID family/model data, so
 * every program sees a consistent, faithful Linux identity.
 */
#include "xrl_internal.h"
#include <intrin.h>

/* 3 leaves × 4 regs × 4 bytes = 48 bytes; read by dbt.asm's cpuid handler */
int xrl_cpuid_brand[12];

/* ------------------------------------------------------------- topology */
static int g_core_of[64], g_ncores;
static INIT_ONCE g_topo_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK topo_init(PINIT_ONCE io, PVOID prm, PVOID *ctx)
{
    (void)io; (void)prm; (void)ctx;
    int n = xrl_nprocs();
    for (int i = 0; i < n; i++) g_core_of[i] = -1;
    DWORD sz = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &sz);
    BYTE *buf = sz ? (BYTE *)malloc(sz) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &sz)) {
        for (DWORD off = 0; off < sz;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            const GROUP_AFFINITY *ga = &e->Processor.GroupMask[0];
            if (ga->Group == 0) {
                int used = 0;
                for (int i = 0; i < n; i++)
                    if ((ga->Mask >> i) & 1) { g_core_of[i] = g_ncores; used = 1; }
                g_ncores += used;
            }
            off += e->Size;
        }
    }
    free(buf);
    for (int i = 0; i < n; i++) if (g_core_of[i] < 0) g_core_of[i] = g_ncores++;   /* unknown: its own core */
    return TRUE;
}

int xrl_cpu_core_id(int cpu)
{
    InitOnceExecuteOnce(&g_topo_once, topo_init, NULL, NULL);
    return cpu >= 0 && cpu < xrl_nprocs() ? g_core_of[cpu] : 0;
}

int xrl_cpu_ncores(void)
{
    InitOnceExecuteOnce(&g_topo_once, topo_init, NULL, NULL);
    return g_ncores;
}

/* "0,4" / "2-3": the logical CPUs sharing `cpu`'s core, in sysfs list syntax */
void xrl_cpu_siblings_list(int cpu, char *out, size_t cap)
{
    int core = xrl_cpu_core_id(cpu), n = xrl_nprocs();
    size_t w = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        if (g_core_of[i] != core) continue;
        int j = i;
        while (j + 1 < n && g_core_of[j + 1] == core) j++;
        w += (size_t)snprintf(out + w, w < cap ? cap - w : 0, j > i ? "%s%d-%d" : "%s%d", w ? "," : "", i, j);
        i = j;
    }
    if (w + 2 <= cap) { out[w] = '\n'; out[w + 1] = 0; }
}

/* ---------------------------------------------------------------- flags */
/* One table per CPUID register Linux maps to a feature word, indexed by bit.
 * NULL = no /proc name (Linux's "" entries, or a bit Linux does not define). */
typedef const char *const bitnames[32];

static bitnames f1_edx = { "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce", "cx8", "apic", NULL, "sep", "mtrr",
    "pge", "mca", "cmov", "pat", "pse36", "pn", "clflush", NULL, "dts", "acpi", "mmx", "fxsr", "sse", "sse2", "ss",
    "ht", "tm", "ia64", "pbe" };
static bitnames e1_edx = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "syscall", NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, "mp", "nx", NULL, "mmxext", NULL, NULL, "fxsr_opt", "pdpe1gb", "rdtscp", NULL, "lm",
    "3dnowext", "3dnow" };
static bitnames f1_ecx = { "pni", "pclmulqdq", "dtes64", "monitor", "ds_cpl", "vmx", "smx", "est", "tm2", "ssse3",
    "cid", "sdbg", "fma", "cx16", "xtpr", "pdcm", NULL, "pcid", "dca", "sse4_1", "sse4_2", "x2apic", "movbe", "popcnt",
    "tsc_deadline_timer", "aes", "xsave", NULL, "avx", "f16c", "rdrand", "hypervisor" };
static bitnames e1_ecx = { "lahf_lm", "cmp_legacy", "svm", "extapic", "cr8_legacy", "abm", "sse4a", "misalignsse",
    "3dnowprefetch", "osvw", "ibs", "xop", "skinit", "wdt", NULL, "lwp", "fma4", "tce", NULL, "nodeid_msr", NULL, "tbm",
    "topoext", "perfctr_core", "perfctr_nb", NULL, "bpext", "ptsc", "perfctr_llc", "mwaitx", NULL, NULL };
static bitnames s7_ebx = { "fsgsbase", "tsc_adjust", "sgx", "bmi1", "hle", "avx2", NULL, "smep", "bmi2", "erms",
    "invpcid", "rtm", "cqm", NULL, "mpx", "rdt_a", "avx512f", "avx512dq", "rdseed", "adx", "smap", "avx512ifma", NULL,
    "clflushopt", "clwb", "intel_pt", "avx512pf", "avx512er", "avx512cd", "sha_ni", "avx512bw", "avx512vl" };
static bitnames xd_eax = { "xsaveopt", "xsavec", "xgetbv1", "xsaves" };
static bitnames s71_eax = { NULL, NULL, NULL, NULL, "avx_vnni", "avx512_bf16" };
static bitnames e8_ebx = { "clzero", "irperf", "xsaveerptr", NULL, "rdpru", NULL, NULL, NULL, NULL, "wbnoinvd" };
static bitnames s6_eax = { "dtherm", "ida", "arat", NULL, "pln", NULL, "pts", "hwp", "hwp_notify", "hwp_act_window",
    "hwp_epp", "hwp_pkg_req" };
/* PKU/OSPKE (bits 3/4) are left out: the layer has no pkey_* syscalls */
static bitnames s7_ecx = { NULL, "avx512vbmi", "umip", NULL, NULL, "waitpkg", "avx512_vbmi2", NULL, "gfni", "vaes",
    "vpclmulqdq", "avx512_vnni", "avx512_bitalg", "tme", "avx512_vpopcntdq", NULL, "la57", NULL, NULL, NULL, NULL,
    NULL, "rdpid", NULL, "bus_lock_detect", "cldemote", NULL, "movdiri", "movdir64b", "enqcmd", "sgx_lc", NULL };
static bitnames s7_edx = { NULL, NULL, "avx512_4vnniw", "avx512_4fmaps", "fsrm", NULL, NULL, NULL,
    "avx512_vp2intersect", NULL, "md_clear", NULL, NULL, NULL, "serialize", NULL, "tsxldtrk", NULL, "pconfig",
    "arch_lbr", "ibt", NULL, "amx_bf16", "avx512_fp16", "amx_tile", "amx_int8", NULL, NULL, "flush_l1d",
    "arch_capabilities", NULL, NULL };

/* Linux drops these when XCR0 lacks the state they need (fpu_xstate / cpuid deps) */
static int needs_ymm(const char *f)
{
    return !strcmp(f, "avx") || !strcmp(f, "avx2") || !strcmp(f, "fma") || !strcmp(f, "f16c") ||
           !strcmp(f, "vaes") || !strcmp(f, "vpclmulqdq") || !strcmp(f, "avx_vnni") || !strcmp(f, "fma4") ||
           !strcmp(f, "xop");
}
static int needs_zmm(const char *f) { return !strncmp(f, "avx512", 6); }
static int needs_amx(const char *f) { return !strncmp(f, "amx_", 4); }

typedef struct sbuf { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        b->cap = (b->n + l + 1) * 2;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static void add_bits(sbuf *b, bitnames t, u32 reg, u64 xcr0)
{
    for (int i = 0; i < 32; i++) {
        if (!((reg >> i) & 1) || !t[i]) continue;
        if (needs_ymm(t[i]) && (xcr0 & 6) != 6) continue;
        if (needs_zmm(t[i]) && (xcr0 & 0xe6) != 0xe6) continue;
        if (needs_amx(t[i]) && (xcr0 & 0x60000) != 0x60000) continue;
        sb_put(b, " ");
        sb_put(b, t[i]);
    }
}

/* -------------------------------------------------------------- the file */
typedef struct cpu_desc {
    char vendor[13], model[49];
    unsigned family, model_no, stepping, level, clflush, phys_bits, virt_bits, cache_kb;
    double mhz;
    char *flags;
} cpu_desc;
static cpu_desc g_cpu;
static INIT_ONCE g_cpu_once = INIT_ONCE_STATIC_INIT;

/* The TSC rate (the base clock on any CPU with an invariant TSC), measured
 * against QPC once: there is no CPUID leaf for it on AMD. */
static double tsc_mhz(void)
{
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    u64 t0 = __rdtsc();
    do QueryPerformanceCounter(&q1); while ((q1.QuadPart - q0.QuadPart) * 50 < f.QuadPart);   /* 20 ms */
    u64 t1 = __rdtsc();
    double secs = (double)(q1.QuadPart - q0.QuadPart) / (double)f.QuadPart;
    return (double)(t1 - t0) / secs / 1e6;
}

static BOOL CALLBACK cpu_init(PINIT_ONCE io, PVOID prm, PVOID *ctx)
{
    (void)io; (void)prm; (void)ctx;
    cpu_desc *c = &g_cpu;
    int r[4];
    __cpuid(r, 0);
    c->level = (unsigned)r[0];
    memcpy(c->vendor, &r[1], 4); memcpy(c->vendor + 4, &r[3], 4); memcpy(c->vendor + 8, &r[2], 4);
    int f1[4] = {0};
    __cpuid(f1, 1);
    unsigned base_fam = ((unsigned)f1[0] >> 8) & 0xf, base_mod = ((unsigned)f1[0] >> 4) & 0xf;
    c->stepping = (unsigned)f1[0] & 0xf;
    c->family = base_fam == 0xf ? base_fam + (((unsigned)f1[0] >> 20) & 0xff) : base_fam;
    c->model_no = base_fam == 0xf || base_fam == 6 ? base_mod | ((((unsigned)f1[0] >> 16) & 0xf) << 4) : base_mod;
    c->clflush = (((unsigned)f1[1] >> 8) & 0xff) * 8;

    __cpuid(r, 0x80000000);
    unsigned ext = (unsigned)r[0];
    int e1[4] = {0}, e8[4] = {0};
    if (ext >= 0x80000001) __cpuid(e1, 0x80000001);
    if (ext >= 0x80000004) {
        int m[12];
        __cpuid(m, 0x80000002); __cpuid(m + 4, 0x80000003); __cpuid(m + 8, 0x80000004);
        memcpy(c->model, m, 48);
        char *s = c->model;
        while (*s == ' ') s++;
        memmove(c->model, s, strlen(s) + 1);
        for (size_t l = strlen(c->model); l && c->model[l - 1] == ' '; l--) c->model[l - 1] = 0;
        if (_strnicmp(c->model, "AMD", 3) && _strnicmp(c->model, "Intel", 5)) {
            snprintf(c->model, sizeof c->model, "AMD Custom APU %02X%02X",
                     c->family, c->model_no);
        }
    }
    memset(xrl_cpuid_brand, 0, sizeof xrl_cpuid_brand);
    memcpy(xrl_cpuid_brand, c->model, min(48, strlen(c->model)));
    if (ext >= 0x80000008) {
        __cpuid(e8, 0x80000008);
        c->phys_bits = (unsigned)e8[0] & 0xff;
        c->virt_bits = ((unsigned)e8[0] >> 8) & 0xff;
    }
    /* cache size: Linux shows the last-level cache on Intel (leaf 4), L2 on AMD */
    if (!strcmp(c->vendor, "GenuineIntel") && c->level >= 4) {
        for (int i = 0; i < 16; i++) {
            __cpuidex(r, 4, i);
            if (!(r[0] & 0x1f)) break;
            unsigned ways = (((unsigned)r[1] >> 22) & 0x3ff) + 1, parts = (((unsigned)r[1] >> 12) & 0x3ff) + 1;
            unsigned line = ((unsigned)r[1] & 0xfff) + 1, sets = (unsigned)r[2] + 1;
            c->cache_kb = (unsigned)((u64)ways * parts * line * sets >> 10);
        }
    } else if (ext >= 0x80000006) {
        __cpuid(r, 0x80000006);
        c->cache_kb = (unsigned)r[2] >> 16;
    }
    int inv_tsc = 0;
    if (ext >= 0x80000007) { __cpuid(r, 0x80000007); inv_tsc = (r[3] >> 8) & 1; }
    c->mhz = tsc_mhz();

    u64 xcr0 = ((unsigned)f1[2] >> 27) & 1 ? _xgetbv(0) : 0;   /* OSXSAVE */
    sbuf b = {0};
    sb_put(&b, "");
    add_bits(&b, f1_edx, (u32)f1[3], xcr0);
    add_bits(&b, e1_edx, (u32)e1[3], xcr0);
    if (inv_tsc) sb_put(&b, " constant_tsc nonstop_tsc");
    sb_put(&b, " cpuid");
    add_bits(&b, f1_ecx, (u32)f1[2], xcr0);
    add_bits(&b, e1_ecx, (u32)e1[2], xcr0);
    int s7[4] = {0}, s71[4] = {0}, xd[4] = {0}, s6[4] = {0};
    if (c->level >= 7) { __cpuidex(s7, 7, 0); if (s7[0] >= 1) __cpuidex(s71, 7, 1); }
    if (c->level >= 0xd) __cpuidex(xd, 0xd, 1);
    if (c->level >= 6) __cpuid(s6, 6);
    add_bits(&b, s7_ebx, (u32)s7[1], xcr0);
    add_bits(&b, xd_eax, (u32)xd[0], xcr0);
    add_bits(&b, s71_eax, (u32)s71[0], xcr0);
    add_bits(&b, e8_ebx, (u32)e8[1], xcr0);
    add_bits(&b, s6_eax, (u32)s6[0], xcr0);
    add_bits(&b, s7_ecx, (u32)s7[2], xcr0);
    add_bits(&b, s7_edx, (u32)s7[3], xcr0);
    c->flags = b.p + 1;   /* skip the leading space */
    return TRUE;
}

char *xrl_cpuinfo_text(size_t *len)
{
    InitOnceExecuteOnce(&g_cpu_once, cpu_init, NULL, NULL);
    const cpu_desc *c = &g_cpu;
    int n = xrl_nprocs(), cores = xrl_cpu_ncores();
    sbuf b = {0};
    char line[512];
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof line,
                 "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n"
                 "cpu MHz\t\t: %.3f\n",
                 i, c->vendor, c->family, c->model_no, c->model, c->stepping, c->mhz);
        sb_put(&b, line);
        if (c->cache_kb) { snprintf(line, sizeof line, "cache size\t: %u KB\n", c->cache_kb); sb_put(&b, line); }
        snprintf(line, sizeof line,
                 "physical id\t: 0\nsiblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\napicid\t\t: %d\ninitial apicid\t: %d\n"
                 "fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\nflags\t\t: ",
                 n, xrl_cpu_core_id(i), cores, i, i, c->level);
        sb_put(&b, line);
        sb_put(&b, c->flags);
        snprintf(line, sizeof line,
                 "\nbogomips\t: %.2f\nclflush size\t: %u\ncache_alignment\t: %u\naddress sizes\t: %u bits physical, %u bits virtual\n"
                 "power management:\n\n",
                 c->mhz * 2, c->clflush, c->clflush, c->phys_bits, c->virt_bits);
        sb_put(&b, line);
    }
    *len = b.n;
    return b.p;
}
