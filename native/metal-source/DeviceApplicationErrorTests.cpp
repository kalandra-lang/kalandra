#include "NumericProgram.h"
#include "DeviceHeap.h"
#include "../gpu-source/AggregateGraphSchema.h"
#include "../gpu-source/DeviceErrorEntry.h"
#include "../gpu-source/Supervisor.h"
#include "../lib/NativeLayout.h"
#include "../runtime/DeviceGraphMaterialize.h"
#include "../runtime/Tensor.h"
#include "llvm/IR/LLVMContext.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace kalandra;

static void verifyOutcomeWire(const AggregateGraphSchema &graph) {
  using namespace runtime;
  std::vector<std::vector<DeviceGraphCell>> cells(graph.layouts.size());
  std::vector<std::vector<DeviceGraphNativeCell>> nativeCells(graph.layouts.size());
  std::vector<DeviceGraphLayout> layouts;
  std::vector<DeviceGraphNativeLayout> nativeLayouts;
  std::vector<const void *> keys;
  for (std::size_t i = 0; i < graph.layouts.size(); ++i) {
    const auto &layout = graph.layouts[i];
    for (const auto &field : layout.cells) {
      auto rule = field.rule;
      rule.guards = field.guards;
      cells[i].push_back(rule);
      nativeCells[i].push_back({rule, field.nativeOffset, field.nativeBytes});
    }
    layouts.push_back({cells[i], !layout.type.array});
    nativeLayouts.push_back({nativeCells[i], layout.nativeStride, !layout.type.array});
    keys.push_back(&layout);
  }
  const auto root = graph.roots.at(0);
  const auto fields = layouts.at(root - 1).cells;
  const auto owned = std::ranges::find_if(fields, [](const auto &cell) {
    return cell.target && std::ranges::any_of(cell.guards, [](const auto &guard) {
      return guard.word == 0 && guard.tag == 2;
    });
  });
  if (owned == fields.end())
    throw std::runtime_error("application error transport lost its guarded owned payload");
  const auto validate = [&](const auto &wire, std::size_t work = 300000) {
    return validateDeviceGraph(wire, layouts, root, work, [] { return false; });
  };
  std::vector<std::uint64_t> success{DeviceGraphWireABI::Magic, 1, 1, root, 1};
  const auto successStart = success.size();
  success.resize(successStart + fields.size());
  success[successStart] = 1;
  success[successStart + 1] = 5;
  if (validate(success) != DeviceGraphValidation::Valid)
    throw std::runtime_error("valid successful outcome rejected");
  success[successStart] = 3;
  if (validate(success) != DeviceGraphValidation::Invalid)
    throw std::runtime_error("invalid outcome discriminator admitted");

  std::vector<std::uint64_t> failure{DeviceGraphWireABI::Magic, 2, 1, root, 1,
                                     owned->target, 2};
  const auto failureStart = failure.size();
  failure.resize(failureStart + fields.size());
  for (const auto &guard : owned->guards)
    failure[failureStart + guard.word] = guard.tag;
  failure[failureStart + (owned - fields.begin())] = 2;
  failure.insert(failure.end(), {7, 11});
  if (validate(failure) != DeviceGraphValidation::Valid ||
      validate(failure, 1) != DeviceGraphValidation::Budget ||
      validateDeviceGraph(failure, layouts, root, 300000, [] { return true; }) !=
          DeviceGraphValidation::Cancelled)
    throw std::runtime_error("owned failure outcome violated validation, budget or cancellation");
  const auto &nativeRoot = nativeLayouts.at(root - 1);
  const auto ownedOffset = nativeRoot.cells[owned - fields.begin()].offset;
  std::vector<std::byte> native(nativeRoot.stride);
  Invocation context;
  {
    Invocation::OwnershipScope scope(context);
    Invocation::OwnershipSelection selected(scope);
    if (materializeDeviceGraph(context, failure, layouts, nativeLayouts, keys, root,
                               native, 300000, 300000) != DeviceGraphValidation::Valid)
      throw std::runtime_error("valid error outcome failed to materialize");
    CpuTensor<NumericType::Int64, 1> array;
    std::memcpy(&array, native.data() + ownedOffset, sizeof(array));
    if (!array.aligned || array.sizes[0] != 2 || array.aligned[0] != 7 || array.aligned[1] != 11)
      throw std::runtime_error("materialized error lost its owned array values");
  }
  if (context.live())
    throw std::runtime_error("releasing the outcome scope retained its error backing");
  const auto untouched = native;
  const auto allocations = context.allocations();
  if (allocations < 2)
    throw std::runtime_error("error materialization did not exercise partial allocation");
  for (std::uint64_t attempt = 1; attempt <= allocations; ++attempt) {
    Invocation exhausted;
    exhausted.failAllocationAt(attempt);
    if (materializeDeviceGraph(exhausted, failure, layouts, nativeLayouts, keys, root,
                               native, 300000, 300000) != DeviceGraphValidation::Budget ||
        exhausted.live() || native != untouched ||
        exhausted.allocations() != exhausted.releases())
      throw std::runtime_error("failed error materialization published data or retained backing");
  }
  failure[failureStart + 1] = 5;
  if (validate(failure) != DeviceGraphValidation::Invalid)
    throw std::runtime_error("inactive success payload admitted alongside an error");
}

static void verifyErrorExport(TypedModule module) {
  const auto original = std::ranges::find(module.functions, "descend", &TypedFunction::name);
  if (original == module.functions.end())
    throw std::runtime_error("missing fallible export fixture");
  const auto function = original - module.functions.begin();
  std::size_t budget = 300000;
  auto entry = appendDeviceErrorEntry(module, function, budget);
  if (!entry)
    throw std::runtime_error(entry.error());
  const auto &outcome = module.functions[*entry].resultType;
  llvm::LLVMContext context;
  auto native = planNativeAggregateLayouts(module, context, llvm::DataLayout(nativeCpuDataLayout));
  if (!native)
    throw std::runtime_error(native.error());
  auto graph = planAggregateGraphSchema(module, std::span(&outcome, 1), *native, budget);
  if (!graph)
    throw std::runtime_error(graph.error());
  verifyOutcomeWire(*graph);
  std::vector<DeviceGraphExportLayout> layouts;
  for (std::size_t i = 0; i < graph->layouts.size(); ++i) {
    const auto &layout = graph->layouts[i];
    const auto identity = layout.type.record
        ? runtime::DeviceHeapBlockABI::nominalLayout(*layout.type.record, layout.type.array)
        : runtime::DeviceArrayWireABI::layout(layout.type.element,
                                              layout.type.builtin == BuiltinValue::Bool);
    DeviceGraphExportLayout exported{static_cast<unsigned>(identity), static_cast<unsigned>(i + 1),
                                     static_cast<unsigned>(layout.cells.size()), {}};
    for (std::size_t cell = 0; cell < layout.cells.size(); ++cell) {
      const auto &field = layout.cells[cell];
      if (!field.rule.target)
        continue;
      exported.references.push_back(static_cast<unsigned>(cell));
    }
    layouts.push_back(std::move(exported));
  }
  auto program = emitNumericProgram(module, *entry, ArithmeticMode::Hardware, false,
                                    true, {}, layouts);
  if (!program)
    throw std::runtime_error(program.error().message);
  if (program->type.record != outcome.record ||
      program->code.back().kind != NumericProgramKind::DeviceControl)
    throw std::runtime_error("application error outcome did not use device graph export");
}

int main(int argc, char **argv) {
  try {
    if (argc != 6)
      throw std::runtime_error("expected the device application error fixture paths");
    std::ifstream fixture(argv[1], std::ios::binary);
    if (!fixture)
      throw std::runtime_error("could not read the device application error fixture");
    const std::string source{std::istreambuf_iterator<char>(fixture), {}};
    auto checked = check(parse(std::make_shared<SourceFile>("device-errors.k", source)));
    if (!checked.ok())
      throw std::runtime_error(checked.diagnostics.front().message);
    const auto &input = *checked.module;
    verifyErrorExport(input);
    const auto entry = std::ranges::find(input.functions, "work", &TypedFunction::name);
    if (entry == input.functions.end())
      throw std::runtime_error("missing checked error fixture entry");
    auto program = emitNumericProgram(input, entry - input.functions.begin(),
                                      ArithmeticMode::Hardware, false);
    if (!program)
      throw std::runtime_error(program.error().message);
    if (std::ranges::none_of(program->code, [](const auto &op) {
          return op.kind == NumericProgramKind::DeviceControl;
        }))
      throw std::runtime_error("recoverable error program did not use device control");
    const std::string placed = source + R"k(
fn main() -> i64 ! TaskError:
  return try parallel:
    try await spawn work(0) on devices.gpu()
)k";
    const std::string observed = source + R"k(
fn main() -> i64 ! TaskError:
  return try parallel:
    child = spawn descend(0) on devices.gpu()
    await child catch fn(error):
      return match error:
        SampleError.Invalid { values }: values[0] + values[1]
        OtherError.Empty: 23
        _: 0
)k";
    const std::string propagated = source + R"k(
fn main() -> i64 ! TaskError:
  result = parallel:
    try await spawn descend(40) on devices.gpu()
  return result catch fn(error):
    return match error:
      SampleError.Invalid { values }: values[0] + values[1]
      OtherError.Empty: 23
      _: 0
)k";
    const std::string uncaught = source + R"k(
fn main() -> i64 ! (SampleError | OtherError | TaskError):
  return try parallel:
    try await spawn descend(0) on devices.gpu()
)k";
    const std::string helper = source + R"k(
fn coordinate(n: i64) -> i64 ! (SampleError | OtherError | TaskError):
  return try parallel:
    try await spawn descend(n) on devices.gpu()

fn main() -> i64 ! TaskError:
  return try parallel:
    child = spawn coordinate(0) on devices.cpu()
    first = await child
    total = first catch fn(error): return 0
    i = 0
    while i < 100:
      response = await child
      value = response catch fn(error):
        return match error:
          SampleError.Invalid { values }: values[0] + values[1]
          OtherError.Empty: 23
          _: 0
      total = total + value
      i = i + 1
    total
)k";
    std::ifstream recursiveFixture(argv[2], std::ios::binary);
    if (!recursiveFixture)
      throw std::runtime_error("could not read the recursive error fixture");
    const std::string recursive{std::istreambuf_iterator<char>(recursiveFixture), {}};
    std::ifstream tensorFixture(argv[3], std::ios::binary);
    if (!tensorFixture)
      throw std::runtime_error("could not read the tensor error fixture");
    const std::string tensor{std::istreambuf_iterator<char>(tensorFixture), {}};
    std::ifstream residentFixture(argv[4], std::ios::binary);
    if (!residentFixture)
      throw std::runtime_error("could not read the resident tensor outcome fixture");
    const std::string resident{std::istreambuf_iterator<char>(residentFixture), {}};
    std::ifstream capturedFixture(argv[5], std::ios::binary);
    if (!capturedFixture)
      throw std::runtime_error("could not read the captured tensor error fixture");
    const std::string captured{std::istreambuf_iterator<char>(capturedFixture), {}};
    const std::string hostOnly = R"k(
error HostError:
  Invalid:
    detail: String

fn validate() -> i64 ! HostError:
  fail HostError.Invalid { detail: "invalid" }

fn main() -> i64 ! (HostError | TaskError):
  return try parallel:
    try await spawn validate() on devices.gpu()
)k";
    for (auto backend : {GpuBackend::Metal, GpuBackend::Vulkan, GpuBackend::Cuda}) {
      auto invalid = parse(std::make_shared<SourceFile>("host-only-error.k", hostOnly));
      if (!invalid.ok())
        throw std::runtime_error(invalid.diagnostics.front().message);
      auto rejected = emitGpuSupervisor(std::move(invalid.module), ArithmeticMode::Hardware,
                                        false, backend);
      if (rejected || !rejected.error().message.contains("HostError.detail") ||
          !rejected.error().message.contains("no device layout"))
        throw std::runtime_error(rejected ? "host-only error payload reached device lowering"
                                         : rejected.error().message);
      for (const auto &program : {placed, observed, propagated, uncaught, helper, recursive, tensor, resident, captured}) {
        auto parsed = parse(std::make_shared<SourceFile>("placed-device-errors.k", program));
        if (!parsed.ok())
          throw std::runtime_error(parsed.diagnostics.front().message);
        auto emitted = emitGpuSupervisor(std::move(parsed.module), ArithmeticMode::Hardware,
                                         false, backend);
        if (!emitted)
          throw std::runtime_error(emitted.error().message);
        const auto &host = emitted->at("host.cpp");
        if (!host.contains("NumericInstructionKind::DeviceControl"))
          throw std::runtime_error("placed error handling did not use a device kernel");
        if (program != placed && !host.contains("DeviceAggregateResultJob"))
          throw std::runtime_error("fallible task did not use the typed device outcome transport");
        if (program == propagated && !host.contains("task.stopGroup("))
          throw std::runtime_error("propagated device error did not reserve the group outcome");
        if (program == helper && (!host.contains("::outcomeLayout,bytes") ||
                                  !host.contains("f.arrayScopes.clear(")))
          throw std::runtime_error("host helper await did not use a typed outcome");
        if (program == captured && !host.contains("DeviceGraphArgument<GpuGraphCapture"))
          throw std::runtime_error("captured tensor error lost its source graph transport");
        if (program == resident && (!host.contains("DeviceResidentOutcome<") ||
                                    !host.contains("> successTensor(")))
          throw std::runtime_error("fallible tensor task lost its resident result publication");
        if (program == tensor && !host.contains("r.fromCpu(task.context(),descriptor.view())"))
          throw std::runtime_error("tensor error projection lost its native descriptor adapter");
        if (program == recursive && !host.contains("kalandra_recursive_read_v1("))
          throw std::runtime_error("recursive error projection bypassed the checked host read");
        if (program == uncaught &&
            (!host.contains("mainOutcomeLayout") ||
             !host.contains(R"(std::fprintf(stderr,"%s.%s\n","SampleError","Invalid");return 89;)")))
          throw std::runtime_error("uncaught application error lost its CLI result");
      }
    }
    std::cout << "Device error recovery and recursive propagation IR verified\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
