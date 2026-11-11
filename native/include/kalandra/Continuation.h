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
  ResumableCall,
  MapInit,
  MapPrepare,
  MapJoin,
  MapPublish,
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
  PropagateGroup,
  CollectInit,
  CollectAppend,
  CollectFinish,
  ResourceEnter,
  ResourceLeave
};
struct Instruction {
  Code code;
  Slot output = 0;
  std::vector<Slot> args;
  std::size_t target = 0, group = 0, jump = 0;
  ValueId source = 0;
  std::optional<Slot> placement = {};
  // Innermost first: collecting expressions abandoned by error propagation.
  std::vector<Slot> collections = {};
  bool tail = false;
};
struct Plan {
  std::vector<ValueType> slots;
  std::vector<Slot> parameters;
  std::vector<Instruction> code;
  std::size_t groups = 0;
  Slot result = 0;
  bool valueScopes = false;
};
struct Options {
  // Preserve ordinary numeric calls; expand resumable helper bodies into the caller.
  bool preserveCalls = false;
  // Plan a selected entry and reachable resumable children, retaining function IDs.
  std::optional<FunctionId> entry = {};
  // Internal optimization admission: retain checked continuation kernels if a
  // synchronous subgraph exceeds the native backend's physical-frame limits.
  bool nativeSubgraphs = true;
  // Ownership scopes for mutable owned values in continuation frames. Placed
  // supervisors use the shared native descriptor scopes for host arrays.
  bool valueScopes = false;
  // Device checkpoint lowering expands non-recursive synchronous loop helpers.
  bool inlineLoopCalls = false;
  // Placement-aware consumers validate their own device value layouts. This is
  // planning admission only, not permission to transport arbitrary host objects.
  bool deviceValues = false;
  // Emit separately resumable plans for direct device calls. The device
  // backend implements their frame storage and return dispatch.
  bool deviceCallFrames = false;
};
struct ModulePlan {
  // The source typed module must outlive its plan and target emission.
  const TypedModule &input;
  TypedModule kernels;
  std::vector<std::unique_ptr<Plan>> plans;
  std::map<ValueId, FunctionId> kernelIds;
  // Original source identity -> imported synchronous function identity. Native
  // closure bridges must use this map rather than assume projection IDs match.
  std::map<FunctionId, FunctionId> importedFunctions;
  explicit ModulePlan(const TypedModule &input) : input(input) {}
};
// Direct/transitive task or control effects require continuation lowering.
[[nodiscard]] std::vector<bool> resumableFunctions(const TypedModule &input);
// Host-only callable boundaries that must retain resumable closure identities
// instead of being projected into the synchronous CPU bridge.
[[nodiscard]] std::vector<bool> hostCallableFunctions(const TypedModule &input);
// Suspension/device effects, excluding synchronous expected-error control flow.
// Ignore console suspension only when proving an uncancellable outer application;
// ordinary tasks and callable admission always use the default resumable mode.
[[nodiscard]] std::vector<bool> coordinatingFunctions(const TypedModule &input,
                                                      bool consoleMaySuspend = true);
// Direct/transitive lexical loop control needs native CFG entries, not task
// scheduling or recursive source-body expansion.
[[nodiscard]] std::vector<bool> loopControlFunctions(const TypedModule &input);
[[nodiscard]] std::expected<ModulePlan, std::string> plan(const TypedModule &input,
                                                          Options options = {});
[[nodiscard]] std::string dump(const ModulePlan &module);
} // namespace kalandra::continuation
