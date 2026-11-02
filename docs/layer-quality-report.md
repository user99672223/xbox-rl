# Layer quality and speed report (branch `claude/layer-quality`)

Rocket League on the layer, laptop (not the Xbox), offline menu:

    XRL_LOG_TIME=1 XRL_RL_REUSE_ARGS=1 XRL_TEST_BUDGET_MB=3584 XRL_REAL_GFX=1 XRL_NO_FSGSBASE=1 XRL_PERF=10 bash tools/run-rl.sh

`XRL_LOG_TIME=1` (test host) stamps each log line with seconds since start. Metrics:
- **swapchain**: first `d3d11_create_swapchain`.
- **menu reached**: the first present after the long loading freeze (the first `hitch:` over 5 s).
- **handler**: the sum over the run of "ms/s in the handler" × 10 s. Threads add up, so it can exceed wall time.
- **swap split**: the timers on the `perf: swap` line (added in `ad0ce5c`).

Results vary a lot from run to run (the swapchain appeared anywhere from 63 to 110 s in identical runs), so only
large effects or direct timer measurements are claimed as proven.

## Baseline (`claude/rl-instruments` @ 963ba7b + C4013 fix)

| run | swapchain | menu reached | handler total | max ms/s | swap stored |
|---|---|---|---|---|---|
| base1 (cold, no timestamps) | ~110 s | — | — | 926 | 4485 MiB @150 s |
| base2 | 74.5 s | 171.6 s | 52.9 s | 1045 | 4979 MiB |
| base3 | 62.9 s | 158.1 s | 41.6 s | 937 | 4708 MiB |
| inst1 (+timers) | 75.2 s | 163.5 s | 53.9 s | 949 | 4741 MiB |

The 3584 MiB budget does trigger swapping: Windows' commit runs short too ("swapping toward ~2.5 GiB").

Swap split, first instrumented boot (180 s): eviction total 44.4 s = gather 15.1 s + XPRESS compression
12.8 s + write 14.0 s + probe 1.6 s + decommit 0.6 s; read-back: read 2.7 s, decompression 1.4 s;
g_lock: 7525 contended acquires, 8.6 s waiting.

Menu frame split (steady menu, about 54 fps): about 19 ms per frame, of which about 15.6 ms is syscalls (waiting), with
1.8 ms in graphics calls and 1.5 ms of game code. The menu is not CPU-bound on the laptop.

## Changes

| commit | change | before → after | status |
|---|---|---|---|
| 73c3e60 | C4013 is an error; 8 UWP-hidden APIs declared (CreateFileA and CreateJobObjectW truncated their HANDLE in the UWP build) | — | correctness |
| 8e392f4 | test host `XRL_LOG_TIME=1` | — | tool |
| ad0ce5c | swap cost split on `perf: swap` | — | tool |
| 8fab245 | batched swap writes (a run's consecutive records in one WriteFile) | write calls 76k → 20k; write time 7.4–14.0 s (median 9.5, 5 runs) → 6.4 / 6.8 s | **proven** (timer) |
| 002428d | PrefetchVirtualMemory per eviction run | handler 39.6 / 42.0 → 27.9 / 28.7 s; swapchain 65.6 / 67.4 → 57.6 / 55.3 s; menu 160.4 / 162.6 → 131.2 / 129.2 s; gather 8.2 → 2.0 s (+2 s prefetch) | **proven** (2+2 runs, both consistent) |
| b6350a5 | LZ4 instead of XPRESS (vendored v1.10.0) | compress 10.6 / 12.0 → 3.7–6.2 s; handler 27.9 / 28.7 → 13.4–22.3 s (mean 18.1); swapchain 57.6 / 55.3 → 31–48 s (mean 41.6); menu 131.2 / 129.2 → 98–129 s (mean 118); swap file ~1200 → ~1110 MiB | **proven** (2 vs 4 runs) |

**Overall, laptop:** menu reached 158–172 s (baseline) → 98–129 s (mean 118); first swapchain 63–75 s → 31–48 s;
fault-handler time 42–55 s → 13–22 s. Steady menu fps unchanged at 54–55 (see below for why).

Off switches (no rebuild): `XRL_SWAP_NOSTAGE=1` (one write per record), `XRL_SWAP_NOPREFETCH=1`,
`XRL_SWAP_XPRESS=1` (the old codec). `XRL_SWAP_VERIFY=1` decodes every new record back and compares it
(a full RL boot with it on: 0 mismatches). The swap file's log line says what is on. `run.sh` passes with
all of them on and all off.

LZ4 caveat: one LZ4 run out of 9 stopped logging at 10 s, just as swapping began, with no crash record.
It did not happen again in 8 later runs (3 with `XRL_HANG_DUMP=20`). The one plausible deadlock (a loader
call in `swap_open` under g_lock) was removed. Watch for it on the Xbox.

### Why the prefetch works

`QueryWorkingSetEx` right after the clock probes a span (`PAGE_NOACCESS`): 0 of 53 277 spans were still in
the working set. Setting NOACCESS takes the pages out of the working set, so the eviction's memcpy took a
soft fault on every page (~7 µs each, 16 per span). This is NT behaviour, not memory pressure on the laptop.

## Tried, not kept

| attempt | result |
|---|---|
| `/Ob2 /GL /LTCG` | swapchain 73.2 / 79.1 s, menu 169.5 / 163.6 s vs 74.5 / 62.9 and 171.6 / 158.1: no gain; reverted |
| ntdll `RtlCompressBuffer` XPRESS instead of the Compression API | compress 13.3 s vs 12.6 s, same ratio: no gain |
| skip all-zero pages | only 2.2 % of evicted pages are zero (736 all-zero spans): not worth it |
| env reads on hot paths | all cached after the first call; GET_VCLOCK reads it once per process: nothing to fix |

## Suspected / open

- **Loading freeze (~20 s before the menu):** the main thread spins at 97 % CPU (84 % kernel), 66k syscalls/s,
  no faults and no swap. This is the known "GetOverlappedResult on an async read that never completes" loop
  (c52a4a5). It's a correctness problem in async I/O completion and is being worked on elsewhere; not touched here
  (wake-ups and sockets are off limits).
- **Menu fps (54–55, not 60):** measured with XRL_PROFILE and XRL_SAMPLE in the menu. The game uses
  0.32 of a core in total. The render thread presents with SyncInterval 0 (no vsync, no pacer). The main thread
  waits for it through ntsync (`ioctl`, ~60 per frame) and spins between waits (`getrusage` 88k/s, i.e.
  GetThreadTimes, plus `sched_yield` 44k/s): RL's frame limiter. Suspected: our ntsync timed wait
  (ntsync.c, `WaitForSingleObject` with the timeout rounded up to whole ms, no high-resolution timer) wakes late,
  so a 60 fps limiter lands at ~55. To be measured (overshoot counter) before any change.
- **File lookups:** over a boot, `stat` 484k calls / 7.9 s, `lstat` 213k / 7.2 s, `openat` 213k / 5.6 s,
  `read` 1.04M / 11.8 s (summed over threads). Path translation and case folding in vfs.c are a cache
  candidate (with invalidation on create/rename/delete).
- **Syscall crossing** is cheap: a TEB view swap pair costs 0.036 µs. Not worth chasing.
- **g_lock waits** (2–2.4 s after the prefetch): faster evictions shrink them. Running compression outside the lock is
  possible but risky; only worth trying if measurements still show it.
- **COM scan:** ~1600 VirtualQuery/s in the menu for pointer checks: to look at.

## Needs an Xbox check

- Batched writes: how much they save depends on the console disk's latency.
- PrefetchVirtualMemory: same kernel behaviour expected; its presence in the app container and its cost there
  are unverified. Without it the swap behaves as before.
- Any compressor change: the swap file's size against the console's TEMP quota (`XRL_SWAP_RESERVE_MB` still applies).
