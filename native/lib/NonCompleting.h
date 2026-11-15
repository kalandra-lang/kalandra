#pragma once
#include "kalandra/Semantics.h"
#include <algorithm>
#include <type_traits>
#include <unordered_set>

namespace kalandra {
// Children precede their parent in the typed value arena. Calls do not imply
// noncompletion: their callee may return normally on another invocation.
inline std::vector<bool> nonCompletingValues(const TypedModule &module) {
  std::unordered_set<std::size_t> exits;
  for (const auto &expression : module.expressions)
    if (const auto *jump = std::get_if<LoopTransferOperation>(&expression.operation);
        jump && !jump->continuing)
      exits.insert(jump->loop);
  std::vector<bool> result(module.expressions.size());
  for (ValueId id = 0; id < result.size(); ++id) {
    const auto stopped = [&](ValueId child) { return child < id && result[child]; };
    const auto &operation = module.expressions[id].operation;
    result[id] = std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, LoopTransferOperation> ||
                        std::is_same_v<T, FailOperation>)
            return true;
          else if constexpr (std::is_same_v<T, ConditionalOperation>)
            return stopped(op.trueResult) && stopped(op.falseResult);
          else if constexpr (std::is_same_v<T, MatchOperation>) {
            return !op.cases.empty() && std::ranges::all_of(op.cases, [&](const auto &arm) {
              return stopped(arm.result);
            });
          } else if constexpr (std::is_same_v<T, ParallelOperation>)
            return stopped(op.result);
          else if constexpr (std::is_same_v<T, WhileOperation>) {
            if (op.functionBoundary || op.traversalErrorBoundary || op.condition >= id ||
                exits.contains(op.identity))
              return false;
            const auto *condition =
                std::get_if<BoolLiteral>(&module.expressions[op.condition].operation);
            return (condition && condition->value) ||
                   (op.normalResult && stopped(*op.normalResult));
          } else
            return false;
        },
        operation);
  }
  return result;
}
} // namespace kalandra
