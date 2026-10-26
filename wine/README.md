# wine/ - Workstream B (Proton-GE Wine)

Valve/Proton Wine at a pinned commit (`pins.env`), unix side built against a **patched musl** whose
syscalls are calls into `xrl_syscall` (include/xrl_abi.h). No `syscall` instruction survives in the
shipped output. wineserver runs as a thread, CreateProcess is an in-process stub for Wine's helper
processes, and the xinput/d3d11/d3d12/dxgi builtins forward to the host op-tables through
`dlls/xrlhost`.

Start with **`notes-B.md`** (pins, patch set, what is real vs. scaffold, seam contracts, open questions)
and **`ABI-CHANGE-REQUESTS.md`** (what B needs from the other workstreams).

```
scripts/build-wine.sh       vendor musl patch tools configure build install check   (WSL)
scripts/fetch-vendor.sh     pin -> vendor/{wine,musl}.git (bare, depth 1)
scripts/apply-patches.sh    export pin + patches/<wine|musl>/*.patch  (never edits vendor/)
scripts/build-musl.sh       patched libc + xrl-musl-gcc wrapper (+ no-syscall check)
scripts/check-no-syscall.sh disassembly check over ELF and PE files
scripts/split-patches.py    re-cut a dev checkout into the patch series
scripts/bake-prefix.sh      produce the shipped Wine prefix
tests/musl-smoke/           libc gateway test with a pass-through "kernel"
tests/run-wine-hosttest.sh  run the musl Wine under WSL (XRL_HOSTTEST=1 build)
```
