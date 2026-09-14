#include "manager/Install.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace sidecar {
namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The sidecar writes these itself. They are listed for the uninstaller because
// leaving a stale config and two logs behind after "remove everything" is the
// kind of tidiness failure that makes people delete the whole folder by hand.
constexpr std::string_view kGeneratedFiles[] = {
    "sidecar.toml", "presets.toml", "sidecar.log", "sidecar-manager.log",
    "ui_mask_calibration.toml", "ui_mask_diff.bmp",
    "nr_input.bmp", "nr_output.bmp", "presented.bmp",
};

}  // namespace

const std::vector<Component>& Components() {
  // Two operator-supplied files. The forwarder the runtime is called through
  // is built with the sidecar and is not the operator's to fetch.
  static const std::vector<Component> components = {
      {"nvngx_dlssnr.dll",
       "DLSS 5 neural rendering runtime",
       "The neural network itself. On an RTX 40 card this must be a build "
       "patched for Ada; the stock runtime is Blackwell-only and fails at "
       "feature creation with no diagnostic.",
       "nvngx_dlssnr.dll",
       "github.com/rakanki911/DLSS5-Swapper releases",
       true},
      {"nvngx_dlss.dll",
       "DLSS upscaling runtime",
       "The neural-rendering runtime builds a DLSS feature of its own for its "
       "temporal pass, so this has to be beside it even though the sidecar never "
       "creates one directly.",
       "nvngx_dlss.dll",
       "github.com/rakanki911/DLSS5-Swapper releases, or any game shipping DLSS",
       true},
  };
  return components;
}

bool FileMatchesComponent(const Component& component, std::string_view fileName) {
  const std::string needle = Lower(fileName);
  const std::string accepts = Lower(component.accepts);

  // Whole-token comparison, not a substring search: `nvngx_dlss.dll` is a
  // prefix of `nvngx_dlssnr.dll`, they do entirely different jobs, and swapping
  // them produces a runtime that loads and then refuses to create a feature.
  size_t start = 0;
  while (start <= accepts.size()) {
    const size_t comma = accepts.find(',', start);
    const size_t end = comma == std::string::npos ? accepts.size() : comma;
    if (accepts.compare(start, end - start, needle) == 0) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

size_t ComponentForFile(std::string_view fileName) {
  const auto& components = Components();
  for (size_t i = 0; i < components.size(); ++i) {
    if (FileMatchesComponent(components[i], fileName)) return i;
  }
  return static_cast<size_t>(-1);
}

std::vector<std::string> AcceptedNames(const Component& component) {
  std::vector<std::string> names;
  const std::string accepts = Lower(component.accepts);
  size_t start = 0;
  while (start <= accepts.size()) {
    const size_t comma = accepts.find(',', start);
    const size_t end = comma == std::string::npos ? accepts.size() : comma;
    if (end > start) names.push_back(accepts.substr(start, end - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return names;
}

std::vector<fs::path> InstalledFiles(const Component& component, const fs::path& sidecarDir) {
  // Checked by name rather than by listing the directory: this runs every
  // frame on the manager's Setup page, and a couple of exists() calls are
  // cheaper than enumerating a folder that also holds a 160 MB runtime.
  std::vector<fs::path> found;
  std::error_code ec;
  for (const auto& name : AcceptedNames(component)) {
    const fs::path candidate = sidecarDir / name;
    if (fs::exists(candidate, ec) && !ec) found.push_back(candidate);
  }
  return found;
}

InstallResult InstallComponent(const Component& component, const fs::path& source,
                               const fs::path& sidecarDir) {
  std::error_code ec;
  if (!fs::exists(source, ec) || ec) {
    return {false, "That file no longer exists."};
  }
  if (!fs::is_regular_file(source, ec) || ec) {
    return {false, "That is not a file."};
  }

  const fs::path destination = sidecarDir / std::string(component.installedAs);

  // A source that is already the destination is not an error worth a copy: the
  // filesystem would happily truncate the file to zero on the way through.
  if (fs::exists(destination, ec) && !ec && fs::equivalent(source, destination, ec) && !ec) {
    return {true, "Already installed."};
  }

  fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    return {false, "Could not copy it in: " + ec.message()};
  }
  return {true, "Installed " + destination.filename().string() + "."};
}

std::vector<fs::path> UninstallPlan(const fs::path& sidecarDir, bool includeGeneratedFiles) {
  std::vector<fs::path> plan;
  std::error_code ec;
  for (const auto& component : Components()) {
    for (const auto& file : InstalledFiles(component, sidecarDir)) plan.push_back(file);
  }
  if (includeGeneratedFiles) {
    for (const auto& name : kGeneratedFiles) {
      const fs::path path = sidecarDir / std::string(name);
      if (fs::exists(path, ec) && !ec) plan.push_back(path);
    }
  }
  return plan;
}

InstallResult RemoveAll(const std::vector<fs::path>& plan) {
  size_t removed = 0;
  for (const auto& path : plan) {
    std::error_code ec;
    if (!fs::remove(path, ec) || ec) {
      // Almost always a file still mapped into a running process, which is
      // worth saying rather than reporting a bare failure.
      return {false, "Could not remove " + path.filename().string() +
                         ". Stop the overlay and close anything using it, then try again."};
    }
    ++removed;
  }
  return {true, "Removed " + std::to_string(removed) + " file(s)."};
}

}  // namespace sidecar
