#include "core/Hotkeys.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace sidecar {
namespace {

std::string Lower(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == ' ' || c == '\t') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::vector<std::string> Split(const std::string& text) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t plus = text.find('+', start);
    const size_t end = plus == std::string::npos ? text.size() : plus;
    parts.push_back(text.substr(start, end - start));
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  return parts;
}

struct NamedKey {
  const char* name;
  unsigned int vk;
  const char* pretty;
};

constexpr NamedKey kNamed[] = {
    {"pageup", VK_PRIOR, "PageUp"},     {"pagedown", VK_NEXT, "PageDown"},
    {"home", VK_HOME, "Home"},          {"end", VK_END, "End"},
    {"insert", VK_INSERT, "Insert"},    {"delete", VK_DELETE, "Delete"},
    {"space", VK_SPACE, "Space"},       {"tab", VK_TAB, "Tab"},
    {"backspace", VK_BACK, "Backspace"}, {"enter", VK_RETURN, "Enter"},
    {"return", VK_RETURN, "Enter"},     {"escape", VK_ESCAPE, "Escape"},
    {"esc", VK_ESCAPE, "Escape"},       {"up", VK_UP, "Up"},
    {"down", VK_DOWN, "Down"},          {"left", VK_LEFT, "Left"},
    {"right", VK_RIGHT, "Right"},       {"pause", VK_PAUSE, "Pause"},
    {"scrolllock", VK_SCROLL, "ScrollLock"},
};

std::optional<unsigned int> KeyFromName(const std::string& name, std::string* pretty) {
  if (name.size() == 1) {
    const char c = name[0];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (pretty) *pretty = std::string(1, upper);
      return static_cast<unsigned int>(upper);
    }
    return std::nullopt;
  }
  for (const auto& k : kNamed) {
    if (name == k.name) {
      if (pretty) *pretty = k.pretty;
      return k.vk;
    }
  }
  if (name.size() >= 2 && name[0] == 'f') {
    const std::string digits = name.substr(1);
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(),
                                       [](char c) { return c >= '0' && c <= '9'; })) {
      const int n = std::stoi(digits);
      if (n >= 1 && n <= 24) {
        if (pretty) *pretty = "F" + std::to_string(n);
        return static_cast<unsigned int>(VK_F1 + (n - 1));
      }
    }
  }
  if (name.size() == 7 && name.compare(0, 6, "numpad") == 0 && name[6] >= '0' && name[6] <= '9') {
    if (pretty) *pretty = "Numpad" + std::string(1, name[6]);
    return static_cast<unsigned int>(VK_NUMPAD0 + (name[6] - '0'));
  }
  return std::nullopt;
}

struct Parsed {
  Hotkey hotkey;
  std::string pretty;
};

std::optional<Parsed> Parse(std::string_view text) {
  const std::string lowered = Lower(text);
  if (lowered.empty()) return std::nullopt;
  const auto parts = Split(lowered);
  if (parts.size() < 2) return std::nullopt;   // a modifier and a key, at least

  Parsed out;
  std::string pretty;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    const auto& p = parts[i];
    unsigned int flag = 0;
    const char* name = nullptr;
    if (p == "ctrl" || p == "control") { flag = MOD_CONTROL; name = "Ctrl"; }
    else if (p == "alt") { flag = MOD_ALT; name = "Alt"; }
    else if (p == "shift") { flag = MOD_SHIFT; name = "Shift"; }
    else if (p == "win") { flag = MOD_WIN; name = "Win"; }
    else return std::nullopt;
    if (out.hotkey.modifiers & flag) return std::nullopt;   // repeated modifier
    out.hotkey.modifiers |= flag;
    pretty += name;
    pretty += '+';
  }
  std::string keyPretty;
  const auto vk = KeyFromName(parts.back(), &keyPretty);
  if (!vk) return std::nullopt;
  out.hotkey.vk = *vk;
  out.pretty = pretty + keyPretty;
  return out;
}

}  // namespace

std::optional<Hotkey> ParseHotkey(std::string_view text) {
  const auto parsed = Parse(text);
  if (!parsed) return std::nullopt;
  return parsed->hotkey;
}

std::string DescribeHotkey(std::string_view text) {
  const auto parsed = Parse(text);
  return parsed ? parsed->pretty : std::string("invalid");
}

}  // namespace sidecar
