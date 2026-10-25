# wine/ — Workstream B (Proton-GE Wine)

Proton-GE wine vendored at a pinned tag (into vendor/, kept as pin + patches/).
Patches: musl + __syscall -> xrl_syscall; Wine's few raw syscall sites the same;
wineserver-as-a-thread with in-memory channel; single-process CreateProcess =
in-process exec; wineboot/services/explorer stubbed; graphics/audio/input
builtin DLLs forward to native through xrl_*_ops (opaque handles only — NO
Windows/WinRT headers on the Wine side). scripts/build-wine.sh builds the unix
side in WSL with a musl cross toolchain. Prefix pre-baked and shipped.
