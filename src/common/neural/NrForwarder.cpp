#include "neural/NrForwarder.h"

namespace sidecar {
namespace {

constexpr const wchar_t* kForwarderName = L"nvngx.dll_sidecar.dll";

// SEH and C++ objects cannot share a frame under /EHsc, so each guarded call
// lives in a function that touches nothing but PODs.
int GuardedInit(NrForwarder::PfnInit fn, const wchar_t* runtime, const wchar_t* data,
                unsigned long long appId, ID3D12Device* device, void* params, DWORD* seh) {
  *seh = 0;
  __try {
    return fn(runtime, data, appId, device, params);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *seh = GetExceptionCode();
    return SIDECAR_NR_ERR_RUNTIME_NOT_LOADED;
  }
}

int GuardedCreate(NrForwarder::PfnCreate fn, ID3D12GraphicsCommandList* cl, void* params,
                  unsigned int w, unsigned int h, const SidecarNrTuning* tuning,
                  void** handle, DWORD* seh) {
  *seh = 0;
  __try {
    return fn(cl, params, w, h, tuning, handle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *seh = GetExceptionCode();
    return SIDECAR_NR_ERR_BAD_ARGUMENT;
  }
}

int GuardedEvaluate(NrForwarder::PfnEvaluate fn, ID3D12GraphicsCommandList* cl, void* handle,
                    void* params, const SidecarNrEval* eval, const SidecarNrTuning* tuning,
                    DWORD* seh) {
  *seh = 0;
  __try {
    return fn(cl, handle, params, eval, tuning);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *seh = GetExceptionCode();
    return SIDECAR_NR_ERR_BAD_ARGUMENT;
  }
}

int GuardedRelease(NrForwarder::PfnRelease fn, void* handle, DWORD* seh) {
  *seh = 0;
  __try {
    return fn(handle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *seh = GetExceptionCode();
    return SIDECAR_NR_ERR_BAD_ARGUMENT;
  }
}

int GuardedProbe(NrForwarder::PfnProbe fn, void* params) {
  __try {
    return fn(params);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

}  // namespace

std::unique_ptr<NrForwarder> NrForwarder::Load(const std::filesystem::path& dir,
                                               std::string& reason) {
  reason.clear();
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(dir / kForwarderName, ec);
  if (!std::filesystem::exists(path, ec) || ec) {
    reason = "nvngx.dll_sidecar.dll is not beside the sidecar";
    return nullptr;
  }

  std::unique_ptr<NrForwarder> f(new NrForwarder());
  f->module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!f->module_) {
    reason = "nvngx.dll_sidecar.dll would not load (error " +
             std::to_string(GetLastError()) + ")";
    return nullptr;
  }

  auto abi = reinterpret_cast<PfnAbi>(GetProcAddress(f->module_, "sidecar_nr_abi_version"));
  f->probe_ = reinterpret_cast<PfnProbe>(GetProcAddress(f->module_, "sidecar_nr_probe_float_slot"));
  f->init_ = reinterpret_cast<PfnInit>(GetProcAddress(f->module_, "sidecar_nr_init"));
  f->create_ = reinterpret_cast<PfnCreate>(GetProcAddress(f->module_, "sidecar_nr_create"));
  f->evaluate_ = reinterpret_cast<PfnEvaluate>(GetProcAddress(f->module_, "sidecar_nr_evaluate"));
  f->release_ = reinterpret_cast<PfnRelease>(GetProcAddress(f->module_, "sidecar_nr_release"));
  if (!abi || !f->probe_ || !f->init_ || !f->create_ || !f->evaluate_ || !f->release_) {
    reason = "nvngx.dll_sidecar.dll is missing exports; it is not this build's forwarder";
    return nullptr;
  }
  if (abi() != SIDECAR_NR_ABI_VERSION) {
    reason = "nvngx.dll_sidecar.dll is ABI version " + std::to_string(abi()) +
             " but this sidecar expects " + std::to_string(SIDECAR_NR_ABI_VERSION);
    return nullptr;
  }
  return f;
}

NrForwarder::~NrForwarder() {
  // Deliberately never freed. The runtime it loaded has feature state and GPU
  // work that may still be in flight, and unloading a module that a driver
  // holds callbacks into is a reliable way to crash on exit. The process is
  // ending or the pass is being rebuilt; either way the OS reclaims it.
}

int NrForwarder::ProbeFloatSlot(void* params) { return GuardedProbe(probe_, params); }

int NrForwarder::Init(const std::filesystem::path& runtimePath,
                      const std::filesystem::path& dataPath, unsigned long long appId,
                      ID3D12Device* device, void* params, DWORD& seh) {
  const std::wstring runtime = runtimePath.wstring();
  const std::wstring data = dataPath.wstring();
  return GuardedInit(init_, runtime.c_str(), data.c_str(), appId, device, params, &seh);
}

int NrForwarder::Create(ID3D12GraphicsCommandList* cl, void* params, unsigned int width,
                        unsigned int height, const SidecarNrTuning& tuning, void*& handle,
                        DWORD& seh) {
  void* out = nullptr;
  const int r = GuardedCreate(create_, cl, params, width, height, &tuning, &out, &seh);
  handle = out;
  return r;
}

int NrForwarder::Evaluate(ID3D12GraphicsCommandList* cl, void* handle, void* params,
                          const SidecarNrEval& eval, const SidecarNrTuning& tuning, DWORD& seh) {
  return GuardedEvaluate(evaluate_, cl, handle, params, &eval, &tuning, &seh);
}

int NrForwarder::Release(void* handle, DWORD& seh) {
  return GuardedRelease(release_, handle, &seh);
}

const char* NrForwarder::ResultName(int result) {
  switch (result) {
    case 1: return "Success";
    case SIDECAR_NR_ERR_RUNTIME_NOT_LOADED: return "forwarder: runtime not loaded";
    case SIDECAR_NR_ERR_BAD_ARGUMENT: return "forwarder: bad argument";
    case SIDECAR_NR_ERR_NOT_INITIALISED: return "forwarder: not initialised";
    // NVSDK_NGX_Result values, spelled out because the SDK header may not be
    // present at configure time and these are the ones that show up.
    case static_cast<int>(0xBAD00000): return "Fail";
    case static_cast<int>(0xBAD00001): return "FeatureNotSupported";
    case static_cast<int>(0xBAD00002): return "PlatformError";
    case static_cast<int>(0xBAD00003): return "FeatureAlreadyExists";
    case static_cast<int>(0xBAD00004): return "FeatureNotFound";
    case static_cast<int>(0xBAD00005): return "InvalidParameter";
    case static_cast<int>(0xBAD00006): return "ScratchBufferTooSmall";
    case static_cast<int>(0xBAD00007): return "NotInitialized";
    case static_cast<int>(0xBAD00008): return "UnsupportedInputFormat";
    case static_cast<int>(0xBAD00009): return "RWFlagMissing";
    case static_cast<int>(0xBAD0000A): return "MissingInput";
    case static_cast<int>(0xBAD0000B): return "UnableToInitializeFeature";
    case static_cast<int>(0xBAD0000C): return "OutOfDate";
    case static_cast<int>(0xBAD0000D): return "OutOfGPUMemory";
    case static_cast<int>(0xBAD0000E): return "UnsupportedFormat";
    case static_cast<int>(0xBAD0000F): return "UnableToWriteToAppDataPath";
    case static_cast<int>(0xBAD00010): return "UnsupportedParameter";
    case static_cast<int>(0xBAD00011): return "Denied";
    case static_cast<int>(0xBAD00012): return "NotImplemented";
    default: return "unknown result";
  }
}

}  // namespace sidecar
