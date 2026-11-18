#include "../runtime/GpuText.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace kalandra::runtime;

static void require(bool success, int line) {
  if (!success) {
    std::fprintf(stderr, "GPU text wire assertion failed at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(value) require(bool(value), __LINE__)

static StringDescriptor string(Invocation &context, std::string_view bytes) {
  StringDescriptor result;
  kalandra_string_literal_v1(&context, &result, bytes.data(), bytes.size());
  CHECK(context.status() == 0);
  return result;
}

static void roundTrip(std::string_view text) {
  Invocation source, destination;
  const auto input = string(source, text);
  const auto packet = packGpuText(source, input);
  CHECK(packet.allocated);
  const auto words = std::span{packet.aligned, static_cast<std::size_t>(packet.sizes[0])};
  CHECK(words[0] == 1 && words[1] == text.size() && words[2] == 0 && words[3] == text.size());
  StringDescriptor result;
  CHECK(unpackGpuText(destination, words, result));
  CHECK(result.byteLength == static_cast<std::int64_t>(text.size()));
  source.settle();
  if (text.empty()) {
    CHECK(!result.allocated && !result.data);
  } else {
    CHECK(std::string_view(result.data, result.byteLength) == text);
    CHECK(validateString(destination, result));
  }
}

static void batch() {
  Invocation context;
  const std::array values{string(context, "é"), StringDescriptor{}, string(context, "猫"),
                          string(context, std::string_view{"a\0", 2})};
  auto *backing = context.allocateRecord(sizeof(values));
  CHECK(backing);
  std::memcpy(backing, values.data(), sizeof(values));
  const auto packet = packGpuText(context, StringArrayDescriptor{backing, backing, 0, 4, 1});
  constexpr std::array<std::uint32_t, 9> expected{4, 7, 0, 2, 2, 5, 7, 0x8ce7a9c3, 0x61ab};
  CHECK(packet.allocated && packet.sizes[0] == expected.size());
  CHECK(std::equal(expected.begin(), expected.end(), packet.aligned));
  const auto empty = packGpuText(context, StringArrayDescriptor{});
  CHECK(empty.allocated && empty.sizes[0] == 3);
  CHECK(empty.aligned[0] == 0 && empty.aligned[1] == 0 && empty.aligned[2] == 0);
}

static void failedImports() {
  // Wrong header, missing payload, nonzero padding, and invalid UTF-8.
  constexpr std::array<std::array<std::uint32_t, 5>, 6> invalid{{{0, 1, 0, 1, 97},
                                                                 {1, 1, 1, 1, 97},
                                                                 {1, 1, 0, 0, 97},
                                                                 {1, 5, 0, 5, 97},
                                                                 {1, 1, 0, 1, 0x161},
                                                                 {1, 1, 0, 1, 0x80}}};
  for (const auto &words : invalid) {
    Invocation context;
    auto result = string(context, "prior");
    const auto baseline = context.live();
    CHECK(!unpackGpuText(context, words, result));
    CHECK(context.status() == 3 && context.live() == baseline);
    CHECK(!result.allocated && !result.data && result.byteLength == 0);
  }
}

static void failuresAndCancellation() {
  Invocation source;
  const auto input = string(source, "Kalandra");
  Invocation foreign;
  CHECK(!packGpuText(foreign, input).allocated && foreign.status() == 3);
  Invocation exhausted;
  exhausted.failAllocationAt(1);
  CHECK(!packGpuText(exhausted, StringDescriptor{}).allocated && exhausted.status() == 2);
  Invocation cancelled;
  std::atomic<bool> stop{true};
  cancelled.bindCancellation(&stop);
  CHECK(!packGpuText(cancelled, StringDescriptor{}).allocated && cancelled.status() == 0);
  constexpr std::array<std::uint32_t, 5> words{1, 1, 0, 1, 97};
  StringDescriptor result;
  CHECK(!unpackGpuText(cancelled, words, result) && !result.allocated && cancelled.status() == 0);
  CHECK(cancelled.live() == 0);
}

int main() {
  roundTrip("");
  roundTrip(std::string_view{"\0é猫🧪e\u0301", 13});
  roundTrip("abcd");
  batch();
  failedImports();
  failuresAndCancellation();
}
