#include "Environment.h"
#include "../include/kalandra/Utf8.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
extern char **environ;
#endif

namespace {
using namespace kalandra;
using namespace kalandra::runtime;
#ifdef _WIN32
using Character = wchar_t;
#else
using Character = char;
#endif
using Text = std::basic_string<Character>;
using View = std::basic_string_view<Character>;

struct Entry {
  Text name;
  Text value;
};
struct Environment {
  std::vector<Entry> entries;
  unsigned fault = 0;
#ifdef _WIN32
  std::size_t longestName = 0;
#endif

  void append(View entry) {
    const auto delimiter = entry.find(Character('='));
    // Windows also stores per-drive working directories under names beginning
    // with '='. These are outside the public name syntax.
    if (delimiter != View::npos && delimiter != 0) {
      entries.push_back({Text(entry.substr(0, delimiter)), Text(entry.substr(delimiter + 1))});
#ifdef _WIN32
      longestName = std::max(longestName, delimiter);
#endif
    }
  }

  Environment() noexcept {
    try {
#ifdef _WIN32
      const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> block(
          GetEnvironmentStringsW(), FreeEnvironmentStringsW);
      if (!block) {
        fault = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? 2 : 3;
        return;
      }
      for (auto *start = block.get(); *start;) {
        const View entry(start);
        append(entry);
        start += entry.size() + 1;
      }
#else
#ifdef __APPLE__
      auto *variables = *_NSGetEnviron();
#else
      auto *variables = environ;
#endif
      for (auto *entry = variables; entry && *entry; ++entry)
        append(*entry);
#endif
    } catch (const std::bad_alloc &) {
      fault = 2;
    } catch (const std::length_error &) {
      fault = 2;
    }
    if (fault)
      std::vector<Entry>().swap(entries);
  }
};

std::atomic<const Environment *> environmentSnapshot = nullptr;

bool ready(Invocation &context) noexcept { return !context.poll() && context.usable(); }

#ifdef _WIN32
EnvironmentResult readValue(Invocation &context, StringDescriptor &output, View value) noexcept {
  if (value.empty())
    return EnvironmentResult::Value;
  if (value.size() > INT_MAX) {
    context.fault(2);
    return EnvironmentResult::Missing;
  }
  const auto size = static_cast<int>(value.size());
  const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, nullptr,
                                         0, nullptr, nullptr);
  if (!bytes) {
    if (GetLastError() == ERROR_NO_UNICODE_TRANSLATION)
      return EnvironmentResult::InvalidEncoding;
    context.fault(3);
    return EnvironmentResult::Missing;
  }
  if (!ready(context))
    return EnvironmentResult::Missing;
  auto *data = static_cast<char *>(context.allocateRecord(bytes));
  if (!data)
    return EnvironmentResult::Missing;
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), size, data, bytes, nullptr,
                          nullptr) != bytes) {
    context.fault(3);
    return EnvironmentResult::Missing;
  }
  if (ready(context))
    output = {data, data, bytes};
  return EnvironmentResult::Value;
}
#else
EnvironmentResult readValue(Invocation &context, StringDescriptor &output, View value) noexcept {
  const auto status = validateUtf8(value, [&] { return ready(context); });
  if (status == Utf8Status::Invalid)
    return EnvironmentResult::InvalidEncoding;
  if (status == Utf8Status::Valid)
    kalandra_string_literal_v1(&context, &output, value.data(), value.size());
  return EnvironmentResult::Value;
}

bool equalName(Invocation &context, View left, View right) noexcept {
  if (left.size() != right.size())
    return false;
  for (std::size_t offset = 0; offset < left.size();) {
    if (!ready(context))
      return false;
    const auto size = std::min<std::size_t>(1024, left.size() - offset);
    if (std::memcmp(left.data() + offset, right.data() + offset, size) != 0)
      return false;
    offset += size;
  }
  return true;
}
#endif

EnvironmentResult lookup(Invocation &context, StringDescriptor &output,
                         const StringDescriptor &name) noexcept {
  if (!validateString(context, name))
    return EnvironmentResult::Missing;
  if (!name.byteLength)
    return EnvironmentResult::InvalidName;
  const std::string_view bytes(name.data, name.byteLength);
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    if ((offset % 1024 == 0) && !ready(context))
      return EnvironmentResult::Missing;
    if (bytes[offset] == '=' || bytes[offset] == '\0')
      return EnvironmentResult::InvalidName;
  }
  const auto *environment = environmentSnapshot.load(std::memory_order_acquire);
  if (!environment) {
    context.fault(3);
    return EnvironmentResult::Missing;
  }
#ifdef _WIN32
  // A longer name cannot match a Windows environment variable.
  if (bytes.size() > INT_MAX)
    return EnvironmentResult::Missing;
  const auto size = static_cast<int>(bytes.size());
  const auto units =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
  if (!units) {
    context.fault(3);
    return EnvironmentResult::Missing;
  }
  if (static_cast<std::size_t>(units) > environment->longestName || !ready(context))
    return EnvironmentResult::Missing;
  Text key;
  try {
    key.resize(units);
  } catch (const std::bad_alloc &) {
    context.fault(2);
    return EnvironmentResult::Missing;
  } catch (const std::length_error &) {
    context.fault(2);
    return EnvironmentResult::Missing;
  }
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, key.data(), units) !=
      units) {
    context.fault(3);
    return EnvironmentResult::Missing;
  }
#endif
  for (const auto &entry : environment->entries) {
    if (!ready(context))
      return EnvironmentResult::Missing;
#ifdef _WIN32
    if (entry.name.size() > INT_MAX)
      continue;
    const auto order = CompareStringOrdinal(key.data(), units, entry.name.data(),
                                            static_cast<int>(entry.name.size()), TRUE);
    if (!order) {
      context.fault(3);
      return EnvironmentResult::Missing;
    }
    const bool matches = order == CSTR_EQUAL;
#else
    const bool matches = equalName(context, bytes, entry.name);
#endif
    if (matches)
      return readValue(context, output, entry.value);
  }
  return EnvironmentResult::Missing;
}
} // namespace

extern "C" bool kalandra_environment_initialize_v1(Invocation *context) noexcept {
  if (!ready(*context))
    return false;
  static const Environment inherited;
  if (inherited.fault)
    context->fault(inherited.fault);
  else
    environmentSnapshot.store(&inherited, std::memory_order_release);
  return ready(*context);
}

extern "C" std::uint32_t kalandra_environment_get_v1(Invocation *context, StringDescriptor *output,
                                                     const StringDescriptor *name) noexcept {
  const auto input = *name;
  *output = {};
  const auto result = lookup(*context, *output, input);
  if (!ready(*context)) {
    *output = {};
    return unsigned(EnvironmentResult::Missing);
  }
  return unsigned(result);
}
