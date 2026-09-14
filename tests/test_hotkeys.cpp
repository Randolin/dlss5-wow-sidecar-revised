#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include "core/Hotkeys.h"

using namespace sidecar;

TEST_CASE("the default bindings parse", "[unit]") {
  const auto hud = ParseHotkey("ctrl+alt+h");
  REQUIRE(hud);
  REQUIRE(hud->modifiers == (MOD_CONTROL | MOD_ALT));
  REQUIRE(hud->vk == 'H');

  const auto next = ParseHotkey("ctrl+alt+pageup");
  REQUIRE(next);
  REQUIRE(next->vk == VK_PRIOR);

  const auto prev = ParseHotkey("ctrl+alt+pagedown");
  REQUIRE(prev);
  REQUIRE(prev->vk == VK_NEXT);
}

TEST_CASE("case and spacing do not matter", "[unit]") {
  const auto a = ParseHotkey("Ctrl + Shift + F9");
  REQUIRE(a);
  REQUIRE(a->modifiers == (MOD_CONTROL | MOD_SHIFT));
  REQUIRE(a->vk == VK_F9);
  REQUIRE(DescribeHotkey("CONTROL+alt+f12") == "Ctrl+Alt+F12");
  REQUIRE(DescribeHotkey("alt+numpad5") == "Alt+Numpad5");
}

TEST_CASE("a bare key, an unknown key or a repeated modifier is invalid", "[unit]") {
  // A bare letter registered globally would steal it from the game.
  REQUIRE_FALSE(ParseHotkey("h"));
  REQUIRE_FALSE(ParseHotkey("ctrl+alt+bogus"));
  REQUIRE_FALSE(ParseHotkey("ctrl+ctrl+h"));
  REQUIRE_FALSE(ParseHotkey(""));
  REQUIRE_FALSE(ParseHotkey("ctrl+"));
  REQUIRE(DescribeHotkey("nope") == "invalid");
}

TEST_CASE("digits and the named keys resolve", "[unit]") {
  REQUIRE(ParseHotkey("alt+5")->vk == '5');
  REQUIRE(ParseHotkey("alt+home")->vk == VK_HOME);
  REQUIRE(ParseHotkey("alt+delete")->vk == VK_DELETE);
  REQUIRE(ParseHotkey("win+f24")->vk == VK_F24);
  REQUIRE_FALSE(ParseHotkey("alt+f25"));
}
