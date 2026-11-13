#include "ArrayAccess.h"
#include <algorithm>
#include <compare>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace kalandra {
namespace {
// Storage identity must not depend on the current expression/symbol counts.
// Distinct kinds keep a cursor's control cell separate from retained data, and
// let loop aging preserve both identities without arithmetic namespace overlap.
class StorageRoot {
public:
  enum class Kind : std::uint64_t {
    Local,
    Parameter,
    OldLocal,
    Data,
    OldData,
    CursorDataParameter,
    Unknown = 127
  };
  static constexpr StorageRoot make(Kind kind, std::size_t identity) {
    return StorageRoot((std::uint64_t(kind) << 56) | identity);
  }
  constexpr Kind kind() const { return Kind((bits_ >> 56) & 127); }
  constexpr std::size_t identity() const { return bits_ & ((UINT64_C(1) << 56) - 1); }
  constexpr bool borrowed() const { return bits_ >> 63; }
  constexpr StorageRoot borrow() const { return StorageRoot(bits_ | (UINT64_C(1) << 63)); }
  constexpr StorageRoot data() const { return StorageRoot(bits_ & ~(UINT64_C(1) << 63)); }
  constexpr StorageRoot aged() const {
    const auto next = kind() == Kind::Local  ? Kind::OldLocal
                      : kind() == Kind::Data ? Kind::OldData
                                             : kind();
    return StorageRoot((bits_ & (UINT64_C(1) << 63)) | (std::uint64_t(next) << 56) | identity());
  }
  auto operator<=>(const StorageRoot &) const = default;

private:
  explicit constexpr StorageRoot(std::uint64_t bits) : bits_(bits) {}
  std::uint64_t bits_;
};
static_assert(sizeof(StorageRoot) == sizeof(std::uint64_t));
using Roots = std::set<StorageRoot>;
constexpr auto unknown = StorageRoot::make(StorageRoot::Kind::Unknown, 0);
struct Exhausted {};
struct Summary {
  std::set<std::size_t> parameters, borrowedParameters, cursorDataParameters,
      borrowedCursorDataParameters;
  bool fresh = false, uncertain = false, borrowedFresh = false, borrowedUnknown = false;
  bool operator==(const Summary &) const = default;
};
struct ReadLoan {
  Roots roots, controls; // Nonempty controls identify a cursor's retained-data loan.
  bool published = false; // Yielded backing cannot be rewritten by the producer.
  bool operator==(const ReadLoan &) const = default;
};
struct LoopFlow;
struct Environment {
  std::unordered_map<ValueId, Roots> values, inputs;
  std::unordered_map<SymbolId, Roots> symbols;
  std::map<StorageRoot, ReadLoan> readers;
  std::unordered_map<SymbolId, Roots> nextSymbols;
  Roots exhausted;
  bool stopped = false;
  std::set<SymbolId> moved, definitelyMoved;
  std::map<std::size_t, LoopFlow *> loops;
  std::map<std::size_t, Roots *> collections;
};
struct LoopFlow {
  std::vector<Environment> breaks, continues;
  Roots result;
  std::set<SymbolId> symbols;
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
                 std::holds_alternative<BufferOperation>(v.operation) ||
                 std::holds_alternative<IteratorOperation>(v.operation) ||
                 std::holds_alternative<GeneratorCreate>(v.operation);
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
          else if (const auto *producer =
                       std::get_if<GeneratorCreate>(&module.expressions[id].operation))
            callers.at(producer->function).push_back(fn);
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
          if (root.data() == unknown)
            (root.borrowed() ? next.borrowedUnknown : next.uncertain) = true;
          else if (freshRoot(root))
            (root.borrowed() ? next.borrowedFresh : next.fresh) = true;
          else {
            const auto symbol = root.identity();
            const auto &parameters = module.functions[fn].parameters;
            const auto found = std::ranges::find(parameters, symbol);
            if (found == parameters.end())
              (root.borrowed() ? next.borrowedUnknown : next.uncertain) = true;
            else if (root.kind() == StorageRoot::Kind::CursorDataParameter)
              (root.borrowed() ? next.borrowedCursorDataParameters : next.cursorDataParameters)
                  .insert(found - parameters.begin());
            else
              (root.borrowed() ? next.borrowedParameters : next.parameters)
                  .insert(found - parameters.begin());
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
  std::optional<std::size_t> producerCollection;
  SourceSpan readerSpan(StorageRoot reader) const {
    return reader.kind() == StorageRoot::Kind::Parameter
               ? module.symbols.at(reader.identity()).declaration
               : module.expressions.at(reader.identity()).span;
  }
  std::string resourceName(const ValueType &type) const {
    return isIterator(module, type)     ? "Iterator"
           : isByteBuffer(module, type) ? "ByteBuffer"
                                        : "file handle";
  }
  std::map<StorageRoot, bool> writable;
  std::map<ValueId, std::set<ValueId>> loopAllocations;
  bool parameterRoot(StorageRoot root) const { return root.kind() == StorageRoot::Kind::Parameter; }
  bool freshRoot(StorageRoot root) const {
    return root.kind() == StorageRoot::Kind::Local || root.kind() == StorageRoot::Kind::OldLocal ||
           root.kind() == StorageRoot::Kind::Data || root.kind() == StorageRoot::Kind::OldData;
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
        if ((root.kind() == StorageRoot::Kind::Local || root.kind() == StorageRoot::Kind::Data) &&
            sites.contains(root.identity())) {
          const auto old = root.aged();
          writable[old.data()] = writable.at(root.data());
          root = old;
        }
        next.insert(root);
      }
      roots = std::move(next);
    };
    for (auto &[symbol, roots] : env.symbols)
      age(roots);
    for (auto &[reader, loan] : env.readers) {
      age(loan.roots);
      age(loan.controls);
    }
    for (auto &[symbol, roots] : env.nextSymbols)
      age(roots);
    age(env.exhausted);
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
  void unite(ReadLoan &to, const ReadLoan &from) {
    unite(to.roots, from.roots);
    unite(to.controls, from.controls);
    to.published |= from.published;
  }
  Roots cursorControl(const Roots &roots) {
    Roots result;
    for (auto root : roots)
      if (!root.borrowed())
        result.insert(root);
    return result;
  }
  Roots cursorData(const Roots &roots) {
    Roots result;
    for (auto root : roots)
      if (root.borrowed())
        result.insert(root.data());
    return result;
  }
  void releaseCursor(const Roots &roots, Environment &env) {
    const auto controls = cursorControl(roots);
    std::erase_if(env.readers, [&](const auto &entry) {
      // Control cells have exact identities: retained data aliasing is not a
      // reason to release another cursor's independent loan.
      return std::ranges::any_of(entry.second.controls,
                                 [&](auto root) { return controls.contains(root); });
    });
  }
  void rememberCursor(ValueId id, const Roots &roots, Environment &env) {
    auto data = cursorData(roots);
    const auto controls = cursorControl(roots);
    if (!data.empty() && (controls.empty() || !std::ranges::all_of(controls, [&](auto root) {
                            return env.exhausted.contains(root);
                          })))
      env.readers[StorageRoot::make(StorageRoot::Kind::Local, id)] =
          ReadLoan{std::move(data), cursorControl(roots)};
  }
  Roots intersect(const Roots &left, const Roots &right) {
    Roots result;
    spend(left.size() + right.size());
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                          std::inserter(result, result.end()));
    return result;
  }
  Roots nextWitness(ValueId id, const Environment &env, std::size_t depth = 0) {
    spend();
    if (depth >= 128 || id >= module.expressions.size())
      return {};
    const auto &node = module.expressions[id];
    if (!node.type.record || module.records[*node.type.record].standard != StandardRecord::Option ||
        node.type.task)
      return {};
    if (const auto *read = std::get_if<SymbolRead>(&node.operation)) {
      const auto found = env.nextSymbols.find(read->symbol);
      return found == env.nextSymbols.end() ? Roots{} : found->second;
    }
    if (const auto *task = std::get_if<TaskOperation>(&node.operation);
        task && task->prefix == TaskPrefix::Try)
      return nextWitness(task->operand, env, depth + 1);
    if (const auto *caught = std::get_if<CatchOperation>(&node.operation))
      return nextWitness(caught->operand, env, depth + 1);
    ValueId cursor;
    if (const auto *next = std::get_if<IteratorOperation>(&node.operation);
        next && next->action == IteratorAction::Next)
      cursor = next->arguments.at(0);
    else {
      const auto parameter = [&](FunctionId target) -> std::optional<std::size_t> {
        const auto &fn = module.functions.at(target);
        if (fn.result >= module.expressions.size())
          return {};
        const auto *next = std::get_if<IteratorOperation>(&module.expressions[fn.result].operation);
        if (!next || next->action != IteratorAction::Next || next->arguments.size() != 1)
          return {};
        const auto *read =
            std::get_if<SymbolRead>(&module.expressions[next->arguments[0]].operation);
        if (!read)
          return {};
        const auto found = std::ranges::find(fn.parameters, read->symbol);
        return found == fn.parameters.end()
                   ? std::nullopt
                   : std::optional<std::size_t>(found - fn.parameters.begin());
      };
      if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
        const auto index = parameter(call->function);
        if (!index || *index >= call->arguments.size())
          return {};
        cursor = call->arguments[*index];
      } else if (const auto *call = std::get_if<FunctionInvoke>(&node.operation)) {
        if (!flow || flow->unknown[id] || flow->targets[id].empty())
          return {};
        std::optional<std::size_t> index;
        for (auto target : flow->targets[id]) {
          const auto found = parameter(target);
          if (!found || (index && *index != *found))
            return {};
          index = found;
        }
        if (!index || *index >= call->arguments.size())
          return {};
        cursor = call->arguments[*index];
      } else
        return {};
    }
    const auto found = env.values.find(cursor);
    if (found == env.values.end())
      return {};
    auto controls = cursorControl(found->second);
    // A union of possible owners cannot prove that every owner exhausted: only
    // the dynamically selected one did. Keep those cases conservative.
    return controls.size() == 1 && !controls.contains(unknown) ? controls : Roots{};
  }
  void bindWitness(SymbolId symbol, ValueId value, Environment &env) {
    auto witness = nextWitness(value, env);
    if (witness.empty())
      env.nextSymbols.erase(symbol);
    else
      env.nextSymbols[symbol] = std::move(witness);
  }
  void proveExhausted(const Roots &controls, Environment &env) {
    if (controls.empty())
      return;
    releaseCursor(controls, env);
    unite(env.exhausted, controls);
  }
  void refineOptionBranch(ValueId condition, Environment &yes, Environment &no) {
    const auto *compare = std::get_if<ScalarCompare>(&module.expressions[condition].operation);
    if (!compare || compare->operation != ComparisonKind::Equal)
      return;
    const auto refine = [&](ValueId subject, ValueId tag) {
      const auto *field = std::get_if<RecordFieldRead>(&module.expressions[subject].operation);
      const auto *number = std::get_if<Int64Literal>(&module.expressions[tag].operation);
      if (!field || field->field != 0 || !number || (number->value != 1 && number->value != 2))
        return;
      const auto witness = nextWitness(field->input, yes);
      proveExhausted(witness, number->value == 1 ? yes : no);
    };
    refine(compare->left, compare->right);
    refine(compare->right, compare->left);
  }
  Environment fork(const Environment &from) {
    spend(from.values.size() + from.symbols.size() + from.readers.size() + from.nextSymbols.size() +
          from.exhausted.size());
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
    const auto root = StorageRoot::make(StorageRoot::Kind::Local, id);
    writable[root] = true;
    return {root};
  }
  bool overlap(const Roots &a, const Roots &b) {
    if (a.empty() || b.empty())
      return false;
    if (a.contains(unknown) || b.contains(unknown) || a.contains(unknown.borrow()) ||
        b.contains(unknown.borrow()))
      return true;
    for (auto tagged : a) {
      const auto root = tagged.data();
      if (b.contains(root) || b.contains(root.borrow()))
        return true;
      // Different incoming parameters can refer to the same backing. Only
      // local fresh storage is automatically disjoint from caller-owned data.
      if (parameterRoot(root) || root.kind() == StorageRoot::Kind::CursorDataParameter)
        for (auto taggedOther : b) {
          const auto other = taggedOther.data();
          if (parameterRoot(other) || other.kind() == StorageRoot::Kind::CursorDataParameter) {
            // Opaque control allocations cannot be contained in an array or
            // captured as its element storage. Hidden cursor data can alias
            // ordinary array parameters, even when their names differ.
            if ((parameterRoot(root) &&
                 isOwnedResource(module, module.symbols[root.identity()].type)) ||
                (parameterRoot(other) &&
                 isOwnedResource(module, module.symbols[other.identity()].type)))
              continue;
            return true;
          }
        }
    }
    return false;
  }
  void write(ValueId id, const Roots &roots, const Environment &env, bool spawned) {
    const bool file = isFileResource(module, module.expressions[id].type);
    const bool cursor = isIterator(module, module.expressions[id].type);
    const auto writableRoots = cursor ? cursorControl(roots) : roots;
    if (!checking || diagnostics.size() >= 32)
      return;
    if (spawned && !file) {
      diagnostics.push_back(
          {"S077",
           isIterator(module, module.expressions[id].type)
               ? "a spawned call cannot mutate a borrowed Iterator; transfer it with an own "
                 "parameter"
               : "spawned calls receive read-only arrays; create private storage inside the task",
           module.expressions[id].span});
      return;
    }
    if (writableRoots.empty() || std::ranges::any_of(writableRoots, [&](auto root) {
          return root == unknown || !writable.contains(root) || !writable.at(root);
        })) {
      diagnostics.push_back(
          {"S077",
           isOwnedResource(module, module.expressions[id].type)
               ? "mutating " + resourceName(module.expressions[id].type) +
                     " requires a local owner or an explicit mut parameter"
               : "mutating parameter requires writable array storage; parameters and captures are "
                 "read-only unless explicitly mut; use copy for independent storage",
           module.expressions[id].span});
      return;
    }
    for (const auto &[reader, loan] : env.readers)
      if (!(cursor && !loan.controls.empty()) && overlap(writableRoots, loan.roots)) {
        if (loan.published) {
          diagnostics.push_back(
              {"S077", "can't mutate backing after yielding it; yield copy(...) for a snapshot",
               module.expressions[id].span});
          return;
        }
        const auto [line, column] = module.source->location(readerSpan(reader).begin);
        diagnostics.push_back(
            {"S077",
             (!loan.controls.empty() ? "can't mutate array while iterator at "
              : file                 ? "can't access file handle while task at "
              : isByteBuffer(module, module.expressions[id].type)
                  ? "can't mutate ByteBuffer while task at "
                  : "can't mutate array while task at ") +
                 std::to_string(line) + ":" + std::to_string(column) +
                 (loan.controls.empty() ? " may still read it; await every reader or use copy"
                                        : " retains a read loan; close every cursor or use copy"),
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
          overlap(isIterator(module, module.expressions[value].type) ? cursorControl(roots) : roots,
                  isIterator(module, module.symbols[symbol].type) ? cursorControl(stored) : stored))
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
             resourceName(module.expressions[use.value].type) +
                 (isFileResource(module, module.expressions[use.value].type)
                      ? " was moved or closed while evaluating a later argument"
                      : " was moved or consumed while evaluating a later argument"),
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
    const auto append = [&](const auto &parameters, bool borrowed, bool dataOnly) {
      for (auto parameter : parameters) {
        const auto actual = parameter < args.size()
                                ? (dataOnly ? cursorData(args[parameter]) : args[parameter])
                                : Roots{unknown};
        for (auto root : actual)
          result.insert(borrowed ? root.borrow() : root);
      }
    };
    append(summary.parameters, false, false);
    append(summary.borrowedParameters, true, false);
    append(summary.cursorDataParameters, false, true);
    append(summary.borrowedCursorDataParameters, true, true);
    if (summary.borrowedFresh) {
      auto root = StorageRoot::make(StorageRoot::Kind::Data, call);
      writable[root] = true;
      result.insert(root.borrow());
    }
    if (summary.borrowedUnknown)
      result.insert(unknown.borrow());
    return result;
  }
  void joinSymbols(Environment &to, const Environment &from) {
    std::erase_if(to.nextSymbols, [&](const auto &entry) {
      const auto found = from.nextSymbols.find(entry.first);
      return found == from.nextSymbols.end() || found->second != entry.second;
    });
    to.exhausted = intersect(to.exhausted, from.exhausted);
    to.moved.insert(from.moved.begin(), from.moved.end());
    std::erase_if(to.definitelyMoved,
                  [&](auto symbol) { return !from.definitelyMoved.contains(symbol); });
    for (auto &[symbol, roots] : to.symbols)
      if (auto found = from.symbols.find(symbol); found != from.symbols.end())
        unite(roots, found->second);
  }
  void consume(ValueId id, const Roots &roots, Environment &env) {
    // A task handle observes a future resource; binding or aliasing the handle
    // does not claim that resource. An await result performs the owning move.
    if (module.expressions[id].type.task)
      return;
    if (!isOwnedResource(module, module.expressions[id].type))
      return;
    const auto controls =
        isIterator(module, module.expressions[id].type) ? cursorControl(roots) : roots;
    for (auto root : controls) {
      if (parameterRoot(root)) {
        const auto symbol = root.identity();
        if (isOwnedResource(module, module.symbols[symbol].type) && !module.symbols[symbol].owned &&
            checking)
          diagnostics.push_back({"S081",
                                 "can't move a borrowed " +
                                     resourceName(module.expressions[id].type) +
                                     "; declare an own parameter to transfer ownership",
                                 module.expressions[id].span});
      }
    }
    write(id, roots, env, false);
    for (const auto &[symbol, value] : env.symbols)
      if (isOwnedResource(module, module.symbols[symbol].type) &&
          overlap(controls,
                  isIterator(module, module.symbols[symbol].type) ? cursorControl(value) : value)) {
        env.moved.insert(symbol);
        env.definitelyMoved.insert(symbol);
      }
  }
  std::set<SymbolId> visibleSymbols(const Environment &env) {
    spend(env.symbols.size());
    std::set<SymbolId> result;
    for (const auto &[symbol, roots] : env.symbols)
      result.insert(symbol);
    return result;
  }
  void retireCursorLoans(Environment &env, const Roots &escaping = {},
                         const std::set<SymbolId> *visible = nullptr, bool retireBindings = true) {
    spend(env.symbols.size() + env.readers.size());
    auto live = cursorControl(escaping);
    for (const auto &[symbol, roots] : env.symbols)
      if ((!visible || visible->contains(symbol)) && !env.definitelyMoved.contains(symbol) &&
          isIterator(module, module.symbols[symbol].type) && !module.symbols[symbol].type.task)
        unite(live, cursorControl(roots));
    std::erase_if(env.readers, [&](const auto &reader) {
      // Task read loans (no cursor controls) end only at settlement. A union of
      // possible cursor owners remains live if any owner can still be reached.
      return !reader.second.controls.empty() &&
             std::ranges::none_of(reader.second.controls,
                                  [&](auto root) { return live.contains(root); });
    });
    if (visible && retireBindings) {
      std::erase_if(env.symbols,
                    [&](const auto &entry) { return !visible->contains(entry.first); });
      std::erase_if(env.nextSymbols,
                    [&](const auto &entry) { return !visible->contains(entry.first); });
    }
  }
  Roots escapingCursor(ValueId id, const Roots &roots) const {
    return !module.expressions[id].type.task && isIterator(module, module.expressions[id].type)
               ? roots
               : Roots{};
  }
  Roots body(const std::vector<TypedStatement> &statements, ValueId result, Environment &env) {
    const auto visible = visibleSymbols(env);
    for (const auto &statement : statements) {
      if (env.stopped)
        return {};
      auto value = evaluate(statement.value, env);
      if (statement.binding) {
        consume(statement.value, value, env);
        bindWitness(*statement.binding, statement.value, env);
        env.symbols[*statement.binding] = value;
        env.moved.erase(*statement.binding);
        env.definitelyMoved.erase(*statement.binding);
      }
      retireCursorLoans(env, statement.value == result ? escapingCursor(result, value) : Roots{});
    }
    if (env.stopped)
      return {};
    auto roots = evaluate(result, env);
    consume(result, roots, env);
    retireCursorLoans(env, escapingCursor(result, roots), &visible);
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
    producerCollection = function.generator ? std::optional(function.generator->collection)
                                            : function.publicationCollection;
    Roots published;
    if (function.generator)
      env.collections[function.generator->collection] = &published;
    for (auto symbol : function.parameters)
      if (storage(module.symbols[symbol].type)) {
        const auto root = StorageRoot::make(StorageRoot::Kind::Parameter, symbol);
        env.symbols[symbol] = {root};
        writable[root] = module.symbols[symbol].mutableAccess || module.symbols[symbol].owned ||
                         isFileResource(module, module.symbols[symbol].type);
        if (isIterator(module, module.symbols[symbol].type)) {
          const auto data = StorageRoot::make(StorageRoot::Kind::CursorDataParameter, symbol);
          writable[data] = false;
          env.symbols[symbol].insert(data.borrow());
          env.readers[root] = ReadLoan{{data}, {root}};
        }
      }
    auto result = body(function.body, function.result, env);
    unite(result, published);
    return result;
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
                   resourceName(node.type) + " '" + module.symbols[op.symbol].name +
                       (isFileResource(module, node.type) ? "' was moved or closed"
                                                          : "' was moved or consumed"),
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
                  if (overlap(isIterator(module, parameter.type) ? cursorControl(args[i]) : args[i],
                              isIterator(module, module.symbols[function.parameters.at(j)].type)
                                  ? cursorControl(args[j])
                                  : args[j]) &&
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
            for (std::size_t i = 0; i < args.size(); ++i)
              if (module.symbols[function.parameters.at(i)].owned &&
                  isIterator(module, module.symbols[function.parameters.at(i)].type))
                releaseCursor(args[i], env);
            return storage(node.type) ? returned(op.function, id, args) : Roots{};
          } else if constexpr (std::is_same_v<T, GeneratorCreate>) {
            std::vector<Roots> captures;
            std::vector<ResourceUse> uses;
            for (auto capture : op.captures) {
              captures.push_back(input(capture));
              rememberResource(capture, captures.back(), env, uses);
            }
            validateResources(uses, env);
            auto data = returned(op.function, id, captures);
            auto result = fresh(id);
            for (std::size_t index = 0; index < captures.size(); ++index) {
              unite(data, captures[index]);
              consume(op.captures[index], captures[index], env);
            }
            for (auto root : data)
              result.insert(root.borrow());
            return result;
          } else if constexpr (std::is_same_v<T, IteratorOperation>) {
            std::vector<Roots> args;
            std::vector<ResourceUse> uses;
            for (auto argument : op.arguments) {
              args.push_back(input(argument));
              rememberResource(argument, args.back(), env, uses);
            }
            validateResources(uses, env);
            if (op.action == IteratorAction::FromArray) {
              auto result = fresh(id);
              for (auto root : args.at(0))
                result.insert(root.borrow());
              return result;
            }
            if (op.action != IteratorAction::FromRange && !args.empty()) {
              write(op.arguments[0], args[0], env, spawned);
              if (op.action == IteratorAction::Close) {
                consume(op.arguments[0], args[0], env);
                releaseCursor(args[0], env);
              }
            }
            if (op.action == IteratorAction::Next && storage(node.type))
              return cursorData(args.at(0));
            return storage(node.type) ? fresh(id) : Roots{};
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
                  if (overlap(isIterator(module, type.callable->parameters.at(i))
                                  ? cursorControl(roots)
                                  : roots,
                              isIterator(module, type.callable->parameters.at(j))
                                  ? cursorControl(args[j])
                                  : args[j]) &&
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
            if (type.callable)
              for (std::size_t i = 0; i < args.size(); ++i)
                if (type.callable->owns(i) && isIterator(module, type.callable->parameters.at(i)))
                  releaseCursor(args[i], env);
            if (!storage(node.type))
              return {};
            if (!flow || flow->unknown[id] || flow->targets[id].empty())
              return isIterator(module, node.type) ? Roots{unknown, unknown.borrow()}
                                                   : Roots{unknown};
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
              env.readers[StorageRoot::make(StorageRoot::Kind::Local, id)] =
                  ReadLoan{env.inputs[op.operand], {}};
            else if (op.prefix == TaskPrefix::Await) {
              // Task identity survives a local alias. Cancellation alone never
              // reaches this settlement edge and therefore never removes a reader.
              const auto identity = module.expressions[op.operand].type.identity;
              std::erase_if(env.readers, [&](const auto &reader) {
                return !reader.second.published && reader.second.controls.empty() &&
                       reader.first.kind() == StorageRoot::Kind::Local &&
                       module.expressions[reader.first.identity()].type.identity == identity;
              });
            }
            return value;
          } else if constexpr (std::is_same_v<T, ParallelOperation>) {
            const auto incoming = env.readers;
            auto value = body(op.body, op.result, env);
            std::erase_if(env.readers, [&](const auto &reader) {
              return !reader.second.published && reader.second.controls.empty() &&
                     !incoming.contains(reader.first);
            });
            return value;
          } else if constexpr (std::is_same_v<T, ConditionalOperation>) {
            input(op.condition);
            auto yes = fork(env), no = fork(env);
            refineOptionBranch(op.condition, yes, no);
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
            if (yes.stopped && !no.stopped) {
              env.moved = no.moved;
              env.definitelyMoved = no.definitelyMoved;
            } else if (no.stopped && !yes.stopped) {
              env.moved = yes.moved;
              env.definitelyMoved = yes.definitelyMoved;
            } else {
              env.moved = yes.moved;
              env.moved.insert(no.moved.begin(), no.moved.end());
              env.definitelyMoved = yes.definitelyMoved;
              std::erase_if(env.definitelyMoved,
                            [&](auto symbol) { return !no.definitelyMoved.contains(symbol); });
            }
            if (yes.stopped && !no.stopped) {
              env.nextSymbols = no.nextSymbols;
              env.exhausted = no.exhausted;
            } else if (no.stopped && !yes.stopped) {
              env.nextSymbols = yes.nextSymbols;
              env.exhausted = yes.exhausted;
            } else {
              env.nextSymbols = yes.nextSymbols;
              env.exhausted = intersect(yes.exhausted, no.exhausted);
              std::erase_if(env.nextSymbols, [&](const auto &entry) {
                const auto found = no.nextSymbols.find(entry.first);
                return found == no.nextSymbols.end() || found->second != entry.second;
              });
            }
            env.stopped = yes.stopped && no.stopped;
            return value;
          } else if constexpr (std::is_same_v<T, YieldOperation>) {
            const auto found = env.collections.find(op.collection);
            if (found == env.collections.end())
              throw Exhausted{};
            unite(*found->second, input(op.operand));
            if (producerCollection == op.collection)
              unite(env.readers[StorageRoot::make(StorageRoot::Kind::Local, id)],
                    ReadLoan{inputs, {}, true});
            return {};
          } else if constexpr (std::is_same_v<T, LoopTransferOperation>) {
            auto found = env.loops.find(op.loop);
            if (found == env.loops.end())
              throw Exhausted{};
            auto &loop = *found->second;
            Roots escaped;
            if (op.value) {
              auto result = input(*op.value);
              consume(*op.value, result, env);
              unite(loop.result, result);
              escaped = escapingCursor(*op.value, result);
            }
            // Keep symbol-table keys for joins with other stopped branches;
            // the target boundary owns the snapshot's escaped result.
            retireCursorLoans(env, escaped, &loop.symbols, false);
            (op.continuing ? loop.continues : loop.breaks).push_back(fork(env));
            env.stopped = true;
            return {};
          } else if constexpr (std::is_same_v<T, WhileOperation>) {
            const auto beforeLoop = visibleSymbols(env);
            const auto statements = [&](const auto &items, Environment &path,
                                        std::optional<ValueId> result = {}) {
              for (const auto &statement : items) {
                if (path.stopped)
                  break;
                auto roots = evaluate(statement.value, path);
                if (statement.binding) {
                  consume(statement.value, roots, path);
                  bindWitness(*statement.binding, statement.value, path);
                  path.symbols[*statement.binding] = roots;
                  path.moved.erase(*statement.binding);
                  path.definitelyMoved.erase(*statement.binding);
                }
                retireCursorLoans(path, result == statement.value
                                            ? escapingCursor(statement.value, roots)
                                            : Roots{});
              }
            };
            statements(op.initial, env);
            const auto &sites = allocationSites(id);
            auto head = fork(env);
            LoopFlow paths;
            paths.symbols = visibleSymbols(env);
            Roots collected;
            if (op.collecting)
              head.collections[op.identity] = &collected;
            for (;;) {
              spend();
              spend(head.symbols.size() + head.readers.size());
              const auto previousSymbols = head.symbols;
              const auto previousReaders = head.readers;
              const auto previousMoved = head.moved;
              const auto previousDefinitelyMoved = head.definitelyMoved;
              const auto previousWitnesses = head.nextSymbols;
              const auto previousExhausted = head.exhausted;
              auto iteration = fork(head);
              iteration.values.clear();
              iteration.inputs.clear();
              iteration.loops[op.identity] = &paths;
              ageLoopStorage(iteration, sites);
              unite(inputs, evaluate(op.condition, iteration));
              retireCursorLoans(iteration);
              statements(op.iteration, iteration);
              statements(op.body, iteration);
              if (!iteration.stopped) {
                retireCursorLoans(iteration, {}, &paths.symbols);
                joinSymbols(head, iteration);
                joinReaders(head, iteration);
              }
              for (const auto &path : paths.continues) {
                joinSymbols(head, path);
                joinReaders(head, path);
              }
              paths.continues.clear();
              if (head.symbols == previousSymbols && head.readers == previousReaders &&
                  head.moved == previousMoved && head.definitelyMoved == previousDefinitelyMoved &&
                  head.nextSymbols == previousWitnesses && head.exhausted == previousExhausted)
                break;
            }
            auto normal = fork(head);
            if (const auto *condition =
                    std::get_if<BoolLiteral>(&module.expressions[op.condition].operation);
                condition && condition->value)
              normal.stopped = true;
            normal.values.clear();
            normal.inputs.clear();
            statements(op.whenDone, normal, op.normalResult);
            Roots value = paths.result;
            if (op.traversalErrorBoundary) {
              // A next failure exits before the normal-completion suite. Its
              // error payload may retain backing read through this cursor.
              paths.breaks.push_back(fork(head));
              unite(value, inputs);
            }
            if (op.normalResult && !normal.stopped)
              unite(value, evaluate(*op.normalResult, normal));
            if (!normal.stopped) {
              for (auto &[symbol, roots] : env.symbols)
                roots = normal.symbols.at(symbol);
              env.readers = normal.readers;
              env.moved = normal.moved;
              env.definitelyMoved = normal.definitelyMoved;
              env.nextSymbols = normal.nextSymbols;
              env.exhausted = normal.exhausted;
            } else {
              for (auto &[symbol, roots] : env.symbols)
                roots.clear();
              env.readers.clear();
              env.nextSymbols.clear();
              env.exhausted.clear();
            }
            bool continuation = !normal.stopped;
            for (const auto &path : paths.breaks) {
              if (!continuation) {
                env.nextSymbols = path.nextSymbols;
                env.exhausted = path.exhausted;
                continuation = true;
              }
              joinSymbols(env, path);
              joinReaders(env, path);
            }
            env.stopped = normal.stopped && paths.breaks.empty();
            retireCursorLoans(env, escapingCursor(id, value), &beforeLoop);
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
            proveExhausted(nextWitness(op.operand, recovered), recovered);
            unite(value, body(op.body, op.result, recovered));
            joinSymbols(env, recovered);
            joinReaders(env, recovered);
            return value;
          } else if constexpr (std::is_same_v<T, MatchOperation>) {
            input(op.subject);
            Roots value;
            auto incoming = fork(env);
            std::vector<Environment> continuing;
            const auto witness = nextWitness(op.subject, incoming);
            bool someSeen = false;
            for (const auto &arm : op.cases) {
              auto branch = fork(incoming);
              if (arm.tag == 1 || (!arm.tag && someSeen))
                proveExhausted(witness, branch);
              someSeen |= arm.tag == 2;
              unite(value, body(arm.body, arm.result, branch));
              if (!branch.stopped)
                continuing.push_back(std::move(branch));
            }
            if (continuing.empty()) {
              env.stopped = true;
              return value;
            }
            const auto &first = continuing.front();
            for (auto &[symbol, roots] : env.symbols)
              roots = first.symbols.at(symbol);
            env.readers = first.readers;
            env.moved = first.moved;
            env.definitelyMoved = first.definitelyMoved;
            env.nextSymbols = first.nextSymbols;
            env.exhausted = first.exhausted;
            for (std::size_t i = 1; i < continuing.size(); ++i) {
              joinSymbols(env, continuing[i]);
              joinReaders(env, continuing[i]);
            }
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
    if (isIterator(module, node.type) && !node.type.task)
      rememberCursor(id, result, env);
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
