#include "../../metal/Metal.h"
#include "ResidualFfn.h"
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation ctx;
  alignas(m::Buffer) std::byte bytes[sizeof(m::Buffer)];
  ffn::check(core.observe(task, m::Runtime::bufferLayout(), ctx, bytes));
  auto *p = std::launder(reinterpret_cast<m::Buffer *>(bytes));
  auto b = *p;
  p->~Buffer();
  return b;
}
static void retire(TaskCore &c, GroupHandle g) {
  ffn::check(c.close(g) && c.drain() && c.release(g));
}
int main(int argc, char **argv) {
  ffn::check(argc == 9);
  unsigned rows = std::stoul(argv[2]), inner = std::stoul(argv[3]), cols = std::stoul(argv[4]),
           count = std::stoul(argv[5]), rounds = std::stoul(argv[6]), dump = std::stoul(argv[7]);
  ffn::check(rows && inner && cols && std::stoul(argv[8]) == 0);
  auto made = m::Runtime::create();
  ffn::check(bool(made));
  auto &r = **made;
  auto core = TaskCore::create();
  auto device = r.device();
  ffn::check(core && device);
  auto loaded = r.loadNumericMatmulKernel(argv[1], *device, kalandra::NumericType::Float32);
  ffn::check(bool(loaded));
  std::vector<float> a(std::size_t(rows) * inner), b(std::size_t(inner) * cols);
  for (std::size_t i = 0; i < a.size(); ++i)
    a[i] = float(int(i % 17) - 8) / 8;
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = float(int(i % 13) - 6) / 8;
  auto upload = [&](auto &v, auto shape) {
    auto h = r.hostTensor(std::as_bytes(std::span(v)), shape, m::Format::Float32);
    ffn::check(bool(h));
    auto g = core->openRoot();
    auto t = r.upload(*core, g, *h, *device);
    ffn::check(t && core->drain());
    auto d = observe(*core, t);
    retire(*core, g);
    return d;
  };
  auto da = upload(a, std::array{std::int64_t(rows), std::int64_t(inner)}),
       db = upload(b, std::array{std::int64_t(inner), std::int64_t(cols)});
  const auto resident = r.stats().bytes;
  auto call = [&](bool save) {
    auto g = core->openRoot();
    auto t = r.matmul(*core, g, *loaded, da, db);
    ffn::check(t && core->drain() && core->failure(t).kind == TaskFailure::None);
    auto value = observe(*core, t);
    ffn::check(value.bytes() == std::size_t(rows) * cols * 4);
    if (save) {
      auto dt = r.download(*core, g, value);
      ffn::check(dt && core->drain());
      auto h = observe(*core, dt);
      auto bytes = h.hostBytes();
      ffn::check(bool(bytes));
      std::vector<float> v(rows * cols);
      std::memcpy(v.data(), bytes->data(), bytes->size());
      ffn::dump(v, ffn::sum(v));
    }
    value = {};
    retire(*core, g);
    ffn::check(r.stats().bytes == resident && !r.stats().inFlight);
    return double(rows) * cols * 4;
  };
  if (dump)
    call(true);
  else
    for (int round = -2; round < int(rounds); ++round) {
      double checksum = 0;
      auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < count; ++i)
        checksum += call(false);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g}\n", round, ffn::elapsed(start),
                  checksum);
    }
}
