# Workstream B notes - Proton-GE Wine on the xbox-rl layer

Owner: B (`wine/`, `patches/`, `vendor/`). The integrator merges decisions into `docs/decisions.md`.
Read `ABI-CHANGE-REQUESTS.md` next to this file: the list of things B needs from A/C/D/integrator.

Legend: **TESTED** = exercised by a test in this repo and passing. **BUILT** = compiles (full build),
never run on the target. **WRITTEN** = source exists, not verified. **NOT DONE**.

---------------------------------------------------------------------------------------------

## 0. Status in one table

| deliverable | state |
|---|---|
| Pin Proton-GE's Wine + patch-apply script, vendor/ never edited | **TESTED** (`fetch-vendor.sh`, `apply-patches.sh`; 1 musl + 9 wine patches apply cleanly to the pin) |
| Wine unix side against musl, **no `syscall` instruction in the output** | **TESTED**: prod install = 802 binaries (ELF unix libs + loader + libc + 726 PE DLLs/EXEs): **0** `syscall`/`sysenter`/`int $0x80` (`check-no-syscall.sh`; negative control: glibc fails it) |
| musl `__syscall*` -> `xrl_syscall` | **TESTED** (static smoke test with a pass-through kernel; dynamic `libc.so` as interpreter incl. `dlopen`; Wine itself runs on it) |
| Wine's raw syscall sites patched | **TESTED** (map in section 4) |
| `scripts/build-wine.sh` (WSL, musl) | **TESTED**: full pipeline `vendor musl patch tools configure build install check` from scratch; whole Wine (611 DLLs, 800+ binaries) compiles on musl |
| wineserver as a thread | **TESTED** on a PC (wineboot, services stub, cmd, reg, notepad run on it). **In-memory channel: NOT DONE** (pipes/socketpair kept) |
| CreateProcess in-process, wineboot/services/explorer stubbed | **TESTED** (stubs complete with exit 0; any other image fails cleanly). A real second PE image: not possible, see section 7 |
| Prefix recipe | **TESTED**: `bake-prefix.sh` -> 892 files / 280 MB; the baked prefix runs cmd/reg/notepad and the PE test |
| xinput forwarder | **TESTED** end to end against a fake host (PE -> xinput1_4.dll -> xrlhost -> op-table) |
| d3d11 / d3d12 device creation | **TESTED** against the fake host (returns the host's object; makes the queue with a direct MS-ABI COM call) |
| dxgi swapchain redirect | **BUILT**, not run. Swapchain *wrapper for D3D11* (`GetBuffer`->wrapped resource): **NOT DONE** |
| audio (winexrl.drv) | **NOT DONE** (blocked on an ABI addition, section 8) |
| Anything on the actual console | **NOT DONE** - nothing here has run under the real layer |

---------------------------------------------------------------------------------------------

## 1. Pins

| what | pin |
|---|---|
| Proton-GE tag | **GE-Proton11-11** (published 2026-10-19), tag object `5de4b52`, commit `c0c2ee35e6d4d3bff4d39c6246ac46636434feb8` |
| Wine tree | `ValveSoftware/wine` @ **`c53678ab22a85f0fad3588dd34f0452a463bd015`** (the `wine` submodule of that GE tag; "Wine version 11.0") |
| musl | `v1.2.6` = `9fa28ece75d8a2191de7c5bb53bed224c5947417` |

Single source of truth: `wine/pins.env`. `vendor/wine.git` and `vendor/musl.git` are *bare, depth-1* repos
holding exactly those commits (`wine/scripts/fetch-vendor.sh`; ~60 MB, `vendor/*/` is gitignored). Never edited.

**What "Proton-GE's Wine" means here.** GE does not ship a Wine tree. It builds Valve's wine submodule and, at
Proton build time, runs `protonprep-valve-staging.sh` (wine-staging + GE hotfixes). We pin the Valve commit and do
**not** apply GE's staging layer: ~800 patches touching the same files (server, ntdll, win32u) as ours, nothing RL
needs. (Open question Q6.)

## 2. How to build and test (all in WSL)

```
wine/scripts/build-wine.sh                         # vendor musl patch tools configure build install check
wine/scripts/build-wine.sh musl                    # just the patched libc (20 s)
wine/tests/musl-smoke/run.sh                       # libc gateway test (static, pass-through kernel)
XRL_HOSTTEST=1 wine/scripts/build-wine.sh musl tools configure build   # laptop-runnable variant
wine/scripts/bake-prefix.sh                        # shipped prefix  (-> $XRL_WORK/out/prefix)
wine/tests/run-pe-test.sh                          # PE program vs xinput/d3d11/d3d12 forwarders + fake host
wine/tests/run-wine-hosttest.sh 'C:\windows\system32\cmd.exe' /c ver
```
Output of the real build: `$XRL_WORK/out/rootfs` (default `~/xrl-build/out/rootfs`, 273 MB, ~900 files):
`lib/ld-musl-x86_64.so.1` (= patched libc.so), `usr/lib/wine/x86_64-unix/{wine64,ntdll.so,win32u.so,
wineserver.so,xrlhost.so,...}` (RUNPATH `$ORIGIN`), `usr/lib/wine/x86_64-windows/*.dll`, `usr/share/wine/{nls,wine.inf}`.

Timings on this laptop (14 cores, 3.6 GB RAM VM, `-j6`, `-O1 -g0`): musl 20 s; host tools 10 s; full Wine ~40 min cold;
build tree 650 MB. WSL packages installed with root apt (no sudo password for the user): `build-essential make flex
bison git patch xz-utils curl pkg-config gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 linux-libc-dev rsync file python3
autoconf perl libfreetype-dev`. No prebuilt musl cross toolchain: we build **our own patched musl** and drive the distro gcc
with a generated `musl-gcc.specs` (`$SYSROOT/bin/xrl-musl-gcc`, adds `-Wl,-rpath,'$ORIGIN'`). Wine's host tools are built
with plain gcc in a separate tools build (a musl-linked tool would need the gateway to run).

Disk: the VHDX lives on C:. `build-wine.sh build` refuses to start under `XRL_MIN_FREE_GB` (default 4) of **C:** free;
`XRL_KEEP_OBJS=0` deletes objects after install.

## 3. The patch set

`patches/musl/` (1) and `patches/wine/` (9), applied in order by `wine/scripts/apply-patches.sh` onto a fresh
`git archive` export of the pin; `./autogen.sh` then regenerates `configure` and the server protocol headers
(this Valve tree does not commit them in sync). `scripts/split-patches.py <tree> <base> <out>` re-cuts a working tree
(`$XRL_WORK/src/wine`, base tag `xrl-base`) into this series.

### musl

| patch | what |
|---|---|
| `musl/0001-xrl-syscall-gateway` | Every `syscall` in musl x86_64 -> call into `xrl_syscall`: the 7 inline `__syscall0..6`, `__syscall_cp_asm`, `__clone`, `__unmapself`, `__restore_rt`, `vfork`, `__set_thread_area`. New: `xrl_syscall`/`__xrl_syscall` trampoline (`jmp *__xrl_gateway`), the `__xrl_gateway` slot, vDSO off, `_dl_find_object` stub so the distro's `libgcc_eh` links. |

### Wine

| patch | what |
|---|---|
| `0001-pe-syscall-thunk` | The x86_64 PE stub macro (`include/wine/asm.h`) - body of **every `Nt*`** in ntdll.dll (522) and win32u.dll (the NtUser/NtGdi set) - held `syscall` and `call *0x7ffe1000`. Now `jmp; ...; call *__wine_syscall_dispatcher(%rip); ret`, same 0x20-byte geometry (the dispatcher's `-0xb` return fix-up still lands on a `ret`). |
| `0002-unix-syscall-sites` | The two `mov $158,%eax; syscall` (arch_prctl SET_FS slow path) in `__wine_syscall_dispatcher` / `__wine_unix_call_dispatcher` -> register-preserving `call xrl_syscall`. WoW64's `int $0x80` gates (`alloc_fs_sel`, `modify_ldt`) stubbed (win64-only). Syscall-user-dispatch off; the `0x7ffe1000` page and its `mmap` removed; instrumentation-callback switch patches ntdll's dispatcher variable; `HAVE_WINE_PRELOADER` undefined (no re-exec through the preloader). |
| `0003-musl-compat` | `mallopt(M_PERTURB)` guarded; `<pthread.h>` in `media-converter.h`; `SONAME_LIBEGL` fallback in `win32u/opengl.c` (it does not compile without, even with no GL). |
| `0004-runtime-flags-host-usd` | `dlls/ntdll/unix/xrl.c`: `XRL_HOST_USD`, `XRL_NO_SPAWN`, `XRL_SKIP_WINEBOOT`; forces `WINEFSYNC=0 WINEESYNC=0 PROTON_NO_NTSYNC=1`. Host-owned `KUSER_SHARED_DATA` (no `mmap` over `0x7ffe0000`). `run_wineboot` skipped. |
| `0005-wineserver-thread` | `server/` -> `wineserver.so` (makedep learns "unix-lib-only directory"; every `server/*.c` gets `#pragma makedep unix`). `main()` -> `xrl_wineserver_start(nls_dir)`: socketpair, server thread (2 MiB stack, all signals blocked), init, first client adopted as `accept()` would, blocks until init is done. No master socket / lock / daemonize / `/tmp/.wine-uid`; scratch dir in the prefix. `init_signals` no-op; `close_socket_timeout` ends the thread; registry is flushed `atexit`. All `fchdir` dances replaced by `*at()` or save/restore (cwd is shared with the client). NLS dir comes from the client (the server cannot use `/proc/self/exe` inside the layer). Client `server_connect()` dlopens it. |
| `0006-inproc-process` | `XRL_NO_SPAWN`: `NtCreateUserProcess` never forks; wineboot/services/explorer/winedevice/plugplay/rpcss/svchost/conhost/... are completed in-process by a new server request `xrl_stub_process`; any other image -> `STATUS_NOT_SUPPORTED` + `ERR`. |
| `0007-xrlhost` | New `dlls/xrlhost` (PE + unix lib), the single PE->host seam: `XrlCall(code, params, &xrl_status)` -> `xrl_gfx/audio/input_ops` through opaque handles, `XrlIsNativeObject`, `XrlTrackDevice/XrlLookupDevice`. The only Wine code that includes `xrl_abi.h` (copied into `include/` by the build, listed in `include/Makefile.in`). |
| `0008-xinput-forwarder` | `xinput1_1..1_4` and `xinputuap` (one source): state/vibration/capabilities/keystrokes from `xrl_input_ops`; no HID/setupapi/update thread. |
| `0009-d3d-forwarders` | `d3d11.dll`: `D3D11CreateDevice*`/`D3D11On12CreateDevice` = host D3D12 device + queue (direct COM call) + `d3d11on12_create`; FL capped at 11_0. `d3d12.dll`: `D3D12CreateDevice` -> host, serialisers still vkd3d. `dxgi`: swapchains for native devices -> `create_swapchain_for_corewindow`. |

## 4. Map of raw `syscall` sites and what happened to each

**musl** (musl/0001): `arch/x86_64/syscall_arch.h` (7 inline), `src/thread/x86_64/{syscall_cp,clone,__unmapself,__set_thread_area}.s`,
`src/signal/x86_64/restore.s`, `src/process/x86_64/vfork.s`. Nothing else in musl x86_64 executes `syscall`.

**Wine** (the pinned tree, grep for the instruction, `0f 05`, `int $0x80`, `__NR_`/`syscall(`):

| site | what | action |
|---|---|---|
| `include/wine/asm.h` x86_64 `__ASM_SYSCALL_FUNC` | PE stub of every Nt* in ntdll.dll + win32u.dll | **patched** (0001) |
| `signal_x86_64.c` 2x `mov $158,%eax; syscall` | arch_prctl(SET_FS) slow path | **patched** (0002) |
| `signal_x86_64.c` `alloc_fs_sel`, `modify_ldt` | 32-bit `int $0x80` for WoW64 LDT | **stubbed** (0002) |
| `prctl(PR_SET_SYSCALL_USER_DISPATCH)` | trapped the PE `syscall` | **disabled** (0002) |
| `signal_x86_64.c` Apple `_thread_set_tsd_base` x3; `asm.h` arm64ec_x64 variant; `signal_arm64ec.c:1997` | other platforms | not compiled for Linux x86_64 |
| `loader/preloader.c` x3 | `wine-preloader`, freestanding, own syscalls | built by `make`, **removed at install**; never used (0002 undefines `HAVE_WINE_PRELOADER`) |
| libc `syscall(__NR_x)`: `arch_prctl`, `futex`, `gettid`, `membarrier`, `userfaultfd`, `utimensat`, `tgkill/tkill`, `futex_waitv` | go through musl `syscall()` -> `xrl_syscall` | automatic. `userfaultfd`, `membarrier`, `futex_waitv` may return `-ENOSYS`; Wine falls back |

## 5. Two ways to run it

* **Console:** `lib/ld-musl-x86_64.so.1` with the real layer behind `xrl_syscall`. Not testable here.
* **Laptop, `XRL_HOSTTEST=1`:** a second libc whose gateway slot is statically initialised to the pass-through shim
  (`tests/musl-smoke/shim.S`) plus a fake host (`host_ops.c`: xinput pad, fake native D3D COM objects with MS-ABI methods).
  `tests/run-wine-hosttest.sh` runs the musl Wine as `libc.so wine ...` (musl's explicit-interpreter form; nothing in `/lib`).
  This is how the prefix is baked and how the forwarders were tested. The shim is the only `syscall` in that build; it is never shipped.

What was actually run under it (all with `XRL_NO_SPAWN=1`, wineserver thread): `wineboot --init` and wine.inf install
(bake); `cmd /c ver` -> "Microsoft Windows 10.0.19045"; `reg query`; `notepad` creates its window and sits in its message
loop; `xrl_pe_test.exe` (xinput state/vibration/caps from the fake pad; `D3D11CreateDevice` -> fake 11on12 object, FL 11_0,
the guest-made queue; `D3D12CreateDevice`, FL gate). Known noise in those runs: `bcrypt: no encryption support` (built
`--without-gnutls`), `rundll32.exe`/`iexplore.exe` refused by the in-process policy, `wineusb.inf` copy error,
`vulkan: Wine was built without Vulkan support`. `cmd /c dir C:\` printed nothing (not investigated).

## 6. Seam contract with C (layer)

Full text in `ABI-CHANGE-REQUESTS.md` #5/#6. In short: `__xrl_gateway` bootstrap before any guest instruction;
`clone` with `resume_pc`; `exit` with region; `rt_sigreturn` with `&ucontext`; real `arch_prctl` FS/GS; no vDSO;
`socketpair`+`SCM_RIGHTS`, `pipe2`, `poll` (epoll compiled out), `*at()` file calls, a file `MAP_SHARED` mapped twice
coherently in one process, `/proc/{self/exe,cpuinfo,meminfo,uptime,stat}`; `getuid()` consistent with `stat`.
Env the layer passes at boot (boot ctx `envp`): `WINEPREFIX=/prefix WINEARCH=win64 USER=steamuser HOME=/home/steamuser
XRL_HOST_USD=1 XRL_NO_SPAWN=1 XRL_SKIP_WINEBOOT=1 WINEDEBUG=err+all WINEDLLOVERRIDES=winemenubuilder.exe=d`; argv =
`/usr/lib/wine/x86_64-unix/wine64 C:\path\to\RocketLeague.exe ...` (an explicit *path* matters: a bare name makes Wine
launch `start.exe /exec`, which tries to spawn a child).

## 7. wineserver as a thread: design and honest limits

Implemented: a real thread, the real wire protocol, no socket on disk, no fork/exec. Kept: request/reply/wait travel over
`pipe2`/`socketpair` fds because every blocking wait in Wine is "poll an fd" and the server loop is `poll()`. The brief's
"in-memory request/reply channel" is **not** done: it means replacing `send_request/wait_reply` (client) and
`read_request/send_reply` (server) with a ring buffer + futex/eventfd and teaching `main_loop` to poll the ring's eventfd;
a performance step, so it comes after Wine boots under the layer. Hazards: shared cwd (server `fchdir`s removed except
registry load at startup, which the start handshake serialises, and `flush_registry`, which saves/restores); server
`exit()` on fatal errors takes the process down; no fsync/ntsync, so `NtWaitForMultipleObjects` costs a server round trip
(Q5).

CreateProcess: a *second PE image* in one OS process needs its own PEB/ntdll state, so the policy is "stub the helpers,
refuse the rest". If RL needs a real child (launcher, crash reporter) the options are to stub it by name or to run it as
a thread of a second Wine instance - a different project.

## 8. Forwarding builtins

| DLL | state | note |
|---|---|---|
| `xrlhost.dll/.so` | TESTED | needs ABI request #1 to find the ops |
| `xinput1_1..1_4`, `xinputuap`, `xinput9_1_0` | TESTED (fake host) | no battery/headset info (ops lack them) |
| `d3d12.dll` | TESTED (fake host) | `D3D12CreateDevice` only; FL gate 12_0 until `get_adapter_info` |
| `d3d11.dll` | TESTED (fake host) | device creation; `D3D11On12CreateDevice`; swapchain-with-device helper BUILT |
| `dxgi.dll` | BUILT | Wine's own factory/adapter/output enumeration (wined3d "no3d" + `renderer=no3d` baked in the prefix); swapchain redirect only. **D3D11 swapchain wrapper NOT DONE** |
| `xaudio2_*` / audio | NOT DONE | see below |

Audio: not an XAudio2 forwarder. Wine's `xaudio2_7/8/9` are FAudio-over-WASAPI; one mmdevapi driver (`winexrl.drv`,
~600 lines modelled on `winealsa.drv`'s `unix_funcs`) feeding `create_source_voice` + `voice_submit_buffer` covers WASAPI,
DirectSound, winmm and XAudio2 with no callback thunking. Blocked on pacing info (ABI request #4); `xrlhost` already exposes
the 7 audio ops. Also needed: add `xrl` to mmdevapi's default driver list.

Window system: win32u's lazy driver load fails the **first top-level `CreateWindow`** ("no driver could be loaded") unless a
driver is selected. The baked prefix sets `HKLM\System\CurrentControlSet\Control\Video\{0}\0000 GraphicsDriver=null`, which
selects win32u's built-in null user driver: windows are server-side objects only (notepad opens its window under test).
Keyboard/mouse/clipboard/display modes have no driver behind them (Q8).

## 9. The prefix

`bake-prefix.sh` (recipe and contents in its header) runs the hosttest Wine in the same in-process model, as *main*
processes in `WINEBOOTSTRAPMODE=1` (builtins loaded from the build tree): `wineboot --init`, `rundll32 setupapi,
InstallHinfSection PreInstall/DefaultInstall 128 wine.inf` (fake-DLL stubs, fonts, inf), `reg add` for `GraphicsDriver=null`
and `Direct3D\renderer=no3d`; then trims host symlinks and writes a MANIFEST. Result: 892 files, 280 MB, user `steamuser`
(Valve Wine hard-codes it). Ship as `<LocalState>\rootfs\prefix`. The server's periodic/at-exit registry flush writes
`*.reg` there (LocalState is writable; real registry writes stay virtualised). Trimming is minimal so far (no DLL pruning).

## 10. Open questions

* **Q1 (C+B, big): GS and stacks.** Wine PE code needs GS = Wine TEB, host code needs GS = real TEB; every transition into
  host code (gateway, op thunks, native COM objects of request #2) must swap and restore it, and the host's native code must
  survive running on a guest-allocated stack (stack-limit checks). Wine's `wrfsbase`/`PF_RDWRFSGSBASE_AVAILABLE` fast path
  reads the real USD flag.
* **Q2 (integrator):** request #2 (handles directly callable) - decides whether the D3D forwarders are ~300 lines or ~10 000.
* **Q3:** fonts - host tools build `.fon` files (FreeType there), the target has no FreeType: TrueType text via GDI is
  unavailable. RL draws its own UI; confirm or ship a musl FreeType.
* **Q4:** audio driver (request #4).
* **Q5:** wait performance without fsync/ntsync; measure under the layer before building the in-memory channel. A
  `WaitOnAddress`-based fsync in the layer is the likely fix.
* **Q6:** any GE staging/hotfix patches on top of the Valve commit?
* **Q7:** `userfaultfd` unavailable -> Wine uses `mprotect` write-watch (check RL's use of `GetWriteWatch`).
* **Q8:** mouse/keyboard (later): an `xrl` user driver (win32u `user_driver_funcs`) or an `xrl_input_ops` extension.
* **Q9:** crypto: `--without-gnutls` means bcrypt asymmetric keys / schannel are absent (`err:bcrypt` during bake). RL offline
  should not need them; verify.
