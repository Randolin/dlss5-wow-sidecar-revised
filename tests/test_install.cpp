#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "manager/Install.h"

using namespace sidecar;

namespace {

const Component& ByName(std::string_view installedAs) {
  for (const auto& component : Components()) {
    if (component.installedAs == installedAs) return component;
  }
  FAIL("no such component: " << installedAs);
  return Components().front();
}

}  // namespace

TEST_CASE("every component names a file, a source and a purpose", "[unit]") {
  // A missing dependency that cannot tell the operator where to get it is not a
  // diagnostic, and this list is the only place that information exists.
  for (const auto& component : Components()) {
    REQUIRE_FALSE(component.installedAs.empty());
    REQUIRE_FALSE(component.title.empty());
    REQUIRE_FALSE(component.purpose.empty());
    REQUIRE_FALSE(component.source.empty());
    REQUIRE_FALSE(component.accepts.empty());
  }
}

TEST_CASE("the Setup page asks for the two runtimes and nothing else", "[unit]") {
  // The neural runtime, and the DLSS runtime it builds a feature from
  // internally. Nothing from the retired ReShade route.
  REQUIRE(Components().size() == 2);
  REQUIRE(ByName("nvngx_dlssnr.dll").required);
  REQUIRE(ByName("nvngx_dlss.dll").required);
  for (const auto& c : Components()) {
    REQUIRE(std::string(c.accepts).find("addon") == std::string::npos);
    REQUIRE(std::string(c.accepts).find("dxgi") == std::string::npos);
  }
}

TEST_CASE("file matching is case-insensitive and whole-name", "[unit]") {
  const auto& nr = ByName("nvngx_dlssnr.dll");
  REQUIRE(FileMatchesComponent(nr, "nvngx_dlssnr.dll"));
  REQUIRE(FileMatchesComponent(nr, "NVNGX_DLSSNR.DLL"));
  // A prefix of the right name is the wrong file.
  REQUIRE_FALSE(FileMatchesComponent(nr, "nvngx_dlss.dll"));
  REQUIRE_FALSE(FileMatchesComponent(nr, "nvngx_dlssnr.dll.bak"));
  REQUIRE_FALSE(FileMatchesComponent(ByName("nvngx_dlss.dll"), "nvngx_dlssnr.dll"));
}

TEST_CASE("a file matching nothing reports no component", "[unit]") {
  REQUIRE(ComponentForFile("readme.txt") == static_cast<size_t>(-1));
  REQUIRE(ComponentForFile("") == static_cast<size_t>(-1));
  REQUIRE(ComponentForFile("dxgi.dll") == static_cast<size_t>(-1));
  REQUIRE(ComponentForFile("nvngx_dlss.dll") != static_cast<size_t>(-1));
  REQUIRE(ComponentForFile("nvngx_dlss.dll") != ComponentForFile("nvngx_dlssnr.dll"));
}

TEST_CASE("presence and the uninstall plan are judged on disk", "[unit]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "dlss5-sidecar-install-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  REQUIRE_FALSE(ec);

  const auto& nr = ByName("nvngx_dlssnr.dll");
  REQUIRE(InstalledFiles(nr, dir).empty());
  REQUIRE(UninstallPlan(dir, false).empty());

  { std::ofstream(dir / "nvngx_dlssnr.dll") << "x"; }
  REQUIRE(InstalledFiles(nr, dir).size() == 1);
  REQUIRE(UninstallPlan(dir, false).size() == 1);

  // Generated files join the plan only when asked, and only if they exist.
  { std::ofstream(dir / "presets.toml") << "x"; }
  REQUIRE(UninstallPlan(dir, false).size() == 1);
  REQUIRE(UninstallPlan(dir, true).size() == 2);

  fs::remove_all(dir, ec);
}

TEST_CASE("installing refuses a self-copy and a missing source", "[unit]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "dlss5-sidecar-install-copy";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const auto& nr = ByName("nvngx_dlssnr.dll");

  REQUIRE_FALSE(InstallComponent(nr, dir / "does-not-exist.dll", dir).ok);

  { std::ofstream(dir / "nvngx_dlssnr.dll") << "already here"; }
  const auto self = InstallComponent(nr, dir / "nvngx_dlssnr.dll", dir);
  REQUIRE(self.ok);
  REQUIRE(fs::file_size(dir / "nvngx_dlssnr.dll") > 0);   // not truncated

  fs::remove_all(dir, ec);
}
