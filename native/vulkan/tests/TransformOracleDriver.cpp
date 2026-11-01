#include "Program.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %d\n", message, int(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
std::expected<TensorValue, Error> execute(Runtime &runtime, ProgramExecution invocation) {
  for (;;) {
    auto next = invocation.next(runtime);
    if (!next)
      return std::unexpected(next.error());
    if (!*next) {
      if (invocation.finished())
        return invocation.result();
      continue;
    }
    auto &command = **next;
    auto result = runtime.execute(command.kernel, command.inputs, command.parameters,
                                  command.outputBytes, command.groups);
    if (!result)
      return std::unexpected(result.error());
    auto accepted = invocation.accept(runtime, std::move(*result));
    if (!accepted)
      return std::unexpected(accepted.error());
  }
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 3)
    return 2;
  auto runtime = take(
      Runtime::create({.deviceIndex = argc == 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0,
                       .maxBytes = 128 * 1024 * 1024}),
      "oracle runtime");
  std::map<std::string, Kernel> kernels;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream command(line);
    std::string name, file;
    unsigned operation, deviceEpsilon;
    std::uint64_t rows, columns, epsilon;
    check(bool(command >> name >> operation >> rows >> columns >> epsilon >> deviceEpsilon >>
               std::quoted(file)),
          "oracle request");
    auto numeric = resolveNumeric(name);
    check(numeric.has_value() && operation <= 2 && rows <= INT64_MAX && columns <= INT64_MAX,
          "oracle type/shape");
    const auto type = *describeNumeric(*numeric);
    check(!rows || !columns || rows <= 4000000 / columns, "oracle bounded cells");
    const auto count = rows && columns ? rows * columns : 0;
    std::vector<std::byte> data(count * type.bytes());
    std::ifstream input(file, std::ios::binary);
    input.read(reinterpret_cast<char *>(data.data()), data.size());
    check(std::size_t(input.gcount()) == data.size(), "oracle input bytes");
    const auto kind = operation == 0   ? TensorTransformKind::Transpose
                      : operation == 1 ? TensorTransformKind::SoftmaxRows
                                       : TensorTransformKind::RmsNormRows;
    const auto key = std::string(tensorTransformName(kind)) + "-" + name;
    auto found = kernels.find(key);
    if (found == kernels.end())
      found =
          kernels
              .emplace(key,
                       take(runtime->loadKernel(std::filesystem::path(argv[1]) / (key + ".spv")),
                            "oracle kernel"))
              .first;
    ProgramKernel shader;
    shader.kernel = found->second;
    shader.type = *numeric;
    shader.tensorTypes = {*numeric};
    shader.scalarBuffers = {0};
    shader.transform = kind;
    ProgramInstruction op{NumericProgramKind::Map, shader, {0}, {}, {}, 1};
    TensorValue value{take(runtime->uploadBytes(data), "oracle input"), *numeric};
    value.rank = 2;
    value.dimensions[0] = rows;
    value.dimensions[1] = columns;
    std::vector<TensorValue> inputs(operation == 2 && deviceEpsilon ? 2 : 1);
    inputs[0] = value;
    if (operation == 2) {
      if (deviceEpsilon) {
        op.kernel->tensorTypes.push_back(*numeric);
        op.kernel->scalarBuffers.push_back(1);
        op.inputs.push_back(1);
        op.output = 2;
        auto raw = std::as_bytes(std::span(&epsilon, 1)).first(type.bytes());
        TensorValue eps{take(runtime->uploadBytes(raw), "oracle device epsilon"), *numeric};
        eps.scalar = true;
        inputs[1] = std::move(eps);
      } else {
        op.kernel->scalarTypes = {*numeric};
        op.constants = {epsilon};
      }
    }
    auto program = take(
        Program::prepare(*runtime, inputs.size(), std::span(&op, 1), op.output, *numeric, false),
        "oracle plan");
    auto result = execute(*runtime, take(program.start(*runtime, inputs, {}), "oracle start"));
    std::vector<std::byte> output;
    if (result)
      output = take(runtime->downloadBytes(result->data), "oracle output");
    std::printf("{\"status\":%lld,\"bits\":[",
                result ? 0LL : static_cast<long long>(result.error()));
    for (std::size_t i = 0; i < output.size() / type.bytes(); ++i) {
      std::uint64_t word = 0;
      std::memcpy(&word, output.data() + i * type.bytes(), type.bytes());
      std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(word));
    }
    std::puts("]}");
    std::fflush(stdout);
    check(!runtime->validationErrors(), "oracle Vulkan validation");
  }
  check(!runtime->retainedBytes(), "oracle backing cleanup");
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
