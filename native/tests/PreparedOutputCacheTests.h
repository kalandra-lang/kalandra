// Included after the common prepared-program ownership/readback helpers.
#include <atomic>
struct PreparedOutputCacheFixture {
  m::Options options;
  std::unique_ptr<m::Runtime> runtime;
  m::Kernel add;
  std::array<std::array<std::uint32_t, 2>, 5> operands{};
  std::array<m::NumericInstruction, 5> code{};
  m::NumericExecutable plan;
  PreparedOutputCacheFixture(const char *path, m::Options config) : options(config) {
    auto made = m::Runtime::create(config); CHECK(made); runtime = std::move(*made);
    constexpr auto f32 = kalandra::NumericType::Float32;
    auto kernel = r().loadNumericGraphKernel(std::string(path) + "/kernel0-stage5.metallib",
        *r().device(), f32, {2, 0}, std::array{f32, f32}, {},
        std::array<std::uint32_t, 2>{0, 1}, std::array<std::uint8_t, 2>{0, 0});
    CHECK(kernel); add = *kernel;
    for (unsigned i = 0; i < code.size(); ++i) {
      operands[i] = {i, i};
      code[i] = {m::NumericInstructionKind::Map, add, operands[i], {}, i + 1, 1, 96};
    }
    auto prepared = r().prepareNumericProgram(1, code, 5, f32, false);
    CHECK(prepared); plan = *prepared;
  }
  PreparedOutputCacheFixture(const PreparedOutputCacheFixture &) = delete;
  PreparedOutputCacheFixture &operator=(const PreparedOutputCacheFixture &) = delete;
  m::Runtime &r() { return *runtime; }
  void check() {
    const auto s = r().stats();
    CHECK(s.retainedBytes == s.bytes + s.cachedUploadBytes + s.cachedPreparedOutputBytes);
    CHECK(s.cachedUploadBytes + s.cachedPreparedOutputBytes <= options.maxCachedDeviceBytes);
    CHECK(s.cachedUploadBytes <= options.maxCachedUploadBytes);
    CHECK(s.cachedUploadBuffers + s.cachedPreparedOutputBuffers <= 64);
    CHECK(s.peakRetainedBytes <= options.maxBytes);
  }
  m::Buffer upload(const std::vector<std::uint32_t> &bits, std::span<const std::int64_t> shape) {
    auto core = TaskCore::create(); auto group = core->openRoot();
    auto host = r().hostTensor(std::as_bytes(std::span(bits)), shape, m::Format::Float32);
    CHECK(host);
    auto task = r().upload(*core, group, *host, *r().device());
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::None);
    auto result = observe(*core, task); retire(*core, group); return result;
  }
  m::Buffer run(m::Buffer input) {
    auto core = TaskCore::create(); auto group = core->openRoot();
    auto task = r().executeNumericProgram(*core, group, plan, std::array{input});
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::None);
    auto result = observe(*core, task); retire(*core, group); return result;
  }
  std::vector<std::uint32_t> read(m::Buffer buffer) {
    auto core = TaskCore::create(); auto group = core->openRoot();
    auto result = download(r(), *core, group, buffer); retire(*core, group); return result;
  }
};
static void preparedOutputCache(const char *path) {
  m::Options options; options.maxBytes = 1024 * 1024;
  options.maxCachedDeviceBytes = 4096; options.maxCachedUploadBytes = 4096;
  PreparedOutputCacheFixture f(path, options);
  constexpr std::array<std::uint32_t, 12> edge{
      0, 0x80000000, 1, 0x80000001, 0x7f7fffff, 0xff7fffff,
      0x7f800000, 0xff800000, 0x7fc00001, 0xffc00031, 0x3f800001, 0xbf800001};
  std::vector<std::uint32_t> a(24), b(24);
  for (unsigned i = 0; i < 24; ++i) { a[i] = edge[i % edge.size()]; b[i] = std::bit_cast<std::uint32_t>(float(int(i) - 7)); }
  auto source = f.upload(a, std::array<std::int64_t, 1>{24});
  auto other = f.upload(b, std::array<std::int64_t, 1>{24});
  f.r().setPreparedOutputCaching(false);
  const auto expected = f.read(f.run(source)), different = f.read(f.run(other));
  CHECK(!f.r().stats().cachedPreparedOutputBytes);
  f.r().setPreparedOutputCaching(true);
  // An escaped alias pins backing even after another owner is dropped.
  auto first = f.run(source), alias = first; first = {};
  const auto hits = f.r().stats().preparedOutputCacheHits;
  { auto second = f.run(other); CHECK(f.read(second) == different); }
  CHECK(f.r().stats().preparedOutputCacheHits == hits && f.read(alias) == expected);
  alias = {}; CHECK(f.r().stats().cachedPreparedOutputBytes);
  // Warm the exact-size pool with opposite bits, including NaNs/negative zero;
  // every new tensor gets fresh metadata even when ranks change.
  for (const auto &shape : std::vector<std::vector<std::int64_t>>{{24}, {2, 3, 4}, {1, 2, 1, 3, 1, 1, 1, 4}}) {
    { auto warm = f.run(other); }
    auto input = f.upload(a, shape); auto result = f.run(input);
    CHECK(std::equal(result.shape().begin(), result.shape().end(), shape.begin(), shape.end()));
    CHECK(f.read(result) == expected && f.read(input) == a); f.check();
  }
  CHECK(f.r().stats().preparedOutputCacheHits > hits);
  // Shared uploads cannot take private entries of the same physical length.
  const auto privateBytes = f.r().stats().cachedPreparedOutputBytes;
  { auto input = f.upload(a, std::array<std::int64_t, 1>{24}); }
  CHECK(f.r().stats().cachedPreparedOutputBytes == privateBytes);
  f.r().trimUploadCache(); CHECK(!f.r().stats().cachedUploadBytes && f.r().stats().cachedPreparedOutputBytes == privateBytes);
  f.r().trimDeviceCache(); CHECK(!f.r().stats().cachedPreparedOutputBytes);
  { auto input = f.upload({}, std::array<std::int64_t, 2>{0, 3}); auto result = f.run(input); CHECK(!result.bytes()); }
  CHECK(!f.r().stats().cachedPreparedOutputBytes);
  // Idle entries never prevent an otherwise satisfiable live reservation.
  { auto warm = f.run(other); }
  const auto live = f.r().stats().bytes; CHECK(live <= options.maxBytes);
  {
    const std::int64_t size = options.maxBytes - live;
    std::vector<std::byte> bytes(size, std::byte{0x35});
    auto value = f.r().hostTensor(bytes, {&size, 1}, m::Format::UInt8);
    CHECK(value && f.r().stats().bytes == options.maxBytes);
    CHECK(!f.r().stats().cachedPreparedOutputBytes && !f.r().stats().cachedUploadBytes); f.check();
  }
  // Cache hits preserve logical allocation/failure checkpoints.
  f.r().setPreparedOutputCaching(false); f.r().failAllocationAt(0);
  { auto result = f.run(source); }
  const auto cold = f.r().stats().attempts;
  f.r().setPreparedOutputCaching(true); { auto warm = f.run(other); }
  f.r().failAllocationAt(0); { auto result = f.run(source); }
  const auto attempts = f.r().stats().attempts; CHECK(attempts == cold);
  for (std::uint64_t point = 1; point <= attempts; ++point) {
    f.r().failAllocationAt(0); { auto warm = f.run(other); }
    f.r().failAllocationAt(point);
    auto core = TaskCore::create(); auto group = core->openRoot();
    auto task = f.r().executeNumericProgram(*core, group, f.plan, std::array{source});
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::Fault);
    CHECK(core->failure(task).code == std::int64_t(m::Error::Allocation));
    f.r().failAllocationAt(0); retire(*core, group); f.check();
  }
  // A pending acknowledgement cannot populate the idle pool. Policy changes
  // affect retirement without invalidating pending owners or fault precedence.
  for (unsigned mode = 0; mode < 3; ++mode) {
    f.r().trimDeviceCache(); f.r().holdCompletions(true);
    auto core = TaskCore::create(); auto group = core->openRoot();
    auto task = f.r().executeNumericProgram(*core, group, f.plan, std::array{source});
    until(*core, [&] { return core->stats().externalPending != 0; });
    CHECK(f.r().stats().inFlight && !f.r().stats().cachedPreparedOutputBytes);
    f.r().setPreparedOutputCaching(false); f.r().trimDeviceCache();
    if (mode == 2) f.r().failNextCompletion();
    else CHECK(core->cancel(group, mode == 1));
    f.r().holdCompletions(false); CHECK(core->drain());
    CHECK(core->failure(task).kind == (mode == 2 ? TaskFailure::Fault : TaskFailure::Cancelled));
    if (mode == 2) CHECK(core->failure(task).code == std::int64_t(m::Error::Device));
    retire(*core, group); CHECK(!f.r().stats().cachedPreparedOutputBytes);
    f.r().setPreparedOutputCaching(true); CHECK(f.read(f.run(source)) == expected); f.check();
  }
  // Concurrent cores, trimming and policy changes share one bounded ledger.
  std::array<std::jthread, 4> threads;
  for (unsigned t = 0; t < threads.size(); ++t) threads[t] = std::jthread([&, t] {
    for (unsigned i = 0; i < 12; ++i) {
      auto result = f.run(i % 2 ? source : other);
      CHECK(f.read(result) == (i % 2 ? expected : different));
      if (t == 0) { f.r().setPreparedOutputCaching(i % 2); f.r().trimDeviceCache(); }
    }
  });
  for (auto &thread : threads) thread.join();
  f.r().setPreparedOutputCaching(true); f.check();
  // Entry and byte caps include both kinds. Disable shared retention so this
  // reaches the 64-entry bound using only eligible prepared outputs.
  {
    auto limits = options; limits.maxCachedDeviceBytes = 65536; limits.maxCachedUploadBytes = 0;
    PreparedOutputCacheFixture capped(path, limits);
    for (std::int64_t n = 1; n <= 70; ++n) {
      auto input = capped.upload(std::vector<std::uint32_t>(n, 0x3f800000), {&n, 1});
      { auto result = capped.run(input); }
      capped.check();
    }
    CHECK(capped.r().stats().cachedPreparedOutputBuffers == 64 && !capped.r().stats().cachedUploadBytes);
    capped.r().trimDeviceCache(); CHECK(!capped.r().stats().cachedPreparedOutputBytes);
  }
  for (bool disableAll : {false, true}) {
    auto limits = options;
    if (disableAll) limits.maxCachedDeviceBytes = 0;
    else limits.cachePreparedOutputs = false;
    PreparedOutputCacheFixture disabled(path, limits);
    auto input = disabled.upload(a, std::array<std::int64_t, 1>{24});
    { auto result = disabled.run(input); CHECK(disabled.read(result) == expected); }
    CHECK(!disabled.r().stats().cachedPreparedOutputBytes && !disabled.r().stats().preparedOutputCacheHits);
  }
  // A private miss must not evict a reusable upload. Combined pressure prefers
  // private entries, preserving distinct upload sizes that fit their subquota.
  for (std::uint64_t cap : {128, 256}) {
    auto limits = options; limits.maxCachedDeviceBytes = cap; limits.maxCachedUploadBytes = cap;
    PreparedOutputCacheFixture priority(path, limits);
    auto input = priority.upload(a, std::array<std::int64_t, 1>{24});
    if (cap == 128) {
      { auto idle = priority.upload(b, std::array<std::int64_t, 1>{24}); }
      const auto before = priority.r().stats().uploadCacheHits;
      for (unsigned i = 0; i < 4; ++i) {
        { auto result = priority.run(input); CHECK(priority.read(result) == expected); }
        CHECK(priority.r().stats().cachedUploadBytes == 96 && !priority.r().stats().cachedPreparedOutputBytes);
        { auto reuse = priority.upload(b, std::array<std::int64_t, 1>{24}); }
        priority.check();
      }
      CHECK(priority.r().stats().uploadCacheHits == before + 4);
    } else {
      { auto first = priority.run(input); auto second = priority.run(input); }
      CHECK(priority.r().stats().cachedPreparedOutputBytes == 192);
      { auto idle = priority.upload(b, std::array<std::int64_t, 1>{24}); }
      CHECK(priority.r().stats().cachedUploadBytes == 96 && priority.r().stats().cachedPreparedOutputBytes == 96);
      { auto idle = priority.upload(std::vector<std::uint32_t>(40, 0x3f800000), std::array<std::int64_t, 1>{40}); }
      CHECK(priority.r().stats().cachedUploadBytes == 256 && !priority.r().stats().cachedPreparedOutputBytes);
      const auto before = priority.r().stats().uploadCacheHits;
      { auto reuse = priority.upload(b, std::array<std::int64_t, 1>{24}); }
      { auto reuse = priority.upload(std::vector<std::uint32_t>(40, 0x3f800000), std::array<std::int64_t, 1>{40}); }
      CHECK(priority.r().stats().uploadCacheHits == before + 2); priority.check();
    }
  }
  // Destruction closes the pool even while escaped values retain registry state.
  {
    PreparedOutputCacheFixture ending(path, options);
    auto input = ending.upload(a, std::array<std::int64_t, 1>{24});
    auto escaped = ending.run(input); { auto idle = ending.run(input); }
    CHECK(ending.r().stats().cachedPreparedOutputBytes);
    auto observer = *ending.runtime; ending.runtime.reset();
    CHECK(!observer.stats().cachedPreparedOutputBytes);
    escaped = {}; CHECK(!observer.stats().cachedPreparedOutputBytes);
  }
  std::puts("Prepared output cache: aliases, overwrite, rank, type, budget, failures, ACK, concurrency, caps and teardown passed");
}
