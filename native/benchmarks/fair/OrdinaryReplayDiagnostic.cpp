#include "../../runtime/OrdinaryFault.h"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using I = std::int64_t;
static I wantedSeed, wantedRounds;
static unsigned replays;
extern "C" void ordinary_replay_observer(I seed, I rounds) {
  if (seed != wantedSeed || rounds != wantedRounds)
    std::abort();
  ++replays;
}
extern "C" double _mlir_ciface_kalandra_test_entry_ordinary_v1(I, I,
                                                               kalandra::runtime::OrdinaryFault *);

int main() {
  struct Case {
    I seed, rounds;
    unsigned expectedReplays;
  };
  const Case cases[] = {{17, 0, 0},
                        {17, 1, 0},
                        {17, 2, 0},
                        {17, 16, 0},
                        {65535, 0, 0},
                        {65536, 0, 0},
                        {65535, 1, 0},
                        {65536, 1, 0},
                        {4294967296LL - WIDTH, 0, 0},
                        {4294967297LL - WIDTH, 0, 1},
                        {4294967296LL, 0, 1},
                        {-31, 0, 1},
                        {-31, 1, 1},
                        {-31, 2, 1}};
  for (auto c : cases) {
    std::vector<I> values;
    for (I i = 0; i < WIDTH; ++i)
      values.push_back(c.seed + i);
    for (I r = 0; r < c.rounds; ++r)
      for (auto &value : values)
        value = (value * 13 + 7) % 1000003;
    I sum = 0;
    for (auto value : values)
      sum += value;
    wantedSeed = c.seed;
    wantedRounds = c.rounds;
    replays = 0;
    kalandra::runtime::OrdinaryFault fault;
    const double value = _mlir_ciface_kalandra_test_entry_ordinary_v1(c.seed, c.rounds, &fault);
    if (kalandra::runtime::ordinaryFaulted(value, fault) ||
        std::bit_cast<std::uint64_t>(value) != std::bit_cast<std::uint64_t>(double(sum)) ||
        replays != c.expectedReplays)
      std::abort();
    std::printf("{\"width\":%d,\"seed\":%lld,\"rounds\":%lld,\"replays\":%u,\"value\":%.17g}\n",
                WIDTH, (long long)c.seed, (long long)c.rounds, replays, value);
  }
}
