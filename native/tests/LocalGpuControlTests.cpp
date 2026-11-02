#include "../gpu-source/Supervisor.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    const std::string kernel =
        "fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n  x+x\n";
    const std::string host =
        "fn main()->Tensor[float32,3] ! TaskError:\n"
        "  i=0\n  while i<3:\n    try parallel:\n"
        "      gpu=devices.gpu()\n"
        "      x:Tensor[float32,3]=tensor([1,2,3])\n"
        "      input=try await transfer(x,gpu)\n"
        "      result=try await spawn twice(input) on gpu\n"
        "      output=try await transfer(result,devices.cpu())\n"
        "      i=i+1\n      output\n"
        "  tensor([4,5,6])\n";
    for (auto backend : {GpuBackend::Metal, GpuBackend::Vulkan, GpuBackend::Cuda}) {
      auto emit = [&](const std::string &text) {
        auto source = std::make_shared<SourceFile>("gpu-locals.k", text);
        auto parsed = parse(source);
        require(parsed.ok(), "GPU local-control fixture did not parse");
        return emitGpuSupervisor(std::move(parsed.module), ArithmeticMode::Software, false,
                                 backend);
      };
      auto result = emit(kernel + host);
      require(bool(result), "host loop rejected: " + (result ? "" : result.error().message));
      const auto &cpp = result->at("host.cpp");
      require(cpp.find("task.releaseGroup(") != std::string::npos,
              "completed mixed groups retain iteration resources");
      require(cpp.find("task.context().poll()") != std::string::npos,
              "mixed loop lost cancellation checkpoint");
      auto mutation = emit(
          "fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
          "  scale:float32=1\n  scale=2\n  x*scale\n" + host);
      require(!mutation && mutation.error().message.find("CPU execution") != std::string::npos,
              "GPU kernel admitted mutable scalar state");
      auto loop = emit(
          "fn twice(x:Tensor[float32,?])->Tensor[float32,?]:\n"
          "  while false: 0\n  x+x\n" + host);
      require(!loop && loop.error().message.find("CPU execution") != std::string::npos,
              "GPU kernel admitted statement loops");
    }
    std::cout << "Metal/Vulkan/CUDA: host loops, group release and kernel rejection gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
