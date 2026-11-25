// Arithmetic capabilities are identified by their compiler-owned module, never
// by a user-defined trait with the same spelling. Builtin IR remains unchanged.
std::optional<std::size_t> arithmeticTrait(std::string_view label) const {
  for (std::size_t id = 0; id < syntax_.traits.size(); ++id) {
    const auto &trait = syntax_.traits[id];
    const auto module = syntax_.source->partIndex(trait.name.begin);
    if (module < syntax_.standardModules.size() &&
        syntax_.standardModules[module] == StandardModule::Arithmetic && name(trait.name) == label)
      return id;
  }
  return {};
}
bool builtinArithmeticConformance(std::size_t trait, const ValueType &self,
                                  const std::vector<ValueType> &arguments, SourceSpan site,
                                  TraitConformance &conformance) {
  const auto label = name(syntax_.traits[trait].name);
  if (arithmeticTrait(label) != trait)
    return false;
  const auto completed = [](const ValueType &t) {
    return !t.array && !t.task && !t.fallible && !t.device;
  };
  if (!completed(self))
    return false;
  const bool unary = label == "Neg";
  if (arguments.size() != (unary ? 0 : 1))
    return false;
  const auto right = unary ? self : arguments[0];
  if (!completed(right))
    return false;
  const bool text = label == "Add" && self.builtin == BuiltinValue::String &&
                    right.builtin == BuiltinValue::String && !self.tensor && !right.tensor;
  const bool numeric = self.builtin == BuiltinValue::Numeric &&
                       right.builtin == BuiltinValue::Numeric && self.element == right.element &&
                       (label != "Rem" || isInteger(self.element));
  if (!text && !numeric)
    return false;
  auto result = self.tensor ? self : right;
  if (self.tensor && right.tensor) {
    if (self.shape.size() != right.shape.size())
      return false;
    for (std::size_t n = 0; n < self.shape.size(); ++n) {
      if (self.shape[n].constant && right.shape[n].constant &&
          self.shape[n].constant != right.shape[n].constant)
        return false;
      if (right.shape[n].constant)
        result.shape[n] = right.shape[n];
    }
  }
  conformance.bindings = {{"Self", self}, {"Self.Output", result}};
  if (!unary)
    conformance.bindings["Rhs"] = right;
  conformance.builtinArithmetic = true;
  if (genericRegistryReady_) {
    const auto method = label == "Add"   ? "add"
                        : label == "Sub" ? "sub"
                        : label == "Mul" ? "mul"
                        : label == "Div" ? "div"
                        : label == "Rem" ? "rem"
                                         : "neg";
    const auto module = syntax_.source->partIndex(syntax_.traits[trait].name.begin);
    auto helper = functions_.find(functionKey(module, std::string("__") + method));
    if (helper == functions_.end()) {
      error("S083", "missing canonical arithmetic implementation", site);
      conformance.state = State::Failed;
      return true;
    }
    GenericBindings bindings =
        unary ? GenericBindings{{"T", self}} : GenericBindings{{"L", self}, {"R", right}};
    auto function = specializeFunction(helper->second, std::move(bindings), site);
    if (!function || !analyzeFunction(*function)) {
      conformance.state = State::Failed;
      return true;
    }
    conformance.bindings["Self.Output"] = output_.functions[*function].resultType;
    conformance.methods[method] = *function;
    conformance.signaturesChecked = true;
  }
  conformance.state = State::Complete;
  return true;
}
std::optional<ValueId> invokeArithmetic(Frame &frame, SourceSpan span, TokenKind operation,
                                        const std::vector<ExprId> &arguments,
                                        std::vector<ValueId> operands,
                                        const std::optional<ValueType> &expected) {
  const bool unary = operands.size() == 1;
  const auto label = unary                           ? "Neg"
                     : operation == TokenKind::Plus  ? "Add"
                     : operation == TokenKind::Minus ? "Sub"
                     : operation == TokenKind::Star  ? "Mul"
                     : operation == TokenKind::Slash ? "Div"
                                                     : "Rem";
  auto trait = arithmeticTrait(label);
  if (!trait) {
    error("S017",
          "arithmetic requires compatible numeric operands or an implementation from std/ops",
          span);
    return {};
  }
  const auto self = genericValueType(node(operands[0]).type);
  std::vector<ValueType> types;
  std::vector<GenericTerm> terms;
  if (!unary) {
    types.push_back(genericValueType(node(operands[1]).type));
    terms.push_back(genericTerm(operands[1]));
  }
  auto conformance = requireConformance(*trait, self, span, types);
  if (!conformance ||
      !requireGenericTraitProof(frame, *trait, genericTerm(operands[0]), span, terms))
    return {};
  const auto requirement = syntax_.traits[*trait].methods.front();
  const auto method = conformance->methods.find(name(requirement.name));
  if (method == conformance->methods.end())
    return {};
  auto result = invoke(frame, span, requirement.name, arguments, expected, {}, nullptr, false,
                       method->second, operands);
  if (result && genericContext(frame))
    genericValueTerms_[*result] =
        projectedGenericTerm(*trait, genericTerm(operands[0]), terms, "Output", node(*result).type);
  return result;
}
