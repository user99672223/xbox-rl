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

/* {offset, qword count}. TlsSlots[63] (0x1678) is the guest thread pointer slot
 * (XRL_TP_GS_OFFSET) and is deliberately not swapped: it must stay live. */
static const struct { u16 off, n; } k_swap[] = {
    {0x008, 2},   /* NtTib.StackBase, StackLimit                               */
    {0x038, 1},   /* EnvironmentPointer                                        */
    {0x040, 2},   /* ClientId.UniqueProcess, UniqueThread                      */
    {0x058, 2},   /* ThreadLocalStoragePointer, ProcessEnvironmentBlock        */
    {0x068, 1},   /* LastErrorValue, CountOfOwnedCriticalSections              */
    {0x108, 1},   /* CurrentLocale, FpSoftwareStatusRegister                   */
    {0x2c8, 1},   /* ActivationContextStackPointer                             */
    {0x1250, 1},  /* LastStatusValue                                           */
    {0x1478, 64}, /* DeallocationStack, TlsSlots[0..62]                        */
    {0x1758, 1},  /* ReservedForOle (COM/WinRT apartment)                      */
    {0x1778, 2},  /* ThreadPoolData, TlsExpansionSlots                         */
    {0x17a0, 1},  /* NlsCache                                                  */
    {0x17c8, 5},  /* FlsData, Preferred/User/MergedPrefLanguages, SameTebFlags */
};

static int g_swap = 1;

static void copy_out(u64 *dst)      /* live TEB -> dst */
{
    for (size_t i = 0, k = 0; i < ARRAY_LEN(k_swap); i++)
        for (u16 j = 0; j < k_swap[i].n; j++) dst[k++] = __readgsqword(k_swap[i].off + 8u * j);
}

static void copy_in(const u64 *src) /* src -> live TEB */
{
    for (size_t i = 0, k = 0; i < ARRAY_LEN(k_swap); i++)
        for (u16 j = 0; j < k_swap[i].n; j++) __writegsqword(k_swap[i].off + 8u * j, src[k++]);
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
void xrl_teb_thread_init(xrl_thread *t)
{
    copy_out(t->teb_host);
    memcpy(t->teb_guest, t->teb_host, sizeof t->teb_guest);
    t->teb_guest_live = 0;
}

int xrl_teb_enter_host(xrl_thread *t)
{
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
