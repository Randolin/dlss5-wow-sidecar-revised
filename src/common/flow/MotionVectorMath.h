#pragma once
#include <cstdint>

namespace sidecar {

struct MotionVec {
  float x = 0.0f;
  float y = 0.0f;
};

// NVOFA emits S10.5 fixed point: 32 raw units per pixel.
inline constexpr float kS10_5Scale = 32.0f;

// Decodes a raw flow vector to pixels in the convention the neural runtime
// expects.
//
// No negation, and that is the finding rather than an oversight. NVOFA answers
// "where did this pixel come from", so its vector points backwards in time --
// which is exactly the DLSS convention: a motion vector added to the current
// position gives the previous one. An earlier version negated here, reasoning
// that NGX wanted "where is it going". That was wrong, and it hid for a long
// time because a doubled reprojection error is normally rejected by the
// runtime's neighbourhood colour clamp -- except on a smooth surface, where
// the misreprojected history lands on near-identical pixels, sails through the
// clamp and accumulates until the runtime re-seeds. The symptom was a slow
// ramp in reflected light and shadow with a sudden drop, on polished floors,
// worst at low turn rates.
//
// FlowToMotionVec.hlsl must match this line for line.
constexpr MotionVec FlowToMotionPixels(int16_t rawX, int16_t rawY) {
  return MotionVec{static_cast<float>(rawX) / kS10_5Scale,
                   static_cast<float>(rawY) / kS10_5Scale};
}

// NGX reads MV_Scale_X/Y to decide whether vectors are pixels or NDC. This
// pipeline sends NDC, so the scale is the reciprocal of the full extent.
constexpr MotionVec MotionPixelsToNdc(MotionVec pixels, uint32_t width, uint32_t height) {
  return MotionVec{pixels.x / static_cast<float>(width),
                   pixels.y / static_cast<float>(height)};
}

}  // namespace sidecar
