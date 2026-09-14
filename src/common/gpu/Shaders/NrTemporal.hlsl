// Stabilises the model's edit over time, not the image, on a still scene.
//
// The frame the game drew is already stable; what flickers is the model's
// per-frame decision about lighting -- its output expressed as a gain on what
// it was shown. So the gain is what gets filtered: where a pixel's input did
// not change since last frame, last frame's stabilised gain at that pixel is
// blended in; where it did change, this frame's gain stands. Nothing is ever
// moved, so nothing can ghost: on a camera turn the whole frame changes and
// the filter steps out of the way by itself.
//
// Two refinements:
// - Asymmetry: the shimmer is shadowy -- darkening that comes and goes -- so
//   the gain rises quickly and falls slowly. A shadow has to persist to be
//   believed.
// - A 3x3 guided blur on this frame's gain before blending, weighted by how
//   similar the input pixels are, kills sub-pixel flicker without crossing
//   edges.

Texture2D<float4>   g_shown     : register(t0);   // model input this frame, sRGB
Texture2D<float4>   g_result    : register(t1);   // model output this frame, sRGB
Texture2D<float4>   g_gainPrev  : register(t2);   // last frame's stabilised gain, linear
Texture2D<float4>   g_shownPrev : register(t3);   // last frame's model input, sRGB
RWTexture2D<float4> g_gainOut   : register(u0);   // this frame's stabilised gain
RWTexture2D<float4> g_resultOut : register(u1);   // shown * stabilised gain, sRGB
RWTexture2D<float4> g_shownCopy : register(u2);   // this frame's input, for next frame

cbuffer Params : register(b0) {
  uint2 g_size;
  float g_alphaDark;     // history weight when the gain is falling (darkening)
  float g_alphaBright;   // history weight when the gain is rising
  float g_rejectLo;      // input difference below which history is trusted
  float g_rejectHi;      // ... above which it is discarded
  uint  g_spatial;       // 1: guided 3x3 blur of this frame's gain
  uint  g_reset;         // 1: no valid history; take this frame's gain as-is
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

// This frame's raw gain at a pixel: result / shown in linear light, 1 where
// the model was shown black.
float3 RawGain(int2 p) {
  p = clamp(p, int2(0, 0), int2(g_size) - 1);
  const float3 shown = SrgbToLinear(saturate(g_shown.Load(int3(p, 0)).rgb));
  const float3 result = SrgbToLinear(saturate(g_result.Load(int3(p, 0)).rgb));
  const float3 gain = clamp(result / max(shown, 1e-4), 0.0, 8.0);
  return lerp(gain, 1.0, step(shown, 1e-4));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_size.x || id.y >= g_size.y) return;
  const int2 p = int2(id.xy);
  const float4 shownPx = g_shown.Load(int3(p, 0));
  const float3 shownLin = SrgbToLinear(saturate(shownPx.rgb));
  const float yShown = Luma(shownLin);

  float3 gain = RawGain(p);
  if (g_spatial != 0) {
    float3 sum = 0.0;
    float wsum = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
      [unroll] for (int dx = -1; dx <= 1; ++dx) {
        const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(g_size) - 1);
        const float yq = Luma(SrgbToLinear(saturate(g_shown.Load(int3(q, 0)).rgb)));
        const float w = exp(-abs(yq - yShown) * 24.0);
        sum += RawGain(q) * w;
        wsum += w;
      }
    }
    gain = sum / max(wsum, 1e-4);
  }

  float3 stabilised = gain;
  if (g_reset == 0) {
    const float3 gainPrev = g_gainPrev.Load(int3(p, 0)).rgb;
    const float3 shownPrevLin = SrgbToLinear(saturate(g_shownPrev.Load(int3(p, 0)).rgb));
    // Trust the history only where this pixel's input is what it was.
    const float diff = abs(Luma(shownPrevLin) - yShown);
    const float trust = 1.0 - smoothstep(g_rejectLo, g_rejectHi, diff);
    const float3 falling = step(gain, gainPrev);   // 1 where this frame is darker
    const float3 alpha = lerp(g_alphaBright, g_alphaDark, falling) * trust;
    stabilised = lerp(gain, gainPrev, alpha);
  }

  g_gainOut[p] = float4(stabilised, 1.0);
  g_resultOut[p] = float4(LinearToSrgb(saturate(shownLin * stabilised)), shownPx.a);
  g_shownCopy[p] = shownPx;
}
