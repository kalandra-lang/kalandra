// Included by the generated-source harness after its shared lifecycle utilities.
void scalarConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  const std::array<m::ElementwiseSignature, 5> signatures{{{1, 1}, {1, 2}, {2, 1}, {1, 0}, {1, 8}}};
  std::array<m::Kernel, 5> kernels;
  for (unsigned i = 0; i < kernels.size(); ++i) {
    auto path = std::string(directory) + "/kernel" + std::to_string(i) + ".metallib";
    auto kernel = runtime.loadElementwiseKernel(path, *device, signatures[i]);
    CHECK(kernel);
    kernels[i] = *kernel;
    CHECK(!runtime.loadElementwiseKernel(path, *device));
    auto wrong = signatures[i];
    ++wrong.scalars;
    CHECK(!runtime.loadElementwiseKernel(path, *device, wrong));
    wrong = signatures[i];
    wrong.tensors = 3 - wrong.tensors;
    CHECK(!runtime.loadElementwiseKernel(path, *device, wrong));
  }
  CHECK(!runtime.loadElementwiseKernel(std::string(directory) + "/kernel0.metallib", *device,
                                       {0, 1}));
  CHECK(!runtime.loadElementwiseKernel(std::string(directory) + "/kernel0.metallib", *device,
                                       {1, 9}));
  auto corpus = float_proof::corpus();
  std::uint64_t comparisons = 0;
  auto run = [&](std::size_t n, std::uint32_t factorBits) {
    Invocation context;
    auto *a = static_cast<float *>(context.allocate(n * 4)),
         *b = static_cast<float *>(context.allocate(n * 4));
    CHECK(a && b);
    for (std::size_t i = 0; i < n; ++i) {
      auto pair = corpus[(i * 71) % corpus.size()];
      std::memcpy(a + i, &pair.a, 4);
      std::memcpy(b + i, &pair.b, 4);
    }
    Float32Tensor<1> left{a, a, 0, {std::int64_t(n)}, {1}}, right{b, b, 0, {std::int64_t(n)}, {1}};
    auto ha = runtime.fromCpu(context, left.view()), hb = runtime.fromCpu(context, right.view());
    CHECK(ha && hb);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto ua = runtime.upload(*core, group, *ha, *device),
         ub = runtime.upload(*core, group, *hb, *device);
    CHECK(core->drain());
    auto da = observe(*core, ua), db = observe(*core, ub);
    float factor = std::bit_cast<float>(factorBits), bias = std::bit_cast<float>(0x80000001u);
    for (unsigned op = 0; op < 5; ++op) {
      std::array<m::Buffer, 2> tensors{da, db};
      std::array<float, 8> scalars{factor, bias, 1, 2, 3, 4, 5, 6};
      if (op == 1) {
        scalars[0] = bias;
        scalars[1] = factor;
      }
      Float32Tensor<1> cpu{};
      if (op == 0)
        _mlir_ciface_kalandra_cpu_0(&cpu, &left, factor, &context);
      if (op == 1)
        _mlir_ciface_kalandra_cpu_1(&cpu, bias, &left, factor, &context);
      if (op == 2)
        _mlir_ciface_kalandra_cpu_2(&cpu, &left, factor, &right, &context);
      if (op == 3)
        _mlir_ciface_kalandra_cpu_3(&cpu, &left, &context);
      if (op == 4)
        _mlir_ciface_kalandra_cpu_4(&cpu, &left, factor, bias, 1, 2, 3, 4, 5, 6, &context);
      CHECK(!context.status() && cpu.sizes[0] == std::int64_t(n));
      auto work = runtime.elementwise(*core, group, kernels[op],
                                      std::span(tensors).first(signatures[op].tensors),
                                      std::span(scalars).first(signatures[op].scalars));
      CHECK(work);
      // Submission owns all captures; caller storage can change before any task step.
      scalars.fill(0);
      tensors.fill({});
      CHECK(core->drain());
      auto result = observe(*core, work);
      auto down = runtime.download(*core, group, result);
      CHECK(core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes && bytes->size() == n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t av, bv, cb, gb;
        std::memcpy(&av, a + i, 4);
        std::memcpy(&bv, b + i, 4);
        std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
        std::memcpy(&gb, bytes->data() + i * 4, 4);
        CHECK(av == corpus[(i * 71) % corpus.size()].a && bv == corpus[(i * 71) % corpus.size()].b);
        auto expected = op == 3 ? av : float_proof::oracle({av, factorBits}, 1);
        if (op == 1 || op == 4)
          expected = float_proof::oracle({expected, 0x80000001u}, 0);
        if (op == 2)
          expected = float_proof::oracle({expected, bv}, 0);
        if (op == 4)
          for (auto bits :
               {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u, 0x40a00000u, 0x40c00000u})
            expected = float_proof::oracle({expected, bits}, 0);
        CHECK(float_proof::equal(cb, expected) && float_proof::equal(gb, expected));
        if (op == 3)
          CHECK(gb == av); // Identity preserves even NaN payloads and signed zero.
        ++comparisons;
      }
      context.release(cpu.allocated);
    }
    // Wrong counts, host residency and shape are faults, without an encoded command.
    auto reject = [&](std::span<const m::Buffer> tensors, std::span<const float> scalars,
                      std::int64_t code) {
      auto rejected = TaskCore::create();
      CHECK(rejected);
      auto root = rejected->openRoot();
      auto before = runtime.stats().submissions;
      auto task = runtime.elementwise(*rejected, root, kernels[0], tensors, scalars);
      CHECK(task && rejected->drain());
      CHECK(rejected->failure(task).kind == TaskFailure::Fault &&
            rejected->failure(task).code == code);
      CHECK(runtime.stats().submissions == before);
      retire(*rejected, root);
    };
    std::array<m::Buffer, 2> tensors{da, db};
    std::array<float, 9> scalars{};
    reject({}, std::span(scalars).first(1), 3);
    reject(tensors, std::span(scalars).first(1), 3);
    reject(std::span(tensors).first(1), {}, 3);
    reject(std::span(tensors).first(1), scalars, 3);
    tensors[0] = *ha;
    reject(std::span(tensors).first(1), std::span(scalars).first(1), 7);
    retire(*core, group);
    context.release(a);
    context.release(b);
    CHECK(!context.live() && context.allocations() == context.releases());
  };
  for (auto n : {0u, 1u, 2u, 63u, 64u, 65u, 4097u})
    run(n, 0x3f800001u);
  for (auto factor : {0u, 0x80000000u, 1u, 0x80000001u, 0x007fffffu, 0x00800000u, 0x3f000000u,
                      0x3f800000u, 0x3f800001u, 0xbf7ffffeu, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u,
                      0xff800000u, 0x7fc12345u, 0x7f800001u})
    run(8193, factor);
  empty(runtime);
  std::printf("Scalar entries: %llu CPU/GPU cases match APFloat; owned captures and checked "
              "signatures passed\n",
              (unsigned long long)comparisons);
}
