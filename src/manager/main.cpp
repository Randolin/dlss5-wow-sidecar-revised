// sidecar-manager.exe: the control panel.
//
// It does five things, and refuses to do a sixth. It reports what the machine
// has and what it is missing; it copies operator-supplied files into place; it
// starts and stops the overlay; it edits the settings the sidecar reads; and it
// shows the overlay's live numbers while it runs.
//
// What it does not do is touch the target application, in either direction.
// The probes it runs are the same predicates the unit tests pin down (I7, I8,
// I9), and the uninstaller deletes only files inside the sidecar's own
// directory.
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "core/Config.h"
#include "core/ControlChannel.h"
#include "core/Hotkeys.h"
#include "core/Log.h"
#include "core/Presets.h"
#include "core/Text.h"
#include "manager/Install.h"
#include "manager/Probes.h"
#include "manager/Theme.h"
#include "present/Hud.h"
#include "present/WindowTracker.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using namespace sidecar;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int kBaseWindowWidth = 1180;
constexpr int kBaseWindowHeight = 820;

// The display's DPI scale, and the layout constants derived from it. The
// manager is per-monitor DPI aware, so Windows hands it raw pixels and does
// not scale anything itself -- everything here has to be multiplied or the
// whole panel renders postage-stamp sized on a 4K display.
float g_scale = 1.0f;
float kNavWidth = 208.0f;
float kHeaderHeight = 104.0f;
float kPad = 22.0f;

// A constant in the layout, at the current scale. Everything that is a size in
// pixels goes through this.
float S(float value) { return value * g_scale; }

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<IDXGISwapChain1> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_backBufferRtv;

// The notice a first-time operator has to read before anything is enabled.
// It is the honest version of the safety claim, not a disclaimer.
constexpr const char* kFirstRunTitle = "Before you use this";
constexpr const char* kFirstRunBody =
    "This sidecar never loads code into the application it captures. It reads "
    "what Windows has already composed, and it draws a window on top. That is "
    "what makes it safe, and it is checked automatically every time it is "
    "built.\n\n"
    "It is the same class of tool as a screen recorder or a streaming overlay, "
    "and it touches nothing in the application's own folder.\n\n"
    "No third-party tool can promise you will never be banned. What this one "
    "can promise is that it does not do any of the things publishers ban "
    "people for.";

// Both DXGI and D3D11 are delay-loaded (see CMakeLists) and pinned to their
// System32 copies here, before the first D3D call. A proxy DLL of either name
// left beside the executables by some other tool is then never consulted by
// the manager.
void PinSystemGraphicsDlls() {
  LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

void CreateBackBufferRtv() {
  ComPtr<ID3D11Texture2D> back;
  if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_backBufferRtv);
  }
}

bool CreateDeviceAndSwapChain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC1 scd{};
  scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               _countof(levels), D3D11_SDK_VERSION,
                               &g_device, nullptr, &g_context))) {
    return false;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(g_device.As(&dxgiDevice)) ||
      FAILED(dxgiDevice->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  if (FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), hwnd, &scd, nullptr,
                                             nullptr, &g_swapChain))) {
    return false;
  }
  CreateBackBufferRtv();
  return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
  switch (msg) {
    case WM_SIZE:
      if (g_swapChain && wp != SIZE_MINIMIZED) {
        g_backBufferRtv.Reset();
        g_swapChain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        CreateBackBufferRtv();
      }
      return 0;
    case WM_DPICHANGED: {
      // Moved to a display with a different scale. Windows suggests a new
      // window rectangle; take it. The fonts were rasterised for the old
      // scale and are not rebuilt here -- they scale with the style, which is
      // slightly soft but immediate, and a relaunch is sharp again.
      const RECT* suggested = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      const float scale = static_cast<float>(HIWORD(wp)) / 96.0f;
      if (scale > 0.1f && g_scale > 0.1f) {
        ImGui::GetIO().FontGlobalScale = scale / g_scale;
      }
      return 0;
    }
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize =
          POINT{static_cast<LONG>(S(900.0f)), static_cast<LONG>(S(620.0f))};
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

fs::path ExecutableDirectory() {
  wchar_t buffer[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return fs::path(buffer).parent_path();
}

ImVec4 Rgb(unsigned int hex, float alpha = 1.0f) {
  return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                (hex & 0xFF) / 255.0f, alpha);
}

const char* StateLabel(ProbeState state) {
  switch (state) {
    case ProbeState::Ok:   return "READY";
    case ProbeState::Warn: return "CHECK";
    default:               return "BLOCKED";
  }
}

ImVec4 StateColor(ProbeState state, const ThemeColors& colors) {
  return Rgb(state == ProbeState::Ok ? colors.ok
             : state == ProbeState::Warn ? colors.warn
                                         : colors.fail);
}

ThemeFonts g_fonts;
ThemeColors g_colors;

void GoldRule(float alpha = 0.45f, float padBelow = 10.0f) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  draw->AddLine(ImVec2(at.x, at.y), ImVec2(at.x + width, at.y),
                ImGui::GetColorU32(Rgb(g_colors.accent, alpha)), 1.0f);
  ImGui::Dummy(ImVec2(0.0f, padBelow));
}

void SectionHeading(const char* text) {
  if (g_fonts.heading) ImGui::PushFont(g_fonts.heading);
  ImGui::TextColored(Rgb(g_colors.goldBright), "%s", text);
  if (g_fonts.heading) ImGui::PopFont();
  GoldRule(0.35f, 8.0f);
}

void Hint(const char* text) {
  if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  if (g_fonts.caption) ImGui::PopFont();
}

void Dot(bool good, const char* label, bool warnNotFail = true) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float radius = S(5.0f);
  const ImU32 colour = ImGui::GetColorU32(
      good ? Rgb(g_colors.ok) : Rgb(warnNotFail ? g_colors.warn : g_colors.fail));
  draw->AddCircleFilled(ImVec2(at.x + radius + 1.0f, at.y + ImGui::GetTextLineHeight() * 0.5f),
                        radius, colour);
  ImGui::Dummy(ImVec2(radius * 2.0f + S(8.0f), 0.0f));
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::TextUnformatted(label);
}

void StatCard(const char* label, const char* value, float width, ImVec4 valueColor) {
  ImGui::BeginChild(label, ImVec2(width, S(52.0f)), ImGuiChildFlags_Border);
  ImGui::Indent(S(10.0f));
  if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
  ImGui::TextDisabled("%s", label);
  if (g_fonts.caption) ImGui::PopFont();
  ImGui::TextColored(valueColor, "%s", value);
  ImGui::Unindent(S(10.0f));
  ImGui::EndChild();
}

fs::path AskForFile(HWND owner, const wchar_t* filter, const wchar_t* title) {
  wchar_t buffer[MAX_PATH]{};
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buffer;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return {};
  return fs::path(buffer);
}

// Copies a std::string into a fixed ImGui edit buffer.
void SetBuffer(std::string& buffer, const std::string& value) {
  const size_t n = std::min(value.size(), buffer.size() - 1);
  std::memcpy(buffer.data(), value.data(), n);
  buffer[n] = '\0';
}

struct LiveState {
  bool appRunning = false;
  bool appBorderless = false;
  uint32_t appWidth = 0;
  uint32_t appHeight = 0;
  bool overlayRunning = false;
  std::optional<SidecarStatus> status;
};

LiveState PollLiveState(const TargetAppSettings& app) {
  LiveState live;
  if (app.Chosen()) {
    const AppMatch match{Utf8ToWide(app.windowClass), Utf8ToWide(app.title)};
    if (auto window = FindAppWindow(match)) {
      live.appRunning = true;
      live.appBorderless = window->borderless;
      live.appWidth = static_cast<uint32_t>(window->clientScreen.right - window->clientScreen.left);
      live.appHeight = static_cast<uint32_t>(window->clientScreen.bottom - window->clientScreen.top);
    }
  }
  live.overlayRunning = control::IsRunning();
  if (live.overlayRunning) live.status = control::Read();
  return live;
}

// Windows the operator might point the overlay at: visible, not tiny, not
// ours. Class name and title are what the config remembers; both are read
// off the window, never from a process handle (I2).
struct WindowChoice {
  HWND hwnd = nullptr;
  std::string title;
  std::string windowClass;
};

std::vector<WindowChoice> EnumerateCandidateWindows() {
  std::vector<WindowChoice> out;
  EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        auto* list = reinterpret_cast<std::vector<WindowChoice>*>(lparam);
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        wchar_t title[256] = {};
        if (GetWindowTextW(hwnd, title, 256) <= 0) return TRUE;
        wchar_t cls[256] = {};
        if (GetClassNameW(hwnd, cls, 256) <= 0) return TRUE;
        RECT r{};
        if (!GetClientRect(hwnd, &r) || r.right < 320 || r.bottom < 240) return TRUE;
        list->push_back(WindowChoice{hwnd, WideToUtf8(title), WideToUtf8(cls)});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

enum class Section { Status, Setup, Checks, Tuning, Log, Count };

const char* SectionName(Section section) {
  switch (section) {
    case Section::Status: return "Status";
    case Section::Setup:  return "Setup";
    case Section::Checks: return "Checks";
    case Section::Tuning: return "Tuning";
    default:              return "Log";
  }
}

bool AnyBlockingFailure(const std::vector<ProbeResult>& results) {
  for (const auto& r : results) {
    if (r.state == ProbeState::Fail) return true;
  }
  return false;
}

void DrawBoard(const std::vector<ProbeResult>& results) {
  if (!ImGui::BeginTable("probes", 3,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, S(88.0f));
  ImGui::TableSetupColumn("check", ImGuiTableColumnFlags_WidthFixed, S(250.0f));
  ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);

  for (const auto& r : results) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
    ImGui::TextColored(StateColor(r.state, g_colors), "%s", StateLabel(r.state));
    if (g_fonts.caption) ImGui::PopFont();

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(r.title.c_str());

    ImGui::TableSetColumnIndex(2);
    ImGui::TextWrapped("%s", r.detail.c_str());
    if (r.state != ProbeState::Ok && !r.remedy.empty()) Hint(r.remedy.c_str());
  }
  ImGui::EndTable();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  // Per-monitor DPI aware, before any window: otherwise every client rect the
  // manager reads is virtualised at the desktop scale and the Checks page
  // reports a 4K game as 2560x1440.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  PinSystemGraphicsDlls();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SidecarManager";
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DLSS 5 Sidecar",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              kBaseWindowWidth, kBaseWindowHeight, nullptr, nullptr,
                              inst, nullptr);
  if (!hwnd) {
    MessageBoxW(nullptr, L"Could not create the window.", L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }
  // Now that there is a window, ask which display it landed on and scale the
  // whole interface to it. The window itself is resized to match, so a 4K
  // display gets a panel of the same apparent size as a 1080p one.
  {
    const UINT dpi = GetDpiForWindow(hwnd);
    g_scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    if (g_scale < 1.0f) g_scale = 1.0f;
    kNavWidth = S(208.0f);
    kHeaderHeight = S(104.0f);
    kPad = S(22.0f);
    if (g_scale != 1.0f) {
      SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(S(kBaseWindowWidth)),
                   static_cast<int>(S(kBaseWindowHeight)),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  if (!CreateDeviceAndSwapChain(hwnd)) {
    MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.",
                L"DLSS 5 Sidecar", MB_ICONERROR);
    return 1;
  }
  {
    // The caption painted in the panel's own colours; every attribute is
    // Windows 11 and fails harmlessly on anything older.
    const BOOL darkTitleBar = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* USE_IMMERSIVE_DARK_MODE */, &darkTitleBar,
                          sizeof(darkTitleBar));
    const COLORREF caption = 0x00120C0B;
    const COLORREF text = 0x00CEE0E8;
    const COLORREF border = 0x006EAAC8;
    DwmSetWindowAttribute(hwnd, 35 /* CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* BORDER_COLOR */, &border, sizeof(border));
  }
  ShowWindow(hwnd, show);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  g_fonts = LoadThemeFonts(g_scale);
  ApplySidecarTheme(true, g_scale);
  g_colors = CurrentThemeColors(true);
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

  const fs::path sidecarDir = ExecutableDirectory();
  const fs::path configPath = sidecarDir / "sidecar.toml";
  const fs::path presetsPath = sidecarDir / "presets.toml";

  GlobalLog().OpenFile(sidecarDir / "sidecar-manager.log");
  GlobalLog().Info("manager starting");

  std::error_code ec;
  bool noticeAcknowledged = fs::exists(configPath, ec) && !ec;

  std::vector<std::string> warnings;
  Config config;
  if (auto loaded = LoadConfig(configPath, warnings)) {
    config = *loaded;
  } else {
    // A first run starts on the default look rather than on bare defaults.
    ApplyPreset(BuiltinPresets().front(), config);
  }
  for (const auto& warning : warnings) GlobalLog().Warn("config: " + warning);

  std::vector<std::string> presetWarnings;
  std::vector<Preset> presets = AllPresets(presetsPath, presetWarnings);
  for (const auto& warning : presetWarnings) GlobalLog().Warn("presets: " + warning);

  const auto saveCustomPresets = [&]() {
    if (SaveCustomPresets(presetsPath, presets)) {
      GlobalLog().Info("presets saved");
    } else {
      GlobalLog().Error("could not write presets.toml");
    }
  };

  std::string presetNameUtf8(64, '\0');
  std::string hotkeyBuffers[5];
  for (auto& b : hotkeyBuffers) b.resize(64);
  SetBuffer(hotkeyBuffers[0], config.hotkeys.toggleHud);
  SetBuffer(hotkeyBuffers[1], config.hotkeys.toggleOverlay);
  SetBuffer(hotkeyBuffers[2], config.hotkeys.nextPreset);
  SetBuffer(hotkeyBuffers[3], config.hotkeys.previousPreset);
  SetBuffer(hotkeyBuffers[4], config.hotkeys.dumpFrames);

  auto results = RunAllProbes(sidecarDir, config.app);

  Section section = Section::Status;
  {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) {
      for (int i = 0; i < static_cast<int>(Section::Count); ++i) {
        const auto candidate = static_cast<Section>(i);
        const std::wstring name = Utf8ToWide(SectionName(candidate));
        if (_wcsicmp(argv[1], name.c_str()) == 0) section = candidate;
      }
    }
    if (argv) LocalFree(argv);
  }
  bool dirty = false;
  std::string setupMessage;
  bool setupMessageIsError = false;
  bool captureBound = false;
  bool starving = false;
  bool liveApply = true;
  std::string lastMirroredPreset = config.activePreset;
  // A custom preset the operator asked to overwrite; the confirmation modal
  // reads it. npos when none.
  size_t overwriteIndex = static_cast<size_t>(-1);

  const auto save = [&]() {
    // A look chosen while a target is current belongs to that target: next
    // time this window is picked, it comes back with it.
    config.RememberLookForApp(config.app.windowClass, config.activePreset);
    if (SaveConfig(configPath, config)) {
      dirty = false;
      GlobalLog().Info("settings saved");
      // A running overlay picks the file up now: compose knobs on the next
      // frame, pass setups by a hot swap, anything structural by a rebuild.
      if (control::IsRunning()) {
        if (control::Send(SidecarCommand::ReloadSettings)) {
          GlobalLog().Info("settings sent to the running overlay");
        } else {
          GlobalLog().Warn("the overlay did not acknowledge the new settings");
        }
      }
    } else {
      GlobalLog().Error("could not write sidecar.toml");
    }
  };

  const auto startOverlay = [&]() {
    if (dirty) save();
    // The runtime has to be able to hand the foreground to the app, and a
    // launched process only inherits that right from the process that owned
    // the foreground -- which, at this instant, is this one.
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(nullptr, L"open", (sidecarDir / L"sidecar.exe").c_str(), nullptr,
                  sidecarDir.c_str(), SW_SHOWNORMAL);
    GlobalLog().Info("overlay launch requested for \"" + config.app.name + "\"");
    ShowWindow(hwnd, SW_MINIMIZE);
  };

  MSG msg{};
  while (msg.message != WM_QUIT) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      continue;
    }

    const LiveState live = PollLiveState(config.app);
    const bool blocked = AnyBlockingFailure(results);
    size_t missingComponents = 0;
    for (const auto& component : Components()) {
      if (!component.required) continue;
      if (InstalledFiles(component, sidecarDir).empty()) ++missingComponents;
    }

    // A hotkey in the runtime changed the look. Mirror it here so the file
    // matches what is on screen and the next launch keeps it. The runtime
    // never writes the file itself, so there is one writer.
    if (live.status && *live.status->activePreset) {
      const std::string reported = live.status->activePreset;
      if (reported != lastMirroredPreset && reported != config.activePreset) {
        for (const auto& p : presets) {
          if (p.name == reported) {
            ApplyPreset(p, config);
            SaveConfig(configPath, config);
            dirty = false;
            GlobalLog().Info("look changed by hotkey: \"" + reported + "\"");
            break;
          }
        }
      }
      lastMirroredPreset = reported;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ------------------------------------------------------------- header band
    ImGui::BeginChild("header", ImVec2(0.0f, kHeaderHeight));
    ImGui::Indent(kPad);
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (g_fonts.title) ImGui::PushFont(g_fonts.title);
    ImGui::TextColored(Rgb(g_colors.goldBright), "DLSS 5 Sidecar");
    if (g_fonts.title) ImGui::PopFont();

    if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
    ImGui::TextDisabled("Neural rendering for any borderless window, from outside its process");
    if (g_fonts.caption) ImGui::PopFont();

    const float buttonWidth = S(190.0f);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth - kPad);
    ImGui::SetCursorPosY(S(30.0f));

    if (!noticeAcknowledged) ImGui::BeginDisabled();
    if (live.overlayRunning) {
      ImGui::PushStyleColor(ImGuiCol_Button, Rgb(g_colors.fail, 0.20f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(g_colors.fail, 0.36f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(g_colors.fail, 0.50f));
      if (ImGui::Button("Stop overlay", ImVec2(buttonWidth, S(42.0f)))) {
        if (!control::Send(SidecarCommand::Stop)) {
          GlobalLog().Warn("the overlay did not answer; it may already be closing");
        }
      }
      ImGui::PopStyleColor(3);
    } else {
      const bool canStart = !blocked && live.appRunning;
      if (!canStart) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, Rgb(g_colors.accent, 0.26f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(g_colors.accent, 0.42f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(g_colors.goldBright, 0.55f));
      ImGui::PushStyleColor(ImGuiCol_Text, Rgb(g_colors.goldBright));
      if (ImGui::Button("Start overlay", ImVec2(buttonWidth, S(42.0f)))) startOverlay();
      ImGui::PopStyleColor(4);
      if (!canStart) ImGui::EndDisabled();
    }
    if (!noticeAcknowledged) ImGui::EndDisabled();

    ImGui::Unindent(kPad);
    ImGui::EndChild();

    ImGui::Indent(kPad);
    ImGui::PushItemWidth(-kPad);
    GoldRule(0.55f, 8.0f);
    ImGui::PopItemWidth();
    ImGui::Unindent(kPad);

    if (!noticeAcknowledged) ImGui::BeginDisabled();

    // ---------------------------------------------------------------- nav rail
    ImGui::Indent(kPad);
    ImGui::BeginChild("nav", ImVec2(kNavWidth, -kPad));
    for (int i = 0; i < static_cast<int>(Section::Count); ++i) {
      const auto candidate = static_cast<Section>(i);
      const bool selected = candidate == section;
      if (selected) ImGui::PushStyleColor(ImGuiCol_Text, Rgb(g_colors.goldBright));
      const ImVec2 rowAt = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable(SectionName(candidate), selected,
                            ImGuiSelectableFlags_None, ImVec2(0.0f, S(34.0f)))) {
        section = candidate;
      }
      if (selected) {
        ImGui::PopStyleColor();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(rowAt.x, rowAt.y), ImVec2(rowAt.x + S(3.0f), rowAt.y + S(34.0f)),
            ImGui::GetColorU32(Rgb(g_colors.goldBright)));
      }
      const char* badge = nullptr;
      ImVec4 badgeColor = Rgb(g_colors.warn);
      if (candidate == Section::Setup && missingComponents > 0) {
        badge = "!";
        badgeColor = Rgb(g_colors.fail);
      } else if (candidate == Section::Checks && blocked) {
        badge = "!";
        badgeColor = Rgb(g_colors.fail);
      } else if (candidate == Section::Tuning && dirty) {
        badge = "*";
      }
      if (badge) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(kNavWidth - S(26.0f));
        ImGui::TextColored(badgeColor, "%s", badge);
      }
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(110.0f));
    GoldRule(0.30f, 8.0f);
    if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
    {
      const std::string appLabel =
          !config.app.Chosen()  ? "No target chosen"
          : live.appRunning     ? (live.appBorderless ? "Target: borderless" : "Target: not borderless")
                                : "Target: not running";
      Dot(live.appRunning && live.appBorderless, appLabel.c_str());
    }
    Dot(live.overlayRunning, live.overlayRunning ? "Overlay: running" : "Overlay: stopped");
    if (live.appRunning) ImGui::TextDisabled("   %ux%u", live.appWidth, live.appHeight);
    ImGui::TextDisabled("   %s", config.activePreset.c_str());
    if (g_fonts.caption) ImGui::PopFont();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, kPad);
    ImGui::BeginChild("content", ImVec2(-kPad, -kPad));
    // Everything inside the content pane is laid out in pixels that were
    // written for a 96-dpi display. Scaling the item width here covers every
    // slider and combo in one place; the handful of explicit button sizes
    // below are close enough once the font and padding have grown.
    ImGui::PushItemWidth(S(440.0f));

    // Live tuning, on whichever page the change was made: once nothing is
    // being dragged or typed into, save and send.
    if (liveApply && dirty && live.overlayRunning && !ImGui::IsAnyItemActive()) save();

    // ------------------------------------------------------------------ Status
    if (section == Section::Status) {
      SectionHeading("Live");
      if (live.status) {
        const auto& s = *live.status;
        const GateVerdict verdict = JudgeGate(s.p99Ms);
        const ProbeState asState = verdict == GateVerdict::Playable   ? ProbeState::Ok
                                   : verdict == GateVerdict::Marginal ? ProbeState::Warn
                                                                      : ProbeState::Fail;
        char fps[32], p50[32], p99[32], captured[32];
        std::snprintf(fps, sizeof(fps), "%.0f", s.fps);
        std::snprintf(p50, sizeof(p50), "%.1f ms", s.p50Ms);
        std::snprintf(p99, sizeof(p99), "%.1f ms", s.p99Ms);
        std::snprintf(captured, sizeof(captured), "%.0f", s.captureFps);

        const float cardWidth = (ImGui::GetContentRegionAvail().x - S(30.0f)) / 4.0f;
        StatCard("ON SCREEN", fps, cardWidth,
                 Rgb(s.fps >= 90.0 ? g_colors.ok : s.fps >= 55.0 ? g_colors.warn : g_colors.fail));
        ImGui::SameLine();
        StatCard("CAPTURED", captured, cardWidth, Rgb(g_colors.parchment));
        ImGui::SameLine();
        StatCard("LATENCY p50", p50, cardWidth, Rgb(g_colors.parchment));
        ImGui::SameLine();
        StatCard("LATENCY p99", p99, cardWidth, StateColor(asState, g_colors));

        // One line for everything that is a fact rather than a measurement.
        if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
        ImGui::TextDisabled("%s  |  %s%s%s  |  %ux%u  |  %llu frames, %llu dropped  |  %s",
                            *s.activePreset ? s.activePreset : "--", s.passName,
                            *s.runtimeVariant ? " / " : "", s.runtimeVariant, s.width, s.height,
                            static_cast<unsigned long long>(s.frames),
                            static_cast<unsigned long long>(s.drops),
                            verdict == GateVerdict::Playable   ? "playable"
                            : verdict == GateVerdict::Marginal ? "marginal"
                                                               : "too slow");
        if (g_fonts.caption) ImGui::PopFont();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        const bool visible = s.overlayVisible != 0;
        // Height left to ImGui: an explicit one does not grow with the font
        // and squashes the label on a scaled display. Only the widths are set,
        // so the row stays even.
        if (ImGui::Button(visible ? "Hide overlay (A/B compare)" : "Show overlay",
                          ImVec2(S(230.0f), 0.0f))) {
          control::Send(visible ? SidecarCommand::HideOverlay : SidecarCommand::ShowOverlay);
        }
        ImGui::SameLine();
        const bool hudUp = s.hudVisible != 0;
        if (ImGui::Button(hudUp ? "Hide HUD" : "Show HUD", ImVec2(S(150.0f), 0.0f))) {
          control::Send(hudUp ? SidecarCommand::HideHud : SidecarCommand::ShowHud);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save debug frames", ImVec2(S(190.0f), 0.0f))) {
          control::Send(SidecarCommand::DumpDebugFrames);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy diagnostics", ImVec2(S(170.0f), 0.0f))) {
          // Everything a bug report needs, in one paste: the settings, the
          // live numbers, the checks, and the tails of both logs. Also written
          // beside the sidecar as diagnostics.txt.
          std::string text;
          text += "DLSS 5 Sidecar diagnostics\n";
          {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            char when[64];
            std::snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u\n", st.wYear,
                          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            text += when;
          }
          text += "\n== live ==\n";
          {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                          "pass %s  runtime %s  look %s\n"
                          "capture %ux%u  presented %.1f fps  captured %.1f fps\n"
                          "latency p50 %.2f ms  p99 %.2f ms  frames %llu  drops %llu\n"
                          "budget per frame: idle %.2f  cpu %.2f  present wait %.2f  gpu %.2f ms\n"
                          "vram used %u / budget %u MB  over budget %u MB  system %u MB\n"
                          "hotkeys registered mask %u  overlay %s  hud %s\n",
                          s.passName, s.runtimeVariant, s.activePreset, s.width, s.height,
                          s.fps, s.captureFps, s.p50Ms, s.p99Ms,
                          static_cast<unsigned long long>(s.frames),
                          static_cast<unsigned long long>(s.drops), s.idleMs, s.recordMs,
                          s.presentWaitMs, s.gpuWaitMs, s.vramUsedMb, s.vramBudgetMb,
                          s.vramSpilledMb, s.vramSystemMb, s.hotkeysRegistered,
                          s.overlayVisible ? "shown" : "hidden", s.hudVisible ? "shown" : "hidden");
            text += buf;
            if (*s.lastError) text += std::string("last error: ") + s.lastError + "\n";
          }
          text += "\n== checks ==\n";
          for (const auto& r : results) {
            text += std::string(StateLabel(r.state)) + "  " + r.title + ": " + r.detail + "\n";
          }
          text += "\n== sidecar.toml ==\n" + SerializeConfig(config);
          const auto tail = [](const fs::path& path, size_t lines) {
            std::ifstream in(path);
            std::vector<std::string> all;
            std::string line;
            while (std::getline(in, line)) all.push_back(line);
            std::string out;
            const size_t start = all.size() > lines ? all.size() - lines : 0;
            for (size_t i = start; i < all.size(); ++i) out += all[i] + "\n";
            return out;
          };
          text += "\n== sidecar.log (last 150 lines) ==\n" + tail(sidecarDir / "sidecar.log", 150);
          text += "\n== sidecar-manager.log (last 60 lines) ==\n" +
                  tail(sidecarDir / "sidecar-manager.log", 60);

          {
            std::ofstream out(sidecarDir / "diagnostics.txt", std::ios::binary | std::ios::trunc);
            out << text;
          }
          if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            const std::wstring wide = Utf8ToWide(text);
            const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
              if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
              }
            }
            CloseClipboard();
            GlobalLog().Info("diagnostics copied to the clipboard and written to diagnostics.txt");
          } else {
            GlobalLog().Warn("could not open the clipboard; diagnostics written to diagnostics.txt");
          }
        }
        ImGui::TextDisabled("Hiding the overlay uncovers the untouched app; it also steps "
                            "aside by itself when the app is not foreground. Debug frames go "
                            "beside the sidecar as .bmp.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SetNextItemWidth(S(320.0f));
        {
          int percent = static_cast<int>(config.nr.splitView * 100.0f + 0.5f);
          if (ImGui::SliderInt("A/B split", &percent, 0, 100, percent == 0 ? "off" : "%d%%")) {
            config.nr.splitView = static_cast<float>(percent) / 100.0f;
            dirty = true;
            save();
          }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("original left, processed right");

        if (s.captureFps > 0.0) {
          const double share = s.fps / s.captureFps;
          if (share >= 0.92) captureBound = true;
          if (share < 0.86) captureBound = false;
          if (share < 0.72) starving = true;
          if (share >= 0.80) starving = false;
        }
        const bool spilling = s.vramSpilledMb > 0;
        const bool overBudget = s.vramBudgetMb > 0 && s.vramUsedMb > s.vramBudgetMb;

        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        // One line of advice, and only when there is something to say.
        if (spilling || overBudget) {
          ImGui::TextColored(Rgb(g_colors.fail),
                             "Over the GPU memory budget by %u MB -- frames are waiting on "
                             "PCIe. Close what else is using the card.", s.vramSpilledMb);
        } else if (captureBound) {
          ImGui::TextColored(Rgb(g_colors.warn),
                             "Capture-bound: Windows is handing over %.0f frames a second, "
                             "so that is the ceiling.", s.captureFps);
        } else if (starving) {
          ImGui::TextColored(Rgb(g_colors.warn),
                             "Presenting %.0f of %.0f captured -- the app is competing for "
                             "the GPU. Cap its frame rate, or lower the model resolution.",
                             s.fps, s.captureFps);
        } else {
          ImGui::TextColored(Rgb(g_colors.ok), "Keeping up with capture.");
        }

        if (s.vramBudgetMb > 0) {
          const float ratio = static_cast<float>(s.vramUsedMb) /
                              static_cast<float>(s.vramBudgetMb);
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
          ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                (spilling || overBudget) ? Rgb(g_colors.fail)
                                : ratio > 0.9f           ? Rgb(g_colors.warn)
                                                         : Rgb(g_colors.accent));
          char label[128];
          std::snprintf(label, sizeof(label), "GPU memory  %u / %u MB  (+%u staged)",
                        s.vramUsedMb, s.vramBudgetMb, s.vramSystemMb);
          ImGui::ProgressBar(ratio > 1.0f ? 1.0f : ratio, ImVec2(-1.0f, S(16.0f)), label);
          ImGui::PopStyleColor();
        }

        // Where the frame goes: one bar, with the legend on the line under it.
        const double total = s.idleMs + s.recordMs + s.presentWaitMs + s.gpuWaitMs;
        if (total > 0.0) {
          const struct { const char* label; double ms; unsigned int colour; } parts[] = {
              {"neural", s.gpuWaitMs, g_colors.epic},
              {"idle", s.idleMs, 0x4A5568},
              {"cpu", s.recordMs, g_colors.accent},
              {"present", s.presentWaitMs, g_colors.warn},
          };
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
          const float barWidth = ImGui::GetContentRegionAvail().x - 4.0f;
          const ImVec2 barAt = ImGui::GetCursorScreenPos();
          ImDrawList* draw = ImGui::GetWindowDrawList();
          float x = barAt.x;
          for (const auto& part : parts) {
            const float w = static_cast<float>(part.ms / total) * barWidth;
            draw->AddRectFilled(ImVec2(x, barAt.y), ImVec2(x + w, barAt.y + S(16.0f)),
                                ImGui::GetColorU32(Rgb(part.colour, 0.85f)));
            x += w;
          }
          ImGui::Dummy(ImVec2(0.0f, S(20.0f)));
          if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
          bool firstPart = true;
          for (const auto& part : parts) {
            if (!firstPart) ImGui::SameLine(0.0f, S(14.0f));
            firstPart = false;
            const ImVec2 swatch = ImGui::GetCursorScreenPos();
            const float size = ImGui::GetTextLineHeight() * 0.6f;
            const float top = swatch.y + (ImGui::GetTextLineHeight() - size) * 0.5f;
            draw->AddRectFilled(ImVec2(swatch.x, top), ImVec2(swatch.x + size, top + size),
                                ImGui::GetColorU32(Rgb(part.colour)));
            ImGui::Dummy(ImVec2(size + S(5.0f), 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s %.1f ms", part.label, part.ms);
          }
          if (g_fonts.caption) ImGui::PopFont();
        }

        // Hotkeys, only when one failed to register -- otherwise it is noise.
        if ((s.hotkeysRegistered & 31) != 31) {
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
          const struct { uint32_t bit; const char* what; const std::string* key; } keys[] = {
              {1, "HUD", &config.hotkeys.toggleHud},
              {2, "overlay", &config.hotkeys.toggleOverlay},
              {4, "next look", &config.hotkeys.nextPreset},
              {8, "previous look", &config.hotkeys.previousPreset},
              {16, "debug frames", &config.hotkeys.dumpFrames},
          };
          std::string failed;
          for (const auto& k : keys) {
            if ((s.hotkeysRegistered & k.bit) == 0) {
              if (!failed.empty()) failed += ", ";
              failed += DescribeHotkey(*k.key) + " (" + k.what + ")";
            }
          }
          ImGui::TextColored(Rgb(g_colors.fail), "Hotkeys not registered: %s", failed.c_str());
          Hint("Either the binding does not parse or another program owns it. Change it "
               "on the Setup page.");
        }

        if (*s.lastError) {
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
          ImGui::TextColored(Rgb(g_colors.fail), "The overlay reported");
          ImGui::TextWrapped("%s", s.lastError);
        }
      } else if (live.overlayRunning) {
        ImGui::TextDisabled("The overlay is starting. Numbers appear after the first "
                            "few frames.");
      } else {
        ImGui::TextDisabled("Nothing running.");
        Hint(blocked ? "Some checks are failing. Open Checks to see what."
             : missingComponents > 0 ? "Files are missing. Open Setup to install them."
             : !config.app.Chosen()
                 ? "Choose a window below, then press Start overlay."
             : !live.appRunning
                 ? "Start the app in borderless windowed mode, then press Start overlay."
                 : "Press Start overlay.");
      }

      ImGui::Dummy(ImVec2(0.0f, 20.0f));
      SectionHeading("Capture target");
      Hint("The overlay is keyed to a window, not to a game: any borderless window "
           "the compositor can see works -- a game, an emulator, a video player. The "
           "choice is remembered.");
      {
        static std::vector<WindowChoice> choices;
        static double lastRefresh = 0.0;
        static std::string filter(64, '\0');
        const double now = ImGui::GetTime();
        if (now - lastRefresh > 2.0) {
          choices = EnumerateCandidateWindows();
          lastRefresh = now;
        }
        const bool chosen = config.app.Chosen();
        ImGui::Text("Current: %s", chosen ? config.app.name.c_str() : "nothing chosen yet");
        if (chosen) {
          ImGui::TextDisabled("Window class \"%s\", title \"%s\"%s",
                              config.app.windowClass.c_str(), config.app.title.c_str(),
                              live.appRunning ? "" : "  --  not running");
        }

        if (live.overlayRunning) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(S(300.0f));
        ImGui::InputTextWithHint("##targetfilter", "Filter by title or class", filter.data(),
                                 filter.size());
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(S(90.0f), 0.0f))) {
          choices = EnumerateCandidateWindows();
          lastRefresh = now;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu window(s)", choices.size());

        // A list in the page, not a popup: it scrolls with the wheel like the
        // rest of the page, and nothing can hide off its bottom edge.
        const std::string needle = [&] {
          std::string n(filter.c_str());
          std::transform(n.begin(), n.end(), n.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          return n;
        }();
        const auto matches = [&](const WindowChoice& c) {
          if (needle.empty()) return true;
          std::string hay = c.title + " " + c.windowClass;
          std::transform(hay.begin(), hay.end(), hay.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
          return hay.find(needle) != std::string::npos;
        };
        if (ImGui::BeginListBox("##targets", ImVec2(-1.0f, S(220.0f)))) {
          for (const auto& choice : choices) {
            if (!matches(choice)) continue;
            ImGui::PushID(choice.hwnd);
            const bool isCurrent = chosen && choice.windowClass == config.app.windowClass &&
                                   choice.title == config.app.title;
            const std::string label = choice.title + "   [" + choice.windowClass + "]";
            if (ImGui::Selectable(label.c_str(), isCurrent)) {
              config.app.name = choice.title;
              config.app.windowClass = choice.windowClass;
              config.app.title = choice.title;
              // The look this window was last used with, if there is one.
              const std::string remembered = config.LookForApp(choice.windowClass);
              if (!remembered.empty() && remembered != config.activePreset) {
                for (const auto& p : presets) {
                  if (p.name == remembered) {
                    ApplyPreset(p, config);
                    GlobalLog().Info("look \"" + remembered + "\" restored for this window");
                    break;
                  }
                }
              }
              SaveConfig(configPath, config);
              results = RunAllProbes(sidecarDir, config.app);
              GlobalLog().Info("capture target set to \"" + choice.title + "\" (class " +
                               choice.windowClass + ")");
            }
            ImGui::PopID();
          }
          ImGui::EndListBox();
        }
        if (live.overlayRunning) ImGui::EndDisabled();
        Hint("Only visible, non-minimised windows with a title and a real client area "
             "are listed. A game in exclusive fullscreen has no window the compositor "
             "can see; switch it to borderless windowed and press Refresh. The choice "
             "is remembered and used again next time, if that window is running.");
      }
    }

    // ------------------------------------------------------------------- Setup
    if (section == Section::Setup) {
      SectionHeading("Required files");
      Hint("Two files have to sit next to the sidecar. Neither is ours to redistribute "
           "and nothing here downloads anything -- neither binary in this project can "
           "reach the network at all. Fetch them yourself, then point this at them. "
           "Hover a name for what it is for.");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));

      for (size_t i = 0; i < Components().size(); ++i) {
        const auto& component = Components()[i];
        const bool present = !InstalledFiles(component, sidecarDir).empty();

        ImGui::PushID(static_cast<int>(i));
        Dot(present, std::string(component.installedAs).c_str(), !component.required);
        if (ImGui::IsItemHovered()) {
          ImGui::SetNextWindowSize(ImVec2(S(420.0f), 0.0f));
          ImGui::BeginTooltip();
          ImGui::TextWrapped("%s", std::string(component.purpose).c_str());
          ImGui::Dummy(ImVec2(0.0f, 4.0f));
          ImGui::TextDisabled("Source: %s", std::string(component.source).c_str());
          ImGui::EndTooltip();
        }
        ImGui::SameLine(S(320.0f));
        if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
        ImGui::TextColored(present ? Rgb(g_colors.ok)
                           : component.required ? Rgb(g_colors.fail) : Rgb(g_colors.warn),
                           "%s", present ? "installed" : component.required ? "MISSING"
                                                                           : "optional");
        if (g_fonts.caption) ImGui::PopFont();
        ImGui::SameLine(S(420.0f));
        if (ImGui::SmallButton(present ? "Replace..." : "Choose file...")) {
          const fs::path picked = AskForFile(hwnd, L"DLL files\0*.dll\0All files\0*.*\0\0",
                                             L"Choose the file to install");
          if (!picked.empty()) {
            const std::string name = picked.filename().string();
            if (!FileMatchesComponent(component, name)) {
              setupMessage = name + " is not what this slot wants (" +
                             std::string(component.installedAs) + ").";
              setupMessageIsError = true;
            } else {
              const auto result = InstallComponent(component, picked, sidecarDir);
              setupMessage = result.message;
              setupMessageIsError = !result.ok;
              if (result.ok) {
                GlobalLog().Info(result.message);
                results = RunAllProbes(sidecarDir, config.app);
              }
            }
          }
        }
        ImGui::PopID();
      }
      if (!setupMessage.empty()) {
        ImGui::TextColored(setupMessageIsError ? Rgb(g_colors.fail) : Rgb(g_colors.ok),
                           "%s", setupMessage.c_str());
      }

      // ---- hotkeys
      ImGui::Dummy(ImVec2(0.0f, 16.0f));
      SectionHeading("Hotkeys");
      Hint("Global, so they work while the app has the keyboard. Written as "
           "\"ctrl+alt+key\" with at least one modifier. Applied when saved; the "
           "Status page shows which ones registered.");
      {
        const char* labels[5] = {"Toggle HUD", "Toggle overlay", "Next look", "Previous look",
                                 "Save debug frames"};
        std::string* targets[5] = {&config.hotkeys.toggleHud, &config.hotkeys.toggleOverlay,
                                   &config.hotkeys.nextPreset, &config.hotkeys.previousPreset,
                                   &config.hotkeys.dumpFrames};
        for (int i = 0; i < 5; ++i) {
          ImGui::PushID(i);
          ImGui::SetNextItemWidth(S(220.0f));
          if (ImGui::InputText(labels[i], hotkeyBuffers[i].data(), hotkeyBuffers[i].size())) {
            *targets[i] = hotkeyBuffers[i].c_str();
            dirty = true;
          }
          ImGui::SameLine();
          const std::string described = DescribeHotkey(*targets[i]);
          if (described == "invalid") {
            ImGui::TextColored(Rgb(g_colors.fail), "invalid -- not registered");
          } else {
            ImGui::TextDisabled("%s", described.c_str());
          }
          ImGui::PopID();
        }
        Hint((std::string("Named keys: ") + kNamedKeys + ". Ctrl+Alt+Backspace always takes "
              "the overlay down and cannot be rebound.").c_str());
      }

      // ---- overlay
      ImGui::Dummy(ImVec2(0.0f, 16.0f));
      SectionHeading("Overlay");
      if (ImGui::Checkbox("Show the HUD", &config.showHud)) dirty = true;
      if (ImGui::Checkbox("Show the overlay on start", &config.showOverlay)) dirty = true;

      ImGui::Dummy(ImVec2(0.0f, 14.0f));
      if (!dirty) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Text, Rgb(g_colors.goldBright));
      if (ImGui::Button("Save settings", ImVec2(S(170.0f), 0.0f))) save();
      ImGui::PopStyleColor();
      if (!dirty) ImGui::EndDisabled();
      ImGui::SameLine();
      if (dirty) ImGui::TextColored(Rgb(g_colors.warn), "Unsaved changes.");
    }

    // ------------------------------------------------------------------ Checks
    if (section == Section::Checks) {
      SectionHeading("System checks");
      Hint("Nothing here opens, reads, writes or hooks the app's process. The "
           "import table of every binary is checked against that claim at build "
           "time.");
      ImGui::Dummy(ImVec2(0.0f, 8.0f));
      if (ImGui::Button("Re-run checks", ImVec2(S(160.0f), 0.0f))) {
        results = RunAllProbes(sidecarDir, config.app);
        int failures = 0;
        for (const auto& r : results) {
          if (r.state == ProbeState::Fail) ++failures;
        }
        if (failures > 0) {
          GlobalLog().Error(std::to_string(failures) + " check(s) failing");
        } else {
          GlobalLog().Info("all checks green");
        }
      }
      ImGui::Dummy(ImVec2(0.0f, 8.0f));
      DrawBoard(results);
    }

    // ------------------------------------------------------------------ Tuning
    if (section == Section::Tuning) {
      SectionHeading("Choose a look");
      Hint("Pick one, or save your own. Two ship with the sidecar; the rest are yours, "
           "kept in presets.toml beside it. Hover a name for what it does; the preset "
           "hotkeys step through them in this order while you play.");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));

      size_t active = presets.size();
      for (size_t i = 0; i < presets.size(); ++i) {
        if (PresetMatchesConfig(presets[i], config)) { active = i; break; }
      }
      size_t deleteIndex = presets.size();
      // Which look this window is associated with, so the list answers "which
      // of these does this game use?" without pretending a preset belongs to
      // an app -- any look can be picked for any window.
      const std::string appLook = config.LookForApp(config.app.windowClass);
      // One row per look. The description lives in a tooltip rather than on the
      // page: it is read once and then only gets in the way of the list.
      for (size_t i = 0; i < presets.size(); ++i) {
        const auto& preset = presets[i];
        const bool selected = i == active;
        ImGui::PushID(static_cast<int>(i));

        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowAt = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0.0f, rowHeight))) {
          if (!selected) {
            ApplyPreset(preset, config);
            dirty = true;
          }
        }
        if (ImGui::IsItemHovered() && !preset.detail.empty()) {
          ImGui::SetNextWindowSize(ImVec2(S(420.0f), 0.0f));
          ImGui::BeginTooltip();
          ImGui::TextWrapped("%s", preset.detail.c_str());
          ImGui::EndTooltip();
        }
        if (selected) {
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(rowAt.x - 6.0f, rowAt.y), ImVec2(rowAt.x - 3.0f, rowAt.y + rowHeight),
              ImGui::GetColorU32(Rgb(g_colors.goldBright)));
        }

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorScreenPos(ImVec2(rowAt.x + 4.0f, rowAt.y));
        ImGui::TextColored(selected ? Rgb(g_colors.goldBright) : Rgb(g_colors.parchment),
                           "%s", preset.name.c_str());
        if (!appLook.empty() && preset.name == appLook && config.app.Chosen()) {
          ImGui::SameLine(0.0f, 8.0f);
          if (g_fonts.caption) ImGui::PushFont(g_fonts.caption);
          ImGui::TextDisabled("- %s", config.app.name.c_str());
          if (g_fonts.caption) ImGui::PopFont();
        }
        if (!preset.builtin) {
          // Measured, not guessed: a SmallButton is as wide as its label plus
          // the frame padding, and both of those grow with the display scale,
          // so any fixed reservation is wrong on a scaled screen. Positioned
          // from the row's own right edge in screen space, which cannot be
          // thrown off by where the cursor happens to be.
          const ImGuiStyle& st = ImGui::GetStyle();
          const float overwriteWidth =
              ImGui::CalcTextSize("Overwrite").x + st.FramePadding.x * 2.0f;
          const float deleteWidth =
              ImGui::CalcTextSize("Delete").x + st.FramePadding.x * 2.0f;
          const float total = overwriteWidth + deleteWidth + st.ItemSpacing.x;
          ImGui::SameLine();
          ImGui::SetCursorScreenPos(ImVec2(rowAt.x + rowWidth - total, rowAt.y));
          if (ImGui::SmallButton("Overwrite")) overwriteIndex = i;
          ImGui::SameLine();
          if (ImGui::SmallButton("Delete")) deleteIndex = i;
        }
        ImGui::PopID();
      }
      if (deleteIndex < presets.size()) {
        GlobalLog().Info("preset \"" + presets[deleteIndex].name + "\" deleted");
        presets.erase(presets.begin() + static_cast<std::ptrdiff_t>(deleteIndex));
        saveCustomPresets();
      }

      // Overwrite, with a confirmation: the current look replaces a saved one
      // and there is no undo, so the name is shown and the click is deliberate.
      if (overwriteIndex < presets.size()) ImGui::OpenPopup("Overwrite preset?");
      if (ImGui::BeginPopupModal("Overwrite preset?", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings)) {
        if (overwriteIndex < presets.size()) {
          ImGui::TextWrapped("Replace \"%s\" with the current settings? The saved look is "
                             "lost.", presets[overwriteIndex].name.c_str());
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
          if (ImGui::Button("Overwrite", ImVec2(S(120.0f), 0.0f))) {
            auto& target = presets[overwriteIndex];
            Preset replaced = PresetFromConfig(target.name, config);
            replaced.summary = target.summary.empty() ? "Saved from the manager." : target.summary;
            replaced.detail = target.detail;
            target = replaced;
            saveCustomPresets();
            config.activePreset = target.name;
            dirty = true;
            GlobalLog().Info("preset overwritten: \"" + target.name + "\"");
            overwriteIndex = static_cast<size_t>(-1);
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel", ImVec2(S(120.0f), 0.0f))) {
            overwriteIndex = static_cast<size_t>(-1);
            ImGui::CloseCurrentPopup();
          }
        } else {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      if (active == presets.size()) {
        ImGui::TextColored(Rgb(g_colors.warn),
                           "Custom -- your settings do not match any preset. Save them "
                           "below to keep them.");
      }

      ImGui::Dummy(ImVec2(0.0f, 8.0f));
      ImGui::SetNextItemWidth(S(300.0f));
      ImGui::InputTextWithHint("##presetname", "Name for the current look",
                               presetNameUtf8.data(), presetNameUtf8.size());
      ImGui::SameLine();
      {
        const std::string name(presetNameUtf8.c_str());
        bool shadowsBuiltin = false;
        for (const auto& p : presets) {
          if (p.builtin && p.name == name) shadowsBuiltin = true;
        }
        const bool canSave = !name.empty() && !shadowsBuiltin;
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("Save current look as preset", ImVec2(S(230.0f), 0.0f))) {
          Preset custom = PresetFromConfig(name, config);
          custom.summary = "Saved from the manager.";
          bool replaced = false;
          for (auto& p : presets) {
            if (!p.builtin && p.name == name) {
              p = custom;
              replaced = true;
            }
          }
          if (!replaced) presets.push_back(custom);
          saveCustomPresets();
          config.activePreset = name;
          dirty = true;
          GlobalLog().Info(std::string(replaced ? "preset replaced: \"" : "preset saved: \"") +
                           name + "\"");
        }
        if (!canSave) ImGui::EndDisabled();
        if (shadowsBuiltin) ImGui::TextColored(Rgb(g_colors.warn), "That name is taken by a built-in.");
      }

      ImGui::Dummy(ImVec2(0.0f, 14.0f));
      if (!dirty) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Text, Rgb(g_colors.goldBright));
      if (ImGui::Button("Save settings", ImVec2(S(170.0f), 0.0f))) save();
      ImGui::PopStyleColor();
      if (!dirty) ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Checkbox("Apply changes live", &liveApply);
      ImGui::SameLine();
      if (dirty) {
        ImGui::TextColored(Rgb(g_colors.warn), "Unsaved changes.");
      } else if (live.overlayRunning) {
        ImGui::TextDisabled("Saved settings reach the running overlay without a restart.");
      }
      Hint("Live: release a slider and it is saved and sent to the overlay. Compose "
           "knobs change on the next frame; pass settings and model resolution are "
           "hot-swapped a frame later; the pass type and flow grid rebuild "
           "the pipeline with a brief flicker.");

      ImGui::Dummy(ImVec2(0.0f, 10.0f));
      if (ImGui::Checkbox("Advanced tuning", &config.advancedTuning)) {
        SaveConfig(configPath, config);
      }
      Hint("Shows the settings that are rarely worth touching: the pipeline internals, "
           "the model's preset slot and its own mask switches, pass chaining and "
           "per-pass tuning. Hiding them changes nothing about what "
           "the overlay does -- the values keep working, they are just out of the way.");

      if (config.advancedTuning) {
      ImGui::Dummy(ImVec2(0.0f, 16.0f));
      SectionHeading("Pipeline");
      {
        int passIndex = config.neuralPass == "direct" ? 1 : 0;
        const char* passes[] = {"passthrough -- capture and present, untouched",
                                "direct -- DLSS 5 neural rendering"};
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::Combo("Neural pass", &passIndex, passes, 2)) {
          config.neuralPass = passIndex == 1 ? "direct" : "passthrough";
          dirty = true;
        }
        Hint("Passthrough is the honest A/B baseline: the same capture and present "
             "path with no neural work in it at all.");
      }
      {
        const char* grids[] = {"1 -- finest, most expensive", "2", "4 -- default"};
        int index = config.flowGridSize == 1 ? 0 : config.flowGridSize == 2 ? 1 : 2;
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::Combo("Optical flow grid", &index, grids, 3)) {
          config.flowGridSize = index == 0 ? 1u : index == 1 ? 2u : 4u;
          dirty = true;
        }
        Hint("Pixels per estimated motion vector. Finer costs more and is not "
        "obviously better: the vectors are estimated from colour, so a finer "
        "grid can sharpen the estimate or the error equally.");
        }
      }   // advanced: Pipeline

      ImGui::Dummy(ImVec2(0.0f, 16.0f));
      SectionHeading("Neural rendering");
      Hint("The model's own controls, as the runtime names them. It reads them when "
           "a pass is built; a change hot-swaps the passes a frame later when the "
           "overlay is running. With more than one pass, each pass feeds the next "
           "and has its own settings.");
      ImGui::Dummy(ImVec2(0.0f, 8.0f));
      {
        auto effective = config.nr.Effective();
        int passCount = static_cast<int>(effective.size());
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::SliderInt("Passes", &passCount, 1, 4)) {
          if (passCount <= 1) {
            config.nr.base = effective.front();
            config.nr.passes.clear();
          } else {
            config.nr.passes = effective;
            while (static_cast<int>(config.nr.passes.size()) < passCount) {
              config.nr.passes.push_back(config.nr.passes.back());
            }
            config.nr.passes.resize(static_cast<size_t>(passCount));
          }
          dirty = true;
        }
        Hint("Each pass costs about as much GPU time as the first.");
        if (config.advancedTuning && config.nr.Effective().size() > 1) {
          if (ImGui::Checkbox("Chain passes through the compose", &config.nr.chainComposed)) {
            dirty = true;
          }
          Hint("On: each pass sees the colour-preserved, highlight-protected result of "
               "the one before. Off: raw output feeds the next pass and brightening "
               "compounds each time.");
        }
        ImGui::SetNextItemWidth(440.0f);
        {
          int percent = static_cast<int>(config.nr.modelScale * 100.0f + 0.5f);
          if (ImGui::SliderInt("Model resolution (%)", &percent, 50, 100)) {
            config.nr.modelScale = static_cast<float>(percent) / 100.0f;
            dirty = true;
          }
        }
        Hint("The size the model works at, as a fraction of the capture. The GPU cost "
             "scales with it; 75% costs a little over half. Colour and detail come "
             "back from the full-resolution frame in the compose; what is lost is the "
             "model's own fine structure, which is what the option below is for.");
        {
          const bool several = config.nr.Effective().size() > 1;
          if (!several) ImGui::BeginDisabled();
          if (ImGui::Checkbox("Final pass at full resolution", &config.nr.finalPassFull)) {
            dirty = true;
          }
          if (!several) ImGui::EndDisabled();
        }
        Hint("With two or more passes: the earlier ones run at the model resolution "
             "above and decide the lighting; the last runs at full resolution on the "
             "result and adds the fine structure. Most of the quality of every pass at "
             "100% for the cost of one full pass plus the cheap ones. Implies chaining "
             "through the compose.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextUnformatted("How the model's answer is composed with the frame");
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::SliderFloat("Keep original colour", &config.nr.colourPreserve, 0.0f, 1.0f,
                               "%.2f")) {
          dirty = true;
        }
        Hint("The model returns lighting and structure but drains colour, and its "
             "cinematic style grades colour on purpose. At 1 the frame's own hue and "
             "saturation are kept and only the light is taken from the model; lower it "
             "to let the model's colour grade through. No model knob does this: local "
             "tone is luminance only.");
        ImGui::SetNextItemWidth(440.0f);
        if (ImGui::SliderFloat("Protect highlights", &config.nr.highlightProtect, 0.0f, 1.0f,
                               "%.2f")) {
          dirty = true;
        }
        Hint("Holds back the model's brightening where the frame is already near white, "
             "and rolls off what is left before it clips. On an SDR desktop there is no "
             "headroom to pass highlights through, so this is what keeps a bright scene "
             "from going to paper. Darkening is always kept.");

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextUnformatted("HDR games");
        Hint("Used only when the app's display is in HDR mode. The capture is then "
             "FP16 scRGB, the model is shown a tone-mapped SDR view, and its edit is "
             "applied back to the HDR frame; the overlay presents in HDR. These set "
             "the tone-map.");
        ImGui::SetNextItemWidth(440.0f);
        {
          const bool automatic = config.nr.paperWhiteNits <= 0.0f;
          float shown = automatic ? 80.0f : config.nr.paperWhiteNits;
          if (ImGui::SliderFloat("Paper white (nits)", &shown, 80.0f, 1000.0f,
                                 automatic ? "automatic (display's SDR white)" : "%.0f")) {
            config.nr.paperWhiteNits = shown;
            dirty = true;
          }
          if (!automatic) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Auto")) {
              config.nr.paperWhiteNits = 0.0f;
              dirty = true;
            }
          }
        }
        Hint("The luminance that maps to SDR white in the model's view. Automatic uses "
             "the display's own SDR white level, which is what an SDR game on an HDR "
             "desktop is composed at -- the right answer almost always. Set a number "
             "only for a true HDR game whose paper white you know. How far above it the "
             "model sees is derived from the display's peak brightness.");
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        const auto drawPass = [&](NrPassSettings& s, const char* label) {
          ImGui::PushID(label);
          if (label && *label) ImGui::TextUnformatted(label);
          const char* styles[] = {"Standard -- strongest; deepens lighting, boosts local contrast",
                                  "Natural -- lighter touch", "Cinematic -- tone and colour"};
          ImGui::SetNextItemWidth(440.0f);
          if (ImGui::Combo("Style", &s.style, styles, 3)) dirty = true;
          if (config.advancedTuning) {
            const char* modelPresets[] = {"Model's choice", "Preset 1", "Preset 2", "Preset 3"};
            ImGui::SetNextItemWidth(440.0f);
            if (ImGui::Combo("Model preset", &s.preset, modelPresets, 4)) dirty = true;
          }
          ImGui::SetNextItemWidth(440.0f);
          if (ImGui::SliderFloat("Intensity", &s.intensity, 0.0f, 2.0f, "%.2f")) dirty = true;
          ImGui::SetNextItemWidth(440.0f);
          if (ImGui::SliderFloat("Local structure", &s.localStructure, 0.0f, 2.0f, "%.2f")) {
            dirty = true;
          }
          ImGui::SetNextItemWidth(440.0f);
          if (ImGui::SliderFloat("Local tone", &s.localTone, 0.0f, 2.0f, "%.2f")) dirty = true;
          ImGui::SetNextItemWidth(440.0f);
          if (ImGui::SliderFloat("Skin structure (-1 = off)", &s.skinStructure, -1.0f, 2.0f,
                                 "%.2f")) {
            dirty = true;
          }
          if (config.advancedTuning) {
            if (ImGui::Checkbox("Model's own UI mask", &s.autoMask)) dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("UI correction", &s.uiCorrection)) dirty = true;
          }
          ImGui::PopID();
        };
        if (config.nr.passes.empty()) {
          drawPass(config.nr.base, "");
        } else {
          // A look whose passes differ was tuned per pass; show it that way
          // rather than flattening it to the first pass on sight.
          if (!config.nr.perPassTuning) {
            const auto same = [](const NrPassSettings& a, const NrPassSettings& b) {
              return a.preset == b.preset && a.style == b.style && a.intensity == b.intensity &&
                     a.localStructure == b.localStructure && a.localTone == b.localTone &&
                     a.skinStructure == b.skinStructure && a.autoMask == b.autoMask &&
                     a.uiCorrection == b.uiCorrection;
            };
            for (size_t i = 1; i < config.nr.passes.size(); ++i) {
              if (!same(config.nr.passes[i], config.nr.passes.front())) {
                config.nr.perPassTuning = true;
                break;
              }
            }
          }
          if (config.advancedTuning) {
            if (ImGui::Checkbox("Tune each pass separately", &config.nr.perPassTuning)) {
              if (!config.nr.perPassTuning) {
                // Collapsing: the first pass's settings become everyone's.
                config.nr.base = config.nr.passes.front();
                for (auto& p : config.nr.passes) p = config.nr.base;
              }
              dirty = true;
            }
            Hint("Off: one set of model controls applies to every pass. On: each pass has "
                 "its own -- a different style per pass, say -- at the cost of more to "
                 "fiddle with.");
          }
          if (!config.nr.perPassTuning) {
            // One set of controls, bound to the first pass and copied to the
            // rest, so the file and the runtime always see full passes.
            drawPass(config.nr.passes.front(), "All passes");
            config.nr.base = config.nr.passes.front();
            for (size_t i = 1; i < config.nr.passes.size(); ++i) {
              config.nr.passes[i] = config.nr.passes.front();
            }
          } else {
            for (size_t i = 0; i < config.nr.passes.size(); ++i) {
              char label[32];
              std::snprintf(label, sizeof(label), "Pass %zu", i + 1);
              ImGui::Dummy(ImVec2(0.0f, 6.0f));
              drawPass(config.nr.passes[i], label);
            }
          }
        }
      }

      ImGui::Dummy(ImVec2(0.0f, 12.0f));
      if (ImGui::Button("Reset look to Recommended", ImVec2(S(230.0f), 0.0f))) {
        ApplyPreset(BuiltinPresets().front(), config);
        dirty = true;
      }
    }

    // --------------------------------------------------------------------- Log
    if (section == Section::Log) {
      SectionHeading("Log");
      const std::string lastError = GlobalLog().LastError();
      if (!lastError.empty()) {
        ImGui::TextColored(Rgb(g_colors.fail), "Last error");
        ImGui::TextWrapped("%s", lastError.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
      }
      ImGui::BeginChild("logscroll", ImVec2(0.0f, -S(28.0f)), ImGuiChildFlags_Border);
      if (g_fonts.mono) ImGui::PushFont(g_fonts.mono);
      for (const auto& line : GlobalLog().Recent()) ImGui::TextUnformatted(line.c_str());
      if (g_fonts.mono) ImGui::PopFont();
      ImGui::EndChild();
      const uint64_t dropped = GlobalLog().Dropped();
      if (dropped > 0) {
        ImGui::TextDisabled("%llu earlier line(s) dropped", static_cast<unsigned long long>(dropped));
      }
    }

    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::Unindent(kPad);
    if (!noticeAcknowledged) ImGui::EndDisabled();
    ImGui::End();

    if (!noticeAcknowledged) {
      ImGui::OpenPopup(kFirstRunTitle);
      const ImVec2 center = viewport->GetCenter();
      ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(S(600.0f), 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24.0f), S(20.0f)));
      if (ImGui::BeginPopupModal(kFirstRunTitle, nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoTitleBar)) {
        if (g_fonts.heading) ImGui::PushFont(g_fonts.heading);
        ImGui::TextColored(Rgb(g_colors.goldBright), "Before you use this");
        if (g_fonts.heading) ImGui::PopFont();
        GoldRule(0.45f, 10.0f);
        ImGui::TextWrapped("%s", kFirstRunBody);
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Rgb(g_colors.goldBright));
        if (ImGui::Button("I understand", ImVec2(S(170.0f), 0.0f))) {
          noticeAcknowledged = true;
          // The config file's existence records the acknowledgement. It lives
          // beside the manager, never in the app's directory (I5).
          SaveConfig(configPath, config);
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
      }
      ImGui::PopStyleVar();
    }

    ImGui::Render();
    const float clear[4] = {0.043f, 0.047f, 0.071f, 1.0f};
    ID3D11RenderTargetView* rtv = g_backBufferRtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
