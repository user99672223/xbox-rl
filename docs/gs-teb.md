# GS / TEB design (Wine on top of real Windows NT)

Status: design, 2026-10-26. Owner: integrator. Implements in layer/ (C) + a Wine patch (B).

## The problem

On x86-64 every Windows PE module reads its per-thread block (the TEB) through
`%gs` (`gs:[0x30]` = TEB self, `gs:[0x58]` TLS array, `gs:[0x60]` PEB, ...). On
Linux, Wine allocates its own TEB per thread and points GS at it
(`arch_prctl(ARCH_SET_GS)`). On the Xbox, GS *is* the real Windows TEB of the OS
thread: the kernel reloads it on every context switch, and there is no
user-mode way to rebase it (and no FSGSBASE, see decisions.md). So the guest's
PE code (Wine's ntdll/kernelbase and the game) and the host's native code
(Windows ntdll/kernelbase, the CRT, D3D, XAudio2, WinRT, and the layer itself)
must share **one** TEB per thread.

## Decision: adoption + field swapping

1. **Adoption (Wine patch, B).** Wine does not allocate TEBs. A guest thread's
   TEB is the host TEB of the OS thread the layer runs it on, obtained with
   `arch_prctl(0x5801)` (XRL_GET_HOST_TEB). `arch_prctl(ARCH_SET_GS, teb)` with
   that value is accepted as a no-op. Wine writes its own values into the TEB
   exactly as on Linux.
2. **Field swapping (layer, C).** The TEB fields that both sides use with
   different meanings are kept in two views per thread: the *guest view* (live
   while guest code runs) and the *host view* (captured at thread start, before
   Wine initialises the TEB). Every guest->host transition swaps the host view
   in; the return swaps the guest view back. Transitions the layer already owns:
   - `xrl_syscall` thunk entry/exit (it also switches to the host stack),
   - op-table SysV->Win64 downcall stubs,
   - the vectored exception handler (host code entered by the kernel).
3. **Direct COM calls** from PE code into native objects (the game calling
   `ID3D11DeviceContext::Draw` on a handle) are the remaining transition. They
   are needed only once something renders; `cmd /c ver`-class programs never
   make them. Handled in a second stage (below).

### Swap set (initial; x64 TEB offsets)

| Offset | Field | Why it differs |
|---|---|---|
| 0x008 / 0x010 | NtTib.StackBase / StackLimit | guest stack vs host stack (`__chkstk`, SEH unwind checks) |
| 0x038 | EnvironmentPointer | |
| 0x040 / 0x048 | ClientId (pid/tid) | Wine's server ids vs real ids (critical-section owners, GetCurrentThreadId) |
| 0x058 | ThreadLocalStoragePointer | implicit TLS: game/Wine modules vs host DLLs |
| 0x060 | ProcessEnvironmentBlock | Wine's PEB vs host PEB (process heap, loader, process parameters) |
| 0x068 | LastErrorValue (+CountOfOwnedCriticalSections) | per-side error state |
| 0x108 | CurrentLocale | |
| 0x2c8 | ActivationContextStackPointer | |
| 0x7d8 | RealClientId | |
| 0x800..0x848 | Win32ClientInfo (first 9 qwords) | Wine's `ntuser_thread_info` (the thread's desktop and message-window handles) vs win32k's CLIENTINFO, which real user32/D3D calls on the same OS thread fill in. Unswapped, Rocket League's main window got win32k's value as its desktop handle and creation failed with ERROR_INVALID_WINDOW_HANDLE |
| 0x1250 | LastStatusValue | |
| 0x1478 | DeallocationStack | |
| 0x1480..0x1670 | TlsSlots[0..62] | independent TlsAlloc bitmaps (slot 63 = XRL_TP, never swapped) |
| 0x1758 | ReservedForOle | COM/WinRT apartment state (host RoInitialize vs Wine ole32) |
| 0x1778 | ThreadPoolData | |
| 0x1780 | TlsExpansionSlots | |
| 0x17a0 | NlsCache | |
| 0x17c8 | FlsData | UCRT per-thread data (errno, locale) vs Wine FLS |
| 0x17d0..0x17e8 | Preferred/User/MergedPrefLanguages | |
| 0x17ee | SameTebFlags | loader thread flags |

Cost: ~100 qwords each way, `rep movsq` from a per-thread save area: tens of
nanoseconds per transition, far below the cost of the syscall itself.

Not swapped, by design: `NtTib.Self` (same pointer), `GdiTebBatch`
(0x2f0..0x7d8: Wine keeps `ntdll_thread_data` there; the host stack we use —
D3D12/DXGI on CoreWindow, XAudio2, WinRT — does no GDI batching. If a host API
ever writes it, add it to the set and move Wine's thread data elsewhere), and
fields the kernel or host ntdll maintain asynchronously (instrumentation
callbacks, `TlsLinks`, `ActiveFrame`, `WaitingOnLoaderLock`, ...).

### Wine-side requirements (B patch)
- TEB = `arch_prctl(0x5801)`; no `virtual_alloc_teb` for 64-bit threads; no
  wow64 32-bit TEB (64-bit game only).
- PE `TlsAlloc`: mark index 63 (XRL_TP_TLS_INDEX) used in Wine's TLS bitmap.
- Never assume the TEB sits inside Wine-managed memory (no `teb + 0x2000`
  arithmetic, no freeing it on thread exit).
- The PEB is Wine's own (allocated by Wine), referenced only through the
  swapped field.

### Layer-side requirements (C)
- Capture the host view at `thread_main` before entering guest code.
- Swap in the three transition paths; nesting-safe (a host->guest callback
  inside a host call swaps back to guest and restores on return).
- `ARCH_SET_GS` accepts the host TEB (already done).
- A desktop test: fill the TEB with sentinel "guest" values, make syscalls that
  exercise Win32 (files, threads, heap, errors), verify host APIs work and the
  guest values survive.

## Stage 2: direct COM calls (built: layer/src/com.c)

The game calls methods on native D3D/DXGI/XAudio2 objects from PE code with the
guest view live; those methods need the host view (they allocate from the
process heap via the PEB, use TLS/FLS, set LastError) and a host stack.
Observed: D3D12 `CreateCommandQueue` from Wine's d3d11 returned E_OUTOFMEMORY.

Built: **in-place vtable patching with a conditional trampoline.** Each entry
of a host COM vtable that reaches the guest becomes `mov r11, orig; jmp
xrl_com_tramp`. The tramp looks the thread up (gs:[30h] hash, as the other
thunks) and, only if the guest view is live, switches to the host stack, swaps
the host view in, copies 12 stack arguments, preserves xmm0-3 / the xmm0
result, calls, scans the arguments for new objects, and swaps back. From host
code it is a jump. Out-parameter objects are found by scanning every downcall's
and COM call's arguments after return (pointer -> pointer -> object whose
vtable is outside guest memory and starts with three host-image code
pointers); vtables in host images and on the host heap (D3D11's per-device
context dispatch table) both qualify. Over-hooking only costs a few
instructions, so the heuristics err on the side of hooking.

Rejected: IDL-generated wrapper objects (in-parameters would need unwrapping,
~600 methods of code, wrappers break identity comparisons); forwarding calls
to a host worker thread (microseconds per call).

Known gaps: host code calling back into guest code (would run with the host
view on the host stack; D3D does not); every guest COM call pays the full
100-qword swap twice (a smaller set for COM calls is a possible optimisation).

## Alternative considered: no Wine for the game

The console is Windows. A PE loader in the host could map RocketLeague.exe
directly against the console's own system DLLs, with shims only for what the
AppContainer lacks (HWND windowing -> CoreWindow, file paths -> LocalState,
registry virtualisation, XInput -> Windows.Gaming.Input). That avoids this whole
document: the game would run on the real TEB with native D3D11. It does not use
the Linux layer, which the brief fixes as the architecture and which the
project's longer-term goal (a general Linux-program layer) needs. Recorded so
the trade-off is explicit; not pursued unless the project owner chooses it.
