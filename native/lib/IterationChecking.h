// Resolve the canonical standard Option without depending on a user import
// alias. Iterator declarations already load std/option as a module dependency.
template <class Loop>
std::optional<ValueId> generatorLoop(Frame &frame, SourceSpan site, const Loop &source,
                                     const std::optional<ValueType> &expected,
                                     const std::optional<ValueType> &materializedItem = {}) {
  if (expected && !isIterator(output_, *expected)) {
    error("S076", "a yielding loop produces an Iterator; use array(...) to materialize it", site);
    return {};
  }
  if (!source.whenDone.empty()) {
    error("S076", "a yielding loop ends at exhaustion and cannot have an else value", site);
    return {};
  }
  const auto id = output_.functions.size();
  output_.functions.emplace_back();
  requirements_.resize(output_.functions.size());
  output_.functions[id].name = "__generator_" + std::to_string(id);
  output_.functions[id].declaration = site;
  output_.functions[id].anonymous = true;
  Frame producer{id, {}, frame.depth, std::make_shared<TaskChecks>(), {}, {}, {}, 0, false};
  producer.closure = std::make_shared<ClosureScope>(ClosureScope{&frame, {}, {}, true});
  producer.collection = std::make_shared<CollectionScope>();
  producer.collection->identity = nextLoop_++;
  producer.collection->lazy = true;
  if (expected)
    producer.collection->element = output_.records[*expected->record].typeArguments.at(0);
  else if (materializedItem)
    producer.collection->element = *materializedItem;
  producer.handlerFallible = std::make_shared<bool>(false);
  producer.handlerErrorRecord = std::make_shared<std::optional<std::size_t>>();
  GenericErrorScope errors(*this, producer.handlerErrorRecord.get());
  auto loop = source;
  loop.collecting = false;
  loop.statementOnly = true;
  prepareBindings(producer, loop.body);
  auto body = infer(producer, site, loop, {});
  if (!body)
    return {};
  if (node(*body).type.fallible) {
    const auto traversal = node(*body).type;
    producer.tasks->handled.insert(traversal.identity);
    if (!propagateError(producer, traversal.errorRecord, site, *body))
      return {};
    auto completed = traversal;
    completed.fallible = false;
    completed.errorRecord.reset();
    completed.identity = 0;
    body = add(producer, completed, site, TaskOperation{TaskPrefix::Try, *body});
    if (!body)
      return {};
  }
  const auto collection = producer.collection;
  if (!collection->element || collection->values.empty()) {
    error("S076", "a yielding loop requires a yield to determine its item type", site);
    return {};
  }
  const auto item = *collection->element;
  const auto option = iteratorOption(item, site);
  if (!option)
    return {};
  auto resultType = recordType(*option);
  resultType.fallible = *producer.handlerFallible;
  resultType.errorRecord = resultType.fallible ? *producer.handlerErrorRecord : std::nullopt;
  auto completed = resultType;
  completed.fallible = false;
  completed.errorRecord.reset();
  const auto terminal = add(producer, completed, site, RecordValue{{}, {}, 0});
  if (!terminal)
    return {};
  if (node(*terminal).type.fallible)
    producer.tasks->handled.insert(node(*terminal).type.identity);
  for (auto pending : producer.tasks->pending)
    if (!producer.tasks->handled.contains(pending)) {
      error("S036", "a generator must handle or propagate each expected error", site);
      return {};
    }
  auto &function = output_.functions[id];
  function.body = {{site, {}, *body}, {site, {}, *terminal}};
  function.result = *terminal;
  function.resultType = resultType;
  function.declaredResult = resultType;
  function.captureCount = producer.closure->origins.size();
  function.generator = TypedFunction::Generator{item, collection->identity};
  ValueType errorType;
  if (resultType.fallible && !resultType.errorRecord)
    errorType.builtin = BuiltinValue::TaskError;
  else
    errorType = recordType(resultType.errorRecord ? *resultType.errorRecord : neverFamily());
  std::optional<std::size_t> cursor;
  for (std::size_t record = 0; record < syntax_.records.size(); ++record) {
    const auto declaration = syntax_.records[record];
    const auto origin = syntax_.source->partIndex(declaration.name.begin);
    if (origin < syntax_.standardModules.size() &&
        syntax_.standardModules[origin] == StandardModule::Iteration &&
        name(declaration.name) == "Iterator" && declaration.generics.size() == 2) {
      cursor = specializeRecord(record,
                                {{name(declaration.generics[0].name), item},
                                 {name(declaration.generics[1].name), errorType}},
                                site);
      break;
    }
  }
  if (!cursor) {
    error("S076", "a yielding loop requires its standard Iterator module", site);
    return {};
  }
  std::vector<ValueId> captures;
  for (auto origin : producer.closure->origins) {
    auto capture = add(frame, output_.symbols[origin].type, site, SymbolRead{origin});
    if (!capture)
      return {};
    captures.push_back(*capture);
  }
  auto created = add(frame, recordType(*cursor), site, GeneratorCreate{id, std::move(captures)});
  if (created && genericContext(frame))
    if (auto propagated = genericErrorScopes_.find(producer.handlerErrorRecord.get());
        propagated != genericErrorScopes_.end()) {
      auto term = genericTerm(*created);
      if (term.arguments.size() == 2) {
        auto error = propagated->second;
        error.actual = errorType;
        term.arguments[1] = std::move(error);
        genericValueTerms_[*created] = finishGenericTerm(std::move(term));
      }
    }
  return created;
}
// An immediately materialized literal does not escape as a cursor. Keep its
// private capture bindings and error boundary, but collect in that lifted
// function directly. Besides avoiding per-item cursor dispatch, this gives
// device-local collection a synchronous implementation without a CPU cursor.
std::optional<ValueId> materializeFreshGenerator(Frame &frame, ValueId created, SourceSpan site) {
  const auto construction = std::get<GeneratorCreate>(node(created).operation);
  auto &function = output_.functions.at(construction.function);
  const auto generator = *function.generator;
  if (generator.item.array || generator.item.tensor) {
    error("S076", "array materialization cannot nest arrays or tensors; wrap each item in a record",
          site);
    return {};
  }
  const auto cursorTerm = genericTerm(created);
  auto result = generator.item;
  result.array = result.tensor = true;
  result.shape = {dynamic()};
  result.fallible = function.resultType.fallible;
  result.errorRecord = function.resultType.errorRecord;
  result.emptyErrorChannel = !result.fallible;
  result.identity = 0;
  const auto body = function.body.front().value;
  auto root = body;
  if (const auto *operation = std::get_if<TaskOperation>(&node(root).operation);
      operation && operation->prefix == TaskPrefix::Try)
    root = operation->operand;
  auto &loop = std::get<WhileOperation>(output_.expressions[root].operation);
  loop.collecting = true;
  loop.exitValues.clear();
  const auto collection = loop.identity;
  for (auto value : function.expressions)
    if (auto *yield = std::get_if<YieldOperation>(&output_.expressions[value].operation);
        yield && yield->collection == generator.collection) {
      yield->collection = collection;
      loop.exitValues.push_back(yield->operand);
    }
  const auto rootError = node(root).type;
  output_.expressions[root].type = result;
  output_.expressions[root].type.fallible = rootError.fallible;
  output_.expressions[root].type.errorRecord = rootError.errorRecord;
  output_.expressions[root].type.identity = rootError.identity;
  if (body != root) {
    output_.expressions[body].type = result;
    output_.expressions[body].type.fallible = false;
    output_.expressions[body].type.errorRecord.reset();
  }
  function.body = {{site, {}, body}};
  function.result = body;
  function.resultType = result;
  function.declaredResult = result;
  function.captureCount = 0;
  function.generator.reset();
  function.publicationCollection = collection;
  function.name = "__collect_" + std::to_string(construction.function);
  output_.expressions[created].type = result;
  output_.expressions[created].operation =
      FunctionCall{construction.function, construction.captures};
  if (result.fallible) {
    output_.expressions[created].type.identity = created + 1;
    frame.tasks->pending.insert(created + 1);
  }
  if (genericContext(frame)) {
    auto term = concreteGenericTerm(result);
    if (cursorTerm.arguments.size() == 2) {
      term.arguments = {cursorTerm.arguments[0]};
      if (result.fallible)
        term.failure = std::make_shared<const GenericTerm>(cursorTerm.arguments[1]);
    }
    genericValueTerms_[created] = finishGenericTerm(std::move(term));
  }
  return created;
}
void traversalGenericError(const Frame &frame, ValueId result, ValueId advanced) {
  if (!genericContext(frame) || !node(result).type.fallible)
    return;
  if (auto failure = genericTerm(advanced).failure) {
    auto term = genericTerm(result);
    term.failure = std::move(failure);
    genericValueTerms_[result] = finishGenericTerm(std::move(term));
  }
}
std::optional<std::size_t> iteratorOption(const ValueType &item, SourceSpan site) {
  for (std::size_t id = 0; id < syntax_.records.size(); ++id) {
    const auto &record = syntax_.records[id];
    const auto origin = syntax_.source->partIndex(record.name.begin);
    if (origin < syntax_.standardModules.size() &&
        syntax_.standardModules[origin] == StandardModule::Option &&
        name(record.name) == "Option" && record.generics.size() == 1)
      return specializeRecord(id, {{name(record.generics[0].name), item}}, site);
  }
  error("S076", "Iterator traversal requires its standard Option module", site);
  return {};
}
std::optional<ValueId> materializeIterator(Frame &frame, SourceSpan site, ValueId input) {
  auto type = node(input).type;
  if (isIterator(output_, type) && genericContext(frame) && !genericTerm(input).variables.empty()) {
    error("S083", "array requires an explicit Iterator[T, E] or an Iterable bound", site);
    return {};
  }
  if (!isIterator(output_, type)) {
    auto cursor = iterableCursor(frame, input, site);
    if (!cursor)
      return {};
    input = *cursor;
    type = node(input).type;
  }
  if (type.task || type.fallible || type.device || !isIterator(output_, type)) {
    error("S076", "array materialization requires a completed CPU iterable source", site);
    return {};
  }
  const auto arguments = output_.records[*type.record].typeArguments;
  const auto itemType = arguments.at(0);
  if (itemType.array || itemType.tensor) {
    error("S076", "array materialization cannot nest arrays or tensors; wrap each item in a record",
          site);
    return {};
  }
  const auto option = iteratorOption(itemType, site);
  if (!option)
    return {};
  auto nextType = recordType(*option);
  const auto failure = arguments[1];
  const bool fallible = !failure.record || !output_.records[*failure.record].uninhabited;
  nextType.fallible = fallible;
  nextType.emptyErrorChannel = !fallible;
  if (fallible && failure.builtin != BuiltinValue::TaskError &&
      !(failure.record && output_.records[*failure.record].taskErrorFamily))
    nextType.errorRecord = failure.record;
  const auto symbol = output_.symbols.size();
  output_.symbols.push_back(
      {"__materialize_" + std::to_string(symbol), type, site, frame.function, false});
  recordGenericBinding(frame, symbol, input);
  auto cursor = add(frame, type, site, SymbolRead{symbol});
  if (!cursor)
    return {};
  auto advanced =
      add(frame, nextType, site, IteratorOperation{IteratorAction::Next, {*cursor}, site});
  if (!advanced)
    return {};
  const auto traversal = *advanced;
  if (fallible) {
    frame.tasks->handled.insert(node(*advanced).type.identity);
    auto completed = nextType;
    completed.fallible = false;
    completed.errorRecord.reset();
    advanced = add(frame, completed, site, TaskOperation{TaskPrefix::Try, *advanced});
    if (!advanced)
      return {};
  }
  ValueType marker;
  marker.builtin = BuiltinValue::Bool;
  const auto no = add(frame, marker, site, BoolLiteral{false});
  const auto yes = add(frame, marker, site, BoolLiteral{true});
  if (!no || !yes)
    return {};
  const auto test =
      add(frame, marker, site,
          MatchOperation{*advanced, {{1, {{site, {}, *no}}, *no}, {2, {{site, {}, *yes}}, *yes}}});
  const auto item = add(frame, itemType, site,
                        RecordFieldRead{*advanced, output_.records[*option].variants[1].fields[0]});
  if (!test || !item)
    return {};
  const auto identity = nextLoop_++;
  const auto yield = add(frame, marker, site, YieldOperation{identity, *item});
  if (!yield)
    return {};
  auto result = itemType;
  result.array = result.tensor = true;
  result.shape = {dynamic()};
  result.fallible = fallible;
  result.errorRecord = nextType.errorRecord;
  result.emptyErrorChannel = !fallible;
  result.identity = 0;
  auto materialized = add(frame, result, site,
                          WhileOperation{*test,
                                         {{site, {}, *yield}},
                                         {},
                                         {},
                                         identity,
                                         false,
                                         {{site, symbol, input}},
                                         {},
                                         {*item},
                                         true,
                                         false,
                                         fallible});
  if (materialized)
    traversalGenericError(frame, *materialized, traversal);
  return materialized;
}
// Lower a reusable user source through the canonical static Iterable contract.
// Reuse the already checked input so construction effects run exactly once.
std::optional<ValueId> iterableCursor(Frame &frame, ValueId input, SourceSpan site,
                                      bool construct = true) {
  for (FunctionId id = 0; id < syntax_.functions.size(); ++id) {
    const auto original = syntax_.functions[id];
    if (original.moduleIndex >= syntax_.standardModules.size() ||
        syntax_.standardModules[original.moduleIndex] != StandardModule::Iteration ||
        name(original.name) != "iter" || original.generics.size() != 1)
      continue;
    const auto label = genericParameterLabel(original.generics[0]);
    GenericBindings bindings{{label, node(input).type}};
    GenericTermBindings terms{{label, genericTerm(input)}};
    if (genericContext(frame) && !checkGenericTermBounds(frame, original.generics, terms, site))
      return {};
    if (!construct)
      return input;
    auto target = specializeFunction(id, std::move(bindings), site);
    if (!target || !analyzeFunction(*target))
      return {};
    const auto type = output_.functions[*target].resultType;
    if (!isIterator(output_, type) || type.fallible || type.task || type.device) {
      error("S083", "Iterable.iterator must return a completed owned Iterator", site);
      return {};
    }
    auto result = add(frame, type, site, FunctionCall{*target, {input}});
    if (result && genericContext(frame) && original.result) {
      auto term = instantiateGenericTerm(*original.result, type, terms);
      term.actual = type;
      genericValueTerms_[*result] = std::move(term);
    }
    return result;
  }
  error(genericContext(frame) ? "S083" : "S076",
        "for expects an array, Range, Iterator or an Iterable source with a declared capability",
        site);
  return {};
}
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
