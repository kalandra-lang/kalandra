#include "Hover.h"
#include "SemanticQuery.h"
#include <algorithm>
namespace kalandra::lsp {
namespace {
struct TypePrinter {
  const TypedModule &module;
  std::size_t budget = 4096;
  bool valid = true;
  std::string errorName(std::optional<std::size_t> id, std::size_t depth = 0) {
    if (!id)
      return "TaskError";
    if (!budget || depth > 32 || *id >= module.records.size()) {
      valid = false;
      return {};
    }
    --budget;
    const auto &record = module.records[*id];
    if (record.taskErrorFamily)
      return "TaskError";
    if (record.errorMembers.empty())
      return record.name;
    std::string result;
    for (auto member : record.errorMembers) {
      if (!result.empty())
        result += " | ";
      result += errorName(member, depth + 1);
      if (!valid || result.size() > 16384) {
        valid = false;
        return {};
      }
    }
    return result;
  }
  std::string type(ValueType value, std::size_t depth = 0) {
    if (!budget || depth > 32) {
      valid = false;
      return {};
    }
    --budget;
    const bool task = value.task, fallible = value.fallible;
    const auto family = value.errorRecord;
    value.task = value.fallible = false;
    value.errorRecord.reset();
    std::string result;
    if (isFunctionArray(value))
      result = "[" + type(recordArrayElement(value), depth + 1) + "]";
    else if (value.builtin == BuiltinValue::Function && value.callable) {
      result = "fn(";
      for (std::size_t i = 0; i < value.callable->parameters.size(); ++i) {
        if (i)
          result += ", ";
        result += type(value.callable->parameters[i], depth + 1);
        if (!valid || result.size() > 16384) {
          valid = false;
          return {};
        }
      }
      result += ") -> " + returnedType(value.callable->result, depth + 1);
    } else if (value.builtin == BuiltinValue::Record && value.record &&
               *value.record < module.records.size() &&
               !module.records[*value.record].errorMembers.empty()) {
      result = errorName(value.record);
      if (value.array)
        result = "[" + result + "]";
    } else {
      for (auto &extent : value.shape)
        extent.identity = 0;
      result = describeType(value);
      // Dynamic extent identities are private inference details, not source types.
      for (std::size_t at = 0; (at = result.find("?d0", at)) != result.npos;)
        result.replace(at, 3, "?");
    }
    if (task)
      result = "task<" + result + ">";
    if (fallible) {
      if (value.builtin == BuiltinValue::Function)
        result = "(" + result + ")";
      result += " ! " + errorName(family);
    }
    if (result.size() > 16384) {
      valid = false;
      return {};
    }
    return result;
  }
  std::string returnedType(ValueType value, std::size_t depth) {
    const bool fallible = value.fallible;
    const auto family = value.errorRecord;
    value.fallible = false;
    value.errorRecord.reset();
    auto result = type(value, depth + 1);
    if (value.builtin == BuiltinValue::Function)
      result = "(" + result + ")";
    if (fallible)
      result += " ! " + errorName(family);
    return result;
  }
  std::string function(const TypedFunction &function, std::string_view name,
                       std::vector<std::string> *parameters = nullptr) {
    std::string result = "fn " + std::string(name) + "(";
    const auto count = function.parameters.size() - function.captureCount;
    for (std::size_t i = 0; i < count; ++i) {
      if (i)
        result += ", ";
      const auto &parameter = module.symbols[function.parameters[i]];
      const auto label = parameter.name + ": " + type(parameter.type);
      result += label;
      if (parameters)
        parameters->push_back(label);
      if (!valid || result.size() > 16384) {
        valid = false;
        return {};
      }
    }
    result += ") -> " + returnedType(function.resultType, 0);
    if (result.size() > 16384)
      valid = false;
    return result;
  }
};
std::optional<SignatureInfo> callableSignature(const TypedModule &module, const ValueType &type,
                                               std::string_view name, std::size_t active,
                                               std::span<const std::string> parameterNames = {}) {
  if (type.builtin != BuiltinValue::Function || !type.callable || type.array || type.task ||
      type.fallible)
    return {};
  TypePrinter printer{module};
  SignatureInfo result;
  result.label = "fn" + (name.empty() ? std::string{} : " " + std::string(name)) + "(";
  for (const auto &parameter : type.callable->parameters) {
    if (!result.parameters.empty())
      result.label += ", ";
    auto label = printer.type(parameter);
    if (!parameterNames.empty())
      label = parameterNames[result.parameters.size()] + ": " + label;
    result.parameters.push_back(std::move(label));
    result.label += result.parameters.back();
    if (!printer.valid || result.label.size() > 16384)
      return {};
  }
  result.label += ") -> " + printer.returnedType(type.callable->result, 0);
  if (!printer.valid || result.label.size() > 16384)
    return {};
  if (!result.parameters.empty())
    result.activeParameter = std::min(active, result.parameters.size() - 1);
  return result;
}
bool hasBuiltinSignature(std::string_view name) {
  return name == "length" || name == "byteLength" || name == "shape" || name == "slice" ||
         name == "concat" || name == "relu" || name == "reduceSum" || name == "matmul" ||
         name == "array" || name == "map" || name == "parallelMap" || name == "transfer" ||
         name == "devices.cpu" || name == "devices.gpu" || resolveNumeric(name).has_value();
}
std::optional<SignatureInfo> builtinSignature(const TypedModule &module, SourceSpan call,
                                              std::string_view name, std::size_t active,
                                              std::size_t argumentCount) {
  std::optional<SignatureInfo> result;
  for (const auto &expression : module.expressions) {
    if (expression.span.begin != call.begin || expression.span.end != call.end)
      continue;
    std::vector<ValueId> arguments;
    std::vector<std::string> labels;
    std::optional<ValueType> callback;
    std::optional<ValueType> selectorIndex;
    if (const auto *map = std::get_if<ArrayMap>(&expression.operation);
        map && name == (map->parallel ? "parallelMap" : "map")) {
      if (map->function >= module.functions.size())
        return {};
      const auto &function = module.functions[map->function];
      // An ArrayMap entry takes one element followed by its retained captures.
      // Function-value wrappers also retain the target callable in that tail.
      if (function.parameters.empty() || function.parameters.size() - 1 != map->captures.size() ||
          function.parameters.front() >= module.symbols.size())
        return {};
      callback = ValueType{};
      callback->builtin = BuiltinValue::Function;
      callback->callable = std::make_shared<const FunctionSignature>(
          FunctionSignature{{module.symbols[function.parameters.front()].type},
                            function.declaredResult.value_or(function.resultType)});
      arguments = {map->input};
      labels = {"input", "callback"};
    } else if (const auto *selector = std::get_if<DeviceSelection>(&expression.operation);
               selector && name == (selector->device ? "devices.gpu" : "devices.cpu")) {
      if (argumentCount > (selector->device ? 1u : 0u))
        return {};
      if (argumentCount) {
        // Selection indices are checked uint32 literals, not lowered scalar operands.
        // Describe the checked call arity rather than invent an optional-argument type.
        selectorIndex = ValueType{};
        selectorIndex->element = *resolveNumeric("uint32");
        labels = {"index"};
      }
    } else if (const auto *transfer = std::get_if<Transfer>(&expression.operation);
               transfer && name == "transfer") {
      arguments = {transfer->input, transfer->target};
      labels = {"input", "target"};
    } else if (const auto *conversion = std::get_if<NumericConvert>(&expression.operation);
               conversion && resolveNumeric(name) == expression.type.element) {
      arguments = {conversion->input};
      labels = {"input"};
    } else if (const auto *sum = std::get_if<ReduceSum>(&expression.operation);
               sum && name == "reduceSum") {
      arguments = {sum->input};
      labels = {"input"};
    } else if (const auto *unary = std::get_if<TensorUnary>(&expression.operation);
               unary && unary->operation == TokenKind::Relu && name == "relu") {
      arguments = {unary->operand};
      labels = {"input"};
    } else if (const auto *product = std::get_if<Matmul>(&expression.operation);
               product && name == "matmul") {
      arguments = {product->left, product->right};
      labels = {"left", "right"};
    } else if (const auto *array = std::get_if<TensorToArray>(&expression.operation);
               array && name == "array") {
      arguments = {array->input};
      labels = {"input"};
    } else if (const auto *query = std::get_if<CollectionQuery>(&expression.operation);
               query && name == (query->kind == CollectionQueryKind::Length ? "length" : "shape")) {
      arguments = {query->input};
      labels = {"input"};
    } else if (const auto *query = std::get_if<StringQuery>(&expression.operation);
               query &&
               name == (query->kind == StringQueryKind::Length ? "length" : "byteLength")) {
      arguments = {query->input};
      labels = {"input"};
    } else if (const auto *slice = std::get_if<ArraySlice>(&expression.operation);
               slice && name == "slice") {
      arguments = {slice->input, slice->start, slice->end};
      labels = {"input", "start", "end"};
    } else if (const auto *slice = std::get_if<StringSlice>(&expression.operation);
               slice && name == "slice") {
      arguments = {slice->input, slice->start, slice->end};
      labels = {"input", "start", "end"};
    } else if (const auto *concat = std::get_if<ArrayConcat>(&expression.operation);
               concat && name == "concat") {
      arguments = {concat->left, concat->right};
      labels = {"left", "right"};
    } else
      continue;
    FunctionSignature signature{{}, expression.type};
    for (const auto id : arguments) {
      if (id >= module.expressions.size())
        return {};
      signature.parameters.push_back(module.expressions[id].type);
    }
    if (callback)
      signature.parameters.push_back(std::move(*callback));
    if (selectorIndex)
      signature.parameters.push_back(std::move(*selectorIndex));
    ValueType type;
    type.builtin = BuiltinValue::Function;
    type.callable = std::make_shared<const FunctionSignature>(std::move(signature));
    auto candidate = callableSignature(module, type, name, active, labels);
    if (!candidate || (result && (result->label != candidate->label ||
                                  result->parameters != candidate->parameters)))
      return {};
    result = std::move(candidate);
  }
  return result;
}
std::optional<SignatureInfo> snapshotSignature(const CallableSignatureSnapshot &snapshot,
                                               std::string_view name, std::size_t active) {
  if (active >= snapshot.signature.parameters.size() ||
      (!snapshot.parameterNames.empty() &&
       snapshot.parameterNames.size() != snapshot.signature.parameters.size()))
    return {};
  TypedModule context;
  for (const auto &family : snapshot.errorFamilies) {
    TypedRecord record{};
    record.name = family.name;
    record.errorMembers = family.members;
    record.taskErrorFamily = family.taskError;
    context.records.push_back(std::move(record));
  }
  ValueType type;
  type.builtin = BuiltinValue::Function;
  type.callable = std::make_shared<const FunctionSignature>(snapshot.signature);
  return callableSignature(context, type, name, active, snapshot.parameterNames);
}
} // namespace
std::optional<HoverInfo> hoverInfo(ParseResult syntax, std::string_view path, std::size_t offset,
                                   CheckOptions options) {
  if (syntax.ok() && syntax.module.source) {
    std::optional<SourceLocation> target;
    std::optional<ExprId> selectedCall;
    std::size_t argumentCount = 0;
    SourceSpan invocation{};
    for (ExprId id = 0; id < syntax.module.expressions.size(); ++id) {
      const auto &expression = syntax.module.expressions[id];
      const NameExpr *name = nullptr;
      bool callSite = false;
      std::size_t arity = 1;
      if (const auto *call = std::get_if<CallExpr>(&expression.value)) {
        name = std::get_if<NameExpr>(&syntax.module.expressions[call->callee].value);
        callSite = true;
        arity = call->arguments.size();
      } else if (const auto *pipe = std::get_if<BinaryExpr>(&expression.value);
                 pipe && pipe->operation == TokenKind::Arrow)
        name = std::get_if<NameExpr>(&syntax.module.expressions[pipe->right].value);
      if (!name || !hasBuiltinSignature(syntax.module.source->slice(name->name)))
        continue;
      auto location = sourceLocation(syntax.module.source, name->name);
      if (!location || location->source->path() != path || offset < location->span.begin ||
          offset >= location->span.end)
        continue;
      target = std::move(location);
      invocation = expression.span;
      argumentCount = arity;
      if (callSite)
        selectedCall = id;
      break;
    }
    if (target) {
      if (selectedCall)
        for (const auto &expression : syntax.module.expressions)
          if (const auto *pipe = std::get_if<BinaryExpr>(&expression.value);
              pipe && pipe->operation == TokenKind::Arrow && pipe->right == *selectedCall) {
            invocation = expression.span;
            ++argumentCount;
            break;
          }
      auto checked = check(syntax, {}, options);
      if (!checked.ok())
        return {};
      if (auto signature = builtinSignature(*checked.module, invocation,
                                            target->source->slice(target->span), 0, argumentCount))
        return HoverInfo{std::move(signature->label), target->source, target->span};
    }
  }
  auto selected = semanticToken(std::move(syntax), path, offset, options);
  if (!selected)
    return {};
  TypePrinter printer{selected->module};
  const auto word = selected->token.source->slice(selected->token.span);
  std::string text;
  if (selected->function)
    text = printer.function(selected->module.functions[*selected->function], word);
  else if (selected->record) {
    const auto &record = selected->module.records[*selected->record];
    text = std::string(record.error              ? "error "
                       : record.variants.empty() ? "type "
                                                 : "enum ") +
           std::string(word);
  } else if (selected->type) {
    text = printer.type(*selected->type);
    if (!word.starts_with('"'))
      text = std::string(word) + ": " + text;
  }
  if (!printer.valid || text.empty() || text.size() > 16384)
    return {};
  return HoverInfo{std::move(text), selected->token.source, selected->token.span};
}
std::optional<SignatureInfo> signatureInfo(ParseResult syntax, std::string_view path,
                                           std::size_t offset, CheckOptions options,
                                           bool requireFieldProjection,
                                           bool requireNameProjection) {
  if ((!syntax.ok() && !syntax.module.editorOnly) || !syntax.module.source)
    return {};
  std::optional<ExprId> selected;
  std::optional<SourceLocation> callLocation, calleeLocation;
  for (ExprId i = 0; i < syntax.module.expressions.size(); ++i) {
    const auto &expression = syntax.module.expressions[i];
    const auto *call = std::get_if<CallExpr>(&expression.value);
    if (!call)
      continue;
    auto location = sourceLocation(syntax.module.source, expression.span);
    auto callee =
        sourceLocation(syntax.module.source, syntax.module.expressions[call->callee].span);
    if (!location || !callee || location->source->path() != path ||
        callee->source->path() != path || offset <= callee->span.end ||
        (offset > location->span.end ||
         (offset == location->span.end && !syntax.module.editorOnly)))
      continue;
    if (!callLocation || location->span.end - location->span.begin <
                             callLocation->span.end - callLocation->span.begin) {
      selected = i;
      callLocation = std::move(location);
      calleeLocation = std::move(callee);
    }
  }
  if (!selected)
    return {};
  const auto &call = std::get<CallExpr>(syntax.module.expressions[*selected].value);
  const auto &callee = syntax.module.expressions[call.callee];
  std::optional<SourceSpan> token;
  if (const auto *name = std::get_if<NameExpr>(&callee.value))
    token = name->name;
  else if (const auto *field = std::get_if<FieldExpr>(&callee.value))
    token = field->field;
  auto target = token ? sourceLocation(syntax.module.source, *token) : std::nullopt;
  if (token && !target)
    return {};
  SyntaxLimits limits;
  limits.editorRecovery = syntax.module.editorOnly;
  const auto lexed = lex(callLocation->source, limits);
  if (!lexed.diagnostics.empty() && !syntax.module.editorOnly)
    return {};
  bool opened = false;
  std::size_t depth = 0, active = 0;
  std::optional<TokenKind> last;
  for (const auto &token : lexed.tokens) {
    if (token.span.begin < calleeLocation->span.end)
      continue;
    if (token.span.begin >= offset)
      break;
    if (!opened) {
      if (token.kind == TokenKind::Newline || token.kind == TokenKind::Indent ||
          token.kind == TokenKind::Dedent)
        continue;
      if (token.kind != TokenKind::LeftParen)
        return {};
      opened = true;
      last = token.kind;
      continue;
    }
    if (token.kind != TokenKind::Newline && token.kind != TokenKind::Indent &&
        token.kind != TokenKind::Dedent)
      last = token.kind;
    if (token.kind == TokenKind::LeftParen || token.kind == TokenKind::LeftBrace ||
        token.kind == TokenKind::LeftBracket)
      ++depth;
    else if (token.kind == TokenKind::RightParen || token.kind == TokenKind::RightBrace ||
             token.kind == TokenKind::RightBracket) {
      if (!depth)
        return {};
      --depth;
    } else if (token.kind == TokenKind::Comma && !depth)
      ++active;
  }
  if (!opened)
    return {};
  auto invocationSpan = syntax.module.expressions[*selected].span;
  bool piped = false;
  for (const auto &expression : syntax.module.expressions)
    if (const auto *binary = std::get_if<BinaryExpr>(&expression.value);
        binary && binary->operation == TokenKind::Arrow && binary->right == *selected) {
      ++active; // The pipeline supplies the first parameter implicitly.
      invocationSpan = expression.span;
      piped = true;
      break;
    }
  // Builtin guidance is a specialization of an actual checked operation, not a
  // signature inferred from spelling. In particular, a lexical/module binding
  // with the same name must continue through ordinary callable resolution.
  if (target && !requireFieldProjection && !requireNameProjection) {
    const auto name = target->source->slice(target->span);
    if (hasBuiltinSignature(name)) {
      auto checked = check(syntax, {}, options);
      if (checked.ok())
        if (auto result = builtinSignature(*checked.module, invocationSpan, name, active,
                                           call.arguments.size() + (piped ? 1u : 0u)))
          return result;
    }
  }
  // Describe a simple identifier/field editing slot only in this request.
  // A field receiver still needs to check before a signature can be captured.
  bool editingName = false;
  std::optional<ExprId> argumentReceiver;
  std::optional<std::size_t> argumentIndex;
  const auto implicit = piped ? 1u : 0u;
  if (!call.arguments.empty() && active >= implicit && active - implicit < call.arguments.size()) {
    const auto argumentId = call.arguments[active - implicit];
    const auto &argument = syntax.module.expressions[argumentId];
    std::optional<SourceSpan> editingToken;
    if (const auto *name = std::get_if<NameExpr>(&argument.value);
        name && name->name.begin == argument.span.begin && name->name.end == argument.span.end)
      editingToken = name->name;
    else if (const auto *field = std::get_if<FieldExpr>(&argument.value);
             field && field->field.end == argument.span.end) {
      editingToken = field->field;
      argumentReceiver = field->input;
    }
    if (editingToken) {
      const auto location = sourceLocation(syntax.module.source, *editingToken);
      if (location && location->source->path() == path && location->span.begin < offset &&
          offset <= location->span.end) {
        if (active - implicit == call.arguments.size() - 1)
          std::get<CallExpr>(syntax.module.expressions[*selected].value).arguments.pop_back();
        else
          argumentIndex = active; // Keep the edited and later arguments in the request AST.
        editingName = true;
      }
    }
  }
  // A synthetic field probe may supply guidance only through the checked
  // receiver projection, never by checking an invented argument as a real value.
  if ((requireFieldProjection && (!editingName || !argumentReceiver)) ||
      (requireNameProjection && !editingName))
    return {};
  CallableSignatureQuery query{
      invocationSpan, {}, editingName ? argumentReceiver : std::nullopt, argumentIndex};
  if (syntax.module.editorOnly && active >= implicit && active - implicit < call.arguments.size()) {
    const auto argument = call.arguments[active - implicit];
    std::optional<SourceSpan> hole;
    bool direct = false;
    for (const auto &node : syntax.module.expressions) {
      std::optional<SourceSpan> token;
      if (const auto *name = std::get_if<NameExpr>(&node.value))
        token = name->name;
      else if (const auto *field = std::get_if<FieldExpr>(&node.value))
        token = field->field;
      const auto span = syntax.module.expressions[argument].span;
      if (!token || token->begin < span.begin || token->end > span.end)
        continue;
      const auto [file, begin] = syntax.module.source->origin(token->begin);
      const auto [lastFile, end] = syntax.module.source->origin(token->end);
      const bool atToken = begin <= offset && offset <= end;
      const bool nearHole =
          token->begin == token->end &&
          std::ranges::all_of(file->slice({std::min(begin, offset), std::max(begin, offset)}),
                              [](char c) { return c == ' ' || c == '\n' || c == '\r'; });
      if (file == lastFile && file->path() == path && (atToken || (!direct && nearHole)))
        if (!hole || (atToken && !direct) || token->end - token->begin < hole->end - hole->begin) {
          hole = token;
          direct = atToken;
        }
    }
    if (hole) {
      query.argumentIndex = active;
      query.argumentReceiver.reset();
      query.editingExpression = argument;
      query.editingSite = *hole;
      editingName = true;
    }
  }
  const auto following = std::ranges::find_if(lexed.tokens, [&](const auto &token) {
    return token.span.begin >= offset && token.kind != TokenKind::Newline &&
           token.kind != TokenKind::Indent && token.kind != TokenKind::Dedent;
  });
  if (editingName || (!depth && (last == TokenKind::LeftParen || last == TokenKind::Comma) &&
                      following != lexed.tokens.end() && following->kind == TokenKind::RightParen))
    options.signature = &query;
  const auto projected = [&]() -> std::optional<SignatureInfo> {
    const auto name = target ? target->source->slice(target->span) : std::string_view{};
    return query.snapshot ? snapshotSignature(*query.snapshot, name, active) : std::nullopt;
  };
  if (syntax.module.editorOnly) {
    if (editingName || (!depth && (last == TokenKind::LeftParen || last == TokenKind::Comma)))
      options.signature = &query;
    (void)check(std::move(syntax), {}, options);
    return projected();
  }
  if (!target) {
    auto checked = check(std::move(syntax), {}, options);
    if (editingName || !checked.ok())
      return projected();
    const ValueType *type = nullptr;
    for (const auto &expression : checked.module->expressions)
      if (expression.span.begin == invocationSpan.begin &&
          expression.span.end == invocationSpan.end)
        if (const auto *invoke = std::get_if<FunctionInvoke>(&expression.operation)) {
          const auto &candidate = checked.module->expressions[invoke->callee].type;
          if (type && !sameFunctionValueType(*type, candidate))
            return {}; // Ambiguous lowered identities do not provide a guessed signature.
          type = &candidate;
        }
    return type ? callableSignature(*checked.module, *type, {}, active) : std::nullopt;
  }
  auto semantic = semanticToken(std::move(syntax), path, target->span.begin, options);
  if (editingName || !semantic)
    return projected();
  TypePrinter printer{semantic->module};
  SignatureInfo result;
  const auto name = semantic->token.source->slice(semantic->token.span);
  if (semantic->function)
    result.label =
        printer.function(semantic->module.functions[*semantic->function], name, &result.parameters);
  else if (semantic->type)
    return callableSignature(semantic->module, *semantic->type, name, active);
  else
    return {};
  if (!printer.valid || result.label.empty() || result.label.size() > 16384)
    return {};
  if (!result.parameters.empty())
    result.activeParameter = std::min(active, result.parameters.size() - 1);
  return result;
}
} // namespace kalandra::lsp
