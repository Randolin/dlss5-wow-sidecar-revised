// An HDR capture, as the model should see it: SDR, sRGB-encoded.
//
// The frame is scRGB linear (1.0 = 80 nits) as Windows composes an HDR
// desktop. The model runs in the DLSS family's LDR convention, so it is shown
// a tone-mapped view: divide by paper white so SDR-reference white lands at
// 1.0, keep everything below a knee as-is, and fold the range above it -- up to
// `headroom` times paper white -- into the remaining space with a soft
// shoulder. Then sRGB-encode. This is the input the model was trained on; the
// highlights it never sees are put back by HdrCompose.hlsl.

Texture2D<float4>   g_hdr : register(t0);
RWTexture2D<float4> g_sdr : register(u0);

cbuffer Params : register(b0) {
  uint2 g_size;
  float g_paperWhite;   // scRGB units: nits / 80
  float g_headroom;     // multiples of paper white folded under the shoulder
}

float3 LinearToSrgb(float3 c) {
  const float3 lo = c * 12.92;
  const float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
  return lerp(hi, lo, step(c, 0.0031308));
}

// Identity to the knee, then an exponential shoulder that reaches ~1 at the
// headroom and never clips.
float Shoulder(float l, float knee, float headroom) {
  if (l <= knee) return l;
  const float span = max(headroom - knee, 1e-3);
  return knee + (1.0 - knee) * (1.0 - exp(-(l - knee) / span * 3.0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_size.x || id.y >= g_size.y) return;
  const float4 src = g_hdr.Load(int3(id.xy, 0));
  const float3 lin = max(src.rgb / max(g_paperWhite, 1e-4), 0.0);

  // Tone-map on luminance and keep the hue: the same idea the compose uses,
  // so the round trip back to HDR is a clean ratio rather than a re-grade.
  // Identity to paper white: an SDR game on an HDR desktop peaks exactly
  // there and must come through untouched; only real HDR highlights above it
  // are folded under the shoulder.
  const float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
  const float mapped = Shoulder(y, 1.0, max(g_headroom, 1.05));
  const float3 sdr = y > 1e-5 ? lin * (mapped / y) : lin;

  g_sdr[id.xy] = float4(LinearToSrgb(saturate(sdr)), 1.0);
}
