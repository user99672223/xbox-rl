# gfx/ — Workstream D (graphics)

DXGI + CreateSwapChainForCoreWindow against the host CoreWindow; caps reporting
(the probe already measured FL 11_0, binding tier 3, heap tier 2, D3D11On12
present — see docs/decisions.md); D3D11On12 wiring over the shared d3d12 device/
queue. Implements the D3D12 parts of xrl_gfx_ops with host/. DXVK -> Dozen
branch ONLY if D3D11On12 fails on the console (no system Vulkan — would have to
be shipped). Controller-first; present path must hit native flip-model.
