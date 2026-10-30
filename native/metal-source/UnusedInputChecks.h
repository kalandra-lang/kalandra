// Compact physical bindings must not change logical argument ownership or output domains.
void unusedInputConformance(m::Runtime &runtime, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  const auto first = std::string(directory) + "/kernel0.metallib";
  const auto manyPath = std::string(directory) + "/kernel2.metallib";
  auto invalid = [&](m::ElementwiseSignature signature, std::span<const std::uint32_t> reads) {
    auto result = runtime.loadElementwiseKernel(first, *device, signature, reads);
    CHECK(!result && result.error() == m::Error::Invalid);
  };
  invalid({3, 0}, {});
  invalid({3, 0}, std::array<std::uint32_t, 1>{3});
  invalid({3, 0}, std::array<std::uint32_t, 2>{1, 1});
  invalid({3, 0}, std::array<std::uint32_t, 2>{2, 1});
  invalid({0, 0}, std::array<std::uint32_t, 1>{0});
  invalid({3, UINT32_MAX}, std::array<std::uint32_t, 1>{1});
  std::array<std::uint32_t, 30> excess{};
  for (unsigned i = 0; i < excess.size(); ++i)
    excess[i] = i;
  invalid({40, 0}, excess);
  // Metadata is copied, never borrowed from a caller's temporary array.
  std::array<std::uint32_t, 1> selected{1};
  auto chosen = runtime.loadElementwiseKernel(first, *device, {3, 0}, selected);
  CHECK(chosen);
  selected[0] = 0;
  auto many =
      runtime.loadElementwiseKernel(manyPath, *device, {40, 0}, std::array<std::uint32_t, 1>{39});
  CHECK(many);
  auto matrix = runtime.loadElementwiseKernel(std::string(directory) + "/kernel3.metallib", *device,
                                              {2, 0}, std::array<std::uint32_t, 1>{1});
  CHECK(matrix);
  auto arithmetic =
      runtime.loadElementwiseKernel(std::string(directory) + "/kernel1.metallib", *device, {4, 1},
                                    std::array<std::uint32_t, 2>{1, 3});
  CHECK(arithmetic);
  auto corpus = float_proof::corpus();
  std::uint64_t cases = 0, lanes = 0;
  for (auto n : {std::size_t(0), std::size_t(1), std::size_t(3), std::size_t(65), corpus.size()}) {
    for (bool emptyIgnored : {true, false}) {
      std::vector<std::uint32_t> bits;
      for (std::size_t i = 0; i < n; ++i)
        bits.push_back(corpus[i].a);
      const std::array<std::uint32_t, 4> junk{0x7f801234u, 0xffc01234u, 1u, 0x80000000u};
      auto active =
          runtime.hostTensor(std::as_bytes(std::span(bits)),
                             std::array<std::int64_t, 1>{std::int64_t(n)}, m::Format::Float32);
      auto ignored = runtime.hostTensor(
          emptyIgnored ? std::span<const std::byte>{} : std::as_bytes(std::span(junk)),
          std::array<std::int64_t, 2>{2, emptyIgnored ? 0 : 2}, m::Format::Float32);
      CHECK(active && ignored);
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto up = runtime.upload(*core, group, *active, *device);
      auto unusedUp = runtime.upload(*core, group, *ignored, *device);
      CHECK(up && unusedUp && core->drain());
      auto da = observe(*core, up), di = observe(*core, unusedUp);
      for (bool large : {false, true}) {
        std::vector<m::Buffer> arguments(large ? 40 : 3, di);
        arguments[large ? 39 : 1] = da;
        auto work = runtime.elementwise(*core, group, large ? *many : *chosen, arguments, {});
        arguments.clear(); // Every logical owner has already been captured.
        CHECK(work && core->drain());
        CHECK(core->failure(work).kind == TaskFailure::None);
        auto result = observe(*core, work);
        CHECK(result.shape().size() == 1 && result.shape()[0] == std::int64_t(n));
        auto down = runtime.download(*core, group, result);
        CHECK(down && core->drain());
        auto host = observe(*core, down);
        auto bytes = host.hostBytes();
        CHECK(bytes && bytes->size() == n * 4);
        // Identity preserves every payload, including signaling NaNs and signed zero.
        CHECK(!n || std::memcmp(bytes->data(), bits.data(), n * 4) == 0);
        ++cases;
        lanes += n;
      }
      auto matrixWork =
          runtime.elementwise(*core, group, *matrix, std::array<m::Buffer, 2>{da, di}, {});
      CHECK(matrixWork && core->drain());
      auto matrixResult = observe(*core, matrixWork);
      CHECK(matrixResult.shape().size() == 2 && matrixResult.shape()[0] == 2 &&
            matrixResult.shape()[1] == (emptyIgnored ? 0 : 2));
      auto matrixDown = runtime.download(*core, group, matrixResult);
      CHECK(matrixDown && core->drain());
      auto matrixHost = observe(*core, matrixDown);
      auto matrixBytes = matrixHost.hostBytes();
      CHECK(matrixBytes && matrixBytes->size() == (emptyIgnored ? 0 : 16));
      CHECK(emptyIgnored || std::memcmp(matrixBytes->data(), junk.data(), 16) == 0);
      ++cases;
      lanes += emptyIgnored ? 0 : 4;
      // Bounds safety still requires every read tensor to share all axes.
      auto badGroup = core->openRoot();
      auto bad =
          runtime.elementwise(*core, badGroup, *arithmetic,
                              std::array<m::Buffer, 4>{di, da, di, di}, std::array<float, 1>{1});
      CHECK(bad && core->drain());
      CHECK(core->failure(bad).kind == TaskFailure::Fault &&
            core->failure(bad).code == std::int64_t(m::Error::Invalid));
      CHECK(core->close(badGroup) && core->drain() && core->release(badGroup));
      // Residency is required for all logical captures, including ignored parameters.
      auto residentGroup = core->openRoot();
      auto resident = runtime.elementwise(*core, residentGroup, *chosen,
                                          std::array<m::Buffer, 3>{*ignored, da, da}, {});
      CHECK(resident && core->drain());
      CHECK(core->failure(resident).kind == TaskFailure::Fault &&
            core->failure(resident).code == std::int64_t(m::Error::Residency));
      CHECK(core->close(residentGroup) && core->drain() && core->release(residentGroup));
      // A projected entry cannot accidentally use the old two-buffer overload.
      auto wrong = runtime.elementwise(*core, group, *chosen, da, da);
      CHECK(wrong && core->drain());
      CHECK(core->failure(wrong).kind == TaskFailure::Fault &&
            core->failure(wrong).code == std::int64_t(m::Error::Invalid));
      retire(*core, group);
    }
    empty(runtime);
  }
  // Even unbound arguments remain owned through command acknowledgement.
  for (unsigned scenario : {0u, 1u, 2u, 3u}) {
    const std::array<std::uint32_t, 2> bits{0x80000000u, 0x7f801234u};
    const std::array<std::uint32_t, 4> junk{1, 2, 3, 4};
    auto active = runtime.hostTensor(std::as_bytes(std::span(bits)), std::array<std::int64_t, 1>{2},
                                     m::Format::Float32);
    auto ignored = runtime.hostTensor(std::as_bytes(std::span(junk)),
                                      std::array<std::int64_t, 2>{2, 2}, m::Format::Float32);
    CHECK(active && ignored);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto up = runtime.upload(*core, group, *active, *device);
    auto unusedUp = runtime.upload(*core, group, *ignored, *device);
    CHECK(up && unusedUp && core->drain());
    std::vector<m::Buffer> args(40, observe(*core, unusedUp));
    args[39] = observe(*core, up);
    runtime.holdCompletions(true);
    if (scenario == 2)
      runtime.failNextCompletion();
    auto work = runtime.elementwise(*core, group, *many, args, {});
    args.clear();
    CHECK(work);
    until(*core, [&] { return core->stats().externalPending == 1; });
    CHECK(runtime.stats().argumentBytes == 40 * sizeof(m::Buffer) &&
          runtime.stats().argumentPacks == 1);
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    CHECK(runtime.stats().argumentPacks == 1);
    runtime.holdCompletions(false);
    CHECK(core->drain());
    CHECK((core->failure(work).kind == TaskFailure::None) == (scenario == 3));
    if (scenario == 3)
      CHECK(!runtime.stats().argumentPacks);
    retire(*core, group);
    active = {};
    ignored = {};
    empty(runtime);
  }
  std::printf("Projected GPU inputs: %llu cases, %llu exact identity lanes; 40 logical arguments\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(lanes));
}
