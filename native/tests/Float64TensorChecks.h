#pragma once
static void checkFloat64Tensors() {
  for (auto expression :
       {"a+b", "a-b", "a*b", "a/b", "a+1.5", "1.5-a", "a*2", "2/a", "-a", "+a", "a*b+a"}) {
    auto typed = analyze(
        std::string("fn f(a:Tensor[float,?,?],b:Tensor[float64,?,?]) -> Tensor[float64,?,?]:\n  ") +
        expression + "\nfn main() -> float:\n  0\n");
    require(typed.ok(), expression);
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
    auto native = lowerNative(*typed.module, {}, {}, ArithmeticMode::Software);
    require(bool(native), "float64 tensor backend rejected");
    require(native->llvm.find("fadd double") == std::string::npos &&
                native->llvm.find("fmul double") == std::string::npos,
            "float64 tensor arithmetic bypassed strict lowering");
#endif
    require(!numericCapabilityError(*typed.module, NumericTarget::AppleGpu),
            "float64 tensor GPU numerical capability rejected");
  }
  for (auto shape : {"", ",4", ",2,2", ",2,2,1"}) {
    const auto values =
        std::string(shape).empty() ? "tensor(-0.0)"
        : std::string(shape) == ",2,2"
            ? "tensor([[-0.0,1.0000000000000002],[4.9406564584124654e-324,1.7976931348623157e308]])"
        : std::string(shape) == ",2,2,1"
            ? "tensor([-0.0,1.0000000000000002,4.9406564584124654e-324,1.7976931348623157e308],[2,"
              "2,1])"
            : "tensor([-0.0,1.0000000000000002,4.9406564584124654e-324,1.7976931348623157e308])";
    auto typed = analyze("fn values() -> Tensor[float" + std::string(shape) + "]:\n  " + values +
                         "\nfn main() -> float:\n  0\n");
    require(typed.ok(), "ranked binary64 tensor literal rejected");
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
    require(bool(lowerNative(*typed.module, {}, {}, ArithmeticMode::Software)),
            "ranked binary64 literal lowering rejected");
#endif
  }
  for (auto shape : {"", ",?", ",?,?", ",?,?,?"}) {
    auto typed = analyze("fn f(x:Tensor[float" + std::string(shape) +
                         "]) -> float:\n  reduceSum(x)\nfn main() -> float:\n  0\n");
    require(typed.ok(), "float64 reduction rejected");
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
    auto native = lowerNative(*typed.module, {}, {}, ArithmeticMode::Software);
    require(bool(native), "float64 reduction lowering rejected");
    require(native->llvm.find("fadd double") == std::string::npos,
            "float64 reduction bypassed strict arithmetic");
    for (auto &expression : typed.module->expressions)
      if (std::holds_alternative<ReduceSum>(expression.operation))
        expression.type.element = NumericType::Float32;
    require(!lowerNative(*typed.module, {}, {}, ArithmeticMode::Software),
            "mismatched reduction element type admitted");
#endif
  }
  struct Bad {
    const char *source;
    const char *code;
  };
  for (auto test :
       {Bad{"fn f(x:Tensor[float,?]) -> Tensor[float,?]:\n  x%2\n", "S019"},
        Bad{"fn f(x:Tensor[float,?],y:Tensor[float32,?]) -> Tensor[float,?]:\n  x+y\n", "S017"},
        Bad{"fn f() -> Tensor[float,1]:\n  tensor([1.8e308])\n", "S015"},
        Bad{"fn f(x:Tensor[float,1152921504606846976]) -> float:\n  0\n", "S002"}}) {
    auto typed = analyze(test.source);
    require(!typed.ok() && typed.diagnostics[0].code == test.code, test.source);
  }
  auto limit = analyze("fn f(x:Tensor[float,1152921504606846975]) -> float:\n  0\n");
  require(limit.ok(), "exact eight-byte static byte bound rejected");
  auto matrix = analyze("fn f(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?]:\n  "
                        "matmul(a,b)\nfn main() -> float:\n  0\n");
  require(matrix.ok(), "float64 matmul rejected");
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
  require(bool(lowerNative(*matrix.module)), "float64 matmul lowering rejected");
  auto mismatched = *matrix.module;
  for (auto &expr : mismatched.expressions)
    if (std::holds_alternative<Matmul>(expr.operation))
      expr.type.element = NumericType::Float32;
  require(!lowerNative(mismatched), "mismatched matmul element type admitted");
  for (unsigned policy = 0; policy < 2; ++policy) {
    auto forged = *matrix.module;
    for (auto &expr : forged.expressions)
      if (auto *multiply = std::get_if<Matmul>(&expr.operation)) {
        if (policy == 0)
          multiply->separatelyRoundedProducts = false;
        else
          multiply->order = static_cast<ReductionOrder>(255);
      }
    require(!lowerNative(forged), "unsupported matmul numerical policy admitted");
  }
  auto typed = analyze(
      "fn f(x:Tensor[float,?]) -> Tensor[float,?]:\n  x+1\nfn main() -> float ! TaskError:\n  try "
      "parallel:\n    x:Tensor[float,1]=tensor([1])\n    try await spawn f(x)\n    0\n");
  require(typed.ok(), "float64 tensor task rejected");
  auto native = lowerNative(*typed.module, {}, {}, ArithmeticMode::Software);
  require(bool(native) && native->llvm.find("value.layout.tensor.float64.1") != std::string::npos,
          "float64 tensor task lost typed layout");
#endif
}
