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
struct LoopFlow;
struct Environment {
  std::unordered_map<ValueId, Roots> values, inputs;
  std::unordered_map<SymbolId, Roots> symbols;
  std::map<ValueId, Roots> readers;
  bool stopped = false;
  std::set<SymbolId> moved;
  std::map<std::size_t, LoopFlow *> loops;
  std::map<std::size_t, Roots *> collections;
};
struct LoopFlow {
  std::vector<Environment> breaks, continues;
  Roots result;
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
                 std::holds_alternative<ArraySort>(v.operation) ||
                 std::holds_alternative<FileOperation>(v.operation) ||
                 std::holds_alternative<BufferOperation>(v.operation);
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
          else if (freshRoot(root))
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
  std::map<ValueId, std::set<ValueId>> loopAllocations;
  std::size_t parameterEnd() const { return module.expressions.size() + module.symbols.size(); }
  bool parameterRoot(std::size_t root) const {
    return root >= module.expressions.size() && root < parameterEnd();
  }
  bool freshRoot(std::size_t root) const {
    return root < module.expressions.size() ||
           (root >= parameterEnd() && root - parameterEnd() < module.expressions.size());
  }
  // Recency is scoped to the loop's structural expression graph. Never age
  // unrelated allocations: a surrounding expression can hold a destination
  // snapshot while its RHS executes an inner loop.
  const std::set<ValueId> &allocationSites(ValueId loop) {
    if (auto found = loopAllocations.find(loop); found != loopAllocations.end())
      return found->second;
    std::set<ValueId> sites;
    std::vector<ValueId> pending{loop};
    while (!pending.empty()) {
      spend();
      const auto id = pending.back();
      pending.pop_back();
      if (!sites.insert(id).second)
        continue;
      if (id >= module.expressions.size())
        throw Exhausted{};
      std::visit(
          [&](const auto &op) {
            using T = std::decay_t<decltype(op)>;
            const auto add = [&](ValueId value) { pending.push_back(value); };
            const auto body = [&](const auto &statements) {
              for (const auto &statement : statements)
                add(statement.value);
            };
            if constexpr (requires { op.input; })
              add(op.input);
            if constexpr (requires { op.operand; })
              add(op.operand);
            if constexpr (requires { op.subject; })
              add(op.subject);
            if constexpr (requires { op.error; })
              add(op.error);
            if constexpr (requires { op.condition; })
              add(op.condition);
            if constexpr (requires { op.callee; })
              add(op.callee);
            if constexpr (requires { op.arguments; })
              for (auto v : op.arguments)
                add(v);
            if constexpr (requires {
                            op.left;
                            op.right;
                          }) {
              add(op.left);
              add(op.right);
            }
            if constexpr (requires {
                            op.start;
                            op.end;
                          }) {
              add(op.start);
              add(op.end);
            }
            if constexpr (requires { op.index; })
              add(op.index);
            if constexpr (requires { op.elements; })
              for (auto v : op.elements)
                add(v);
            if constexpr (requires { op.captures; })
              for (auto v : op.captures)
                add(v);
            if constexpr (requires { op.indices; })
              for (auto v : op.indices)
                add(v);
            if constexpr (requires { op.dimensions; }) {
              if constexpr (std::is_same_v<T, ArrayToTensor>) {
                if (op.dimensions)
                  for (auto v : *op.dimensions)
                    add(v);
              } else
                for (auto v : op.dimensions)
                  add(v);
            }
            if constexpr (requires { op.epsilon; })
              if (op.epsilon)
                add(*op.epsilon);
            if constexpr (std::is_same_v<T, Transfer>)
              add(op.target);
            if constexpr (std::is_same_v<T, TaskOperation>)
              if (op.placement)
                add(*op.placement);
            if constexpr (std::is_same_v<T, ArrayStore>)
              add(op.value);
            if constexpr (std::is_same_v<T, RecordValue>) {
              if (op.base)
                add(*op.base);
              for (const auto &field : op.fields)
                add(field.value);
            }
            if constexpr (requires { op.body; })
              body(op.body);
            if constexpr (std::is_same_v<T, WhileOperation>) {
              body(op.iteration);
            }
            if constexpr (std::is_same_v<T, LoopTransferOperation>)
              if (op.value)
                add(*op.value);
            if constexpr (requires { op.result; })
              add(op.result);
            if constexpr (std::is_same_v<T, ConditionalOperation>) {
              body(op.whenTrue);
              body(op.whenFalse);
              add(op.trueResult);
              add(op.falseResult);
            }
            if constexpr (std::is_same_v<T, MatchOperation>)
              for (const auto &arm : op.cases) {
                body(arm.body);
                add(arm.result);
              }
          },
          module.expressions[id].operation);
    }
    return loopAllocations.emplace(loop, std::move(sites)).first->second;
  }
  void ageLoopStorage(Environment &env, const std::set<ValueId> &sites) {
    const auto age = [&](Roots &roots) {
      spend(roots.size());
      Roots next;
      for (auto root : roots) {
        if (root < module.expressions.size() && sites.contains(root)) {
          const auto old = parameterEnd() + root;
          writable[old] = writable.at(root);
          root = old;
        }
        next.insert(root);
      }
      roots = std::move(next);
    };
    for (auto &[symbol, roots] : env.symbols)
      age(roots);
    for (auto &[reader, roots] : env.readers)
      age(roots);
  }
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
      if (parameterRoot(root))
        for (auto other : b)
          if (parameterRoot(other)) {
            const auto a = root - module.expressions.size(), b = other - module.expressions.size();
            if (isOwnedResource(module, module.symbols[a].type) &&
                isOwnedResource(module, module.symbols[b].type))
              continue;
            return true;
          }
    }
    return false;
  }
  void write(ValueId id, const Roots &roots, const Environment &env, bool spawned) {
    const bool file = isFileResource(module, module.expressions[id].type);
    if (!checking || diagnostics.size() >= 32)
      return;
    if (spawned && !file) {
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
           isByteBuffer(module, module.expressions[id].type)
               ? "mutating ByteBuffer requires a local owner or an explicit mut parameter"
               : "mutating parameter requires writable array storage; parameters and captures are "
                 "read-only unless explicitly mut; use copy for independent storage",
           module.expressions[id].span});
      return;
    }
    for (const auto &[reader, protectedRoots] : env.readers)
      if (overlap(roots, protectedRoots)) {
        const auto [line, column] = module.source->location(module.expressions[reader].span.begin);
        diagnostics.push_back({"S077",
                               (file ? "can't access file handle while task at "
                                : isByteBuffer(module, module.expressions[id].type)
                                    ? "can't mutate ByteBuffer while task at "
                                    : "can't mutate array while task at ") +
                                   std::to_string(line) + ":" + std::to_string(column) +
                                   " may still read it; await every reader or use copy",
                               module.expressions[id].span});
        return;
      }
  }
  struct ResourceUse {
    ValueId value;
    std::vector<SymbolId> lenders;
  };
  void rememberResource(ValueId value, const Roots &roots, const Environment &env,
                        std::vector<ResourceUse> &uses) {
    if (!checking || !isOwnedResource(module, module.expressions[value].type))
      return;
    ResourceUse use{value, {}};
    // A returned owning expression may share provenance with its consumed input.
    // Only bindings still lending this value after its evaluation are live loans.
    for (const auto &[symbol, stored] : env.symbols)
      if (!env.moved.contains(symbol) && isOwnedResource(module, module.symbols[symbol].type) &&
          overlap(roots, stored))
        use.lenders.push_back(symbol);
    uses.push_back(std::move(use));
  }
  void validateResources(const std::vector<ResourceUse> &uses, const Environment &env) {
    if (!checking || env.stopped)
      return;
    for (const auto &use : uses)
      if (std::ranges::any_of(use.lenders,
                              [&](auto symbol) { return env.moved.contains(symbol); })) {
        diagnostics.push_back(
            {"S081",
             isByteBuffer(module, module.expressions[use.value].type)
                 ? "ByteBuffer was moved or consumed while evaluating a later argument"
                 : "file handle was moved or closed while evaluating a later argument",
             module.expressions[use.value].span});
        if (diagnostics.size() >= 32)
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
    to.moved.insert(from.moved.begin(), from.moved.end());
    for (auto &[symbol, roots] : to.symbols)
      if (auto found = from.symbols.find(symbol); found != from.symbols.end())
        unite(roots, found->second);
  }
  void consume(ValueId id, const Roots &roots, Environment &env) {
    if (!isOwnedResource(module, module.expressions[id].type))
      return;
    for (auto root : roots) {
      if (parameterRoot(root)) {
        const auto symbol = root - module.expressions.size();
        if (isOwnedResource(module, module.symbols[symbol].type) && !module.symbols[symbol].owned &&
            checking)
          diagnostics.push_back({"S081",
                                 isByteBuffer(module, module.expressions[id].type)
                                     ? "can't move a borrowed ByteBuffer; declare an own parameter "
                                       "to transfer ownership"
                                     : "can't move a borrowed file handle; declare an own "
                                       "parameter to transfer ownership",
                                 module.expressions[id].span});
      }
    }
    write(id, roots, env, false);
    for (const auto &[symbol, value] : env.symbols)
      if (isOwnedResource(module, module.symbols[symbol].type) && overlap(roots, value))
        env.moved.insert(symbol);
  }
  Roots body(const std::vector<TypedStatement> &statements, ValueId result, Environment &env) {
    for (const auto &statement : statements) {
      if (env.stopped)
        return {};
      auto value = evaluate(statement.value, env);
      if (statement.binding) {
        consume(statement.value, value, env);
        env.symbols[*statement.binding] = std::move(value);
        env.moved.erase(*statement.binding);
      }
    }
    if (env.stopped)
      return {};
    auto roots = evaluate(result, env);
    consume(result, roots, env);
    return roots;
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
        writable[root] = module.symbols[symbol].mutableAccess || module.symbols[symbol].owned ||
                         isFileResource(module, module.symbols[symbol].type);
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
            if (checking && env.moved.contains(op.symbol))
              diagnostics.push_back(
                  {"S081",
                   (isByteBuffer(module, node.type) ? "ByteBuffer '" : "file handle '") +
                       module.symbols[op.symbol].name +
                       (isByteBuffer(module, node.type) ? "' was moved or consumed"
                                                        : "' was moved or closed"),
                   node.span});
            if (auto found = env.symbols.find(op.symbol); found != env.symbols.end())
              return found->second;
            return storage(node.type) ? Roots{unknown} : Roots{};
          } else if constexpr (std::is_same_v<T, FunctionCall>) {
            std::vector<Roots> args;
            std::vector<ResourceUse> uses;
            for (auto value : op.arguments) {
              args.push_back(input(value));
              rememberResource(value, args.back(), env, uses);
            }
            validateResources(uses, env);
            const auto &function = module.functions.at(op.function);
            for (std::size_t i = 0; i < args.size(); ++i) {
              const auto &parameter = module.symbols[function.parameters.at(i)];
              if (parameter.mutableAccess || isFileResource(module, parameter.type))
                write(op.arguments[i], args[i], env, spawned);
              if (parameter.owned)
                consume(op.arguments[i], args[i], env);
              if (checking && isOwnedResource(module, parameter.type))
                for (std::size_t j = 0; j < i; ++j)
                  if (overlap(args[i], args[j]) &&
                      (parameter.mutableAccess || parameter.owned ||
                       isFileResource(module, parameter.type) ||
                       module.symbols[function.parameters.at(j)].mutableAccess ||
                       module.symbols[function.parameters.at(j)].owned ||
                       isFileResource(module, module.symbols[function.parameters.at(j)].type)))
                    diagnostics.push_back(
                        {"S081",
                         isByteBuffer(module, parameter.type)
                             ? "a call cannot borrow the same ByteBuffer through mutable or owned "
                               "and other parameters"
                             : "a call cannot access the same file through multiple parameters",
                         node.span});
            }
            return storage(node.type) ? returned(op.function, id, args) : Roots{};
          } else if constexpr (std::is_same_v<T, BufferOperation>) {
            std::vector<Roots> args;
            std::vector<ResourceUse> uses;
            for (auto argument : op.arguments) {
              args.push_back(input(argument));
              rememberResource(argument, args.back(), env, uses);
            }
            validateResources(uses, env);
            if (op.action != BufferAction::Create && mutatesBuffer(op.action)) {
              write(op.arguments[0], args[0], env, spawned);
              if (op.action == BufferAction::IntoArray)
                consume(op.arguments[0], args[0], env);
            }
            return storage(node.type) ? fresh(id) : Roots{};
          } else if constexpr (std::is_same_v<T, FileOperation>) {
            std::vector<Roots> args;
            std::vector<ResourceUse> uses;
            for (auto argument : op.arguments) {
              args.push_back(input(argument));
              rememberResource(argument, args.back(), env, uses);
            }
            validateResources(uses, env);
            if (op.action != FileAction::Open && !args.empty()) {
              write(op.arguments[0], args[0], env, spawned);
              if (consumesFile(op.action))
                consume(op.arguments[0], args[0], env);
            }
            if (op.action == FileAction::ReadInto || op.action == FileAction::ReadIntoBuffer ||
                op.action == FileAction::ReadAppend)
              write(op.arguments[1], args[1], env, spawned);
            return storage(node.type) ? fresh(id) : Roots{};
          } else if constexpr (std::is_same_v<T, ConsoleOperation> ||
                               std::is_same_v<T, ParseOperation> ||
                               std::is_same_v<T, PathOperation> ||
                               std::is_same_v<T, EnvironmentLookup>) {
            input(op.input);
            return {};
          } else if constexpr (std::is_same_v<T, FormatOperation> ||
                               std::is_same_v<T, TimeOperation>) {
            for (auto value : op.arguments)
              input(value);
            return {};
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
            std::vector<ResourceUse> uses;
            for (std::size_t i = 0; i < op.arguments.size(); ++i) {
              auto roots = input(op.arguments[i]);
              // Existing callable-value moves happen as each owning argument is
              // evaluated. Its own transfer must not be mistaken for a later use.
              if (!type.callable || !type.callable->owns(i))
                rememberResource(op.arguments[i], roots, env, uses);
              if (type.callable && (type.callable->mutates(i) ||
                                    isFileResource(module, type.callable->parameters.at(i))))
                write(op.arguments[i], roots, env, spawned);
              if (type.callable && type.callable->owns(i))
                consume(op.arguments[i], roots, env);
              if (checking && type.callable &&
                  isOwnedResource(module, type.callable->parameters.at(i)))
                for (std::size_t j = 0; j < args.size(); ++j)
                  if (overlap(roots, args[j]) &&
                      (type.callable->mutates(i) || type.callable->owns(i) ||
                       isFileResource(module, type.callable->parameters.at(i)) ||
                       type.callable->mutates(j) || type.callable->owns(j) ||
                       isFileResource(module, type.callable->parameters.at(j))))
                    diagnostics.push_back(
                        {"S081",
                         isByteBuffer(module, type.callable->parameters.at(i))
                             ? "a call cannot borrow the same ByteBuffer through mutable or owned "
                               "and other parameters"
                             : "a call cannot access the same file through multiple parameters",
                         node.span});
              args.push_back(std::move(roots));
            }
            validateResources(uses, env);
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
              if (yes.stopped && !no.stopped)
                roots = no.symbols.at(symbol);
              else if (no.stopped && !yes.stopped)
                roots = yes.symbols.at(symbol);
              else {
                roots = yes.symbols.at(symbol);
                unite(roots, no.symbols.at(symbol));
              }
            }
            if (yes.stopped && !no.stopped)
              env.readers = no.readers;
            else if (no.stopped && !yes.stopped)
              env.readers = yes.readers;
            else {
              env.readers = yes.readers;
              joinReaders(env, no);
            }
            if (yes.stopped && !no.stopped)
              env.moved = no.moved;
            else if (no.stopped && !yes.stopped)
              env.moved = yes.moved;
            else {
              env.moved = yes.moved;
              env.moved.insert(no.moved.begin(), no.moved.end());
            }
            env.stopped = yes.stopped && no.stopped;
            return value;
          } else if constexpr (std::is_same_v<T, YieldOperation>) {
            const auto found = env.collections.find(op.collection);
            if (found == env.collections.end())
              throw Exhausted{};
            unite(*found->second, input(op.operand));
            return {};
          } else if constexpr (std::is_same_v<T, LoopTransferOperation>) {
            auto found = env.loops.find(op.loop);
            if (found == env.loops.end())
              throw Exhausted{};
            auto &loop = *found->second;
            if (op.value)
              unite(loop.result, input(*op.value));
            (op.continuing ? loop.continues : loop.breaks).push_back(fork(env));
            env.stopped = true;
            return {};
          } else if constexpr (std::is_same_v<T, WhileOperation>) {
            const auto statements = [&](const auto &items, Environment &path) {
              for (const auto &statement : items) {
                if (path.stopped)
                  break;
                auto roots = evaluate(statement.value, path);
                if (statement.binding) {
                  consume(statement.value, roots, path);
                  path.symbols[*statement.binding] = std::move(roots);
                  path.moved.erase(*statement.binding);
                }
              }
            };
            statements(op.initial, env);
            const auto &sites = allocationSites(id);
            auto head = fork(env);
            LoopFlow paths;
            Roots collected;
            if (op.collecting)
              head.collections[op.identity] = &collected;
            for (;;) {
              spend();
              spend(head.symbols.size() + head.readers.size());
              const auto previousSymbols = head.symbols;
              const auto previousReaders = head.readers;
              const auto previousMoved = head.moved;
              auto iteration = fork(head);
              iteration.values.clear();
              iteration.inputs.clear();
              iteration.loops[op.identity] = &paths;
              ageLoopStorage(iteration, sites);
              unite(inputs, evaluate(op.condition, iteration));
              statements(op.iteration, iteration);
              statements(op.body, iteration);
              if (!iteration.stopped) {
                joinSymbols(head, iteration);
                joinReaders(head, iteration);
              }
              for (const auto &path : paths.continues) {
                joinSymbols(head, path);
                joinReaders(head, path);
              }
              paths.continues.clear();
              if (head.symbols == previousSymbols && head.readers == previousReaders &&
                  head.moved == previousMoved)
                break;
            }
            auto normal = fork(head);
            normal.values.clear();
            normal.inputs.clear();
            statements(op.whenDone, normal);
            Roots value = paths.result;
            if (op.normalResult && !normal.stopped)
              unite(value, evaluate(*op.normalResult, normal));
            if (!normal.stopped) {
              for (auto &[symbol, roots] : env.symbols)
                roots = normal.symbols.at(symbol);
              env.readers = normal.readers;
              env.moved = normal.moved;
            } else {
              for (auto &[symbol, roots] : env.symbols)
                roots.clear();
              env.readers.clear();
            }
            for (const auto &path : paths.breaks) {
              joinSymbols(env, path);
              joinReaders(env, path);
            }
            env.stopped = normal.stopped && paths.breaks.empty();
            if (op.collecting) {
              unite(inputs, collected);
              unite(value, fresh(id));
              // The array backing is new; owned elements keep their aliases.
              unite(value, collected);
            }
            return value;
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
