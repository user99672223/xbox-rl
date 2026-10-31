/*
 * com.c - host COM objects called directly by the guest (docs/gs-teb.md, Stage 2).
 *
 * The guest gets native COM objects from the host (D3D12 device and queue,
 * D3D11On12 device and context, DXGI swapchain, and everything created from
 * them) and calls their methods directly through the vtable. Those methods are
 * host code: they need the host TEB view and must not run on the guest stack
 * (exception dispatch and __chkstk check rsp against the TEB stack limits).
 *
 * So every entry of such a vtable is patched, in place, to a stub
 *     mov r11, <original>; jmp [xrl_com_tramp]
 * and xrl_com_tramp (thunks.asm) does the downcall dance when the calling
 * thread has the guest view live, and jumps straight to <original> otherwise
 * (host code, including D3D itself, calls the same vtables).
 *
 * Discovery: an object reaches the guest as an out-parameter or return value of
 * a downcall or of a thunked COM call. After each of those returns we scan its
 * arguments: a value that points to a pointer to an object whose vtable lies
 * outside guest memory (in a host image, or on the host heap: D3D11 builds its
 * context dispatch tables at run time) and starts with three code pointers into
 * host images (QueryInterface, AddRef, Release) is
 * taken as an interface out-parameter, and that vtable is hooked. The vtable
 * length is likewise guessed (entries that point into executable sections of
 * host images). Both are heuristics, and safe ones: a stub is conditional, so
 * hooking too much costs a few instructions per call and nothing else. Guest
 * (Wine-mapped) images are not host-loaded images, so guest objects are never
 * touched.
 *
 * Not handled: a host method that calls back into guest code (the callback
 * would run with the host view live, on the host stack). D3D does not.
 */
#include "xrl_internal.h"

/* ------------------------------------------------------- host image cache */
typedef struct img_sec { u32 lo, hi, ch; } img_sec;
typedef struct img {
    u64 base, end;
    int nsec;
    img_sec sec[32];
    char name[32];
} img;

static img g_img[256];
static volatile LONG g_nimg;
static SRWLOCK g_img_lock = SRWLOCK_INIT;

#define K_NONE 0
#define K_CODE 1
#define K_RODATA 2

static const img *img_lookup(u64 a)
{
    LONG n = g_nimg;
    for (LONG i = 0; i < n; i++)
        if (a >= g_img[i].base && a < g_img[i].end) return &g_img[i];
    return NULL;
}

static const img *img_add(u64 a)
{
    void *base = NULL;
    if (!RtlPcToFileHeader((PVOID)a, &base) || !base) return NULL;
    AcquireSRWLockExclusive(&g_img_lock);
    const img *r = img_lookup(a);
    if (!r && g_nimg < (LONG)ARRAY_LEN(g_img)) {
        img *m = &g_img[g_nimg];
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        m->base = (u64)base;
        m->end = m->base + nt->OptionalHeader.SizeOfImage;
        m->nsec = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections && m->nsec < (int)ARRAY_LEN(m->sec); i++, s++) {
            m->sec[m->nsec].lo = s->VirtualAddress;
            m->sec[m->nsec].hi = s->VirtualAddress + (s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData);
            m->sec[m->nsec].ch = s->Characteristics;
            m->nsec++;
        }
        m->name[0] = 0;
        wchar_t w[MAX_PATH];
        if (GetModuleFileNameW((HMODULE)base, w, MAX_PATH)) {
            const wchar_t *b = wcsrchr(w, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, m->name, sizeof m->name, NULL, NULL);
        }
        InterlockedIncrement(&g_nimg);   /* publish after the entry is complete */
        r = m;
    }
    ReleaseSRWLockExclusive(&g_img_lock);
    return r;
}

static int plausible(u64 p) { return p >= 0x10000 && p < 0x7ffffffff000ull && !(p & 7); }

/* What `a` points into: code / read-only data of a host-loaded image, or neither. */
static int addr_kind(u64 a, const img **out)
{
    if (a < 0x10000 || a >= 0x7ffffffff000ull) return K_NONE;   /* code pointers need not be aligned */
    const img *m = img_lookup(a);
    if (!m) {
        if (xrl_mem_is_mapped(a)) return K_NONE;   /* guest memory (incl. Wine's PE images) */
        if (!(m = img_add(a))) return K_NONE;
    }
    if (out) *out = m;
    u32 rva = (u32)(a - m->base);
    for (int i = 0; i < m->nsec; i++) {
        if (rva < m->sec[i].lo || rva >= m->sec[i].hi) continue;
        u32 ch = m->sec[i].ch;
        if (ch & IMAGE_SCN_MEM_EXECUTE) return K_CODE;
        if ((ch & IMAGE_SCN_MEM_READ) && !(ch & IMAGE_SCN_MEM_WRITE)) return K_RODATA;
        return K_NONE;
    }
    return K_NONE;
}

/* ---------------------------------------------------------- hooked vtables */
#define VT_SET 8192
static volatile u64 g_vt[VT_SET];          /* open addressing; 0 = empty */
static SRWLOCK g_hook_lock = SRWLOCK_INIT;
static u8 *g_stub_cur, *g_stub_end;
static u64 g_stub_lo = ~0ull, g_stub_hi;
static volatile LONG g_nhooked;

static int vt_known(u64 vt)
{
    for (u32 i = (u32)(vt >> 3) & (VT_SET - 1), n = 0; n < VT_SET; n++, i = (i + 1) & (VT_SET - 1)) {
        u64 k = g_vt[i];
        if (k == vt) return 1;
        if (!k) return 0;
    }
    return 0;
}

static void vt_insert(u64 vt)   /* hook lock held */
{
    for (u32 i = (u32)(vt >> 3) & (VT_SET - 1), n = 0; n < VT_SET; n++, i = (i + 1) & (VT_SET - 1))
        if (!g_vt[i]) { g_vt[i] = vt; return; }
}

static int is_stub(u64 p) { return p >= g_stub_lo && p < g_stub_hi; }

/* 24-byte stub: mov r11, imm64 ; jmp [rip+0] ; dq xrl_com_tramp */
static u64 make_stub(u64 target)   /* hook lock held */
{
    if (g_stub_end - g_stub_cur < 24) {
        size_t sz = 1 << 16;
        u8 *p = (u8 *)xrl_exec_alloc(sz);
        if (!p) return 0;
        g_stub_cur = p; g_stub_end = p + sz;
        if ((u64)p < g_stub_lo) g_stub_lo = (u64)p;
        if ((u64)p + sz > g_stub_hi) g_stub_hi = (u64)p + sz;
    }
    u8 *s = g_stub_cur;
    g_stub_cur += 24;
    s[0] = 0x49; s[1] = 0xBB; memcpy(s + 2, &target, 8);          /* mov r11, target */
    s[10] = 0xFF; s[11] = 0x25; memset(s + 12, 0, 4);              /* jmp [rip+0] */
    u64 tramp = (u64)(uintptr_t)xrl_com_tramp; memcpy(s + 16, &tramp, 8);
    return (u64)s;
}

/* QueryInterface of every hooked vtable (slot 0): it may return an interface of a type not seen
 * yet on any call, so its results are always scanned (xrl_com_after) */
#define QI_SET 1024
static volatile u64 g_qi[QI_SET];

static void qi_add(u64 fn)   /* hook lock held */
{
    for (u32 i = (u32)(fn >> 4) & (QI_SET - 1), n = 0; n < QI_SET; n++, i = (i + 1) & (QI_SET - 1)) {
        if (g_qi[i] == fn) return;
        if (!g_qi[i]) { g_qi[i] = fn; return; }
    }
}

static int is_qi(u64 fn)
{
    for (u32 i = (u32)(fn >> 4) & (QI_SET - 1), n = 0; n < QI_SET; n++, i = (i + 1) & (QI_SET - 1)) {
        u64 k = g_qi[i];
        if (k == fn) return 1;
        if (!k) return 0;
    }
    return 0;
}

static int hook_vtable(u64 vt)
{
    AcquireSRWLockExclusive(&g_hook_lock);
    u64 *e = (u64 *)vt;
    /* a known address whose first entry is no stub: a heap vtable freed and reallocated */
    if (vt_known(vt) && is_stub(e[0])) { ReleaseSRWLockExclusive(&g_hook_lock); return 0; }
    if (!vt_known(vt)) vt_insert(vt);
    int n = 0;
    while (n < 1024 && (is_stub(e[n]) || addr_kind(e[n], NULL) == K_CODE)) n++;
    const img *m = NULL;
    addr_kind(vt, &m);
    DWORD old;
    int err = n ? xrl_host_protect(e, (size_t)n * 8, PAGE_READWRITE, &old) : 0;
    int hooked = 0;
    if (n) qi_add(is_stub(e[0]) ? *(const u64 *)(e[0] + 2) : e[0]);
    if (!err) {
        for (int i = 0; i < n; i++) {
            if (is_stub(e[i])) continue;
            u64 s = make_stub(e[i]);
            if (!s) break;
            InterlockedExchange64((volatile LONG64 *)&e[i], (LONG64)s);
            hooked++;
        }
        DWORD tmp;
        if (n) xrl_host_protect(e, (size_t)n * 8, old, &tmp);
    }
    ReleaseSRWLockExclusive(&g_hook_lock);
    InterlockedIncrement(&g_nhooked);
    if (err) LOGW("com: cannot unprotect vtable %llx (%s+%llx): win err %d", vt, m ? m->name : "?", m ? vt - m->base : 0ull, err);
    else LOGI("com: vtable %llx (%s+%llx): %d of %d entries routed through the host-view thunk",
              vt, m ? m->name : "?", m ? vt - m->base : 0ull, hooked, n);
    return hooked > 0;
}

static int g_trace = -1;   /* XRL_COM_TRACE=1: log every candidate object */

/* Is `obj` a host COM object? Hook its vtable if so (1: this call hooked a new one). */
static int consider_object(u64 obj)
{
    if (!plausible(obj)) return 0;
    u64 vt = *(volatile u64 *)obj;
    if (g_trace < 0) { char v[4]; g_trace = GetEnvironmentVariableA("XRL_COM_TRACE", v, sizeof v) && v[0] == '1'; }
    if (!plausible(vt)) return 0;
    if (vt_known(vt) && is_stub(*(volatile u64 *)vt)) return 0;
    /* Where the vtable lives: an image's .rdata (or .text, some images merge them), or
     * host heap (D3D11 builds per-device dispatch tables for its context objects).
     * Never guest memory. */
    int k = addr_kind(vt, NULL);
    if (k == K_NONE && xrl_mem_is_mapped(vt)) return 0;
    if (g_trace) {
        const img *m = NULL;
        addr_kind(vt, &m);
        LOGI("com: candidate obj %llx vt %llx kind %d (%s) e0 kind %d", obj, vt, k, m ? m->name : "heap",
             addr_kind(((volatile u64 *)vt)[0], NULL));
    }
    /* QueryInterface, AddRef, Release: three code pointers (never true of instruction bytes) */
    for (int i = 0; i < 3; i++)
        if (addr_kind(((volatile u64 *)vt)[i], NULL) != K_CODE) return 0;
    return hook_vtable(vt);
}

static int consider(u64 v)
{
    int r = 0;
    if (!plausible(v)) return 0;
    __try {
        r |= consider_object(*(volatile u64 *)v);   /* v = &out_interface_pointer */
        r |= consider_object(v);                    /* v = an interface pointer itself */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

/* 1 if any value led to a newly hooked vtable */
int xrl_com_scan(const u64 *v, int n)
{
    int r = 0;
    for (int i = 0; i < n; i++) r |= consider(v[i]);
    return r;
}

/* Calls per method (the hang dump names the busiest): open addressing on the
 * method address; a full table just stops counting new methods. */
#define HOT_SET 1024
static volatile u64 g_hot_fn[HOT_SET];
static volatile LONG64 g_hot_n[HOT_SET];
static volatile u64 g_hot_ret[HOT_SET];   /* the last return value */
static volatile u8 g_hot_scan[HOT_SET];   /* 1: always scan this method's arguments (it hooked something,
                                            * or is a QueryInterface); 2: decided, skip mostly */

/* the method's slot (-1: table full), with its call count after this call in *n_out */
static int hot_count(u64 fn, u64 ret, LONG64 *n_out)
{
    for (u32 i = (u32)(fn >> 4) & (HOT_SET - 1), n = 0; n < 32; n++, i = (i + 1) & (HOT_SET - 1)) {
        u64 k = g_hot_fn[i];
        if (!k) {   /* claim the slot, or see who did */
            u64 prev = (u64)InterlockedCompareExchange64((volatile LONG64 *)&g_hot_fn[i], (LONG64)fn, 0);
            k = prev ? prev : fn;
        }
        if (k == fn) { *n_out = InterlockedIncrement64(&g_hot_n[i]); g_hot_ret[i] = ret; return (int)i; }
    }
    return -1;
}

/* The hooked vtable slot that calls `fn` (a stub holds its target at +2). */
static int slot_of(u64 fn, u64 *vt_out)
{
    for (u32 i = 0; i < VT_SET; i++) {
        u64 vt = g_vt[i];
        if (!vt) continue;
        const u64 *e = (const u64 *)vt;
        __try {
            for (int s = 0; s < 1024 && is_stub(e[s]); s++)
                if (*(const u64 *)(e[s] + 2) == fn) { *vt_out = vt; return s; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return -1;
}

/* The `max` busiest methods, one line each, into `emit`; max < 0: every method
 * outside d3d11/d3d11on12 (the layer's own gfx objects, DXGI, ...). */
void xrl_com_hot(void (*emit)(const char *line), int max)
{
    int others = max < 0;
    if (others) max = HOT_SET;
    static int idx[HOT_SET];
    int n = 0;
    for (int i = 0; i < HOT_SET; i++) if (g_hot_fn[i]) idx[n++] = i;
    for (int k = 0; k < max && k < n; k++) {
        int best = k;
        for (int j = k + 1; j < n; j++) if (g_hot_n[idx[j]] > g_hot_n[idx[best]]) best = j;
        int t = idx[k]; idx[k] = idx[best]; idx[best] = t;
        u64 fn = g_hot_fn[idx[k]], vt = 0;
        const img *m = NULL, *vm = NULL;
        addr_kind(fn, &m);
        if (others && m && !strncmp(m->name, "d3d11", 5)) continue;
        int s = slot_of(fn, &vt);
        if (vt) addr_kind(vt, &vm);
        char line[200];
        snprintf(line, sizeof line, "%12lld calls  %s+%llx  (vtable %llx %s+%llx slot %d) last ret %llx", (long long)g_hot_n[idx[k]],
                 m ? m->name : "?", m ? fn - m->base : fn, vt, vm ? vm->name : "heap", vm ? vt - vm->base : 0ull, s, g_hot_ret[idx[k]]);
        emit(line);
    }
}

/* After a thunked COM call: rcx/rdx/r8/r9 as saved, args 5..16 from the guest
 * stack (the callee may have reused its own copy), the return value and the method. */
volatile LONG64 xrl_com_calls;   /* guest -> host COM calls (XRL_PROFILE output) */

volatile LONG64 xrl_com_after_ticks;   /* XRL_PROFILE: QPC ticks spent in here (perf.c) */
static int g_com_prof = -1;

/* The argument scan is what finds new host objects (an out-parameter or return value of a type
 * not hooked yet), and it is most of the cost of a call (1-2 us, mostly on the game's render
 * thread). A method that has run SCAN_LEARN times without its scan ever hooking anything only
 * hands out known types or none (Draw*, *Set*, Map...): it is scanned once in SCAN_RECHECK calls
 * after that. QueryInterface and any method whose scan once hooked a vtable are always scanned.
 * XRL_COM_SCAN_ALL=1 scans every call (as before). */
#define SCAN_LEARN 512
#define SCAN_RECHECK 4096
static int g_scan_all = -1;

/* ---- the frame's COM calls (gfx calls xrl_com_frame_end from the game's Present) ----
 * Per thread, each method called since the last frame end: how often, and the time up to its return since the
 * thread's previous COM call returned (the game's own work before it plus the call). For a stuttering frame (and for
 * one normal frame every 10 s, to compare) one log line of the methods, the slowest first. */
#define FR_N 96
typedef struct fr_ent { u64 fn; u32 n; LONG64 t; } fr_ent;
static __declspec(thread) fr_ent *t_fr;
static __declspec(thread) int t_nfr, t_frover;
static __declspec(thread) LONG64 t_fr_last;
static LONG64 g_qf;

static void frame_note(u64 fn)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (!g_qf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qf = f.QuadPart; }
    LONG64 dt = t_fr_last ? q.QuadPart - t_fr_last : 0;
    t_fr_last = q.QuadPart;
    if (!t_fr && !(t_fr = (fr_ent *)calloc(FR_N, sizeof *t_fr))) return;
    for (int i = 0; i < t_nfr; i++) if (t_fr[i].fn == fn) { t_fr[i].n++; t_fr[i].t += dt; return; }
    if (t_nfr < FR_N) t_fr[t_nfr++] = (fr_ent){fn, 1, dt};
    else t_frover++;
}

void xrl_com_frame_end(double gap_ms, double wait_ms)
{
    static volatile LONG nlog;
    static LONG64 last_base;
    if (!t_fr) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    double tail = t_fr_last ? (q.QuadPart - t_fr_last) * 1000.0 / g_qf : 0;   /* after the last call: the game, then Present */
    t_fr_last = q.QuadPart;
    int base = !last_base || q.QuadPart - last_base > 10 * g_qf;
    if ((gap_ms > 50 || base) && InterlockedIncrement(&nlog) <= 1500) {
        if (base) last_base = q.QuadPart;
        char line[1400];
        int o = _snprintf_s(line, sizeof line, _TRUNCATE, "frame: %s %.1f ms; %.1f ms after the last call until Present ended, %.1f of it waiting for the GPU; %d methods%s:",
                            gap_ms > 50 ? "STUTTER" : "normal", gap_ms, tail, wait_ms, t_nfr, t_frover ? " (+more)" : "");
        for (int k = 0; k < 14 && k < t_nfr && o > 0 && o < (int)sizeof line - 90; k++) {
            int b = k;
            for (int j = k + 1; j < t_nfr; j++) if (t_fr[j].t > t_fr[b].t) b = j;
            fr_ent e = t_fr[k]; t_fr[k] = t_fr[b]; t_fr[b] = e;
            u64 vt = 0;
            int s = slot_of(t_fr[k].fn, &vt);
            const img *m = NULL;
            addr_kind(vt, &m);
            o += _snprintf_s(line + o, sizeof line - o, _TRUNCATE, " %s+%llx#%d x%u %.1fms;", m ? m->name : "heap", m ? vt - m->base : 0ull, s,
                             t_fr[k].n, t_fr[k].t * 1000.0 / g_qf);
        }
        LOGI("%s", line);
    }
    t_nfr = 0; t_frover = 0;
}

void xrl_com_after(const u64 *regs, const u64 *stack_args, u64 ret, u64 fn)
{
    LARGE_INTEGER q0 = {0}, q1;
    if (g_com_prof < 0) { char v[4]; g_com_prof = GetEnvironmentVariableA("XRL_PROFILE", v, sizeof v) && v[0] == '1'; }
    if (g_scan_all < 0) { char v[4]; g_scan_all = GetEnvironmentVariableA("XRL_COM_SCAN_ALL", v, sizeof v) && v[0] == '1'; }
    if (g_com_prof) QueryPerformanceCounter(&q0);
    InterlockedIncrement64(&xrl_com_calls);
    LONG64 n = 0;
    int i = hot_count(fn, ret, &n);
    frame_note(fn);
    {   /* a host COM call that failed in a way a game may not check (a NULL resource later): log it, with its slot */
        u32 hr = (u32)ret;
        static volatile LONG nfail;
        if ((hr == 0x8007000Eu || hr == 0x80070057u || hr == 0x887A0005u || hr == 0x887A0006u || hr == 0x887A0007u) &&
            InterlockedIncrement(&nfail) <= 64) {
            u64 vt = 0;
            int s = slot_of(fn, &vt);
            const img *m = NULL;
            addr_kind(vt, &m);
            LOGW("com: call failed hr=%08x: method %llx = slot %d of vtable %s+%llx, call %lld", hr, fn, s,
                 m ? m->name : "heap", m ? vt - m->base : vt, (long long)n);
        }
    }
    if (i >= 0 && !g_scan_all) {
        u8 m = g_hot_scan[i];
        if (!m && is_qi(fn)) g_hot_scan[i] = m = 1;
        if (!m && n > SCAN_LEARN) g_hot_scan[i] = m = 2;
        if (m == 2 && (n % SCAN_RECHECK)) goto done;
    }
    int found = xrl_com_scan(regs, 4);
    found |= xrl_com_scan(stack_args, 12);
    found |= consider(ret);
    if (i >= 0 && found && g_hot_scan[i] != 1) {
        if (g_hot_scan[i] == 2) LOGW("com: method %llx handed out a new object type after %lld calls: scanning it always again", fn, (long long)n);
        g_hot_scan[i] = 1;
    }
done:
    if (g_com_prof) { QueryPerformanceCounter(&q1); InterlockedAdd64(&xrl_com_after_ticks, q1.QuadPart - q0.QuadPart); }
}
