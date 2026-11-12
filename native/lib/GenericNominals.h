struct GenericRecordInstance {
  std::size_t declaration;
  std::vector<ValueType> arguments;
};
std::unordered_map<std::string, std::size_t> genericRecords_, traitNames_;
std::unordered_map<std::string, std::size_t> recordSpecializations_;
std::unordered_map<std::size_t, GenericRecordInstance> genericRecordInstances_;
std::unordered_map<std::size_t, std::shared_ptr<const GenericBindings>> recordBindings_;
bool validGenericParameters(const std::vector<GenericParameter> &parameters) {
  std::unordered_set<std::string> seen;
  bool valid = true;
  for (const auto &parameter : parameters) {
    const auto label = name(parameter.name);
    if (!seen.insert(label).second || reservedGenericDeclaration(label)) {
      error("S082", "duplicate or reserved generic type parameter '" + label + "'", parameter.name);
      valid = false;
    }
    std::unordered_set<std::string> bounds;
    for (auto bound : parameter.bounds)
      if (!bounds.insert(name(bound)).second) {
        error("S083", "duplicate trait bound", bound);
        valid = false;
      }
  }
  return valid;
}
bool reservedGenericDeclaration(const std::string &label) const {
  return reservedControl(label) || resolveNumeric(label) || label == "Self" || label == "bool" ||
         label == "unit" || label == "String" || label == "Tensor" || label == "TaskError" ||
         label == "FaultKind" || label == "Device";
}
void registerGenericDeclarations() {
  for (std::size_t i = 0; i < syntax_.records.size(); ++i) {
    const auto &record = syntax_.records[i];
    if (record.generics.empty())
      continue;
    validGenericParameters(record.generics);
    if (record.error || !record.errorAlias.empty())
      error("S082", "generic declarations use type or enum, not error families/aliases",
            record.name);
    const auto label = name(record.name);
    if (reservedGenericDeclaration(label))
      error("S082", "reserved generic type name '" + label + "'", record.name);
    const auto key = functionKey(syntax_.source->partIndex(record.name.begin), label);
    if (!genericRecords_.emplace(key, i).second)
      error("S082", "duplicate generic nominal declaration '" + label + "'", record.name);
    if (record.exported)
      exports_.emplace(key, ExportBinding{false, i, false, true});
  }
  for (std::size_t i = 0; i < syntax_.traits.size(); ++i) {
    const auto &trait = syntax_.traits[i];
    const auto label = name(trait.name);
    if (reservedGenericDeclaration(label))
      error("S083", "reserved trait name '" + label + "'", trait.name);
    const auto key = functionKey(syntax_.source->partIndex(trait.name.begin), label);
    if (genericRecords_.contains(key) || !traitNames_.emplace(key, i).second)
      error("S083", "duplicate trait/type declaration '" + label + "'", trait.name);
    if (trait.exported)
      exports_.emplace(key, ExportBinding{false, i, false, false, true});
    std::unordered_set<std::string> names;
    for (auto associated : trait.associatedTypes)
      if (!names.insert(name(associated)).second || name(associated) == "Self")
        error("S083", "duplicate or reserved associated type", associated);
    for (const auto &method : trait.methods) {
      if (!names.insert(name(method.name)).second || !method.generics.empty() || method.exported)
        error("S083", "trait functions require unique names and no method type parameters",
              method.name);
    }
  }
  for (const auto &function : syntax_.functions) {
    validGenericParameters(function.generics);
    const auto key = functionKey(function.moduleIndex, name(function.name));
    if (genericRecords_.contains(key) || traitNames_.contains(key))
      error("S082", "function conflicts with generic type or trait name", function.name);
    if (!function.generics.empty() && name(function.name) == "main")
      error("S082", "main cannot have unresolved generic type parameters", function.name);
  }
}
bool inferGenericRecordArguments(const TypeSyntax &pattern, const ValueType &actual,
                                 const std::vector<GenericParameter> &parameters,
                                 GenericBindings &bindings, SourceSpan site) {
  if (pattern.arguments.empty() || !actual.record)
    return true;
  const auto instance = genericRecordInstances_.find(*actual.record);
  const auto declaration = genericRecords_.find(
      functionKey(syntax_.source->partIndex(pattern.name.begin), name(pattern.name)));
  if (instance != genericRecordInstances_.end() && declaration != genericRecords_.end() &&
      instance->second.declaration != declaration->second) {
    error("S010", "generic argument refers to a distinct nominal type declaration", site);
    return false;
  }
  if (instance == genericRecordInstances_.end() || declaration == genericRecords_.end() ||
      instance->second.arguments.size() != pattern.arguments.size())
    return true; // Ordinary concrete-type comparison reports the mismatch.
  const auto arguments = instance->second.arguments;
  for (std::size_t i = 0; i < arguments.size(); ++i)
    if (!inferGenericType(pattern.arguments[i], arguments[i], parameters, bindings, site))
      return false;
  return true;
}
std::optional<std::size_t> specializeRecord(std::size_t declaration, GenericBindings bindings,
                                            SourceSpan site) {
  const auto original = syntax_.records[declaration];
  std::vector<ValueType> arguments;
  std::string key = std::to_string(declaration), label = name(original.name) + "[";
  for (const auto &parameter : original.generics) {
    const auto found = bindings.find(name(parameter.name));
    if (found == bindings.end()) {
      error("S082",
            "cannot infer nominal type parameter '" + name(parameter.name) +
                "'; supply explicit arguments or an expected type",
            site);
      return {};
    }
    auto value = genericValueType(found->second);
    arguments.push_back(value);
    key += "[" + genericTypeKey(value) + "]";
    if (arguments.size() > 1)
      label += ", ";
    label += describeType(value);
  }
  label += "]";
  if (auto found = recordSpecializations_.find(key); found != recordSpecializations_.end())
    return found->second;
  if (!checkGenericBounds(original.generics, bindings, site))
    return {};
  if (++specializationCount_ > limits_.maxSpecializations) {
    error("S082", "generic nominal specialization budget exceeded", site);
    return {};
  }
  const auto id = output_.records.size();
  TypedRecord header{label, original.name, {}};
  output_.records.push_back(std::move(header));
  recordDeclarations_.resize(id);
  recordStates_.resize(id, State::Complete);
  recordDeclarations_.push_back(declaration);
  recordStates_.push_back(State::Unseen);
  recordBindings_[id] = std::make_shared<const GenericBindings>(std::move(bindings));
  recordSpecializations_[std::move(key)] = id;
  genericRecordInstances_[id] = GenericRecordInstance{declaration, std::move(arguments)};
  // Predeclare recursive nominal identities before resolving their fields, just
  // as ordinary source declarations do during initial registration.
  const auto registering = registeringRecords_;
  registeringRecords_ = true;
  const bool populated = populateRecord(id, site);
  registeringRecords_ = registering;
  if (!populated)
    return {};
  if (!registering) {
    auto storage = planRecordStorage(output_);
    if (!storage) {
      error(storage.error().code, storage.error().message, storage.error().span);
      return {};
    }
    recursiveValues_ |= hasRecursiveStorage(*storage);
    if (auto invalid = recursiveRecordLayoutError(output_, limits_)) {
      error(invalid->code, invalid->message, invalid->span);
      return {};
    }
  }
  return id;
}
std::optional<ValueType> genericNominalType(const TypeSyntax &syntax, std::size_t declaration) {
  const auto original = syntax_.records[declaration];
  if (syntax.arguments.size() != original.generics.size()) {
    error("S082",
          "generic type '" + name(original.name) + "' requires " +
              std::to_string(original.generics.size()) + " type arguments",
          syntax.span);
    return {};
  }
  GenericBindings bindings;
  for (std::size_t i = 0; i < syntax.arguments.size(); ++i) {
    auto resolved = type(syntax.arguments[i]);
    if (!resolved)
      return {};
    bindings.emplace(name(original.generics[i].name), genericValueType(*resolved));
  }
  if (!checkGenericTypeApplication(original.generics, bindings, syntax.arguments, syntax.span))
    return {};
  auto record = specializeRecord(declaration, std::move(bindings), syntax.span);
  if (!record)
    return {};
  auto result = recordType(*record);
  if (syntax.array) {
    result.array = result.tensor = true;
    result.shape = {dynamic()};
  }
  return result;
}
std::optional<ValueId> genericRecordValue(Frame &frame, SourceSpan span, const RecordExpr &syntax,
                                          const std::optional<ValueType> &expected,
                                          std::size_t declaration) {
  const auto original = syntax_.records[declaration];
  GenericBindings bindings;
  if (!syntax.typeArguments.empty()) {
    if (syntax.typeArguments.size() != original.generics.size()) {
      error("S082", "wrong number of nominal type arguments", span);
      return {};
    }
    for (std::size_t i = 0; i < syntax.typeArguments.size(); ++i) {
      auto resolved = type(syntax.typeArguments[i]);
      if (!resolved)
        return {};
      bindings.emplace(name(original.generics[i].name), genericValueType(*resolved));
    }
  }
  if (expected && expected->record)
    if (auto instance = genericRecordInstances_.find(*expected->record);
        instance != genericRecordInstances_.end() && instance->second.declaration == declaration)
      for (std::size_t i = 0; i < original.generics.size(); ++i)
        bindings.try_emplace(name(original.generics[i].name), instance->second.arguments[i]);
  GenericTermBindings terms;
  if (genericContext(frame)) {
    for (std::size_t i = 0; i < syntax.typeArguments.size(); ++i)
      terms[name(original.generics[i].name)] = sourceGenericTerm(
          frame, syntax.typeArguments[i], bindings.at(name(original.generics[i].name)));
    if (expected && genericExpectedTerm_ &&
        genericTypeKey(*expected) == genericTypeKey(genericExpectedTerm_->actual) &&
        genericExpectedTerm_->arguments.size() == original.generics.size())
      for (std::size_t i = 0; i < original.generics.size(); ++i)
        terms.try_emplace(name(original.generics[i].name), genericExpectedTerm_->arguments[i]);
  }
  std::vector<Parameter> fields;
  if (syntax.family) {
    const auto variant = std::ranges::find_if(original.variants, [&](const auto &variant) {
      return name(variant.name) == name(syntax.name);
    });
    if (variant == original.variants.end()) {
      error("S071", "unknown generic enum variant", syntax.name);
      return {};
    }
    fields = variant->fields;
  } else {
    if (!original.variants.empty()) {
      error("S071", "generic enum construction requires Type.Variant", span);
      return {};
    }
    fields = original.fields;
  }
  if (syntax.fields.size() != fields.size()) {
    error("S068", "generic construction requires every field exactly once", span);
    return {};
  }
  // These values are evaluated once, in source order, before concrete assembly.
  std::vector<std::pair<std::string, ValueId>> values;
  std::unordered_set<std::string> seen;
  for (const auto &initializer : syntax.fields) {
    if (!resolveKnownGenericBounds(original.generics, bindings, initializer.name))
      return {};
    if (completing(initializer.name)) {
      std::vector<std::string> names;
      for (const auto &field : fields)
        names.push_back(name(field.name));
      completeLabels(initializer.name, syntax.fields, std::move(names));
    }
    const auto label = name(initializer.name);
    const auto formal =
        std::ranges::find_if(fields, [&](const auto &p) { return name(p.name) == label; });
    if (formal == fields.end() || !seen.insert(label).second) {
      error("S068", "unknown or duplicate generic constructor field", initializer.name);
      return {};
    }
    std::optional<ValueType> context;
    if (genericTypeKnown(formal->type, original.generics, bindings)) {
      auto owner = std::make_shared<GenericProofOwner>();
      owner->parameters = original.generics;
      GenericScope scope(*this, std::make_shared<const GenericBindings>(bindings),
                         std::move(owner));
      context = type(formal->type);
      if (!context)
        return {};
    }
    GenericExpectedScope expectedField(
        *this, context && genericContext(frame)
                   ? std::optional{instantiateGenericTerm(formal->type, *context, terms)}
                   : std::nullopt);
    auto value = expression(frame, initializer.value, context);
    if (!value || !inferGenericType(formal->type, node(*value).type, original.generics, bindings,
                                    initializer.name))
      return {};
    values.emplace_back(label, *value);
    if (genericContext(frame))
      inferGenericTermBindings(formal->type, genericTerm(*value), original.generics, terms);
  }
  auto record = specializeRecord(declaration, std::move(bindings), span);
  if (!record)
    return {};
  const auto concrete = output_.records[*record];
  std::vector<RecordFieldValue> assembled;
  for (const auto &[label, value] : values) {
    const auto field =
        std::ranges::find_if(concrete.fields, [&](const auto &f) { return f.name == label; });
    if (field == concrete.fields.end()) {
      error("S082", "generic constructor field has incompatible type", span);
      return {};
    }
    auto checked = conform(frame, value, field->type, span);
    if (!checked)
      return {};
    const auto initializer =
        std::ranges::find_if(syntax.fields, [&](const auto &f) { return name(f.name) == label; });
    rememberSourceToken({initializer->name, field->declaration, field->type, {}});
    assembled.push_back({static_cast<std::size_t>(field - concrete.fields.begin()), *checked});
  }
  std::optional<std::size_t> variant;
  if (syntax.family) {
    const auto found = std::ranges::find_if(
        concrete.variants, [&](const auto &v) { return v.name == name(syntax.name); });
    variant = static_cast<std::size_t>(found - concrete.variants.begin());
  }
  rememberTypeUse(syntax.family.value_or(syntax.name), recordType(*record));
  auto result =
      add(frame, recordType(*record), span, RecordValue{{}, std::move(assembled), variant});
  if (result && genericContext(frame)) {
    if (!checkGenericTermBounds(frame, original.generics, terms, span))
      return {};
    auto term = concreteGenericTerm(recordType(*record));
    for (std::size_t i = 0; i < original.generics.size(); ++i)
      if (auto found = terms.find(name(original.generics[i].name)); found != terms.end())
        term.arguments[i] = found->second;
    genericValueTerms_[*result] = std::move(term);
  }
  return result;
}

bool sameGenericFamily(const Frame &frame, SourceSpan family, std::size_t record) const {
  const auto declaration = genericRecords_.find(functionKey(frame, name(family)));
  const auto instance = genericRecordInstances_.find(record);
  return declaration != genericRecords_.end() && instance != genericRecordInstances_.end() &&
         declaration->second == instance->second.declaration;
}
