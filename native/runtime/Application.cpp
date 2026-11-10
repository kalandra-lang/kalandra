#include "Application.h"
#include "../include/kalandra/Utf8.h"
#include "OrdinaryFault.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {
std::atomic<unsigned> initialized{0};
int argumentCount = 0;
#ifdef _WIN32
wchar_t **argumentValues = nullptr;
#else
char **argumentValues = nullptr;
#endif
bool ready(kalandra::runtime::Invocation &ctx) noexcept { return ctx.poll() == 0 && ctx.usable(); }
} // namespace
namespace kalandra::runtime {
#ifdef _WIN32
bool initializeApplicationArguments(int argc, wchar_t **argv) noexcept {
#else
bool initializeApplicationArguments(int argc, char **argv) noexcept {
#endif
  unsigned expected = 0;
  if (!initialized.compare_exchange_strong(expected, 1))
    return false;
  bool valid = argc >= 0 && (!argc || argv);
  for (int i = 1; valid && i < argc; ++i) {
    if (!argv[i]) {
      valid = false;
      break;
    }
#ifdef _WIN32
    const auto length = std::wcslen(argv[i]);
    valid = length <= INT_MAX &&
            (!length || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], int(length),
                                            nullptr, 0, nullptr, nullptr) != 0);
#else
    valid = validUtf8(argv[i]);
#endif
  }
  if (valid) {
    argumentCount = argc;
    argumentValues = argv;
  }
  initialized.store(valid ? 2 : 3, std::memory_order_release);
  return valid;
}
} // namespace kalandra::runtime
extern "C" void
kalandra_application_arguments_v1(kalandra::runtime::Invocation *ctx,
                                  kalandra::runtime::StringArrayDescriptor *out) noexcept {
  using namespace kalandra::runtime;
  *out = {};
  if (!ready(*ctx))
    return;
  if (initialized.load(std::memory_order_acquire) != 2) {
    ctx->fault(3);
    return;
  }
  const auto count = argumentCount > 0 ? std::uint64_t(argumentCount - 1) : 0;
  if (!count)
    return;
  if (count > std::uint64_t(INT64_MAX) / sizeof(StringDescriptor)) {
    ctx->fault(2);
    return;
  }
  auto *parts =
      static_cast<StringDescriptor *>(ctx->allocateRecord(count * sizeof(StringDescriptor)));
  if (!parts)
    return;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!ready(*ctx))
      return;
    parts[i] = {};
#ifdef _WIN32
    const auto wide = std::wcslen(argumentValues[i + 1]);
    const auto bytes =
        wide ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argumentValues[i + 1], int(wide),
                                   nullptr, 0, nullptr, nullptr)
             : 0;
    if (wide && !bytes) {
      ctx->fault(3);
      return;
    }
    if (bytes) {
      auto *text = static_cast<char *>(ctx->allocateRecord(bytes));
      if (!text)
        return;
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argumentValues[i + 1], int(wide), text,
                              bytes, nullptr, nullptr) != bytes) {
        ctx->fault(3);
        return;
      }
      parts[i] = {text, text, bytes};
    }
#else
    const auto bytes = std::strlen(argumentValues[i + 1]);
    kalandra_string_literal_v1(ctx, &parts[i], argumentValues[i + 1], bytes);
#endif
    if (!ready(*ctx))
      return;
  }
  *out = {parts, parts, 0, static_cast<std::int64_t>(count), 1};
}

extern "C" bool kalandra_application_initialize_v1(std::int32_t argc, void *argv) noexcept {
#ifdef _WIN32
  const auto valid =
      kalandra::runtime::initializeApplicationArguments(argc, static_cast<wchar_t **>(argv));
#else
  const auto valid =
      kalandra::runtime::initializeApplicationArguments(argc, static_cast<char **>(argv));
#endif
  if (!valid)
    std::fputs("command-line arguments must contain valid Unicode text\n", stderr);
  return valid;
}
extern "C" std::int32_t
kalandra_application_finish_v1(std::uint64_t bits, std::int32_t kind,
                               const kalandra::runtime::OrdinaryFault *fault) noexcept {
  using namespace kalandra::runtime;
  const auto code = std::bit_cast<std::int32_t>(std::uint32_t(bits));
  if (kind != 3 || ordinaryFaulted(code, *fault) || code < 0 || code > 255) {
    Invocation ctx;
    if (kind == 3 && ordinaryFaulted(code, *fault))
      ctx.fault(fault->code, fault->line, fault->column);
    else
      ctx.fault(3);
    ctx.reportApplicationFailure();
  }
  return code;
}
extern "C" [[noreturn]] void kalandra_application_fail_v1(std::uint64_t code, std::uint64_t line,
                                                          std::uint64_t column) noexcept {
  kalandra::runtime::Invocation ctx;
  ctx.fault(code ? code : 3, line, column);
  ctx.reportApplicationFailure();
}
