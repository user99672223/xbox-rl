/*
 * rewrite.c - load-time rewriting of foreign ELF images (docs/decisions.md,
 * "Easy Anti-Cheat" entry).
 *
 * The guest is Wine's unix side on our patched musl: it never executes a
 * `syscall` instruction and keeps its thread pointer in a TEB slot. A prebuilt
 * Linux binary (Rocket League's closed easyanticheat_x64.so, built against
 * glibc; later any Linux program) has both. Every file-backed ELF mapping the
 * layer sees becoming executable - its own loader for the main program and
 * interpreter, and mmap(PROT_EXEC) for everything dlopen'ed afterwards - is
 * passed through rewrite_core.c before a guest instruction of it runs:
 * `syscall` jumps to a stub that calls xrl_syscall through rawsys.asm, and each
 * `%fs:` operand is re-based on the thread pointer in %gs:XRL_TP_GS_OFFSET.
 * Images our own build verified clean (tools/xrl-tpfix.py stamps e_flags) are
 * skipped, so Wine's unix side costs nothing.
 *
 * What is patched is what the ELF says is code: the SHF_EXECINSTR sections, or
 * the executable PT_LOAD segments when there are no section headers; function
 * starts come from .eh_frame_hdr so a desynchronised decode is bounded.
 * Instructions that cannot be moved are replaced by int3 and redirected by the
 * small vectored handler below.
 *
 * Knobs: XRL_NO_REWRITE=1 disables; XRL_REWRITE_TRACE=1 logs every trap site.
 */
#include "xrl_internal.h"
#include "rewrite_core.h"

extern void xrl_rawsys(void);      /* rawsys.asm */

#pragma pack(push, 1)
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } REhdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } RPhdr;
typedef struct { u32 name, type; u64 flags, addr, offset, size; u32 link, info; u64 addralign, entsize; } RShdr;
#pragma pack(pop)
#define R_PT_LOAD 1
#define R_PT_GNU_EH_FRAME 0x6474e550
#define R_PF_X 1
#define R_SHF_EXECINSTR 4
#define R_SHT_PROGBITS 1

static SRWLOCK g_rw_lock = SRWLOCK_INIT;
static int g_rw_off = -1, g_rw_trace;

/* ----------------------------------------------------------- trap sites */
#define TRAP_BITS 17
#define TRAP_SLOTS (1u << TRAP_BITS)
typedef struct trapslot { volatile LONG64 site; LONG64 stub; } trapslot;
static trapslot g_trap0[TRAP_SLOTS];
static trapslot *volatile g_trap = g_trap0;   /* replaced whole by trap_sweep; rw_veh reads it lock-free */
static PVOID g_rw_veh;
static LONG g_trap_n;

static u32 trap_hash(u64 site) { return (u32)((site * 0x9e3779b97f4a7c15ull) >> (64 - TRAP_BITS)); }

static void trap_put(trapslot *t, u64 site, u64 stub)
{
    for (u32 i = trap_hash(site);; i = (i + 1) & (TRAP_SLOTS - 1)) {
        if (t[i].site == 0) {
            t[i].stub = (LONG64)stub;
            InterlockedExchange64(&t[i].site, (LONG64)site);     /* publish after the stub */
            InterlockedIncrement(&g_trap_n);
            return;
        }
        if ((u64)t[i].site == site) { t[i].stub = (LONG64)stub; return; }
    }
}

/* Sites of images that are gone stay in the table (every process of a desktop session loads libc's 17 at an address
 * of its own): when it is half full, a new table keeps only the sites that are still mapped and still hold the int3.
 * The old one is left as it is, a fault handler may be reading it. g_rw_lock held. */
static void trap_sweep(void)
{
    trapslot *old = g_trap, *nt = (trapslot *)calloc(TRAP_SLOTS, sizeof *nt);
    if (!nt) return;
    LONG before = g_trap_n;
    g_trap_n = 0;
    for (u32 i = 0; i < TRAP_SLOTS; i++) {
        u64 site = (u64)old[i].site; u8 b = 0;
        if (site && xrl_mem_is_mapped(site) && !xrl_cfu(&b, site, 1) && b == 0xcc) trap_put(nt, site, (u64)old[i].stub);
    }
    InterlockedExchangePointer((PVOID volatile *)&g_trap, nt);
    LOGI("rewrite: trap table swept: %ld sites -> %ld still mapped", (long)before, (long)g_trap_n);
}

static void trap_add(u64 site, u64 stub)
{
    if (g_trap_n >= (LONG)(TRAP_SLOTS / 2)) trap_sweep();
    if (g_trap_n >= (LONG)(TRAP_SLOTS / 2)) { LOGE("rewrite: trap table full, site %llx will not be redirected", site); return; }
    trap_put(g_trap, site, stub);
}

/* Runs wherever the guest faulted, with the guest TEB view live: touches only static memory. */
static LONG CALLBACK rw_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    u64 site = (u64)ep->ExceptionRecord->ExceptionAddress;
    trapslot *t = g_trap;
    for (u32 i = trap_hash(site);; i = (i + 1) & (TRAP_SLOTS - 1)) {
        u64 k = (u64)t[i].site;
        if (k == 0) return EXCEPTION_CONTINUE_SEARCH;
        if (k == site) { ep->ContextRecord->Rip = (DWORD64)t[i].stub; return EXCEPTION_CONTINUE_EXECUTION; }
    }
}

/* ------------------------------------------------------------- helpers */
static void rw_log(const char *fmt, ...)
{
    char buf[400];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    LOGW("%s", buf);
}

static int pread_all(xfile *f, void *dst, u64 n, s64 off)
{
    u64 done = 0;
    while (done < n) {
        slong r = f->ops->read(f, (char *)dst + done, n - done, off + (s64)done);
        if (r <= 0) return -1;
        done += (u64)r;
    }
    return 0;
}

typedef struct crange { u64 mem, size; } crange;      /* guest address, bytes */

/* the file offset of a link-time address, via PT_LOAD */
static int vaddr_to_off(const RPhdr *ph, int n, u64 v, u64 *off)
{
    for (int i = 0; i < n; i++)
        if (ph[i].type == R_PT_LOAD && v >= ph[i].vaddr && v < ph[i].vaddr + ph[i].filesz) { *off = ph[i].offset + (v - ph[i].vaddr); return 0; }
    return -1;
}

static int cmp_u64(const void *a, const void *b) { u64 x = *(const u64 *)a, y = *(const u64 *)b; return x < y ? -1 : x > y; }

/* The rewrite cache (XRL_SHARED_TEXT): a segment rewritten once with its pool at the fixed slot below its image
 * (mem.c xrl_mem_pool_slot) has the same rewritten bytes in every process, and they live in a shared section. The
 * next process that maps it gets that section mapped as is and the pool from the first one's template, with the
 * absolute jumps back into the code (xrw.abs) and the int3 trap sites moved by the image's distance: no decode, no
 * dry run, no rewrite. Never with XRL_FORK_TASKS. */
typedef struct rwcache {
    char path[160]; u64 ino, size, mtime; s64 off; u64 len;
    u64 code, pool;              /* where the first one was (mapping start, pool) */
    size_t cap, used; u8 *tpl;   /* its pool bytes (used of cap) */
    u32 *abs; size_t nabs;
    xrw_trap *traps; size_t ntraps;
    xrw_stats st;
    struct rwcache *next;
} rwcache;
static rwcache *g_rwc;
static u64 g_rwc_bytes;
static rwcache *rwc_find(const lx_stat *st, const char *path, s64 off, u64 len)
{
    for (rwcache *c = g_rwc; c; c = c->next)
        if (c->ino == st->st_ino && c->size == (u64)st->st_size && c->mtime == (u64)st->st_mtim.tv_sec && c->off == off && c->len == len &&
            !strcmp(c->path, path)) return c;
    return NULL;
}
/* g_rw_lock held. 1 = the mapping now runs the cached rewrite */
static int rwc_apply(xfile *f, rwcache *c, u64 addr, u64 len, s64 off, int prot, const char *name)
{
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    u64 slot = xrl_mem_pool_slot(addr, c->cap);
    if (!slot || addr - slot != c->code - c->pool) return 0;   /* not the same place below the image: rewrite */
    slong pm = xrl_mem_mmap(slot, c->cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                            LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE | LX_MAP_FIXED_NOREPLACE, -1, 0);
    if (pm != (slong)slot) { if (pm >= 0) xrl_mem_munmap((u64)pm, c->cap); return 0; }
    u8 *pool = (u8 *)pm;
    u64 delta = addr - c->code;
    memcpy(pool, c->tpl, c->used);
    memset(pool + c->used, 0xcc, c->cap - c->used);
    for (size_t i = 0; i < c->nabs; i++) { u64 v; memcpy(&v, pool + c->abs[i], 8); v += delta; memcpy(pool + c->abs[i], &v, 8); }
    *(u64 *)pool = (u64)(uintptr_t)&xrl_rawsys;
    if (!xrl_mem_share_cached(f, addr, len, off, prot)) { xrl_mem_munmap(slot, c->cap); return 0; }   /* no live section: rewrite */
    FlushInstructionCache(GetCurrentProcess(), pool, c->cap);
    char pn[96]; const char *b = strrchr(name, '/');
    snprintf(pn, sizeof pn, "[xrl-pool %s]", b ? b + 1 : name);
    xrl_mem_rename_range(slot, slot + c->cap, pn);
    xrl_mem_mprotect(slot, c->cap, LX_PROT_READ | LX_PROT_EXEC);
    for (size_t i = 0; i < c->ntraps; i++) trap_add(c->traps[i].site + delta, c->traps[i].stub + delta);
    if (c->ntraps && !g_rw_veh) g_rw_veh = AddVectoredExceptionHandler(1, rw_veh);
    QueryPerformanceCounter(&q1);
    LOGI("rewrite: %s: %u syscall + %u %%fs + %u %%gs sites from the cache (shared code, pool %zu bytes at %llx) in %.1f ms",
         name, c->st.syscalls, c->st.fs_sites, c->st.gs_sites, c->used, (u64)pool, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
    return 1;
}

void xrl_rewrite_mapped(xfile *f, u64 addr, u64 len, s64 off, int prot)
{
    if (!(prot & LX_PROT_EXEC) || !f || !f->ops->read) return;
    if (g_rw_off < 0) {
        char v[8];
        g_rw_off = GetEnvironmentVariableA("XRL_NO_REWRITE", v, sizeof v) && v[0] == '1';
        g_rw_trace = GetEnvironmentVariableA("XRL_REWRITE_TRACE", v, sizeof v) && v[0] == '1';
    }
    if (g_rw_off) return;

    if (xrl_dbt_wanted_file(f->gpath)) {
        xrl_dbt_attach(f->gpath ? f->gpath : "?", addr, len);
        return;
    }

    REhdr eh;
    if (pread_all(f, &eh, sizeof eh, 0)) return;
    if (eh.ident[0] == 'M' && eh.ident[1] == 'Z') return;           /* a Windows image: not ours (dbt.c xrl_dbt_exec_hint) */
    if (memcmp(eh.ident, "\177ELF", 4) || eh.ident[4] != 2 || eh.ident[5] != 1 || eh.machine != 62) return;
    if (eh.flags == XRL_ELF_E_FLAGS_CLEAN) return;                 /* verified clean at build time */
    if (eh.phentsize != sizeof(RPhdr) || eh.phnum == 0 || eh.phnum > 128) return;

    AcquireSRWLockExclusive(&g_rw_lock);
    LARGE_INTEGER q0, qf; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    const char *name = f->gpath ? f->gpath : "?";
    lx_stat fst; int have_st = !xrl_fork_tasks_enabled() && f->gpath && f->ops->fstat && !f->ops->fstat(f, &fst);
    if (have_st) {
        rwcache *c = rwc_find(&fst, f->gpath, off, len);
        if (c && rwc_apply(f, c, addr, len, off, prot, name)) { ReleaseSRWLockExclusive(&g_rw_lock); return; }
    }
    int at_slot = 0;
    RPhdr *ph = (RPhdr *)malloc(eh.phnum * sizeof(RPhdr));
    crange *rg = NULL; int nrg = 0;
    u64 *starts = NULL; size_t nst = 0;
    u8 *hdr = NULL;
    RShdr *sh = NULL;
    if (!ph || pread_all(f, ph, eh.phnum * sizeof(RPhdr), (s64)eh.phoff)) goto out;

    /* code ranges: executable sections, else executable segments, clipped to this mapping */
    u64 mlo = (u64)off, mhi = (u64)off + len;
    rg = (crange *)calloc(256 + eh.phnum, sizeof *rg);
    if (!rg) goto out;
    if (eh.shoff && eh.shentsize == sizeof(RShdr) && eh.shnum && eh.shnum <= 2048) {
        sh = (RShdr *)malloc(eh.shnum * sizeof(RShdr));
        if (sh && pread_all(f, sh, eh.shnum * sizeof(RShdr), (s64)eh.shoff)) { free(sh); sh = NULL; }
    }
    if (sh) {
        for (int i = 0; i < eh.shnum && nrg < 256; i++) {
            if (!(sh[i].flags & R_SHF_EXECINSTR) || sh[i].type != R_SHT_PROGBITS || !sh[i].size) continue;
            u64 lo = sh[i].offset > mlo ? sh[i].offset : mlo, hi = sh[i].offset + sh[i].size < mhi ? sh[i].offset + sh[i].size : mhi;
            if (lo < hi) { rg[nrg].mem = addr + (lo - mlo); rg[nrg].size = hi - lo; nrg++; }
        }
    } else {
        for (int i = 0; i < eh.phnum; i++) {
            if (ph[i].type != R_PT_LOAD || !(ph[i].flags & R_PF_X)) continue;
            u64 lo = ph[i].offset > mlo ? ph[i].offset : mlo, hi = ph[i].offset + ph[i].filesz < mhi ? ph[i].offset + ph[i].filesz : mhi;
            if (lo < hi) { rg[nrg].mem = addr + (lo - mlo); rg[nrg].size = hi - lo; nrg++; }
        }
        if (nrg) LOGW("rewrite: %s has no section headers: scanning whole executable segments", name);
    }
    if (!nrg) goto out;

    /* function starts from .eh_frame_hdr, as guest addresses inside this mapping */
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != R_PT_GNU_EH_FRAME || ph[i].filesz < 12 || ph[i].filesz > (32u << 20)) continue;
        hdr = (u8 *)malloc((size_t)ph[i].filesz);
        if (!hdr || pread_all(f, hdr, ph[i].filesz, (s64)ph[i].offset)) break;
        size_t n = 0;
        u64 *v = xrw_eh_starts(hdr, (size_t)ph[i].filesz, ph[i].vaddr, &n);
        size_t k = 0;
        for (size_t j = 0; j < n; j++) {
            u64 fo;
            if (vaddr_to_off(ph, eh.phnum, v[j], &fo) || fo < mlo || fo >= mhi) continue;
            v[k++] = addr + (fo - mlo);
        }
        qsort(v, k, sizeof *v, cmp_u64);
        starts = v; nst = k;
        break;
    }

    /* dry run: how big a trampoline pool? */
    xrw dry; memset(&dry, 0, sizeof dry);
    dry.tp_gs_off = XRL_TP_GS_OFFSET; dry.gs_slot_off = xrl_gs_slot_off(); dry.log = rw_log;
    dry.pool_vaddr = rg[0].mem;
    for (int i = 0; i < nrg; i++) xrw_rewrite_range(&dry, (u8 *)rg[i].mem, rg[i].mem, (size_t)rg[i].size, starts, nst);
    if (!dry.st.syscalls && !dry.st.fs_sites && !dry.st.gs_sites) {
        if (dry.st.decode_fail) LOGW("rewrite: %s: %u undecodable region(s), nothing to rewrite elsewhere", name, dry.st.decode_fail);
        goto out;
    }

    size_t cap = (dry.pool_used + 64 + 4095) & ~(size_t)4095;
    /* near the code: a stub is reached by jmp rel32, so a pool more than ~2 GiB away makes every site an int3 trap.
     * The default placement is the highest free gap, which in a long session (holes left by ended processes) can be
     * far above the image: try the space just below the image first, stepping down. */
    slong pm = -LX_ENOMEM;
    {   /* XRL_SHARED_TEXT: at the same offset below the image in every process, so the rewritten code (which reaches
         * the pool by rel32) is the same bytes everywhere and can be one shared section (mem.c) */
        u64 slot = xrl_fork_tasks_enabled() ? 0 : xrl_mem_pool_slot(rg[0].mem, cap);
        if (slot) {
            slong r = xrl_mem_mmap(slot, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                                   LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE | LX_MAP_FIXED_NOREPLACE, -1, 0);
            if (r == (slong)slot) { pm = r; at_slot = 1; }
            else if (r >= 0) xrl_mem_munmap((u64)r, cap);
        }
    }
    for (int tries = 0; tries < 4 && pm < 0 && !xrl_fork_tasks_enabled(); tries++) {   /* the EAC path keeps its placement */
        u64 hint = xrl_mem_find_near(cap, rg[0].mem, 0x70000000ull - (u64)cap - (u64)rg[nrg - 1].size);
        if (!hint) break;
        slong r = xrl_mem_mmap(hint, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                               LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE, -1, 0);
        if (r == (slong)hint) pm = r;
        else if (r >= 0) xrl_mem_munmap((u64)r, cap);   /* taken meanwhile: look again */
    }
    if (pm < 0) pm = xrl_mem_mmap(0, cap, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC,
                                  LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_POPULATE, -1, 0);
    if (pm < 0) { LOGE("rewrite: %s: cannot allocate %zu bytes for the trampoline pool (%lld); image left as is", name, cap, pm); goto out; }
    u8 *pool = (u8 *)pm;
    if (!xrl_fork_tasks_enabled()) {   /* named after its image in /proc/self/maps and backtraces (EAC's maps stay as they were) */
        char pn[96]; const char *b = strrchr(name, '/');
        snprintf(pn, sizeof pn, "[xrl-pool %s]", b ? b + 1 : name);
        xrl_mem_rename_range((u64)pool, (u64)pool + cap, pn);
    }
    memset(pool, 0xcc, cap);
    u64 dlo = (u64)pool > rg[0].mem ? (u64)pool - rg[0].mem : rg[0].mem - (u64)pool;
    if (dlo > 0x70000000ull) LOGW("rewrite: %s: trampoline pool %llx is far from the code (%llx): every site becomes a trap", name, (u64)pool, rg[0].mem);

    xrw w; memset(&w, 0, sizeof w);
    w.tp_gs_off = XRL_TP_GS_OFFSET; w.gs_slot_off = dry.gs_slot_off; w.log = rw_log; w.verbose = g_rw_trace;
    w.pool = pool; w.pool_vaddr = (u64)pool; w.pool_cap = cap;
    DWORD olds[256]; int ok = 1;
    for (int i = 0; i < nrg; i++) {
        u64 a = ALIGN_DOWN(rg[i].mem, LX_PAGE), e = ALIGN_UP(rg[i].mem + rg[i].size, LX_PAGE);
        if (xrl_host_protect((void *)a, (size_t)(e - a), PAGE_EXECUTE_READWRITE, &olds[i])) { LOGE("rewrite: %s: cannot make code writable", name); ok = 0; nrg = i; break; }
    }
    if (ok) {
        for (int i = 0; i < nrg; i++)
            if (xrw_rewrite_range(&w, (u8 *)rg[i].mem, rg[i].mem, (size_t)rg[i].size, starts, nst)) { LOGE("rewrite: %s: trampoline pool too small", name); ok = 0; break; }
    }
    for (int i = 0; i < nrg; i++) {
        u64 a = ALIGN_DOWN(rg[i].mem, LX_PAGE), e = ALIGN_UP(rg[i].mem + rg[i].size, LX_PAGE);
        DWORD tmp;
        xrl_host_protect((void *)a, (size_t)(e - a), olds[i], &tmp);
        FlushInstructionCache(GetCurrentProcess(), (void *)a, (SIZE_T)(e - a));
    }
    *(u64 *)pool = (u64)(uintptr_t)&xrl_rawsys;
    FlushInstructionCache(GetCurrentProcess(), pool, cap);
    /* nothing writes a pool after this: read+exec, so a stray write (another guest process's, in the shared address
     * space) faults at the writer instead of turning trampolines into data. Not with XRL_FORK_TASKS (Rocket League
     * through EAC keeps its pools as they always were). */
    if (!xrl_fork_tasks_enabled()) xrl_mem_mprotect((u64)pool, cap, LX_PROT_READ | LX_PROT_EXEC);
    if (w.ntraps) {
        for (size_t i = 0; i < w.ntraps; i++) trap_add(w.traps[i].site, w.traps[i].stub);
        if (!g_rw_veh) g_rw_veh = AddVectoredExceptionHandler(1, rw_veh);
    }
    LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
    LOGI("rewrite: %s: %u syscall + %u %%fs + %u %%gs sites (%u in place, %u jumps, %u traps, %u neighbours moved), pool %zu bytes at %llx%s in %.1f ms",
         name, w.st.syscalls, w.st.fs_sites, w.st.gs_sites, w.st.inplace, w.st.jumps, w.st.traps, w.st.moved, w.pool_used, (u64)pool, ok ? "" : " [INCOMPLETE]",
         (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart);
    if (w.st.unsupported || w.st.decode_fail || w.st.legacy_sys)
        LOGW("rewrite: %s: %u unsupported site(s), %u undecodable region(s), %u sysenter/int80 left alone", name, w.st.unsupported, w.st.decode_fail, w.st.legacy_sys);
    /* the cache: this rewrite at the fixed slot is what every later mapping of the segment will get (64 MiB of
     * templates at most) */
    if (ok && at_slot && have_st && w.abs_cap != (size_t)-1 && strlen(f->gpath) < 160 && g_rwc_bytes + w.pool_used < (64ull << 20) &&
        !rwc_find(&fst, f->gpath, off, len)) {
        rwcache *c = (rwcache *)calloc(1, sizeof *c);
        u8 *tpl = c ? (u8 *)malloc(w.pool_used) : NULL;
        u32 *abs = c && w.nabs ? (u32 *)malloc(w.nabs * sizeof *abs) : NULL;
        xrw_trap *tr = c && w.ntraps ? (xrw_trap *)malloc(w.ntraps * sizeof *tr) : NULL;
        if (c && tpl && (abs || !w.nabs) && (tr || !w.ntraps)) {
            strcpy_s(c->path, sizeof c->path, f->gpath);
            c->ino = fst.st_ino; c->size = (u64)fst.st_size; c->mtime = (u64)fst.st_mtim.tv_sec; c->off = off; c->len = len;
            c->code = addr; c->pool = (u64)pool; c->cap = cap; c->used = w.pool_used;
            memcpy(tpl, pool, w.pool_used); c->tpl = tpl;
            if (w.nabs) memcpy(abs, w.abs, w.nabs * sizeof *abs);
            c->abs = abs; c->nabs = w.nabs;
            for (size_t i = 0; i < w.ntraps; i++) { tr[i].site = w.traps[i].site; tr[i].stub = w.traps[i].stub; }
            c->traps = tr; c->ntraps = w.ntraps; c->st = w.st;
            c->next = g_rwc; g_rwc = c; g_rwc_bytes += w.pool_used;
        } else { free(c); free(tpl); free(abs); free(tr); }
    }
    free(w.traps); free(w.abs);
out:
    free(ph); free(rg); free(starts); free(hdr); free(sh);
    ReleaseSRWLockExclusive(&g_rw_lock);
}

void xrl_rewrite_fd_mapped(int fd, u64 addr, u64 len, s64 off, int prot)
{
    if (!(prot & LX_PROT_EXEC)) return;
    xfile *f = xrl_fd_get(fd);
    if (!f) return;
    xrl_rewrite_mapped(f, addr, len, off, prot);
    xfile_unref(f);
}
