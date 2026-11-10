#include "kalandra/Application.h"
#include <algorithm>
namespace kalandra {
std::expected<ApplicationEntryInfo, Diagnostic>
validateApplicationEntry(const TypedModule &module) {
  const auto found =
      std::ranges::find_if(module.functions, [](const auto &fn) { return fn.name == "main"; });
  const auto fail = [&](std::string message) {
    return std::unexpected(
        Diagnostic{"S081", std::move(message),
                   found == module.functions.end() ? SourceSpan{} : found->declaration});
  };
  if (module.applicationEntry)
    return fail("application entry was already prepared");
  if (found == module.functions.end())
    return fail("an application requires a main function");
  const auto sourceId = static_cast<FunctionId>(found - module.functions.begin());
  const auto &source = *found;
  const auto &result = source.resultType;
  if (source.anonymous || source.captureCount || source.compileTimeOnly ||
      source.parameters.size() > 1)
    return fail("main takes no parameters or a single args: [String] parameter");
  if (!source.parameters.empty()) {
    if (source.parameters[0] >= module.symbols.size())
      return fail("invalid main parameter");
    const auto &argument = module.symbols.at(source.parameters[0]);
    const auto &type = argument.type;
    if (!isStringArray(type) || !type.tensor || type.task || type.fallible || type.device ||
        type.shape.size() != 1 || type.shape[0].constant || argument.mutableAccess)
      return fail("main's optional argument must be a read-only [String] array");
  }
  if (result.tensor || result.array || result.task || result.device ||
      (result.builtin == BuiltinValue::Unit && result.record) ||
      (result.builtin != BuiltinValue::Unit &&
       (result.builtin != BuiltinValue::Record || !result.record ||
        *result.record >= module.records.size() ||
        module.records[*result.record].standard != StandardRecord::ExitStatus)))
    return fail("main must return unit or ExitStatus imported from std/process");
  if (result.record) {
    const auto &fields = module.records[*result.record].fields;
    if (fields.size() != 1 || fields[0].name != "code" || fields[0].type.tensor ||
        fields[0].type.builtin != BuiltinValue::Numeric ||
        fields[0].type.element != NumericType::UInt8)
      return fail("invalid standard ExitStatus layout");
  }
  return ApplicationEntryInfo{sourceId, !source.parameters.empty(),
                              result.builtin == BuiltinValue::Unit};
}
std::optional<Diagnostic> prepareApplication(TypedModule &module, SemanticLimits limits) {
  auto validated = validateApplicationEntry(module);
  if (!validated)
    return validated.error();
  const auto sourceId = validated->function;
  const auto source = module.functions[sourceId];
  const auto &result = source.resultType;
  const auto fail = [&](std::string message) -> std::optional<Diagnostic> {
    return Diagnostic{"S081", std::move(message), source.declaration};
  };
  if (module.expressions.size() > limits.maxValues ||
      limits.maxValues - module.expressions.size() < 6)
    return fail("application entry exceeds the typed value limit");
  TypedFunction wrapper;
  wrapper.name = "main";
  wrapper.declaration = source.declaration;
  ValueType exit;
  exit.element = NumericType::Int32;
  wrapper.resultType = exit;
  wrapper.resultType.fallible = result.fallible;
  wrapper.resultType.errorRecord = result.errorRecord;
  wrapper.declaredResult = wrapper.resultType;
  const auto add = [&](ValueType type, auto operation) {
    const auto id = module.expressions.size();
    if (type.fallible)
      type.identity = id + 1;
    wrapper.expressions.push_back(id);
    wrapper.body.push_back({source.declaration, {}, id});
    module.expressions.push_back({source.declaration, std::move(type), std::move(operation)});
    return id;
  };
  std::vector<ValueId> arguments;
  if (!source.parameters.empty()) {
    auto type = module.symbols[source.parameters[0]].type;
    type.identity = 0;
    arguments.push_back(add(type, ApplicationArguments{}));
  }
  auto value = add(result, FunctionCall{sourceId, std::move(arguments)});
  auto completed = result;
  completed.fallible = false;
  completed.errorRecord.reset();
  completed.identity = 0;
  if (result.fallible)
    value = add(completed, TaskOperation{TaskPrefix::Try, value});
  if (result.builtin == BuiltinValue::Unit)
    value = add(exit, Int32Literal{0});
  else {
    value = add(module.records[*result.record].fields[0].type, RecordFieldRead{value, 0});
    value = add(exit, NumericConvert{value});
  }
  wrapper.result = value;
  std::string hidden = "__application_main_" + std::to_string(sourceId);
  while (std::ranges::any_of(module.functions, [&](const auto &fn) { return fn.name == hidden; }))
    hidden += '_';
  module.functions[sourceId].name = std::move(hidden);
  module.functions.push_back(std::move(wrapper));
  module.applicationEntry = true;
  return {};
}
} // namespace kalandra
