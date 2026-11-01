// Reference-only invocation benchmark: resident inputs, real output/parameter/
// workspace allocation, launch, completion, status readback and cleanup.
#include "Cuda.h"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace gpu = kalandra::runtime::cuda;
template <class T> T take(std::expected<T, gpu::Error> value) {
  if (!value)
    throw std::runtime_error("CUDA runtime error " + std::to_string(int(value.error())));
  return std::move(*value);
}
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      throw std::runtime_error("usage: runtime-matmul catalog.ptx legacy.ptx output.jsonl");
    std::ofstream out(argv[3]);
    out << "{\"scope\":\"runtime invocation, resident inputs; includes "
           "allocation/status/fence/cleanup; excludes input transfer, module/JIT, task scheduling "
           "and process startup\"}\n";
    for (unsigned budgetMiB : {64u, 256u}) {
      auto runtime =
          take(gpu::Runtime::create({.maxBytes = std::uint64_t(budgetMiB) * 1024 * 1024}));
      const std::array kernels{take(runtime->loadKernel(argv[2])),
                               take(runtime->loadKernel(argv[1]))};
      for (auto shape : {std::array{512u, 512u, 512u}, std::array{1024u, 1024u, 1024u},
                         std::array{1536u, 1536u, 1536u}, std::array{2048u, 2048u, 2048u},
                         std::array{512u, 1024u, 768u}}) {
        const auto [m, n, k] = shape;
        std::vector<float> a(std::size_t(m) * k, 1), b(std::size_t(k) * n, 1);
        const std::array inputs{take(runtime->upload(a)), take(runtime->upload(b))};
        const std::array words{m, k, n, UINT32_MAX};
        auto invoke = [&](unsigned i) {
          return take(runtime->execute(kernels[i], inputs, words, std::uint64_t(m) * n * 4, 1));
        };
        for (unsigned i = 0; i < 2; ++i) {
          auto result = invoke(i);
          auto values = take(runtime->download(result.output));
          if (values.size() != std::size_t(m) * n)
            throw std::runtime_error("output extent");
          for (float value : values)
            if (value != float(k))
              throw std::runtime_error("complete output correctness");
          if (result.parameters != std::vector<unsigned>(words.begin(), words.end()))
            throw std::runtime_error("status metadata");
        }
        // Warm both implementations equally without changing device clocks.
        const auto warmUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < warmUntil) {
          for (unsigned i = 0; i < 2; ++i) {
            auto result = invoke(i);
          }
        }
        auto retained = runtime->retainedBytes();
        constexpr unsigned repeats = 4;
        for (auto pair : {std::array{0u, 0u}, std::array{1u, 1u}, std::array{0u, 1u}})
          for (unsigned block = 0; block < 4; ++block)
            for (unsigned position = 0; position < 4; ++position) {
              const unsigned side = (position == 0 || position == 3) ? 0 : 1;
              const auto begin = std::chrono::steady_clock::now();
              for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                auto result = invoke(pair[side]);
              }
              const auto us = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count() /
                              repeats;
              if (runtime->retainedBytes() != retained)
                throw std::runtime_error("invocation quota leak");
              out << "{\"budgetMiB\":" << budgetMiB << ",\"m\":" << m << ",\"n\":" << n
                  << ",\"k\":" << k << ",\"a\":" << pair[0] << ",\"b\":" << pair[1]
                  << ",\"block\":" << block << ",\"position\":" << position << ",\"side\":" << side
                  << ",\"wallUs\":" << us << "}\n";
              out.flush();
            }
        auto stats = runtime->dispatchStats();
        std::cout << "PASS " << budgetMiB << "MiB " << m << "x" << n << "x" << k
                  << " banded=" << stats.bandedMatmuls
                  << " scratchFree=" << stats.scratchFreeMatmuls << '\n'
                  << std::flush;
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
