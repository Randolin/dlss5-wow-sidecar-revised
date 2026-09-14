#include "present/Hud.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sidecar {
namespace {

constexpr wchar_t kClassName[] = L"SidecarHud";
constexpr int kWidth = 480;
constexpr int kHeight = 152;
Hud* g_activeHud = nullptr;

const char* VerdictWord(GateVerdict v) {
  switch (v) {
    case GateVerdict::Playable: return "playable";
    case GateVerdict::Marginal: return "marginal";
    default:                    return "too slow";
  }
}

COLORREF kInk = RGB(0xCE, 0xE0, 0xE8);
COLORREF kDim = RGB(0x7A, 0x8A, 0x94);
COLORREF kGold = RGB(0xE8, 0xC5, 0x6A);
COLORREF kGood = RGB(0x5C, 0xD6, 0x8C);
COLORREF kWarn = RGB(0xE8, 0xA8, 0x4A);
COLORREF kBad = RGB(0xE6, 0x5A, 0x5A);

HFONT MakeFont(int height, int weight, const wchar_t* face) {
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, face);
}

void Text(HDC dc, int x, int y, COLORREF colour, HFONT font, const char* s) {
  SetTextColor(dc, colour);
  HGDIOBJ previous = SelectObject(dc, font);
  TextOutA(dc, x, y, s, static_cast<int>(strlen(s)));
  SelectObject(dc, previous);
}

COLORREF FpsColour(double fps) {
  if (fps >= 90.0) return kGood;
  if (fps >= 55.0) return kWarn;
  return kBad;
}

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace

GateVerdict JudgeGate(double p99Ms) {
  if (p99Ms < 40.0) return GateVerdict::Playable;
  if (p99Ms < 80.0) return GateVerdict::Marginal;
  return GateVerdict::Failed;
}

std::string FormatHud(const HudModel& model) {
  char pass[128];
  if (model.runtimeVariant && *model.runtimeVariant) {
    std::snprintf(pass, sizeof(pass), "%s [%s]", model.passName, model.runtimeVariant);
  } else {
    std::snprintf(pass, sizeof(pass), "%s", model.passName);
  }
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "%s | %s | p50 %.1f ms  p99 %.1f ms (%s) | %llu frames  %llu drops",
                model.gpuName, pass, model.p50Ms, model.p99Ms,
                VerdictWord(JudgeGate(model.p99Ms)),
                static_cast<unsigned long long>(model.frames),
                static_cast<unsigned long long>(model.drops));
  return buffer;
}

namespace {

void Paint(HDC dc, const HudModel& m) {
  RECT client{0, 0, kWidth, kHeight};
  HBRUSH back = CreateSolidBrush(RGB(0x0B, 0x0C, 0x12));
  FillRect(dc, &client, back);
  DeleteObject(back);
  SetBkMode(dc, TRANSPARENT);

  HFONT big = MakeFont(44, FW_BOLD, L"Segoe UI");
  HFONT small = MakeFont(15, FW_NORMAL, L"Segoe UI");
  HFONT mono = MakeFont(14, FW_NORMAL, L"Consolas");

  // A gold rule down the left, like the manager's cards.
  HBRUSH rule = CreateSolidBrush(kGold);
  RECT ruleRect{0, 0, 3, kHeight};
  FillRect(dc, &ruleRect, rule);
  DeleteObject(rule);

  // The big number: frames reaching the screen per second.
  char fps[32];
  std::snprintf(fps, sizeof(fps), "%.0f", m.fps);
  Text(dc, 14, 6, FpsColour(m.fps), big, fps);
  Text(dc, 14, 54, kDim, small, "FPS on screen");

  // The column of numbers beside it.
  char line[160];
  const int cx = 128;
  std::snprintf(line, sizeof(line), "capture  %5.0f fps", m.captureFps);
  Text(dc, cx, 8, kInk, mono, line);
  std::snprintf(line, sizeof(line), "gpu      %5.1f ms   cpu %4.1f ms", m.gpuMs, m.cpuMs);
  Text(dc, cx, 26, kInk, mono, line);
  std::snprintf(line, sizeof(line), "latency  p50 %5.1f   p99 %5.1f ms  (%s)", m.p50Ms, m.p99Ms,
                VerdictWord(JudgeGate(m.p99Ms)));
  Text(dc, cx, 44, kInk, mono, line);
  const double dropPct =
      m.frames + m.drops > 0 ? 100.0 * static_cast<double>(m.drops) /
                                   static_cast<double>(m.frames + m.drops)
                             : 0.0;
  std::snprintf(line, sizeof(line), "dropped  %5.1f%%   %llu frames", dropPct,
                static_cast<unsigned long long>(m.frames));
  Text(dc, cx, 62, kDim, mono, line);

  // What is running.
  if (m.modelWidth > 0) {
    std::snprintf(line, sizeof(line), "%s  %s  %ux%u -> %ux%u  x%d%s%s", m.preset,
                  m.passName, m.width, m.height, m.modelWidth, m.modelHeight, m.passCount,
                  m.temporal ? "  temporal" : "", m.hdr ? "  HDR" : "");
  } else {
    std::snprintf(line, sizeof(line), "%s  %s  %ux%u%s", m.preset, m.passName, m.width,
                  m.height, m.hdr ? "  HDR" : "");
  }
  Text(dc, 14, 82, kGold, small, line);

  // Frame-interval graph: one bar per presented frame, oldest on the left.
  // Guide lines at 60 and 120 fps. A hitch is a tall bar; a steady rate is a
  // flat line; a half-rate stutter is a comb.
  const int gx = 14, gy = 104, gw = kWidth - 28, gh = 42;
  HBRUSH graphBack = CreateSolidBrush(RGB(0x14, 0x16, 0x20));
  RECT graphRect{gx, gy, gx + gw, gy + gh};
  FillRect(dc, &graphRect, graphBack);
  DeleteObject(graphBack);
  const float maxMs = 33.4f;   // top of the graph: 30 fps
  auto yFor = [&](float ms) {
    const float t = std::min(ms, maxMs) / maxMs;
    return gy + gh - static_cast<int>(t * gh);
  };
  HPEN guide = CreatePen(PS_SOLID, 1, RGB(0x2A, 0x2E, 0x3C));
  HGDIOBJ oldPen = SelectObject(dc, guide);
  for (float ms : {16.7f, 8.3f}) {
    MoveToEx(dc, gx, yFor(ms), nullptr);
    LineTo(dc, gx + gw, yFor(ms));
  }
  SelectObject(dc, oldPen);
  DeleteObject(guide);

  const int n = std::min(m.intervalCount, HudModel::kHistory);
  if (n > 0) {
    const float barW = static_cast<float>(gw) / static_cast<float>(HudModel::kHistory);
    for (int i = 0; i < n; ++i) {
      const float ms = m.intervalMs[static_cast<size_t>(i)];
      const int x0 = gx + static_cast<int>(i * barW);
      const int x1 = gx + static_cast<int>((i + 1) * barW);
      const COLORREF c = ms <= 8.8f ? kGood : ms <= 17.5f ? RGB(0x8C, 0xC8, 0x70)
                         : ms <= 34.0f ? kWarn : kBad;
      HBRUSH bar = CreateSolidBrush(c);
      RECT r{x0, yFor(ms), std::max(x1, x0 + 1), gy + gh};
      FillRect(dc, &r, bar);
      DeleteObject(bar);
    }
  }
  Text(dc, gx + gw - 92, gy + 1, kDim, mono, "60 --- 120 ---");

  DeleteObject(big);
  DeleteObject(small);
  DeleteObject(mono);
}

LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT && g_activeHud) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    // Double-buffered: the graph redraws a few times a second and a direct
    // paint would flicker.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, kWidth, kHeight);
    HGDIOBJ old = SelectObject(mem, bmp);
    Paint(mem, g_activeHud->Model());
    BitBlt(dc, 0, 0, kWidth, kHeight, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::unique_ptr<Hud> Hud::Create() {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HudProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
  }

  std::unique_ptr<Hud> h(new Hud());
  h->hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
          WS_EX_TOOLWINDOW,
      kClassName, L"", WS_POPUP, 12, 12, kWidth, kHeight,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!h->hwnd_) return nullptr;
  SetLayeredWindowAttributes(h->hwnd_, 0, 225, LWA_ALPHA);
  g_activeHud = h.get();
  return h;
}

Hud::~Hud() {
  if (g_activeHud == this) g_activeHud = nullptr;
  if (hwnd_) DestroyWindow(hwnd_);
}

void Hud::Update(const HudModel& model) {
  model_ = model;
  text_ = FormatHud(model);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Hud::Show() { ShowWindow(hwnd_, SW_SHOWNOACTIVATE); }
void Hud::Hide() noexcept { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }

}  // namespace sidecar
