#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("reshape.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (auto dimension : {"int32", "uint32", "int64", "uint64"}) {
        auto typed = analyze("fn reshape(values:[" + std::string(type) + "],rows:" + dimension +
                             ",columns:" + dimension +
                             "):\n  tensor(values,[rows,columns])\nfn main()->int:\n  0\n");
        require(typed.ok(), "numeric/dimension type pair rejected");
        const auto &node = typed.module->expressions[typed.module->functions[0].result];
        const auto &op = std::get<ArrayToTensor>(node.operation);
        require(node.type.tensor && !node.type.array && node.type.element == resolveNumeric(type) &&
                    node.type.shape.size() == 2 && op.dimensions && op.dimensions->size() == 2,
                "reshape type/rank/operands lost");
        require(!node.type.shape[0].constant && !node.type.shape[1].constant,
                "runtime dimensions became static");
        require(bool(continuation::plan(*typed.module)), "reshape planning failed");
#ifndef KALANDRA_RESHAPE_FRONTEND_ONLY
        require(bool(lowerNative(*typed.module)), "reshape numeric/dimension lowering failed");
#endif
      }
    for (auto text : {"fn run(values:[int]):\n  tensor(values,[2,2])\n",
                      "fn run(values:[int]):\n  tensor(values,[])\n",
                      "fn run(values:[int],n:int):\n  tensor(values,[n,0,2])\n",
                      "fn run(rows:int,columns:int):\n  tensor([1,2,3,4],[rows,columns])\n",
                      "fn run(rows:int,columns:int)->Tensor[float32,?,?]:\n  "
                      "tensor([1,2,3,4],[rows,columns])\n",
                      "fn run(values:[int],rows:int32,columns:uint64)->Tensor[int,2,2]:\n  "
                      "tensor(values,[rows,columns])\n",
                      "fn run(values:[int],rows:int,columns:int) ! TaskError:\n  try parallel:\n   "
                      " work=spawn tensor(values,[rows,columns])\n    first=try await work\n    "
                      "again=try await work\n    again\n",
                      "fn run(values:[int],side:int):\n  map(values,fn(x): "
                      "reduceSum(tensor(map([x],fn(y): y+x),[side,side])))\n"}) {
      auto typed = analyze(std::string(text) + "fn main()->int:\n  0\n");
      require(typed.ok(), std::string("valid reshape rejected: ") + text);
      require(bool(continuation::plan(*typed.module)), "reshape helper import failed");
#ifndef KALANDRA_RESHAPE_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered),
              "reshape lowering failed: " + (lowered ? std::string{} : lowered.error()));
#endif
    }
    for (auto text :
         {"fn run(values:[int]):\n  tensor(values,[-1,2])\n",
          "fn run(values:[int]):\n  tensor(values,[uint64(18446744073709551615),0])\n",
          "fn run(values:[int]):\n  tensor(values,[9223372036854775807,2])\n",
          "fn run(values:[int]):\n  tensor(values,[0,9223372036854775807,9223372036854775807])\n",
          "fn run(values:[int],rows:float):\n  tensor(values,[rows,2])\n",
          "fn run(values:[int],rows:Bool):\n  tensor(values,[rows,2])\n",
          "fn run(values:[int],shape:[int]):\n  tensor(values,shape)\n",
          "fn run():\n  values=[1,2,3]\n  tensor(values,[2,2])\n",
          "fn run()->Tensor[float32,2,2]:\n  values=[1,2,3,4]\n  tensor(values,[2,2])\n",
          "fn run(values:[int],rows:int)->Tensor[int,?,?]:\n  tensor(values,[rows])\n"})
      require(!analyze(text).ok(), std::string("invalid reshape accepted: ") + text);
#ifndef KALANDRA_RESHAPE_FRONTEND_ONLY
    auto good =
        analyze("fn run(values:[int],n:int):\n  tensor(values,[n,n])\nfn main()->int:\n  0\n");
    for (unsigned surface = 0; surface < 4; ++surface) {
      auto forged = *good.module;
      auto &node = forged.expressions[forged.functions[0].result];
      auto &op = std::get<ArrayToTensor>(node.operation);
      if (surface == 0)
        (*op.dimensions)[0] = SIZE_MAX;
      if (surface == 1)
        op.dimensions->pop_back();
      if (surface == 2)
        forged.expressions[(*op.dimensions)[0]].type.element = NumericType::Float64;
      if (surface == 3)
        node.type.shape.pop_back();
      require(!lowerNative(forged), "malformed reshape IR admitted");
    }
#endif
    std::cout << "reshape: 24 element/dimension pairs, fixed/dynamic/empty shapes, callback/worker "
                 "planning, rank/type/count/overflow rejections and malformed IR gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
