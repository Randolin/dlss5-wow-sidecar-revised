#pragma once
#include <windows.h>
#include <d3d12.h>

#include <filesystem>
#include <memory>
#include <string>

#include "forwarder/nvngx_forwarder_abi.h"

namespace sidecar {

// The host's handle on nvngx.dll_sidecar.dll.
//
// Every entry point is resolved by name at runtime. The forwarder has no import
// library on purpose: an import would make the sidecar refuse to start when
// the DLL is missing, and a missing forwarder should degrade to passthrough
// with a sentence, like every other missing runtime piece (spec section 11).
//
// Every call into the runtime is wrapped in SEH. The neural-rendering runtime
// is a closed vendor binary being driven by reverse-engineered contract; when
// it faults, the process must not go with it, and the command list it was
// recording into must be discarded rather than submitted.
class NrForwarder {
 public:
  // Looks for nvngx.dll_sidecar.dll in `dir`. Null with a reason when it is
  // absent, will not load, or is a different ABI version.
  static std::unique_ptr<NrForwarder> Load(const std::filesystem::path& dir,
                                           std::string& reason);
  ~NrForwarder();

  // Finds which vtable slot the driver's parameter block keeps floats in. Must
  // run before any create. Returns the slot, or -1 if none round-trips.
  int ProbeFloatSlot(void* params);

  // Result of the call, or a SIDECAR_NR_ERR_* value. `seh` receives a
  // structured-exception code if the runtime faulted, and 0 otherwise.
  int Init(const std::filesystem::path& runtimePath, const std::filesystem::path& dataPath,
           unsigned long long appId, ID3D12Device* device, void* params, DWORD& seh);
  int Create(ID3D12GraphicsCommandList* cl, void* params, unsigned int width,
             unsigned int height, const SidecarNrTuning& tuning, void*& handle, DWORD& seh);
  int Evaluate(ID3D12GraphicsCommandList* cl, void* handle, void* params,
               const SidecarNrEval& eval, const SidecarNrTuning& tuning, DWORD& seh);
  int Release(void* handle, DWORD& seh);

  static const char* ResultName(int result);

  // Public because the SEH trampolines in the .cpp are free functions.
  using PfnAbi = int(__cdecl*)();
  using PfnProbe = int(__cdecl*)(void*);
  using PfnInit = int(__cdecl*)(const wchar_t*, const wchar_t*, unsigned long long,
                                ID3D12Device*, void*);
  using PfnCreate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, unsigned int,
                                  unsigned int, const SidecarNrTuning*, void**);
  using PfnEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*,
                                    const SidecarNrEval*, const SidecarNrTuning*);
  using PfnRelease = int(__cdecl*)(void*);

 private:
  NrForwarder() = default;

  HMODULE module_ = nullptr;
  PfnProbe probe_ = nullptr;
  PfnInit init_ = nullptr;
  PfnCreate create_ = nullptr;
  PfnEvaluate evaluate_ = nullptr;
  PfnRelease release_ = nullptr;
};

}  // namespace sidecar
