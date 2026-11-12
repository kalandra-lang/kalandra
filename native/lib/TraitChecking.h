struct TraitImpl {
  std::size_t declaration, trait;
  std::unordered_map<std::string, FunctionId> methods;
};
struct TraitConformance {
  State state = State::Unseen;
  GenericBindings bindings;
  std::unordered_map<std::string, FunctionId> methods;
  std::optional<std::size_t> implementation;
  bool signaturesChecked = false;
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
  return prefix + name(syntax.name);
}
bool overlappingPatterns(const TypeSyntax &a, const std::vector<GenericParameter> &ap,
                         const TypeSyntax &b, const std::vector<GenericParameter> &bp) {
  if ((!a.array && genericParameterNamed(ap, name(a.name))) ||
      (!b.array && genericParameterNamed(bp, name(b.name))))
    return true;
  if (a.array != b.array || a.tensor != b.tensor || bool(a.function) != bool(b.function))
    return false;
  if (genericParameterNamed(ap, name(a.name)) || genericParameterNamed(bp, name(b.name)))
    return true;
  if (a.function && b.function) {
    const auto &left = *a.function, &right = *b.function;
    if (left.parameters.size() != right.parameters.size())
      return false;
    for (std::size_t i = 0; i < left.parameters.size(); ++i) {
      const auto flag = [i](const std::vector<bool> &flags) {
        return i < flags.size() && flags[i];
      };
      if (flag(left.mutableParameters) != flag(right.mutableParameters) ||
          flag(left.ownedParameters) != flag(right.ownedParameters) ||
          !overlappingPatterns(left.parameters[i], ap, right.parameters[i], bp))
        return false;
    }
    if (left.errorTypes.empty() != right.errorTypes.empty())
      return false;
    return overlappingPatterns(left.result, ap, right.result, bp);
  }
  if (nominalPatternHead(a) != nominalPatternHead(b) || a.arguments.size() != b.arguments.size())
    return false;
  if (a.tensor) {
    if (a.dimensions.size() != b.dimensions.size())
      return false;
    for (std::size_t i = 0; i < a.dimensions.size(); ++i)
      if (a.dimensions[i].extent && b.dimensions[i].extent &&
          a.dimensions[i].extent != b.dimensions[i].extent)
        return false;
    if (a.element && b.element && name(*a.element) != name(*b.element) &&
        !genericParameterNamed(ap, name(*a.element)) &&
        !genericParameterNamed(bp, name(*b.element)))
      return false;
  }
  for (std::size_t i = 0; i < a.arguments.size(); ++i)
    if (!overlappingPatterns(a.arguments[i], ap, b.arguments[i], bp))
      return false;
  return true;
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
    std::unordered_set<std::string> targetParameters;
    std::vector<const TypeSyntax *> pending{&implementation.target};
    while (!pending.empty()) {
      const auto *type = pending.back();
      pending.pop_back();
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
        error("S083", "implementation type parameters must be determined by the target type",
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
    if (module != syntax_.source->partIndex(trait.name.begin) && owner != module)
      error("S083", "implementation must belong to the module defining the trait or target type",
            implementation.trait);
    if (!implementation.target.array &&
        genericParameterNamed(implementation.generics, name(implementation.target.name)))
      error("S083", "blanket implementations for an unconstrained type parameter are not supported",
            implementation.target.span);
    for (const auto &previous : traitImplementations_)
      if (previous.trait == selected->second) {
        const auto &pattern = syntax_.implementations[previous.declaration];
        if (overlappingPatterns(pattern.target, pattern.generics, implementation.target,
                                implementation.generics))
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
  return signature;
}
std::shared_ptr<TraitConformance> requireConformance(std::size_t trait, ValueType actual,
                                                     SourceSpan site) {
  actual = genericValueType(actual);
  const auto key = std::to_string(trait) + ":" + genericTypeKey(actual);
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
  const auto before = errorCount_;
  const auto declaration = syntax_.traits[trait];
  for (const auto &implementation : traitImplementations_) {
    if (implementation.trait != trait)
      continue;
    const auto original = syntax_.implementations[implementation.declaration];
    GenericBindings bindings;
    if (!matchImplementation(original.target, actual, original.generics, bindings))
      continue;
    if (!checkGenericBounds(original.generics, bindings, site))
      break;
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
    for (std::size_t i = 0; i < declaration.associatedTypes.size(); ++i)
      if (i < declaration.associatedBounds.size())
        for (auto bound : declaration.associatedBounds[i]) {
          const auto needed =
              traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
          if (needed == traitNames_.end() ||
              !requireConformance(needed->second,
                                  bindings.at("Self." + name(declaration.associatedTypes[i])),
                                  original.trait)) {
            error("S083", "associated type does not satisfy its declared trait bounds", bound);
            conformance->state = State::Failed;
            return nullptr;
          }
        }
    conformance->bindings = bindings;
    conformance->implementation = implementation.declaration;
    if (!genericRegistryReady_) {
      conformance->state = State::Complete;
      return conformance;
    }
    for (const auto &required : declaration.methods) {
      const auto selected = implementation.methods.find(name(required.name));
      if (selected == implementation.methods.end())
        break;
      auto signature = requirementSignature(required, bindings, trait);
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
                        syntax_.functions[*method].taskError == required.taskError &&
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
  for (const auto &parameter : parameters) {
    const auto label = name(parameter.name);
    if (parameter.bounds.empty())
      continue;
    auto actual = bindings.find(label);
    if (actual == bindings.end()) {
      error("S082", "cannot infer constrained parameter '" + label + "'", site);
      return false;
    }
    const auto value = actual->second;
    for (auto bound : parameter.bounds) {
      auto trait =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (trait == traitNames_.end()) {
        error("S083", "unknown trait bound '" + name(bound) + "'", bound);
        return false;
      }
      auto conformance = requireConformance(trait->second, value, site);
      if (!conformance)
        return false;
      for (auto associated : syntax_.traits[trait->second].associatedTypes) {
        const auto key = label + "." + name(associated);
        const auto item = conformance->bindings.at("Self." + name(associated));
        const auto [previous, inserted] = bindings.emplace(key, item);
        if (!inserted && genericTypeKey(previous->second) != genericTypeKey(item)) {
          error("S083", "conflicting associated type requirements for " + key, site);
          return false;
        }
      }
    }
  }
  return true;
}
bool resolveKnownGenericBounds(const std::vector<GenericParameter> &parameters,
                               GenericBindings &bindings, SourceSpan site) {
  for (const auto &parameter : parameters)
    if (bindings.contains(genericParameterLabel(parameter)) && !parameter.bounds.empty())
      if (!checkGenericBounds({parameter}, bindings, site))
        return false;
  return true;
}
void validateTraitImplementations() {
  genericRegistryReady_ = true;
  const auto validateBounds = [&](const std::vector<SourceSpan> &bounds) {
    std::unordered_set<std::size_t> seen;
    for (auto bound : bounds) {
      const auto found =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (found == traitNames_.end())
        error("S083", "unknown trait bound '" + name(bound) + "'", bound);
      else if (!seen.insert(found->second).second)
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
  for (const auto &trait : syntax_.traits)
    for (const auto &bounds : trait.associatedBounds)
      validateBounds(bounds);
  // Concrete implementations are checked even when not selected by a call.
  for (const auto &implementation : traitImplementations_) {
    const auto original = syntax_.implementations[implementation.declaration];
    if (!original.generics.empty())
      continue;
    GenericScope scope(*this, nullptr);
    if (auto target = type(original.target))
      requireConformance(implementation.trait, *target, original.trait);
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
    if (types.size() != 1) {
      error("S083", "explicit trait calls require exactly one implementing type", span);
      return {};
    }
    auto actual = type(types[0]);
    if (!actual)
      return {};
    bindings["Self"] = genericValueType(*actual);
  }
  // Reuse recursive generic unification for Self in arrays, nominal arguments,
  // tensor elements and callback signatures, not only a bare Self parameter.
  GenericParameter selfParameter;
  selfParameter.name = trait.name;
  selfParameter.internalName = "Self";
  const std::vector<GenericParameter> selfParameters{selfParameter};
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
    if (bindings.contains("Self")) {
      auto conformance = requireConformance(found->second, bindings.at("Self"), span);
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
  auto conformance = requireConformance(found->second, bindings.at("Self"), span);
  if (!conformance)
    return {};
  auto selfTerm = concreteGenericTerm(bindings.at("Self"));
  if (!types.empty())
    selfTerm = sourceGenericTerm(frame, types[0], bindings.at("Self"));
  GenericTermBindings selfTerms;
  for (std::size_t i = 0; i < operands.size(); ++i)
    inferGenericTermBindings(requirement->parameters[i].type, genericTerm(operands[i]),
                             selfParameters, selfTerms);
  if (auto inferred = selfTerms.find("Self"); inferred != selfTerms.end())
    selfTerm = mergeGenericTerms(std::move(selfTerm), inferred->second);
  if (!requireGenericTraitProof(frame, found->second, selfTerm, span))
    return {};
  const auto method = conformance->methods.at(name(methodName));
  ResolvedSourceToken methodToken{methodName, requirement->name, {}, {}};
  methodToken.label = sourceFunctionLabel(*requirement);
  rememberSourceToken(std::move(methodToken));
  auto result = invoke(frame, span, methodName, arguments, expected, selection, checkedSelection,
                       spawned, method, std::move(operands));
  if (result && genericContext(frame)) {
    GenericTermBindings terms{{"Self", selfTerm}};
    for (auto associated : trait.associatedTypes) {
      auto item = concreteGenericTerm(conformance->bindings.at("Self." + name(associated)));
      for (const auto &variable : selfTerm.variables)
        item.variables.insert(variable + "." + name(associated));
      if (selfTerm.variables.empty() && conformance->implementation) {
        const auto &impl = syntax_.implementations[*conformance->implementation];
        GenericTermBindings implTerms;
        inferGenericTermBindings(impl.target, selfTerm, impl.generics, implTerms);
        implTerms["Self"] = selfTerm;
        for (const auto &[label, annotation] : impl.associatedTypes)
          if (name(label) == name(associated))
            item = instantiateGenericTerm(annotation, item.actual, implTerms);
      }
      terms["Self." + name(associated)] = std::move(item);
    }
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
