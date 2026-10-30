#include "../metal/MatmulLayout.h"
#include "../metal/Metal.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <vector>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
using Clock = std::chrono::steady_clock;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "GPU benchmark check %d: %s\n", __LINE__, #x);                          \
      std::exit(1);                                                                                \
    }                                                                                              \
  } while (false)
extern "C" void _mlir_ciface_kalandra_cpu_0(Float32Tensor<2> *, Float32Tensor<2> *,
                                            Float32Tensor<2> *, Invocation *);
extern "C" float _mlir_ciface_kalandra_cpu_1(Float32Tensor<1> *, Invocation *);
double milliseconds(Clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
struct Shape {
  const char *name;
  bool matrix;
  std::int64_t rows, inner, columns;
};
struct Data {
  Shape shape;
  std::vector<float> a, b, expected;
  explicit Data(Shape s)
      : shape(s), a(s.matrix ? s.rows * s.inner : s.inner), b(s.matrix ? s.inner * s.columns : 0),
        expected(s.matrix ? s.rows * s.columns : 1) {
    for (std::size_t i = 0; i < a.size(); ++i)
      a[i] = s.matrix ? (int(i % 17) - 8) / 16.0f : (int((i * 13) % 37) - 18) / 16.0f;
    for (std::size_t i = 0; i < b.size(); ++i)
      b[i] = (int(i % 11) - 5) / 16.0f;
    // Exact dyadic workloads: every product and partial sum fits float32 exactly.
    // This independent double reference therefore also matches the fixed float32 tree.
    if (s.matrix)
      for (std::int64_t row = 0; row < s.rows; ++row)
        for (std::int64_t col = 0; col < s.columns; ++col) {
          double sum = 0;
          for (std::int64_t k = 0; k < s.inner; ++k)
            sum += double(a[row * s.inner + k]) * double(b[k * s.columns + col]);
          // A singleton contraction has no initial +0 addition: preserve -0.
          expected[row * s.columns + col] =
              s.inner == 1 ? float(double(a[row]) * double(b[col])) : float(sum);
        }
    else {
      double sum = 0;
      for (auto v : a)
        sum += double(v);
      expected[0] = float(sum);
    }
  }
  void verify(const void *raw, std::size_t bytes) const {
    CHECK(bytes == expected.size() * 4);
    CHECK(std::memcmp(raw, expected.data(), bytes) == 0);
  }
  void immutable() const {
    for (std::size_t i = 0; i < a.size(); ++i)
      CHECK(a[i] == (shape.matrix ? (int(i % 17) - 8) / 16.0f : (int((i * 13) % 37) - 18) / 16.0f));
    for (std::size_t i = 0; i < b.size(); ++i)
      CHECK(b[i] == (int(i % 11) - 5) / 16.0f);
  }
};
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation c;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), c, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
}
void quiescent(TaskCore &core) {
  auto s = core.stats();
  CHECK(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
m::Buffer host(m::Runtime &r, const std::vector<float> &values,
               std::span<const std::int64_t> shape) {
  auto value = r.hostTensor(std::as_bytes(std::span(values)), shape, m::Format::Float32);
  CHECK(value);
  return *value;
}
m::Buffer upload(m::Runtime &r, TaskCore &core, m::Buffer value, m::Device device) {
  auto g = core.openRoot();
  auto t = r.upload(core, g, value, device);
  CHECK(t && core.drain());
  auto result = observe(core, t);
  retire(core, g);
  return result;
}
struct Direct {
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> a, b, output, parameters, readback;
  std::vector<id<MTLBuffer>> levels, counts;
  m::MatmulLayout matrixLayout;
  Direct(id<MTLDevice> device, id<MTLComputePipelineState> p, const Data &data,
         m::MatmulLayout layout = m::MatmulLayout::Scalar)
      : queue([device newCommandQueue]), pipeline(p), matrixLayout(layout) {
    CHECK(queue);
    auto make = [&](std::size_t bytes, bool shared) -> id<MTLBuffer> {
      auto value = [device newBufferWithLength:bytes
                                       options:shared ? MTLResourceStorageModeShared
                                                      : MTLResourceStorageModePrivate];
      CHECK(value);
      return value;
    };
    auto put = [&](const std::vector<float> &values) -> id<MTLBuffer> {
      auto staging = make(values.size() * 4, true), result = make(values.size() * 4, false);
      std::memcpy(staging.contents, values.data(), values.size() * 4);
      auto cmd = [queue commandBuffer];
      auto encoder = [cmd blitCommandEncoder];
      CHECK(cmd && encoder);
      [encoder copyFromBuffer:staging
                 sourceOffset:0
                     toBuffer:result
            destinationOffset:0
                         size:values.size() * 4];
      [encoder endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];
      CHECK(cmd.status == MTLCommandBufferStatusCompleted);
      return result;
    };
    a = put(data.a);
    if (data.shape.matrix)
      b = put(data.b);
    readback = make(data.expected.size() * 4, true);
    if (data.shape.matrix) {
      output = make(data.expected.size() * 4, false);
      parameters = make(12, true);
      const std::uint32_t dims[] = {std::uint32_t(data.shape.rows), std::uint32_t(data.shape.inner),
                                    std::uint32_t(data.shape.columns)};
      std::memcpy(parameters.contents, dims, 12);
    } else
      for (std::uint64_t n = data.a.size(); n > 1; n = (n + 1) / 2) {
        levels.push_back(make(((n + 1) / 2) * 4, false));
        auto count = make(4, true);
        auto word = std::uint32_t(n);
        std::memcpy(count.contents, &word, 4);
        counts.push_back(count);
      }
  }
  id<MTLBuffer> result(const Data &data) const {
    return data.shape.matrix ? output : levels.empty() ? a : levels.back();
  }
  id<MTLCommandBuffer> command(const Data &data) {
    auto cmd = [queue commandBuffer];
    CHECK(cmd);
    auto encode = [&](id<MTLBuffer> left, id<MTLBuffer> right, id<MTLBuffer> out,
                      id<MTLBuffer> params, std::uint64_t lanes) {
      auto encoder = [cmd computeCommandEncoder];
      CHECK(encoder);
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:left offset:0 atIndex:0];
      if (data.shape.matrix)
        [encoder setBuffer:right offset:0 atIndex:1];
      [encoder setBuffer:out offset:0 atIndex:data.shape.matrix ? 2 : 1];
      [encoder setBuffer:params offset:0 atIndex:data.shape.matrix ? 3 : 2];
      const auto workItems = data.shape.matrix
                                 ? m::matmulWorkItems(matrixLayout, std::uint32_t(data.shape.rows),
                                                      std::uint32_t(data.shape.columns))
                                 : lanes;
      [encoder dispatchThreadgroups:MTLSizeMake((workItems + 63) / 64, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
    };
    if (data.shape.matrix)
      encode(a, b, output, parameters, data.expected.size());
    else {
      auto input = a;
      std::uint64_t n = data.a.size();
      for (std::size_t i = 0; i < levels.size(); ++i) {
        n = (n + 1) / 2;
        encode(input, nil, levels[i], counts[i], n);
        input = levels[i];
      }
    }
    return cmd;
  }
  void verify(const Data &data) {
    auto cmd = [queue commandBuffer];
    auto encoder = [cmd blitCommandEncoder];
    CHECK(cmd && encoder);
    [encoder copyFromBuffer:result(data)
               sourceOffset:0
                   toBuffer:readback
          destinationOffset:0
                       size:data.expected.size() * 4];
    [encoder endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    CHECK(cmd.status == MTLCommandBufferStatusCompleted);
    data.verify(readback.contents, data.expected.size() * 4);
  }
  void immutable(const Data &data) {
    auto read = [&](id<MTLBuffer> input, const std::vector<float> &expected) {
      auto device = input.device;
      auto staging = [device newBufferWithLength:expected.size() * 4
                                         options:MTLResourceStorageModeShared];
      CHECK(staging);
      auto cmd = [queue commandBuffer];
      auto encoder = [cmd blitCommandEncoder];
      CHECK(cmd && encoder);
      [encoder copyFromBuffer:input
                 sourceOffset:0
                     toBuffer:staging
            destinationOffset:0
                         size:expected.size() * 4];
      [encoder endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];
      CHECK(cmd.status == MTLCommandBufferStatusCompleted);
      CHECK(std::memcmp(staging.contents, expected.data(), expected.size() * 4) == 0);
    };
    read(a, data.a);
    if (data.shape.matrix)
      read(b, data.b);
  }
};
struct Timing {
  double wall = 0, gpu = -1, upload = 0, compute = 0, download = 0, retire = 0;
};
Timing cpu(Data &data) {
  std::optional<Invocation> context;
  Float32Tensor<2> result{};
  Float32Tensor<2> a{
      data.a.data(), data.a.data(), 0, {data.shape.rows, data.shape.inner}, {data.shape.inner, 1}},
      b{data.b.data(),
        data.b.data(),
        0,
        {data.shape.inner, data.shape.columns},
        {data.shape.columns, 1}};
  Float32Tensor<1> v{data.a.data(), data.a.data(), 0, {data.shape.inner}, {1}};
  float scalar = 0;
  auto start = Clock::now();
  context.emplace();
  if (data.shape.matrix)
    _mlir_ciface_kalandra_cpu_0(&result, &a, &b, &*context);
  else
    scalar = _mlir_ciface_kalandra_cpu_1(&v, &*context);
  Timing t;
  t.wall = milliseconds(start);
  CHECK(!context->status());
  if (data.shape.matrix) {
    CHECK(result.sizes[0] == data.shape.rows && result.sizes[1] == data.shape.columns);
    data.verify(result.aligned + result.offset, data.expected.size() * 4);
    context->release(result.allocated);
  } else
    data.verify(&scalar, 4);
  CHECK(!context->live());
  return t;
}
Timing direct(Direct &gpu, const Data &data) {
  auto start = Clock::now();
  auto cmd = gpu.command(data);
  [cmd commit];
  [cmd waitUntilCompleted];
  Timing t;
  t.wall = milliseconds(start);
  CHECK(cmd.status == MTLCommandBufferStatusCompleted);
  if (cmd.GPUStartTime > 0 && cmd.GPUEndTime >= cmd.GPUStartTime)
    t.gpu = (cmd.GPUEndTime - cmd.GPUStartTime) * 1000;
  gpu.verify(data);
  return t;
}
Timing managed(m::Runtime &r, TaskCore &core, m::Kernel kernel, m::Device device, const Data &data,
               m::Buffer ha, m::Buffer hb, m::Buffer da, m::Buffer db, bool transfer) {
  Timing t;
  m::Buffer result;
  float scalar = 0;
  auto start = Clock::now();
  auto group = core.openRoot();
  CHECK(group);
  if (transfer) {
    auto a = r.upload(core, group, ha, device);
    TaskHandle b{};
    if (data.shape.matrix)
      b = r.upload(core, group, hb, device);
    CHECK(a && core.drain());
    da = observe(core, a);
    if (data.shape.matrix) {
      CHECK(b);
      db = observe(core, b);
    }
    t.upload = milliseconds(start);
  }
  auto compute = Clock::now();
  auto task = data.shape.matrix ? r.matmul(core, group, kernel, da, db)
                                : r.reduceSum(core, group, kernel, da);
  CHECK(task && core.drain());
  if (data.shape.matrix)
    result = observe(core, task);
  else {
    Invocation c;
    CHECK(core.observe(task, m::Runtime::scalarLayout(), c, &scalar));
  }
  t.compute = milliseconds(compute);
  if (transfer && data.shape.matrix) {
    auto begin = Clock::now();
    auto download = r.download(core, group, result);
    CHECK(download && core.drain());
    result = observe(core, download);
    t.download = milliseconds(begin);
  }
  auto done = Clock::now();
  retire(core, group);
  if (transfer) {
    da = {};
    db = {};
  }
  t.retire = milliseconds(done);
  t.wall = milliseconds(start);
  // Numerical validation and output deallocation are outside every timed path.
  if (data.shape.matrix) {
    if (result.onDevice()) {
      auto g = core.openRoot();
      auto down = r.download(core, g, result);
      CHECK(down && core.drain());
      result = observe(core, down);
      retire(core, g);
    }
    auto bytes = result.hostBytes();
    CHECK(bytes);
    data.verify(bytes->data(), bytes->size());
  } else
    data.verify(&scalar, 4);
  quiescent(core);
  return t;
}
int main(int argc, char **argv) {
  @autoreleasepool {
    CHECK(argc == 5 || argc == 6);
    const std::string directory = argv[1];
    unsigned warmups = std::stoul(argv[2]), samples = std::stoul(argv[3]),
             process = std::stoul(argv[4]);
    CHECK(warmups && samples);
    auto made = m::Runtime::create({.maxBytes = (argc == 6 ? 512ull : 64ull) * 1024 * 1024});
    CHECK(made);
    auto runtime = std::move(*made);
    auto selected = runtime->device();
    CHECK(selected);
    id<MTLDevice> device = nil;
    for (id<MTLDevice> d in MTLCopyAllDevices())
      if (d.registryID == selected->identity())
        device = d;
    CHECK(device);
    auto core = TaskCore::create({.workers = 1});
    CHECK(core);
    auto metadata = @{
      @"metadata" : @{
        @"device" : device.name,
        @"registry_id" : @(device.registryID),
        @"max_buffer_length" : @(device.maxBufferLength),
        @"workers" : @1
      }
    };
    auto encoded = [NSJSONSerialization dataWithJSONObject:metadata options:0 error:nullptr];
    CHECK(encoded);
    std::printf("%s\n", [[[NSString alloc] initWithData:encoded
                                               encoding:NSUTF8StringEncoding] UTF8String]);
    std::vector<Shape> shapes = {{"matmul-8", true, 8, 8, 8},
                                 {"matmul-32", true, 32, 32, 32},
                                 {"matmul-96", true, 96, 96, 96},
                                 {"matmul-256", true, 256, 256, 256},
                                 {"matmul-deep-tail", true, 32, 1025, 65},
                                 {"sum-64", false, 0, 64, 0},
                                 {"sum-4097", false, 0, 4097, 0},
                                 {"sum-65536", false, 0, 65536, 0},
                                 {"sum-1048576", false, 0, 1048576, 0}};
    if (argc == 6) {
      CHECK(std::string(argv[5]) == "selection");
      shapes = {{"small-scalar", true, 32, 256, 32},  {"narrow-scalar", true, 1, 256, 65536},
                {"short-scalar", true, 256, 8, 256},  {"rows2-256", true, 256, 256, 256},
                {"rows2-odd", true, 257, 257, 257},   {"quad-512", true, 512, 512, 512},
                {"quad-odd", true, 513, 513, 513},    {"outside-profile", true, 514, 256, 514},
                {"sum-control", false, 0, 1048576, 0}};
    }
    std::mt19937 rng(0x562214u + process);
    std::shuffle(shapes.begin(), shapes.end(), rng);
    for (auto shape : shapes) {
      @autoreleasepool {
        Data data(shape);
        auto path = directory + (shape.matrix ? "/kernel0.metallib" : "/kernel1.metallib");
        auto loaded = shape.matrix ? runtime->loadMatmulKernel(path, *selected)
                                   : runtime->loadReductionKernel(path, *selected);
        CHECK(loaded);
        NSError *error = nil;
        auto library = [device
            newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]
                        error:&error];
        CHECK(library);
        auto layout = m::MatmulLayout::Scalar;
#ifndef KALANDRA_BENCH_SCALAR_MATMUL
        if (shape.matrix && [device.name isEqualToString:@"Apple M2 Max"])
          layout = m::selectMatmulLayout(std::uint32_t(shape.rows), std::uint32_t(shape.inner),
                                         std::uint32_t(shape.columns));
#endif
        auto function = [library newFunctionWithName:shape.matrix ? @(m::matmulEntryName(layout))
                                                                  : @"kalandra_reduce_sum"];
        CHECK(function);
        auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        CHECK(pipeline && pipeline.maxTotalThreadsPerThreadgroup >= 64);
        Direct gpu(device, pipeline, data, layout);
        std::array<std::int64_t, 2> aShape{shape.rows, shape.inner},
            bShape{shape.inner, shape.columns};
        auto ha = host(*runtime, data.a,
                       shape.matrix ? std::span<const std::int64_t>(aShape)
                                    : std::span<const std::int64_t>(&shape.inner, 1));
        m::Buffer hb, db;
        auto da = upload(*runtime, *core, ha, *selected);
        if (shape.matrix) {
          hb = host(*runtime, data.b, bShape);
          db = upload(*runtime, *core, hb, *selected);
        }
        const auto baseline = runtime->stats().bytes;
        for (unsigned round = 0; round < warmups + samples; ++round) {
          std::array<unsigned, 4> order{0, 1, 2, 3};
          std::shuffle(order.begin(), order.end(), rng);
          for (auto mode : order) {
            @autoreleasepool {
              Timing t;
              if (mode == 0)
                t = cpu(data);
              else if (mode == 1)
                t = direct(gpu, data);
              else
                t = managed(*runtime, *core, *loaded, *selected, data, ha, hb, da, db, mode == 3);
              quiescent(*core);
              auto s = runtime->stats();
              CHECK(s.bytes == baseline && !s.inFlight && !s.stagingBuffers);
              if (round >= warmups) {
                const char *names[] = {"cpu", "device", "resident", "transfers"};
                std::printf(
                    "{\"workload\":\"%s\",\"mode\":\"%s\",\"round\":%u,\"rows\":%lld,\"inner\":%"
                    "lld,\"columns\":%lld,\"wall_ms\":%.9f,\"upload_ms\":%.9f,\"compute_ms\":%.9f,"
                    "\"download_ms\":%.9f,\"retire_ms\":%.9f,\"gpu_ms\":",
                    shape.name, names[mode], round - warmups, (long long)shape.rows,
                    (long long)shape.inner, (long long)shape.columns, t.wall, t.upload, t.compute,
                    t.download, t.retire);
                if (t.gpu < 0)
                  std::printf("null");
                else
                  std::printf("%.9f", t.gpu);
                std::puts("}");
                std::fflush(stdout);
              }
            }
          }
        }
        data.immutable();
        gpu.immutable(data);
        // Public device inputs must remain byte-identical too, outside timings.
        for (auto pair : {std::pair{da, &data.a}, std::pair{db, &data.b}})
          if (pair.first) {
            auto group = core->openRoot();
            auto down = runtime->download(*core, group, pair.first);
            CHECK(down && core->drain());
            auto value = observe(*core, down);
            auto bytes = value.hostBytes();
            CHECK(bytes && bytes->size() == pair.second->size() * 4 &&
                  std::memcmp(bytes->data(), pair.second->data(), bytes->size()) == 0);
            retire(*core, group);
          }
      }
    }
    quiescent(*core);
    auto stats = runtime->stats();
    CHECK(!stats.bytes && !stats.inFlight && !stats.metalBuffers && !stats.stagingBuffers &&
          !stats.hostBackings && !stats.deviceBackings);
  }
}
