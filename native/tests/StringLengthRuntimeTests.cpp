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
  StringDescriptor value;
  kalandra_string_literal_v1(&ctx, &value, text.data(), text.size());
  return value;
}
int main() {
  try {
    struct Case {
      std::string text;
      std::int64_t scalars;
    };
    for (const auto &test :
         {Case{"", 0}, Case{"hello", 5}, Case{"سلام", 4}, Case{"é", 1}, Case{"é", 2},
          Case{"👩‍💻", 3}, Case{std::string("a\0b", 3), 3}, Case{"\xef\xbb\xbf", 1},
          Case{"\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf",
               7}}) {
      Invocation ctx;
      auto value = make(ctx, test.text);
      auto owners = ctx.allocations();
      for (unsigned repetition = 0; repetition < 3; ++repetition) {
        require(kalandra_string_length_v1(&ctx, &value) == test.scalars, "scalar count mismatch");
        require(kalandra_string_byte_length_v1(&ctx, &value) ==
                    static_cast<std::int64_t>(test.text.size()),
                "byte count mismatch");
      }
      require(!ctx.status() && ctx.allocations() == owners, "queries allocated or faulted");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "String length leaked");
    }
    for (unsigned query = 0; query < 2; ++query)
      for (unsigned mode = 0; mode < 8; ++mode) {
        Invocation ctx;
        auto value = make(ctx, std::string(4097, 'a'));
        if (mode == 0)
          value.byteLength = -1;
        if (mode == 1)
          value.byteLength = 4098;
        if (mode == 2)
          value.byteLength = 0;
        if (mode == 3)
          value.data++;
        if (mode == 4)
          value.allocated = reinterpret_cast<void *>(0x1000);
        if (mode == 5)
          static_cast<char *>(value.allocated)[4096] = char(0x80);
        if (mode == 6)
          ctx.settle();
        if (mode == 7)
          ctx.fault(5);
        auto result = query ? kalandra_string_byte_length_v1(&ctx, &value)
                            : kalandra_string_length_v1(&ctx, &value);
        require(result == 0 && ctx.status() != 0, "invalid storage yielded a String count");
        ctx.settle();
        require(!ctx.live() && ctx.allocations() == ctx.releases(), "invalid query leaked");
      }
    // The shared decoder publishes counts only after a complete valid scan.
    for (const std::string &bytes :
         {std::string("\x80"), std::string("\xc0\x80"), std::string("\xe0\x80\x80"),
          std::string("\xf0\x80\x80\x80"), std::string("\xed\xa0\x80"), std::string("\xed\xbf\xbf"),
          std::string("\xf4\x90\x80\x80"), std::string("\xf5\x80\x80\x80"),
          std::string("\xf0\x9f\x92"), std::string("\xe2\x28\xa1")}) {
      std::uint64_t count = 999;
      require(validateUtf8(
                  bytes, [] { return true; }, &count) == Utf8Status::Invalid &&
                  count == 999,
              "invalid UTF-8 published partial count");
      for (unsigned query = 0; query < 2; ++query) {
        Invocation ctx;
        auto *data = static_cast<char *>(ctx.allocateRecord(bytes.size()));
        require(data, "setup allocation failed");
        std::memcpy(data, bytes.data(), bytes.size());
        StringDescriptor value{data, data, static_cast<std::int64_t>(bytes.size())};
        auto result = query ? kalandra_string_byte_length_v1(&ctx, &value)
                            : kalandra_string_length_v1(&ctx, &value);
        require(result == 0 && ctx.status() == 3, "malformed UTF-8 yielded String length");
        ctx.settle();
        require(!ctx.live(), "malformed query leaked");
      }
    }
    std::uint64_t count = 999;
    require(validateUtf8(
                "hello", [] { return false; }, &count) == Utf8Status::Interrupted &&
                count == 999,
            "cancelled decoder published count");
    for (unsigned query = 0; query < 2; ++query) {
      Invocation ctx;
      std::string text(16 * 1024 * 1024, 'a');
      auto value = make(ctx, text);
      require(validateString(ctx, value), "cancel setup invalid");
      std::atomic<bool> stop = false;
      std::atomic<std::uint64_t> polls = 0;
      ctx.bindCancellation(&stop);
      ctx.watchPolls(&polls);
      std::thread cancel([&] {
        while (polls.load(std::memory_order_acquire) < 3)
          std::this_thread::yield();
        stop = true;
      });
      auto result = query ? kalandra_string_byte_length_v1(&ctx, &value)
                          : kalandra_string_length_v1(&ctx, &value);
      cancel.join();
      require(result == 0 && ctx.poll() != 0, "String length ignored cancellation");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "cancelled length leaked");
    }
    std::cout << "String lengths: exact scalar/byte counts, no allocations, malformed "
                 "UTF-8/owners, count publication and in-scan cancellation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
