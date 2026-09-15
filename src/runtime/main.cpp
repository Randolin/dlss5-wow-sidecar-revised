#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW; WIN32_LEAN_AND_MEAN drops it
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "core/Config.h"
#include "core/ControlChannel.h"
#include "core/GpuProfile.h"
#include "core/Hotkeys.h"
#include "core/Log.h"
#include "core/PanicSwitch.h"
#include "core/Presets.h"
#include "core/Text.h"
#include "neural/NeuralPassFactory.h"
#include "present/WindowTracker.h"
#include "runtime/Pipeline.h"

namespace fs = std::filesystem;
using namespace sidecar;

namespace {

struct Target {
  HWND hwnd = nullptr;
  std::wstring problem;
};

fs::path ExecutableDirectory() {
  wchar_t buffer[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return fs::path(buffer).parent_path();
}

// Config problems are warnings by design: they must not interrupt startup with
// a dialog, but they do have to end up somewhere the operator can read.
void ReportWarnings(const std::vector<std::string>& warnings) {
  for (const auto& warning : warnings) GlobalLog().Warn("config: " + warning);
}

// The overlay is opaque and covers the app completely, so from the moment it
// comes up the player is looking at our window and clicking *through* it at
// whatever sits underneath. That has to be the app -- and by default it is
// not: the runtime is launched from the manager, so the foreground belongs to
// the manager's window, which the overlay has just made invisible.
//
// Setting another process's window foreground is permitted here because the
// process that started us owned the foreground when it did.
void GiveTargetTheForeground(HWND target) {
  if (SetForegroundWindow(target)) {
    GlobalLog().Info("foreground handed to the capture target");
    return;
  }
  GlobalLog().Warn("could not give the app the foreground; alt-tab to it once if the "
                   "mouse and keyboard do not reach it");
}

Target ResolveTarget(int argc, wchar_t** argv, const TargetAppSettings& app) {
  // An explicit window, by handle. The manager passes this when it has one in
  // hand; the pipeline is keyed to a window, not to an application.
  if (argc > 2 && _wcsicmp(argv[1], L"--hwnd") == 0) {
    const auto value = wcstoull(argv[2], nullptr, 10);
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    if (!hwnd || !IsWindow(hwnd)) return {nullptr, L"The chosen window no longer exists."};
    if (!IsWindowVisible(hwnd)) return {nullptr, L"The chosen window is not visible."};
    if (!IsBorderless(hwnd)) {
      GlobalLog().Warn("the target window has a border; the overlay covers only its "
                       "client area. Borderless windowed is tidier.");
    }
    return {hwnd, {}};
  }
  // An explicit class name still works, so the runtime can be driven against
  // testpattern.exe.
  if (argc > 1) {
    HWND hwnd = FindWindowW(argv[1], nullptr);
    return hwnd ? Target{hwnd, {}} : Target{nullptr, L"Window class not found."};
  }
  // Otherwise the window the manager last chose. No guessing: an unset or
  // absent target is a message, not a search.
  if (!app.Chosen()) {
    return {nullptr, L"No capture target has been chosen yet.\n"
                     L"Pick a window on the manager's Status page, then start the overlay."};
  }
  const AppMatch match{Utf8ToWide(app.windowClass), Utf8ToWide(app.title)};
  auto window = FindAppWindow(match);
  const std::wstring name = Utf8ToWide(app.name.empty() ? "The chosen window" : app.name);
  if (!window) return {nullptr, name + L" is not running."};
  if (!window->borderless) {
    return {nullptr, name + L" must run in borderless windowed mode.\n"
                            L"Exclusive fullscreen has no compositor surface to capture."};
  }
  return {window->hwnd, {}};
}

// The runtime's view of the config file, built the same way at launch, on a
// live reload and on a hotkey so the three can never drift apart.
PipelineConfig BuildPipelineConfig(const Config& config, HWND target) {
  PipelineConfig cfg;
  cfg.target = target;
  cfg.showOverlay = config.showOverlay;
  cfg.showHud = config.showHud;
  cfg.flowGridSize = config.flowGridSize;
  // Calibrated at the capture resolution; leaving the source size zero means
  // "same as the capture".
  cfg.uiMaskRects = config.uiMaskRects;
  cfg.neuralPass = config.neuralPass;
  cfg.syntheticDepth = config.syntheticDepth;
  cfg.depthGradient = config.depthMode == "gradient";
  cfg.depthInverted = config.depthInverted;
  cfg.nr = config.nr;
  cfg.uiMaskFeather = static_cast<int32_t>(config.uiMaskFeather);
  cfg.runtimeDir = ExecutableDirectory();
  cfg.activePreset = config.activePreset;
  // The HDR tone-map shares the [nr] paper-white key; headroom is derived from
  // the display by the pipeline.
  cfg.hdrPaperWhiteNits = config.nr.paperWhiteNits;
  return cfg;
}

// Global hotkeys, registered on this thread so WM_HOTKEY arrives in the message
// loop below. RegisterHotKey installs no hook and loads nothing anywhere (I3,
// I4); an unparseable binding is logged and skipped, and one another program
// already owns fails to register and is logged the same way.
enum HotkeyId : int {
  kHotkeyToggleHud = 0xB010,
  kHotkeyToggleOverlay = 0xB011,
  kHotkeyNextPreset = 0xB012,
  kHotkeyPreviousPreset = 0xB013,
  kHotkeyDumpFrames = 0xB014,
  kHotkeyRecordTimings = 0xB015,
};

bool RegisterOneHotkey(int id, const std::string& text, const char* what) {
  UnregisterHotKey(nullptr, id);
  const auto parsed = ParseHotkey(text);
  if (!parsed) {
    GlobalLog().Warn(std::string("hotkey for ") + what + " (\"" + text +
                     "\") does not parse; not registered");
    return false;
  }
  if (!RegisterHotKey(nullptr, id, parsed->modifiers | MOD_NOREPEAT, parsed->vk)) {
    GlobalLog().Warn(std::string("hotkey for ") + what + " (" + DescribeHotkey(text) +
                     ") could not be registered (error " + std::to_string(GetLastError()) +
                     "); another program may own it");
    return false;
  }
  GlobalLog().Info(std::string("hotkey: ") + DescribeHotkey(text) + " -> " + what);
  return true;
}

// Returns the bitmask the status block reports: 1 HUD, 2 overlay, 4 next, 8
// previous, 16 dump frames.
uint32_t RegisterHotkeys(const HotkeySettings& keys) {
  uint32_t mask = 0;
  if (RegisterOneHotkey(kHotkeyToggleHud, keys.toggleHud, "toggle HUD")) mask |= 1;
  if (RegisterOneHotkey(kHotkeyToggleOverlay, keys.toggleOverlay, "toggle overlay")) mask |= 2;
  if (RegisterOneHotkey(kHotkeyNextPreset, keys.nextPreset, "next preset")) mask |= 4;
  if (RegisterOneHotkey(kHotkeyPreviousPreset, keys.previousPreset, "previous preset")) mask |= 8;
  if (RegisterOneHotkey(kHotkeyDumpFrames, keys.dumpFrames, "save debug frames")) mask |= 16;
  if (RegisterOneHotkey(kHotkeyRecordTimings, keys.recordTimings, "record pass timings")) {
    mask |= 32;
  }
  return mask;
}

void UnregisterHotkeys() {
  for (const int id : {kHotkeyToggleHud, kHotkeyToggleOverlay, kHotkeyNextPreset,
                       kHotkeyPreviousPreset, kHotkeyDumpFrames, kHotkeyRecordTimings}) {
    UnregisterHotKey(nullptr, id);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Before any window, GDI or User32 call. Without this the process is
  // DPI-unaware and Windows virtualises every coordinate it sees at the
  // desktop's scale factor: on a 4K display at 150 % the app's client rect
  // reads 2560x1440, the overlay and its swapchain are built at that size, and
  // the capture item -- which is not virtualised -- delivers 3840x2160 frames
  // that no longer match. The symptom is a black overlay at full frame rate.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  const fs::path dir = ExecutableDirectory();
  const fs::path configPath = dir / "sidecar.toml";
  const fs::path presetsPath = dir / "presets.toml";

  // Truncated each run: the interesting log is always the most recent session.
  GlobalLog().OpenFile(dir / "sidecar.log");
  GlobalLog().Info("sidecar starting");

  auto gpu = DetectPrimaryGpu();
  if (!gpu) {
    GlobalLog().Error("no NVIDIA adapter found");
    MessageBoxW(nullptr, L"No NVIDIA adapter found.", L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }
  GlobalLog().Info(std::string("adapter: ") + ToString(gpu->arch));

  if (gpu->arch != GpuArch::Ada && gpu->arch != GpuArch::Blackwell) {
    GlobalLog().Error(std::string(ToString(gpu->arch)) +
                      " is not supported; RTX 40 or RTX 50 required");
    wchar_t msg[256];
    swprintf_s(msg, L"%hs is not supported. RTX 40 or RTX 50 required.", ToString(gpu->arch));
    MessageBoxW(nullptr, msg, L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }

  // An absent config file is normal: every value has a working default, and a
  // malformed one warns rather than stopping the overlay.
  std::vector<std::string> warnings;
  Config config;
  if (auto loaded = LoadConfig(configPath, warnings)) config = *loaded;
  ReportWarnings(warnings);
  // Before anything starts logging per-frame lines.
  GlobalLog().SetVerboseCategories(config.logging.Mask());

  const Target target = ResolveTarget(argc, argv, config.app);
  if (!target.hwnd) {
    GlobalLog().Error("no capture target: see the dialog for what to do");
    MessageBoxW(nullptr, target.problem.c_str(), L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }

  auto pipeline = Pipeline::Create(*gpu, BuildPipelineConfig(config, target.hwnd), nullptr);
  if (!pipeline) {
    GlobalLog().Error("could not create the pipeline");
    MessageBoxW(nullptr, L"Failed to create the pipeline.", L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }
  Pipeline* raw = pipeline.get();
  const HWND targetHwnd = target.hwnd;

  // Hands a new configuration to the running pipeline: live where it can, a
  // rebuild where it must. Used by the manager's reload and by the hotkeys.
  const auto applyConfig = [&](const Config& next) {
    // What actually changed, from the serialised form -- generic, so a setting
    // added later is covered without touching this. "settings applied" with no
    // idea what was applied is the log line nobody could ever use.
    {
      std::istringstream before(SerializeConfig(config));
      std::istringstream after(SerializeConfig(next));
      std::string a, b, changes;
      int count = 0;
      while (std::getline(before, a) && std::getline(after, b)) {
        if (a == b || a.empty() || a[0] == '#') continue;
        if (++count > 6) { changes += ", ..."; break; }
        if (!changes.empty()) changes += ", ";
        // "key = old -> new", with the key taken from the left of the equals.
        const size_t eq = b.find(" = ");
        changes += eq == std::string::npos
                       ? b
                       : b.substr(0, eq) + " " + a.substr(eq + 3) + " -> " + b.substr(eq + 3);
      }
      GlobalLog().Info(changes.empty() ? "settings reloaded, nothing changed"
                                       : "settings changed: " + changes);
    }
    config = next;
    GlobalLog().SetVerboseCategories(config.logging.Mask());
    const PipelineConfig cfg = BuildPipelineConfig(config, targetHwnd);
    if (raw->ApplySettings(cfg)) {
      GlobalLog().Info("settings applied live");
      return;
    }
    GlobalLog().Info("settings need a rebuild; rebuilding the pipeline");
    if (!raw->RebuildAndRestart()) {
      GlobalLog().Error("the pipeline could not be rebuilt with the new settings");
      PostQuitMessage(0);
      return;
    }
    GiveTargetTheForeground(targetHwnd);
  };

  // A hotkey steps through the presets beside the sidecar. The config file is
  // not written here -- the manager owns it and mirrors what the status block
  // reports -- so the runtime never races the manager for the same file.
  const auto stepPreset = [&](int direction) {
    std::vector<std::string> presetWarnings;
    const auto presets = AllPresets(presetsPath, presetWarnings);
    ReportWarnings(presetWarnings);
    if (presets.empty()) return;
    size_t current = presets.size();
    for (size_t i = 0; i < presets.size(); ++i) {
      if (presets[i].name == config.activePreset) { current = i; break; }
    }
    if (current == presets.size()) {
      // The name is unknown (a deleted custom preset, say): go by the look.
      for (size_t i = 0; i < presets.size(); ++i) {
        if (PresetMatchesConfig(presets[i], config)) { current = i; break; }
      }
    }
    const size_t n = presets.size();
    const size_t next = current == n ? 0
                        : direction > 0 ? (current + 1) % n
                                        : (current + n - 1) % n;
    Config changed = config;
    ApplyPreset(presets[next], changed);
    GlobalLog().Info("hotkey: preset \"" + presets[next].name + "\"");
    applyConfig(changed);
  };

  // The manager's end of the channel. Created before Start() so the first
  // status the render loop publishes has somewhere to land, and refused when
  // another overlay already owns it.
  auto control = ControlServer::Create([&](SidecarCommand command) {
    // Runs on this thread, from the message loop below, which is the thread
    // that owns the windows. That is the whole reason commands travel as
    // messages.
    switch (command) {
      case SidecarCommand::Stop:        PostQuitMessage(0); break;
      case SidecarCommand::ShowOverlay: raw->SetOverlayVisible(true); break;
      case SidecarCommand::HideOverlay: raw->SetOverlayVisible(false); break;
      case SidecarCommand::ShowHud:     raw->SetHudVisible(true); break;
      case SidecarCommand::HideHud:     raw->SetHudVisible(false); break;
      case SidecarCommand::EnterConfigMode: raw->SetOverlayInteractive(true); break;
      case SidecarCommand::ExitConfigMode:  raw->SetOverlayInteractive(false); break;
      case SidecarCommand::DumpDebugFrames: raw->RequestDebugDump(); break;
      case SidecarCommand::CalibrateUiWithInterface:    raw->RequestCalibrationCapture(1); break;
      case SidecarCommand::CalibrateUiWithoutInterface: raw->RequestCalibrationCapture(2); break;
      case SidecarCommand::ReloadSettings: {
        std::vector<std::string> reloadWarnings;
        Config fresh;
        if (auto loaded = LoadConfig(configPath, reloadWarnings)) fresh = *loaded;
        ReportWarnings(reloadWarnings);
        // Bindings may have changed too; re-registering is cheap.
        raw->SetHotkeyMask(RegisterHotkeys(fresh.hotkeys));
        applyConfig(fresh);
        break;
      }
      default: break;
    }
  });
  if (!control) {
    GlobalLog().Error("another overlay is already running");
    MessageBoxW(nullptr,
                L"An overlay is already running.\n"
                L"Stop it from the manager before starting another.",
                L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }
  ControlServer* channel = control.get();
  pipeline->SetStatusSink([channel](const SidecarStatus& status) { channel->Publish(status); });

  raw->SetHotkeyMask(RegisterHotkeys(config.hotkeys));
  pipeline->Start();
  GlobalLog().Info("overlay running");
  GiveTargetTheForeground(target.hwnd);

  // The owner loop. Not a blocking GetMessage: it wakes on messages *or* every
  // quarter second, and on every wake it asks the pipeline whether there is
  // still anything to run. The render thread does post a WM_NULL when the
  // target goes away, but a single wake-up that lands while this thread is
  // busy is lost, and a process that then sits here with no overlay is the
  // "still in memory" symptom. Polling costs nothing and cannot miss.
  const char* exitReason = nullptr;
  while (!exitReason) {
    MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    MSG msg{};
    bool quit = false;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        quit = true;
        break;
      }
      if (msg.message == WM_HOTKEY) {
        // Hotkeys registered on this thread land here, not in a window
        // procedure. (The panic switch is registered and pumped on the render
        // thread, so it never arrives here.)
        switch (static_cast<int>(msg.wParam)) {
          case kHotkeyToggleHud:      raw->SetHudVisible(!raw->HudVisible()); break;
          case kHotkeyToggleOverlay:  raw->SetOverlayVisible(!raw->OverlayVisible()); break;
          case kHotkeyNextPreset:     stepPreset(+1); break;
          case kHotkeyPreviousPreset: stepPreset(-1); break;
          case kHotkeyDumpFrames:
            GlobalLog().Info("hotkey: saving debug frames");
            raw->RequestDebugDump();
            break;
          case kHotkeyRecordTimings:
            raw->ToggleTimingRecord();
            break;
          default: break;
        }
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (quit) {
      exitReason = "stopped by the manager";
      break;
    }

    // The app closed, or was restarted. Nothing to recover -- a new launch is
    // a new window and a new capture item -- so exit rather than linger. Asked
    // two ways: what the render thread saw, and what the window manager says
    // right now, in case the render thread never got to say.
    if (pipeline->TargetLost() || !IsWindow(targetHwnd)) {
      exitReason = "the app closed";
      break;
    }

    // The render thread cannot rebuild after device loss: it would create the
    // overlay window on a thread that never pumps. It flags it and the rebuild
    // happens here, where the windows live.
    if (pipeline->NeedsRebuild()) {
      if (!pipeline->RebuildAndRestart()) {
        MessageBoxW(nullptr, L"The graphics device was reset and could not be rebuilt.",
                    L"DLSS 5 Sidecar", MB_ICONERROR);
        exitReason = "the graphics device could not be rebuilt";
        break;
      }
      continue;
    }

    // The render loop ended on its own -- the panic switch, or a failure that
    // does not ask for a rebuild -- and there is no overlay to run. A process
    // with no overlay and no reason to exist must not stay resident.
    if (!pipeline->Running()) {
      exitReason = "the render loop stopped";
      break;
    }
  }

  GlobalLog().Info(std::string("sidecar exiting: ") + exitReason);
  UnregisterHotkeys();
  // Tear down in dependency order: the pipeline first (it drains the GPU,
  // closes capture and joins the render thread, which is the last thing that
  // publishes status), then the channel it published to.
  pipeline.reset();
  control.reset();
  if (argv) LocalFree(argv);

  // Leave without running the vendor runtimes' process-detach code. Two
  // closed drivers' worth of DLLs are loaded here, and a DLL that blocks in
  // DLL_PROCESS_DETACH would keep this process alive as a window-less zombie
  // after everything of ours is already gone. Our own teardown is complete at
  // this point, so nothing is lost by not giving them the chance.
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}
