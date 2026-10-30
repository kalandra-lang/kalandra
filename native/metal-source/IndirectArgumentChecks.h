// Both ranks share lane arithmetic; all forty resources contribute, with aliases.
template <std::size_t Rank> void indirectConformance(m::Runtime &runtime, const char *directory) {
  constexpr auto Other = Rank == 1 ? 2 : 1;
  auto device = runtime.device();
  CHECK(device);
  std::array<std::uint32_t, 40> reads{};
  for (unsigned i = 0; i < 40; ++i)
    reads[i] = i + 1;
  auto kernel = runtime.loadElementwiseKernel(
      std::string(directory) + (Rank == 1 ? "/kernel0.metallib" : "/kernel1.metallib"), *device,
      {41, 2, m::ElementwiseBinding::ArgumentBuffer}, reads);
  CHECK(kernel);
  reads.fill(0); // Kernel owns its mapping.
  auto corpus = float_proof::corpus();
  std::uint64_t cases = 0, lanes = 0;
  auto run = [&](std::size_t n, std::uint32_t scaleBits = 0x3f000001u,
                 std::uint32_t biasBits = 0xbe800001u) {
    Invocation context;
    std::array<float *, 2> data{};
    std::array<Float32Tensor<Rank>, 2> views{};
    std::array<m::Buffer, 2> hosts{}, buffers{};
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    const auto rows = n % 7 == 0 ? 7 : n % 5 == 0 ? 5 : 1;
    for (unsigned t = 0; t < 2; ++t) {
      data[t] = static_cast<float *>(context.allocate(n * 4));
      CHECK(data[t]);
      for (std::size_t i = 0; i < n; ++i) {
        const auto bits = t ? corpus[i].b : corpus[i].a;
        std::memcpy(data[t] + i, &bits, 4);
      }
      views[t].allocated = views[t].aligned = data[t];
      if constexpr (Rank == 1) {
        views[t].sizes[0] = n;
        views[t].strides[0] = 1;
      } else {
        views[t].sizes[0] = rows;
        views[t].sizes[1] = n / rows;
        views[t].strides[0] = n / rows;
        views[t].strides[1] = 1;
      }
      auto host = runtime.fromCpu(context, views[t].view());
      CHECK(host);
      hosts[t] = *host;
      auto up = runtime.upload(*core, group, *host, *device);
      CHECK(up && core->drain());
      buffers[t] = observe(*core, up);
    }
    auto *junk = static_cast<float *>(context.allocate(8));
    CHECK(junk);
    junk[0] = 7;
    junk[1] = 8;
    Float32Tensor<Other> ignored{};
    ignored.allocated = ignored.aligned = junk;
    if constexpr (Other == 1) {
      ignored.sizes[0] = 2;
      ignored.strides[0] = 1;
    } else {
      ignored.sizes[0] = 2;
      ignored.sizes[1] = 1;
      ignored.strides[0] = 1;
      ignored.strides[1] = 1;
    }
    auto host = runtime.fromCpu(context, ignored.view());
    CHECK(host);
    auto up = runtime.upload(*core, group, *host, *device);
    CHECK(up && core->drain());
    std::vector<m::Buffer> arguments(41);
    arguments[0] = observe(*core, up);
    for (unsigned i = 0; i < 40; ++i)
      arguments[i + 1] = buffers[i % 2];
    std::array<float, 2> scalars{std::bit_cast<float>(scaleBits), std::bit_cast<float>(biasBits)};
    Float32Tensor<Rank> cpu{};
    if constexpr (Rank == 1)
      _mlir_ciface_kalandra_cpu_0(
          &cpu, &ignored, &views[0], scalars[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], scalars[1], &context);
    else
      _mlir_ciface_kalandra_cpu_1(
          &cpu, &ignored, &views[0], scalars[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], &views[0], &views[1], &views[0], &views[1], &views[0],
          &views[1], &views[0], &views[1], scalars[1], &context);
    CHECK(!context.status());
    auto work = runtime.elementwise(*core, group, *kernel, arguments, scalars);
    arguments.clear();
    scalars.fill(0);
    CHECK(work && core->drain());
    auto result = observe(*core, work);
    CHECK(result.onDevice() && result.shape().size() == Rank);
    for (std::size_t i = 0; i < Rank; ++i)
      CHECK(result.shape()[i] == views[0].sizes[i]);
    auto down = runtime.download(*core, group, result);
    CHECK(down && core->drain());
    auto gpu = observe(*core, down);
    auto bytes = gpu.hostBytes();
    CHECK(bytes && bytes->size() == n * 4);
    for (std::size_t i = 0; i < n; ++i) {
      auto expected = corpus[i].a;
      for (unsigned t = 1; t < 40; ++t)
        expected = float_proof::oracle({expected, t % 2 ? corpus[i].b : corpus[i].a}, 0);
      expected = float_proof::oracle({expected, scaleBits}, 1);
      expected = float_proof::oracle({expected, biasBits}, 0);
      std::uint32_t cb, gb;
      std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
      std::memcpy(&gb, bytes->data() + 4 * i, 4);
      CHECK(float_proof::equal(expected, cb) && float_proof::equal(expected, gb));
      CHECK(std::memcmp(data[0] + i, &corpus[i].a, 4) == 0);
      CHECK(std::memcmp(data[1] + i, &corpus[i].b, 4) == 0);
    }
    ++cases;
    lanes += n;
    context.release(cpu.allocated);
    context.release(data[0]);
    context.release(data[1]);
    context.release(junk);
    retire(*core, group);
  };
  for (auto n :
       {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(6),
        std::size_t(63), std::size_t(64), std::size_t(65), std::size_t(127), std::size_t(129),
        std::size_t(1025), std::size_t(65537), std::size_t(131073), corpus.size()}) {
    run(n);
    empty(runtime);
  }
  for (auto edge : {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu,
                    0x7f800000u, 0xff800000u, 0x7fc01234u, 0xff801234u}) {
    run(65, edge, 0x80000000u);
    empty(runtime);
  }
  std::printf("Indirect rank-%zu: %llu cases, %llu CPU/GPU/APFloat lanes passed\n", Rank,
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(lanes));
}

void indirectLifecycle(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  std::array<std::uint32_t, 40> reads{};
  for (unsigned i = 0; i < 40; ++i)
    reads[i] = i + 1;
  const auto path = std::string(directory) + "/kernel0.metallib";
  const m::ElementwiseSignature signature{41, 2, m::ElementwiseBinding::ArgumentBuffer};
  auto kernel = runtime.loadElementwiseKernel(path, *device, signature, reads);
  CHECK(kernel);
  // A direct layout or the wrong reflected scalar/member count is rejected.
  auto wrong = runtime.loadElementwiseKernel(path, *device, {41, 2}, reads);
  CHECK(!wrong && wrong.error() == m::Error::Invalid);
  wrong = runtime.loadElementwiseKernel(path, *device,
                                        {41, 1, m::ElementwiseBinding::ArgumentBuffer}, reads);
  CHECK(!wrong && wrong.error() == m::Error::Invalid);
  auto invalid = reads;
  invalid[39] = 41;
  wrong = runtime.loadElementwiseKernel(path, *device, signature, invalid);
  CHECK(!wrong && wrong.error() == m::Error::Invalid);
  const std::array<float, 4> values{1, 2, 3, 4};
  auto host = runtime.hostTensor(std::as_bytes(std::span(values)), std::array<std::int64_t, 1>{4},
                                 m::Format::Float32);
  CHECK(host);
  m::Buffer input;
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto up = runtime.upload(*core, group, *host, *device);
    CHECK(up && core->drain());
    input = observe(*core, up);
    retire(*core, group);
  }
  host = {};
  const auto baseline = runtime.stats().bytes;
  CHECK(baseline == 16);
  constexpr auto captured = 41 * sizeof(m::Buffer) + 2 * sizeof(std::uint32_t);
  auto unchanged = [&] {
    auto stats = runtime.stats();
    CHECK(stats.bytes == baseline && stats.metalBuffers == 1 && !stats.stagingBuffers &&
          !stats.inFlight && !stats.argumentBytes && !stats.argumentPacks);
  };
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    std::vector<m::Buffer> args(41, input);
    auto work = runtime.elementwise(*core, group, *kernel, args, std::array<float, 2>{0.5, 1});
    args.clear();
    CHECK(work && core->drain());
    CHECK((core->failure(work).kind == TaskFailure::None) == success);
    if (!success)
      CHECK(core->failure(work).code == 2);
    retire(*core, group);
    unchanged();
  };
  runtime.failAllocationAt(0);
  run(true);
  const auto attempts = runtime.stats().attempts;
  CHECK(attempts > 10);
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    runtime.failAllocationAt(i);
    run(false);
  }
  runtime.failAllocationAt(0);
  std::uint64_t residentBytes = 0;
  for (unsigned scenario : {0u, 1u, 2u, 3u}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    if (scenario == 2)
      runtime.failNextCompletion();
    std::vector<m::Buffer> args(41, input);
    auto work = runtime.elementwise(*core, group, *kernel, args, std::array<float, 2>{0.5, 1});
    args.clear();
    CHECK(work);
    until(*core, [&] { return core->stats().externalPending == 1; });
    const auto held = runtime.stats();
    CHECK(held.argumentBytes == captured && held.argumentPacks == 1 && held.stagingBuffers == 2);
    residentBytes = held.bytes;
    CHECK(residentBytes > baseline + 16 + 12); // Additional argument buffer is charged.
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    CHECK(runtime.stats().stagingBuffers == 2);
    runtime.holdCompletions(false);
    CHECK(core->drain());
    CHECK((core->failure(work).kind == TaskFailure::None) == (scenario == 3));
    if (scenario == 3)
      CHECK(!runtime.stats().argumentPacks && !runtime.stats().stagingBuffers);
    retire(*core, group);
    unchanged();
  }
  // Concurrent calls share a kernel but never an encoder destination or argument buffer.
  {
    auto a = TaskCore::create(), b = TaskCore::create();
    CHECK(a && b);
    auto ga = a->openRoot(), gb = b->openRoot();
    std::vector<m::Buffer> args(41, input);
    runtime.holdCompletions(true);
    auto first = runtime.elementwise(*a, ga, *kernel, args, std::array<float, 2>{0.5, 1});
    auto second = runtime.elementwise(*b, gb, *kernel, args, std::array<float, 2>{1, 2});
    CHECK(first && second);
    until(*a, [&] { return a->stats().externalPending == 1; });
    until(*b, [&] { return b->stats().externalPending == 1; });
    CHECK(runtime.stats().argumentPacks == 2 && runtime.stats().stagingBuffers == 4);
    CHECK(a->cancel(ga));
    runtime.holdCompletions(false);
    CHECK(a->drain() && b->drain());
    CHECK(a->failure(first).kind != TaskFailure::None &&
          b->failure(second).kind == TaskFailure::None);
    auto result = observe(*b, second);
    auto down = runtime.download(*b, gb, result);
    CHECK(down && b->drain());
    auto value = observe(*b, down);
    auto bytes = value.hostBytes();
    CHECK(bytes);
    for (unsigned i = 0; i < 4; ++i) {
      float actual;
      std::memcpy(&actual, bytes->data() + 4 * i, 4);
      CHECK(actual == 40 * values[i] + 2);
    }
    retire(*a, ga);
    retire(*b, gb);
  }
  unchanged();
  // Destruction must wait for acknowledgement before releasing indirect resources.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    std::vector<m::Buffer> args(41, input);
    runtime.holdCompletions(true);
    auto work = runtime.elementwise(*core, group, *kernel, args, std::array<float, 2>{0.5, 1});
    CHECK(work);
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::jthread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      runtime.holdCompletions(false);
    });
    core.reset();
  }
  unchanged();
  // Requested argument-buffer bytes participate in the normal device byte budget.
  for (unsigned budget : {0u, 1u, 2u}) {
    const bool enough = budget == 0;
    auto limited = m::Runtime::create({.maxBytes = residentBytes - (budget == 1 ? 1 : 0),
                                       .maxArgumentBytes = captured - (budget == 2 ? 1 : 0)});
    CHECK(limited);
    auto selected = (*limited)->device();
    CHECK(selected);
    auto entry = (*limited)->loadElementwiseKernel(path, *selected, signature, reads);
    CHECK(entry);
    auto h = (*limited)->hostTensor(std::as_bytes(std::span(values)),
                                    std::array<std::int64_t, 1>{4}, m::Format::Float32);
    CHECK(h);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto up = (*limited)->upload(*core, group, *h, *selected);
    CHECK(up && core->drain());
    auto buffer = observe(*core, up);
    retire(*core, group);
    h = {};
    group = core->openRoot();
    std::vector<m::Buffer> args(41, buffer);
    auto work = (*limited)->elementwise(*core, group, *entry, args, std::array<float, 2>{0.5, 1});
    CHECK(work && core->drain());
    CHECK((core->failure(work).kind == TaskFailure::None) == enough);
    if (!enough)
      CHECK(core->failure(work).code == std::int64_t(m::Error::Budget));
    retire(*core, group);
    args.clear();
    buffer = {};
    empty(**limited);
  }
  input = {};
  empty(runtime);
  std::printf("Indirect capture/dispatch checkpoints: %llu; resident budget %llu bytes; "
              "concurrency and acknowledgement passed\n",
              static_cast<unsigned long long>(attempts),
              static_cast<unsigned long long>(residentBytes));
}
