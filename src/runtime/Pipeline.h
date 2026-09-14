#pragma once
#include <windows.h>

#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/Config.h"
#include "core/ControlChannel.h"
#include "core/GpuProfile.h"
#include "core/LatencyStats.h"
#include "core/PanicSwitch.h"
#include "flow/FlowToMotionVec.h"
#include "flow/NvofaFlow.h"
#include "gpu/FormatNormalize.h"
#include "gpu/HdrBridge.h"
#include "gpu/Luminance.h"
#include "gpu/UiMask.h"
#include "neural/INeuralPass.h"
#include "present/Hud.h"
#include "present/WindowTracker.h"

namespace sidecar {

class DeviceBridge;
class WgcSource;
class DCompOverlay;

struct PipelineConfig {
  HWND target = nullptr;
  bool showOverlay = true;
  bool showHud = true;
  // Pixels per flow vector. Validated to 1, 2 or 4 by Config before it gets
  // here; NvofaFlow refuses anything else.
  uint32_t flowGridSize = 4;
  // Screen-space rectangles the neural pass must leave alone, in the resolution
  // the operator calibrated at. Empty means no masking, and the mask pass is
  // then not created at all.
  std::vector<UiRect> uiMaskRects;
  uint32_t uiMaskWidth = 0;
  uint32_t uiMaskHeight = 0;
  int32_t uiMaskFeather = 4;

  // Which pass to build. Only consulted when Create() is handed a null pass,
  // which is how the runtime asks for one -- a device-backed pass cannot be
  // constructed before the device exists, and must be rebuilt with it after
  // device loss. "direct" or "passthrough"; a pass that cannot be built
  // degrades to passthrough with a logged reason.
  std::string neuralPass = "direct";
  // The constant filling the synthetic depth plane, or a ground-plane
  // gradient; and whether the runtime should read near as the high value.
  float syntheticDepth = 0.0f;
  bool depthGradient = false;
  bool depthInverted = false;
  // The direct path's settings. Only consulted when neuralPass is "direct".
  NrSettings nr;
  // Where nvngx_*.dll live. Defaults to the executable's own directory.
  std::filesystem::path runtimeDir;
  // The look these settings came from, for the status readout.
  std::string activePreset;
  // HDR: how an HDR capture is tone-mapped for the model and how its edit is
  // applied back. Only consulted when the target's display is in HDR mode,
  // which the pipeline decides for itself at creation.
  float hdrPaperWhiteNits = 0.0f;   // 0 = automatic
};

// Owns the render thread and the per-frame orchestration: acquire the newest
// captured frame, run the neural pass, present through the overlay.
//
// Pacing is latest-wins. DeviceBridge already overwrites unconsumed slots; the
// pipeline records each overwrite as a drop and never builds a queue.
class Pipeline {
 public:
  static std::unique_ptr<Pipeline> Create(const GpuInfo& gpu,
                                          const PipelineConfig& config,
                                          std::unique_ptr<INeuralPass> pass);
  ~Pipeline();

  void Start();
  void Stop();
  bool Running() const { return running_.load(std::memory_order_acquire); }

  const LatencyStats& Stats() const { return stats_; }
  HWND OverlayHwnd() const;
  const Hud* GetHud() const { return dev_.hud.get(); }
  std::string LastError() const;

  // Show and hide either window without stopping capture, which is what makes
  // an A/B comparison against the untouched game one keystroke rather than one
  // restart. Call from the thread that called Start(): these move windows.
  void SetOverlayVisible(bool visible);
  void SetHudVisible(bool visible);
  bool OverlayVisible() const { return overlayVisible_.load(std::memory_order_acquire); }
  bool HudVisible() const { return hudVisible_.load(std::memory_order_acquire); }

  // Configure mode: the overlay takes input so the ReShade UI inside this
  // process can be driven. Leaving it hands the foreground back to the game.
  // Never restored across a rebuild -- a fresh overlay always comes up
  // click-through, because that is the only state that is safe by default.
  // Call from the thread that called Start().
  void SetOverlayInteractive(bool on);
  bool OverlayInteractive() const { return overlayInteractive_.load(std::memory_order_acquire); }

  // The foreground changed. The overlay is only shown while the game -- or, in
  // configure mode, the overlay itself -- is what has focus; otherwise it gets
  // out of the way so the manager and everything else stay reachable. Runs on
  // the owner thread, from the watcher's hook.
  void OnForegroundChanged(HWND foreground);

  // Asks the render loop to write the neural pass's input and output, and the
  // presented frame, to BMPs beside the sidecar on its next frame, and to log
  // how much the pass changed the picture. Safe from any thread.
  void RequestDebugDump() { dumpRequested_.store(true, std::memory_order_release); }

  // The UI-mask calibrator's two captures. Step 1 stores the frame with the
  // interface drawn; step 2 diffs the current frame against it and writes the
  // result beside the sidecar. Safe from any thread.
  void RequestCalibrationCapture(int step) {
    calibrationRequest_.store(step, std::memory_order_release);
  }

  // Applies a new configuration to the running pipeline where it can: the
  // direct pass's compose parameters and pass setup are handed over live.
  // Returns false when something changed that needs a rebuild -- the caller
  // then calls RebuildAndRestart, which reads the config stored here. Owner
  // thread.
  bool ApplySettings(const PipelineConfig& config);

  // The name shown in the status block. Set by the runtime when a hotkey
  // changes the look; the manager mirrors it into the config file.
  void SetActivePreset(const std::string& name);

  // Which global hotkeys registered, for the status block. Any thread.
  void SetHotkeyMask(uint32_t mask) { hotkeyMask_.store(mask, std::memory_order_release); }

  // Where the manager's live numbers come from.
  //
  // Pushed from the render loop rather than pulled by a poller, because
  // LatencyStats is written on the render thread without a lock -- reading it
  // from anywhere else is a data race for a HUD's worth of benefit. The render
  // loop already assembles these numbers for the HUD, so this rides along at
  // the same cadence and costs a memcpy. Set before Start().
  using StatusSink = std::function<void(const SidecarStatus&)>;
  void SetStatusSink(StatusSink sink) { statusSink_ = std::move(sink); }

  // I13. Hides the overlay before anything else and asks the render loop to
  // stop. Safe to call from the hotkey callback.
  void Panic() noexcept;

  // True once the render loop has seen the device go away. The render thread
  // cannot rebuild by itself: rebuilding creates the overlay window, and a
  // window belongs to the thread that creates it, so it has to be the thread
  // that pumps. Poll this from the owner's message loop.
  bool NeedsRebuild() const { return rebuildRequested_.load(std::memory_order_acquire); }

  // True once the captured window has gone away for good -- the player quit the
  // game, or restarted it. There is nothing to recover: the capture item is
  // bound to a window that no longer exists, and a new game launch is a new
  // window with a new item.
  //
  // The runtime must exit when this happens rather than linger. A lingering
  // process keeps the control channel open, so the manager goes on reporting
  // "Overlay: running" over a render loop that stopped, and the operator is
  // left looking at a stopped overlay that claims to be working.
  bool TargetLost() const { return targetLost_.load(std::memory_order_acquire); }

  // Rebuilds everything and restarts capture. Call only from the thread that
  // called Start(), which is the thread that owns the windows.
  bool RebuildAndRestart();

  ID3D12Device* DeviceForTest() const;

 private:
  Pipeline() = default;
  void RenderLoop();
  void FailAndHide(const char* reason);

  // Reconciles what the operator asked for (overlayVisible_, hudVisible_) with
  // whether the game currently has focus (gameFocused_), and shows or hides the
  // two windows accordingly. Owner thread only: it moves windows.
  void ApplyVisibility();

  // Render thread, after a present. Reads back the widened frame, the pass's
  // output and the presented frame, writes them as BMPs, logs the mean
  // difference the pass made. Stalls the GPU for one frame; only on request.
  void DumpDebugFrames();

  // What a motion dump found, in pixels of displacement per frame. Filled by
  // ReadbackBgr when it renders a motion field; the dump logs it, because the
  // numbers say more than the picture does.
  struct MotionFieldStats {
    float medianPx = 0.0f;
    float p95Px = 0.0f;
    float maxPx = 0.0f;
    // What full colour means in the written BMP. Chosen from the frame's own
    // p95 so the picture is legible whatever the turn rate; it is therefore
    // NOT comparable between dumps without reading this number.
    float fullScalePx = 0.0f;
  };

  // Render thread. Copies one of the pipeline's textures to the CPU as 8-bit
  // BGR, top row first, at the texture's own size (reported through outWidth
  // and outHeight). Float formats are converted per channel with `scale` and
  // `bias` applied first, so a gain map can be written as mid-grey-is-one.
  // `motionNdc` instead reads an RG16F NDC motion field and renders it as a
  // legible picture, auto-scaled, reporting what it found through `outMotion`.
  // Stalls the GPU; only for diagnostics and calibration.
  std::vector<uint8_t> ReadbackBgr(ID3D12Resource* texture, D3D12_RESOURCE_STATES restState,
                                   bool rgba16f, bool scRgb = false, bool motionNdc = false,
                                   uint32_t* outWidth = nullptr,
                                   uint32_t* outHeight = nullptr, float scale = 1.0f,
                                   float bias = 0.0f, MotionFieldStats* outMotion = nullptr);

  // Render thread, after a present. Performs the requested calibration step.
  void CalibrationCapture(int step);

  // One reporting window's worth of "where did the frame go", averaged per
  // frame. Assembled by the render loop, which is the only place that can see
  // all four costs.
  struct FrameBudget {
    double fps = 0.0;
    // What Windows Graphics Capture delivered over the same window. When this
    // is the lower of the two, the ceiling is the capture rate and nothing in
    // the pipeline can raise it.
    double captureFps = 0.0;
    double idleMs = 0.0;
    double recordMs = 0.0;
    double presentWaitMs = 0.0;
    double gpuWaitMs = 0.0;
    // How long our neural command list actually ran on the GPU, from its own
    // timestamps. gpuWaitMs is wall clock and includes time queued behind
    // whatever else the GPU is doing; the difference between the two is
    // contention, and separating them is the only way to tell "our work is
    // expensive" from "our work is waiting".
    double gpuWorkMs = 0.0;
  };

  // Copies one HUD refresh into the shared status block. Render thread only.
  void PublishStatus(const HudModel& model, const FrameBudget& budget);

  // Recreates every device-derived object. Does not start anything; Start()
  // does that, so the two paths cannot drift apart.
  bool Rebuild();

  // Blocks until the GPU has finished with everything this pipeline owns.
  void DrainGpu();

  // Everything derived from the D3D device lives in one movable struct.
  //
  // Rebuild() after device loss has to replace all of it, and the earlier
  // version transferred members by hand -- which silently missed the luminance
  // pass, both luma targets and the second command list when those were added
  // later, leaving the rebuilt pipeline holding objects from a dead device.
  // Moving one struct cannot miss a member, and a new member added for M3
  // is carried across for free.
  //
  // Declaration order is load-bearing, because members are destroyed in
  // reverse: the tracker's callback points at the overlay, the source points
  // at the bridge, and the NVIDIA optical flow driver is fussy about the order
  // its registered textures come apart relative to its session. This order is
  // the one that was verified working on hardware -- do not rearrange it
  // without re-running the [device] suite.
  struct DeviceState {
    std::unique_ptr<DeviceBridge> bridge;
    std::unique_ptr<WgcSource> source;
    std::unique_ptr<DCompOverlay> overlay;
    std::unique_ptr<WindowTracker> tracker;
    std::unique_ptr<Hud> hud;
    std::unique_ptr<INeuralPass> pass;
    std::string gpuName;
    // Names the live neural runtime build for the HUD. Points at a string
    // literal from RuntimeManifest, so it outlives the frame that reads it.
    const char* runtimeVariant = "";

    Microsoft::WRL::ComPtr<ID3D12Resource> workTarget;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    // A second list, because optical flow sits between the two: its inputs
    // must be submitted and fenced before it starts, and its output consumed
    // after.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc2;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList2;
    uint64_t inputFenceValue = 0;

    // GPU timestamps bracketing cmdList2 -- the neural passes and the compose,
    // which is where the cost in question lives. Phase one and the wait on the
    // optical-flow queue sit outside the span deliberately, so a slow NVOFA
    // does not read as expensive neural work.
    //
    // Ring of kTimestampFrames pairs, resolved into a readback buffer and read
    // one frame late, so nothing stalls to collect them. Null when the device
    // would not provide them; the log then reports wait time only.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback;
    uint64_t timestampFrequency = 0;   // ticks per second
    uint64_t timestampFrame = 0;
    double lastGpuWorkMs = 0.0;

    std::unique_ptr<FormatNormalize> normalize;
    std::unique_ptr<Luminance> luminance;
    std::unique_ptr<NvofaFlow> flow;
    std::unique_ptr<FlowToMotionVec> flowToMv;
    Microsoft::WRL::ComPtr<ID3D12Resource> normalized;
    Microsoft::WRL::ComPtr<ID3D12Resource> motionTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> previousLuma;
    Microsoft::WRL::ComPtr<ID3D12Resource> currentLuma;
    bool havePreviousFrame = false;

    // Only created when the operator configured mask rectangles. When absent the
    // render loop takes exactly the path it took before this existed, so an
    // unmasked run pays nothing and cannot regress.
    //
    // Appended at the end deliberately: members are destroyed in reverse, so
    // these come apart before the bridge they were created from.
    std::unique_ptr<UiMask> uiMask;
    Microsoft::WRL::ComPtr<ID3D12Resource> neuralTarget;
    bool maskUploaded = false;

    // HDR capture. When the target's display is in HDR mode the ring, the work
    // target and the swapchain are FP16 scRGB, `normalized` holds the
    // tone-mapped SDR view the model and the flow see, and the compose below
    // puts the model's edit back onto the HDR frame instead of the mask blend.
    bool hdr = false;
    std::unique_ptr<HdrBridge> hdrBridge;
    HdrBridge::Params hdrParams;

    // The resolve folded into the compose: the direct pass writes the
    // presentable BGRA8 target itself, so the mask blend (which doubled as the
    // RGBA16F-to-BGRA8 resolve) is not created and not run. Only on an SDR
    // desktop, with no mask rectangles, on a device that can store typed BGRA8
    // from a compute shader. One full-resolution pass saved per frame.
    bool directResolve = false;
  };
  DeviceState dev_;

  PipelineConfig config_;
  GpuInfo gpu_;
  std::unique_ptr<PanicSwitch> panic_;
  // Outside DeviceState on purpose: it is not device-derived and must survive
  // a rebuild, and it is created on the owner thread in Start(), which is the
  // thread whose message loop delivers its events.
  std::unique_ptr<ForegroundWatcher> foreground_;
  // Whether the game (or this process, in configure mode) has the foreground.
  // Owner thread only.
  bool gameFocused_ = true;
  std::thread renderThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> rebuildRequested_{false};
  std::atomic<bool> targetLost_{false};
  std::atomic<bool> dumpRequested_{false};
  // The number the next dump's filenames carry, so repeated dumps sit beside
  // each other instead of overwriting. Render thread only.
  uint32_t dumpIndex_ = 0;
  // Set while the overlay is not on screen; the render thread idles.
  std::atomic<bool> paused_{false};
  std::atomic<int> calibrationRequest_{0};
  // The calibrator's first capture, and what the status block reports.
  std::vector<uint8_t> calibrationWithUi_;
  std::atomic<uint32_t> calibrationStep_{0};
  std::atomic<uint32_t> calibrationRects_{0};
  std::mutex presetMutex_;
  std::string activePreset_;
  std::atomic<uint32_t> hotkeyMask_{0};
  // True when dev_.pass was built from config_ rather than handed in, and so
  // must be rebuilt against a new device rather than carried across one.
  bool passFromConfig_ = false;
  // The thread that called Start(), and therefore owns the windows.
  DWORD ownerThreadId_ = 0;
  LatencyStats stats_;

  // What the two windows are meant to be doing. Kept outside DeviceState so a
  // rebuild after device loss restores the operator's choice rather than
  // reverting to the configured default.
  std::atomic<bool> overlayVisible_{true};
  std::atomic<bool> hudVisible_{true};
  std::atomic<bool> overlayInteractive_{false};
  StatusSink statusSink_;

  mutable std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace sidecar
