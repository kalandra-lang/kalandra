#include "SpirvInterface.h"
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
  auto result = reflectSpirv(module);
  check(result && result->entry == "main" && result->localSize == 64 && result->buffers == 1);
  for (std::size_t n = 0; n < module.size(); ++n)
    check(!reflectSpirv(std::span(module).first(n)));
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
  for (auto cap : {10u, 11u, 22u, 39u, 4433u, 4448u}) {
    changed = module;
    changed.insert(changed.begin() + 7, {(2u << 16) | 17, cap});
    check(bool(reflectSpirv(changed)));
  }
  std::puts("PASS: compiler SPIR-V ABI reflection and malformed-interface rejection");
}
