#include "kalandra/Continuation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(std::string code) {
  return check(parse(std::make_shared<SourceFile>("worker-functions.k", std::move(code))));
}
int main() {
  try {
    unsigned count = 0;
    for (const auto *directory :
         {KALANDRA_WORKER_FUNCTION_FIXTURES, KALANDRA_WORKER_CLOSURE_FIXTURES,
          KALANDRA_WORKER_NESTED_FIXTURES, KALANDRA_WORKER_RESULT_FIXTURES,
          KALANDRA_WORKER_AGGREGATE_FIXTURES, KALANDRA_WORKER_PARAMETER_FIXTURES,
          KALANDRA_WORKER_INDIRECT_FIXTURES, KALANDRA_INDIRECT_SPAWN_FIXTURES,
          KALANDRA_WORKER_MAP_FIXTURES, KALANDRA_WORKER_COLLECTION_FIXTURES,
          KALANDRA_WORKER_ERROR_FIXTURES}) {
      for (const auto &file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() != ".k")
          continue;
        std::ifstream stream(file.path());
        auto result = analyze({std::istreambuf_iterator<char>(stream), {}});
        if (!result.ok())
          for (const auto &diagnostic : result.diagnostics)
            std::cerr << renderDiagnostic(*result.source, diagnostic);
        require(result.ok(), "named function worker fixture rejected");
        require(bool(continuation::plan(*result.module)), "named worker callable planning failed");
        ++count;
      }
    }
    require(count >= 148, "worker callable fixture catalog incomplete");
    const std::string base = "fn double(value:int)->int:\n  value*2\nfn "
                             "apply(value:int,transform:fn(int)->int)->int:\n  transform(value)\n";
    for (const auto *body :
         {"fn wrap(target:fn(int)->int)->fn(int)->int:\n  fn(value:int):target(value)\nfn "
          "main()->int ! TaskError:\n  f=wrap(double)\n  try parallel:\n    try await spawn "
          "apply(21,f)\n",
          "fn make(target:fn(int)->int)->fn(int)->int:\n  target\nfn main()->int ! TaskError:\n  "
          "try parallel:\n    try await spawn apply(21,make(double))\n",
          "fn forward(f:fn(int)->int)->int ! TaskError:\n  try parallel:\n    try await spawn "
          "apply(21,f)\nfn main()->int ! TaskError:\n  try forward(double)\n"}) {
      auto result = analyze(base + body);
      require(result.ok(), "direct parameter provenance rejected");
    }
    for (const auto *directory :
         {KALANDRA_WORKER_PARAMETER_FIXTURES, KALANDRA_WORKER_INDIRECT_FIXTURES,
          KALANDRA_INDIRECT_SPAWN_FIXTURES, KALANDRA_WORKER_MAP_FIXTURES,
          KALANDRA_WORKER_COLLECTION_FIXTURES, KALANDRA_WORKER_ERROR_FIXTURES})
      for (const auto &file :
           std::filesystem::directory_iterator(std::filesystem::path(directory) / "rejected")) {
        if (file.path().extension() != ".k")
          continue;
        std::ifstream stream(file.path());
        auto result = analyze({std::istreambuf_iterator<char>(stream), {}});
        require(!result.ok() && std::ranges::any_of(result.diagnostics,
                                                    [](const auto &d) { return d.code == "S074"; }),
                "unproven parameter sources admitted");
      }
    auto result = analyze(base + "fn main()->int ! TaskError:\n  f=double\n  try parallel:\n    "
                                 "try await spawn apply(21,f)\n");
    require(result.ok(), "mutation fixture rejected");
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto module = *result.module;
      for (auto &expression : module.expressions)
        if (auto *ref = std::get_if<FunctionReference>(&expression.operation)) {
          if (mode == 0)
            ref->function = module.functions.size();
          if (mode == 1)
            ref->captures.push_back(0);
          if (mode == 2)
            module.functions[ref->function].captureCount = 1;
          break;
        }
      require(numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "forged worker callable provenance accepted");
    }
    auto returned = analyze("fn make()->fn(int)->int:\n  fn(value:int):value\nfn main()->int ! "
                            "TaskError:\n  f=try parallel:\n    try await spawn make()\n  f(42)\n");
    require(returned.ok(), "returned function mutation fixture rejected");
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto module = *returned.module;
      bool changed = false;
      for (auto &expression : module.expressions) {
        if (const auto *task = std::get_if<TaskOperation>(&expression.operation)) {
          if ((mode == 0 && task->prefix == TaskPrefix::Spawn) ||
              (mode == 1 && task->prefix == TaskPrefix::Await)) {
            expression.type.task = !expression.type.task;
            changed = true;
            break;
          }
        }
        if (mode == 2 && expression.type.builtin == BuiltinValue::Function &&
            std::holds_alternative<FunctionCall>(expression.operation)) {
          expression.type.callable.reset();
          changed = true;
          break;
        }
      }
      require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "forged function result task/signature admitted");
    }
    auto indirect =
        analyze(base + "fn make()->fn(int)->int:\n  double\n"
                       "fn main()->int ! TaskError:\n  factory=if true:make\n  else:make\n"
                       "  f=factory()\n  try parallel:\n    try await spawn apply(21,f)\n");
    require(indirect.ok(), "indirect provenance mutation fixture rejected");
    for (unsigned mode = 0; mode < 2; ++mode) {
      auto module = *indirect.module;
      bool changed = false;
      for (auto &expression : module.expressions) {
        if (mode == 0)
          if (const auto *ref = std::get_if<FunctionReference>(&expression.operation);
              ref && module.functions[ref->function].name == "make") {
            // One known branch must not hide an unknown branch's target.
            expression.operation = BoolLiteral{false};
            changed = true;
            break;
          }
        if (mode == 1)
          if (auto *call = std::get_if<FunctionInvoke>(&expression.operation)) {
            call->callee = module.expressions.size();
            changed = true;
            break;
          }
      }
      bool rejected = false;
      for (const auto &expression : module.expressions)
        if (const auto *task = std::get_if<TaskOperation>(&expression.operation);
            task && task->prefix == TaskPrefix::Spawn)
          rejected =
              functionValueTransportError(module, module.expressions[task->operand]).has_value();
      require(changed && rejected && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "partial or malformed indirect provenance admitted");
    }
    auto mapped = analyze(base + "fn main()->int ! TaskError:\n  f=double\n  try parallel:\n    "
                                 "values=try await spawn map([21],f)\n    values[0]\n");
    require(mapped.ok(), "map provenance mutation fixture rejected");
    auto forgedMap = *mapped.module;
    bool changedMap = false;
    for (auto &expression : forgedMap.expressions)
      if (std::holds_alternative<FunctionReference>(expression.operation)) {
        expression.operation = BoolLiteral{false};
        changedMap = true;
        break;
      }
    bool rejectedMap = false;
    for (const auto &expression : forgedMap.expressions)
      if (const auto *task = std::get_if<TaskOperation>(&expression.operation);
          task && task->prefix == TaskPrefix::Spawn)
        rejectedMap = functionValueTransportError(forgedMap, forgedMap.expressions[task->operand])
                          .has_value();
    require(changedMap && rejectedMap, "forged map callback provenance admitted");
    for (const auto *operation :
         {"slice(callbacks,0,1)", "concat(callbacks,callbacks)", "length(callbacks)"}) {
      const std::string query =
          std::string(operation).starts_with("length") ? "values" : "length(values)";
      auto checked = analyze(base +
                             "fn main()->int ! TaskError:\n  callbacks=[double]\n  try parallel:\n "
                             "   values=try await spawn " +
                             operation + "\n    " + query + "\n");
      require(checked.ok(), "collection provenance mutation fixture rejected");
      auto module = *checked.module;
      bool changed = false, rejected = false;
      for (auto &value : module.expressions)
        if (std::holds_alternative<FunctionReference>(value.operation)) {
          value.operation = BoolLiteral{false};
          changed = true;
          break;
        }
      for (const auto &value : module.expressions)
        if (const auto *task = std::get_if<TaskOperation>(&value.operation);
            task && task->prefix == TaskPrefix::Spawn)
          rejected =
              functionValueTransportError(module, module.expressions[task->operand]).has_value();
      require(changed && rejected, "forged collection element provenance admitted");
    }
    auto errorResult =
        analyze("error E:\n  Bad:\n    callback:fn(int)->int\nfn worker()->int ! E:\n  if "
                "true:fail E.Bad {callback:fn(value:int):value}\n  42\nfn main()->int ! (E | "
                "TaskError):\n  try parallel:\n    try await spawn worker()\n");
    require(errorResult.ok(), "error provenance mutation fixture rejected");
    for (unsigned mode = 0; mode < 2; ++mode) {
      auto module = *errorResult.module;
      bool changed = false, rejected = false;
      for (auto &value : module.expressions) {
        if (mode == 0)
          if (auto *failure = std::get_if<FailOperation>(&value.operation)) {
            failure->error = module.expressions.size();
            changed = true;
            break;
          }
        if (mode == 1 && std::holds_alternative<FunctionReference>(value.operation)) {
          value.operation = BoolLiteral{false};
          changed = true;
          break;
        }
      }
      for (const auto &value : module.expressions)
        if (const auto *task = std::get_if<TaskOperation>(&value.operation);
            task && task->prefix == TaskPrefix::Spawn)
          rejected =
              functionValueTransportError(module, module.expressions[task->operand]).has_value();
      require(changed && rejected, "forged early error provenance admitted");
    }
    std::cout << "Worker named functions: " << count << " valid programs, 19 rejected boundaries, 14 forged IR "
                 "cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
