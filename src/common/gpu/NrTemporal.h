#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace sidecar {

// Temporal stabilisation of the model's edit on a still scene (see
// NrTemporal.hlsl). Owns its history textures; the caller owns the model's
// input/output and the state transitions around Record (inputs
// NON_PIXEL_SHADER_RESOURCE, the result target UNORDERED_ACCESS).
//
// Only created when the feature is on. Off means this object does not exist
// and the frame path is exactly what it was.
class NrTemporal {
 public:
  struct Params {
    float smoothing = 0.6f;      // 0-0.95: history weight for darkening
    bool spatial = true;         // the 3x3 guided blur
    float rejectLo = 0.02f;      // input difference where history starts losing trust
    float rejectHi = 0.10f;      // ... and where it is fully discarded
  };

  static std::unique_ptr<NrTemporal> Create(ID3D12Device* device, uint32_t width,
                                            uint32_t height);

  // shown/result -> resultOut. History is updated inside. `resultOut` is a
  // caller-owned RGBA16F UAV at the working size; the stabilised output goes
  // there and the caller composes from it instead of `result`.
  void Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* shown, ID3D12Resource* result,
              ID3D12Resource* resultOut, const Params& params);

  // Next Record starts fresh: after a pass swap or a resolution change.
  void Reset() { reset_ = true; }

 private:
  NrTemporal() = default;
  void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                  D3D12_RESOURCE_STATES b);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  // Two of each, ping-ponged: last frame's stabilised gain and last frame's
  // model input. All rest in UNORDERED_ACCESS.
  Microsoft::WRL::ComPtr<ID3D12Resource> gain_[2];
  Microsoft::WRL::ComPtr<ID3D12Resource> shown_[2];
  uint32_t current_ = 0;
  bool reset_ = true;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  UINT descriptorSize_ = 0;
};

}  // namespace sidecar
