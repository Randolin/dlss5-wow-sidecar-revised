// Resolves an HDR-mode model's output back into the display frame. The inverse
// of NrEncode.hlsl, with the same colour preservation and highlight protection
// as NrCompose.hlsl. EXPERIMENTAL, like the encode.

Texture2D<float4>   g_frame  : register(t0);   // original, sRGB-encoded
Texture2D<float4>   g_model  : register(t1);   // the model's output, scaled linear
RWTexture2D<float4> g_dst    : register(u0);
SamplerState        g_linear : register(s0);

cbuffer Params : register(b0) {
  uint2 g_size;
  float g_paperWhite;
  float g_headroom;
  float g_strength;
  float g_colourPreserve;
  float g_highlightProtect;
  float g_pad;
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
  const float3 model = g_model.SampleLevel(g_linear, uv, 0).rgb;

  const float3 frameLin = SrgbToLinear(saturate(frame.rgb));
  const float k = 1.0 - 1.0 / max(g_headroom, 1.0);
  const float3 x = max(model / max(g_paperWhite, 1e-4), 0.0);
  const float3 toned = x / (1.0 + x * k);

  const float yOrig = Luma(frameLin);
  const float yModel = Luma(toned);
  float gain = yOrig > 1e-4 ? min(yModel / yOrig, 8.0) : 0.0;
  const float protect = saturate(g_highlightProtect) * smoothstep(0.55, 0.95, yOrig);
  gain = gain > 1.0 ? lerp(gain, 1.0, protect) : gain;
  const float3 recoloured = yOrig > 1e-4 ? frameLin * gain : toned;
  float3 composed = lerp(toned, recoloured, saturate(g_colourPreserve));
  const float yComposed = Luma(composed);
  if (yComposed > 1e-4) {
    const float kneed = lerp(yComposed, Knee(yComposed, 0.85), saturate(g_highlightProtect));
    composed *= kneed / yComposed;
  }

  const float3 lin = lerp(frameLin, composed, saturate(g_strength));
  g_dst[id.xy] = float4(LinearToSrgb(saturate(lin)), frame.a);
}
