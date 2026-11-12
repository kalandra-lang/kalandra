// Private Checker helpers: source-level editor metadata, never runtime types.
std::string sourceErrorLabel(const std::vector<SourceSpan> &errors) const {
  if (errors.empty())
    return {};
  std::string result = " ! ";
  if (errors.size() > 1)
    result += "(";
  for (auto error : errors) {
    if (result.back() != ' ' && result.back() != '(')
      result += " | ";
    result += qualifiedErrorName(error);
  }
  if (errors.size() > 1)
    result += ")";
  return result;
}
std::string sourceTypeLabel(const TypeSyntax &type, std::size_t depth = 0) const {
  if (depth >= limits_.maxDepth)
    return "…";
  std::string result;
  if (type.function) {
    result = "fn(";
    const auto &function = *type.function;
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
      if (i)
        result += ", ";
      if (i < function.ownedParameters.size() && function.ownedParameters[i])
        result += "own ";
      if (i < function.mutableParameters.size() && function.mutableParameters[i])
        result += "mut ";
      result += sourceTypeLabel(function.parameters[i], depth + 1);
    }
    result += ") -> " + sourceTypeLabel(function.result, depth + 1) +
              sourceErrorLabel(function.errorTypes);
  } else if (type.tensor) {
    result = "Tensor[" + (type.element ? name(*type.element) : "?");
    for (const auto &dimension : type.dimensions)
      result += ", " + (dimension.extent ? std::to_string(*dimension.extent) : "?");
    result += "]";
  } else {
    result = associatedName(type);
    if (!type.arguments.empty()) {
      result += "[";
      for (std::size_t i = 0; i < type.arguments.size(); ++i) {
        if (i)
          result += ", ";
        result += sourceTypeLabel(type.arguments[i], depth + 1);
      }
      result += "]";
    }
  }
  return type.array ? "[" + result + "]" : result;
}
std::string sourceTypeParameters(const std::vector<GenericParameter> &parameters) const {
  if (parameters.empty())
    return {};
  std::string result = "[";
  for (const auto &parameter : parameters) {
    if (result.size() > 1)
      result += ", ";
    result += genericParameterLabel(parameter);
    for (std::size_t i = 0; i < parameter.bounds.size(); ++i)
      result += (i ? " + " : ": ") + name(parameter.bounds[i]);
  }
  return result + "]";
}
SourceCallableSignature sourceCallableSignature(const Function &function) const {
  SourceCallableSignature result;
  result.typeParameters = sourceTypeParameters(function.generics);
  for (const auto &parameter : function.parameters)
    result.parameters.push_back(std::string(parameter.owned ? "own " : "") +
                                (parameter.mutableAccess ? "mut " : "") + name(parameter.name) +
                                ": " + sourceTypeLabel(parameter.type));
  if (function.result)
    result.result = " -> " + sourceTypeLabel(*function.result);
  result.result += sourceErrorLabel(function.errorTypes);
  return result;
}
void rememberGenericFunctionUse(SourceSpan site, FunctionId declaration) {
  ResolvedSourceToken token{site, syntax_.functions[declaration].name, {}, {}};
  token.function = declaration;
  rememberSourceToken(std::move(token));
}
std::string sourceFunctionLabel(const Function &function) const {
  const auto signature = sourceCallableSignature(function);
  std::string label = "fn " + name(function.name) + signature.typeParameters + "(";
  for (std::size_t i = 0; i < signature.parameters.size(); ++i)
    label += (i ? ", " : "") + signature.parameters[i];
  return label + ")" + signature.result;
}
void rememberTraitName(SourceSpan site) {
  const auto found =
      traitNames_.find(functionKey(syntax_.source->partIndex(site.begin), name(site)));
  if (found == traitNames_.end())
    return;
  const auto &trait = syntax_.traits[found->second];
  ResolvedSourceToken token{site, trait.name, {}, {}};
  token.label = "trait " + name(trait.name);
  rememberSourceToken(std::move(token));
}
void rememberGenericTypeSyntax(const TypeSyntax &type,
                               const std::vector<GenericParameter> &parameters) {
  std::vector<const TypeSyntax *> pending{&type};
  while (!pending.empty()) {
    const auto &current = *pending.back();
    pending.pop_back();
    for (const auto &argument : current.arguments)
      pending.push_back(&argument);
    if (current.function) {
      pending.push_back(&current.function->result);
      for (const auto &parameter : current.function->parameters)
        pending.push_back(&parameter);
    }
    const auto site = current.element.value_or(current.name);
    for (const auto &parameter : parameters)
      if (name(site) == genericParameterLabel(parameter)) {
        ResolvedSourceToken token{site, parameter.name, {}, {}};
        token.label = "type " + genericParameterLabel(parameter);
        for (std::size_t i = 0; i < parameter.bounds.size(); ++i)
          token.label += (i ? " + " : ": ") + name(parameter.bounds[i]);
        rememberSourceToken(std::move(token));
      }
    const auto found =
        genericRecords_.find(functionKey(syntax_.source->partIndex(site.begin), name(site)));
    if (found != genericRecords_.end() && !genericParameterNamed(parameters, name(site))) {
      const auto &record = syntax_.records[found->second];
      ResolvedSourceToken token{site, record.name, {}, {}};
      token.label = std::string(record.variants.empty() ? "type " : "enum ") + name(record.name) +
                    sourceTypeParameters(record.generics);
      rememberSourceToken(std::move(token));
    }
  }
}
void rememberGenericDeclarations() {
  const auto parameters = [&](const std::vector<GenericParameter> &generics) {
    for (const auto &parameter : generics) {
      TypeSyntax type;
      type.name = type.span = parameter.name;
      rememberGenericTypeSyntax(type, generics);
      for (auto bound : parameter.bounds)
        rememberTraitName(bound);
    }
  };
  for (const auto &record : syntax_.records) {
    if (record.generics.empty())
      continue;
    parameters(record.generics);
    TypeSyntax type;
    type.name = type.span = record.name;
    rememberGenericTypeSyntax(type, {});
    for (const auto &field : record.fields)
      rememberGenericTypeSyntax(field.type, record.generics);
    for (const auto &variant : record.variants)
      for (const auto &field : variant.fields)
        rememberGenericTypeSyntax(field.type, record.generics);
  }
  for (const auto &function : syntax_.functions) {
    if (function.generics.empty() || !function.specializedName.empty())
      continue;
    parameters(function.generics);
    for (const auto &parameter : function.parameters)
      rememberGenericTypeSyntax(parameter.type, function.generics);
    if (function.result)
      rememberGenericTypeSyntax(*function.result, function.generics);
  }
  for (const auto &trait : syntax_.traits) {
    rememberTraitName(trait.name);
    for (const auto &bounds : trait.associatedBounds)
      for (auto bound : bounds)
        rememberTraitName(bound);
    for (const auto &method : trait.methods) {
      ResolvedSourceToken token{method.name, method.name, {}, {}};
      token.label = sourceFunctionLabel(method);
      rememberSourceToken(std::move(token));
    }
  }
  for (const auto &implementation : syntax_.implementations) {
    rememberTraitName(implementation.trait);
    parameters(implementation.generics);
    rememberGenericTypeSyntax(implementation.target, implementation.generics);
  }
}
