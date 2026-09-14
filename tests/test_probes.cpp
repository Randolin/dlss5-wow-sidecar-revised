#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include "core/Sha256.h"
#include "manager/Probes.h"
#include "neural/RuntimeManifest.h"

using namespace sidecar;

// The neural-runtime probe is not a presence check.
//
// These use the real filesystem because that is the thing under test -- a
// hash-verifying probe that never opens a file would pass its own tests and
// still be useless. Each writes into a directory it owns and cleans up.

namespace {

std::filesystem::path ProbeScratchDir(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "sidecar_probe_tests" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
}

}  // namespace

TEST_CASE("Neural runtime probe warns when the runtime is absent", "[unit]") {
  const auto dir = ProbeScratchDir("absent");
  const auto r = ProbeNeuralRuntime(dir);
  CHECK(r.state == ProbeState::Warn);
  CHECK(r.remedy.empty() == false);
  std::filesystem::remove_all(dir);
}

// An unrecognised digest is amber, never red: the operator may legitimately
// have a newer build than the manifest knows. But the message has to name the
// digest, or they cannot tell us which build it was.
TEST_CASE("Neural runtime probe reports an unknown build as amber with its digest",
          "[unit]") {
  const auto dir = ProbeScratchDir("unknown");
  WriteFile(dir / "nvngx_dlssnr.dll", "not really a runtime");
  const auto r = ProbeNeuralRuntime(dir);
  CHECK(r.state == ProbeState::Warn);
  CHECK(r.remedy.empty() == false);
  // "not really a runtime" hashed with SHA-256.
  CHECK(r.detail.find(Sha256Hex("not really a runtime")) != std::string::npos);
  std::filesystem::remove_all(dir);
}

// The recognised path, tested through the pure verdict. It cannot be reached by
// writing a file, because that would mean producing input that hashes to a
// manifest entry.
TEST_CASE("Neural runtime verdict accepts a known build and names its version",
          "[unit]") {
  const auto r = NeuralRuntimeVerdict(
      "nvngx_dlssnr.dll",
      "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
      GpuArch::Blackwell);
  CHECK(r.state == ProbeState::Ok);
  CHECK(r.detail.find("310.8.0") != std::string::npos);
  // An Ok row needs no remedy; there is nothing to remedy.
  CHECK(r.remedy.empty());
}

TEST_CASE("Neural runtime verdict warns, with the digest, on an unknown build",
          "[unit]") {
  const std::string digest(64, 'c');
  const auto r = NeuralRuntimeVerdict("nvngx_dlssnr.dll", digest, GpuArch::Blackwell);
  CHECK(r.state == ProbeState::Warn);
  CHECK(r.detail.find(digest) != std::string::npos);
  CHECK(r.remedy.empty() == false);
}

TEST_CASE("Neural runtime verdict treats an unreadable file as its own case",
          "[unit]") {
  const auto r = NeuralRuntimeVerdict("nvngx_dlssnr.dll", "", GpuArch::Blackwell);
  CHECK(r.state == ProbeState::Warn);
  CHECK(r.detail.find("could not be read") != std::string::npos);
}

// Opt-in, because it needs a real ~166 MB runtime that is never committed.
// Point SIDECAR_TEST_RUNTIME_DIR at a directory holding nvngx_dlssnr.dll to
// check the probe end to end -- real hash, real manifest lookup, real detected
// GPU -- rather than only through the pure verdict above.
//
//   set SIDECAR_TEST_RUNTIME_DIR=D:\path\to\runtimes
TEST_CASE("Neural runtime probe diagnoses a real runtime against this GPU",
          "[device]") {
  size_t len = 0;
  char raw[1024] = {};
  if (getenv_s(&len, raw, sizeof(raw), "SIDECAR_TEST_RUNTIME_DIR") != 0 || len == 0) {
    SUCCEED("SIDECAR_TEST_RUNTIME_DIR not set");
    return;
  }

  const auto r = ProbeNeuralRuntime(std::filesystem::path(raw));
  INFO("state:  " << static_cast<int>(r.state));
  INFO("detail: " << r.detail);
  INFO("remedy: " << r.remedy);

  // Whatever the answer, it must be a real one: never silently Ok on a runtime
  // this card cannot run, and never a bare failure with nothing to act on.
  CHECK(r.detail.empty() == false);
  if (r.state != ProbeState::Ok) CHECK(r.remedy.empty() == false);

  const auto gpu = DetectPrimaryGpu();
  const std::string digest = Sha256File(std::filesystem::path(raw) / "nvngx_dlssnr.dll");
  if (gpu && !digest.empty()) {
    if (const auto entry = LookupRuntime(digest)) {
      // A recognised runtime that cannot run here must be reported as such.
      const bool runnable =
          CheckRuntimeCompatibility(gpu->arch, entry->variant) ==
          RuntimeCompatibility::Ok;
      CHECK((r.state == ProbeState::Ok) == runnable);
    }
  }
}

TEST_CASE("every probe result carries a remedy when it is not Ok", "[unit]") {
  const auto results = RunAllProbes("C:/Tools/dlss5-sidecar", TargetAppSettings{});
  REQUIRE(results.empty() == false);
  for (const auto& r : results) {
    INFO("probe: " << r.title);
    REQUIRE(r.title.empty() == false);
    if (r.state != ProbeState::Ok) REQUIRE(r.remedy.empty() == false);
  }
}

TEST_CASE("the app window probe names the app", "[unit]") {
  TargetAppSettings app;
  app.name = "Some Game";
  app.windowClass = "NoSuchWindowClassAnywhere";
  const auto r = ProbeAppWindow(app);
  REQUIRE(r.title.find("Some Game") != std::string::npos);
  // Not running is a warning with a remedy, never a blocking failure.
  REQUIRE(r.state == ProbeState::Warn);
  REQUIRE(r.remedy.empty() == false);
}
