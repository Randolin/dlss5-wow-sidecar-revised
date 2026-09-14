#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace sidecar {

// A global hotkey, as RegisterHotKey wants it: MOD_* flags and a virtual key.
struct Hotkey {
  unsigned int modifiers = 0;
  unsigned int vk = 0;
};

// Parses "ctrl+alt+h", "ctrl+shift+f9", "alt+pageup" and the like. Case does
// not matter, neither does spacing around the plus signs. Modifiers: ctrl or
// control, alt, shift, win. Keys: a single letter or digit, f1-f24, or one of
// the named keys below. At least one modifier is required, because a bare
// letter registered globally would steal it from the game.
//
// Pure, so it is unit-tested; nothing here touches the OS.
std::optional<Hotkey> ParseHotkey(std::string_view text);

// The canonical spelling of a hotkey string ("Ctrl+Alt+H"), or "invalid" when
// it does not parse. For display beside the edit box.
std::string DescribeHotkey(std::string_view text);

// The named keys ParseHotkey understands, for a hint in the manager.
inline constexpr const char* kNamedKeys =
    "pageup, pagedown, home, end, insert, delete, space, tab, backspace, enter, "
    "escape, up, down, left, right, numpad0-numpad9, f1-f24";

}  // namespace sidecar
