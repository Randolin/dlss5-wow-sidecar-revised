#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Config.h"
#include "core/GpuProfile.h"

namespace sidecar {

enum class ProbeState { Ok, Warn, Fail };

// A row on the manager's dependency board. A probe that is not Ok always
// carries a remedy: telling someone their system is wrong without telling them
// what to do about it is not a diagnostic.
struct ProbeResult {
  ProbeState state = ProbeState::Fail;
  std::string title;
  std::string detail;
  std::string remedy;
};

// Pure predicates, so what the probes decide is unit-testable without
// hardware anywhere near the test machine.

// The neural-runtime verdict, given a digest rather than a file. Split out
// because the recognised path cannot otherwise be tested: producing a file that
// hashes to a manifest entry would mean inverting SHA-256. An empty digest means
// the file was found but could not be read.
//
// arch is the card the runtime would have to run on. A recognised runtime that
// cannot run on this architecture is still amber, but it says so -- that pairing
// is the difference between a readable diagnostic and an unexplained failure
// deep inside NGX.
ProbeResult NeuralRuntimeVerdict(std::string_view fileName, std::string_view sha256Hex,
                                 GpuArch arch);

ProbeResult ProbeGpu();
ProbeResult ProbeDriver();
ProbeResult ProbeWindows();
ProbeResult ProbeRefreshRate();
ProbeResult ProbeNeuralRuntime(const std::filesystem::path& sidecarDir);
ProbeResult ProbeNrForwarder(const std::filesystem::path& sidecarDir);
// Whether the target application's window is up and borderless.
ProbeResult ProbeAppWindow(const TargetAppSettings& app);

std::vector<ProbeResult> RunAllProbes(const std::filesystem::path& sidecarDir,
                                      const TargetAppSettings& app);

}  // namespace sidecar
