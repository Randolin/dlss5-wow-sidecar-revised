#include <catch2/catch_test_macros.hpp>
#include "core/Config.h"

using namespace sidecar;

TEST_CASE("defaults apply to an empty document", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig("", warnings);
  REQUIRE(cfg.showHud == true);
  REQUIRE(cfg.showOverlay == true);
  REQUIRE(cfg.flowGridSize == 4);
  REQUIRE(cfg.neuralPass == "direct");
  REQUIRE(cfg.syntheticDepth == 0.0f);
  REQUIRE(cfg.nr.passes.empty());
  REQUIRE(cfg.nr.Effective().size() == 1);
  REQUIRE(cfg.uiMaskRects.empty());
  REQUIRE(cfg.activePreset == "Recommended");
  // No capture target until one is chosen.
  REQUIRE(cfg.app.name.empty());
  REQUIRE(cfg.app.windowClass.empty());
  REQUIRE_FALSE(cfg.app.Chosen());
  REQUIRE(cfg.hotkeys.toggleHud == "ctrl+alt+h");
  REQUIRE(cfg.hotkeys.dumpFrames == "ctrl+alt+d");
  REQUIRE(warnings.empty());
}

TEST_CASE("values are read from the document", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"toml(
    show_hud = false
    flow_grid_size = 2
    neural_pass = "passthrough"
    active_preset = "Off (A/B baseline)"
  )toml", warnings);
  REQUIRE(cfg.showHud == false);
  REQUIRE(cfg.flowGridSize == 2);
  REQUIRE(cfg.neuralPass == "passthrough");
  REQUIRE(cfg.activePreset == "Off (A/B baseline)");
  REQUIRE(warnings.empty());
}

TEST_CASE("retired route names read as direct", "[unit]") {
  // Old files name routes that no longer exist. They become the direct path
  // without a warning: the file is not wrong, it is old.
  std::vector<std::string> warnings;
  REQUIRE(ParseConfig("neural_pass = \"reshade\"", warnings).neuralPass == "direct");
  REQUIRE(ParseConfig("neural_pass = \"ngx\"", warnings).neuralPass == "direct");
  REQUIRE(warnings.empty());
}

TEST_CASE("retired keys are accepted silently", "[unit]") {
  // A file written by an earlier build carries [neural] and dlss_preset. They
  // are ignored, and they do not earn an "unknown key" warning.
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    dlss_preset = "cnn-f"
    [neural]
    intensity = 0.5
  )", warnings);
  REQUIRE(warnings.empty());
  REQUIRE(cfg.neuralPass == "direct");
}

TEST_CASE("an old wow_dir is accepted and ignored", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(wow_dir = "C:\\Games\\WoW")", warnings);
  REQUIRE(warnings.empty());
  REQUIRE_FALSE(cfg.app.Chosen());
}

TEST_CASE("the retired capture_mode key is accepted silently", "[unit]") {
  std::vector<std::string> warnings;
  ParseConfig("capture_mode = \"monitor\"", warnings);
  REQUIRE(warnings.empty());
}

TEST_CASE("UI mask rectangles round-trip", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    [[ui_mask]]
    left = 0
    top = 900
    right = 1920
    bottom = 1080

    [[ui_mask]]
    left = 1600
    top = 0
    right = 1920
    bottom = 300
  )", warnings);
  REQUIRE(cfg.uiMaskRects.size() == 2);
  REQUIRE(cfg.uiMaskRects[0].top == 900);
  REQUIRE(cfg.uiMaskRects[1].left == 1600);
  REQUIRE(warnings.empty());
}

TEST_CASE("an invalid grid size warns and falls back", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig("flow_grid_size = 7", warnings);
  REQUIRE(cfg.flowGridSize == 4);
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("flow_grid_size") != std::string::npos);
}

TEST_CASE("malformed TOML warns rather than throwing", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig("this is not = = toml", warnings);
  REQUIRE(cfg.showHud == true);
  REQUIRE(warnings.size() >= 1);
}

TEST_CASE("unknown keys warn but do not break the rest of the document", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    show_hud = false
    nonexistent_key = 42
  )", warnings);
  REQUIRE(cfg.showHud == false);
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("nonexistent_key") != std::string::npos);
}

TEST_CASE("a written config reads back as itself", "[unit]") {
  Config config;
  config.neuralPass = "passthrough";
  config.showHud = false;
  config.flowGridSize = 2;
  config.syntheticDepth = 0.25f;
  config.uiMaskFeather = 8;
  config.activePreset = "My look";
  config.app.name = "Some Game";
  config.app.windowClass = "UnrealWindow";
  config.app.title = "Some Game  ";
  config.hotkeys.nextPreset = "ctrl+shift+f9";
  config.uiMaskRects.push_back(UiRect{0, 900, 1920, 1080});

  std::vector<std::string> warnings;
  const auto reread = ParseConfig(SerializeConfig(config), warnings);

  REQUIRE(warnings.empty());
  REQUIRE(reread.neuralPass == config.neuralPass);
  REQUIRE(reread.showHud == config.showHud);
  REQUIRE(reread.flowGridSize == config.flowGridSize);
  REQUIRE(reread.syntheticDepth == config.syntheticDepth);
  REQUIRE(reread.uiMaskFeather == config.uiMaskFeather);
  REQUIRE(reread.activePreset == config.activePreset);
  REQUIRE(reread.app.name == config.app.name);
  REQUIRE(reread.app.windowClass == config.app.windowClass);
  REQUIRE(reread.app.title == config.app.title);
  REQUIRE(reread.hotkeys.nextPreset == config.hotkeys.nextPreset);
  REQUIRE(reread.hotkeys.toggleHud == config.hotkeys.toggleHud);
  REQUIRE(reread.uiMaskRects.size() == 1);
  REQUIRE(reread.uiMaskRects[0].bottom == 1080);
}

TEST_CASE("the app title survives backslashes and quotes on a round trip", "[unit]") {
  Config config;
  config.app.title = R"(Some "Game" \ Window)";
  std::vector<std::string> warnings;
  const auto reread = ParseConfig(SerializeConfig(config), warnings);
  REQUIRE(warnings.empty());
  REQUIRE(reread.app.title == config.app.title);
}

TEST_CASE("direct-route passes inherit the base and override what they name", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    [nr]
    intensity = 0.8
    style = 1

    [[nr.pass]]

    [[nr.pass]]
    style = 2
    intensity = 0.5

    [[nr.pass]]
    skin_structure = 1.0
  )", warnings);
  REQUIRE(warnings.empty());
  REQUIRE(cfg.nr.base.intensity == 0.8f);
  REQUIRE(cfg.nr.base.style == 1);
  REQUIRE(cfg.nr.passes.size() == 3);
  REQUIRE(cfg.nr.passes[0].intensity == 0.8f);
  REQUIRE(cfg.nr.passes[0].style == 1);
  REQUIRE(cfg.nr.passes[1].style == 2);
  REQUIRE(cfg.nr.passes[1].intensity == 0.5f);
  REQUIRE(cfg.nr.passes[1].autoMask == true);
  REQUIRE(cfg.nr.passes[2].skinStructure == 1.0f);
  REQUIRE(cfg.nr.passes[2].style == 1);
  REQUIRE(cfg.nr.Effective().size() == 3);
}

TEST_CASE("direct-route passes round-trip through the writer", "[unit]") {
  Config config;
  config.nr.base.intensity = 0.75f;
  config.nr.base.style = 2;
  NrPassSettings second = config.nr.base;
  second.intensity = 0.25f;
  second.autoMask = false;
  config.nr.passes = {config.nr.base, second};

  std::vector<std::string> warnings;
  const auto reread = ParseConfig(SerializeConfig(config), warnings);
  REQUIRE(warnings.empty());
  REQUIRE(reread.nr.base.intensity == 0.75f);
  REQUIRE(reread.nr.base.style == 2);
  REQUIRE(reread.nr.passes.size() == 2);
  REQUIRE(reread.nr.passes[1].intensity == 0.25f);
  REQUIRE(reread.nr.passes[1].autoMask == false);
  REQUIRE(reread.nr.passes[0].intensity == 0.75f);
}

TEST_CASE("direct-route values are clamped to the model's ranges", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    [nr]
    style = 7
    intensity = 9.0
    skin_structure = -3.0
  )", warnings);
  REQUIRE(cfg.nr.base.style == 2);
  REQUIRE(cfg.nr.base.intensity == 2.0f);
  REQUIRE(cfg.nr.base.skinStructure == -1.0f);
  REQUIRE(warnings.size() == 3);
}

TEST_CASE("the compose settings are whole-frame, not per pass", "[unit]") {
  std::vector<std::string> warnings;
  const auto cfg = ParseConfig(R"(
    [nr]
    paper_white_nits = 160.0
    colour_preserve = 0.5

    [[nr.pass]]
    colour_preserve = 0.1
  )", warnings);
  REQUIRE(cfg.nr.paperWhiteNits == 160.0f);
  REQUIRE(cfg.nr.colourPreserve == 0.5f);
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("whole-frame") != std::string::npos);

  // The retired blend and headroom keys are accepted without a word.
  std::vector<std::string> retired;
  ParseConfig("[nr]\nblend = 0.5\nhdr_headroom = 3.0\n", retired);
  REQUIRE(retired.empty());

  const auto defaults = ParseConfig("", warnings);
  REQUIRE(defaults.nr.bridge == false);
  REQUIRE(defaults.nr.paperWhiteNits == 0.0f);   // automatic
  REQUIRE(defaults.nr.colourPreserve == 1.0f);
  REQUIRE(defaults.nr.highlightProtect == 0.6f);
  REQUIRE(defaults.nr.temporalSmoothing == 0.0f);
  REQUIRE(defaults.nr.temporalSpatial == false);
  // Retired keys read silently.
  std::vector<std::string> retiredWarnings;
  ParseConfig("[nr]\nevaluate_every = 2\ntemporal_reproject = true\n", retiredWarnings);
  REQUIRE(retiredWarnings.empty());
  REQUIRE(defaults.nr.modelScale == 1.0f);
  REQUIRE(defaults.nr.finalPassFull == false);
  REQUIRE(defaults.nr.chainComposed == true);

  std::vector<std::string> scaleWarnings;
  const auto scaled = ParseConfig(R"(
    [nr]
    model_scale = 0.75
    highlight_protect = 0.3
    chain_composed = false
  )", scaleWarnings);
  REQUIRE(scaleWarnings.empty());
  REQUIRE(scaled.nr.modelScale == 0.75f);
  REQUIRE(scaled.nr.highlightProtect == 0.3f);
  REQUIRE(scaled.nr.chainComposed == false);
  const auto tooSmall = ParseConfig("[nr]\nmodel_scale = 0.1\n", scaleWarnings);
  REQUIRE(tooSmall.nr.modelScale == 0.5f);
  REQUIRE(scaleWarnings.size() == 1);

  std::vector<std::string> rewarn;
  const auto reread = ParseConfig(SerializeConfig(cfg), rewarn);
  REQUIRE(rewarn.empty());
  REQUIRE(reread.nr.paperWhiteNits == 160.0f);
  REQUIRE(reread.nr.colourPreserve == 0.5f);
}

TEST_CASE("per-app looks round-trip", "[unit]") {
  Config config;
  config.RememberLookForApp("GxWindowClass", "Cinematic Tuned");
  config.RememberLookForApp("Chrome_WidgetWin_1", "Softer");
  // Re-remembering replaces rather than appends.
  config.RememberLookForApp("GxWindowClass", "Recommended");
  REQUIRE(config.appLooks.size() == 2);
  REQUIRE(config.LookForApp("GxWindowClass") == "Recommended");

  std::vector<std::string> warnings;
  const auto reread = ParseConfig(SerializeConfig(config), warnings);
  REQUIRE(warnings.empty());
  REQUIRE(reread.LookForApp("GxWindowClass") == "Recommended");
  REQUIRE(reread.LookForApp("Chrome_WidgetWin_1") == "Softer");
  REQUIRE(reread.LookForApp("nothing").empty());
}
