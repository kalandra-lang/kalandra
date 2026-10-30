#include "kalandra/Backend.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("int64.k", source)));
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
#include "Integer64TensorChecks.h"
int main() {
  try {
    checkInteger64Tensors("int64", true);
    for (const auto *value : {"9223372036854775807", "-9223372036854775808", "9007199254740993",
                              "+17", "-5 % 2", "5 % -2", "-9223372036854775808 % -1",
                              "3037000499 * 3037000499", "9223372036854775807 * 0"}) {
      auto checked = analyze(std::string("fn main() -> int64:\n  ") + value + "\n");
      require(checked.ok(), value);
#ifndef KALANDRA_INT64_FRONTEND_ONLY
      auto lowered = lowerNative(*checked.module);
      require(bool(lowered), "int64 lowering rejected");
      require(lowered->llvm.find("_mlir_ciface_kalandra_int64_entry") != std::string::npos,
              "int64 CLI entry lost");
#endif
    }
    for (const auto *value :
         {"9223372036854775808", "-9223372036854775809", "18446744073709551616", "1.0", "1e0",
          "9223372036854775807 + 1", "-9223372036854775808 - 1", "-9223372036854775808 * -1",
          "-(-9223372036854775808)", "-9223372036854775808 / -1", "3037000500 * 3037000500",
          "9223372036854775807 * 9223372036854775807", "1 / 0", "1 % 0"}) {
      auto checked = analyze(std::string("fn main() -> int64:\n  ") + value + "\n");
      require(!checked.ok(), value);
      require(checked.diagnostics.front().code == "S051" ||
                  checked.diagnostics.front().code == "S052",
              "wrong int64 constant diagnostic");
    }
    for (const auto *source :
         {"fn main() -> int:\n  x = 9007199254740993\n  x + 1\n",
          "fn main():\n  -9223372036854775808\n",
          "fn f(x:int) -> int64:\n  x + 1\nfn main() -> int:\n  f(9007199254740993)\n"}) {
      auto checked = analyze(source);
      require(checked.ok(), "int alias or default integer inference rejected");
      require(checked.module->functions.back().resultType.element == NumericType::Int64,
              "default integer lost int64 identity");
#ifndef KALANDRA_INT64_FRONTEND_ONLY
      require(bool(lowerNative(*checked.module)), "inferred int64 lowering rejected");
#endif
    }
    for (const auto *source :
         {"fn f(x:int32) -> int64:\n  x\n", "fn f(x:int64) -> int32:\n  x\n",
          "fn f(x:int64,y:uint32) -> int64:\n  x + y\n",
          "fn f(x:int64,y:float32) -> int64:\n  x + y\n", "fn f() -> uint16:\n  65536\n",
          "fn f() -> int16:\n  1.0\n", "fn main() -> int:\n  x = 9223372036854775807\n  x + 1\n",
          "fn f(x:int64) -> int64:\n  z = 0\n  x / z\n"})
      require(!analyze(source).ok(), "unsupported conversion/type or known overflow accepted");
    auto task = analyze("fn f(x:int64) -> int64:\n  x + 1\nfn main() -> int ! TaskError:\n"
                        "  try parallel:\n    try await spawn f(9007199254740993)\n");
    require(task.ok(), "int64 task rejected");
#ifndef KALANDRA_INT64_FRONTEND_ONLY
    auto lowered = lowerNative(*task.module);
    require(bool(lowered) && lowered->llvm.find("value.layout.scalar.int64") != std::string::npos,
            "int64 task layout lost");
#endif
    require(!numericCapabilityError(*task.module, NumericTarget::AppleGpu),
            "supported int64 GPU numeric capability rejected");
    std::cout << "Checked int64 literals/constants, int alias/default inference, scalar task "
                 "identity and strict tensor/type/target gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
