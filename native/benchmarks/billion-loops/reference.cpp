#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
int main(int argc, char **argv) {
  if (argc != 4 && argc != 5) return 2;
  const auto argument = [&](int index) {
    std::int32_t value;
    std::string_view text(argv[index]);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) std::exit(2);
    return value;
  };
  const auto u = argument(1), rows = argument(2), count = argument(3);
  const std::int32_t r = static_cast<std::int32_t>(arc4random_uniform(10000));
  std::vector<std::int32_t> a(rows, 0);
  for (std::int32_t i = 0; i < rows; ++i) {
    for (std::int32_t j = 0; j < count; ++j) {
#ifdef CHECKED
      if (__builtin_add_overflow(a[i], j % u, &a[i])) return 86;
#else
      a[i] = a[i] + j % u;
#endif
    }
#ifdef CHECKED
    if (__builtin_add_overflow(a[i], r, &a[i])) return 86;
#else
    a[i] += r;
#endif
  }
  if (argc == 5 && std::string_view(argv[4]) == "--verify") {
    std::printf("[%d", r);
    for (auto value : a) std::printf(", %d", value);
    std::puts("]");
  } else std::printf("%lld\n", static_cast<long long>(std::int64_t(r)*4294967296LL+a[r % rows]));
}
