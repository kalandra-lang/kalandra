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
  return check(parse(std::make_shared<SourceFile>("concat.k", text)));
}
int main() {
  try {
    for (auto element :
         {"int32", "uint32", "int64", "uint64", "float32", "float64", "Bool", "String", "Row"}) {
      auto result = analyze(
          std::string("type Row:\n  texts:[String]\n  values:[int]\nfn join(a:[") + element +
          "],b:[" + element + "])->[" + element + "]:\n  concat(a,b)\nfn main()->int:\n  0\n");
      require(result.ok(), "array concat type rejected");
      require(dumpTypedIR(*result.module).find("array.concat checked") != std::string::npos,
              "concat IR missing");
      require(bool(continuation::plan(*result.module)), "concat planning failed");
      require(bool(numericCapabilityError(*result.module, NumericTarget::AppleGpu)),
              "GPU concat enabled");
#ifndef KALANDRA_ARRAY_CONCAT_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *source :
         {"fn main():\n  concat([1,2],[3])\n", "fn main():\n  concat([],[1])\n",
          "fn main():\n  concat([1],[])\n", "fn main()->[float32]:\n  concat([],[])\n",
          "fn main()->[float32]:\n  concat([1],[2])\n",
          "fn main()->[String]:\n  concat([],[\"é\"])\n",
          "fn main():\n  [1,2]->concat([3])->slice(1,3)->map(fn(x):x*2)\n",
          "fn main()->[int] ! TaskError:\n  try parallel:\n    work=spawn concat([1],[2,3])\n    "
          "a=try await work\n    b=try await work\n    concat(a,b)\n",
          "fn concat(a:int,b:int)->int:\n  a+b\nfn main():\n  concat(20,22)\n"}) {
      auto result = analyze(source);
      require(result.ok(), std::string("valid concat rejected: ") + source);
      require(bool(continuation::plan(*result.module)), "concat composition failed");
#ifndef KALANDRA_ARRAY_CONCAT_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *source :
         {"fn main():\n  concat([],[])\n", "fn main():\n  concat([int32(1)],[2])\n",
          "fn main():\n  concat([1],[float(2)])\n", "fn main():\n  concat([1],[true])\n",
          "fn main():\n  concat(\"a\",\"b\")\n", "fn main():\n  concat(tensor([1]),[2])\n",
          "fn main():\n  concat([1],tensor([2]))\n", "fn main():\n  concat([1])\n",
          "fn main():\n  concat([1],[2],[3])\n",
          "fn main()->[float32]:\n  a=[1]\n  concat(a,[2])\n",
          "type A:\n  x:int\ntype B:\n  x:int\nfn main():\n  concat([A {x:1}],[B {x:2}])\n",
          "fn main():\n  concat([1],[])[1]\n"})
      require(!analyze(source).ok(), std::string("invalid concat accepted: ") + source);
    for (unsigned mode = 0; mode < 13; ++mode) {
      auto result =
          analyze("fn join(a:[int],b:[int])->[int]:\n  concat(a,b)\nfn main()->int:\n  0\n");
      auto &node = result.module->expressions[result.module->functions[0].result];
      auto &op = std::get<ArrayConcat>(node.operation);
      if (mode == 0)
        op.left = SIZE_MAX;
      if (mode == 1)
        op.right = SIZE_MAX;
      if (mode == 2)
        node.type.array = false;
      if (mode == 3)
        node.type.element = NumericType::Float64;
      if (mode == 4)
        node.type.shape.clear();
      if (mode == 5)
        node.type.task = true;
      if (mode == 6)
        result.module->expressions[op.left].type.fallible = true;
      if (mode == 7)
        result.module->expressions[op.right].type.element = NumericType::UInt64;
      if (mode == 8)
        result.module->expressions[op.right].type.array = false;
      if (mode == 9)
        node.type.device = 0;
      if (mode == 10)
        node.type.builtin = BuiltinValue::String;
      if (mode == 11)
        node.type.record = 0;
      if (mode == 12)
        node.type.shape[0].constant = UINT64_MAX;
      require(bool(numericCapabilityError(*result.module, NumericTarget::Cpu)),
              "malformed concat IR accepted");
#ifndef KALANDRA_ARRAY_CONCAT_FRONTEND_ONLY
      require(!lowerNative(*result.module), "malformed concat reached backend");
#endif
    }
    auto limited =
        analyze("fn join(a:[int])->[int]:\n  large=array(tensor(a,[1152921504606846975]))\n  "
                "concat(large,large)\nfn main()->int:\n  0\n");
    require(!limited.ok(), "constant combined byte count overflow admitted");
    std::cout << "Array concat: nine element types, empty contexts, strict identities, direct "
                 "dispatch and malformed IR gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
