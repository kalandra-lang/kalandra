// Compile-time type provenance. Equal concrete types do not make an abstract T
// interchangeable with an independently named concrete type in a generic body.
struct GenericTerm {
  ValueType actual;
  std::set<std::string> variables;
  std::vector<GenericTerm> arguments;
  std::size_t nodes = 1;
  std::shared_ptr<const GenericTerm> error = {};
  std::shared_ptr<const GenericTerm> failure = {};
};
using GenericTermBindings = std::unordered_map<std::string, GenericTerm>;
struct GenericProofOwner {
  std::vector<GenericParameter> parameters;
  std::optional<std::size_t> selfTrait;
  std::optional<std::size_t> implementation;
};
std::shared_ptr<const GenericProofOwner> genericProofOwner_;
std::shared_ptr<const GenericProofOwner> proofOwnerForImplementation(std::size_t declaration,
                                                                     std::size_t trait) {
  auto owner = std::make_shared<GenericProofOwner>();
  owner->parameters = syntax_.implementations[declaration].generics;
  owner->selfTrait = trait;
  owner->implementation = declaration;
  return owner;
}
std::shared_ptr<const GenericProofOwner> proofOwnerForFunction(FunctionId function) {
  const auto origin = genericOrigins_.contains(function) ? genericOrigins_.at(function) : function;
  auto owner = std::make_shared<GenericProofOwner>();
  owner->parameters = syntax_.functions[origin].generics;
  if (const auto impl = implementationMethodDeclarations_.find(origin);
      impl != implementationMethodDeclarations_.end()) {
    owner->implementation = impl->second;
    owner->selfTrait = implementationMethodOwners_.at(origin);
  }
  return owner;
}
std::shared_ptr<const GenericProofOwner> proofOwnerForRecord(std::size_t record) {
  auto owner = std::make_shared<GenericProofOwner>();
  if (const auto generic = genericRecordInstances_.find(record);
      generic != genericRecordInstances_.end())
    owner->parameters = syntax_.records[generic->second.declaration].generics;
  return owner;
}
std::shared_ptr<const GenericProofOwner> proofOwnerForTrait(std::size_t trait) {
  auto owner = std::make_shared<GenericProofOwner>();
  owner->selfTrait = trait;
  return owner;
}

std::optional<GenericTerm> genericExpectedTerm_;
struct GenericExpectedScope {
  Checker &checker;
  std::optional<GenericTerm> previous;
  GenericExpectedScope(Checker &checker, std::optional<GenericTerm> expected)
      : checker(checker), previous(std::move(checker.genericExpectedTerm_)) {
    checker.genericExpectedTerm_ = std::move(expected);
  }
  ~GenericExpectedScope() { checker.genericExpectedTerm_ = std::move(previous); }
};
// Error provenance follows the checker's existing propagation boundaries.
// Scope keys live for the duration of their catch/group check and are erased
// before the owning shared state can be reused.
std::unordered_map<const void *, GenericTerm> genericErrorScopes_;
struct GenericErrorScope {
  Checker &checker;
  const void *key;
  GenericErrorScope(Checker &checker, const void *key) : checker(checker), key(key) {}
  ~GenericErrorScope() { checker.genericErrorScopes_.erase(key); }
};
std::unordered_map<FunctionId, FunctionId> genericHelperOwners_;
std::unordered_map<ValueId, GenericTerm> genericValueTerms_;
std::unordered_map<SymbolId, GenericTerm> genericSymbolTerms_;
std::unordered_map<FunctionId, GenericTerm> genericReturnTerms_;
std::size_t genericTermWork_ = 0;
bool genericTermWork(std::size_t work = 1) {
  if (work > limits_.maxValues || genericTermWork_ > limits_.maxValues - work) {
    if (genericTermWork_ != std::numeric_limits<std::size_t>::max())
      error("S082", "generic type provenance exceeds the configured work budget", {});
    genericTermWork_ = std::numeric_limits<std::size_t>::max();
    return false;
  }
  genericTermWork_ += work;
  return true;
}
GenericTerm finishGenericTerm(GenericTerm term) {
  term.nodes = 1;
  if (term.error)
    term.nodes += term.error->nodes;
  if (term.failure)
    term.nodes += term.failure->nodes;
  if (term.nodes > limits_.maxValues) {
    genericTermWork(limits_.maxValues + 1);
    term.variables.insert("$budget");
    term.nodes = limits_.maxValues;
    return term;
  }
  for (const auto &argument : term.arguments) {
    if (argument.nodes > limits_.maxValues || term.nodes > limits_.maxValues - argument.nodes) {
      genericTermWork(limits_.maxValues + 1);
      term.variables.insert("$budget");
      term.nodes = limits_.maxValues;
      return term;
    }
    term.nodes += argument.nodes;
  }
  return term;
}

FunctionId genericRoot(const Frame &frame) const {
  const Frame *root = &frame;
  while (root->closure)
    root = root->closure->parent;
  if (const auto helper = genericHelperOwners_.find(root->function);
      helper != genericHelperOwners_.end())
    return helper->second;
  return root->function;
}
bool genericContext(const Frame &frame) const {
  return genericOrigins_.contains(genericRoot(frame));
}
GenericTerm concreteGenericTerm(ValueType actual, std::size_t depth = 0) {
  GenericTerm term{actual, {}, {}};
  if (depth >= limits_.maxDepth || !genericTermWork()) {
    if (depth >= limits_.maxDepth)
      error("S082", "generic type provenance exceeds the configured depth", {});
    term.variables.insert("$budget");
    return term;
  }
  if (actual.array) {
    term.arguments.push_back(concreteGenericTerm(recordArrayElement(actual), depth + 1));
  } else if (actual.tensor) {
    auto element = actual;
    element.tensor = false;
    element.shape.clear();
    term.arguments.push_back(concreteGenericTerm(element, depth + 1));
  } else if (actual.record) {
    if (const auto found = genericRecordInstances_.find(*actual.record);
        found != genericRecordInstances_.end())
      for (const auto &argument : found->second.arguments)
        term.arguments.push_back(concreteGenericTerm(argument, depth + 1));
  } else if (actual.callable) {
    for (const auto &argument : actual.callable->parameters)
      term.arguments.push_back(concreteGenericTerm(argument, depth + 1));
    term.arguments.push_back(concreteGenericTerm(actual.callable->result, depth + 1));
    if (actual.callable->result.fallible) {
      ValueType family;
      if (actual.callable->result.errorRecord)
        family = recordType(*actual.callable->result.errorRecord);
      else
        family.builtin = BuiltinValue::TaskError;
      term.error = std::make_shared<const GenericTerm>(concreteGenericTerm(family, depth + 1));
    }
  }
  if (actual.fallible) {
    ValueType family;
    if (actual.errorRecord)
      family = recordType(*actual.errorRecord);
    else
      family.builtin = BuiltinValue::TaskError;
    term.failure = std::make_shared<const GenericTerm>(concreteGenericTerm(family, depth + 1));
  }
  return finishGenericTerm(std::move(term));
}
GenericTerm genericTerm(ValueId value) {
  if (const auto found = genericValueTerms_.find(value); found != genericValueTerms_.end()) {
    if (!genericTermWork(found->second.nodes))
      return GenericTerm{node(value).type, {"$budget"}, {}};
    return found->second;
  }
  return concreteGenericTerm(node(value).type);
}
bool dependentGenericTerm(const GenericTerm &term) const {
  return !term.variables.empty() || (term.error && dependentGenericTerm(*term.error)) ||
         (term.failure && dependentGenericTerm(*term.failure)) ||
         std::ranges::any_of(term.arguments,
                             [&](const auto &argument) { return dependentGenericTerm(argument); });
}
GenericTerm mergeGenericTerms(GenericTerm a, const GenericTerm &b) {
  a.variables.insert(b.variables.begin(), b.variables.end());
  if (a.error && b.error)
    a.error = std::make_shared<const GenericTerm>(mergeGenericTerms(*a.error, *b.error));
  else if (b.error)
    a.error = b.error;
  if (a.failure && b.failure)
    a.failure = std::make_shared<const GenericTerm>(mergeGenericTerms(*a.failure, *b.failure));
  else if (b.failure)
    a.failure = b.failure;
  if (a.arguments.size() == b.arguments.size())
    for (std::size_t i = 0; i < a.arguments.size(); ++i)
      a.arguments[i] = mergeGenericTerms(std::move(a.arguments[i]), b.arguments[i]);
  else if (dependentGenericTerm(b))
    for (const auto &argument : b.arguments)
      a.variables.insert(argument.variables.begin(), argument.variables.end());
  return finishGenericTerm(std::move(a));
}
GenericTerm abstractGenericTerm(GenericTerm term, const std::string &variable,
                                std::size_t depth = 0) {
  if (depth >= limits_.maxDepth) {
    error("S082", "generic type provenance exceeds the configured depth", {});
    term.variables.insert("$budget");
    return term;
  }
  term.variables = {variable};
  for (std::size_t i = 0; i < term.arguments.size(); ++i) {
    const auto suffix = term.actual.array || term.actual.tensor                  ? ".$element"
                        : term.actual.callable && i + 1 == term.arguments.size() ? ".$result"
                        : term.actual.callable ? ".$parameter" + std::to_string(i)
                                               : ".$argument" + std::to_string(i);
    term.arguments[i] =
        abstractGenericTerm(std::move(term.arguments[i]), variable + suffix, depth + 1);
  }
  if (term.error)
    term.error = std::make_shared<const GenericTerm>(
        abstractGenericTerm(*term.error, variable + ".$error", depth + 1));
  if (term.failure)
    term.failure = std::make_shared<const GenericTerm>(
        abstractGenericTerm(*term.failure, variable + ".$failure", depth + 1));
  return finishGenericTerm(std::move(term));
}
GenericTermBindings genericOwnerEnvironment(const GenericProofOwner &owner,
                                            const GenericBindings &bindings) {
  GenericTermBindings environment;
  for (const auto &[label, actual] : bindings) {
    environment.emplace(label, abstractGenericTerm(concreteGenericTerm(actual), label));
  }
  if (owner.implementation) {
    const auto &declaration = syntax_.implementations[*owner.implementation];
    if (auto self = environment.find("Self"); self != environment.end()) {
      auto actual = self->second.actual;
      environment["Self"] = instantiateGenericTerm(declaration.target, actual, environment);
    }
    for (std::size_t pass = 0; pass <= declaration.associatedTypes.size(); ++pass)
      for (const auto &[label, pattern] : declaration.associatedTypes) {
        const auto key = "Self." + name(label);
        if (auto previous = environment.find(key); previous != environment.end()) {
          auto actual = previous->second.actual;
          environment[key] = instantiateGenericTerm(pattern, actual, environment);
        }
      }
  }
  return environment;
}
GenericTermBindings genericTermEnvironment(FunctionId function) {
  if (const auto bindings = functionBindings_.find(function); bindings != functionBindings_.end())
    return genericOwnerEnvironment(*proofOwnerForFunction(function), *bindings->second);
  return {};
}
GenericTerm instantiateGenericTerm(const TypeSyntax &pattern, const ValueType &actual,
                                   const GenericTermBindings &environment, std::size_t depth = 0) {
  auto term = concreteGenericTerm(actual);
  if (depth >= limits_.maxDepth) {
    error("S082", "generic type provenance exceeds the configured depth", pattern.span);
    term.variables.insert("$budget");
    return finishGenericTerm(std::move(term));
  }
  if (pattern.array && actual.array) {
    auto element = pattern;
    element.array = false;
    term.arguments = {
        instantiateGenericTerm(element, recordArrayElement(actual), environment, depth + 1)};
    return finishGenericTerm(std::move(term));
  }
  if (auto variable = environment.find(associatedName(pattern)); variable != environment.end()) {
    term = variable->second;
    term.actual = actual;
    return finishGenericTerm(std::move(term));
  }
  if (!pattern.members.empty())
    if (auto variable = environment.find(name(pattern.name)); variable != environment.end()) {
      for (const auto &label : variable->second.variables) {
        auto projected = label;
        for (auto member : pattern.members)
          projected += "." + name(member);
        term.variables.insert(std::move(projected));
      }
      return finishGenericTerm(std::move(term));
    }
  if (pattern.tensor && pattern.element && !term.arguments.empty()) {
    TypeSyntax element;
    element.name = element.span = *pattern.element;
    term.arguments[0] =
        instantiateGenericTerm(element, term.arguments[0].actual, environment, depth + 1);
  }
  if (!pattern.arguments.empty() && term.arguments.size() == pattern.arguments.size())
    for (std::size_t i = 0; i < pattern.arguments.size(); ++i)
      term.arguments[i] = instantiateGenericTerm(pattern.arguments[i], term.arguments[i].actual,
                                                 environment, depth + 1);
  if (pattern.function && actual.callable &&
      term.arguments.size() == pattern.function->parameters.size() + 1) {
    for (std::size_t i = 0; i < pattern.function->parameters.size(); ++i)
      term.arguments[i] = instantiateGenericTerm(pattern.function->parameters[i],
                                                 term.arguments[i].actual, environment, depth + 1);
    term.arguments.back() = instantiateGenericTerm(
        pattern.function->result, term.arguments.back().actual, environment, depth + 1);
    if (term.error)
      term.error = std::make_shared<const GenericTerm>(
          genericErrorChannelsTerm(pattern.function->errorTypes, term.error->actual, environment));
  }
  return finishGenericTerm(std::move(term));
}
GenericTerm sourceGenericTerm(const Frame &frame, const TypeSyntax &pattern,
                              const ValueType &actual) {
  return instantiateGenericTerm(pattern, actual, genericTermEnvironment(genericRoot(frame)));
}
void inferGenericTermBindings(const TypeSyntax &pattern, const GenericTerm &actual,
                              const std::vector<GenericParameter> &parameters,
                              GenericTermBindings &bindings, std::size_t depth = 0) {
  if (depth >= limits_.maxDepth) {
    error("S082", "generic type provenance exceeds the configured depth", pattern.span);
    return;
  }
  if (pattern.array) {
    if (!actual.arguments.empty()) {
      auto element = pattern;
      element.array = false;
      inferGenericTermBindings(element, actual.arguments[0], parameters, bindings, depth + 1);
    }
    return;
  }
  if (genericParameterNamed(parameters, name(pattern.name)) && pattern.members.empty()) {
    const auto [previous, inserted] = bindings.emplace(name(pattern.name), actual);
    if (!inserted)
      previous->second = mergeGenericTerms(previous->second, actual);
    return;
  }
  if (pattern.tensor && pattern.element && !actual.arguments.empty()) {
    TypeSyntax element;
    element.name = element.span = *pattern.element;
    inferGenericTermBindings(element, actual.arguments[0], parameters, bindings, depth + 1);
  }
  if (pattern.arguments.size() == actual.arguments.size())
    for (std::size_t i = 0; i < pattern.arguments.size(); ++i)
      inferGenericTermBindings(pattern.arguments[i], actual.arguments[i], parameters, bindings,
                               depth + 1);
  if (pattern.function && actual.arguments.size() == pattern.function->parameters.size() + 1) {
    for (std::size_t i = 0; i < pattern.function->parameters.size(); ++i)
      inferGenericTermBindings(pattern.function->parameters[i], actual.arguments[i], parameters,
                               bindings, depth + 1);
    inferGenericTermBindings(pattern.function->result, actual.arguments.back(), parameters,
                             bindings, depth + 1);
    if (actual.error && pattern.function->errorTypes.size() == 1) {
      const auto label = qualifiedErrorName(pattern.function->errorTypes[0]);
      if (genericParameterNamed(parameters, label)) {
        auto [previous, inserted] = bindings.emplace(label, *actual.error);
        if (!inserted)
          previous->second = mergeGenericTerms(previous->second, *actual.error);
      }
    }
  }
}
GenericTerm recordFieldGenericTerm(const GenericTerm &input, std::size_t field,
                                   const ValueType &actual) {
  auto term = concreteGenericTerm(actual);
  if (!input.variables.empty()) {
    for (const auto &variable : input.variables)
      term.variables.insert(variable + ".$field");
    return term;
  }
  if (!input.actual.record)
    return term;
  const auto record = *input.actual.record;
  const auto instance = genericRecordInstances_.find(record);
  if (instance == genericRecordInstances_.end())
    return term; // Fields of an explicitly concrete nominal type have concrete types.
  const auto declaration = syntax_.records[instance->second.declaration];
  GenericTermBindings parameters;
  for (std::size_t i = 0; i < declaration.generics.size() && i < input.arguments.size(); ++i)
    parameters[name(declaration.generics[i].name)] = input.arguments[i];
  const auto site = output_.records[record].fields[field].declaration;
  const auto project = [&](const std::vector<Parameter> &fields) -> std::optional<GenericTerm> {
    for (const auto &f : fields)
      if (f.name.begin == site.begin && f.name.end == site.end)
        return instantiateGenericTerm(f.type, actual, parameters);
    return {};
  };
  if (auto projected = project(declaration.fields))
    return *projected;
  for (const auto &variant : declaration.variants)
    if (auto projected = project(variant.fields))
      return *projected;
  return term;
}
void recordGenericParameter(FunctionId function, SymbolId symbol, const TypeSyntax &annotation) {
  if (!genericOrigins_.contains(function))
    return;
  genericSymbolTerms_[symbol] = instantiateGenericTerm(annotation, output_.symbols[symbol].type,
                                                       genericTermEnvironment(function));
}
void recordGenericBinding(const Frame &frame, SymbolId symbol, ValueId value,
                          const std::optional<TypeSyntax> &annotation = {}) {
  if (!genericContext(frame))
    return;
  auto term = annotation ? sourceGenericTerm(frame, *annotation, output_.symbols[symbol].type)
                         : genericTerm(value);
  if (auto previous = genericSymbolTerms_.find(symbol); previous != genericSymbolTerms_.end())
    term = mergeGenericTerms(std::move(term), previous->second);
  genericSymbolTerms_[symbol] = finishGenericTerm(std::move(term));
}
void recordGenericValue(const Frame &frame, ValueId id) {
  if (!genericContext(frame))
    return;
  const auto &expression = node(id);
  auto term = concreteGenericTerm(expression.type);
  std::visit(
      [&](const auto &operation) {
        using Op = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Op, SymbolRead>) {
          if (auto symbol = genericSymbolTerms_.find(operation.symbol);
              symbol != genericSymbolTerms_.end())
            term = symbol->second;
        } else if constexpr (std::is_same_v<Op, IteratorOperation>) {
          if (!operation.arguments.empty()) {
            const auto source = genericTerm(operation.arguments[0]);
            if (!source.arguments.empty() && !term.arguments.empty() &&
                (operation.action == IteratorAction::Next ||
                 operation.action == IteratorAction::FromArray))
              term.arguments[0] = source.arguments[0];
            if (operation.action == IteratorAction::Next && source.arguments.size() == 2 &&
                term.failure) {
              auto failure = source.arguments[1];
              failure.actual = term.failure->actual;
              term.failure = std::make_shared<const GenericTerm>(std::move(failure));
            }
          }
        } else if constexpr (std::is_same_v<Op, GeneratorCreate>) {
          const auto &function = output_.functions[operation.function];
          if (function.generator && !term.arguments.empty())
            for (auto value : function.expressions)
              if (const auto *yield = std::get_if<YieldOperation>(&node(value).operation);
                  yield && yield->collection == function.generator->collection)
                term.arguments[0] =
                    mergeGenericTerms(std::move(term.arguments[0]), genericTerm(yield->operand));
        } else if constexpr (std::is_same_v<Op, RecordFieldRead>) {
          term = recordFieldGenericTerm(genericTerm(operation.input), operation.field,
                                        expression.type);
        } else if constexpr (std::is_same_v<Op, ArrayIndex> || std::is_same_v<Op, TensorIndex>) {
          const auto input = genericTerm(operation.input);
          if (!input.variables.empty())
            for (const auto &variable : input.variables)
              term.variables.insert(variable + ".$element");
          else if (!input.arguments.empty())
            term = input.arguments[0];
        } else if constexpr (std::is_same_v<Op, ShapeCheck> || std::is_same_v<Op, ArraySort> ||
                             std::is_same_v<Op, ArraySlice> || std::is_same_v<Op, Transfer> ||
                             std::is_same_v<Op, TensorReshape> ||
                             std::is_same_v<Op, ArrayToTensor> ||
                             std::is_same_v<Op, TensorToArray>) {
          term = genericTerm(operation.input);
        } else if constexpr (std::is_same_v<Op, ScalarUnary> || std::is_same_v<Op, TensorUnary> ||
                             std::is_same_v<Op, TaskOperation> ||
                             std::is_same_v<Op, YieldOperation>) {
          term = genericTerm(operation.operand);
        } else if constexpr (std::is_same_v<Op, ScalarBinary> || std::is_same_v<Op, TensorBinary> ||
                             std::is_same_v<Op, ArrayConcat>) {
          term = mergeGenericTerms(genericTerm(operation.left), genericTerm(operation.right));
        } else if constexpr (std::is_same_v<Op, TensorLiteral>) {
          if (!term.arguments.empty())
            for (auto element : operation.elements)
              term.arguments[0] =
                  mergeGenericTerms(std::move(term.arguments[0]), genericTerm(element));
        } else if constexpr (std::is_same_v<Op, ConditionalOperation>) {
          term = mergeGenericTerms(genericTerm(operation.trueResult),
                                   genericTerm(operation.falseResult));
        } else if constexpr (std::is_same_v<Op, MatchOperation>) {
          for (const auto &branch : operation.cases)
            term = mergeGenericTerms(std::move(term), genericTerm(branch.result));
        } else if constexpr (std::is_same_v<Op, ParallelOperation>) {
          term = genericTerm(operation.result);
        } else if constexpr (std::is_same_v<Op, CatchOperation>) {
          term = mergeGenericTerms(genericTerm(operation.operand), genericTerm(operation.result));
          term.failure.reset(); // Operand failures were handled by this catch.
        } else if constexpr (std::is_same_v<Op, LoopTransferOperation>) {
          if (operation.value)
            term = genericTerm(*operation.value);
        } else if constexpr (std::is_same_v<Op, WhileOperation>) {
          if (operation.normalResult)
            term = genericTerm(*operation.normalResult);
          for (auto exit : operation.exitValues)
            if (operation.collecting && !term.arguments.empty())
              term.arguments[0] =
                  mergeGenericTerms(std::move(term.arguments[0]), genericTerm(exit));
            else
              term = mergeGenericTerms(std::move(term), genericTerm(exit));
        } else if constexpr (std::is_same_v<Op, FunctionReference>) {
          const auto &function = output_.functions[operation.function];
          const auto explicitParameters = function.parameters.size() - function.captureCount;
          if (term.arguments.size() == explicitParameters + 1) {
            for (std::size_t i = 0; i < explicitParameters; ++i)
              if (const auto parameter = genericSymbolTerms_.find(function.parameters[i]);
                  parameter != genericSymbolTerms_.end())
                term.arguments[i] = parameter->second;
            term.arguments.back() = genericTerm(function.result);
          }
        } else if constexpr (std::is_same_v<Op, FunctionInvoke>) {
          const auto function = genericTerm(operation.callee);
          if (!function.variables.empty()) {
            for (const auto &variable : function.variables)
              term.variables.insert(variable + ".$result");
            if (term.failure) {
              auto error = *term.failure;
              for (const auto &variable : function.variables)
                error.variables.insert(variable + ".$error");
              term.failure =
                  std::make_shared<const GenericTerm>(finishGenericTerm(std::move(error)));
            }
          } else if (!function.arguments.empty()) {
            term = function.arguments.back();
            if (expression.type.fallible)
              term.failure = function.error;
          }
        } else if constexpr (std::is_same_v<Op, ArrayMap>) {
          if (!term.arguments.empty())
            term.arguments[0] = genericTerm(output_.functions[operation.function].result);
        }
      },
      expression.operation);
  if (const auto *task = std::get_if<TaskOperation>(&expression.operation);
      task && task->prefix == TaskPrefix::Await && term.failure) {
    ValueType taskError;
    taskError.builtin = BuiltinValue::TaskError;
    auto failure = genericErrorUnionTerm(*term.failure, concreteGenericTerm(taskError));
    if (auto actual = concreteGenericTerm(expression.type).failure)
      failure.actual = actual->actual;
    term.failure = std::make_shared<const GenericTerm>(finishGenericTerm(std::move(failure)));
  }
  term.actual = expression.type;
  if (!expression.type.fallible && !expression.type.task)
    term.failure.reset();
  if (dependentGenericTerm(term))
    genericValueTerms_[id] = finishGenericTerm(std::move(term));
}
std::set<std::size_t> declaredGenericTraits(const GenericProofOwner &owner,
                                            const std::string &variable) {
  std::set<std::size_t> traits;
  const auto dot = variable.find('.');
  const auto base = variable.substr(0, dot);
  for (const auto &parameter : owner.parameters)
    if (genericParameterLabel(parameter) == base)
      for (auto bound : parameter.bounds) {
        const auto found =
            traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
        if (found != traitNames_.end())
          traits.insert(found->second);
      }
  if (base == "Self" && owner.selfTrait)
    traits.insert(*owner.selfTrait);
  auto position = dot;
  while (position != std::string::npos) {
    const auto next = variable.find('.', position + 1);
    const auto member = variable.substr(
        position + 1, next == std::string::npos ? std::string::npos : next - position - 1);
    std::set<std::size_t> associated;
    for (auto trait : traits) {
      const auto &declaration = syntax_.traits[trait];
      for (std::size_t i = 0; i < declaration.associatedTypes.size(); ++i)
        if (name(declaration.associatedTypes[i]) == member &&
            i < declaration.associatedBounds.size())
          for (auto bound : declaration.associatedBounds[i]) {
            const auto found =
                traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
            if (found != traitNames_.end())
              associated.insert(found->second);
          }
    }
    traits = std::move(associated);
    position = next;
  }
  return traits;
}
bool requireGenericTraitProof(const GenericProofOwner &owner, std::size_t trait,
                              const GenericTerm &term, SourceSpan site, std::size_t depth = 0) {
  if (!dependentGenericTerm(term))
    return true;
  if (depth >= limits_.maxDepth) {
    error("S083", "generic trait proof exceeds the configured depth", site);
    return false;
  }
  for (const auto &variable : term.variables)
    if (!declaredGenericTraits(owner, variable).contains(trait)) {
      const auto required = name(syntax_.traits[trait].name);
      std::string message;
      if (variable.find(".$errorUnion") != std::string::npos)
        message = "a bound on an error member does not establish '" + required +
                  "' for the combined error channel; constrain the complete error type";
      else if (variable.find(".$") != std::string::npos)
        message = "cannot establish '" + required + "' for a type derived from generic '" +
                  variable.substr(0, variable.find(".$")) +
                  "'; name and constrain that type in the generic signature";
      else
        message =
            "generic type '" + variable + "' requires a declared '" + required + "' trait bound";
      error("S083", std::move(message), site);
      return false;
    }
  if (!term.variables.empty())
    return true;
  auto conformance = requireConformance(trait, term.actual, site);
  if (!conformance || !conformance->implementation)
    return false;
  const auto &implementation = syntax_.implementations[*conformance->implementation];
  GenericTermBindings bindings;
  inferGenericTermBindings(implementation.target, term, implementation.generics, bindings);
  for (const auto &parameter : implementation.generics)
    for (auto bound : parameter.bounds) {
      const auto needed =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      const auto value = bindings.find(name(parameter.name));
      if (needed == traitNames_.end() || value == bindings.end() ||
          !requireGenericTraitProof(owner, needed->second, value->second, site, depth + 1))
        return false;
    }
  return true;
}
bool requireGenericTraitProof(const Frame &frame, std::size_t trait, const GenericTerm &term,
                              SourceSpan site) {
  if (!genericContext(frame))
    return true;
  return requireGenericTraitProof(*proofOwnerForFunction(genericRoot(frame)), trait, term, site);
}
bool checkGenericTermBounds(const Frame &frame, const std::vector<GenericParameter> &parameters,
                            const GenericTermBindings &bindings, SourceSpan site) {
  for (const auto &parameter : parameters)
    for (auto bound : parameter.bounds) {
      const auto trait =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      const auto term = bindings.find(genericParameterLabel(parameter));
      if (trait == traitNames_.end() || term == bindings.end() ||
          !requireGenericTraitProof(frame, trait->second, term->second, site))
        return false;
    }
  return true;
}

GenericTerm substituteGenericTerm(const GenericTerm &source, const GenericTermBindings &bindings,
                                  std::size_t depth = 0) {
  auto term = concreteGenericTerm(source.actual);
  if (depth >= limits_.maxDepth) {
    error("S082", "generic type provenance exceeds the configured depth", {});
    term.variables.insert("$budget");
    return term;
  }
  for (const auto &variable : source.variables) {
    const auto found = bindings.find(variable);
    if (found != bindings.end())
      term = mergeGenericTerms(std::move(term), found->second);
    else {
      const auto dot = variable.find('.');
      const auto root = bindings.find(variable.substr(0, dot));
      if (root != bindings.end() && dot != std::string::npos)
        for (const auto &replacement : root->second.variables)
          term.variables.insert(replacement + variable.substr(dot));
    }
  }
  if (source.arguments.size() == term.arguments.size())
    for (std::size_t i = 0; i < term.arguments.size(); ++i)
      term.arguments[i] = substituteGenericTerm(source.arguments[i], bindings, depth + 1);
  if (source.error)
    term.error = std::make_shared<const GenericTerm>(
        substituteGenericTerm(*source.error, bindings, depth + 1));
  if (source.failure)
    term.failure = std::make_shared<const GenericTerm>(
        substituteGenericTerm(*source.failure, bindings, depth + 1));
  return finishGenericTerm(std::move(term));
}

GenericTerm genericErrorTerm(SourceSpan channel, const ValueType &actual,
                             const GenericTermBindings &bindings) {
  const auto label = qualifiedErrorName(channel);
  if (auto direct = bindings.find(label); direct != bindings.end())
    return direct->second;
  auto result = concreteGenericTerm(actual);
  const auto dot = label.find('.');
  if (dot != std::string::npos)
    if (auto root = bindings.find(label.substr(0, dot)); root != bindings.end())
      for (const auto &variable : root->second.variables)
        result.variables.insert(variable + label.substr(dot));
  return result;
}

// A bound on E does not imply a bound on the constructed union E | Other.
// Preserve this distinction even when a concrete specialization collapses both
// alternatives to the same error family.
GenericTerm genericErrorUnionTerm(GenericTerm left, const GenericTerm &right) {
  if (left.variables == right.variables &&
      genericTypeKey(left.actual) == genericTypeKey(right.actual))
    return mergeGenericTerms(std::move(left), right);
  auto result = mergeGenericTerms(std::move(left), right);
  std::set<std::string> variables;
  for (const auto &variable : result.variables)
    variables.insert(variable.ends_with(".$errorUnion") ? variable : variable + ".$errorUnion");
  result.variables = std::move(variables);
  return finishGenericTerm(std::move(result));
}
GenericTerm genericErrorChannelsTerm(const std::vector<SourceSpan> &channels,
                                     const ValueType &actual, const GenericTermBindings &bindings) {
  if (channels.empty())
    return concreteGenericTerm(actual);
  auto term = genericErrorTerm(channels[0], actual, bindings);
  for (std::size_t i = 1; i < channels.size(); ++i)
    term = genericErrorUnionTerm(std::move(term), genericErrorTerm(channels[i], actual, bindings));
  term.actual = actual;
  return finishGenericTerm(std::move(term));
}

void recordGenericErrorPropagation(const Frame &frame, const void *scope, ValueId origin,
                                   bool directError) {
  if (!genericContext(frame))
    return;
  auto value = genericTerm(origin);
  auto term = directError ? value : value.failure ? *value.failure : value;
  if (auto found = genericErrorScopes_.find(scope); found != genericErrorScopes_.end())
    found->second = genericErrorUnionTerm(std::move(found->second), term);
  else
    genericErrorScopes_.emplace(scope, std::move(term));
}
void finishGenericErrorScope(const Frame &frame, ValueId value, const void *scope,
                             std::optional<ValueId> handlerResult = {}) {
  if (!genericContext(frame))
    return;
  auto term = genericTerm(value);
  term.failure.reset();
  if (node(value).type.fallible) {
    auto failure = concreteGenericTerm(node(value).type).failure;
    if (auto found = genericErrorScopes_.find(scope); found != genericErrorScopes_.end()) {
      auto propagated = found->second;
      if (handlerResult)
        if (auto handler = genericTerm(*handlerResult).failure)
          propagated = genericErrorUnionTerm(std::move(propagated), *handler);
      propagated.actual = failure->actual;
      failure = std::make_shared<const GenericTerm>(finishGenericTerm(std::move(propagated)));
    } else if (handlerResult) {
      if (auto handler = genericTerm(*handlerResult).failure)
        failure = handler;
    }
    term.failure = std::move(failure);
  }
  genericValueTerms_[value] = finishGenericTerm(std::move(term));
}

bool checkGenericTypeApplication(const std::vector<GenericParameter> &parameters,
                                 const GenericBindings &bindings,
                                 const std::vector<TypeSyntax> &arguments, SourceSpan site) {
  if (!genericProofOwner_ || !genericBindings_)
    return true;
  const auto environment = genericOwnerEnvironment(*genericProofOwner_, *genericBindings_);
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    const auto term = instantiateGenericTerm(
        arguments[i], bindings.at(genericParameterLabel(parameters[i])), environment);
    for (auto bound : parameters[i].bounds) {
      const auto needed =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (needed == traitNames_.end() ||
          !requireGenericTraitProof(*genericProofOwner_, needed->second, term, site))
        return false;
    }
  }
  return true;
}
