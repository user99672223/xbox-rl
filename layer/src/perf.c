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

typedef struct pf_row { int tid; u64 cpu, nsys; char name[16]; u32 topn[5]; u16 topnr[5]; u32 srvn[6]; u16 srvr[6]; } pf_row;

/* XRL_SRVREQ: the thread's six most frequent Wine server requests since the last read (counts reset) */
static void take_srv(xrl_thread *t, pf_row *r)
{
    memset(r->srvn, 0, sizeof r->srvn);
    if (!t->srvreq) return;
    for (int q = 0; q < XRL_SRVREQ_MAX; q++) {
        u32 c = t->srvreq[q];
        if (!c) continue;
        t->srvreq[q] = 0;
        for (int k = 0; k < 6; k++)
            if (c > r->srvn[k]) {
                memmove(&r->srvn[k + 1], &r->srvn[k], (5 - k) * sizeof *r->srvn);
                memmove(&r->srvr[k + 1], &r->srvr[k], (5 - k) * sizeof *r->srvr);
                r->srvn[k] = c; r->srvr[k] = (u16)q;
                break;
            }
    }
}

/* XRL_PROFILE: the thread's five most frequent syscalls since the last read (counts reset) */
static void take_top(xrl_thread *t, pf_row *r)
{
    memset(r->topn, 0, sizeof r->topn);
    if (!t->sysn) return;
    for (int nr = 0; nr < XRL_NR_MAX; nr++) {
        u32 c = t->sysn[nr];
        if (!c) continue;
        t->sysn[nr] = 0;   /* races with the owner's increment: a lost count or two */
        for (int k = 0; k < 5; k++)
            if (c > r->topn[k]) {
                memmove(&r->topn[k + 1], &r->topn[k], (4 - k) * sizeof *r->topn);
                memmove(&r->topnr[k + 1], &r->topnr[k], (4 - k) * sizeof *r->topnr);
                r->topn[k] = c; r->topnr[k] = (u16)nr;
                break;
            }
    }
}

static u64 ft64(FILETIME f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* A thread that spent the whole interval on the CPU without one syscall is spinning in its own
 * code (the console's startup stall): where. Registers are taken with the thread suspended for
 * a moment (under the thread list's shared lock); its stack is read afterwards with
 * after a VirtualQuery (only what is committed and readable). */
typedef struct pf_spin { int tid; char name[16]; u64 rip, rsp, pend, mask; int in_sys, guest_live, sig_jump, gctx; } pf_spin;
static int spin_sample(xrl_thread *t, pf_spin *s)
{
    if (SuspendThread(t->handle) == (DWORD)-1) return 0;
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = GetThreadContext(t->handle, &c);
    ResumeThread(t->handle);
    if (!ok) return 0;
    s->tid = t->tid; s->rip = c.Rip; s->rsp = c.Rsp;
    /* why a signal (Wine's SIGUSR1 for a system APC, e.g. an async read's completion) might not get into it */
    s->pend = t->sigpending; s->mask = t->sigmask; s->in_sys = t->in_sys; s->guest_live = t->teb_guest_live;
    s->sig_jump = t->sig_jump != 0; s->gctx = xrl_mem_guest_ctx(c.Rip, c.Rsp);
    memcpy(s->name, t->name, sizeof s->name);
    s->name[sizeof s->name - 1] = 0;
    return 1;
}
static void spin_report(const pf_spin *s)
{
    char nm[96], line[700];
    u64 off = 0;
    int len = snprintf(line, sizeof line, "perf: spin %s/%d rip %llx", s->name, s->tid, s->rip);
    if (xrl_mem_name_of(s->rip, nm, sizeof nm, &off) == 1)
        len += snprintf(line + len, sizeof line - len, " (%s+0x%llx)", nm[0] ? nm : "anon", off);
    else
        len += snprintf(line + len, sizeof line - len, " (host)");
    xrl_guest_bt(s->rsp, line + len, (int)sizeof line - len, 8);
    LOGI("%s", line);
    char st[300];
    xrl_sig_stats(st, sizeof st);
    LOGI("perf: spin %s/%d signal state: pending %llx blocked %llx, in_sys %d, guest view %d, sig_jump %d, guest ctx %d; %s",
         s->name, s->tid, s->pend, s->mask, s->in_sys, s->guest_live, s->sig_jump, s->gctx, st);
}

/* Return addresses into guest code on the top of a guest stack (a scan, not an unwind: a stale one may show up) */
int xrl_guest_bt(u64 rsp, char *out, int n, int max)
{
    char nm[96];
    u64 off = 0, stk[512];
    SIZE_T got = 0;
    MEMORY_BASIC_INFORMATION mbi;
    int len = 0;
    if (n > 0) out[0] = 0;
    if (VirtualQuery((void *)rsp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
        !(mbi.Protect & PAGE_GUARD)) {
        u64 end = (u64)mbi.BaseAddress + mbi.RegionSize;
        got = end - rsp < sizeof stk ? (SIZE_T)(end - rsp) : sizeof stk;
        memcpy(stk, (void *)rsp, got);
    }
    int shown = 0;
    for (size_t i = 0; i < got / 8 && shown < max && len < n - 120; i++) {
        if (stk[i] < 0x10000 || xrl_mem_is_exec(stk[i]) != 1) continue;
        if (xrl_mem_name_of(stk[i], nm, sizeof nm, &off) != 1) continue;
        const char *b = strrchr(nm, '/');
        len += snprintf(out + len, n - len, " <- %llx %s+0x%llx", stk[i], b ? b + 1 : (nm[0] ? nm : "anon"), off);
        shown++;
    }
    return len;
}

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

        int n = 0, nn = 0, nthr = 0, nspin = 0;
        pf_spin spins[2];
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
            take_top(t, r);
            take_srv(t, r);
            memcpy(r->name, t->name, sizeof r->name);
            r->name[sizeof r->name - 1] = 0;
            if (r->nsys == 0 && (double)r->cpu >= 0.9e7 * secs && nspin < 2 && spin_sample(t, &spins[nspin])) nspin++;
            if (nn < PF_MAX) { next[nn].tid = t->tid; next[nn].cpu = cpu; next[nn].nsys = t->nsys; nn++; }
        }
        ReleaseSRWLockShared(&G.thread_lock);
        for (int i = 0; i < nspin; i++) spin_report(&spins[i]);
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
        {   /* XRL_PROFILE: the argument scan after each COM call, and one TEB view swap pair */
            extern volatile LONG64 xrl_com_after_ticks;
            static u64 last_after;
            u64 at = (u64)xrl_com_after_ticks;
            if (at) LOGI("perf: com scan %.1f ms/s (%.2f us per call)", (double)(at - last_after) * 1000.0 / (double)qf.QuadPart / secs,
                         com > last_com ? (double)(at - last_after) * 1e6 / (double)qf.QuadPart / (double)(com - last_com) : 0.0);
            last_after = at;
        }
        {   /* the swap: eviction work, write/read cost (the boot's long load is mostly this) */
            extern volatile LONG64 xrl_lazy_faults;
            static u64 last_so;
            u64 sn2, so2, si2;
            xrl_mem_swap_stats(&sn2, &so2, &si2);
            if (so2 != last_so) { char cb[600]; xrl_mem_clock_stats(cb, sizeof cb); LOGI("perf: swap %s", cb); }
            last_so = so2;
        }
        {   /* com.c's argument scan: pointers checked, and how many needed a VirtualQuery */
            extern volatile LONG64 xrl_com_probes, xrl_com_probe_misses, xrl_com_scans;
            static u64 lp, lm, ls;
            u64 np = (u64)xrl_com_probes, nmi = (u64)xrl_com_probe_misses, ns = (u64)xrl_com_scans;
            LOGI("perf: com scan %.0f calls/s scanned, %.0f pointer checks/s, %.0f/s needed a VirtualQuery", (double)(ns - ls) / secs,
                 (double)(np - lp) / secs, (double)(nmi - lm) / secs);
            lp = np; lm = nmi; ls = ns;
        }
        {   /* every I/O state change wakes every thread asleep in poll/read/...: how many per second */
            extern volatile LONG64 xrl_io_notifies, xrl_io_woken;
            extern volatile LONG xrl_io_sleepers;
            static u64 ln, lw;
            u64 nn = (u64)xrl_io_notifies, nw = (u64)xrl_io_woken;
            LOGI("perf: io wake-ups: %.0f notifies/s, %.0f sleepers woken/s (%ld asleep now)", (double)(nn - ln) / secs,
                 (double)(nw - lw) / secs, (long)xrl_io_sleepers);
            ln = nn; lw = nw;
        }
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
            if (shown < 3 && rows[b].topn[0]) {   /* XRL_PROFILE: what the three busiest threads call */
                char sub[300]; int sl = 0;
                for (int k = 0; k < 5 && rows[b].topn[k]; k++)
                    sl += snprintf(sub + sl, sizeof sub - sl, " %s %.0f/s", xrl_sysname(rows[b].topnr[k]), (double)rows[b].topn[k] / secs);
                LOGI("perf: thr %s/%d calls%s", rows[b].name, rows[b].tid, sub);
            }
            if (shown < 3 && rows[b].srvn[0]) {   /* XRL_SRVREQ: its Wine server requests, by code (names: wineserver's req_names) */
                char sub[300]; int sl = 0;
                for (int k = 0; k < 6 && rows[b].srvn[k]; k++)
                    sl += snprintf(sub + sl, sizeof sub - sl, " req%u %.0f/s", rows[b].srvr[k], (double)rows[b].srvn[k] / secs);
                LOGI("perf: thr %s/%d server requests%s", rows[b].name, rows[b].tid, sub);
            }
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
