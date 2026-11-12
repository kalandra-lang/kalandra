bool standardRange(FunctionId id) const {
  if (id >= syntax_.functions.size())
    return false;
  const auto &function = syntax_.functions[id];
  return function.moduleIndex < syntax_.standardModules.size() &&
         syntax_.standardModules[function.moduleIndex] == StandardModule::Iteration &&
         name(function.name) == "range";
}
std::optional<IteratorAction> standardIterator(FunctionId id) const {
  if (id >= syntax_.functions.size())
    return {};
  const auto &function = syntax_.functions[id];
  if (function.moduleIndex >= syntax_.standardModules.size() ||
      syntax_.standardModules[function.moduleIndex] != StandardModule::Iteration)
    return {};
  const auto label = syntax_.source->slice(function.name);
  if (label == "iterator") {
    if (id < output_.functions.size() && !output_.functions[id].parameters.empty() &&
        output_.symbols[output_.functions[id].parameters[0]].type.array)
      return IteratorAction::FromArray;
    return IteratorAction::FromRange;
  }
  if (label == "next")
    return IteratorAction::Next;
  if (label == "close")
    return IteratorAction::Close;
  return {};
}
std::optional<ValueId> iteratorBody(Frame &frame, FunctionId id, IteratorAction action) {
  const auto &fn = output_.functions[id];
  std::vector<ValueId> arguments;
  for (auto parameter : fn.parameters) {
    auto value = add(frame, output_.symbols[parameter].type, fn.declaration, SymbolRead{parameter});
    if (!value)
      return {};
    arguments.push_back(*value);
  }
  return add(frame, *fn.declaredResult, fn.declaration,
             IteratorOperation{action, std::move(arguments), fn.declaration});
}
std::optional<ValueId> rangeSignature(Frame &frame, FunctionId id, SourceSpan span,
                                       const std::vector<ExprId> &arguments) {
  if (!analyzeFunction(id))
    return {};
  ValueType integer;
  integer.element = NumericType::Int64;
  const auto count = std::clamp<std::size_t>(arguments.size(), 1, 3);
  FunctionSignature signature;
  signature.parameters.assign(count, integer);
  signature.result = output_.functions[id].resultType;
  std::vector<std::string> names = count == 1 ? std::vector<std::string>{"stop"}
                                             : std::vector<std::string>{"start", "stop"};
  if (count == 3)
    names.push_back("step");
  const auto prefix = options_.signature->argumentIndex.value_or(arguments.size());
  for (std::size_t i = 0; i < std::min(prefix, arguments.size()); ++i)
    if (!expression(frame, arguments[i], integer))
      return {};
  captureSignature(frame, signature, std::move(names));
  error("S027", "editing range argument requires a signature snapshot", span);
  return {};
}
std::optional<ValueId> rangeBody(Frame &frame, FunctionId id) {
  const auto &fn = output_.functions[id];
  std::vector<ValueId> inputs;
  for (auto parameter : fn.parameters) {
    auto value = add(frame, output_.symbols[parameter].type, fn.declaration, SymbolRead{parameter});
    if (!value)
      return {};
    inputs.push_back(*value);
  }
  auto start = add(frame, node(inputs[0]).type, fn.declaration,
                   IntegerRangeOperation{IntegerRangeOperation::Kind::Validate, inputs[0],
                                         inputs[1], inputs[2]});
  if (!start)
    return {};
  return add(frame, *fn.declaredResult, fn.declaration,
              RecordValue{{}, {{0, *start}, {1, inputs[1]}, {2, inputs[2]}}});
}
std::optional<std::vector<ValueId>> rangeArguments(Frame &frame, SourceSpan span,
                                                   const std::vector<ExprId> &arguments) {
  if (arguments.empty() || arguments.size() > 3) {
    error("S076", "range expects stop, start/stop, or start/stop/step", span);
    return {};
  }
  ValueType integer;
  integer.element = NumericType::Int64;
  std::vector<ValueId> checked;
  for (auto argument : arguments) {
    auto value = expression(frame, argument, integer);
    if (!value || !sameFunctionValueType(node(*value).type, integer)) {
      if (value)
        error("S076", "range arguments require completed i64 scalars", node(*value).span);
      return {};
    }
    checked.push_back(*value);
  }
  auto zero = add(frame, integer, span, Int64Literal{0});
  auto one = add(frame, integer, span, Int64Literal{1});
  if (!zero || !one)
    return {};
  return std::vector<ValueId>{checked.size() == 1 ? *zero : checked[0],
                              checked.size() == 1 ? checked[0] : checked[1],
                              checked.size() == 3 ? checked[2] : *one};
}
