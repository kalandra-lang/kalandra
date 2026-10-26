#include <cstdint>
#include <vector>
using I = std::int64_t;
static double arrayRounds(std::vector<I> values, I rounds) {
  if (!rounds) {
    I sum = 0;
    for (auto value : values) sum += value;
    return double(sum);
  }
  std::vector<I> next(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) next[i] = (values[i] * 13 + 7) % 1000003;
  return arrayRounds(std::move(next), rounds - 1);
}
template<class F> static double applyRounds(F transform, I value, I rounds) {
  if (!rounds) return double(value);
  return applyRounds(transform, transform(value), rounds - 1);
}
extern "C" double runtime_workload(I kind, I seed, I rounds) {
  if (!kind) return arrayRounds({seed,seed+1,seed+2,seed+3,seed+4,seed+5,seed+6,seed+7}, rounds);
  const auto offset = seed + 7;
  return applyRounds([offset](I value) { return (value * 13 + offset) % 1000003; }, seed, rounds);
}
