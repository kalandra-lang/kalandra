// Compile-time identities for associated projections. Tokens name a complete
// trait application, never just its currently coincident concrete result.
struct GenericProjection {
  std::size_t trait;
  GenericTerm self;
  std::vector<GenericTerm> arguments;
  std::string member;
  ValueType output;
};
std::unordered_map<std::string, GenericProjection> genericProjections_;
std::unordered_map<std::string, std::string> genericProjectionKeys_;
std::string genericVariableLabel(const std::string &variable, std::size_t depth = 0) const {
  if (depth >= limits_.maxDepth)
    return "associated type";
  const auto dot = variable.find('.');
  if (auto found = genericProjections_.find(variable.substr(0, dot));
      found != genericProjections_.end()) {
    const auto &projection = found->second;
    const auto label = [&](const GenericTerm &term) {
      return term.variables.size() == 1 ? genericVariableLabel(*term.variables.begin(), depth + 1)
                                        : describeType(term.actual);
    };
    std::string result = name(syntax_.traits[projection.trait].name) + "[" + label(projection.self);
    for (const auto &argument : projection.arguments)
      result += ", " + label(argument);
    return result + "]." + projection.member +
           (dot == std::string::npos ? "" : variable.substr(dot));
  }
  if (variable.starts_with("$"))
    return "derived generic type";
  return variable;
}
std::string genericTermIdentityKey(const GenericTerm &term, std::size_t depth = 0) {
  if (depth >= limits_.maxDepth || !genericTermWork())
    return "$budget";
  std::string result;
  const auto append = [&](const std::string &part) {
    result += std::to_string(part.size()) + ":" + part;
  };
  append(genericTypeKey(term.actual));
  append(std::to_string(term.variables.size()));
  for (const auto &variable : term.variables)
    append(variable);
  append(std::to_string(term.arguments.size()));
  for (const auto &argument : term.arguments)
    append(genericTermIdentityKey(argument, depth + 1));
  append(term.error ? genericTermIdentityKey(*term.error, depth + 1) : "-");
  append(term.failure ? genericTermIdentityKey(*term.failure, depth + 1) : "-");
  if (result.size() > limits_.maxValues) {
    genericTermWork(limits_.maxValues + 1);
    return "$budget";
  }
  return result;
}
GenericTerm projectedGenericTerm(std::size_t trait, const GenericTerm &self,
                                 const std::vector<GenericTerm> &arguments,
                                 const std::string &member, ValueType output) {
  if (!dependentGenericTerm(self) && std::ranges::none_of(arguments, [&](const auto &argument) {
        return dependentGenericTerm(argument);
      }))
    return concreteGenericTerm(output);
  // A known nominal constructor may expose its generic implementation's
  // associated definition. An opaque T must retain the public projection.
  if (self.variables.empty()) {
    std::vector<ValueType> actual;
    for (const auto &argument : arguments)
      actual.push_back(argument.actual);
    auto key = std::to_string(trait) + ":" + genericTypeKey(genericValueType(self.actual));
    for (const auto &argument : actual)
      key += ";" + genericTypeKey(argument);
    std::optional<std::size_t> selected;
    auto cached = conformances_.find(key);
    if (cached != conformances_.end() && cached->second->state == State::Checking) {
      if (genericProofOwner_ && genericProofOwner_->selfTrait == trait)
        selected = genericProofOwner_->implementation;
    } else {
      auto conformance = requireConformance(trait, self.actual, syntax_.traits[trait].name, actual);
      if (conformance)
        selected = conformance->implementation;
    }
    if (selected) {
      const auto &implementation = syntax_.implementations[*selected];
      GenericTermBindings terms;
      inferGenericTermBindings(implementation.target, self, implementation.generics, terms);
      for (std::size_t n = 0; n < arguments.size(); ++n)
        inferGenericTermBindings(implementation.traitArguments[n], arguments[n],
                                 implementation.generics, terms);
      bool matches = sameGenericTermIdentity(
          instantiateGenericTerm(implementation.target, self.actual, terms), self);
      for (std::size_t n = 0; matches && n < arguments.size(); ++n)
        matches = sameGenericTermIdentity(
            instantiateGenericTerm(implementation.traitArguments[n], arguments[n].actual, terms),
            arguments[n]);
      if (matches) {
        terms["Self"] = self;
        for (const auto &[label, annotation] : implementation.associatedTypes)
          if (name(label) == member)
            return instantiateGenericTerm(annotation, output, terms);
      }
    }
  }
  std::string key = std::to_string(trait) + ":" + member + ":" + genericTermIdentityKey(self);
  for (const auto &argument : arguments)
    key += ";" + genericTermIdentityKey(argument);
  std::string token;
  if (auto it = genericProjectionKeys_.find(key); it != genericProjectionKeys_.end())
    token = it->second;
  else {
    token = "$projection:" + std::to_string(genericProjections_.size());
    genericProjections_.emplace(token, GenericProjection{trait, self, arguments, member, output});
    genericProjectionKeys_.emplace(std::move(key), token);
  }
  return abstractGenericTerm(concreteGenericTerm(output), token);
}
std::optional<std::vector<ValueType>>
concreteTraitArguments(const std::vector<TypeSyntax> &annotations,
                       const GenericBindings &bindings) {
  GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings));
  return traitArgumentTypes(annotations, bindings);
}
void normalizeAssociatedGenericTerms(const GenericProofOwner &owner,
                                     const GenericBindings &bindings,
                                     GenericTermBindings &environment) {
  for (const auto &parameter : owner.parameters) {
    const auto label = genericParameterLabel(parameter);
    if (!environment.contains(label))
      continue;
    for (const auto &bound : parameter.bounds) {
      auto found =
          traitNames_.find(functionKey(syntax_.source->partIndex(bound.begin), name(bound)));
      if (found == traitNames_.end())
        continue;
      auto actual = concreteTraitArguments(bound.arguments, bindings);
      if (!actual)
        continue;
      auto arguments = instantiateTraitArguments(bound.arguments, *actual, environment);
      for (auto member : syntax_.traits[found->second].associatedTypes) {
        const auto key = label + "." + name(member);
        if (auto item = bindings.find(key);
            item != bindings.end() && !bindings.contains("$ambiguous:" + key))
          environment[key] = projectedGenericTerm(found->second, environment.at(label), arguments,
                                                  name(member), item->second);
      }
    }
  }
}
std::optional<GenericTerm> instantiatedQualifiedProjection(const TypeSyntax &pattern,
                                                           const ValueType &actual,
                                                           const GenericTermBindings &environment) {
  if (pattern.members.size() != 1 || pattern.arguments.empty())
    return {};
  auto found = traitNames_.find(
      functionKey(syntax_.source->partIndex(pattern.name.begin), name(pattern.name)));
  if (found == traitNames_.end())
    return {};
  GenericBindings bindings;
  for (const auto &[label, term] : environment)
    bindings[label] = term.actual;
  auto values = concreteTraitArguments(pattern.arguments, bindings);
  if (!values || values->size() != syntax_.traits[found->second].generics.size() + 1)
    return {};
  auto terms = instantiateTraitArguments(pattern.arguments, *values, environment);
  auto self = terms.front();
  terms.erase(terms.begin());
  return projectedGenericTerm(found->second, self, terms, name(pattern.members[0]), actual);
}
std::pair<bool, std::optional<ValueType>> qualifiedTraitProjection(const TypeSyntax &syntax) {
  if (syntax.members.empty())
    return {false, {}};
  auto found = traitNames_.find(
      functionKey(syntax_.source->partIndex(syntax.name.begin), name(syntax.name)));
  if (found == traitNames_.end())
    return {false, {}};
  const auto &trait = syntax_.traits[found->second];
  if (syntax.arguments.size() != trait.generics.size() + 1 || syntax.members.size() != 1) {
    error("S083", "use Trait[Self, Arguments...].AssociatedType for a qualified projection",
          syntax.span);
    return {true, {}};
  }
  std::vector<ValueType> values;
  for (const auto &argument : syntax.arguments) {
    auto value = type(argument);
    if (!value)
      return {true, {}};
    values.push_back(*value);
  }
  auto self = values.front();
  values.erase(values.begin());
  const auto member = name(syntax.members[0]);
  std::optional<ValueType> output;
  if (genericProofOwner_ && genericBindings_ && genericProofOwner_->selfTrait == found->second &&
      name(syntax.arguments[0].name) == "Self" && syntax.arguments[0].members.empty() &&
      syntax.arguments[0].arguments.empty() && genericBindings_->contains("Self." + member) &&
      genericBindings_->contains("Self") &&
      genericTypeKey(genericBindings_->at("Self")) == genericTypeKey(self)) {
    std::optional<std::vector<ValueType>> own;
    if (genericProofOwner_->implementation)
      own = concreteTraitArguments(
          syntax_.implementations[*genericProofOwner_->implementation].traitArguments,
          *genericBindings_);
    else {
      own = std::vector<ValueType>{};
      for (const auto &parameter : trait.generics)
        if (auto value = genericBindings_->find(genericParameterLabel(parameter));
            value != genericBindings_->end())
          own->push_back(value->second);
    }
    bool matches = own && own->size() == values.size();
    for (std::size_t n = 0; matches && n < values.size(); ++n)
      matches = genericTypeKey((*own)[n]) == genericTypeKey(values[n]);
    if (matches)
      output = genericBindings_->at("Self." + member);
  }
  if (!output) {
    auto conformance = requireConformance(found->second, self, syntax.span, values);
    if (!conformance)
      return {true, {}};
    if (auto value = conformance->bindings.find("Self." + member);
        value != conformance->bindings.end())
      output = value->second;
  }
  const auto selected = std::ranges::find_if(trait.associatedTypes,
                                             [&](auto label) { return name(label) == member; });
  if (selected == trait.associatedTypes.end()) {
    error("S083", "trait does not declare associated type '" + member + "'", syntax.members[0]);
    return {true, {}};
  }
  if (genericProofOwner_ && genericBindings_) {
    auto environment = genericOwnerEnvironment(*genericProofOwner_, *genericBindings_);
    auto term = instantiateGenericTerm(syntax.arguments[0], self, environment);
    auto arguments = std::vector<TypeSyntax>(syntax.arguments.begin() + 1, syntax.arguments.end());
    if (!requireGenericTraitProof(*genericProofOwner_, found->second, term, syntax.span, 0,
                                  instantiateTraitArguments(arguments, values, environment)))
      return {true, {}};
  }
  rememberTraitName(syntax.name);
  ResolvedSourceToken token{syntax.members[0], *selected, {}, {}};
  token.label = "associated type " + name(trait.name) + "." + member;
  rememberSourceToken(std::move(token));
  if (!output)
    return {true, {}};
  if (syntax.array) {
    if (output->tensor || output->array || isOwnedResource(output_, *output)) {
      error("S082", "array element type must be a scalar, record or function", syntax.span);
      return {true, {}};
    }
    output->array = output->tensor = true;
    output->shape = {dynamic()};
  }
  return {true, output};
}
std::optional<ValueId> traitFunctionValue(Frame &frame, SourceSpan span, SourceSpan traitName,
                                          SourceSpan methodName,
                                          const std::vector<TypeSyntax> &types) {
  const auto trait = traitNames_.at(functionKey(frame, name(traitName)));
  const auto declaration = syntax_.traits[trait];
  completeTraitMember(traitName, methodName, trait);
  auto method = std::ranges::find_if(declaration.methods, [&](const auto &method) {
    return name(method.name) == name(methodName);
  });
  if (method == declaration.methods.end() || types.size() != declaration.generics.size() + 1) {
    error("S083", "trait function values require Trait[Self, Arguments...].function", span);
    return {};
  }
  GenericBindings bindings;
  GenericTermBindings terms;
  auto self = type(types[0]);
  if (!self)
    return {};
  bindings["Self"] = genericValueType(*self);
  terms["Self"] = sourceGenericTerm(frame, types[0], bindings.at("Self"));
  std::vector<ValueType> arguments;
  std::vector<GenericTerm> argumentTerms;
  for (std::size_t n = 0; n < declaration.generics.size(); ++n) {
    auto argument = type(types[n + 1]);
    if (!argument)
      return {};
    arguments.push_back(genericValueType(*argument));
    argumentTerms.push_back(sourceGenericTerm(frame, types[n + 1], arguments.back()));
    const auto label = genericParameterLabel(declaration.generics[n]);
    bindings[label] = arguments.back();
    terms[label] = argumentTerms.back();
  }
  auto conformance = requireConformance(trait, bindings.at("Self"), span, arguments);
  if (!conformance ||
      !requireGenericTraitProof(frame, trait, terms.at("Self"), span, argumentTerms))
    return {};
  for (auto member : declaration.associatedTypes)
    terms["Self." + name(member)] =
        projectedGenericTerm(trait, terms.at("Self"), argumentTerms, name(member),
                             conformance->bindings.at("Self." + name(member)));
  auto signature = requirementSignature(*method, conformance->bindings, trait);
  if (!signature)
    return {};
  auto type = callableType(signature->parameters, signature->result, span,
                           signature->mutableParameters, signature->ownedParameters);
  if (!type)
    return {};
  const auto target = conformance->methods.find(name(methodName));
  if (target == conformance->methods.end() || !analyzeFunction(target->second))
    return {};
  auto result = add(frame, *type, span, FunctionReference{target->second});
  if (result && genericContext(frame)) {
    auto term = concreteGenericTerm(*type);
    for (std::size_t n = 0; n < method->parameters.size(); ++n)
      term.arguments[n] =
          instantiateGenericTerm(method->parameters[n].type, signature->parameters[n], terms);
    if (method->result)
      term.arguments.back() = instantiateGenericTerm(*method->result, signature->result, terms);
    if (term.error)
      term.error = std::make_shared<const GenericTerm>(
          genericErrorChannelsTerm(method->errorTypes, term.error->actual, terms));
    genericValueTerms_[*result] = finishGenericTerm(std::move(term));
  }
  rememberTraitName(traitName);
  ResolvedSourceToken token{methodName, method->name, {}, {}};
  token.label = sourceFunctionLabel(*method);
  rememberSourceToken(std::move(token));
  return result;
}
