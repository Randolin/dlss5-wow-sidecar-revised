#include <catch2/catch_test_macros.hpp>
#include <d3d12.h>
#include <wrl/client.h>

#include <filesystem>

#include "core/GpuProfile.h"
#include "gpu/DeviceBridge.h"
#include "neural/NgxSession.h"

using Microsoft::WRL::ComPtr;
using namespace sidecar;

// NgxSession brings up a vendor runtime on the render device, and it is built
// behind a compile-time gate, so the no-SDK build never executes a line of it.
// These tests assert the contract that holds whether or not a runtime is
// actually installed.

TEST_CASE("NgxSession refuses a null device", "[unit]") {
  REQUIRE(NgxSession::Create(nullptr, ".") == nullptr);
}

TEST_CASE("NgxSession always explains itself when unavailable", "[device]") {
  const auto gpu = DetectPrimaryGpu();
  if (!gpu) { SUCCEED("no NVIDIA adapter"); return; }
  auto bridge = DeviceBridge::Create(gpu->luid, 256, 256);
  REQUIRE(bridge != nullptr);

  // No runtimes are placed next to the test binary, so on a machine without
  // them this exercises the failure path -- the one every user without the
  // runtimes will hit.
  auto session = NgxSession::Create(bridge->D3d12(), std::filesystem::current_path());
  REQUIRE(session != nullptr);

  // Never silently unavailable: either it came up, or it says why in terms
  // an operator could act on.
  if (!session->Available() || !session->DlssSupported()) {
    INFO("reason: " << session->UnavailableReason());
    CHECK(session->UnavailableReason().empty() == false);
  }
  // An available session always has the core's capability block.
  if (session->Available()) CHECK(session->CapabilityParameters() != nullptr);
}

// Teardown is where an earlier vendor-runtime integration went wrong twice
// over. Constructing and destroying repeatedly is the cheapest way to catch
// the same class of mistake here.
TEST_CASE("NgxSession can be created and destroyed repeatedly", "[device]") {
  const auto gpu = DetectPrimaryGpu();
  if (!gpu) { SUCCEED("no NVIDIA adapter"); return; }
  auto bridge = DeviceBridge::Create(gpu->luid, 256, 256);
  REQUIRE(bridge != nullptr);
  for (int i = 0; i < 3; ++i) {
    INFO("iteration " << i);
    auto session = NgxSession::Create(bridge->D3d12(), std::filesystem::current_path());
    REQUIRE(session != nullptr);
  }
  SUCCEED();
}
