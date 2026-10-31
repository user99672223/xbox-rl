// fsr1.hlsl - AMD FidelityFX Super Resolution 1 (gfx/third_party/fsr1, MIT) as full-screen pixel shaders for the
// present path (fsr.cpp): EASU upscales the game's frame to the output size, RCAS sharpens the result.
// 32-bit math (FL 11_0, shader model 5). Rebuild the headers with gen-shaders.sh after a change.
//   vs:          full-screen triangle from SV_VertexID
//   ps, EASU=1:  t0 = the game's frame, s0 = linear clamp, b0 = con0..con3 from FsrEasuCon
//   ps, RCAS=1:  t0 = the EASU output, b0 = con0 from FsrRcasCon
#define A_GPU 1
#define A_HLSL 1
#include "../../third_party/fsr1/ffx_a.h"

cbuffer FsrCon : register(b0) { uint4 con0; uint4 con1; uint4 con2; uint4 con3; };
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

#if EASU
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return src.GatherRed(lin, p); }
AF4 FsrEasuGF(AF2 p) { return src.GatherGreen(lin, p); }
AF4 FsrEasuBF(AF2 p) { return src.GatherBlue(lin, p); }
#include "../../third_party/fsr1/ffx_fsr1.h"
float4 ps(float4 pos : SV_Position) : SV_Target {
    AF3 c;
    FsrEasuF(c, AU2(pos.xy), con0, con1, con2, con3);
    return float4(c, 1);
}
#endif

#if RCAS
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return src.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "../../third_party/fsr1/ffx_fsr1.h"
float4 ps(float4 pos : SV_Position) : SV_Target {
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(pos.xy), con0);
    return float4(r, g, b, 1);
}
#endif
