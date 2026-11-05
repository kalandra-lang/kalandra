#pragma once
#include "kalandra/Semantics.h"

namespace kalandra {
// A reachable back edge requires a device activation stack. A repeated
// callee in an acyclic diamond does not. Use an explicit traversal stack.
inline bool deviceNeedsCallFrames(const TypedModule &module, FunctionId entry) {
  enum State : unsigned char { New, Active, Done };
  struct Scan { FunctionId function; std::size_t next; };
  std::vector<State> states(module.functions.size(), New);
  std::vector<Scan> pending{{entry, 0}};
  states.at(entry) = Active;
  while (!pending.empty()) {
    auto &frame = pending.back();
    const auto &expressions = module.functions.at(frame.function).expressions;
    if (frame.next == expressions.size()) {
      states[frame.function] = Done;
      pending.pop_back();
      continue;
    }
    const auto &op = module.expressions.at(expressions[frame.next++]).operation;
    if (std::holds_alternative<FunctionInvoke>(op) ||
        std::holds_alternative<FunctionReference>(op))
      return true;
    const auto *call = std::get_if<FunctionCall>(&op);
    if (!call)
      continue;
    auto &state = states.at(call->function);
    if (state == Active)
      return true;
    if (state == New) {
      state = Active;
      pending.push_back({call->function, 0});
    }
  }
  return false;
}
} // namespace kalandra
