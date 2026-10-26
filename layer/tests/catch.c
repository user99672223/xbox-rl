/*
 * xrl_catch.exe - minimal debugger wrapper for the desktop test host.
 *
 *   xrl_catch.exe <program> [args...]
 *
 * Runs the program under DEBUG_ONLY_THIS_PROCESS and reports every exception
 * the kernel raises in it (to stderr), with registers and the module holding
 * rip. It sees faults the in-process VEH never gets: the kernel terminates a
 * process whose fault cannot be dispatched in user mode (e.g. rsp points at
 * unwritable memory), and only a debugger hears about it. The program's own
 * stdout/stderr are inherited.
 *
 * First-chance access violations from xrl_cfu() probes are normal; the
 * interesting ones are the last few before "exit".
 *
 * XRL_CATCH_ATTACH=N: run the program normally and attach only after N
 * seconds (lazy commit and swap make every early fault a debugger stop), then
 * report second-chance exceptions and first-chance ones other than access
 * violations (fail-fast, stack overruns, C++ throws). If the program exits
 * before that, its exact exit status is printed.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

/* function name for addr via DbgHelp (PDBs next to the images), or "" */
static const char *sym_of(HANDLE proc, ULONG64 addr)
{
    static char buf[sizeof(SYMBOL_INFO) + 256];
    static char out[300];
    SYMBOL_INFO *s = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    if (!SymFromAddr(proc, addr, &disp, s)) return "";
    snprintf(out, sizeof out, " %s+%llx", s->Name, disp);
    return out;
}

typedef struct { LPVOID base; char name[MAX_PATH]; } mod;
static mod g_mods[512];
static int g_nmods;

static void add_mod(HANDLE h, LPVOID base)
{
    if (g_nmods >= (int)(sizeof g_mods / sizeof g_mods[0])) return;
    char path[MAX_PATH] = "?";
    if (h) GetFinalPathNameByHandleA(h, path, sizeof path, 0);
    const char *b = strrchr(path, '\\');
    g_mods[g_nmods].base = base;
    strncpy_s(g_mods[g_nmods].name, sizeof g_mods[0].name, b ? b + 1 : path, _TRUNCATE);
    g_nmods++;
}

/* nearest module base below addr (good enough for a crash line) */
static const char *mod_of(ULONG64 addr, ULONG64 *off)
{
    int best = -1;
    for (int i = 0; i < g_nmods; i++)
        if ((ULONG64)g_mods[i].base <= addr && (best < 0 || g_mods[i].base > g_mods[best].base)) best = i;
    if (best < 0 || addr - (ULONG64)g_mods[best].base > 0x4000000) { *off = 0; return "?"; }
    *off = addr - (ULONG64)g_mods[best].base;
    return g_mods[best].name;
}

/* XRL_CATCH_DUMP=N: N seconds in, print every thread's rip/rsp and the code
 * addresses found on its stack (where a hung process is waiting). */
static void dump_threads(HANDLE proc, HANDLE *threads, DWORD *tids, int n)
{
    fprintf(stderr, "[catch] ===== thread dump (%d threads) =====\n", n);
    static int sym_ready;
    if (!sym_ready) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); sym_ready = SymInitialize(proc, NULL, TRUE) ? 1 : -1; }
    for (int i = 0; i < n; i++) {
        if (!threads[i]) continue;
        CONTEXT c; c.ContextFlags = CONTEXT_FULL;
        SuspendThread(threads[i]);
        if (GetThreadContext(threads[i], &c)) {
            ULONG64 off;
            const char *m = mod_of(c.Rip, &off);
            fprintf(stderr, "[catch] tid %lu rip=%016llx (%s+%llx%s) rsp=%016llx\n", tids[i], c.Rip, m, off, sym_of(proc, c.Rip), c.Rsp);
            ULONG64 stk[512]; SIZE_T got = 0;
            ReadProcessMemory(proc, (LPCVOID)c.Rsp, stk, sizeof stk, &got);
            int shown = 0;
            for (size_t k = 0; k < got / 8 && shown < 14; k++) {
                const char *mm = mod_of(stk[k], &off);
                int guest = stk[k] >= 0x6ff000000000ull && stk[k] < 0x700000000000ull;   /* Wine PE builtins */
                if (strcmp(mm, "?") || guest || (stk[k] >= 0x140000000ull && stk[k] < 0x150000000ull)) {
                    fprintf(stderr, "[catch]     [rsp+%04zx] %016llx %s+%llx%s\n", k * 8, stk[k], mm, off, sym_of(proc, stk[k]));
                    shown++;
                }
            }
        }
        ResumeThread(threads[i]);
    }
    fflush(stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: xrl_catch <program> [args...]\n"); return 2; }
    char cmd[32768] = "";
    for (int i = 1; i < argc; i++) {
        strcat_s(cmd, sizeof cmd, "\"");
        strcat_s(cmd, sizeof cmd, argv[i]);
        strcat_s(cmd, sizeof cmd, i + 1 < argc ? "\" " : "\"");
    }
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    char av[16];
    DWORD attach = GetEnvironmentVariableA("XRL_CATCH_ATTACH", av, sizeof av) ? strtoul(av, NULL, 10) : 0;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, attach ? 0 : DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[catch] CreateProcess failed: %lu\n", GetLastError());
        return 2;
    }
    if (attach) {
        if (WaitForSingleObject(pi.hProcess, attach * 1000) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            fprintf(stderr, "[catch] exited before attaching: status %08lx\n", code);
            return (int)code;
        }
        if (!DebugActiveProcess(pi.dwProcessId)) { fprintf(stderr, "[catch] attach failed: %lu\n", GetLastError()); return 2; }
        fprintf(stderr, "[catch] attached after %lu s\n", attach);
        fflush(stderr);
    }
    static HANDLE threads[4096];   /* by tid % 4096 */
    static HANDLE live[4096]; static DWORD live_tid[4096]; int nlive = 0;
    DWORD exit_code = 0;
    int first_bp = 1;
    char dv[16];
    ULONGLONG dump_at = GetEnvironmentVariableA("XRL_CATCH_DUMP", dv, sizeof dv) ? GetTickCount64() + 1000ull * strtoul(dv, NULL, 10) : 0;
    for (;;) {
        DEBUG_EVENT ev;
        if (!WaitForDebugEvent(&ev, 500)) {
            if (GetLastError() != ERROR_SEM_TIMEOUT) break;
            if (dump_at && GetTickCount64() >= dump_at) {
                nlive = 0;
                for (int i = 0; i < 4096; i++) if (threads[i]) { live[nlive] = threads[i]; live_tid[nlive] = GetThreadId(threads[i]); nlive++; }
                dump_threads(pi.hProcess, live, live_tid, nlive);
                static int dumps;
                dump_at = ++dumps < 3 ? GetTickCount64() + 3000 : 0;   /* three dumps, 3 s apart */
            }
            continue;
        }
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            add_mod(ev.u.CreateProcessInfo.hFile, ev.u.CreateProcessInfo.lpBaseOfImage);
            threads[ev.dwThreadId % 4096] = ev.u.CreateProcessInfo.hThread;
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            threads[ev.dwThreadId % 4096] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            threads[ev.dwThreadId % 4096] = NULL;
            break;
        case LOAD_DLL_DEBUG_EVENT:
            add_mod(ev.u.LoadDll.hFile, ev.u.LoadDll.lpBaseOfDll);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *r = &ev.u.Exception.ExceptionRecord;
            if (r->ExceptionCode == EXCEPTION_BREAKPOINT && first_bp) { first_bp = 0; break; }   /* loader breakpoint */
            cont = DBG_EXCEPTION_NOT_HANDLED;
            if (r->ExceptionCode == 0x406D1388 /* thread name */ || r->ExceptionCode == 0x40010006 /* OutputDebugString */) break;
            if (r->ExceptionCode == 0x4001000A /* OutputDebugStringW */) break;
            if (attach && ev.u.Exception.dwFirstChance && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) break;   /* lazy commit / swap */
            CONTEXT c; c.ContextFlags = CONTEXT_FULL;
            HANDLE th = threads[ev.dwThreadId % 4096];
            ULONG64 off;
            const char *m = mod_of((ULONG64)r->ExceptionAddress, &off);
            fprintf(stderr, "[catch] %s-chance exception %08lx at %p (%s+%llx) tid=%lu",
                    ev.u.Exception.dwFirstChance ? "first" : "SECOND", r->ExceptionCode, r->ExceptionAddress, m, off, ev.dwThreadId);
            if (r->NumberParameters >= 2) fprintf(stderr, " info=%llu addr=%016llx", (ULONG64)r->ExceptionInformation[0], (ULONG64)r->ExceptionInformation[1]);
            fputc('\n', stderr);
            if (th && GetThreadContext(th, &c)) {
                fprintf(stderr, "[catch]   rip=%016llx rsp=%016llx rbp=%016llx rax=%016llx rbx=%016llx\n", c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx);
                fprintf(stderr, "[catch]   rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx\n", c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9);
                fprintf(stderr, "[catch]   r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n", c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
                UCHAR code[48]; SIZE_T cgot = 0;   /* the bytes around rip: what the CPU was running (rewritten code, stubs) */
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)(c.Rip - 24), code, sizeof code, &cgot) && cgot == sizeof code) {
                    fprintf(stderr, "[catch]   code[rip-24..+24]:");
                    for (int k = 0; k < 48; k++) fprintf(stderr, k == 24 ? " |%02x" : " %02x", code[k]);
                    fputc('\n', stderr);
                }
                ULONG64 stk[6] = {0}; SIZE_T got = 0;
                if (ReadProcessMemory(pi.hProcess, (LPCVOID)c.Rsp, stk, sizeof stk, &got) && got)
                    fprintf(stderr, "[catch]   [rsp] %016llx %016llx %016llx %016llx %016llx %016llx\n", stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
                else
                    fprintf(stderr, "[catch]   [rsp] unreadable\n");
                if (!ev.u.Exception.dwFirstChance) {   /* the code addresses on its stack: who called */
                    static int sym_ready;
                    if (!sym_ready) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); sym_ready = SymInitialize(pi.hProcess, NULL, TRUE) ? 1 : -1; }
                    ULONG64 big[1024]; SIZE_T n = 0;
                    ReadProcessMemory(pi.hProcess, (LPCVOID)c.Rsp, big, sizeof big, &n);
                    for (size_t k = 0, shown = 0; k < n / 8 && shown < 24; k++) {
                        ULONG64 o; const char *mm = mod_of(big[k], &o);
                        if (!strcmp(mm, "?")) continue;
                        fprintf(stderr, "[catch]     [rsp+%04zx] %016llx %s+%llx%s\n", k * 8, big[k], mm, o, sym_of(pi.hProcess, big[k]));
                        shown++;
                    }
                }
            }
            fflush(stderr);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_code = ev.u.ExitProcess.dwExitCode;
            fprintf(stderr, "[catch] exit %08lx\n", exit_code);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return (int)exit_code;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    return (int)exit_code;
}
