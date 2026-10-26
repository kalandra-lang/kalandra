#include "kalandra/Continuation.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::continuation;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void verify(const ModulePlan &module) {
  for (const auto &pointer : module.plans) {
    const auto &plan = *pointer;
    if (plan.code.empty())
      continue;
    require(plan.result < plan.slots.size(), "invalid plan result");
    std::vector<unsigned> opened(plan.groups), joined(plan.groups);
    std::map<Slot, std::size_t> defined;
    for (auto parameter : plan.parameters)
      defined.emplace(parameter, 0);
    for (std::size_t pc = 0; pc < plan.code.size(); ++pc) {
      const auto &op = plan.code[pc];
      require(op.output < plan.slots.size(), "invalid output slot");
      for (auto arg : op.args)
        require(arg < plan.slots.size(), "invalid argument slot");
      if (op.code == Code::Open || op.code == Code::Join || op.code == Code::Spawn ||
          op.code == Code::Transfer)
        require(op.group < plan.groups, "invalid owning group");
      if (op.code == Code::Open) {
        require(op.target == SIZE_MAX || op.target < op.group, "invalid parent group");
        ++opened[op.group];
      }
      if (op.code == Code::Join)
        ++joined[op.group];
      if (op.code == Code::Try || op.code == Code::Fail) {
        require(op.jump < plan.code.size(), "invalid failure exit");
        if (op.group != SIZE_MAX) {
          require(plan.code[op.jump].code == Code::Join && plan.code[op.jump].group == op.group,
                  "failure bypasses owning settlement");
        } else
          require(plan.code[op.jump].code == Code::Label, "invalid handler/function failure exit");
      }
      if (op.code == Code::Catch) {
        require(op.target < plan.code.size() && plan.code[op.target].code == Code::Label,
                "invalid catch success exit");
        require(op.jump == pc + 1, "invalid catch handler entry");
        require(plan.slots[op.args[0]].fallible, "catch lost expected-error operand");
      }
      if (op.code == Code::Jump || op.code == Code::Case)
        require((op.jump == SIZE_MAX && op.code == Code::Case) || op.jump < plan.code.size(),
                "invalid branch edge");
      if (op.code == Code::Await) {
        require(plan.slots[op.args[0]].task, "await lost task handle");
        require(plan.slots[op.output].fallible && !plan.slots[op.output].task,
                "await lost expected-error result");
      }
      if (op.code == Code::Spawn && op.placement) {
        require(*op.placement < plan.slots.size() &&
                    plan.slots[*op.placement].builtin == BuiltinValue::Device,
                "spawn lost device selector");
        require(defined.contains(*op.placement), "target used before evaluation");
        const auto &spawn = std::get<TaskOperation>(module.input.expressions[op.source].operation);
        // Reading an existing immutable selector emits no work; a fresh selector
        // must follow all argument computation rather than moving admission earlier.
        if (!std::holds_alternative<SymbolRead>(
                module.input.expressions[*spawn.placement].operation))
          for (auto arg : op.args)
            require(defined.contains(arg) &&
                        (std::ranges::find(plan.parameters, arg) != plan.parameters.end() ||
                         defined.at(arg) < defined.at(*op.placement)),
                    "task arguments must precede fresh target evaluation");
      }
      if (op.code == Code::Transfer) {
        require(plan.slots[op.output].tensor && plan.slots[op.output].task,
                "transfer lost owned tensor result");
        require(plan.slots[op.output].device == plan.slots[op.args[1]].device,
                "transfer lost residence");
      }
      // Inputs/aliases have one initial definition; branch merge copies may define again.
      defined.try_emplace(op.output, pc);
      if (op.code == Code::Catch)
        defined.try_emplace(op.args[1], pc);
    }
    for (std::size_t group = 0; group < plan.groups; ++group)
      require(opened[group] == 1 && joined[group] == 1, "group lacks unique open/join");
    require(plan.code.back().code == Code::Return, "plan lacks terminal return");
  }
}
CheckResult analyze(const std::string &source, bool placement = false) {
  return check(parse(std::make_shared<SourceFile>("plan.k", source)), {}, {.placement = placement});
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected project root");
    const std::filesystem::path root = argv[1];
    unsigned fixtures = 0;
    for (const char *name : {"placed",
                             "composed",
                             "mixed",
                             "tensor-results",
                             "shared-helpers",
                             "scalar-entry",
                             "matrix-elementwise",
                             "differences",
                             "arguments",
                             "unused-inputs",
                             "indirect-arguments",
                             "tensor-negation",
                             "division",
                             "host-expressions",
                             "cpu-spawn",
                             "nested-parallel",
                             "recovery",
                             "reduction",
                             "matmul",
                             "ir-supervisor",
                             "resumable-helpers",
                             "spawned-helpers"}) {
      auto file = loadSource((root / "native/metal-source" / (std::string(name) + ".k")).string());
      require(bool(file), "cannot read mixed fixture");
      auto checked = check(parse(*file), {}, {.placement = true});
      require(checked.ok(), "fixture failed shared checking");
      auto main = std::ranges::find_if(checked.module->functions,
                                       [](const auto &f) { return f.name == "main"; });
      auto result =
          plan(*checked.module, {.preserveCalls = true,
                                 .entry = std::size_t(main - checked.module->functions.begin())});
      if (!result)
        throw std::runtime_error(result.error());
      verify(*result);
      auto again =
          plan(*checked.module, {.preserveCalls = true,
                                 .entry = std::size_t(main - checked.module->functions.begin())});
      require(bool(again) && dump(*result) == dump(*again), "non-deterministic shared plan");
      require(!plan(*checked.module), "CPU planning silently dropped placement");
      require(!plan(*checked.module,
                    {.preserveCalls = true, .entry = checked.module->functions.size()}),
              "invalid entry accepted");
      ++fixtures;
    }
    for (const char *name : {"task-recovery", "task-recovery-allocated"}) {
      auto file =
          loadSource((root / "native/tests/fixtures" / (std::string(name) + ".k")).string());
      require(bool(file), "cannot read CPU fixture");
      auto checked = check(parse(*file));
      require(checked.ok(), "CPU fixture checking failed");
      auto result = plan(*checked.module);
      require(bool(result), "CPU planning failed");
      verify(*result);
      require(std::ranges::none_of(result->plans,
                                   [](const auto &p) {
                                     return std::ranges::any_of(p->code, [](const auto &op) {
                                       return op.code == Code::Call || op.code == Code::Transfer ||
                                              op.code == Code::Device;
                                     });
                                   }),
              "CPU plan contains target-only operations");
      ++fixtures;
    }
    auto helpers = analyze("fn numeric(x:float32) -> float32:\n  x*2\n"
                           "fn leaf(x:float32) -> float32 ! TaskError:\n  try parallel:\n"
                           "    try await spawn numeric(x)\n"
                           "fn recovered(x:float32) -> float32:\n  leaf(x) catch fn(error): 0\n"
                           "fn wrapper(x:float32) -> float32:\n  recovered(x)\n"
                           "fn main() -> float32:\n  wrapper(3)\n",
                           true);
    require(helpers.ok(), "helper effects fixture checking failed");
    const auto effects = resumableFunctions(*helpers.module);
    require(effects == std::vector<bool>({false, true, true, true, true}),
            "resumable effects failed transitive/nonfallible classification");
    auto helperPlan = plan(*helpers.module, {.preserveCalls = true, .entry = 4});
    require(bool(helperPlan), "resumable helper planning failed");
    verify(*helperPlan);
    require(helperPlan->plans[4]->groups == 1 &&
                std::ranges::none_of(
                    helperPlan->plans[4]->code,
                    [](const auto &op) { return op.code == Code::Call && op.target != 0; }),
            "resumable helper escaped into a synchronous dispatch");
    auto spawnedHelper = analyze("fn leaf() -> float32 ! TaskError:\n  try parallel:\n    1\n"
                                 "fn main() -> float32 ! TaskError:\n  try parallel:\n"
                                 "    try await spawn leaf()\n",
                                 true);
    require(spawnedHelper.ok(), "spawned helper checking failed");
    auto scheduled = plan(*spawnedHelper.module, {.preserveCalls = true, .entry = 1});
    require(bool(scheduled) && !scheduled->plans[0]->code.empty() &&
                scheduled->plans[0]->parameters.empty(),
            "resumable child plan missing");
    verify(*scheduled);
    auto gpuHelper = analyze("fn leaf() -> float32 ! TaskError:\n  try parallel:\n    1\n"
                             "fn main() -> float32 ! TaskError:\n  try parallel:\n"
                             "    try await spawn leaf() on devices.gpu()\n",
                             true);
    require(gpuHelper.ok(), "placed helper checking failed");
    auto unsupported = plan(*gpuHelper.module, {.preserveCalls = true, .entry = 1});
    require(!unsupported && unsupported.error().find("resumable helpers require CPU placement") !=
                                std::string::npos,
            "resumable control flow admitted as a GPU shader");
    auto recursive = analyze("fn loop(x:float32) -> float32:\n  loop(x)\nfn main() -> float32 ! "
                             "TaskError:\n  try parallel:\n    try await spawn loop(1)\n");
    require(recursive.ok(), "recursive CPU fixture rejected");
    auto recursivePlan = plan(*recursive.module);
    require(bool(recursivePlan), "recursive CPU calls were expanded instead of planned");
    bool backedge = false;
    for (FunctionId id = 0; id < recursivePlan->kernels.functions.size(); ++id)
      for (auto value : recursivePlan->kernels.functions[id].expressions)
        if (const auto *call = std::get_if<FunctionCall>(
                &recursivePlan->kernels.expressions[value].operation))
          backedge |= call->function == id;
    require(backedge, "recursive CPU call lost its native backedge");
    for (const auto &entry : recursivePlan->plans)
      require(std::ranges::none_of(entry->code, [](const auto &op) {
                return op.code == Code::SynchronousCall;
              }), "pure recursive work retained per-call continuation frames");
    auto legacyPlan = plan(*recursive.module, {.nativeSubgraphs = false});
    require(bool(legacyPlan), "checked continuation fallback was removed");
    backedge = false;
    for (const auto &entry : legacyPlan->plans)
      for (const auto &op : entry->code) backedge |= op.code == Code::SynchronousCall;
    require(backedge, "fallback lost checked recursive execution");
    verify(*legacyPlan);
    auto builtin =
        analyze("fn main() -> float32 ! TaskError:\n  try parallel:\n    data:Tensor[float32,2] = "
                "tensor([1,2])\n    try await spawn reduceSum(data)\n");
    require(builtin.ok(), "builtin fixture checking failed");
    auto result = plan(*builtin.module);
    require(bool(result) && result->plans.size() == 2, "builtin child plan missing");
    verify(*result);
    auto projected =
        analyze("type Row:\n  value:int\nfn main()->int:\n  row=Row {value:42}\n  row.value\n");
    require(projected.ok(), "record projection plan source rejected");
    for (bool preserved : {false, true}) {
      auto planned = plan(*projected.module, {.preserveCalls = preserved});
      require(bool(planned), "record projection planning failed");
      bool found = false;
      for (const auto &entry : planned->plans)
        for (const auto &op : entry->code)
          if (op.source < projected.module->expressions.size() &&
              std::holds_alternative<RecordFieldRead>(
                  projected.module->expressions[op.source].operation)) {
            require(op.code == Code::Kernel,
                    "record field plan used the wrong backend path");
            found = true;
          }
      require(found, "record field projection missing from plan");
      verify(*planned);
    }
    auto narrowed = analyze("error A:\n  Retry:\n    code:int\nerror B:\n  Other:\n    code:int\n"
                            "fn source()->int ! (A | B):\n  fail A.Retry {code:1}\n"
                            "fn main()->int ! B:\n  source() catch fn(error):\n    match error:\n"
                            "      A.Retry {code}:code\n      _:fail error\n");
    require(narrowed.ok(), "error narrowing plan source rejected");
    for (bool preserved : {false, true}) {
      auto planned = plan(*narrowed.module, {.preserveCalls = preserved});
      require(bool(planned), "error narrowing planning failed");
      bool found = false;
      for (const auto &entry : planned->plans)
        for (const auto &op : entry->code)
          if (op.source < narrowed.module->expressions.size() &&
              std::holds_alternative<ErrorNarrow>(
                  narrowed.module->expressions[op.source].operation)) {
            require(op.code == (preserved ? Code::Kernel : Code::Field),
                    "error narrowing plan used the wrong backend path");
            found = true;
          }
      require(found, "error narrowing missing from plan");
      verify(*planned);
    }
    std::string large = "fn main() -> float32 ! TaskError:\n  try parallel:\n    "
                        "data:Tensor[float32,21000]=tensor([";
    for (unsigned i = 0; i < 21000; ++i)
      large += i ? ",1" : "1";
    large += "])\n    reduceSum(data)\n";
    auto checkedLarge = analyze(large);
    require(checkedLarge.ok(), "large constant tensor checking failed");
    auto embedded = plan(*checkedLarge.module, {.preserveCalls = true, .entry = 0});
    require(bool(embedded) && embedded->plans[0]->slots.size() < 20,
            "embedded constants consumed continuation frame slots");
    verify(*embedded);
    std::cout << fixtures
              << " source fixtures: shared group/branch/failure edges, typed placement, evaluation "
                 "order, deterministic plans and CPU capability/recursion guards passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
