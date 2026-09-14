#include "manager/Probes.h"

#include "core/Sha256.h"
#include "neural/RuntimeManifest.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <system_error>

#include "core/GpuProfile.h"
#include "core/Text.h"
#include "present/WindowTracker.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace sidecar {

ProbeResult ProbeGpu() {
  ProbeResult r;
  r.title = "Graphics adapter";

  const auto gpu = DetectPrimaryGpu();
  if (!gpu) {
    r.state = ProbeState::Fail;
    r.detail = "No NVIDIA adapter found.";
    r.remedy = "This sidecar needs an NVIDIA RTX 40 or RTX 50 card.";
    return r;
  }

  // Adapter descriptions are ASCII in practice; narrow explicitly so the
  // conversion is deliberate rather than a warning.
  for (const wchar_t c : gpu->name) r.detail.push_back(c < 128 ? static_cast<char>(c) : '?');
  r.detail += " - ";
  r.detail += ToString(gpu->arch);

  if (gpu->arch == GpuArch::Ada || gpu->arch == GpuArch::Blackwell) {
    r.state = ProbeState::Ok;
    return r;
  }
  r.state = ProbeState::Fail;
  r.remedy = "RTX 40 (Ada) or RTX 50 (Blackwell) is required. Older cards are "
             "refused rather than run badly.";
  return r;
}

ProbeResult ProbeDriver() {
  ProbeResult r;
  r.title = "Display driver";

  const auto gpu = DetectPrimaryGpu();
  if (!gpu) {
    r.state = ProbeState::Fail;
    r.detail = "No NVIDIA adapter to query.";
    r.remedy = "Install an NVIDIA RTX 40 or RTX 50 card.";
    return r;
  }

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  LARGE_INTEGER umd{};
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->EnumAdapterByLuid(gpu->luid, IID_PPV_ARGS(&adapter))) &&
      SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    // The NVIDIA user-mode driver version encodes the public driver number in
    // its low digits, which is enough to show the operator what they are on.
    const uint64_t version = static_cast<uint64_t>(umd.QuadPart);
    const unsigned product = static_cast<unsigned>((version >> 16) & 0xFFFF);
    const unsigned build = static_cast<unsigned>(version & 0xFFFF);
    r.detail = "User-mode driver " + std::to_string(product) + "." + std::to_string(build);
    r.state = ProbeState::Ok;
    return r;
  }

  r.state = ProbeState::Warn;
  r.detail = "Could not read the driver version.";
  r.remedy = "Not fatal. Update to a current NVIDIA driver if capture misbehaves.";
  return r;
}

ProbeResult ProbeWindows() {
  ProbeResult r;
  r.title = "Windows version";

  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  bool queried = false;
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    // GetVersionEx lies unless the binary carries a compatibility manifest;
    // RtlGetVersion reports the real build.
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn && fn(&info) == 0) queried = true;
  }

  if (!queried) {
    r.state = ProbeState::Warn;
    r.detail = "Could not read the Windows build number.";
    r.remedy = "Not fatal, but this project is only supported on Windows 11.";
    return r;
  }

  r.detail = "Build " + std::to_string(info.dwBuildNumber);
  if (info.dwBuildNumber >= 22000) {
    r.state = ProbeState::Ok;
    return r;
  }
  r.state = ProbeState::Fail;
  r.remedy = "Windows 11 is required: the overlay depends on compositor "
             "behaviour that Windows 10 does not provide.";
  return r;
}

ProbeResult ProbeRefreshRate() {
  ProbeResult r;
  r.title = "Display refresh rate";

  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
    r.state = ProbeState::Warn;
    r.detail = "Could not read the current display mode.";
    r.remedy = "Not fatal. Check your monitor settings if pacing looks wrong.";
    return r;
  }

  r.detail = std::to_string(mode.dmDisplayFrequency) + " Hz";
  if (mode.dmDisplayFrequency >= 120) {
    r.state = ProbeState::Ok;
    return r;
  }
  r.state = ProbeState::Warn;
  r.remedy = "Below 120 Hz the overlay's added latency is a larger share of the "
             "frame. It will still run.";
  return r;
}

ProbeResult ProbeNeuralRuntime(const fs::path& sidecarDir) {
  ProbeResult r;
  r.title = "Neural runtime";

  std::error_code ec;
  const fs::path dll = sidecarDir / "nvngx_dlssnr.dll";
  if (!fs::exists(dll, ec) || ec) {
    r.state = ProbeState::Warn;
    r.detail = "nvngx_dlssnr.dll not found next to the sidecar.";
    r.remedy = "Optional until the neural pass ships. Without it the pipeline "
               "runs the passthrough pass.";
    return r;
  }

  // Hash it rather than trust the filename. A wrong or truncated binary here
  // becomes an access violation inside NGX at feature creation (spec §10), and
  // a digest is the only thing that distinguishes it from a good one beforehand.
  const auto gpu = DetectPrimaryGpu();
  return NeuralRuntimeVerdict(dll.filename().string(), Sha256File(dll),
                              gpu ? gpu->arch : GpuArch::Unsupported);
}

ProbeResult NeuralRuntimeVerdict(std::string_view fileName,
                                 std::string_view sha256Hex, GpuArch arch) {
  ProbeResult r;
  r.title = "Neural runtime";

  if (sha256Hex.empty()) {
    r.state = ProbeState::Warn;
    r.detail = std::string(fileName) + " is present but could not be read.";
    r.remedy = "Check the file is not locked by another process, and that the "
               "sidecar has permission to read it.";
    return r;
  }

  r.detail = DescribeRuntime(fileName, sha256Hex);
  if (const auto entry = LookupRuntime(sha256Hex)) {
    // Recognising the build is only half the question. The stock runtime is
    // built for Blackwell and fails on Ada at feature creation with a bare
    // error code, so saying so here is what turns that into a diagnosis.
    const std::string mismatch = DescribeCompatibility(arch, entry->variant);
    if (mismatch.empty()) {
      r.state = ProbeState::Ok;
      return r;
    }
    r.state = ProbeState::Warn;
    r.detail += " " + mismatch;
    r.remedy = "Supply a runtime built for this GPU, or the neural pass will "
               "fall back to passthrough.";
    return r;
  }

  // Amber, not red. A newer runtime than this manifest knows is a legitimate
  // thing for an operator to have, and refusing it outright would age badly.
  r.state = ProbeState::Warn;
  r.remedy = "The pass will still try this build. If it fails, quote the "
             "SHA-256 above when reporting it.";
  return r;
}

ProbeResult ProbeNrForwarder(const fs::path& sidecarDir) {
  ProbeResult r;
  r.title = "Neural forwarder";

  // Built with the sidecar and copied beside it. The direct route calls the
  // neural runtime through it -- the runtime only takes calls from a module
  // whose path contains "nvngx.dll", and this is the module named for that.
  std::error_code ec;
  const fs::path dll = sidecarDir / "nvngx.dll_sidecar.dll";
  if (fs::exists(dll, ec) && !ec) {
    r.state = ProbeState::Ok;
    r.detail = "nvngx.dll_sidecar.dll present next to the sidecar.";
    return r;
  }
  r.state = ProbeState::Warn;
  r.detail = "nvngx.dll_sidecar.dll not found next to the sidecar.";
  r.remedy = "It is built alongside sidecar.exe and lands in the same output "
             "folder; copy it here. Without it the direct neural pass falls back "
             "to passthrough.";
  return r;
}

ProbeResult ProbeAppWindow(const TargetAppSettings& app) {
  ProbeResult r;
  r.title = app.name.empty() ? std::string("Capture target") : app.name + " window";

  if (!app.Chosen()) {
    r.state = ProbeState::Warn;
    r.detail = "No capture target chosen.";
    r.remedy = "Pick a window on the Status page. Any borderless window the compositor "
               "can see will do -- a game, an emulator, a video player.";
    return r;
  }

  const AppMatch match{Utf8ToWide(app.windowClass), Utf8ToWide(app.title)};
  const auto window = FindAppWindow(match);
  if (!window) {
    r.state = ProbeState::Warn;
    r.detail = (app.name.empty() ? std::string("The chosen window") : app.name) +
               " is not running.";
    r.remedy = "Start it, then run the probes again.";
    return r;
  }

  const auto width = window->clientScreen.right - window->clientScreen.left;
  const auto height = window->clientScreen.bottom - window->clientScreen.top;
  r.detail = std::to_string(width) + "x" + std::to_string(height);

  if (window->borderless) {
    r.state = ProbeState::Ok;
    r.detail += " borderless windowed";
    return r;
  }
  r.state = ProbeState::Fail;
  r.detail += " windowed with a border, or exclusive fullscreen";
  r.remedy = "Set the app to borderless windowed. Exclusive fullscreen has no "
             "compositor surface to capture and yields black frames; a bordered "
             "window leaves its title bar above the overlay.";
  return r;
}

std::vector<ProbeResult> RunAllProbes(const fs::path& sidecarDir, const TargetAppSettings& app) {
  return {
      ProbeGpu(),
      ProbeDriver(),
      ProbeWindows(),
      ProbeRefreshRate(),
      ProbeAppWindow(app),
      ProbeNeuralRuntime(sidecarDir),
      ProbeNrForwarder(sidecarDir),
  };
}

}  // namespace sidecar
