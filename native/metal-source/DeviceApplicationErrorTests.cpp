#include "NumericProgram.h"
#include "../gpu-source/Supervisor.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace kalandra;

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected the device application error fixture path");
    std::ifstream fixture(argv[1], std::ios::binary);
    if (!fixture)
      throw std::runtime_error("could not read the device application error fixture");
    const std::string source{std::istreambuf_iterator<char>(fixture), {}};
    auto checked = check(parse(std::make_shared<SourceFile>("device-errors.k", source)));
    if (!checked.ok())
      throw std::runtime_error(checked.diagnostics.front().message);
    const auto &input = *checked.module;
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
fn main() -> int ! TaskError:
  try parallel:
    try await spawn work(0) on devices.gpu()
)k";
    for (auto backend : {GpuBackend::Metal, GpuBackend::Vulkan, GpuBackend::Cuda}) {
      auto parsed = parse(std::make_shared<SourceFile>("placed-device-errors.k", placed));
      if (!parsed.ok())
        throw std::runtime_error(parsed.diagnostics.front().message);
      auto emitted = emitGpuSupervisor(std::move(parsed.module), ArithmeticMode::Hardware,
                                       false, backend);
      if (!emitted)
        throw std::runtime_error(emitted.error().message);
      if (!emitted->at("host.cpp").contains("NumericInstructionKind::DeviceControl"))
        throw std::runtime_error("placed error handling did not use a device kernel");
    }
    std::cout << "Device error recovery and recursive propagation IR verified\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
