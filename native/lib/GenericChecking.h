// Private members of Checker. Specialization leaves ordinary, concrete typed IR.
using GenericBindings = std::unordered_map<std::string, ValueType>;
struct GenericProofOwner;
struct GenericScope {
  Checker &checker;
  std::shared_ptr<const GenericBindings> previous;
  std::shared_ptr<const GenericProofOwner> previousOwner;
  GenericScope(Checker &checker, std::shared_ptr<const GenericBindings> bindings,
               std::shared_ptr<const GenericProofOwner> owner = {})
      : checker(checker), previous(checker.genericBindings_),
        previousOwner(checker.genericProofOwner_) {
    checker.genericBindings_ = std::move(bindings);
    checker.genericProofOwner_ = std::move(owner);
  }
  ~GenericScope() {
    checker.genericBindings_ = std::move(previous);
    checker.genericProofOwner_ = std::move(previousOwner);
  }
};
std::shared_ptr<const GenericBindings> genericBindings_;
std::unordered_map<FunctionId, std::shared_ptr<const GenericBindings>> functionBindings_;
std::unordered_map<FunctionId, FunctionId> genericOrigins_;
std::unordered_map<std::string, FunctionId> functionSpecializations_;
std::size_t specializationCount_ = 0;
std::string genericParameterLabel(const GenericParameter &parameter) const {
  return parameter.internalName.empty() ? name(parameter.name) : parameter.internalName;
}
std::string functionName(const Function &function) const {
  return function.specializedName.empty() ? name(function.name) : function.specializedName;
}
std::string genericTypeKey(const ValueType &value, std::size_t depth = 0) const {
  if (depth >= limits_.maxDepth)
    return "$depth";
  std::string key = std::to_string(static_cast<int>(value.builtin)) + ":" +
                    std::to_string(static_cast<int>(value.element)) + ":" +
                    (value.record ? std::to_string(*value.record) : "-") +
                    (value.array    ? "a"
                     : value.tensor ? "t"
                                    : "s");
  if (value.tensor && !value.array)
    for (auto extent : value.shape)
      key += extent.constant ? "/" + std::to_string(*extent.constant) : "/?";
  if (value.callable) {
    key += "(";
    for (std::size_t i = 0; i < value.callable->parameters.size(); ++i)
      key += (value.callable->owns(i)      ? "own"
              : value.callable->mutates(i) ? "mut"
                                           : "") +
             genericTypeKey(value.callable->parameters[i], depth + 1) + ",";
    key += ")->" + genericTypeKey(value.callable->result, depth + 1);
  }
  if (value.fallible)
    key += "!" + (value.errorRecord ? std::to_string(*value.errorRecord) : "TaskError");
  return key;
}
ValueType genericValueType(ValueType value) {
  value.identity = 0;
  value.task = value.fallible = false;
  value.errorRecord.reset();
  value.device.reset();
  if (value.array)
    value.shape = {dynamic()};
  return value;
}
bool genericParameterNamed(const std::vector<GenericParameter> &parameters,
                           const std::string &label) const {
  return std::ranges::any_of(parameters,
                             [&](const auto &p) { return genericParameterLabel(p) == label; });
}
bool inferGenericType(const TypeSyntax &pattern, ValueType actual,
                      const std::vector<GenericParameter> &parameters, GenericBindings &bindings,
                      SourceSpan site) {
  const auto label = name(pattern.name);
  if (!pattern.function && !pattern.tensor && pattern.members.empty() &&
      genericParameterNamed(parameters, label)) {
    if (!pattern.arguments.empty()) {
      error("S082", "type parameters are types, not generic type constructors", pattern.span);
      return false;
    }
    if (pattern.array) {
      if (!actual.array)
        return true; // The ordinary argument checker reports category mismatches.
      actual = recordArrayElement(actual);
    }
    actual = genericValueType(actual);
    const auto [found, inserted] = bindings.emplace(label, actual);
    if (!inserted && genericTypeKey(found->second) != genericTypeKey(actual)) {
      error("S082",
            "conflicting inferred types for '" + label + "': " + describeType(found->second) +
                " versus " + describeType(actual),
            site);
      return false;
    }
    return true;
  }
  if (pattern.tensor && pattern.element && actual.tensor && !actual.array) {
    TypeSyntax element;
    element.name = element.span = *pattern.element;
    auto scalar = actual;
    scalar.tensor = false;
    scalar.shape.clear();
    return inferGenericType(element, scalar, parameters, bindings, site);
  }
  if (pattern.function && actual.callable) {
    if (pattern.function->parameters.size() != actual.callable->parameters.size())
      return true;
    for (std::size_t i = 0; i < pattern.function->parameters.size(); ++i)
      if (!inferGenericType(pattern.function->parameters[i], actual.callable->parameters[i],
                            parameters, bindings, site))
        return false;
    auto result = actual.callable->result;
    result.fallible = false;
    result.errorRecord.reset();
    if (!inferGenericType(pattern.function->result, result, parameters, bindings, site))
      return false;
    if (pattern.function->errorTypes.size() == 1 && actual.callable->result.fallible) {
      const auto familyName = qualifiedErrorName(pattern.function->errorTypes[0]);
      if (genericParameterNamed(parameters, familyName)) {
        auto family = actual.callable->result.errorRecord
                          ? recordType(*actual.callable->result.errorRecord)
                          : ValueType{};
        if (!actual.callable->result.errorRecord)
          family.builtin = BuiltinValue::TaskError;
        TypeSyntax channel;
        channel.name = channel.span = pattern.function->errorTypes[0];
        return inferGenericType(channel, family, parameters, bindings, site);
      }
    }
    return true;
  }
  return inferGenericRecordArguments(pattern, actual, parameters, bindings, site);
}
bool genericTypeKnown(const TypeSyntax &syntax, const std::vector<GenericParameter> &parameters,
                      const GenericBindings &bindings) const {
  if (genericParameterNamed(parameters, name(syntax.name)) && !bindings.contains(name(syntax.name)))
    return false;
  if (!syntax.members.empty() && genericParameterNamed(parameters, name(syntax.name)) &&
      !bindings.contains(associatedName(syntax)))
    return false;
  if (syntax.tensor && syntax.element && genericParameterNamed(parameters, name(*syntax.element)) &&
      !bindings.contains(name(*syntax.element)))
    return false;
  if (syntax.function) {
    if (!genericTypeKnown(syntax.function->result, parameters, bindings))
      return false;
    for (auto channel : syntax.function->errorTypes) {
      const auto label = qualifiedErrorName(channel);
      if (genericParameterNamed(parameters, label.substr(0, label.find('.'))) &&
          !bindings.contains(label))
        return false;
    }
    for (const auto &p : syntax.function->parameters)
      if (!genericTypeKnown(p, parameters, bindings))
        return false;
  }
  return std::ranges::all_of(
      syntax.arguments, [&](const auto &p) { return genericTypeKnown(p, parameters, bindings); });
}
std::optional<ValueType> genericArgumentHint(Frame &frame, ExprId expression) {
  const auto &value = syntax_.expressions[expression].value;
  if (const auto *read = std::get_if<NameExpr>(&value)) {
    if (auto symbol = lookupSymbol(frame, read->name))
      return output_.symbols[*symbol].type;
    if (auto found = functions_.find(functionKey(frame, name(read->name)));
        found != functions_.end() && syntax_.functions[found->second].generics.empty()) {
      const auto function = output_.functions[found->second];
      if (!function.declaredResult)
        return {};
      FunctionSignature signature{{}, *function.declaredResult};
      for (auto parameter : function.parameters) {
        const auto &symbol = output_.symbols[parameter];
        signature.parameters.push_back(symbol.type);
        signature.mutableParameters.push_back(symbol.mutableAccess);
        signature.ownedParameters.push_back(symbol.owned);
      }
      ValueType type;
      type.builtin = BuiltinValue::Function;
      type.callable = std::make_shared<const FunctionSignature>(std::move(signature));
      return type;
    }
  } else if (const auto *call = std::get_if<CallExpr>(&value)) {
    if (const auto *callee = std::get_if<NameExpr>(&syntax_.expressions[call->callee].value)) {
      if (auto symbol = lookupSymbol(frame, callee->name)) {
        const auto &type = output_.symbols[*symbol].type;
        return type.callable ? std::optional{type.callable->result} : std::nullopt;
      }
      if (auto numeric = resolveNumeric(name(callee->name))) {
        ValueType type;
        type.element = *numeric;
        return type;
      }
      if (auto found = functions_.find(functionKey(frame, name(callee->name)));
          found != functions_.end() && syntax_.functions[found->second].generics.empty())
        return output_.functions[found->second].declaredResult;
    }
  }
  return {};
}
std::optional<FunctionId> specializeFunction(FunctionId declaration, GenericBindings bindings,
                                             SourceSpan site) {
  const auto original = syntax_.functions[declaration];
  std::string key = std::to_string(declaration);
  for (const auto &parameter : original.generics) {
    const auto found = bindings.find(genericParameterLabel(parameter));
    if (found == bindings.end()) {
      error("S082",
            "cannot infer type parameter '" + genericParameterLabel(parameter) +
                "'; supply explicit type arguments",
            site);
      return {};
    }
    key += "[" + genericTypeKey(found->second) + "]";
  }
  if (auto found = functionSpecializations_.find(key); found != functionSpecializations_.end())
    return found->second;
  if (!checkGenericBounds(original.generics, bindings, site))
    return {};
  if (++specializationCount_ > limits_.maxSpecializations) {
    error("S082", "generic specialization budget exceeded; check for expanding recursive types",
          site);
    return {};
  }
  // Anonymous functions have typed IDs but no source Function. Preserve their
  // slots when adding a new named specialization after closure checking.
  const auto id = output_.functions.size();
  syntax_.functions.resize(id);
  auto specialized = original;
  specialized.generics.clear();
  specialized.exported = false;
  specialized.specializedName =
      "__generic_" + std::to_string(declaration) + "::" + std::to_string(id);
  syntax_.functions.push_back(std::move(specialized));
  output_.functions.emplace_back();
  states_.resize(id + 1, State::Complete);
  states_[id] = State::Unseen;
  signatures_.resize(id + 1, false);
  signatures_[id] = true;
  errorFamilies_.resize(id + 1);
  requirements_.resize(id + 1);
  functionBindings_[id] = std::make_shared<const GenericBindings>(std::move(bindings));
  genericOrigins_[id] = declaration;
  functionSpecializations_[std::move(key)] = id;
  registerSignatures(id, false);
  if (!signatures_[id])
    return {};
  return id;
}
std::optional<ValueId>
invokeGeneric(Frame &frame, SourceSpan span, SourceSpan callee, FunctionId declaration,
              const std::vector<TypeSyntax> &explicitTypes, const std::vector<ExprId> &arguments,
              const std::optional<ValueType> &expected, std::optional<ExprId> selection = {},
              std::optional<ValueId> *checkedSelection = nullptr, bool spawned = false) {
  const auto function = syntax_.functions[declaration];
  const bool editing = queryingSignature(span);
  if (arguments.size() != function.parameters.size() &&
      !(editing && arguments.size() <= function.parameters.size())) {
    error("S027", "wrong argument count for generic function '" + name(function.name) + "'", span);
    return {};
  }
  GenericBindings bindings;
  if (!explicitTypes.empty()) {
    if (explicitTypes.size() != function.generics.size()) {
      error("S082", "wrong number of generic type arguments", span);
      return {};
    }
    for (std::size_t i = 0; i < explicitTypes.size(); ++i) {
      auto resolved = type(explicitTypes[i]);
      if (!resolved)
        return {};
      bindings.emplace(genericParameterLabel(function.generics[i]), genericValueType(*resolved));
    }
  }
  // Seed from already-known values without executing or duplicating any operand.
  for (std::size_t i = 0; i < arguments.size(); ++i)
    if (auto hint = genericArgumentHint(frame, arguments[i]))
      if (!inferGenericType(function.parameters[i].type, *hint, function.generics, bindings, span))
        return {};
  if (expected && function.result) {
    auto success = *expected;
    success.task = success.fallible = false;
    success.errorRecord.reset();
    if (!inferGenericType(*function.result, success, function.generics, bindings, span))
      return {};
  }
  if (!resolveKnownGenericBounds(function.generics, bindings, span))
    return {};
  GenericTermBindings terms;
  if (genericContext(frame)) {
    for (std::size_t i = 0; i < explicitTypes.size(); ++i)
      terms[genericParameterLabel(function.generics[i])] = sourceGenericTerm(
          frame, explicitTypes[i], bindings.at(genericParameterLabel(function.generics[i])));
    for (std::size_t i = 0; i < arguments.size(); ++i)
      if (const auto *read = std::get_if<NameExpr>(&syntax_.expressions[arguments[i]].value))
        if (auto symbol = lookupSymbol(frame, read->name))
          if (auto term = genericSymbolTerms_.find(*symbol); term != genericSymbolTerms_.end())
            inferGenericTermBindings(function.parameters[i].type, term->second, function.generics,
                                     terms);
    if (expected && function.result && genericExpectedTerm_ &&
        genericTypeKey(genericExpectedTerm_->actual) == genericTypeKey(*expected))
      inferGenericTermBindings(*function.result, *genericExpectedTerm_, function.generics, terms);
  }
  std::vector<ValueId> operands;
  const auto prefix =
      editing ? options_.signature->argumentIndex.value_or(arguments.size()) : arguments.size();
  if (prefix > arguments.size())
    return {};
  for (std::size_t i = 0; i < prefix; ++i) {
    std::optional<ValueType> context;
    if (genericTypeKnown(function.parameters[i].type, function.generics, bindings)) {
      GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                         proofOwnerForFunction(declaration));
      context = type(function.parameters[i].type);
      if (!context)
        return {};
    }
    GenericExpectedScope expectedTerm(
        *this,
        context && genericContext(frame)
            ? std::optional{instantiateGenericTerm(function.parameters[i].type, *context, terms)}
            : std::nullopt);
    auto value = expression(frame, arguments[i], context, options_.placement && spawned);
    if (!value)
      return {};
    if (node(*value).type.task || node(*value).type.fallible) {
      error(node(*value).type.task ? "S035" : "S036",
            "generic arguments require completed values; await/try the argument first",
            syntax_.expressions[arguments[i]].span);
      return {};
    }
    if (!inferGenericType(function.parameters[i].type, node(*value).type, function.generics,
                          bindings, span))
      return {};
    operands.push_back(*value);
    if (!resolveKnownGenericBounds(function.generics, bindings, span))
      return {};
    if (genericContext(frame))
      inferGenericTermBindings(function.parameters[i].type, genericTerm(*value), function.generics,
                               terms);
  }
  if (editing) {
    // Earlier arguments have been checked normally. A compound incomplete
    // argument needs its ordinary checked-hole probe; decline rather than
    // fabricate an abstract runtime type or trust an unresolved expression.
    if (!options_.signature->argumentReceiver && !options_.signature->editingExpression) {
      CallableSignatureSnapshot snapshot{};
      snapshot.sourceSignature = output_.functions[declaration].sourceSignature;
      options_.signature->snapshot = std::move(snapshot);
    }
    error("S027", "editing generic argument requires a signature snapshot", span);
    return {};
  }
  rememberGenericFunctionUse(callee, declaration);
  if (genericContext(frame)) {
    for (const auto &parameter : function.generics) {
      const auto label = genericParameterLabel(parameter);
      if (!terms.contains(label) && bindings.contains(label))
        terms[label] = concreteGenericTerm(bindings.at(label));
    }
    if (!checkGenericTermBounds(frame, function.generics, terms, span))
      return {};
  }
  auto specialized = specializeFunction(declaration, std::move(bindings), span);
  if (!specialized)
    return {};
  auto result = invoke(frame, span, callee, arguments, expected, selection, checkedSelection,
                       spawned, specialized, std::move(operands));
  if (result && genericContext(frame)) {
    auto term =
        function.result
            ? instantiateGenericTerm(*function.result, node(*result).type, terms)
            : substituteGenericTerm(genericTerm(output_.functions[*specialized].result), terms);
    term.actual = node(*result).type;
    if (node(*result).type.fallible && !term.failure)
      term.failure = concreteGenericTerm(node(*result).type).failure;
    if (term.failure && function.errorTypes.size() == 1)
      term.failure = std::make_shared<const GenericTerm>(
          genericErrorTerm(function.errorTypes[0], term.failure->actual, terms));
    genericValueTerms_[*result] = finishGenericTerm(std::move(term));
  }
  return result;
}
std::optional<ValueId> specializedFunctionValue(Frame &frame, SourceSpan span,
                                                FunctionId declaration,
                                                const std::vector<TypeSyntax> &arguments,
                                                SourceSpan callee) {
  const auto original = syntax_.functions[declaration];
  rememberGenericFunctionUse(callee, declaration);
  if (original.generics.size() != arguments.size()) {
    error("S082", "wrong number of generic type arguments", span);
    return {};
  }
  GenericBindings bindings;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    auto value = type(arguments[i]);
    if (!value)
      return {};
    bindings.emplace(genericParameterLabel(original.generics[i]), genericValueType(*value));
  }
  if (!checkGenericTypeApplication(original.generics, bindings, arguments, span))
    return {};
  auto target = specializeFunction(declaration, std::move(bindings), span);
  if (!target || !analyzeFunction(*target))
    return {};
  const auto function = output_.functions[*target];
  std::vector<ValueType> parameters;
  std::vector<bool> mutations, ownership;
  for (auto symbol : function.parameters) {
    parameters.push_back(output_.symbols[symbol].type);
    mutations.push_back(output_.symbols[symbol].mutableAccess);
    ownership.push_back(output_.symbols[symbol].owned);
  }
  auto signature = callableType(std::move(parameters), function.resultType, span,
                                std::move(mutations), std::move(ownership));
  if (!signature)
    return {};
  auto result = add(frame, *signature, span, FunctionReference{*target});
  if (result && genericContext(frame)) {
    GenericTermBindings terms;
    for (std::size_t i = 0; i < arguments.size(); ++i)
      terms[genericParameterLabel(original.generics[i])] = sourceGenericTerm(
          frame, arguments[i],
          functionBindings_.at(*target)->at(genericParameterLabel(original.generics[i])));
    auto term = concreteGenericTerm(*signature);
    for (std::size_t i = 0; i < original.parameters.size(); ++i)
      term.arguments[i] = instantiateGenericTerm(original.parameters[i].type,
                                                 signature->callable->parameters[i], terms);
    if (original.result)
      term.arguments.back() =
          instantiateGenericTerm(*original.result, signature->callable->result, terms);
    if (term.error && original.errorTypes.size() == 1)
      if (auto channel = terms.find(qualifiedErrorName(original.errorTypes[0]));
          channel != terms.end())
        term.error = std::make_shared<const GenericTerm>(channel->second);
    genericValueTerms_[*result] = std::move(term);
  }
  return result;
}
#include "GenericNominals.h"
#include "GenericTerms.h"
#include "TraitChecking.h"

#include "GenericSource.h"
