// Reuse the unchanged tree algorithms in a separate translation unit.
#define source_kernel tree_kernel
#include "../../benchmarks/fair/Source.cpp"
#undef source_kernel
static double chain(std::int64_t x, std::int64_t depth) {
  if (!depth)
    return double(x);
  const auto next = (x * 13 + 7) % 1000003;
  const double child = chain(next, depth - 1);
  return child * 0.5 + double(x) * 0.25;
}
extern "C" double source_kernel(int kind, std::int64_t x, std::int64_t depth) {
  return kind == 3 ? chain(x, depth) : tree_kernel(kind, x, depth);
}
