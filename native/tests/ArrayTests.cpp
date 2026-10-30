#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("arrays.k", text)), limits);
}
static void accept(const std::string &source, NumericType element) {
  auto typed = analyze(source);
  if (!typed.ok())
    throw std::runtime_error(renderDiagnostic(*typed.source, typed.diagnostics.front()));
  const auto type = typed.module->functions[0].resultType;
  require(type.array && type.tensor && type.shape.size() == 1 && type.element == element,
          "array source identity or storage rank lost");
  require(describeType(type) == "[" + std::string(describeNumeric(element)->name) + "]",
          "array length leaked into source type");
  require(bool(continuation::plan(*typed.module)), "array continuation planning failed");
  require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
          "GPU arrays admitted");
#ifndef KALANDRA_ARRAY_FRONTEND_ONLY
  auto native = lowerNative(*typed.module);
  require(bool(native), "array native lowering failed");
#endif
}
int main() {
  try {
    accept("fn values():\n  [1,2,3]\nfn main()->int64:\n  0\n", NumericType::Int64);
    accept("fn values():\n  [-0.0,4.9406564584124654e-324]\nfn main()->int64:\n  0\n",
           NumericType::Float64);
    for (auto element : {NumericType::Int32, NumericType::UInt32, NumericType::Int64,
                         NumericType::UInt64, NumericType::Float32, NumericType::Float64}) {
      auto name = std::string(describeNumeric(element)->name);
      accept("fn values(a:" + name + ")->[" + name + "]:\n  [a,a+1]\nfn main()->int64:\n  0\n",
             element);
      accept("fn empty()->[" + name + "]:\n  []\nfn main()->int64:\n  0\n", element);
      accept("fn values():\n  a=" + name + "(1)\n  [a,a]\nfn main()->int64:\n  0\n", element);
    }
    accept("fn values()->[float32]:\n  [1,2,3]\nfn main()->int64:\n  0\n", NumericType::Float32);
    accept("fn values()->[float64]:\n  [1,2.5]\nfn main()->int64:\n  0\n", NumericType::Float64);
    auto choice = analyze("fn values(flag:Bool):\n  if flag:\n    [1]\n  else:\n    [2,3]\n");
    require(choice.ok() && choice.module->functions[0].resultType.array &&
                !choice.module->functions[0].resultType.shape[0].constant,
            "array branch length join lost");
    auto syntax = parse(
        std::make_shared<SourceFile>("arrays.k", "fn values(a:[int])->[float]:\n  []\n"));
    require(syntax.ok() && syntax.module.functions[0].parameters[0].type.array &&
                syntax.module.functions[0].result->array,
            "array annotation AST lost");
    for (auto source :
         {"fn main():\n  []\n",
          "fn main():\n  [1,2.0]\n",
          "fn main():\n  [2.0,1]\n",
          "fn main():\n  [[1],[2]]\n",
          "fn main()->[int128]:\n  []\n",
          "fn main()->[[int]]:\n  []\n",
          "fn main()->[int]:\n  tensor([1,2])\n",
          "fn main()->Tensor[int64,2]:\n  [1,2]\n",
          "fn main():\n  a=[1,2]\n  a+a\n",
          "fn main():\n  a=[1,2]\n  -a\n",
          "fn main():\n  a=[1,2]\n  int64(a)\n",
          "fn main():\n  a=[1,2]\n  reduceSum(a)\n",
          "fn main():\n  a=[1,2]\n  tensor(a,[2,2])\n",
          "fn main():\n  a=[1,2]\n  a[0]=3\n",
          "fn main():\n  a=[1,2]\n  a==a\n",
          "fn main():\n  if true:\n    [1]\n  else:\n    tensor([1])\n",
          "fn use(a:Tensor[int64,?])->int64:\n  0\nfn main()->int64:\n  a=[1,2]\n  use(a)\n",
          "fn use(a:[int])->int64:\n  0\nfn main()->int64:\n  a=tensor([1,2])\n  use(a)\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn identity(1)\n    [work]\nfn "
          "identity(a:int)->int:\n  a\n"}) {
      require(!analyze(source).ok(), std::string("invalid array source accepted: ") + source);
    }
    SemanticLimits limits;
    limits.maxTensorElements = 2;
    require(!analyze("fn values():\n  [1,2,3]\n", limits).ok(), "array element budget bypassed");
    auto invalid = analyze("fn values(a:int):\n  [a,2.0]\n");
    require(!invalid.ok() && invalid.source->location(invalid.diagnostics.front().span.begin) ==
                                 std::pair<std::size_t, std::size_t>{2, 6},
            "array element diagnostic location lost");
#ifndef KALANDRA_ARRAY_FRONTEND_ONLY
    auto typed = analyze("fn values()->[int]:\n  [1,2]\nfn main()->int64:\n  0\n");
    for (unsigned surface = 0; surface < 3; ++surface) {
      auto forged = *typed.module;
      auto &type = surface == 0   ? forged.functions[0].resultType
                   : surface == 1 ? forged.expressions[0].type
                                  : forged.expressions[forged.functions[0].result].type;
      type.array = true;
      type.tensor = false;
      require(!lowerNative(forged), "forged array storage admitted");
    }
    auto flow = analyze("fn copy(a:[int])->[int]:\n  a\nfn values()->[int]:\n  copy([1,2])\nfn "
                        "main()->int64:\n  0\n");
    auto forged = *flow.module;
    forged.symbols[forged.functions[0].parameters[0]].type.array = false;
    require(!lowerNative(forged), "forged implicit tensor/array value flow admitted");
#endif
    std::cout << "immutable arrays: six numeric element types, contexts, empty data, source "
                 "categories, branch lengths, worker planning and rejection gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
