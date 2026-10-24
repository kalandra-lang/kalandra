#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/Semantics.h"
#include <expected>
namespace kalandra {
struct BackendOutput {
  std::string mlir;
  std::string llvm;
};
// Nonempty exports emit checked C interfaces kalandra_cpu_0, ... without a CLI main.
// Exported functions must be synchronous; callers supply the invocation context.
// Empty export slots reserve wrapper ordinals; only export-reachable CPU bodies
// are lowered (open callable flow conservatively retains every function).
// testEntry is private: synchronous modules emit a checked C wrapper; task modules
// expose a const TaskEntry pointer named kalandra_task_test_entry for a selected
// parameterless numeric/Bool function or a record/String function with typed parameters.
// These are trusted test descriptors, not a public dynamic or wire ABI.
[[nodiscard]] std::expected<BackendOutput, std::string>
lowerNative(const TypedModule &module, std::optional<std::string> testEntry = {},
            const std::vector<std::string> &exports = {},
            ArithmeticMode arithmetic = ArithmeticMode::Hardware,
            bool deferCallFinalization = false);
} // namespace kalandra
