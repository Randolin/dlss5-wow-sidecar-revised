#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Log.h"

namespace sidecar {

struct UiRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

// The direct neural-rendering path's tuning: the model's own knobs, as the
// runtime names them. One set applies to every pass unless a pass overrides
// it. Ranges are the model's: preset 0-3 (0 lets the model choose), style 0
// standard / 1 natural / 2 cinematic, strengths 0-2, skin structure -1 for off.
struct NrPassSettings {
  int preset = 0;
  int style = 0;
  float intensity = 1.0f;
  float localStructure = 1.0f;
  float localTone = 1.0f;
  float skinStructure = -1.0f;
  bool autoMask = true;
  bool uiCorrection = true;
};

struct NrSettings {
  NrPassSettings base;
  // Per-pass overrides, in order. Empty means one pass at `base`. Each entry
  // starts as a copy of base and takes only the keys it names, so a file can
  // say "three passes, the last one softer" without restating everything.
  std::vector<NrPassSettings> passes;

  // How the model's answer is composed back into the frame (gpu/NrColorBridge).
  // Whole-frame settings, not per pass.
  //
  // The bridge is the retired SDR-side experiment and is off by default.
  // Headroom is no longer a setting: the tone-map is identity below paper
  // white, so spanning the display's full range costs SDR content nothing and
  // is always right for HDR content. The runtime derives it.
  bool bridge = false;
  // 0 means automatic: the display's own SDR white level, which is what an SDR
  // game on an HDR desktop is composed at. A number overrides it.
  float paperWhiteNits = 0.0f;
  float colourPreserve = 1.0f;     // 0 the model's colour .. 1 the original's
  float highlightProtect = 0.6f;   // 0 pure composition .. 1 no brightening near white
  // The model works at this fraction of the capture resolution. 1.0 is full;
  // 0.75 costs a little over half as much.
  float modelScale = 1.0f;
  // Run the last pass at full resolution while the earlier passes work at
  // modelScale. Lighting from the cheap passes, fine structure from the
  // expensive one; needs two or more passes to mean anything.
  bool finalPassFull = false;
  // With more than one pass, each pass sees the composed result of the one
  // before rather than its raw output.
  bool chainComposed = true;
  // Whether the manager exposes the model's knobs per pass. Off: one set of
  // controls, propagated into every pass. On: each pass has its own. The
  // passes themselves are always written out in full, so the runtime never
  // needs to know which mode produced them.
  bool perPassTuning = false;

  // A/B split: the fraction of the frame's width, from the left, presented
  // untouched. 0 is off, 0.5 splits down the middle. A thin line marks the
  // seam. Costs nothing when off -- it is a constant in the compose.
  float splitView = 0.0f;

  // What the model is told about motion.
  //
  // Not settings: the runtime refuses to evaluate without a motion field, and
  // its reset flag has no observable effect, so both were tried and removed.
  // Nor is there a stabiliser here any more. One was built to steady the
  // model's edit, and it was only ever compensating for motion vectors that
  // were negated on the way in (flow/MotionVectorMath.h). With the sign
  // corrected the model's own temporal handling is steady, and the stabiliser
  // and its two settings were removed rather than left as dials that do
  // nothing useful.

  // The passes that will actually run: `passes`, or one pass of `base`.
  std::vector<NrPassSettings> Effective() const {
    if (passes.empty()) return {base};
    return passes;
  }
};

// The application the overlay captures: the window last chosen in the
// manager. Identified by what can be read from a window without opening its
// process -- class name and title. Empty means nothing has been chosen yet,
// and the runtime refuses to start rather than guess.
struct TargetAppSettings {
  std::string name;
  std::string windowClass;
  std::string title;
  bool Chosen() const { return !windowClass.empty(); }
};

// Global hotkeys the runtime registers. Strings, as ParseHotkey (core/Hotkeys.h)
// reads them; an unparseable one is reported and simply not registered.
struct HotkeySettings {
  std::string toggleHud = "ctrl+alt+h";
  std::string toggleOverlay = "ctrl+alt+o";
  std::string nextPreset = "ctrl+alt+pageup";
  std::string previousPreset = "ctrl+alt+pagedown";
  // Save debug frames without leaving the game: the dump happens on the
  // frame after the press, camera motion and all. It also ends a timing
  // recording and prints its histogram, if one is running.
  std::string dumpFrames = "ctrl+alt+d";
  // Start or stop accumulating per-pass GPU times. Starting clears whatever
  // was there; stopping with this key discards it, stopping with the dump key
  // prints it.
  std::string recordTimings = "ctrl+alt+r";
};

// Which of the recurring log lines are written. All off by default: an
// ordinary session should leave a log an operator can read, and these are the
// per-window lines that bury it. System events, warnings and errors are never
// affected by any of this.
//
// `verbose` is the master switch and the state of the manager's drawer. Off
// silences every category without forgetting which ones were chosen, so
// turning it back on restores the same selection.
struct LogSettings {
  bool verbose = false;
  bool performance = false;
  bool stages = false;
  bool capture = false;
  bool neural = false;

  uint32_t Mask() const {
    if (!verbose) return 0;
    uint32_t mask = 0;
    if (performance) mask |= static_cast<uint32_t>(LogCategory::Performance);
    if (stages) mask |= static_cast<uint32_t>(LogCategory::Stages);
    if (capture) mask |= static_cast<uint32_t>(LogCategory::Capture);
    if (neural) mask |= static_cast<uint32_t>(LogCategory::Neural);
    return mask;
  }
};

struct Config {
  bool showHud = true;
  bool showOverlay = true;
  uint32_t flowGridSize = 4;

  // "direct" drives the neural-rendering runtime itself; "passthrough" is the
  // A/B baseline. Every failure path degrades to passthrough with a warning
  // (spec section 11), so a first run with none of the operator-supplied files
  // produces a working overlay rather than an error.
  std::string neuralPass = "direct";

  // The synthetic depth plane, until there is a real one. "flat" writes the
  // constant below; "gradient" writes a ground-plane guess -- near at the
  // bottom of the frame, far at the top -- which is crude but is a shape, and
  // whether the model reacts to it decides whether real depth is worth
  // estimating. `depthInverted` tells the runtime near is the high value.
  float syntheticDepth = 0.0f;
  std::string depthMode = "flat";
  bool depthInverted = false;

  // Rectangles the neural pass leaves untouched, in capture pixels.
  std::vector<UiRect> uiMaskRects;
  uint32_t uiMaskFeather = 24;

  // The name of the preset these settings were last set from, so the manager
  // and the runtime's hotkeys can say which look is on and cycle from it.
  // Informational: the values below are the truth.
  std::string activePreset = "Recommended";

  // Whether the manager shows the settings that are rarely touched: the
  // pipeline internals, the model's preset slot, the mask's own switches,
  // chaining, per-pass tuning. Off by default; a manager preference, not
  // something the runtime reads.
  bool advancedTuning = false;

  TargetAppSettings app;
  HotkeySettings hotkeys;
  LogSettings logging;
  NrSettings nr;

  // Which look goes with which window, by window class. The manager applies
  // the remembered look when a target is chosen and records it when a look is
  // saved while that target is current.
  std::vector<std::pair<std::string, std::string>> appLooks;

  // The look remembered for `windowClass`, or empty.
  std::string LookForApp(std::string_view windowClass) const {
    for (const auto& [cls, look] : appLooks) {
      if (cls == windowClass) return look;
    }
    return {};
  }
  void RememberLookForApp(const std::string& windowClass, const std::string& look) {
    if (windowClass.empty() || look.empty()) return;
    for (auto& [cls, remembered] : appLooks) {
      if (cls == windowClass) { remembered = look; return; }
    }
    appLooks.emplace_back(windowClass, look);
  }
};

// Parses a TOML document. Never throws; problems are appended to `warnings`
// and the affected field keeps its default.
Config ParseConfig(std::string_view text, std::vector<std::string>& warnings);

// nullopt when the file does not exist or cannot be read. A malformed file is
// a warning, not a failure -- it parses to defaults.
std::optional<Config> LoadConfig(const std::filesystem::path& path,
                                 std::vector<std::string>& warnings);

std::string SerializeConfig(const Config& config);
bool SaveConfig(const std::filesystem::path& path, const Config& config);

}  // namespace sidecar
