#include "kalandra/CudaArtifact.h"
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
void check(bool condition) {
  if (!condition)
    std::abort();
}
int main() {
  CudaArtifact value{"kernel",
                     120,
                     64,
                     0,
                     true,
                     {{64, -1}, {64, -1}, {32, 2}},
                     ".version 8.7\n.target sm_120\n.visible .entry kernel() { ret; }\n"};
  const auto bytes = encodeCudaArtifact(value);
  auto restored = decodeCudaArtifact(bytes);
  check(bool(restored) && restored->entry == value.entry && restored->nativeFloat64 &&
        restored->ptx == value.ptx && restored->buffers.size() == 3);
  // Every byte, including launch and buffer metadata, is integrity-covered.
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto corrupt = bytes;
    corrupt[i] ^= 1;
    check(!decodeCudaArtifact(corrupt));
  }
  for (std::size_t end = 0; end < bytes.size(); ++end)
    check(!decodeCudaArtifact(std::string_view(bytes).substr(0, end)));
  auto invalid = value;
  invalid.policy = 1;
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  invalid = value;
  invalid.threads = 0;
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  invalid = value;
  invalid.buffers[0].bits = 7;
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  invalid = value;
  invalid.buffers.back().extent = -1;
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  invalid = value;
  invalid.entry = "bad\nentry";
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  invalid = value;
  invalid.buffers.resize(65);
  check(!decodeCudaArtifact(encodeCudaArtifact(invalid)));
  CudaArtifact native{"kalandra_numeric_v1_matmul_float32_tile128_cuda_desc_v1",
                      80,
                      128,
                      0,
                      false,
                      {{32, -1}, {32, -1}, {32, -1}, {32, 4}},
                      ".version 8.7\n.target sm_80\n",
                      128};
  auto encoded = encodeCudaArtifact(native);
  auto parsed = decodeCudaArtifact(encoded);
  check(parsed && *parsed == native);
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    auto corrupt = encoded;
    corrupt[i] ^= 1;
    check(!decodeCudaArtifact(corrupt));
  }
  auto bad = native;
  bad.tile = 63;
  check(!decodeCudaArtifact(encodeCudaArtifact(bad)));
  bad = native;
  bad.threads = 256;
  check(!decodeCudaArtifact(encodeCudaArtifact(bad)));
  bad = native;
  bad.buffers[0].bits = 64;
  check(!decodeCudaArtifact(encodeCudaArtifact(bad)));
  bad = native;
  bad.policy = 1;
  check(!decodeCudaArtifact(encodeCudaArtifact(bad)));
  std::puts("PASS: private CUDA artifact integrity covers launch metadata, payload and truncation; "
            "invalid contracts rejected");
}
