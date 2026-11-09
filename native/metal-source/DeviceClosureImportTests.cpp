#include "NumericShader.h"
#include "NumericProgram.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    auto checked = check(parse(std::make_shared<SourceFile>("device-import.k", R"k(
fn make(offset:i64)->fn(i64)->i64:
  return fn(value:i64):return value+offset
fn invoke(callback:fn(i64)->i64,n:i64)->i64:
  return callback(n)
fn main()->i64:
  return invoke(make(7),5)
)k")));
    require(checked.ok(), "imported closure fixture rejected");
    const auto &input = *checked.module;
    FunctionId entry = input.functions.size(), target = entry;
    for (FunctionId id = 0; id < input.functions.size(); ++id) {
      if (input.functions[id].name == "invoke")
        entry = id;
      if (input.functions[id].captureCount)
        target = id;
    }
    require(entry < input.functions.size() && target < input.functions.size(), "missing closure fixture identities");
    const DeviceClosureImport valid{target, 1000, 1001, 2, 1};
    const auto emit = [&](std::span<const DeviceClosureImport> imports) {
      return emitScalarControlShader(input, entry, ArithmeticMode::Hardware, false, false, 0, {}, {}, imports);
    };
    require(!emit({}), "external callable admitted without import schema");
    auto shader = emit(std::span(&valid, 1));
    if (!shader)
      throw std::runtime_error(shader.error().message);
    require(shader->deviceStorage && shader->mlir.find("gpu.func") != std::string::npos,
            "closure import did not emit device storage/control IR");
    const auto program = emitNumericProgram(input, entry, ArithmeticMode::Hardware, false,
                                            false, 0, {}, {}, std::span(&valid, 1));
    if (!program)
      throw std::runtime_error(program.error().message);
    require(std::ranges::any_of(program->code, [](const auto &op) {
              return op.kind == NumericProgramKind::DeviceControl;
            }), "imported closure program did not retain device execution");
    for (auto invalid : {DeviceClosureImport{target, 1, 1001, 2, 1},
                         DeviceClosureImport{target, 1000, 1000, 2, 1},
                         DeviceClosureImport{target, 1000, 1001, 2, 2},
                         DeviceClosureImport{target, 1000, 0, 2, 1},
                         DeviceClosureImport{input.functions.size(), 1000, 1001, 2, 1}})
      require(!emit(std::span(&invalid, 1)), "malformed device closure import admitted");
    const std::array duplicate{valid, valid};
    require(!emit(duplicate), "duplicate closure import target admitted");
    std::cout << "device imported closure target, environment and graph entry IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
