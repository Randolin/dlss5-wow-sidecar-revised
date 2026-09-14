#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace sidecar {

// The two passes that let an HDR capture go through an LDR model.
//
// ToSdr: the scRGB frame becomes the sRGB-encoded SDR view the model is shown
// (and the luminance pass reads). Compose: the model's SDR result is expressed
// as a ratio against that view and applied to the original HDR frame.
//
// The caller owns the textures and the state transitions: sources in
// NON_PIXEL_SHADER_RESOURCE, destinations in UNORDERED_ACCESS. Two descriptor
// slots, one per pass, since both are recorded into one command list.
class HdrBridge {
 public:
  struct Params {
    float paperWhiteNits = 203.0f;   // SDR reference white
    float headroom = 4.0f;           // multiples of paper white the model's view folds in
    float maxGain = 4.0f;            // cap on the compose's brightening
    float split = 0.0f;              // A/B: fraction of the width left untouched
  };

  static std::unique_ptr<HdrBridge> Create(ID3D12Device* device, uint32_t width,
                                           uint32_t height);

  void RecordToSdr(ID3D12GraphicsCommandList* cl, ID3D12Resource* hdr, ID3D12Resource* sdr,
                   const Params& params);
  void RecordCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* hdr,
                     ID3D12Resource* sdrShown, ID3D12Resource* sdrResult, ID3D12Resource* dst,
                     const Params& params);

 private:
  HdrBridge() = default;

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> toSdr_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  UINT descriptorSize_ = 0;
};

}  // namespace sidecar
