#include "kalandra/CudaArtifact.h"
#include "kalandra/CudaMatmul.h"
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
  auto catalog = native;
  catalog.tile = 0;
  catalog.catalog = 1;
  catalog.entry = cudaMatmulEntry;
  encoded = encodeCudaArtifact(catalog);
  parsed = decodeCudaArtifact(encoded);
  check(parsed && *parsed == catalog);
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    auto corrupt = encoded;
    corrupt[i] ^= 1;
    check(!decodeCudaArtifact(corrupt));
  }
  for (unsigned kind = 0; kind < 5; ++kind) {
    auto corrupt = catalog;
    if (kind == 0)
      corrupt.catalog = 2;
    if (kind == 1)
      corrupt.tile = 64;
    if (kind == 2)
      corrupt.entry += "_large";
    if (kind == 3)
      corrupt.threads = 256;
    if (kind == 4)
      corrupt.buffers.back().extent = 5;
    check(!decodeCudaArtifact(encodeCudaArtifact(corrupt)));
  }
  check(cudaMatmulCatalogShape(64, 128, 16));
  check(!cudaMatmulCatalogShape(0, 128, 16));
  check(!cudaMatmulCatalogShape(65, 128, 16));
  check(!cudaMatmulCatalogShape(64, 128, 17));
  check(!cudaMatmulCatalogShape(64, 128, UINT32_MAX - 15));
  check(!cudaMatmulCatalogShape(UINT32_MAX, 128, 16));
  check(!cudaMatmulCatalogShape(65536, 65536, 16));
  check(!cudaMatmulWide(512, 1024, 768));
  check(cudaMatmulWide(1024, 1024, 1024));
  check(!cudaMatmulWide(1536, 1536, 1536));
  check(cudaMatmulWide(2048, 2048, 2048));
  std::puts("PASS: private CUDA artifact integrity covers launch metadata, payload and truncation; "
            "invalid contracts rejected");
}
