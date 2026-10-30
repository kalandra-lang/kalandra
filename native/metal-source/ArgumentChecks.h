// Included by the source harness: owned argument arrays, not borrowed caller spans.
template <std::size_t Rank> void argumentConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  auto kernel = runtime.loadElementwiseKernel(
      std::string(directory) + (Rank == 2 ? "/kernel0.metallib" : "/kernel1.metallib"), *device,
      {3, 12});
  CHECK(kernel);
  auto corpus = float_proof::corpus();
  std::uint64_t cases = 0, comparisons = 0;
  auto run = [&](std::span<const float_proof::Pair> pairs, std::int64_t rows, std::int64_t columns,
                 unsigned edge = 0, bool alias = false) {
    Invocation context;
    const auto n = pairs.size();
    std::array<float *, 3> data{};
    std::array<Float32Tensor<Rank>, 3> views{};
    std::array<m::Buffer, 3> hosts{}, devices{};
    std::vector<std::uint32_t> third;
    for (std::size_t i = 0; i < n; ++i)
      third.push_back(alias ? pairs[i].a : pairs[(i + 1) % n].b ^ 0x80000000u);
    CHECK(std::size_t(rows * columns) == n);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    for (unsigned t = 0; t < 3; ++t) {
      data[t] = static_cast<float *>(context.allocate(n * 4));
      CHECK(data[t]);
      for (std::size_t i = 0; i < n; ++i) {
        auto bits = t == 0 ? pairs[i].a : t == 1 ? (alias ? pairs[i].a : pairs[i].b) : third[i];
        std::memcpy(data[t] + i, &bits, 4);
      }
      views[t].allocated = views[t].aligned = data[t];
      if constexpr (Rank == 1) {
        views[t].sizes[0] = n;
        views[t].strides[0] = 1;
      } else {
        views[t].sizes[0] = rows;
        views[t].sizes[1] = columns;
        views[t].strides[0] = columns;
        views[t].strides[1] = 1;
      }
      auto host = runtime.fromCpu(context, views[t].view());
      CHECK(host);
      hosts[t] = *host;
      auto up = runtime.upload(*core, group, hosts[t], *device);
      CHECK(up && core->drain());
      devices[t] = alias && t ? devices[0] : observe(*core, up);
    }
    std::array<std::uint32_t, 12> bits{0x3f800001u, 0x3f000001u, 0xbe800001u, 1u,
                                       0x80000001u, 0x3e800001u, 0xbe800000u, 0x3f000000u,
                                       0xbf000001u, 0x3d800000u, 0xbd800001u, 0x80000000u};
    if (edge) {
      const std::array<std::uint32_t, 12> edges{0u,          0x80000000u, 1u,          0x80000001u,
                                                0x007fffffu, 0x00800000u, 0x3f800000u, 0x7f7fffffu,
                                                0x7f800000u, 0xff800000u, 0x7fc01234u, 0xff801234u};
      bits[(edge - 1) % 12] = edges[(edge - 1) % 12];
    }
    std::array<float, 12> scalars{};
    for (unsigned i = 0; i < 12; ++i)
      scalars[i] = std::bit_cast<float>(bits[i]);
    Float32Tensor<Rank> cpu{};
    if constexpr (Rank == 2)
      _mlir_ciface_kalandra_cpu_0(&cpu, &views[0], scalars[0], &views[1], scalars[1], &views[2],
                                  scalars[2], scalars[3], scalars[4], scalars[5], scalars[6],
                                  scalars[7], scalars[8], scalars[9], scalars[10], scalars[11],
                                  &context);
    else
      _mlir_ciface_kalandra_cpu_1(&cpu, &views[0], scalars[0], &views[1], scalars[1], &views[2],
                                  scalars[2], scalars[3], scalars[4], scalars[5], scalars[6],
                                  scalars[7], scalars[8], scalars[9], scalars[10], scalars[11],
                                  &context);
    CHECK(!context.status());
    auto work = runtime.elementwise(*core, group, *kernel, devices, scalars);
    devices.fill({});
    scalars.fill(0); // Caller captures are gone before the task executes.
    CHECK(work && core->drain());
    auto result = observe(*core, work);
    CHECK(result.onDevice() && result.shape().size() == Rank);
    for (std::size_t axis = 0; axis < Rank; ++axis)
      CHECK(result.shape()[axis] == views[0].sizes[axis] &&
            cpu.sizes[axis] == views[0].sizes[axis]);
    auto down = runtime.download(*core, group, result);
    CHECK(down && core->drain());
    auto host = observe(*core, down);
    auto bytes = host.hostBytes();
    CHECK(bytes && bytes->size() == n * 4);
    for (std::size_t i = 0; i < n; ++i) {
      auto expected = float_proof::oracle({pairs[i].a, bits[0]}, 1);
      const auto b = alias ? pairs[i].a : pairs[i].b;
      expected = float_proof::oracle({expected, float_proof::oracle({b, bits[1]}, 1)}, 0);
      expected = float_proof::oracle({expected, float_proof::oracle({third[i], bits[2]}, 1)}, 0);
      for (unsigned s = 3; s < 12; ++s)
        expected = float_proof::oracle({expected, bits[s]}, 0);
      std::uint32_t cb, gb;
      std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
      std::memcpy(&gb, bytes->data() + 4 * i, 4);
      CHECK(float_proof::equal(expected, cb) && float_proof::equal(expected, gb));
      CHECK(std::memcmp(data[0] + i, &pairs[i].a, 4) == 0);
      CHECK(std::memcmp(data[1] + i, &b, 4) == 0);
      CHECK(std::memcmp(data[2] + i, &third[i], 4) == 0);
      ++comparisons;
    }
    context.release(cpu.allocated);
    retire(*core, group);
    result = {};
    host = {};
    hosts.fill({});
    for (auto *p : data)
      context.release(p);
    CHECK(!context.live() && context.allocations() == context.releases());
    empty(runtime);
    ++cases;
  };
  for (auto shape : {std::array{0, 0}, std::array{0, 7}, std::array{7, 0}, std::array{1, 1},
                     std::array{1, 65}, std::array{65, 1}, std::array{2, 3}, std::array{3, 21},
                     std::array{8, 8}, std::array{5, 13}})
    run(std::span(corpus).first(shape[0] * shape[1]), shape[0], shape[1]);
  for (unsigned e = 1; e <= 12; ++e)
    run(std::span(corpus).first(4097), 1, 4097, e);
  run(std::span(corpus).first(65), 5, 13, 0, true);
  for (std::size_t i = 0; i < corpus.size(); i += 8193) {
    const auto n = std::min<std::size_t>(8193, corpus.size() - i);
    run(std::span(corpus).subspan(i, n), n % 3 ? 1 : 3, n % 3 ? n : n / 3);
  }
  std::printf("Arguments rank-%zu: %llu cases, %llu CPU/GPU/APFloat lane comparisons passed\n",
              Rank, (unsigned long long)cases, (unsigned long long)comparisons);
}
void argumentLifecycle(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  auto kernel =
      runtime.loadElementwiseKernel(std::string(directory) + "/kernel2.metallib", *device, {29, 0});
  auto wide =
      runtime.loadElementwiseKernel(std::string(directory) + "/kernel1.metallib", *device, {3, 12});
  CHECK(kernel && wide);
  CHECK(!runtime.loadElementwiseKernel(std::string(directory) + "/kernel2.metallib", *device,
                                       {30, 0}));
  const std::array<std::uint32_t, 7> patterns{0x3f800001u, 1u,          0x80000001u, 0x80000000u,
                                              0x7f7fffffu, 0x7fc01234u, 0xff801234u};
  std::array<std::uint32_t, 65> values{};
  for (unsigned i = 0; i < values.size(); ++i)
    values[i] = patterns[i % patterns.size()];
  const std::int64_t length = values.size();
  auto upload = [&](m::Runtime &r) {
    auto selected = r.device();
    CHECK(selected);
    auto host = r.hostTensor(std::as_bytes(std::span(values)), {&length, 1}, m::Format::Float32);
    CHECK(host);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto up = r.upload(*core, group, *host, *selected);
    CHECK(up && core->drain());
    auto result = observe(*core, up);
    retire(*core, group);
    return result;
  };
  auto input = upload(runtime);
  const auto baseline = runtime.stats().bytes;
  const std::uint64_t captured = 29 * sizeof(m::Buffer);
  auto baselineCheck = [&] {
    auto s = runtime.stats();
    CHECK(s.bytes == baseline && !s.argumentBytes && !s.argumentPacks && !s.inFlight &&
          !s.stagingBuffers);
  };
  auto verify = [&](TaskCore &core, GroupHandle group, TaskHandle task) {
    auto result = observe(core, task);
    auto down = runtime.download(core, group, result);
    CHECK(down && core.drain());
    auto host = observe(core, down);
    auto bytes = host.hostBytes();
    CHECK(bytes && bytes->size() == values.size() * 4);
    for (unsigned i = 0; i < values.size(); ++i) {
      auto expected = values[i];
      for (unsigned j = 1; j < 29; ++j)
        expected = float_proof::oracle({expected, values[i]}, 0);
      std::uint32_t actual;
      std::memcpy(&actual, bytes->data() + i * 4, 4);
      CHECK(float_proof::equal(actual, expected));
    }
  };
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    std::vector<m::Buffer> tensors(29, input);
    auto work = runtime.elementwise(*core, group, *kernel, tensors, {});
    tensors.clear();
    CHECK(work && core->drain());
    if (success)
      CHECK(core->failure(work).kind == TaskFailure::None);
    else
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
    retire(*core, group);
    baselineCheck();
  };
  runtime.failAllocationAt(0);
  run(true);
  const auto attempts = runtime.stats().attempts;
  CHECK(attempts > 4);
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    runtime.failAllocationAt(i);
    run(false);
  }
  runtime.failAllocationAt(0);
  for (unsigned scenario : {0u, 1u, 2u, 3u}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    if (scenario == 2)
      runtime.failNextCompletion();
    std::vector<m::Buffer> tensors(29, input);
    auto work = runtime.elementwise(*core, group, *kernel, tensors, {});
    tensors.clear();
    CHECK(work);
    until(*core, [&] { return core->stats().externalPending == 1; });
    CHECK(runtime.stats().argumentBytes == captured && runtime.stats().argumentPacks == 1);
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    runtime.holdCompletions(false);
    CHECK(core->drain());
    if (scenario == 3)
      verify(*core, group, work);
    else {
      Invocation context;
      alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
      CHECK(!core->observe(work, m::Runtime::bufferLayout(), context, slot));
    }
    retire(*core, group);
    baselineCheck();
  }
  // Two pending commands own independent captures; cancelling one does not revoke the other.
  {
    auto a = TaskCore::create(), b = TaskCore::create();
    CHECK(a && b);
    auto ga = a->openRoot(), gb = b->openRoot();
    std::vector<m::Buffer> tensors(29, input);
    runtime.holdCompletions(true);
    auto ta = runtime.elementwise(*a, ga, *kernel, tensors, {}),
         tb = runtime.elementwise(*b, gb, *kernel, tensors, {});
    tensors.clear();
    CHECK(ta && tb);
    until(*a, [&] { return a->stats().externalPending == 1; });
    until(*b, [&] { return b->stats().externalPending == 1; });
    CHECK(runtime.stats().argumentBytes == 2 * captured && runtime.stats().argumentPacks == 2);
    CHECK(a->cancel(ga));
    runtime.holdCompletions(false);
    CHECK(a->drain() && b->drain());
    CHECK(a->failure(ta).kind == TaskFailure::Cancelled);
    verify(*b, gb, tb);
    retire(*a, ga);
    retire(*b, gb);
    baselineCheck();
  }
  // Last tensor is validated too; errors must precede command publication.
  auto reject = [&](m::Buffer last, std::int64_t code) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    std::array<m::Buffer, 3> tensors{input, input, last};
    std::array<float, 12> scalars{};
    auto before = runtime.stats().submissions;
    auto work = runtime.elementwise(*core, group, *wide, tensors, scalars);
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == code);
    CHECK(runtime.stats().submissions == before);
    retire(*core, group);
    CHECK(!runtime.stats().argumentBytes && !runtime.stats().argumentPacks);
  };
  reject({}, 3);
  {
    auto host =
        runtime.hostTensor(std::as_bytes(std::span(values)), {&length, 1}, m::Format::Float32);
    CHECK(host);
    reject(*host, 7);
    auto foreign = m::Runtime::create();
    CHECK(foreign);
    reject(upload(**foreign), 7);
  }
  baselineCheck();
  // Exact capture admission and one-byte-below boundary, without changing tensor payload budgets.
  for (bool admitted : {false, true}) {
    auto other = m::Runtime::create({.maxArgumentBytes = captured - (admitted ? 0 : 1)});
    CHECK(other);
    auto selected = (*other)->device();
    CHECK(selected);
    auto loaded = (*other)->loadElementwiseKernel(std::string(directory) + "/kernel2.metallib",
                                                  *selected, {29, 0});
    CHECK(loaded);
    auto value = upload(**other);
    std::vector<m::Buffer> tensors(29, value);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = (*other)->elementwise(*core, group, *loaded, tensors, {});
    tensors.clear();
    CHECK(work && core->drain());
    if (admitted)
      CHECK(core->failure(work).kind == TaskFailure::None);
    else
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 6);
    retire(*core, group);
    CHECK(!(*other)->stats().argumentBytes && !(*other)->stats().argumentPacks);
    value = {};
    empty(**other);
  }
  // Core destruction cannot release captures before command acknowledgement.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    std::vector<m::Buffer> tensors(29, input);
    CHECK(runtime.elementwise(*core, group, *kernel, tensors, {}));
    tensors.clear();
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      runtime.holdCompletions(false);
    });
    core.reset();
    release.join();
    baselineCheck();
  }
  input = {};
  empty(runtime);
  std::printf("Argument capture checkpoints swept: %llu; binding boundary, budgets and "
              "cancellation passed\n",
              (unsigned long long)attempts);
}
