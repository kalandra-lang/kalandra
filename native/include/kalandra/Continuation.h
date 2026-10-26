#pragma once
#include "kalandra/Semantics.h"
#include <expected>
#include <map>
#include <memory>

namespace kalandra::continuation {
using Slot = std::size_t;
enum class Code {
  Literal,
  Unary,
  Binary,
  Kernel,
  Copy,
  Open,
  Spawn,
  Await,
  Try,
  Join,
  Label,
  Return,
  Catch,
  Case,
  Jump,
  Field,
  Fail,
  Call,
  Device,
  Transfer,
  Branch,
  FunctionReference,
  FunctionInvoke,
  SynchronousCall,
  MapInit,
  MapCheck,
  MapLoad,
  MapStore,
  MapAdvance,
  MapSuccess,
  ReleaseGroup,
  CallableTarget,
  CallableCaptures,
  CallableSyncGuard,
  InvalidCallable,
  PropagateGroup
};
struct Instruction {
  Code code;
  Slot output = 0;
  std::vector<Slot> args;
  std::size_t target = 0, group = 0, jump = 0;
  ValueId source = 0;
  std::optional<Slot> placement = {};
};
struct Plan {
  std::vector<ValueType> slots;
  std::vector<Slot> parameters;
  std::vector<Instruction> code;
  std::size_t groups = 0;
  Slot result = 0;
};
struct Options {
  // Preserve ordinary numeric calls; expand resumable helper bodies into the caller.
  bool preserveCalls = false;
  // Plan a selected entry and reachable resumable children, retaining function IDs.
  std::optional<FunctionId> entry = {};
  // Internal optimization admission: retain checked continuation kernels if a
  // synchronous subgraph exceeds the native backend's physical-frame limits.
  bool nativeSubgraphs = true;
};
struct ModulePlan {
  // The source typed module must outlive its plan and target emission.
  const TypedModule &input;
  TypedModule kernels;
  std::vector<std::unique_ptr<Plan>> plans;
  std::map<ValueId, FunctionId> kernelIds;
  explicit ModulePlan(const TypedModule &input) : input(input) {}
};
// Direct/transitive task or control effects require continuation lowering.
[[nodiscard]] std::vector<bool> resumableFunctions(const TypedModule &input);
// Suspension/device effects, excluding synchronous expected-error control flow.
[[nodiscard]] std::vector<bool> coordinatingFunctions(const TypedModule &input);
[[nodiscard]] std::expected<ModulePlan, std::string> plan(const TypedModule &input,
                                                          Options options = {});
[[nodiscard]] std::string dump(const ModulePlan &module);
} // namespace kalandra::continuation
