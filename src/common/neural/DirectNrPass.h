#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/GpuProfile.h"
#include "gpu/NrColorBridge.h"
#include "neural/INeuralPass.h"
#include "neural/NgxSession.h"
#include "neural/NrForwarder.h"
#include "neural/RuntimeManifest.h"

namespace sidecar {

// One pass's worth of model tuning. The model reads these when the feature is
// built, so every distinct tuning is its own feature, and a change means a
// rebuild rather than a parameter write.
struct NrPassTuning {
  uint32_t preset = 0;          // 0 leaves the choice to the model; 1-3 are its own
  uint32_t style = 0;           // 0 standard, 1 natural, 2 cinematic
  float intensity = 1.0f;       // 0-2
  float localStructure = 1.0f;  // 0-2
  float localTone = 1.0f;       // 0-2
  float skinStructure = -1.0f;  // -1 off, else 0-2
  bool autoMask = true;         // the model's own UI/text mask
  bool uiCorrection = true;     // the model's own UI handling, at its default
};

// The settings that need new features when they change: the passes and the
// size the model works at. Applied live by hot-swapping a generation.
struct NrPassSetup {
  std::vector<NrPassTuning> passes;   // empty means one pass at defaults
  float modelScale = 1.0f;            // 0.5-1.0 of the capture size
  bool chainComposed = true;
  // Run the last pass at full resolution while the earlier ones work at
  // modelScale: lighting from the cheap passes, fine structure from the
  // expensive one. Its input is the previous pass composed onto the full
  // frame, so chaining through the compose is implied for that link.
  bool finalFull = false;
};

// Route A, finally: DLSS 5 Neural Rendering driven by the sidecar itself.
//
// No ReShade, no add-on, no detours. The runtime is called through
// nvngx.dll_sidecar.dll -- a forwarder named to pass the runtime's caller
// check -- with the NGX core's own capability block. See the forwarder source
// for the finding this rests on, and docs/spikes/2026-09-13-direct-nr.md for
// how it was arrived at.
//
// Multipass is a loop: pass N's output is pass N+1's colour input, each pass
// with its own feature and its own tuning. The model works at a fraction of
// the capture size on a resampled frame; the compose brings its answer back
// against the full-size original.
//
// Everything the model needs per configuration -- features, working textures
// -- lives in a Generation. Live tuning builds the next generation beside the
// current one, creates its features on one frame while the current one still
// renders, and swaps on the frame after, so a slider never costs a frame of
// passthrough.
class DirectNrPass : public INeuralPass {
 public:
  struct Options {
    std::filesystem::path runtimeDir;
    uint32_t width = 0;
    uint32_t height = 0;
    GpuArch arch = GpuArch::Unsupported;
    float syntheticDepth = 0.5f;
    // A ground-plane gradient instead of the constant: near at the bottom of
    // the frame, far at the top. Crude, but a shape. And whether the runtime
    // should read near as the high value.
    bool depthGradient = false;
    bool depthInverted = false;
    NrPassSetup setup;
    NrBridgeParams bridge;
    // Ticks per second on the queue this pass records into, for the per-stage
    // timestamps. 0 disables them.
    uint64_t timestampFrequency = 0;
  };

  static std::unique_ptr<DirectNrPass> Create(ID3D12Device* device, const Options& options,
                                              std::string& reason);
  ~DirectNrPass() override;

  bool Evaluate(ID3D12GraphicsCommandList* cl, ID3D12Resource* color,
                ID3D12Resource* motion, ID3D12Resource* depth,
                ID3D12Resource* out) override;

  D3D12_RESOURCE_STATES OutputState() const override {
    return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  DXGI_FORMAT OutputFormat() const override { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
  const char* Name() const override { return name_.c_str(); }

  // Live tuning. Safe from any thread; picked up at the start of the next
  // Evaluate. Compose parameters apply on that frame; a pass setup builds a
  // new generation and swaps it in a frame later.
  void SetComposeParams(const NrBridgeParams& params);
  void RequestPassSetup(const NrPassSetup& setup);

  // For the debug dump: the model's working size.
  RuntimeVariant Variant() const { return variant_; }
  size_t PassCount() const { return current_.passes.size(); }
  uint32_t WorkWidth() const { return current_.workWidth; }
  uint32_t WorkHeight() const { return current_.workHeight; }

  // Where the GPU time inside Evaluate went, one entry per stage, in
  // milliseconds, from the frame before last. Empty when timestamps are
  // unavailable. The whole point is to stop guessing which stage is expensive:
  // the model's own evaluations and the compose passes around them are very
  // different things to optimise.
  const std::vector<std::pair<std::string, double>>& StageTimings() const {
    return lastStages_;
  }

 private:
  DirectNrPass() = default;

  struct Pass {
    NrPassTuning tuning;
    void* handle = nullptr;
  };

  struct Generation {
    std::vector<Pass> passes;
    // passes.size() - 1 of each: the raw hand-off between consecutive passes,
    // and the composed version of it when chaining is on. All at work size.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> intermediates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> chained;
    Microsoft::WRL::ComPtr<ID3D12Resource> modelIn;
    Microsoft::WRL::ComPtr<ID3D12Resource> modelOut;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    // The last pass's size: the full frame when finalFull is on with more
    // than one pass, else the working size. modelOut and the last chained
    // input are at this size.
    uint32_t finalWidth = 0;
    uint32_t finalHeight = 0;
    bool finalFull = false;
    bool chainComposed = true;
    bool featuresCreated = false;
  };

  // The texture the last pass reads: modelIn for a single pass, else the last
  // chained (or raw) hand-off. At finalWidth x finalHeight.
  static ID3D12Resource* FinalInput(const Generation& gen);

  bool PrepareDepth(ID3D12Device* device, float value, bool gradient, bool inverted);
  bool BuildGeneration(const NrPassSetup& setup, Generation& out) const;

  // Per-stage GPU timestamps inside Evaluate. Slot layout within a frame:
  // 0 begin, 1 after the input is prepared, then for each pass i: 2+2i after
  // its evaluation and 3+2i after the chained compose that follows it (only
  // between passes, so the last pass has none), and 2n+1 after the compose
  // that writes the output. Every slot up to the last must be written, or a
  // difference against an unwritten one reports a raw tick count.
  static constexpr uint32_t kStageSlots = 12;    // enough for four passes
  static constexpr uint32_t kStageFrames = 3;
  bool CreateStageQueries(ID3D12Device* device, uint64_t frequency);
  void MarkStage(ID3D12GraphicsCommandList* cl, uint32_t slot);
  void ResolveStages(ID3D12GraphicsCommandList* cl);
  void CollectStages();

  bool CreateFeatures(ID3D12GraphicsCommandList* cl, Generation& gen);
  void ReleaseFeatures(Generation& gen);
  void CopyThrough(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* out);
  void TakePending();

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::unique_ptr<NgxSession> session_;
  std::unique_ptr<NrForwarder> forwarder_;
  void* capability_ = nullptr;

  Generation current_;
  std::optional<Generation> next_;

  std::unique_ptr<NrColorBridge> bridge_;
  NrBridgeParams bridgeParams_;

  // Written by any thread, taken by the render thread at the top of Evaluate.
  std::mutex pendingMutex_;
  std::optional<NrBridgeParams> pendingCompose_;
  std::optional<NrPassSetup> pendingSetup_;

  Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
  Microsoft::WRL::ComPtr<ID3D12Resource> depthUpload_;
  uint32_t depthRowPitch_ = 0;
  bool depthUploaded_ = false;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  RuntimeVariant variant_ = RuntimeVariant::None;
  std::string name_;
  bool depthInverted_ = false;

  enum class State { WaitingToCreate, CreatedNotYetRun, Running, Failed };
  State state_ = State::WaitingToCreate;
  bool reset_ = true;
  uint64_t frames_ = 0;
  uint32_t consecutiveFailures_ = 0;

  Microsoft::WRL::ComPtr<ID3D12QueryHeap> stageHeap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> stageReadback_;
  uint64_t stageFrequency_ = 0;
  uint64_t stageFrame_ = 0;
  uint32_t stageUsed_ = 0;       // slots written last frame
  size_t stagePasses_ = 0;       // passes that frame, for the labels
  bool stageChained_ = false;
  std::vector<std::pair<std::string, double>> lastStages_;
};

}  // namespace sidecar
