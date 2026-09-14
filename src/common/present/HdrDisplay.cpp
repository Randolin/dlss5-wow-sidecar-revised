#include "present/HdrDisplay.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cwchar>
#include <optional>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sidecar {
namespace {

// The DXGI output whose monitor matches the window's.
std::optional<DXGI_OUTPUT_DESC1> OutputForWindow(HWND window) {
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return std::nullopt;

  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      ComPtr<IDXGIOutput6> output6;
      if (FAILED(output.As(&output6))) continue;
      DXGI_OUTPUT_DESC1 desc{};
      if (FAILED(output6->GetDesc1(&desc))) continue;
      if (desc.Monitor == monitor) return desc;
    }
  }
  return std::nullopt;
}

}  // namespace

bool DisplayIsHdr(HWND window) {
  const auto desc = OutputForWindow(window);
  if (!desc) return false;
  // The compositor is in HDR when the output's colour space is the PQ one;
  // an SDR desktop reports the sRGB space whatever the panel is capable of.
  return desc->ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

float MaxLuminanceNits(HWND window) {
  const auto desc = OutputForWindow(window);
  if (!desc) return 0.0f;
  return desc->MaxLuminance;
}

float SdrWhiteLevelNits(HWND window) {
  // Not in DXGI_OUTPUT_DESC1; it comes from the display configuration API.
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return 0.0f;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return 0.0f;

  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
    return 0.0f;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return 0.0f;
  }
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    if (wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;

    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = paths[i].targetInfo.adapterId;
    white.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) continue;
    // Reported in units where 1000 == 80 nits.
    return static_cast<float>(white.SDRWhiteLevel) * 80.0f / 1000.0f;
  }
  return 0.0f;
}

}  // namespace sidecar
