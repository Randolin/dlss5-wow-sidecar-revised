#pragma once
#include <string>
#include <string_view>

namespace sidecar {

// UTF-8 <-> UTF-16, for window titles and class names crossing between the
// config file (UTF-8) and the Win32 API (UTF-16). Invalid input maps to the
// replacement character rather than failing.
std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

}  // namespace sidecar
