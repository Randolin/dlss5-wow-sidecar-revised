// Encodes the captured frame into what an HDR-mode model would expect to see:
// linear scene light. EXPERIMENTAL and off by default -- the runtime is being
// driven in the DLSS family's LDR convention, where the frame as captured is
// the right input; this produced a blown-out picture and is kept only for
// experiment. NrResolve.hlsl is this pass's inverse.
//
// Sampled rather than loaded so the model's input may be produced below the
// frame's resolution.

Texture2D<float4>   g_frame  : register(t0);
Texture2D<float4>   g_unused : register(t1);
RWTexture2D<float4> g_dst    : register(u0);
SamplerState        g_linear : register(s0);

cbuffer Params : register(b0) {
  uint2 g_size;              // destination size
  float g_paperWhite;        // scRGB units: nits / 80
  float g_headroom;          // where display white lands after expansion, >= 1
  float g_strength;          // unused here
  float g_colourPreserve;    // unused here
  float g_highlightProtect;  // unused here
  float g_pad;
}

float3 SrgbToLinear(float3 c) {
  const float3 lo = c / 12.92;
  const float3 hi = pow((c + 0.055) / 1.055, 2.4);
  return lerp(hi, lo, step(c, 0.04045));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_size.x || id.y >= g_size.y) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(g_size);
  const float4 src = g_frame.SampleLevel(g_linear, uv, 0);
  const float3 lin = SrgbToLinear(saturate(src.rgb));
  const float k = 1.0 - 1.0 / max(g_headroom, 1.0);
  const float3 expanded = lin / max(1.0 - lin * k, 1e-4);
  g_dst[id.xy] = float4(expanded * g_paperWhite, 1.0);
}
