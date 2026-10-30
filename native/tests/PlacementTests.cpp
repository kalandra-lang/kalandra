#include "kalandra/Backend.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
CheckResult analyze(std::string source, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("placement.k", std::move(source))), limits,
               {.placement = true});
}
void accepted(const CheckResult &result) {
  if (!result.ok()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << renderDiagnostic(*result.source, diagnostic);
    throw std::runtime_error("expected placement-aware typed IR");
  }
}
unsigned rejectedCases = 0;
void reject(const std::string &source, std::string_view code, SemanticLimits limits = {}) {
  auto result = analyze(source, limits);
  require(!result.ok() && !result.module, "partial IR published on a placement error");
  if (!std::ranges::any_of(result.diagnostics, [&](const auto &d) { return d.code == code; })) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << renderDiagnostic(*result.source, diagnostic);
    throw std::runtime_error("missing placement diagnostic: " + std::string(code));
  }
  ++rejectedCases;
}
const std::string helpers =
    R"(fn scale(values:Tensor[float32,?], factor:float32) -> Tensor[float32,?]:
  values*factor

fn total(values:Tensor[float32,?]) -> float32:
  reduceSum(values)

fn fixed(values:Tensor[float32,2]) -> float32:
  reduceSum(values)

fn forward(values:Tensor[float32,?]) -> Tensor[float32,?]:
  values

)";
const std::string start = helpers + R"(fn main() -> float32 ! TaskError:
  try parallel:
    values:Tensor[float32,3] = tensor([1,2,3])
    cpu=devices.cpu()
    gpu=devices.gpu()
    input=try await transfer(values,gpu)
)";
void verify(const TypedModule &module) {
  std::vector<FunctionId> owners(module.expressions.size(), module.functions.size());
  for (FunctionId f = 0; f < module.functions.size(); ++f)
    for (auto id : module.functions[f].expressions) {
      require(id < owners.size() && owners[id] == module.functions.size(),
              "invalid typed value ownership");
      owners[id] = f;
    }
  for (ValueId id = 0; id < module.expressions.size(); ++id) {
    const auto &e = module.expressions[id];
    require(owners[id] < module.functions.size(), "unowned typed value");
    auto operand = [&](ValueId child) {
      require(child < id && owners[child] == owners[id], "invalid typed value dependency");
    };
    if (e.type.device)
      require(e.type.tensor || e.type.builtin == BuiltinValue::Device,
              "a scalar numeric value became device-resident");
    if (auto *device = std::get_if<DeviceSelection>(&e.operation)) {
      require(e.type.builtin == BuiltinValue::Device && e.type.device == device->device,
              "device selection type lost its exact identity");
    } else if (auto *transfer = std::get_if<Transfer>(&e.operation)) {
      operand(transfer->input);
      operand(transfer->target);
      const auto input = module.expressions[transfer->input].type;
      const auto target = module.expressions[transfer->target].type;
      require(input.tensor && !input.task && !input.fallible &&
                  target.builtin == BuiltinValue::Device && e.type.task &&
                  e.type.device == target.device && e.type.shape.size() == input.shape.size(),
              "invalid transfer result contract");
      for (std::size_t axis = 0; axis < input.shape.size(); ++axis)
        require(input.shape[axis].constant == e.type.shape[axis].constant &&
                    input.shape[axis].identity == e.type.shape[axis].identity,
                "transfer changed shape facts");
    } else if (auto *task = std::get_if<TaskOperation>(&e.operation)) {
      operand(task->operand);
      const auto source = module.expressions[task->operand].type;
      if (task->prefix == TaskPrefix::Spawn) {
        require(task->placement.has_value(), "implicit CPU spawn has no checked target");
        operand(*task->placement);
        const auto target = module.expressions[*task->placement].type;
        require(target.builtin == BuiltinValue::Device, "spawn target lost its selector type");
        if (e.type.tensor)
          require(e.type.device == target.device, "spawn result lost residence");
        auto *call = std::get_if<FunctionCall>(&module.expressions[task->operand].operation);
        if (!call) {
          require(
              !target.device &&
                  (std::holds_alternative<ReduceSum>(module.expressions[task->operand].operation) ||
                   std::holds_alternative<Matmul>(module.expressions[task->operand].operation)),
              "unsupported placed builtin invocation");
          continue;
        }
        for (auto arg : call->arguments) {
          require(arg < *task->placement, "target evaluated before an argument");
          const auto type = module.expressions[arg].type;
          if (type.tensor)
            require(type.device == target.device, "task capture has wrong residence");
        }
      } else {
        require(!task->placement, "await/try unexpectedly selects a device");
        require(e.type.device == source.device, "observation changed tensor residence");
      }
    }
  }
}
void contracts() {
  auto result = analyze(start + R"(    destination=gpu
    task=spawn scale(input,2) on destination
    resident=try await task
    host=try await transfer(resident,cpu)
    work=spawn scale(host,3)
    result=try await work
    scalar=try await spawn total(input) on gpu
    reduceSum(result)+scalar
)");
  accepted(result);
  verify(*result.module);
  auto dump = dumpTypedIR(*result.module);
  require(hasPlacement(*result.module) && dump.find("task.transfer") != std::string::npos &&
              dump.find("Device[cpu]") != std::string::npos &&
              dump.find("on gpu:0") != std::string::npos,
          "placement facts are absent from typed IR");
  auto lowered = lowerNative(*result.module);
  require(!lowered && lowered.error().find("placement-capable") != std::string::npos,
          "CPU backend silently discarded placement IR");
  auto builtin = analyze(start + "    try await spawn reduceSum(values)\n");
  accepted(builtin);
  verify(*builtin.module);
  // A named CPU ABI exposes its declared result dimensions. Unknown fixed
  // parameter checks belong to the spawned child, not an eager parent guard.
  auto child = analyze(start + "    value=try await spawn fixed(forward(values))\n    value\n");
  accepted(child);
  verify(*child.module);
  bool found = false;
  for (const auto &e : child.module->expressions)
    if (auto *spawn = std::get_if<TaskOperation>(&e.operation);
        spawn && spawn->prefix == TaskPrefix::Spawn) {
      const auto &call =
          std::get<FunctionCall>(child.module->expressions[spawn->operand].operation);
      if (child.module->functions[call.function].name == "fixed") {
        const auto &argument = child.module->expressions[call.arguments[0]];
        require(!argument.type.shape[0].constant &&
                    std::holds_alternative<FunctionCall>(argument.operation),
                "child fixed-shape obligation was moved into the supervisor");
        found = true;
      }
    }
  require(found, "missing fixed-shape child call");
  for (const auto &source :
       {std::string("fn main() -> Tensor[float32,0,3]:\n  tensor([], [0,3])\n"),
        std::string("fn main() -> Tensor[float32,2,3]:\n  tensor([1,2,3,4,5,6], [2,3])\n"),
        std::string("fn main() -> Tensor[float32]:\n  tensor([2], [])\n")}) {
    auto literal = analyze(source);
    accepted(literal);
    require(bool(lowerNative(*literal.module, {}, {"main"})),
            "explicit shape did not reuse CPU tensor lowering");
  }
  auto selector =
      analyze("fn main() -> float32:\n  gpu=devices.gpu(4294967295)\n  alias=gpu\n  0\n");
  accepted(selector);
  verify(*selector.module);
  require(dumpTypedIR(*selector.module).find("Device[gpu:4294967295]") != std::string::npos,
          "GPU identity was rounded through float32");
  for (auto &e : selector.module->expressions)
    if (std::holds_alternative<SymbolRead>(e.operation))
      require(e.type.device == UINT32_MAX, "selector alias lost its identity");
  // Even explicit CPU selectors may not be silently erased by the old lowerer.
  auto cpu = analyze("fn main() -> float32:\n  cpu=devices.cpu()\n  0\n");
  accepted(cpu);
  require(!lowerNative(*cpu.module), "CPU selector erased by backend");
  auto ordinary = check(parse(std::make_shared<SourceFile>(
      "cpu.k", helpers + "fn main() -> float32:\n  total(tensor([1,2]))\n")));
  accepted(ordinary);
  require(bool(lowerNative(*ordinary.module)), "parameter literal context regressed");
  auto cpuSource =
      helpers + "fn main() -> float32:\n  data:Tensor[float32,2] = tensor([1,2])\n  total(data)\n";
  ordinary = check(parse(std::make_shared<SourceFile>("cpu.k", cpuSource)));
  accepted(ordinary);
  require(!hasPlacement(*ordinary.module), "ordinary CPU IR changed kind");
  require(bool(lowerNative(*ordinary.module)), "ordinary CPU lowering regressed");
  auto cpuDefault = check(parse(std::make_shared<SourceFile>(
      "cpu.k", start + "    try await spawn total(values) on cpu\n")));
  require(!cpuDefault.ok(), "ordinary CPU frontend silently enabled placement");
}
void failures() {
  for (const auto *selector : {"devices.cpu()", "devices.gpu()"}) {
    const std::string target = selector;
    reject("fn double(value:int)->int:\n  value*2\nfn main()->int ! TaskError:\n"
           "  f=double\n  try parallel:\n    try await spawn f(21) on " +
               target + "\n",
           "S074");
    reject("fn double(value:int)->int:\n  value*2\nfn main()->int ! TaskError:\n"
           "  functions=[double]\n  try parallel:\n    try await spawn functions[0](21) on " +
               target + "\n",
           "S074");
  }
  reject(start + "    try await spawn scale(values,2) on gpu\n", "S047");
  reject(start + "    try await spawn scale(input,2) on cpu\n", "S047");
  reject(start + "    try await spawn scale(input,2)\n", "S047");
  reject(start + "    try await spawn scale(input,2) on devices.gpu(1)\n", "S047");
  reject(start + "    scale(input,2)\n", "S047");
  reject(start + "    reduceSum(input)\n", "S047");
  reject(start + "    reduceSum(input+1)\n", "S047");
  reject(start + "    reduceSum(-input)\n", "S047");
  reject(start + "    try await spawn total(values) on 1\n", "S046");
  reject(start + "    try await transfer(values,1)\n", "S047");
  reject(start + "    try await transfer(1,gpu)\n", "S047");
  reject(start + "    pending=transfer(values,gpu)\n    try await transfer(pending,cpu)\n", "S047");
  reject(start + "    ignored=transfer(values,gpu)\n    0\n", "S038");
  reject(start + "    ignored=await transfer(values,gpu)\n    0\n", "S036");
  reject(start + "    try await spawn fixed(values)\n", "S011");
  reject(start + "    try await spawn total(values) on devices.cpu(1)\n", "S027");
  reject(start + "    try await spawn total(values) on devices.gpu(1,2)\n", "S027");
  for (const auto *index : {"-1", "1.5", "1e0", "4294967296", "cpu"})
    reject(start + "    selected=devices.gpu(" + index + ")\n    0\n", "S046");
  reject("fn main() -> float32:\n  devices:float32=1\n  gpu=devices.gpu()\n  0\n", "S030");
  reject("fn devices() -> float32:\n  0\n", "S049");
  reject("fn transfer() -> float32:\n  0\n", "S049");
  reject(start + "    transfer:float32=1\n    try await transfer(values,gpu)\n", "S030");
  reject("fn main() -> float32 ! TaskError:\n  data:Tensor[float32,1] = tensor([1])\n  try await "
         "transfer(data,devices.gpu())\n",
         "S037");
  reject(start + "    ignored=parallel:\n      gpu\n    0\n", "S035");
  reject(helpers + R"(fn main() -> Tensor[float32,3] ! TaskError:
  try parallel:
    data:Tensor[float32,3] = tensor([1,2,3])
    try await transfer(data,devices.gpu())
)",
         "S047");
  reject(start + R"(    outcome=parallel:
      try await spawn scale(input,2) on gpu
    recovered=outcome catch fn(error): values
    host=try await transfer(recovered,cpu)
    reduceSum(host)
)",
         "S047");
  reject("fn main() -> float32:\n  value=1.5\n  value\n", "S010");
  reject("fn main() -> Tensor[float32,0,3]:\n  tensor([])\n", "S010");
  reject("fn main() -> Tensor[float32,2,3]:\n  tensor([1,2,3], [2,3])\n", "S050");
  reject("fn main() -> Tensor[float32,1,2]:\n  tensor([[1,2]], [1,2])\n", "S050");
  reject("fn main() -> Tensor[float32,1]:\n  tensor([1], [-1])\n", "S050");
  reject("fn main() -> Tensor[float32,1]:\n  tensor([1], [1.0])\n", "S050");
  reject("fn main() -> Tensor[float32,1]:\n  tensor([1], [18446744073709551616])\n", "S050");
  reject("fn main() -> Tensor[float32,?]:\n  tensor([], [9223372036854775808])\n", "S001");
  reject("fn main() -> Tensor[float32,?,?]:\n  tensor([], [9223372036854775807,2])\n", "S002");
  reject("fn main() -> Tensor[float32,0,?,?]:\n  tensor([], [0,9223372036854775807,2])\n", "S002");
  auto limits = SemanticLimits{};
  limits.maxValues = 3;
  reject(start + "    reduceSum(values)\n", "S009", limits);
  auto depth = SemanticLimits{};
  depth.maxDepth = 2;
  reject(start + "    reduceSum(values)\n", "S012", depth);
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected repository root");
    contracts();
    failures();
    unsigned fixtures = 0;
    for (const auto *name :
         {"placed", "composed", "mixed", "tensor-results", "shared-helpers", "scalar-entry",
          "matrix-elementwise", "differences", "arguments", "unused-inputs", "indirect-arguments",
          "tensor-negation", "division", "host-expressions", "cpu-spawn", "nested-parallel",
          "recovery", "reduction", "matmul"}) {
      auto source = loadSource(std::string(argv[1]) + "/native/metal-source/" + name + ".k");
      require(bool(source), "missing mixed fixture");
      auto result = check(parse(*source), {}, {.placement = true});
      accepted(result);
      verify(*result.module);
      ++fixtures;
      auto again = check(parse(*source), {}, {.placement = true});
      accepted(again);
      require(dumpTypedIR(*result.module) == dumpTypedIR(*again.module),
              "nondeterministic IR dump");
    }
    std::cout << fixtures << " mixed fixtures and " << rejectedCases
              << " placement rejection cases passed; CPU capability gate and typed dependencies "
                 "verified\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
