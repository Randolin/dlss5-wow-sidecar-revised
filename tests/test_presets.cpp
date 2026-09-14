#include <catch2/catch_test_macros.hpp>
#include "core/Presets.h"

using namespace sidecar;

TEST_CASE("two presets ship, and the first is the default look", "[unit]") {
  const auto& builtin = BuiltinPresets();
  REQUIRE(builtin.size() == 2);
  REQUIRE(builtin[0].name == "Recommended");
  REQUIRE(builtin[0].builtin);
  REQUIRE(builtin[0].neuralPass == "direct");
  REQUIRE(builtin[0].nr.Effective().size() == 2);
  REQUIRE(builtin[1].name == "Off (A/B baseline)");
  REQUIRE(builtin[1].neuralPass == "passthrough");
}

TEST_CASE("applying a preset sets the look and records its name", "[unit]") {
  Config config;
  config.showHud = false;                 // not a look setting; must survive
  config.app.name = "Some Game";          // neither is this
  ApplyPreset(BuiltinPresets()[0], config);
  REQUIRE(config.activePreset == "Recommended");
  REQUIRE(config.nr.passes.size() == 2);
  REQUIRE(config.nr.colourPreserve == 0.90f);
  REQUIRE(config.showHud == false);
  REQUIRE(config.app.name == "Some Game");
  REQUIRE(PresetMatchesConfig(BuiltinPresets()[0], config));
  REQUIRE_FALSE(PresetMatchesConfig(BuiltinPresets()[1], config));
}

TEST_CASE("matching is on the look, not the name or the machine", "[unit]") {
  Config config;
  ApplyPreset(BuiltinPresets()[0], config);
  config.activePreset = "something else";
  config.uiMaskRects.push_back(UiRect{0, 0, 10, 10});
  REQUIRE(PresetMatchesConfig(BuiltinPresets()[0], config));
  // One knob off the preset, and it no longer matches.
  config.nr.colourPreserve = 0.5f;
  REQUIRE_FALSE(PresetMatchesConfig(BuiltinPresets()[0], config));
  // Passthrough matches passthrough regardless of the neural knobs.
  config.neuralPass = "passthrough";
  REQUIRE(PresetMatchesConfig(BuiltinPresets()[1], config));
}

TEST_CASE("custom presets round-trip through the writer", "[unit]") {
  Config config;
  ApplyPreset(BuiltinPresets()[0], config);
  config.nr.modelScale = 0.75f;
  config.nr.passes[1].intensity = 0.4f;
  config.flowGridSize = 2;

  Preset custom = PresetFromConfig("Mine", config);
  custom.summary = "A summary";
  custom.detail = "Some \"quoted\" detail\\with a backslash";

  std::vector<std::string> warnings;
  const auto reread = ParsePresets(SerializePresets({custom}), warnings);
  REQUIRE(warnings.empty());
  REQUIRE(reread.size() == 1);
  REQUIRE(reread[0].name == "Mine");
  REQUIRE(reread[0].summary == "A summary");
  REQUIRE(reread[0].detail == custom.detail);
  REQUIRE_FALSE(reread[0].builtin);
  REQUIRE(reread[0].flowGridSize == 2);
  REQUIRE(reread[0].nr.modelScale == 0.75f);
  REQUIRE(reread[0].nr.passes.size() == 2);
  REQUIRE(reread[0].nr.passes[1].intensity == 0.4f);
  REQUIRE(PresetMatchesConfig(reread[0], config));
}

TEST_CASE("built-ins are never written and cannot be shadowed", "[unit]") {
  std::vector<Preset> all = BuiltinPresets();
  Preset custom = PresetFromConfig("Mine", Config{});
  all.push_back(custom);
  const std::string text = SerializePresets(all);
  REQUIRE(text.find("Recommended") == std::string::npos);
  REQUIRE(text.find("Mine") != std::string::npos);

  // A file that tries to define "Recommended" is refused with a warning.
  std::vector<std::string> warnings;
  const auto parsed = ParsePresets(R"(
    [[preset]]
    name = "Recommended"
    [[preset]]
    name = "Mine"
    [[preset]]
    name = "Mine"
  )", warnings);
  REQUIRE(parsed.size() == 1);
  REQUIRE(parsed[0].name == "Mine");
  REQUIRE(warnings.size() == 2);
}

TEST_CASE("a preset with no name, or malformed text, is skipped not fatal", "[unit]") {
  std::vector<std::string> warnings;
  REQUIRE(ParsePresets("[[preset]]\nsummary = \"x\"\n", warnings).empty());
  REQUIRE(warnings.size() == 1);
  warnings.clear();
  REQUIRE(ParsePresets("not = = toml", warnings).empty());
  REQUIRE(warnings.size() == 1);
}
