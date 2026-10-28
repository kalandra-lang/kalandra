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
static void test(const char *path, std::uint64_t budget, bool full, bool preencoded,
                 unsigned rows = 3, unsigned width = 7) {
  m::Options options;
  options.maxBytes = budget;
  options.profile = true;
  options.preencodedProgression = preencoded;
  options.retainedProgressionReferences = false;
  auto made = m::Runtime::create(options);
  CHECK(made);
  auto &r = **made;
  {
    Program prepared(r, path);
    LegacyProgram legacy(r, path);
    CHECK(prepared.executable.on(*r.device()));
    transformer::Inputs data(rows, width, 1);
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
            s.argumentBytes == baseline.argumentBytes &&
            s.argumentPacks == baseline.argumentPacks && !r.progressionCommandReservations());
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
    CHECK(after.dispatches - before.dispatches == (full ? 14 : 16));
    CHECK(commands >= 4 && commands <= 16);
    if (full)
      CHECK(commands == (rows == 3 ? 4 : 7));
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
    // Provenance and live aliases: only invocation-produced, last-use buffers
    // may be overwritten. An Alias of an input remains an input forever.
    {
      constexpr auto f32 = kalandra::NumericType::Float32;
      auto add = r.loadNumericGraphKernel(std::string(path) + "/kernel0-stage5.metallib",
                                          *r.device(), f32, {2, 0}, std::array{f32, f32}, {},
                                          std::array<std::uint32_t, 2>{0, 1},
                                          std::array<std::uint8_t, 2>{0, 0});
      CHECK(add);
      using K = m::NumericInstructionKind;
      const std::array<std::uint32_t, 2> aa{0, 0}, ba{1, 0}, fork{2, 3};
      const std::array<std::uint32_t, 1> one{1}, original{0};
      for (unsigned scenario = 0; scenario < 4; ++scenario) {
        std::vector<m::NumericInstruction> code;
        std::uint32_t result = 2;
        if (scenario == 0 || scenario == 3) {
          code = {{K::Map, *add, aa, {}, 1, 1}, {K::Map, *add, ba, {}, 2, 1}};
          if (scenario == 3)
            result = 1; // Result observation is a live use.
        } else if (scenario == 1) {
          code = {{K::Map, *add, aa, {}, 1, 1},
                  {K::Alias, {}, one, {}, 2},
                  {K::Map, *add, ba, {}, 3, 1},
                  {K::Map, *add, fork, {}, 4, 1}};
          result = 4;
        } else
          code = {{K::Alias, {}, original, {}, 1}, {K::Map, *add, ba, {}, 2, 1}};
        auto c = TaskCore::create();
        auto g = c->openRoot();
        auto legacyTask = r.numericProgram(*c, g, std::array{input[0]}, code, result, f32, false);
        CHECK(c->drain() && c->failure(legacyTask).kind == TaskFailure::None);
        const auto expected = download(r, *c, g, observe(*c, legacyTask));
        retire(*c, g);
        {
          auto plan = r.prepareNumericProgram(1, code, result, f32, false);
          CHECK(plan);
          c = TaskCore::create();
          g = c->openRoot();
          r.holdCompletions(true);
          const auto backing = r.stats().deviceBackings;
          auto t = r.executeNumericProgram(*c, g, *plan, std::array{input[0]});
          until(*c, [&] { return c->stats().externalPending == 1; });
          CHECK(r.stats().deviceBackings == backing + (scenario == 1 || scenario == 3 ? 2 : 1));
          r.holdCompletions(false);
          CHECK(c->drain() && c->failure(t).kind == TaskFailure::None);
          CHECK(download(r, *c, g, observe(*c, t)) == expected);
          retire(*c, g);
        }
        auto verify = TaskCore::create();
        auto vg = verify->openRoot();
        auto originalBits = download(r, *verify, vg, input[0]);
        CHECK(std::memcmp(originalBits.data(), data.values[0].data(), originalBits.size() * 4) ==
              0);
        retire(*verify, vg);
        clean();
      }
      // Prepared same-cell chains also preserve higher-rank tensor metadata,
      // private-backing reuse, and immutable original inputs.
      for (unsigned stages : {5u, 513u})
        for (const auto &shape :
             std::vector<std::vector<std::int64_t>>{{24}, {2, 3, 4}, {1, 2, 1, 3, 1, 1, 1, 4}}) {
          {
            std::vector<float> cells(24);
            std::vector<std::uint32_t> original(24), expected(24);
            for (unsigned i = 0; i < cells.size(); ++i) {
              cells[i] = float(int(i) - 12);
              original[i] = std::bit_cast<std::uint32_t>(cells[i]);
              float value = cells[i];
              for (unsigned stage = 0; stage < stages; ++stage)
                value = value + value;
              expected[i] = std::bit_cast<std::uint32_t>(value);
            }
            auto c = TaskCore::create();
            auto g = c->openRoot();
            auto h = r.hostTensor(std::as_bytes(std::span(cells)), shape, m::Format::Float32);
            CHECK(h);
            auto upload = r.upload(*c, g, *h, *r.device());
            CHECK(upload && c->drain());
            auto source = observe(*c, upload);
            std::vector<std::array<std::uint32_t, 2>> operands(stages);
            std::vector<m::NumericInstruction> code(stages);
            for (unsigned i = 0; i < code.size(); ++i) {
              operands[i] = {i, i};
              code[i] = {K::Map, *add, operands[i], {}, i + 1, 1};
            }
            auto plan = r.prepareNumericProgram(1, code, stages, f32, false);
            CHECK(plan);
            auto before = r.stats();
            auto task = r.executeNumericProgram(*c, g, *plan, std::array{source});
            CHECK(task && c->drain() && c->failure(task).kind == TaskFailure::None);
            auto after = r.stats();
            CHECK(after.submissions - before.submissions == (stages + 3) / 4 &&
                  after.dispatches - before.dispatches == stages);
            auto result = observe(*c, task);
            CHECK(std::equal(result.shape().begin(), result.shape().end(), shape.begin(),
                             shape.end()));
            CHECK(download(r, *c, g, result) == expected);
            CHECK(download(r, *c, g, source) == original);
            retire(*c, g);
          }
          clean();
        }
    }
    // Fused projections retain the staged bit patterns, including odd reductions,
    // cancellation-sensitive values, signed zeros, subnormals, infinities and NaNs.
    {
      constexpr auto f32 = kalandra::NumericType::Float32;
      auto matrix = r.loadNumericMatmulKernel(std::string(path) + "/kernel0-stage1.metallib",
                                              *r.device(), f32);
      auto scale = r.loadNumericGraphKernel(
          std::string(path) + "/kernel0-stage3.metallib", *r.device(), f32, {1, 1}, std::array{f32},
          std::array{f32}, std::array<std::uint32_t, 1>{0}, std::array<std::uint8_t, 1>{0});
      auto relu = r.loadNumericGraphKernel(
          std::string(path) + "/kernel0-stage6.metallib", *r.device(), f32, {1, 0}, std::array{f32},
          {}, std::array<std::uint32_t, 1>{0}, std::array<std::uint8_t, 1>{0});
      CHECK(matrix && scale && relu);
      auto uploadBits = [&](const std::vector<std::uint32_t> &bits,
                            std::array<std::int64_t, 2> shape) {
        auto h = r.hostTensor(std::as_bytes(std::span(bits)), shape, m::Format::Float32);
        CHECK(h);
        auto c = TaskCore::create();
        auto g = c->openRoot();
        auto t = r.upload(*c, g, *h, *r.device());
        CHECK(c->drain());
        auto b = observe(*c, t);
        retire(*c, g);
        return b;
      };
      for (auto dims : std::array<std::array<unsigned, 3>, 5>{
               {{1, 1, 1}, {1, 7, 9}, {3, 5, 7}, {17, 65, 9}, {16, 64, 16}}})
        for (unsigned pattern = 0; pattern < 4; ++pattern) {
          std::vector<std::uint32_t> a(dims[0] * dims[1]), b(dims[1] * dims[2]);
          constexpr std::array<std::uint32_t, 12> corners{
              0,          0x80000000, 1,          0x80000001, 0x7f7fffff, 0xff7fffff,
              0x7f800000, 0xff800000, 0x7fc00001, 0xffc00031, 0x3f800001, 0xbf800001};
          auto fill = [&](auto &v, unsigned salt) {
            for (std::size_t j = 0; j < v.size(); ++j) {
              if (!pattern)
                v[j] = corners[(j * 7 + salt) % corners.size()];
              else {
                float n = float(int((j * 37 + salt) % 43) - 21);
                float q = pattern == 1   ? n / 7.f
                          : pattern == 2 ? n * 0x1p-140f
                                         : n * 1.00000011920928955078125f;
                v[j] = std::bit_cast<std::uint32_t>(q);
              }
            }
          };
          fill(a, 3);
          fill(b, 11);
          {
            auto left = uploadBits(a, {dims[0], dims[1]}),
                 right = uploadBits(b, {dims[1], dims[2]});
            for (unsigned post = 0; post < 3; ++post) {
              using K = m::NumericInstructionKind;
              std::array<std::uint32_t, 2> operands{0, 1};
              std::array<std::uint32_t, 1> produced{2};
              std::array<std::uint64_t, 1> factor{post == 1 ? 0x80000000u : 0x3f000001u};
              std::array<m::NumericInstruction, 2> code{
                  {{K::Matmul, *matrix, operands, {}, 2},
                   {K::Map, post == 2 ? *relu : *scale, produced,
                    post == 2 ? std::span<const std::uint64_t>{} : factor, 3, 1}}};
              auto c = TaskCore::create();
              auto g = c->openRoot();
              auto old = r.numericProgram(*c, g, std::array{left, right}, code, 3, f32, false);
              CHECK(c->drain() && c->failure(old).kind == TaskFailure::None);
              auto expected = download(r, *c, g, observe(*c, old));
              retire(*c, g);
              auto plan = r.prepareNumericProgram(2, code, 3, f32, false);
              CHECK(plan);
              c = TaskCore::create();
              g = c->openRoot();
              auto before = r.stats();
              auto t = r.executeNumericProgram(*c, g, *plan, std::array{left, right});
              CHECK(c->drain() && c->failure(t).kind == TaskFailure::None);
              CHECK(r.stats().dispatches - before.dispatches == 1);
              CHECK(download(r, *c, g, observe(*c, t)) == expected);
              retire(*c, g);
            }
          }
          clean();
        }
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
        r.holdCompletionAt(start + boundary + 1);
        auto t = prepared.launch(r, *c, g, input, transformer::epsilon, data.scale);
        until(*c, [&] {
          return r.stats().submissions == start + boundary + 1 && c->stats().externalPending == 1;
        });
        auto held = r.stats();
        CHECK(held.inFlight == 1 && held.stagingBuffers == (preencoded ? 16 : 4) &&
              held.bytes > baseline.bytes);
        CHECK(r.progressionCommandReservations() == (preencoded ? 15 : 0));
        if (scenario < 2)
          CHECK(c->cancel(g, scenario == 1));
        else
          r.failNextCompletion();
        CHECK(r.stats().bytes == held.bytes && c->stats().externalPending == 1);
        r.holdCompletionAt(0);
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
    std::printf("prepared: %llu allocation checkpoints, %llu commands/14 dispatches, %llu held "
                "failure boundaries, changing captures and 8 concurrent calls passed\n",
                (unsigned long long)attempts, (unsigned long long)commands,
                (unsigned long long)(commands * 3));
  }
  auto s = r.stats();
  CHECK(!s.bytes && !s.argumentBytes && !s.argumentPacks && !s.metalBuffers && !s.hostBackings &&
        !s.deviceBackings);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  test(argv[1], UINT64_C(512) * 1024 * 1024, true, false);
  test(argv[1], UINT64_C(512) * 1024 * 1024, true, true);
  test(argv[1], UINT64_C(512) * 1024 * 1024, true, true, 64, 512);
  for (auto budget : {16384u, 32768u, 65536u})
    test(argv[1], budget, false, true);
}
