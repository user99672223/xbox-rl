/*
 * perf.c - XRL_PERF=N (default 10; 0 = off): every N seconds one block of numbers in the log,
 * on the desktop and on the console alike, for the interval just ended:
 *
 *   perf: cpu   - the whole process (all host threads: GPU driver, audio, wineserver too), in % of
 *                 one core, and user/kernel split
 *   perf: guest - syscalls, guest->host COM calls (D3D), lazy-commit faults and the time their
 *                 handler took (swap-ins included), committed memory, swap file use and traffic
 *   perf: thr   - the busiest guest threads by CPU, with their names (prctl / comm) and syscalls
 *
 * The frame rate is logged by gfx/ (d3d11_swapchain.cpp) on the same interval. Cheap: a few
 * GetThreadTimes calls under the thread list's shared lock, nothing on any hot path except the
 * per-thread syscall counter (xrl_thread.nsys, a plain increment by the owning thread).
 */
#include "xrl_internal.h"

#define PF_MAX 512
typedef struct pf_prev { int tid; u64 cpu, nsys; } pf_prev;
static pf_prev g_prev[PF_MAX];
static int g_nprev;

typedef struct pf_row { int tid; u64 cpu, nsys; char name[16]; } pf_row;

static u64 ft64(FILETIME f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static const pf_prev *prev_of(int tid)
{
    for (int i = 0; i < g_nprev; i++) if (g_prev[i].tid == tid) return &g_prev[i];
    return NULL;
}

static DWORD WINAPI perf_thread(LPVOID p)
{
    DWORD every = (DWORD)(UINT_PTR)p;
    static pf_row rows[PF_MAX];
    static pf_prev next[PF_MAX];
    u64 last_t = GetTickCount64(), last_user = 0, last_kern = 0;
    u64 last_com = 0, last_flt = 0, last_flt_ticks = 0, last_sys = 0, last_out = 0, last_in = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    FILETIME c0, e0, k0, u0;
    if (GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0)) { last_user = ft64(u0); last_kern = ft64(k0); }
    for (;;) {
        Sleep(every * 1000);
        u64 now = GetTickCount64();
        double secs = (double)(now - last_t) / 1000.0;
        if (secs <= 0) continue;
        last_t = now;

        FILETIME c, e, k, u;
        u64 user = 0, kern = 0;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { user = ft64(u); kern = ft64(k); }
        double pu = (double)(user - last_user) / 1e5 / secs, pk = (double)(kern - last_kern) / 1e5 / secs;   /* 100 ns -> % */
        last_user = user; last_kern = kern;

        int n = 0, nn = 0, nthr = 0;
        u64 sys_all = 0;
        AcquireSRWLockShared(&G.thread_lock);
        for (xrl_thread *t = G.threads; t && n < PF_MAX; t = t->next) {
            FILETIME tc, te, tk, tu;
            nthr++;
            sys_all += t->nsys;
            if (!t->handle || !GetThreadTimes(t->handle, &tc, &te, &tk, &tu)) continue;
            u64 cpu = ft64(tk) + ft64(tu);
            const pf_prev *pp = prev_of(t->tid);
            pf_row *r = &rows[n++];
            r->tid = t->tid;
            r->cpu = cpu - (pp && pp->cpu <= cpu ? pp->cpu : 0);
            r->nsys = t->nsys - (pp && pp->nsys <= t->nsys ? pp->nsys : 0);
            memcpy(r->name, t->name, sizeof r->name);
            r->name[sizeof r->name - 1] = 0;
            if (nn < PF_MAX) { next[nn].tid = t->tid; next[nn].cpu = cpu; next[nn].nsys = t->nsys; nn++; }
        }
        ReleaseSRWLockShared(&G.thread_lock);
        memcpy(g_prev, next, nn * sizeof *next);
        g_nprev = nn;
        /* exited threads take their counts with them: the interval's total can only be estimated
         * from the live ones, so the sum is clamped at 0 rather than going negative */
        u64 dsys = sys_all >= last_sys ? sys_all - last_sys : 0;
        last_sys = sys_all;

        extern volatile LONG64 xrl_com_calls, xrl_lazy_faults, xrl_lazy_fault_ticks;
        u64 com = (u64)xrl_com_calls, flt = (u64)xrl_lazy_faults, fltt = (u64)xrl_lazy_fault_ticks;
        u64 sn, so, si;
        xrl_mem_swap_stats(&sn, &so, &si);
        LOGI("perf: cpu %.0f%% of a core (user %.0f%%, kernel %.0f%%) over %.1f s; %d guest threads", pu + pk, pu, pk, secs, nthr);
        LOGI("perf: guest %.0f syscalls/s, %.0f COM calls/s, %.0f lazy faults/s (%.0f ms/s in the handler); "
             "committed %llu MiB, swap %llu MiB in the file, out %.1f MiB/s, in %.1f MiB/s",
             (double)dsys / secs, (double)(com - last_com) / secs, (double)(flt - last_flt) / secs,
             (double)(fltt - last_flt_ticks) * 1000.0 / (double)qf.QuadPart / secs,
             xrl_mem_committed() >> 20, sn >> 20,
             (double)(so - last_out) / 1048576.0 / secs, (double)(si - last_in) / 1048576.0 / secs);
        last_com = com; last_flt = flt; last_flt_ticks = fltt; last_out = so; last_in = si;

        /* the busiest threads, by CPU */
        char line[600];
        int len = snprintf(line, sizeof line, "perf: thr");
        for (int shown = 0; shown < 8; shown++) {
            int b = -1;
            for (int i = 0; i < n; i++) if (rows[i].tid && (b < 0 || rows[i].cpu > rows[b].cpu)) b = i;
            if (b < 0 || rows[b].cpu < 100000ull * (u64)secs / 2) break;   /* < 0.5% of a core */
            int w = snprintf(line + len, sizeof line - len, " %s%s%d %.0f%% %.0fk/s;", rows[b].name, rows[b].name[0] ? "/" : "",
                             rows[b].tid, (double)rows[b].cpu / 1e5 / secs, (double)rows[b].nsys / 1000.0 / secs);
            if (w < 0 || w >= (int)(sizeof line) - len) break;
            len += w;
            rows[b].tid = 0;
        }
        LOGI("%s", line);
    }
}

void xrl_perf_start(void)
{
    char v[16];
    DWORD s = 10;
    if (GetEnvironmentVariableA("XRL_PERF", v, sizeof v)) s = (DWORD)strtoul(v, NULL, 10);
    if (!s) return;
    HANDLE h = CreateThread(NULL, 256 << 10, perf_thread, (LPVOID)(UINT_PTR)s, 0, NULL);
    if (h) CloseHandle(h);
    LOGI("perf: numbers every %lu s (XRL_PERF)", s);
}
