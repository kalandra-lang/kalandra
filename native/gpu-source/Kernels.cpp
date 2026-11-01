#include "NumericProgram.h"
#include "kalandra/Diagnostics.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <filesystem>
#include <optional>
#include <string_view>
using namespace kalandra;
namespace {
llvm::json::Array indices(const auto &values) {
  llvm::json::Array result;
  for (auto value : values)
    result.push_back(static_cast<std::int64_t>(value));
  return result;
}
llvm::json::Array types(const auto &values) {
  llvm::json::Array result;
  for (auto type : values)
    result.push_back(std::string(describeNumeric(type)->name));
  return result;
}
bool write(const std::filesystem::path &path, llvm::StringRef text) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path.string(), error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message() << '\n';
    return false;
  }
  stream << text;
  stream.close();
  return !stream.has_error();
}
} // namespace
// Internal artifact emitter. It deliberately does not claim host/task compiler
// support until the supervisor and structured Vulkan runtime are integrated.
int main(int argc, char **argv) {
  if (argc != 5) {
    llvm::errs()
        << "usage: kalandra-vulkan-kernels source.k function output-directory arithmetic-mode\n";
    return 2;
  }
  auto arithmetic = parseArithmeticMode(argv[4]);
  if (!arithmetic)
    return 2;
  auto loaded = loadModules(argv[1]);
  if (!loaded) {
    llvm::errs() << loaded.error() << '\n';
    return 1;
  }
  auto source = loaded->module.source;
  auto checked = check(std::move(*loaded), {}, {.placement = true});
  if (!checked.ok()) {
    for (const auto &diagnostic : checked.diagnostics)
      llvm::errs() << renderDiagnostic(*source, diagnostic);
    return 1;
  }
  std::optional<FunctionId> selected;
  for (FunctionId i = 0; i < checked.module->functions.size(); ++i)
    if (checked.module->functions[i].name == argv[2]) {
      if (selected) {
        llvm::errs() << "ambiguous function\n";
        return 1;
      }
      selected = i;
    }
  if (!selected) {
    llvm::errs() << "function not found\n";
    return 1;
  }
  auto program = emitNumericProgram(*checked.module, *selected, *arithmetic);
  if (!program) {
    llvm::errs() << renderDiagnostic(*source, program.error());
    return 1;
  }
  for (const auto &kernel : program->kernels)
    if (!kernel.nativeMetal.empty() && !kernel.transform) {
      llvm::errs() << "unsupported native kernel artifact\n";
      return 1;
    }
  auto directory = std::filesystem::path(argv[3]);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    llvm::errs() << error.message() << '\n';
    return 1;
  }
  llvm::json::Array kernels, code, parameters;
  for (std::size_t i = 0; i < program->kernels.size(); ++i) {
    const auto &kernel = program->kernels[i];
    auto file = "kernel" + std::to_string(i) + (kernel.transform ? ".vulkan-transform" : ".mlir");
    const auto contents =
        kernel.transform ? "kalandra-vulkan-transform-v1\nkind=" +
                               std::string(tensorTransformName(*kernel.transform)) +
                               "\ntype=" + std::string(describeNumeric(kernel.element)->name) + "\n"
                         : kernel.mlir;
    if (!write(directory / file, contents))
      return 1;
    kernels.push_back(
        llvm::json::Object{{"file", file},
                           {"type", std::string(describeNumeric(kernel.element)->name)},
                           {"tensorTypes", types(kernel.tensorTypes)},
                           {"scalarTypes", types(kernel.scalarTypes)},
                           {"scalarBuffers", indices(kernel.scalarBuffers)},
                           {"reads", indices(kernel.reads)},
                           {"matrix", kernel.matrix},
                           {"scalar", kernel.scalar},
                           {"noArithmeticFault", kernel.noArithmeticFault}});
  }
  constexpr std::array names{"Map",   "Reduce",     "Matmul", "Branch",      "Jump",
                             "Alias", "EqualShape", "Extent", "MatrixShape", "ShapeFault"};
  for (const auto &op : program->code) {
    llvm::json::Array constants;
    // Decimal strings preserve every uint64 carrier even in non-integer JSON consumers.
    for (auto value : op.constants)
      constants.push_back(std::to_string(value));
    code.push_back(llvm::json::Object{{"kind", names.at(static_cast<unsigned>(op.kind))},
                                      {"kernel", std::int64_t(op.kernel)},
                                      {"inputs", indices(op.inputs)},
                                      {"scalarSources", indices(op.scalarSources)},
                                      {"constants", std::move(constants)},
                                      {"output", std::int64_t(op.output)},
                                      {"target", std::int64_t(op.target)},
                                      {"workPerElement", std::int64_t(op.workPerElement)}});
  }
  const auto &function = checked.module->functions[*selected];
  for (auto parameter : function.parameters) {
    const auto &symbol = checked.module->symbols[parameter];
    llvm::json::Array shape;
    for (const auto &extent : symbol.type.shape)
      shape.push_back(extent.constant ? llvm::json::Value(*extent.constant)
                                      : llvm::json::Value(nullptr));
    parameters.push_back(
        llvm::json::Object{{"name", symbol.name},
                           {"type", std::string(describeNumeric(symbol.type.element)->name)},
                           {"tensor", symbol.type.tensor},
                           {"shape", std::move(shape)}});
  }
  llvm::json::Object artifact{{"format", 1},
                              {"backend", "vulkan"},
                              {"scope", "kernel-program"},
                              {"arithmetic", std::string(arithmeticModeName(*arithmetic))},
                              {"parameters", std::move(parameters)},
                              {"type", std::string(describeNumeric(program->type.element)->name)},
                              {"tensor", program->type.tensor},
                              {"rank", std::int64_t(program->type.shape.size())},
                              {"result", std::int64_t(program->result)},
                              {"kernels", std::move(kernels)},
                              {"code", std::move(code)}};
  return write(directory / "program.json",
               llvm::formatv("{0:2}\n", llvm::json::Value(std::move(artifact))).str())
             ? 0
             : 1;
}
