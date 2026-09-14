#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Config.h"

namespace sidecar {

// A look: the settings that decide what the overlay does to the picture. Not
// the mask, not the app, not the hotkeys -- those are about the machine, and a
// preset is about the image.
struct Preset {
  std::string name;
  std::string summary;   // one line, for the card
  std::string detail;    // a paragraph, for the card
  bool builtin = false;  // shipped with the sidecar; cannot be deleted or replaced

  std::string neuralPass = "direct";
  uint32_t flowGridSize = 4;
  float syntheticDepth = 0.0f;
  NrSettings nr;
};

// Writes the preset's look into the config and records its name as the active
// preset. Everything else in the config is untouched.
void ApplyPreset(const Preset& preset, Config& config);

// The config's current look, as a preset with the given name.
Preset PresetFromConfig(std::string name, const Config& config);

// Whether the config's look is this preset's look. Compared on the fields a
// preset carries, so an unrelated change -- the HUD toggle, a mask rectangle --
// does not read as "custom". The name is not consulted.
bool PresetMatchesConfig(const Preset& preset, const Config& config);

// The two that ship. First is the default look; second is the A/B baseline.
const std::vector<Preset>& BuiltinPresets();

// Custom presets live beside the sidecar in presets.toml, one [[preset]] per
// entry. Parsing never throws; problems go to `warnings` and the entry is
// skipped. Names are unique; a custom preset with a built-in's name is
// dropped with a warning rather than shadowing it.
std::vector<Preset> ParsePresets(std::string_view text, std::vector<std::string>& warnings);
std::string SerializePresets(const std::vector<Preset>& presets);
std::vector<Preset> LoadCustomPresets(const std::filesystem::path& path,
                                      std::vector<std::string>& warnings);
bool SaveCustomPresets(const std::filesystem::path& path, const std::vector<Preset>& presets);

// Built-ins followed by the custom ones in `path`, which is what the manager
// shows and the runtime's hotkeys cycle through.
std::vector<Preset> AllPresets(const std::filesystem::path& customPath,
                               std::vector<std::string>& warnings);

}  // namespace sidecar
