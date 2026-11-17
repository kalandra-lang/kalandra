// Private members of Checker. These forms create explicit cooperative IR;
// ordinary calls and structurally similar user records never gain the capability.
bool standardWorkgroupDispatch(FunctionId id) const {
  if (id >= syntax_.functions.size())
    return false;
  const auto &function = syntax_.functions[id];
  const auto origin = syntax_.source->partIndex(function.name.begin);
  return origin < syntax_.standardModules.size() &&
         syntax_.standardModules[origin] == StandardModule::Gpu &&
         syntax_.source->slice(function.name) == "dispatch";
}
struct WorkgroupMethodSyntax {
  ExprId receiver;
  SourceSpan member;
  std::vector<TypeSyntax> types;
};
std::optional<WorkgroupMethodSyntax> workgroupMethodSyntax(Frame &frame, const CallExpr &call) {
  auto callee = call.callee;
  std::vector<TypeSyntax> types;
  if (const auto *indexed = std::get_if<IndexExpr>(&syntax_.expressions[callee].value)) {
    callee = indexed->input;
    types = indexed->typeArguments;
  }
  const auto *field = std::get_if<FieldExpr>(&syntax_.expressions[callee].value);
  if (!field)
    return {};
  const auto *receiver = std::get_if<NameExpr>(&syntax_.expressions[field->input].value);
  if (!receiver)
    return {};
  const auto symbol = lookupSymbol(frame, receiver->name);
  if (!symbol || !plainWorkgroupCapability(output_, output_.symbols[*symbol].type,
                                           StandardRecord::WorkItem))
    return {};
  return WorkgroupMethodSyntax{field->input, field->field, std::move(types)};
}
std::optional<ValueId> workgroupMethodCall(Frame &frame, SourceSpan span,
                                           const WorkgroupMethodSyntax &method,
                                           const std::vector<ExprId> &arguments) {
  const auto member = name(method.member);
  const bool shared = member == "shared", barrier = member == "barrier";
  if (!shared && !barrier && member != "globalIndex" && member != "localIndex") {
    error("S087", "unknown WorkItem operation '" + member + "'", method.member);
    return {};
  }
  if (arguments.size() != (barrier ? 0u : 1u) || method.types.size() != (shared ? 1u : 0u)) {
    error("S087", "WorkItem operation has the wrong value or type argument count", span);
    return {};
  }
  auto receiver = expression(frame, method.receiver);
  if (!receiver)
    return {};
  WorkgroupOperation operation{{barrier ? WorkgroupAction::Barrier
                                      : shared ? WorkgroupAction::Shared
                                      : member == "globalIndex" ? WorkgroupAction::GlobalIndex
                                                                : WorkgroupAction::LocalIndex},
                               {*receiver}};
  ValueType result;
  result.element = NumericType::Int64;
  if (barrier)
    result.builtin = BuiltinValue::Unit;
  else {
    auto argument = expression(frame, arguments.front(), result);
    if (!argument)
      return {};
    operation.arguments.push_back(*argument);
    if (shared) {
      auto element = type(method.types.front());
      if (!element || element->builtin != BuiltinValue::Numeric || element->tensor ||
          element->array || element->task || element->fallible || element->device ||
          !describeNumeric(element->element)) {
        error("S087", "shared scratch requires a numeric primitive element type", span);
        return {};
      }
      const auto count = intConstants_.find(*argument);
      if (count == intConstants_.end() || count->second <= 0 || count->second > 1000000) {
        error("S087", "shared scratch size must be a positive constant within the proof budget",
              span);
        return {};
      }
      operation.metadata.count = static_cast<std::uint64_t>(count->second);
      operation.metadata.element = element->element;
      std::optional<std::size_t> declaration;
      for (std::size_t id = 0; id < syntax_.records.size(); ++id) {
        const auto &record = syntax_.records[id];
        const auto origin = syntax_.source->partIndex(record.name.begin);
        if (origin < syntax_.standardModules.size() &&
            syntax_.standardModules[origin] == StandardModule::Gpu &&
            syntax_.source->slice(record.name) == "Shared") {
          declaration = id;
          break;
        }
      }
      if (!declaration) {
        error("S087", "missing canonical std/gpu scratch declaration", span);
        return {};
      }
      const auto &parameter = syntax_.records[*declaration].generics.front();
      auto record = specializeRecord(*declaration, {{name(parameter.name), *element}}, span);
      if (!record)
        return {};
      result = recordType(*record);
    }
  }
  return add(frame, result, span, std::move(operation));
}
std::optional<ValueId> workgroupDispatch(Frame &frame, SourceSpan span,
                                         const std::vector<ExprId> &arguments) {
  if (arguments.size() != 3) {
    error("S087", "dispatch requires output shape, group shape and a WorkItem callback", span);
    return {};
  }
  ValueType extents;
  extents.element = NumericType::Int64;
  extents.tensor = extents.array = true;
  extents.shape = {{{}, 0}};
  auto shape = expression(frame, arguments[0], extents);
  auto group = expression(frame, arguments[1], extents);
  if (!shape || !group)
    return {};
  const auto shapeType = node(*shape).type;
  const auto groupType = node(*group).type;
  if (shapeType.shape.size() != 1 || !shapeType.shape.front().constant ||
      *shapeType.shape.front().constant > 8 || groupType.shape.size() != 1 ||
      groupType.shape.front().constant != shapeType.shape.front().constant) {
    error("S087", "dispatch shapes require equal statically known lengths from zero to eight", span);
    return {};
  }
  const auto *literal = std::get_if<TensorLiteral>(&node(*group).operation);
  if (!literal) {
    error("S087", "dispatch group shape requires constant integer extents", span);
    return {};
  }
  std::vector<std::uint32_t> groupShape;
  for (auto value : literal->elements) {
    const auto known = intConstants_.find(value);
    if (known == intConstants_.end() || known->second <= 0 || known->second > UINT32_MAX) {
      error("S087", "dispatch group extents must be positive constant integers", node(value).span);
      return {};
    }
    groupShape.push_back(static_cast<std::uint32_t>(known->second));
  }
  auto callback = expression(frame, arguments[2]);
  if (!callback)
    return {};
  const auto signature = node(*callback).type.callable;
  if (!signature || signature->parameters.size() != 1 ||
      !plainWorkgroupCapability(output_, signature->parameters.front(), StandardRecord::WorkItem) ||
      signature->owns(0) || signature->mutates(0)) {
    error("S087", "dispatch callback must take one borrowed WorkItem parameter", span);
    return {};
  }
  auto result = signature->result;
  if (result.builtin != BuiltinValue::Numeric || result.tensor || result.array || result.task ||
      result.device || !describeNumeric(result.element)) {
    error("S087", "dispatch callback must return a numeric primitive element", span);
    return {};
  }
  result.tensor = true;
  result.shape.clear();
  for (std::uint64_t axis = 0; axis < *shapeType.shape.front().constant; ++axis)
    result.shape.push_back({{}, nextExtent_++});
  WorkgroupOperation operation{{WorkgroupAction::Dispatch, result.element},
                               {*shape, *group, *callback}, std::move(groupShape)};
  return add(frame, std::move(result), span, std::move(operation));
}
std::optional<ValueId> workgroupIndex(Frame &frame, SourceSpan span, ValueId input,
                                      ExprId index, std::optional<ExprId> stored = {}) {
  ValueType integer;
  integer.element = NumericType::Int64;
  auto position = expression(frame, index, integer);
  if (!position)
    return {};
  const auto &record = output_.records[*node(input).type.record];
  if (record.typeArguments.size() != 1 ||
      record.typeArguments.front().builtin != BuiltinValue::Numeric) {
    error("S087", "shared scratch requires its canonical numeric element type", span);
    return {};
  }
  const auto element = record.typeArguments.front();
  WorkgroupOperation operation{{stored ? WorkgroupAction::Write : WorkgroupAction::Read,
                                element.element},
                               {input, *position}};
  if (stored) {
    auto value = expression(frame, *stored, element);
    if (!value)
      return {};
    operation.arguments.push_back(*value);
  }
  auto result = element;
  if (stored)
    result.builtin = BuiltinValue::Unit;
  return add(frame, result, span, std::move(operation));
}
