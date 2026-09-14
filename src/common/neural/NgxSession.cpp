#include "neural/NgxSession.h"

#include <windows.h>

#include "core/Log.h"

#if SIDECAR_HAVE_NGX
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>
#endif

namespace sidecar {

#if SIDECAR_HAVE_NGX

namespace {

// This project is not a registered NVIDIA title, so it identifies itself by
// project id rather than application id. NGX rejects application id 0 on some
// drivers; the project-id path is what NVIDIA points unregistered callers at.
constexpr const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";

const char* ResultName(NVSDK_NGX_Result r) {
  switch (r) {
    case NVSDK_NGX_Result_Success:                        return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:       return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError:             return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:      return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:           return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter:          return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized:            return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:    return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:             return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput:              return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate:                 return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:            return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:         return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter:      return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied:                    return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented:            return "NotImplemented";
    default:                                              return "Fail";
  }
}

}  // namespace

std::unique_ptr<NgxSession> NgxSession::Create(ID3D12Device* device,
                                               const std::filesystem::path& runtimeDir) {
  if (!device) return nullptr;

  std::unique_ptr<NgxSession> s(new NgxSession());
  s->device_ = device;

  std::error_code ec;
  std::filesystem::path absoluteDir = std::filesystem::absolute(runtimeDir, ec);
  if (ec) absoluteDir = runtimeDir;
  const std::wstring dir = absoluteDir.wstring();

  const wchar_t* paths[1] = {dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo.Path = paths;
  common.PathListInfo.Length = 1;

  const NVSDK_NGX_Result init = NVSDK_NGX_D3D12_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(), device,
      &common, NVSDK_NGX_Version_API);
  if (init != NVSDK_NGX_Result_Success) {
    s->unavailableReason_ =
        std::string("NGX init failed: ") + ResultName(init) +
        ". Runtimes were looked for in " + absoluteDir.string() + ".";
    GlobalLog().Warn(s->unavailableReason_);
    return s;   // Available() == false, but the reason survives.
  }
  s->initialised_ = true;

  NVSDK_NGX_Parameter* caps = nullptr;
  if (NVSDK_NGX_D3D12_GetCapabilityParameters(&caps) == NVSDK_NGX_Result_Success && caps) {
    s->capability_ = caps;
    int available = 0;
    caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    s->dlssSupported_ = available != 0;
    if (!s->dlssSupported_) {
      int initResult = 0;
      int needsDriver = 0;
      caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
      caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
      s->unavailableReason_ =
          std::string("DLSS reports unavailable: ") +
          ResultName(static_cast<NVSDK_NGX_Result>(initResult)) +
          (needsDriver ? ". The driver is too old."
                       : ". Check nvngx_dlss.dll is beside the sidecar; the neural "
                         "runtime builds a DLSS feature of its own.");
      GlobalLog().Warn(s->unavailableReason_);
    }
  } else {
    s->unavailableReason_ = "NGX capability parameters unavailable.";
    GlobalLog().Warn(s->unavailableReason_);
  }
  return s;
}

NgxSession::~NgxSession() {
  if (initialised_ && device_) {
    NVSDK_NGX_D3D12_Shutdown1(device_);
    initialised_ = false;
  }
}

bool NgxSession::Available() const { return initialised_ && capability_ != nullptr; }

void* NgxSession::CapabilityParameters() {
  if (!initialised_) return nullptr;
  if (!capability_) {
    NVSDK_NGX_Parameter* caps = nullptr;
    if (NVSDK_NGX_D3D12_GetCapabilityParameters(&caps) == NVSDK_NGX_Result_Success) {
      capability_ = caps;
    }
  }
  return capability_;
}

#else  // !SIDECAR_HAVE_NGX

std::unique_ptr<NgxSession> NgxSession::Create(ID3D12Device* device,
                                               const std::filesystem::path& runtimeDir) {
  (void)runtimeDir;
  if (!device) return nullptr;
  std::unique_ptr<NgxSession> s(new NgxSession());
  s->unavailableReason_ =
      "Built without the NVIDIA DLSS SDK, so no NGX session is possible. "
      "Configure with -DDLSS_SDK_DIR= to enable it.";
  return s;
}

NgxSession::~NgxSession() = default;
bool NgxSession::Available() const { return false; }
void* NgxSession::CapabilityParameters() { return nullptr; }

#endif  // SIDECAR_HAVE_NGX

}  // namespace sidecar
