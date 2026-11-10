#include "../runtime/Environment.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace kalandra::runtime;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "environment assertion at line %d: %s\n", __LINE__, #condition);        \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)

static StringDescriptor string(Invocation &context, std::string_view text) {
  StringDescriptor result;
  kalandra_string_literal_v1(&context, &result, text.data(), text.size());
  CHECK(context.usable());
  return result;
}

static EnvironmentResult lookup(Invocation &context, StringDescriptor &output,
                                std::string_view name) {
  auto key = string(context, name);
  return static_cast<EnvironmentResult>(kalandra_environment_get_v1(&context, &output, &key));
}

static void expect(std::string_view name, EnvironmentResult expected, std::string_view text = {}) {
  Invocation context;
  StringDescriptor result;
  CHECK(lookup(context, result, name) == expected);
  CHECK(validateString(context, result));
  CHECK(std::string_view(result.data ? result.data : "", result.byteLength) == text);
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}

int main() {
  {
    Invocation context;
    StringDescriptor result;
    CHECK(lookup(context, result, "KALANDRA_ENV_UNIT_TEXT") == EnvironmentResult::Missing);
    CHECK(context.status() == 3 && !result.allocated);
  }
#ifdef _WIN32
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_TEXT", L"Gr\u00fc\u00dfe=\u65e5\u672c"));
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_EMPTY", L""));
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_MISSING", nullptr) ||
        GetLastError() == ERROR_ENVVAR_NOT_FOUND);
  const wchar_t invalid[] = {wchar_t(0xd800), 0};
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_INVALID", invalid));
#else
  CHECK(setenv("KALANDRA_ENV_UNIT_TEXT", "Grüße=日本", 1) == 0);
  CHECK(setenv("KALANDRA_ENV_UNIT_EMPTY", "", 1) == 0);
  CHECK(unsetenv("KALANDRA_ENV_UNIT_MISSING") == 0);
  CHECK(setenv("KALANDRA_ENV_UNIT_INVALID", "\xff", 1) == 0);
#endif
  Invocation application;
  CHECK(kalandra_environment_initialize_v1(&application));
#ifdef _WIN32
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_TEXT", L"changed"));
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_EMPTY", nullptr));
  CHECK(SetEnvironmentVariableW(L"KALANDRA_ENV_UNIT_MISSING", L"added"));
#else
  CHECK(setenv("KALANDRA_ENV_UNIT_TEXT", "changed", 1) == 0);
  CHECK(unsetenv("KALANDRA_ENV_UNIT_EMPTY") == 0);
  CHECK(setenv("KALANDRA_ENV_UNIT_MISSING", "added", 1) == 0);
#endif
  CHECK(kalandra_environment_initialize_v1(&application));
  expect("KALANDRA_ENV_UNIT_TEXT", EnvironmentResult::Value, "Grüße=日本");
  expect("KALANDRA_ENV_UNIT_EMPTY", EnvironmentResult::Value);
  expect("KALANDRA_ENV_UNIT_MISSING", EnvironmentResult::Missing);
  expect("KALANDRA_ENV_UNIT_INVALID", EnvironmentResult::InvalidEncoding);
#ifdef _WIN32
  expect("kalandra_env_unit_text", EnvironmentResult::Value, "Grüße=日本");
#else
  expect("kalandra_env_unit_text", EnvironmentResult::Missing);
#endif
  expect("", EnvironmentResult::InvalidName);
  expect("a=b", EnvironmentResult::InvalidName);
  expect(std::string_view("a\0b", 3), EnvironmentResult::InvalidName);
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker)
    threads.emplace_back([] {
      for (int i = 0; i < 100; ++i)
        expect("KALANDRA_ENV_UNIT_TEXT", EnvironmentResult::Value, "Grüße=日本");
    });
  for (auto &thread : threads)
    thread.join();
  Invocation stopped;
  auto key = string(stopped, "KALANDRA_ENV_UNIT_TEXT");
  std::atomic<bool> cancelled{true};
  stopped.bindCancellation(&cancelled);
  StringDescriptor result;
  CHECK(kalandra_environment_get_v1(&stopped, &result, &key) ==
        unsigned(EnvironmentResult::Missing));
  CHECK(!result.allocated && stopped.poll() && !stopped.status());
  stopped.fault(1);
  CHECK(!kalandra_environment_initialize_v1(&stopped) && stopped.status() == 1);
  std::puts("PASS: immutable environment, encoding, ownership, concurrent lookup and cancellation");
}
