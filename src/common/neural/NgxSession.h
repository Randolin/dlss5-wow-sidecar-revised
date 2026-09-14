#pragma once
#include <d3d12.h>

#include <filesystem>
#include <memory>
#include <string>

namespace sidecar {

// The NGX core, brought up on the render device.
//
// The direct neural-rendering path needs two things from the driver's NGX
// core: that it is initialised on our device, and its capability parameter
// block -- the block carries the snippet and preset callbacks a feature expects
// at create time, which a freshly allocated block does not. The neural runtime
// itself is then driven through the forwarder (neural/NrForwarder.h), never
// through the core's feature API.
//
// Built only when the DLSS SDK headers are available at configure time. Without
// them this compiles to a stub whose Create returns a session that explains why
// it is unavailable, exactly as NvofaFlow does without the Optical Flow SDK.
//
// Linking the NGX SDK library adds no DLL import: it resolves _nvngx.dll with
// LoadLibrary at first use, which was verified against the import-table checker.
class NgxSession {
 public:
  // runtimeDir is where nvngx_*.dll live -- normally next to the executable.
  // It is resolved to an absolute path before NGX sees it: a relative path is
  // accepted silently and then finds nothing, which surfaces as "unavailable"
  // and is indistinguishable from an unsupported driver.
  static std::unique_ptr<NgxSession> Create(ID3D12Device* device,
                                            const std::filesystem::path& runtimeDir);
  ~NgxSession();

  NgxSession(const NgxSession&) = delete;
  NgxSession& operator=(const NgxSession&) = delete;

  bool Available() const;

  // Whether the driver reports DLSS usable. The neural-rendering runtime
  // builds a DLSS feature of its own for its temporal pass, so nvngx_dlss.dll
  // has to be beside the sidecar even though this project never creates one.
  bool DlssSupported() const { return dlssSupported_; }

  // Why the session is degraded, or empty when it is not. Suitable for a log.
  const std::string& UnavailableReason() const { return unavailableReason_; }

  // The driver core's own capability parameter block, cached after the first
  // request. Null when the session is unavailable. Opaque here so this header
  // never needs the NGX types.
  void* CapabilityParameters();
  ID3D12Device* Device() const { return device_; }

 private:
  NgxSession() = default;

  ID3D12Device* device_ = nullptr;
  bool initialised_ = false;
  bool dlssSupported_ = false;
  std::string unavailableReason_;
  void* capability_ = nullptr;
};

}  // namespace sidecar
