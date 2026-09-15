#include "neural/DirectNrPass.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "core/Log.h"
#include "core/Sha256.h"

using Microsoft::WRL::ComPtr;

namespace sidecar {
namespace {

// Create on this frame, evaluate from the next. The feature's initialisation
// work is recorded into the command list it is created on, and the runtime
// has been seen to hang the GPU when create and first evaluate share a list.
constexpr uint64_t kCreateAtFrame = 2;

// The application ids the runtime is asked to accept, in order. The first is
// this project's own. The second is the generic id the OptiScaler fork found
// the runtime accepts for any unregistered caller; it is tried only if ours is
// refused, and the log says which one worked.
constexpr unsigned long long kAppIds[] = {0x5349444341524Eull /* "SIDCARN" */,
                                          0x24480451ull};

constexpr uint32_t kUploadAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

// Descriptor slots in the colour bridge. Every pass recorded into one command
// list needs its own; up to four model passes means up to three chained
// composes between them.
constexpr uint32_t kSlotResample = 0;
constexpr uint32_t kSlotChainBase = 1;   // 1, 2, 3
constexpr uint32_t kSlotFinal = 4;

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (!resource || before == after) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  cl->ResourceBarrier(1, &b);
}

SidecarNrTuning ToAbi(const NrPassTuning& t) {
  SidecarNrTuning out{};
  out.preset = t.preset;
  out.style = t.style;
  out.intensity = t.intensity;
  out.localStructure = t.localStructure;
  out.localTone = t.localTone;
  out.skinStructure = t.skinStructure;
  out.useAutoMask = t.autoMask ? 1u : 0u;
  out.uiCorrection = t.uiCorrection ? 1u : 0u;
  return out;
}

std::string Hex(int value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned int>(value));
  return buffer;
}

std::string Short(float value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  return buffer;
}

ComPtr<ID3D12Resource> MakeRgba16f(ID3D12Device* device, uint32_t w, uint32_t h) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> tex;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, kUav, nullptr,
                                             IID_PPV_ARGS(&tex)))) {
    return nullptr;
  }
  return tex;
}

std::string DescribeSetup(const NrPassSetup& setup, uint32_t workW, uint32_t workH) {
  const size_t n = setup.passes.empty() ? 1 : setup.passes.size();
  return std::to_string(n) + " pass(es), model at " + std::to_string(workW) + "x" +
         std::to_string(workH) +
         (n > 1 ? (setup.chainComposed || setup.finalFull ? ", chained through the compose"
                                                          : ", chained raw")
                : "") +
         (n > 1 && setup.finalFull ? ", final pass at full resolution" : "");
}

}  // namespace

std::unique_ptr<DirectNrPass> DirectNrPass::Create(ID3D12Device* device,
                                                   const Options& options,
                                                   std::string& reason) {
  reason.clear();
  if (!device || options.width == 0 || options.height == 0) {
    reason = "invalid device or dimensions";
    return nullptr;
  }

  const std::filesystem::path runtime = options.runtimeDir / "nvngx_dlssnr.dll";
  std::error_code ec;
  if (!std::filesystem::exists(runtime, ec) || ec) {
    reason = "nvngx_dlssnr.dll is not beside the sidecar";
    return nullptr;
  }
  const std::string digest = Sha256File(runtime);
  const auto entry = LookupRuntime(digest);
  if (entry) {
    const std::string mismatch = DescribeCompatibility(options.arch, entry->variant);
    if (!mismatch.empty()) {
      reason = mismatch;
      return nullptr;
    }
  }

  std::unique_ptr<DirectNrPass> p(new DirectNrPass());
  p->device_ = device;
  p->width_ = options.width;
  p->height_ = options.height;
  p->variant_ = entry ? entry->variant : RuntimeVariant::None;

  p->session_ = NgxSession::Create(device, options.runtimeDir);
  if (!p->session_ || !p->session_->Available()) {
    reason = p->session_ ? p->session_->UnavailableReason() : "NGX session could not be created";
    if (reason.empty()) reason = "the NGX core would not initialise";
    return nullptr;
  }
  p->capability_ = p->session_->CapabilityParameters();
  if (!p->capability_) {
    reason = "the NGX core refused its capability parameters";
    return nullptr;
  }

  p->forwarder_ = NrForwarder::Load(options.runtimeDir, reason);
  if (!p->forwarder_) return nullptr;

  const int slot = p->forwarder_->ProbeFloatSlot(p->capability_);
  if (slot < 0) {
    GlobalLog().Warn("direct NR: no vtable slot round-trips a float on this driver's "
                     "parameter block; intensity and the structure strengths will not "
                     "reach the model. Presets and style still will.");
  } else {
    GlobalLog().Info("direct NR: float parameters go through vtable slot " +
                     std::to_string(slot));
  }

  bool initialised = false;
  for (const unsigned long long appId : kAppIds) {
    DWORD seh = 0;
    const int r = p->forwarder_->Init(runtime, options.runtimeDir, appId, device,
                                      p->capability_, seh);
    if (seh != 0) {
      reason = "the neural runtime faulted during init (SEH " + std::to_string(seh) + ")";
      return nullptr;
    }
    if (r == 1) {
      char id[32];
      std::snprintf(id, sizeof(id), "0x%llX", appId);
      GlobalLog().Info(std::string("direct NR: runtime initialised (app id ") + id + ")");
      initialised = true;
      break;
    }
    GlobalLog().Warn(std::string("direct NR: runtime init refused: ") +
                     NrForwarder::ResultName(r) + " (" + Hex(r) + ")");
  }
  if (!initialised) {
    reason = "the neural runtime refused to initialise; see the log for the codes";
    return nullptr;
  }

  if (!p->PrepareDepth(device, options.syntheticDepth, options.depthGradient,
                       options.depthInverted)) {
    reason = "the synthetic depth plane could not be allocated";
    return nullptr;
  }
  p->depthInverted_ = options.depthInverted;
  GlobalLog().Info(std::string("direct NR: depth is ") +
                   (options.depthGradient ? "a ground-plane gradient" : "a flat plane") +
                   (options.depthInverted ? ", near = 1" : ", near = 0"));
  if (!p->BuildGeneration(options.setup, p->current_)) {
    reason = "the model's working textures could not be allocated";
    return nullptr;
  }
  p->name_ = "direct DLSS 5 NR x" + std::to_string(p->current_.passes.size());

  p->bridgeParams_ = options.bridge;
  p->bridge_ = NrColorBridge::Create(device);
  if (!p->bridge_) {
    reason = "the colour bridge's compute pipelines could not be created";
    return nullptr;
  }
  if (!p->CreateStageQueries(device, options.timestampFrequency)) {
    GlobalLog().Warn("direct NR: per-stage GPU timestamps unavailable; the log cannot "
                     "say which stage the frame time went to");
  }

  GlobalLog().Info(options.bridge.enabled
                       ? "direct NR: HDR encode ON (experimental), paper white " +
                             std::to_string(static_cast<int>(options.bridge.paperWhiteNits)) +
                             " nits, headroom " + Short(options.bridge.headroom) + "x"
                       : std::string("direct NR: the model sees the captured frame as-is"));
  GlobalLog().Info("direct NR: compose strength " + Short(options.bridge.strength) +
                   ", colour preserve " + Short(options.bridge.colourPreserve) +
                   ", highlight protect " + Short(options.bridge.highlightProtect));
  GlobalLog().Info(std::string("neural runtime: ") + ToString(p->variant_) +
                   (entry ? " " + entry->version : std::string(" (unrecognised build)")) +
                   ", direct path, " +
                   DescribeSetup(options.setup, p->current_.workWidth, p->current_.workHeight) +
                   " for a " + std::to_string(p->width_) + "x" + std::to_string(p->height_) +
                   " frame on " + ToString(options.arch));
  return p;
}

DirectNrPass::~DirectNrPass() {
  ReleaseFeatures(current_);
  if (next_) ReleaseFeatures(*next_);
}

void DirectNrPass::SetComposeParams(const NrBridgeParams& params) {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingCompose_ = params;
}

void DirectNrPass::RequestPassSetup(const NrPassSetup& setup) {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingSetup_ = setup;
}

bool DirectNrPass::PrepareDepth(ID3D12Device* device, float value, bool gradient,
                                bool inverted) {
  // Guides stay at full resolution; the runtime takes their size separately.
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = width_;
  rd.Height = height_;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R32_FLOAT;
  rd.SampleDesc.Count = 1;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&depth_)))) {
    return false;
  }

  const uint32_t rowPitch = AlignUp(width_ * sizeof(float), kUploadAlignment);
  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC ud{};
  ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  ud.Width = static_cast<uint64_t>(rowPitch) * height_;
  ud.Height = 1;
  ud.DepthOrArraySize = 1;
  ud.MipLevels = 1;
  ud.Format = DXGI_FORMAT_UNKNOWN;
  ud.SampleDesc.Count = 1;
  ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ud,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&depthUpload_)))) {
    return false;
  }
  void* mapped = nullptr;
  D3D12_RANGE noRead{0, 0};
  if (FAILED(depthUpload_->Map(0, &noRead, &mapped)) || !mapped) return false;
  for (uint32_t y = 0; y < height_; ++y) {
    auto* row = reinterpret_cast<float*>(static_cast<uint8_t*>(mapped) +
                                         static_cast<size_t>(y) * rowPitch);
    float depth = value;
    if (gradient) {
      // Ground plane: the bottom of the frame is near, the top is far. In the
      // standard convention near is 0; inverted, near is 1. Kept off the
      // extremes so the runtime never sees a degenerate 0 or 1 everywhere.
      const float distant = 0.05f + 0.90f * (1.0f - static_cast<float>(y) /
                                                        static_cast<float>(height_ - 1));
      depth = inverted ? 1.0f - distant : distant;
    }
    for (uint32_t x = 0; x < width_; ++x) row[x] = depth;
  }
  depthUpload_->Unmap(0, nullptr);
  depthRowPitch_ = rowPitch;
  return true;
}

bool DirectNrPass::BuildGeneration(const NrPassSetup& setup, Generation& out) const {
  Generation gen;
  // The model's working size, rounded to a multiple of eight.
  const float scale = std::clamp(setup.modelScale, 0.5f, 1.0f);
  gen.workWidth = std::max(8u, (static_cast<uint32_t>(width_ * scale + 0.5f) + 7u) & ~7u);
  gen.workHeight = std::max(8u, (static_cast<uint32_t>(height_ * scale + 0.5f) + 7u) & ~7u);
  gen.workWidth = std::min(gen.workWidth, width_);
  gen.workHeight = std::min(gen.workHeight, height_);

  if (setup.passes.empty()) {
    gen.passes.push_back(Pass{NrPassTuning{}, nullptr});
  } else {
    for (const auto& t : setup.passes) gen.passes.push_back(Pass{t, nullptr});
  }
  if (gen.passes.size() > 4) gen.passes.resize(4);

  // The final pass at full size means the last hand-off has to be composed
  // onto the full frame, so chaining is implied there. With one pass there is
  // no cheap pass to pair it with; the size is simply the working size.
  gen.finalFull = setup.finalFull && gen.passes.size() > 1;
  gen.chainComposed = setup.chainComposed || gen.finalFull;
  gen.finalWidth = gen.finalFull ? width_ : gen.workWidth;
  gen.finalHeight = gen.finalFull ? height_ : gen.workHeight;

  gen.modelIn = MakeRgba16f(device_.Get(), gen.workWidth, gen.workHeight);
  gen.modelOut = MakeRgba16f(device_.Get(), gen.finalWidth, gen.finalHeight);
  if (!gen.modelIn || !gen.modelOut) return false;
  for (size_t i = 0; i + 1 < gen.passes.size(); ++i) {
    auto raw = MakeRgba16f(device_.Get(), gen.workWidth, gen.workHeight);
    if (!raw) return false;
    gen.intermediates.push_back(raw);
    if (gen.chainComposed) {
      // The last link feeds the final pass and is at the final pass's size.
      const bool lastLink = i + 2 == gen.passes.size();
      auto composed = MakeRgba16f(device_.Get(), lastLink ? gen.finalWidth : gen.workWidth,
                                  lastLink ? gen.finalHeight : gen.workHeight);
      if (!composed) return false;
      gen.chained.push_back(composed);
    }
  }
  out = std::move(gen);
  return true;
}

ID3D12Resource* DirectNrPass::FinalInput(const Generation& gen) {
  if (gen.passes.size() <= 1) return gen.modelIn.Get();
  return gen.chainComposed ? gen.chained.back().Get() : gen.intermediates.back().Get();
}

bool DirectNrPass::CreateFeatures(ID3D12GraphicsCommandList* cl, Generation& gen) {
  for (size_t i = 0; i < gen.passes.size(); ++i) {
    auto& pass = gen.passes[i];
    const bool last = i + 1 == gen.passes.size();
    const uint32_t w = last ? gen.finalWidth : gen.workWidth;
    const uint32_t h = last ? gen.finalHeight : gen.workHeight;
    DWORD seh = 0;
    void* handle = nullptr;
    const int r = forwarder_->Create(cl, capability_, w, h, ToAbi(pass.tuning), handle, seh);
    if (seh != 0) {
      GlobalLog().Error("direct NR: feature creation faulted (SEH " + std::to_string(seh) +
                        ") on pass " + std::to_string(i + 1));
      ReleaseFeatures(gen);
      return false;
    }
    if (r != 1 || !handle) {
      GlobalLog().Error(std::string("direct NR: feature creation failed on pass ") +
                        std::to_string(i + 1) + ": " + NrForwarder::ResultName(r) + " (" +
                        Hex(r) + ")");
      ReleaseFeatures(gen);
      return false;
    }
    pass.handle = handle;
  }
  gen.featuresCreated = true;
  return true;
}

void DirectNrPass::ReleaseFeatures(Generation& gen) {
  if (!forwarder_) return;
  for (auto& pass : gen.passes) {
    if (!pass.handle) continue;
    DWORD seh = 0;
    forwarder_->Release(pass.handle, seh);
    pass.handle = nullptr;
  }
  gen.featuresCreated = false;
}

bool DirectNrPass::CreateStageQueries(ID3D12Device* device, uint64_t frequency) {
  if (!device || frequency == 0) return false;
  D3D12_QUERY_HEAP_DESC qh{};
  qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qh.Count = kStageSlots * kStageFrames;
  if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&stageHeap_)))) return false;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = sizeof(uint64_t) * kStageSlots * kStageFrames;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&stageReadback_)))) {
    stageHeap_.Reset();
    return false;
  }
  stageFrequency_ = frequency;
  return true;
}

void DirectNrPass::MarkStage(ID3D12GraphicsCommandList* cl, uint32_t slot) {
  if (!stageHeap_ || slot >= kStageSlots) return;
  const uint32_t base = static_cast<uint32_t>(stageFrame_ % kStageFrames) * kStageSlots;
  cl->EndQuery(stageHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + slot);
  stageUsed_ = std::max(stageUsed_, slot + 1);
}

void DirectNrPass::ResolveStages(ID3D12GraphicsCommandList* cl) {
  if (!stageHeap_ || stageUsed_ == 0) return;
  const uint32_t base = static_cast<uint32_t>(stageFrame_ % kStageFrames) * kStageSlots;
  cl->ResolveQueryData(stageHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, stageUsed_,
                       stageReadback_.Get(), sizeof(uint64_t) * base);
}

void DirectNrPass::CollectStages() {
  // The frame before last: its work has retired, and reading a frame late is
  // what keeps this from stalling anything.
  if (!stageHeap_ || stageFrame_ == 0 || stageUsed_ < 2) return;
  const uint32_t base =
      static_cast<uint32_t>((stageFrame_ - 1) % kStageFrames) * kStageSlots;
  void* mapped = nullptr;
  D3D12_RANGE read{sizeof(uint64_t) * base, sizeof(uint64_t) * (base + stageUsed_)};
  if (FAILED(stageReadback_->Map(0, &read, &mapped)) || !mapped) return;
  const auto* t = static_cast<const uint64_t*>(mapped) + base;

  std::vector<std::pair<std::string, double>> stages;
  const auto span = [&](uint32_t from, uint32_t to) {
    if (to >= stageUsed_ || t[to] <= t[from]) return 0.0;
    return 1000.0 * static_cast<double>(t[to] - t[from]) /
           static_cast<double>(stageFrequency_);
  };
  stages.emplace_back("input", span(0, 1));
  for (size_t i = 0; i < stagePasses_; ++i) {
    const uint32_t evalEnd = static_cast<uint32_t>(2 + 2 * i);
    const double ms = span(evalEnd - 1, evalEnd);
    stages.emplace_back("pass" + std::to_string(i + 1), ms);
    if (recording_ && ms > 0.0) {
      const auto bucket = static_cast<uint32_t>(ms);
      ++passHistogram_[std::min(bucket, kHistogramBuckets - 1)];
      if (histogramSamples_ == 0 || ms < histogramMinMs_) histogramMinMs_ = ms;
      if (ms > histogramMaxMs_) histogramMaxMs_ = ms;
      ++histogramSamples_;
    }
    if (stageChained_ && i + 1 < stagePasses_) {
      stages.emplace_back("chain" + std::to_string(i + 1), span(evalEnd, evalEnd + 1));
    }
  }
  // The final compose lands in the slot after the last pass's evaluation:
  // pass i ends at 2+2i, so the last one ends at 2n, and the compose is 2n+1.
  // (Writing it at 2n+2 leaves slot 2n+1 unwritten, and differencing against
  // an unwritten slot yields a raw tick count rather than a duration.)
  const uint32_t last = static_cast<uint32_t>(1 + 2 * stagePasses_);
  stages.emplace_back("compose", span(last - 1, last));
  stages.emplace_back("total", span(0, last));

  D3D12_RANGE wrote{0, 0};
  stageReadback_->Unmap(0, &wrote);
  lastStages_ = std::move(stages);
}

void DirectNrPass::StartRecordingTimings() {
  passHistogram_.fill(0);
  histogramSamples_ = 0;
  histogramMinMs_ = 0.0;
  histogramMaxMs_ = 0.0;
  recording_ = true;
}

std::string DirectNrPass::PassHistogram() const {
  if (histogramSamples_ == 0) return {};
  uint32_t peak = 0;
  uint32_t lowest = kHistogramBuckets;
  uint32_t highest = 0;
  for (uint32_t i = 0; i < kHistogramBuckets; ++i) {
    peak = std::max(peak, passHistogram_[i]);
    if (passHistogram_[i] > 0) {
      lowest = std::min(lowest, i);
      highest = i;
    }
  }
  if (peak == 0) return {};

  char header[192];
  std::snprintf(header, sizeof(header),
                "per-pass GPU time, %llu samples, %.1f to %.1f ms:",
                static_cast<unsigned long long>(histogramSamples_), histogramMinMs_,
                histogramMaxMs_);
  std::string out = header;

  // Only the occupied range, so an idle tail does not bury the shape. Bars are
  // scaled to the tallest bucket; the count is what actually matters and is
  // printed alongside.
  for (uint32_t i = lowest; i <= highest; ++i) {
    const uint32_t count = passHistogram_[i];
    const int width = static_cast<int>(40.0 * static_cast<double>(count) /
                                       static_cast<double>(peak) + 0.5);
    char line[128];
    std::snprintf(line, sizeof(line), "\n  %2u-%2u ms | %-40s %u", i, i + 1,
                  std::string(static_cast<size_t>(width), '#').c_str(), count);
    out += line;
  }
  return out;
}

void DirectNrPass::CopyThrough(ID3D12GraphicsCommandList* cl, ID3D12Resource* color,
                               ID3D12Resource* out) {
  if (color->GetDesc().Format != out->GetDesc().Format) {
    // The output is the presentable BGRA8 target (the resolve was folded into
    // the compose). CopyResource needs matching formats; a resample at strength
    // zero is the same copy through a view that converts.
    Transition(cl, color, kUav, kSrv);
    bridge_->RecordResample(cl, color, out, width_, height_, kSlotFinal);
    Transition(cl, color, kSrv, kUav);
    return;
  }
  Transition(cl, out, kUav, D3D12_RESOURCE_STATE_COPY_DEST);
  Transition(cl, color, kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cl->CopyResource(out, color);
  Transition(cl, color, D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
  Transition(cl, out, D3D12_RESOURCE_STATE_COPY_DEST, kUav);
}

void DirectNrPass::TakePending() {
  std::optional<NrBridgeParams> compose;
  std::optional<NrPassSetup> setup;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    compose.swap(pendingCompose_);
    setup.swap(pendingSetup_);
  }
  if (compose) {
    bridgeParams_ = *compose;
    GlobalLog().Verbose(LogCategory::Neural,
                        "direct NR: compose updated live: strength " + Short(compose->strength) +
                            ", colour preserve " + Short(compose->colourPreserve) +
                            ", highlight protect " + Short(compose->highlightProtect));
  }
  if (setup) {
    // The GPU is idle between frames (the overlay's present waits on its
    // fence), so an un-created next generation can simply be replaced.
    if (next_ && next_->featuresCreated) ReleaseFeatures(*next_);
    Generation gen;
    if (BuildGeneration(*setup, gen)) {
      next_ = std::move(gen);
      GlobalLog().Info("direct NR: new setup requested: " +
                       DescribeSetup(*setup, next_->workWidth, next_->workHeight) +
                       "; building beside the running one.");
    } else {
      GlobalLog().Error("direct NR: could not allocate textures for the new setup; keeping "
                        "the current one.");
    }
  }
}

bool DirectNrPass::Evaluate(ID3D12GraphicsCommandList* cl, ID3D12Resource* color,
                            ID3D12Resource* motion, ID3D12Resource* /*depth*/,
                            ID3D12Resource* out) {
  if (!cl || !color || !out) return false;

  TakePending();

  if (!depthUploaded_) {
    Transition(cl, depth_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = depth_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = depthUpload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    src.PlacedFootprint.Footprint.Width = width_;
    src.PlacedFootprint.Footprint.Height = height_;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = depthRowPitch_;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(cl, depth_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
    depthUploaded_ = true;
  }

  ++frames_;

  // A pending generation: created on this frame beside the running one, then
  // swapped in on the next. A failed session restarts on the new setup.
  if (next_) {
    if (state_ == State::Failed) {
      ReleaseFeatures(current_);
      current_ = std::move(*next_);
      next_.reset();
      state_ = State::WaitingToCreate;
      frames_ = kCreateAtFrame - 1;
    } else if (!next_->featuresCreated) {
      if (!CreateFeatures(cl, *next_)) {
        GlobalLog().Error("direct NR: the new setup's features could not be created; "
                          "keeping the current one.");
        next_.reset();
      }
    } else {
      ReleaseFeatures(current_);
      current_ = std::move(*next_);
      next_.reset();
      name_ = "direct DLSS 5 NR x" + std::to_string(current_.passes.size());
      reset_ = true;
      GlobalLog().Info("direct NR: switched to the new setup.");
    }
  }

  switch (state_) {
    case State::WaitingToCreate:
      if (frames_ >= kCreateAtFrame) {
        if (CreateFeatures(cl, current_)) {
          state_ = State::CreatedNotYetRun;
          GlobalLog().Info("direct NR: " + std::to_string(current_.passes.size()) +
                           " feature(s) created at frame " + std::to_string(frames_) +
                           "; evaluating from the next frame.");
        } else {
          state_ = State::Failed;
          GlobalLog().Error("direct NR: unavailable this session; passing frames through.");
        }
      }
      CopyThrough(cl, color, out);
      return true;
    case State::CreatedNotYetRun:
      state_ = State::Running;
      reset_ = true;
      break;
    case State::Failed:
      CopyThrough(cl, color, out);
      return true;
    case State::Running:
      break;
  }

  if (!motion) {
    reset_ = true;
    CopyThrough(cl, color, out);
    return true;
  }

  Generation& gen = current_;
  const bool bridged = bridgeParams_.enabled;
  const uint32_t workW = gen.workWidth;
  const uint32_t workH = gen.workHeight;

  // Per-stage timing starts here, once the frame is definitely going to run
  // the model: everything above is bookkeeping that happens whatever the state.
  CollectStages();
  stageUsed_ = 0;
  stagePasses_ = gen.passes.size();
  stageChained_ = gen.chainComposed && !bridged;
  MarkStage(cl, 0);

  Transition(cl, color, kUav, kSrv);
  Transition(cl, motion, kUav, kSrv);

  if (bridged) {
    bridge_->RecordEncode(cl, color, gen.modelIn.Get(), workW, workH, bridgeParams_,
                          kSlotResample);
  } else if (workW == width_ && workH == height_) {
    // Same size: a copy, not a dispatch. The resample shader was costing a
    // full-resolution pass to reproduce its input.
    Transition(cl, color, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cl, gen.modelIn.Get(), kUav, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(gen.modelIn.Get(), color);
    Transition(cl, gen.modelIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kUav);
    Transition(cl, color, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
  } else {
    bridge_->RecordResample(cl, color, gen.modelIn.Get(), workW, workH, kSlotResample);
  }
  Transition(cl, gen.modelIn.Get(), kUav, kSrv);
  MarkStage(cl, 1);

  ID3D12Resource* input = gen.modelIn.Get();
  bool ok = true;
  bool faulted = false;
  size_t rawHandedOff = 0;
  size_t chainedHandedOff = 0;
  for (size_t i = 0; i < gen.passes.size() && ok; ++i) {
    const bool last = i + 1 == gen.passes.size();
    ID3D12Resource* target = last ? gen.modelOut.Get() : gen.intermediates[i].Get();
    const uint32_t passW = last ? gen.finalWidth : workW;
    const uint32_t passH = last ? gen.finalHeight : workH;

    SidecarNrEval eval{};
    eval.color = input;
    eval.depth = depth_.Get();
    eval.motion = motion;
    eval.output = target;
    eval.width = passW;
    eval.height = passH;
    eval.guideWidth = width_;
    eval.guideHeight = height_;
    eval.depthInverted = depthInverted_ ? 1u : 0u;
    eval.reset = reset_ ? 1u : 0u;
    eval.mvScaleX = static_cast<float>(passW);
    eval.mvScaleY = static_cast<float>(passH);

    DWORD seh = 0;
    const int r = forwarder_->Evaluate(cl, gen.passes[i].handle, capability_, eval,
                                       ToAbi(gen.passes[i].tuning), seh);
    if (seh != 0) {
      GlobalLog().Error("direct NR: evaluate faulted (SEH " + std::to_string(seh) +
                        ") on pass " + std::to_string(i + 1) + "; dropping the features.");
      ok = false;
      faulted = true;
      break;
    }
    if (r != 1) {
      GlobalLog().Error(std::string("direct NR: evaluate failed on pass ") +
                        std::to_string(i + 1) + ": " + NrForwarder::ResultName(r) + " (" +
                        Hex(r) + ")");
      ok = false;
      break;
    }
    MarkStage(cl, static_cast<uint32_t>(2 + 2 * i));
    if (!last) {
      Transition(cl, target, kUav, kSrv);
      ++rawHandedOff;
      if (gen.chainComposed && !bridged) {
        // The next pass sees this one's edit composed onto the frame. For the
        // link into a full-resolution final pass, that frame is the real one:
        // the low-res lighting decision lands on the full-res picture, and the
        // final pass sees full detail.
        const bool intoFinal = i + 2 == gen.passes.size();
        const bool fullLink = intoFinal && gen.finalFull;
        ID3D12Resource* composed = gen.chained[i].Get();
        // The split belongs to the final compose only: an intermediate that
        // was half-original would feed the next pass a seam.
        NrBridgeParams chainParams = bridgeParams_;
        chainParams.split = 0.0f;
        bridge_->RecordCompose(cl, fullLink ? color : gen.modelIn.Get(), gen.modelIn.Get(),
                               target, composed, fullLink ? gen.finalWidth : workW,
                               fullLink ? gen.finalHeight : workH, chainParams,
                               kSlotChainBase + static_cast<uint32_t>(i));
        Transition(cl, composed, kUav, kSrv);
        ++chainedHandedOff;
        input = composed;
        MarkStage(cl, static_cast<uint32_t>(3 + 2 * i));
      } else {
        input = target;
      }
    }
  }

  if (ok) {
    Transition(cl, gen.modelOut.Get(), kUav, kSrv);
    // The ratio for the compose and the stabiliser is against the ORIGINAL at
    // the last pass's size -- the frame itself when that pass is full-res,
    // the resampled frame otherwise -- so it carries every pass's edit. Taking
    // it against the last pass's own input would keep only that pass's
    // increment and throw the chain away.
    ID3D12Resource* shown = gen.finalFull ? color : gen.modelIn.Get();
    if (bridged) {
      bridge_->RecordResolve(cl, color, gen.modelOut.Get(), out, width_, height_, bridgeParams_,
                             kSlotFinal);
    } else {
      bridge_->RecordCompose(cl, color, shown, gen.modelOut.Get(), out, width_, height_,
                             bridgeParams_, kSlotFinal);
    }
    Transition(cl, gen.modelOut.Get(), kSrv, kUav);
    MarkStage(cl, static_cast<uint32_t>(1 + 2 * gen.passes.size()));
    ResolveStages(cl);
  }
  ++stageFrame_;

  for (size_t i = 0; i < rawHandedOff; ++i) Transition(cl, gen.intermediates[i].Get(), kSrv, kUav);
  for (size_t i = 0; i < chainedHandedOff; ++i) Transition(cl, gen.chained[i].Get(), kSrv, kUav);
  Transition(cl, gen.modelIn.Get(), kSrv, kUav);
  Transition(cl, motion, kSrv, kUav);
  Transition(cl, color, kSrv, kUav);

  if (ok) {
    reset_ = false;
    consecutiveFailures_ = 0;
    return true;
  }

  if (faulted) {
    ReleaseFeatures(current_);
    state_ = State::Failed;
    return false;
  }

  if (++consecutiveFailures_ >= 8) {
    GlobalLog().Error("direct NR: eight refused evaluates in a row; passing frames "
                      "through for the rest of this session.");
    ReleaseFeatures(current_);
    state_ = State::Failed;
  }
  reset_ = true;
  CopyThrough(cl, color, out);
  return true;
}

}  // namespace sidecar
