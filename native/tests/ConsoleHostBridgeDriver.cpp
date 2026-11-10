#include "../runtime/Console.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
using namespace kalandra::runtime;
struct Line {
  std::int64_t tag;
  StringDescriptor text;
};
struct IoError {
  std::int64_t tag;
  std::int32_t code;
  StringDescriptor message;
  std::uint64_t limit;
};
static_assert(sizeof(Line) == 32 && sizeof(IoError) == 48 && offsetof(IoError, message) == 16);
extern "C" bool test_read(Invocation *, void **, const void *, void *, Failure *, void *);
extern "C" bool test_write(Invocation *, void **, const void *, void *, Failure *, void *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected mode");
    const std::string mode = argv[1];
    Invocation ctx;
    void *state = nullptr;
    Failure failure{};
    IoError error{};
    if (mode == "write" || mode == "broken") {
      StringDescriptor text;
      kalandra_string_literal_v1(&ctx, &text, "α\0é", 5);
      bool unit = true;
      require(test_write(&ctx, &state, &text, &unit, &failure, &error), "unbound write suspended");
      require(!unit && !state && !ctx.status() && failure.kind == TaskFailure::None,
              "console host unit packet layout mismatch");
      if (mode == "broken") {
        require(error.tag == 1 && error.code && validateString(ctx, error.message),
                "console host system code/String payload mismatch");
        std::cerr << "PASS\n";
      } else
        require(!error.tag, "console host write returned an unexpected error");
    } else {
      const std::uint64_t limit = mode == "limit" ? 2 : 100;
      Line line{773, {}};
      if (mode == "cancel") {
        std::atomic<bool> stop{false};
        ctx.bindCancellation(&stop);
        require(!test_read(&ctx, &state, &limit, &line, &failure, &error) && state &&
                    line.tag == 773,
                "pending console bridge published a result");
        stop.store(true);
        require(test_read(&ctx, &state, &limit, &line, &failure, &error) && !state &&
                    line.tag == 773 && !ctx.status(),
                "cancelled console bridge published a result or retained state");
      } else {
        require(test_read(&ctx, &state, &limit, &line, &failure, &error) && !state &&
                    !ctx.status() && failure.kind == TaskFailure::None,
                "console host read transport failed");
        if (mode == "text") {
          require(line.tag == 2 && !error.tag && validateString(ctx, line.text) &&
                      line.text.byteLength == 5 && !std::memcmp(line.text.data, "α\0é", 5),
                  "console host String/Line layout mismatch");
        } else if (mode == "end")
          require(line.tag == 1 && !error.tag, "console host EOF layout mismatch");
        else if (mode == "limit")
          require(!line.tag && error.tag == 3 && error.limit == 2,
                  "console host line-limit payload mismatch");
        else if (mode == "encoding")
          require(!line.tag && error.tag == 2, "console host encoding error tag mismatch");
        else
          throw std::runtime_error("unknown mode");
      }
      std::cout << "PASS\n";
    }
    ctx.settle();
    require(!ctx.live() && ctx.allocations() == ctx.releases(),
            "console host packet backing leaked");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
