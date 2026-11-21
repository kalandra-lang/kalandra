#pragma once
#include "kalandra/CliOutput.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace kalandra::cli {
class BuildProgress {
public:
  explicit BuildProgress(Output options);
  ~BuildProgress();
  void phase(std::string_view name, std::string_view subject);
  void built(std::string_view output, std::string_view target);
  std::ostream &diagnostic();
  void replay(std::istream &messages);
  bool live() const { return interactive; }

private:
  using Clock = std::chrono::steady_clock;
  Output options;
  std::optional<unsigned long> originalMode;
  bool interactive, color;
  const Clock::time_point start = Clock::now();
  Clock::time_point phaseStart = start;
  std::string label, detail;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread timer;
  bool finished = false, visible = false;
  void draw();
  void finish();
  void recordTime() const;
};
} // namespace kalandra::cli
