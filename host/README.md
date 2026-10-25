# host/ — Workstream A (UWP app)

C++ UWP app on SDL2's WinRT backend. Owns the window, gamepad, keyboard, audio
device, the manifest (caps: codeGeneration, expandedResources), logging to
LocalState/log.txt, and the implementations of `xrl_gfx_ops`/`xrl_audio_ops`/
`xrl_input_ops` (with gfx/ for the D3D12 parts). Calls `xrl_layer_boot()`.

MUST be a real C++/WinRT UWP .vcxproj with an activation class + .winmd, or the
Xbox launcher refuses it (see docs/decisions.md). Device/swapchain creation must
happen OFF the CoreWindow UI thread (UI-thread D3D create hangs on Xbox).

Device Portal scripts in tools/ (deploy/run/kill/logs/push-game) read .env.
