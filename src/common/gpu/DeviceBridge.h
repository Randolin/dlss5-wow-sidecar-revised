#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace sidecar {

struct BridgeFrame {
  ID3D12Resource* texture = nullptr;
  uint64_t fenceValue = 0;
  uint32_t index = 0;
};

// Bridges Windows Graphics Capture (which hands out D3D11 textures) to the
// D3D12 device that NGX and NVOFA require. Two devices on one adapter, joined
// by shared NT-handle textures and a shared fence.
//
// A keyed mutex is deliberately not used: it serialises the two queues and
// costs latency the frame budget cannot absorb (spec section 7).
class DeviceBridge {
 public:
  static constexpr uint32_t kRingDepth = 3;

  static std::unique_ptr<DeviceBridge> Create(LUID adapterLuid, uint32_t width, uint32_t height,
                                              DXGI_FORMAT ringFormat = DXGI_FORMAT_B8G8R8A8_UNORM);
  ~DeviceBridge();

  ID3D11Device5* D3d11() const { return d3d11_.Get(); }
  ID3D11DeviceContext4* D3d11Context() const { return d3d11Ctx_.Get(); }
  ID3D12Device* D3d12() const { return d3d12_.Get(); }
  ID3D12CommandQueue* Queue() const { return queue_.Get(); }
  ID3D12Fence* SharedFence() const { return fence12_.Get(); }

  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }
  // BGRA8 for an SDR capture; RGBA16F (scRGB linear) for an HDR one. Every
  // frame in the ring, and everything copied from it, is in this format.
  DXGI_FORMAT RingFormat() const { return format_; }

  // Capture thread. Copies src into the next ring slot on the D3D11 queue,
  // signals the shared fence, and publishes the slot.
  // Latest-wins: returns true when it overwrote a frame the render thread had
  // not yet consumed, which the caller records as a drop.
  bool Publish(ID3D11Texture2D* src);

  // Render thread. Blocks until a frame is published or the timeout expires;
  // returns true if one is waiting. Polling with Sleep(1) is not an
  // alternative: a process that has not raised the timer resolution sleeps to
  // the next 15.6 ms tick, which quantises the whole overlay to well under the
  // capture rate. The event is auto-reset and signalled by Publish.
  bool WaitForFrame(uint32_t timeoutMs) const;

  // Render thread. Returns the newest published-but-unconsumed frame and marks
  // it consumed. The caller must have the queue wait on SharedFence() at the
  // returned fenceValue before reading the texture.
  std::optional<BridgeFrame> AcquireLatest();

 private:
  DeviceBridge() = default;
  bool Commit();

  struct Slot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex11;
    Microsoft::WRL::ComPtr<ID3D12Resource> tex12;
    HANDLE sharedHandle = nullptr;
    uint64_t fenceValue = 0;
  };

  Microsoft::WRL::ComPtr<ID3D11Device5> d3d11_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> d3d11Ctx_;
  Microsoft::WRL::ComPtr<ID3D12Device> d3d12_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
  HANDLE fenceHandle_ = nullptr;

  std::array<Slot, kRingDepth> ring_;
  uint32_t writeIndex_ = 0;
  uint64_t nextFenceValue_ = 1;

  // -1 means "nothing published". Written by the capture thread, read and
  // cleared by the render thread.
  std::atomic<int32_t> published_{-1};

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  // Signalled by Publish, waited on by the render thread.
  HANDLE frameReady_ = nullptr;
};

}  // namespace sidecar
