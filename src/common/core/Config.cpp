#include "core/Config.h"
#include "core/Presets.h"

#include <toml++/toml.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace sidecar {
namespace {

// Every top-level key the document may contain. Anything else earns a warning
// so a typo is visible rather than silently ignored. The retired keys of the
// ReShade-hosted route are accepted silently so an old file does not warn
// about its own history.
constexpr std::array<std::string_view, 15> kKnownKeys = {
    "show_hud", "show_overlay", "flow_grid_size", "neural_pass", "synthetic_depth",
    "ui_mask",  "ui_mask_feather", "active_preset", "app", "hotkeys", "nr",
    "depth_mode", "depth_inverted", "app_look", "advanced_tuning",
};
constexpr std::array<std::string_view, 4> kRetiredKeys = {
    "dlss_preset", "neural", "wow_dir", "capture_mode",
};

bool IsKnown(std::string_view key) {
  return std::find(kKnownKeys.begin(), kKnownKeys.end(), key) != kKnownKeys.end() ||
         std::find(kRetiredKeys.begin(), kRetiredKeys.end(), key) != kRetiredKeys.end();
}

void ReadBool(const toml::table& root, std::string_view key, bool& target,
              std::vector<std::string>& warnings) {
  const auto node = root.get(key);
  if (!node) return;
  if (auto value = node->value<bool>()) {
    target = *value;
  } else {
    warnings.emplace_back(std::string(key) + ": expected a boolean; keeping the default");
  }
}

// Floats are clamped rather than rejected. A value out of range is a mistake
// worth reporting, but the nearest legal value is always a better outcome than
// silently reverting to a default the operator did not ask for.
void ReadFloat(const toml::table& root, std::string_view key, float& target,
               float low, float high, std::vector<std::string>& warnings) {
  const auto node = root.get(key);
  if (!node) return;
  const auto value = node->value<double>();
  if (!value) {
    warnings.emplace_back(std::string(key) + ": expected a number; keeping the default");
    return;
  }
  const auto clamped = std::clamp(static_cast<float>(*value), low, high);
  if (clamped != static_cast<float>(*value)) {
    warnings.emplace_back(std::string(key) + ": out of range; clamped");
  }
  target = clamped;
}

void ReadInt(const toml::table& root, std::string_view key, int& target,
             int low, int high, std::vector<std::string>& warnings) {
  const auto node = root.get(key);
  if (!node) return;
  const auto value = node->value<int64_t>();
  if (!value) {
    warnings.emplace_back(std::string(key) + ": expected an integer; keeping the default");
    return;
  }
  const auto clamped = std::clamp(static_cast<int>(*value), low, high);
  if (clamped != static_cast<int>(*value)) {
    warnings.emplace_back(std::string(key) + ": out of range; clamped");
  }
  target = clamped;
}

void ReadString(const toml::table& root, std::string_view key, std::string& target,
                std::vector<std::string>& warnings) {
  const auto node = root.get(key);
  if (!node) return;
  if (auto value = node->value<std::string>()) {
    target = *value;
  } else {
    warnings.emplace_back(std::string(key) + ": expected a string; keeping the default");
  }
}

// Two decimals, and a trailing ".0" on whole numbers so the file reads as TOML
// floats rather than integers -- which matters, because a bare 1 parses as an
// integer and would warn on the way back in.
std::string Number(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  return buffer;
}

const char* Boolean(bool value) { return value ? "true" : "false"; }

// A TOML basic string. Windows paths are full of backslashes, which a basic
// string treats as escapes, so they have to be doubled on the way out -- and
// a quote inside a path, unlikely as it is, must not end the string early.
std::string Quoted(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

// ---- the direct path's knobs ----------------------------------------------

// Read into a settings struct that already holds whatever it should inherit.
// Only keys present in the table change anything, which is what lets a pass
// override just one of them.
void ReadNrPass(const toml::table& table, NrPassSettings& s, const std::string& where,
                bool topLevel, std::vector<std::string>& warnings) {
  ReadInt(table, "preset", s.preset, 0, 3, warnings);
  ReadInt(table, "style", s.style, 0, 2, warnings);
  ReadFloat(table, "intensity", s.intensity, 0.0f, 2.0f, warnings);
  ReadFloat(table, "local_structure", s.localStructure, 0.0f, 2.0f, warnings);
  ReadFloat(table, "local_tone", s.localTone, 0.0f, 2.0f, warnings);
  ReadFloat(table, "skin_structure", s.skinStructure, -1.0f, 2.0f, warnings);
  ReadBool(table, "auto_mask", s.autoMask, warnings);
  ReadBool(table, "ui_correction", s.uiCorrection, warnings);
  for (const auto& [key, value] : table) {
    (void)value;
    const std::string_view k = key.str();
    const bool passKey = k == "preset" || k == "style" || k == "intensity" ||
                         k == "local_structure" || k == "local_tone" ||
                         k == "skin_structure" || k == "auto_mask" || k == "ui_correction";
    const bool frameKey = k == "pass" || k == "paper_white_nits" || k == "hdr_headroom" ||
                          k == "blend" || k == "bridge" || k == "colour_preserve" ||
                          k == "highlight_protect" || k == "model_scale" ||
                          k == "chain_composed" || k == "temporal_smoothing" ||
                          k == "temporal_spatial" || k == "temporal_reproject" ||
                          k == "evaluate_every" || k == "final_pass_full" ||
                          k == "per_pass_tuning" || k == "split_view" ||
                          k == "hdr_headroom";   // headroom/reproject/every are retired
    if (passKey) continue;
    if (frameKey && topLevel) continue;
    warnings.emplace_back(where + ": " + (frameKey ? "whole-frame key not allowed per pass: "
                                                   : "unknown key ignored: ") +
                          std::string(k));
  }
}

void ReadNrTable(const toml::table& table, NrSettings& nr, const std::string& where,
                 std::vector<std::string>& warnings) {
  ReadNrPass(table, nr.base, where, true, warnings);
  ReadBool(table, "bridge", nr.bridge, warnings);
  // 0 is "automatic"; anything else is clamped to a sane display range.
  {
    float paper = nr.paperWhiteNits;
    ReadFloat(table, "paper_white_nits", paper, 0.0f, 1000.0f, warnings);
    if (paper > 0.0f && paper < 80.0f) {
      warnings.emplace_back(where + ".paper_white_nits: below 80; using automatic");
      paper = 0.0f;
    }
    nr.paperWhiteNits = paper;
  }
  // hdr_headroom is retired: the runtime derives it from the display, and the
  // tone-map is identity below paper white so it never needed to be a dial.
  // "blend" is retired: intensity is the model's own dial for the same thing.
  // Accepted silently so an older file does not warn.
  ReadFloat(table, "colour_preserve", nr.colourPreserve, 0.0f, 1.0f, warnings);
  ReadFloat(table, "highlight_protect", nr.highlightProtect, 0.0f, 1.0f, warnings);
  ReadFloat(table, "split_view", nr.splitView, 0.0f, 1.0f, warnings);
  ReadFloat(table, "temporal_smoothing", nr.temporalSmoothing, 0.0f, 0.95f, warnings);
  ReadBool(table, "temporal_spatial", nr.temporalSpatial, warnings);
  // temporal_reproject and evaluate_every are retired: reprojection along
  // estimated vectors ghosted and half-rate needed it. Both read and ignored.
  ReadFloat(table, "model_scale", nr.modelScale, 0.5f, 1.0f, warnings);
  ReadBool(table, "final_pass_full", nr.finalPassFull, warnings);
  ReadBool(table, "per_pass_tuning", nr.perPassTuning, warnings);
  ReadBool(table, "chain_composed", nr.chainComposed, warnings);
  nr.passes.clear();
  if (const auto passesNode = table.get("pass")) {
    if (const auto* array = passesNode->as_array()) {
      size_t index = 0;
      for (const auto& element : *array) {
        ++index;
        const auto* entry = element.as_table();
        if (!entry) {
          warnings.emplace_back(where + ".pass: expected a table; entry ignored");
          continue;
        }
        NrPassSettings pass = nr.base;
        ReadNrPass(*entry, pass, where + ".pass[" + std::to_string(index) + "]", false,
                   warnings);
        nr.passes.push_back(pass);
      }
      if (nr.passes.size() > 4) {
        warnings.emplace_back(where + ".pass: more than four passes; keeping the first four");
        nr.passes.resize(4);
      }
    } else {
      warnings.emplace_back(where + ".pass: expected an array of tables; ignored");
    }
  }
}

void WriteNrPass(std::ostringstream& out, const NrPassSettings& s) {
  out << "preset = " << s.preset << "\n";
  out << "style = " << s.style << "\n";
  out << "intensity = " << Number(s.intensity) << "\n";
  out << "local_structure = " << Number(s.localStructure) << "\n";
  out << "local_tone = " << Number(s.localTone) << "\n";
  out << "skin_structure = " << Number(s.skinStructure) << "\n";
  out << "auto_mask = " << Boolean(s.autoMask) << "\n";
  out << "ui_correction = " << Boolean(s.uiCorrection) << "\n";
}

// `header` is the table name the section is written under: "nr" in the config
// file, "preset.nr" inside a [[preset]] entry.
void WriteNrTable(std::ostringstream& out, const NrSettings& nr, const std::string& header) {
  out << "[" << header << "]\n";
  out << "bridge = " << Boolean(nr.bridge) << "\n";
  out << "paper_white_nits = " << Number(nr.paperWhiteNits) << "\n";
  out << "colour_preserve = " << Number(nr.colourPreserve) << "\n";
  out << "highlight_protect = " << Number(nr.highlightProtect) << "\n";
  out << "split_view = " << Number(nr.splitView) << "\n";
  out << "temporal_smoothing = " << Number(nr.temporalSmoothing) << "\n";
  out << "temporal_spatial = " << Boolean(nr.temporalSpatial) << "\n";
  out << "model_scale = " << Number(nr.modelScale) << "\n";
  out << "final_pass_full = " << Boolean(nr.finalPassFull) << "\n";
  out << "per_pass_tuning = " << Boolean(nr.perPassTuning) << "\n";
  out << "chain_composed = " << Boolean(nr.chainComposed) << "\n";
  WriteNrPass(out, nr.base);
  for (const auto& pass : nr.passes) {
    out << "\n[[" << header << ".pass]]\n";
    WriteNrPass(out, pass);
  }
}

void ReadNeuralPass(const toml::table& table, std::string& target,
                    std::vector<std::string>& warnings) {
  ReadString(table, "neural_pass", target, warnings);
  // The names of retired routes map onto what they became; the factory says so
  // in its own warning if it ever sees one, but a config file should not keep
  // carrying them.
  if (target == "reshade" || target == "ngx") target = "direct";
}

void ReadFlowGrid(const toml::table& table, uint32_t& target,
                  std::vector<std::string>& warnings) {
  const auto node = table.get("flow_grid_size");
  if (!node) return;
  const auto value = node->value<int64_t>();
  if (!value) {
    warnings.emplace_back("flow_grid_size: expected an integer; using 4");
  } else if (*value != 1 && *value != 2 && *value != 4) {
    warnings.emplace_back("flow_grid_size: must be 1, 2 or 4; using 4");
  } else {
    target = static_cast<uint32_t>(*value);
  }
}

}  // namespace

// ---- the config file --------------------------------------------------------

Config ParseConfig(std::string_view text, std::vector<std::string>& warnings) {
  Config config;

  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& error) {
    warnings.emplace_back(std::string("could not parse config: ") +
                          std::string(error.description()));
    return config;   // every default stays in place
  }

  ReadBool(root, "show_hud", config.showHud, warnings);
  ReadBool(root, "show_overlay", config.showOverlay, warnings);
  ReadFlowGrid(root, config.flowGridSize, warnings);
  ReadNeuralPass(root, config.neuralPass, warnings);
  ReadFloat(root, "synthetic_depth", config.syntheticDepth, 0.0f, 1.0f, warnings);
  ReadString(root, "depth_mode", config.depthMode, warnings);
  if (config.depthMode != "flat" && config.depthMode != "gradient") {
    warnings.emplace_back("depth_mode: must be \"flat\" or \"gradient\"; using flat");
    config.depthMode = "flat";
  }
  ReadBool(root, "depth_inverted", config.depthInverted, warnings);
  ReadString(root, "active_preset", config.activePreset, warnings);
  ReadBool(root, "advanced_tuning", config.advancedTuning, warnings);

  if (const auto node = root.get("ui_mask_feather")) {
    int feather = static_cast<int>(config.uiMaskFeather);
    ReadInt(root, "ui_mask_feather", feather, 0, 256, warnings);
    config.uiMaskFeather = static_cast<uint32_t>(feather);
  }

  if (const auto node = root.get("ui_mask")) {
    if (const auto* array = node->as_array()) {
      for (const auto& element : *array) {
        const auto* entry = element.as_table();
        if (!entry) {
          warnings.emplace_back("ui_mask: expected a table of edges; entry ignored");
          continue;
        }
        UiRect rect;
        rect.left = static_cast<int32_t>((*entry)["left"].value_or<int64_t>(0));
        rect.top = static_cast<int32_t>((*entry)["top"].value_or<int64_t>(0));
        rect.right = static_cast<int32_t>((*entry)["right"].value_or<int64_t>(0));
        rect.bottom = static_cast<int32_t>((*entry)["bottom"].value_or<int64_t>(0));
        config.uiMaskRects.push_back(rect);
      }
    } else {
      warnings.emplace_back("ui_mask: expected an array of tables; ignored");
    }
  }

  // The app. An old file's wow_dir is accepted and ignored.
  if (const auto node = root.get("app")) {
    if (const auto* table = node->as_table()) {
      ReadString(*table, "name", config.app.name, warnings);
      ReadString(*table, "window_class", config.app.windowClass, warnings);
      ReadString(*table, "title", config.app.title, warnings);
    } else {
      warnings.emplace_back("app: expected a table; ignored");
    }
  }

  if (const auto node = root.get("hotkeys")) {
    if (const auto* table = node->as_table()) {
      ReadString(*table, "toggle_hud", config.hotkeys.toggleHud, warnings);
      ReadString(*table, "toggle_overlay", config.hotkeys.toggleOverlay, warnings);
      ReadString(*table, "next_preset", config.hotkeys.nextPreset, warnings);
      ReadString(*table, "previous_preset", config.hotkeys.previousPreset, warnings);
      ReadString(*table, "dump_frames", config.hotkeys.dumpFrames, warnings);
    } else {
      warnings.emplace_back("hotkeys: expected a table; ignored");
    }
  }

  if (const auto node = root.get("app_look")) {
    if (const auto* array = node->as_array()) {
      for (const auto& element : *array) {
        const auto* entry = element.as_table();
        if (!entry) continue;
        std::string cls, look;
        ReadString(*entry, "window_class", cls, warnings);
        ReadString(*entry, "preset", look, warnings);
        config.RememberLookForApp(cls, look);
      }
    } else {
      warnings.emplace_back("app_look: expected an array of tables; ignored");
    }
  }

  if (const auto node = root.get("nr")) {
    if (const auto* table = node->as_table()) {
      ReadNrTable(*table, config.nr, "nr", warnings);
    } else {
      warnings.emplace_back("nr: expected a table; ignored");
    }
  }

  for (const auto& [key, value] : root) {
    (void)value;
    if (!IsKnown(key.str())) {
      warnings.emplace_back(std::string("unknown key ignored: ") + std::string(key.str()));
    }
  }
  return config;
}

std::optional<Config> LoadConfig(const std::filesystem::path& path,
                                 std::vector<std::string>& warnings) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  return ParseConfig(text, warnings);
}

std::string SerializeConfig(const Config& config) {
  std::ostringstream out;
  out << "# DLSS 5 sidecar. Written by the manager; hand edits are read back on\n"
         "# the next launch and overwritten on the next save.\n\n";

  out << "neural_pass = \"" << config.neuralPass << "\"\n";
  out << "active_preset = " << Quoted(config.activePreset) << "\n";
  out << "advanced_tuning = " << Boolean(config.advancedTuning) << "\n";
  out << "show_hud = " << Boolean(config.showHud) << "\n";
  out << "show_overlay = " << Boolean(config.showOverlay) << "\n";
  out << "flow_grid_size = " << config.flowGridSize << "\n";
  out << "synthetic_depth = " << Number(config.syntheticDepth) << "\n";
  out << "depth_mode = \"" << config.depthMode << "\"\n";
  out << "depth_inverted = " << Boolean(config.depthInverted) << "\n";
  out << "ui_mask_feather = " << config.uiMaskFeather << "\n";

  out << "\n# The window the overlay captures: whichever was last chosen in the\n"
         "# manager. Empty until something is.\n";
  out << "[app]\n";
  out << "name = " << Quoted(config.app.name) << "\n";
  out << "window_class = " << Quoted(config.app.windowClass) << "\n";
  out << "title = " << Quoted(config.app.title) << "\n";

  out << "\n# Global hotkeys, as \"ctrl+alt+key\". One modifier at least.\n";
  out << "[hotkeys]\n";
  out << "toggle_hud = " << Quoted(config.hotkeys.toggleHud) << "\n";
  out << "toggle_overlay = " << Quoted(config.hotkeys.toggleOverlay) << "\n";
  out << "next_preset = " << Quoted(config.hotkeys.nextPreset) << "\n";
  out << "previous_preset = " << Quoted(config.hotkeys.previousPreset) << "\n";
  out << "dump_frames = " << Quoted(config.hotkeys.dumpFrames) << "\n";

  out << "\n# The neural-rendering path. The model's own knobs; ranges are the model's\n"
         "# (preset 0-3, style 0-2, strengths 0-2, skin structure -1 for off). Each\n"
         "# [[nr.pass]] starts from these and overrides what it names; no [[nr.pass]]\n"
         "# means one pass at these values.\n";
  WriteNrTable(out, config.nr, "nr");

  // The mask goes last: it is the only unbounded section, and a long one would
  // otherwise push everything readable off the top of the file.
  if (!config.appLooks.empty()) {
    out << "\n# Which look goes with which window, by window class. Written when a look\n"
           "# is saved while that window is the capture target.\n";
    for (const auto& [cls, look] : config.appLooks) {
      out << "\n[[app_look]]\nwindow_class = " << Quoted(cls) << "\npreset = " << Quoted(look)
          << "\n";
    }
  }
  for (const auto& rect : config.uiMaskRects) {
    out << "\n[[ui_mask]]\n";
    out << "left = " << rect.left << "\n";
    out << "top = " << rect.top << "\n";
    out << "right = " << rect.right << "\n";
    out << "bottom = " << rect.bottom << "\n";
  }
  return out.str();
}

bool SaveConfig(const std::filesystem::path& path, const Config& config) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  const std::string text = SerializeConfig(config);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

// ---- presets -----------------------------------------------------------------

void ApplyPreset(const Preset& preset, Config& config) {
  config.neuralPass = preset.neuralPass;
  config.flowGridSize = preset.flowGridSize;
  config.syntheticDepth = preset.syntheticDepth;
  config.nr = preset.nr;
  config.activePreset = preset.name;
}

Preset PresetFromConfig(std::string name, const Config& config) {
  Preset p;
  p.name = std::move(name);
  p.neuralPass = config.neuralPass;
  p.flowGridSize = config.flowGridSize;
  p.syntheticDepth = config.syntheticDepth;
  p.nr = config.nr;
  return p;
}

namespace {

bool SameNrPass(const NrPassSettings& a, const NrPassSettings& b) {
  return a.preset == b.preset && a.style == b.style && a.intensity == b.intensity &&
         a.localStructure == b.localStructure && a.localTone == b.localTone &&
         a.skinStructure == b.skinStructure && a.autoMask == b.autoMask &&
         a.uiCorrection == b.uiCorrection;
}

bool SameNr(const NrSettings& a, const NrSettings& b) {
  if (a.bridge != b.bridge || a.paperWhiteNits != b.paperWhiteNits ||
      a.colourPreserve != b.colourPreserve || a.highlightProtect != b.highlightProtect ||
      a.temporalSmoothing != b.temporalSmoothing || a.temporalSpatial != b.temporalSpatial ||
      a.modelScale != b.modelScale || a.finalPassFull != b.finalPassFull ||
      a.chainComposed != b.chainComposed) {
    return false;
  }
  const auto ea = a.Effective();
  const auto eb = b.Effective();
  if (ea.size() != eb.size()) return false;
  for (size_t i = 0; i < ea.size(); ++i) {
    if (!SameNrPass(ea[i], eb[i])) return false;
  }
  return true;
}

}  // namespace

bool PresetMatchesConfig(const Preset& preset, const Config& config) {
  if (preset.neuralPass != config.neuralPass) return false;
  if (preset.neuralPass == "passthrough") return true;
  return preset.flowGridSize == config.flowGridSize &&
         preset.syntheticDepth == config.syntheticDepth && SameNr(preset.nr, config.nr);
}

const std::vector<Preset>& BuiltinPresets() {
  static const std::vector<Preset> presets = [] {
    std::vector<Preset> out;

    Preset recommended;
    recommended.name = "Recommended";
    recommended.summary = "The tuned default. Start here.";
    recommended.detail =
        "Two cinematic passes: the first at half resolution for the lighting, the "
        "second at full resolution for the structure, chained through the compose. "
        "Local tone eased, a little skin structure, the original's colour kept at "
        "90%. Tuned on Silvermoon at 4K, which is about the harshest lighting WoW has.";
    recommended.builtin = true;
    recommended.nr.colourPreserve = 0.90f;
    recommended.nr.highlightProtect = 0.60f;
    recommended.nr.temporalSmoothing = 0.60f;
    recommended.nr.modelScale = 0.50f;
    recommended.nr.finalPassFull = true;
    NrPassSettings pass;
    pass.style = 2;
    pass.intensity = 1.0f;
    pass.localStructure = 0.95f;
    pass.localTone = 0.65f;
    pass.skinStructure = 0.40f;
    recommended.nr.base = pass;
    recommended.nr.passes = {pass, pass};
    out.push_back(recommended);

    Preset off;
    off.name = "Off (A/B baseline)";
    off.summary = "Capture and present, untouched.";
    off.detail =
        "No neural work at all, on the same capture and present path. This is the "
        "honest comparison: whatever you see here is what the overlay costs you "
        "before any neural rendering happens.";
    off.builtin = true;
    off.neuralPass = "passthrough";
    out.push_back(off);
    return out;
  }();
  return presets;
}

std::vector<Preset> ParsePresets(std::string_view text, std::vector<std::string>& warnings) {
  std::vector<Preset> presets;
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error& error) {
    warnings.emplace_back(std::string("could not parse presets: ") +
                          std::string(error.description()));
    return presets;
  }
  const auto node = root.get("preset");
  if (!node) return presets;
  const auto* array = node->as_array();
  if (!array) {
    warnings.emplace_back("preset: expected an array of tables; ignored");
    return presets;
  }
  size_t index = 0;
  for (const auto& element : *array) {
    ++index;
    const auto* table = element.as_table();
    if (!table) {
      warnings.emplace_back("preset[" + std::to_string(index) + "]: expected a table; ignored");
      continue;
    }
    Preset p;
    ReadString(*table, "name", p.name, warnings);
    if (p.name.empty()) {
      warnings.emplace_back("preset[" + std::to_string(index) + "]: no name; ignored");
      continue;
    }
    bool clashes = false;
    for (const auto& b : BuiltinPresets()) {
      if (b.name == p.name) clashes = true;
    }
    for (const auto& existing : presets) {
      if (existing.name == p.name) clashes = true;
    }
    if (clashes) {
      warnings.emplace_back("preset \"" + p.name + "\" repeats a name; ignored");
      continue;
    }
    ReadString(*table, "summary", p.summary, warnings);
    ReadString(*table, "detail", p.detail, warnings);
    ReadNeuralPass(*table, p.neuralPass, warnings);
    ReadFlowGrid(*table, p.flowGridSize, warnings);
    ReadFloat(*table, "synthetic_depth", p.syntheticDepth, 0.0f, 1.0f, warnings);
    if (const auto nrNode = table->get("nr")) {
      if (const auto* nrTable = nrNode->as_table()) {
        ReadNrTable(*nrTable, p.nr, "preset \"" + p.name + "\".nr", warnings);
      } else {
        warnings.emplace_back("preset \"" + p.name + "\": nr is not a table; ignored");
      }
    }
    presets.push_back(std::move(p));
  }
  return presets;
}

std::string SerializePresets(const std::vector<Preset>& presets) {
  std::ostringstream out;
  out << "# Custom looks, saved from the manager's Tuning page. One [[preset]] each;\n"
         "# the built-in presets are not written here and cannot be replaced.\n";
  for (const auto& p : presets) {
    if (p.builtin) continue;
    out << "\n[[preset]]\n";
    out << "name = " << Quoted(p.name) << "\n";
    out << "summary = " << Quoted(p.summary) << "\n";
    out << "detail = " << Quoted(p.detail) << "\n";
    out << "neural_pass = \"" << p.neuralPass << "\"\n";
    out << "flow_grid_size = " << p.flowGridSize << "\n";
    out << "synthetic_depth = " << Number(p.syntheticDepth) << "\n";
    out << "\n";
    WriteNrTable(out, p.nr, "preset.nr");
  }
  return out.str();
}

std::vector<Preset> LoadCustomPresets(const std::filesystem::path& path,
                                      std::vector<std::string>& warnings) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  return ParsePresets(text, warnings);
}

bool SaveCustomPresets(const std::filesystem::path& path, const std::vector<Preset>& presets) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  const std::string text = SerializePresets(presets);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::vector<Preset> AllPresets(const std::filesystem::path& customPath,
                               std::vector<std::string>& warnings) {
  std::vector<Preset> all = BuiltinPresets();
  for (auto& p : LoadCustomPresets(customPath, warnings)) all.push_back(std::move(p));
  return all;
}

}  // namespace sidecar
