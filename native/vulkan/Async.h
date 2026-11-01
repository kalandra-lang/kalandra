#pragma once
#include "../runtime/Tasks.h"
#include "Vulkan.h"
#include <optional>
namespace kalandra::runtime::vulkan {
struct AsyncLimits {
  std::uint32_t maxPending = 64;
  std::uint64_t maxCaptureBytes = 1024 * 1024;
};
struct AsyncStats {
  std::uint32_t pending = 0, queued = 0, running = 0, held = 0;
  std::uint64_t captureBytes = 0, completed = 0;
};
// Internal managed dispatch adapter. GPU waits run on the completion service,
// never a TaskCore worker. Pending frames are pinned by ExternalCompletion.
class AsyncRuntime final {
public:
  static std::unique_ptr<AsyncRuntime> create(std::shared_ptr<Runtime>, AsyncLimits = {}) noexcept;
  ~AsyncRuntime();
  TaskHandle execute(TaskCore &, GroupHandle, Kernel, std::span<const Buffer>,
                     std::span<const std::uint32_t> parameters, std::uint64_t outputBytes,
                     std::uint32_t groups, std::optional<std::uint32_t> faultWord = {}) noexcept;
  TaskHandle execute(Task &, GroupHandle, Kernel, std::span<const Buffer>,
                     std::span<const std::uint32_t> parameters, std::uint64_t outputBytes,
                     std::uint32_t groups, std::optional<std::uint32_t> faultWord = {}) noexcept;
  static const ValueLayout &bufferLayout() noexcept;
  AsyncStats stats() const noexcept;
  // Private deterministic conformance controls. Held resources remain pinned
  // until acknowledgement. Release holds before destroying a core/facade.
  void holdCompletions(bool) noexcept;
  void failNextCompletion() noexcept;

private:
  struct State;
  struct Bridge;
  std::shared_ptr<State> state_;
  explicit AsyncRuntime(std::shared_ptr<State> state) : state_(std::move(state)) {}
};
} // namespace kalandra::runtime::vulkan
