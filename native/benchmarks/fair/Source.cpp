#include <cstdint>
static std::int64_t gcd(std::int64_t a, std::int64_t b) { return b == 0 ? a : gcd(b, a % b); }
static double polynomial(std::int64_t x, std::int64_t depth) {
  if (!depth) {
    const double value = double(x);
    return (value * 0.125 + 0.25) * value - 0.5;
  }
  return polynomial(x * 2, depth - 1) + polynomial(x * 2 + 1, depth - 1);
}
static double branches(std::int64_t x, std::int64_t depth) {
  if (!depth)
    return x % 3 == 0 ? double(x) * 0.5 : x % 3 == 1 ? double(x) * (-0.25) : double(x) + 0.125;
  return branches(x * 2, depth - 1) + branches(x * 2 + 1, depth - 1);
}
static double euclid(std::int64_t x, std::int64_t depth) {
  if (!depth)
    return double(gcd(x * 13 + 7, x * 7 + 11));
  return euclid(x * 2, depth - 1) + euclid(x * 2 + 1, depth - 1);
}
extern "C" double source_kernel(int kind, std::int64_t x, std::int64_t depth) {
  return kind == 0 ? polynomial(x, depth) : kind == 1 ? branches(x, depth) : euclid(x, depth);
}
