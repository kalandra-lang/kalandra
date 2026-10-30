#include "StringArray.h"
#include "kalandra/CollectionABI.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
extern "C" {
void _mlir_ciface_kalandra_collection_entry(kalandra::cli::CollectionDescriptor *,
                                            kalandra::runtime::Invocation *);
extern const std::uint64_t kalandra_collection_rank_v1, kalandra_collection_element_v1,
    kalandra_collection_array_v1;
}
static bool quoted(const kalandra::runtime::StringDescriptor &value) noexcept {
  if (std::fputc('"', stdout) == EOF)
    return false;
  for (std::int64_t i = 0; i < value.byteLength; ++i) {
    const auto c = static_cast<unsigned char>(value.data[i]);
    const char *escape = c == '"'    ? "\\\""
                         : c == '\\' ? "\\\\"
                         : c == '\n' ? "\\n"
                         : c == '\r' ? "\\r"
                         : c == '\t' ? "\\t"
                                     : nullptr;
    if (escape) {
      if (std::fputs(escape, stdout) < 0)
        return false;
    } else if (c < 0x20 || c == 0x7f) {
      if (std::fprintf(stdout, "\\u%04x", c) < 0)
        return false;
    } else if (std::fputc(c, stdout) == EOF)
      return false;
  }
  return std::fputc('"', stdout) != EOF;
}
int main(int argc, char **argv) {
  using namespace kalandra;
  using namespace kalandra::runtime;
  std::uint64_t limit = 100;
  bool valid = argc == 1;
  if (argc == 2 && std::string_view(argv[1]) == "--all") {
    limit = UINT64_MAX;
    valid = true;
  } else if (argc == 3 && std::string_view(argv[1]) == "--limit") {
    const std::string_view text(argv[2]);
    auto result = std::from_chars(text.data(), text.data() + text.size(), limit);
    valid = !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
  }
  if (!valid) {
    std::fprintf(stderr, "usage: %s [--all | --limit <nonnegative element count>]\n", argv[0]);
    return 2;
  }
  Invocation context, retained;
  if (kalandra_collection_rank_v1 != 1 ||
      kalandra_collection_element_v1 != cli::stringArrayElement ||
      kalandra_collection_array_v1 != 1) {
    context.fault(3);
    context.reportFailure();
  }
  cli::CollectionDescriptor output;
  _mlir_ciface_kalandra_collection_entry(&output, &context);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  StringArrayDescriptor array{output.allocated, output.aligned, output.offset, output.dimensions[0],
                              output.dimensions[1]};
  if (!kalandra_string_array_retain_v1(&retained, &context, &array)) {
    retained.settle();
    context.fault(retained.status() ? retained.status() : 3);
    context.reportFailure();
  }
  context.settle();
  if (context.live() || context.allocations() != context.releases()) {
    context.fault(3);
    context.reportFailure();
  }
  if (std::getenv("KALANDRA_TEST_REPORT_CLEANUP"))
    std::fprintf(stderr, "cleanup: live=0 allocations=%llu releases=%llu\n",
                 static_cast<unsigned long long>(context.allocations()),
                 static_cast<unsigned long long>(context.releases()));
  bool ok = std::fputc('[', stdout) != EOF;
  const auto shown = std::min(std::uint64_t(array.size), limit);
  const auto *data = static_cast<const std::byte *>(array.data);
  for (std::uint64_t i = 0; ok && i < shown; ++i) {
    StringDescriptor cell;
    std::memcpy(&cell, data + i * sizeof(cell), sizeof(cell));
    ok = (!i || std::fputs(", ", stdout) >= 0) && quoted(cell);
  }
  if (ok && shown < std::uint64_t(array.size))
    ok = std::fputs(shown ? ", ..." : "...", stdout) >= 0;
  if (ok)
    ok = std::fputs("]\n", stdout) >= 0;
  retained.settle();
  if (!ok) {
    std::fputs("cannot write String array result\n", stderr);
    return 1;
  }
}
