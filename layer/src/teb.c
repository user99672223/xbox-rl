/*
 * teb.c - guest and host views of the shared TEB (docs/gs-teb.md).
 *
 * The guest (Wine's PE side and the game) and the host (Windows ntdll, the CRT,
 * D3D, WinRT and this layer) share one TEB per OS thread, because GS cannot be
 * rebased on Windows. The fields both sides use with different meanings are
 * kept in two per-thread copies; the live TEB holds the guest view while guest
 * code runs and the host view while host code runs. Transitions: the syscall
 * dispatcher, the op-table downcall trampoline, and the vectored exception
 * handler.
 */
#include "xrl_internal.h"

/* {offset, qword count}: the shared table in xrl_abi.h (Wine adopts the same set).
 * TlsSlots[63] (XRL_TP_GS_OFFSET) is not in it: the guest thread pointer stays live. */
#define XRL__TEB_ROW(off, n) { off, n },
static const struct { u16 off, n; } k_swap[] = { XRL_TEB_SWAP_RANGES(XRL__TEB_ROW) };

static int g_swap = 1;

/* Through the TEB's own linear address (NtTib.Self, never swapped) one memcpy per range: a
 * gs-prefixed move per qword cost ~0.17 us per swap pair, paid on every syscall and host COM call. */
static void copy_out(u64 *dst)      /* live TEB -> dst */
{
    const u8 *teb = (const u8 *)__readgsqword(0x30);
    for (size_t i = 0, k = 0; i < ARRAY_LEN(k_swap); k += k_swap[i].n, i++)
        memcpy(dst + k, teb + k_swap[i].off, 8u * k_swap[i].n);
}

static void copy_in(const u64 *src) /* src -> live TEB */
{
    u8 *teb = (u8 *)__readgsqword(0x30);
    for (size_t i = 0, k = 0; i < ARRAY_LEN(k_swap); k += k_swap[i].n, i++)
        memcpy(teb + k_swap[i].off, src + k, 8u * k_swap[i].n);
}

void xrl_teb_init(void)
{
    size_t n = 0;
    for (size_t i = 0; i < ARRAY_LEN(k_swap); i++) n += k_swap[i].n;
    if (n != XRL_TEB_SWAP_QWORDS) { LOGE("teb: swap table has %zu qwords, expected %d", n, XRL_TEB_SWAP_QWORDS); g_swap = 0; }
    char v[4];
    if (GetEnvironmentVariableA("XRL_NO_TEB_SWAP", v, sizeof v) && v[0] == '1') g_swap = 0;

    /* Check the offsets we rely on against this OS before trusting them. */
    u64 teb = __readgsqword(0x30), rsp = (u64)(uintptr_t)&teb;
    SetLastError(0x5852u);
    int ok = __readgsqword(0x30) == teb
          && (u32)__readgsqword(0x68) == 0x5852u
          && (u32)__readgsqword(0x48) == GetCurrentThreadId()
          && __readgsqword(0x08) > rsp && __readgsqword(0x10) <= rsp;
    SetLastError(0);
    if (!ok) { LOGE("teb: TEB layout check failed on this OS; guest/host TEB swapping disabled"); g_swap = 0; }
    LOGI("teb: guest/host TEB views %s (%d qwords per transition)", g_swap ? "enabled" : "DISABLED", XRL_TEB_SWAP_QWORDS);
}

/* Called on the OS thread itself before any guest code runs on it: the TEB is
 * pristine, so both views start as the host's values. */
/* XRL_PROFILE: what a host<->guest view swap pair costs on this machine (logged once) */
static void swap_bench(xrl_thread *t)
{
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    t->teb_guest_live = 1;
    memcpy(t->teb_guest, t->teb_host, sizeof t->teb_guest);
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < 10000; i++) { xrl_teb_enter_host(t); xrl_teb_enter_guest(t); }
    QueryPerformanceCounter(&q1);
    xrl_teb_enter_host(t);
    LOGI("teb: a view swap pair (guest->host->guest, %d qwords each way) takes %.3f us", XRL_TEB_SWAP_QWORDS,
         (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)f.QuadPart / 10000.0);
}

void xrl_teb_thread_init(xrl_thread *t)
{
    copy_out(t->teb_host);
    memcpy(t->teb_guest, t->teb_host, sizeof t->teb_guest);
    t->teb_guest_live = 0;
    static volatile LONG once;
    char v[4];
    if (g_swap && GetEnvironmentVariableA("XRL_PROFILE", v, sizeof v) && v[0] == '1' && !InterlockedExchange(&once, 1)) {
        swap_bench(t);
        memcpy(t->teb_guest, t->teb_host, sizeof t->teb_guest);
        t->teb_guest_live = 0;
    }
}

int xrl_teb_enter_host(xrl_thread *t)
{
    if (t) t->host_t0 = __rdtsc();
    if (!g_swap || !t || !t->teb_guest_live) return 0;
    copy_out(t->teb_guest);
    copy_in(t->teb_host);
    t->teb_guest_live = 0;
    return 1;
}

void xrl_teb_enter_guest(xrl_thread *t)
{
    if (!g_swap || !t || t->teb_guest_live) return;
    copy_out(t->teb_host);
    copy_in(t->teb_guest);
    t->teb_guest_live = 1;
}

void xrl_teb_restore(xrl_thread *t, int was_guest)
{
    if (was_guest) xrl_teb_enter_guest(t);
}

/* The VEH runs on the faulting thread's own (guest) stack, but the host view's
 * StackBase/StackLimit describe the thread's host stack. Host code checks rsp
 * against them (__chkstk walks pages from StackLimit down to rsp), which ran a
 * guest-stack VEH into STATUS_STACK_OVERFLOW. While host code runs on the
 * current stack, describe a window around rsp instead; undo before any view
 * swap so the host view keeps its real values. */
void xrl_teb_stack_here(u64 saved[2])
{
    u64 here = (u64)(uintptr_t)&saved;
    saved[0] = __readgsqword(0x08);
    saved[1] = __readgsqword(0x10);
    if (here < saved[1] || here >= saved[0]) {
        __writegsqword(0x08, ALIGN_UP(here, 0x1000) + 0x10000);
        __writegsqword(0x10, ALIGN_DOWN(here, 0x1000) - 0x40000);
    }
}

void xrl_teb_stack_back(const u64 saved[2])
{
    __writegsqword(0x08, saved[0]);
    __writegsqword(0x10, saved[1]);
}
