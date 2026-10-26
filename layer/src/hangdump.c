/*
 * hangdump.c - XRL_HANG_DUMP=N (desktop builds): every N seconds, print where
 * every thread of the process is.
 *
 * A debugger (layer/tests/catch.c) is useless once swap and lazy commit are
 * busy: every first-chance fault stops in it. This runs in-process instead:
 * suspend all other threads, copy their registers and the top of their stacks,
 * resume them, then name the addresses: layer/host code through DbgHelp
 * (loaded at boot) and a module list read with psapi, guest code through the
 * VMA names. Nothing here takes the loader lock (a hung thread may hold it; the
 * dump says whether one does). Written with WriteFile to stderr, not the log
 * (a hung thread may hold the log lock).
 */
#include "xrl_internal.h"
#include <tlhelp32.h>
#include <psapi.h>

#if !(defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
#include <dbghelp.h>

#define HD_MAX 256
#define HD_STK 192
#define HD_GSTK 2048   /* qwords of guest stack above a syscall's frame */
typedef struct hd_thread { DWORD tid; int guest, in_sys, live; u64 rip, rsp, pend, mask; u64 stk[HD_STK]; int nstk;
                           u64 grip, grsp; u64 gstk[HD_GSTK]; int ngstk;
                           u64 pe_lo, pe_hi, pe_rsp; u64 pstk[HD_GSTK]; int npstk; char name[16]; } hd_thread;
static hd_thread g_hd[HD_MAX];

static void out(const char *fmt, ...)
{
    char line[700];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);
    line[n++] = '\n';
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)n, &w, NULL);
}

static void hot_line(const char *l) { out("[hang]   %s", l); }

typedef BOOL (WINAPI *sym_init_fn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *sym_from_fn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef DWORD (WINAPI *sym_opt_fn)(DWORD);
typedef DWORD64 (WINAPI *sym_load_fn)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
static sym_from_fn g_symfrom;
static sym_load_fn g_symload;

#define HD_MODS 512
typedef struct hd_mod { u64 base, size; char name[64]; } hd_mod;
static hd_mod g_mods[HD_MODS];
static int g_nmods;

/* psapi reads the PEB's module list without the loader lock */
static void refresh_modules(void)
{
    HMODULE hs[HD_MODS];
    DWORD need = 0;
    HANDLE p = GetCurrentProcess();
    if (!K32EnumProcessModules(p, hs, sizeof hs, &need)) return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > HD_MODS) n = HD_MODS;
    for (int i = 0; i < n; i++) {
        int k;
        for (k = 0; k < g_nmods; k++) if (g_mods[k].base == (u64)hs[i]) break;
        if (k < g_nmods || g_nmods >= HD_MODS) continue;
        MODULEINFO mi;
        if (!K32GetModuleInformation(p, hs[i], &mi, sizeof mi)) continue;
        hd_mod *m = &g_mods[g_nmods++];
        m->base = (u64)hs[i];
        m->size = mi.SizeOfImage;
        char path[MAX_PATH] = "";
        K32GetModuleFileNameExA(p, hs[i], path, sizeof path);
        const char *b = strrchr(path, '\\');
        strncpy_s(m->name, sizeof m->name, b ? b + 1 : path, _TRUNCATE);
        if (g_symload) g_symload(p, NULL, path, NULL, m->base, (DWORD)m->size, NULL, 0);
    }
}

/* PEB->LoaderLock (a CRITICAL_SECTION): its owner's thread id, 0 = free */
static DWORD loader_lock_owner(void)
{
    u64 peb = __readgsqword(0x60);
    CRITICAL_SECTION *cs = *(CRITICAL_SECTION **)(peb + 0x110);
    return cs ? (DWORD)(UINT_PTR)cs->OwningThread : 0;
}

/* "D3D12Core.dll!Fn+0x12", "guest:wine64+0x1234", or "" */
static const char *name_of(u64 a, char *b, size_t n)
{
    b[0] = 0;
    if (a < 0x10000) return b;
    const hd_mod *m = NULL;
    for (int i = 0; i < g_nmods; i++)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) { m = &g_mods[i]; break; }
    if (m) {
        char sb[sizeof(SYMBOL_INFO) + 200];
        SYMBOL_INFO *s = (SYMBOL_INFO *)sb;
        memset(sb, 0, sizeof sb);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 199;
        DWORD64 d = 0;
        if (g_symfrom && g_symfrom(GetCurrentProcess(), a, &d, s)) snprintf(b, n, "%s!%s+0x%llx", m->name, s->Name, d);
        else snprintf(b, n, "%s+0x%llx", m->name, a - m->base);
        return b;
    }
    char vn[48];
    u64 off;
    if (xrl_mem_name_of(a, vn, sizeof vn, &off) == 1) snprintf(b, n, "guest:%s+0x%llx", vn[0] ? vn : "anon", off);
    return b;
}

static void dump_once(void)
{
    int n = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && n < HD_MAX; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        hd_thread *d = &g_hd[n];
        memset(d, 0, sizeof *d);
        d->tid = te.th32ThreadID;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &c)) {
                d->rip = c.Rip;
                d->rsp = c.Rsp;
                /* the layer's view of the thread, read while it is stopped so it matches rip */
                for (xrl_thread *t = G.threads; t; t = t->next)
                    if (t->win_tid == d->tid) {
                        d->guest = 1; d->in_sys = t->in_sys; d->live = t->teb_guest_live;
                        d->pend = t->sigpending; d->mask = t->sigmask;
                        memcpy(d->name, t->name, sizeof d->name); d->name[15] = 0;
                        /* in a syscall: the guest's own state, from its gateway frame (r15..rbp, rip; the stack above) */
                        const u64 *gf = (const u64 *)t->frame;
                        if (t->in_sys && gf) {
                            __try {
                                d->grip = gf[6]; d->grsp = (u64)(gf + 7);
                                memcpy(d->gstk, gf + 7, sizeof d->gstk); d->ngstk = HD_GSTK;
                            } __except (EXCEPTION_EXECUTE_HANDLER) { d->ngstk = 0; }
                            /* Wine's Unix side runs on its own stack; the Windows-side (PE) stack is the
                             * guest TEB view's StackLimit..StackBase (swap rows 0/1), and Wine's syscall
                             * frame on the Unix stack holds the PE rsp: the lowest value in that range */
                            d->pe_hi = t->teb_guest[0]; d->pe_lo = t->teb_guest[1];
                            for (int k = 0; k < d->ngstk; k++)
                                if (d->gstk[k] >= d->pe_lo && d->gstk[k] < d->pe_hi && !(d->gstk[k] & 7) &&
                                    (!d->pe_rsp || d->gstk[k] < d->pe_rsp)) d->pe_rsp = d->gstk[k];
                            if (d->pe_rsp) {
                                u64 nq = (d->pe_hi - d->pe_rsp) / 8;
                                if (nq > HD_GSTK) nq = HD_GSTK;
                                __try { memcpy(d->pstk, (const void *)d->pe_rsp, nq * 8); d->npstk = (int)nq; }
                                __except (EXCEPTION_EXECUTE_HANDLER) { d->npstk = 0; }
                            }
                        }
                        break;
                    }
                __try { memcpy(d->stk, (void *)c.Rsp, sizeof d->stk); d->nstk = HD_STK; }
                __except (EXCEPTION_EXECUTE_HANDLER) { d->nstk = 0; }
            }
            ResumeThread(h);
            n++;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    u64 sn, so, si;
    xrl_mem_swap_stats(&sn, &so, &si);
    refresh_modules();
    out("[hang] ===== %d threads; committed %llu MiB, swap %llu MiB in the file; memory lock %s; loader lock owner tid %lu =====",
        n, xrl_mem_committed() >> 20, sn >> 20, xrl_mem_lock_held() ? "HELD" : "free", loader_lock_owner());
    char b[300];
    xrl_mem_clock_stats(b, sizeof b);
    out("[hang] %s", b);
    xrl_sig_stats(b, sizeof b);
    out("[hang] %s", b);
    extern volatile LONG64 xrl_com_calls;
    out("[hang] guest->host COM calls so far: %lld; the busiest methods:", (long long)xrl_com_calls);
    xrl_com_hot(hot_line, 40);
    out("[hang] other host COM methods called:");
    xrl_com_hot(hot_line, -1);
    for (int i = 0; i < n; i++) {
        hd_thread *d = &g_hd[i];
        out("[hang] tid %lu%s%s%s in_sys=%d guest_view=%d sigpend=%llx sigmask=%llx rip=%llx %s", d->tid, d->guest ? " (guest)" : "",
            d->name[0] ? " " : "", d->name,
            d->in_sys, d->live, d->pend, d->mask, d->rip, name_of(d->rip, b, sizeof b));
        int shown = 0;
        for (int k = 0; k < d->nstk && shown < 12; k++) {
            name_of(d->stk[k], b, sizeof b);
            if (!b[0] || !strncmp(b, "guest:anon", 10)) continue;
            out("[hang]     [rsp+%03x] %s", k * 8, b);
            shown++;
        }
        if (!d->grip) continue;
        out("[hang]   guest at syscall: rip=%llx %s rsp=%llx", d->grip, name_of(d->grip, b, sizeof b), d->grsp);
        shown = 0;
        for (int k = 0; k < d->ngstk && shown < 20; k++) {
            if (xrl_mem_is_exec(d->gstk[k]) != 1) continue;
            out("[hang]     guest [+%04x] %s", k * 8, name_of(d->gstk[k], b, sizeof b));
            shown++;
        }
        if (!d->npstk) continue;
        out("[hang]   PE stack from %llx (%llx..%llx):", d->pe_rsp, d->pe_lo, d->pe_hi);
        shown = 0;
        for (int k = 0; k < d->npstk && shown < 24; k++) {
            if (xrl_mem_is_exec(d->pstk[k]) != 1) continue;
            out("[hang]     pe [+%04x] %016llx %s", k * 8, d->pstk[k], name_of(d->pstk[k], b, sizeof b));
            shown++;
        }
    }
}

static DWORD WINAPI hang_thread(LPVOID p)
{
    DWORD every = (DWORD)(UINT_PTR)p;
    for (;;) { Sleep(every * 1000); dump_once(); }
}

void xrl_hang_watch_start(void)
{
    char v[16];
    if (!GetEnvironmentVariableA("XRL_HANG_DUMP", v, sizeof v)) return;
    DWORD s = (DWORD)strtoul(v, NULL, 10);
    if (!s) return;
    HMODULE dh = LoadLibraryW(L"dbghelp.dll");   /* now: later a hung thread may hold the loader lock */
    if (dh) {
        sym_opt_fn so = (sym_opt_fn)GetProcAddress(dh, "SymSetOptions");
        sym_init_fn si = (sym_init_fn)GetProcAddress(dh, "SymInitialize");
        g_symfrom = (sym_from_fn)GetProcAddress(dh, "SymFromAddr");
        g_symload = (sym_load_fn)GetProcAddress(dh, "SymLoadModuleEx");
        if (so) so(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (!si || !si(GetCurrentProcess(), NULL, FALSE)) { g_symfrom = NULL; g_symload = NULL; }
    }
    refresh_modules();
    HANDLE h = CreateThread(NULL, 256 << 10, hang_thread, (LPVOID)(UINT_PTR)s, 0, NULL);
    if (h) CloseHandle(h);
    LOGI("hang dump: every %lu s (XRL_HANG_DUMP)", s);
}
#else
void xrl_hang_watch_start(void) {}
#endif
