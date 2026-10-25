#include "../metal/Metal.h"
#include "ReluCases.h"
#include <array>
#include <cstdio>
#include <new>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
static void retire(TaskCore &core, GroupHandle group) {
  relu_tests::check(core.close(group) && core.drain() && core.release(group));
  const auto s = core.stats();
  relu_tests::check(!s.records && !s.groups && !s.externalPending && !s.retainedBytes &&
                    !s.retainedOwners);
}
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  relu_tests::check(core.observe(task, m::Runtime::bufferLayout(), ctx, slot));
  auto value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto out = *value;
  value->~Buffer();
  return out;
}
int main(int argc, char **argv) {
  relu_tests::check(argc == 2);
  auto owner = m::Runtime::create();
  relu_tests::check(bool(owner));
  auto &rt = **owner;
  auto device = rt.device();
  relu_tests::check(bool(device));
  auto core = TaskCore::create();
  relu_tests::check(bool(core));
  std::size_t cells = 0, cases = 0;
  for (auto description : numericTypes) {
    auto type = description.type;
    const auto file =
        std::string(argv[1]) + "/" + std::string(description.name) + "/kernel0.metallib";
    auto kernel =
        type == NumericType::Float32
            ? rt.loadElementwiseKernel(file, *device, {1, 0}, std::array<std::uint32_t, 1>{0})
            : rt.loadNumericKernel(file, *device, type, {1, 0}, std::array{type},
                                   std::array<NumericType, 0>{}, std::array<std::uint32_t, 1>{0});
    relu_tests::check(bool(kernel));
    for (auto shape : std::vector<std::vector<std::int64_t>>{{0},
                                                             {1},
                                                             {3},
                                                             {31},
                                                             {32},
                                                             {33},
                                                             {127},
                                                             {128},
                                                             {129},
                                                             {255},
                                                             {256},
                                                             {257},
                                                             {1025},
                                                             {3, 7},
                                                             {1, 3, 7},
                                                             {1, 1, 1, 1, 1, 1, 1, 33}}) {
      std::size_t n = 1;
      for (auto d : shape)
        n *= d;
      auto data = relu_tests::corpus(type, n), original = data;
      auto host = rt.hostTensor(data, shape, *m::storageFormat(type));
      relu_tests::check(bool(host));
      auto group = core->openRoot();
      auto upload = rt.upload(*core, group, *host, *device);
      relu_tests::check(upload && core->drain());
      auto input = observe(*core, upload);
      auto work = type == NumericType::Float32
                      ? rt.elementwise(*core, group, *kernel, std::array{input}, {})
                      : rt.numericElementwise(*core, group, *kernel, std::array{input});
      relu_tests::check(work && core->drain());
      auto output = observe(*core, work);
      relu_tests::check(output.shape().size() == shape.size() &&
                        std::equal(shape.begin(), shape.end(), output.shape().begin()));
      auto download = rt.download(*core, group, output);
      relu_tests::check(download && core->drain());
      auto result = observe(*core, download);
      auto bytes = result.hostBytes();
      relu_tests::check(bool(bytes) && bytes->size() == data.size());
      for (std::size_t i = 0; i < n; ++i) {
        auto actual =
            relu_tests::read(bytes->data() + i * description.bytes(), description.bytes());
        auto expected = relu_tests::expected(
            type, relu_tests::read(data.data() + i * description.bytes(), description.bytes()));
        if (actual != expected)
          std::fprintf(stderr, "ReLU GPU %.*s case %zu cell %zu: %llx != %llx\n",
                       int(description.name.size()), description.name.data(), cases, i,
                       (unsigned long long)actual, (unsigned long long)expected);
        relu_tests::check(actual == expected);
        ++cells;
      }
      relu_tests::check(original == data);
      retire(*core, group);
      ++cases;
    }
  }
  const auto s = rt.stats();
  relu_tests::check(!s.bytes && !s.inFlight && !s.stagingBuffers && !s.argumentPacks &&
                    !s.argumentBytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings);
  std::printf("GPU ReLU: %zu exact cells in %zu type/shape cases; NaN payloads, signed zero, "
              "subnormals, immutability and cleanup passed\n",
              cells, cases);
}
