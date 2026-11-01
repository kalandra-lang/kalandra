// Private declarations/implementation included inside an explicit backend namespace.
struct HostTensorStorage;
// Invocation-local metadata over immutable device storage. A scalar is distinct
// from a rank-zero tensor, although both contain one numeric cell.
struct TensorValue {
  Buffer data;
  NumericType type = NumericType::Float32;
  std::array<std::uint64_t, 8> dimensions{};
  std::uint8_t rank = 0;
  bool scalar = false;
  std::shared_ptr<const HostTensorStorage> host{};
  explicit operator bool() const noexcept { return bool(data) || bool(host); }
  bool onDevice() const noexcept { return bool(data) && !host; }
  NumericType format() const noexcept { return type; }
  std::uint64_t bytes() const noexcept;
  std::span<const std::uint64_t> shape() const { return {dimensions.data(), rank}; }
};
struct ProgramKernel {
  Kernel kernel;
  NumericType type = NumericType::Float32;
  std::vector<NumericType> tensorTypes, scalarTypes;
  std::vector<std::uint8_t> scalarBuffers;
  bool scalar = false;
  // Logical-to-physical projection retained by the shared typed emitter.
  // Omission means identity; an explicitly empty projection binds no inputs.
  std::optional<std::vector<std::uint32_t>> reads{};
  std::optional<TensorTransformKind> transform{};
  Kernel matmulTranspose{};
};
std::expected<KernelContract, Error> kernelContract(const ProgramKernel &, NumericProgramKind);
struct ProgramInstruction {
  NumericProgramKind kind;
  std::optional<ProgramKernel> kernel;
  std::vector<std::uint32_t> inputs;
  // Host scalar IDs address an invocation's scalar word array; constants follow
  // these captures, matching the shared source emitter's parameter ABI.
  std::vector<std::uint32_t> scalarSources;
  std::vector<std::uint64_t> constants;
  std::uint32_t output = UINT32_MAX, target = 0;
};
struct ProgramLimits {
  std::uint64_t maxMetadataBytes = 1024 * 1024;
};
struct ProgramDispatch {
  Kernel kernel;
  std::vector<Buffer> inputs;
  std::vector<std::uint32_t> parameters;
  std::uint64_t outputBytes = 0;
  std::uint32_t groups = 0;
};
struct ProgramDefinition;
class ProgramExecution;
// Prepared immutable code, retained by every invocation. No MLIR/LLVM dependency
// or host numerical fallback is needed by this runtime representation.
class Program final {
public:
  static std::expected<Program, Error> prepare(Runtime &, std::uint32_t inputCount,
                                               std::span<const ProgramInstruction>,
                                               std::uint32_t result, NumericType resultType,
                                               bool scalar, ProgramLimits = {});
  std::expected<ProgramExecution, Error> start(Runtime &, std::span<const TensorValue>,
                                               std::span<const std::uint64_t>) const;
  std::expected<std::uint64_t, Error> invocationBytes(std::size_t scalarCount) const;
  std::optional<NumericType> scalarType() const noexcept;
  explicit operator bool() const { return bool(definition_); }

private:
  std::shared_ptr<const ProgramDefinition> definition_;
};
// A completion-service continuation: next() advances one control instruction or
// prepares one GPU dispatch; an empty dispatch with !finished() is progress.
// accept() consumes a fence-complete result. The service can check cancellation
// and interleave invocations between steps without nested task records.
class ProgramExecution final {
public:
  std::expected<std::optional<ProgramDispatch>, Error> next(Runtime &);
  std::expected<void, Error> accept(Runtime &, KernelResult);
  std::expected<TensorValue, Error> result() const;
  bool finished() const noexcept { return finished_; }

private:
  std::shared_ptr<const ProgramDefinition> definition_;
  std::vector<TensorValue> values_;
  std::vector<std::uint32_t> uses_;
  std::vector<std::uint64_t> scalars_;
  std::size_t pc_ = 0;
  TensorValue output_, reduction_;
  TensorValue matrixRight_;
  bool matrixTransposing_ = false, matrixReady_ = false;
  bool pending_ = false, reducing_ = false, finished_ = false;
  std::size_t parameterWords_ = 0;
  std::optional<Error> error_;
  void release(std::uint32_t);
  void skip(std::size_t, std::size_t);
  friend class Program;
};
