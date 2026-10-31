/*
 * sample.c - XRL_SAMPLE=<hz> (0/absent: off): a sampling profiler for the whole app, guest and host code alike.
 *
 * A thread wakes `hz` times a second (a high-resolution timer) and reads every guest thread's cycle count
 * (QueryThreadCycleTime). A thread that used at least 5% of the tick since the last one is sampled:
 * SuspendThread, GetThreadContext (rip only), ResumeThread. Nothing is allocated and no lock is taken while a
 * thread is suspended, so a thread stopped inside the heap or a layer lock cannot deadlock the sampler.
 *
 * The threshold matters: suspending a blocked thread makes it run (the kernel delivers the suspend to it), so
 * sampling every thread whose count moved would keep waking every thread on every tick and measure the
 * sampler. Each sample is weighted by the cycles the thread used since the last tick, so the shares are CPU
 * time, not sample counts. A thread that ran and then blocked is found sitting in a wait (a host system call
 * stub named *Wait*, *Delay*, *RemoveIoCompletion*): that work is reported apart as "ran, then blocked",
 * since where it ran is unknown.
 *
 * Every XRL_SAMPLE_DUMP seconds (default 30) the window is attributed and logged, then reset:
 *   sample: window ...           - samples, CPU time covered, the sampler's own cost
 *   sample: threads ...          - CPU share per thread name
 *   sample: mod <share> <name>   - by module: a guest mapping (its guest path: the game's .exe, Wine's DLLs and
 *                                  .so files, musl, anonymous), code the DBT translated (mapped back to the
 *                                  original address first), or a host module (the app itself = the layer and
 *                                  host, d3d11.dll, ntdll.dll, the GPU driver, ...)
 *   sample: hot <share> <name>+<off> (export+delta) [thread]
 *                                - the hottest exact addresses; <off> is the file offset in a guest mapping and the
 *                                  RVA in a host module, so they resolve offline (nm/objdump on the rootfs file,
 *                                  the PDB for the app); host exports are named in place
 */
#include "xrl_internal.h"
#include <intrin.h>

#define S_BITS 16
#define S_SIZE (1u << S_BITS)
#define S_THR 128

typedef struct s_ent { u64 rip, w; u32 n; u16 thr; } s_ent;   /* w: the cycles the samples stand for */
static s_ent *g_tab;                    /* open addressing on (rip, thread) */
static char g_thr_name[S_THR][24];      /* "name/tid": Wine's unnamed threads are all "xrl-guest" */
static int g_nthr_name;
static u64 g_total, g_lost, g_lost_w;

typedef struct s_prev { int tid; u64 cycles; } s_prev;
static s_prev g_prev[512];
static int g_nprev;

static int thr_index(const char *name, int tid)
{
    char key[24];
    snprintf(key, sizeof key, "%.15s/%d", name[0] ? name : "?", tid);
    for (int i = 0; i < g_nthr_name; i++) if (!strcmp(g_thr_name[i], key)) return i;
    if (g_nthr_name == S_THR) return S_THR - 1;   /* the last slot takes the rest */
    memcpy(g_thr_name[g_nthr_name], key, sizeof key);
    return g_nthr_name++;
}

static void add(u64 rip, int thr, u64 w)
{
    g_total++;
    u32 h = (u32)((rip * 0x9E3779B97F4A7C15ull) >> (64 - S_BITS)) ^ (u32)thr;
    for (u32 k = 0; k < 64; k++) {
        s_ent *e = &g_tab[(h + k) & (S_SIZE - 1)];
        if (!e->n) { e->rip = rip; e->thr = (u16)thr; e->n = 1; e->w = w; return; }
        if (e->rip == rip && e->thr == thr) { e->n++; e->w += w; return; }
    }
    g_lost++; g_lost_w += w;   /* the table is full around here: counted in the totals, not attributed */
}

static int cycles_prev(int tid, u64 *c)
{
    for (int i = 0; i < g_nprev; i++) if (g_prev[i].tid == tid) { *c = g_prev[i].cycles; return 1; }
    return 0;
}

/* ---- attribution (at dump time, no thread suspended) ---- */
typedef struct s_where { char mod[96]; u64 off; int kind; } s_where;   /* kind: 0 guest, 1 translated, 2 host, 3 unknown */

static void where_of(u64 rip, s_where *w)
{
    u64 orig = xrl_dbt_reverse(rip);
    u64 a = orig ? orig : rip;
    if (xrl_mem_vma_of(a, w->mod, sizeof w->mod, &w->off) == 1) {
        if (!w->mod[0]) strcpy_s(w->mod, sizeof w->mod, "[anon guest]");
        w->kind = orig ? 1 : 0;
        return;
    }
    HMODULE hm = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)rip, &hm) && hm) {
        wchar_t p[MAX_PATH];
        DWORD n = GetModuleFileNameW(hm, p, MAX_PATH);
        const wchar_t *b = p;
        for (DWORD i = 0; i < n; i++) if (p[i] == L'\\' || p[i] == L'/') b = p + i + 1;
        WideCharToMultiByte(CP_UTF8, 0, b, -1, w->mod, (int)sizeof w->mod, NULL, NULL);
        w->off = rip - (u64)(uintptr_t)hm;
        w->kind = 2;
        return;
    }
    strcpy_s(w->mod, sizeof w->mod, "[unknown]");
    w->off = rip;
    w->kind = 3;
}

/* The named export at or below `rva` in a loaded host module: Windows' ntdll/win32u system call stubs and
 * most of what a host DLL spends time in are exports. Returns 0 when nothing fits. */
static int export_of(HMODULE hm, u32 rva, char *out, size_t n, u32 *delta)
{
    const u8 *b = (const u8 *)hm;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd->VirtualAddress || !dd->Size) return 0;
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(b + dd->VirtualAddress);
    const u32 *fn = (const u32 *)(b + ed->AddressOfFunctions);
    const u32 *nm = (const u32 *)(b + ed->AddressOfNames);
    const u16 *ord = (const u16 *)(b + ed->AddressOfNameOrdinals);
    u32 best = 0; int bi = -1;
    for (u32 i = 0; i < ed->NumberOfNames; i++) {
        u32 f = fn[ord[i]];
        int fwd = f >= dd->VirtualAddress && f < dd->VirtualAddress + dd->Size;   /* a forwarder string, not code */
        if (!fwd && f <= rva && f >= best) { best = f; bi = (int)i; }
    }
    if (bi < 0 || rva - best > 0x10000) return 0;
    strncpy_s(out, n, (const char *)(b + nm[bi]), _TRUNCATE);
    *delta = rva - best;
    return 1;
}

static int host_export(u64 rip, char *out, size_t n, u32 *delta)
{
    HMODULE hm = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)rip, &hm) || !hm) return 0;
    return export_of(hm, (u32)(rip - (u64)(uintptr_t)hm), out, n, delta);
}

/* rip is the return address of a `syscall` (0f 05) in a host stub whose name says it blocks */
static int in_wait(u64 rip)
{
    u8 c[2]; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)(rip - 2), c, 2, &got) || got != 2 || c[0] != 0x0f || c[1] != 0x05) return 0;
    char x[72]; u32 d;
    if (!host_export(rip, x, sizeof x, &d)) return 0;
    return strstr(x, "Wait") || strstr(x, "Delay") || strstr(x, "RemoveIoCompletion");
}

typedef struct s_mod { char name[96]; u64 w; } s_mod;

static void dump(double secs, u64 tick_cyc, u64 own_cyc)
{
    if (!g_total) return;
    static s_mod mods[256];
    static int order[S_SIZE];
    u64 thr_w[S_THR] = {0};
    u64 work = 0, blocked = 0;
    int nm = 0, no = 0;
    for (u32 i = 0; i < S_SIZE; i++) {
        s_ent *e = &g_tab[i];
        if (!e->n) continue;
        if (in_wait(e->rip)) { blocked += e->w; continue; }
        work += e->w;
        thr_w[e->thr] += e->w;
        order[no++] = (int)i;
    }
    double cores = tick_cyc ? (double)(work + blocked + g_lost_w) / (double)tick_cyc : 0;
    LOGI("sample: window %.0f s, %llu samples (%.0f/s) covering %.2f cores of CPU time: %.1f%% located, %.1f%% ran then blocked (where unknown), %.1f%% table full; the sampler itself %.1f%% of a core",
         secs, g_total, (double)g_total / secs, cores,
         100.0 * (double)work / (double)(work + blocked + g_lost_w + 1), 100.0 * (double)blocked / (double)(work + blocked + g_lost_w + 1),
         100.0 * (double)g_lost_w / (double)(work + blocked + g_lost_w + 1), tick_cyc ? 100.0 * (double)own_cyc / (double)tick_cyc : 0.0);
    if (!work) goto reset;
    int top_thr[4], ntop = 0;
    {   /* per thread, as a share of the located CPU time */
        char line[900]; int len = snprintf(line, sizeof line, "sample: threads");
        for (int k = 0; k < 12; k++) {
            int b = -1;
            for (int i = 0; i < g_nthr_name; i++) if (thr_w[i] && (b < 0 || thr_w[i] > thr_w[b])) b = i;
            if (b < 0) break;
            if (ntop < 4) top_thr[ntop++] = b;
            len += snprintf(line + len, sizeof line - len, " %s %.1f%%;", g_thr_name[b], 100.0 * (double)thr_w[b] / (double)work);
            thr_w[b] = 0;
        }
        LOGI("%s", line);
    }
    for (int t = 0; t < ntop; t++) {   /* the busiest threads' own hottest addresses */
        int best[8], nb = 0;
        u64 tw = 0;
        for (int i = 0; i < no; i++) if (g_tab[order[i]].thr == top_thr[t]) tw += g_tab[order[i]].w;
        for (int k = 0; k < 8; k++) {
            int b = -1;
            for (int i = 0; i < no; i++) {
                s_ent *e = &g_tab[order[i]];
                if (e->thr != top_thr[t]) continue;
                int taken = 0;
                for (int j = 0; j < nb; j++) if (best[j] == i) taken = 1;
                if (!taken && (b < 0 || e->w > g_tab[order[b]].w)) b = i;
            }
            if (b < 0) break;
            best[nb++] = b;
        }
        for (int k = 0; k < nb; k++) {
            s_ent *e = &g_tab[order[best[k]]];
            s_where w; where_of(e->rip, &w);
            const char *b = strrchr(w.mod, '/');
            char sym[96] = "", x[72]; u32 d = 0;
            if (w.kind == 2 && host_export(e->rip, x, sizeof x, &d)) snprintf(sym, sizeof sym, " (%s+0x%x)", x, d);
            LOGI("sample: thread %s: %5.2f%% of it at %s+0x%llx%s%s", g_thr_name[top_thr[t]], 100.0 * (double)e->w / (double)(tw + 1),
                 b ? b + 1 : w.mod, w.off, sym, w.kind == 1 ? " (translated)" : "");
        }
    }
    /* the hottest addresses: sort a top 40 out of the entries */
    for (int k = 0; k < 40 && k < no; k++) {
        int b = k;
        for (int j = k + 1; j < no; j++) if (g_tab[order[j]].w > g_tab[order[b]].w) b = j;
        int t = order[k]; order[k] = order[b]; order[b] = t;
    }
    /* by module: every entry */
    for (int i = 0; i < no; i++) {
        s_ent *e = &g_tab[order[i]];
        s_where w; where_of(e->rip, &w);
        if (w.kind == 1) { char t[96]; snprintf(t, sizeof t, "%s (translated)", w.mod); strcpy_s(w.mod, sizeof w.mod, t); }
        int m = 0;
        while (m < nm && strcmp(mods[m].name, w.mod)) m++;
        if (m == nm && nm < 256) { strcpy_s(mods[nm].name, sizeof mods[nm].name, w.mod); mods[nm].w = 0; nm++; }
        if (m < nm) mods[m].w += e->w;
        if (i < 40) {
            const char *b = strrchr(w.mod, '/');
            char sym[96] = "", x[72]; u32 d = 0;
            if (w.kind == 2 && host_export(e->rip, x, sizeof x, &d)) snprintf(sym, sizeof sym, " (%s+0x%x)", x, d);
            LOGI("sample: hot %5.2f%% %s+0x%llx%s [%s] %u samples", 100.0 * (double)e->w / (double)work, b ? b + 1 : w.mod, w.off, sym,
                 g_thr_name[e->thr][0] ? g_thr_name[e->thr] : "?", e->n);
        }
    }
    for (int k = 0; k < nm && k < 25; k++) {
        int b = k;
        for (int j = k + 1; j < nm; j++) if (mods[j].w > mods[b].w) b = j;
        s_mod t = mods[k]; mods[k] = mods[b]; mods[b] = t;
        LOGI("sample: mod %5.1f%% %s", 100.0 * (double)mods[k].w / (double)work, mods[k].name);
    }
reset:
    memset(g_tab, 0, S_SIZE * sizeof *g_tab);
    g_total = g_lost = g_lost_w = 0;
    xrl_mem_breakdown();
}

static DWORD WINAPI sample_thread(LPVOID p)
{
    DWORD hz = (DWORD)(UINT_PTR)p;
    DWORD every_ms = hz >= 1000 ? 1 : 1000 / hz;
    DWORD dump_s = 30;
    char v[16];
    if (GetEnvironmentVariableA("XRL_SAMPLE_DUMP", v, sizeof v)) dump_s = (DWORD)strtoul(v, NULL, 10);
    if (!dump_s) dump_s = 30;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    /* Sleep(1) wakes on the system tick (up to 15.6 ms); a high-resolution timer keeps the asked rate */
    HANDLE tick = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static struct { HANDLE h; int thr; u64 w; } cand[512];
    static s_prev next[512];
    u64 t_dump = GetTickCount64(), tsc_dump = __rdtsc(), tsc_prev = tsc_dump, own0 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &own0);
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)every_ms * 10000;
        if (tick && SetWaitableTimer(tick, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tick, INFINITE);
        else Sleep(every_ms);
        u64 tsc = __rdtsc(), span = tsc - tsc_prev;   /* thread cycle counts are in TSC ticks too */
        tsc_prev = tsc;
        int nc = 0, nn = 0;
        AcquireSRWLockShared(&G.thread_lock);
        for (xrl_thread *t = G.threads; t && nc < 512; t = t->next) {
            ULONG64 cyc = 0;
            u64 before;
            if (!t->handle || !QueryThreadCycleTime(t->handle, &cyc)) continue;
            if (nn < 512) { next[nn].tid = t->tid; next[nn].cycles = cyc; nn++; }
            if (!cycles_prev(t->tid, &before) || cyc - before < span / 20) continue;   /* under 5% of the tick: idle or only woken by us */
            cand[nc].h = t->handle; cand[nc].thr = thr_index(t->name, t->tid); cand[nc].w = cyc - before;
            nc++;
        }
        for (int i = 0; i < nc; i++) {   /* suspended only between these three calls: no allocation, no lock */
            if (SuspendThread(cand[i].h) == (DWORD)-1) continue;
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(cand[i].h, &c);
            ResumeThread(cand[i].h);
            if (ok) add(c.Rip, cand[i].thr, cand[i].w);
        }
        ReleaseSRWLockShared(&G.thread_lock);
        memcpy(g_prev, next, (size_t)nn * sizeof *next);
        g_nprev = nn;
        u64 now = GetTickCount64();
        if (now - t_dump >= (u64)dump_s * 1000) {
            ULONG64 own = 0;
            QueryThreadCycleTime(GetCurrentThread(), &own);
            dump((double)(now - t_dump) / 1000.0, tsc - tsc_dump, own - own0);
            t_dump = now; tsc_dump = __rdtsc();
            QueryThreadCycleTime(GetCurrentThread(), &own0);
        }
    }
}

/* ---- stall snapshot (XRL_STALL=<ms>, default 250 with XRL_SAMPLE) ----
 * The game's graphics calls reach the host as COM calls (com.c counts them). When they have been flowing (over
 * 1000/s) and then stop for XRL_STALL ms, the frame is stuck: log, for every guest thread, the syscall it is in (or
 * where it runs, sampled), and who holds the memory lock and for how long; again at 4x and 16x the threshold if the
 * stall lasts. At most 40 snapshots a run. */
extern volatile LONG64 xrl_com_calls;
static void stall_snapshot(u64 ms)
{
    static volatile LONG n;
    if (InterlockedIncrement(&n) > 40) return;
    u32 ltid; u64 lus;
    xrl_mem_lock_owner(&ltid, &lus);
    LOGW("stall: no graphics call for %llu ms; memory lock %s%u (held %llu ms)", ms, ltid ? "held by tid " : "free", ltid,
         lus / 1000);
    AcquireSRWLockShared(&G.thread_lock);
    for (xrl_thread *t = G.threads; t; t = t->next) {
        int nr = t->cur_nr;
        if (t->in_sys > 0 && nr >= 0) {
            LOGW("stall:   %s/%d in %s%s", t->name, t->tid, xrl_sysname(nr), (u32)t->win_tid == ltid ? " (holds the memory lock)" : "");
            continue;
        }
        if (!t->handle || SuspendThread(t->handle) == (DWORD)-1) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(t->handle, &c);
        ResumeThread(t->handle);
        if (!ok) continue;
        s_where w; where_of(c.Rip, &w);
        char ex[96] = ""; u32 d = 0;
        if (w.kind == 2 && host_export(c.Rip, ex, sizeof ex, &d)) LOGW("stall:   %s/%d running %s!%s+0x%x%s", t->name, t->tid, w.mod, ex, d,
                                                                       (u32)t->win_tid == ltid ? " (holds the memory lock)" : "");
        else LOGW("stall:   %s/%d running %s+0x%llx%s", t->name, t->tid, w.mod, w.off, (u32)t->win_tid == ltid ? " (holds the memory lock)" : "");
    }
    ReleaseSRWLockShared(&G.thread_lock);
}

static DWORD WINAPI stall_thread(LPVOID p)
{
    const u64 lim = (u64)(UINT_PTR)p;
    LONG64 last = xrl_com_calls, rate_mark = last;
    u64 t_last = GetTickCount64(), t_rate = t_last, next = lim;
    int active = 0;
    for (;;) {
        Sleep(20);
        LONG64 c = xrl_com_calls;
        u64 now = GetTickCount64();
        if (now - t_rate >= 1000) { active = c - rate_mark > 1000; rate_mark = c; t_rate = now; }
        if (c != last) { last = c; t_last = now; next = lim; continue; }
        if (active && now - t_last >= next) { stall_snapshot(now - t_last); next *= 4; }
    }
}

void xrl_sample_start(void)
{
    {
        char sv[16];
        u64 ms = GetEnvironmentVariableA("XRL_STALL", sv, sizeof sv) ? strtoull(sv, NULL, 10)
               : GetEnvironmentVariableA("XRL_SAMPLE", sv, sizeof sv) ? 250 : 0;
        if (ms) {
            HANDLE sh = CreateThread(NULL, 64 << 10, stall_thread, (LPVOID)(UINT_PTR)ms, 0, NULL);
            if (sh) CloseHandle(sh);
            LOGI("stall: a snapshot of every guest thread when graphics calls stop for %llu ms (XRL_STALL)", ms);
        }
    }
    char v[16];
    if (!GetEnvironmentVariableA("XRL_SAMPLE", v, sizeof v)) return;
    DWORD hz = (DWORD)strtoul(v, NULL, 10);
    if (!hz) return;
    g_tab = (s_ent *)calloc(S_SIZE, sizeof *g_tab);
    if (!g_tab) return;
    HANDLE h = CreateThread(NULL, 256 << 10, sample_thread, (LPVOID)(UINT_PTR)hz, 0, NULL);
    if (h) CloseHandle(h);
    LOGI("sample: sampling running guest threads at %lu Hz (XRL_SAMPLE), a report every XRL_SAMPLE_DUMP s", hz);
}
