#include "present/WindowTracker.h"

#include <cwchar>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace sidecar {
namespace {

std::mutex g_mutex;
std::unordered_map<HWND, WindowTracker*> g_trackers;
ForegroundWatcher* g_foregroundWatcher = nullptr;

}  // namespace

bool IsBorderless(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  // Borderless means no caption and no thick frame. WoW's borderless mode is
  // WS_POPUP; its windowed mode carries WS_CAPTION.
  return (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
}

std::optional<RECT> ClientRectInScreen(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return std::nullopt;
  RECT client{};
  if (!GetClientRect(hwnd, &client)) return std::nullopt;
  POINT topLeft{client.left, client.top};
  POINT bottomRight{client.right, client.bottom};
  if (!ClientToScreen(hwnd, &topLeft)) return std::nullopt;
  if (!ClientToScreen(hwnd, &bottomRight)) return std::nullopt;
  return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

bool ClassNameIsSpecificEnough(const wchar_t* className) {
  if (!className) return false;
  // Short names are the ones that could belong to anything. The threshold is
  // deliberately crude: the point is to separate "w" from "GxWindowClass", not
  // to rank plausibility.
  return wcslen(className) > 4;
}

std::optional<TargetWindow> FindAppWindow(const AppMatch& match) {
  if (match.windowClass.empty()) return std::nullopt;

  // Every visible window of the class with a real client area is a candidate;
  // one whose caption is the remembered title wins, otherwise the first.
  std::optional<TargetWindow> fallback;
  HWND hwnd = nullptr;
  while ((hwnd = FindWindowExW(nullptr, hwnd, match.windowClass.c_str(), nullptr)) != nullptr) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
    auto rect = ClientRectInScreen(hwnd);
    if (!rect) continue;
    if (rect->right - rect->left <= 0 || rect->bottom - rect->top <= 0) continue;

    TargetWindow t;
    t.hwnd = hwnd;
    t.clientScreen = *rect;
    t.borderless = IsBorderless(hwnd);

    if (!match.title.empty()) {
      wchar_t title[256] = {};
      GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
      if (match.title == title) return t;
    }
    if (!fallback) fallback = t;
  }
  return fallback;
}

std::unique_ptr<WindowTracker> WindowTracker::Create(HWND target, MovedCallback onMoved) {
  if (!target || !IsWindow(target)) return nullptr;

  std::unique_ptr<WindowTracker> t(new WindowTracker());
  t->target_ = target;
  t->onMoved_ = std::move(onMoved);

  DWORD processId = 0;
  const DWORD threadId = GetWindowThreadProcessId(target, &processId);

  // WINEVENT_OUTOFCONTEXT is mandatory (I3): it delivers events to our own
  // process without loading anything into the observed one.
  //
  // The hook is already narrowed to the target's process and thread, so
  // WINEVENT_SKIPOWNPROCESS would buy nothing against WoW while making a
  // target inside this process -- which is how the tracker is tested --
  // impossible to observe.
  t->hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                             nullptr, &WindowTracker::EventProc,
                             processId, threadId,
                             WINEVENT_OUTOFCONTEXT);
  if (!t->hook_) return nullptr;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_trackers[target] = t.get();
  }
  return t;
}

WindowTracker::~WindowTracker() {
  if (hook_) UnhookWinEvent(hook_);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_trackers.erase(target_);
}

void CALLBACK WindowTracker::EventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                       LONG objectId, LONG, DWORD, DWORD) {
  if (event != EVENT_OBJECT_LOCATIONCHANGE || objectId != OBJID_WINDOW) return;

  WindowTracker* tracker = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_trackers.find(hwnd);
    if (it == g_trackers.end()) return;
    tracker = it->second;
  }
  if (!tracker || !tracker->onMoved_) return;
  if (auto rect = ClientRectInScreen(hwnd)) tracker->onMoved_(*rect);
}

std::unique_ptr<ForegroundWatcher> ForegroundWatcher::Create(ChangedCallback onChanged) {
  std::unique_ptr<ForegroundWatcher> w(new ForegroundWatcher());
  w->onChanged_ = std::move(onChanged);
  // Process and thread zero: every process. Still OUTOFCONTEXT, so the
  // system delivers the event to this thread's queue rather than loading
  // anything anywhere (I3).
  w->hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, &ForegroundWatcher::EventProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT);
  if (!w->hook_) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_foregroundWatcher = w.get();
  }
  return w;
}

ForegroundWatcher::~ForegroundWatcher() {
  if (hook_) UnhookWinEvent(hook_);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_foregroundWatcher == this) g_foregroundWatcher = nullptr;
}

void CALLBACK ForegroundWatcher::EventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                           LONG, LONG, DWORD, DWORD) {
  if (event != EVENT_SYSTEM_FOREGROUND) return;
  ForegroundWatcher* watcher = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    watcher = g_foregroundWatcher;
  }
  if (watcher && watcher->onChanged_) watcher->onChanged_(hwnd);
}

}  // namespace sidecar
