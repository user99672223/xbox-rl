# Vendored upstream pins

`vendor/<tree>/` directories are git-ignored (see `/.gitignore`); this file and the
constants in the fetch scripts are what is versioned. Upstream trees are never edited
in place - changes are `patches/<tree>-NNNN-*.patch` applied by a script.

## Workstream A (host/)

| Tree | Source | Pin | Fetched by |
|------|--------|-----|-----------|
| `vendor/SDL2` | https://github.com/libsdl-org/SDL | tag `release-2.32.10`, commit `5d249570393f7a37e037abf22cd6012a4cc56a71` | `tools/fetch-deps.ps1` (verifies HEAD == pin) |
| `vendor/packages/Microsoft.Windows.CppWinRT.2.0.250303.1` | nuget.org `Microsoft.Windows.CppWinRT` 2.0.250303.1 | nupkg SHA-256 `955E3051B35DB1C00177ED14AB6B7B995C3B32A53FDB6C184EA53B1DBA66E439` | `tools/fetch-deps.ps1` (verifies hash) |

SDL2 patches: `patches/sdl2-*.patch`, applied by `tools/apply-patches.ps1`. **None at present** -
the pinned release builds and works unmodified on its WinRT backend; the only deviations
(toolset v143, SDK 10.0.26100.0, app-type revision 10.0) are MSBuild command-line properties
set by `tools/build.ps1`, not source edits.

To bump a pin: change the constants at the top of `tools/fetch-deps.ps1` AND this table in
the same commit, run `tools/fetch-deps.ps1 -Force`, rebuild `-Mode Full`.
