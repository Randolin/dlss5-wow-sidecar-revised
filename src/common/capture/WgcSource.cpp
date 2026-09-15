#include "capture/WgcSource.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gpu/DeviceBridge.h"
#include "core/Log.h"

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace sidecar {

struct WgcSource::Impl {
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::event_token frameToken{};
  winrt::event_token closedToken{};
  DeviceBridge* bridge = nullptr;
  WgcSource::DropCallback onDrop;
  WgcSource* owner = nullptr;
  // Whether this Windows has MinUpdateInterval at all, and what we last asked
  // for, so the render loop can drive it without a call per frame.
  bool canSetInterval = false;
  double intervalMs = 1.0;
};

namespace {

wgdx::Direct3D11::IDirect3DDevice WrapDevice(ID3D11Device* dev) {
  ComPtr<IDXGIDevice> dxgi;
  if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return nullptr;
  winrt::com_ptr<::IInspectable> inspectable;
  if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) return nullptr;
  return inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
}

ComPtr<ID3D11Texture2D> SurfaceToTexture(
    const wgdx::Direct3D11::IDirect3DSurface& surface) {
  auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  ComPtr<ID3D11Texture2D> tex;
  access->GetInterface(IID_PPV_ARGS(&tex));
  return tex;
}

}  // namespace

std::unique_ptr<WgcSource> WgcSource::CreateForWindow(HWND target, DeviceBridge& bridge,
                                                      DropCallback onDrop) {
  if (!wgc::GraphicsCaptureSession::IsSupported()) return nullptr;

  auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem>()
                     .as<IGraphicsCaptureItemInterop>();
  wgc::GraphicsCaptureItem item{nullptr};
  if (FAILED(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                      winrt::put_abi(item)))) {
    return nullptr;
  }

  auto device = WrapDevice(bridge.D3d11());
  if (!device) return nullptr;

  // The frame pool is sized from the capture item, which reports physical
  // pixels whatever this process's DPI mode; the bridge ring was sized from
  // the window's client rect, which is virtualised when the process is
  // DPI-unaware. If the two disagree every Publish() is a CopyResource between
  // mismatched textures, which D3D11 drops without a word, and the overlay is
  // black at full frame rate. Refuse loudly rather than run silently wrong.
  const auto size = item.Size();
  GlobalLog().Info("capture item " + std::to_string(size.Width) + "x" +
                   std::to_string(size.Height) + ", bridge ring " +
                   std::to_string(bridge.Width()) + "x" + std::to_string(bridge.Height()));
  if (static_cast<uint32_t>(size.Width) != bridge.Width() ||
      static_cast<uint32_t>(size.Height) != bridge.Height()) {
    GlobalLog().Error(
        "capture item and bridge ring differ in size; the process is probably "
        "not DPI-aware and the desktop is scaled. Refusing to capture into a "
        "ring that cannot receive the frames.");
    return nullptr;
  }

  std::unique_ptr<WgcSource> s(new WgcSource());
  s->target_ = target;
  s->impl_ = std::make_unique<Impl>();
  s->impl_->owner = s.get();
  s->impl_->bridge = &bridge;
  s->impl_->onDrop = std::move(onDrop);
  s->impl_->item = item;

  // Free-threaded so frames arrive on a pool thread rather than needing a
  // message loop on the capture thread. The pool's pixel format follows the
  // ring's: FP16 for an HDR desktop, so the compositor hands over the scRGB
  // surface intact rather than tone-clipped into 8 bits.
  const bool hdr = bridge.RingFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT;
  s->impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
      device,
      hdr ? wgdx::DirectXPixelFormat::R16G16B16A16Float
          : wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
      static_cast<int32_t>(DeviceBridge::kRingDepth), item.Size());
  GlobalLog().Info(hdr ? "capture: FP16 scRGB (HDR desktop)" : "capture: BGRA8 (SDR desktop)");

  Impl* impl = s->impl_.get();
  impl->frameToken = impl->pool.FrameArrived(
      [impl](const wgc::Direct3D11CaptureFramePool& pool, auto&&) {
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        auto tex = SurfaceToTexture(frame.Surface());
        if (!tex) return;
        bool dropped = impl->bridge->Publish(tex.Get());
        impl->owner->delivered_.fetch_add(1, std::memory_order_relaxed);
        if (dropped && impl->onDrop) impl->onDrop();
      });

  impl->closedToken = item.Closed([impl](auto&&, auto&&) {
    impl->owner->closed_.store(true, std::memory_order_release);
  });

  s->impl_->session = s->impl_->pool.CreateCaptureSession(item);
  s->impl_->session.IsCursorCaptureEnabled(false);   // the app draws its own cursor
  s->impl_->session.IsBorderRequired(false);         // no yellow capture border

  // The throttle that was holding everything to 60. Windows 11 24H2 added a
  // per-session minimum update interval and its default is 16.67 ms -- one
  // frame at 60 Hz -- for window and monitor capture alike, whatever the game
  // or the display are doing. Ask for the display's own period instead. On an
  // older Windows the property does not exist and the default behaviour
  // (compositor-paced) is what it always was.
  if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
          L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
    // 100-nanosecond units: 1 ms. "As fast as the compositor has frames."
    const winrt::Windows::Foundation::TimeSpan interval{10000};
    try {
      s->impl_->session.MinUpdateInterval(interval);
      s->impl_->canSetInterval = true;
      s->impl_->intervalMs = 1.0;
      GlobalLog().Info("capture: minimum update interval set to 1 ms (the OS default is "
                       "16.67 ms, which caps capture at 60 fps); the render loop raises it "
                       "to match what it can actually present");
    } catch (const winrt::hresult_error& e) {
      GlobalLog().Warn("capture: could not set the minimum update interval (" +
                       winrt::to_string(e.message()) + "); capture may be capped at 60 fps");
    }
  } else {
    GlobalLog().Info("capture: this Windows has no MinUpdateInterval; capture is compositor-paced");
  }
  return s;
}

WgcSource::~WgcSource() { Stop(); }

void WgcSource::SetMinUpdateIntervalMs(double ms) {
  if (!impl_ || !impl_->session || !impl_->canSetInterval) return;
  // Never slower than 30 fps of requests, whatever the present rate says: a
  // transient stall must not throttle capture into a hole it cannot climb out
  // of, and a stale frame is worse than a wasted one.
  const double clamped = std::clamp(ms, 1.0, 33.0);
  // Only when it has moved enough to matter. The rate this is derived from
  // jitters by a frame or two, and every set is a call into the compositor.
  if (std::abs(clamped - impl_->intervalMs) < 0.2 * impl_->intervalMs) return;
  const winrt::Windows::Foundation::TimeSpan interval{
      static_cast<int64_t>(clamped * 10000.0)};   // 100 ns units
  try {
    impl_->session.MinUpdateInterval(interval);
    impl_->intervalMs = clamped;
    char line[128];
    std::snprintf(line, sizeof(line),
                  "capture: asking for a frame every %.1f ms (%.0f fps of requests)",
                  clamped, 1000.0 / clamped);
    GlobalLog().Verbose(LogCategory::Capture, line);
  } catch (const winrt::hresult_error& e) {
    // Stop trying rather than log once per update forever.
    impl_->canSetInterval = false;
    GlobalLog().Warn("capture: the minimum update interval was refused (" +
                     winrt::to_string(e.message()) + "); leaving it where it is");
  }
}

bool WgcSource::IsClosed() const {
  if (closed_.load(std::memory_order_acquire)) return true;
  // WGC raises Closed when a window is destroyed normally, but not when the
  // owning process is terminated -- the frames simply stop. Ask the window
  // manager directly rather than waiting for an event that will not arrive.
  return target_ != nullptr && !IsWindow(target_);
}

void WgcSource::Start() {
  if (impl_ && impl_->session) impl_->session.StartCapture();
}

void WgcSource::Stop() {
  if (!impl_) return;
  if (impl_->pool && impl_->frameToken) {
    impl_->pool.FrameArrived(impl_->frameToken);
    impl_->frameToken = {};
  }
  if (impl_->item && impl_->closedToken) {
    impl_->item.Closed(impl_->closedToken);
    impl_->closedToken = {};
  }
  if (impl_->session) { impl_->session.Close(); impl_->session = nullptr; }
  if (impl_->pool) { impl_->pool.Close(); impl_->pool = nullptr; }
}

}  // namespace sidecar
