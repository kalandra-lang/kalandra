#include "ArrayAccess.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace kalandra {
namespace {
using Roots = std::set<std::size_t>;
constexpr auto unknown = std::numeric_limits<std::size_t>::max();
struct Exhausted {};
struct Summary {
  std::set<std::size_t> parameters;
  bool fresh = false, uncertain = false;
  bool operator==(const Summary &) const = default;
};
struct Environment {
  std::unordered_map<ValueId, Roots> values, inputs;
  std::unordered_map<SymbolId, Roots> symbols;
  std::map<ValueId, Roots> readers;
};
class AccessProof {
public:
  explicit AccessProof(const TypedModule &module)
      : module(module), summaries(module.functions.size()) {
    budget =
        std::max<std::size_t>(1000000, 32 * (module.expressions.size() + module.symbols.size()));
  }
  std::vector<Diagnostic> run() {
    // Mut contracts and primitive mutators introduce a write effect.
    const bool direct =
        std::ranges::any_of(module.symbols, [](const auto &s) { return s.mutableAccess; }) ||
        std::ranges::any_of(module.expressions, [](const auto &v) {
          return std::holds_alternative<ArrayStore>(v.operation) ||
                 std::holds_alternative<ArraySort>(v.operation);
        });
    const bool indirect = std::ranges::any_of(module.expressions, [&](const auto &value) {
      const auto *call = std::get_if<FunctionInvoke>(&value.operation);
      if (!call || call->callee >= module.expressions.size())
        return false;
      const auto &type = module.expressions[call->callee].type;
      return type.callable &&
             std::ranges::any_of(type.callable->mutableParameters, [](bool mut) { return mut; });
    });
    if (!direct && !indirect)
      return {};
    if (std::ranges::any_of(module.expressions, [](const auto &value) {
          return std::holds_alternative<FunctionInvoke>(value.operation);
        }))
      flow = functionInvocationFlow(module);
    try {
      std::vector<std::vector<FunctionId>> callers(module.functions.size());
      std::vector<FunctionId> pending;
      std::vector<bool> queued(module.functions.size(), true);
      for (FunctionId fn = 0; fn < module.functions.size(); ++fn) {
        pending.push_back(fn);
        for (auto id : module.functions[fn].expressions) {
          if (const auto *call = std::get_if<FunctionCall>(&module.expressions[id].operation))
            callers.at(call->function).push_back(fn);
          else if (flow && std::holds_alternative<FunctionInvoke>(module.expressions[id].operation))
            for (auto target : flow->targets[id])
              callers.at(target).push_back(fn);
        }
      }
      for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto fn = pending[cursor];
        queued[fn] = false;
        auto result = analyze(fn, false);
        Summary next;
        for (auto root : result) {
          if (root == unknown)
            next.uncertain = true;
          else if (root < module.expressions.size())
            next.fresh = true;
          else {
            const auto symbol = root - module.expressions.size();
            const auto &parameters = module.functions[fn].parameters;
            const auto found = std::ranges::find(parameters, symbol);
            if (found == parameters.end())
              next.uncertain = true;
            else
              next.parameters.insert(found - parameters.begin());
          }
        }
        if (next != summaries[fn]) {
          summaries[fn] = std::move(next);
          for (auto caller : callers[fn])
            if (!queued[caller]) {
              queued[caller] = true;
              pending.push_back(caller);
            }
        }
      }
      for (FunctionId fn = 0; fn < module.functions.size(); ++fn)
        analyze(fn, true);
    } catch (const Exhausted &) {
      diagnostics.push_back(
          {"S077", "array access proof exceeded its bounded analysis budget", {}});
    }
    return diagnostics;
  }

private:
  const TypedModule &module;
  std::vector<Summary> summaries;
  std::optional<FunctionInvocationFlow> flow;
  std::vector<Diagnostic> diagnostics;
  std::size_t budget = 1000000, depth = 0;
  bool checking = false;
  std::map<std::size_t, bool> writable;
  void spend(std::size_t count = 1) {
    if (count > budget)
      throw Exhausted{};
    budget -= count;
  }
  void unite(Roots &to, const Roots &from) {
    spend(from.size());
    to.insert(from.begin(), from.end());
  }
  Environment fork(const Environment &from) {
    spend(from.values.size() + from.symbols.size() + from.readers.size());
    return from;
  }
  bool storage(const ValueType &type) {
    if (type.array && (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool))
      return true;
    if (type.builtin == BuiltinValue::Function)
      return true; // Its environment may contain arrays, regardless of signature.
    std::vector<std::size_t> pending;
    if (type.record)
      pending.push_back(*type.record);
    if (type.errorRecord)
      pending.push_back(*type.errorRecord);
    std::set<std::size_t> seen;
    while (!pending.empty()) {
      spend();
      const auto id = pending.back();
      pending.pop_back();
      if (!seen.insert(id).second)
        continue;
      if (id >= module.records.size())
        return true;
      for (const auto &field : module.records[id].fields) {
        const auto &value = field.type;
        if ((value.array &&
             (value.builtin == BuiltinValue::Numeric || value.builtin == BuiltinValue::Bool)) ||
            value.builtin == BuiltinValue::Function)
          return true;
        if (value.record)
          pending.push_back(*value.record);
      }
    }
    return false;
  }
  Roots fresh(ValueId id) {
    writable[id] = true;
    return {id};
  }
  bool overlap(const Roots &a, const Roots &b) {
    if (a.empty() || b.empty())
      return false;
    if (a.contains(unknown) || b.contains(unknown))
      return true;
    for (auto root : a) {
      if (b.contains(root))
        return true;
      // Different incoming parameters can refer to the same backing. Only
      // local fresh storage is automatically disjoint from caller-owned data.
      if (root >= module.expressions.size())
        for (auto other : b)
          if (other >= module.expressions.size())
            return true;
    }
    return false;
  }
  void write(ValueId id, const Roots &roots, const Environment &env, bool spawned) {
    if (!checking || diagnostics.size() >= 32)
      return;
    if (spawned) {
      diagnostics.push_back(
          {"S077", "spawned calls receive read-only arrays; create private storage inside the task",
           module.expressions[id].span});
      return;
    }
    if (roots.empty() || std::ranges::any_of(roots, [&](auto root) {
          return root == unknown || !writable.contains(root) || !writable.at(root);
        })) {
      diagnostics.push_back(
          {"S077",
           "mutating parameter requires writable array storage; parameters and captures are "
           "read-only unless explicitly mut; use copy for independent storage",
           module.expressions[id].span});
      return;
    }
    for (const auto &[reader, protectedRoots] : env.readers)
      if (overlap(roots, protectedRoots)) {
        const auto [line, column] = module.source->location(module.expressions[reader].span.begin);
        diagnostics.push_back({"S077",
                               "can't mutate array while task at " + std::to_string(line) + ":" +
                                   std::to_string(column) +
                                   " may still read it; await every reader or use copy",
                               module.expressions[id].span});
        return;
      }
  }
  Roots returned(FunctionId target, ValueId call, const std::vector<Roots> &args) {
    if (target >= summaries.size())
      return {unknown};
    Roots result;
    const auto &summary = summaries[target];
    if (summary.fresh)
      result = fresh(call);
    if (summary.uncertain)
      result.insert(unknown);
    for (auto parameter : summary.parameters)
      if (parameter < args.size())
        unite(result, args[parameter]);
      else
        result.insert(unknown);
    return result;
  }
  void joinSymbols(Environment &to, const Environment &from) {
    for (auto &[symbol, roots] : to.symbols)
      if (auto found = from.symbols.find(symbol); found != from.symbols.end())
        unite(roots, found->second);
  }
  Roots body(const std::vector<TypedStatement> &statements, ValueId result, Environment &env) {
    for (const auto &statement : statements) {
      auto value = evaluate(statement.value, env);
      if (statement.binding)
        env.symbols[*statement.binding] = std::move(value);
    }
    return evaluate(result, env);
  }
  void joinReaders(Environment &into, const Environment &other) {
    for (const auto &[reader, roots] : other.readers)
      unite(into.readers[reader], roots);
  }
  Roots analyze(FunctionId id, bool validate) {
    spend();
    checking = validate;
    writable.clear();
    Environment env;
    const auto &function = module.functions[id];
    for (auto symbol : function.parameters)
      if (storage(module.symbols[symbol].type)) {
        const auto root = module.expressions.size() + symbol;
        env.symbols[symbol] = {root};
        writable[root] = module.symbols[symbol].mutableAccess;
      }
    return body(function.body, function.result, env);
  }
  Roots evaluate(ValueId id, Environment &env, bool spawned = false) {
    spend();
    if (id >= module.expressions.size() || depth >= 512)
      throw Exhausted{};
    if (auto found = env.values.find(id); found != env.values.end())
      return found->second;
    struct Depth {
      std::size_t &value;
      Depth(std::size_t &v) : value(v) { ++value; }
      ~Depth() { --value; }
    } guard(depth);
    const auto &node = module.expressions[id];
    Roots inputs;
    const auto input = [&](ValueId value) {
      auto roots = evaluate(value, env);
      unite(inputs, roots);
      return roots;
    };
    auto result = std::visit(
        [&](const auto &op) -> Roots {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, SymbolRead>) {
            if (auto found = env.symbols.find(op.symbol); found != env.symbols.end())
              return found->second;
            return storage(node.type) ? Roots{unknown} : Roots{};
          } else if constexpr (std::is_same_v<T, FunctionCall>) {
            std::vector<Roots> args;
            for (auto value : op.arguments)
              args.push_back(input(value));
            const auto &function = module.functions.at(op.function);
            for (std::size_t i = 0; i < args.size(); ++i)
              if (module.symbols[function.parameters.at(i)].mutableAccess)
                write(op.arguments[i], args[i], env, spawned);
            return storage(node.type) ? returned(op.function, id, args) : Roots{};
          } else if constexpr (std::is_same_v<T, ArraySort>) {
            auto destination = input(op.input);
            if (op.copying)
              return fresh(id);
            write(op.input, destination, env, spawned);
            return destination;
          } else if constexpr (std::is_same_v<T, ArrayStore>) {
            auto destination = input(op.input);
            input(op.index); // Checked before the RHS, including its faults.
            input(op.value);
            write(op.input, destination, env, false);
            return {};
          } else if constexpr (std::is_same_v<T, FunctionInvoke>) {
            auto captures = input(op.callee);
            const auto &type = module.expressions[op.callee].type;
            std::vector<Roots> args;
            for (std::size_t i = 0; i < op.arguments.size(); ++i) {
              auto roots = input(op.arguments[i]);
              if (type.callable && type.callable->mutates(i))
                write(op.arguments[i], roots, env, spawned);
              args.push_back(std::move(roots));
            }
            if (!storage(node.type))
              return {};
            if (!flow || flow->unknown[id] || flow->targets[id].empty())
              return {unknown};
            Roots result;
            for (auto target : flow->targets[id]) {
              auto actuals = args;
              // Aggregate/function-array projections conservatively carry the
              // union of their reachable captures. This cannot invent access.
              actuals.resize(module.functions[target].parameters.size(), captures);
              unite(result, returned(target, id, actuals));
            }
            return result;
          } else if constexpr (std::is_same_v<T, TaskOperation>) {
            auto value = evaluate(op.operand, env, op.prefix == TaskPrefix::Spawn);
            if (op.prefix == TaskPrefix::Spawn)
              env.readers[id] = env.inputs[op.operand];
            else if (op.prefix == TaskPrefix::Await) {
              // Task identity survives a local alias. Cancellation alone never
              // reaches this settlement edge and therefore never removes a reader.
              const auto identity = module.expressions[op.operand].type.identity;
              std::erase_if(env.readers, [&](const auto &reader) {
                return module.expressions[reader.first].type.identity == identity;
              });
            }
            return value;
          } else if constexpr (std::is_same_v<T, ParallelOperation>) {
            const auto incoming = env.readers;
            auto value = body(op.body, op.result, env);
            std::erase_if(env.readers,
                          [&](const auto &reader) { return !incoming.contains(reader.first); });
            return value;
          } else if constexpr (std::is_same_v<T, ConditionalOperation>) {
            input(op.condition);
            auto yes = fork(env), no = fork(env);
            auto value = body(op.whenTrue, op.trueResult, yes);
            unite(value, body(op.whenFalse, op.falseResult, no));
            for (auto &[symbol, roots] : env.symbols) {
              roots = yes.symbols.at(symbol);
              unite(roots, no.symbols.at(symbol));
            }
            env.readers = std::move(yes.readers);
            joinReaders(env, no);
            return value;
          } else if constexpr (std::is_same_v<T, WhileOperation>) {
            // Monotonic origins/readers include the zero-iteration edge. A
            // later replacement can expose a readonly origin on a backedge.
            // Rebuild expression caches for each abstract iteration; names
            // keep their current origin sets, old captured values keep theirs.
            for (;;) {
              spend();
              spend(env.symbols.size() + env.readers.size());
              const auto previousSymbols = env.symbols;
              const auto previousReaders = env.readers;
              auto iteration = fork(env);
              iteration.values.clear();
              iteration.inputs.clear();
              unite(inputs, evaluate(op.condition, iteration));
              for (const auto &statement : op.body) {
                auto value = evaluate(statement.value, iteration);
                if (statement.binding)
                  iteration.symbols[*statement.binding] = std::move(value);
              }
              joinSymbols(env, iteration);
              joinReaders(env, iteration);
              if (env.symbols == previousSymbols && env.readers == previousReaders)
                break;
            }
            return {};
          } else if constexpr (std::is_same_v<T, GuardOperation>) {
            input(op.condition);
            auto stopped = fork(env);
            body(op.body, op.result, stopped);
            return {};
          } else if constexpr (std::is_same_v<T, CatchOperation>) {
            auto value = input(op.operand);
            auto recovered = fork(env);
            unite(value, body(op.body, op.result, recovered));
            joinSymbols(env, recovered);
            joinReaders(env, recovered);
            return value;
          } else if constexpr (std::is_same_v<T, MatchOperation>) {
            input(op.subject);
            Roots value;
            std::map<ValueId, Roots> readers;
            auto incoming = fork(env);
            for (auto &[symbol, roots] : env.symbols)
              roots.clear();
            for (const auto &arm : op.cases) {
              auto branch = fork(incoming);
              unite(value, body(arm.body, arm.result, branch));
              joinSymbols(env, branch);
              for (const auto &[reader, roots] : branch.readers)
                unite(readers[reader], roots);
            }
            env.readers = std::move(readers);
            return value;
          } else {
            if constexpr (std::is_same_v<T, RecordValue>) {
              if (op.base)
                input(*op.base);
              for (const auto &field : op.fields)
                input(field.value);
            } else {
              if constexpr (requires { op.input; })
                input(op.input);
              if constexpr (requires { op.operand; })
                input(op.operand);
              if constexpr (requires { op.subject; })
                input(op.subject);
              if constexpr (requires { op.error; })
                input(op.error);
              if constexpr (requires {
                              op.left;
                              op.right;
                            }) {
                input(op.left);
                input(op.right);
              }
              if constexpr (requires {
                              op.start;
                              op.end;
                            }) {
                input(op.start);
                input(op.end);
              }
              if constexpr (requires { op.index; })
                input(op.index);
              if constexpr (requires { op.elements; })
                for (auto value : op.elements)
                  input(value);
              if constexpr (requires { op.captures; })
                for (auto value : op.captures)
                  input(value);
              if constexpr (requires { op.indices; })
                for (auto value : op.indices)
                  input(value);
              if constexpr (requires { op.dimensions; }) {
                if constexpr (std::is_same_v<T, ArrayToTensor>) {
                  if (op.dimensions)
                    for (auto value : *op.dimensions)
                      input(value);
                } else
                  for (auto value : op.dimensions)
                    input(value);
              }
              if constexpr (requires { op.epsilon; })
                if (op.epsilon)
                  input(*op.epsilon);
              if constexpr (std::is_same_v<T, Transfer>)
                input(op.target);
            }
            if (!storage(node.type))
              return {};
            if constexpr (std::is_same_v<T, ArrayConcat> || std::is_same_v<T, ArraySlice> ||
                          std::is_same_v<T, ArrayMap> || std::is_same_v<T, TensorLiteral> ||
                          std::is_same_v<T, TensorToArray>) {
              if (node.type.array && (node.type.builtin == BuiltinValue::Numeric ||
                                      node.type.builtin == BuiltinValue::Bool))
                return fresh(id);
            }
            if constexpr (std::is_same_v<T, CollectionQuery>)
              if (op.kind == CollectionQueryKind::Shape)
                return fresh(id);
            return inputs;
          }
        },
        node.operation);
    env.inputs[id] = std::move(inputs);
    env.values[id] = result;
    return result;
  }
};
} // namespace
std::vector<Diagnostic> arrayAccessDiagnostics(const TypedModule &module) {
  return AccessProof(module).run();
}
} // namespace kalandra
