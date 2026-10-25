// Included by the generated-source harness after its shared lifecycle utilities.
void matrixConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  const std::array<m::ElementwiseSignature, 4> signatures{{{2, 2}, {1, 1}, {1, 0}, {2, 0}}};
  std::array<m::Kernel, 4> kernels;
  for (unsigned i = 0; i < kernels.size(); ++i) {
    auto path = std::string(directory) + "/kernel" + std::to_string(i) + ".metallib";
    auto kernel = i == 3 ? runtime.loadElementwiseKernel(path, *device)
                         : runtime.loadElementwiseKernel(path, *device, signatures[i]);
    CHECK(kernel);
    kernels[i] = *kernel;
  }
  auto corpus = float_proof::corpus();
  std::uint64_t cases = 0, comparisons = 0;
  auto run = [&](std::span<const float_proof::Pair> pairs, std::int64_t rows, std::int64_t columns,
                 bool alias = false) {
    CHECK(std::size_t(rows * columns) == pairs.size());
    Invocation context;
    auto *a = static_cast<float *>(context.allocate(pairs.size() * 4));
    auto *b = static_cast<float *>(context.allocate(pairs.size() * 4));
    CHECK(a && b);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      std::memcpy(a + i, &pairs[i].a, 4);
      std::memcpy(b + i, alias ? &pairs[i].a : &pairs[i].b, 4);
    }
    Float32Tensor<2> left{a, a, 0, {rows, columns}, {columns, 1}},
        right{b, b, 0, {rows, columns}, {columns, 1}};
    auto ha = runtime.fromCpu(context, left.view()), hb = runtime.fromCpu(context, right.view());
    CHECK(ha && hb);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto ua = runtime.upload(*core, group, *ha, *device),
         ub = runtime.upload(*core, group, *hb, *device);
    CHECK(core->drain());
    auto da = observe(*core, ua), db = alias ? da : observe(*core, ub);
    const auto factor = std::bit_cast<float>(0x3f800001u), bias = std::bit_cast<float>(0x80000001u);
    for (unsigned op = 0; op < kernels.size(); ++op) {
      Float32Tensor<2> cpu{};
      if (op == 0)
        _mlir_ciface_kalandra_cpu_2(&cpu, &left, factor, &right, bias, &context);
      if (op == 1)
        _mlir_ciface_kalandra_cpu_0(&cpu, &left, factor, &context);
      if (op == 2)
        _mlir_ciface_kalandra_cpu_3(&cpu, &left, &context);
      if (op == 3)
        _mlir_ciface_kalandra_cpu_1(&cpu, &left, &right, &context);
      CHECK(!context.status() && cpu.sizes[0] == rows && cpu.sizes[1] == columns);
      std::array<m::Buffer, 2> tensors{da, db};
      std::array<float, 2> scalars{factor, bias};
      const auto before = runtime.stats().submissions;
      auto work = runtime.elementwise(*core, group, kernels[op],
                                      std::span(tensors).first(signatures[op].tensors),
                                      std::span(scalars).first(signatures[op].scalars));
      CHECK(work);
      tensors.fill({});
      scalars.fill(0); // The queued packet owns its input/scalar captures.
      CHECK(core->drain());
      CHECK(runtime.stats().submissions == before + (pairs.empty() ? 0 : 1));
      auto result = observe(*core, work), cached = observe(*core, work);
      CHECK(result.onDevice() && result.shape().size() == 2 && result.shape()[0] == rows &&
            result.shape()[1] == columns && cached.shape()[1] == columns);
      auto down = runtime.download(*core, group, result);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes && bytes->size() == pairs.size() * 4 && host.shape()[0] == rows &&
            host.shape()[1] == columns);
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        auto rhs = alias ? pairs[i].a : pairs[i].b;
        auto expected = op == 2   ? pairs[i].a
                        : op == 3 ? float_proof::oracle({pairs[i].a, rhs}, 0)
                                  : float_proof::oracle({pairs[i].a, 0x3f800001u}, 1);
        if (op == 0) {
          expected = float_proof::oracle({expected, rhs}, 0);
          expected = float_proof::oracle({expected, 0x80000001u}, 0);
        }
        std::uint32_t cb, gb;
        std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
        std::memcpy(&gb, bytes->data() + i * 4, 4);
        CHECK(float_proof::equal(cb, expected) && float_proof::equal(gb, expected));
        if (op == 2)
          CHECK(gb == pairs[i].a); // Identity preserves NaN payloads and signed zero.
        CHECK(std::memcmp(a + i, &pairs[i].a, 4) == 0);
        CHECK(std::memcmp(b + i, &rhs, 4) == 0);
        ++comparisons;
      }
      context.release(cpu.allocated);
      ++cases;
    }
    // Neither the original device tensor nor an aliased input is overwritten.
    for (unsigned which = 0; which < 2; ++which) {
      auto down = runtime.download(*core, group, which ? db : da);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes);
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        auto expected = which && !alias ? pairs[i].b : pairs[i].a;
        CHECK(std::memcmp(bytes->data() + i * 4, &expected, 4) == 0);
      }
    }
    retire(*core, group);
    context.release(a);
    context.release(b);
    CHECK(!context.live() && context.allocations() == context.releases());
  };
  for (auto shape :
       {std::array{0, 0}, std::array{0, 7}, std::array{7, 0}, std::array{1, 1}, std::array{1, 65},
        std::array{65, 1}, std::array{2, 3}, std::array{3, 2}, std::array{3, 21}, std::array{8, 8},
        std::array{5, 13}, std::array{17, 65}, std::array{257, 513}})
    run(std::span(corpus).first(shape[0] * shape[1]), shape[0], shape[1]);
  run(std::span(corpus).first(221), 13, 17, true);
  for (std::size_t i = 0; i < corpus.size(); i += 8193) {
    const auto n = std::min<std::size_t>(8193, corpus.size() - i);
    run(std::span(corpus).subspan(i, n), n % 3 ? 1 : 3, n % 3 ? n : n / 3);
  }
  empty(runtime);
  // Full shape equality matters even when byte counts match, including empty axes.
  auto rejectShapes = [&](std::span<const std::int64_t> leftShape,
                          std::span<const std::int64_t> rightShape,
                          m::Format format = m::Format::Float32) {
    auto count = [](auto shape) {
      std::size_t n = 1;
      for (auto d : shape)
        n *= d;
      return n;
    };
    std::vector<std::uint32_t> a(count(leftShape)), b(count(rightShape));
    auto ha = runtime.hostTensor(std::as_bytes(std::span(a)), leftShape, m::Format::Float32),
         hb = runtime.hostTensor(std::as_bytes(std::span(b)), rightShape, format);
    CHECK(ha && hb);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto ua = runtime.upload(*core, group, *ha, *device),
         ub = runtime.upload(*core, group, *hb, *device);
    CHECK(core->drain());
    auto da = observe(*core, ua), db = observe(*core, ub);
    auto before = runtime.stats().submissions;
    auto work = runtime.elementwise(*core, group, kernels[3], da, db);
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    CHECK(runtime.stats().submissions == before);
    retire(*core, group);
  };
  using Shapes = std::array<std::int64_t, 4>;
  for (auto shapes :
       {Shapes{2, 3, 3, 2}, Shapes{2, 0, 3, 0}, Shapes{0, 2, 0, 3}, Shapes{2, 3, 2, 4}})
    rejectShapes(std::span(shapes).first(2), std::span(shapes).last(2));
  const std::int64_t matrix[] = {2, 3}, vector[] = {6};
  rejectShapes(matrix, vector);
  rejectShapes(vector, matrix);
  // Equal rank-three shapes are supported and covered by numeric matrix tests.
  rejectShapes(matrix, matrix, m::Format::UInt32);
  empty(runtime);
  std::printf("Matrix elementwise: %llu cases, %llu CPU/GPU/APFloat lane comparisons; "
              "shape equality and immutable captures passed\n",
              (unsigned long long)cases, (unsigned long long)comparisons);
}
