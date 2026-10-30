#include "Metal.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
extern "C" float _mlir_ciface_kalandra_test_entry(Float32Tensor<1> *, Invocation *);
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "CPU/Metal ABI assertion %d: %s\n", __LINE__, #x);                      \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
m::Buffer observe(TaskCore &core, TaskHandle t) {
  Invocation c;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(t, m::Runtime::bufferLayout(), c, slot));
  auto *p = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto value = *p;
  p->~Buffer();
  return value;
}
void clean(TaskCore &c, GroupHandle g) {
  CHECK(c.close(g));
  CHECK(c.drain());
  CHECK(c.release(g));
  auto s = c.stats();
  CHECK(!s.records && !s.groups && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
void empty(m::Runtime &r) {
  auto s = r.stats();
  CHECK(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers && !s.stagingBuffers &&
        !s.inFlight);
}
Float32Tensor<1> allocated(Invocation &context, std::size_t n) {
  auto *p = static_cast<float *>(context.allocate(n * 4));
  CHECK(p);
  return {p, p, 0, {std::int64_t(n)}, {1}};
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  auto made = m::Runtime::create();
  CHECK(made);
  auto r = std::move(*made);
  auto kernel = r->loadElementwiseKernel(std::string(argv[1]) + "/kernel0.metallib", *r->device());
  CHECK(kernel);
  std::uint64_t cases = 0;
  for (unsigned workers : {1u, 2u, 4u})
    for (unsigned n : {0u, 1u, 2u, 63u, 64u, 65u, 4097u, 8193u}) {
      Invocation input;
      auto a = allocated(input, n), b = allocated(input, n);
      std::vector<float> reference;
      for (unsigned i = 0; i < n; ++i) {
        a.aligned[i] = float(i % 11) - 5;
        b.aligned[i] = float(i % 7) - 3;
        volatile float product = a.aligned[i] * b.aligned[i];
        reference.push_back(product + a.aligned[i]);
      }
      auto ha = r->fromCpu(input, a.view()), hb = r->fromCpu(input, b.view());
      CHECK(ha && hb);
      if (n)
        CHECK(ha->hostBytes()->data() == reinterpret_cast<const std::byte *>(a.aligned));
      input.settle();
      CHECK(!input.live()); // Retained immutable payloads remain readable.
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      auto group = core->openRoot();
      auto ta = r->upload(*core, group, *ha, *r->device()),
           tb = r->upload(*core, group, *hb, *r->device());
      ha = {};
      hb = {};
      CHECK(core->drain());
      auto da = observe(*core, ta), db = observe(*core, tb);
      auto work = r->elementwise(*core, group, *kernel, da, db);
      CHECK(core->drain());
      auto device = observe(*core, work);
      Invocation output;
      CHECK(!r->toCpu<1>(output, device));
      CHECK(!output.live()); // No implicit download.
      auto down = r->download(*core, group, device);
      CHECK(core->drain());
      auto host = observe(*core, down);
      auto cpu = r->toCpu<1>(output, host);
      CHECK(cpu);
      clean(*core, group);
      da = {};
      db = {};
      device = {};
      host = {};
      empty(*r);
      CHECK(output.live() == 1 && cpu->sizes[0] == n && cpu->strides[0] == 1 && cpu->offset == 0);
      for (unsigned i = 0; i < n; ++i)
        CHECK(std::bit_cast<std::uint32_t>(cpu->aligned[i]) ==
              std::bit_cast<std::uint32_t>(reference[i]));
      while (reference.size() > 1) {
        std::vector<float> next;
        for (std::size_t i = 0; i < reference.size(); i += 2) {
          if (i + 1 == reference.size())
            next.push_back(reference[i]);
          else {
            volatile float sum = reference[i] + reference[i + 1];
            next.push_back(float(sum));
          }
        }
        reference = std::move(next);
      }
      auto result = _mlir_ciface_kalandra_test_entry(&*cpu, &output);
      CHECK(!output.status());
      CHECK(std::bit_cast<std::uint32_t>(result) ==
            std::bit_cast<std::uint32_t>(reference.empty() ? 0.f : reference[0]));
      output.settle();
      CHECK(!output.live());
      ++cases;
    }
  // Dense subview offsets are checked against the actual retained allocation.
  {
    Invocation context;
    auto data = allocated(context, 16);
    for (unsigned i = 0; i < 16; ++i)
      data.aligned[i] = float(i);
    data.aligned += 2;
    data.offset = 1;
    data.sizes[0] = 3;
    auto view = r->fromCpu(context, data.view());
    CHECK(view && view->bytes() == 12);
    CHECK(view->hostBytes()->data() == reinterpret_cast<const std::byte *>(data.allocated + 3));
    CHECK(r->stats().bytes == 64); // Charge the full allocation retained by a small view.
    context.settle();
    Invocation out;
    auto copy = r->toCpu<1>(out, *view);
    CHECK(copy);
    CHECK(copy->aligned[0] == 3 && copy->aligned[2] == 5);
    view = {};
    empty(*r);
  }
  // Invalid addresses are compared against ledger metadata, never dereferenced.
  for (unsigned bad = 0; bad < 10; ++bad) {
    Invocation owner, foreign;
    auto data = allocated(owner, 4);
    auto view = data.view();
    std::array<std::int64_t, 1> shape{4}, stride{1};
    view.sizes = shape;
    view.strides = stride;
    if (bad == 0)
      view.allocated = reinterpret_cast<void *>(0x1000);
    if (bad == 1)
      view.aligned = reinterpret_cast<void *>(0x1000);
    if (bad == 2)
      view.aligned = reinterpret_cast<std::byte *>(data.aligned) + 1;
    if (bad == 3)
      view.offset = INT64_MAX;
    if (bad == 4)
      view.offset = -1;
    if (bad == 5)
      shape[0] = 5;
    if (bad == 6)
      stride[0] = 2;
    if (bad == 7)
      shape[0] = -1;
    if (bad == 8)
      view.strides = {};
    if (bad == 9)
      view.aligned = data.aligned + 4;
    CHECK(!r->fromCpu(owner, view));
    CHECK(!r->fromCpu(foreign, data.view()));
    empty(*r);
  }
  // Rank-zero/two/eight metadata and empty descriptors share the MLIR layout.
  {
    Invocation owner, out;
    auto data = allocated(owner, 8);
    const std::array<std::uint32_t, 8> bits{0,          0x80000000, 1,          0x80000001,
                                            0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001};
    std::memcpy(data.aligned, bits.data(), 32);
    Float32Tensor<2> matrix{data.allocated, data.aligned, 0, {2, 4}, {4, 1}};
    auto alias = r->fromCpu(owner, matrix.view());
    CHECK(alias);
    auto two = r->toCpu<2>(out, *alias);
    CHECK(two);
    CHECK(two->sizes == matrix.sizes && two->strides == matrix.strides &&
          !std::memcmp(two->aligned, bits.data(), 32));
    CHECK(!r->toCpu<1>(out, *alias));
    Float32Tensor<0> scalar{data.allocated, data.aligned, 2};
    auto one = r->fromCpu(owner, scalar.view());
    CHECK(one);
    auto zero = r->toCpu<0>(out, *one);
    CHECK(zero);
    CHECK(std::bit_cast<std::uint32_t>(*zero->aligned) == 1);
    Float32Tensor<8> eight{
        data.allocated, data.aligned, 0, {1, 1, 1, 1, 1, 1, 2, 4}, {8, 8, 8, 8, 8, 8, 4, 1}};
    auto high = r->fromCpu(owner, eight.view());
    CHECK(high);
    auto highCopy = r->toCpu<8>(out, *high);
    CHECK(highCopy && highCopy->sizes == eight.sizes);
    Float32Tensor<1> noData{nullptr, nullptr, 0, {0}, {1}};
    auto nothing = r->fromCpu(owner, noData.view());
    CHECK(nothing);
    auto noCopy = r->toCpu<1>(out, *nothing);
    CHECK(noCopy && noCopy->sizes[0] == 0);
    owner.settle();
    CHECK(!r->fromCpu(owner, noData.view()));
  }
  empty(*r);
  // Export failure cannot leak or publish a CPU descriptor; alias budget failures unwind.
  {
    Invocation source;
    auto data = allocated(source, 16);
    auto alias = r->fromCpu(source, data.view());
    CHECK(alias);
    Invocation failed;
    failed.failAllocationAt(1);
    CHECK(!r->toCpu<1>(failed, *alias));
    CHECK(!failed.live());
    source.fault(1);
    CHECK(!r->fromCpu(source, data.view()));
  }
  empty(*r);
  for (bool budget : {false, true}) {
    m::Options options;
    if (budget)
      options.maxBytes = 8;
    auto local = m::Runtime::create(options);
    CHECK(local);
    Invocation source;
    auto data = allocated(source, 16);
    auto view = data.view();
    std::array<std::int64_t, 1> shape{1};
    view.sizes = shape;
    if (!budget)
      (*local)->failAllocationAt(1);
    CHECK(!(*local)->fromCpu(source, view));
    empty(**local);
    CHECK(source.live() == 1);
  }
  std::printf("CPU/Metal tensor ABI passed: %llu source GPU to compiled CPU reductions, checked "
              "leases, shapes, bounds and cleanup\n",
              (unsigned long long)cases);
}
