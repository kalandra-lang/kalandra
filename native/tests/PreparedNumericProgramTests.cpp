#include "../benchmarks/fair/TransformerBlock.h"
#include "../metal/Metal.h"
#include <new>
#include <thread>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Prepared program check %d: %s\n", __LINE__, #x);                       \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
#include "LegacyProgram.h"
#include "TransformerProgram.h"
static void retire(TaskCore &c, GroupHandle g) {
  CHECK(c.close(g) && c.drain() && c.release(g));
  auto s = c.stats();
  CHECK(!s.groups && !s.records && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
static m::Buffer observe(TaskCore &c, TaskHandle t) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(c.observe(t, m::Runtime::bufferLayout(), ctx, slot));
  auto *v = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto b = *v;
  v->~Buffer();
  return b;
}
template <class F> static void until(TaskCore &c, F done) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!done()) {
    CHECK(std::chrono::steady_clock::now() < end);
    c.step();
    std::this_thread::yield();
  }
}
static std::vector<std::uint32_t> download(m::Runtime &r, TaskCore &c, GroupHandle g, m::Buffer b) {
  auto t = r.download(c, g, b);
  CHECK(t && c.drain());
  auto h = observe(c, t);
  auto bytes = h.hostBytes();
  CHECK(bytes);
  std::vector<std::uint32_t> result(bytes->size() / 4);
  std::memcpy(result.data(), bytes->data(), bytes->size());
  return result;
}
static void test(const char *path, std::uint64_t budget, bool full) {
  m::Options options;
  options.maxBytes = budget;
  options.profile = true;
  auto made = m::Runtime::create(options);
  CHECK(made);
  auto &r = **made;
  {
    Program prepared(r, path);
    LegacyProgram legacy(r, path);
    CHECK(prepared.executable.on(*r.device()));
    transformer::Inputs data(3, 7, 1);
    std::array<m::Buffer, 7> input;
    for (unsigned i = 0; i < 7; ++i) {
      auto core = TaskCore::create();
      auto g = core->openRoot();
      auto h = r.hostTensor(std::as_bytes(std::span(data.values[i])), data.shapes[i],
                            m::Format::Float32);
      CHECK(h);
      auto t = r.upload(*core, g, *h, *r.device());
      CHECK(t && core->drain());
      input[i] = observe(*core, t);
      retire(*core, g);
    }
    const auto baseline = r.stats();
    auto clean = [&] {
      auto s = r.stats();
      CHECK(s.bytes == baseline.bytes && !s.inFlight && !s.stagingBuffers &&
            s.argumentBytes == baseline.argumentBytes && s.argumentPacks == baseline.argumentPacks);
    };
    auto run = [&](auto &program, float epsilon, float scale) {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      auto t = program.launch(r, *c, g, input, epsilon, scale);
      CHECK(t && c->drain());
      CHECK(c->failure(t).kind == TaskFailure::None);
      auto result = download(r, *c, g, observe(*c, t));
      retire(*c, g);
      clean();
      return result;
    };
    std::array<std::vector<std::uint32_t>, 8> expected;
    for (unsigned i = 0; i < 8; ++i) {
      float eps = i % 2 ? .03f : transformer::epsilon;
      float scale = data.scale * (i + 1) / 4;
      expected[i] = run(legacy, eps, scale);
      CHECK(run(prepared, eps, scale) == expected[i]);
    }
    auto before = r.stats();
    run(prepared, transformer::epsilon, data.scale);
    auto after = r.stats();
    const auto commands =
        after.submissions - before.submissions - 1; // readback is outside the program
    CHECK(after.dispatches - before.dispatches == 16);
    CHECK(commands >= 4 && commands <= 16);
    if (full)
      CHECK(commands == 4);
    else if (budget <= 65536)
      CHECK(commands == 16);
    // Shared immutable code, separate values/captures/parameter storage.
    if (full) {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      std::array<TaskHandle, 8> tasks;
      for (unsigned i = 0; i < 8; ++i)
        tasks[i] = prepared.launch(r, *c, g, input, i % 2 ? .03f : transformer::epsilon,
                                   data.scale * (i + 1) / 4);
      CHECK(c->drain());
      for (unsigned i = 0; i < 8; ++i) {
        CHECK(c->failure(tasks[i]).kind == TaskFailure::None);
        CHECK(download(r, *c, g, observe(*c, tasks[i])) == expected[i]);
      }
      retire(*c, g);
      clean();
    }
    for (unsigned i = 0; i < 7; ++i) {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      auto actual = download(r, *c, g, input[i]);
      CHECK(actual.size() == data.values[i].size());
      CHECK(std::memcmp(actual.data(), data.values[i].data(), actual.size() * 4) == 0);
      retire(*c, g);
      clean();
    }
    for (float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      auto t = prepared.launch(r, *c, g, input, invalid, data.scale);
      CHECK(c->drain());
      auto failure = c->failure(t);
      CHECK(failure.kind == TaskFailure::Fault && failure.code == 4);
      retire(*c, g);
      clean();
    }
    {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      std::array<std::uint64_t, 3> bad{UINT64_C(1) << 40, 0, 0};
      auto t = r.executeNumericProgram(*c, g, prepared.executable, input, bad);
      CHECK(c->drain() && c->failure(t).kind == TaskFailure::Fault && c->failure(t).code == 3);
      retire(*c, g);
      clean();
    }
    if (!full) {
      input = {};
      prepared.executable = {};
      legacy.kernels = {};
      const auto final = r.stats();
      CHECK(!final.bytes && !final.argumentBytes && !final.argumentPacks && !final.metalBuffers &&
            !final.hostBackings && !final.deviceBackings);
      return;
    }
    // A later host shape fault waits for prior encoded work. Its device failure
    // has precedence; an earlier invalid epsilon keeps arithmetic fault 4.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      {
        auto norm = r.loadTensorTransformKernel(
            std::string(path) + "/kernel0.metallib", *r.device(),
            kalandra::TensorTransformKind::RmsNormRows, kalandra::NumericType::Float32);
        CHECK(norm);
        std::array<std::uint32_t, 1> operands{0};
        std::array<std::uint64_t, 1> epsilon{
            std::bit_cast<std::uint32_t>(scenario == 2 ? -1.f : transformer::epsilon)};
        using K = m::NumericInstructionKind;
        std::array<m::NumericInstruction, 2> code{
            {{K::Map, *norm, operands, epsilon, 1}, {K::ShapeFault}}};
        auto plan = r.prepareNumericProgram(1, code, 1, kalandra::NumericType::Float32, false);
        CHECK(plan);
        auto c = TaskCore::create();
        auto g = c->openRoot();
        auto start = r.stats().submissions;
        r.holdCompletions(true);
        auto t = r.executeNumericProgram(*c, g, *plan, std::array{input[0]});
        until(*c, [&] { return c->stats().externalPending == 1; });
        if (scenario == 1)
          r.failNextCompletion();
        r.holdCompletions(false);
        CHECK(c->drain());
        CHECK(c->failure(t).kind == TaskFailure::Fault);
        CHECK(c->failure(t).code == (scenario == 0   ? 1
                                     : scenario == 1 ? std::int64_t(m::Error::Device)
                                                     : 4));
        CHECK(r.stats().submissions == start + 1);
        retire(*c, g);
      }
      clean();
    }
    r.failAllocationAt(0);
    std::uint64_t attempts;
    {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      auto t = prepared.launch(r, *c, g, input, transformer::epsilon, data.scale);
      CHECK(t && c->drain() && c->failure(t).kind == TaskFailure::None);
      attempts = r.stats().attempts;
      retire(*c, g);
      clean();
    }
    for (std::uint64_t point = 1; point <= attempts; ++point) {
      r.failAllocationAt(point);
      auto c = TaskCore::create();
      auto g = c->openRoot();
      auto t = prepared.launch(r, *c, g, input, transformer::epsilon, data.scale);
      CHECK(t && c->drain());
      CHECK(c->failure(t).kind == TaskFailure::Fault);
      CHECK(c->failure(t).code == 2);
      r.failAllocationAt(0);
      retire(*c, g);
      clean();
    }
    for (unsigned boundary = 0; boundary < commands; ++boundary)
      for (unsigned scenario = 0; scenario < 3; ++scenario) {
        auto c = TaskCore::create();
        auto g = c->openRoot();
        auto start = r.stats().submissions;
        r.holdCompletions(true);
        auto t = prepared.launch(r, *c, g, input, transformer::epsilon, data.scale);
        for (unsigned i = 0; i < boundary; ++i) {
          until(*c, [&] {
            return r.stats().submissions == start + i + 1 && c->stats().externalPending == 1;
          });
          r.holdCompletions(false);
          auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
          while (r.stats().completions < start + i + 1) {
            CHECK(std::chrono::steady_clock::now() < end);
            std::this_thread::yield();
          }
          r.holdCompletions(true);
        }
        until(*c, [&] {
          return r.stats().submissions == start + boundary + 1 && c->stats().externalPending == 1;
        });
        auto held = r.stats();
        CHECK(held.inFlight == 1 && held.stagingBuffers == 4 && held.bytes > baseline.bytes);
        if (scenario < 2)
          CHECK(c->cancel(g, scenario == 1));
        else
          r.failNextCompletion();
        CHECK(r.stats().bytes == held.bytes && c->stats().externalPending == 1);
        r.holdCompletions(false);
        CHECK(c->drain());
        CHECK(c->failure(t).kind == (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
        if (scenario == 1)
          CHECK(c->failure(g).kind == TaskFailure::DeadlineExceeded);
        if (scenario == 2)
          CHECK(c->failure(t).code == std::int64_t(m::Error::Device));
        CHECK(r.stats().submissions == start + boundary + 1);
        retire(*c, g);
        clean();
      }
    {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      r.holdCompletions(true);
      prepared.launch(r, *c, g, input, transformer::epsilon, data.scale);
      until(*c, [&] { return c->stats().externalPending == 1; });
      std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        r.holdCompletions(false);
      });
      c.reset();
      release.join();
      clean();
    }
    {
      auto c = TaskCore::create();
      auto g = c->openRoot();
      Program temporary(r, path);
      r.holdCompletions(true);
      auto t = temporary.launch(r, *c, g, input, transformer::epsilon, data.scale);
      temporary.executable = {};
      until(*c, [&] { return c->stats().externalPending == 1; });
      r.holdCompletions(false);
      CHECK(c->drain() && c->failure(t).kind == TaskFailure::None);
      retire(*c, g);
      clean();
    }
    std::printf("prepared: %llu allocation checkpoints, %llu commands/16 dispatches, 12 held "
                "failure boundaries, changing captures and 8 concurrent calls passed\n",
                (unsigned long long)attempts, (unsigned long long)commands);
  }
  auto s = r.stats();
  CHECK(!s.bytes && !s.argumentBytes && !s.argumentPacks && !s.metalBuffers && !s.hostBackings &&
        !s.deviceBackings);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  test(argv[1], UINT64_C(512) * 1024 * 1024, true);
  for (auto budget : {16384u, 32768u, 65536u})
    test(argv[1], budget, false);
}
