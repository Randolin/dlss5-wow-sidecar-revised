// Composes the model's answer with the original frame: the model's light, the
// original's colour and detail, and the highlights protected.
//
// The model's edit is a per-channel gain -- its result over what it was shown,
// both at the model's own resolution -- and that gain is applied to the
// full-resolution original. Taking the ratio at the model's size is what keeps
// every pixel of the original's detail: dividing a soft image by a sharp one
// would bake the inverse of the fine detail into the gain and cancel it on the
// way back. The model contributes lighting and structure at its scale, on top.
//
// `colourPreserve` interpolates the gain between per-channel (the model's own
// colour grade comes through) and luminance-only (the original's hue and
// saturation are kept exactly). Highlight protection fades the gain to 1 as
// the original nears white and rolls off the result before it clips.

Texture2D<float4>   g_frame  : register(t0);   // original, full size, sRGB
Texture2D<float4>   g_model  : register(t1);   // the model's output, model size, sRGB
Texture2D<float4>   g_shown  : register(t2);   // the model's input, model size, sRGB
RWTexture2D<float4> g_dst    : register(u0);
SamplerState        g_linear : register(s0);

cbuffer Params : register(b0) {
  uint2 g_size;              // destination size
  float g_paperWhite;        // unused on this path
  float g_headroom;          // unused on this path
  float g_strength;
  float g_colourPreserve;
  float g_highlightProtect;
  float g_split;             // 0 off; else the fraction of width left untouched
}

float3 SrgbToLinear(float3 c) {
  const float3 lo = c / 12.92;
  const float3 hi = pow((c + 0.055) / 1.055, 2.4);
  return lerp(hi, lo, step(c, 0.04045));
}

float3 LinearToSrgb(float3 c) {
  const float3 lo = c * 12.92;
  const float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
  return lerp(hi, lo, step(c, 0.0031308));
}

float Luma(float3 lin) { return dot(lin, float3(0.2126, 0.7152, 0.0722)); }

float Knee(float y, float k) {
  return y <= k ? y : k + (1.0 - k) * (1.0 - exp(-(y - k) / (1.0 - k)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_size.x || id.y >= g_size.y) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(g_size);
  const float4 frame = g_frame.SampleLevel(g_linear, uv, 0);
  const float3 model = SrgbToLinear(saturate(g_model.SampleLevel(g_linear, uv, 0).rgb));
  const float3 shown = SrgbToLinear(saturate(g_shown.SampleLevel(g_linear, uv, 0).rgb));
  const float3 origLin = SrgbToLinear(saturate(frame.rgb));

  // The model's edit, at its own resolution, as a gain. Per channel for the
  // model's colour, luminance-only for the original's.
  const float3 perChannel = clamp(model / max(shown, 1e-4), 0.0, 8.0);
  const float yShown = Luma(shown);
  const float lumaOnly = clamp(Luma(model) / max(yShown, 1e-4), 0.0, 8.0);
  float3 gain = lerp(perChannel, lumaOnly.xxx, saturate(g_colourPreserve));
  // Where the model was shown black there is no ratio; leave the pixel alone.
  gain = lerp(gain, 1.0, step(shown, 1e-4));

  // Brightening fades out as the original nears white. Darkening is kept.
  const float yOrig = Luma(origLin);
  const float protect = saturate(g_highlightProtect) * smoothstep(0.55, 0.95, yOrig);
  const float yGain = Luma(gain);
  gain = yGain > 1.0 ? lerp(gain, gain / yGain, protect) : gain;

  float3 composed = origLin * gain;

  // Soft knee on the result, weighted by protection.
  const float yComposed = Luma(composed);
  if (yComposed > 1e-4) {
    const float kneed = lerp(yComposed, Knee(yComposed, 0.85), saturate(g_highlightProtect));
    composed *= kneed / yComposed;
  }

  const float3 lin = lerp(origLin, composed, saturate(g_strength));

  // A/B split: the left fraction of the frame is the original, with a thin
  // line marking the seam. Off when the fraction is zero, and it costs one
  // comparison then.
  if (g_split > 0.0) {
    const float x = (float(id.x) + 0.5) / float(g_size.x);
    const float seam = 1.5 / float(g_size.x);
    if (abs(x - g_split) < seam) {
      g_dst[id.xy] = float4(LinearToSrgb(float3(1.0, 0.82, 0.42) * 0.6), frame.a);
      return;
    }
    if (x < g_split) {
      g_dst[id.xy] = frame;
      return;
    }
  }

  g_dst[id.xy] = float4(LinearToSrgb(saturate(lin)), frame.a);
}
