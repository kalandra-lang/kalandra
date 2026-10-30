// Sign changes and identities compare exact bits, including signaling NaNs.
template <std::size_t Rank>
void tensorNegationConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  std::array<m::Kernel, 4> kernels;
  for (unsigned op = 0; op < 4; ++op) {
    auto path =
        std::string(directory) + "/kernel" + std::to_string((Rank == 2 ? 0 : 4) + op) + ".metallib";
    auto kernel = op == 3 ? runtime.loadElementwiseKernel(path, *device)
                          : runtime.loadElementwiseKernel(path, *device, {1, 0});
    CHECK(kernel);
    kernels[op] = *kernel;
  }
  auto corpus = float_proof::corpus();
  std::uint64_t cases = 0, lanes = 0, exact = 0;
  auto run = [&](std::size_t n, bool alias = false) {
    Invocation context;
    auto *a = static_cast<float *>(context.allocate(n * 4));
    auto *b = static_cast<float *>(context.allocate(n * 4));
    CHECK(a && b);
    Float32Tensor<Rank> left{}, right{};
    left.allocated = left.aligned = a;
    right.allocated = right.aligned = b;
    if constexpr (Rank == 1) {
      left.sizes[0] = right.sizes[0] = n;
      left.strides[0] = right.strides[0] = 1;
    } else {
      const auto rows = n % 7 == 0 ? 7 : n % 5 == 0 ? 5 : 1;
      left.sizes[0] = right.sizes[0] = rows;
      left.sizes[1] = right.sizes[1] = n / rows;
      left.strides[0] = right.strides[0] = n / rows;
      left.strides[1] = right.strides[1] = 1;
    }
    for (std::size_t i = 0; i < n; ++i) {
      std::memcpy(a + i, &corpus[i].a, 4);
      std::memcpy(b + i, alias ? &corpus[i].a : &corpus[i].b, 4);
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
    for (unsigned op = 0; op < 4; ++op) {
      Float32Tensor<Rank> cpu{};
      if constexpr (Rank == 2) {
        if (op == 0)
          _mlir_ciface_kalandra_cpu_0(&cpu, &left, &context);
        if (op == 1)
          _mlir_ciface_kalandra_cpu_1(&cpu, &left, &context);
        if (op == 2)
          _mlir_ciface_kalandra_cpu_2(&cpu, &left, &context);
        if (op == 3)
          _mlir_ciface_kalandra_cpu_3(&cpu, &left, &right, &context);
      } else {
        if (op == 0)
          _mlir_ciface_kalandra_cpu_4(&cpu, &left, &context);
        if (op == 1)
          _mlir_ciface_kalandra_cpu_5(&cpu, &left, &context);
        if (op == 2)
          _mlir_ciface_kalandra_cpu_6(&cpu, &left, &context);
        if (op == 3)
          _mlir_ciface_kalandra_cpu_7(&cpu, &left, &right, &context);
      }
      CHECK(!context.status());
      std::array<m::Buffer, 2> args{da, db};
      auto work = runtime.elementwise(*core, group, kernels[op],
                                      std::span<const m::Buffer>(args.data(), op == 3 ? 2 : 1), {});
      args.fill({});
      CHECK(work && core->drain());
      auto deviceResult = observe(*core, work);
      CHECK(deviceResult.shape().size() == Rank);
      for (std::size_t axis = 0; axis < Rank; ++axis)
        CHECK(deviceResult.shape()[axis] == left.sizes[axis] &&
              cpu.sizes[axis] == left.sizes[axis]);
      auto down = runtime.download(*core, group, deviceResult);
      CHECK(down && core->drain());
      auto result = observe(*core, down);
      auto bytes = result.hostBytes();
      CHECK(bytes && bytes->size() == n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        const auto bBits = alias ? corpus[i].a : corpus[i].b;
        auto expected = op == 3 ? float_proof::oracle({corpus[i].a, bBits}, 0) ^ 0x80000000u
                                : corpus[i].a ^ (op == 0 ? 0x80000000u : 0u);
        std::uint32_t cb, gb;
        std::memcpy(&cb, cpu.aligned + cpu.offset + i, 4);
        std::memcpy(&gb, bytes->data() + 4 * i, 4);
        if (op == 3)
          CHECK(float_proof::equal(cb, expected) && float_proof::equal(gb, expected));
        else {
          CHECK(cb == expected && gb == expected);
          ++exact;
        }
        CHECK(std::memcmp(a + i, &corpus[i].a, 4) == 0 && std::memcmp(b + i, &bBits, 4) == 0);
        ++lanes;
      }
      context.release(cpu.allocated);
      ++cases;
    }
    context.release(a);
    context.release(b);
    retire(*core, group);
  };
  for (auto n :
       {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(6),
        std::size_t(63), std::size_t(64), std::size_t(65), std::size_t(127), std::size_t(129),
        std::size_t(1025), std::size_t(65537), std::size_t(131073), corpus.size()}) {
    run(n);
    empty(runtime);
  }
  for (auto n : {std::size_t(1), std::size_t(65), std::size_t(1025)}) {
    run(n, true);
    empty(runtime);
  }
  std::printf("Tensor unary rank-%zu: %llu cases, %llu lanes (%llu exact-bit) passed\n", Rank,
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(lanes),
              static_cast<unsigned long long>(exact));
}
