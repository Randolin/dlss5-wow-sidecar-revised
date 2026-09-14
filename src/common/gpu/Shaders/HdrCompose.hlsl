// Puts the model's edit back onto the HDR frame.
//
// The model saw a tone-mapped SDR view of the frame (HdrToSdr.hlsl) and
// returned an SDR result. The change it made is expressed as a per-channel
// ratio between its result and what it was shown, in linear light, and that
// ratio is applied to the original scRGB frame. Below SDR white the model's
// relighting comes through in full; above it the ratio fades to 1, so the
// game's own highlights -- the part the model never saw -- pass untouched. The
// result is scRGB linear, ready for an FP16 swapchain.

Texture2D<float4>   g_hdr    : register(t0);   // original, scRGB linear
Texture2D<float4>   g_sdrIn  : register(t1);   // what the model was shown, sRGB
Texture2D<float4>   g_sdrOut : register(t2);   // what the model returned, sRGB
RWTexture2D<float4> g_dst    : register(u0);

cbuffer Params : register(b0) {
  uint2 g_size;
  float g_paperWhite;   // scRGB units: nits / 80
  float g_maxGain;      // cap on brightening, so a near-black pixel cannot explode
  float g_headroom;     // multiples of paper white the model's view folded in
  float g_split;        // 0 off; else the fraction of width left untouched
}

float3 SrgbToLinear(float3 c) {
  const float3 lo = c / 12.92;
  const float3 hi = pow((c + 0.055) / 1.055, 2.4);
  return lerp(hi, lo, step(c, 0.04045));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_size.x || id.y >= g_size.y) return;
  const int3 at = int3(id.xy, 0);
  const float4 hdr = g_hdr.Load(at);
  const float3 shown = SrgbToLinear(saturate(g_sdrIn.Load(at).rgb));
  const float3 result = SrgbToLinear(saturate(g_sdrOut.Load(at).rgb));

  // Per-channel ratio, guarded where the model was shown black.
  const float3 denom = max(shown, 1e-4);
  float3 gain = clamp(result / denom, 0.0, max(g_maxGain, 1.0));
  const float3 black = step(shown, 1e-4);
  gain = lerp(gain, 1.0, black);

  // The model saw everything up to headroom x paper white, compressed under
  // the tone-map's shoulder, and its edit there is real. Only the top of that
  // range -- where the shoulder flattened the view to nothing -- is faded
  // back to the original. An SDR game lives below paper white and is never
  // touched by this; an HDR game keeps its edit through its midtones and
  // highlights and gives up only the last stop before the model's clip.
  const float y = dot(max(hdr.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
  const float top = max(g_headroom, 1.0) * g_paperWhite;
  const float above = smoothstep(0.7 * top, top, y);
  gain = lerp(gain, 1.0, above);

  // A/B split, as in NrCompose: the left fraction stays original, a thin
  // line marks the seam. In scRGB the line is drawn at paper white so it is
  // visible without being a highlight.
  if (g_split > 0.0) {
    const float x = (float(id.x) + 0.5) / float(g_size.x);
    const float seam = 1.5 / float(g_size.x);
    if (abs(x - g_split) < seam) {
      g_dst[id.xy] = float4(float3(1.0, 0.82, 0.42) * g_paperWhite, hdr.a);
      return;
    }
    if (x < g_split) {
      g_dst[id.xy] = hdr;
      return;
    }
  }

  g_dst[id.xy] = float4(hdr.rgb * gain, hdr.a);
}
