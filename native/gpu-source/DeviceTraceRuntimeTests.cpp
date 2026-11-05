// Physical device adapter for the same independent graph oracle used by the
// emitted-IR CPU checks. One device step per completed submission exposes every
// collection phase; host work constructs test fixtures and copies opaque bytes.
#define KALANDRA_TRACE_PHYSICAL
#include "../metal-source/DeviceTraceExecutionTests.cpp"
#if defined(KALANDRA_TRACE_CUDA)
#include "../cuda/CudaRuntime.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Vulkan.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include <cstdlib>
#include <span>
template <class T> static T take(std::expected<T, g::Error> result, const char *message) {
  if (!result)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(result.error())));
  return std::move(*result);
}
struct PhysicalTrace {
  std::shared_ptr<g::Runtime> runtime;
  g::Kernel kernel;
  PhysicalTrace() {
    runtime = take(g::Runtime::create(), "create physical trace runtime");
    const auto device = runtime->device();
    require(device.discrete || device.integrated, "physical trace needs hardware GPU");
    std::cout << "Physical device: " << device.name << '\n';
    const char *file = std::getenv("KALANDRA_TRACE_KERNEL");
    require(file != nullptr, "set KALANDRA_TRACE_KERNEL");
    kernel = take(runtime->loadKernel(file), "load emitted trace kernel");
  }
};
extern "C" void _mlir_ciface_step(Memref<std::uint64_t> *heap, Memref<std::uint32_t> *frame) {
  static PhysicalTrace state;
  {
    auto arena =
        take(state.runtime->uploadBytes(std::as_bytes(std::span(heap->aligned, heap->size))),
             "upload trace fixture");
    auto result = take(state.runtime->execute(state.kernel, std::array{arena},
                                              std::span(frame->aligned, frame->size), 8, 1),
                       "execute emitted trace step");
    require(result.parameters.size() == std::size_t(frame->size), "trace frame shape changed");
    std::copy(result.parameters.begin(), result.parameters.end(), frame->aligned);
    auto read = state.runtime->readBytes(
        arena, std::as_writable_bytes(std::span(heap->aligned, heap->size)));
    require(bool(read), "read fenced trace fixture");
  }
  require(state.runtime->retainedBytes() == 0, "trace step retained device owners");
#if !defined(KALANDRA_TRACE_CUDA)
  require(state.runtime->validationErrors() == 0, "trace emitted Vulkan validation errors");
#endif
}
