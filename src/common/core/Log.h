#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sidecar {

enum class LogLevel { Info, Warn, Error };

// File sink plus a fixed-capacity in-memory ring.
//
// The ring exists because the manager and the HUD both need to show what most
// recently happened without reading a file back, and because a diagnostic the
// operator can paste into a bug report is worth more than one that only exists
// under a debugger. It never grows, so writing to it cannot allocate its way
// into a frame-time spike on the render thread.
class Log {
 public:
  static constexpr size_t kCapacity = 256;

  Log() = default;

  // Optional. Without it the log is memory-only, which is what tests use.
  // Any existing file at `path` is moved aside to `<path>.prev` first: a crash
  // takes its own explanation with it otherwise, since the next launch
  // truncates the file before anyone reads it.
  bool OpenFile(const std::filesystem::path& path);

  // Lines carry milliseconds since the log opened. Relative rather than
  // wall-clock because the questions asked of a log are "how long after the
  // start" and "how far apart were these two", not "what time was it".
  void Write(LogLevel level, std::string_view message);

  void Info(std::string_view message) { Write(LogLevel::Info, message); }
  void Warn(std::string_view message) { Write(LogLevel::Warn, message); }
  void Error(std::string_view message) { Write(LogLevel::Error, message); }

  // Lines a level below this are discarded, and are not counted as dropped.
  void SetMinimumLevel(LogLevel level);

  std::vector<std::string> Recent() const;

  // Most recent Error line, so callers do not have to walk the ring for the
  // one thing they almost always want.
  std::string LastError() const;

  // Lines the ring overwrote. A non-zero count means diagnostics were lost.
  uint64_t Dropped() const;

  void Clear();

 private:
  mutable std::mutex mutex_;
  std::array<std::string, kCapacity> lines_;
  size_t next_ = 0;
  size_t count_ = 0;
  uint64_t dropped_ = 0;
  LogLevel minimum_ = LogLevel::Info;
  std::string lastError_;
  std::ofstream file_;
  std::chrono::steady_clock::time_point opened_ = std::chrono::steady_clock::now();
};

// The process-wide log. A singleton because every subsystem needs to reach it
// and threading one through every constructor would be noise.
Log& GlobalLog();

}  // namespace sidecar
