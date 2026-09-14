#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace sidecar {

// How the model's answer is composed back into the frame, and (experimental)
// how the frame may be encoded for it. See NrCompose.hlsl, NrEncode.hlsl and
// NrResolve.hlsl.
struct NrBridgeParams {
  // The experimental HDR encode/resolve pair. Off: the model is shown the
  // frame exactly as captured, which is the DLSS family's LDR convention and
  // the path that works.
  bool enabled = false;
  float paperWhiteNits = 203.0f;
  float headroom = 2.0f;
  // Blend from the untouched frame (0) to the composed result (1), in linear
  // light.
  float strength = 1.0f;
  // How much of the original frame's colour to keep: 0 shows the model's own
  // (desaturated) colour, 1 keeps the original's hue and chroma and takes only
  // the light from the model.
  float colourPreserve = 1.0f;
  // How much the model's brightening is held back near display white, plus a
  // soft knee before clipping. 0 is the pure composition; 1 stops the model
  // brightening anything that is already bright.
  float highlightProtect = 0.6f;
  // A/B split: fraction of the frame's width, from the left, left untouched.
  // 0 is off.
  float split = 0.0f;
};

// The compute passes around the model. Every pass samples its sources and
// writes a destination of the size it is told, so the model can work below the
// capture resolution: the compose reads the full-res frame and the model's
// smaller output together, and a resample of the frame to the model's size is
// the same shader at strength zero.
//
// The caller owns every texture and the state transitions around each Record
// call: sources in NON_PIXEL_SHADER_RESOURCE, destination in UNORDERED_ACCESS.
// `slot` picks a descriptor set; passes recorded into one command list must
// use distinct slots so their views are not overwritten before the GPU reads
// them.
class NrColorBridge {
 public:
  static constexpr uint32_t kSlots = 8;

  static std::unique_ptr<NrColorBridge> Create(ID3D12Device* device);

  // frame (sRGB, any size) + shown/model (sRGB, the model's size) -> dst (sRGB,
  // dst size). The model's edit is taken as a ratio at its own size and applied
  // to the frame, so the frame's detail survives whatever size the model ran at.
  void RecordCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* frame,
                     ID3D12Resource* shown, ID3D12Resource* model, ID3D12Resource* dst,
                     uint32_t dstWidth, uint32_t dstHeight, const NrBridgeParams& params,
                     uint32_t slot);

  // frame -> dst at the destination's size, bilinear. The frame as the model's
  // input when the model works below capture resolution.
  void RecordResample(ID3D12GraphicsCommandList* cl, ID3D12Resource* frame,
                      ID3D12Resource* dst, uint32_t dstWidth, uint32_t dstHeight, uint32_t slot);

  // Experimental HDR path.
  void RecordEncode(ID3D12GraphicsCommandList* cl, ID3D12Resource* frame, ID3D12Resource* dst,
                    uint32_t dstWidth, uint32_t dstHeight, const NrBridgeParams& params,
                    uint32_t slot);
  void RecordResolve(ID3D12GraphicsCommandList* cl, ID3D12Resource* frame,
                     ID3D12Resource* model, ID3D12Resource* dst, uint32_t dstWidth,
                     uint32_t dstHeight, const NrBridgeParams& params, uint32_t slot);

 private:
  NrColorBridge() = default;

  void Record(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso, uint32_t slot,
              ID3D12Resource* srv0, ID3D12Resource* srv1, ID3D12Resource* srv2,
              ID3D12Resource* uav, uint32_t dstWidth, uint32_t dstHeight,
              const NrBridgeParams& params);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> encode_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  UINT descriptorSize_ = 0;
};

}  // namespace sidecar
