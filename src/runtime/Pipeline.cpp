#include "runtime/Pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "capture/WgcSource.h"
#include "core/ImageDump.h"
#include "core/Log.h"
#include "core/UiCalibration.h"
#include "gpu/DeviceBridge.h"
#include "neural/DirectNrPass.h"
#include "neural/NeuralPassFactory.h"
#include "neural/PassthroughPass.h"
#include "present/DCompOverlay.h"
#include "present/HdrDisplay.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace sidecar {
namespace {

// Two decimals is the useful precision for a millisecond figure; std::to_string
// would print six and bury it.
std::string FormatMs(double ms) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", ms);
  return buffer;
}

// Truncating copy into one of SidecarStatus' fixed buffers. The status block
// lives in shared memory, so nothing in it may be a pointer, and a name that
// does not fit is better clipped than left as a dangling read.
template <size_t N>
void CopyInto(char (&destination)[N], const char* source) {
  if (!source) source = "";
  const size_t length = std::min(std::strlen(source), N - 1);
  std::memcpy(destination, source, length);
  std::memset(destination + length, 0, N - length);
}

// Same shape as the work target but in a caller-chosen format, for a pass that
// writes something other than the presentable one. It rests in
// UNORDERED_ACCESS because that is how NGX writes it.
ComPtr<ID3D12Resource> CreateTypedTarget(ID3D12Device* dev, uint32_t w, uint32_t h,
                                         DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = format;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ComPtr<ID3D12Resource> tex;
  dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                               IID_PPV_ARGS(&tex));
  return tex;
}

// A D3D12 texture the neural pass can write into, matching the capture format.
// It rests in COPY_SOURCE between frames because the overlay's present is the
// last thing to touch it; the render loop brackets the pass with barriers.
ComPtr<ID3D12Resource> CreateWorkTarget(ID3D12Device* dev, uint32_t w, uint32_t h,
                                        DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = format;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ComPtr<ID3D12Resource> tex;
  dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                               D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                               IID_PPV_ARGS(&tex));
  return tex;
}

}  // namespace

std::unique_ptr<Pipeline> Pipeline::Create(const GpuInfo& gpu,
                                           const PipelineConfig& config,
                                           std::unique_ptr<INeuralPass> pass) {
  if (!config.target || !IsWindow(config.target)) return nullptr;

  RECT client{};
  if (!GetClientRect(config.target, &client)) return nullptr;
  const uint32_t w = static_cast<uint32_t>(client.right - client.left);
  const uint32_t h = static_cast<uint32_t>(client.bottom - client.top);
  if (w == 0 || h == 0) return nullptr;

  std::unique_ptr<Pipeline> p(new Pipeline());
  p->config_ = config;
  p->activePreset_ = config.activePreset;
  p->gpu_ = gpu;
  p->overlayVisible_.store(config.showOverlay, std::memory_order_release);
  p->hudVisible_.store(config.showHud, std::memory_order_release);
  // A caller-supplied pass is device-free by construction -- that is the only
  // kind that can exist before this function runs. A pass named in config may
  // hold device resources, so it is built below, once the device does, and
  // rebuilt with it after device loss.
  p->passFromConfig_ = (pass == nullptr);
  p->dev_.pass = std::move(pass);

  // The display decides the pixel format. On an HDR desktop the compositor
  // holds the window as scRGB FP16; captured into 8 bits it would arrive with
  // every highlight clipped to white, so the whole frame path goes FP16.
  p->dev_.hdr = DisplayIsHdr(config.target);
  const DXGI_FORMAT ringFormat =
      p->dev_.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  {
    // Paper white: the operator's number, else the display's own SDR white,
    // else the SDR reference. An SDR game on an HDR desktop is composed at the
    // display's SDR white, so that is the level that maps to 1.0 exactly.
    const float sdrWhite = SdrWhiteLevelNits(config.target);
    p->dev_.hdrParams.paperWhiteNits =
        config.hdrPaperWhiteNits > 0.0f ? config.hdrPaperWhiteNits
        : sdrWhite > 0.0f               ? sdrWhite
                                        : 203.0f;
    // Headroom spans what the display can show. It is not a taste dial: the
    // tone-map is identity below paper white, so an SDR game is untouched by
    // it whatever it is, and an HDR game wants the model to see everything the
    // panel can display.
    const float peak = MaxLuminanceNits(config.target);
    p->dev_.hdrParams.headroom =
        peak > 0.0f ? std::clamp(peak / p->dev_.hdrParams.paperWhiteNits, 1.0f, 8.0f) : 4.0f;
  }
  p->dev_.hdrParams.split = config.nr.splitView;

  p->dev_.bridge = DeviceBridge::Create(gpu.luid, w, h, ringFormat);
  if (!p->dev_.bridge) return nullptr;

  p->dev_.overlay = DCompOverlay::Create(*p->dev_.bridge, w, h);
  if (!p->dev_.overlay) return nullptr;

  POINT origin{client.left, client.top};
  ClientToScreen(config.target, &origin);
  RECT bounds{origin.x, origin.y, origin.x + static_cast<LONG>(w),
              origin.y + static_cast<LONG>(h)};
  p->dev_.overlay->SetBounds(bounds);

  p->dev_.hud = Hud::Create();          // a missing HUD is not fatal
  // Narrow the adapter description to ASCII for the HUD's GDI text path.
  p->dev_.gpuName.reserve(gpu.name.size());
  for (const wchar_t c : gpu.name) {
    p->dev_.gpuName.push_back(c < 128 ? static_cast<char>(c) : '?');
  }

  auto* dev = p->dev_.bridge->D3d12();
  p->dev_.workTarget = CreateWorkTarget(dev, w, h, ringFormat);
  if (!p->dev_.workTarget) return nullptr;
  if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&p->dev_.alloc)))) return nullptr;
  if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->dev_.alloc.Get(),
                                    nullptr, IID_PPV_ARGS(&p->dev_.cmdList)))) return nullptr;
  p->dev_.cmdList->Close();
  if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         IID_PPV_ARGS(&p->dev_.alloc2)))) return nullptr;
  if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->dev_.alloc2.Get(),
                                    nullptr, IID_PPV_ARGS(&p->dev_.cmdList2)))) return nullptr;
  p->dev_.cmdList2->Close();

  // Optical flow and its consumers. Every one of these is optional: a failure
  // here costs motion vectors, not the frame, so the pipeline still runs with
  // a static-scene assumption (spec section 11).
  p->dev_.normalize = FormatNormalize::Create(dev, w, h);
  p->dev_.normalized = FormatNormalize::CreateRgba16fTarget(dev, w, h);
  if (p->dev_.hdr) {
    // The tone-map that produces the SDR view, and the compose that applies
    // the model's edit back. Without them an HDR capture cannot be shown to
    // an LDR model at all, so this one is not optional.
    p->dev_.hdrBridge = HdrBridge::Create(dev, w, h);
    if (!p->dev_.hdrBridge || !p->dev_.normalized) {
      GlobalLog().Error("HDR: the tone-map passes could not be created");
      return nullptr;
    }
    const float sdrWhite = SdrWhiteLevelNits(config.target);
    GlobalLog().Info("HDR desktop: capturing FP16 scRGB; the model sees a tone-mapped "
                     "view at paper white " +
                     std::to_string(static_cast<int>(p->dev_.hdrParams.paperWhiteNits)) +
                     " nits" + (config.hdrPaperWhiteNits > 0.0f ? " (set)" : " (automatic)") +
                     ", headroom " + FormatMs(p->dev_.hdrParams.headroom) +
                     "x (from the display's " +
                     std::to_string(static_cast<int>(MaxLuminanceNits(config.target))) +
                     " nit peak)" +
                     (sdrWhite > 0.0f ? "; Windows SDR white is " +
                                            std::to_string(static_cast<int>(sdrWhite)) + " nits"
                                      : std::string()));
  }
  p->dev_.luminance = Luminance::Create(dev, w, h);
  p->dev_.previousLuma = Luminance::CreateR8Target(dev, w, h, D3D12_RESOURCE_STATE_COMMON);
  p->dev_.currentLuma = Luminance::CreateR8Target(dev, w, h, D3D12_RESOURCE_STATE_COMMON);
  p->dev_.flow = NvofaFlow::Create(dev, p->dev_.bridge->Queue(), w, h, config.flowGridSize);
  p->dev_.flowToMv = FlowToMotionVec::Create(dev, w, h);
  p->dev_.motionTarget = FlowToMotionVec::CreateMotionTarget(dev, w, h);

  // Build the configured pass now that there is a device for it to hold. Any
  // failure inside MakeNeuralPass degrades to passthrough and explains itself,
  // so this cannot fail the pipeline (spec section 11).
  if (p->passFromConfig_) {
    NeuralPassContext ctx;
    ctx.device = dev;
    ctx.runtimeDir = config.runtimeDir;
    ctx.width = w;
    ctx.height = h;
    ctx.arch = gpu.arch;
    ctx.syntheticDepth = config.syntheticDepth;
    ctx.depthGradient = config.depthGradient;
    ctx.depthInverted = config.depthInverted;
    ctx.nr = config.nr;

    std::vector<std::string> warnings;
    p->dev_.pass = MakeNeuralPass(config.neuralPass, ctx, warnings);
    for (const auto& warning : warnings) GlobalLog().Warn(warning);
    if (!p->dev_.pass) return nullptr;
    GlobalLog().Info(std::string("neural pass: ") + p->dev_.pass->Name());
    // Only a route-B pass has a runtime behind it; passthrough leaves this
    // empty and the HUD omits the field entirely.
    if (auto* direct = dynamic_cast<DirectNrPass*>(p->dev_.pass.get())) {
      p->dev_.runtimeVariant = ToString(direct->Variant());
    }
  }

  // A pass that names its own output format wants an intermediate target in it,
  // which the mask blend then resolves down to the presentable BGRA8. That is
  // also the only stage that can convert between the two, so a pass asking for
  // a format implies the blend runs whether or not any rectangles are masked.
  const DXGI_FORMAT passFormat = p->dev_.pass->OutputFormat();
  const bool needsResolve = passFormat != DXGI_FORMAT_UNKNOWN;

  // Fold the resolve into the pass's own compose where that is possible: the
  // direct pass can write the presentable target straight from its final
  // compute pass if the device can store typed BGRA8 from a shader. HDR keeps
  // its own compose (that *is* the resolve there), and mask rectangles need
  // the blend.
  if (needsResolve && !p->dev_.hdr && config.uiMaskRects.empty() &&
      dynamic_cast<DirectNrPass*>(p->dev_.pass.get()) != nullptr) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                           sizeof(support))) &&
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
      p->dev_.directResolve = true;
      GlobalLog().Info("resolve folded into the compose: one full-resolution pass fewer");
    } else {
      GlobalLog().Info("the device cannot store typed BGRA8 from compute; keeping the "
                       "resolve pass");
    }
  }

  // The UI mask, only when the operator actually configured rectangles. Without
  // it the render loop takes the path it took before this existed, so an
  // unmasked run pays nothing for the feature and cannot regress because of it.
  if (!config.uiMaskRects.empty() || (needsResolve && !p->dev_.directResolve)) {
    p->dev_.uiMask = UiMask::Create(dev, w, h);
    if (p->dev_.uiMask) {
      p->dev_.neuralTarget = needsResolve
                                 ? CreateTypedTarget(dev, w, h, passFormat)
                                 : CreateWorkTarget(dev, w, h, ringFormat);
    }
    if (!p->dev_.uiMask || !p->dev_.neuralTarget) {
      p->dev_.uiMask.reset();
      p->dev_.neuralTarget.Reset();
      if (needsResolve) {
        // Without the resolve stage this pass's output cannot be presented at
        // all, so fall back to the one pass that needs no resolve.
        GlobalLog().Error("the neural pass needs a resolve stage that could not "
                          "be created; falling back to passthrough.");
        p->dev_.pass = PassthroughPass::Create();
      } else {
        // Degrade rather than fail: an unmasked overlay is worth more than none,
        // and the failure rule reserves hiding for things that break the frame.
        GlobalLog().Warn("UI mask could not be created; the interface will be "
                         "processed along with the rest of the frame.");
      }
    } else if (!config.uiMaskRects.empty()) {
      GlobalLog().Info("UI mask active with " +
                       std::to_string(config.uiMaskRects.size()) + " rectangle(s).");
    }
  }

  // Say which way this went. "No motion vectors" is the difference between a
  // neural pass that tracks the scene and one that assumes it is static, and
  // silently guessing wrong is exactly the kind of thing that gets diagnosed
  // as a quality problem months later.
  if (p->dev_.flow && p->dev_.flow->Available()) {
    GlobalLog().Info("optical flow available, grid size " +
                     std::to_string(config.flowGridSize));
  } else {
    GlobalLog().Warn("optical flow unavailable; running on a zero motion field");
  }

  Pipeline* raw = p.get();
  p->dev_.source = WgcSource::CreateForWindow(config.target, *p->dev_.bridge,
                                              [raw] { raw->stats_.RecordDrop(); });
  if (!p->dev_.source) return nullptr;

  DCompOverlay* overlay = p->dev_.overlay.get();
  p->dev_.tracker = WindowTracker::Create(config.target, [overlay](const RECT& r) {
    overlay->SetBounds(r);
  });
  // A missing tracker is not fatal: the overlay simply will not follow moves.

  return p;
}

Pipeline::~Pipeline() { Stop(); }

HWND Pipeline::OverlayHwnd() const { return dev_.overlay ? dev_.overlay->Hwnd() : nullptr; }

std::string Pipeline::LastError() const {
  std::lock_guard<std::mutex> lock(errorMutex_);
  return lastError_;
}

void Pipeline::FailAndHide(const char* reason) {
  // Spec failure rule: hide first, then record. Never leave an opaque overlay
  // over a live game.
  if (dev_.overlay) dev_.overlay->Hide();
  GlobalLog().Error(reason);
  std::lock_guard<std::mutex> lock(errorMutex_);
  lastError_ = reason;
}

void Pipeline::PublishStatus(const HudModel& model, const FrameBudget& budget) {
  if (!statusSink_) return;

  SidecarStatus status;
  status.fps = budget.fps;
  status.captureFps = budget.captureFps;
  status.idleMs = budget.idleMs;
  status.recordMs = budget.recordMs;
  status.presentWaitMs = budget.presentWaitMs;
  status.gpuWaitMs = budget.gpuWaitMs;
  status.overlayVisible = overlayVisible_.load(std::memory_order_acquire) ? 1u : 0u;
  status.hudVisible = hudVisible_.load(std::memory_order_acquire) ? 1u : 0u;
  status.overlayInteractive = overlayInteractive_.load(std::memory_order_acquire) ? 1u : 0u;
  status.calibrationStep = calibrationStep_.load(std::memory_order_acquire);
  status.calibrationRects = calibrationRects_.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(presetMutex_);
    std::snprintf(status.activePreset, sizeof(status.activePreset), "%s",
                  activePreset_.c_str());
  }
  status.hotkeysRegistered = hotkeyMask_.load(std::memory_order_acquire);
  status.width = dev_.bridge ? dev_.bridge->Width() : 0;
  status.height = dev_.bridge ? dev_.bridge->Height() : 0;
  if (auto memory = QueryVideoMemory(gpu_.luid)) {
    status.vramUsedMb = static_cast<uint32_t>(memory->usedBytes / (1024 * 1024));
    status.vramBudgetMb = static_cast<uint32_t>(memory->budgetBytes / (1024 * 1024));
    status.vramSpilledMb = static_cast<uint32_t>(memory->spilledBytes / (1024 * 1024));
    status.vramSystemMb = static_cast<uint32_t>(memory->systemBytes / (1024 * 1024));
  }
  status.p50Ms = model.p50Ms;
  status.p99Ms = model.p99Ms;
  status.frames = model.frames;
  status.drops = model.drops;
  CopyInto(status.passName, model.passName);
  CopyInto(status.runtimeVariant, model.runtimeVariant);
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    CopyInto(status.lastError, lastError_.c_str());
  }
  statusSink_(status);
}

void Pipeline::SetOverlayVisible(bool visible) {
  overlayVisible_.store(visible, std::memory_order_release);
  ApplyVisibility();
}

void Pipeline::SetHudVisible(bool visible) {
  hudVisible_.store(visible, std::memory_order_release);
  ApplyVisibility();
}

void Pipeline::ApplyVisibility() {
  // The operator's intent is necessary but not sufficient: an opaque topmost
  // window over something other than the game is a window the player cannot
  // get out from under.
  const bool overlay = overlayVisible_.load(std::memory_order_acquire) && gameFocused_;
  const bool hud = hudVisible_.load(std::memory_order_acquire) && gameFocused_;
  // With nothing on screen the render thread idles rather than feeding the
  // model frames nobody sees.
  paused_.store(!overlay, std::memory_order_release);
  if (dev_.overlay) {
    if (overlay) dev_.overlay->Show(); else dev_.overlay->Hide();
  }
  if (dev_.hud) {
    if (hud) dev_.hud->Show(); else dev_.hud->Hide();
  }
}

std::vector<uint8_t> Pipeline::ReadbackBgr(ID3D12Resource* texture,
                                           D3D12_RESOURCE_STATES restState, bool rgba16f,
                                           bool scRgb, uint32_t* outWidth, uint32_t* outHeight,
                                           float scale, float bias) {
  std::vector<uint8_t> bgr;
  if (!texture || !dev_.bridge) return bgr;
  auto* device = dev_.bridge->D3d12();

  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  const uint32_t w = static_cast<uint32_t>(desc.Width);
  const uint32_t h = desc.Height;
  if (outWidth) *outWidth = w;
  if (outHeight) *outHeight = h;
  // Half-float channel count per pixel: RGBA16F has four, RG16F (the motion
  // field) two.
  const int halves = desc.Format == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = bytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> buffer;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&buffer)))) {
    GlobalLog().Error("readback: could not allocate a buffer");
    return bgr;
  }

  dev_.alloc->Reset();
  dev_.cmdList->Reset(dev_.alloc.Get(), nullptr);
  D3D12_RESOURCE_BARRIER to{};
  to.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to.Transition.pResource = texture;
  to.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to.Transition.StateBefore = restState;
  to.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (restState != D3D12_RESOURCE_STATE_COPY_SOURCE) dev_.cmdList->ResourceBarrier(1, &to);
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = texture;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = buffer.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  dev_.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (restState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER back = to;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back.Transition.StateAfter = restState;
    dev_.cmdList->ResourceBarrier(1, &back);
  }
  dev_.cmdList->Close();
  ID3D12CommandList* lists[] = {dev_.cmdList.Get()};
  dev_.bridge->Queue()->ExecuteCommandLists(1, lists);
  DrainGpu();

  void* mapped = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
  if (FAILED(buffer->Map(0, &range, &mapped)) || !mapped) return bgr;
  bgr.resize(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped) + footprint.Offset +
                         static_cast<size_t>(y) * footprint.Footprint.RowPitch;
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* px = bgr.data() + (static_cast<size_t>(y) * w + x) * 3;
      if (rgba16f) {
        const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * halves;
        for (int c = 0; c < 3; ++c) {
          float v = c < halves ? HalfToFloat(p[c]) : 0.0f;
          v = v * scale + bias;
          if (scRgb) {
            // scRGB linear to a viewable SDR preview: paper white to 1.0, a
            // soft shoulder above it, sRGB-encoded. Diagnostic only.
            v = v * 80.0f / dev_.hdrParams.paperWhiteNits;
            if (v > 0.8f) v = 0.8f + 0.2f * (1.0f - std::exp(-(v - 0.8f) * 1.5f));
            v = v < 0.0f ? 0.0f : v;
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
          }
          v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
          px[2 - c] = static_cast<uint8_t>(v * 255.0f + 0.5f);   // RGB -> BGR
        }
      } else {
        const uint8_t* p = row + static_cast<size_t>(x) * 4;   // BGRA8
        px[0] = p[0];
        px[1] = p[1];
        px[2] = p[2];
      }
    }
  }
  D3D12_RANGE none{0, 0};
  buffer->Unmap(0, &none);
  return bgr;
}

void Pipeline::DumpDebugFrames() {
  // The widened frame (what the neural pass reads), the pass's output (only
  // when a pass names a format) and the presented frame.
  const uint32_t w = dev_.bridge->Width();
  const uint32_t h = dev_.bridge->Height();
  if (!dev_.neuralTarget && !dev_.directResolve) {
    // Said explicitly, because otherwise a missing nr_output.bmp with an input
    // that equals the presented frame reads as a broken dump rather than what
    // it is: the neural pass is not the one running.
    GlobalLog().Warn(std::string("debug dump: the active pass is \"") + dev_.pass->Name() +
                     "\", which has no neural output to dump; nr_output.bmp is not "
                     "written and presented.bmp will match nr_input.bmp. See earlier "
                     "lines for why the neural pass is not running.");
  }
  struct Item {
    const char* name;
    ID3D12Resource* tex;
    D3D12_RESOURCE_STATES rest;
    bool half;
    bool scRgb;
  };
  const Item items[] = {
      {"nr_input", dev_.normalized.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, false},
      // With the resolve folded in, the composed result *is* the work target.
      {"nr_output",
       dev_.directResolve ? dev_.workTarget.Get() : dev_.neuralTarget.Get(),
       dev_.directResolve ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
       !dev_.directResolve, false},
      // In HDR the presented frame is scRGB; it is previewed through the same
      // tone curve the model's view uses so the BMP is comparable.
      {"presented", dev_.workTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, dev_.hdr, dev_.hdr},
  };
  std::vector<std::vector<uint8_t>> images;
  for (const auto& item : items) {
    if (!item.tex) {
      images.emplace_back();
      continue;
    }
    images.push_back(ReadbackBgr(item.tex, item.rest, item.half, item.scRgb));
    if (images.back().empty()) continue;
    const auto path = config_.runtimeDir / (std::string(item.name) + ".bmp");
    if (WriteBmp(path, w, h, images.back())) {
      GlobalLog().Info(std::string("debug dump: wrote ") + path.filename().string());
    } else {
      GlobalLog().Error(std::string("debug dump: could not write ") + path.string());
    }
  }

  // How much the pass changed the picture: mean absolute difference per
  // channel in 8-bit units, and the fraction of pixels moved more than a few
  // levels. Two numbers that separate "barely touched" from "changed".
  if (images.size() >= 2 && !images[0].empty() && !images[1].empty()) {
    double sum = 0.0;
    size_t changed = 0;
    const size_t pixels = static_cast<size_t>(w) * h;
    for (size_t p = 0; p < pixels; ++p) {
      int d = 0;
      for (int c = 0; c < 3; ++c) {
        d += std::abs(static_cast<int>(images[0][p * 3 + c]) -
                      static_cast<int>(images[1][p * 3 + c]));
      }
      sum += d / 3.0;
      if (d > 12) ++changed;
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "debug dump: neural pass changed the frame by %.2f levels on average "
                  "(of 255); %.1f%% of pixels moved more than 4 levels",
                  sum / static_cast<double>(pixels),
                  100.0 * static_cast<double>(changed) / static_cast<double>(pixels));
    GlobalLog().Info(line);
  }
}

void Pipeline::CalibrationCapture(int step) {
  if (!dev_.normalized) return;
  const uint32_t w = dev_.bridge->Width();
  const uint32_t h = dev_.bridge->Height();

  // The widened capture, before any neural work: this is the game as drawn,
  // interface included, which is exactly what the calibration compares.
  auto frame = ReadbackBgr(dev_.normalized.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
  if (frame.empty()) {
    GlobalLog().Error("calibration: could not read the frame back");
    return;
  }

  if (step == 1) {
    calibrationWithUi_ = std::move(frame);
    calibrationStep_.store(1, std::memory_order_release);
    GlobalLog().Info("calibration: captured the frame with the interface. Hide the "
                     "interface in the game (Alt+Z) and take the second capture.");
    return;
  }

  if (calibrationWithUi_.empty()) {
    GlobalLog().Warn("calibration: no first capture yet; take the one with the interface first");
    return;
  }

  std::vector<uint8_t> diff;
  const auto rects = RectsFromDiff(calibrationWithUi_, frame, w, h, UiCalibrationParams{}, &diff);

  // The rectangles, as the config file would hold them, so the manager can
  // read them with the same parser it uses for sidecar.toml.
  {
    std::ofstream out(config_.runtimeDir / "ui_mask_calibration.toml",
                      std::ios::binary | std::ios::trunc);
    out << "# Written by the sidecar's UI-mask calibrator. Load it from the manager's\n"
           "# Tuning page; it is not read automatically. Coordinates are in the capture\n"
           "# resolution at the time of calibration ("
        << w << "x" << h << ").\n";
    for (const auto& r : rects) {
      out << "\n[[ui_mask]]\nleft = " << r.left << "\ntop = " << r.top
          << "\nright = " << r.right << "\nbottom = " << r.bottom << "\n";
    }
  }
  // And what was detected, as a picture: white where the pixels differed.
  {
    std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
    for (size_t p = 0; p < diff.size(); ++p) bgr[p * 3] = bgr[p * 3 + 1] = bgr[p * 3 + 2] = diff[p];
    WriteBmp(config_.runtimeDir / "ui_mask_diff.bmp", w, h, bgr);
  }

  calibrationRects_.store(static_cast<uint32_t>(rects.size()), std::memory_order_release);
  calibrationStep_.store(2, std::memory_order_release);
  calibrationWithUi_.clear();
  GlobalLog().Info("calibration: found " + std::to_string(rects.size()) +
                   " interface rectangle(s); wrote ui_mask_calibration.toml and "
                   "ui_mask_diff.bmp beside the sidecar.");
}

bool Pipeline::ApplySettings(const PipelineConfig& incoming) {
  const PipelineConfig previous = config_;
  // Whatever happens next reads from config_: a live apply keeps it current
  // for the next rebuild, and a rebuild builds from it.
  PipelineConfig merged = incoming;
  merged.target = previous.target;   // the capture target never changes here
  merged.runtimeDir = previous.runtimeDir;
  config_ = merged;

  // Anything the pass cannot take live means a rebuild.
  const bool structural =
      merged.neuralPass != previous.neuralPass || merged.flowGridSize != previous.flowGridSize ||
      merged.syntheticDepth != previous.syntheticDepth ||
      merged.depthGradient != previous.depthGradient ||
      merged.depthInverted != previous.depthInverted ||
      merged.uiMaskFeather != previous.uiMaskFeather ||
      merged.uiMaskRects.size() != previous.uiMaskRects.size() ||
      !std::equal(merged.uiMaskRects.begin(), merged.uiMaskRects.end(),
                  previous.uiMaskRects.begin(), [](const UiRect& a, const UiRect& b) {
                    return a.left == b.left && a.top == b.top && a.right == b.right &&
                           a.bottom == b.bottom;
                  });
  if (structural) return false;

  if (merged.showHud != previous.showHud) SetHudVisible(merged.showHud);
  SetActivePreset(merged.activePreset);
  // The HDR tone-map is per-frame constants; it takes effect on the next frame.
  if (merged.hdrPaperWhiteNits > 0.0f) {
    dev_.hdrParams.paperWhiteNits = merged.hdrPaperWhiteNits;
  } else if (const float sdrWhite = SdrWhiteLevelNits(config_.target); sdrWhite > 0.0f) {
    dev_.hdrParams.paperWhiteNits = sdrWhite;
  }
  {
    const float peak = MaxLuminanceNits(config_.target);
    dev_.hdrParams.headroom =
        peak > 0.0f ? std::clamp(peak / dev_.hdrParams.paperWhiteNits, 1.0f, 8.0f) : 4.0f;
  }
  dev_.hdrParams.split = merged.nr.splitView;

  auto* direct = dynamic_cast<DirectNrPass*>(dev_.pass.get());
  if (!direct) return true;   // passthrough: nothing else to apply

  direct->SetComposeParams(BridgeFromSettings(merged.nr));

  const auto samePass = [](const NrPassSettings& a, const NrPassSettings& b) {
    return a.preset == b.preset && a.style == b.style && a.intensity == b.intensity &&
           a.localStructure == b.localStructure && a.localTone == b.localTone &&
           a.skinStructure == b.skinStructure && a.autoMask == b.autoMask &&
           a.uiCorrection == b.uiCorrection;
  };
  const auto before = previous.nr.Effective();
  const auto after = merged.nr.Effective();
  const bool setupChanged =
      merged.nr.modelScale != previous.nr.modelScale ||
      merged.nr.finalPassFull != previous.nr.finalPassFull ||
      merged.nr.chainComposed != previous.nr.chainComposed ||
      before.size() != after.size() ||
      !std::equal(after.begin(), after.end(), before.begin(), samePass);
  if (setupChanged) direct->RequestPassSetup(SetupFromSettings(merged.nr));
  return true;
}

void Pipeline::SetActivePreset(const std::string& name) {
  std::lock_guard<std::mutex> lock(presetMutex_);
  activePreset_ = name;
}

void Pipeline::OnForegroundChanged(HWND foreground) {
  // The game, or any window of this process -- the overlay in configure mode,
  // or the HUD if something ever activates it. Anything else is the operator
  // going somewhere and the overlay steps aside until they come back.
  DWORD pid = 0;
  if (foreground) GetWindowThreadProcessId(foreground, &pid);
  const bool ours = pid != 0 && pid == GetCurrentProcessId();
  const bool game = foreground != nullptr && foreground == config_.target;
  const bool focused = game || ours;
  if (focused == gameFocused_) return;
  gameFocused_ = focused;
  ApplyVisibility();
}

void Pipeline::SetOverlayInteractive(bool on) {
  overlayInteractive_.store(on, std::memory_order_release);
  if (!dev_.overlay) return;
  if (on) {
    // The request comes from the manager, so the manager has focus and the
    // overlay is hidden -- and a hidden window cannot take the foreground.
    // Show it first; the foreground event that follows keeps it shown.
    gameFocused_ = true;
    ApplyVisibility();
  }
  dev_.overlay->SetInteractive(on);
  if (!on && config_.target && IsWindow(config_.target)) {
    // The overlay held the foreground a moment ago, which is what entitles this
    // process to give it away -- and the game is where it belongs.
    SetForegroundWindow(config_.target);
  }
  GlobalLog().Info(on ? "configure mode: the overlay is taking mouse and keyboard"
                      : "configure mode off: click-through restored, foreground to the game");
}

void Pipeline::Panic() noexcept {
  // Overlay first, always. The player must be able to see the game again
  // immediately, before any slower teardown happens.
  if (dev_.overlay) dev_.overlay->Hide();
  if (dev_.hud) dev_.hud->Hide();
  overlayInteractive_.store(false, std::memory_order_release);
  stopRequested_.store(true, std::memory_order_release);
}

bool Pipeline::Rebuild() {
  // Device loss invalidates both devices, the shared ring, the swapchain and
  // every pipeline state, so the only correct response is to build all of it
  // again from the adapter up.
  if (dev_.overlay) dev_.overlay->Hide();

  // The GPU must be finished with everything before any of it is released.
  // The accelerator session is deliberately NOT torn down early here: it comes
  // apart inside ~NvofaFlow, after the textures it registered are gone, which
  // is the only ordering the driver survives.
  DrainGpu();

  // A caller-supplied pass holds no device resources and is carried across. One
  // built from config may hold plenty -- an NGX session, a depth plane -- all of
  // it belonging to the device that just went away, so it is dropped here and
  // rebuilt against the new device by Create().
  auto pass = passFromConfig_ ? nullptr : std::move(dev_.pass);

  // Move out and let the temporary die, rather than assigning an empty state
  // over the top. Move-assignment releases the old members in *declaration*
  // order, which would free the bridge first, while the capture source and
  // overlay still point into it. Destruction runs in reverse, which is the
  // order the dependencies actually require.
  {
    DeviceState dying = std::move(dev_);
  }

  auto rebuilt = Pipeline::Create(gpu_, config_, std::move(pass));
  if (!rebuilt) return false;

  // One move, so a member added later cannot be forgotten here. Stats and the
  // panic switch deliberately stay behind: a reset must not erase the latency
  // history the operator is reading.
  //
  // The HUD comes across with everything else, which matters because
  // Hud::Create points a file-scope pointer at the newest instance; leaving
  // the old one in place would stop it painting.
  dev_ = std::move(rebuilt->dev_);
  return true;
}

bool Pipeline::RebuildAndRestart() {
  // Must run on the thread that owns the windows: Rebuild creates a new
  // overlay, and a window belongs to its creating thread.
  Stop();

  // Retry, because a real driver reset takes time to finish and
  // D3D12CreateDevice can refuse the adapter until it has. This is defensive
  // and deliberately unverified: the only way to simulate removal in-process
  // is ID3D12Device5::RemoveDevice, which poisons the adapter permanently, so
  // no amount of retrying recovers from it and it cannot exercise this path.
  //
  // Blocking the owner's message loop for up to a second is acceptable here:
  // the overlay is already hidden, and the game is a separate process that
  // carries on regardless.
  constexpr int kAttempts = 5;
  constexpr auto kDelay = std::chrono::milliseconds(200);
  bool rebuilt = false;
  for (int attempt = 0; attempt < kAttempts && !rebuilt; ++attempt) {
    if (attempt > 0) std::this_thread::sleep_for(kDelay);
    rebuilt = Rebuild();
  }
  if (!rebuilt) {
    FailAndHide("graphics device was reset and could not be rebuilt");
    return false;
  }
  rebuildRequested_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_.clear();
  }
  Start();
  return true;
}

ID3D12Device* Pipeline::DeviceForTest() const {
  return dev_.bridge ? dev_.bridge->D3d12() : nullptr;
}

void Pipeline::Start() {
  // Gate on the thread, not on running_: the render loop clears running_ by
  // itself when a frame fails, and a second Start() must not leave the first
  // thread unjoined.
  if (renderThread_.joinable()) return;
  // Recorded here because Start() is called from the thread that created the
  // windows; the render loop needs it to wake that thread on device loss.
  ownerThreadId_ = GetCurrentThreadId();
  stopRequested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  dev_.source->Start();
  // Watch the foreground from the owner thread, whose message loop delivers
  // the hook. Created once; a rebuild reuses it. Seed the state from whatever
  // is foreground right now, since the first event only arrives on a change.
  if (!foreground_) {
    foreground_ = ForegroundWatcher::Create([this](HWND hwnd) { OnForegroundChanged(hwnd); });
    if (!foreground_) {
      GlobalLog().Warn("could not watch the foreground window; the overlay will stay "
                       "up over other applications until hidden by hand");
    }
  }
  {
    // Seed from the real foreground. At launch that is usually the manager,
    // which means the overlay starts hidden and appears the instant the runtime
    // hands focus to the game a few lines later in main -- the event for that
    // hand-off is what shows it. If the hand-off fails, the overlay stays down
    // until the player clicks into the game, which is the right outcome: the
    // alternative is an opaque window over whatever Windows chose instead.
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    gameFocused_ = (fg != nullptr && fg == config_.target) ||
                   (pid != 0 && pid == GetCurrentProcessId());
  }
  // The visibility atomics, not the config: after a rebuild this has to restore
  // what the operator last asked for, which may not be what the file says.
  // Configure mode is the exception: a new overlay is always click-through.
  overlayInteractive_.store(false, std::memory_order_release);
  ApplyVisibility();
  renderThread_ = std::thread([this] { RenderLoop(); });
}

void Pipeline::Stop() {
  // Always join if there is a thread. The loop may already have exited on a
  // failure and cleared running_, and an unjoined std::thread terminates the
  // process when it is destroyed.
  stopRequested_.store(true, std::memory_order_release);
  if (renderThread_.joinable()) renderThread_.join();
  if (dev_.source) dev_.source->Stop();
  DrainGpu();
  if (dev_.overlay) dev_.overlay->Hide();
  if (dev_.hud) dev_.hud->Hide();
  running_.store(false, std::memory_order_release);
}

void Pipeline::DrainGpu() {
  // Destroying a resource the GPU is still reading is undefined, and with
  // optical flow in the mix it reliably crashed: NVOFA runs on its own queue,
  // so work can still be in flight after the render thread has stopped.
  if (dev_.flow) dev_.flow->WaitForIdle();
  if (!dev_.bridge) return;

  auto* device = dev_.bridge->D3d12();
  if (device->GetDeviceRemovedReason() != S_OK) return;   // never signals again

  ComPtr<ID3D12Fence> drain;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)))) return;
  if (FAILED(dev_.bridge->Queue()->Signal(drain.Get(), 1))) return;
  if (drain->GetCompletedValue() >= 1) return;

  HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!evt) return;
  if (SUCCEEDED(drain->SetEventOnCompletion(1, evt))) {
    WaitForSingleObject(evt, 2000);
  }
  CloseHandle(evt);
}

void Pipeline::RenderLoop() {
  // RegisterHotKey binds the hotkey to the registering thread's message queue
  // and Pump drains that same queue, so both have to happen here rather than
  // on whichever thread called Start().
  panic_ = PanicSwitch::Create([this] { Panic(); });

  uint64_t sinceHudUpdate = 0;
  uint64_t hudUpdates = 0;
  auto lastHitchReport = Clock::now();
  // What the last performance line said, so the next one is only written when
  // something actually changed. A line every ten seconds saying the same thing
  // is what made these logs unreadable.
  std::string lastPerfLine;
  auto lastPerfReport = Clock::now();
  // Wall-clock interval between presents, the last two seconds' worth. This is
  // the frame rate the eye sees, and the shape of it -- a hitch, a comb -- is
  // what the HUD's graph draws.
  std::array<float, HudModel::kHistory> intervals{};
  int intervalHead = 0;
  int intervalCount = 0;
  auto lastPresent = Clock::now();
  bool presentedBefore = false;
  // Accumulated across the reporting window rather than sampled, because the
  // interesting question is where the frame budget goes on average, not what
  // one arbitrary frame did.
  double idleMs = 0.0;
  double recordMs = 0.0;
  double presentWaitMs = 0.0;
  double gpuWaitMs = 0.0;
  uint64_t framesThisWindow = 0;
  uint64_t deliveredAtWindowStart = dev_.source ? dev_.source->FramesDelivered() : 0;
  auto windowBegan = Clock::now();
  while (!stopRequested_.load(std::memory_order_acquire)) {
    if (panic_) panic_->Pump();
    if (panic_ && panic_->Triggered()) break;

    const HRESULT reason = dev_.bridge->D3d12()->GetDeviceRemovedReason();
    if (IsDeviceLost(reason)) {
      // Hide first, then hand the rebuild to the owner thread. Rebuilding here
      // would create the overlay window on this thread, which never pumps, so
      // the new window could never be shown or moved.
      FailAndHide("graphics device was reset; waiting for a rebuild");
      rebuildRequested_.store(true, std::memory_order_release);
      // Wake the owner's GetMessage so it notices without polling on a timer.
      if (ownerThreadId_ != 0) PostThreadMessageW(ownerThreadId_, WM_NULL, 0, 0);
      break;
    }

    if (dev_.source->IsClosed()) {
      FailAndHide("capture item closed: the target window went away");
      // Tell the owner, and wake it so it acts now rather than whenever the
      // next message happens to arrive. Without this the process sits in its
      // message loop with a dead render thread, still answering the manager as
      // though it were running.
      targetLost_.store(true, std::memory_order_release);
      if (ownerThreadId_ != 0) PostThreadMessageW(ownerThreadId_, WM_NULL, 0, 0);
      break;
    }

    // Time spent here is time the pipeline had no work: the capture has not
    // produced a new frame yet. It belongs in the breakdown because "starved by
    // the game" and "too slow to keep up" look identical from a frame counter
    // and have opposite fixes.
    const auto beginAcquire = Clock::now();
    // Block until capture publishes rather than polling: a 1 ms sleep in a
    // process that has not raised the timer resolution lasts until the next
    // 15.6 ms tick, which caps the overlay far below the capture rate. The
    // timeout only exists so the loop can notice a stop request.
    dev_.bridge->WaitForFrame(8);
    auto frame = dev_.bridge->AcquireLatest();
    if (!frame) {
      idleMs += std::chrono::duration<double, std::milli>(Clock::now() - beginAcquire).count();
      continue;
    }

    // Nothing to show while the app is not in the foreground: the overlay is
    // hidden and the model's work would go nowhere. Consume the frame so the
    // ring keeps turning and give the GPU back to whatever the player is
    // doing instead. Not counted as a drop: nothing was owed.
    if (paused_.load(std::memory_order_acquire)) {
      // Still count what capture delivers, so the HUD and the manager keep
      // reporting the capture rate while the overlay is hidden -- the one
      // number that matters in exactly that state.
      static auto lastPausedReport = Clock::now();
      static uint64_t deliveredAtPausedReport = 0;
      const auto now = Clock::now();
      const double seconds = std::chrono::duration<double>(now - lastPausedReport).count();
      if (seconds >= 0.5) {
        const uint64_t delivered = dev_.source ? dev_.source->FramesDelivered() : 0;
        HudModel model;
        model.captureFps = deliveredAtPausedReport
                               ? static_cast<double>(delivered - deliveredAtPausedReport) / seconds
                               : 0.0;
        model.passName = dev_.pass->Name();
        model.gpuName = dev_.gpuName.c_str();
        model.width = dev_.bridge->Width();
        model.height = dev_.bridge->Height();
        model.hdr = dev_.hdr;
        FrameBudget budget;
        budget.captureFps = model.captureFps;
        if (dev_.hud) dev_.hud->Update(model);
        PublishStatus(model, budget);
        deliveredAtPausedReport = delivered;
        lastPausedReport = now;
        deliveredAtWindowStart = delivered;
      }
      Sleep(4);   // idle: the exact interval does not matter here
      continue;
    }

    const auto begin = Clock::now();

    // Phase one: everything NVOFA depends on. It runs on its own queue, so its
    // inputs have to be submitted and fenced before it is asked to start.
    dev_.alloc->Reset();
    dev_.cmdList->Reset(dev_.alloc.Get(), nullptr);

    // The SDR view of the frame: the capture widened into typed RGBA16F, or on
    // an HDR desktop the scRGB capture tone-mapped down. Everything downstream
    // that thinks in display-referred terms -- the flow's luminance, the neural
    // pass -- reads this rather than the raw frame.
    if (dev_.hdr && dev_.hdrBridge && dev_.normalized) {
      dev_.hdrBridge->RecordToSdr(dev_.cmdList.Get(), frame->texture, dev_.normalized.Get(),
                                  dev_.hdrParams);
    } else if (dev_.normalize && dev_.normalized) {
      dev_.normalize->Record(dev_.cmdList.Get(), frame->texture, dev_.normalized.Get());
    }

    // NVOFA consumes GRAYSCALE8, so reduce the frame to luminance before it
    // goes anywhere near the flow accelerator. In HDR the luminance comes from
    // the tone-mapped view: flow on scRGB values would see the highlights the
    // model never does. The texture rests in COMMON because another queue
    // reads it, and D3D12 requires COMMON for cross-queue access.
    const bool haveLuma = dev_.luminance && dev_.currentLuma && dev_.previousLuma;
    if (haveLuma) {
      D3D12_RESOURCE_BARRIER toWriteLuma{};
      toWriteLuma.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      toWriteLuma.Transition.pResource = dev_.currentLuma.Get();
      toWriteLuma.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      toWriteLuma.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      toWriteLuma.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      dev_.cmdList->ResourceBarrier(1, &toWriteLuma);

      const bool lumaFromSdrView = dev_.hdr && dev_.normalized;
      D3D12_RESOURCE_BARRIER viewToRead{};
      if (lumaFromSdrView) {
        viewToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        viewToRead.Transition.pResource = dev_.normalized.Get();
        viewToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        viewToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        viewToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        dev_.cmdList->ResourceBarrier(1, &viewToRead);
      }
      dev_.luminance->Record(dev_.cmdList.Get(),
                             lumaFromSdrView ? dev_.normalized.Get() : frame->texture,
                             dev_.currentLuma.Get());
      if (lumaFromSdrView) {
        D3D12_RESOURCE_BARRIER viewBack = viewToRead;
        viewBack.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        viewBack.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dev_.cmdList->ResourceBarrier(1, &viewBack);
      }

      D3D12_RESOURCE_BARRIER toCommon = toWriteLuma;
      toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      dev_.cmdList->ResourceBarrier(1, &toCommon);
    }

    dev_.cmdList->Close();
    dev_.bridge->Queue()->Wait(dev_.bridge->SharedFence(), frame->fenceValue);
    ID3D12CommandList* first[] = {dev_.cmdList.Get()};
    dev_.bridge->Queue()->ExecuteCommandLists(1, first);

    // A missing motion field is not a frame failure: the pass receives null and
    // treats the scene as static for this frame.
    ID3D12Resource* motion = nullptr;
    FlowOutput flowOut{};
    bool haveFlow = false;
    if (dev_.flow && dev_.flow->Available() && haveLuma && dev_.havePreviousFrame) {
      // Tell NVOFA which fence value means "the luminance is written".
      dev_.bridge->Queue()->Signal(dev_.flow->InputFence(), ++dev_.inputFenceValue);
      haveFlow = dev_.flow->Execute(dev_.currentLuma.Get(), dev_.previousLuma.Get(),
                                dev_.inputFenceValue, flowOut);
    }

    // Phase two: consume the flow grid and present. The GPU waits on NVOFA's
    // output fence, so the render thread never blocks.
    dev_.alloc2->Reset();
    dev_.cmdList2->Reset(dev_.alloc2.Get(), nullptr);

    if (haveFlow && dev_.flowToMv && dev_.motionTarget) {
      dev_.bridge->Queue()->Wait(dev_.flow->OutputFence(), flowOut.readyFenceValue);
      dev_.flowToMv->Record(dev_.cmdList2.Get(), flowOut, dev_.motionTarget.Get());
      motion = dev_.motionTarget.Get();
    }

    // With no mask configured this is exactly the path it always was: the pass
    // writes dev_.workTarget and the overlay presents it. The masked path adds
    // one indirection -- the pass writes a scratch target, and the blend
    // composes that against the original frame into workTarget.
    const bool masked = dev_.uiMask && dev_.neuralTarget;

    // The mask texture is uploaded once, not per frame: it only changes when the
    // configuration does. cmdList2 is open here, which is the cheapest place to
    // record the copy.
    if (masked && !dev_.maskUploaded) {
      std::vector<MaskRect> rects;
      rects.reserve(config_.uiMaskRects.size());
      for (const auto& r : config_.uiMaskRects) {
        rects.push_back(MaskRect{r.left, r.top, r.right, r.bottom});
      }
      const uint32_t sourceWidth =
          config_.uiMaskWidth ? config_.uiMaskWidth : dev_.bridge->Width();
      const uint32_t sourceHeight =
          config_.uiMaskHeight ? config_.uiMaskHeight : dev_.bridge->Height();
      dev_.uiMask->Rasterise(dev_.cmdList2.Get(), rects, sourceWidth, sourceHeight,
                             config_.uiMaskFeather);
      dev_.maskUploaded = true;
    }

    // A pass that names an output format writes the intermediate target and
    // reads the widened frame; one that does not writes the work target and
    // reads the captured frame directly. With the resolve folded in, the direct
    // pass reads the widened frame but writes the work target itself.
    const bool resolving = dev_.pass->OutputFormat() != DXGI_FORMAT_UNKNOWN;
    const bool viaIntermediate = resolving && !dev_.directResolve;
    ID3D12Resource* passTarget =
        (masked || viaIntermediate) ? dev_.neuralTarget.Get() : dev_.workTarget.Get();
    ID3D12Resource* passColor =
        resolving ? dev_.normalized.Get() : frame->texture;

    // Each pass declares the state it needs its output in: PassthroughPass
    // copies and wants COPY_DEST, anything driving NGX writes a UAV. The
    // intermediate target rests in UNORDERED_ACCESS, the work target in
    // COPY_SOURCE, because that is how each is consumed afterwards.
    const D3D12_RESOURCE_STATES restState =
        (masked || viaIntermediate) ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                    : D3D12_RESOURCE_STATE_COPY_SOURCE;
    const D3D12_RESOURCE_STATES writeState = dev_.pass->OutputState();

    D3D12_RESOURCE_BARRIER toWrite{};
    toWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toWrite.Transition.pResource = passTarget;
    toWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toWrite.Transition.StateBefore = restState;
    toWrite.Transition.StateAfter = writeState;
    if (restState != writeState) dev_.cmdList2->ResourceBarrier(1, &toWrite);

    const bool ok = dev_.pass->Evaluate(dev_.cmdList2.Get(), passColor,
                                    motion, nullptr, passTarget);

    // Back to rest, then on to whatever reads it: the blend samples the
    // intermediate target, the overlay copies from the work target.
    if (restState != writeState) {
      D3D12_RESOURCE_BARRIER back = toWrite;
      back.Transition.StateBefore = writeState;
      back.Transition.StateAfter = restState;
      dev_.cmdList2->ResourceBarrier(1, &back);
    }

    D3D12_RESOURCE_BARRIER toRead = toWrite;
    toRead.Transition.StateBefore = restState;
    toRead.Transition.StateAfter =
        (masked || viaIntermediate) ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                    : D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (toRead.Transition.StateBefore != toRead.Transition.StateAfter) {
      dev_.cmdList2->ResourceBarrier(1, &toRead);
    }

    if (masked) {
      // workTarget becomes the blend's UAV output, then goes back to
      // COPY_SOURCE for the present. neuralTarget returns to COPY_SOURCE so the
      // next frame starts from the state this one assumed.
      D3D12_RESOURCE_BARRIER toBlend{};
      toBlend.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      toBlend.Transition.pResource = dev_.workTarget.Get();
      toBlend.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      toBlend.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      toBlend.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      dev_.cmdList2->ResourceBarrier(1, &toBlend);

      if (dev_.hdr && dev_.hdrBridge && resolving) {
        // HDR: the model's SDR result is applied to the scRGB original as a
        // ratio against the SDR view it was shown. The view rests in UAV; the
        // compose reads it.
        D3D12_RESOURCE_BARRIER viewToRead = toBlend;
        viewToRead.Transition.pResource = dev_.normalized.Get();
        viewToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        viewToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        dev_.cmdList2->ResourceBarrier(1, &viewToRead);
        dev_.hdrBridge->RecordCompose(dev_.cmdList2.Get(), frame->texture, dev_.normalized.Get(),
                                      dev_.neuralTarget.Get(), dev_.workTarget.Get(),
                                      dev_.hdrParams);
        D3D12_RESOURCE_BARRIER viewBack = viewToRead;
        viewBack.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        viewBack.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dev_.cmdList2->ResourceBarrier(1, &viewBack);
      } else {
        dev_.uiMask->Record(dev_.cmdList2.Get(), frame->texture,
                            dev_.neuralTarget.Get(), dev_.workTarget.Get());
      }

      D3D12_RESOURCE_BARRIER after[2]{};
      after[0] = toBlend;
      after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      after[1] = toRead;
      after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      // Back to whatever this target rests in, which depends on how the pass
      // writes it -- not unconditionally COPY_SOURCE.
      after[1].Transition.StateAfter = restState;
      dev_.cmdList2->ResourceBarrier(2, after);
    }

    dev_.cmdList2->Close();
    if (!ok) {
      // Spec section 11: an unusable neural pass degrades to passthrough and
      // says so. It does not hide the overlay -- that is reserved for failures
      // that break the frame itself.
      //
      // The list is deliberately not executed: after a failed pass its recorded
      // barriers no longer describe reality. Nothing was submitted, so the GPU's
      // actual states still match what the next frame will assume. Rebuilding
      // reuses the device-loss path, which already drains and reconstructs on
      // the thread that owns the windows.
      GlobalLog().Error("the neural pass rejected a frame; rebuilding on passthrough.");
      {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = "neural pass failed; fell back to passthrough";
      }
      config_.neuralPass = "passthrough";
      passFromConfig_ = true;
      rebuildRequested_.store(true, std::memory_order_release);
      if (ownerThreadId_ != 0) PostThreadMessageW(ownerThreadId_, WM_NULL, 0, 0);
      break;
    }

    ID3D12CommandList* lists[] = {dev_.cmdList2.Get()};
    dev_.bridge->Queue()->ExecuteCommandLists(1, lists);
    const auto afterRecord = Clock::now();

    dev_.overlay->Present(dev_.workTarget.Get(), frame->fenceValue);
    recordMs += std::chrono::duration<double, std::milli>(afterRecord - begin).count();
    {
      const auto present = dev_.overlay->LastTiming();
      presentWaitMs += present.latencyWaitMs;
      gpuWaitMs += present.gpuWaitMs;
      const auto now = Clock::now();
      if (presentedBefore) {
        intervals[static_cast<size_t>(intervalHead)] =
            static_cast<float>(std::chrono::duration<double, std::milli>(now - lastPresent).count());
        intervalHead = (intervalHead + 1) % HudModel::kHistory;
        intervalCount = std::min(intervalCount + 1, HudModel::kHistory);
      }
      lastPresent = now;
      presentedBefore = true;
    }

    if (dumpRequested_.exchange(false, std::memory_order_acq_rel)) DumpDebugFrames();
    if (const int step = calibrationRequest_.exchange(0, std::memory_order_acq_rel)) {
      CalibrationCapture(step);
    }

    // This frame's luminance becomes next frame's reference. Swapping rather
    // than copying keeps both textures alive and costs nothing.
    if (haveLuma) {
      dev_.currentLuma.Swap(dev_.previousLuma);
      dev_.havePreviousFrame = true;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin);
    stats_.Record(elapsed.count());
    ++framesThisWindow;

    // A hitch is invisible in a ten-second average, and it is the thing an
    // operator actually notices. Report one when a frame takes several times
    // the running median, with the phase that was slow -- rate-limited so a
    // sustained bad patch does not become a wall of text.
    {
      const double median = stats_.P50();
      const auto now = Clock::now();
      const double sinceLast =
          std::chrono::duration<double>(now - lastHitchReport).count();
      if (median > 0.5 && elapsed.count() > std::max(3.0 * median, 25.0) && sinceLast > 2.0) {
        const auto present = dev_.overlay->LastTiming();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "hitch: one frame took %.1f ms against a %.1f ms median "
                      "(gpu %.1f, cpu %.1f, present wait %.1f)",
                      elapsed.count(), median, present.gpuWaitMs, present.recordMs,
                      present.latencyWaitMs);
        GlobalLog().Warn(line);
        lastHitchReport = now;
      }
    }

    // Refresh a few times a second rather than every frame. Not gated on the
    // HUD existing: the manager's status pane rides the same cadence, and a
    // failed HUD must not take the live numbers down with it.
    if (++sinceHudUpdate >= 12) {
      sinceHudUpdate = 0;

      HudModel model;
      model.p50Ms = stats_.P50();
      model.p99Ms = stats_.P99();
      model.frames = stats_.Count();
      model.drops = stats_.Dropped();
      model.passName = dev_.pass->Name();
      model.runtimeVariant = dev_.runtimeVariant;
      model.gpuName = dev_.gpuName.c_str();
      model.width = dev_.bridge->Width();
      model.height = dev_.bridge->Height();
      model.hdr = dev_.hdr;
      model.temporal = config_.nr.temporalSmoothing > 0.0f;
      if (auto* direct = dynamic_cast<DirectNrPass*>(dev_.pass.get())) {
        model.modelWidth = direct->WorkWidth();
        model.modelHeight = direct->WorkHeight();
        model.passCount = static_cast<int>(direct->PassCount());
      }
      std::string presetName;
      {
        std::lock_guard<std::mutex> lock(presetMutex_);
        presetName = activePreset_;
      }
      model.preset = presetName.c_str();
      // The graph, oldest first, and the on-screen rate from the same numbers:
      // frames over the time they took.
      double intervalSum = 0.0;
      for (int i = 0; i < intervalCount; ++i) {
        const int index = (intervalHead - intervalCount + i + HudModel::kHistory) % HudModel::kHistory;
        model.intervalMs[static_cast<size_t>(i)] = intervals[static_cast<size_t>(index)];
        intervalSum += intervals[static_cast<size_t>(index)];
      }
      model.intervalCount = intervalCount;
      model.fps = intervalSum > 0.0 ? 1000.0 * intervalCount / intervalSum : 0.0;
      // Per-frame averages over the window so far, for the HUD's columns.
      {
        const double perFrameSoFar = static_cast<double>(framesThisWindow ? framesThisWindow : 1);
        model.gpuMs = gpuWaitMs / perFrameSoFar;
        model.cpuMs = recordMs / perFrameSoFar;
      }

      // Averages over the window just ended. Frame rate is measured against the
      // clock rather than derived from p50: the two differ by exactly the time
      // the pipeline spent idle waiting for the game, and that difference is
      // the whole diagnosis.
      const auto now = Clock::now();
      const double windowSeconds = std::chrono::duration<double>(now - windowBegan).count();
      const double perFrame = static_cast<double>(framesThisWindow ? framesThisWindow : 1);

      // How fast Windows Graphics Capture is actually handing us frames, as
      // against how fast we present them. These are different questions and
      // only one of them is ours: if capture is delivering 60 a second, no
      // amount of pipeline work will present more than 60, and the frame budget
      // below will show idle time rather than a cost to optimise.
      const uint64_t delivered = dev_.source ? dev_.source->FramesDelivered() : 0;
      const uint64_t deliveredThisWindow = delivered - deliveredAtWindowStart;
      deliveredAtWindowStart = delivered;

      FrameBudget budget;
      budget.fps =
          windowSeconds > 0.0 ? static_cast<double>(framesThisWindow) / windowSeconds : 0.0;
      budget.captureFps = windowSeconds > 0.0
                              ? static_cast<double>(deliveredThisWindow) / windowSeconds
                              : 0.0;
      budget.idleMs = idleMs / perFrame;
      budget.recordMs = recordMs / perFrame;
      budget.presentWaitMs = presentWaitMs / perFrame;
      budget.gpuWaitMs = gpuWaitMs / perFrame;
      model.captureFps = budget.captureFps;
      if (dev_.hud) dev_.hud->Update(model);
      PublishStatus(model, budget);

      // Periodically to the log, but only when the picture has changed. The
      // line is bucketed before it is compared, so ordinary jitter does not
      // count as news; a minute passing does, so a quiet session still leaves
      // a heartbeat to anchor the timestamps against.
      {
        const auto bucket = [](double value, double step) {
          return static_cast<int>(value / step) * static_cast<int>(step);
        };
        char summary[256];
        std::snprintf(summary, sizeof(summary),
                      "%d fps presented, %d captured, gpu %d ms, %s",
                      bucket(budget.fps, 10.0), bucket(budget.captureFps, 10.0),
                      bucket(budget.gpuWaitMs, 2.0), dev_.pass->Name());
        const bool stale = std::chrono::duration<double>(now - lastPerfReport).count() > 60.0;
        if (summary != lastPerfLine || stale) {
          char line[384];
          std::snprintf(line, sizeof(line),
                        "%.0f fps presented, %.0f captured | per frame: gpu %.1f, cpu %.1f, "
                        "idle %.1f, present %.1f ms | p50 %.1f, p99 %.1f | %llu dropped",
                        budget.fps, budget.captureFps, budget.gpuWaitMs, budget.recordMs,
                        budget.idleMs, budget.presentWaitMs, stats_.P50(), stats_.P99(),
                        static_cast<unsigned long long>(stats_.Dropped()));
          GlobalLog().Info(line);
          lastPerfLine = summary;
          lastPerfReport = now;
        }
      }
      ++hudUpdates;

      idleMs = 0.0;
      recordMs = 0.0;
      presentWaitMs = 0.0;
      gpuWaitMs = 0.0;
      framesThisWindow = 0;
      windowBegan = now;
    }
  }
  // UnregisterHotKey must run on the thread that registered it.
  panic_.reset();
  running_.store(false, std::memory_order_release);
}

}  // namespace sidecar
