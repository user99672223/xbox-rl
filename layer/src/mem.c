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
 *   * Lazy commit (like Linux overcommit): accessible anonymous mappings of at
 *     least LAZY_MIN bytes, and reserved ranges later mprotect'ed accessible,
 *     stay reserved; the first touch faults and the VEH commits the 64 KiB
 *     around it (xrl_mem_lazy_fault). Windows games and Wine reserve and
 *     "commit" far more than they touch; committing up front ran past the
 *     console's ceiling. Exceptions, which must be committed before use because
 *     the kernel (not user code) writes them: stacks (MAP_STACK, and
 *     MADV_POPULATE_WRITE, which Wine issues for its thread stacks:
 *     the kernel pushes exception frames there), and buffers the layer hands to
 *     Win32 I/O (xrl_mem_populate before ReadFile/WriteFile). Nothing in this
 *     file touches guest memory while holding g_lock, because a lazy fault
 *     there would need g_lock in the VEH.
 *   * File mappings are copy-in: contents are read() into committed anonymous
 *     pages. MAP_SHARED|PROT_WRITE file mappings are written back on
 *     munmap/msync. This is sufficient for Wine's PE mapping and read-only
 *     sections; true coherent shared file mappings are NOT implemented.
 */
#include "xrl_internal.h"
#include <compressapi.h>
#include <winioctl.h>
#pragma comment(lib, "cabinet.lib")   /* Compression API (swap) */

#define GRAN 0x10000ull
static u64 LAZY_MIN = 0x40000ull;   /* smaller anonymous mmaps are committed at once (XRL_LAZY_MIN_KB) */

typedef struct vma {
    u64 start, end;
    int prot;               /* LX_PROT_* */
    int committed;          /* Windows pages are committed (all of them) */
    int lazy;               /* pages commit on first touch; any mix of states */
    int shared;             /* MAP_SHARED */
    xfile *file;            /* write-back target for shared writable file maps */
    s64 foff;               /* write-back offset (shared writable file maps) */
    s64 moff;               /* file offset shown in /proc/self/maps (any file map) */
    u64 ino;                /* inode shown in /proc/self/maps */
    u64 mid;                /* the mmap that made it: separate mappings never merge, so
                             * "the mapping containing X" (a new thread's stack) stays its own */
    char name[160];         /* guest path of a file map, or "[stack]" */
} vma;

static SRWLOCK g_lock = SRWLOCK_INIT;
static vma *g_v;
static size_t g_n, g_cap;

static int g_lazy = 1;   /* XRL_NO_LAZY=1: commit everything at mmap/mprotect time */
/* Bytes committed per lazy fault (XRL_LAZY_CHUNK_KB). Bigger = fewer faults; each
 * fault runs the host exception dispatcher, part of which runs with the guest
 * TEB view live (docs/gs-teb.md), so fewer is also safer. */
static u64 g_lazy_chunk = 1ull << 20;
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

/* Apply `op` over [s,e) split on Windows region boundaries; `state` is the
 * region's MEM_COMMIT / MEM_RESERVE / MEM_FREE (0 if the query failed). */
typedef int (*region_fn)(u64 s, u64 e, DWORD state, void *ctx);
static int each_region(u64 s, u64 e, region_fn fn, void *ctx)
{
    while (s < e) {
        MEMORY_BASIC_INFORMATION mbi;
        u64 ce = e;
        DWORD state = 0;
        if (VirtualQuery((void *)s, &mbi, sizeof mbi)) {
            u64 rend = (u64)mbi.BaseAddress + mbi.RegionSize;
            if (rend < ce) ce = rend;
            if (ce <= s) ce = e;
            state = mbi.State;
        }
        int r = fn(s, ce, state, ctx);
        if (r) return r;
        s = ce;
    }
    return 0;
}

static int op_commit(u64 s, u64 e, DWORD state, void *ctx)
{
    DWORD p = *(DWORD *)ctx;
    (void)state;
    return w_alloc((void *)s, e - s, MEM_COMMIT, p) ? 0 : (int)GetLastError();
}
static int op_protect(u64 s, u64 e, DWORD state, void *ctx)
{
    DWORD p = *(DWORD *)ctx, old;
    if (state != MEM_COMMIT) return 0;   /* lazy: reserved pages take the VMA prot when committed */
    return w_protect((void *)s, e - s, p, &old) ? 0 : (int)GetLastError();
}
static int op_decommit(u64 s, u64 e, DWORD state, void *ctx)
{
    (void)ctx;
    if (state == MEM_COMMIT) VirtualFree((void *)s, (SIZE_T)(e - s), MEM_DECOMMIT);
    return 0;
}
static int op_count(u64 s, u64 e, DWORD state, void *ctx)
{
    if (state == MEM_COMMIT) *(u64 *)ctx += e - s;
    return 0;
}

/* bytes of [s,e) committed in Windows (lazy VMAs are any mix) */
static u64 committed_bytes(u64 s, u64 e)
{
    u64 n = 0;
    each_region(s, e, op_count, &n);
    return n;
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
    return a->end == b->start && a->prot == b->prot && a->committed == b->committed && a->lazy == b->lazy &&
           a->shared == b->shared && a->mid == b->mid && !a->file && !b->file && !strcmp(a->name, b->name) &&
           (!a->name[0] || a->moff + (s64)(a->end - a->start) == b->moff);
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
    if (right.name[0]) right.moff += (s64)(addr - g_v[i].start);
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
static void swap_forget(u64 s, u64 e);

static slong vma_unmap_range(u64 s, u64 e, int release_granules)
{
    if (vma_split(s) || vma_split(e)) return -LX_ENOMEM;
    swap_forget(s, e);
    size_t i = vma_lower(s);
    size_t j = i;
    while (j < g_n && g_v[j].start < e) {
        vma *v = &g_v[j];
        if (v->committed) {
            vma_flush_shared(v, v->start, v->end);
            win_decommit(v->start, v->end);
            InterlockedAdd64(&g_committed, -(LONG64)(v->end - v->start));
        } else if (v->lazy) {
            u64 c = committed_bytes(v->start, v->end);
            win_decommit(v->start, v->end);
            InterlockedAdd64(&g_committed, -(LONG64)c);
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
static void swap_disable(void);
static void swap_reserve(u64 bytes);
static void swap_knobs(void);

int xrl_mem_init(u64 budget)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    g_budget = budget ? budget : (3ull << 30);
    char lz[4];
    /* XRL_NO_LAZY=1 off; =2 only fresh mmaps lazy; =3 only reserved->accessible lazy */
    if (GetEnvironmentVariableA("XRL_NO_LAZY", lz, sizeof lz)) g_lazy = lz[0] == '1' ? 0 : lz[0] == '2' ? 2 : lz[0] == '3' ? 3 : 1;
    char lm[24];
    if (GetEnvironmentVariableA("XRL_LAZY_MIN_KB", lm, sizeof lm)) LAZY_MIN = strtoull(lm, NULL, 10) << 10;
    if (GetEnvironmentVariableA("XRL_NO_SWAP", lm, sizeof lm) && lm[0] == '1') swap_disable();
    if (GetEnvironmentVariableA("XRL_SWAP_RESERVE_MB", lm, sizeof lm)) swap_reserve(strtoull(lm, NULL, 10) << 20);
    swap_knobs();
    if (GetEnvironmentVariableA("XRL_LAZY_CHUNK_KB", lm, sizeof lm)) {
        u64 c = strtoull(lm, NULL, 10) << 10;
        if (c >= LX_PAGE && !(c & (c - 1))) g_lazy_chunk = c;   /* power of two, >= one page */
    }
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
static void swap_room(u64 len, u64 xs, u64 xe);

/* Charge `len` bytes to the budget (g_lock held). With swap, cold memory
 * outside [xs,xe) is evicted first to make room. */
static slong commit_account_ex(u64 len, u64 xs, u64 xe)
{
    static volatile LONG64 high_mark, refusals;
    swap_room(len, xs, xe);
    LONG64 now = InterlockedAdd64(&g_committed, (LONG64)len);
    if ((u64)now > g_budget) {
        InterlockedAdd64(&g_committed, -(LONG64)len);
        if (InterlockedIncrement64(&refusals) <= 8)
            LOGW("mem: commit budget exhausted: %llu MiB committed + %llu KiB requested > %llu MiB",
                 (u64)(now - (LONG64)len) >> 20, len >> 10, g_budget >> 20);
        return -LX_ENOMEM;
    }
    /* one line per new 256 MiB high-water mark: how much the guest really commits */
    LONG64 mark = now >> 28, old = high_mark;
    if (mark > old && InterlockedCompareExchange64(&high_mark, mark, old) == old)
        LOGI("mem: committed %llu MiB (high-water)", (u64)now >> 20);
    return 0;
}
static slong commit_account(u64 len) { return commit_account_ex(len, 0, 0); }

/* Who commits lazy memory (the out-of-memory breakdown): the thread sets
 * t_commit_src around the call. */
enum { CS_WRITE_FAULT, CS_READ_FAULT, CS_EXEC_FAULT, CS_MADVISE, CS_STACK, CS_IO, CS_MINCORE, CS_MREMAP, CS_N };
static const char *const cs_names[CS_N] = { "write faults", "read faults", "exec faults", "madvise populate",
                                            "new-thread stacks", "read/write buffers", "mincore", "mremap" };
static volatile LONG64 g_cs_bytes[CS_N];
static __declspec(thread) int t_commit_src = CS_IO;   /* the default: vfs.c's populate calls */
static slong populate_from(u64 addr, u64 len, int src);

/* ------------------------------------------------------------------ swap
 * Linux swap for lazy anonymous memory, so a guest can touch more than the
 * commit budget (Rocket League writes 6.4 GiB while loading; the console's
 * commit wall is ~4.5 GiB). The unit is a 64 KiB span. When a commit would
 * pass the budget, cold spans are written to a swap file in TEMP and
 * decommitted; the next touch faults and reads them back (xrl_mem_lazy_fault),
 * and so does any layer path that hands guest memory to the kernel
 * (commit_lazy_locked, through xrl_mem_populate).
 * Coldness is a clock over the spans that hold committed lazy pages: the hand
 * first probes a span (PAGE_NOACCESS, contents kept); a touch faults and makes
 * it hot again; a span still probed when the hand comes round is evicted.
 * Probing starts at 80% of the budget, eviction 16 MiB below it.
 * A span read back on a read fault keeps its copy in the file and stays
 * read-only ("clean"): evicting it again costs no write unless it was written.
 * Pinned spans are never probed or evicted: memory committed by MADV_POPULATE*
 * and new-thread stacks, where the kernel itself pushes exception frames.
 * All of it runs under g_lock (exclusive). XRL_NO_SWAP=1 turns it off. */
#define SPAN 0x10000ull
enum { SP_FREE, SP_HOT, SP_PROBE, SP_OUT, SP_QUEUED };   /* QUEUED: picked for this pass's batch eviction */
/* The record of a span's data in the swap file: `nblk` 4 KiB blocks from block
 * `slot`. mask: pages whose copy in the record is valid: all of them when OUT;
 * when HOT/PROBE and `clean`, a span read back and not written since (its pages
 * stay read-only, so the first write faults and drops the copy). */
typedef struct span { u64 base; u32 slot; u16 mask, csz; u8 state, pinned, clean, nblk; } span;   /* csz: see store_span */
static span *g_sp;
static u32 g_nsp, g_capsp, g_hand;
static u32 *g_spfree, g_nspfree, g_capspfree;
static u32 *g_sph, g_sph_cap, g_sph_used;   /* open addressing: span index + 1; SPH_TOMB = deleted */
#define SPH_TOMB 0xffffffffu
static HANDLE g_swapf = INVALID_HANDLE_VALUE;
#define BLK 4096u
static u32 *g_fl[17], g_fln[17], g_flc[17];   /* free runs of n blocks (n = 1..16) */
static u32 g_nblocks;                          /* the file's length in blocks */
static u64 g_freeblk;
static int g_swap = 1;
static int g_swap_nobatch, g_swap_nora;   /* XRL_SWAP_NOBATCH / XRL_SWAP_NORA: debugging */
static int g_clk_clean_only;   /* swap_room: the file is full, the clock evicts only clean spans */
static u64 g_swap_excl_s, g_swap_excl_e;     /* never probe/evict here (mremap's source while copying) */
static volatile LONG64 g_swap_out, g_swap_in, g_swap_now;   /* bytes written, read back, in the file now */
static volatile LONG64 g_swap_clean;                         /* evicted again without a write (clean) */
static volatile LONG64 g_clk_steps, g_clk_probes, g_clk_evicts, g_clk_empty, g_clk_runs;   /* clock work */
static volatile LONG64 g_io_w, g_io_wt, g_io_r, g_io_rt, g_ev_t, g_in_t;   /* I/O calls and QPC ticks; whole evict/swapin */
static volatile LONG64 g_ra_spans;                                        /* spans read ahead */
static u64 qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (u64)q.QuadPart; }

static u32 sph_home(u64 base) { return (u32)(((base >> 16) * 0x9E3779B97F4A7C15ull) >> 40) & (g_sph_cap - 1); }

static int sp_find(u64 base)
{
    if (!g_sph_cap) return -1;
    for (u32 h = sph_home(base), n = 0; n < g_sph_cap; h = (h + 1) & (g_sph_cap - 1), n++) {
        u32 v = g_sph[h];
        if (!v) return -1;
        if (v != SPH_TOMB && g_sp[v - 1].base == base) return (int)(v - 1);
    }
    return -1;
}

static void sph_put(u32 idx)
{
    u32 h = sph_home(g_sp[idx].base);
    while (g_sph[h] && g_sph[h] != SPH_TOMB) h = (h + 1) & (g_sph_cap - 1);
    if (!g_sph[h]) g_sph_used++;
    g_sph[h] = idx + 1;
}

static int sph_room(void)
{
    if (g_sph_cap && (u64)(g_sph_used + 1) * 10 < (u64)g_sph_cap * 7) return 0;
    u32 live = g_nsp - g_nspfree, cap = g_sph_cap ? g_sph_cap : 4096;
    while ((u64)(live + 1) * 10 >= (u64)cap * 5) cap *= 2;
    u32 *n = (u32 *)calloc(cap, sizeof(u32));
    if (!n) return -1;
    free(g_sph);
    g_sph = n; g_sph_cap = cap; g_sph_used = 0;
    for (u32 i = 0; i < g_nsp; i++) if (g_sp[i].state != SP_FREE) sph_put(i);
    return 0;
}

static int sp_get(u64 base)
{
    int i = sp_find(base);
    if (i >= 0) return i;
    if (sph_room()) return -1;
    u32 idx;
    if (g_nspfree) idx = g_spfree[--g_nspfree];
    else {
        if (g_nsp == g_capsp) {
            u32 c = g_capsp ? g_capsp * 2 : 4096;
            span *n = (span *)realloc(g_sp, (size_t)c * sizeof(span));
            if (!n) return -1;
            g_sp = n; g_capsp = c;
        }
        idx = g_nsp++;
    }
    g_sp[idx].base = base; g_sp[idx].slot = 0; g_sp[idx].nblk = 0; g_sp[idx].mask = 0; g_sp[idx].csz = 0; g_sp[idx].state = SP_HOT; g_sp[idx].pinned = 0; g_sp[idx].clean = 0;
    sph_put(idx);
    return (int)idx;
}

static void fl_push(u32 b, u32 n)
{
    if (g_fln[n] == g_flc[n]) {
        u32 c = g_flc[n] ? g_flc[n] * 2 : 1024;
        u32 *x = (u32 *)realloc(g_fl[n], (size_t)c * sizeof(u32));
        if (!x) return;   /* leak the run */
        g_fl[n] = x; g_flc[n] = c;
    }
    g_fl[n][g_fln[n]++] = b;
    g_freeblk += n;
}
static void blk_free(u32 b, u32 n) { if (n >= 1 && n <= 16) fl_push(b, n); }

/* The swap file only grows while the disk keeps XRL_SWAP_RESERVE_MB (default
 * 1 GiB) free: past that, eviction fails and memory stays put, like a full
 * swap partition, instead of filling the disk (the console's TEMP has a quota). */
#define SLOT_NONE 0xffffffffu
static u64 g_swap_reserve = 1ull << 30;
static WCHAR g_swap_dir[MAX_PATH];
static int g_swap_full;
/* n contiguous blocks: an exact free run, else split a bigger one, else grow the file */
static u32 blk_alloc(u32 n)
{
    if (g_fln[n]) { g_freeblk -= n; return g_fl[n][--g_fln[n]]; }
    for (u32 m = n + 1; m <= 16; m++)
        if (g_fln[m]) {
            u32 b = g_fl[m][--g_fln[m]];
            g_freeblk -= m;
            fl_push(b + n, m - n);
            return b;
        }
    if ((g_nblocks >> 12) != ((g_nblocks + n) >> 12) || !g_nblocks) {   /* every 16 MiB of growth */
        ULARGE_INTEGER avail;
        g_swap_full = g_swap_dir[0] && GetDiskFreeSpaceExW(g_swap_dir, &avail, NULL, NULL) && avail.QuadPart < g_swap_reserve;
        if (g_swap_full) {
            static volatile LONG once;
            if (!InterlockedExchange(&once, 1))
                LOGW("mem: swap file stops at %u MiB: less than %llu MiB left on its disk", g_nblocks >> 8, g_swap_reserve >> 20);
        }
    }
    if (g_swap_full) return SLOT_NONE;
    u32 b = g_nblocks;
    g_nblocks += n;
    return b;
}

static u32 popcnt16(u32 m) { u32 n = 0; for (; m; m &= m - 1) n++; return n; }

static void sp_drop(u32 idx)
{
    span *p = &g_sp[idx];
    if (p->state == SP_OUT) {
        blk_free(p->slot, p->nblk);
        InterlockedAdd64(&g_swap_now, -(LONG64)popcnt16(p->mask) * (LONG64)LX_PAGE);
    } else if (p->clean) blk_free(p->slot, p->nblk);
    p->clean = 0;
    u32 h = sph_home(p->base);
    for (u32 n = 0; n < g_sph_cap; h = (h + 1) & (g_sph_cap - 1), n++) {
        if (!g_sph[h]) break;
        if (g_sph[h] == idx + 1) { g_sph[h] = SPH_TOMB; break; }
    }
    p->state = SP_FREE;
    if (g_nspfree == g_capspfree) {
        u32 c = g_capspfree ? g_capspfree * 2 : 1024;
        u32 *n = (u32 *)realloc(g_spfree, (size_t)c * sizeof(u32));
        if (!n) return;
        g_spfree = n; g_capspfree = c;
    }
    g_spfree[g_nspfree++] = idx;
}

/* A span's image (its committed lazy pages at their offsets, zeros elsewhere)
 * is compressed with XPRESS (Windows' Compression API, also in the app/games
 * partitions) and stored as a record of whole 4 KiB blocks, packed densely in
 * the swap file (free runs are reused; a sparse file would not help: NTFS
 * allocates sparse files in 64 KiB units). Spans that do not compress are
 * stored as they are (csz = 0, 16 blocks). */
static COMPRESSOR_HANDLE g_cmp;
static DECOMPRESSOR_HANDLE g_dcmp;
static u8 *g_img, *g_cbuf, *g_rbuf;          /* span image, one record, read-ahead extent (8 slots) */
static volatile LONG64 g_swap_disk;           /* bytes written to the file (compressed) */
#define RA_MAX 8

static int swap_open(void)
{
    if (g_swapf != INVALID_HANDLE_VALUE) return 0;
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH - 40) { g_swap = 0; return -1; }
    wcscpy_s(g_swap_dir, MAX_PATH, dir);
    swprintf_s(path, MAX_PATH, L"%sxrl-swap-%lu.bin", dir, GetCurrentProcessId());
    g_img = (u8 *)w_alloc(NULL, SPAN * (2 + RA_MAX), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_img) { LOGE("mem: no buffers for swap: swap off"); g_swap = 0; return -1; }
    g_cbuf = g_img + SPAN;
    g_rbuf = g_img + 2 * SPAN;
    CREATEFILE2_EXTENDED_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.dwSize = sizeof p;
    p.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    /* unbuffered: the data goes straight to the disk, not into file cache that
     * would eat the memory we are trying to free */
    p.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_NO_BUFFERING;
    g_swapf = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, &p);
    if (g_swapf == INVALID_HANDLE_VALUE) {
        LOGE("mem: cannot create the swap file %ls (%lu): swap off", path, GetLastError());
        g_swap = 0;
        return -1;
    }
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, NULL, &g_cmp) ||
        !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, NULL, &g_dcmp)) {
        g_cmp = NULL; g_dcmp = NULL;
    }
    LOGI("mem: swap file %ls (%s)", path, g_cmp ? "XPRESS-compressed" : "uncompressed");
    return 0;
}

/* Apply fn to the lazy anonymous parts of the span at b (a span can hold
 * pieces of several mappings; only lazy ones are ever probed or evicted). */
typedef int (*part_fn)(u64 s, u64 e, vma *v, void *ctx);
static int range_parts(u64 a, u64 b, part_fn fn, void *ctx)
{
    for (size_t i = vma_lower(a); i < g_n && g_v[i].start < b; i++) {
        vma *v = &g_v[i];
        if (!v->lazy || v->file) continue;
        u64 s = v->start > a ? v->start : a, e = v->end < b ? v->end : b;
        int r = fn(s, e, v, ctx);
        if (r) return r;
    }
    return 0;
}
static int span_parts(u64 b, part_fn fn, void *ctx) { return range_parts(b, b + SPAN, fn, ctx); }

static int part_probe(u64 s, u64 e, vma *v, void *ctx)
{
    (void)v; (void)ctx;
    DWORD p = PAGE_NOACCESS;
    each_region(s, e, op_protect, &p);
    return 0;
}
/* ctx non-NULL: the span is clean, so writable mappings come back read-only */
static int part_unprobe(u64 s, u64 e, vma *v, void *ctx)
{
    int prot = v->prot;
    if (ctx) prot &= ~LX_PROT_WRITE;
    DWORD p = prot ? winprot(prot, NULL) : PAGE_NOACCESS;
    each_region(s, e, op_protect, &p);
    return 0;
}
static int part_readonly(u64 s, u64 e, vma *v, void *ctx)
{
    (void)v; (void)ctx;
    DWORD p = PAGE_READONLY;   /* store_span reads it; a guest write waits on g_lock */
    each_region(s, e, op_protect, &p);
    return 0;
}
typedef struct dc_ctx { u64 bytes; } dc_ctx;
static int op_dc(u64 s, u64 e, DWORD state, void *ctx)
{
    if (state != MEM_COMMIT) return 0;
    VirtualFree((void *)s, (SIZE_T)(e - s), MEM_DECOMMIT);
    ((dc_ctx *)ctx)->bytes += e - s;
    return 0;
}
static int part_decommit(u64 s, u64 e, vma *v, void *ctx) { (void)v; return each_region(s, e, op_dc, ctx); }

static void span_unprobe(u32 idx)
{
    span_parts(g_sp[idx].base, part_unprobe, g_sp[idx].clean ? (void *)1 : NULL);
    g_sp[idx].state = SP_HOT;
}
/* written (or about to be, by the kernel): drop the clean copy, the mappings' own protection */
static void span_dirty(u32 idx)
{
    span *p = &g_sp[idx];
    if (!p->clean) return;
    blk_free(p->slot, p->nblk);
    p->clean = 0; p->mask = 0;
    span_parts(p->base, part_unprobe, NULL);
}

typedef struct cm_ctx { u64 base; u32 mask; } cm_ctx;
static int op_cmask(u64 s, u64 e, DWORD state, void *ctx)
{
    cm_ctx *c = (cm_ctx *)ctx;
    if (state == MEM_COMMIT) for (u64 q = s; q < e; q += LX_PAGE) c->mask |= 1u << ((q - c->base) >> 12);
    return 0;
}
static int part_cmask(u64 s, u64 e, vma *v, void *ctx) { (void)v; return each_region(s, e, op_cmask, ctx); }

typedef struct img_ctx { u64 base; u32 mask; u64 bytes; } img_ctx;
static int op_img(u64 s, u64 e, DWORD state, void *ctx)
{
    img_ctx *c = (img_ctx *)ctx;
    if (state != MEM_COMMIT) return 0;
    memcpy(g_img + (s - c->base), (const void *)s, (size_t)(e - s));
    for (u64 q = s; q < e; q += LX_PAGE) c->mask |= 1u << ((q - c->base) >> 12);
    c->bytes += e - s;
    return 0;
}
static int part_img(u64 s, u64 e, vma *v, void *ctx) { (void)v; return each_region(s, e, op_img, ctx); }

/* Write the span at `base` (its lazy parts readable) as a new record:
 * *slot/*nblk where it went, *mask the pages saved, *csz the compressed size
 * (0: stored raw). 0 ok (nothing written when *mask is 0), ERROR_DISK_FULL
 * when the swap file may not grow, else a Win32 error. */
static DWORD store_span(u64 base, u32 *slot, u32 *nblk, u32 *mask, u16 *csz)
{
    memset(g_img, 0, SPAN);
    img_ctx c = { base, 0, 0 };
    span_parts(base, part_img, &c);
    *mask = c.mask;
    *csz = 0;
    *nblk = 0;
    if (!c.mask) return 0;
    const u8 *buf = g_img;
    u32 len = (u32)SPAN;
    SIZE_T n = 0;
    if (g_cmp && Compress(g_cmp, g_img, SPAN, g_cbuf, SPAN, &n) && n && ALIGN_UP(n, BLK) < SPAN) {
        memset(g_cbuf + n, 0, (size_t)(ALIGN_UP(n, BLK) - n));
        buf = g_cbuf;
        len = (u32)ALIGN_UP(n, BLK);
        *csz = (u16)n;
    }
    u32 b = blk_alloc(len / BLK);
    if (b == SLOT_NONE) return ERROR_DISK_FULL;
    u64 off = (u64)b * BLK;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD w = 0;
    u64 t0 = qpc();
    BOOL ok = WriteFile(g_swapf, buf, len, &w, &ov);
    g_io_wt += qpc() - t0; g_io_w++;
    if (!ok || w != len) { blk_free(b, len / BLK); return GetLastError() ? GetLastError() : 1; }
    InterlockedAdd64(&g_swap_disk, (LONG64)len);
    *slot = b;
    *nblk = len / BLK;
    return 0;
}

static u64 evict(u32 idx)
{
    span *p = &g_sp[idx];
    if (swap_open()) return 0;
    if (p->clean) {
        cm_ctx cm = { p->base, 0 };
        span_parts(p->base, part_cmask, &cm);
        if (cm.mask && !(cm.mask & ~(u32)p->mask)) {   /* nothing new since it was read back: no write */
            dc_ctx d = { 0 };
            span_parts(p->base, part_decommit, &d);
            InterlockedAdd64(&g_committed, -(LONG64)d.bytes);
            InterlockedAdd64(&g_swap_now, (LONG64)d.bytes);
            InterlockedAdd64(&g_swap_clean, (LONG64)d.bytes);
            p->state = SP_OUT; p->mask = (u16)cm.mask; p->clean = 0;
            return d.bytes;
        }
        span_dirty(idx);   /* new pages as well: write the whole span again */
    }
    span_parts(p->base, part_readonly, NULL);
    u32 slot, nblk, mask;
    u16 csz;
    DWORD err = store_span(p->base, &slot, &nblk, &mask, &csz);
    if (err) {   /* swap full, or a write error: the span stays */
        static volatile LONG once;
        if (err != ERROR_DISK_FULL && !InterlockedExchange(&once, 1)) LOGE("mem: swap write failed (%lu): span kept in memory", err);
        span_parts(p->base, part_unprobe, NULL);
        p->state = SP_HOT;
        return 0;
    }
    if (!mask) { sp_drop(idx); return 0; }
    dc_ctx d = { 0 };
    span_parts(p->base, part_decommit, &d);
    InterlockedAdd64(&g_committed, -(LONG64)d.bytes);
    InterlockedAdd64(&g_swap_out, (LONG64)d.bytes);
    InterlockedAdd64(&g_swap_now, (LONG64)d.bytes);
    p->state = SP_OUT; p->slot = slot; p->nblk = (u8)nblk; p->mask = (u16)mask; p->csz = csz;
    return d.bytes;
}

static slong commit_account_ex(u64 len, u64 xs, u64 xe);

/* Protection for read-back pages in [s,e): the mappings' own, minus write when clean. */
static void protect_back(u64 s, u64 e, int clean)
{
    for (size_t i = vma_lower(s); i < g_n && g_v[i].start < e; i++) {
        int pr = g_v[i].prot;
        if (clean) pr &= ~LX_PROT_WRITE;
        if (pr == (LX_PROT_READ | LX_PROT_WRITE)) continue;
        u64 ps = g_v[i].start > s ? g_v[i].start : s, pe = g_v[i].end < e ? g_v[i].end : e;
        DWORD old;
        w_protect((void *)ps, pe - ps, pr ? winprot(pr, NULL) : PAGE_NOACCESS, &old);
    }
}

/* Bring an evicted span back from its record `rec` (NULL: read it from the
 * file). Commits its valid pages, fills them, sets the mappings' protections;
 * with `keep_clean` (a read fault) the record stays valid and writable
 * mappings come back read-only. [xs,xe) is never evicted to make room. */
static slong load_span(u32 idx, const u8 *rec, int keep_clean, u64 xs, u64 xe)
{
    span *p = &g_sp[idx];
    u64 base = p->base;
    u32 m = p->mask;
    u64 bytes = (u64)popcnt16(m) * LX_PAGE;
    slong r = commit_account_ex(bytes, xs, xe);   /* may evict others (and reuse g_img/g_cbuf): before any of that */
    if (r) return r;
    p = &g_sp[idx];
    if (!rec) {
        u32 len = (u32)p->nblk * BLK;
        u64 off = (u64)p->slot * BLK;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        u64 t0 = qpc();
        BOOL ok = ReadFile(g_swapf, g_cbuf, len, &got, &ov);
        g_io_rt += qpc() - t0; g_io_r++;
        if (!ok || got != len) {
            LOGE("mem: swap read of %llx failed (%lu)", base, GetLastError());
            InterlockedAdd64(&g_committed, -(LONG64)bytes);
            return -LX_EIO;
        }
        rec = g_cbuf;
    }
    const u8 *img = rec;
    if (p->csz) {
        SIZE_T out = 0;
        if (!Decompress(g_dcmp, rec, p->csz, g_img, SPAN, &out) || out != SPAN) {
            LOGE("mem: swap record of %llx does not decompress (%lu)", base, GetLastError());
            InterlockedAdd64(&g_committed, -(LONG64)bytes);
            return -LX_EIO;
        }
        img = g_img;
    }
    for (u32 a = 0; a < 16;) {
        if (!((m >> a) & 1)) { a++; continue; }
        u32 b = a;
        while (b < 16 && ((m >> b) & 1)) b++;
        u64 s = base + (u64)a * LX_PAGE, e = base + (u64)b * LX_PAGE;
        if (!w_alloc((void *)s, e - s, MEM_COMMIT, PAGE_READWRITE)) {
            LOGE("mem: cannot commit %llx to read it back", s);
            InterlockedAdd64(&g_committed, -(LONG64)bytes);
            return -LX_ENOMEM;   /* the record stays: the span is still OUT */
        }
        memcpy((void *)s, img + (s - base), (size_t)(e - s));
        protect_back(s, e, keep_clean);
        a = b;
    }
    InterlockedAdd64(&g_swap_in, (LONG64)bytes);
    InterlockedAdd64(&g_swap_now, -(LONG64)bytes);
    p->state = SP_HOT;
    if (keep_clean) p->clean = 1;   /* the record stays: it is still good */
    else { blk_free(p->slot, p->nblk); p->mask = 0; p->clean = 0; }
    return 0;
}

static slong swapin(u32 idx, int keep_clean) { return load_span(idx, NULL, keep_clean, g_sp[idx].base, g_sp[idx].base + SPAN); }

/* Visit the spans overlapping [s,e) (stepping by span, or over the table when
 * the range is much larger than what is tracked). */
typedef void (*span_visit)(u32 idx, u64 s, u64 e);
static void spans_in(u64 s, u64 e, span_visit fn)
{
    if (!g_nsp || e <= s) return;
    u64 b0 = ALIGN_DOWN(s, SPAN);
    if ((e - b0) / SPAN <= (u64)g_nsp * 2 + 64) {
        for (u64 b = b0; b < e; b += SPAN) { int i = sp_find(b); if (i >= 0) fn((u32)i, s, e); }
    } else {
        for (u32 i = 0; i < g_nsp; i++)
            if (g_sp[i].state != SP_FREE && g_sp[i].base < e && g_sp[i].base + SPAN > s) fn(i, s, e);
    }
}

/* A fault in an evicted span: read it and up to RA_MAX-1 following spans
 * whose records follow it in the file in one read (loading is mostly
 * sequential); the extra ones come back clean. */
static slong swapin_ahead(u32 idx, int keep_clean)
{
    span *p = &g_sp[idx];
    if (g_swap_nora || g_swapf == INVALID_HANDLE_VALUE) return swapin(idx, keep_clean);
    u32 run[RA_MAX];
    int k = 1;
    run[0] = idx;
    u32 next = p->slot + p->nblk;
    while (k < RA_MAX) {
        int j = sp_find(p->base + (u64)k * SPAN);
        if (j < 0 || g_sp[j].state != SP_OUT || g_sp[j].slot != next) break;
        next += g_sp[j].nblk;
        run[k++] = (u32)j;
    }
    if (k == 1) return swapin(idx, keep_clean);
    u64 a = p->base, b = a + (u64)k * SPAN;
    u64 off = (u64)p->slot * BLK;
    DWORD ext = (DWORD)(next - p->slot) * BLK;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    u64 t0 = qpc();
    BOOL ok = ReadFile(g_swapf, g_rbuf, ext, &got, &ov);
    g_io_rt += qpc() - t0; g_io_r++;
    if (!ok || got != ext) return swapin(idx, keep_clean);
    u32 first = p->slot;
    slong r = load_span(idx, g_rbuf, keep_clean, a, b);
    if (r) return r;
    for (int q = 1; q < k; q++)
        if (g_sp[run[q]].state == SP_OUT && !load_span(run[q], g_rbuf + (u64)(g_sp[run[q]].slot - first) * BLK, 1, a, b))
            g_ra_spans++;
    return 0;
}

static void visit_restore(u32 idx, u64 s, u64 e)
{
    (void)s; (void)e;
    if (g_sp[idx].state == SP_OUT) swapin(idx, 0);
    else { span_dirty(idx); if (g_sp[idx].state == SP_PROBE) span_unprobe(idx); }
}
/* [s,e) about to be used by the kernel or the layer: no evicted, probed or
 * read-only (clean) pages, as it may be written */
static void swap_restore(u64 s, u64 e) { if (g_nsp) spans_in(s, e, visit_restore); }

static void visit_settle(u32 idx, u64 s, u64 e)
{
    (void)s; (void)e;
    if (g_sp[idx].state == SP_OUT) return;   /* takes the new protection when read back */
    span_dirty(idx);
    if (g_sp[idx].state == SP_PROBE) span_unprobe(idx);
}
/* before mprotect: resident spans back to their mappings' protection */
static void swap_settle(u64 s, u64 e) { if (g_nsp) spans_in(s, e, visit_settle); }

static void visit_forget(u32 idx, u64 s, u64 e)
{
    span *p = &g_sp[idx];
    if (p->state == SP_OUT) {
        u32 m = p->mask;
        for (u32 a = 0; a < 16; a++) {
            u64 q = p->base + (u64)a * LX_PAGE;
            if (((m >> a) & 1) && q >= s && q < e) {
                m &= ~(1u << a);
                InterlockedAdd64(&g_swap_now, -(LONG64)LX_PAGE);
            }
        }
        if (!m) { p->mask = 0; blk_free(p->slot, p->nblk); p->state = SP_HOT; sp_drop(idx); }
        else p->mask = (u16)m;
    } else if (p->base >= s && p->base + SPAN <= e) sp_drop(idx);
    else if (p->clean) span_dirty(idx);   /* part of it discarded: forget the copy */
}
/* [s,e) unmapped or discarded (MADV_DONTNEED): its swapped pages are gone */
static void swap_forget(u64 s, u64 e) { if (g_nsp) spans_in(s, e, visit_forget); }

/* A commit of [s,e) succeeded: track its spans (pinned for stacks/populate). */
static void swap_note(u64 s, u64 e, int pin)
{
    if (!g_swap) return;
    for (u64 b = ALIGN_DOWN(s, SPAN); b < e; b += SPAN) {
        int i = sp_get(b);
        if (i < 0) return;
        if (g_sp[i].state == SP_PROBE) span_unprobe((u32)i);
        g_sp[i].state = SP_HOT;
        if (pin) g_sp[i].pinned = 1;
    }
}

/* Advance the clock up to `steps` spans: hot -> probed; with `evict`, probed
 * -> written out, until `want` bytes are freed. Skips [xs,xe). */
/* Batching: every protection change and decommit makes Windows flush the TLB
 * on all cores, so spans are probed and evicted in address-sorted runs of
 * neighbours, one call per run instead of three per 64 KiB. */
#define CLK_BATCH 256
static int cmp_span_base(const void *a, const void *b)
{
    u64 x = g_sp[*(const u32 *)a].base, y = g_sp[*(const u32 *)b].base;
    return x < y ? -1 : x > y;
}

/* consecutive spans from sorted v[i..]: returns the run length (<= 64) */
static int span_run(const u32 *v, int i, int n)
{
    int j = i + 1;
    while (j < n && j - i < 64 && g_sp[v[j]].base == g_sp[v[j - 1]].base + SPAN) j++;
    return j - i;
}

static void probe_batch(u32 *v, int n)
{
    if (!n) return;
    qsort(v, (size_t)n, sizeof *v, cmp_span_base);
    for (int i = 0; i < n;) {
        int k = span_run(v, i, n);
        range_parts(g_sp[v[i]].base, g_sp[v[i]].base + (u64)k * SPAN, part_probe, NULL);
        i += k;
    }
}

/* Write out the probed, dirty spans v[0..n): per run, one read-only pass,
 * one write per span (its own record), one decommit pass. */
static u64 evict_batch(u32 *v, int n)
{
    if (!n || swap_open()) return 0;
    u64 t0 = qpc();
    qsort(v, (size_t)n, sizeof *v, cmp_span_base);
    u64 freed = 0;
    for (int i = 0; i < n;) {
        int k = span_run(v, i, n);
        u64 a = g_sp[v[i]].base, b = a + (u64)k * SPAN;
        range_parts(a, b, part_readonly, NULL);
        u32 slots[64], nblks[64], masks[64];
        u16 cszs[64];
        int bad = 0;
        for (int q = 0; q < k && !bad; q++) {
            u32 slot = 0, nblk = 0, mask = 0;
            u16 csz = 0;
            DWORD err = store_span(g_sp[v[i + q]].base, &slot, &nblk, &mask, &csz);
            slots[q] = slot; nblks[q] = nblk; masks[q] = mask; cszs[q] = csz;
            if (err) {
                static volatile LONG once;
                if (err != ERROR_DISK_FULL && !InterlockedExchange(&once, 1)) LOGE("mem: swap write failed (%lu): spans kept in memory", err);
                for (int z = 0; z < q; z++) if (nblks[z]) blk_free(slots[z], nblks[z]);
                range_parts(a, b, part_unprobe, NULL);
                for (int z = 0; z < k; z++) g_sp[v[i + z]].state = SP_HOT;
                bad = 1;
            }
        }
        if (!bad) {
            dc_ctx d = { 0 };
            range_parts(a, b, part_decommit, &d);
            InterlockedAdd64(&g_committed, -(LONG64)d.bytes);
            InterlockedAdd64(&g_swap_out, (LONG64)d.bytes);
            InterlockedAdd64(&g_swap_now, (LONG64)d.bytes);
            freed += d.bytes;
            for (int q = 0; q < k; q++) {
                u32 idx = v[i + q];
                if (!masks[q]) { sp_drop(idx); g_clk_empty++; continue; }
                span *p = &g_sp[idx];
                p->state = SP_OUT; p->slot = slots[q]; p->nblk = (u8)nblks[q]; p->mask = (u16)masks[q]; p->csz = cszs[q]; p->clean = 0;
                g_clk_evicts++;
            }
        }
        i += k;
    }
    g_ev_t += qpc() - t0;
    return freed;
}

static u64 clock_run(u64 want, u64 steps, int evict_ok, u64 xs, u64 xe)
{
    u64 freed = 0, pending = 0;
    u32 pv[CLK_BATCH], ev[CLK_BATCH];
    int np = 0, ne = 0;
    for (u64 n = 0; n < steps && g_nsp && (!evict_ok || freed + pending < want); n++) {
        if (g_hand >= g_nsp) g_hand = 0;
        u32 idx = g_hand++;
        span *p = &g_sp[idx];
        /* QUEUED: the hand can lap within one pass; queuing a span twice made the
         * second copy find nothing committed and drop the record of the first */
        if (p->state == SP_FREE || p->state == SP_OUT || p->state == SP_QUEUED || p->pinned) continue;
        if (xe && p->base < xe && p->base + SPAN > xs) continue;
        if (g_swap_excl_e && p->base < g_swap_excl_e && p->base + SPAN > g_swap_excl_s) continue;
        g_clk_steps++;
        if (p->state == SP_HOT) {
            p->state = SP_PROBE;
            g_clk_probes++;
            pv[np++] = idx;
            if (np == CLK_BATCH) { probe_batch(pv, np); np = 0; }
            continue;
        }
        if (!evict_ok) continue;
        if (p->clean) {   /* re-evicting a clean span needs no write: at once */
            u64 f = evict(idx);
            freed += f;
            if (f) g_clk_evicts++; else g_clk_empty++;
            continue;
        }
        if (g_clk_clean_only) continue;   /* the file is full: dirty spans stay (probed) */
        if (g_swap_nobatch) {
            span_parts(p->base, part_probe, NULL);   /* may still be pending in pv */
            u64 f = evict(idx);
            freed += f;
            if (f) g_clk_evicts++; else g_clk_empty++;
            continue;
        }
        p->state = SP_QUEUED;
        ev[ne++] = idx;
        pending += SPAN;
        if (ne == CLK_BATCH) { freed += evict_batch(ev, ne); ne = 0; pending = 0; }
    }
    probe_batch(pv, np);
    freed += evict_batch(ev, ne);
    return freed;
}

/* Before committing `len` more bytes: probe ahead above 80% of the budget,
 * evict when the commit would come within 16 MiB of it. */
static void swap_disable(void) { g_swap = 0; }
static void swap_reserve(u64 bytes) { g_swap_reserve = bytes; }
static void swap_knobs(void)
{
    char v[4];
    g_swap_nobatch = GetEnvironmentVariableA("XRL_SWAP_NOBATCH", v, sizeof v) && v[0] == '1';
    g_swap_nora = GetEnvironmentVariableA("XRL_SWAP_NORA", v, sizeof v) && v[0] == '1';
}
void xrl_mem_swap_stats(u64 *now, u64 *out, u64 *in) { *now = (u64)g_swap_now; *out = (u64)g_swap_out; *in = (u64)g_swap_in; }
u64 xrl_mem_swap_clean(void) { return (u64)g_swap_clean; }
void xrl_mem_clock_stats(char *b, size_t n)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double us = 1e6 / (double)f.QuadPart;
    snprintf(b, n, "clock: %lld eviction runs, %lld steps, %lld probes, %lld evictions (%.0f us each), %lld empty; "
             "writes %lld (%.0f us avg), reads %lld (%.0f us avg), %lld spans read ahead; %u spans; "
             "file %u MiB (%llu MiB free in it); %llu MiB stored as %llu MiB written",
             (long long)g_clk_runs, (long long)g_clk_steps, (long long)g_clk_probes, (long long)g_clk_evicts,
             g_clk_evicts ? (double)g_ev_t * us / (double)g_clk_evicts : 0.0, (long long)g_clk_empty,
             (long long)g_io_w, g_io_w ? (double)g_io_wt * us / (double)g_io_w : 0.0,
             (long long)g_io_r, g_io_r ? (double)g_io_rt * us / (double)g_io_r : 0.0, (long long)g_ra_spans,
             g_nsp - g_nspfree, g_nblocks >> 8, g_freeblk >> 8, (u64)g_swap_out >> 20, (u64)g_swap_disk >> 20);
}

static void swap_room(u64 len, u64 xs, u64 xe)
{
    if (!g_swap || !g_nsp) return;
    u64 now = (u64)g_committed, b = g_budget;
    u64 high = b > (64ull << 20) ? b - (16ull << 20) : b;
    if (now + len <= b / 10 * 8) return;
    if (now + len <= high) { clock_run(0, 2, 0, xs, xe); return; }
    u64 want = now + len - high + (8ull << 20);   /* a little extra: fewer, larger passes */
    if (g_swap_full && !g_freeblk) {   /* full: look at the disk again at most once a second */
        static u64 last;
        u64 t = GetTickCount64();
        if (t - last >= 1000) {
            last = t;
            ULARGE_INTEGER avail;
            g_swap_full = !GetDiskFreeSpaceExW(g_swap_dir, &avail, NULL, NULL) || avail.QuadPart < g_swap_reserve;
        }
        if (g_swap_full) {
            /* no room for new records, but spans read back and not written since still
             * have theirs: those go without a write (and a read-back frees blocks for the rest) */
            g_clk_runs++;
            g_clk_clean_only = 1;
            clock_run(want, (u64)g_nsp * 3 + 16, 1, xs, xe);
            g_clk_clean_only = 0;
            return;
        }
    }
    g_clk_runs++;
    clock_run(want, (u64)g_nsp * 3 + 16, 1, xs, xe);
}

/* Commit the reserved parts of [s,e) with `prot`, charging the budget; evicted
 * or probed spans in the range are restored first. Lock held. */
static slong commit_lazy_locked(u64 s, u64 e, int prot)
{
    u64 s0 = s, e0 = e;
    int pin = t_commit_src == CS_STACK || t_commit_src == CS_MADVISE;
    swap_restore(s, e);
    DWORD wp = winprot(prot, NULL);
    while (s < e) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)s, &mbi, sizeof mbi)) return -LX_EFAULT;
        u64 re = (u64)mbi.BaseAddress + mbi.RegionSize;
        if (re > e) re = e;
        if (re <= s) return -LX_EFAULT;
        if (mbi.State != MEM_COMMIT) {
            slong r = commit_account_ex(re - s, s0, e0);
            if (r) return r;
            if (!w_alloc((void *)s, re - s, MEM_COMMIT, wp)) {
                InterlockedAdd64(&g_committed, -(LONG64)(re - s));
                return -LX_ENOMEM;
            }
            InterlockedAdd64(&g_cs_bytes[t_commit_src], (LONG64)(re - s));
            swap_note(s, re, pin);
        }
        s = re;
    }
    /* a stack: pin its spans that were committed already, too */
    if (pin) swap_note(ALIGN_DOWN(s0, LX_PAGE), e0, 1);
    return 0;
}

/* mremap: copy the committed regions of the source to the same offsets of dst */
typedef struct mremap_copy { u64 dst, src; slong err; } mremap_copy;
static int op_copy_committed(u64 s, u64 e, DWORD state, void *ctx)
{
    mremap_copy *c = (mremap_copy *)ctx;
    if (state != MEM_COMMIT) return 0;
    u64 d = c->dst + (s - c->src);
    int src = t_commit_src;
    t_commit_src = CS_MREMAP;
    slong r = commit_lazy_locked(d, d + (e - s), LX_PROT_READ | LX_PROT_WRITE);
    t_commit_src = src;
    if (r) { c->err = r; return 1; }
    memcpy((void *)d, (const void *)s, (size_t)(e - s));
    return 0;
}

static __declspec(thread) int t_mem_locked;   /* this thread holds g_lock (file_fill) */

static slong file_fill(xfile *f, u64 dst, u64 len, s64 off)
{
    u64 done = 0;
    t_mem_locked = 1;   /* dst is committed: the read path must not populate (needs g_lock) */
    while (done < len) {
        slong n = f->ops->read(f, (void *)(dst + done), len - done, off + (s64)done);
        if (n < 0) { t_mem_locked = 0; return n; }
        if (n == 0) break;
        done += (u64)n;
    }
    t_mem_locked = 0;
    return 0;   /* tail already zero (fresh commit) */
}

/* core: map [addr,addr+len) with given attributes; lock held; range must be
 * already unmapped in the VMA list. `lazy_ok`: an anonymous mapping may stay
 * reserved and commit on first touch. */
static slong map_range(u64 addr, u64 len, int prot, int shared, xfile *file, s64 off, const char *name, int lazy_ok)
{
    u64 end = addr + len;
    int committed = 0, lazy = 0;
    slong r;
    if (ensure_reserved(addr, end)) return -LX_ENOMEM;
    if (!file && lazy_ok && g_lazy && g_lazy != 3 && prot != LX_PROT_NONE) {
        lazy = 1;
    } else if (prot != LX_PROT_NONE || file) {
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
        static u64 next_mid;   /* g_lock held */
        v.start = addr; v.end = end; v.prot = prot; v.committed = committed; v.lazy = lazy; v.shared = shared;
        v.mid = ++next_mid;
        vma_name(&v, name);
        if (file) {
            lx_stat fst;
            v.moff = off;
            if (file->ops->fstat && !file->ops->fstat(file, &fst)) v.ino = fst.st_ino;
        }
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
        if (f->gpath) name = f->gpath;      /* the full guest path, as /proc/self/maps shows it */
    } else if (flags & LX_MAP_STACK) name = "[stack]";

    slong res;
    AcquireSRWLockExclusive(&g_lock);
    int shared = (flags & LX_MAP_SHARED) != 0;
    u64 target = 0;

    if (fixed) {
        if ((flags & LX_MAP_FIXED_NOREPLACE) && !(flags & LX_MAP_FIXED) && !range_free(addr, addr + len)) { res = -LX_EEXIST; goto out; }
        /* range held by the host (a DLL, KUSER_SHARED_DATA at 0x7ffe0000): to the guest
         * that is "already mapped", which Wine's free-area search steps over */
        if ((flags & LX_MAP_FIXED_NOREPLACE) && !(flags & LX_MAP_FIXED) && ensure_reserved(addr, addr + len)) { res = -LX_EEXIST; goto out; }
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
    res = map_range(target, len, prot, shared, f, off, name,
                    !(flags & (LX_MAP_STACK | LX_MAP_POPULATE)) && len >= LAZY_MIN);
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
    swap_settle(addr, end);   /* no probed or read-only (clean) swap pages under the new protection */
    for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < end; i++) {
        vma *v = &g_v[i];
        if (v->prot == prot) continue;
        if (prot == LX_PROT_NONE) {
            if (v->committed || v->lazy) { r = win_protect(v->start, v->end, LX_PROT_NONE); if (r) goto out; }
        } else if (!v->committed && !v->lazy && g_lazy && g_lazy != 2) {
            /* reserved -> accessible (Wine's MEM_COMMIT): commit on first touch */
            v->lazy = 1;
        } else if (!v->committed && !v->lazy) {
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
    char name[sizeof g_v[i].name]; memcpy(name, g_v[i].name, sizeof name);

    if (!(flags & LX_MREMAP_FIXED) && new_len == old_len) { res = (slong)old_addr; goto out; }
    if (!(flags & LX_MREMAP_FIXED) && new_len < old_len) {
        res = vma_unmap_range(old_addr + new_len, old_addr + old_len, 1);
        if (!res) res = (slong)old_addr;
        goto out;
    }
    if (!(flags & LX_MREMAP_FIXED)) {
        u64 ext_s = old_addr + old_len, ext_e = old_addr + new_len;
        if (range_free(ext_s, ext_e) && (in_arena(ext_s, ext_e) || !ensure_reserved(ext_s, ext_e))) {
            slong m = map_range(ext_s, ext_e - ext_s, prot, shared, NULL, 0, name, 1);
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
        res = map_range(target, new_len, prot == LX_PROT_NONE ? LX_PROT_NONE : (LX_PROT_READ | LX_PROT_WRITE), shared, NULL, 0, name, 1);
        if (res < 0) goto out;
        u64 cp = old_len < new_len ? old_len : new_len;
        if (prot != LX_PROT_NONE) {
            /* copy only what is committed (the rest stays lazy); commit the
             * destination first, as no guest page may fault under g_lock */
            mremap_copy c = { target, old_addr, 0 };
            swap_restore(old_addr, old_addr + cp);   /* the copy reads it under g_lock */
            g_swap_excl_s = old_addr; g_swap_excl_e = old_addr + cp;
            each_region(old_addr, old_addr + cp, op_copy_committed, &c);
            g_swap_excl_s = g_swap_excl_e = 0;
            if (c.err) { res = c.err; goto out; }
        }
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
        if (map_range(have, want - have, LX_PROT_READ | LX_PROT_WRITE, 0, NULL, 0, "[heap]", 1) < 0) { res = (slong)g_brk_cur; goto out; }
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
        AcquireSRWLockExclusive(&g_lock);
        swap_forget(addr, addr + len);
        /* anonymous pages: decommit (zero-filled on the next touch, which commits
         * lazily) and give the commit charge back */
        for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < addr + len; i++) {
            vma *v = &g_v[i];
            if (v->file || (!v->committed && !v->lazy)) continue;
            u64 s = v->start > addr ? v->start : addr;
            u64 e = v->end < addr + len ? v->end : addr + len;
            u64 c = v->committed ? e - s : committed_bytes(s, e);
            win_decommit(s, e);
            InterlockedAdd64(&g_committed, -(LONG64)c);
            v->committed = 0;
            v->lazy = 1;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    case LX_MADV_POPULATE_READ:
    case LX_MADV_POPULATE_WRITE:
        /* "commit now": Wine issues this for thread stacks (see header) */
        return populate_from(addr, len, CS_MADVISE);
    case LX_MADV_WILLNEED:
        /* read-ahead hint, as on Linux. Wine sends a game's PrefetchVirtualMemory here;
         * committing (and pinning) it made prefetched data unevictable for good. */
        return 0;
    default:
        return 0;   /* hints (NORMAL, HUGEPAGE, DONTFORK, ...) are no-ops */
    }
}

/* Commit the lazy (reserved) parts of [addr, addr+len): before handing guest
 * memory to Win32 I/O, which fails instead of faulting on reserved pages. */
static slong populate_from(u64 addr, u64 len, int src)
{
    t_commit_src = src;
    slong r = xrl_mem_populate(addr, len);
    t_commit_src = CS_IO;
    return r;
}

slong xrl_mem_populate(u64 addr, u64 len)
{
    if (!len || t_mem_locked) return 0;
    u64 end = ALIGN_UP(addr + len, LX_PAGE);
    addr = ALIGN_DOWN(addr, LX_PAGE);
    slong r = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = vma_lower(addr); i < g_n && g_v[i].start < end && !r; i++) {
        vma *v = &g_v[i];
        if (!v->lazy) continue;
        r = commit_lazy_locked(v->start > addr ? v->start : addr, v->end < end ? v->end : end, v->prot);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

/* Where the commit charge is (logged once, on the first out-of-memory). Lock held. */
static void mem_breakdown_locked(void)
{
    u64 lazy = 0, eager = 0, file = 0, stack = 0, big_c = 0, big_v = 0;
    size_t nlazy = 0;
    for (size_t i = 0; i < g_n; i++) {
        vma *v = &g_v[i];
        u64 c = v->committed ? v->end - v->start : v->lazy ? committed_bytes(v->start, v->end) : 0;
        if (!c) continue;
        if (!strcmp(v->name, "[stack]")) stack += c;
        else if (v->name[0] && !v->lazy) file += c;          /* named = file-backed copy-in */
        else if (v->lazy) { lazy += c; nlazy++; if (c > big_c) { big_c = c; big_v = v->start; } }
        else eager += c;
    }
    LOGE("mem: breakdown: lazy-touched %llu MiB in %zu mappings (largest %llu MiB at %llx), "
         "eager anonymous %llu MiB, file copies %llu MiB, layer stacks %llu MiB",
         lazy >> 20, nlazy, big_c >> 20, big_v, eager >> 20, file >> 20, stack >> 20);
    for (int k = 0; k < CS_N; k++)
        if (g_cs_bytes[k]) LOGE("mem:   lazy commits by %s: %llu MiB", cs_names[k], (u64)g_cs_bytes[k] >> 20);
    u32 npin = 0, nout = 0;
    for (u32 i = 0; i < g_nsp; i++) {
        if (g_sp[i].state == SP_FREE) continue;
        if (g_sp[i].pinned) npin++;
        if (g_sp[i].state == SP_OUT) nout++;
    }
    LOGE("mem:   swap %s: %llu MiB in the file, %llu MiB written, %llu MiB read back, %llu MiB evicted again clean, %u spans tracked "
         "(%u MiB pinned, never evicted; %u MiB out)",
         g_swap ? "on" : "off", (u64)g_swap_now >> 20, (u64)g_swap_out >> 20, (u64)g_swap_in >> 20, (u64)g_swap_clean >> 20,
         g_nsp - g_nspfree, npin >> 4, nout >> 4);
    /* the biggest lazy mappings (ten passes over the list; this runs once) */
    u64 last = ~0ull;
    for (int n = 0; n < 10; n++) {
        u64 best = 0; size_t bi = 0;
        for (size_t i = 0; i < g_n; i++) {
            if (!g_v[i].lazy) continue;
            u64 c = committed_bytes(g_v[i].start, g_v[i].end);
            if (c > best && c < last) { best = c; bi = i; }
        }
        if (!best) break;
        LOGE("mem:   %llx-%llx (%llu MiB) touched %llu MiB prot %d mapping #%llu", g_v[bi].start, g_v[bi].end,
             (g_v[bi].end - g_v[bi].start) >> 20, best >> 20, g_v[bi].prot, g_v[bi].mid);
        last = best;
    }
}

/* hangdump.c: the mapping holding `a` (1), none (0), the lock is held (-1) */
int xrl_mem_name_of(u64 a, char *name, size_t n, u64 *off)
{
    if (!TryAcquireSRWLockShared(&g_lock)) return -1;
    size_t i = vma_lower(a);
    int r = i < g_n && g_v[i].start <= a;
    if (r) { strncpy_s(name, n, g_v[i].name, _TRUNCATE); *off = a - g_v[i].start + (u64)g_v[i].foff; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}
/* `a` lies in an executable guest mapping (-1: the lock is busy) */
int xrl_mem_is_exec(u64 a)
{
    if (!TryAcquireSRWLockShared(&g_lock)) return -1;
    size_t i = vma_lower(a);
    int r = i < g_n && g_v[i].start <= a && (g_v[i].prot & LX_PROT_EXEC);
    ReleaseSRWLockShared(&g_lock);
    return r;
}
int xrl_mem_lock_held(void)
{
    if (!TryAcquireSRWLockExclusive(&g_lock)) return 1;
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

int xrl_mem_guest_ctx(u64 rip, u64 rsp)
{
    if (!TryAcquireSRWLockShared(&g_lock)) return -1;   /* the target may hold it: never wait */
    size_t i = vma_lower(rip);
    int ok = i < g_n && g_v[i].start <= rip && (g_v[i].prot & LX_PROT_EXEC);
    if (ok) {
        i = vma_lower(rsp - 1);
        ok = i < g_n && g_v[i].start <= rsp - 1 && (g_v[i].prot & LX_PROT_WRITE);
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

/* Commit [addr, addr+len) and pin it like a stack: never probed or evicted
 * (a sigaltstack: signal handlers run there and the kernel pushes exception
 * frames onto it, which a no-access or evicted page turns into a fatal fault). */
slong xrl_mem_pin_stack(u64 addr, u64 len) { return len ? populate_from(addr, len, CS_STACK) : 0; }

/* Commit the whole lazy mapping containing `addr` (a new thread's stack). */
slong xrl_mem_populate_containing(u64 addr)
{
    u64 s = 0, e = 0;
    AcquireSRWLockShared(&g_lock);
    size_t i = vma_lower(addr);
    if (i < g_n && g_v[i].start <= addr && g_v[i].lazy) { s = g_v[i].start; e = g_v[i].end; }
    ReleaseSRWLockShared(&g_lock);
    return e ? populate_from(s, e - s, CS_STACK) : 0;
}

/* VEH: a fault at `addr` (access 0 read, 1 write, 8 execute). Commits the
 * 64 KiB around it if it lies in a lazy VMA that permits the access. Returns
 * 1 to retry the instruction, 0 for a real fault. */
volatile LONG64 xrl_lazy_faults, xrl_lazy_fault_ticks;   /* XRL_PROFILE output */
static int lazy_fault_locked_entry(u64 addr, int access);

int xrl_mem_lazy_fault(u64 addr, int access)
{
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    int ok = lazy_fault_locked_entry(addr, access);
    QueryPerformanceCounter(&q1);
    if (ok) { InterlockedIncrement64(&xrl_lazy_faults); InterlockedAdd64(&xrl_lazy_fault_ticks, q1.QuadPart - q0.QuadPart); }
    return ok;
}

static int lazy_fault_locked_entry(u64 addr, int access)
{
    int ok = 0;
    AcquireSRWLockExclusive(&g_lock);
    size_t i = vma_lower(addr);
    if (i < g_n && g_v[i].start <= addr && g_v[i].lazy) {
        vma *v = &g_v[i];
        int need = access == 1 ? LX_PROT_WRITE : access == 8 ? LX_PROT_EXEC : LX_PROT_READ;
        MEMORY_BASIC_INFORMATION mbi;
        int si = g_nsp ? sp_find(ALIGN_DOWN(addr, SPAN)) : -1;
        if (!(v->prot & need)) ok = 0;
        else if (si >= 0 && g_sp[si].state == SP_OUT) {   /* swapped out: read it back */
            t_commit_src = access == 1 ? CS_WRITE_FAULT : access == 8 ? CS_EXEC_FAULT : CS_READ_FAULT;
            ok = swapin_ahead((u32)si, need != LX_PROT_WRITE) == 0;
            t_commit_src = CS_IO;
            if (!ok) LOGE("mem: cannot swap in %llx (%llu MiB committed, budget %llu MiB)", addr, (u64)g_committed >> 20, g_budget >> 20);
        } else if (VirtualQuery((void *)addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            /* probed by the swap clock (or left read-only by a failed eviction):
             * back to the mapping's protection; otherwise another thread won */
            if (mbi.Protect == PAGE_NOACCESS || (need == LX_PROT_WRITE && mbi.Protect == PAGE_READONLY)) {
                if (si >= 0) {
                    if (need == LX_PROT_WRITE) span_dirty((u32)si);   /* first write after a read-back */
                    span_unprobe((u32)si);
                }
                else { DWORD old; w_protect((void *)ALIGN_DOWN(addr, LX_PAGE), LX_PAGE, winprot(v->prot, NULL), &old); }
            }
            ok = 1;
        } else {
            u64 s = ALIGN_DOWN(addr, g_lazy_chunk), e = s + g_lazy_chunk;
            if (s < v->start) s = v->start;
            if (e > v->end) e = v->end;
            t_commit_src = access == 1 ? CS_WRITE_FAULT : access == 8 ? CS_EXEC_FAULT : CS_READ_FAULT;
            slong r = commit_lazy_locked(s, e, v->prot);
            t_commit_src = CS_IO;
            ok = r == 0;
            if (r) {
                LOGE("mem: out of memory: cannot commit the page at %llx (%llu MiB committed, budget %llu MiB)",
                     addr, (u64)g_committed >> 20, g_budget >> 20);
                static volatile LONG once;
                if (!InterlockedExchange(&once, 1)) mem_breakdown_locked();
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
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
    slong r = populate_from(vec, pages, CS_MINCORE);   /* vec is written under g_lock: must not fault */
    if (r) return r;
    AcquireSRWLockShared(&g_lock);
    for (u64 p = 0; p < pages; p++) {
        size_t i = vma_lower(addr + p * LX_PAGE);
        if (i >= g_n || g_v[i].start > addr + p * LX_PAGE) { r = -LX_ENOMEM; break; }
        u8 b = g_v[i].committed ? 1 : g_v[i].lazy ? (u8)(committed_bytes(addr + p * LX_PAGE, addr + (p + 1) * LX_PAGE) != 0) : 0;
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
    for (size_t i = 0; i < g_n && n + 300 < cap; i++) {
        vma *v = &g_v[i];
        int w = snprintf(buf + n, cap - n, "%08llx-%08llx %c%c%c%c %08llx 00:00 %llu %s%s\n",
                         v->start, v->end,
                         v->prot & LX_PROT_READ ? 'r' : '-', v->prot & LX_PROT_WRITE ? 'w' : '-',
                         v->prot & LX_PROT_EXEC ? 'x' : '-', v->shared ? 's' : 'p',
                         (u64)(v->name[0] && v->name[0] != '[' ? v->moff : 0), v->ino, v->name[0] ? "                       " : "", v->name);
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

/* Protection change on host-owned memory outside the VMA list (com.c patches
 * vtables in host DLL images). */
int xrl_host_protect(void *p, size_t n, DWORD prot, DWORD *old)
{
    return w_protect(p, n, prot, old) ? 0 : (int)GetLastError();
}

/* execve without a kept image: every guest mapping goes (the caller runs on its host stack, so its
 * own guest stack may go too), and the program break starts over. */
void xrl_mem_release_all(void)
{
    u64 *rg = NULL;
    size_t n = 0, cap = 0;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_n; i++) {
        if (n + 2 > cap) {
            u64 *nr = (u64 *)realloc(rg, (cap ? cap * 2 : 256) * sizeof(u64));
            if (!nr) break;
            rg = nr; cap = cap ? cap * 2 : 256;
        }
        rg[n++] = g_v[i].start; rg[n++] = g_v[i].end;
    }
    ReleaseSRWLockShared(&g_lock);
    for (size_t i = 0; i + 1 < n; i += 2) xrl_mem_munmap(rg[i], rg[i + 1] - rg[i]);
    free(rg);
    AcquireSRWLockExclusive(&g_lock);
    g_brk_cur = g_brk_base;
    ReleaseSRWLockExclusive(&g_lock);
}
