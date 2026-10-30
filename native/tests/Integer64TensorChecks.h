#pragma once
// Shared frontend and backend admission checks, instantiated for each signedness.
static void checkInteger64Tensors(const std::string &element, bool backend) {
  const auto tensor = "Tensor[" + element + ",?]";
  for (const auto &expression : {"x + 1", "1 - x", "x * 2", "x / 2", "x % 2", "-x", "+x"}) {
    auto result = analyze("fn f(x:" + tensor + ") -> " + tensor + ":\n  " + expression +
                          "\nfn main() -> " + element + ":\n  0\n");
    require(result.ok(), "64-bit tensor expression rejected");
#if !defined(KALANDRA_INT64_FRONTEND_ONLY) && !defined(KALANDRA_UINT64_FRONTEND_ONLY)
    if (backend)
      require(bool(lowerNative(*result.module)), "64-bit tensor backend rejected");
#else
    (void)backend;
#endif
  }
  const auto invalidLiteral = element == "uint64" ? "-1" : "9223372036854775808";
  require(
      !analyze("fn f() -> Tensor[" + element + ",1]:\n  tensor([" + invalidLiteral + "])\n").ok(),
      "signedness/range of contextual tensor literal lost");
  const auto other = element == "uint64" ? "int64" : "uint64";
  require(
      !analyze("fn f(x:" + tensor + ", y:Tensor[" + other + ",?]) -> " + tensor + ":\n  x + y\n")
           .ok(),
      "implicit tensor signedness conversion admitted");
  auto alias =
      analyze("fn f(x:Tensor[" + (element == "uint64" ? std::string("uint") : std::string("int")) +
              ",?]) -> " + tensor + ":\n  x\nfn main() -> " + element + ":\n  0\n");
  require(alias.ok(), "64-bit tensor alias rejected");
  auto task = analyze("fn identity(x:" + tensor + ") -> " + tensor +
                      ":\n  x\n"
                      "fn main() -> " +
                      element +
                      " ! TaskError:\n  try parallel:\n"
                      "    values:Tensor[" +
                      element +
                      ",1] = tensor([9007199254740993])\n"
                      "    first = spawn identity(values)\n    copied = try await first\n"
                      "    try await first\n    0\n");
  require(task.ok(), "64-bit tensor task typing rejected");
  auto taskReduction = analyze("fn sum(x:" + tensor + ") -> " + element +
                               ":\n  reduceSum(x)\n"
                               "fn main() -> " +
                               element +
                               " ! TaskError:\n  try parallel:\n"
                               "    values:Tensor[" +
                               element +
                               ",1] = tensor([9007199254740993])\n"
                               "    try await spawn sum(values)\n");
  require(taskReduction.ok(), "64-bit worker reduction typing rejected");
#if !defined(KALANDRA_INT64_FRONTEND_ONLY) && !defined(KALANDRA_UINT64_FRONTEND_ONLY)
  if (backend) {
    require(bool(lowerNative(*taskReduction.module)), "64-bit worker reduction lowering rejected");
    auto lowered = lowerNative(*task.module);
    require(bool(lowered) &&
                lowered->llvm.find("value.layout.tensor." + element + ".1") != std::string::npos,
            "64-bit tensor task identity lost");
  }
#endif
  for (const auto &expression : {"matmul(x,x)"})
    require(!analyze("fn f(x:" + tensor + ") -> " + element + ":\n  " + expression + "\n").ok(),
            "unsupported 64-bit tensor builtin admitted");
  auto reduction = analyze("fn sum(x:" + tensor + ") -> " + element +
                           ":\n  reduceSum(x)\n"
                           "fn main() -> " +
                           element + ":\n  0\n");
  require(reduction.ok(), "64-bit reduction typing rejected");
#if !defined(KALANDRA_INT64_FRONTEND_ONLY) && !defined(KALANDRA_UINT64_FRONTEND_ONLY)
  if (backend) {
    auto lowered = lowerNative(*reduction.module);
    require(bool(lowered), "64-bit reduction lowering rejected");
    auto forged = *reduction.module;
    for (auto &expr : forged.expressions)
      if (std::holds_alternative<ReduceSum>(expr.operation))
        expr.type.element = element == "uint64" ? NumericType::Int64 : NumericType::UInt64;
    auto rejected = lowerNative(forged);
    require(!rejected && rejected.error().find("reduction element type") != std::string::npos,
            "backend accepted mismatched reduction signedness");
    const auto marker = element == "uint64" ? "checked_u64" : "checked_i64";
    require(dumpTypedIR(*reduction.module).find(marker) != std::string::npos,
            "64-bit reduction typed identity lost");
  }
#endif
  require(!analyze("fn f() -> Tensor[" + element + ",1152921504606846976]:\n  tensor([])\n").ok(),
          "eight-byte static allocation bound lost");
  require(!analyze("fn f(x:" + tensor + ", y:Tensor[int32,?]) -> " + tensor + ":\n  x + y\n").ok(),
          "implicit tensor width conversion admitted");
  require(!analyze("fn f() -> Tensor[" + element + ",1]:\n  tensor([18446744073709551616])\n").ok(),
          "out-of-range tensor literal admitted");
  const auto matrix = "Tensor[" + element + ",?,?]";
  auto product = analyze("fn product(a:" + matrix + ",b:" + matrix + ") -> " + matrix +
                         ":\n  matmul(a,b)\nfn main() -> " + element + ":\n  0\n");
  require(product.ok(), "64-bit matrix typing rejected");
  auto workerProduct = analyze("fn product(a:" + matrix + ",b:" + matrix + ") -> " + matrix +
                               ":\n  matmul(a,b)\nfn main() -> " + element +
                               " ! TaskError:\n  try parallel:\n"
                               "    values:Tensor[" +
                               element +
                               ",1,1] = tensor([[1]])\n"
                               "    result = try await spawn product(values,values)\n"
                               "    reduceSum(result)\n");
  require(workerProduct.ok(), "64-bit worker matrix typing rejected");
#if !defined(KALANDRA_INT64_FRONTEND_ONLY) && !defined(KALANDRA_UINT64_FRONTEND_ONLY)
  if (backend) {
    require(bool(lowerNative(*product.module)) && bool(lowerNative(*workerProduct.module)),
            "64-bit matrix lowering rejected");
    const auto marker = element == "uint64" ? "checked_u64 balanced_adjacent checked_products"
                                            : "checked_i64 balanced_adjacent checked_products";
    require(dumpTypedIR(*product.module).find(marker) != std::string::npos,
            "64-bit matrix typed identity lost");
    auto forged = *product.module;
    for (auto &expr : forged.expressions)
      if (std::holds_alternative<Matmul>(expr.operation))
        expr.type.element = element == "uint64" ? NumericType::Int64 : NumericType::UInt64;
    auto rejected = lowerNative(forged);
    require(!rejected && rejected.error().find("matmul element type") != std::string::npos,
            "backend accepted mismatched matrix signedness");
  }
#endif
}
