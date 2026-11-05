// Private result transport qualification. Source task admission remains a
// separate gate: this proves arena lifetime, opaque bytes and typed metadata.
#if defined(KALANDRA_CONTROL_CUDA)
#include "../cuda/Async.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Async.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
g::Program prepare(g::Runtime &runtime, const char *file) {
  auto kernel = take(runtime.loadKernel(file), "load export kernel");
  const auto words = kernel.parameterWords();
  check(kernel.inputCount() == 2 && words >= 4 && words % 2 == 0, "unexpected export schema");
  g::ProgramKernel schema;
  schema.type = NumericType::UInt64;
  schema.tensorTypes = {NumericType::UInt64, NumericType::UInt64};
  schema.scalarBuffers = {2, 3};
  schema.scalar = true;
  schema.scalarTypes.assign((words - 2) / 2, NumericType::UInt64);
  schema.reads = std::vector<std::uint32_t>{0, 1};
  auto contract =
      take(g::kernelContract(schema, NumericProgramKind::DeviceControl), "export contract");
  schema.kernel = take(runtime.loadKernel(file, &contract), "load checked export shader");
  g::ProgramInstruction op{NumericProgramKind::DeviceControl,           schema, {0}, {},
                           std::vector<std::uint64_t>((words - 2) / 2), 1};
  check(!g::Program::prepare(runtime, 1, std::array{op}, 1, NumericType::UInt64, true),
        "export admitted scalar result");
  check(!g::Program::prepare(runtime, 1, std::array{op}, 1, NumericType::Int64, false),
        "export admitted noncarrier type");
  const std::array tail{op, g::ProgramInstruction{NumericProgramKind::Alias, {}, {1}, {}, {}, 2}};
  check(!g::Program::prepare(runtime, 1, tail, 2, NumericType::UInt64, false),
        "export admitted intermediate tensor use");
  return take(g::Program::prepare(runtime, 1, std::array{op}, 1, NumericType::UInt64, false),
              "prepare terminal export");
}
void run(g::Runtime &runtime, const g::Program &program, std::uint64_t count, bool alias,
         bool corrupt = false, bool interrupt = false) {
  const auto baseline = runtime.retainedBytes();
  {
    std::vector<std::uint64_t> wire{DeviceArrayWireABI::Magic, 4, count, 1};
    for (std::uint64_t i = 0; i < count; ++i)
      wire.push_back(i * 17 + 9);
    g::TensorValue input;
    input.type = NumericType::UInt64;
    input.rank = 1;
    input.dimensions[0] = wire.size();
    input.data = take(runtime.uploadBytes(std::as_bytes(std::span(wire))), "upload capture");
    auto execution = take(program.start(runtime, std::array{input}, {}), "start export");
    unsigned steps = 0, exportSteps = 0;
    bool sourceDone = false, stopped = false;
    for (; !execution.finished() && steps < 20000; ++steps) {
      auto dispatch = take(execution.next(runtime), "advance export");
      if (!dispatch) {
        if (sourceDone) {
          ++exportSteps;
          if (interrupt) {
            stopped = true;
            break;
          }
        }
        continue;
      }
      auto &d = *dispatch;
      auto result = take(runtime.execute(d.kernel, d.inputs, d.parameters, d.outputBytes, d.groups),
                         "execute export source");
      sourceDone = result.parameters[1] == UINT32_MAX && result.parameters[2] == UINT32_MAX;
      if (sourceDone && corrupt) {
        constexpr auto slot = DeviceArrayExportABI::Layout;
        result.parameters[1 + slot * 2] = 12;
      }
      auto accepted = execution.accept(runtime, std::move(result));
      if (sourceDone && corrupt) {
        check(!accepted && accepted.error() == g::Error::Invalid && !execution.result(),
              "malformed export published a value");
        stopped = true;
        break;
      }
      check(bool(accepted), "accept export fence");
      check(!execution.result(), "partial export visible before completion");
    }
    check(sourceDone && steps < 20000, "source did not reach terminal fence");
    if (!stopped) {
      auto result = take(execution.result(), "final export");
      check(result.arrayResult == DeviceArrayResultMetadata{4, count, alias ? 1u : 0u},
            "result metadata differs from source");
      const auto payload = alias ? 0 : count;
      check(!result.scalar && result.rank == 1 && result.dimensions[0] == payload &&
                result.data.bytes() == payload * 8,
            "payload shape masquerades as logical array length");
      auto bytes = take(runtime.downloadBytes(result.data), "download payload");
      check(bytes.size() == payload * 8, "wrong payload byte count");
      for (std::size_t i = 0; i < payload; ++i) {
        std::uint64_t value;
        std::memcpy(&value, bytes.data() + 8 * i, 8);
        check(value == wire[4 + i], "opaque export changed value bits");
      }
      check(!g::validateArrayPage(runtime, input, 0, 1),
            "ordinary tensor admitted as array export");
      check(!g::validateArrayPage(runtime, result, payload + 1, 0) &&
                !g::validateArrayPage(runtime, result, 0, DeviceArrayWireABI::PageWords + 1),
            "array page escaped payload or quantum");
      for (std::uint64_t at = 0; at < payload;) {
        const auto words = std::min<std::uint64_t>(payload - at, DeviceArrayWireABI::PageWords);
        auto page =
            take(g::downloadArrayPagePayload(runtime, result, at, words), "download bounded page");
        auto pageBytes = take(g::hostBytes(page), "page host bytes");
        check(pageBytes.size() == words * 8 &&
                  std::equal(pageBytes.begin(), pageBytes.end(), bytes.begin() + at * 8),
              "offset page download changed payload bytes");
        at += words;
      }
      check(!program.start(runtime, std::array{result}, {}),
            "array envelope accepted as tensor input");
      check(!g::validateTransfer(runtime, result, false),
            "array envelope accepted by tensor download");
      check(exportSteps >=
                (payload + DeviceArrayWireABI::PageWords - 1) / DeviceArrayWireABI::PageWords,
            "export exceeded page quantum");
    }
  }
  check(runtime.retainedBytes() == baseline, "export retained arena or payload owners");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  try {
    auto runtime = take(g::Runtime::create(), "create physical runtime");
    auto alias = prepare(*runtime, argv[1]);
    auto fresh = prepare(*runtime, argv[2]);
    for (auto n : {0u, 1u, 3u, 17000u}) {
      run(*runtime, alias, n, true);
      run(*runtime, fresh, n, false);
    }
    run(*runtime, fresh, 17000, false, true);
    run(*runtime, fresh, 17000, false, false, true);
#if !defined(KALANDRA_CONTROL_CUDA)
    check(runtime->validationErrors() == 0, "Vulkan validation error");
#endif
    std::cout
        << "PASS: terminal array exports, aliases, empty/multipage payloads, malformed metadata, "
           "interrupted export, tensor isolation and zero owners\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
