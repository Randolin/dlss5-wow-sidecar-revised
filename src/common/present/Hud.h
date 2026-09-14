#pragma once
#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace sidecar {

// Everything the HUD shows. Filled by the render thread a few times a second.
struct HudModel {
  // The number that matches what the eye sees: new frames reaching the screen
  // per second, measured on the wall clock between presents.
  double fps = 0.0;
  double captureFps = 0.0;
  double p50Ms = 0.0;
  double p99Ms = 0.0;
  double gpuMs = 0.0;      // GPU time per frame (the neural work, mostly)
  double cpuMs = 0.0;      // our recording time per frame
  uint64_t frames = 0;
  uint64_t drops = 0;
  const char* passName = "";
  const char* gpuName = "";
  const char* runtimeVariant = "";
  const char* preset = "";
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t modelWidth = 0;    // 0 when no model is running
  uint32_t modelHeight = 0;
  int passCount = 0;
  bool hdr = false;

  // The last presented intervals, milliseconds, oldest first. A graph of these
  // is the difference between "60 fps" and "60 fps with a hitch every second".
  static constexpr int kHistory = 120;
  std::array<float, kHistory> intervalMs{};
  int intervalCount = 0;
};

// Spec M1: above roughly 80 ms capture-to-present, live overlay is not the
// product. Below 40 ms it is comfortable; between the two it is a judgement
// call the operator makes with the game in front of them.
enum class GateVerdict { Playable, Marginal, Failed };
GateVerdict JudgeGate(double p99Ms);

// The one-line summary, for the log and the tests. Pure.
std::string FormatHud(const HudModel& model);

// A layered tool window in the corner of the target. Deliberately separate from
// DCompOverlay: that window hosts a flip swapchain and must stay simple, and
// the HUD has to survive a failure that hides the overlay.
class Hud {
 public:
  static std::unique_ptr<Hud> Create();
  ~Hud();

  void Update(const HudModel& model);
  void Show();
  void Hide() noexcept;
  HWND Hwnd() const { return hwnd_; }

  // For the paint handler.
  const HudModel& Model() const { return model_; }
  const std::string& TextForPaint() const { return text_; }

 private:
  Hud() = default;

  HWND hwnd_ = nullptr;
  HudModel model_;
  std::string text_;
};

}  // namespace sidecar
