#if defined(KALANDRA_TEST_CUDA)
#include "../cuda/Program.h"
namespace backend = kalandra::runtime::cuda;
constexpr const char *extension = ".ptx";
#else
#include "../vulkan/Program.h"
namespace backend = kalandra::runtime::vulkan;
constexpr const char *extension = ".spv";
#endif
#include "WorkgroupTestCases.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace kalandra;
namespace {
template <class T> T take(std::expected<T, backend::Error> result, const char *message) {
  if (!result)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(result.error())));
  return std::move(*result);
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3)
      return 2;
    auto runtime = take(backend::Runtime::create(), "runtime");
    unsigned verified = 0;
    for (const auto &test : qualification::workgroupTestCases(argc == 3)) {
      const auto description = *describeNumeric(test.type);
      const auto rank = test.launch.shape.size();
      auto launch = std::make_shared<WorkgroupLaunch>(test.launch);
      backend::ProgramKernel schema;
      schema.type = description.type;
      schema.workgroup = launch;
      std::vector<backend::TensorValue> inputs;
      std::vector<std::uint32_t> operands;
      for (const auto &input : test.inputs) {
        const auto bytes = qualification::workgroupInputBytes(input);
        backend::TensorValue value{take(runtime->uploadBytes(bytes), "upload tiled input"),
                                   input.type};
        value.rank = static_cast<std::uint8_t>(input.shape.size());
        std::copy(input.shape.begin(), input.shape.end(), value.dimensions.begin());
        operands.push_back(inputs.size());
        inputs.push_back(std::move(value));
        schema.tensorTypes.push_back(input.type);
        schema.scalarBuffers.push_back(0);
      }
      const auto path = std::filesystem::path(argv[1]) / (test.name + extension);
      const auto contract =
          take(backend::kernelContract(schema, NumericProgramKind::Workgroup), "contract");
      schema.kernel = take(runtime->loadKernel(path, &contract), "load shared shuffle");
      const auto resultId = static_cast<std::uint32_t>(inputs.size());
      backend::ProgramInstruction instruction{
          NumericProgramKind::Workgroup, schema, operands, {}, {}, resultId};
      const auto program =
          take(backend::Program::prepare(*runtime, resultId, std::span(&instruction, 1), resultId,
                                         description.type, false),
               "prepare shared shuffle");
      auto invocation = take(program.start(*runtime, inputs, {}), "start shared shuffle");
      while (!invocation.finished()) {
        auto next = take(invocation.next(*runtime), "next shared shuffle");
        if (!next)
          continue;
        auto result = take(runtime->execute(next->kernel, next->inputs, next->parameters,
                                            next->outputBytes, next->groups),
                           "execute shared shuffle");
        auto accepted = invocation.accept(*runtime, std::move(result));
        if (!accepted)
          throw std::runtime_error("accept shared shuffle");
      }
      const auto result = take(invocation.result(), "shared shuffle result");
      if (result.rank != rank || result.scalar)
        throw std::runtime_error("shared shuffle rank");
      const auto bytes = take(runtime->downloadBytes(result.data), "download shared shuffle");
      const auto count = test.expected.size();
      if (bytes.size() != count * description.bytes())
        throw std::runtime_error("shared shuffle size");
      for (unsigned cell = 0; cell < count; ++cell) {
        const auto expected = test.expected[cell];
        std::uint64_t actual = 0;
        std::memcpy(&actual, bytes.data() + cell * description.bytes(), description.bytes());
        if (actual != expected)
          throw std::runtime_error(std::string(description.sourceName) + " rank " +
                                   std::to_string(rank) + " cell " + std::to_string(cell) +
                                   " shared shuffle mismatch");
      }
      ++verified;
    }
    std::cout << "PASS: " << verified
              << (argc == 3 ? " bit-exact cooperative caught-error/control fixtures\n"
                            : " bit-exact shared-memory shuffle kernels, all ten numeric types and "
                              "ranks 0..8 with padding\n");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
