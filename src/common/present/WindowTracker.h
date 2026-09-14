#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sidecar {

struct TargetWindow {
  HWND hwnd = nullptr;
  RECT clientScreen{};
  bool borderless = false;
};

// WoW's top-level window classes, observed rather than documented, which is why
// this list has grown twice. Retail has registered "waApplication Window" and,
// as of the 2026-08 client, plain "w"; older and Classic clients use
// "GxWindowClass". Matching by class rather than by process is deliberate: it
// needs no process handle at all (I2).
inline constexpr const wchar_t* kWowWindowClasses[] = {
    L"waApplication Window",
    L"GxWindowClass",
    L"w",
};

// The window title WoW gives its main window, used to disambiguate the classes
// that are too generic to stand alone.
inline constexpr const wchar_t* kWowWindowTitle = L"World of Warcraft";

// Whether a class name is specific enough to identify WoW by itself.
//
// "w" plainly is not -- any application could register it -- so a window of that
// class is only accepted when its title matches as well. The longer names are
// distinctive enough that a title check would only risk rejecting a localised
// client for no gain.
//
// Pure, and unit-tested, because getting it wrong either misses the game
// entirely or captures somebody else's window.
bool ClassNameIsSpecificEnough(const wchar_t* className);

// How the target application is recognised: the class name and title of the
// window the operator chose. Both are read off the window itself, never from
// a process handle (I2). An empty class matches nothing -- there is no
// guessing.
struct AppMatch {
  std::wstring windowClass;
  std::wstring title;
};

// The first visible window with a real client area that matches, preferring
// one whose title is `title` when several share the class.
std::optional<TargetWindow> FindAppWindow(const AppMatch& match);

// Display mode from window styles only. The spec forbids reading WoW's game
// data, so Config.wtf is never parsed (I5).
bool IsBorderless(HWND hwnd);

std::optional<RECT> ClientRectInScreen(HWND hwnd);

// Follows a window through moves and resizes.
//
// The hook is installed WINEVENT_OUTOFCONTEXT. The in-context form maps a DLL
// into the observed process and would destroy the premise of this project (I3).
class WindowTracker {
 public:
  using MovedCallback = std::function<void(const RECT& clientScreen)>;

  static std::unique_ptr<WindowTracker> Create(HWND target, MovedCallback onMoved);
  ~WindowTracker();

 private:
  WindowTracker() = default;

  static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                 LONG objectId, LONG childId,
                                 DWORD threadId, DWORD timestamp);

  HWINEVENTHOOK hook_ = nullptr;
  HWND target_ = nullptr;
  MovedCallback onMoved_;
};

// Reports every change of the foreground window, system-wide.
//
// The overlay is opaque and topmost, so it must only be up while the game is
// what the player is looking at. Alt-tabbing to the manager -- the only way to
// reach Stop or configure mode -- otherwise foregrounds it *behind* an overlay
// that never yields, and the operator is stuck with the panic key. Watching
// the foreground is how every game overlay solves this.
//
// Same terms as WindowTracker: WINEVENT_OUTOFCONTEXT, so nothing is loaded
// into any other process (I3). The hook is system-wide because the interesting
// event is the game *losing* the foreground to something we cannot name in
// advance. The callback runs on the creating thread's message loop, so create
// it on the thread that owns the windows and pumps.
class ForegroundWatcher {
 public:
  using ChangedCallback = std::function<void(HWND foreground)>;

  static std::unique_ptr<ForegroundWatcher> Create(ChangedCallback onChanged);
  ~ForegroundWatcher();

 private:
  ForegroundWatcher() = default;

  static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                 LONG objectId, LONG childId,
                                 DWORD threadId, DWORD timestamp);

  HWINEVENTHOOK hook_ = nullptr;
  ChangedCallback onChanged_;
};

}  // namespace sidecar
