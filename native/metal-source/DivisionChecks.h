// Source-generated CPU and GPU paths are compared independently to APFloat.
template <std::size_t Rank> void divisionConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  const std::array<m::ElementwiseSignature, 4> signatures{{{2, 0}, {1, 1}, {1, 1}, {1, 1}}};
  std::array<m::Kernel, 4> kernels;
  for (unsigned op = 0; op < kernels.size(); ++op) {
    auto path =
        std::string(directory) + "/kernel" + std::to_string((Rank == 2 ? 0 : 4) + op) + ".metallib";
    auto kernel = op == 0 ? runtime.loadElementwiseKernel(path, *device)
                          : runtime.loadElementwiseKernel(path, *device, signatures[op]);
    CHECK(kernel);
    kernels[op] = *kernel;
  }
  auto corpus = float_proof::corpus();
  auto directed = float_proof::divisionCases();
  corpus.insert(corpus.end(), directed.begin(), directed.end());
  std::uint64_t cases = 0, comparisons = 0;
  auto run = [&](std::span<const float_proof::Pair> pairs, std::int64_t rows, std::int64_t columns,
                 std::uint32_t biasBits, bool alias = false) {
    Invocation context;
    const auto n = pairs.size();
    auto *a = static_cast<float *>(context.allocate(n * 4));
    auto *b = static_cast<float *>(context.allocate(n * 4));
    CHECK(a && b && std::size_t(rows * columns) == n);
    for (std::size_t i = 0; i < n; ++i) {
      std::memcpy(a + i, &pairs[i].a, 4);
      std::memcpy(b + i, alias ? &pairs[i].a : &pairs[i].b, 4);
    }
    Float32Tensor<Rank> left{}, right{};
    left.allocated = left.aligned = a;
    right.allocated = right.aligned = b;
    if constexpr (Rank == 1) {
      left.sizes[0] = right.sizes[0] = n;
      left.strides[0] = right.strides[0] = 1;
    } else {
      left.sizes[0] = right.sizes[0] = rows;
      left.sizes[1] = right.sizes[1] = columns;
      left.strides[0] = right.strides[0] = columns;
      left.strides[1] = right.strides[1] = 1;
    }
    auto ha = runtime.fromCpu(context, left.view()), hb = runtime.fromCpu(context, right.view());
    CHECK(ha && hb);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto ua = runtime.upload(*core, group, *ha, *device),
         ub = runtime.upload(*core, group, *hb, *device);
    CHECK(ua && ub && core->drain());
    auto da = observe(*core, ua), db = alias ? da : observe(*core, ub);
    for (unsigned op = 0; op < kernels.size(); ++op) {
      const auto bias = std::bit_cast<float>(biasBits);
      Float32Tensor<Rank> cpu{};
      if constexpr (Rank == 2) {
        if (op == 0)
          _mlir_ciface_kalandra_cpu_1(&cpu, &left, &right, &context);
        if (op == 1)
          _mlir_ciface_kalandra_cpu_2(&cpu, &left, bias, &context);
        if (op == 2)
          _mlir_ciface_kalandra_cpu_3(&cpu, bias, &left, &context);
        if (op == 3)
          _mlir_ciface_kalandra_cpu_4(&cpu, &left, bias, &context);
      } else {
        if (op == 0)
          _mlir_ciface_kalandra_cpu_5(&cpu, &left, &right, &context);
        if (op == 1)
          _mlir_ciface_kalandra_cpu_6(&cpu, &left, bias, &context);
        if (op == 2)
          _mlir_ciface_kalandra_cpu_7(&cpu, bias, &left, &context);
        if (op == 3)
          _mlir_ciface_kalandra_cpu_8(&cpu, &left, bias, &context);
      }
      CHECK(!context.status());
      for (std::size_t axis = 0; axis < Rank; ++axis)
        CHECK(cpu.sizes[axis] == left.sizes[axis]);
      std::array<m::Buffer, 2> tensors{da, db};
      std::array<float, 1> scalars{bias};
      auto before = runtime.stats().submissions;
      auto work = runtime.elementwise(*core, group, kernels[op],
                                      std::span(tensors).first(signatures[op].tensors),
                                      std::span(scalars).first(signatures[op].scalars));
      tensors.fill({});
      scalars.fill(0); // The command owns captures, including a negated helper argument.
      CHECK(work && core->drain());
      CHECK(runtime.stats().submissions == before + (n ? 1 : 0));
      auto result = observe(*core, work), cached = observe(*core, work);
      CHECK(result.onDevice() && result.shape().size() == Rank &&
            std::equal(cached.shape().begin(), cached.shape().end(), result.shape().begin(),
                       result.shape().end()));
      for (std::size_t axis = 0; axis < Rank; ++axis)
        CHECK(result.shape()[axis] == left.sizes[axis]);
      auto down = runtime.download(*core, group, result);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes && bytes->size() == n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        const auto rhs = alias ? pairs[i].a : pairs[i].b;
        auto expected = float_proof::oracle(op == 0   ? float_proof::Pair{pairs[i].a, rhs}
                                            : op == 2 ? float_proof::Pair{biasBits, pairs[i].a}
                                                      : float_proof::Pair{pairs[i].a, biasBits},
                                            5);
        if (op == 3)
          expected =
              float_proof::oracle({pairs[i].a, float_proof::oracle({0x3f800000u, biasBits}, 5)}, 1);
        std::uint32_t cb, gb;
        std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
        std::memcpy(&gb, bytes->data() + i * 4, 4);
        CHECK(float_proof::equal(cb, expected) && float_proof::equal(gb, expected));
        CHECK(std::memcmp(a + i, &pairs[i].a, 4) == 0);
        CHECK(std::memcmp(b + i, &rhs, 4) == 0);
        ++comparisons;
      }
      context.release(cpu.allocated);
      ++cases;
    }
    // Device input bytes, including aliased inputs, remain unchanged.
    for (unsigned which = 0; which < 2; ++which) {
      auto down = runtime.download(*core, group, which ? db : da);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      for (std::size_t i = 0; i < n; ++i) {
        const auto expected = which && !alias ? pairs[i].b : pairs[i].a;
        CHECK(std::memcmp(host.hostBytes()->data() + 4 * i, &expected, 4) == 0);
      }
    }
    retire(*core, group);
    context.release(a);
    context.release(b);
    CHECK(!context.live() && context.allocations() == context.releases());
  };
  for (auto shape : {std::array{0, 0}, std::array{0, 7}, std::array{7, 0}, std::array{1, 1},
                     std::array{1, 65}, std::array{65, 1}, std::array{2, 3}, std::array{3, 21},
                     std::array{8, 8}, std::array{5, 13}, std::array{257, 513}})
    run(std::span(corpus).first(shape[0] * shape[1]), shape[0], shape[1], 0x80000000u);
  // Both scalar/tensor orders with zeros, subnormals, extrema, infinities and NaN payloads.
  for (auto bias : {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x00800000u, 0x3f800001u,
                    0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc01234u, 0xff801234u})
    run(std::span(corpus).first(4097), 1, 4097, bias);
  run(std::span(corpus).first(65), 5, 13, 0u, true);
  for (std::size_t i = 0; i < corpus.size(); i += 8193) {
    const auto n = std::min<std::size_t>(8193, corpus.size() - i);
    run(std::span(corpus).subspan(i, n), n % 3 ? 1 : 3, n % 3 ? n : n / 3, corpus[i].b);
  }
  empty(runtime);
  std::printf("Division rank-%zu: %llu cases, %llu CPU/GPU/APFloat lane comparisons passed\n", Rank,
              (unsigned long long)cases, (unsigned long long)comparisons);
}
