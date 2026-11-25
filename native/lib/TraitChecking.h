std::string traitReferenceLabel(const TraitReference &reference) const {
  std::string label = name(reference);
  if (!reference.arguments.empty()) {
    label += "[";
    for (std::size_t n = 0; n < reference.arguments.size(); ++n) {
      if (n)
        label += ",";
      label += std::string(syntax_.source->slice(reference.arguments[n].span));
    }
    label += "]";
  }
  return label;
}
std::optional<std::vector<ValueType>> traitArgumentTypes(const std::vector<TypeSyntax> &arguments,
                                                         const GenericBindings &bindings) {
  GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings), genericProofOwner_);
  std::vector<ValueType> values;
  for (const auto &argument : arguments) {
    auto value = type(argument);
    if (!value)
      return {};
    values.push_back(genericValueType(*value));
  }
  return values;
}
struct TraitImpl {
  std::size_t declaration, trait;
  std::unordered_map<std::string, FunctionId> methods;
};
struct TraitConformance {
  State state = State::Unseen;
  GenericBindings bindings;
  GenericBindings implementationBindings;
  std::unordered_map<std::string, FunctionId> methods;
  std::optional<std::size_t> implementation;
  bool signaturesChecked = false;
  bool builtinArithmetic = false;
};
std::vector<TraitImpl> traitImplementations_;
std::unordered_map<std::string, std::shared_ptr<TraitConformance>> conformances_;
bool genericRegistryReady_ = false;
std::unordered_map<FunctionId, std::size_t> implementationMethodOwners_;
std::unordered_map<FunctionId, std::size_t> implementationMethodDeclarations_;
std::string associatedName(const TypeSyntax &syntax) const {
  auto label = name(syntax.name);
  for (auto member : syntax.members)
    label += "." + name(member);
  return label;
}
bool associatedTypeReady(const TypeSyntax &syntax, const GenericBindings &bindings) const {
  if (!syntax.members.empty() && bindings.contains(name(syntax.name)) &&
      !bindings.contains(associatedName(syntax)))
    return false;
  if (syntax.function) {
    if (!associatedTypeReady(syntax.function->result, bindings))
      return false;
    for (const auto &p : syntax.function->parameters)
      if (!associatedTypeReady(p, bindings))
        return false;
  }
  return std::ranges::all_of(syntax.arguments, [&](const auto &argument) {
    return associatedTypeReady(argument, bindings);
  });
}
std::string nominalPatternHead(const TypeSyntax &syntax) {
  const auto prefix = syntax.array ? "array:" : syntax.tensor ? "tensor:" : "";
  const auto key = functionKey(syntax_.source->partIndex(syntax.name.begin), name(syntax.name));
  if (auto generic = genericRecords_.find(key); generic != genericRecords_.end())
    return prefix + std::string("generic:") + std::to_string(generic->second);
  if (auto record = records_.find(key); record != records_.end())
    return prefix + std::string("record:") + std::to_string(record->second);
  if (auto alias = aliases_.find(key); alias != aliases_.end()) {
    std::unordered_set<std::size_t> active;
    if (auto resolved = resolveAlias(alias->second, active))
      return prefix + std::string("record:") + std::to_string(*resolved);
  }
  if (name(syntax.name) == "never")
    return prefix + std::string("record:") + std::to_string(neverFamily());
  if (name(syntax.name) == "TaskError")
    return prefix + std::string("record:") + std::to_string(taskFamily());
  return prefix + name(syntax.name);
}
struct TraitPattern {
  std::string head;
  std::vector<std::shared_ptr<TraitPattern>> arguments;
  bool variable = false;
};
using TraitPatternPtr = std::shared_ptr<TraitPattern>;
TraitPatternPtr traitPattern(const TypeSyntax &syntax,
                             const std::vector<GenericParameter> &parameters,
                             const std::string &side, std::size_t &work, std::size_t depth = 0) {
  if (!work || depth > limits_.maxDepth)
    return std::make_shared<TraitPattern>(TraitPattern{"*", {}});
  --work;
  if (syntax.array) {
    auto element = syntax;
    element.array = false;
    return std::make_shared<TraitPattern>(
        TraitPattern{"array", {traitPattern(element, parameters, side, work, depth + 1)}});
  }
  if (!syntax.members.empty())
    return std::make_shared<TraitPattern>(TraitPattern{"*", {}});
  if (!syntax.tensor && !syntax.function && genericParameterNamed(parameters, name(syntax.name)))
    return std::make_shared<TraitPattern>(TraitPattern{side + name(syntax.name), {}, true});
  auto result = std::make_shared<TraitPattern>();
  result->head = nominalPatternHead(syntax);
  if (syntax.tensor) {
    result->head = "tensor:" + std::to_string(syntax.dimensions.size());
    TypeSyntax element;
    element.name = element.span = *syntax.element;
    result->arguments.push_back(traitPattern(element, parameters, side, work, depth + 1));
    for (const auto &dimension : syntax.dimensions)
      result->arguments.push_back(std::make_shared<TraitPattern>(TraitPattern{
          dimension.extent ? "dimension:" + std::to_string(*dimension.extent) : "*", {}}));
  } else if (syntax.function) {
    result->head = "function:" + std::to_string(syntax.function->parameters.size());
    for (std::size_t n = 0; n < syntax.function->parameters.size(); ++n) {
      result->head +=
          syntax.function->ownedParameters.size() > n && syntax.function->ownedParameters[n]
              ? "/own"
              : "/read";
      result->head +=
          syntax.function->mutableParameters.size() > n && syntax.function->mutableParameters[n]
              ? "/mut"
              : "/const";
      result->arguments.push_back(
          traitPattern(syntax.function->parameters[n], parameters, side, work, depth + 1));
    }
    result->arguments.push_back(
        traitPattern(syntax.function->result, parameters, side, work, depth + 1));
    TraitPatternPtr channel;
    if (syntax.function->errorTypes.size() == 1 &&
        genericParameterNamed(parameters, qualifiedErrorName(syntax.function->errorTypes[0])))
      channel = std::make_shared<TraitPattern>(
          TraitPattern{side + qualifiedErrorName(syntax.function->errorTypes[0]), {}, true});
    else {
      bool symbolic = false;
      for (auto error : syntax.function->errorTypes)
        symbolic = symbolic || syntax_.errorProjections.contains(error.begin) ||
                   genericParameterNamed(parameters, qualifiedErrorName(error).substr(
                                                         0, qualifiedErrorName(error).find('.')));
      if (symbolic)
        channel = std::make_shared<TraitPattern>(TraitPattern{"*", {}});
      else {
        std::vector<std::size_t> families;
        std::unordered_set<std::size_t> active;
        for (auto error : syntax.function->errorTypes)
          if (auto family = resolveError(error, active))
            families.push_back(*family);
        auto family = combineErrors(std::move(families), syntax.span);
        channel = std::make_shared<TraitPattern>(TraitPattern{
            !family || *family == neverFamily() ? "error:none" : "error:" + std::to_string(*family),
            {}});
      }
    }
    result->arguments.push_back(std::move(channel));
  } else
    for (const auto &argument : syntax.arguments)
      result->arguments.push_back(traitPattern(argument, parameters, side, work, depth + 1));
  return result;
}
bool overlappingImplementations(const TraitImplementation &a, const TraitImplementation &b) {
  std::size_t work = limits_.maxValues;
  auto left = std::make_shared<TraitPattern>(TraitPattern{"tuple", {}});
  auto right = std::make_shared<TraitPattern>(TraitPattern{"tuple", {}});
  left->arguments.push_back(traitPattern(a.target, a.generics, "left:", work));
  right->arguments.push_back(traitPattern(b.target, b.generics, "right:", work));
  for (const auto &argument : a.traitArguments)
    left->arguments.push_back(traitPattern(argument, a.generics, "left:", work));
  for (const auto &argument : b.traitArguments)
    right->arguments.push_back(traitPattern(argument, b.generics, "right:", work));
  std::unordered_map<std::string, TraitPatternPtr> substitutions;
  bool exhausted = !work;
  const auto resolve = [&](TraitPatternPtr value) {
    while (work && value->variable && substitutions.contains(value->head)) {
      --work;
      value = substitutions.at(value->head);
    }
    if (!work)
      exhausted = true;
    return value;
  };
  std::function<bool(const std::string &, TraitPatternPtr, std::size_t)> occurs;
  occurs = [&](const std::string &variable, TraitPatternPtr value, std::size_t depth) {
    if (!work || depth > limits_.maxDepth * 2) {
      exhausted = true;
      return true;
    }
    --work;
    value = resolve(std::move(value));
    if (value->variable)
      return value->head == variable;
    for (const auto &argument : value->arguments)
      if (occurs(variable, argument, depth + 1))
        return true;
    return false;
  };
  std::function<bool(TraitPatternPtr, TraitPatternPtr, std::size_t)> unify;
  unify = [&](TraitPatternPtr x, TraitPatternPtr y, std::size_t depth) {
    if (!work || depth > limits_.maxDepth * 2) {
      exhausted = true;
      return true;
    }
    --work;
    x = resolve(std::move(x));
    y = resolve(std::move(y));
    if (x->head == "*" || y->head == "*")
      return true;
    if (x->variable || y->variable) {
      if (!x->variable)
        std::swap(x, y);
      if (y->variable && x->head == y->head)
        return true;
      if (occurs(x->head, y, depth + 1))
        return exhausted;
      substitutions[x->head] = std::move(y);
      return true;
    }
    if (x->head != y->head || x->arguments.size() != y->arguments.size())
      return false;
    for (std::size_t n = 0; n < x->arguments.size(); ++n)
      if (!unify(x->arguments[n], y->arguments[n], depth + 1))
        return false;
    return true;
  };
  const bool overlaps = unify(std::move(left), std::move(right), 0);
  return exhausted || overlaps;
}

void registerTraitImplementations() {
  for (std::size_t i = 0; i < syntax_.implementations.size(); ++i) {
    const auto implementation = syntax_.implementations[i];
    validGenericParameters(implementation.generics);
    const auto module = syntax_.source->partIndex(implementation.trait.begin);
    const auto selected = traitNames_.find(functionKey(module, name(implementation.trait)));
    if (selected == traitNames_.end()) {
      error("S083", "unknown trait in implementation", implementation.trait);
      continue;
    }
    const auto trait = syntax_.traits[selected->second];
    if (implementation.traitArguments.size() != trait.generics.size()) {
      error("S083", "incorrect number of trait arguments in implementation", implementation.trait);
      continue;
    }
    std::unordered_set<std::string> targetParameters;
    std::vector<const TypeSyntax *> pending{&implementation.target};
    for (const auto &argument : implementation.traitArguments)
      pending.push_back(&argument);
    while (!pending.empty()) {
      const auto *type = pending.back();
      pending.pop_back();
      // Associated projections are non-injective: T.Item does not determine T.
      if (!type->members.empty())
        continue;
      targetParameters.insert(name(type->name));
      if (type->element)
        targetParameters.insert(name(*type->element));
      for (const auto &argument : type->arguments)
        pending.push_back(&argument);
      if (type->function) {
        pending.push_back(&type->function->result);
        for (const auto &parameter : type->function->parameters)
          pending.push_back(&parameter);
        for (auto channel : type->function->errorTypes)
          targetParameters.insert(qualifiedErrorName(channel));
      }
    }
    for (const auto &parameter : implementation.generics)
      if (!targetParameters.contains(name(parameter.name)))
        error("S083",
              "implementation type parameters must be determined by the implementing type or trait "
              "arguments",
              parameter.name);

    const auto targetKey = functionKey(syntax_.source->partIndex(implementation.target.name.begin),
                                       name(implementation.target.name));
    std::optional<std::size_t> owner;
    if (!implementation.target.array && !implementation.target.tensor) {
      if (auto record = records_.find(targetKey); record != records_.end())
        owner = syntax_.source->partIndex(output_.records[record->second].declaration.begin);
      if (auto record = genericRecords_.find(targetKey); record != genericRecords_.end())
        owner = syntax_.source->partIndex(syntax_.records[record->second].name.begin);
    }
    for (const auto &argument : implementation.traitArguments) {
      if (argument.array || argument.tensor || argument.function)
        continue;
      const auto key =
          functionKey(syntax_.source->partIndex(argument.name.begin), name(argument.name));
      if (auto record = records_.find(key); record != records_.end())
        if (syntax_.source->partIndex(output_.records[record->second].declaration.begin) == module)
          owner = module;
      if (auto record = genericRecords_.find(key); record != genericRecords_.end())
        if (syntax_.source->partIndex(syntax_.records[record->second].name.begin) == module)
          owner = module;
    }
    if (module != syntax_.source->partIndex(trait.name.begin) && owner != module)
      error("S083",
            "implementation must belong to the module defining the trait or an outer nominal "
            "operand type",
            implementation.trait);
    if (!implementation.target.array &&
        genericParameterNamed(implementation.generics, name(implementation.target.name)))
      error("S083", "blanket implementations for an unconstrained type parameter are not supported",
            implementation.target.span);
    for (const auto &previous : traitImplementations_)
      if (previous.trait == selected->second) {
        const auto &pattern = syntax_.implementations[previous.declaration];
        if (overlappingImplementations(pattern, implementation))
          error("S083", "overlapping trait implementations are not permitted",
                implementation.trait);
      }
    std::unordered_set<std::string> associated;
    for (const auto &[label, value] : implementation.associatedTypes) {
      (void)value;
      if (!associated.insert(name(label)).second ||
          !std::ranges::any_of(trait.associatedTypes,
                               [&](auto p) { return name(p) == name(label); }))
        error("S083", "unknown or duplicate associated type definition", label);
    }
    for (auto required : trait.associatedTypes)
      if (!associated.contains(name(required)))
        error("S083", "missing associated type '" + name(required) + "'", implementation.trait);
    TraitImpl registered{i, selected->second, {}};
    for (auto method : implementation.methods) {
      const auto label = name(method.name);
      if (registered.methods.contains(label) || !method.generics.empty() || method.exported ||
          !std::ranges::any_of(trait.methods,
                               [&](const auto &p) { return name(p.name) == label; })) {
        error("S083", "unknown, duplicate or generic implementation function", method.name);
        continue;
      }
      const auto id = syntax_.functions.size();
      method.moduleIndex = module;
      method.specializedName = "__implementation_" + std::to_string(i) + "::" + label;
      method.generics = implementation.generics;
      method.generics.push_back({implementation.target.name, {}, "Self"});
      syntax_.functions.push_back(std::move(method));
      implementationMethodOwners_[id] = selected->second;
      implementationMethodDeclarations_[id] = i;
      registered.methods[label] = id;
    }
    for (const auto &required : trait.methods)
      if (!registered.methods.contains(name(required.name)))
        error("S083", "missing trait function '" + name(required.name) + "'", implementation.trait);
    traitImplementations_.push_back(std::move(registered));
  }
  const auto count = syntax_.functions.size();
  output_.functions.resize(count);
  states_.resize(count, State::Unseen);
  signatures_.resize(count, true);
  errorFamilies_.resize(count);
  requirements_.resize(count);
}
bool matchImplementation(const TypeSyntax &pattern, const ValueType &actual,
                         const std::vector<GenericParameter> &parameters, GenericBindings &bindings,
                         std::size_t depth = 0) {
  if (depth >= limits_.maxDepth)
    return false;
  if (pattern.array) {
    if (!actual.array)
      return false;
    auto element = pattern;
    element.array = false;
    return matchImplementation(element, recordArrayElement(actual), parameters, bindings,
                               depth + 1);
  }
  if (genericParameterNamed(parameters, name(pattern.name)) && pattern.members.empty()) {
    const auto value = genericValueType(actual);
    auto [found, inserted] = bindings.emplace(name(pattern.name), value);
    return inserted || genericTypeKey(found->second) == genericTypeKey(value);
  }
  if (pattern.function) {
    if (!actual.callable || actual.array || actual.tensor ||
        !inferGenericType(pattern, actual, parameters, bindings, pattern.span))
      return false;
    GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings));
    auto resolved = type(pattern);
    return resolved && genericTypeKey(*resolved) == genericTypeKey(actual);
  }
  if (pattern.tensor) {
    if (!actual.tensor || actual.array || actual.builtin != BuiltinValue::Numeric ||
        actual.shape.size() != pattern.dimensions.size())
      return false;
    for (std::size_t i = 0; i < pattern.dimensions.size(); ++i)
      if (pattern.dimensions[i].extent && actual.shape[i].constant != pattern.dimensions[i].extent)
        return false;
    if (pattern.element && genericParameterNamed(parameters, name(*pattern.element))) {
      TypeSyntax element;
      element.name = element.span = *pattern.element;
      auto scalar = actual;
      scalar.tensor = false;
      scalar.shape.clear();
      return matchImplementation(element, scalar, parameters, bindings, depth + 1);
    }
    return pattern.element && resolveNumeric(name(*pattern.element)) == actual.element;
  }
  if (!pattern.arguments.empty()) {
    if (!actual.record)
      return false;
    const auto generic = genericRecords_.find(
        functionKey(syntax_.source->partIndex(pattern.name.begin), name(pattern.name)));
    const auto found = genericRecordInstances_.find(*actual.record);
    if (generic == genericRecords_.end() || found == genericRecordInstances_.end() ||
        generic->second != found->second.declaration ||
        pattern.arguments.size() != found->second.arguments.size())
      return false;
    const auto arguments = found->second.arguments;
    for (std::size_t i = 0; i < arguments.size(); ++i)
      if (!matchImplementation(pattern.arguments[i], arguments[i], parameters, bindings, depth + 1))
        return false;
    return true;
  }
  if (!genericTypeKnown(pattern, parameters, bindings))
    return false;
  GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings));
  auto resolved = type(pattern);
  return resolved && genericTypeKey(*resolved) == genericTypeKey(actual);
}
std::optional<FunctionSignature> requirementSignature(const Function &requirement,
                                                      const GenericBindings &bindings,
                                                      std::size_t trait) {
  GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                     proofOwnerForTrait(trait));
  FunctionSignature signature;
  for (const auto &parameter : requirement.parameters) {
    auto value = type(parameter.type);
    if (!value)
      return {};
    signature.parameters.push_back(*value);
    signature.mutableParameters.push_back(parameter.mutableAccess);
    signature.ownedParameters.push_back(parameter.owned);
  }
  if (requirement.result) {
    auto value = type(*requirement.result);
    if (!value)
      return {};
    signature.result = *value;
  } else {
    signature.result.builtin = BuiltinValue::Unit;
  }
  if (requirement.taskError) {
    std::vector<std::size_t> families;
    std::unordered_set<std::size_t> active;
    for (auto channel : requirement.errorTypes) {
      auto family = resolveError(channel, active);
      if (!family)
        return {};
      families.push_back(*family);
    }
    auto family = combineErrors(std::move(families), requirement.span);
    if (!family)
      return {};
    signature.result.fallible = true;
    signature.result.errorRecord = normalizedError(*family);
  }
  signature.result = eraseNeverError(std::move(signature.result));
  return signature;
}
std::shared_ptr<TraitConformance> requireConformance(std::size_t trait, ValueType actual,
                                                     SourceSpan site,
                                                     const std::vector<ValueType> &arguments = {}) {
  actual = genericValueType(actual);
  if (arguments.size() != syntax_.traits[trait].generics.size()) {
    error("S083", "incorrect number of trait arguments", site);
    return nullptr;
  }
  auto key = std::to_string(trait) + ":" + genericTypeKey(actual);
  for (const auto &argument : arguments)
    key += ";" + genericTypeKey(argument);
  if (auto found = conformances_.find(key); found != conformances_.end()) {
    if (found->second->state == State::Checking)
      error("S083", "cyclic trait conformance requirements", site);
    if (found->second->state != State::Complete)
      return nullptr;
    if (!genericRegistryReady_ || found->second->signaturesChecked)
      return found->second;
    // Nominal fields can project associated types before function signatures
    // are registered. Upgrade that declaration-only proof before publishing IR.
    conformances_.erase(found);
  }
  auto conformance = std::make_shared<TraitConformance>();
  conformances_[key] = conformance;
  conformance->state = State::Checking;
  if (builtinArithmeticConformance(trait, actual, arguments, site, *conformance))
    return conformance->state == State::Complete ? conformance : nullptr;
  const auto before = errorCount_;
  const auto declaration = syntax_.traits[trait];
  for (const auto &implementation : traitImplementations_) {
    if (implementation.trait != trait)
      continue;
    const auto original = syntax_.implementations[implementation.declaration];
    GenericBindings bindings;
    if (!matchImplementation(original.target, actual, original.generics, bindings))
      continue;
    bool matches = true;
    for (std::size_t n = 0; matches && n < arguments.size(); ++n)
      matches = matchImplementation(original.traitArguments[n], arguments[n], original.generics,
                                    bindings);
    if (!matches)
      continue;
    {
      GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                         proofOwnerForImplementation(implementation.declaration, trait));
      if (!checkGenericBounds(original.generics, bindings, site))
        break;
    }
    {
      GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                         proofOwnerForImplementation(implementation.declaration, trait));
      auto target = type(original.target);
      if (!target)
        break;
    }
    bindings["Self"] = actual;
    std::unordered_set<std::string> resolved;
    for (std::size_t pass = 0; pass < original.associatedTypes.size(); ++pass)
      for (const auto &[nameSpan, annotation] : original.associatedTypes) {
        const auto label = name(nameSpan);
        if (resolved.contains(label) || !associatedTypeReady(annotation, bindings))
          continue;
        GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                           proofOwnerForImplementation(implementation.declaration, trait));
        auto value = type(annotation);
        if (!value)
          break;
        bindings["Self." + label] = *value;
        resolved.insert(label);
      }
    if (resolved.size() != declaration.associatedTypes.size()) {
      error("S083", "unresolved or cyclic associated type definitions", original.trait);
      break;
    }
    // Trait parameters and implementation parameters are different lexical scopes.
    // In impl[T] Convert[String] for Box[T], the implementation T is not String.
    GenericBindings requirementBindings{{"Self", actual}};
    for (auto associated : declaration.associatedTypes)
      requirementBindings["Self." + name(associated)] = bindings.at("Self." + name(associated));
    for (std::size_t n = 0; n < arguments.size(); ++n)
      requirementBindings[genericParameterLabel(declaration.generics[n])] = arguments[n];
    {
      GenericScope scope(*this, std::make_shared<const GenericBindings>(requirementBindings),
                         proofOwnerForTrait(trait));
      if (!checkGenericBounds(declaration.generics, requirementBindings, site))
        break;
    }
    for (std::size_t i = 0; i < declaration.associatedTypes.size(); ++i)
      if (i < declaration.associatedBounds.size())
        for (auto bound : declaration.associatedBounds[i]) {
          const auto needed =
              traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
          const auto arguments = traitArgumentTypes(bound.arguments, requirementBindings);
          if (needed == traitNames_.end() || !arguments ||
              !requireConformance(needed->second,
                                  bindings.at("Self." + name(declaration.associatedTypes[i])),
                                  original.trait, *arguments)) {
            error("S083", "associated type does not satisfy its declared trait bounds", bound);
            conformance->state = State::Failed;
            return nullptr;
          }
        }
    conformance->bindings = requirementBindings;
    conformance->implementationBindings = bindings;
    conformance->implementation = implementation.declaration;
    if (!genericRegistryReady_) {
      conformance->state = State::Complete;
      return conformance;
    }
    for (const auto &required : declaration.methods) {
      const auto selected = implementation.methods.find(name(required.name));
      if (selected == implementation.methods.end())
        break;
      auto signature = requirementSignature(required, requirementBindings, trait);
      if (signature && arithmeticTrait(name(declaration.name)) == trait &&
          (signature->result.fallible ||
           std::ranges::any_of(signature->mutableParameters, [](bool value) { return value; }) ||
           std::ranges::any_of(signature->ownedParameters, [](bool value) { return value; }))) {
        error("S083", "arithmetic operator methods must be infallible and read their operands",
              required.name);
        break;
      }
      auto method = specializeFunction(selected->second, bindings, site);
      if (!signature || !method)
        break;
      if (!output_.functions[*method].declaredResult) {
        output_.functions[*method].declaredResult = signature->result;
        if (required.result)
          genericReturnTerms_[*method] = instantiateGenericTerm(*required.result, signature->result,
                                                                genericTermEnvironment(*method));
      }
      const auto concrete = output_.functions[*method];
      bool compatible = concrete.parameters.size() == signature->parameters.size() &&
                        concrete.declaredResult &&
                        sameFunctionValueType(*concrete.declaredResult, signature->result);
      for (std::size_t i = 0; compatible && i < concrete.parameters.size(); ++i) {
        const auto parameter = output_.symbols[concrete.parameters[i]];
        compatible = sameFunctionValueType(parameter.type, signature->parameters[i]) &&
                     parameter.mutableAccess == signature->mutates(i) &&
                     parameter.owned == signature->owns(i);
      }
      if (!compatible) {
        error("S083", "trait implementation signature differs in types, access or errors",
              syntax_.functions[selected->second].name);
        break;
      }
      conformance->methods[name(required.name)] = *method;
    }
    if (conformance->methods.size() != declaration.methods.size() || errorCount_ != before)
      break;
    conformance->signaturesChecked = true;
    conformance->state = State::Complete;
    return conformance;
  }
  if (errorCount_ == before)
    error("S083",
          "type " + describeType(actual) + " does not implement trait '" + name(declaration.name) +
              "'",
          site);
  conformance->state = State::Failed;
  return nullptr;
}
bool checkGenericBounds(const std::vector<GenericParameter> &parameters, GenericBindings &bindings,
                        SourceSpan site) {
  struct PendingBound {
    std::string label;
    TraitReference bound;
    bool complete = false;
  };
  std::vector<PendingBound> pending;
  for (const auto &parameter : parameters)
    for (const auto &bound : parameter.bounds)
      pending.push_back({genericParameterLabel(parameter), bound});
  std::size_t remaining = pending.size();
  while (remaining) {
    bool progress = false;
    for (auto &entry : pending) {
      if (entry.complete)
        continue;
      const auto &label = entry.label;
      const auto &bound = entry.bound;
      auto actual = bindings.find(label);
      if (actual == bindings.end()) {
        error("S082", "cannot infer constrained parameter '" + label + "'", site);
        return false;
      }
      if (!std::ranges::all_of(bound.arguments, [&](const auto &argument) {
            return associatedTypeReady(argument, bindings) &&
                   genericTypeKnown(argument, parameters, bindings);
          }))
        continue;
      const auto value = actual->second;
      auto trait =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (trait == traitNames_.end()) {
        error("S083", "unknown trait bound '" + name(bound) + "'", bound);
        return false;
      }
      auto arguments = traitArgumentTypes(bound.arguments, bindings);
      if (!arguments)
        return false;
      auto conformance = requireConformance(trait->second, value, site, *arguments);
      if (!conformance)
        return false;
      for (auto associated : syntax_.traits[trait->second].associatedTypes) {
        const auto key = label + "." + name(associated);
        const auto item = conformance->bindings.at("Self." + name(associated));
        GenericTermBindings identities;
        for (const auto &[label, actual] : bindings)
          if (!label.starts_with("$"))
            identities.emplace(label, abstractGenericTerm(concreteGenericTerm(actual), label));
        auto applications = instantiateTraitArguments(bound.arguments, *arguments, identities);
        auto origin = std::to_string(trait->second);
        for (const auto &argument : applications)
          origin += ":" + genericTermIdentityKey(argument);
        const auto prefix = "$association:" + key + ":";
        const auto marker = prefix + origin;
        bool ambiguous = bindings.contains("$ambiguous:" + key);
        for (const auto &[candidate, ignored] : bindings) {
          (void)ignored;
          if (candidate.starts_with(prefix) && candidate != marker)
            ambiguous = true;
        }
        bindings[marker] = ValueType{};
        if (ambiguous) {
          bindings.erase(key);
          bindings["$ambiguous:" + key] = ValueType{};
        } else
          bindings[key] = item;
      }

      entry.complete = true;
      --remaining;
      progress = true;
    }
    if (!progress) {
      error("S083", "unresolved or cyclic associated types in trait bounds", site);
      return false;
    }
  }
  return true;
}
bool resolveKnownGenericBounds(const std::vector<GenericParameter> &parameters,
                               GenericBindings &bindings, SourceSpan site) {
  for (const auto &parameter : parameters)
    if (bindings.contains(genericParameterLabel(parameter)) && !parameter.bounds.empty()) {
      auto known = parameter;
      std::erase_if(known.bounds, [&](const auto &bound) {
        return !std::ranges::all_of(bound.arguments, [&](const auto &argument) {
          return associatedTypeReady(argument, bindings) &&
                 genericTypeKnown(argument, parameters, bindings);
        });
      });
      if (!checkGenericBounds({known}, bindings, site))
        return false;
    }
  return true;
}
void validateTraitImplementations() {
  genericRegistryReady_ = true;
  const auto validateBounds = [&](const std::vector<TraitReference> &bounds) {
    std::unordered_set<std::string> seen;
    for (auto bound : bounds) {
      const auto found =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (found == traitNames_.end())
        error("S083", "unknown trait bound '" + name(bound) + "'", bound);
      else if (bound.arguments.size() != syntax_.traits[found->second].generics.size())
        error("S083", "incorrect number of trait arguments in bound", bound);
      else if (!seen.insert(std::to_string(found->second) + ":" + traitReferenceLabel(bound))
                    .second)
        error("S083", "duplicate trait bound", bound);
    }
  };
  const auto validateParameters = [&](const std::vector<GenericParameter> &parameters) {
    for (const auto &parameter : parameters)
      validateBounds(parameter.bounds);
  };
  for (const auto &function : syntax_.functions)
    validateParameters(function.generics);
  for (const auto &record : syntax_.records)
    validateParameters(record.generics);
  for (const auto &implementation : syntax_.implementations)
    validateParameters(implementation.generics);
  for (const auto &trait : syntax_.traits) {
    validateParameters(trait.generics);
    for (const auto &bounds : trait.associatedBounds)
      validateBounds(bounds);
  }
  // Concrete implementations are checked even when not selected by a call.
  for (const auto &implementation : traitImplementations_) {
    const auto original = syntax_.implementations[implementation.declaration];
    if (!original.generics.empty())
      continue;
    GenericScope scope(*this, nullptr);
    if (auto target = type(original.target)) {
      std::vector<ValueType> arguments;
      bool valid = true;
      for (const auto &annotation : original.traitArguments) {
        auto value = type(annotation);
        if (!value) {
          valid = false;
          break;
        }
        arguments.push_back(*value);
      }
      if (valid)
        requireConformance(implementation.trait, *target, original.trait, arguments);
    }
  }
  std::vector<std::size_t> pending;
  for (const auto &[id, instance] : genericRecordInstances_) {
    (void)instance;
    pending.push_back(id);
  }
  for (auto id : pending) {
    auto bindings = *recordBindings_.at(id);
    checkGenericBounds(syntax_.records[genericRecordInstances_.at(id).declaration].generics,
                       bindings, output_.records[id].declaration);
    recordBindings_[id] = std::make_shared<const GenericBindings>(std::move(bindings));
  }
}
void completeTraitMember(SourceSpan traitName, SourceSpan member, std::size_t trait) {
  rememberTraitName(traitName);
  if (!completing(member))
    return;
  std::vector<std::string> names;
  for (const auto &method : syntax_.traits[trait].methods)
    names.push_back(name(method.name));
  completeMembers(member, std::move(names), ScopeNameKind::Function);
}
std::optional<ValueId>
invokeTrait(Frame &frame, SourceSpan span, SourceSpan traitName, SourceSpan methodName,
            const std::vector<TypeSyntax> &types, const std::vector<ExprId> &arguments,
            const std::optional<ValueType> &expected, std::optional<ExprId> selection = {},
            std::optional<ValueId> *checkedSelection = nullptr, bool spawned = false) {
  const auto found = traitNames_.find(functionKey(frame, name(traitName)));
  if (found == traitNames_.end())
    return {};
  completeTraitMember(traitName, methodName, found->second);
  const auto trait = syntax_.traits[found->second];
  const auto requirement = std::ranges::find_if(
      trait.methods, [&](const auto &method) { return name(method.name) == name(methodName); });
  if (requirement == trait.methods.end() || arguments.size() != requirement->parameters.size()) {
    error("S083", "unknown trait function or incorrect argument count", methodName);
    return {};
  }
  GenericBindings bindings;
  if (!types.empty()) {
    if (types.size() != 1 + trait.generics.size()) {
      error("S083", "explicit trait calls require Self followed by all trait arguments", span);
      return {};
    }
    auto actual = type(types[0]);
    if (!actual)
      return {};
    bindings["Self"] = genericValueType(*actual);
    for (std::size_t n = 0; n < trait.generics.size(); ++n) {
      auto value = type(types[n + 1]);
      if (!value)
        return {};
      bindings[genericParameterLabel(trait.generics[n])] = genericValueType(*value);
    }
  }
  // Reuse recursive generic unification for Self in arrays, nominal arguments,
  // tensor elements and callback signatures, not only a bare Self parameter.
  GenericParameter selfParameter;
  selfParameter.name = trait.name;
  selfParameter.internalName = "Self";
  auto selfParameters = trait.generics;
  selfParameters.push_back(selfParameter);
  const auto traitArguments = [&]() -> std::optional<std::vector<ValueType>> {
    std::vector<ValueType> values;
    for (const auto &parameter : trait.generics) {
      auto it = bindings.find(genericParameterLabel(parameter));
      if (it == bindings.end())
        return {};
      values.push_back(it->second);
    }
    return values;
  };
  const auto inferSelf = [&](const TypeSyntax &pattern, const ValueType &actual) {
    return inferGenericType(pattern, actual, selfParameters, bindings, span);
  };
  if (expected && requirement->result && !inferSelf(*requirement->result, *expected))
    return {};
  for (std::size_t i = 0; i < arguments.size(); ++i)
    if (auto hint = genericArgumentHint(frame, arguments[i]))
      if (!inferSelf(requirement->parameters[i].type, *hint))
        return {};
  std::vector<ValueId> operands;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    std::optional<ValueType> context;
    auto knownArguments = traitArguments();
    if (bindings.contains("Self") && knownArguments) {
      auto conformance =
          requireConformance(found->second, bindings.at("Self"), span, *knownArguments);
      if (!conformance)
        return {};
      for (const auto &[key, value] : conformance->bindings)
        if (key == "Self" || key.starts_with("Self."))
          bindings[key] = value;
      GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings));
      context = type(requirement->parameters[i].type);
      if (!context)
        return {};
    }
    auto value = expression(frame, arguments[i], context, options_.placement && spawned);
    if (!value || !inferSelf(requirement->parameters[i].type, node(*value).type))
      return {};
    operands.push_back(*value);
  }
  if (!bindings.contains("Self")) {
    error("S083", "cannot infer the implementing type; use Trait[Type].function(...)", span);
    return {};
  }
  auto argumentsKnown = traitArguments();
  if (!argumentsKnown) {
    error("S083", "cannot infer trait arguments; use Trait[Self, Arguments...].function(...)",
          span);
    return {};
  }
  auto conformance = requireConformance(found->second, bindings.at("Self"), span, *argumentsKnown);
  if (!conformance)
    return {};
  auto selfTerm = concreteGenericTerm(bindings.at("Self"));
  if (!types.empty())
    selfTerm = sourceGenericTerm(frame, types[0], bindings.at("Self"));
  GenericTermBindings selfTerms;
  if (requirement->result && genericExpectedTerm_)
    inferGenericTermBindings(*requirement->result, *genericExpectedTerm_, selfParameters,
                             selfTerms);
  for (std::size_t i = 0; i < operands.size(); ++i)
    inferGenericTermBindings(requirement->parameters[i].type, genericTerm(operands[i]),
                             selfParameters, selfTerms);
  if (auto inferred = selfTerms.find("Self"); inferred != selfTerms.end())
    selfTerm = mergeGenericTerms(std::move(selfTerm), inferred->second);
  std::vector<GenericTerm> argumentTerms;
  for (std::size_t n = 0; n < trait.generics.size(); ++n) {
    const auto label = genericParameterLabel(trait.generics[n]);
    auto term =
        selfTerms.contains(label) ? selfTerms.at(label) : concreteGenericTerm(bindings.at(label));
    if (!types.empty())
      term = mergeGenericTerms(std::move(term),
                               sourceGenericTerm(frame, types[n + 1], bindings.at(label)));
    argumentTerms.push_back(std::move(term));
  }
  if (!requireGenericTraitProof(frame, found->second, selfTerm, span, argumentTerms))
    return {};
  GenericTermBindings terms = selfTerms;
  terms["Self"] = selfTerm;
  for (std::size_t n = 0; n < trait.generics.size(); ++n)
    terms[genericParameterLabel(trait.generics[n])] = argumentTerms[n];
  for (auto associated : trait.associatedTypes)
    terms["Self." + name(associated)] =
        projectedGenericTerm(found->second, selfTerm, argumentTerms, name(associated),
                             conformance->bindings.at("Self." + name(associated)));
  if (genericContext(frame))
    for (std::size_t n = 0; n < operands.size(); ++n) {
      const auto actual = genericTerm(operands[n]);
      const auto required =
          instantiateGenericTerm(requirement->parameters[n].type, node(operands[n]).type, terms);
      if ((dependentGenericTerm(actual) || dependentGenericTerm(required)) &&
          !sameGenericTermIdentity(actual, required)) {
        error("S083",
              "trait method arguments must satisfy the same abstract type relationships as its "
              "signature",
              span);
        return {};
      }
    }
  const auto method = conformance->methods.find(name(methodName));
  if (method == conformance->methods.end())
    return {};
  ResolvedSourceToken methodToken{methodName, requirement->name, {}, {}};
  methodToken.label = sourceFunctionLabel(*requirement);
  rememberSourceToken(std::move(methodToken));
  auto result = invoke(frame, span, methodName, arguments, expected, selection, checkedSelection,
                       spawned, method->second, std::move(operands));
  if (result && genericContext(frame)) {
    auto resultTerm = requirement->result
                          ? instantiateGenericTerm(*requirement->result, node(*result).type, terms)
                          : concreteGenericTerm(node(*result).type);
    if (node(*result).type.fallible) {
      auto failure = concreteGenericTerm(node(*result).type).failure;
      if (failure)
        resultTerm.failure = std::make_shared<const GenericTerm>(
            genericErrorChannelsTerm(requirement->errorTypes, failure->actual, terms));
    }
    genericValueTerms_[*result] = finishGenericTerm(std::move(resultTerm));
  }
  return result;
}
