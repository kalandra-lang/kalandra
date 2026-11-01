// Private declarations/implementation included inside an explicit backend namespace.
struct AsyncLimits {
  std::uint32_t maxPending = 64;
  std::uint64_t maxCaptureBytes = 1024 * 1024;
  std::shared_ptr<ByteBudget> metadataBudget{}, pendingBudget{};
};
struct AsyncStats {
  std::uint32_t pending = 0, queued = 0, running = 0, held = 0;
  std::uint64_t captureBytes = 0, completed = 0, stages = 0;
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
  // Tensor programs publish tensorLayout(); scalar programs use the existing
  // numericScalarLayout(type), matching the source/CPU task ABI.
  TaskHandle executeProgram(TaskCore &, GroupHandle, const Program &, std::span<const TensorValue>,
                            std::span<const std::uint64_t>) noexcept;
  TaskHandle executeProgram(Task &, GroupHandle, const Program &, std::span<const TensorValue>,
                            std::span<const std::uint64_t>) noexcept;
  TaskHandle transfer(TaskCore &, GroupHandle, const TensorValue &, bool upload) noexcept;
  TaskHandle transfer(Task &, GroupHandle, const TensorValue &, bool upload) noexcept;
  TaskHandle reject(TaskCore &, GroupHandle, Error,
                    std::optional<NumericType> scalar = {}) noexcept;
  TaskHandle reject(Task &, GroupHandle, Error, std::optional<NumericType> scalar = {}) noexcept;
  static const ValueLayout &bufferLayout() noexcept;
  static const ValueLayout &tensorLayout() noexcept;
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
