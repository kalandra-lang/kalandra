#include "Supervisor.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace kalandra;
int main(int argc, char **argv) {
  try {
    if (argc < 2)
      return 2;
    for (auto backend : {GpuBackend::Metal, GpuBackend::Vulkan, GpuBackend::Cuda}) {
      for (int sourceIndex = 1; sourceIndex < argc; ++sourceIndex) {
        auto source = loadModules(argv[sourceIndex]);
        if (!source)
          throw std::runtime_error(source.error());
        auto generated = emitGpuSupervisor(
            std::move(source->module),
            backend == GpuBackend::Metal ? ArithmeticMode::HardwareWithSoftwareFallback
                                        : ArithmeticMode::Hardware,
            false, backend, true);
        if (!generated)
          throw std::runtime_error(generated.error().message);
        const auto &host = generated->at("host.cpp");
        if (host.find("decodeWorkgroupError") == host.npos ||
            host.find("loadWorkgroupKernel") == host.npos ||
            host.find("NumericInstructionKind::Workgroup") == host.npos)
          throw std::runtime_error("cooperative outcome lost native dispatch or typed decoding");
      }
      const std::string invalid = R"k(from "std/gpu" import dispatch, WorkItem
fn kernel() -> Tensor[f32, ?]:
  return dispatch([4], [4], fn(item: WorkItem) -> f32: return f32(1))
fn host() -> Tensor[f32, ?]:
  return kernel()
fn main() -> unit ! TaskError:
  result = try parallel:
    child = spawn host() on devices.cpu()
    try await child
  return ()
)k";
      const auto path =
          (std::filesystem::temp_directory_path() / "workgroup-host-rejected.k").string();
      const std::array overlays{SourceOverlay{path, invalid}};
      auto syntax = loadModules(path, {}, {}, {}, overlays);
      if (!syntax)
        throw std::runtime_error(syntax.error());
      auto rejected = emitGpuSupervisor(std::move(syntax->module), ArithmeticMode::Hardware, false,
                                        backend, true);
      if (rejected || rejected.error().message.find("GPU") == std::string::npos)
        throw std::runtime_error("host workgroup call did not diagnose GPU-only execution");
    }
    std::cout << "PASS: cooperative expected-error supervisor, typed export and host/GPU "
                 "projection on all backends\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
