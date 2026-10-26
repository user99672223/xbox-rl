# gfx/ — Workstream D (graphics)

Native graphics backend for `xrl_gfx_ops`: D3D12 device + flip-model
`CreateSwapChainForCoreWindow` swapchain + D3D11On12, all behind opaque
`xrl_handle`s, with caps reporting and an on-console probe. DXVK -> Dozen is a
decision point only (not built).

- **`notes-D.md`** — design, seams (how the host passes the CoreWindow and
  queue), what is verified vs needs the console, the fallback write-up. Start here.
- **`ABI-CHANGE-REQUESTS.md`** — what the frozen ABI v1 cannot express.
- `include/xrl_gfx.h` — the host-facing entry API (A links this).
- `src/` — implementation. `harness/` — desktop test. `build.ps1` — build + test.
