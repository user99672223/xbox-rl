/*
 * mem.c - Linux mmap/munmap/mprotect/mremap/brk/madvise over VirtualAlloc.
 *
 * Model
 *   * The guest shares the host process's 47-bit address space. Guest "pages"
 *     are 4 KiB (same as Windows pages); Windows reservation granularity is
 *     64 KiB.
 *   * A single big arena is reserved at boot (MEM_RESERVE, top-down). Address-
 *     less mmaps are carved out of it by our own VMA bookkeeping (top-down, like
 *     Linux). The low part of the arena is the brk heap.
 *   * MAP_FIXED / hinted mappings outside the arena (Wine places PE images at
 *     their preferred bases, e.g. 0x140000000) reserve the covering 64 KiB
 *     granules individually; granules are released when no VMA touches them.
 *   * The VMA list is the single source of truth for what the guest sees. A
 *     PROT_NONE anonymous mapping stays reserved-only (no commit charge). A
 *     later mprotect to an accessible protection commits it. mprotect(PROT_NONE)
 *     on committed pages keeps them committed (data survives, as on Linux).
 *   * guest_mem_budget bounds the *committed* bytes (the Windows commit
 *     charge). Exceeding it returns -ENOMEM.
 *   * File mappings are copy-in: contents are read() into committed anonymous
 *     pages. MAP_SHARED|PROT_WRITE file mappings are written back on
 *     munmap/msync. This is sufficient for Wine's PE mapping and read-only
 *     sections; true coherent shared file mappings are NOT implemented.
 */
#include "xrl_internal.h"

#define GRAN 0x10000ull

typedef struct vma {
    u64 start, end;
    int prot;               /* LX_PROT_* */
    int committed;          /* Windows pages are committed */
    int shared;             /* MAP_SHARED */
    xfile *file;            /* write-back target for shared writable file maps */
    s64 foff;
    char name[40];
} vma;

static SRWLOCK g_lock = SRWLOCK_INIT;
static vma *g_v;
static size_t g_n, g_cap;

static u64 g_arena_base, g_arena_size, g_arena_end;
static u64 g_brk_base, g_brk_limit, g_brk_cur;
static u64 g_budget;
static volatile LONG64 g_committed;
static u64 g_mmap_floor;

/* individually reserved 64K granules outside the arena (sorted) */
static u64 *g_gr;
static size_t g_gn, g_gcap;

/* ------------------------------------------------------------- Windows */
static DWORD winprot(int prot, int *exec_out)
{
    int r = prot & LX_PROT_READ, w = prot & LX_PROT_WRITE, x = prot & LX_PROT_EXEC;
    (void)r;
    if (exec_out) *exec_out = x;
    if (!prot) return PAGE_NOACCESS;
    if (x) return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return w ? PAGE_READWRITE : PAGE_READONLY;
}

/* In the UWP flavour the WINAPI_FAMILY_APP headers define VirtualAlloc and
 * VirtualProtect as inline wrappers over the *FromApp variants. Referencing them
 * emits that body into this object, which then collides with kernel32.lib's
 * real export in the host link (LNK2005), and would silently give us the
 * FromApp rules. The host links kernel32.lib, and the console probe showed the
 * real VirtualAlloc grants RWX under codeGeneration, so call the real exports
 * through their import slots. */
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP && !XRL_UWP_FROMAPP
extern LPVOID (WINAPI *__imp_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
extern BOOL (WINAPI *__imp_VirtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);
#define XRL_VIRTUAL_ALLOC   __imp_VirtualAlloc
#define XRL_VIRTUAL_PROTECT __imp_VirtualProtect
#else
#define XRL_VIRTUAL_ALLOC   VirtualAlloc
#define XRL_VIRTUAL_PROTECT VirtualProtect
#endif

static void *w_alloc(void *addr, u64 len, DWORD type, DWORD prot)
{
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP && XRL_UWP_FROMAPP
    return VirtualAllocFromApp(addr, (SIZE_T)len, type, prot);
#else
    return XRL_VIRTUAL_ALLOC(addr, (SIZE_T)len, type, prot);
#endif
}

static BOOL w_protect(void *addr, u64 len, DWORD prot, DWORD *old)
{
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP && XRL_UWP_FROMAPP
    return VirtualProtectFromApp(addr, (SIZE_T)len, prot, old);
#else
    return XRL_VIRTUAL_PROTECT(addr, (SIZE_T)len, prot, old);
#endif
}

/* Apply `op` over [s,e) split on Windows allocation boundaries. */
typedef int (*region_fn)(u64 s, u64 e, void *ctx);
static int each_region(u64 s, u64 e, region_fn fn, void *ctx)
{
    while (s < e) {
        MEMORY_BASIC_INFORMATION mbi;
        u64 ce = e;
        if (VirtualQuery((void *)s, &mbi, sizeof mbi)) {
            u64 rend = (u64)mbi.BaseAddress + mbi.RegionSize;
            if (rend < ce) ce = rend;
            if (ce <= s) ce = e;
        }
        int r = fn(s, ce, ctx);
        if (r) return r;
        s = ce;
    }
    return 0;
}

static int op_commit(u64 s, u64 e, void *ctx)
{
    DWORD p = *(DWORD *)ctx;
    return w_alloc((void *)s, e - s, MEM_COMMIT, p) ? 0 : (int)GetLastError();
}
static int op_protect(u64 s, u64 e, void *ctx)
{
    DWORD p = *(DWORD *)ctx, old;
    return w_protect((void *)s, e - s, p, &old) ? 0 : (int)GetLastError();
}
static int op_decommit(u64 s, u64 e, void *ctx)
{
    (void)ctx;
    VirtualFree((void *)s, (SIZE_T)(e - s), MEM_DECOMMIT);
    return 0;
}

static slong win_commit(u64 s, u64 e, int prot)
{
    DWORD p = winprot(prot, NULL);
    int r = each_region(s, e, op_commit, &p);
    if (r) {
        LOGW("mem: commit [%llx,%llx) prot=%d failed, win err %d", s, e, prot, r);
        return r == ERROR_COMMITMENT_LIMIT || r == ERROR_NOT_ENOUGH_MEMORY || r == ERROR_OUTOFMEMORY ? -LX_ENOMEM : -LX_EINVAL;
    }
    return 0;
}
static slong win_protect(u64 s, u64 e, int prot)
{
    DWORD p = winprot(prot, NULL);
    int r = each_region(s, e, op_protect, &p);
    if (r) {
        LOGW("mem: protect [%llx,%llx) prot=%d failed, win err %d", s, e, prot, r);
        return -LX_EINVAL;
    }
    return 0;
}
static void win_decommit(u64 s, u64 e) { each_region(s, e, op_decommit, NULL); }

/* -------------------------------------------------------- granule set */
static size_t gr_lower(u64 g)
{
    size_t lo = 0, hi = g_gn;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_gr[m] < g) lo = m + 1; else hi = m; }
    return lo;
}
static int gr_has(u64 g) { size_t i = gr_lower(g); return i < g_gn && g_gr[i] == g; }
static int gr_add(u64 g)
{
    size_t i = gr_lower(g);
    if (g_gn == g_gcap) {
        size_t nc = g_gcap ? g_gcap * 2 : 256;
        u64 *n = (u64 *)realloc(g_gr, nc * sizeof(u64));
        if (!n) return -1;
        g_gr = n; g_gcap = nc;
    }
    memmove(&g_gr[i + 1], &g_gr[i], (g_gn - i) * sizeof(u64));
    g_gr[i] = g; g_gn++;
    return 0;
}
static void gr_del(u64 g)
{
    size_t i = gr_lower(g);
    if (i < g_gn && g_gr[i] == g) { memmove(&g_gr[i], &g_gr[i + 1], (g_gn - i - 1) * sizeof(u64)); g_gn--; }
}

static int in_arena(u64 s, u64 e) { return s >= g_arena_base && e <= g_arena_end; }

/* Make sure Windows has [s,e) reserved by us. 0 ok, -1 fail. */
static int ensure_reserved(u64 s, u64 e)
{
    if (in_arena(s, e)) return 0;
    if (s >= g_arena_base && s < g_arena_end) return -1;   /* straddles arena edge */
    u64 gs = ALIGN_DOWN(s, GRAN), ge = ALIGN_UP(e, GRAN);
    u64 g;
    for (g = gs; g < ge; g += GRAN) {
        if (gr_has(g)) continue;
        if (!w_alloc((void *)g, GRAN, MEM_RESERVE, PAGE_NOACCESS) || gr_add(g)) {
            if (w_alloc((void *)g, 0, 0, 0)) {}
            /* rollback granules added in this call */
            for (u64 r = gs; r < g; r += GRAN) {
                /* only roll back ones no vma covers */
                (void)r;
            }
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- vma list */
static size_t vma_lower(u64 addr)   /* first vma with end > addr */
{
    size_t lo = 0, hi = g_n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_v[m].end <= addr) lo = m + 1; else hi = m; }
    return lo;
}

static int vma_reserve_slots(size_t extra)
{
    if (g_n + extra <= g_cap) return 0;
    size_t nc = g_cap ? g_cap * 2 : 512;
    while (nc < g_n + extra) nc *= 2;
    vma *n = (vma *)realloc(g_v, nc * sizeof(vma));
    if (!n) return -1;
    g_v = n; g_cap = nc;
    return 0;
}

static int vma_insert_at(size_t i, const vma *v)
{
    if (vma_reserve_slots(1)) return -1;
    memmove(&g_v[i + 1], &g_v[i], (g_n - i) * sizeof(vma));
    g_v[i] = *v;
    g_n++;
    return 0;
}

static int vma_mergeable(const vma *a, const vma *b)
{
    return a->end == b->start && a->prot == b->prot && a->committed == b->committed &&
           a->shared == b->shared && !a->file && !b->file && !strcmp(a->name, b->name);
}

static void vma_try_merge(size_t i)
{
    if (i + 1 < g_n && vma_mergeable(&g_v[i], &g_v[i + 1])) {
        g_v[i].end = g_v[i + 1].end;
        memmove(&g_v[i + 1], &g_v[i + 2], (g_n - i - 2) * sizeof(vma));
        g_n--;
    }
    if (i > 0 && vma_mergeable(&g_v[i - 1], &g_v[i])) {
        g_v[i - 1].end = g_v[i].end;
        memmove(&g_v[i], &g_v[i + 1], (g_n - i - 1) * sizeof(vma));
        g_n--;
    }
}

/* Split so that `addr` is a vma boundary (if inside one). */
static int vma_split(u64 addr)
{
    size_t i = vma_lower(addr);
    if (i >= g_n || g_v[i].start >= addr) return 0;
    vma right = g_v[i];
    right.start = addr;
    if (right.file) { xfile_ref(right.file); right.foff += (s64)(addr - g_v[i].start); }
    g_v[i].end = addr;
    return vma_insert_at(i + 1, &right);
}

static void vma_flush_shared(const vma *v, u64 s, u64 e)
{
    if (!v->file || !v->shared || !(v->prot & LX_PROT_WRITE)) return;
    if (s < v->start) s = v->start;
    if (e > v->end) e = v->end;
    if (s >= e) return;
    s64 off = v->foff + (s64)(s - v->start);
    v->file->ops->write(v->file, (const void *)s, e - s, off);
}

/* Remove [s,e) from the list, decommitting and updating accounting. */
static slong vma_unmap_range(u64 s, u64 e, int release_granules)
{
    if (vma_split(s) || vma_split(e)) return -LX_ENOMEM;
    size_t i = vma_lower(s);
    size_t j = i;
    while (j < g_n && g_v[j].start < e) {
        vma *v = &g_v[j];
        if (v->committed) {
            vma_flush_shared(v, v->start, v->end);
            win_decommit(v->start, v->end);
            InterlockedAdd64(&g_committed, -(LONG64)(v->end - v->start));
        } else {
            win_decommit(v->start, v->end);   /* harmless for reserved-only */
        }
        if (v->file) xfile_unref(v->file);
        j++;
    }
    if (j > i) {
        memmove(&g_v[i], &g_v[j], (g_n - j) * sizeof(vma));
        g_n -= (j - i);
    }
    if (release_granules && !in_arena(s, e)) {
        u64 gs = ALIGN_DOWN(s, GRAN), ge = ALIGN_UP(e, GRAN);
        for (u64 g = gs; g < ge; g += GRAN) {
            size_t k = vma_lower(g);
            if (k < g_n && g_v[k].start < g + GRAN) continue;   /* still used */
            if (gr_has(g)) { VirtualFree((void *)g, 0, MEM_RELEASE); gr_del(g); }
        }
    }
    return 0;
}

static int range_free(u64 s, u64 e)
{
    size_t i = vma_lower(s);
    return !(i < g_n && g_v[i].start < e);
}

/* Highest address in the arena where [x, x+len) fits, below `limit`. */
static u64 find_gap(u64 len, u64 limit)
{
    u64 cand;
    if (limit > g_arena_end) limit = g_arena_end;
    if (limit < len) return 0;
    cand = ALIGN_DOWN(limit - len, LX_PAGE);
    for (;;) {
        if (cand < g_mmap_floor || cand + len > limit) return 0;
        size_t i = vma_lower(cand);
        if (i < g_n && g_v[i].start < cand + len) {
            if (g_v[i].start < len + g_mmap_floor) return 0;
            cand = ALIGN_DOWN(g_v[i].start - len, LX_PAGE);
            continue;
        }
        return cand;
    }
}

static void vma_name(vma *v, const char *n)
{
    memset(v->name, 0, sizeof v->name);
    if (n) strncpy_s(v->name, sizeof v->name, n, _TRUNCATE);
}

/* --------------------------------------------------------------- init */
int xrl_mem_init(u64 budget)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    g_budget = budget ? budget : (3ull << 30);
    static const u64 tries[] = { 1ull << 40, 1ull << 38, 1ull << 36, 1ull << 34, 1ull << 32, 1ull << 31 };
    for (size_t i = 0; i < ARRAY_LEN(tries); i++) {
        void *p = XRL_VIRTUAL_ALLOC(NULL, (SIZE_T)tries[i], MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
        if (p) { g_arena_base = (u64)p; g_arena_size = tries[i]; break; }
    }
    if (!g_arena_base) { LOGE("mem: cannot reserve any arena"); return -1; }
    g_arena_end = g_arena_base + g_arena_size;
    /* brk heap: low 1/16 of the arena capped at 1 GiB */
    u64 brk_sz = g_arena_size / 16;
    if (brk_sz > (1ull << 30)) brk_sz = 1ull << 30;
    g_brk_base = g_brk_cur = g_arena_base;
    g_brk_limit = g_arena_base + brk_sz;
    g_mmap_floor = g_brk_limit + (16ull << 20);
    LOGI("mem: arena [%llx,%llx) %llu MiB VA, brk <= %llx, commit budget %llu MiB, granularity %lu",
         g_arena_base, g_arena_end, g_arena_size >> 20, g_brk_limit, g_budget >> 20, si.dwAllocationGranularity);
    return 0;
}

u64 xrl_mem_committed(void) { return (u64)g_committed; }
u64 xrl_mem_arena_base(void) { return g_arena_base; }
u64 xrl_mem_arena_size(void) { return g_arena_size; }

/* ---------------------------------------------------------------- mmap */
static slong commit_account(u64 len)
{
    LONG64 now = InterlockedAdd64(&g_committed, (LONG64)len);
    if ((u64)now > g_budget) {
        InterlockedAdd64(&g_committed, -(LONG64)len);
        return -LX_ENOMEM;
    }
    return 0;
}

static slong file_fill(xfile *f, u64 dst, u64 len, s64 off)
{
    u64 done = 0;
    while (done < len) {
        slong n = f->ops->read(f, (void *)(dst + done), len - done, off + (s64)done);
        if (n < 0) return n;
        if (n == 0) break;
        done += (u64)n;
    }
    return 0;   /* tail already zero (fresh commit) */
}

/* core: map [addr,addr+len) with given attributes; lock held; range must be
 * already unmapped in the VMA list. */
static slong map_range(u64 addr, u64 len, int prot, int shared, xfile *file, s64 off, const char *name)
{
    u64 end = addr + len;
    int committed = 0;
    slong r;
    if (ensure_reserved(addr, end)) return -LX_ENOMEM;
    if (prot != LX_PROT_NONE || file) {
        r = commit_account(len);
        if (r) goto fail_rel;
        int cprot = file ? (LX_PROT_READ | LX_PROT_WRITE) : prot;   /* writable while filling */
        r = win_commit(addr, end, cprot);
        if (r) { InterlockedAdd64(&g_committed, -(LONG64)len); goto fail_rel; }
        committed = 1;
        if (file) {
            r = file_fill(file, addr, len, off);
            if (r) { win_decommit(addr, end); InterlockedAdd64(&g_committed, -(LONG64)len); goto fail_rel; }
            if (prot != (LX_PROT_READ | LX_PROT_WRITE)) {
                if (prot == LX_PROT_NONE) win_protect(addr, end, LX_PROT_NONE);
                else win_protect(addr, end, prot);
            }
        }
    }
    {
        vma v; memset(&v, 0, sizeof v);
        v.start = addr; v.end = end; v.prot = prot; v.committed = committed; v.shared = shared;
        vma_name(&v, name);
        if (file && shared && (prot & LX_PROT_WRITE)) { v.file = file; xfile_ref(file); v.foff = off; }
        size_t i = vma_lower(addr);
        if (vma_insert_at(i, &v)) { r = -LX_ENOMEM; goto fail_commit; }
        vma_try_merge(i);
    }
    return (slong)addr;
fail_commit:
    if (committed) { win_decommit(addr, end); InterlockedAdd64(&g_committed, -(LONG64)len); }
fail_rel:
    if (!in_arena(addr, end)) {
        for (u64 g = ALIGN_DOWN(addr, GRAN); g < ALIGN_UP(end, GRAN); g += GRAN) {
            size_t k = vma_lower(g);
            if (k < g_n && g_v[k].start < g + GRAN) continue;
            if (gr_has(g)) { VirtualFree((void *)g, 0, MEM_RELEASE); gr_del(g); }
        }
    }
    return r ? r : -LX_ENOMEM;
}

slong xrl_mem_mmap(u64 addr, u64 len, int prot, int flags, int fd, s64 off)
{
    if (!len) return -LX_EINVAL;
    if (len > (1ull << 46)) return -LX_ENOMEM;
    len = ALIGN_UP(len, LX_PAGE);
    if (prot & ~7) return -LX_EINVAL;
    int mtype = flags & 3;
    if (mtype != LX_MAP_SHARED && mtype != LX_MAP_PRIVATE && mtype != LX_MAP_SHARED_VALIDATE) return -LX_EINVAL;
    int fixed = (flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && (addr & (LX_PAGE - 1))) return -LX_EINVAL;
    if (fixed && (addr < 0x10000 || addr + len > 0x7fffffff0000ull || addr + len < addr)) return -LX_ENOMEM;

    xfile *f = NULL;
    const char *name = NULL;
    if (!(flags & LX_MAP_ANONYMOUS)) {
        if (off & (LX_PAGE - 1)) return -LX_EINVAL;
        f = xrl_fd_get(fd);
        if (!f) return -LX_EBADF;
        if (!f->ops->read) { xfile_unref(f); return -LX_ENODEV; }
        if ((flags & LX_MAP_SHARED) && (prot & LX_PROT_WRITE) && !(f->flags & LX_O_RDWR)) { xfile_unref(f); return -LX_EACCES; }
        if (f->gpath) { const char *b = strrchr(f->gpath, '/'); name = b ? b + 1 : f->gpath; }
    } else if (flags & LX_MAP_STACK) name = "[stack]";

    slong res;
    AcquireSRWLockExclusive(&g_lock);
    int shared = (flags & LX_MAP_SHARED) != 0;
    u64 target = 0;

    if (fixed) {
        if ((flags & LX_MAP_FIXED_NOREPLACE) && !(flags & LX_MAP_FIXED) && !range_free(addr, addr + len)) { res = -LX_EEXIST; goto out; }
        target = addr;
        if (target < g_mmap_floor && in_arena(target, target + len) && target + len > g_arena_base) {
            /* do not let the guest stomp the brk region by accident unless it owns it */
        }
        if (vma_unmap_range(target, target + len, 0)) { res = -LX_ENOMEM; goto out; }
    } else {
        if (addr && !(addr & (LX_PAGE - 1)) && addr >= 0x10000 && addr + len > addr && addr + len <= 0x7fffffff0000ull &&
            range_free(addr, addr + len) && !(addr + len > g_arena_base && addr < g_arena_end)) {
            if (!ensure_reserved(addr, addr + len)) target = addr;
        }
        if (!target) {
            target = find_gap(len, g_arena_end);
            if (!target) { res = -LX_ENOMEM; goto out; }
        }
    }
    res = map_range(target, len, prot, shared, f, off, name);
out:
    ReleaseSRWLockExclusive(&g_lock);
    if (f) xfile_unref(f);
    return res;
}

slong xrl_mem_munmap(u64 addr, u64 len)
{
    if ((addr & (LX_PAGE - 1)) || !len) return -LX_EINVAL;
    len = ALIGN_UP(len, LX_PAGE);
    if (addr + len < addr) return -LX_EINVAL;
    AcquireSRWLockExclusive(&g_lock);
    slong r = vma_unmap_range(addr, addr + len, 1);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

slong xrl_mem_mprotect(u64 addr, u64 len, int prot)
{
    if ((addr & (LX_PAGE - 1)) || (prot & ~(7 | 0x01000000 | 0x02000000))) return -LX_EINVAL;
    prot &= 7;
    if (!len) return 0;
    len = ALIGN_UP(len, LX_PAGE);
    u64 end = addr + len;
    if (end < addr) return -LX_ENOMEM;
    slong r = 0;
    AcquireSRWLockExclusive(&g_lock);
    /* coverage check */
    for (u64 p = addr; p < end;) {
        size_t i = vma_lower(p);
        if (i >= g_n || g_v[i].start > p) { r = -LX_ENOMEM; goto out; }
        p = g_v[i].end;
    }
    if (vma_split(addr) || vma_split(end)) { r = -LX_ENOMEM; goto out; }
    for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < end; i++) {
        vma *v = &g_v[i];
        if (v->prot == prot) continue;
        if (prot == LX_PROT_NONE) {
            if (v->committed) { r = win_protect(v->start, v->end, LX_PROT_NONE); if (r) goto out; }
        } else if (!v->committed) {
            r = commit_account(v->end - v->start);
            if (r) goto out;
            r = win_commit(v->start, v->end, prot);
            if (r) { InterlockedAdd64(&g_committed, -(LONG64)(v->end - v->start)); goto out; }
            v->committed = 1;
        } else {
            r = win_protect(v->start, v->end, prot);
            if (r) goto out;
        }
        v->prot = prot;
    }
    for (size_t i = vma_lower(addr); i < g_n && g_v[i].start <= end; i++) { vma_try_merge(i); }
out:
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

slong xrl_mem_mremap(u64 old_addr, u64 old_len, u64 new_len, int flags, u64 new_addr)
{
    if ((old_addr & (LX_PAGE - 1)) || !new_len) return -LX_EINVAL;
    old_len = ALIGN_UP(old_len, LX_PAGE);
    new_len = ALIGN_UP(new_len, LX_PAGE);
    if (flags & ~(LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED | LX_MREMAP_DONTUNMAP)) return -LX_EINVAL;
    if ((flags & LX_MREMAP_FIXED) && !(flags & LX_MREMAP_MAYMOVE)) return -LX_EINVAL;
    if (!old_len) return -LX_EINVAL;
    slong res;
    AcquireSRWLockExclusive(&g_lock);
    size_t i = vma_lower(old_addr);
    if (i >= g_n || g_v[i].start > old_addr || g_v[i].end < old_addr + old_len) { res = -LX_EFAULT; goto out; }
    int prot = g_v[i].prot, shared = g_v[i].shared;
    char name[40]; memcpy(name, g_v[i].name, sizeof name);

    if (!(flags & LX_MREMAP_FIXED) && new_len == old_len) { res = (slong)old_addr; goto out; }
    if (!(flags & LX_MREMAP_FIXED) && new_len < old_len) {
        res = vma_unmap_range(old_addr + new_len, old_addr + old_len, 1);
        if (!res) res = (slong)old_addr;
        goto out;
    }
    if (!(flags & LX_MREMAP_FIXED)) {
        u64 ext_s = old_addr + old_len, ext_e = old_addr + new_len;
        if (range_free(ext_s, ext_e) && (in_arena(ext_s, ext_e) || !ensure_reserved(ext_s, ext_e))) {
            slong m = map_range(ext_s, ext_e - ext_s, prot, shared, NULL, 0, name);
            if (m >= 0) { res = (slong)old_addr; goto out; }
        }
    }
    if (!(flags & LX_MREMAP_MAYMOVE)) { res = -LX_ENOMEM; goto out; }
    {
        u64 target;
        if (flags & LX_MREMAP_FIXED) {
            if (new_addr & (LX_PAGE - 1)) { res = -LX_EINVAL; goto out; }
            target = new_addr;
            if (vma_unmap_range(target, target + new_len, 0)) { res = -LX_ENOMEM; goto out; }
        } else {
            target = find_gap(new_len, g_arena_end);
            if (!target) { res = -LX_ENOMEM; goto out; }
        }
        res = map_range(target, new_len, prot == LX_PROT_NONE ? LX_PROT_NONE : (LX_PROT_READ | LX_PROT_WRITE), shared, NULL, 0, name);
        if (res < 0) goto out;
        u64 cp = old_len < new_len ? old_len : new_len;
        if (prot != LX_PROT_NONE) memcpy((void *)target, (void *)old_addr, cp);
        if (prot != (LX_PROT_READ | LX_PROT_WRITE) && prot != LX_PROT_NONE) {
            /* restore requested protection via the normal path */
            ReleaseSRWLockExclusive(&g_lock);
            slong pr = xrl_mem_mprotect(target, new_len, prot);
            AcquireSRWLockExclusive(&g_lock);
            if (pr) { res = pr; goto out; }
        }
        if (!(flags & LX_MREMAP_DONTUNMAP)) vma_unmap_range(old_addr, old_addr + old_len, 1);
        res = (slong)target;
    }
out:
    ReleaseSRWLockExclusive(&g_lock);
    return res;
}

slong xrl_mem_brk(u64 addr)
{
    slong res;
    AcquireSRWLockExclusive(&g_lock);
    if (addr == 0 || addr < g_brk_base || addr > g_brk_limit) { res = (slong)g_brk_cur; goto out; }
    u64 want = ALIGN_UP(addr, LX_PAGE), have = ALIGN_UP(g_brk_cur, LX_PAGE);
    if (want > have) {
        if (vma_unmap_range(have, want, 0)) { res = (slong)g_brk_cur; goto out; }
        if (map_range(have, want - have, LX_PROT_READ | LX_PROT_WRITE, 0, NULL, 0, "[heap]") < 0) { res = (slong)g_brk_cur; goto out; }
    } else if (want < have) {
        vma_unmap_range(want, have, 0);
    }
    g_brk_cur = addr;
    res = (slong)addr;
out:
    ReleaseSRWLockExclusive(&g_lock);
    return res;
}

slong xrl_mem_madvise(u64 addr, u64 len, int advice)
{
    if (addr & (LX_PAGE - 1)) return -LX_EINVAL;
    len = ALIGN_UP(len, LX_PAGE);
    switch (advice) {
    case LX_MADV_DONTNEED:
    case LX_MADV_FREE: {
        AcquireSRWLockShared(&g_lock);
        /* zero committed, writable anonymous pages; decommit+recommit when big */
        for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < addr + len; i++) {
            vma *v = &g_v[i];
            if (!v->committed || v->file || !(v->prot & LX_PROT_WRITE)) continue;
            u64 s = v->start > addr ? v->start : addr;
            u64 e = v->end < addr + len ? v->end : addr + len;
            if (e - s >= 0x10000) {
                win_decommit(s, e);
                win_commit(s, e, v->prot);
            } else {
                __try { memset((void *)s, 0, (size_t)(e - s)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        ReleaseSRWLockShared(&g_lock);
        return 0;
    }
    default:
        return 0;   /* hints (NORMAL, WILLNEED, HUGEPAGE, DONTFORK, ...) are no-ops */
    }
}

slong xrl_mem_msync(u64 addr, u64 len, int flags)
{
    (void)flags;
    if (addr & (LX_PAGE - 1)) return -LX_EINVAL;
    len = ALIGN_UP(len, LX_PAGE);
    AcquireSRWLockShared(&g_lock);
    for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < addr + len; i++)
        vma_flush_shared(&g_v[i], addr, addr + len);
    ReleaseSRWLockShared(&g_lock);
    return 0;
}

slong xrl_mem_mincore(u64 addr, u64 len, u64 vec)
{
    if (addr & (LX_PAGE - 1)) return -LX_EINVAL;
    u64 pages = ALIGN_UP(len, LX_PAGE) / LX_PAGE;
    slong r = 0;
    AcquireSRWLockShared(&g_lock);
    for (u64 p = 0; p < pages; p++) {
        size_t i = vma_lower(addr + p * LX_PAGE);
        if (i >= g_n || g_v[i].start > addr + p * LX_PAGE) { r = -LX_ENOMEM; break; }
        u8 b = g_v[i].committed ? 1 : 0;
        if (xrl_ctu(vec + p, &b, 1)) { r = -LX_EFAULT; break; }
    }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int xrl_mem_is_mapped(u64 addr)
{
    AcquireSRWLockShared(&g_lock);
    size_t i = vma_lower(addr);
    int r = i < g_n && g_v[i].start <= addr;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

u64 xrl_mem_stack_alloc(u64 size)
{
    size = ALIGN_UP(size, LX_PAGE);
    slong a = xrl_mem_mmap(0, size + LX_PAGE, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_STACK, -1, 0);
    if (a < 0) { LOGE("mem: stack alloc of %llu failed: %lld", size, a); return 0; }
    xrl_mem_mprotect((u64)a, LX_PAGE, LX_PROT_NONE);   /* guard at the low end */
    return (u64)a + LX_PAGE;
}

void xrl_mem_stack_free(u64 base, u64 size)
{
    xrl_mem_munmap(base - LX_PAGE, ALIGN_UP(size, LX_PAGE) + LX_PAGE);
}

size_t xrl_mem_format_maps(char *buf, size_t cap)
{
    size_t n = 0;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_n && n + 160 < cap; i++) {
        vma *v = &g_v[i];
        int w = snprintf(buf + n, cap - n, "%08llx-%08llx %c%c%c%c %08llx 00:00 0 %s%s\n",
                         v->start, v->end,
                         v->prot & LX_PROT_READ ? 'r' : '-', v->prot & LX_PROT_WRITE ? 'w' : '-',
                         v->prot & LX_PROT_EXEC ? 'x' : '-', v->shared ? 's' : 'p',
                         (u64)(v->file ? v->foff : 0), v->name[0] ? "                       " : "", v->name);
        if (w < 0) break;
        n += (size_t)w;
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* Executable memory for generated stubs (SysV->Win64 op-table thunks). */
void *xrl_exec_alloc(size_t n)
{
    return w_alloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}
