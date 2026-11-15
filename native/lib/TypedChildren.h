#pragma once
#include "kalandra/Semantics.h"
#include <type_traits>

namespace kalandra {
// Arena edges only: function/record identities and lexical loop identities are
// not value IDs. Keep capture edges, every control suite and error/placement
// operand when copying or determining liveness of checked typed IR.
template <class Field, class Visit> void visitTypedIds(Field &field, Visit &visit) {
  if constexpr (std::is_same_v<std::remove_cvref_t<Field>, ValueId>)
    visit(field);
  else if constexpr (requires { field.has_value(); }) {
    if (field)
      visitTypedIds(*field, visit);
  } else {
    for (auto &value : field)
      visitTypedIds(value, visit);
  }
}
template <class Expr, class ValueVisit, class SymbolVisit>
void visitTypedChildren(Expr &expression, ValueVisit values, SymbolVisit symbols) {
  const auto suite = [&](auto &statements) {
    for (auto &statement : statements) {
      values(statement.value);
      if (statement.binding)
        symbols(*statement.binding);
    }
  };
  std::visit(
      [&](auto &op) {
        using T = std::remove_cvref_t<decltype(op)>;
        if constexpr (requires { op.arguments; })
          visitTypedIds(op.arguments, values);
        if constexpr (requires { op.captures; })
          visitTypedIds(op.captures, values);
        if constexpr (requires { op.input; })
          values(op.input);
        if constexpr (requires { op.operand; })
          values(op.operand);
        if constexpr (requires { op.left; })
          values(op.left);
        if constexpr (requires { op.right; })
          values(op.right);
        if constexpr (requires { op.callee; })
          values(op.callee);
        if constexpr (requires { op.subject; })
          values(op.subject);
        if constexpr (requires { op.condition; })
          values(op.condition);
        if constexpr (requires { op.current; })
          values(op.current);
        if constexpr (requires { op.stop; })
          values(op.stop);
        if constexpr (requires { op.step; })
          values(op.step);
        if constexpr (requires { op.start; })
          values(op.start);
        if constexpr (requires { op.end; })
          values(op.end);
        if constexpr (requires { op.index; })
          values(op.index);
        if constexpr (requires { op.indices; })
          visitTypedIds(op.indices, values);
        if constexpr (requires { op.elements; })
          visitTypedIds(op.elements, values);
        if constexpr (requires { op.dimensions; })
          visitTypedIds(op.dimensions, values);
        if constexpr (requires { op.epsilon; })
          visitTypedIds(op.epsilon, values);
        if constexpr (requires { op.base; })
          visitTypedIds(op.base, values);
        if constexpr (requires { op.placement; })
          visitTypedIds(op.placement, values);
        if constexpr (std::is_same_v<T, Transfer>)
          values(op.target);
        if constexpr (std::is_same_v<T, ArrayStore> || std::is_same_v<T, LoopTransferOperation>)
          visitTypedIds(op.value, values);
        if constexpr (std::is_same_v<T, FailOperation>)
          values(op.error);
        if constexpr (std::is_same_v<T, CatchOperation>)
          symbols(op.parameter);
        if constexpr (std::is_same_v<T, RecordValue>)
          for (auto &field : op.fields)
            values(field.value);
        if constexpr (requires { op.initial; })
          suite(op.initial);
        if constexpr (requires { op.iteration; })
          suite(op.iteration);
        if constexpr (requires { op.body; })
          suite(op.body);
        if constexpr (requires { op.whenTrue; })
          suite(op.whenTrue);
        if constexpr (requires { op.whenFalse; })
          suite(op.whenFalse);
        if constexpr (requires { op.whenDone; })
          suite(op.whenDone);
        if constexpr (requires { op.trueResult; })
          values(op.trueResult);
        if constexpr (requires { op.falseResult; })
          values(op.falseResult);
        if constexpr (requires { op.normalResult; })
          visitTypedIds(op.normalResult, values);
        if constexpr (requires { op.exitValues; })
          visitTypedIds(op.exitValues, values);
        if constexpr (requires { op.result; })
          values(op.result);
        if constexpr (requires { op.cases; })
          for (auto &arm : op.cases) {
            suite(arm.body);
            values(arm.result);
          }
      },
      expression.operation);
}
} // namespace kalandra
