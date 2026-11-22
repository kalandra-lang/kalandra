#include "BuildProgress.h"
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace kalandra::cli {
namespace {
bool terminal([[maybe_unused]] std::optional<unsigned long> &originalMode) {
  const char *term = std::getenv("TERM");
  if (std::getenv("CI") || (term && std::string_view(term) == "dumb"))
    return false;
#ifdef _WIN32
  DWORD mode = 0;
  auto handle = GetStdHandle(STD_ERROR_HANDLE);
  if (!GetConsoleMode(handle, &mode))
    return false;
  if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    if (!SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
      return false;
    originalMode = mode;
  }
  return true;
#else
  return isatty(STDERR_FILENO);
#endif
}
unsigned columns() {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
    return info.srWindow.Right - info.srWindow.Left + 1;
#else
  winsize size{};
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
    return size.ws_col;
#endif
  return 80;
}
std::string seconds(std::chrono::steady_clock::duration duration) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << std::chrono::duration<double>(duration).count()
       << 's';
  return text.str();
}
} // namespace

BuildProgress::BuildProgress(Output mode)
    : options(mode), interactive(!mode.silent && !mode.debug && terminal(originalMode)),
      color(interactive && !std::getenv("NO_COLOR")) {}
BuildProgress::~BuildProgress() {
  finish();
#ifdef _WIN32
  if (originalMode)
    SetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), *originalMode);
#endif
}
void BuildProgress::recordTime() const {
  if (options.debug && !label.empty())
    std::cerr << "[debug] " << label << " took " << seconds(Clock::now() - phaseStart) << '\n';
}
void BuildProgress::draw() {
  // Leave a spare column so a transient line never wraps into the diagnostics.
  const auto width = columns();
  auto line = "  " + label + "  " + detail;
  if (line.size() >= width) {
    auto end = width - 1;
    while (end && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80)
      --end;
    line.resize(end);
  }
  std::cerr << "\r\033[2K";
  const auto prefix = std::min(line.size(), 2 + label.size());
  if (color)
    std::cerr << "\033[1;36m";
  std::cerr << line.substr(0, prefix);
  if (color)
    std::cerr << "\033[0m";
  std::cerr << line.substr(prefix) << std::flush;
  visible = true;
}
void BuildProgress::phase(std::string_view name, std::string_view subject) {
  if (!interactive && !options.debug)
    return;
  std::lock_guard lock(mutex);
  if (finished)
    return;
  const auto clean = printable(subject);
  if (label == name && detail == clean)
    return;
  recordTime();
  phaseStart = Clock::now();
  label = name;
  detail = clean;
  if (options.debug)
    options.trace(name, detail);
  else if (visible)
    draw();
  else if (!timer.joinable()) {
    timer = std::thread([this] {
      std::unique_lock lock(mutex);
      if (!wake.wait_until(lock, start + std::chrono::milliseconds(200),
                           [this] { return finished; }))
        draw();
    });
  }
}
void BuildProgress::replay(std::istream &messages) {
  std::lock_guard lock(mutex);
  if (visible)
    std::cerr << "\r\033[2K";
  char bytes[8192];
  char last = '\n';
  while (messages.read(bytes, sizeof(bytes)) || messages.gcount()) {
    std::cerr.write(bytes, messages.gcount());
    last = bytes[messages.gcount() - 1];
  }
  if (last != '\n')
    std::cerr << '\n';
  if (visible)
    draw();
}
void BuildProgress::finish() {
  {
    std::lock_guard lock(mutex);
    if (finished)
      return;
    finished = true;
    recordTime();
    if (visible)
      std::cerr << "\r\033[2K" << std::flush;
  }
  wake.notify_one();
  if (timer.joinable())
    timer.join();
}
std::ostream &BuildProgress::diagnostic() {
  finish();
  return std::cerr;
}
void BuildProgress::built(std::string_view output, std::string_view target) {
  finish();
  if (interactive || (options.debug && !options.silent))
    std::cerr << "  " << (color ? "\033[1;36m" : "") << "Built" << (color ? "\033[0m" : "") << "  "
              << printable(output) << "  ·  " << printable(target) << "  ·  "
              << seconds(Clock::now() - start) << '\n';
}
} // namespace kalandra::cli
