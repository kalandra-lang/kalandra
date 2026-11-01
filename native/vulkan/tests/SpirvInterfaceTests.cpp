#include "SpirvInterface.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace kalandra::runtime::vulkan;
void check(bool value) {
  if (!value)
    std::abort();
}
int main() {
  std::vector<std::uint32_t> module{0x07230203,
                                    0x10000,
                                    0,
                                    100,
                                    0,
                                    (2u << 16) | 17,
                                    1, // Shader
                                    (5u << 16) | 15,
                                    5,
                                    1,
                                    0x6e69616d,
                                    0, // GLCompute %1 main
                                    (6u << 16) | 16,
                                    1,
                                    17,
                                    64,
                                    1,
                                    1,
                                    (4u << 16) | 71,
                                    2,
                                    33,
                                    0,
                                    (4u << 16) | 71,
                                    2,
                                    34,
                                    0,
                                    (4u << 16) | 59,
                                    3,
                                    2,
                                    12};
  const auto typesAt = module.size() - 4;
  const std::vector<std::uint32_t> parameterTypes{
      (4u << 16) | 21, 4, 32, 0, // uint32
      (4u << 16) | 43, 4, 5,  2, // constant length 2
      (4u << 16) | 28, 6, 4,  5, // uint32[2]
      (3u << 16) | 30, 7, 6,     // single-member block
      (4u << 16) | 32, 3, 12, 7  // StorageBuffer pointer
  };
  module.insert(module.begin() + typesAt, parameterTypes.begin(), parameterTypes.end());
  auto result = reflectSpirv(module);
  check(result && result->entry == "main" && result->localSize == 64 && result->buffers == 1 &&
        result->parameterWords == 2);
  for (std::size_t n = 0; n < module.size(); ++n)
    check(!reflectSpirv(std::span(module).first(n)));
  for (auto width : {32u, 64u}) {
    auto division = module;
    division.insert(division.end(), {(3u << 16) | 22, 80, width, (5u << 16) | 136, 80, 81, 82, 83});
    const auto reflected = reflectSpirv(division);
    check(reflected && reflected->float64Division == (width == 64));
    division.pop_back();
    check(!reflectSpirv(division));
  }
  auto changed = module;
  changed[5] = 0;
  check(!reflectSpirv(changed));
  changed = module;
  changed[6] = 9999;
  check(!reflectSpirv(changed));
  changed = module;
  changed[9] = 0;
  check(!reflectSpirv(changed)); // entry id
  changed = module;
  changed[16] = 2;
  check(!reflectSpirv(changed)); // local y
  changed = module;
  changed[21] = 3;
  check(!reflectSpirv(changed)); // binding gap
  changed = module;
  changed[25] = 1;
  check(!reflectSpirv(changed)); // foreign set
  changed = module;
  changed.back() = 9;
  check(!reflectSpirv(changed)); // push constant
  changed = module;
  changed[typesAt + 7] = 0;
  check(!reflectSpirv(changed)); // empty parameter block
  changed = module;
  changed[typesAt + 2] = 64;
  check(!reflectSpirv(changed)); // parameter words must be uint32
  for (auto cap : {10u, 11u, 22u, 39u, 4433u, 4448u}) {
    changed.resize(module.size() + 2);
    std::copy_n(module.begin(), 7, changed.begin());
    changed[7] = (2u << 16) | 17;
    changed[8] = cap;
    std::copy(module.begin() + 7, module.end(), changed.begin() + 9);
    check(bool(reflectSpirv(changed)));
  }
  changed = module;
  changed.insert(changed.begin() + 7, {(2u << 16) | 17, 4466});
  const auto modesAt = 20u; // After the existing fixed local-size mode.
  changed.insert(changed.begin() + modesAt, {(4u << 16) | 16, 1, 4461, 32});
  result = reflectSpirv(changed);
  check(result && result->preserveFloat32);
  auto invalidMode = changed;
  invalidMode[modesAt + 3] = 64;
  check(!reflectSpirv(invalidMode));
  invalidMode = changed;
  invalidMode[modesAt + 1] = 2;
  check(!reflectSpirv(invalidMode));
  invalidMode = changed;
  invalidMode.insert(invalidMode.begin() + modesAt, {(4u << 16) | 16, 1, 4461, 32});
  check(!reflectSpirv(invalidMode));
  invalidMode = changed;
  invalidMode[8] = 4467; // Explicit RTE is intentionally not admitted.
  check(!reflectSpirv(invalidMode));
  auto missingPreserve = changed;
  missingPreserve.erase(missingPreserve.begin() + 7, missingPreserve.begin() + 9);
  check(!reflectSpirv(missingPreserve));
  auto preserved64 = changed;
  preserved64[modesAt + 3] = 64;
  preserved64.insert(preserved64.begin() + 7, {(2u << 16) | 17, 10});
  auto reflected64 = reflectSpirv(preserved64);
  check(reflected64 && reflected64->preserveFloat64 && !reflected64->preserveFloat32);
  preserved64.insert(preserved64.begin() + 7, {(2u << 16) | 17, 4464});
  const auto denormAt = modesAt + 8;
  preserved64.insert(preserved64.begin() + denormAt, {(4u << 16) | 16, 1, 4459, 64});
  reflected64 = reflectSpirv(preserved64);
  check(reflected64 && reflected64->preserveFloat64 && reflected64->denormFloat64);
  auto badDenorm = preserved64;
  badDenorm[denormAt + 3] = 32;
  check(!reflectSpirv(badDenorm));
  badDenorm = preserved64;
  badDenorm.insert(badDenorm.begin() + denormAt, {(4u << 16) | 16, 1, 4459, 64});
  check(!reflectSpirv(badDenorm));
  auto native64 = module;
  native64.insert(native64.begin() + 7, {(2u << 16) | 17, 10});
  for (bool denormals : {false, true}) {
    auto normalized = preserveNativeFloat64(native64, denormals);
    check(bool(normalized));
    auto properties = reflectSpirv(*normalized);
    check(properties && properties->preserveFloat64 && properties->denormFloat64 == denormals);
    check(preserveNativeFloat64(*normalized, denormals) == normalized);
  }
  check(!preserveNativeFloat64(module, true));
  std::puts("PASS: compiler SPIR-V ABI reflection and malformed-interface rejection");
}
