// xbox-rl (desktop/plasma overlay): video on the console. Its D3D12 decodes HEVC, VP9 and H.264 (<= 1080p) but not
// AV1 (the main session's measurements, docs/desktop-notes.md): without AV1, YouTube serves VP9, which the hardware
// can decode and which also costs less on the CPU than AV1.
pref("media.av1.enabled", false);
// VA-API decode through Mesa's d3d12 video driver (/opt/xrl-gpu). Firefox's VA-API path needs a DRM render node
// for its dmabuf surfaces, which the layer does not have yet: until then this stays without effect.
pref("media.ffmpeg.vaapi.enabled", true);
