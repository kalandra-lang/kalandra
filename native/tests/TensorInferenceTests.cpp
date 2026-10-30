#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("inference.k", text)), limits);
}
static void accepted(const std::string &body, NumericType element,
                     std::vector<std::uint64_t> shape) {
  auto typed = analyze("fn probe():\n" + body + "\nfn main()->int64:\n  0\n");
  if (!typed.ok())
    throw std::runtime_error(renderDiagnostic(*typed.source, typed.diagnostics.front()));
  const auto &type = typed.module->functions[0].resultType;
  require(type.tensor && type.element == element && type.shape.size() == shape.size(),
          "incorrect inferred tensor identity/rank: " + body);
  for (std::size_t i = 0; i < shape.size(); ++i)
    require(type.shape[i].constant == shape[i], "incorrect inferred extent: " + body);
  require(bool(continuation::plan(*typed.module)), "inferred tensor continuation planning failed");
#ifndef KALANDRA_INFERENCE_FRONTEND_ONLY
  auto native = lowerNative(*typed.module);
  require(bool(native), "inferred tensor lowering failed: " + body);
#endif
}
int main() {
  try {
    accepted("  tensor([1,2,3])", NumericType::Int64, {3});
    accepted("  tensor([[-9223372036854775808,9223372036854775807]])", NumericType::Int64, {1, 2});
    accepted("  tensor([1.0,-0.0,4.9406564584124654e-324])", NumericType::Float64, {3});
    accepted("  tensor(1)", NumericType::Int64, {});
    accepted("  tensor(-0.0)", NumericType::Float64, {});
    accepted("  tensor([1,2,3,4],[2,2])", NumericType::Int64, {2, 2});
    accepted("  tensor([7],[])", NumericType::Int64, {});
    accepted("  values=tensor([[1,2],[3,4]])\n  values+values", NumericType::Int64, {2, 2});
    accepted("  tensor([1+2,3*4,-5])", NumericType::Int64, {3});
    accepted("  tensor([1+2.5,-3.0])", NumericType::Float64, {2});
    accepted("  values:Tensor[float32,2]=tensor([1,2])\n  values", NumericType::Float32, {2});
    accepted("  values:Tensor[float64,2]=tensor([1,2.5])\n  values", NumericType::Float64, {2});
    accepted("  values:Tensor[uint64,2,0]=tensor([],[2,0])\n  values", NumericType::UInt64, {2, 0});
    for (auto type : {NumericType::Int32, NumericType::UInt32, NumericType::Int64,
                      NumericType::UInt64, NumericType::Float32, NumericType::Float64}) {
      const auto name = std::string(describeNumeric(type)->name);
      accepted("  a=" + name + "(2)\n  tensor([[a,a+1],[a*2,a]])", type, {2, 2});
      accepted("  tensor([" + name + "(1)," + name + "(2)])", type, {2});
      accepted("  if true:\n    tensor([" + name + "(1)])\n  else:\n    tensor([" + name + "(2)])",
               type, {1});
    }
    struct Invalid {
      const char *body;
      const char *code;
    };
    for (auto test : {Invalid{"tensor([])", "S026"},
                      {"tensor([[],[]])", "S026"},
                      {"tensor([],[0,3])", "S026"},
                      {"tensor([1,2.5])", "S010"},
                      {"tensor([2.5,1])", "S010"},
                      {"tensor([int32(1),2])", "S010"},
                      {"tensor([1,int32(2)])", "S010"},
                      {"tensor([true,false])", "S026"},
                      {"tensor([tensor([1])])", "S026"},
                      {"tensor([[1],[2,3]])", "S023"},
                      {"tensor([1,2],[2,2])", "S050"},
                      {"tensor([1],[-1])", "S050"},
                      {"tensor([1],[9223372036854775808])", "S001"},
                      {"tensor([1],[9223372036854775807,2])", "S002"},
                      {"tensor([9223372036854775808])", "S051"},
                      {"tensor([1.8e308])", "S015"}}) {
      auto typed = analyze(std::string("fn main():\n  ") + test.body + "\n");
      require(!typed.ok() &&
                  std::ranges::any_of(typed.diagnostics,
                                      [&](const auto &d) { return d.code == test.code; }),
              std::string("missing expected diagnostic: ") + test.body);
    }
    auto typed = analyze("fn probe(work:int64):\n  tensor([work,2.0])\n");
    require(!typed.ok() && typed.source->location(typed.diagnostics[0].span.begin) ==
                               std::pair<std::size_t, std::size_t>{2, 16},
            "mixed element diagnostic location lost");
    auto joined =
        analyze("fn choose(flag:Bool):\n  if flag:\n    tensor([1])\n  else:\n    tensor([2,3])\n");
    require(joined.ok() && joined.module->functions[0].resultType.tensor &&
                joined.module->functions[0].resultType.element == NumericType::Int64 &&
                joined.module->functions[0].resultType.shape.size() == 1 &&
                !joined.module->functions[0].resultType.shape[0].constant,
            "inferred branch shape join lost");
    for (auto source :
         {"fn identity(x:int64)->int64:\n  x\nfn main() ! TaskError:\n  try parallel:\n    "
          "work=spawn identity(1)\n    values=tensor([work])\n    try await work\n",
          "fn identity(x:int64)->int64:\n  x\nfn main() ! TaskError:\n  try parallel:\n    "
          "work=spawn identity(1)\n    values=tensor([await work])\n    0\n"}) {
      auto rejected = analyze(source);
      require(!rejected.ok(), "incomplete task/error accepted as inferred tensor data");
    }
    SemanticLimits limits;
    limits.maxTensorElements = 2;
    require(!analyze("fn main():\n  tensor([1,2,3])\n", limits).ok(), "element budget bypassed");
    std::cout
        << "tensor inference: default types, explicit context, six numeric identities, shapes, "
           "strict mixed-type rejection, source locations and bounded input passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
