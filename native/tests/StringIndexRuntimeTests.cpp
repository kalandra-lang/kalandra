#include "../runtime/String.h"
#include "kalandra/Utf8.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
static StringDescriptor index(Invocation &ctx, const StringDescriptor &value, std::uint64_t i) {
  StringDescriptor out;
  kalandra_string_index_v1(&ctx, &out, &value, i);
  return out;
}
int main() {
  try {
    const std::string pieces[] = {
        "a", "é", "س", "👩", "‍", "💻", std::string(1, '\0'), "\xef\xbb\xbf", "\xf4\x8f\xbf\xbf"};
    std::string joined;
    for (const auto &p : pieces)
      joined += p;
    Invocation ctx;
    auto input = make(ctx, joined);
    for (unsigned i = 0; i < std::size(pieces); ++i) {
      auto out = index(ctx, input, i);
      require(validateString(ctx, out), "selected scalar invalid");
      require(out.byteLength == static_cast<std::int64_t>(pieces[i].size()) &&
                  !std::memcmp(out.data, pieces[i].data(), pieces[i].size()),
              "scalar boundary mismatch");
      require(kalandra_string_length_v1(&ctx, &out) == 1,
              "String index did not produce one scalar");
    }
    auto kept = index(ctx, input, 3);
    auto lease = ctx.lease(kept.allocated);
    ctx.release(input.allocated);
    require(validateString(ctx, kept), "result depended on parent owner");
    ctx.settle();
    require(lease && !std::memcmp(lease->bytes().data(), pieces[3].data(), pieces[3].size()),
            "retained index result lost");
    require(!ctx.live() && ctx.allocations() == ctx.releases(), "index owners leaked");
    for (auto position :
         {std::uint64_t(1), std::uint64_t(INT64_MAX), std::uint64_t(INT64_MAX) + 1, UINT64_MAX}) {
      Invocation bad;
      auto source = make(bad, "é");
      auto out = index(bad, source, position);
      require(bad.status() == 5 && !out.data && !out.byteLength,
              "bad position did not fail with empty Bounds output");
      bad.settle();
      require(!bad.live(), "bad position leaked");
    }
    Invocation empty;
    auto zero = make(empty, "");
    auto missing = index(empty, zero, 0);
    require(empty.status() == 5 && !missing.data && !empty.allocations(),
            "empty index did not fail cleanly");
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
      auto out = index(bad, source, 0);
      require(bad.status() == 3 && !out.data, "index skipped complete descriptor/UTF-8 validation");
      bad.settle();
      require(!bad.live() && bad.allocations() == bad.releases(), "malformed indexing leaked");
    }
    for (unsigned failure = 1; failure <= 4; ++failure) {
      Invocation failed;
      failed.failAllocationAt(failure);
      auto source = make(failed, "é");
      auto out = index(failed, source, 0);
      require((failed.status() == 2) == (failure <= 2),
              "index allocation sweep missed failure position");
      if (failed.status())
        require(!out.data && !out.byteLength, "allocation failure published scalar");
      else
        require(out.byteLength == 2 && validateString(failed, out),
                "successful index sweep invalid");
      failed.settle();
      require(!failed.live() && failed.allocations() == failed.releases(), "failed index leaked");
    }
    Invocation alias;
    auto source = make(alias, "aé");
    kalandra_string_index_v1(&alias, &source, &source, 1);
    require(source.byteLength == 2 && validateString(alias, source),
            "aliased index descriptor failed");
    alias.settle();
    Utf8Scalar selected{0, 999, 999};
    std::uint64_t count = 999;
    require(validateUtf8(
                "a\x80", [] { return true; }, &count, &selected) == Utf8Status::Invalid &&
                selected.offset == 999 && selected.bytes == 999 && count == 999,
            "invalid tail published selected scalar");
    require(validateUtf8(
                "a", [] { return false; }, &count, &selected) == Utf8Status::Interrupted &&
                selected.bytes == 999,
            "cancelled scan published selection");
    Invocation work;
    auto large = make(work, std::string(16 * 1024 * 1024, 'a'));
    require(validateString(work, large), "cancel setup failed");
    std::atomic<bool> stop = false;
    std::atomic<std::uint64_t> polls = 0;
    work.bindCancellation(&stop);
    work.watchPolls(&polls);
    std::thread cancel([&] {
      while (polls.load(std::memory_order_acquire) < 3)
        std::this_thread::yield();
      stop = true;
    });
    auto aborted = index(work, large, 0);
    cancel.join();
    require(work.poll() != 0 && !aborted.data && !aborted.byteLength,
            "cancelled full scan published first scalar");
    work.settle();
    require(!work.live() && work.allocations() == work.releases(), "cancelled indexing leaked");
    std::cout << "String indexing: scalar boundaries, NUL/BOM, retained independent storage, "
                 "bounds, full validation, allocation failure and cancelled publication passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
