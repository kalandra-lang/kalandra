#include "../runtime/String.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
int main() {
  try {
    Invocation source;
    const std::string text("سلام\0👩‍💻", 20);
    auto value = make(source, text);
    require(validateString(source, value), "valid UTF-8 rejected");
    require(value.byteLength == 20 && !std::memcmp(value.data, text.data(), 20),
            "literal changed bytes");
    auto kept = source.lease(value.allocated);
    require(bool(kept), "cannot retain String");
    source.settle();
    require(!source.live() && source.allocations() == source.releases(),
            "source cleanup unbalanced");
    require(!std::memcmp(kept->bytes().data(), text.data(), 20),
            "retained String invalidated by settle");
    require(!validateString(source, value), "settled owner accepted");
    Invocation empty;
    auto zero = make(empty, "");
    require(validateString(empty, zero) && !empty.allocations(),
            "empty String allocated or rejected");
    for (unsigned mode = 0; mode < 9; ++mode) {
      Invocation ctx;
      auto forged = make(ctx, "hello");
      switch (mode) {
      case 0:
        forged.byteLength = -1;
        break;
      case 1:
        forged.byteLength = 6;
        break;
      case 2:
        forged.data++;
        break;
      case 3:
        forged.allocated = nullptr;
        break;
      case 4:
        forged.byteLength = 0;
        break;
      case 5:
        forged.allocated = reinterpret_cast<void *>(0x1000);
        forged.data = static_cast<char *>(forged.allocated);
        break;
      case 6:
        static_cast<char *>(forged.allocated)[4] = char(0x80);
        break;
      case 7:
        ctx.fault(5);
        break;
      case 8:
        forged.allocated = const_cast<char *>(forged.data + 1);
        break;
      }
      require(!validateString(ctx, forged), "forged String admitted");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "rejected String leaked");
    }
    for (unsigned failure = 1; failure <= 3; ++failure) {
      Invocation ctx;
      ctx.failAllocationAt(failure);
      auto out = make(ctx, "hello");
      if (ctx.status())
        require(!out.allocated && !out.data && !out.byteLength, "failed literal published storage");
      else
        require(validateString(ctx, out), "allocation sweep lost successful value");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "allocation failure leaked");
    }
    for (const std::string &bytes :
         {std::string("\x80"), std::string("\xc0\x80"), std::string("\xe0\x80\x80"),
          std::string("\xf0\x80\x80\x80"), std::string("\xed\xa0\x80"), std::string("\xed\xbf\xbf"),
          std::string("\xf4\x90\x80\x80"), std::string("\xf5\x80\x80\x80"),
          std::string("\xf0\x9f\x92"), std::string("\xe2\x28\xa1")}) {
      Invocation ctx;
      auto *data = static_cast<char *>(ctx.allocateRecord(bytes.size()));
      require(data, "invalid UTF-8 setup allocation failed");
      std::memcpy(data, bytes.data(), bytes.size());
      StringDescriptor invalid{data, data, static_cast<std::int64_t>(bytes.size())};
      require(!validateString(ctx, invalid) && ctx.status() == 3,
              "malformed UTF-8 storage admitted");
      ctx.settle();
      require(!ctx.live(), "invalid UTF-8 leaked");
      Invocation copy;
      auto rejected = make(copy, bytes);
      require(copy.status() == 3 && !rejected.data && !copy.allocations(),
              "malformed literal copied/published");
    }
    Invocation cancelled;
    std::atomic<bool> token = true;
    cancelled.bindCancellation(&token);
    auto aborted = make(cancelled, "hello");
    require(!aborted.data && !cancelled.allocations(), "cancelled literal allocated/published");
    Invocation large;
    std::atomic<std::uint64_t> polls = 0;
    large.watchPolls(&polls);
    auto big = make(large, std::string(65536, 'a'));
    polls = 0;
    require(validateString(large, big) && polls >= 257, "String scan is not cooperatively polled");
    std::atomic<bool> stop = false;
    large.bindCancellation(&stop);
    polls = 0;
    // Large validation must observe asynchronous cancellation before publishing success.
    auto huge = make(large, std::string(16 * 1024 * 1024, 'a'));
    require(huge.byteLength == 16 * 1024 * 1024, "large cancellation setup failed");
    polls = 0;
    std::thread cancel([&] {
      while (polls.load(std::memory_order_acquire) < 3)
        std::this_thread::yield();
      stop = true;
    });
    const bool valid = validateString(large, huge);
    cancel.join();
    require(!valid, "String scan ignored cancellation");
    large.settle();
    require(!large.live() && large.allocations() == large.releases(), "cancelled scan leaked");
    Invocation copying;
    std::atomic<std::uint64_t> copyPolls = 0;
    copying.watchPolls(&copyPolls);
    std::atomic<bool> cancelCopy = false;
    copying.bindCancellation(&cancelCopy);
    const std::string longText(16 * 1024 * 1024, 'a');
    std::thread interrupt([&] {
      while (copyPolls.load(std::memory_order_acquire) < 65542)
        std::this_thread::yield();
      cancelCopy = true;
    });
    auto unpublished = make(copying, longText);
    interrupt.join();
    require(!unpublished.data && !unpublished.byteLength && copying.allocations() > 0,
            "interrupted copy published storage");
    copying.settle();
    require(!copying.live() && copying.allocations() == copying.releases(),
            "interrupted copy leaked");
    std::cout
        << "UTF-8 ownership, descriptor rejection, retention, failure and cancellation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
