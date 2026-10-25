#include "../runtime/String.h"
#include "kalandra/Utf8.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static StringDescriptor make(Invocation &ctx, const std::string &text) {
  StringDescriptor out;
  kalandra_string_literal_v1(&ctx, &out, text.data(), text.size());
  return out;
}
static StringDescriptor slice(Invocation &ctx, const StringDescriptor &text, std::uint64_t start,
                              std::uint64_t end) {
  StringDescriptor out;
  kalandra_string_slice_v1(&ctx, &out, &text, start, end);
  return out;
}
int main() {
  try {
    const std::vector<std::string> pieces = {
        "a", "é", "س", "👩", "‍", "💻", std::string(1, '\0'), "\xef\xbb\xbf", "\xf4\x8f\xbf\xbf"};
    std::string text;
    std::vector<std::size_t> offsets = {0};
    for (const auto &p : pieces) {
      text += p;
      offsets.push_back(text.size());
    }
    Invocation ctx;
    auto input = make(ctx, text);
    for (unsigned start = 0; start <= pieces.size(); ++start)
      for (unsigned end = start; end <= pieces.size(); ++end) {
        const auto attempts = ctx.allocations();
        auto result = slice(ctx, input, start, end);
        require(validateString(ctx, result), "valid range rejected");
        const auto expected = text.substr(offsets[start], offsets[end] - offsets[start]);
        require(
            result.byteLength == static_cast<std::int64_t>(expected.size()) &&
                (!result.byteLength || !std::memcmp(result.data, expected.data(), expected.size())),
            "slice split or changed scalar sequence");
        require(kalandra_string_length_v1(&ctx, &result) == end - start,
                "slice scalar length mismatch");
        if (start == end)
          require(!result.allocated && !result.data && ctx.allocations() == attempts,
                  "empty slice allocated/noncanonical");
      }
    auto kept = slice(ctx, input, 1, 4);
    auto lease = ctx.lease(kept.allocated);
    ctx.release(input.allocated);
    require(validateString(ctx, kept), "slice depended on parent owner");
    ctx.settle();
    require(lease && !std::memcmp(lease->bytes().data(), text.data() + offsets[1],
                                  offsets[4] - offsets[1]),
            "retained slice lost after settlement");
    require(!ctx.live() && ctx.allocations() == ctx.releases(), "slice owners leaked");
    for (const auto &bounds : {std::pair<std::uint64_t, std::uint64_t>{2, 1},
                               {0, 4},
                               {4, 4},
                               {UINT64_MAX, 1},
                               {0, UINT64_MAX},
                               {0, std::uint64_t(INT64_MAX) + 1}}) {
      Invocation bad;
      auto source = make(bad, "aé👩");
      auto out = slice(bad, source, bounds.first, bounds.second);
      require(bad.status() == 5 && !out.data && !out.byteLength,
              "invalid range did not return Bounds/empty output");
      bad.settle();
      require(!bad.live(), "bad range leaked");
    }
    Invocation empty;
    auto zero = make(empty, "");
    auto result = slice(empty, zero, 0, 0);
    require(!empty.status() && !result.data && !empty.allocations(), "empty/empty slice failed");
    for (unsigned mode = 0; mode < 7; ++mode) {
      Invocation bad;
      auto source = make(bad, std::string(4097, 'a'));
      if (mode == 0)
        source.data++;
      if (mode == 1)
        source.byteLength = -1;
      if (mode == 2)
        source.byteLength = 4098;
      if (mode == 3)
        source.allocated = reinterpret_cast<void *>(0x1000);
      if (mode == 4)
        static_cast<char *>(source.allocated)[4096] = char(0x80);
      if (mode == 5)
        bad.settle();
      if (mode == 6)
        source.byteLength = 0;
      auto out = slice(bad, source, 0, 0);
      require(bad.status() == 3 && !out.data, "empty slice bypassed complete validation");
      bad.settle();
      require(!bad.live(), "bad descriptor leaked");
    }
    for (unsigned failure = 1; failure <= 4; ++failure) {
      Invocation failed;
      failed.failAllocationAt(failure);
      auto source = make(failed, "aé👩");
      auto out = slice(failed, source, 1, 3);
      require((failed.status() == 2) == (failure <= 2), "slice allocation sweep missed a failure");
      if (failed.status())
        require(!out.data && !out.byteLength, "failed allocation published slice");
      else
        require(validateString(failed, out) && out.byteLength == 6,
                "successful sweep changed slice");
      failed.settle();
      require(!failed.live() && failed.allocations() == failed.releases(), "failed slice leaked");
    }
    Invocation alias;
    auto source = make(alias, "aé👩");
    kalandra_string_slice_v1(&alias, &source, &source, 1, 3);
    require(validateString(alias, source) && source.byteLength == 6, "aliased output slice failed");
    alias.settle();
    Utf8Range range{0, 0, 999, 999, true};
    std::uint64_t count = 999;
    require(validateUtf8(
                "a\x80", [] { return true; }, &count, nullptr, &range) == Utf8Status::Invalid &&
                range.offset == 999 && range.bytes == 999 && count == 999,
            "bad tail published empty range");
    require(validateUtf8(
                "a", [] { return false; }, &count, nullptr, &range) == Utf8Status::Interrupted &&
                range.bytes == 999,
            "cancelled scan published range");
    for (unsigned kind = 0; kind < 2; ++kind) {
      Invocation work;
      auto input = make(work, std::string(16 * 1024 * 1024, 'a'));
      require(validateString(work, input), "cancellation setup failed");
      std::atomic<bool> stop = false;
      std::atomic<std::uint64_t> polls = 0;
      work.bindCancellation(&stop);
      work.watchPolls(&polls);
      const auto threshold = kind ? 65542u : 3u;
      std::thread cancel([&] {
        while (polls.load(std::memory_order_acquire) < threshold)
          std::this_thread::yield();
        stop = true;
      });
      auto out = slice(work, input, 0, kind ? 16 * 1024 * 1024 : 0);
      cancel.join();
      require(work.poll() != 0, "slice watcher did not cancel");
      // A watcher scheduled after completion may observe a complete result.
      // Partial publication is forbidden in either scheduling order.
      require((!out.data && !out.byteLength) ||
                  (kind && out.byteLength == input.byteLength &&
                   !std::memcmp(out.data, input.data, input.byteLength)),
              "concurrent cancellation published partial slice");
      auto stopped = slice(work, input, 0, kind ? input.byteLength : 0);
      require(!stopped.data && !stopped.byteLength,
              "visible cancellation ignored by slice");
      if (kind)
        require(work.allocations() == 2, "copy cancellation did not reach result allocation");
      work.settle();
      require(!work.live() && work.allocations() == work.releases(), "cancelled slice leaked");
    }
    std::cout
        << "String slices: 55 scalar intervals, canonical empties, independent retained results, "
           "bounds, full validation, allocation failure and scan/copy cancellation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
