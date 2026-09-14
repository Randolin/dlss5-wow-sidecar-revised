#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>

#include "core/GpuProfile.h"
#include "gpu/DeviceBridge.h"
#include "neural/NeuralPassFactory.h"

using namespace sidecar;

TEST_CASE("the default pass name builds passthrough without complaint", "[unit]") {
  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("passthrough", warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(std::string(pass->Name()) == "passthrough");
  REQUIRE(warnings.empty());
}

TEST_CASE("an unknown pass name warns and falls back to passthrough", "[unit]") {
  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("no-such-pass", warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(std::string(pass->Name()) == "passthrough");
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("no-such-pass") != std::string::npos);
}

// "ngx" and "reshade" are retired names; both become "direct". The warning
// says so, and without a device the direct pass then explains that it needs
// one.
TEST_CASE("retired pass names are aliases for direct", "[unit]") {
  for (const char* old : {"ngx", "reshade"}) {
    std::vector<std::string> warnings;
    auto pass = MakeNeuralPass(old, warnings);
    REQUIRE(pass != nullptr);
    REQUIRE(std::string(pass->Name()) == "passthrough");
    REQUIRE(warnings.size() == 2);
    REQUIRE(warnings[0].find("retired") != std::string::npos);
    REQUIRE(warnings[1].find("device") != std::string::npos);
  }
}

TEST_CASE("the direct pass reports that it needs a device", "[unit]") {
  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("direct", warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(std::string(pass->Name()) == "passthrough");
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("direct") != std::string::npos);
  REQUIRE(warnings[0].find("device") != std::string::npos);
}

// The direct pass with a device but nothing beside it: it must say which piece
// is missing, because the runtime, the forwarder and the NGX core are three
// different things to go and fetch.
TEST_CASE("the direct pass explains itself when its pieces are missing", "[device]") {
  const auto gpu = DetectPrimaryGpu();
  if (!gpu) { SUCCEED("no NVIDIA adapter"); return; }
  auto bridge = DeviceBridge::Create(gpu->luid, 256, 256);
  REQUIRE(bridge != nullptr);

  NeuralPassContext ctx;
  ctx.device = bridge->D3d12();
  ctx.runtimeDir = std::filesystem::temp_directory_path() / "dlss5-sidecar-no-runtime-here";
  ctx.width = 256;
  ctx.height = 256;
  ctx.arch = gpu->arch;

  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("direct", ctx, warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(std::string(pass->Name()) == "passthrough");
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("nvngx_dlssnr.dll") != std::string::npos);
}

// With a device but no runtime beside it, the failure has to name a reason an
// operator can act on -- this is the path anyone without the optional runtime
// takes, so it is the one worth pinning.
TEST_CASE("the reshade pass explains itself when the runtime is missing",
          "[device]") {
  const auto gpu = DetectPrimaryGpu();
  if (!gpu) { SUCCEED("no NVIDIA adapter"); return; }
  auto bridge = DeviceBridge::Create(gpu->luid, 256, 256);
  REQUIRE(bridge != nullptr);

  NeuralPassContext ctx;
  ctx.device = bridge->D3d12();
  ctx.runtimeDir = std::filesystem::current_path();
  ctx.width = 256;
  ctx.height = 256;

  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("reshade", ctx, warnings);
  REQUIRE(pass != nullptr);
  INFO("warning: " << (warnings.empty() ? std::string("<none>") : warnings[0]));
  // Either it came up -- a runtime really is present -- or it explained why not.
  if (std::string(pass->Name()) == "passthrough") {
    REQUIRE(warnings.size() == 1);
    REQUIRE(warnings[0].find("reshade") != std::string::npos);
    REQUIRE(warnings[0].length() > std::string("neural_pass \"reshade\"").length());
  } else {
    REQUIRE(warnings.empty());
  }
}

TEST_CASE("an unknown pass name never returns null", "[unit]") {
  // Spec section 11: a bad neural runtime degrades to passthrough rather than
  // refusing to start. A typo must not cost the operator their overlay.
  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("dlss6-ultra", warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(std::string(pass->Name()) == "passthrough");
  REQUIRE(warnings.size() == 1);
  REQUIRE(warnings[0].find("dlss6-ultra") != std::string::npos);
}

TEST_CASE("an empty pass name still yields a usable pass", "[unit]") {
  std::vector<std::string> warnings;
  auto pass = MakeNeuralPass("", warnings);
  REQUIRE(pass != nullptr);
  REQUIRE(warnings.size() == 1);
}
