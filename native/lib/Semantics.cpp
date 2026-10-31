#include "kalandra/Semantics.h"
#include "ErrorFamilies.h"
#include "RecordLayout.h"
#include "RecordStorage.h"
#include "RecursiveAdmission.h"
#include "kalandra/Continuation.h"
#include "kalandra/StringLiteral.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace kalandra {
namespace {
class Checker {
  enum class State { Unseen, Checking, Complete, Failed };
  struct TaskChecks {
    std::unordered_map<std::size_t, std::vector<ValueId>> children;
    std::unordered_set<std::size_t> observed, pending, handled;
  };
  struct Frame;
  struct ClosureScope {
    Frame *parent;
    std::unordered_map<std::string, SymbolId> captures;
    std::vector<SymbolId> origins;
  };
  struct Frame {
    FunctionId function;
    std::unordered_map<std::string, SymbolId> locals;
    std::size_t depth = 0;
    std::shared_ptr<TaskChecks> tasks = std::make_shared<TaskChecks>();
    std::vector<std::size_t> groups;
    std::vector<std::shared_ptr<ValueType>> groupErrors;
    std::shared_ptr<bool> handlerFallible;
    std::size_t handlerDepth = 0;
    bool conditional = false;
    std::shared_ptr<ClosureScope> closure = {};
    std::size_t enumBranchDepth = 0;
    std::shared_ptr<std::optional<std::size_t>> handlerErrorRecord = {};
  };
  struct TensorData {
    std::vector<std::uint64_t> shape;
    std::vector<ExprId> leaves;
  };

public:
  Checker(Module syntax, SemanticLimits limits, CheckOptions options)
      : syntax_(std::move(syntax)), limits_(limits), options_(options) {
    output_.source = syntax_.source;
    output_.functions.resize(syntax_.functions.size());
    states_.resize(syntax_.functions.size(), State::Unseen);
    signatures_.resize(syntax_.functions.size(), true);
    errorFamilies_.resize(syntax_.functions.size());
    requirements_.resize(syntax_.functions.size());
    literal_.resize(syntax_.expressions.size());
    floatingLiteral_.resize(syntax_.expressions.size());
    // AST children precede their parent. Compute literal-only context eligibility
    // once, without recursive probing or quadratic walks over long chains.
    for (ExprId id = 0; id < syntax_.expressions.size(); ++id) {
      const auto &value = syntax_.expressions[id].value;
      if (const auto *number = std::get_if<NumberExpr>(&value)) {
        literal_[id] = true;
        floatingLiteral_[id] =
            syntax_.source->slice(number->literal).find_first_of(".eE") != std::string_view::npos;
      } else if (const auto *unary = std::get_if<UnaryExpr>(&value)) {
        literal_[id] = unary->operation != TokenKind::Bang && literal_[unary->operand];
        floatingLiteral_[id] = floatingLiteral_[unary->operand];
      } else if (const auto *binary = std::get_if<BinaryExpr>(&value)) {
        literal_[id] = binary->operation != TokenKind::Arrow && literal_[binary->left] &&
                       literal_[binary->right];
        floatingLiteral_[id] = floatingLiteral_[binary->left] || floatingLiteral_[binary->right];
      }
    }
  }
  CheckResult run() {
    for (FunctionId id = 0; id < syntax_.functions.size(); ++id) {
      const auto &function = syntax_.functions[id];
      if (function.exported)
        exports_.emplace(functionKey(function.moduleIndex, name(function.name)),
                         ExportBinding{false, id});
    }
    for (const auto &import : syntax_.imports)
      if (import.exported && import.target)
        for (const auto &imported : import.names)
          reexports_.emplace(
              functionKey(import.moduleIndex, name(imported.alias.value_or(imported.name))),
              std::pair{*import.target, name(imported.name)});
    registerRecords();
    for (const auto &[key, id] : aliases_) {
      std::unordered_set<std::size_t> active;
      if (auto resolved = resolveAlias(id, active))
        records_[key] = *resolved;
    }
    if (!diagnostics_.empty())
      return {syntax_.source, {}, std::move(diagnostics_)};
    {
      // Nominal identities and fields are now complete. Resolve cycles before
      // function analysis; existing value/ownership walkers are not cycle-safe.
      auto storage = planRecordStorage(output_);
      if (!storage) {
        error(storage.error().code, storage.error().message, storage.error().span);
        return {syntax_.source, {}, std::move(diagnostics_)};
      }
      recursiveValues_ = hasRecursiveStorage(*storage);
      if (auto invalid = recursiveRecordLayoutError(output_, limits_)) {
        error(invalid->code, invalid->message, invalid->span);
        return {syntax_.source, {}, std::move(diagnostics_)};
      }
    }
    registerSignatures();
    if (diagnostics_.empty())
      if (auto invalid = recursiveRecordLayoutError(output_, limits_)) {
        error(invalid->code, invalid->message, invalid->span);
        return {syntax_.source, {}, std::move(diagnostics_)};
      }
    const auto site = options_.completion  ? std::optional(options_.completion->site)
                      : options_.signature ? std::optional(options_.signature->site)
                                           : std::nullopt;
    if (syntax_.editorOnly && site)
      for (FunctionId i = 0; i < syntax_.functions.size(); ++i) {
        const auto span = syntax_.functions[i].span;
        if (span.begin <= site->begin && site->end <= span.end) {
          analyzeFunction(i);
          break;
        }
      }
    for (FunctionId i = 0;
         i < syntax_.functions.size() && diagnostics_.size() < limits_.maxDiagnostics; ++i) {
      if (syntax_.editorOnly && ((options_.completion && options_.completion->reached) ||
                                 (options_.signature && options_.signature->snapshot)))
        break;
      analyzeFunction(i);
    }
    // Transport provenance needs complete immutable bindings, including statements
    // in the parallel/conditional body currently being checked.
    if (diagnostics_.empty())
      for (const auto &value : output_.expressions)
        if (const auto *task = std::get_if<TaskOperation>(&value.operation);
            task && task->prefix == TaskPrefix::Spawn)
          if (auto unsupported =
                  functionValueTransportError(output_, output_.expressions[task->operand]))
            error("S074", *unsupported, value.span);
    if (diagnostics_.empty())
      if (auto unsupported = numericCapabilityError(output_, NumericTarget::Cpu))
        error("S054", *unsupported,
              syntax_.functions.empty() ? SourceSpan{} : syntax_.functions[0].name);
    if (!diagnostics_.empty())
      return {syntax_.source, {}, std::move(diagnostics_)};
    recordImportOrigins();
    for (auto &[span, token] : sourceTokens_)
      output_.sourceTokens.push_back(std::move(token));
    return {syntax_.source, std::move(output_), {}};
  }

private:
  std::string name(SourceSpan span) const { return std::string(syntax_.source->slice(span)); }
  static std::string functionKey(std::size_t module, const std::string &name) {
    return std::to_string(module) + ":" + name;
  }
  std::string functionKey(const Frame &frame, const std::string &name) const {
    const Frame *owner = &frame;
    while (owner->closure)
      owner = owner->closure->parent;
    return functionKey(syntax_.functions[owner->function].moduleIndex, name);
  }
  bool completing(SourceSpan site) const {
    const auto *query = options_.completion;
    return query && !query->reached && diagnostics_.empty() && query->site.begin == site.begin &&
           query->site.end == site.end;
  }
  void completeMembers(SourceSpan site, std::vector<std::string> names, ScopeNameKind kind) {
    if (!completing(site))
      return;
    auto &query = *options_.completion;
    query.reached = true;
    std::erase_if(names, [&](const auto &name) { return !name.starts_with(query.prefix); });
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    query.truncated = names.size() > 256;
    for (std::size_t i = 0; i < std::min<std::size_t>(names.size(), 256); ++i)
      query.names.push_back({names[i], kind});
  }
  void completeDeviceSelector(const Frame &frame, SourceSpan site) {
    if (!options_.completion || !options_.placement || frame.locals.contains("devices") ||
        !name(site).starts_with("devices."))
      return;
    const SourceSpan member{site.begin + std::string_view("devices.").size(), site.end};
    completeMembers(member, {"cpu", "gpu"}, ScopeNameKind::Function);
  }
  void completeLabels(SourceSpan site, const std::vector<FieldInitializer> &initializers,
                      std::vector<std::string> candidates) {
    std::unordered_set<std::string> supplied;
    for (const auto &field : initializers)
      if (field.name.begin != site.begin || field.name.end != site.end)
        supplied.insert(name(field.name));
    std::erase_if(candidates, [&](const auto &label) { return supplied.contains(label); });
    completeMembers(site, std::move(candidates), ScopeNameKind::Field);
  }
  void completeScope(const Frame &frame, SourceSpan site, bool callTarget = false,
                     bool allowTransfer = false) {
    auto *query = options_.completion;
    if (!query || query->reached || !diagnostics_.empty() || query->site.begin != site.begin ||
        query->site.end != site.end)
      return;
    query->reached = true;
    std::map<std::string, ScopeNameKind> names;
    std::unordered_set<std::string> hidden;
    const auto insert = [&](const std::string &label, ScopeNameKind kind) {
      if (label.starts_with(query->prefix) && hidden.insert(label).second)
        names.try_emplace(label, kind);
    };
    const Frame *scope = &frame;
    bool captured = false;
    while (true) {
      for (const auto &[label, id] : scope->locals) {
        const auto &symbol = output_.symbols[id];
        const auto &type = symbol.type;
        // Mirror closure capture eligibility without lifting a capture or mutating IR.
        if (captured &&
            (type.task || type.fallible || type.device ||
             (type.builtin != BuiltinValue::Numeric && type.builtin != BuiltinValue::Bool &&
              type.builtin != BuiltinValue::Record && type.builtin != BuiltinValue::String &&
              type.builtin != BuiltinValue::Function))) {
          hidden.insert(label);
          continue;
        }
        insert(label, symbol.parameter ? ScopeNameKind::Parameter : ScopeNameKind::Variable);
      }
      if (!scope->closure)
        break;
      scope = scope->closure->parent;
      captured = true;
    }
    const auto prefix = functionKey(frame, "");
    for (const auto &[key, id] : functions_)
      if (key.starts_with(prefix) && signatures_[id])
        insert(key.substr(prefix.size()), ScopeNameKind::Function);
    for (const auto &[key, id] : records_)
      if (key.starts_with(prefix))
        insert(key.substr(prefix.size()), ScopeNameKind::Type);
    if (callTarget) {
      // These names are compiler call forms, not first-class function values.
      // Insert last so lexical and module bindings retain their normal priority.
      for (const auto *label :
           {"array", "tensor", "reshape", "map", "parallelMap", "matmul", "reduceSum", "relu",
            "transpose", "softmaxRows", "rmsNormRows",
            "length", "byteLength", "shape", "slice", "concat", "generate", "args.int32", "random.int32"})
        insert(label, ScopeNameKind::Function);
      if (allowTransfer && options_.placement && !frame.groups.empty())
        insert("transfer", ScopeNameKind::Function);
      for (const auto &numeric : numericTypes)
        insert(std::string(numeric.name), ScopeNameKind::Function);
      for (const auto *alias : {"int", "uint", "float"})
        insert(alias, ScopeNameKind::Function);
    }
    if (!callTarget)
      for (const auto *keyword : {"true", "false", "if", "match", "fn", "not"})
        insert(keyword, ScopeNameKind::Keyword);
    for (const auto &[label, kind] : names) {
      if (query->names.size() == 256) {
        query->truncated = true;
        break;
      }
      query->names.push_back({label, kind});
    }
  }
  void error(std::string code, std::string message, SourceSpan span) {
    ++errorCount_;
    if (diagnostics_.size() < limits_.maxDiagnostics)
      diagnostics_.push_back({std::move(code), std::move(message), span});
  }
  Extent dynamic() { return {{}, nextExtent_++}; }
  bool same(const Extent &a, const Extent &b) const {
    if (a.constant && b.constant)
      return a.constant == b.constant;
    return !a.constant && !b.constant && a.identity && a.identity == b.identity;
  }
  bool parameterExtent(FunctionId function, const Extent &extent) const {
    if (extent.constant)
      return true;
    for (auto parameter : output_.functions[function].parameters)
      for (const auto &dimension : output_.symbols[parameter].type.shape)
        if (!dimension.constant && dimension.identity == extent.identity)
          return true;
    return false;
  }
  bool requireEqual(Frame &frame, const Extent &left, const Extent &right) {
    if (++shapeWork_ > limits_.maxShapeRequirements) {
      error("S034", "shape requirement analysis limit exceeded",
            output_.functions[frame.function].declaration);
      return false;
    }
    if (frame.conditional || same(left, right) || !parameterExtent(frame.function, left) ||
        !parameterExtent(frame.function, right))
      return true;
    const auto key = [](const Extent &dimension) {
      return dimension.constant ? "c" + std::to_string(*dimension.constant)
                                : "d" + std::to_string(dimension.identity);
    };
    const auto a = key(left), b = key(right);
    const auto pair = a < b ? a + ":" + b : b + ":" + a;
    if (requirements_[frame.function].insert(pair).second)
      output_.functions[frame.function].preconditions.push_back({left, right});
    return true;
  }
  bool validSize(const ValueType &type, SourceSpan span) {
    if (!type.tensor)
      return true;
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    bool allKnown = true;
    bool zero = false;
    for (const auto &dimension : type.shape) {
      if (dimension.constant && *dimension.constant > maximum) {
        error("S001", "tensor extent exceeds the supported 64-bit indexing range", span);
        return false;
      }
      allKnown &= dimension.constant.has_value();
      zero |= dimension.constant == 0;
    }
    if (!allKnown || zero)
      return true;
    std::uint64_t count = 1;
    for (const auto &dimension : type.shape) {
      if (*dimension.constant > maximum / count) {
        error("S002", "tensor element-count product overflows the indexing range", span);
        return false;
      }
      count *= *dimension.constant;
    }
    if (isBoolArray(type))
      return true;
    if (isFunctionArray(type)) {
      if (count > maximum / 16) {
        error("S002", "function array storage exceeds the indexing range", span);
        return false;
      }
      return true;
    }
    if (isStringArray(type)) {
      if (count > maximum / 24) {
        error("S002", "String array storage exceeds the indexing range", span);
        return false;
      }
      return true;
    }
    if (!checkedNumericBytes(type.element, count, maximum)) {
      error("S002", "tensor allocation byte count overflows the supported range", span);
      return false;
    }
    return true;
  }
  bool dynamicSize(const ValueType &type) const {
    return type.tensor &&
           std::ranges::any_of(type.shape, [](const auto &d) { return !d.constant; });
  }
  static bool reservedControl(const std::string &name) {
    return name == "true" || name == "false" || name == "if" || name == "else" || name == "and" ||
           name == "or" || name == "not" || name == "type" || name == "with";
  }
  void rememberSourceToken(ResolvedSourceToken token) {
    if (token.span.end > token.span.begin)
      sourceTokens_.insert_or_assign(std::pair{token.span.begin, token.span.end}, std::move(token));
  }
  void rememberTypeUse(SourceSpan span, ValueType value) {
    ResolvedSourceToken token{span, {}, value, {}};
    const auto key = functionKey(syntax_.source->partIndex(span.begin), name(span));
    if (const auto alias = aliases_.find(key); alias != aliases_.end())
      token.declaration = syntax_.records[alias->second].name;
    else if (value.record && *value.record < output_.records.size()) {
      const auto declaration = output_.records[*value.record].declaration;
      if (declaration.end > declaration.begin) {
        token.declaration = declaration;
        token.record = value.record;
      }
    }
    rememberSourceToken(std::move(token));
  }
  void completeTypes(const TypeSyntax &syntax) {
    const auto site = syntax.tensor ? syntax.element : std::optional<SourceSpan>{syntax.name};
    if (syntax.function || !site || !completing(*site))
      return;
    std::vector<std::string> names;
    for (const auto &numeric : numericTypes)
      names.emplace_back(numeric.name);
    names.insert(names.end(), {"int", "uint", "float"});
    if (!syntax.tensor) {
      names.insert(names.end(), {"Bool", "String"});
      if (!syntax.array)
        names.insert(names.end(), {"TaskError", "Tensor"});
      const auto prefix = functionKey(syntax_.source->partIndex(site->begin), "");
      for (const auto &[key, id] : records_)
        if (key.starts_with(prefix))
          names.push_back(key.substr(prefix.size()));
      for (const auto &[key, declaration] : aliases_)
        if (key.starts_with(prefix))
          names.push_back(key.substr(prefix.size()));
    }
    completeMembers(*site, std::move(names), ScopeNameKind::Type);
  }
  std::optional<ValueType> type(const TypeSyntax &syntax) {
    completeTypes(syntax);
    auto result = resolveType(syntax);
    if (result && !syntax.function) {
      auto named = *result;
      if (syntax.array) {
        named.tensor = named.array = false;
        named.shape.clear();
      }
      rememberTypeUse(syntax.name, named);
      if (syntax.element) {
        auto element = *result;
        element.tensor = element.array = false;
        element.shape.clear();
        rememberTypeUse(*syntax.element, element);
      }
    }
    return result;
  }
  std::optional<ValueType> resolveType(const TypeSyntax &syntax) {
    if (syntax.function) {
      std::vector<ValueType> parameters;
      for (const auto &parameter : syntax.function->parameters) {
        auto resolved = type(parameter);
        if (!resolved)
          return {};
        parameters.push_back(*resolved);
      }
      auto result = type(syntax.function->result);
      if (!result)
        return {};
      if (!syntax.function->errorTypes.empty()) {
        std::vector<std::size_t> families;
        std::unordered_set<std::size_t> active;
        for (auto channel : syntax.function->errorTypes) {
          auto family = resolveError(channel, active);
          if (!family)
            return {};
          families.push_back(*family);
        }
        auto family = combineErrors(std::move(families), syntax.span);
        if (!family)
          return {};
        result->fallible = true;
        result->errorRecord = normalizedError(*family);
      }
      auto callable = callableType(std::move(parameters), *result, syntax.span);
      if (callable && syntax.array) {
        callable->tensor = callable->array = true;
        callable->shape = {dynamic()};
      }
      return callable;
    }
    if (!syntax.tensor && !syntax.array && name(syntax.name) == "TaskError") {
      ValueType result;
      result.builtin = BuiltinValue::TaskError;
      return result;
    }
    if (!syntax.tensor && name(syntax.name) == "String") {
      ValueType result;
      result.builtin = BuiltinValue::String;
      if (syntax.array) {
        result.tensor = result.array = true;
        result.shape = {dynamic()};
      }
      return result;
    }
    if (!syntax.tensor && name(syntax.name) == "Bool") {
      ValueType result;
      result.builtin = BuiltinValue::Bool;
      if (syntax.array) {
        result.tensor = result.array = true;
        result.shape = {dynamic()};
      }
      return result;
    }
    if (!syntax.tensor) {
      const auto key = functionKey(syntax_.source->partIndex(syntax.name.begin), name(syntax.name));
      if (!records_.contains(key))
        if (auto alias = aliases_.find(key); alias != aliases_.end()) {
          std::unordered_set<std::size_t> active;
          if (auto resolved = resolveAlias(alias->second, active))
            records_[key] = *resolved;
          else
            return {};
        }
      if (auto found = records_.find(key); found != records_.end()) {
        const auto id = found->second;
        // Field annotations refer to predeclared nominal identities. Deferring
        // ordinary field expansion permits a complete cyclic storage graph.
        // Error aliases reserve their identity here and copy completed payloads later.
        if (!registeringRecords_ && !populateRecord(id, syntax.span))
          return {};
        auto result = recordType(id);
        if (syntax.array) {
          result.tensor = result.array = true;
          result.shape = {dynamic()};
        }
        return result;
      }
    }
    const auto numeric =
        syntax.tensor ? (syntax.element ? resolveNumeric(name(*syntax.element)) : std::nullopt)
                      : resolveNumeric(name(syntax.name));
    if (!numeric ||
        !(syntax.array    ? numericCollectionStorageAvailable(*numeric, NumericTarget::Cpu)
          : syntax.tensor ? numericTensorElementwiseAvailable(*numeric, NumericTarget::Cpu)
                          : numericScalarArithmeticAvailable(*numeric, NumericTarget::Cpu))) {
      error("S003",
            "expected a declared record or enum type, String, Bool, TaskError, or a supported "
            "numeric scalar, "
            "tensor "
            "or array "
            "type",
            syntax.span);
      return {};
    }
    ValueType result{syntax.tensor || syntax.array, {}};
    result.array = syntax.array;
    if (syntax.array)
      result.shape.push_back(dynamic());
    result.element = *numeric;
    for (const auto &dimension : syntax.dimensions)
      result.shape.push_back(dimension.extent ? Extent{dimension.extent, 0} : dynamic());
    if (!validSize(result, syntax.span))
      return {};
    return result;
  }
  std::optional<ValueType> callableType(std::vector<ValueType> parameters, ValueType result,
                                        SourceSpan span) {
    const auto supported = [](const ValueType &value) {
      return !value.task && !value.fallible && !value.device &&
             value.builtin != BuiltinValue::Device;
    };
    auto success = result;
    success.fallible = false;
    success.errorRecord.reset();
    if (!supported(success) || !std::ranges::all_of(parameters, supported) || options_.placement) {
      error("S074", "function values require completed CPU parameters and synchronous CPU results",
            span);
      return {};
    }
    // Array lengths are value facts, not part of the [T] callable contract.
    // Inferred constant-length results must agree with annotated [T] results
    // and with other array-producing targets selected at runtime.
    if (result.array)
      result.shape[0] = dynamic();
    ValueType value;
    value.builtin = BuiltinValue::Function;
    value.callable = std::make_shared<const FunctionSignature>(
        FunctionSignature{std::move(parameters), std::move(result)});
    return value;
  }
  ValueType recordType(std::size_t id) const {
    ValueType type;
    if (output_.records[id].taskErrorFamily) {
      type.builtin = BuiltinValue::TaskError;
      return type;
    }
    type.builtin = BuiltinValue::Record;
    type.record = id;
    type.recordName = output_.records[id].name;
    return type;
  }
  struct ExportBinding {
    bool record;
    std::size_t id;
    bool alias = false;
  };
  std::optional<ExportBinding> exportedBinding(std::size_t module, const std::string &label,
                                               std::size_t depth = 0) {
    const auto key = functionKey(module, label);
    if (auto found = exports_.find(key); found != exports_.end())
      return found->second;
    if (depth >= limits_.maxDepth || !resolvingExports_.insert(key).second)
      return {};
    std::optional<ExportBinding> resolved;
    if (auto edge = reexports_.find(key); edge != reexports_.end())
      resolved = exportedBinding(edge->second.first, edge->second.second, depth + 1);
    resolvingExports_.erase(key);
    exports_.emplace(key, resolved);
    return resolved;
  }
  void completeImports() {
    if (!options_.completion)
      return;
    for (const auto &import : syntax_.imports) {
      if (!import.target)
        continue;
      for (const auto &imported : import.names) {
        if (!completing(imported.name))
          continue;
        auto &query = *options_.completion;
        query.reached = true;
        const auto prefix = functionKey(*import.target, "");
        // Copy names before resolving re-exports, which memoizes new export keys.
        std::set<std::string> labels;
        const auto collect = [&](const auto &bindings) {
          for (const auto &[key, binding] : bindings)
            if (key.starts_with(prefix) && key.substr(prefix.size()).starts_with(query.prefix))
              labels.insert(key.substr(prefix.size()));
        };
        collect(exports_);
        collect(reexports_);
        for (const auto &label : labels) {
          const auto binding = exportedBinding(*import.target, label);
          if (!binding)
            continue;
          if (query.names.size() == 256) {
            query.truncated = true;
            break;
          }
          query.names.push_back({label, binding->record || binding->alias
                                            ? ScopeNameKind::Type
                                            : ScopeNameKind::Function});
        }
        return;
      }
    }
  }
  void recordImportOrigins() {
    for (const auto &import : syntax_.imports) {
      if (!import.target)
        continue;
      for (const auto &imported : import.names) {
        const auto target = exportedBinding(*import.target, name(imported.name));
        if (!target)
          continue; // Invalid graphs are never published.
        ResolvedImport origin{imported.name, imported.alias, {}, {}, {}, import.exported};
        if (target->alias)
          origin.declaration = syntax_.records[target->id].name;
        else if (target->record) {
          origin.declaration = output_.records[target->id].declaration;
          origin.record = target->id;
        } else {
          origin.declaration = output_.functions[target->id].declaration;
          origin.function = target->id;
        }
        output_.imports.push_back(std::move(origin));
      }
    }
  }
  std::size_t taskFamily() {
    if (auto found = taskErrorFamily(output_))
      return *found;
    TypedRecord task{"TaskError", {}, {}};
    task.error = task.taskErrorFamily = true;
    ValueType tag;
    tag.element = NumericType::Int64;
    task.fields.push_back({"$tag", tag, {}});
    ValueType payload;
    payload.builtin = BuiltinValue::TaskError;
    for (const auto *label : {"Fault", "Cancelled", "DeadlineExceeded", "AdmissionRejected"}) {
      auto slot = task.fields.size();
      task.fields.push_back({"$failure" + std::to_string(slot), payload, {}});
      task.variants.push_back({label, {}, {slot}});
    }
    const auto id = output_.records.size();
    output_.records.push_back(std::move(task));
    return id;
  }
  std::size_t concreteError(std::optional<std::size_t> family) {
    return family ? *family : taskFamily();
  }
  std::optional<std::size_t> normalizedError(std::size_t family) const {
    return output_.records[family].taskErrorFamily ? std::nullopt : std::optional{family};
  }
  std::optional<std::size_t> combineErrors(std::vector<std::size_t> members, SourceSpan span) {
    std::vector<std::size_t> flattened;
    for (auto id : members) {
      const auto part = errorMembers(output_, id);
      flattened.insert(flattened.end(), part.begin(), part.end());
    }
    std::ranges::sort(flattened);
    flattened.erase(std::unique(flattened.begin(), flattened.end()), flattened.end());
    if (flattened.empty())
      return {};
    if (flattened.size() == 1)
      return flattened.front();
    for (std::size_t i = 0; i < output_.records.size(); ++i)
      if (output_.records[i].errorMembers == flattened)
        return i;
    TypedRecord joined{"", span, {}};
    joined.error = true;
    joined.errorMembers = flattened;
    ValueType tag;
    tag.element = NumericType::Int64;
    joined.fields.push_back({"$tag", tag, span});
    for (auto id : flattened) {
      const auto &source = output_.records[id];
      if (!joined.name.empty())
        joined.name += " | ";
      joined.name += source.name;
    }
    const auto id = output_.records.size();
    output_.records.push_back(std::move(joined));
    if (!registeringRecords_) {
      finishErrorUnion(id);
      if (auto invalid = recursiveRecordLayoutError(output_, limits_)) {
        error(invalid->code, invalid->message, invalid->span);
        return {};
      }
    }
    return id;
  }
  void finishErrorUnion(std::size_t id) {
    // Members are flattened nominal families; their fields may themselves point
    // back to this stable union identity. Copy metadata without expanding them.
    const auto members = output_.records[id].errorMembers;
    const auto site = output_.records[id].declaration;
    TypedRecord joined{output_.records[id].name, site, {}};
    joined.error = true;
    joined.errorMembers = members;
    ValueType tag;
    tag.element = NumericType::Int64;
    joined.fields.push_back({"$tag", tag, site});
    for (auto member : members) {
      const auto &source = output_.records[member];
      for (std::size_t v = 0; v < source.variants.size(); ++v) {
        const auto &original = source.variants[v];
        TypedVariant variant{
            std::to_string(member) + "." + original.name, original.declaration, {}};
        for (auto field : original.fields) {
          variant.fields.push_back(joined.fields.size());
          joined.fields.push_back(source.fields[field]);
        }
        joined.variants.push_back(std::move(variant));
        joined.errorOrigins.push_back({member, v});
      }
    }
    output_.records[id] = std::move(joined);
  }
  std::optional<std::size_t> resolveAlias(std::size_t id, std::unordered_set<std::size_t> &active) {
    if (auto cached = resolvedAliases_.find(id); cached != resolvedAliases_.end())
      return cached->second;
    const auto &alias = syntax_.records[id];
    if (active.size() >= limits_.maxDepth || !active.insert(id).second) {
      error("S072", "cyclic or excessively nested error alias", alias.name);
      return {};
    }
    std::vector<std::size_t> members;
    bool valid = true;
    for (auto member : alias.errorAlias) {
      auto family = resolveError(member, active);
      if (!family)
        valid = false;
      else
        members.push_back(*family);
    }
    active.erase(id);
    auto result = valid ? combineErrors(std::move(members), alias.name) : std::nullopt;
    resolvedAliases_[id] = result;
    if (result)
      rememberTypeUse(alias.name, recordType(*result));
    return result;
  }
  void completeErrorTypes(SourceSpan site, const std::unordered_set<std::size_t> &active) {
    if (!completing(site))
      return;
    std::vector<std::string> names{"TaskError"};
    const auto prefix = functionKey(syntax_.source->partIndex(site.begin), "");
    for (const auto &[key, id] : records_)
      if (key.starts_with(prefix) && output_.records[id].error)
        names.push_back(key.substr(prefix.size()));
    for (const auto &[key, declaration] : aliases_)
      if (key.starts_with(prefix) && !active.contains(declaration))
        names.push_back(key.substr(prefix.size()));
    completeMembers(site, std::move(names), ScopeNameKind::Type);
  }
  std::optional<std::size_t> resolveError(SourceSpan member,
                                          std::unordered_set<std::size_t> &active) {
    completeErrorTypes(member, active);
    if (name(member) == "TaskError") {
      const auto id = taskFamily();
      ValueType value;
      value.builtin = BuiltinValue::TaskError;
      rememberTypeUse(member, value);
      return id;
    }
    const auto key = functionKey(syntax_.source->partIndex(member.begin), name(member));
    if (auto alias = aliases_.find(key); alias != aliases_.end()) {
      auto resolved = resolveAlias(alias->second, active);
      if (resolved)
        rememberTypeUse(member, recordType(*resolved));
      return resolved;
    }
    if (auto found = records_.find(key);
        found != records_.end() && output_.records[found->second].error) {
      const auto id = found->second;
      if (!registeringRecords_ && !populateRecord(id, member))
        return {};
      rememberTypeUse(member, recordType(id));
      return id;
    }
    error("S072", "expected a declared error family or error alias", member);
    return {};
  }
  void registerRecords() {
    for (std::size_t declaration = 0; declaration < syntax_.records.size(); ++declaration) {
      const auto &record = syntax_.records[declaration];
      const auto label = name(record.name);
      if (reservedControl(label) || resolveNumeric(label) || label == "Bool" || label == "Tensor" ||
          label == "TaskError" || label == "FaultKind" || label == "String" || label == "Device") {
        error("S068", "reserved record type name '" + label + "'", record.name);
        continue;
      }
      const auto key = functionKey(syntax_.source->partIndex(record.name.begin), label);
      if (records_.contains(key) || aliases_.contains(key)) {
        error("S068", "duplicate type '" + label + "'", record.name);
        continue;
      }
      if (!record.errorAlias.empty()) {
        aliases_.emplace(key, declaration);
        if (record.exported)
          exports_.emplace(key, ExportBinding{false, declaration, true});
        continue;
      }
      if (!records_.emplace(key, output_.records.size()).second) {
        error("S068", "duplicate record type '" + label + "'", record.name);
        continue;
      }
      if (record.exported)
        exports_.emplace(key, ExportBinding{true, output_.records.size()});
      TypedRecord header{label, record.name, {}};
      header.error = record.error;
      output_.records.push_back(std::move(header));
      recordDeclarations_.push_back(declaration);
      recordStates_.push_back(State::Unseen);
    }
    completeImports();
    // Resolve type imports before checking fields or function annotations. Explicit
    // re-exports follow the original declaration; ordinary imports remain private.
    for (const auto &import : syntax_.imports) {
      if (!import.target || options_.placement)
        continue;
      for (const auto &imported : import.names) {
        const auto found = exportedBinding(*import.target, name(imported.name));
        if (!found || (!found->record && !found->alias))
          continue;
        const auto binding = imported.alias.value_or(imported.name);
        const auto label = name(binding);
        if (reservedControl(label) || resolveNumeric(label) || label == "Bool" ||
            label == "Tensor" || label == "TaskError" || label == "FaultKind" ||
            label == "String" || label == "Device") {
          error("S067", "reserved import alias '" + label + "'", binding);
          continue;
        }
        const auto key = functionKey(import.moduleIndex, label);
        if (records_.contains(key) || aliases_.contains(key))
          error("S068", "duplicate module type binding '" + label + "'", binding);
        else if (found->alias)
          aliases_.emplace(key, found->id);
        else
          records_.emplace(key, found->id);
      }
    }
    // Ordinary fields resolve predeclared identities without expanding children.
    // Error aliases reserve identities before copying their completed families.
    // No references into records survive lookup, which can append union records.
    for (std::size_t id = 0; id < recordDeclarations_.size(); ++id)
      populateRecord(id);
    for (std::size_t id = 0; id < output_.records.size(); ++id)
      if (!output_.records[id].errorMembers.empty())
        finishErrorUnion(id);
    registeringRecords_ = false;
  }
  bool populateRecord(std::size_t id, std::optional<SourceSpan> reference = {}) {
    if (id >= recordStates_.size() || recordStates_[id] == State::Complete)
      return true; // Canonical unions and TaskError are populated at creation.
    if (recordStates_[id] == State::Failed)
      return false;
    if (recordStates_[id] == State::Checking) {
      error("S068", "recursive record/enum field type cycles are not yet supported",
            reference.value_or(output_.records[id].declaration));
      return false;
    }
    if (recordDepth_ >= limits_.maxDepth) {
      error("S068", "record layout exceeds configured nesting depth",
            reference.value_or(output_.records[id].declaration));
      return false;
    }
    const auto before = errorCount_;
    recordStates_[id] = State::Checking;
    ++recordDepth_;
    fillRecord(id);
    --recordDepth_;
    const bool valid = errorCount_ == before;
    recordStates_[id] = valid ? State::Complete : State::Failed;
    return valid;
  }
  void fillRecord(std::size_t id) {
    const auto &record = syntax_.records[recordDeclarations_[id]];
    if (!record.variants.empty()) {
      ValueType tag;
      tag.element = NumericType::Int64;
      output_.records[id].fields.push_back({"$tag", tag, record.name});
      std::unordered_set<std::string> cases;
      for (const auto &variant : record.variants) {
        const auto label = name(variant.name);
        if (reservedControl(label) || !cases.insert(label).second) {
          error("S071", "duplicate or reserved enum variant '" + label + "'", variant.name);
          continue;
        }
        TypedVariant typed{label, variant.name, {}};
        rememberSourceToken({variant.name, variant.name, recordType(id), {}});
        std::unordered_set<std::string> fields;
        for (const auto &field : variant.fields) {
          const auto label = name(field.name);
          if (reservedControl(label) || !fields.insert(label).second) {
            error("S071", "duplicate or reserved variant field '" + label + "'", field.name);
            continue;
          }
          auto value = type(field.type);
          if (!value)
            continue;
          if ((value->tensor && value->builtin != BuiltinValue::Numeric && !isBoolArray(*value) &&
               !isStringArray(*value) && !isRecordArray(*value) && !isFunctionArray(*value)) ||
              (value->builtin != BuiltinValue::Numeric && value->builtin != BuiltinValue::Bool &&
               value->builtin != BuiltinValue::String &&
               value->builtin != BuiltinValue::TaskError &&
               value->builtin != BuiltinValue::Record &&
               value->builtin != BuiltinValue::Function)) {
            error("S071", "enum payload requires a supported completed CPU value", field.type.span);
            continue;
          }
          typed.fields.push_back(output_.records[id].fields.size());
          output_.records[id].fields.push_back({label, *value, field.name});
        }
        output_.records[id].variants.push_back(std::move(typed));
      }
      return;
    }
    std::unordered_set<std::string> fields;
    for (const auto &field : record.fields) {
      const auto label = name(field.name);
      if (reservedControl(label) || !fields.insert(label).second) {
        error("S068", "duplicate or reserved record field '" + label + "'", field.name);
        continue;
      }
      auto fieldType = type(field.type);
      if (!fieldType)
        continue;
      if ((fieldType->tensor && fieldType->builtin != BuiltinValue::Numeric &&
           !isBoolArray(*fieldType) && !isStringArray(*fieldType) && !isRecordArray(*fieldType) &&
           !isFunctionArray(*fieldType)) ||
          (fieldType->builtin != BuiltinValue::Numeric &&
           fieldType->builtin != BuiltinValue::Bool && fieldType->builtin != BuiltinValue::Record &&
           fieldType->builtin != BuiltinValue::TaskError &&
           fieldType->builtin != BuiltinValue::String &&
           fieldType->builtin != BuiltinValue::Function)) {
        error("S068",
              "record fields require CPU numbers, numeric collections, Bool, Bool arrays, "
              "Strings, String arrays, TaskError values, functions, records or record arrays",
              field.type.span);
        continue;
      }
      output_.records[id].fields.push_back({label, *fieldType, field.name});
    }
  }
  void registerSignatures() {
    for (FunctionId id = 0; id < syntax_.functions.size(); ++id) {
      const auto &function = syntax_.functions[id];
      auto &target = output_.functions[id];
      target.name = name(function.name);
      target.declaration = function.name;
      if (records_.contains(functionKey(function.moduleIndex, target.name)))
        error("S068", "function conflicts with nominal type name", function.name);
      if (target.name == "Bool" || target.name == "String" || reservedControl(target.name))
        error("S056", "reserved builtin/control-flow name", function.name);
      if (resolveNumeric(target.name)) {
        error("S055", "numeric type names are reserved conversion functions", function.name);
        signatures_[id] = false;
      }
      if (options_.placement && (target.name == "devices" || target.name == "transfer" ||
                                 target.name == "devices.cpu" || target.name == "devices.gpu")) {
        error("S049", "reserved placement builtin name", function.name);
        signatures_[id] = false;
      }
      if (!functions_.emplace(functionKey(function.moduleIndex, target.name), id).second) {
        error("S004", "duplicate function '" + target.name + "'", function.name);
        signatures_[id] = false;
      }
      std::unordered_map<std::string, bool> parameters;
      for (const auto &parameter : function.parameters) {
        const auto parameterName = name(parameter.name);
        if (reservedControl(parameterName))
          error("S056", "Control-flow keywords cannot be parameter names", parameter.name);
        if (!parameters.emplace(parameterName, true).second) {
          error("S005", "duplicate parameter '" + parameterName + "'", parameter.name);
          signatures_[id] = false;
        }
        auto parameterType = type(parameter.type);
        if (!parameterType) {
          signatures_[id] = false;
          continue;
        }
        target.parameters.push_back(output_.symbols.size());
        output_.symbols.push_back(
            {parameterName, std::move(*parameterType), parameter.name, id, true});
      }
      if (function.errorType &&
          !(function.errorTypes.size() <= 1 && name(*function.errorType) == "TaskError")) {
        auto channels = function.errorTypes;
        if (channels.empty())
          channels.push_back(*function.errorType);
        std::vector<std::size_t> families;
        std::unordered_set<std::size_t> active;
        for (auto channel : channels) {
          if (auto family = resolveError(channel, active))
            families.push_back(*family);
          else
            signatures_[id] = false;
        }
        if (signatures_[id]) {
          if (auto family = combineErrors(std::move(families), *function.errorType))
            errorFamilies_[id] = normalizedError(*family);
          else
            signatures_[id] = false;
        }
      }
      if (function.result) {
        target.declaredResult = type(*function.result);
        if (!target.declaredResult)
          signatures_[id] = false;
        else {
          target.declaredResult->fallible = function.taskError;
          target.declaredResult->errorRecord = errorFamilies_[id];
        }
      }
      if (function.moduleIndex)
        target.name = "__module_" + std::to_string(function.moduleIndex) + "::" + target.name;
    }
    for (const auto &import : syntax_.imports) {
      if (!import.target) {
        error("S067", "local imports require the file-module loader", import.path);
        continue;
      }
      for (const auto &imported : import.names) {
        const auto binding = imported.alias.value_or(imported.name);
        const auto local = name(binding);
        const auto found = exportedBinding(*import.target, name(imported.name));
        if (found && (found->record || found->alias))
          continue;
        if (records_.contains(functionKey(import.moduleIndex, local))) {
          error("S068", "import conflicts with record type name", binding);
          continue;
        }
        if (local == "Bool" || local == "String" || reservedControl(local) ||
            resolveNumeric(local)) {
          error("S067", "reserved import alias '" + local + "'", binding);
          continue;
        }
        if (!found) {
          error("S067", "module does not export declaration '" + name(imported.name) + "'",
                imported.name);
          continue;
        }
        if (!functions_.emplace(functionKey(import.moduleIndex, local), found->id).second)
          error("S004", "duplicate module binding '" + local + "'", binding);
      }
    }
  }
  bool analyzeFunction(FunctionId id) {
    if (states_[id] == State::Complete)
      return true;
    if (states_[id] == State::Failed)
      return false;
    if (states_[id] == State::Checking) {
      if (!output_.functions[id].declaredResult) {
        error("S006", "recursive return inference requires an explicit return annotation",
              syntax_.functions[id].name);
        return false;
      }
      return true;
    }
    if (!signatures_[id]) {
      states_[id] = State::Failed;
      return false;
    }
    if (functionDepth_ >= limits_.maxDepth) {
      error("S007", "function analysis depth limit exceeded", syntax_.functions[id].name);
      states_[id] = State::Failed;
      return false;
    }
    ++functionDepth_;
    states_[id] = State::Checking;
    Frame frame{id, {}, 0, std::make_shared<TaskChecks>(), {}, {}, {}, 0, false};
    for (auto parameter : output_.functions[id].parameters)
      frame.locals.emplace(output_.symbols[parameter].name, parameter);
    const auto initialErrors = errorCount_;
    bool success = true;
    const auto &body = syntax_.functions[id].body;
    for (std::size_t index = 0; index < body.size() && diagnostics_.size() < limits_.maxDiagnostics;
         ++index) {
      const auto &statement = body[index];
      std::optional<ValueType> expected;
      if (statement.annotation) {
        expected = type(*statement.annotation);
        if (!expected) {
          success = false;
          continue;
        }
      } else if (index + 1 == body.size() && !statement.binding)
        expected = output_.functions[id].declaredResult;
      auto value = statementValue(frame, statement, expected, index + 1 == body.size());
      std::optional<SymbolId> symbol;
      if (statement.binding) {
        const auto bindingName = name(*statement.binding);
        if (reservedControl(bindingName))
          error("S056", "Control-flow keywords cannot be binding names", *statement.binding);
        // The initializer is checked before this name is introduced.
        if (frame.locals.contains(bindingName)) {
          error("S008", "duplicate binding '" + bindingName + "'; bindings are immutable",
                *statement.binding);
          success = false;
        } else if (value) {
          symbol = output_.symbols.size();
          output_.symbols.push_back(
              {bindingName, node(*value).type, *statement.binding, id, false});
          frame.locals.emplace(bindingName, *symbol);
          if (auto it = stringCounts_.find(*value); it != stringCounts_.end())
            stringBindings_[*symbol] = it->second;
          if (auto it = intConstants_.find(*value); it != intConstants_.end())
            intBindings_[*symbol] = it->second;
          if (auto it = floatConstants_.find(*value); it != floatConstants_.end())
            floatBindings_.insert_or_assign(*symbol, it->second);
        }
      }
      if (!value) {
        success = false;
        continue;
      }
      output_.functions[id].body.push_back({statement.span, symbol, *value});
      if (index + 1 == body.size()) {
        output_.functions[id].result = *value;
        output_.functions[id].resultType = node(*value).type;
        if (options_.placement && node(*value).type.tensor && node(*value).type.device)
          error("S047", "function results require an explicit transfer to devices.cpu()",
                statement.span);
        if (node(*value).type.builtin == BuiltinValue::Device)
          error("S048", "device selectors cannot escape as function results", statement.span);
        if (node(*value).type.task)
          error("S035", "task handles cannot escape their owning parallel group", statement.span);
        if (node(*value).type.fallible) {
          frame.tasks->handled.insert(node(*value).type.identity);
          if (!syntax_.functions[id].taskError)
            error("S036", "a fallible function must declare its error return channel",
                  statement.span);
          else if (!errorSubset(output_, node(*value).type.errorRecord, errorFamilies_[id]))
            error("S072", "return channel does not cover every returned error family",
                  statement.span);
        }
      }
    }
    if (syntax_.functions[id].taskError) {
      output_.functions[id].resultType.fallible = true;
      output_.functions[id].resultType.errorRecord = errorFamilies_[id];
    }
    for (auto pending : frame.tasks->pending)
      if (!frame.tasks->handled.contains(pending))
        error("S036", "expected error must be propagated with try or returned",
              syntax_.functions[id].name);
    --functionDepth_;
    // Any diagnostic means no module is published; success still tracks failed
    // dependencies even once the bounded diagnostic sink is full.
    success &= initialErrors == errorCount_;
    states_[id] = success ? State::Complete : State::Failed;
    return success;
  }
  const TypedExpr &node(ValueId id) const { return output_.expressions[id]; }
  std::unordered_map<ValueId, std::uint64_t> stringCounts_;
  std::unordered_map<SymbolId, std::uint64_t> stringBindings_;
  template <class Operation>
  std::optional<ValueId> add(Frame &frame, ValueType type, SourceSpan span, Operation operation) {
    if (output_.expressions.size() >= limits_.maxValues) {
      error("S009", "typed value limit exceeded", span);
      return {};
    }
    const auto id = output_.expressions.size();
    if ((type.task || type.fallible) && !type.identity) {
      type.identity = id + 1;
      if (type.fallible && !type.task)
        frame.tasks->pending.insert(type.identity);
    }
    if constexpr (std::is_same_v<Operation, StringLiteral>) {
      std::uint64_t count = 0;
      validateUtf8(operation.value, [] { return true; }, &count);
      stringCounts_[id] = count;
    } else if constexpr (std::is_same_v<Operation, StringSlice>) {
      auto a = intConstants_.find(operation.start), b = intConstants_.find(operation.end);
      if (a != intConstants_.end() && b != intConstants_.end() && a->second >= 0 &&
          b->second >= a->second && b->second <= INT64_MAX)
        stringCounts_[id] = static_cast<std::uint64_t>(b->second - a->second);
    } else if constexpr (std::is_same_v<Operation, StringIndex>)
      stringCounts_[id] = 1;
    else if constexpr (std::is_same_v<Operation, SymbolRead>) {
      if (auto found = stringBindings_.find(operation.symbol); found != stringBindings_.end())
        stringCounts_[id] = found->second;
    } else if constexpr (std::is_same_v<Operation, StringConcat>) {
      auto a = stringCounts_.find(operation.left), b = stringCounts_.find(operation.right);
      if (a != stringCounts_.end() && b != stringCounts_.end() &&
          b->second <= std::uint64_t(INT64_MAX) - a->second)
        stringCounts_[id] = a->second + b->second;
    }
    output_.expressions.push_back({span, std::move(type), std::move(operation)});
    output_.functions[frame.function].expressions.push_back(id);
    return id;
  }
  std::optional<ValueId> conform(Frame &frame, ValueId value, const ValueType &expected,
                                 SourceSpan span, bool deferShape = false) {
    auto actual = node(value).type;
    if (actual.task || expected.task) {
      error("S035", "a task handle cannot be used as a value or escape its group", span);
      return {};
    }
    if (actual.fallible && !expected.fallible) {
      error("S036", "expected error requires try before using the success value", span);
      return {};
    }
    if (actual.builtin != expected.builtin || actual.record != expected.record ||
        (actual.builtin == BuiltinValue::Function &&
         !sameFunctionValueType(recordArrayElement(actual), recordArrayElement(expected))) ||
        (actual.builtin == BuiltinValue::Numeric && actual.element != expected.element) ||
        actual.tensor != expected.tensor || actual.array != expected.array ||
        actual.shape.size() != expected.shape.size()) {
      error("S010",
            "expected " + describeType(expected) + ", got " + describeType(actual) +
                (actual.record != expected.record && actual.record && expected.record
                     ? " (distinct nominal type declarations)"
                     : ""),
            span);
      return {};
    }
    std::vector<ExtentRequirement> checks;
    for (std::size_t axis = 0; axis < actual.shape.size(); ++axis) {
      if (!expected.shape[axis].constant)
        continue;
      if (actual.shape[axis].constant &&
          actual.shape[axis].constant != expected.shape[axis].constant) {
        error("S011",
              "tensor extent mismatch at axis " + std::to_string(axis) + ": expected " +
                  std::to_string(*expected.shape[axis].constant) + ", got " +
                  std::to_string(*actual.shape[axis].constant),
              span);
        return {};
      }
      if (!actual.shape[axis].constant) {
        if (deferShape)
          continue;
        if (!requireEqual(frame, actual.shape[axis], expected.shape[axis]))
          return {};
        checks.push_back({axis, *expected.shape[axis].constant});
        actual.shape[axis] = expected.shape[axis];
      }
    }
    if (checks.empty())
      return value;
    return add(frame, std::move(actual), span, ShapeCheck{value, std::move(checks)});
  }
  struct Depth {
    std::size_t &value;
    explicit Depth(std::size_t &value) : value(value) { ++value; }
    ~Depth() { --value; }
  };
  std::optional<ValueId> expression(Frame &frame, ExprId id, std::optional<ValueType> expected = {},
                                    bool deferShape = false) {
    if (diagnostics_.size() >= limits_.maxDiagnostics)
      return {};
    if (frame.depth >= limits_.maxDepth) {
      error("S012", "semantic expression depth limit exceeded", syntax_.expressions[id].span);
      return {};
    }
    const Depth guard(frame.depth);
    const auto &syntax = syntax_.expressions[id];
    auto value = std::visit(
        [&](const auto &operation) { return infer(frame, syntax.span, operation, expected); },
        syntax.value);
    if (value && isInteger(node(*value).type.element)) {
      const auto element = node(*value).type.element;
      const auto typeName = std::string(describeNumeric(element)->name);
      const bool unsignedValue = isUnsigned(element);
      const __int128_t maximum = integerMaximum(element);
      const __int128_t minimum = integerMinimum(element);
      std::optional<__int128_t> constant;
      const auto &op = node(*value).operation;
      if (const auto *n = std::get_if<Int32Literal>(&op))
        constant = n->value;
      else if (const auto *n = std::get_if<Int64Literal>(&op))
        constant = n->value;
      else if (const auto *n = std::get_if<UInt32Literal>(&op))
        constant = n->value;
      else if (const auto *n = std::get_if<UInt64Literal>(&op))
        constant = n->value;
      else if (const auto *read = std::get_if<SymbolRead>(&op)) {
        if (auto it = intBindings_.find(read->symbol); it != intBindings_.end())
          constant = it->second;
      } else if (const auto *unary = std::get_if<ScalarUnary>(&op)) {
        if (auto it = intConstants_.find(unary->operand); it != intConstants_.end())
          constant = unary->operation == TokenKind::Minus ? -__int128_t(it->second)
                                                          : __int128_t(it->second);
      } else if (const auto *binary = std::get_if<ScalarBinary>(&op)) {
        auto a = intConstants_.find(binary->left), b = intConstants_.find(binary->right);
        if ((binary->operation == TokenKind::Slash || binary->operation == TokenKind::Percent) &&
            b != intConstants_.end() && b->second == 0) {
          error("S052", std::string(typeName) + " division or remainder by zero", syntax.span);
          return {};
        }
        if (a != intConstants_.end() && b != intConstants_.end()) {
          switch (binary->operation) {
          case TokenKind::Plus:
            constant = __int128_t(a->second) + b->second;
            break;
          case TokenKind::Minus:
            constant = __int128_t(a->second) - b->second;
            break;
          case TokenKind::Star:
            if (unsignedValue && b->second && a->second > maximum / b->second) {
              error("S052", std::string("constant ") + typeName + " arithmetic overflows",
                    syntax.span);
              return {};
            }
            constant = __int128_t(a->second) * b->second;
            break;
          case TokenKind::Slash:
            constant = __int128_t(a->second) / b->second;
            break;
          case TokenKind::Percent:
            constant = __int128_t(a->second) % b->second;
            break;
          default:
            break;
          }
        }
      }
      if (constant) {
        if (*constant < minimum || *constant > maximum) {
          error("S052", std::string("constant ") + typeName + " arithmetic overflows", syntax.span);
          return {};
        }
        intConstants_[*value] = *constant;
      }
    }
    if (value)
      floatingConstant(*value);
    if (value && expected)
      return conform(frame, *value, *expected, syntax.span, deferShape);
    return value;
  }
  std::unordered_map<ValueId, __int128_t> intConstants_;
  std::unordered_map<SymbolId, __int128_t> intBindings_;
  std::optional<ValueId> intLiteral(Frame &frame, SourceSpan span, const NumberExpr &literal,
                                    bool negative = false,
                                    NumericType element = NumericType::Int32) {
    const bool unsignedValue = isUnsigned(element);
    const bool uint64Value = element == NumericType::UInt64;
    const bool int64Value = element == NumericType::Int64;
    const auto text = syntax_.source->slice(literal.literal);
    if (text.size() > limits_.maxLiteralBytes) {
      error("S032", "numeric literal exceeds the semantic size limit", span);
      return {};
    }
    std::uint64_t magnitude = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), magnitude);
    const std::uint64_t limit = unsignedValue ? (negative ? 0 : integerMaximum(element))
                                              : integerMaximum(element) + (negative ? 1 : 0);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || magnitude > limit) {
      error("S051",
            std::string(describeNumeric(element)->name) + " requires an in-range integer literal",
            span);
      return {};
    }
    if (uint64Value) {
      ValueType type;
      type.element = element;
      return add(frame, type, span, UInt64Literal{magnitude});
    }
    const auto value =
        negative ? (magnitude == (std::uint64_t(1) << 63) ? INT64_MIN
                                                          : -static_cast<std::int64_t>(magnitude))
                 : static_cast<std::int64_t>(magnitude);
    ValueType type;
    type.element = element;
    if (int64Value)
      return add(frame, type, span, Int64Literal{value});
    if (unsignedValue)
      return add(frame, type, span, UInt32Literal{static_cast<std::uint32_t>(value)});
    return add(frame, type, span, Int32Literal{static_cast<std::int32_t>(value)});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const StringExpr &literal,
                               const std::optional<ValueType> &) {
    auto source = syntax_.source->slice(literal.literal);
    auto decoded =
        source.size() <= limits_.maxLiteralBytes ? decodeStringLiteral(source) : std::nullopt;
    if (!decoded) {
      error("S069", "invalid or oversized UTF-8 String literal or escape", span);
      return {};
    }
    ValueType value;
    value.builtin = BuiltinValue::String;
    return add(frame, value, span, StringLiteral{std::move(*decoded)});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const NumberExpr &literal,
                               const std::optional<ValueType> &context) {
    auto expected = context;
    if (!expected &&
        syntax_.source->slice(literal.literal).find_first_of(".eE") == std::string_view::npos)
      return intLiteral(frame, span, literal, false, NumericType::Int64);
    if (!expected) {
      expected = ValueType{};
      expected->element = NumericType::Float64;
    }
    if (expected->tensor || expected->builtin != BuiltinValue::Numeric) {
      error("S013", "numeric literals require a numeric scalar context", span);
      return {};
    }
    if (isInteger(expected->element))
      return intLiteral(frame, span, literal, false, expected->element);
    const bool doubleWidth = expected->element == NumericType::Float64;
    const auto formatName = doubleWidth ? "float64" : "float32";
    llvm::APFloat value(doubleWidth ? llvm::APFloat::IEEEdouble() : llvm::APFloat::IEEEsingle());
    const auto text = syntax_.source->slice(literal.literal);
    if (text.size() > limits_.maxLiteralBytes) {
      error("S032", "numeric literal exceeds the semantic size limit", span);
      return {};
    }
    auto status = value.convertFromString(llvm::StringRef(text.data(), text.size()),
                                          llvm::APFloat::rmNearestTiesToEven);
    if (!status) {
      llvm::consumeError(status.takeError());
      error("S014", std::string("invalid ") + formatName + " literal", span);
      return {};
    }
    if ((*status & llvm::APFloat::opOverflow) || !value.isFinite()) {
      error("S015", std::string("finite literal overflows ") + formatName, span);
      return {};
    }
    ValueType output;
    output.element = expected->element;
    if (doubleWidth)
      return add(frame, output, span, Float64Literal{value.bitcastToAPInt().getZExtValue()});
    return add(frame, output, span,
               Float32Literal{static_cast<std::uint32_t>(value.bitcastToAPInt().getZExtValue())});
  }
  bool propagateError(Frame &frame, std::optional<std::size_t> family, SourceSpan span) {
    if (frame.handlerFallible && frame.groups.size() == frame.handlerDepth) {
      if (*frame.handlerFallible && *frame.handlerErrorRecord != family) {
        auto combined =
            combineErrors({concreteError(*frame.handlerErrorRecord), concreteError(family)}, span);
        if (!combined)
          return false;
        family = normalizedError(*combined);
      }
      *frame.handlerFallible = true;
      *frame.handlerErrorRecord = family;
      return true;
    }
    if (!frame.groups.empty()) {
      ValueType errorType;
      errorType.fallible = true;
      errorType.errorRecord = family;
      return joinErrors(*frame.groupErrors.back(), errorType, span);
    }
    if (!sourceTaskError(frame.function)) {
      error("S036", "error propagation requires a declared error channel", span);
      return false;
    }
    const auto declaredFamily = frame.function < errorFamilies_.size()
                                    ? errorFamilies_[frame.function]
                                : anonymousErrors_.contains(frame.function)
                                    ? anonymousErrors_.at(frame.function)
                                    : output_.functions[frame.function].declaredResult->errorRecord;
    if (!errorSubset(output_, family, declaredFamily)) {
      error("S072", "declared channel does not cover every propagated error family", span);
      return false;
    }
    return true;
  }
  bool joinErrors(ValueType &result, const ValueType &other, SourceSpan span) {
    if (!other.fallible)
      return true;
    auto family = other.errorRecord;
    if (result.fallible && result.errorRecord != family) {
      auto combined =
          combineErrors({concreteError(result.errorRecord), concreteError(family)}, span);
      if (!combined)
        return false;
      family = normalizedError(*combined);
    }
    result.fallible = true;
    result.errorRecord = family;
    return true;
  }
  bool sourceTaskError(FunctionId id) const {
    return id < syntax_.functions.size()
               ? syntax_.functions[id].taskError
               : anonymousErrors_.contains(id) || (output_.functions[id].declaredResult &&
                                                   output_.functions[id].declaredResult->fallible);
  }
  std::optional<SymbolId> lookupSymbol(Frame &frame, SourceSpan span) {
    const auto identifier = name(span);
    if (auto found = frame.locals.find(identifier); found != frame.locals.end())
      return found->second;
    if (!frame.closure)
      return {};
    if (auto found = frame.closure->captures.find(identifier);
        found != frame.closure->captures.end())
      return found->second;
    auto origin = lookupSymbol(*frame.closure->parent, span);
    if (!origin)
      return {};
    const auto capture = output_.symbols[*origin];
    const auto type = capture.type;
    if (type.task || type.fallible || type.device ||
        (type.builtin != BuiltinValue::Numeric && type.builtin != BuiltinValue::Bool &&
         type.builtin != BuiltinValue::Record && type.builtin != BuiltinValue::String &&
         type.builtin != BuiltinValue::Function)) {
      error("S060",
            "anonymous callbacks capture only completed CPU numbers, collections, Bool, records or "
            "Strings",
            span);
      return {};
    }
    const auto symbol = output_.symbols.size();
    output_.symbols.push_back({identifier, type, span, frame.function, true,
                               capture.definition.value_or(capture.declaration)});
    output_.functions[frame.function].parameters.push_back(symbol);
    frame.closure->captures.emplace(identifier, symbol);
    frame.closure->origins.push_back(*origin);
    return symbol;
  }
  std::optional<ValueId> variantValue(Frame &frame, SourceSpan span, SourceSpan family,
                                      SourceSpan variant,
                                      const std::vector<FieldInitializer> &initializers) {
    const auto owner = records_.find(functionKey(frame, name(family)));
    if (owner == records_.end() || output_.records[owner->second].variants.empty() ||
        lookupSymbol(frame, family)) {
      error("S071", "expected an unshadowed enum type name", family);
      return {};
    }
    const auto &record = output_.records[owner->second];
    if (record.errorMembers.empty() && completing(variant)) {
      std::vector<std::string> names;
      for (const auto &item : record.variants)
        names.push_back(item.name);
      completeMembers(variant, std::move(names), ScopeNameKind::EnumCase);
    }
    const auto found = std::ranges::find_if(record.variants,
                                            [&](const auto &v) { return v.name == name(variant); });
    if (found == record.variants.end()) {
      error("S071", "unknown enum variant '" + name(variant) + "'", variant);
      return {};
    }
    rememberTypeUse(family, recordType(owner->second));
    const auto [original, originalVariant] = errorOrigin(
        output_, owner->second, static_cast<std::size_t>(found - record.variants.begin()));
    const auto variantDeclaration = output_.records[original].variants[originalVariant].declaration;
    rememberSourceToken({variant,
                         variantDeclaration.end > variantDeclaration.begin
                             ? std::optional{variantDeclaration}
                             : std::nullopt,
                         recordType(original),
                         {}});
    std::vector<RecordFieldValue> values;
    std::unordered_set<std::size_t> seen;
    for (const auto &field : initializers) {
      if (completing(field.name)) {
        std::vector<std::string> candidates;
        for (auto slot : found->fields)
          candidates.push_back(record.fields[slot].name);
        completeLabels(field.name, initializers, std::move(candidates));
      }
      const auto slot = std::ranges::find_if(
          found->fields, [&](auto i) { return record.fields[i].name == name(field.name); });
      if (slot == found->fields.end() || !seen.insert(*slot).second) {
        error("S071", "unknown or duplicate enum payload field", field.name);
        return {};
      }
      rememberSourceToken(
          {field.name, record.fields[*slot].declaration, record.fields[*slot].type, {}});
      auto value = expression(frame, field.value, record.fields[*slot].type);
      if (!value)
        return {};
      values.push_back({*slot, *value});
    }
    if (initializers.size() != found->fields.size()) {
      error("S071", "variant construction requires every named payload field", span);
      return {};
    }
    return add(frame, recordType(owner->second), span,
               RecordValue{{},
                           std::move(values),
                           static_cast<std::size_t>(found - record.variants.begin())});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const RecordExpr &record,
                               const std::optional<ValueType> &) {
    if (record.family)
      return variantValue(frame, span, *record.family, record.name, record.fields);
    std::optional<ValueId> base;
    ValueType result;
    if (record.base) {
      base = expression(frame, *record.base);
      if (!base)
        return {};
      result = node(*base).type;
      if (result.builtin != BuiltinValue::Record || result.tensor || result.array || result.task ||
          result.fallible || !result.record) {
        error("S068", "with requires a completed record value", span);
        return {};
      }
    } else {
      const auto found = records_.find(functionKey(frame, name(record.name)));
      if (found == records_.end()) {
        error("S068", "unknown record type '" + name(record.name) + "'", record.name);
        return {};
      }
      if (lookupSymbol(frame, record.name)) {
        error("S068", "record constructor is shadowed by a local value", record.name);
        return {};
      }
      result = recordType(found->second);
    }
    const auto &declaration = output_.records[*result.record];
    if (!declaration.variants.empty()) {
      error("S071",
            "enum construction requires Type.Variant; enum updates require an explicit match",
            span);
      return {};
    }
    if (!base)
      rememberTypeUse(record.name, result);
    std::unordered_set<std::string> seen;
    std::vector<RecordFieldValue> values;
    for (const auto &field : record.fields) {
      if (completing(field.name)) {
        std::vector<std::string> candidates;
        for (const auto &item : declaration.fields)
          candidates.push_back(item.name);
        completeLabels(field.name, record.fields, std::move(candidates));
      }
      const auto label = name(field.name);
      const auto found =
          std::ranges::find_if(declaration.fields, [&](const auto &f) { return f.name == label; });
      if (found == declaration.fields.end() || !seen.insert(label).second) {
        error("S068", "unknown or duplicate field '" + label + "' in " + result.recordName,
              field.name);
        return {};
      }
      rememberSourceToken({field.name, found->declaration, found->type, {}});
      auto value = expression(frame, field.value, found->type);
      if (!value)
        return {};
      values.push_back({static_cast<std::size_t>(found - declaration.fields.begin()), *value});
    }
    if (!base && values.size() != declaration.fields.size()) {
      error("S068", "construction of " + result.recordName + " requires every named field", span);
      return {};
    }
    return add(frame, result, span, RecordValue{base, std::move(values)});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const FieldExpr &read,
                               const std::optional<ValueType> &) {
    if (const auto *family = std::get_if<NameExpr>(&syntax_.expressions[read.input].value)) {
      const auto owner = records_.find(functionKey(frame, name(family->name)));
      if (owner != records_.end() && !output_.records[owner->second].variants.empty() &&
          !lookupSymbol(frame, family->name))
        return variantValue(frame, span, family->name, read.field, {});
    }
    auto input = expression(frame, read.input);
    if (!input)
      return {};
    const auto type = node(*input).type;
    if (type.builtin != BuiltinValue::Record || type.tensor || type.array || type.task ||
        type.fallible || !type.record) {
      error("S068", "field access requires a completed record value", span);
      return {};
    }
    if (!output_.records[*type.record].variants.empty()) {
      error("S071", "enum payload fields are available only inside a matching variant branch",
            span);
      return {};
    }
    const auto &fields = output_.records[*type.record].fields;
    // Capture names only after the receiver has a checked, completed record type.
    // The synthetic field still diagnoses normally; no partial typed IR is published.
    if (completing(read.field)) {
      std::vector<std::string> names;
      for (const auto &field : fields)
        names.push_back(field.name);
      completeMembers(read.field, std::move(names), ScopeNameKind::Field);
    }
    const auto found =
        std::ranges::find_if(fields, [&](const auto &f) { return f.name == name(read.field); });
    if (found == fields.end()) {
      error("S068", "unknown field '" + name(read.field) + "' in " + type.recordName, read.field);
      return {};
    }
    auto result = found->type;
    for (auto &extent : result.shape)
      if (!extent.constant)
        extent = dynamic();
    return add(frame, result, span,
               RecordFieldRead{*input, static_cast<std::size_t>(found - fields.begin())});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const LambdaExpr &lambda,
                               const std::optional<ValueType> &expected) {
    const auto context =
        expected && expected->builtin == BuiltinValue::Function && !expected->tensor
            ? expected->callable
            : nullptr;
    if (context && context->parameters.size() != lambda.parameters.size()) {
      error("S060", "anonymous function parameter count differs from its expected signature", span);
      return {};
    }
    if (functionDepth_ >= limits_.maxDepth || frame.depth >= limits_.maxDepth) {
      error("S007", "anonymous function analysis depth limit exceeded", span);
      return {};
    }
    const Depth functionDepth(functionDepth_);
    std::vector<ValueType> parameters;
    for (std::size_t i = 0; i < lambda.parameters.size(); ++i) {
      const auto &parameter = lambda.parameters[i];
      auto resolved = parameter.annotation ? type(*parameter.annotation)
                      : context            ? std::optional<ValueType>{context->parameters[i]}
                                           : std::nullopt;
      if (!resolved) {
        if (!parameter.annotation)
          error("S060", "anonymous parameter needs a type annotation or an expected function type",
                parameter.name);
        return {};
      }
      if (context && !sameFunctionValueType(*resolved, context->parameters[i])) {
        error("S060", "anonymous parameter differs from its expected function type",
              parameter.name);
        return {};
      }
      parameters.push_back(*resolved);
    }
    auto resultContext = lambda.result ? type(*lambda.result)
                         : context     ? std::optional<ValueType>{context->result}
                                       : std::nullopt;
    if (lambda.result && !resultContext)
      return {};
    std::optional<std::size_t> declaredError;
    if (!lambda.errorTypes.empty()) {
      std::vector<std::size_t> families;
      std::unordered_set<std::size_t> active;
      for (auto channel : lambda.errorTypes) {
        auto family = resolveError(channel, active);
        if (!family)
          return {};
        families.push_back(*family);
      }
      auto family = combineErrors(std::move(families), span);
      if (!family)
        return {};
      declaredError = normalizedError(*family);
      if (resultContext) {
        resultContext->fallible = true;
        resultContext->errorRecord = declaredError;
      }
    }
    const auto id = output_.functions.size();
    if (!lambda.errorTypes.empty())
      anonymousErrors_.emplace(id, declaredError);
    output_.functions.emplace_back();
    requirements_.resize(output_.functions.size());
    output_.functions[id].name = "__closure_" + std::to_string(id);
    output_.functions[id].anonymous = true;
    output_.functions[id].declaration = span;
    output_.functions[id].declaredResult = resultContext;
    Frame nested{id, {}, frame.depth, std::make_shared<TaskChecks>(), {}, {}, {}, 0, false};
    nested.closure = std::make_shared<ClosureScope>(ClosureScope{&frame, {}, {}});
    for (std::size_t i = 0; i < parameters.size(); ++i) {
      const auto label = name(lambda.parameters[i].name);
      if (reservedControl(label) || nested.locals.contains(label)) {
        error("S060", "invalid or duplicate anonymous parameter name", lambda.parameters[i].name);
        return {};
      }
      auto symbol = output_.symbols.size();
      output_.symbols.push_back({label, parameters[i], lambda.parameters[i].name, id, true});
      output_.functions[id].parameters.push_back(symbol);
      nested.locals.emplace(label, symbol);
    }
    std::unordered_set<std::string> introduced;
    for (const auto &parameter : lambda.parameters)
      introduced.insert(name(parameter.name));
    std::vector<TypedStatement> statements;
    auto result = suite(nested, lambda.body, resultContext, statements, introduced);
    if (!result)
      return {};
    const auto resultType = node(*result).type;
    auto declaredResult = resultContext.value_or(resultType);
    if (!lambda.errorTypes.empty()) {
      declaredResult.fallible = true;
      declaredResult.errorRecord = declaredError;
    }
    if (declaredResult.fallible && resultType.fallible &&
        !errorSubset(output_, resultType.errorRecord, declaredResult.errorRecord)) {
      error("S072", "anonymous function return channel does not cover every error family", span);
      return {};
    }
    auto signature = callableType(parameters, declaredResult, span);
    if (!signature)
      return {};
    output_.functions[id].body = std::move(statements);
    output_.functions[id].result = *result;
    output_.functions[id].resultType = resultType;
    if (!lambda.errorTypes.empty())
      output_.functions[id].declaredResult = declaredResult;
    if (declaredResult.fallible) {
      output_.functions[id].resultType.fallible = true;
      output_.functions[id].resultType.errorRecord = declaredResult.errorRecord;
    }
    if (resultType.fallible)
      nested.tasks->handled.insert(resultType.identity);
    for (auto pending : nested.tasks->pending)
      if (!nested.tasks->handled.contains(pending)) {
        error("S036", "anonymous function must handle, propagate or return each expected error",
              span);
        return {};
      }
    output_.functions[id].captureCount = nested.closure->origins.size();
    std::vector<ValueId> captures;
    for (auto origin : nested.closure->origins) {
      auto value = add(frame, output_.symbols[origin].type, span, SymbolRead{origin});
      if (!value)
        return {};
      captures.push_back(*value);
    }
    return add(frame, *signature, span, FunctionReference{id, std::move(captures)});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const NameExpr &read,
                               const std::optional<ValueType> &) {
    completeDeviceSelector(frame, read.name);
    completeScope(frame, read.name);
    const auto identifier = name(read.name);
    if (identifier == "true" || identifier == "false") {
      ValueType boolean;
      boolean.builtin = BuiltinValue::Bool;
      return add(frame, boolean, span, BoolLiteral{identifier == "true"});
    }
    if (auto found = lookupSymbol(frame, read.name))
      return add(frame, output_.symbols[*found].type, span, SymbolRead{*found});
    if (auto found = functions_.find(functionKey(frame, identifier)); found != functions_.end()) {
      const auto id = found->second;
      if (!analyzeFunction(id))
        return {};
      const auto &target = output_.functions[id];
      std::vector<ValueType> parameters;
      for (auto symbol : target.parameters)
        parameters.push_back(output_.symbols[symbol].type);
      auto signature = callableType(std::move(parameters),
                                    target.declaredResult.value_or(target.resultType), span);
      if (!signature)
        return {};
      return add(frame, *signature, span, FunctionReference{id});
    }
    error("S016", "unknown value '" + identifier + "'", read.name);
    return {};
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const UnaryExpr &unary,
                               const std::optional<ValueType> &expected) {
    if (unary.operation == TokenKind::Bang) {
      auto input = expression(frame, unary.operand);
      if (!input)
        return {};
      auto type = node(*input).type;
      if (type.builtin != BuiltinValue::Bool || type.task || type.fallible || type.tensor ||
          type.device) {
        error("S057", "not requires a completed CPU Bool", span);
        return {};
      }
      auto no = add(frame, type, span, BoolLiteral{false});
      if (!no)
        return {};
      return add(frame, type, span, ScalarCompare{ComparisonKind::Equal, *input, *no});
    }
    if ((!expected || (isInteger(expected->element) && !expected->tensor)) &&
        unary.operation == TokenKind::Minus)
      if (const auto *literal = std::get_if<NumberExpr>(&syntax_.expressions[unary.operand].value))
        if (expected ||
            syntax_.source->slice(literal->literal).find_first_of(".eE") == std::string_view::npos)
          return intLiteral(frame, span, *literal, true,
                            expected ? expected->element : NumericType::Int64);
    auto operand = expression(frame, unary.operand, expected);
    if (!operand)
      return {};
    const auto type = node(*operand).type;
    if (type.task || type.fallible || type.array || type.builtin != BuiltinValue::Numeric) {
      error("S017", "arithmetic requires completed numeric values", span);
      return {};
    }
    if (type.tensor && type.device) {
      error("S047", "host arithmetic requires an explicit transfer to devices.cpu()", span);
      return {};
    }
    if (type.tensor) {
      if (unary.operation == TokenKind::Plus)
        return operand;
      return add(frame, type, span, TensorUnary{unary.operation, *operand});
    }
    return add(frame, type, span, ScalarUnary{unary.operation, *operand});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const ComparisonExpr &comparison,
                               const std::optional<ValueType> &) {
    std::optional<ValueId> left, right;
    if (literal_[comparison.left] && !literal_[comparison.right]) {
      right = expression(frame, comparison.right);
      if (right)
        left = expression(frame, comparison.left, node(*right).type);
    } else {
      std::optional<ValueType> context;
      if (literal_[comparison.left] && literal_[comparison.right] &&
          (floatingLiteral_[comparison.left] || floatingLiteral_[comparison.right])) {
        context = ValueType{};
        context->element = NumericType::Float64;
      }
      left = expression(frame, comparison.left, context);
      if (left)
        right = expression(frame, comparison.right,
                           literal_[comparison.right] ? std::optional<ValueType>(node(*left).type)
                                                      : std::nullopt);
    }
    if (!left || !right)
      return {};
    const auto a = node(*left).type, b = node(*right).type;
    const bool numeric = a.builtin == BuiltinValue::Numeric && b.builtin == BuiltinValue::Numeric &&
                         a.element == b.element;
    const bool boolean = a.builtin == BuiltinValue::Bool && b.builtin == BuiltinValue::Bool &&
                         (comparison.operation == ComparisonKind::Equal ||
                          comparison.operation == ComparisonKind::NotEqual);
    const bool string = a.builtin == BuiltinValue::String && b.builtin == BuiltinValue::String &&
                        (comparison.operation == ComparisonKind::Equal ||
                         comparison.operation == ComparisonKind::NotEqual);
    if ((!numeric && !boolean && !string) || a.tensor || b.tensor || a.task || b.task ||
        a.fallible || b.fallible || a.device || b.device) {
      error("S056",
            "comparison requires completed CPU scalars of the same numeric type, or Bool/String "
            "equality",
            span);
      return {};
    }
    ValueType result;
    result.builtin = BuiltinValue::Bool;
    return add(frame, result, span, ScalarCompare{comparison.operation, *left, *right});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const BinaryExpr &binary,
                               const std::optional<ValueType> &context) {
    auto expected = context;
    if (!expected && literal_[binary.left] && literal_[binary.right] &&
        (floatingLiteral_[binary.left] || floatingLiteral_[binary.right])) {
      expected = ValueType{};
      expected->element = NumericType::Float64;
    }
    if (binary.operation == TokenKind::Arrow) {
      const auto &right = syntax_.expressions[binary.right];
      if (const auto *target = std::get_if<NameExpr>(&right.value))
        return invoke(frame, span, target->name, {binary.left}, expected);
      if (const auto *target = std::get_if<CallExpr>(&right.value)) {
        std::vector<ExprId> arguments{binary.left};
        arguments.insert(arguments.end(), target->arguments.begin(), target->arguments.end());
        if (const auto *callee = std::get_if<NameExpr>(&syntax_.expressions[target->callee].value))
          return invoke(frame, span, callee->name, arguments, expected);
        return invokeValue(frame, span, target->callee, arguments);
      }
      if (std::holds_alternative<FieldExpr>(right.value) ||
          std::holds_alternative<IndexExpr>(right.value))
        return invokeValue(frame, span, binary.right, {binary.left});
      error("S018", "pipeline requires a named function, function-valued field/index or call",
            right.span);
      return {};
    }
    const auto operand = [&](ExprId id, std::optional<ValueType> context) {
      if (literal_[id]) {
        if (context && context->tensor && context->builtin == BuiltinValue::Numeric) {
          const auto element = context->element;
          context = ValueType{};
          context->element = element;
        }
        return expression(frame, id, context);
      }
      const auto *call = std::get_if<CallExpr>(&syntax_.expressions[id].value);
      const auto *callee =
          call ? std::get_if<NameExpr>(&syntax_.expressions[call->callee].value) : nullptr;
      if (callee && name(callee->name) == "tensor")
        return expression(frame, id, context);
      return expression(frame, id);
    };
    std::optional<ValueId> left, right;
    if (expected && !expected->tensor) {
      left = expression(frame, binary.left, expected);
      right = expression(frame, binary.right, expected);
    } else if (literal_[binary.left] && !literal_[binary.right]) {
      right = operand(binary.right, expected);
      if (right)
        left = operand(binary.left, node(*right).type);
    } else {
      left = operand(binary.left, expected);
      if (left)
        right = operand(binary.right, node(*left).type);
    }
    if (!left || !right)
      return {};
    const auto a = node(*left).type, b = node(*right).type;
    if (binary.operation == TokenKind::Plus && a.builtin == BuiltinValue::String &&
        b.builtin == BuiltinValue::String && !a.tensor && !b.tensor && !a.task && !b.task &&
        !a.fallible && !b.fallible && !a.device && !b.device)
      return add(frame, a, span, StringConcat{*left, *right});
    if (binary.operation == TokenKind::Percent &&
        (!isInteger(a.element) || !isInteger(b.element))) {
      error("S019", "'%' requires integer operands", span);
      return {};
    }
    if (a.task || b.task || a.fallible || b.fallible || a.array || b.array ||
        a.builtin != BuiltinValue::Numeric || b.builtin != BuiltinValue::Numeric ||
        a.element != b.element) {
      error("S017", "arithmetic requires completed values of the same numeric type", span);
      return {};
    }
    if ((a.tensor && a.device) || (b.tensor && b.device)) {
      error("S047", "host arithmetic requires an explicit transfer to devices.cpu()", span);
      return {};
    }
    if (a.tensor || b.tensor) {
      if (binary.operation != TokenKind::Plus && binary.operation != TokenKind::Minus &&
          binary.operation != TokenKind::Star && binary.operation != TokenKind::Slash &&
          !(isInteger(a.element) && binary.operation == TokenKind::Percent)) {
        error("S017", "elementwise float32 arithmetic currently supports only +, -, * and /", span);
        return {};
      }
      auto result = a.tensor ? a : b;
      if (a.tensor && b.tensor) {
        if (a.shape.size() != b.shape.size()) {
          error("S017", "elementwise operands must have equal ranks", span);
          return {};
        }
        for (std::size_t axis = 0; axis < a.shape.size(); ++axis) {
          if (a.shape[axis].constant && b.shape[axis].constant &&
              a.shape[axis].constant != b.shape[axis].constant) {
            error("S011", "elementwise operand extents differ", span);
            return {};
          }
          if (!requireEqual(frame, a.shape[axis], b.shape[axis]))
            return {};
          if (b.shape[axis].constant)
            result.shape[axis] = b.shape[axis];
        }
      }
      if (!validSize(result, span))
        return {};
      return add(frame, result, span, TensorBinary{binary.operation, *left, *right});
    }
    return add(frame, a, span, ScalarBinary{binary.operation, *left, *right});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const CallExpr &call,
                               const std::optional<ValueType> &expected) {
    if (const auto *field = std::get_if<FieldExpr>(&syntax_.expressions[call.callee].value)) {
      if (const auto *owner = std::get_if<NameExpr>(&syntax_.expressions[field->input].value);
          owner && !lookupSymbol(frame, owner->name) && name(field->field) == "int32" &&
          (name(owner->name) == "args" || name(owner->name) == "random"))
        return runtimeInput(frame, span, name(owner->name) + ".int32", call.arguments);
    }
    const auto *callee = std::get_if<NameExpr>(&syntax_.expressions[call.callee].value);
    if (!callee)
      return invokeValue(frame, span, call.callee, call.arguments);
    return invoke(frame, span, callee->name, call.arguments, expected);
  }
  std::optional<ValueId> invokeValue(Frame &frame, SourceSpan span, ExprId callee,
                                     const std::vector<ExprId> &arguments) {
    auto target = expression(frame, callee);
    if (!target)
      return {};
    return callValue(frame, span, *target, arguments);
  }
  bool queryingSignature(SourceSpan span) const {
    const auto *query = options_.signature;
    return query && !query->snapshot && diagnostics_.empty() && query->site.begin == span.begin &&
           query->site.end == span.end;
  }
  bool completingArgument(const std::vector<ExprId> &arguments) const {
    const auto *query = options_.completion;
    if (!query || query->reached || !diagnostics_.empty() || arguments.empty())
      return false;
    return std::ranges::any_of(arguments, [&](ExprId argument) {
      const auto span = syntax_.expressions[argument].span;
      return span.begin <= query->site.begin && query->site.end <= span.end;
    });
  }
  void captureSignature(Frame &frame, const FunctionSignature &signature,
                        std::vector<std::string> names = {}) {
    if (const auto receiver = options_.signature->argumentReceiver) {
      if (*receiver >= syntax_.expressions.size())
        return;
      const auto value = expression(frame, *receiver);
      if (!value)
        return;
      const auto &type = node(*value).type;
      if (type.builtin != BuiltinValue::Record || type.tensor || type.array || type.task ||
          type.fallible || !type.record || !output_.records[*type.record].variants.empty())
        return;
    }
    if (options_.signature->editingExpression && options_.signature->editingSite) {
      ScopeCompletion probe{*options_.signature->editingSite, "", {}, false, false};
      auto *previous = options_.completion;
      options_.completion = &probe;
      const auto errors = diagnostics_.size();
      const auto slot = options_.signature->argumentIndex;
      (void)expression(frame, *options_.signature->editingExpression,
                       slot && *slot < signature.parameters.size()
                           ? std::optional(signature.parameters[*slot])
                           : std::nullopt);
      options_.completion = previous;
      if (!probe.reached)
        return;
      diagnostics_.resize(errors); // Discard only diagnostics reached after the checked hole.
    }
    CallableSignatureSnapshot snapshot{signature, std::move(names), {}};
    for (const auto &record : output_.records)
      snapshot.errorFamilies.push_back({record.name, record.errorMembers, record.taskErrorFamily});
    options_.signature->snapshot = std::move(snapshot);
  }
  std::optional<ValueId> callValue(Frame &frame, SourceSpan span, ValueId target,
                                   const std::vector<ExprId> &arguments) {
    const auto type = node(target).type;
    if (type.builtin != BuiltinValue::Function || !type.callable || type.tensor || type.array ||
        type.task || type.fallible) {
      error("S030", "call requires a completed function value", span);
      return {};
    }
    const bool signatureQuery = queryingSignature(span);
    const auto slot = signatureQuery ? options_.signature->argumentIndex : std::nullopt;
    const bool projected = slot && *slot < arguments.size() &&
                           *slot < type.callable->parameters.size() &&
                           arguments.size() <= type.callable->parameters.size();
    if (projected || ((signatureQuery || completingArgument(arguments)) &&
                      arguments.size() < type.callable->parameters.size())) {
      for (std::size_t i = 0; i < (projected ? *slot : arguments.size()); ++i)
        if (!expression(frame, arguments[i], type.callable->parameters[i]))
          return {};
      if (signatureQuery)
        captureSignature(frame, *type.callable);
      if (projected) {
        error("S027", "editing argument requires a signature snapshot", span);
        return {}; // Query-only failure: no invented argument or executable partial IR.
      }
    }
    if (arguments.size() != type.callable->parameters.size()) {
      error("S027", "wrong argument count for function value", span);
      return {};
    }
    std::vector<ValueId> values;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      auto value = expression(frame, arguments[i], type.callable->parameters[i]);
      if (!value)
        return {};
      values.push_back(*value);
    }
    // A callable signature promises a rank and any fixed extents, not a
    // relationship between the shapes returned by separate invocations.
    auto result = type.callable->result;
    for (auto &extent : result.shape)
      if (!extent.constant)
        extent = dynamic();
    return add(frame, std::move(result), span, FunctionInvoke{target, std::move(values)});
  }
  std::optional<ValueId> arrayConcat(Frame &frame, SourceSpan span,
                                     const std::vector<ExprId> &arguments,
                                     const std::optional<ValueType> &expected) {
    const auto array = [](const ValueType &type) {
      return type.array && type.tensor && type.shape.size() == 1 && !type.task && !type.fallible &&
             !type.device && (type.builtin == BuiltinValue::Numeric || isOwnedArray(type));
    };
    const auto elementContext = [](ValueType type) {
      type.shape = {{{}, 0}};
      return type;
    };
    std::optional<ValueType> context =
        expected && expected->array ? std::optional(elementContext(*expected)) : std::nullopt;
    // The first checked array fixes concat's element contract. Capture that
    // contract only for this editor query, before the ordinary arity error.
    if (queryingSignature(span) && arguments.size() == 1 && !options_.placement) {
      if (const auto left = expression(frame, arguments[0], context);
          left && diagnostics_.empty() && array(node(*left).type)) {
        const auto first = node(*left).type;
        const auto other = elementContext(first);
        captureSignature(frame, FunctionSignature{{first, other}, other}, {"left", "right"});
      }
    }
    if (arguments.size() != 2) {
      error("S027", "concat expects exactly two arrays", span);
      return {};
    }
    if (options_.placement) {
      error("S061", "concat requires CPU arrays without placement", span);
      return {};
    }
    const auto empty = [&](ExprId id) {
      auto *literal = std::get_if<ArrayExpr>(&syntax_.expressions[id].value);
      return literal && literal->elements.empty();
    };
    std::optional<ValueId> left, right;
    // An empty literal has no effects; infer its element context from the other
    // operand without changing the left-to-right runtime evaluation order.
    if (!context && empty(arguments[0])) {
      right = expression(frame, arguments[1]);
      if (!right)
        return {};
      if (!array(node(*right).type)) {
        error("S061", "concat requires completed CPU arrays", span);
        return {};
      }
      left = expression(frame, arguments[0], elementContext(node(*right).type));
    } else {
      left = expression(frame, arguments[0], context);
      if (!left)
        return {};
      if (!array(node(*left).type)) {
        error("S061", "concat requires completed CPU arrays", span);
        return {};
      }
      right = expression(frame, arguments[1],
                         context               ? context
                         : empty(arguments[1]) ? std::optional(elementContext(node(*left).type))
                                               : std::nullopt);
    }
    if (!left || !right)
      return {};
    auto type = node(*left).type;
    const auto other = node(*right).type;
    if (!array(type) || !array(other) || type.builtin != other.builtin ||
        type.record != other.record ||
        (type.builtin == BuiltinValue::Function &&
         !sameFunctionValueType(recordArrayElement(type), recordArrayElement(other))) ||
        (type.builtin == BuiltinValue::Numeric && type.element != other.element)) {
      error("S061", "concat requires completed CPU arrays with the same element type", span);
      return {};
    }
    const auto a = type.shape[0].constant, b = other.shape[0].constant;
    if (a && b) {
      if (*b > std::uint64_t(INT64_MAX) || *a > std::uint64_t(INT64_MAX) - *b) {
        error("S024", "concatenated array length exceeds the supported range", span);
        return {};
      }
      type.shape = {{{*a + *b}, 0}};
    } else
      type.shape = {dynamic()};
    if (!validSize(type, span))
      return {};
    return add(frame, type, span, ArrayConcat{*left, *right});
  }
  std::optional<ValueId> collectionSlice(Frame &frame, SourceSpan span,
                                         const std::vector<ExprId> &arguments,
                                         const std::optional<ValueType> &expected) {
    if (arguments.size() != 3) {
      error("S027", "slice expects a String or array, start and end", span);
      return {};
    }
    auto input =
        expression(frame, arguments[0], expected && expected->array ? expected : std::nullopt);
    if (!input)
      return {};
    auto type = node(*input).type;
    const bool array = type.array && type.tensor && type.shape.size() == 1 &&
                       (type.builtin == BuiltinValue::Numeric || isOwnedArray(type));
    const bool string = type.builtin == BuiltinValue::String && !type.tensor && !type.array;
    if ((!array && !string) || type.task || type.fallible || type.device || options_.placement) {
      error("S061", "slice requires a completed CPU String or array without placement", span);
      return {};
    }
    std::vector<ValueId> bounds;
    for (std::size_t j = 1; j < 3; ++j) {
      auto bound = expression(frame, arguments[j]);
      if (!bound)
        return {};
      const auto i = node(*bound).type;
      if (i.builtin != BuiltinValue::Numeric || !isInteger(i.element) || i.tensor || i.array ||
          i.task || i.fallible || i.device) {
        error("S061", "slice bounds require completed CPU integer scalars", span);
        return {};
      }
      auto known = intConstants_.find(*bound);
      auto textExtent = stringCounts_.find(*input);
      std::optional<std::uint64_t> extent = array ? type.shape[0].constant
                                            : textExtent != stringCounts_.end()
                                                ? std::optional(textExtent->second)
                                                : std::nullopt;
      if (known != intConstants_.end() &&
          (known->second < 0 || known->second > INT64_MAX || (extent && known->second > *extent))) {
        error("S062", "slice bound is outside its input range", span);
        return {};
      }
      bounds.push_back(*bound);
    }
    auto a = intConstants_.find(bounds[0]), b = intConstants_.find(bounds[1]);
    if (a != intConstants_.end() && b != intConstants_.end() && a->second > b->second) {
      error("S062", "slice start must not exceed end", span);
      return {};
    }
    if (array) {
      type.shape = {a != intConstants_.end() && b != intConstants_.end()
                        ? Extent{static_cast<std::uint64_t>(b->second - a->second), 0}
                        : Extent{{}, nextExtent_++}};
      if (!validSize(type, span))
        return {};
      return add(frame, type, span, ArraySlice{*input, bounds[0], bounds[1]});
    }
    return add(frame, type, span, StringSlice{*input, bounds[0], bounds[1]});
  }
  std::optional<ValueId> collectionQuery(Frame &frame, SourceSpan span, const std::string &callee,
                                         const std::vector<ExprId> &arguments) {
    if (arguments.size() != 1) {
      error("S027", callee + " expects exactly one argument", span);
      return {};
    }
    auto input = expression(frame, arguments[0]);
    if (!input)
      return {};
    const auto type = node(*input).type;
    const bool length = callee == "length";
    if ((length || callee == "byteLength") && type.builtin == BuiltinValue::String &&
        !type.tensor && !type.array && !type.task && !type.fallible && !type.device) {
      ValueType result;
      result.element = NumericType::Int64;
      return add(
          frame, result, span,
          StringQuery{length ? StringQueryKind::Length : StringQueryKind::ByteLength, *input});
    }
    if (callee == "byteLength") {
      error("S064", "byteLength requires a completed CPU String", span);
      return {};
    }
    if (!type.tensor ||
        (type.builtin != BuiltinValue::Numeric && !(length && isOwnedArray(type))) || type.task ||
        type.fallible || type.device || type.array != length) {
      error("S064",
            callee + (length ? " requires a completed CPU array or String"
                             : " requires a completed CPU numeric tensor"),
            span);
      return {};
    }
    ValueType result;
    result.element = NumericType::Int64;
    if (!length) {
      result.tensor = result.array = true;
      result.shape = {{type.shape.size(), 0}};
    }
    return add(
        frame, result, span,
        CollectionQuery{length ? CollectionQueryKind::Length : CollectionQueryKind::Shape, *input});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const IndexExpr &read,
                               const std::optional<ValueType> &expected) {
    std::optional<ValueType> context;
    if (expected && expected->builtin == BuiltinValue::Numeric && !expected->tensor &&
        !expected->task && !expected->fallible && !expected->device &&
        std::holds_alternative<ArrayExpr>(syntax_.expressions[read.input].value)) {
      context = *expected;
      context->array = context->tensor = true;
      context->shape = {{std::nullopt, 0}};
    }
    auto input = expression(frame, read.input, context);
    if (!input)
      return {};
    const auto a = node(*input).type;
    if (a.builtin == BuiltinValue::String && !a.tensor && !a.array && !a.task && !a.fallible &&
        !a.device) {
      if (read.indices.size() != 1) {
        error("S063", "String indexing requires exactly one integer scalar index", span);
        return {};
      }
      auto index = expression(frame, read.indices[0]);
      if (!index)
        return {};
      const auto i = node(*index).type;
      if (i.builtin != BuiltinValue::Numeric || !isInteger(i.element) || i.tensor || i.array ||
          i.task || i.fallible || i.device) {
        error("S061", "String indexing requires a completed CPU integer scalar index", span);
        return {};
      }
      auto extent = stringCounts_.find(*input);
      auto known = intConstants_.find(*index);
      if ((extent != stringCounts_.end() && extent->second == 0) ||
          (known != intConstants_.end() &&
           (known->second < 0 || known->second > INT64_MAX ||
            (extent != stringCounts_.end() && known->second >= extent->second)))) {
        error("S062", "String index is outside its valid zero-based scalar range", span);
        return {};
      }
      return add(frame, a, span, StringIndex{*input, *index});
    }
    if (!a.tensor || a.task || a.fallible || a.device ||
        (a.builtin != BuiltinValue::Numeric && !isOwnedArray(a))) {
      error("S061", "indexing requires a completed CPU String, array or numeric tensor", span);
      return {};
    }
    if (read.indices.size() != a.shape.size()) {
      error("S063", "indexing requires exactly one integer index per dimension", span);
      return {};
    }
    std::vector<ValueId> indices;
    for (auto id : read.indices) {
      auto value = expression(frame, id);
      if (!value)
        return {};
      const auto i = node(*value).type;
      if (i.tensor || i.task || i.fallible || i.device || i.builtin != BuiltinValue::Numeric ||
          !isInteger(i.element)) {
        error("S061", "indexing requires completed CPU integer scalar indices",
              syntax_.expressions[id].span);
        return {};
      }
      indices.push_back(*value);
    }
    for (std::size_t axis = 0; axis < indices.size(); ++axis) {
      const auto &extent = a.shape[axis];
      if (auto known = intConstants_.find(indices[axis]); known != intConstants_.end()) {
        if (known->second < 0 || known->second > INT64_MAX ||
            (extent.constant && known->second >= *extent.constant)) {
          error("S062", "index is outside its valid zero-based dimension range", span);
          return {};
        }
      } else if (extent.constant && *extent.constant == 0) {
        error("S062", "cannot index an empty dimension", span);
        return {};
      }
    }
    ValueType result = isOwnedArray(a) ? recordArrayElement(a) : ValueType{};
    result.element = a.element;
    if (a.array)
      return add(frame, result, span, ArrayIndex{*input, indices[0]});
    return add(frame, result, span, TensorIndex{*input, std::move(indices)});
  }

  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const ArrayExpr &array,
                               const std::optional<ValueType> &expected) {
    if (expected && !expected->array) {
      error("S021", "array literals require an array context; use tensor(...) for tensors", span);
      return {};
    }
    if (array.elements.size() > limits_.maxTensorElements) {
      error("S024", "array literal element limit exceeded", span);
      return {};
    }
    if (!expected && array.elements.empty()) {
      error("S058", "empty arrays need an explicit element type", span);
      return {};
    }
    ValueType output{true, {{array.elements.size(), 0}}};
    output.array = true;
    if (expected) {
      output.element = expected->element;
      output.builtin = expected->builtin;
      output.record = expected->record;
      output.recordName = expected->recordName;
      output.callable = expected->callable;
    }
    std::vector<ValueId> elements;
    for (auto leaf : array.elements) {
      std::optional<ValueType> context;
      if (expected) {
        context = recordArrayElement(output);
      }
      auto value = expression(frame, leaf, context);
      if (!value)
        return {};
      const auto actual = node(*value).type;
      if (actual.tensor || actual.task || actual.fallible || actual.device ||
          (actual.builtin != BuiltinValue::Numeric && actual.builtin != BuiltinValue::Record &&
           actual.builtin != BuiltinValue::Bool && actual.builtin != BuiltinValue::String &&
           actual.builtin != BuiltinValue::Function)) {
        error("S058",
              "array elements require completed CPU numeric scalars, Bool, String, records or "
              "functions",
              syntax_.expressions[leaf].span);
        return {};
      }
      if (!expected && elements.empty()) {
        output.element = actual.element;
        output.builtin = actual.builtin;
        output.record = actual.record;
        output.recordName = actual.recordName;
        output.callable = actual.callable;
      }
      auto scalar = recordArrayElement(output);
      if (!conform(frame, *value, scalar, syntax_.expressions[leaf].span))
        return {};
      elements.push_back(*value);
    }
    if (!validSize(output, span))
      return {};
    if (isRecordArray(output) &&
        elements.size() > limits_.maxValues / recordInlineUnits(output_, *output.record)) {
      error("S024", "record array literal exceeds configured expanded storage budget", span);
      return {};
    }
    if (isFunctionArray(output) && elements.size() > limits_.maxValues / 2) {
      error("S024", "function array literal exceeds expanded storage budget", span);
      return {};
    }
    if (isStringArray(output) && elements.size() > limits_.maxValues / 3) {
      error("S024", "String array literal exceeds expanded storage budget", span);
      return {};
    }
    return add(frame, std::move(output), span, TensorLiteral{std::move(elements)});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const TaskExpr &operation,
                               const std::optional<ValueType> &expected) {
    if (operation.placement && !options_.placement) {
      error("S045",
            "placed calls require the optional Metal source compiler; CPU lowering does not ignore "
            "placement",
            span);
      return {};
    }
    if (operation.prefix == TaskPrefix::Spawn && frame.groups.empty()) {
      error("S037", "spawn requires an owning parallel group", span);
      return {};
    }
    if (operation.prefix == TaskPrefix::Spawn &&
        !std::holds_alternative<CallExpr>(syntax_.expressions[operation.operand].value)) {
      error("S037", "spawn requires a function invocation", span);
      return {};
    }
    std::optional<ValueType> context;
    if (operation.prefix == TaskPrefix::Try && expected) {
      context = *expected;
      context->fallible = true;
    }
    std::optional<ValueId> placement;
    std::optional<ValueId> value;
    if (operation.prefix == TaskPrefix::Spawn) {
      const auto &call = std::get<CallExpr>(syntax_.expressions[operation.operand].value);
      if (const auto *field = std::get_if<FieldExpr>(&syntax_.expressions[call.callee].value)) {
      if (const auto *owner = std::get_if<NameExpr>(&syntax_.expressions[field->input].value);
          owner && !lookupSymbol(frame, owner->name) && name(field->field) == "int32" &&
          (name(owner->name) == "args" || name(owner->name) == "random"))
        return runtimeInput(frame, span, name(owner->name) + ".int32", call.arguments);
    }
    const auto *callee = std::get_if<NameExpr>(&syntax_.expressions[call.callee].value);
      if (!callee) {
        if (operation.placement) {
          error("S074", "indirect spawn requires ordinary CPU execution", span);
          return {};
        }
        value = invokeValue(frame, syntax_.expressions[operation.operand].span, call.callee,
                            call.arguments);
      } else
        value = invoke(frame, syntax_.expressions[operation.operand].span, callee->name,
                       call.arguments, {}, operation.placement, &placement, true);
    } else
      value = expression(frame, operation.operand, context);
    if (!value)
      return {};
    auto result = node(*value).type;
    if (operation.prefix == TaskPrefix::Spawn) {
      if (!std::holds_alternative<FunctionCall>(node(*value).operation) &&
          !std::holds_alternative<FunctionInvoke>(node(*value).operation) &&
          !std::holds_alternative<Matmul>(node(*value).operation) &&
          !std::holds_alternative<NumericConvert>(node(*value).operation) &&
          !std::holds_alternative<ReduceSum>(node(*value).operation) &&
          !std::holds_alternative<ArrayMap>(node(*value).operation) &&
          !std::holds_alternative<ArrayToTensor>(node(*value).operation) &&
          !std::holds_alternative<TensorReshape>(node(*value).operation) &&
          !std::holds_alternative<TensorToArray>(node(*value).operation) &&
          !std::holds_alternative<ArraySlice>(node(*value).operation) &&
          !std::holds_alternative<ArrayConcat>(node(*value).operation) &&
          !std::holds_alternative<CollectionQuery>(node(*value).operation)) {
        error("S037", "this invocation is not a supported task entry", span);
        return {};
      }
      if (options_.placement && !placement) {
        ValueType type;
        type.builtin = BuiltinValue::Device;
        placement = add(frame, type, span, DeviceSelection{{}});
        if (!placement)
          return {};
      }
      if (result.fallible)
        frame.tasks->handled.insert(result.identity);
      result.task = true;
      result.identity = 0;
      auto task = add(frame, result, span, TaskOperation{operation.prefix, *value, placement});
      if (task)
        frame.tasks->children[frame.groups.back()].push_back(*task);
      return task;
    }
    if (operation.prefix == TaskPrefix::Await) {
      if (!result.task) {
        error("S037", "await requires a task handle", span);
        return {};
      }
      frame.tasks->observed.insert(result.identity);
      result.task = false;
      if (result.errorRecord) {
        auto family = combineErrors({*result.errorRecord, taskFamily()}, span);
        if (!family)
          return {};
        result.errorRecord = normalizedError(*family);
      }
      result.fallible = true;
      result.identity = 0;
      return add(frame, result, span, TaskOperation{operation.prefix, *value});
    }
    if (result.task || !result.fallible) {
      error("S036", "try requires an expected-error result", span);
      return {};
    }
    if (!propagateError(frame, result.errorRecord, span))
      return {};
    frame.tasks->handled.insert(result.identity);
    result.fallible = false;
    result.errorRecord.reset();
    result.identity = 0;
    return add(frame, result, span, TaskOperation{operation.prefix, *value});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const ParallelExpr &block,
                               const std::optional<ValueType> &expected) {
    Frame nested = frame;
    const auto group = nextGroup_++;
    nested.groups.push_back(group);
    auto errors = std::make_shared<ValueType>();
    errors->fallible = true; // Every group can fail during execution.
    nested.groupErrors.push_back(errors);
    std::vector<TypedStatement> statements;
    std::unordered_set<std::string> bindings;
    std::optional<ValueId> result;
    for (std::size_t index = 0; index < block.body.size(); ++index) {
      const auto &statement = block.body[index];
      std::optional<ValueType> context;
      if (statement.annotation)
        context = type(*statement.annotation);
      else if (index + 1 == block.body.size() && expected) {
        context = *expected;
        context->task = context->fallible = false;
        context->identity = 0;
      }
      auto value = statementValue(nested, statement, context, index + 1 == block.body.size());
      if (!value)
        return {};
      std::optional<SymbolId> symbol;
      if (statement.binding) {
        auto binding = name(*statement.binding);
        if (reservedControl(binding)) {
          error("S056", "Control-flow keywords cannot be binding names", *statement.binding);
          return {};
        }
        if (!bindings.insert(binding).second) {
          error("S008", "duplicate immutable binding", *statement.binding);
          return {};
        }
        symbol = output_.symbols.size();
        output_.symbols.push_back(
            {binding, node(*value).type, *statement.binding, frame.function, false});
        nested.locals[binding] = *symbol;
        if (auto known = stringCounts_.find(*value); known != stringCounts_.end())
          stringBindings_[*symbol] = known->second;
      }
      statements.push_back({statement.span, symbol, *value});
      result = value;
    }
    if (!result)
      return {};
    for (auto child : frame.tasks->children[group])
      if (!frame.tasks->observed.contains(node(child).type.identity))
        error("S038", "every child result must be observed on a successful group path",
              node(child).span);
    auto resultType = node(*result).type;
    if (resultType.task || resultType.fallible || resultType.builtin == BuiltinValue::Device) {
      error("S035", "parallel body must produce a completed value; use try await", span);
      return {};
    }
    resultType.fallible = true;
    resultType.errorRecord = errors->errorRecord;
    resultType.identity = 0;
    return add(frame, resultType, span, ParallelOperation{std::move(statements), *result, group});
  }
  // Branch-local obligations must be discharged on the path which creates
  // them. Conditional observation cannot discharge an incoming obligation.
  void finishBranch(const TaskChecks &before, Frame &branch, ValueId result, SourceSpan span) {
    if (node(result).type.fallible)
      branch.tasks->handled.insert(node(result).type.identity);
    for (auto pending : branch.tasks->pending)
      if (!before.pending.contains(pending) && !branch.tasks->handled.contains(pending))
        error("S036", "branch discards an expected error", span);
    for (const auto &[group, children] : branch.tasks->children) {
      auto old = before.children.find(group);
      for (auto child : children) {
        const bool incoming = old != before.children.end() &&
                              std::ranges::find(old->second, child) != old->second.end();
        if (!incoming && !branch.tasks->observed.contains(node(child).type.identity))
          error("S038", "branch leaves an unobserved child", span);
      }
    }
  }
  std::optional<ValueId> statementValue(Frame &frame, const Statement &statement,
                                        const std::optional<ValueType> &expected, bool last) {
    const auto &ast = syntax_.expressions[statement.expression];
    const auto *guard = std::get_if<ConditionalExpr>(&ast.value);
    if (!guard || !guard->whenFalse.empty())
      return expression(frame, statement.expression, expected);
    if (last || statement.binding || guard->whenTrue.empty() ||
        !std::holds_alternative<FailExpr>(
            syntax_.expressions[guard->whenTrue.back().expression].value)) {
      error("S073", "an if without else is a non-result guard whose body must end with fail",
            ast.span);
      return {};
    }
    auto condition = expression(frame, guard->condition);
    if (!condition)
      return {};
    const auto &t = node(*condition).type;
    if (t.builtin != BuiltinValue::Bool || t.tensor || t.task || t.fallible) {
      error("S057", "guard conditions require a completed Bool", ast.span);
      return {};
    }
    auto branch = frame;
    branch.conditional = true;
    branch.tasks = std::make_shared<TaskChecks>(*frame.tasks);
    std::vector<TypedStatement> body;
    auto context = expected ? expected : output_.functions[frame.function].declaredResult;
    auto result = suite(branch, guard->whenTrue, context, body);
    if (!result)
      return {};
    finishBranch(*frame.tasks, branch, *result, ast.span);
    ValueType unit;
    unit.builtin = BuiltinValue::Bool;
    return add(frame, unit, ast.span, GuardOperation{*condition, std::move(body), *result});
  }
  std::optional<ValueId> suite(Frame &frame, const std::vector<Statement> &body,
                               std::optional<ValueType> context,
                               std::vector<TypedStatement> &statements,
                               const std::unordered_set<std::string> &introduced = {}) {
    auto bindings = introduced;
    std::optional<ValueId> result;
    for (std::size_t index = 0; index < body.size(); ++index) {
      const auto &statement = body[index];
      auto expected = index + 1 == body.size() ? context : std::optional<ValueType>{};
      if (statement.annotation) {
        expected = type(*statement.annotation);
        if (!expected)
          return {};
      }
      auto value = statementValue(frame, statement, expected, index + 1 == body.size());
      if (!value)
        return {};
      std::optional<SymbolId> symbol;
      if (statement.binding) {
        auto binding = name(*statement.binding);
        if (reservedControl(binding)) {
          error("S056", "Control-flow keywords cannot be binding names", *statement.binding);
          return {};
        }
        if (!bindings.insert(binding).second) {
          error("S008", "duplicate immutable binding", *statement.binding);
          return {};
        }
        symbol = output_.symbols.size();
        output_.symbols.push_back(
            {binding, node(*value).type, *statement.binding, frame.function, false});
        frame.locals[binding] = *symbol;
        if (auto known = stringCounts_.find(*value); known != stringCounts_.end())
          stringBindings_[*symbol] = known->second;
      }
      statements.push_back({statement.span, symbol, *value});
      result = value;
    }
    return result;
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const CatchExpr &handler,
                               const std::optional<ValueType> &expected) {
    if (reservedControl(name(handler.parameter))) {
      error("S056", "Control-flow keywords cannot be handler parameter names", handler.parameter);
      return {};
    }
    auto context = expected;
    if (context)
      context->fallible = true;
    auto operand = expression(frame, handler.operand, context);
    if (!operand)
      return {};
    auto resultType = node(*operand).type;
    if (resultType.task || !resultType.fallible) {
      error("S039", "catch requires a completed expected-error result", span);
      return {};
    }
    frame.tasks->handled.insert(resultType.identity);
    const auto before = *frame.tasks;
    auto nested = frame;
    nested.tasks = std::make_shared<TaskChecks>(before);
    nested.handlerFallible = std::make_shared<bool>(false);
    nested.handlerErrorRecord = std::make_shared<std::optional<std::size_t>>();
    nested.handlerDepth = nested.groups.size();
    nested.conditional = true;
    ValueType errorType;
    if (resultType.errorRecord)
      errorType = recordType(*resultType.errorRecord);
    else
      errorType.builtin = BuiltinValue::TaskError;
    const auto symbol = output_.symbols.size();
    output_.symbols.push_back(
        {name(handler.parameter), errorType, handler.parameter, frame.function, true});
    nested.locals[name(handler.parameter)] = symbol;
    std::vector<TypedStatement> body;
    auto handlerContext = resultType;
    // The success path's inferred extent is not a constraint on a fallback.
    // Preserve explicit outer constraints; otherwise join differing extents.
    if (expected)
      handlerContext.shape = expected->shape;
    else
      for (auto &dimension : handlerContext.shape)
        dimension = dynamic();
    auto result = suite(nested, handler.body, handlerContext, body, {name(handler.parameter)});
    if (!result)
      return {};
    finishBranch(before, nested, *result, span);
    const auto handlerType = node(*result).type;
    if (resultType.device != handlerType.device) {
      error("S047", "catch branches require matching residency and GPU index; transfer explicitly",
            span);
      return {};
    }
    for (std::size_t axis = 0; axis < resultType.shape.size(); ++axis)
      if (!same(resultType.shape[axis], handlerType.shape[axis]))
        resultType.shape[axis] = dynamic();
    resultType.fallible = false;
    resultType.errorRecord.reset();
    if (!joinErrors(resultType, handlerType, span))
      return {};
    if (*nested.handlerFallible) {
      auto escaping = resultType;
      escaping.fallible = true;
      escaping.errorRecord = *nested.handlerErrorRecord;
      if (!joinErrors(resultType, escaping, span))
        return {};
    }
    resultType.identity = 0;
    return add(frame, resultType, span, CatchOperation{*operand, symbol, std::move(body), *result});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const FailExpr &failure,
                               const std::optional<ValueType> &expected) {
    auto value = expression(frame, failure.operand);
    if (!value)
      return {};
    const auto &errorValue = node(*value).type;
    const bool application = errorValue.builtin == BuiltinValue::Record && errorValue.record &&
                             output_.records[*errorValue.record].error && !errorValue.tensor;
    if ((!application && errorValue.builtin != BuiltinValue::TaskError) || errorValue.fallible ||
        errorValue.task) {
      error("S039", "fail requires a declared error value", span);
      return {};
    }
    if (!expected ||
        (expected->builtin != BuiltinValue::Numeric && expected->builtin != BuiltinValue::Bool &&
         expected->builtin != BuiltinValue::Record && expected->builtin != BuiltinValue::String &&
         expected->builtin != BuiltinValue::Function) ||
        expected->task) {
      error("S039", "fail needs a concrete success-type context in this subset", span);
      return {};
    }
    const auto family = application ? errorValue.record : std::optional<std::size_t>{};
    if (!propagateError(frame, family, span))
      return {};
    auto result = *expected;
    result.fallible = true;
    result.errorRecord = family;
    result.identity = 0;
    return add(frame, result, span, FailOperation{*value});
  }
  template <class Evaluate>
  std::optional<ValueId> conditional(Frame &frame, SourceSpan span, ValueId condition,
                                     const std::optional<ValueType> &expected, Evaluate evaluate) {
    const auto conditionType = node(condition).type;
    if (conditionType.builtin != BuiltinValue::Bool || conditionType.task ||
        conditionType.fallible || conditionType.tensor || conditionType.device) {
      error("S057", "conditions require a completed CPU Bool", node(condition).span);
      return {};
    }
    const auto before = *frame.tasks;
    std::vector<std::shared_ptr<TaskChecks>> paths;
    std::vector<std::vector<TypedStatement>> bodies(2);
    std::vector<ValueId> results;
    std::optional<ValueType> resultType;
    auto context = expected;
    if (context)
      context->fallible = true;
    for (unsigned index = 0; index < 2; ++index) {
      auto branch = frame;
      branch.conditional = true;
      branch.tasks = std::make_shared<TaskChecks>(before);
      auto value = evaluate(branch, index, context, bodies[index]);
      if (!value)
        return {};
      auto actual = node(*value).type;
      if (actual.task || actual.builtin == BuiltinValue::Device) {
        error("S035", "conditional branches cannot return task handles or device selectors", span);
        return {};
      }
      if (resultType) {
        if (actual.builtin != resultType->builtin || actual.record != resultType->record ||
            (actual.builtin == BuiltinValue::Function &&
             !sameFunctionValueType(recordArrayElement(actual), recordArrayElement(*resultType))) ||
            (actual.builtin == BuiltinValue::Numeric && actual.element != resultType->element) ||
            actual.tensor != resultType->tensor || actual.array != resultType->array ||
            actual.shape.size() != resultType->shape.size()) {
          error("S043", "conditional branches require a common success type", span);
          return {};
        }
        if (actual.device != resultType->device) {
          error("S047", "conditional branches require matching residency", span);
          return {};
        }
        if (!joinErrors(*resultType, actual, span))
          return {};
        for (std::size_t axis = 0; axis < actual.shape.size(); ++axis)
          if (!same(resultType->shape[axis], actual.shape[axis]))
            resultType->shape[axis] = dynamic();
      } else
        resultType = actual;
      finishBranch(before, branch, *value, span);
      paths.push_back(branch.tasks);
      results.push_back(*value);
    }
    for (auto pending : before.pending)
      if (std::ranges::all_of(paths,
                              [&](const auto &path) { return path->handled.contains(pending); }))
        frame.tasks->handled.insert(pending);
    for (const auto &[group, children] : before.children)
      for (auto child : children)
        if (std::ranges::all_of(paths, [&](const auto &path) {
              return path->observed.contains(node(child).type.identity);
            }))
          frame.tasks->observed.insert(node(child).type.identity);
    resultType->identity = 0;
    return add(frame, *resultType, span,
               ConditionalOperation{condition, std::move(bodies[0]), std::move(bodies[1]),
                                    results[0], results[1]});
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const ConditionalExpr &choice,
                               const std::optional<ValueType> &expected) {
    if (choice.whenFalse.empty()) {
      error("S073", "if expressions require an else branch", span);
      return {};
    }
    auto condition = expression(frame, choice.condition);
    if (!condition)
      return {};
    return conditional(frame, span, *condition, expected,
                       [&](Frame &branch, unsigned index, const std::optional<ValueType> &context,
                           std::vector<TypedStatement> &body) {
                         return suite(branch, index == 0 ? choice.whenTrue : choice.whenFalse,
                                      context, body);
                       });
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const LogicalExpr &logical,
                               const std::optional<ValueType> &) {
    auto left = expression(frame, logical.left);
    if (!left)
      return {};
    ValueType boolean;
    boolean.builtin = BuiltinValue::Bool;
    return conditional(frame, span, *left, boolean,
                       [&](Frame &branch, unsigned index, const std::optional<ValueType> &,
                           std::vector<TypedStatement> &body) {
                         auto value =
                             (index == 0) == logical.conjunction
                                 ? expression(branch, logical.right, boolean)
                                 : add(branch, boolean, span, BoolLiteral{!logical.conjunction});
                         if (value)
                           body.push_back({span, {}, *value});
                         return value;
                       });
  }
  std::optional<ValueId> infer(Frame &frame, SourceSpan span, const MatchExpr &match,
                               const std::optional<ValueType> &expected) {
    auto subject = expression(frame, match.subject);
    if (!subject)
      return {};
    const auto subjectType = node(*subject).type;
    const bool enumeration = subjectType.builtin == BuiltinValue::Record && subjectType.record &&
                             !output_.records[*subjectType.record].variants.empty();
    if (enumeration &&
        (frame.enumBranchDepth > limits_.maxDepth - frame.depth ||
         match.arms.size() > limits_.maxDepth - frame.depth - frame.enumBranchDepth)) {
      error("S071", "enum match exceeds configured branch-depth budget", span);
      return {};
    }
    const bool taskError = subjectType.builtin == BuiltinValue::TaskError;
    if ((!enumeration && !taskError && subjectType.builtin != BuiltinValue::FaultKind) ||
        subjectType.task || subjectType.fallible || subjectType.tensor || subjectType.array) {
      error("S040", "match requires a completed enum, TaskError or FaultKind value", span);
      return {};
    }
    std::vector<std::string> names =
        taskError
            ? std::vector<std::string>{"Fault", "Cancelled", "DeadlineExceeded",
                                       "AdmissionRejected"}
            : std::vector<std::string>{"Arithmetic", "Bounds", "Shape", "Allocation", "Native"};
    const std::string family = enumeration ? output_.records[*subjectType.record].name
                               : taskError ? "TaskError"
                                           : "FaultKind";
    if (enumeration) {
      names.clear();
      for (const auto &v : output_.records[*subjectType.record].variants)
        names.push_back(v.name);
    }
    std::unordered_set<unsigned> seen;
    bool fallback = false;
    std::vector<MatchCase> cases;
    const auto before = *frame.tasks;
    std::vector<std::shared_ptr<TaskChecks>> paths;
    std::optional<ValueType> resultType;
    auto context = expected;
    if (context)
      context->fallible = true;
    for (const auto &arm : match.arms) {
      std::optional<unsigned> tag;
      if (arm.variant) {
        const auto owner = records_.find(functionKey(frame, name(*arm.family)));
        if (enumeration && !output_.records[*subjectType.record].errorMembers.empty()) {
          const auto &joined = output_.records[*subjectType.record];
          const auto selectedFamily = name(*arm.family) == "TaskError" ? taskErrorFamily(output_)
                                      : owner != records_.end() ? std::optional{owner->second}
                                                                : std::nullopt;
          if (selectedFamily && !lookupSymbol(frame, *arm.family)) {
            if (!fallback && completing(*arm.variant)) {
              std::vector<std::string> candidates;
              for (std::size_t i = 0; i < joined.errorOrigins.size(); ++i) {
                const auto [original, variant] = joined.errorOrigins[i];
                if (original == *selectedFamily && !seen.contains(static_cast<unsigned>(i + 1)))
                  candidates.push_back(output_.records[original].variants[variant].name);
              }
              completeMembers(*arm.variant, std::move(candidates), ScopeNameKind::EnumCase);
            }
            for (std::size_t i = 0; i < joined.errorOrigins.size(); ++i) {
              const auto [original, variant] = joined.errorOrigins[i];
              if (original == *selectedFamily &&
                  output_.records[original].variants[variant].name == name(*arm.variant))
                tag = static_cast<unsigned>(i + 1);
            }
          }
        } else {
          auto found = std::ranges::find(names, name(*arm.variant));
          const bool sameFamily =
              enumeration ? (owner != records_.end() && owner->second == *subjectType.record &&
                             !lookupSymbol(frame, *arm.family))
                          : name(*arm.family) == family;
          if (sameFamily && !fallback && completing(*arm.variant)) {
            std::vector<std::string> candidates;
            for (std::size_t i = 0; i < names.size(); ++i)
              if (!seen.contains(static_cast<unsigned>(i + 1)))
                candidates.push_back(names[i]);
            completeMembers(*arm.variant, std::move(candidates), ScopeNameKind::EnumCase);
          }
          if (sameFamily && found != names.end())
            tag = static_cast<unsigned>(found - names.begin()) + 1;
        }
        if (!tag) {
          error("S040", "unknown variant for " + std::string(family), arm.span);
          return {};
        }
        if (fallback || !seen.insert(*tag).second) {
          error("S041", "duplicate or unreachable match case", arm.span);
          return {};
        }
      } else {
        if (fallback || seen.size() == names.size()) {
          error("S041", "unreachable fallback", arm.span);
          return {};
        }
        fallback = true;
      }
      if (tag) {
        if (enumeration) {
          const auto [original, variant] = errorOrigin(output_, *subjectType.record, *tag - 1);
          rememberTypeUse(*arm.family, recordType(original));
          const auto declaration = output_.records[original].variants[variant].declaration;
          rememberSourceToken(
              {*arm.variant,
               declaration.end > declaration.begin ? std::optional{declaration} : std::nullopt,
               recordType(original),
               {}});
        } else {
          rememberTypeUse(*arm.family, subjectType);
          rememberSourceToken({*arm.variant, {}, subjectType, {}});
        }
      }
      auto branch = frame;
      if (enumeration)
        branch.enumBranchDepth += match.arms.size();
      branch.conditional = true;
      branch.tasks = std::make_shared<TaskChecks>(before);
      std::vector<TypedStatement> statements;
      // The original subject remains the source for dispatch and field slots.
      // Only its lexical binding is replaced in this branch; snapshots and
      // bindings in other branches retain their original types.
      const auto *read = std::get_if<SymbolRead>(&node(*subject).operation);
      if (read && enumeration && !output_.records[*subjectType.record].errorMembers.empty()) {
        const auto originalSymbol = read->symbol;
        std::vector<std::size_t> remaining;
        if (tag)
          remaining.push_back(errorOrigin(output_, *subjectType.record, *tag - 1).first);
        else
          for (std::size_t i = 0; i < names.size(); ++i)
            if (!seen.contains(static_cast<unsigned>(i + 1)))
              remaining.push_back(errorOrigin(output_, *subjectType.record, i).first);
        auto family = combineErrors(std::move(remaining), arm.span);
        if (!family)
          return {};
        if (*family != *subjectType.record) {
          auto narrowedType = recordType(*family);
          auto narrowed = add(branch, narrowedType, arm.span, ErrorNarrow{*subject});
          if (!narrowed)
            return {};
          const auto binding = output_.symbols[originalSymbol].name;
          const auto declaration = output_.symbols[originalSymbol].declaration;
          const auto symbol = output_.symbols.size();
          output_.symbols.push_back({binding, narrowedType, declaration, frame.function, false,
                                     output_.symbols[originalSymbol].definition});
          branch.locals[binding] = symbol;
          statements.push_back({arm.span, symbol, *narrowed});
        }
      }
      std::unordered_set<std::string> fields, fieldBindings;
      for (const auto &pattern : arm.fields) {
        const auto fieldSpan = pattern.name;
        const auto field = name(fieldSpan);
        const auto binding = name(pattern.binding);
        if (reservedControl(binding)) {
          error("S056", "Control-flow keywords cannot be pattern binding names", pattern.binding);
          return {};
        }
        ErrorField fieldId = ErrorField::Kind;
        std::optional<std::size_t> slot, taskSlot;
        std::optional<unsigned> taskVariant = taskError && tag ? tag : std::nullopt;
        if (enumeration && tag) {
          const auto [original, variant] = errorOrigin(output_, *subjectType.record, *tag - 1);
          if (output_.records[original].taskErrorFamily) {
            taskVariant = static_cast<unsigned>(variant + 1);
            taskSlot = output_.records[*subjectType.record].variants[*tag - 1].fields.front();
          }
        }
        if (completing(fieldSpan)) {
          std::vector<std::string> candidates;
          if (enumeration && tag && !taskVariant) {
            const auto &record = output_.records[*subjectType.record];
            for (const auto index : record.variants[*tag - 1].fields)
              candidates.push_back(record.fields[index].name);
          } else if (taskVariant == 1)
            candidates = {"kind", "message"};
          else if (taskVariant == 4)
            candidates = {"reason"};
          std::unordered_set<std::string> supplied;
          for (const auto &other : arm.fields)
            if (other.name.begin != fieldSpan.begin || other.name.end != fieldSpan.end)
              supplied.insert(name(other.name));
          std::erase_if(candidates,
                        [&](const auto &candidate) { return supplied.contains(candidate); });
          completeMembers(fieldSpan, std::move(candidates), ScopeNameKind::Field);
        }
        ValueType fieldType;
        if (enumeration && tag && !taskVariant) {
          const auto &record = output_.records[*subjectType.record];
          const auto &variant = record.variants[*tag - 1];
          const auto found = std::ranges::find_if(
              variant.fields, [&](auto i) { return record.fields[i].name == field; });
          if (found == variant.fields.end()) {
            error("S042", "unknown field for selected variant", fieldSpan);
            return {};
          }
          slot = *found;
          fieldType = record.fields[*slot].type;
          for (auto &extent : fieldType.shape)
            if (!extent.constant)
              extent = dynamic();
        } else if (taskVariant == 1 && field == "kind") {
          fieldId = ErrorField::Kind;
          fieldType.builtin = BuiltinValue::FaultKind;
        } else if (taskVariant == 1 && field == "message") {
          fieldId = ErrorField::Message;
          fieldType.builtin = BuiltinValue::String;
        } else if (taskVariant == 4 && field == "reason") {
          fieldId = ErrorField::Reason;
          fieldType.builtin = BuiltinValue::String;
        } else {
          error("S042", "unknown field for selected variant", fieldSpan);
          return {};
        }
        if (!fields.insert(field).second || !fieldBindings.insert(binding).second) {
          error("S042", "duplicate pattern field", fieldSpan);
          return {};
        }
        const bool shorthand =
            fieldSpan.begin == pattern.binding.begin && fieldSpan.end == pattern.binding.end;
        const auto declaration =
            slot ? std::optional{output_.records[*subjectType.record].fields[*slot].declaration}
                 : std::nullopt;
        rememberSourceToken({fieldSpan, declaration, fieldType, {}, shorthand});
        auto value =
            slot ? add(branch, fieldType, fieldSpan, RecordFieldRead{*subject, *slot})
                 : add(branch, fieldType, fieldSpan, ErrorFieldRead{*subject, fieldId, taskSlot});
        if (!value)
          return {};
        const auto symbol = output_.symbols.size();
        output_.symbols.push_back({binding, fieldType, pattern.binding, frame.function, false});
        branch.locals[binding] = symbol;
        statements.push_back({fieldSpan, symbol, *value});
      }
      auto value = suite(branch, arm.body, context, statements, fieldBindings);
      if (!value)
        return {};
      auto actual = node(*value).type;
      if (actual.task || actual.builtin == BuiltinValue::Device) {
        error("S035", "match cannot return a task handle", arm.span);
        return {};
      }
      if (resultType) {
        if (actual.device != resultType->device) {
          error("S047",
                "match branches require matching residency and GPU index; transfer explicitly",
                arm.span);
          return {};
        }
        if (actual.builtin != resultType->builtin || actual.record != resultType->record ||
            (actual.builtin == BuiltinValue::Function &&
             !sameFunctionValueType(recordArrayElement(actual), recordArrayElement(*resultType))) ||
            (actual.builtin == BuiltinValue::Numeric && actual.element != resultType->element) ||
            actual.tensor != resultType->tensor || actual.array != resultType->array ||
            actual.shape.size() != resultType->shape.size()) {
          error("S043", "match branches need a common success type", arm.span);
          return {};
        }
        if (!joinErrors(*resultType, actual, span))
          return {};
        for (std::size_t axis = 0; axis < actual.shape.size(); ++axis)
          if (!same(resultType->shape[axis], actual.shape[axis]))
            resultType->shape[axis] = dynamic();
      } else
        resultType = actual;
      finishBranch(before, branch, *value, arm.span);
      paths.push_back(branch.tasks);
      cases.push_back({tag, std::move(statements), *value});
    }
    if (!fallback && seen.size() != names.size()) {
      error("S044", "non-exhaustive match over " + std::string(family), span);
      return {};
    }
    for (auto pending : before.pending)
      if (std::ranges::all_of(paths, [&](const auto &p) { return p->handled.contains(pending); }))
        frame.tasks->handled.insert(pending);
    for (const auto &[group, children] : before.children)
      for (auto child : children)
        if (std::ranges::all_of(paths, [&](const auto &p) {
              return p->observed.contains(node(child).type.identity);
            }))
          frame.tasks->observed.insert(node(child).type.identity);
    resultType->identity = 0;
    // Keep recursive matches explicit for bounded flat-switch lowering and
    // independent executable-IR validation. The established acyclic path stays.
    if (enumeration && !recursiveValues_) {
      ValueType tagType;
      tagType.element = NumericType::Int64;
      auto selected = add(frame, tagType, span, RecordFieldRead{*subject, 0});
      if (!selected)
        return {};
      auto result = cases.back().result;
      auto otherwise = std::move(cases.back().body);
      if (cases.size() == 1) {
        auto one = add(frame, tagType, span, Int64Literal{1});
        if (!one)
          return {};
        ValueType boolean;
        boolean.builtin = BuiltinValue::Bool;
        auto condition =
            add(frame, boolean, span, ScalarCompare{ComparisonKind::GreaterEqual, *selected, *one});
        if (!condition)
          return {};
        return add(frame, *resultType, span,
                   ConditionalOperation{*condition, otherwise, otherwise, result, result});
      }
      for (std::size_t i = cases.size() - 1; i > 0; --i) {
        auto &arm = cases[i - 1];
        auto tag = add(frame, tagType, span, Int64Literal{*arm.tag});
        if (!tag)
          return {};
        ValueType boolean;
        boolean.builtin = BuiltinValue::Bool;
        auto condition =
            add(frame, boolean, span, ScalarCompare{ComparisonKind::Equal, *selected, *tag});
        if (!condition)
          return {};
        // Only synthesized dispatch nodes create an obligation in this frame.
        // Incoming results retain the all-branches handling rule above.
        if (i + 1 < cases.size() && node(result).type.fallible)
          frame.tasks->handled.insert(node(result).type.identity);
        auto choice = add(frame, *resultType, span,
                          ConditionalOperation{*condition, std::move(arm.body),
                                               std::move(otherwise), arm.result, result});
        if (!choice)
          return {};
        result = *choice;
        otherwise = {{span, {}, result}};
      }
      return result;
    }
    return add(frame, *resultType, span, MatchOperation{*subject, std::move(cases)});
  }
  std::optional<TensorData> tensorData(ExprId id, const std::optional<ValueType> &context,
                                       std::size_t depth = 0) {
    const auto &syntax = syntax_.expressions[id];
    const auto *array = std::get_if<ArrayExpr>(&syntax.value);
    if (!array)
      return TensorData{{}, {id}};
    if (depth >= limits_.maxDepth) {
      error("S012", "tensor literal nesting limit exceeded", syntax.span);
      return {};
    }
    TensorData result{{array->elements.size()}, {}};
    if (array->elements.empty())
      return result;
    std::optional<std::vector<std::uint64_t>> inner;
    for (auto child : array->elements) {
      auto data = tensorData(child, context, depth + 1);
      if (!data)
        return {};
      if (inner && *inner != data->shape) {
        error("S023",
              "ragged tensor literal: inconsistent shape at depth " + std::to_string(depth + 1),
              syntax_.expressions[child].span);
        return {};
      }
      inner = data->shape;
      if (data->leaves.size() > limits_.maxTensorElements - result.leaves.size()) {
        error("S024", "tensor literal element limit exceeded", syntax.span);
        return {};
      }
      result.leaves.insert(result.leaves.end(), data->leaves.begin(), data->leaves.end());
    }
    result.shape.insert(result.shape.end(), inner->begin(), inner->end());
    return result;
  }
  std::optional<ValueId> arrayTensor(Frame &frame, SourceSpan span, ValueId input,
                                     std::optional<ExprId> shapeSyntax,
                                     const std::optional<ValueType> &expected,
                                     bool reshaping = false) {
    auto output = node(input).type;
    if (output.array == reshaping || output.task || output.fallible || output.device ||
        !output.tensor || output.builtin != BuiltinValue::Numeric ||
        (!reshaping && output.shape.size() != 1)) {
      error("S026",
            reshaping ? "reshape requires a completed CPU numeric tensor"
                      : "tensor construction requires a completed CPU numeric array",
            span);
      return {};
    }
    output.array = false;
    ArrayToTensor operation{input};
    if (shapeSyntax) {
      const auto *shape = std::get_if<ArrayExpr>(&syntax_.expressions[*shapeSyntax].value);
      if (!shape || shape->elements.size() > limits_.maxDepth) {
        error("S050",
              "tensor shape requires a bounded list of integer dimensions with a fixed rank", span);
        return {};
      }
      operation.dimensions.emplace();
      output.shape.clear();
      for (auto axis : shape->elements) {
        auto value = expression(frame, axis);
        if (!value)
          return {};
        const auto &actual = node(*value).type;
        if (actual.builtin != BuiltinValue::Numeric || !isInteger(actual.element) ||
            actual.tensor || actual.task || actual.fallible || actual.device) {
          error("S050", "tensor dimensions require completed CPU integer scalars",
                syntax_.expressions[axis].span);
          return {};
        }
        operation.dimensions->push_back(*value);
        if (auto constant = intConstants_.find(*value); constant != intConstants_.end()) {
          if (constant->second < 0 || constant->second > INT64_MAX) {
            error("S050", "tensor dimensions must fit the nonnegative 64-bit indexing range",
                  syntax_.expressions[axis].span);
            return {};
          }
          output.shape.push_back({std::uint64_t(constant->second), 0});
        } else
          output.shape.push_back(dynamic());
      }
      if (expected && expected->shape.size() == output.shape.size())
        for (std::size_t axis = 0; axis < output.shape.size(); ++axis)
          if (expected->shape[axis].constant) {
            if (output.shape[axis].constant &&
                output.shape[axis].constant != expected->shape[axis].constant) {
              error("S011", "reshape dimension differs from expected tensor extent", span);
              return {};
            }
            output.shape[axis] = expected->shape[axis];
          }
      if (!validSize(output, span))
        return {};
      bool known = true;
      std::uint64_t stride = 1;
      for (auto axis = output.shape.rbegin(); axis != output.shape.rend(); ++axis) {
        if (!axis->constant) {
          known = false;
          continue;
        }
        if (*axis->constant == 0) {
          stride = 0;
          known = true;
          continue;
        }
        if (known) {
          if (stride > std::uint64_t(INT64_MAX) / *axis->constant) {
            error("S002", "tensor shape overflows row-major strides", span);
            return {};
          }
          stride *= *axis->constant;
        }
      }
      if (std::ranges::any_of(output.shape,
                              [](const Extent &axis) { return axis.constant == 0; }) ||
          std::ranges::all_of(output.shape,
                              [](const Extent &axis) { return axis.constant.has_value(); })) {
        std::uint64_t count = 1;
        if (std::ranges::any_of(output.shape,
                                [](const Extent &axis) { return axis.constant == 0; }))
          count = 0;
        else
          for (const auto &axis : output.shape)
            count *= *axis.constant;
        if (!reshaping) {
          if (!requireEqual(frame, node(input).type.shape[0], Extent{count, 0}))
            return {};
          const auto length = node(input).type.shape[0].constant;
          if (length && *length != count) {
            error("S050", "tensor shape element count does not match array length", span);
            return {};
          }
        } else {
          const auto &source = node(input).type.shape;
          std::optional<std::uint64_t> length;
          if (std::ranges::any_of(source, [](const Extent &d) { return d.constant == 0; }))
            length = 0;
          else if (std::ranges::all_of(source,
                                       [](const Extent &d) { return d.constant.has_value(); })) {
            length = 1;
            for (const auto &d : source)
              *length *= *d.constant;
          }
          if (length && *length != count) {
            error("S050", "reshape must preserve the tensor element count", span);
            return {};
          }
        }
      }
    }
    if (!validSize(output, span))
      return {};
    if (reshaping)
      return add(frame, output, span, TensorReshape{input, std::move(*operation.dimensions)});
    return add(frame, output, span, std::move(operation));
  }
  std::optional<ValueId> tensor(Frame &frame, SourceSpan span, const std::vector<ExprId> &arguments,
                                const std::optional<ValueType> &expected) {
    if (arguments.size() != 1 && arguments.size() != 2) {
      error("S025", "tensor expects numeric data and an optional fixed-rank shape list", span);
      return {};
    }
    if (expected && (!expected->tensor || expected->array)) {
      error("S026", "tensor construction requires a tensor result context", span);
      return {};
    }
    bool runtimeLiteralShape = false;
    if (arguments.size() == 2)
      if (const auto *shape = std::get_if<ArrayExpr>(&syntax_.expressions[arguments[1]].value))
        runtimeLiteralShape = std::ranges::any_of(shape->elements, [&](ExprId axis) {
          return !std::holds_alternative<NumberExpr>(syntax_.expressions[axis].value);
        });
    std::optional<ValueId> checkedInput;
    // Literal arrays keep the ranked literal constructor. Runtime arrays have an
    // explicit rank-one construction operation; scalar expressions retain their
    // existing rank-zero behavior and are evaluated only once.
    if (runtimeLiteralShape ||
        (!std::holds_alternative<ArrayExpr>(syntax_.expressions[arguments[0]].value) &&
         !literal_[arguments[0]])) {
      std::optional<ValueType> inputContext;
      if (expected && arguments.size() == 2) {
        inputContext = ValueType{true, {dynamic()}};
        inputContext->array = true;
        inputContext->element = expected->element;
      } else if (expected && expected->shape.empty()) {
        inputContext = ValueType{};
        inputContext->element = expected->element;
      } else if (expected && expected->shape.size() == 1) {
        inputContext = *expected;
        inputContext->array = true;
      }
      checkedInput = expression(frame, arguments[0], inputContext);
      if (!checkedInput)
        return {};
      auto output = node(*checkedInput).type;
      if (output.array)
        return arrayTensor(
            frame, span, *checkedInput,
            arguments.size() == 2 ? std::optional<ExprId>(arguments[1]) : std::nullopt, expected);
    }
    auto data = tensorData(arguments[0], expected);
    if (!data)
      return {};
    if (data->leaves.size() > limits_.maxTensorElements) {
      error("S024", "tensor literal element limit exceeded", span);
      return {};
    }
    const bool contextual = expected && expected->tensor;
    if (!contextual && data->leaves.empty()) {
      error("S026", "empty tensor data needs an explicit Tensor element type", span);
      return {};
    }
    if (arguments.size() == 2) {
      const auto *shape = std::get_if<ArrayExpr>(&syntax_.expressions[arguments[1]].value);
      if (!shape || shape->elements.size() > limits_.maxDepth || data->shape.size() != 1) {
        error("S050",
              "explicit tensor shape requires flat literal data and a bounded constant shape",
              span);
        return {};
      }
      std::vector<std::uint64_t> dimensions;
      for (auto axis : shape->elements) {
        const auto *literal = std::get_if<NumberExpr>(&syntax_.expressions[axis].value);
        if (!literal) {
          error("S050", "tensor shape dimensions must be nonnegative uint64 constants",
                syntax_.expressions[axis].span);
          return {};
        }
        auto text = syntax_.source->slice(literal->literal);
        std::uint64_t value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
          error("S050", "invalid constant tensor shape dimension", literal->literal);
          return {};
        }
        dimensions.push_back(value);
      }
      ValueType shapeType{true, {}};
      // Validate extents/products before multiplying the explicit dimensions. The
      // inferred element width is checked again on the final output type.
      shapeType.element = contextual ? expected->element : NumericType::Int32;
      for (auto dimension : dimensions)
        shapeType.shape.push_back({dimension, 0});
      if (!validSize(shapeType, span))
        return {};
      std::uint64_t stride = 1;
      constexpr auto maximum = std::uint64_t(INT64_MAX);
      for (auto axis = dimensions.rbegin(); axis != dimensions.rend(); ++axis) {
        if (*axis && stride > maximum / (*axis)) {
          error("S002", "explicit tensor shape overflows row-major strides", span);
          return {};
        }
        stride *= *axis;
      }
      std::uint64_t count = 1;
      if (std::ranges::find(dimensions, 0) != dimensions.end())
        count = 0;
      else
        for (auto dimension : dimensions)
          count *= dimension;
      if (count != data->leaves.size()) {
        error("S050", "explicit tensor shape element count does not match flat data", span);
        return {};
      }
      data->shape = std::move(dimensions);
    }
    ValueType output{true, {}};
    if (contextual)
      output.element = expected->element;
    for (auto dimension : data->shape)
      output.shape.push_back({dimension, 0});
    if (contextual && !validSize(output, span))
      return {};
    std::vector<ValueId> elements;
    for (auto leaf : data->leaves) {
      std::optional<ValueType> context;
      if (contextual) {
        context = recordArrayElement(output);
      }
      // Infer unannotated leaves independently, in source order. A previously
      // inferred leaf must not silently supply narrowing/promotion context.
      auto value =
          checkedInput && leaf == arguments[0] ? checkedInput : expression(frame, leaf, context);
      if (!value)
        return {};
      const auto actual = node(*value).type;
      if (actual.tensor || actual.task || actual.fallible || actual.device ||
          actual.builtin != BuiltinValue::Numeric) {
        error("S026", "tensor elements must be completed CPU numeric scalars",
              syntax_.expressions[leaf].span);
        return {};
      }
      if (!contextual && elements.empty())
        output.element = actual.element;
      ValueType scalar;
      scalar.element = output.element;
      if (!conform(frame, *value, scalar, syntax_.expressions[leaf].span))
        return {};
      elements.push_back(*value);
    }
    if (!validSize(output, span))
      return {};
    if (isRecordArray(output) &&
        elements.size() > limits_.maxValues / recordInlineUnits(output_, *output.record)) {
      error("S024", "record array literal exceeds configured expanded storage budget", span);
      return {};
    }
    return add(frame, std::move(output), span, TensorLiteral{std::move(elements)});
  }
  static const llvm::fltSemantics &floatSemantics(NumericType type) {
    return type == NumericType::Float32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  }
  std::unordered_map<ValueId, llvm::APFloat> floatConstants_;
  std::unordered_map<SymbolId, llvm::APFloat> floatBindings_;
  void floatingConstant(ValueId id) {
    const auto &value = node(id);
    if (value.type.tensor || value.type.task || value.type.fallible ||
        value.type.builtin != BuiltinValue::Numeric || isInteger(value.type.element))
      return;
    const auto &op = value.operation;
    std::optional<llvm::APFloat> constant;
    if (auto *n = std::get_if<Float32Literal>(&op))
      constant.emplace(llvm::APFloat::IEEEsingle(), llvm::APInt(32, n->bits));
    else if (auto *n = std::get_if<Float64Literal>(&op))
      constant.emplace(llvm::APFloat::IEEEdouble(), llvm::APInt(64, n->bits));
    else if (auto *read = std::get_if<SymbolRead>(&op)) {
      if (auto found = floatBindings_.find(read->symbol); found != floatBindings_.end())
        constant = found->second;
    } else if (auto *unary = std::get_if<ScalarUnary>(&op)) {
      if (auto found = floatConstants_.find(unary->operand); found != floatConstants_.end()) {
        constant = found->second;
        if (unary->operation == TokenKind::Minus)
          constant->changeSign();
      }
    } else if (auto *binary = std::get_if<ScalarBinary>(&op)) {
      auto a = floatConstants_.find(binary->left), d = floatConstants_.find(binary->right);
      if (a != floatConstants_.end() && d != floatConstants_.end()) {
        constant = a->second;
        constexpr auto rounding = llvm::APFloat::rmNearestTiesToEven;
        if (binary->operation == TokenKind::Plus)
          constant->add(d->second, rounding);
        if (binary->operation == TokenKind::Minus)
          constant->subtract(d->second, rounding);
        if (binary->operation == TokenKind::Star)
          constant->multiply(d->second, rounding);
        if (binary->operation == TokenKind::Slash)
          constant->divide(d->second, rounding);
      }
    }
    if (constant)
      floatConstants_.insert_or_assign(id, *constant);
  }
  std::optional<ValueId> conversion(Frame &frame, SourceSpan span, NumericType destination,
                                    const std::vector<ExprId> &arguments) {
    if (!numericScalarArithmeticAvailable(destination, NumericTarget::Cpu)) {
      error("S003", "conversion destination is not implemented", span);
      return {};
    }
    if (arguments.size() != 1) {
      error("S027", "numeric conversion expects one argument", span);
      return {};
    }
    auto input = expression(frame, arguments[0]);
    if (!input)
      return {};
    const auto source = node(*input).type;
    if (source.builtin != BuiltinValue::Numeric || source.array || source.task || source.fallible ||
        source.device) {
      error("S055", "numeric conversion requires an ordinary CPU numeric value", span);
      return {};
    }
    if (source.tensor) {
      if (!numericTensorConversionAvailable(destination, NumericTarget::Cpu) ||
          !numericTensorConversionAvailable(source.element, NumericTarget::Cpu)) {
        error("S003", "tensor numeric conversion is not supported for these element types", span);
        return {};
      }
      auto result = source;
      result.element = destination;
      if (!validSize(result, span))
        return {};
      return add(frame, result, span, NumericConvert{*input});
    }
    std::optional<__int128_t> integer;
    std::optional<llvm::APFloat> floating;
    bool invalid = false;
    if (isInteger(destination)) {
      const auto bits = describeNumeric(destination)->bits;
      const __int128_t maximum = integerMaximum(destination);
      const __int128_t minimum = integerMinimum(destination);
      if (auto found = intConstants_.find(*input); found != intConstants_.end()) {
        integer = found->second;
        invalid = *integer < minimum || *integer > maximum;
      } else if (auto found = floatConstants_.find(*input); found != floatConstants_.end()) {
        llvm::APSInt result(bits, isUnsigned(destination));
        bool exact = false;
        auto status = found->second.convertToInteger(result, llvm::APFloat::rmTowardZero, &exact);
        invalid = (status & llvm::APFloat::opInvalidOp) != 0;
        if (!invalid)
          integer = isUnsigned(destination) ? __int128_t(result.getZExtValue())
                                            : __int128_t(result.getSExtValue());
      }
    } else {
      if (auto found = intConstants_.find(*input); found != intConstants_.end()) {
        floating.emplace(floatSemantics(destination));
        floating->convertFromAPInt(llvm::APInt(64, static_cast<std::uint64_t>(found->second)),
                                   !isUnsigned(source.element), llvm::APFloat::rmNearestTiesToEven);
      } else if (auto found = floatConstants_.find(*input); found != floatConstants_.end()) {
        floating = found->second;
        bool loses = false;
        const bool finite = floating->isFinite();
        auto status = floating->convert(floatSemantics(destination),
                                        llvm::APFloat::rmNearestTiesToEven, &loses);
        invalid = finite && (status & llvm::APFloat::opOverflow);
      }
    }
    if (invalid) {
      error("S055", "constant numeric conversion is outside the destination range", span);
      return {};
    }
    ValueType result;
    result.element = destination;
    auto id = add(frame, result, span, NumericConvert{*input});
    if (!id)
      return {};
    if (integer)
      intConstants_[*id] = *integer;
    if (floating)
      floatConstants_.insert_or_assign(*id, *floating);
    return id;
  }
  std::optional<ValueId> runtimeInput(Frame &frame, SourceSpan span, const std::string &callee,
                                      const std::vector<ExprId> &arguments) {
    if (options_.placement) {
      error("S027", "experimental runtime inputs support CPU compilation only", span);
      return {};
    }
    const bool argument = callee == "args.int32";
    if (arguments.size() != (argument ? 1 : 2)) {
      error("S027", callee + " has incorrect argument count", span);
      return {};
    }
    ValueType parameter;
    parameter.element = argument ? NumericType::Int64 : NumericType::Int32;
    std::vector<ValueId> operands;
    for (auto syntax : arguments) {
      auto value = expression(frame, syntax, parameter);
      if (!value) return {};
      if ((node(*value).type.builtin != BuiltinValue::Numeric || node(*value).type.element != parameter.element || node(*value).type.tensor || node(*value).type.task || node(*value).type.fallible || node(*value).type.device)) {
        error("S027", callee + " requires exact integer scalar argument types", span);
        return {};
      }
      operands.push_back(*value);
    }
    ValueType result;
    result.element = NumericType::Int32;
    return add(frame, result, span, RuntimeInput{argument ? RuntimeInputKind::ArgumentInt32
                                                        : RuntimeInputKind::RandomInt32,
                                               std::move(operands)});
  }
  std::optional<ValueId> mapInput(Frame &frame, ExprId syntax,
                                 std::optional<ValueType> context, bool generate) {
    if (!generate) return expression(frame, syntax, context);
    ValueType scalar;
    scalar.element = NumericType::Int64;
    if (context && (!context->array || context->builtin != BuiltinValue::Numeric ||
                    context->element != NumericType::Int64)) {
      error("S059", "generate callback requires one int index parameter", syntax_.expressions[syntax].span);
      return {};
    }
    auto value = expression(frame, syntax, scalar);
    if (value && (node(*value).type.builtin != BuiltinValue::Numeric || node(*value).type.element != NumericType::Int64 || node(*value).type.tensor || node(*value).type.task || node(*value).type.fallible || node(*value).type.device)) {
      error("S059", "generate count must be an int scalar", syntax_.expressions[syntax].span);
      return {};
    }
    return value;
  }
  ValueType mapInputType(ValueId value, bool generate) {
    auto result = node(value).type;
    if (generate) { result.tensor = result.array = true; result.shape = {dynamic()}; }
    return result;
  }
  std::optional<ValueId> finishMap(Frame &frame, SourceSpan span, ValueType output, ArrayMap map,
                                   bool parallel) {
    if (parallel) {
      const auto &fn = output_.functions[map.function];
      const auto result = fn.declaredResult.value_or(fn.resultType);
      auto family = combineErrors({concreteError(result.errorRecord), taskFamily()}, span);
      if (!family)
        return {};
      output.fallible = true;
      output.errorRecord = normalizedError(*family);
      map.parallel = true;
    }
    return add(frame, output, span, std::move(map));
  }
  std::optional<ValueId> anonymousMap(Frame &frame, SourceSpan span, ExprId inputSyntax,
                                      SourceSpan lambdaSpan, const LambdaExpr &lambda,
                                      bool parallel = false, bool generate = false) {
    if (!parallel && !lambda.errorTypes.empty()) {
      error("S060",
            "map callbacks must return completed elements; handle errors inside an infallible "
            "callback",
            lambdaSpan);
      return {};
    }
    if (lambda.parameters.size() != 1 || reservedControl(name(lambda.parameters[0].name))) {
      error("S060", "map anonymous functions require one element parameter", lambdaSpan);
      return {};
    }
    const auto mapElement = [parallel](const ValueType &type) {
      return (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Record ||
              type.builtin == BuiltinValue::Bool || type.builtin == BuiltinValue::String ||
              type.builtin == BuiltinValue::Function) &&
             !type.tensor && !type.task && (!type.fallible || parallel) && !type.device;
    };
    std::optional<ValueType> inputContext;
    if (lambda.parameters[0].annotation) {
      auto annotation = type(*lambda.parameters[0].annotation);
      if (!annotation)
        return {};
      if (!mapElement(*annotation)) {
        error("S060", "anonymous map parameter requires a numeric scalar, Bool or record type",
              lambdaSpan);
        return {};
      }
      inputContext = ValueType{true, {dynamic()}};
      inputContext->array = true;
      inputContext->element = annotation->element;
      inputContext->builtin = annotation->builtin;
      inputContext->record = annotation->record;
      inputContext->callable = annotation->callable;
    }
    auto input = mapInput(frame, inputSyntax, inputContext, generate);
    if (!input)
      return {};
    const auto inputType = mapInputType(*input, generate);
    if (!inputType.array ||
        (inputType.builtin != BuiltinValue::Numeric && inputType.builtin != BuiltinValue::Record &&
         inputType.builtin != BuiltinValue::Bool && inputType.builtin != BuiltinValue::String &&
         inputType.builtin != BuiltinValue::Function) ||
        inputType.task || inputType.fallible || inputType.device) {
      error("S060", "anonymous map requires a completed CPU numeric, Bool or record array", span);
      return {};
    }
    std::optional<ValueType> resultContext;
    if (lambda.result) {
      resultContext = type(*lambda.result);
      if (!resultContext)
        return {};
      if (!mapElement(*resultContext)) {
        error("S060", "anonymous map result requires a numeric scalar, Bool or record type",
              lambdaSpan);
        return {};
      }
    }
    if (functionDepth_ >= limits_.maxDepth || frame.depth >= limits_.maxDepth) {
      error("S007", "anonymous function analysis depth limit exceeded", lambdaSpan);
      return {};
    }
    const Depth functionDepth(functionDepth_);
    std::optional<std::size_t> declaredError;
    if (parallel && !lambda.errorTypes.empty()) {
      std::vector<std::size_t> families;
      std::unordered_set<std::size_t> active;
      for (auto channel : lambda.errorTypes) {
        auto family = resolveError(channel, active);
        if (!family)
          return {};
        families.push_back(*family);
      }
      auto family = combineErrors(std::move(families), lambdaSpan);
      if (!family)
        return {};
      declaredError = normalizedError(*family);
      if (resultContext) {
        resultContext->fallible = true;
        resultContext->errorRecord = declaredError;
      }
    }
    const auto id = output_.functions.size();
    if (parallel && !lambda.errorTypes.empty())
      anonymousErrors_.emplace(id, declaredError);
    output_.functions.emplace_back();
    requirements_.resize(output_.functions.size());
    output_.functions[id].name = "__lambda_" + std::to_string(id);
    output_.functions[id].anonymous = true;
    output_.functions[id].declaration = lambdaSpan;
    output_.functions[id].declaredResult = resultContext;
    auto element = recordArrayElement(inputType);
    const auto parameter = output_.symbols.size();
    output_.symbols.push_back(
        {name(lambda.parameters[0].name), element, lambda.parameters[0].name, id, true});
    output_.functions[id].parameters.push_back(parameter);
    Frame nested{id, {}, frame.depth, std::make_shared<TaskChecks>(), {}, {}, {}, 0, false};
    nested.locals.emplace(name(lambda.parameters[0].name), parameter);
    nested.closure = std::make_shared<ClosureScope>(ClosureScope{&frame, {}, {}});
    std::vector<TypedStatement> statements;
    auto result =
        suite(nested, lambda.body, resultContext, statements, {name(lambda.parameters[0].name)});
    if (!result)
      return {};
    const auto resultType = node(*result).type;
    if (!mapElement(resultType)) {
      error("S060", "anonymous map body must produce a completed numeric scalar, Bool or record",
            lambdaSpan);
      return {};
    }
    if (parallel && resultType.fallible)
      nested.tasks->handled.insert(resultType.identity);
    for (auto pending : nested.tasks->pending)
      if (!nested.tasks->handled.contains(pending)) {
        error("S036", "anonymous map must handle each expected error before returning an element",
              lambdaSpan);
        return {};
      }
    output_.functions[id].captureCount = nested.closure->origins.size();
    output_.functions[id].body = std::move(statements);
    output_.functions[id].result = *result;
    output_.functions[id].resultType = resultType;
    if (parallel && !lambda.errorTypes.empty()) {
      output_.functions[id].resultType.fallible = true;
      output_.functions[id].resultType.errorRecord = declaredError;
      output_.functions[id].declaredResult = output_.functions[id].resultType;
    }
    std::vector<ValueId> captures;
    for (auto origin : nested.closure->origins) {
      auto value = add(frame, output_.symbols[origin].type, lambdaSpan, SymbolRead{origin});
      if (!value)
        return {};
      captures.push_back(*value);
    }
    auto output = inputType;
    output.element = resultType.element;
    output.builtin = resultType.builtin;
    output.record = resultType.record;
    output.recordName = resultType.recordName;
    output.callable = resultType.callable;
    if (!validSize(output, span))
      return {};
    return finishMap(frame, span, output, ArrayMap{*input, id, std::move(captures), false, generate}, parallel);
  }
  std::optional<ValueId> arrayMap(Frame &frame, SourceSpan span,
                                  const std::vector<ExprId> &arguments, bool parallel = false, bool generate = false) {
    if (arguments.size() != 2) {
      error("S027", "map expects an array and a one-argument function", span);
      return {};
    }
    if (const auto *lambda = std::get_if<LambdaExpr>(&syntax_.expressions[arguments[1]].value))
      return anonymousMap(frame, span, arguments[0], syntax_.expressions[arguments[1]].span,
                          *lambda, parallel, generate);
    const auto *callback = std::get_if<NameExpr>(&syntax_.expressions[arguments[1]].value);
    if (callback)
      completeScope(frame, callback->name);
    if (!callback || lookupSymbol(frame, callback->name)) {
      auto target = expression(frame, arguments[1]);
      if (!target)
        return {};
      const auto targetType = node(*target).type;
      const auto scalar = [parallel](const ValueType &type) {
        return !type.tensor && !type.task && (!type.fallible || parallel) && !type.device &&
               (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool ||
                type.builtin == BuiltinValue::String || type.builtin == BuiltinValue::Record ||
                type.builtin == BuiltinValue::Function);
      };
      if (targetType.builtin != BuiltinValue::Function || targetType.tensor ||
          targetType.fallible || targetType.task || !targetType.callable ||
          targetType.callable->parameters.size() != 1 ||
          targetType.callable->parameters[0].fallible ||
          !scalar(targetType.callable->parameters[0]) || !scalar(targetType.callable->result)) {
        error("S059", "map requires a one-argument scalar function value", span);
        return {};
      }
      auto inputType = targetType.callable->parameters[0];
      inputType.tensor = inputType.array = true;
      inputType.shape = {dynamic()};
      auto input = mapInput(frame, arguments[0], inputType, generate);
      if (!input)
        return {};
      const auto id = output_.functions.size();
      output_.functions.push_back({});
      output_.functions[id].name = "__function_map_" + std::to_string(id);
      output_.functions[id].anonymous = true;
      output_.functions[id].declaration = span;
      Frame helper{};
      helper.function = id;
      std::vector<ValueId> reads;
      for (const auto &parameter : {targetType.callable->parameters[0], targetType}) {
        const auto symbol = output_.symbols.size();
        output_.symbols.push_back({"argument", parameter, span, id, true});
        output_.functions[id].parameters.push_back(symbol);
        auto read = add(helper, parameter, span, SymbolRead{symbol});
        if (!read)
          return {};
        reads.push_back(*read);
      }
      const auto resultType = targetType.callable->result;
      auto result = add(helper, resultType, span, FunctionInvoke{reads[1], {reads[0]}});
      if (!result)
        return {};
      output_.functions[id].declaredResult = resultType;
      output_.functions[id].resultType = resultType;
      output_.functions[id].result = *result;
      output_.functions[id].body = {{span, {}, *result}};
      auto output = mapInputType(*input, generate);
      output.builtin = resultType.builtin;
      output.element = resultType.element;
      output.record = resultType.record;
      output.recordName = resultType.recordName;
      output.callable = resultType.callable;
      if (!validSize(output, span))
        return {};
      return finishMap(frame, span, output, ArrayMap{*input, id, {*target}, false, generate}, parallel);
    }
    auto found = functions_.find(functionKey(frame, name(callback->name)));
    if (found == functions_.end() || !analyzeFunction(found->second)) {
      error("S059", "map callback must name a declared function", callback->name);
      return {};
    }
    const auto id = found->second;
    const auto &fn = output_.functions[id];
    const auto scalar = [parallel](const ValueType &type) {
      return (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Record ||
              type.builtin == BuiltinValue::Bool || type.builtin == BuiltinValue::String ||
              type.builtin == BuiltinValue::Function) &&
             !type.tensor && !type.task && (!type.fallible || parallel) && !type.device;
    };
    const auto result = fn.declaredResult.value_or(fn.resultType);
    if (fn.parameters.size() != 1 || !scalar(result) ||
        (output_.symbols[fn.parameters[0]].type.fallible ||
         !scalar(output_.symbols[fn.parameters[0]].type))) {
      error("S059",
            "map callback requires one numeric scalar, Bool or record parameter and a numeric "
            "scalar or "
            "record result",
            callback->name);
      return {};
    }
    ValueType context{true, {dynamic()}};
    context.array = true;
    context.element = output_.symbols[fn.parameters[0]].type.element;
    context.builtin = output_.symbols[fn.parameters[0]].type.builtin;
    context.record = output_.symbols[fn.parameters[0]].type.record;
    context.callable = output_.symbols[fn.parameters[0]].type.callable;
    auto input = mapInput(frame, arguments[0], context, generate);
    if (!input)
      return {};
    auto output = mapInputType(*input, generate);
    output.element = result.element;
    output.builtin = result.builtin;
    output.record = result.record;
    output.callable = result.callable;
    if (!validSize(output, span))
      return {};
    return finishMap(frame, span, output, ArrayMap{*input, id, {}, false, generate}, parallel);
  }
  std::optional<ValueId> builtin(Frame &frame, SourceSpan span, const std::string &callee,
                                 const std::vector<ExprId> &arguments,
                                 const std::optional<ValueType> &expected) {
    if (callee == "tensor")
      return tensor(frame, span, arguments, expected);
    const bool transform = callee == "transpose" || callee == "softmaxRows" || callee == "rmsNormRows";
    const auto arity = callee == "matmul" || callee == "rmsNormRows" ? 2u : 1u;
    if (arguments.size() != arity) {
      error("S027", callee + " expects " + std::to_string(arity) + " arguments", span);
      return {};
    }
    auto left = expression(frame, arguments[0]);
    if (!left)
      return {};
    const auto leftType = node(*left).type;
    if (leftType.tensor && leftType.device) {
      error("S047", "host tensor operations require an explicit transfer to devices.cpu()", span);
      return {};
    }
    if (leftType.task || leftType.fallible || leftType.array || !leftType.tensor ||
        ((callee == "matmul" || transform) && leftType.shape.size() != 2)) {
      error("S028", callee + " requires " + (callee == "matmul" || transform ? "rank-two tensors" : "a tensor"),
            syntax_.expressions[arguments[0]].span);
      return {};
    }
    if (transform) {
      if (leftType.builtin != BuiltinValue::Numeric || !describeNumeric(leftType.element)) {
        error("S053", callee + " requires a numeric tensor", span);
        return {};
      }
      if (callee != "transpose" && leftType.element != NumericType::Float32 &&
          leftType.element != NumericType::Float64) {
        error("S053", callee + " requires a floating-point tensor", span);
        return {};
      }
      auto result = leftType;
      const auto kind = callee == "transpose" ? TensorTransformKind::Transpose
                        : callee == "softmaxRows" ? TensorTransformKind::SoftmaxRows
                                                    : TensorTransformKind::RmsNormRows;
      std::optional<ValueId> epsilon;
      if (kind == TensorTransformKind::Transpose)
        std::swap(result.shape[0], result.shape[1]);
      if (kind == TensorTransformKind::RmsNormRows) {
        ValueType scalar;
        scalar.element = leftType.element;
        epsilon = expression(frame, arguments[1], scalar);
        if (!epsilon)
          return {};
        const auto &type = node(*epsilon).type;
        if (type.tensor || type.array || type.task || type.fallible ||
            type.builtin != BuiltinValue::Numeric || type.element != leftType.element) {
          error("S017", "rmsNormRows epsilon must be a matching floating scalar", span);
          return {};
        }
      }
      return add(frame, result, span, TensorTransform{kind, *left, epsilon});
    }
    if (callee == "relu") {
      if (!describeNumeric(leftType.element)) {
        error("S053", "relu requires a numeric tensor", span);
        return {};
      }
      return add(frame, leftType, span, TensorUnary{TokenKind::Relu, *left});
    }
    if (!(callee == "reduceSum" ? numericReductionAvailable(leftType.element, NumericTarget::Cpu)
                                : numericMatmulAvailable(leftType.element, NumericTarget::Cpu))) {
      error("S053", callee + " currently requires implemented numeric tensor elements", span);
      return {};
    }
    if (callee == "reduceSum") {
      ValueType result;
      result.element = leftType.element;
      return add(frame, result, span, ReduceSum{*left});
    }
    auto right = expression(frame, arguments[1]);
    if (!right)
      return {};
    const auto rightType = node(*right).type;
    if (rightType.tensor && rightType.device) {
      error("S047", "host tensor operations require an explicit transfer to devices.cpu()", span);
      return {};
    }
    if (rightType.task || rightType.fallible || rightType.array || !rightType.tensor ||
        rightType.shape.size() != 2) {
      error("S028", "matmul requires rank-two tensors", syntax_.expressions[arguments[1]].span);
      return {};
    }
    const auto &a = leftType.shape[1], &b = rightType.shape[0];
    if (a.constant && b.constant && a.constant != b.constant) {
      error("S029",
            "matmul inner extents differ: " + std::to_string(*a.constant) + " versus " +
                std::to_string(*b.constant),
            span);
      return {};
    }
    if (leftType.element != rightType.element) {
      error("S017", "matmul requires matching numeric element types", span);
      return {};
    }
    ValueType output{true, {leftType.shape[0], rightType.shape[1]}};
    output.element = leftType.element;
    if (!validSize(output, span))
      return {};
    if (!requireEqual(frame, a, b))
      return {};
    const auto sizeCheck = dynamicSize(output);
    return add(frame, std::move(output), span, Matmul{*left, *right, !same(a, b), sizeCheck});
  }
  std::optional<ValueId> placementBuiltin(Frame &frame, SourceSpan span, const std::string &callee,
                                          const std::vector<ExprId> &arguments) {
    if (callee == "devices.cpu" || callee == "devices.gpu") {
      if (frame.locals.contains("devices")) {
        error("S030", "devices is shadowed by a local binding", span);
        return {};
      }
      const bool gpu = callee == "devices.gpu";
      if (arguments.size() > (gpu ? 1u : 0u)) {
        error("S027", "invalid device selector argument count", span);
        return {};
      }
      std::optional<std::uint32_t> device;
      if (gpu) {
        device = 0;
        if (!arguments.empty()) {
          const auto *literal = std::get_if<NumberExpr>(&syntax_.expressions[arguments[0]].value);
          if (!literal) {
            error("S046", "device selector requires a constant uint32 GPU index", span);
            return {};
          }
          const auto text = syntax_.source->slice(literal->literal);
          const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *device);
          if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            error("S046", "invalid uint32 GPU index", literal->literal);
            return {};
          }
        }
      }
      ValueType type;
      type.builtin = BuiltinValue::Device;
      type.device = device;
      return add(frame, type, span, DeviceSelection{device});
    }
    if (frame.groups.empty()) {
      error("S037", "transfer requires an owning parallel group", span);
      return {};
    }
    if (arguments.size() != 2) {
      error("S027", "transfer expects a tensor and device target", span);
      return {};
    }
    if (literal_[arguments[0]] || literal_[arguments[1]]) {
      error("S047", "transfer requires a completed tensor and device selector", span);
      return {};
    }
    auto input = expression(frame, arguments[0]);
    auto target = expression(frame, arguments[1]);
    if (!input || !target)
      return {};
    auto type = node(*input).type;
    const auto selection = node(*target).type;
    if (!type.tensor || type.array || type.task || type.fallible ||
        selection.builtin != BuiltinValue::Device) {
      error("S047", "transfer requires a completed tensor and device selector", span);
      return {};
    }
    type.device = selection.device;
    type.task = true;
    type.identity = 0;
    auto transfer = add(frame, type, span, Transfer{*input, *target});
    if (transfer)
      frame.tasks->children[frame.groups.back()].push_back(*transfer);
    return transfer;
  }
  ValueType callResult(FunctionId id, const std::vector<ValueId> &arguments,
                       bool cpuContract = false) {
    const auto &function = output_.functions[id];
    auto result = cpuContract && function.declaredResult ? *function.declaredResult
                  : states_[id] == State::Complete       ? function.resultType
                                                         : *function.declaredResult;
    std::unordered_map<ExtentId, Extent> substitution;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto &formal = output_.symbols[function.parameters[i]].type;
      const auto &actual = node(arguments[i]).type;
      for (std::size_t axis = 0; axis < formal.shape.size(); ++axis)
        if (!formal.shape[axis].constant)
          substitution.emplace(formal.shape[axis].identity, actual.shape[axis]);
    }
    for (auto &dimension : result.shape) {
      if (dimension.constant)
        continue;
      auto found = substitution.find(dimension.identity);
      if (found == substitution.end())
        found = substitution.emplace(dimension.identity, dynamic()).first;
      dimension = found->second;
    }
    return result;
  }
  std::optional<ValueId>
  invoke(Frame &frame, SourceSpan span, SourceSpan calleeSpan, const std::vector<ExprId> &arguments,
         const std::optional<ValueType> &expected, std::optional<ExprId> selection = {},
         std::optional<ValueId> *checkedSelection = nullptr, bool spawned = false) {
    completeDeviceSelector(frame, calleeSpan);
    completeScope(frame, calleeSpan, true, !spawned && !selection);
    const auto callee = name(calleeSpan);
    if (auto symbol = lookupSymbol(frame, calleeSpan)) {
      if (output_.symbols[*symbol].type.builtin == BuiltinValue::Function) {
        if (selection) {
          error("S074", "indirect spawn requires ordinary CPU execution", span);
          return {};
        }
        auto target = add(frame, output_.symbols[*symbol].type, calleeSpan, SymbolRead{*symbol});
        if (!target)
          return {};
        return callValue(frame, span, *target, arguments);
      }
      error("S030", "local value '" + callee + "' is not callable", calleeSpan);
      return {};
    }
    if (options_.placement &&
        (callee == "devices.cpu" || callee == "devices.gpu" || callee == "transfer")) {
      if (selection) {
        error("S037", "device selectors and transfers are not spawn function entries", span);
        return {};
      }
      return placementBuiltin(frame, span, callee, arguments);
    }
    auto found = functions_.find(functionKey(frame, callee));
    if (found == functions_.end()) {
      if (selection) {
        error("S037", "placed spawn requires a statically named function", span);
        return {};
      }
      if (auto numeric = resolveNumeric(callee))
        return conversion(frame, span, *numeric, arguments);
      if (callee == "array") {
        if (arguments.size() != 1) {
          error("S027", "array expects exactly one tensor argument", span);
          return {};
        }
        if (expected && !expected->array) {
          error("S026", "array construction requires an array result context", span);
          return {};
        }
        auto input = expression(frame, arguments[0]);
        if (!input)
          return {};
        auto output = node(*input).type;
        if (!output.tensor || output.array || output.task || output.fallible || output.device ||
            output.builtin != BuiltinValue::Numeric) {
          error("S026", "array construction requires a completed CPU numeric tensor", span);
          return {};
        }
        Extent count = dynamic();
        if (output.shape.size() == 1)
          count = output.shape[0];
        else if (std::ranges::any_of(output.shape, [](const Extent &d) { return d.constant == 0; }))
          count = {0, 0};
        else if (std::ranges::all_of(output.shape,
                                     [](const Extent &d) { return d.constant.has_value(); })) {
          count = {1, 0};
          for (const auto &d : output.shape) {
            if (*count.constant > std::uint64_t(INT64_MAX) / *d.constant) {
              error("S002", "tensor element count exceeds the indexing range", span);
              return {};
            }
            *count.constant *= *d.constant;
          }
        }
        output.array = true;
        output.shape = {count};
        if (!validSize(output, span))
          return {};
        return add(frame, output, span, TensorToArray{*input});
      }
      if (callee == "reshape") {
        if (arguments.size() != 2) {
          error("S027", "reshape expects a tensor and a fixed-rank shape list", span);
          return {};
        }
        if (expected && (!expected->tensor || expected->array)) {
          error("S026", "reshape requires a tensor result context", span);
          return {};
        }
        auto input = expression(frame, arguments[0]);
        if (!input)
          return {};
        return arrayTensor(frame, span, *input, arguments[1], expected, true);
      }
      if (callee == "concat")
        return arrayConcat(frame, span, arguments, expected);
      if (callee == "slice")
        return collectionSlice(frame, span, arguments, expected);
      if (callee == "length" || callee == "shape" || callee == "byteLength")
        return collectionQuery(frame, span, callee, arguments);
      if (callee == "args.int32" || callee == "random.int32")
        return runtimeInput(frame, span, callee, arguments);
      if (callee == "generate") {
        if (options_.placement) {
          error("S027", "experimental generate supports CPU compilation only", span);
          return {};
        }
        return arrayMap(frame, span, arguments, false, true);
      }
      if (callee == "map" || callee == "parallelMap")
        return arrayMap(frame, span, arguments, callee == "parallelMap");
      if (callee == "tensor" || callee == "matmul" || callee == "reduceSum" || callee == "relu" ||
          callee == "transpose" || callee == "softmaxRows" || callee == "rmsNormRows")
        return builtin(frame, span, callee, arguments, expected);
      error("S031", "unknown function '" + callee + "'", calleeSpan);
      return {};
    }
    const auto id = found->second;
    if (!signatures_[id])
      return {};
    const bool signatureQuery = queryingSignature(span);
    const auto slot = signatureQuery ? options_.signature->argumentIndex : std::nullopt;
    const bool projected = slot && *slot < arguments.size() &&
                           *slot < output_.functions[id].parameters.size() &&
                           arguments.size() <= output_.functions[id].parameters.size();
    if (projected || ((signatureQuery || completingArgument(arguments)) &&
                      arguments.size() < output_.functions[id].parameters.size())) {
      if (!analyzeFunction(id))
        return {};
      if (states_[id] == State::Complete && diagnostics_.empty()) {
        FunctionSignature signature;
        std::vector<std::string> names;
        for (const auto symbol : output_.functions[id].parameters) {
          signature.parameters.push_back(output_.symbols[symbol].type);
          names.push_back(output_.symbols[symbol].name);
        }
        signature.result = output_.functions[id].resultType;
        for (std::size_t i = 0; i < (projected ? *slot : arguments.size()); ++i)
          if (!expression(frame, arguments[i], signature.parameters[i]))
            return {};
        if (signatureQuery)
          captureSignature(frame, signature, std::move(names));
        if (projected) {
          error("S027", "editing argument requires a signature snapshot", span);
          return {}; // Preserve the original later arguments without constructing a value.
        }
      }
    }
    if (arguments.size() != output_.functions[id].parameters.size()) {
      error("S027", "wrong argument count for '" + callee + "'", span);
      return {};
    }
    if (!analyzeFunction(id))
      return {};
    std::vector<ValueId> operands;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto expectedType = output_.symbols[output_.functions[id].parameters[i]].type;
      auto argument = expression(frame, arguments[i], expectedType, options_.placement && spawned);
      if (!argument)
        return {};
      operands.push_back(*argument);
    }
    std::optional<std::uint32_t> device;
    if (selection) {
      if (literal_[*selection]) {
        error("S046", "task placement requires a device selector", span);
        return {};
      }
      auto target = expression(frame, *selection);
      if (!target)
        return {};
      const auto type = node(*target).type;
      if (type.builtin != BuiltinValue::Device || type.task || type.fallible) {
        error("S046", "task placement requires a device selector", span);
        return {};
      }
      device = type.device;
      if (checkedSelection)
        *checkedSelection = *target;
    }
    if (!selection && spawned && options_.placement && checkedSelection) {
      ValueType type;
      type.builtin = BuiltinValue::Device;
      auto target = add(frame, type, span, DeviceSelection{{}});
      if (!target)
        return {};
      *checkedSelection = *target;
    }
    for (auto operand : operands) {
      const auto type = node(operand).type;
      if (type.tensor && type.device != device) {
        error("S047",
              "tensor arguments require an explicit transfer to the selected execution device",
              span);
        return {};
      }
    }
    if (states_[id] == State::Complete) {
      std::unordered_map<ExtentId, Extent> substitution;
      for (std::size_t i = 0; i < operands.size(); ++i) {
        const auto &formal = output_.symbols[output_.functions[id].parameters[i]].type;
        const auto &actual = node(operands[i]).type;
        for (std::size_t axis = 0; axis < formal.shape.size(); ++axis)
          if (!formal.shape[axis].constant)
            substitution.emplace(formal.shape[axis].identity, actual.shape[axis]);
      }
      const auto substitute = [&](Extent extent) {
        if (!extent.constant)
          extent = substitution.at(extent.identity);
        return extent;
      };
      for (const auto &requirement : output_.functions[id].preconditions) {
        const auto left = substitute(requirement.left), right = substitute(requirement.right);
        if (left.constant && right.constant && left.constant != right.constant) {
          error("S033",
                "call to '" + callee + "' violates a known tensor shape requirement: " +
                    std::to_string(*left.constant) + " versus " + std::to_string(*right.constant),
                span);
          return {};
        }
        if (!requireEqual(frame, left, right))
          return {};
      }
    }
    auto resultType = callResult(id, operands, options_.placement && !device);
    if (resultType.tensor)
      resultType.device = device;
    resultType.fallible = syntax_.functions[id].taskError;
    if (resultType.fallible)
      resultType.errorRecord = errorFamilies_[id];
    resultType.identity = 0;
    if (!validSize(resultType, span))
      return {};
    return add(frame, std::move(resultType), span, FunctionCall{id, std::move(operands)});
  }
  Module syntax_;
  SemanticLimits limits_;
  CheckOptions options_;
  TypedModule output_;
  std::map<std::pair<std::size_t, std::size_t>, ResolvedSourceToken> sourceTokens_;
  std::unordered_map<std::string, FunctionId> functions_;
  std::unordered_map<std::string, std::size_t> records_;
  std::unordered_map<std::string, std::size_t> aliases_;
  std::vector<std::size_t> recordDeclarations_;
  std::vector<State> recordStates_;
  bool registeringRecords_ = true, recursiveValues_ = false;
  std::size_t recordDepth_ = 0;
  std::unordered_map<std::size_t, std::optional<std::size_t>> resolvedAliases_;
  std::unordered_map<std::string, std::optional<ExportBinding>> exports_;
  std::unordered_set<std::string> resolvingExports_;
  std::unordered_map<std::string, std::pair<std::size_t, std::string>> reexports_;
  std::vector<State> states_;
  std::vector<bool> signatures_;
  std::vector<std::optional<std::size_t>> errorFamilies_;
  std::vector<bool> literal_, floatingLiteral_;
  std::vector<std::unordered_set<std::string>> requirements_;
  // Keep declared channels available while an anonymous success type is inferred.
  std::unordered_map<FunctionId, std::optional<std::size_t>> anonymousErrors_;
  std::vector<Diagnostic> diagnostics_;
  ExtentId nextExtent_ = 1;
  std::size_t functionDepth_ = 0;
  std::size_t errorCount_ = 0;
  std::size_t shapeWork_ = 0;
  std::size_t nextGroup_ = 1;
};
} // namespace
CheckResult check(ParseResult syntax, SemanticLimits limits, CheckOptions options) {
  if (options.completion) {
    options.completion->names.clear();
    options.completion->reached = options.completion->truncated = false;
  }
  if (options.signature)
    options.signature->snapshot.reset();
  const bool editing = syntax.module.editorOnly;
  if ((!syntax.ok() && !editing) || (editing && !options.completion && !options.signature)) {
    if (syntax.diagnostics.empty())
      syntax.diagnostics.push_back({"E001", "editor recovery syntax cannot be compiled", {}});
    return {syntax.module.source, {}, std::move(syntax.diagnostics)};
  }
  limits.maxDiagnostics = std::max<std::size_t>(1, limits.maxDiagnostics);
  auto result = Checker(std::move(syntax.module), limits, options).run();
  if (options.signature && options.signature->snapshot) {
    const auto site = options.signature->site;
    if (result.ok() || result.diagnostics.empty() ||
        std::ranges::any_of(result.diagnostics, [&](const auto &diagnostic) {
          return diagnostic.code != "S027" || diagnostic.span.begin != site.begin ||
                 diagnostic.span.end != site.end;
        }))
      options.signature->snapshot.reset();
  }
  if (editing) {
    result.module.reset();
    if (result.diagnostics.empty())
      result.diagnostics.push_back({"E001", "editor query does not publish typed IR", {}});
  }
  return result;
}
} // namespace kalandra
