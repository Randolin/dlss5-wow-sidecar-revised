#pragma once
#include <array>
#include <atomic>
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

// The recurring lines, each of which can be silenced independently.
//
// System events and anything at Warn or Error are never categorised and are
// always written: a log that can be configured into saying nothing about a
// failure is worse than no log. These are the per-frame and per-window lines
// that make a session's file thousands of lines long, and that only matter
// while something specific is being investigated.
enum class LogCategory : uint32_t {
  Performance = 1u << 0,   // the per-window frame budget line
  Stages = 1u << 1,        // the per-stage GPU breakdown under it
  Capture = 1u << 2,       // capture-rate requests as they are re-aimed
  Neural = 1u << 3,        // live tuning updates inside the neural pass
};

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

  // Which categories are written. Zero -- the default -- writes none of them,
  // which is what keeps an ordinary session's log readable. Set from the
  // config file and updated on a live reload; read on the render thread, so
  // it is atomic rather than guarded.
  void SetVerboseCategories(uint32_t mask) {
    verbose_.store(mask, std::memory_order_relaxed);
  }
  uint32_t VerboseCategories() const { return verbose_.load(std::memory_order_relaxed); }
  bool VerboseEnabled(LogCategory category) const {
    return (VerboseCategories() & static_cast<uint32_t>(category)) != 0;
  }

  // Written only when its category is on. Costs an atomic load and nothing
  // else when it is off -- but the caller still pays for building the string,
  // so anything expensive to format should be guarded with VerboseEnabled.
  void Verbose(LogCategory category, std::string_view message) {
    if (VerboseEnabled(category)) Write(LogLevel::Info, message);
  }

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
  std::atomic<uint32_t> verbose_{0};
  std::string lastError_;
  std::ofstream file_;
  std::chrono::steady_clock::time_point opened_ = std::chrono::steady_clock::now();
};

// The process-wide log. A singleton because every subsystem needs to reach it
// and threading one through every constructor would be noise.
Log& GlobalLog();

}  // namespace sidecar
